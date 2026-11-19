// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6771 CONNSYS power bring-up probe
 *
 * The MT6771 has no mainline power-domain or clock driver for the
 * connectivity subsystem, so the WiFi/BT combo (MT6631, "A-die") cannot be
 * brought up by the generic MediaTek code.  This driver implements the
 * vendor power-up sequence for the CONN subsystem in a self contained,
 * opt-in way:
 *
 *   - MT6358 LDO_VCN18 (1.8 V core) and LDO_VCN33 (3.3 V PA) are enabled
 *     through the pwrap regmap, because the MT6358 MFD cannot bind on this
 *     port (no PMIC IRQ),
 *   - the CONNSYS interface pinmux is switched from "D-die" to the SoC
 *     interface,
 *   - the CONN MTCMOS domain is switched on in the SPM and the AXI bus
 *     protection is released,
 *   - the A-die chip ID is polled at 0x18070008 (0x0788 = MT6631).
 *
 * Nothing here touches the WLAN/BT firmware or the HIF; it only proves that
 * the rails, the interface and the SPM handshake work.  The sequence is
 * debugfs triggered and every poll has a timeout, so a wrong assumption
 * cannot spin forever.  Register offsets and bit positions come from the
 * vendor clk-mt6771-pg.c, the vendor common/platform/mt6771.c (WMT) and the
 * vendor clkbuf driver.
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kstrtox.h>
#include <linux/mfd/mt6358/registers.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>

/* SPM, relative to 0x10006000 */
#define SPM_PWR_STATUS			0x0180
#define SPM_PWR_STATUS_2ND		0x0184
#define SPM_CONN_PWR_CON		0x032c

/* CONN MTCMOS control bits (same layout as the other MTCMOS domains) */
#define PWR_RST_B			BIT(0)
#define PWR_ISO				BIT(1)
#define PWR_ON				BIT(2)
#define PWR_ON_2ND			BIT(3)
#define PWR_CLK_DIS			BIT(4)
#define CONN_PWR_STA			BIT(1)

/* INFRA_AO bus protection, relative to 0x10001000 */
#define INFRA_TOPAXI_PROT_EN_CLR	0x02a4
#define CONN_PROT_MASK			(BIT(13) | BIT(14))

/* AP_RGU, relative to 0x10007000 */
#define AP_RGU_CPU_SW_RST		0x0018
#define CPU_SW_RST_BIT			BIT(12)
#define CPU_SW_RST_KEY			(0x88 << 24)

/* CONN MCU config, relative to 0x18070000 */
#define CONSYS_CHIP_ID			0x0008
#define CONSYS_ACR			0x0140
#define CONSYS_CPUPCR			0x0160
#define CONSYS_ROM_DESEL		0x0600
#define CONSYS_CHIP_ID_MT6631		0x0788
#define CONSYS_ROM_DESEL_VALUE		0x001702a4
#define CONSYS_ACR_MBIST_BYPASS		GENMASK(1, 0)

/* CONN AFE (analog front end), separate 4 KiB block */
#define CONSYS_AFE_BASE			0x180b6000
#define CONSYS_AFE_SIZE			0x0100
#define CONSYS_AFE_PLL03		0x0038
#define CONSYS_AFE_PLL03_VALUE		0x000c1df0
#define CONSYS_AFE_GPS02		0x0054
#define CONSYS_AFE_GPS02_VALUE		0x110a2000

/* CONNSYS interface pinmux, inside the 0x10005000 pin controller block */
#define CONSYS_PINMUX_BASE		0x10005000
#define CONSYS_PINMUX_SIZE		0x1000
#define CONSYS_PINMUX_01		0x03f0
#define CONSYS_PINMUX_02		0x03e0

#define MTCMOS_POLL_US			10
#define MTCMOS_TIMEOUT_US		200000
#define CHIPID_POLL_MS			20
#define CHIPID_TIMEOUT_MS		400

