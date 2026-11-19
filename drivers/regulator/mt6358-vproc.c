// SPDX-License-Identifier: GPL-2.0
/*
 * MT6358 VPROC11/VPROC12 buck regulators (CPU cluster supplies)
 *
 * The full mt6358 MFD (and with it mt6358-regulator) cannot bind on this
 * port because the PMIC interrupt is not wired up.  CPU DVFS only needs the
 * two cluster bucks, and they are reachable through the pwrap regmap, so
 * expose just BUCK_VPROC11 (big cluster) and BUCK_VPROC12 (little cluster,
 * shared with CCI) as stand-alone regulators.
 *
 * The MT6358 tracks LDO_VSRAM_PROC11/PROC12 to the matching buck voltage in
 * hardware (the vendor bootloader leaves the tracking enabled), so only the
 * bucks have to be programmed from software.
 */

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/uaccess.h>

#include <linux/mfd/mt6358/registers.h>

#define MT6358_VPROC_VSEL_MASK	GENMASK(6, 0)
#define MT6358_VPROC_MIN_UV	500000
#define MT6358_VPROC_MAX_UV	1293750
#define MT6358_VPROC_STEP_UV	6250

/* Be conservative: a voltage increase must be settled before freq goes up. */
#define MT6358_VPROC_RAMP_DELAY	100	/* uV/us */

#define MT6358_ID_VPROC11	0
#define MT6358_ID_VPROC12	1
#define MT6358_ID_VGPU		2

#define MT6358_N_CPU_RAILS	2
#define MT6358_N_RAILS		3

struct mt6358_vproc {
	struct regmap *regmap;
	struct dentry *debugfs;
	/* Last selector the regulator core asked for, before any offset. */
	unsigned int last_sel[MT6358_N_RAILS];
	struct regulator_dev *rdev[MT6358_N_RAILS];
};

/*
 * Bring-up knob: shift both CPU rails by a fixed amount without touching the
 * OPP table.  Negative values test how much headroom the silicon has at a
 * given frequency, positive ones give an overclocked point more voltage.
 * Reset to 0 on every boot, so a value that hangs the machine cannot stick.
 */
static int mt6358_vproc_offset_uv;

static const u32 mt6358_vproc_status_reg[] = {
	[MT6358_ID_VPROC11] = MT6358_BUCK_VPROC11_DBG1,
	[MT6358_ID_VPROC12] = MT6358_BUCK_VPROC12_DBG1,
	[MT6358_ID_VGPU] = MT6358_BUCK_VGPU_DBG1,
};

/*
 * Read the voltage back from DBG0, like the in-tree mt6358 driver does.
 * ELR0 is the value software writes; DBG0 reflects what the buck DAC is
 * actually running at (they differ when the PMIC tracks a rail in hardware).
 */
static const u32 mt6358_vproc_feedback_reg[] = {
	[MT6358_ID_VPROC11] = MT6358_BUCK_VPROC11_DBG0,
	[MT6358_ID_VPROC12] = MT6358_BUCK_VPROC12_DBG0,
	[MT6358_ID_VGPU] = MT6358_BUCK_VGPU_DBG0,
};

static int mt6358_vproc_get_voltage_sel(struct regulator_dev *rdev)
{
	struct mt6358_vproc *priv = rdev_get_drvdata(rdev);
	unsigned int val = 0;
	int ret;

	ret = regmap_read(priv->regmap,
			  mt6358_vproc_feedback_reg[rdev_get_id(rdev)], &val);
	if (ret)
		return ret;

	return val & MT6358_VPROC_VSEL_MASK;
}

static unsigned int mt6358_vproc_apply_offset(unsigned int sel)
{
	s64 uv = MT6358_VPROC_MIN_UV + (s64)sel * MT6358_VPROC_STEP_UV;
	s64 target = uv + mt6358_vproc_offset_uv;
	unsigned int max_sel = (MT6358_VPROC_MAX_UV - MT6358_VPROC_MIN_UV) /
			       MT6358_VPROC_STEP_UV;
	int new_sel;

	if (target < MT6358_VPROC_MIN_UV)
		target = MT6358_VPROC_MIN_UV;
	if (target > MT6358_VPROC_MAX_UV)
		target = MT6358_VPROC_MAX_UV;

	new_sel = DIV_ROUND_CLOSEST_ULL(target - MT6358_VPROC_MIN_UV,
					MT6358_VPROC_STEP_UV);
	if (new_sel < 0)
		new_sel = 0;
	if (new_sel > max_sel)
		new_sel = max_sel;

	return new_sel;
}

static int mt6358_vproc_set_voltage_sel(struct regulator_dev *rdev,
					unsigned int sel)
{
	struct mt6358_vproc *priv = rdev_get_drvdata(rdev);

	priv->last_sel[rdev_get_id(rdev)] = sel;

	return regulator_set_voltage_sel_regmap(rdev,
						mt6358_vproc_apply_offset(sel));
}