struct mt6771_consys {
	struct device *dev;
	void __iomem *mcu;	/* 0x18070000 */
	void __iomem *ap_rgu;	/* 0x10007000 */
	void __iomem *infra;	/* 0x10001000 */
	void __iomem *spm;	/* 0x10006000 */
	void __iomem *afe;	/* 0x180b6000 */
	void __iomem *pinmux;	/* 0x10005000 */
	struct regmap *pmic;	/* pwrap regmap, for the MT6358 LDOs */
	struct dentry *dbg;
	bool rails_on;
	bool powered;
};

static struct mt6771_consys *g_consys;

/*
 * The MT6631 boot ROM looks for its patch in the CONNSYS EMI window while the
 * MCU is still held in reset, so whatever wants to stage a patch has to do it
 * from inside consys_power_on(), just before the reset is released.  The WMT
 * glue registers the callback (mt6771_consys_set_pre_boot_hook()).
 */
static void (*consys_pre_boot_hook)(void);

void mt6771_consys_set_pre_boot_hook(void (*fn)(void))
{
	consys_pre_boot_hook = fn;
}
EXPORT_SYMBOL(mt6771_consys_set_pre_boot_hook);

static bool consys_auto;
module_param_named(auto, consys_auto, bool, 0444);
MODULE_PARM_DESC(auto, "Power on CONNSYS at probe time (default: no)");

static int consys_rails_on(struct mt6771_consys *c)
{
	struct regmap *pmic = c->pmic;
	int ret;

	if (!pmic)
		return -ENODEV;

	/*
	 * VCN33 feeds the A-die PA.  It has two separate enable bits (BT and
	 * WiFi) for the same output, only one of them is used by the vendor
	 * kernel.  Set 3.3 V (ANA_CON0 [9:8] = 1) like the DTS constraint
	 * suggests, then enable both bits.
	 */
	ret = regmap_update_bits(pmic, MT6358_VCN33_ANA_CON0, GENMASK(9, 8),
				 BIT(8));
	if (ret)
		return ret;

	ret = regmap_update_bits(pmic, MT6358_LDO_VCN33_CON0_0, BIT(0), BIT(0));
	if (ret)
		return ret;

	ret = regmap_update_bits(pmic, MT6358_LDO_VCN33_CON0_1, BIT(0), BIT(0));
	if (ret)
		return ret;

	/* VCN18 is a fixed 1.8 V LDO, only the enable bit matters. */
	ret = regmap_update_bits(pmic, MT6358_LDO_VCN18_CON0, BIT(0), BIT(0));
	if (ret)
		return ret;

	/* LDO ramp-up time is 270 us according to the DTS. */
	usleep_range(500, 800);

	c->rails_on = true;
	return 0;
}

static void consys_rails_off(struct mt6771_consys *c)
{
	struct regmap *pmic = c->pmic;

	if (!pmic)
		return;

	regmap_update_bits(pmic, MT6358_LDO_VCN18_CON0, BIT(0), 0);
	regmap_update_bits(pmic, MT6358_LDO_VCN33_CON0_1, BIT(0), 0);
	regmap_update_bits(pmic, MT6358_LDO_VCN33_CON0_0, BIT(0), 0);

	c->rails_on = false;
}

static void consys_if_pinmux(struct mt6771_consys *c)
{
	u32 val;

	if (!c->pinmux)
		return;

	val = readl(c->pinmux + CONSYS_PINMUX_01);
	writel((val & 0xffffff00) | 0x00000011, c->pinmux + CONSYS_PINMUX_01);
	val = readl(c->pinmux + CONSYS_PINMUX_02);
	writel((val & 0x0000000f) | 0x11111110, c->pinmux + CONSYS_PINMUX_02);
	dev_info(c->dev, "CONNSYS interface pinmux set (0x%08x 0x%08x)\n",
		 readl(c->pinmux + CONSYS_PINMUX_01),
		 readl(c->pinmux + CONSYS_PINMUX_02));
}

static void consys_cpu_reset(struct mt6771_consys *c, bool assert)
{
	u32 val;

	val = readl(c->ap_rgu + AP_RGU_CPU_SW_RST);
	if (assert)
		val |= CPU_SW_RST_BIT;
	else
		val &= ~CPU_SW_RST_BIT;
	writel(val | CPU_SW_RST_KEY, c->ap_rgu + AP_RGU_CPU_SW_RST);
}