static int mt6358_vproc_get_status(struct regulator_dev *rdev)
{
	struct mt6358_vproc *priv = rdev_get_drvdata(rdev);
	unsigned int val;
	int ret;

	ret = regmap_read(priv->regmap,
			  mt6358_vproc_status_reg[rdev_get_id(rdev)], &val);
	if (ret)
		return ret;

	return (val & BIT(0)) ? REGULATOR_STATUS_ON : REGULATOR_STATUS_OFF;
}

static const struct regulator_ops mt6358_vproc_ops = {
	.list_voltage		= regulator_list_voltage_linear,
	.map_voltage		= regulator_map_voltage_linear,
	.set_voltage_sel	= mt6358_vproc_set_voltage_sel,
	.get_voltage_sel	= mt6358_vproc_get_voltage_sel,
	.set_voltage_time_sel	= regulator_set_voltage_time_sel,
	.enable			= regulator_enable_regmap,
	.disable		= regulator_disable_regmap,
	.is_enabled		= regulator_is_enabled_regmap,
	.get_status		= mt6358_vproc_get_status,
};

#define MT6358_VPROC_DESC(_name, _id, _vreg)				\
{									\
	.name		= #_name,					\
	.of_match	= of_match_ptr(#_name),				\
	.ops		= &mt6358_vproc_ops,				\
	.type		= REGULATOR_VOLTAGE,				\
	.owner		= THIS_MODULE,					\
	.id		= _id,						\
	.n_voltages	= ((MT6358_VPROC_MAX_UV) - (MT6358_VPROC_MIN_UV)) / \
			  (MT6358_VPROC_STEP_UV) + 1,			\
	.min_uV		= MT6358_VPROC_MIN_UV,				\
	.uV_step	= MT6358_VPROC_STEP_UV,				\
	.ramp_delay	= MT6358_VPROC_RAMP_DELAY,			\
	.vsel_reg	= MT6358_BUCK_##_vreg##_ELR0,			\
	.vsel_mask	= MT6358_VPROC_VSEL_MASK,			\
	.enable_reg	= MT6358_BUCK_##_vreg##_CON0,			\
	.enable_mask	= BIT(0),					\
}

static const struct regulator_desc mt6358_vproc11_desc =
	MT6358_VPROC_DESC(vproc11, MT6358_ID_VPROC11, VPROC11);
static const struct regulator_desc mt6358_vproc12_desc =
	MT6358_VPROC_DESC(vproc12, MT6358_ID_VPROC12, VPROC12);
static const struct regulator_desc mt6358_vgpu_desc =
	MT6358_VPROC_DESC(vgpu, MT6358_ID_VGPU, VGPU);

static void mt6358_vproc_show_reg(struct seq_file *s, struct mt6358_vproc *priv,
				  const char *name, u32 reg)
{
	unsigned int val = 0;

	regmap_read(priv->regmap, reg, &val);
	seq_printf(s, "  %-22s @%#06x = %#06x\n", name, reg, val);
}

static void mt6358_vproc_show_buck(struct seq_file *s, struct mt6358_vproc *priv,
				   const char *name, u32 elr, u32 dbg0,
				   u32 con0, u32 dbg1)
{
	unsigned int vsel = 0;

	regmap_read(priv->regmap, elr, &vsel);
	vsel &= MT6358_VPROC_VSEL_MASK;

	seq_printf(s, "%s: %u uV (vsel %u)\n", name,
		   MT6358_VPROC_MIN_UV + vsel * MT6358_VPROC_STEP_UV, vsel);
	mt6358_vproc_show_reg(s, priv, "ELR0", elr);
	mt6358_vproc_show_reg(s, priv, "DBG0", dbg0);
	mt6358_vproc_show_reg(s, priv, "CON0", con0);
	mt6358_vproc_show_reg(s, priv, "DBG1", dbg1);
}

static int mt6358_vproc_status_show(struct seq_file *s, void *unused)
{
	struct mt6358_vproc *priv = s->private;

	seq_printf(s, "voltage offset: %+d uV\n", mt6358_vproc_offset_uv);
	mt6358_vproc_show_buck(s, priv, "vproc11", MT6358_BUCK_VPROC11_ELR0,
			       MT6358_BUCK_VPROC11_DBG0,
			       MT6358_BUCK_VPROC11_CON0,
			       MT6358_BUCK_VPROC11_DBG1);
	mt6358_vproc_show_buck(s, priv, "vproc12", MT6358_BUCK_VPROC12_ELR0,
			       MT6358_BUCK_VPROC12_DBG0,
			       MT6358_BUCK_VPROC12_CON0,
			       MT6358_BUCK_VPROC12_DBG1);
	mt6358_vproc_show_buck(s, priv, "vgpu", MT6358_BUCK_VGPU_ELR0,
			       MT6358_BUCK_VGPU_DBG0,
			       MT6358_BUCK_VGPU_CON0,
			       MT6358_BUCK_VGPU_DBG1);

	seq_puts(s, "vsram_gpu:\n");
	mt6358_vproc_show_reg(s, priv, "ANA_CON0",
			      MT6358_VSRAM_GPU_ANA_CON0);
	mt6358_vproc_show_reg(s, priv, "CON0",
			      MT6358_LDO_VSRAM_GPU_CON0);
	mt6358_vproc_show_reg(s, priv, "DBG0",
			      MT6358_LDO_VSRAM_GPU_DBG0);
	mt6358_vproc_show_reg(s, priv, "DBG1",
			      MT6358_LDO_VSRAM_GPU_DBG1);

	/* Is the hardware VSRAM tracking still enabled? */
	seq_puts(s, "vsram_proc11:\n");
	mt6358_vproc_show_reg(s, priv, "ANA_CON0",
			      MT6358_VSRAM_PROC11_ANA_CON0);
	mt6358_vproc_show_reg(s, priv, "DBG0",
			      MT6358_LDO_VSRAM_PROC11_DBG0);
	mt6358_vproc_show_reg(s, priv, "TRACKING_CON0",
			      MT6358_LDO_VSRAM_PROC11_TRACKING_CON0);
	mt6358_vproc_show_reg(s, priv, "TRACKING_CON1",
			      MT6358_LDO_VSRAM_PROC11_TRACKING_CON1);
	mt6358_vproc_show_reg(s, priv, "TRACKING_CON2",
			      MT6358_LDO_VSRAM_PROC11_TRACKING_CON2);
	mt6358_vproc_show_reg(s, priv, "TRACKING_CON3",
			      MT6358_LDO_VSRAM_PROC11_TRACKING_CON3);

	seq_puts(s, "vsram_proc12:\n");
	mt6358_vproc_show_reg(s, priv, "ANA_CON0",
			      MT6358_VSRAM_PROC12_ANA_CON0);
	mt6358_vproc_show_reg(s, priv, "DBG0",
			      MT6358_LDO_VSRAM_PROC12_DBG0);
	mt6358_vproc_show_reg(s, priv, "TRACKING_CON0",
			      MT6358_LDO_VSRAM_PROC12_TRACKING_CON0);
	mt6358_vproc_show_reg(s, priv, "TRACKING_CON1",
			      MT6358_LDO_VSRAM_PROC12_TRACKING_CON1);
	mt6358_vproc_show_reg(s, priv, "TRACKING_CON2",
			      MT6358_LDO_VSRAM_PROC12_TRACKING_CON2);
	mt6358_vproc_show_reg(s, priv, "TRACKING_CON3",
			      MT6358_LDO_VSRAM_PROC12_TRACKING_CON3);

	return 0;
}
static ssize_t mt6358_vproc_offset_read(struct file *f, char __user *ubuf,
					size_t len, loff_t *ppos)
{
	char tmp[24];
	int n = scnprintf(tmp, sizeof(tmp), "%d\n", mt6358_vproc_offset_uv);

	return simple_read_from_buffer(ubuf, len, ppos, tmp, n);
}

static ssize_t mt6358_vproc_offset_write(struct file *f,
					 const char __user *ubuf,
					 size_t len, loff_t *ppos)
{
	struct mt6358_vproc *priv = f->private_data;
	char buf[32];
	s64 val;
	int i, ret;

	if (!len || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	ret = kstrtos64(buf, 10, &val);
	if (ret)
		return ret;

	mt6358_vproc_offset_uv = clamp_val(val, -MT6358_VPROC_MAX_UV,
					   MT6358_VPROC_MAX_UV);

	/* Re-apply immediately so the new offset takes effect at once. */
	for (i = 0; i < MT6358_N_CPU_RAILS; i++) {
		if (!priv->rdev[i])
			continue;
		regulator_set_voltage_sel_regmap(priv->rdev[i],
			mt6358_vproc_apply_offset(priv->last_sel[i]));
	}

	pr_info("mt6358-vproc: voltage offset %+d uV\n", mt6358_vproc_offset_uv);

	return len;
}

static ssize_t mt6358_vgpu_uv_read(struct file *f, char __user *ubuf,
				   size_t len, loff_t *ppos)
{
	struct mt6358_vproc *priv = f->private_data;
	char tmp[24];
	unsigned int vsel = 0;
	int n;

	if (priv->rdev[MT6358_ID_VGPU])
		vsel = mt6358_vproc_get_voltage_sel(priv->rdev[MT6358_ID_VGPU]);

	n = scnprintf(tmp, sizeof(tmp), "%u\n",
		      MT6358_VPROC_MIN_UV + vsel * MT6358_VPROC_STEP_UV);
	return simple_read_from_buffer(ubuf, len, ppos, tmp, n);
}

static ssize_t mt6358_vgpu_uv_write(struct file *f, const char __user *ubuf,
				    size_t len, loff_t *ppos)
{
	struct mt6358_vproc *priv = f->private_data;
	char buf[32];
	s64 val;
	unsigned int sel;
	int ret;

	if (!priv->rdev[MT6358_ID_VGPU])
		return -ENODEV;
	if (!len || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	ret = kstrtos64(buf, 10, &val);
	if (ret)
		return ret;

	val = clamp_val(val, MT6358_VPROC_MIN_UV, MT6358_VPROC_MAX_UV);
	sel = DIV_ROUND_CLOSEST_ULL(val - MT6358_VPROC_MIN_UV,
				    MT6358_VPROC_STEP_UV);
	if (sel >= priv->rdev[MT6358_ID_VGPU]->desc->n_voltages)
		sel = priv->rdev[MT6358_ID_VGPU]->desc->n_voltages - 1;

	priv->last_sel[MT6358_ID_VGPU] = sel;
	ret = regulator_set_voltage_sel_regmap(priv->rdev[MT6358_ID_VGPU], sel);
	if (ret)
		return ret;

	pr_info("mt6358-vproc: vgpu set to %lld uV (vsel %u)\n", val, sel);

	return len;
}

static const struct file_operations mt6358_vproc_offset_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mt6358_vproc_offset_read,
	.write = mt6358_vproc_offset_write,
	.llseek = default_llseek,
};

static const struct file_operations mt6358_vgpu_uv_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mt6358_vgpu_uv_read,
	.write = mt6358_vgpu_uv_write,
	.llseek = default_llseek,
};

DEFINE_SHOW_ATTRIBUTE(mt6358_vproc_status);

static int mt6358_vproc_probe(struct platform_device *pdev)
{
	static struct of_regulator_match matches[] = {
		{ .name = "vproc11", .desc = &mt6358_vproc11_desc },
		{ .name = "vproc12", .desc = &mt6358_vproc12_desc },
		{ .name = "vgpu", .desc = &mt6358_vgpu_desc },
	};
	struct regulator_config config = {};
	struct mt6358_vproc *priv;
	int i, ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	platform_set_drvdata(pdev, priv);

	/* The pwrap driver owns the PMIC regmap. */
	priv->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!priv->regmap)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "no parent PMIC regmap\n");

	ret = of_regulator_match(&pdev->dev, pdev->dev.of_node, matches,
				 ARRAY_SIZE(matches));
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to parse regulator nodes\n");

	config.dev = &pdev->dev;
	config.driver_data = priv;

	for (i = 0; i < ARRAY_SIZE(matches); i++) {
		struct regulator_dev *rdev;

		if (!matches[i].of_node)
			continue;

		config.of_node = matches[i].of_node;
		config.init_data = matches[i].init_data;

		rdev = devm_regulator_register(&pdev->dev, matches[i].desc,
					       &config);
		if (IS_ERR(rdev))
			return dev_err_probe(&pdev->dev, PTR_ERR(rdev),
					     "failed to register %s\n",
					     matches[i].name);

		priv->rdev[matches[i].desc->id] = rdev;
	}

	priv->debugfs = debugfs_create_dir("mt6358-vproc", NULL);
	debugfs_create_file("status", 0444, priv->debugfs, priv,
			    &mt6358_vproc_status_fops);
	debugfs_create_file("offset_uv", 0644, priv->debugfs, priv,
			    &mt6358_vproc_offset_fops);
	debugfs_create_file("vgpu_uv", 0644, priv->debugfs, priv,
			    &mt6358_vgpu_uv_fops);

	dev_info(&pdev->dev, "VPROC11/VPROC12/VGPU registered\n");

	return 0;
}

static void mt6358_vproc_remove(struct platform_device *pdev)
{
	struct mt6358_vproc *priv = platform_get_drvdata(pdev);

	debugfs_remove_recursive(priv->debugfs);
}

static const struct of_device_id mt6358_vproc_of_match[] = {
	{ .compatible = "mediatek,mt6358-vproc" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6358_vproc_of_match);

static struct platform_driver mt6358_vproc_driver = {
	.driver = {
		.name = "mt6358-vproc",
		.of_match_table = mt6358_vproc_of_match,
	},
	.probe = mt6358_vproc_probe,
	.remove = mt6358_vproc_remove,
};
module_platform_driver(mt6358_vproc_driver);

MODULE_LICENSE("GPL");