static int consys_mtcmos_on(struct mt6771_consys *c)
{
	void __iomem *con = c->spm + SPM_CONN_PWR_CON;
	u32 val;
	int ret;

	val = readl(con) | PWR_ON;
	writel(val, con);
	writel(readl(con) | PWR_ON_2ND, con);

	ret = readl_poll_timeout(c->spm + SPM_PWR_STATUS, val,
				 val & CONN_PWR_STA, MTCMOS_POLL_US,
				 MTCMOS_TIMEOUT_US);
	if (ret) {
		dev_err(c->dev, "CONN power status timeout (0x%08x)\n",
			readl(c->spm + SPM_PWR_STATUS));
		return ret;
	}

	ret = readl_poll_timeout(c->spm + SPM_PWR_STATUS_2ND, val,
				 val & CONN_PWR_STA, MTCMOS_POLL_US,
				 MTCMOS_TIMEOUT_US);
	if (ret) {
		dev_err(c->dev, "CONN power status 2nd timeout (0x%08x)\n",
			readl(c->spm + SPM_PWR_STATUS_2ND));
		return ret;
	}

	/* Release clock gating and isolation, then deassert the reset. */
	val = readl(con) & ~(PWR_CLK_DIS | PWR_ISO);
	writel(val, con);
	writel(val | PWR_RST_B, con);

	/* Release the AXI bus protection for the CONN master/slave ports. */
	val = readl(c->infra + INFRA_TOPAXI_PROT_EN_CLR);
	writel(val & ~CONN_PROT_MASK, c->infra + INFRA_TOPAXI_PROT_EN_CLR);

	return 0;
}

static int consys_wait_chipid(struct mt6771_consys *c)
{
	unsigned int i;

	for (i = 0; i < CHIPID_TIMEOUT_MS / CHIPID_POLL_MS; i++) {
		u32 id = readl(c->mcu + CONSYS_CHIP_ID);

		if (id == CONSYS_CHIP_ID_MT6631) {
			dev_info(c->dev, "A-die chip id 0x%08x detected\n", id);
			return 0;
		}
		usleep_range(CHIPID_POLL_MS * 1000, CHIPID_POLL_MS * 1000 + 1000);
	}

	dev_err(c->dev, "no A-die chip id, last value 0x%08x (CONN_PWR_CON 0x%08x, PWR_STATUS 0x%08x)\n",
		readl(c->mcu + CONSYS_CHIP_ID),
		readl(c->spm + SPM_CONN_PWR_CON),
		readl(c->spm + SPM_PWR_STATUS));
	return -ETIMEDOUT;
}

static int consys_power_on(struct mt6771_consys *c)
{
	int ret;

	if (c->powered)
		return 0;
	ret = consys_rails_on(c);
	if (ret) {
		dev_err(c->dev, "cannot enable VCN18/VCN33: %d\n", ret);
		return ret;
	}

	consys_if_pinmux(c);

	/* Keep the A-die CPU in reset while the interface comes up. */
	consys_cpu_reset(c, true);
	usleep_range(150, 200);

	ret = consys_mtcmos_on(c);
	if (ret)
		goto err_rails;

	ret = consys_wait_chipid(c);
	if (ret)
		goto err_rails;

	/* Bypass the at-speed MBIST, then release the ROM remap. */
	writel(CONSYS_ROM_DESEL_VALUE, c->mcu + CONSYS_ROM_DESEL);
	writel(readl(c->mcu + CONSYS_ACR) | CONSYS_ACR_MBIST_BYPASS,
	       c->mcu + CONSYS_ACR);

	if (c->afe) {
		writel(CONSYS_AFE_PLL03_VALUE, c->afe + CONSYS_AFE_PLL03);
		writel(CONSYS_AFE_GPS02_VALUE, c->afe + CONSYS_AFE_GPS02);
	}

	/* The MCU still has its reset asserted: give the connectivity stack a
	 * chance to place the ROM patch in EMI before the ROM starts running.
	 */
	if (consys_pre_boot_hook)
		consys_pre_boot_hook();

	consys_cpu_reset(c, false);
	msleep(20);

	c->powered = true;
	dev_info(c->dev, "CONNSYS powered on (CPUPCR 0x%08x)\n",
		 readl(c->mcu + CONSYS_CPUPCR));
	return 0;

err_rails:
	consys_cpu_reset(c, true);
	consys_rails_off(c);
	return ret;
}

static void consys_power_off(struct mt6771_consys *c)
{
	void __iomem *con = c->spm + SPM_CONN_PWR_CON;
	u32 val;

	if (!c->powered)
		return;

	consys_cpu_reset(c, true);

	val = readl(c->infra + INFRA_TOPAXI_PROT_EN_CLR);
	writel(val | CONN_PROT_MASK, c->infra + INFRA_TOPAXI_PROT_EN_CLR);

	val = readl(con) | PWR_ISO | PWR_CLK_DIS;
	writel(val, con);
	writel(val & ~PWR_RST_B, con);
	writel(readl(con) & ~(PWR_ON | PWR_ON_2ND), con);

	consys_rails_off(c);
	c->powered = false;
}

static int consys_status_show(struct seq_file *s, void *unused)
{
	struct mt6771_consys *c = s->private;
	unsigned int vcn18 = 0, vcn33_bt = 0, vcn33_wifi = 0, vcn33_ana = 0;

	if (c->pmic) {
		regmap_read(c->pmic, MT6358_LDO_VCN18_CON0, &vcn18);
		regmap_read(c->pmic, MT6358_LDO_VCN33_CON0_0, &vcn33_bt);
		regmap_read(c->pmic, MT6358_LDO_VCN33_CON0_1, &vcn33_wifi);
		regmap_read(c->pmic, MT6358_VCN33_ANA_CON0, &vcn33_ana);
	}

	seq_printf(s, "powered: %d  rails_on: %d\n", c->powered, c->rails_on);
	seq_printf(s, "chip id:            0x%08x (expect 0x%08x)\n",
		   readl(c->mcu + CONSYS_CHIP_ID), CONSYS_CHIP_ID_MT6631);
	seq_printf(s, "CONN_PWR_CON:       0x%08x\n",
		   readl(c->spm + SPM_CONN_PWR_CON));
	seq_printf(s, "PWR_STATUS:         0x%08x\n",
		   readl(c->spm + SPM_PWR_STATUS));
	seq_printf(s, "PWR_STATUS_2ND:     0x%08x\n",
		   readl(c->spm + SPM_PWR_STATUS_2ND));
	seq_printf(s, "AP_RGU CPU_SW_RST:  0x%08x\n",
		   readl(c->ap_rgu + AP_RGU_CPU_SW_RST));
	seq_printf(s, "CONNSYS ACR:        0x%08x\n",
		   readl(c->mcu + CONSYS_ACR));
	seq_printf(s, "CONNSYS ROM_DESEL:  0x%08x\n",
		   readl(c->mcu + CONSYS_ROM_DESEL));
	seq_printf(s, "CONNSYS CPUPCR:     0x%08x\n",
		   readl(c->mcu + CONSYS_CPUPCR));
	seq_printf(s, "VCN18 CON0:         0x%04x\n", vcn18);
	seq_printf(s, "VCN33 CON0_0/1/ANA: 0x%04x 0x%04x 0x%04x\n",
		   vcn33_bt, vcn33_wifi, vcn33_ana);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(consys_status);

static ssize_t consys_power_write(struct file *file, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	struct mt6771_consys *c = file->private_data;
	int val, ret;

	ret = kstrtoint_from_user(ubuf, count, 0, &val);
	if (ret)
		return ret;

	if (val)
		consys_power_on(c);
	else
		consys_power_off(c);

	return count;
}

static const struct file_operations consys_power_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = consys_power_write,
};

static void __iomem *consys_map(struct platform_device *pdev, int idx,
				const char *name)
{
	static const resource_size_t fallback_base[] = {
		0x18070000, 0x10007000, 0x10001000, 0x10006000,
	};
	static const resource_size_t fallback_size[] = {
		0x1000, 0x100, 0x1000, 0x1000,
	};
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, idx);
	resource_size_t start, size;
	void __iomem *base;

	if (res) {
		start = res->start;
		size = resource_size(res);
	} else {
		/*
		 * The device tree passed by the bootloader is not always what
		 * we think it is (an overlay can drop or rewrite "reg"), so fall
		 * back to the fixed windows from the SoC memory map instead of
		 * refusing to probe.
		 */
		start = fallback_base[idx];
		size = fallback_size[idx];
	}

	base = devm_ioremap(&pdev->dev, start, size);
	dev_info(&pdev->dev, "%s at %pa size 0x%zx (%s)%s\n", name, &start,
		 (size_t)size, res ? "DT reg" : "fallback",
		 base ? "" : " - ioremap FAILED");

	return base;
}

static int mt6771_consys_probe(struct platform_device *pdev)
{
	struct mt6771_consys *c;
	struct platform_device *pwrap;
	struct device_node *np;
	int ret;

	if (g_consys) {
		dev_info(&pdev->dev, "already set up by the lazy path, skipping\n");
		return 0;
	}

	c = devm_kzalloc(&pdev->dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	c->dev = &pdev->dev;

	dev_info(c->dev, "probe: %u platform resources\n",
		 pdev->num_resources);

	/*
	 * The SPM, AP_RGU and INFRA_AO register windows are already owned by
	 * the MFG power driver, the watchdog and the clock driver.  Only a few
	 * registers inside them are touched here, so map them without taking
	 * an exclusive claim on the region.
	 */
	c->mcu = consys_map(pdev, 0, "CONN MCU");
	if (!c->mcu)
		return -ENOMEM;

	c->ap_rgu = consys_map(pdev, 1, "AP_RGU");
	if (!c->ap_rgu)
		return -ENOMEM;

	c->infra = consys_map(pdev, 2, "INFRA_AO");
	if (!c->infra)
		return -ENOMEM;

	c->spm = consys_map(pdev, 3, "SPM");
	if (!c->spm)
		return -ENOMEM;

	c->afe = devm_ioremap(&pdev->dev, CONSYS_AFE_BASE, CONSYS_AFE_SIZE);
	c->pinmux = devm_ioremap(&pdev->dev, CONSYS_PINMUX_BASE,
				 CONSYS_PINMUX_SIZE);

	/*
	 * The MT6358 MFD cannot bind on this port, but the pwrap driver does
	 * create the PMIC regmap, which is all the LDO control needs.
	 */
	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6779-pwrap");
	if (np) {
		pwrap = of_find_device_by_node(np);
		if (pwrap) {
			c->pmic = dev_get_regmap(&pwrap->dev, NULL);
			put_device(&pwrap->dev);
		}
		of_node_put(np);
	}
	if (!c->pmic)
		dev_warn(c->dev, "no pwrap regmap, VCN18/VCN33 untouched\n");

	platform_set_drvdata(pdev, c);
	g_consys = c;

	c->dbg = debugfs_create_dir("mt6771-consys", NULL);
	debugfs_create_file("status", 0444, c->dbg, c, &consys_status_fops);
	debugfs_create_file("power", 0200, c->dbg, c, &consys_power_fops);

	dev_info(c->dev, "probe ready, write 1 to %s/power to bring CONNSYS up\n",
		 "debugfs/mt6771-consys");

	if (consys_auto) {
		ret = consys_power_on(c);
		if (ret)
			dev_err(c->dev, "auto power on failed: %d\n", ret);
	}

	return 0;
}

static void mt6771_consys_remove(struct platform_device *pdev)
{
	struct mt6771_consys *c = platform_get_drvdata(pdev);

	consys_power_off(c);
	debugfs_remove_recursive(c->dbg);
	g_consys = NULL;
}

static const struct of_device_id mt6771_consys_of_match[] = {
	{ .compatible = "mediatek,mt6771-consys" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6771_consys_of_match);

/*
 * Fallback state, built from the fixed SoC memory map when the platform
 * device never probes (the bootloader-injected DT can keep it deferred for
 * ever, which used to make mt6771_consys_power_on() return -ENODEV).
 */
static struct mt6771_consys lazy_consys;
static bool lazy_tried;

static int consys_lazy_init(void)
{
	struct mt6771_consys *c = &lazy_consys;
	struct platform_device *pdev;
	struct device_node *np;

	if (g_consys)
		return 0;
	if (lazy_tried)
		return -ENODEV;
	lazy_tried = true;

	memset(c, 0, sizeof(*c));

	/* Use the real device for logging if the node still exists. */
	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6771-consys");
	if (np) {
		pdev = of_find_device_by_node(np);
		if (pdev) {
			c->dev = &pdev->dev;
			put_device(&pdev->dev);
		}
		of_node_put(np);
	}

	c->mcu = ioremap(0x18070000, 0x1000);
	c->ap_rgu = ioremap(0x10007000, 0x100);
	c->infra = ioremap(0x10001000, 0x1000);
	c->spm = ioremap(0x10006000, 0x1000);
	c->afe = ioremap(CONSYS_AFE_BASE, CONSYS_AFE_SIZE);
	c->pinmux = ioremap(CONSYS_PINMUX_BASE, CONSYS_PINMUX_SIZE);

	pr_info("mt6771-consys: lazy init mcu=%px ap_rgu=%px infra=%px spm=%px afe=%px pinmux=%px\n",
		c->mcu, c->ap_rgu, c->infra, c->spm, c->afe, c->pinmux);

	if (!c->mcu || !c->ap_rgu || !c->infra || !c->spm) {
		pr_err("mt6771-consys: lazy init failed, cannot map the register windows\n");
		return -ENOMEM;
	}

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6779-pwrap");
	if (np) {
		pdev = of_find_device_by_node(np);
		if (pdev) {
			c->pmic = dev_get_regmap(&pdev->dev, NULL);
			put_device(&pdev->dev);
		}
		of_node_put(np);
	}
	pr_info("mt6771-consys: lazy pwrap regmap %s\n",
		c->pmic ? "found" : "MISSING (VCN18/VCN33 stay off)");

	c->dbg = debugfs_create_dir("mt6771-consys", NULL);
	debugfs_create_file("status", 0444, c->dbg, c, &consys_status_fops);
	debugfs_create_file("power", 0200, c->dbg, c, &consys_power_fops);

	g_consys = c;
	return 0;
}

/*
 * Small API for the WLAN/BT drivers, which have to bring CONNSYS up before
 * they can talk to the A-die.  Use plain EXPORT_SYMBOL because the vendor
 * WLAN module is licensed GPL but the BT one may not be.
 */
int mt6771_consys_power_on(void)
{
	if (!g_consys && consys_lazy_init())
		return -ENODEV;
	return consys_power_on(g_consys);
}
EXPORT_SYMBOL(mt6771_consys_power_on);

void mt6771_consys_power_off(void)
{
	if (g_consys)
		consys_power_off(g_consys);
}
EXPORT_SYMBOL(mt6771_consys_power_off);

bool mt6771_consys_is_powered(void)
{
	return g_consys && g_consys->powered;
}
EXPORT_SYMBOL(mt6771_consys_is_powered);

u32 mt6771_consys_read_cpupcr(void)
{
	if (!g_consys)
		return 0;
	return readl(g_consys->mcu + CONSYS_CPUPCR);
}
EXPORT_SYMBOL(mt6771_consys_read_cpupcr);

u32 mt6771_consys_read_chip_id(void)
{
	if (!g_consys)
		return 0;
	return readl(g_consys->mcu + CONSYS_CHIP_ID);
}
EXPORT_SYMBOL(mt6771_consys_read_chip_id);

static struct platform_driver mt6771_consys_driver = {
	.probe = mt6771_consys_probe,
	.remove = mt6771_consys_remove,
	.driver = {
		.name = "mt6771-consys",
		.of_match_table = mt6771_consys_of_match,
	},
};
module_platform_driver(mt6771_consys_driver);

MODULE_DESCRIPTION("MediaTek MT6771 CONNSYS power bring-up probe");
MODULE_LICENSE("GPL");
