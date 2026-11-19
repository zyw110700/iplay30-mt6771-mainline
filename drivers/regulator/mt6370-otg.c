// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6370 OTG boost regulator
 *
 * The MT6370 charger block contains a 5 V boost converter for USB OTG.
 * The upstream mt6370-charger driver exposes it, but that driver also
 * implements BC1.2 detection and needs three interrupt sources plus the
 * ADC channels, none of which are described in the iPlay 30 device tree
 * (the vendor charger node is a different, Android-only binding).
 *
 * This small driver exposes only the boost regulator so that the MTU3
 * controller can switch the port to host mode and power a USB hub.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>

#define MT6370_REG_CHG_CTRL1	0x111
#define MT6370_REG_CHG_CTRL5	0x115
#define MT6370_REG_CHG_CTRL10	0x11A
#define MT6370_REG_ROLE_CTRL	0x01A

#define MT6370_OPA_MODE_MASK	BIT(0)
#define MT6370_VOBST_MASK	GENMASK(7, 2)
#define MT6370_OTG_OC_MASK	GENMASK(2, 0)

/* TCPCI ROLE_CONTROL: CC1/CC2 = Rp (source) or Rd (sink), no DRP */
#define MT6370_ROLE_CTRL_SRC	0x05
#define MT6370_ROLE_CTRL_SNK	0x0a
#define MT6370_ROLE_CTRL_MASK	0x3f

static int mt6370_otg_update(struct regulator_dev *rdev, unsigned int reg,
			     unsigned int mask, unsigned int val)
{
	struct regmap *regmap = rdev_get_regmap(rdev);
	int ret, i;

	for (i = 0; i < 3; i++) {
		ret = regmap_update_bits(regmap, reg, mask, val);
		if (!ret)
			return 0;
		usleep_range(1000, 2000);
	}

	dev_err(rdev_get_dev(rdev), "reg 0x%03x write failed: %d\n", reg, ret);
	return ret;
}

static int mt6370_otg_enable(struct regulator_dev *rdev)
{
	int ret;

	/*
	 * Present Rp on both CC lines before turning the boost on, otherwise
	 * a Type-C hub/device sees a sink and never enables its upstream port.
	 */
	ret = mt6370_otg_update(rdev, MT6370_REG_ROLE_CTRL,
				MT6370_ROLE_CTRL_MASK, MT6370_ROLE_CTRL_SRC);
	if (ret)
		return ret;

	return mt6370_otg_update(rdev, MT6370_REG_CHG_CTRL1,
				 MT6370_OPA_MODE_MASK, MT6370_OPA_MODE_MASK);
}

static int mt6370_otg_disable(struct regulator_dev *rdev)
{
	int ret;

	ret = mt6370_otg_update(rdev, MT6370_REG_CHG_CTRL1,
				MT6370_OPA_MODE_MASK, 0);
	if (ret)
		return ret;

	/* Back to sink mode (Rd) so normal charging/device mode still works. */
	return mt6370_otg_update(rdev, MT6370_REG_ROLE_CTRL,
				 MT6370_ROLE_CTRL_MASK, MT6370_ROLE_CTRL_SNK);
}

static const struct regulator_ops mt6370_otg_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.enable = mt6370_otg_enable,
	.disable = mt6370_otg_disable,
	.is_enabled = regulator_is_enabled_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_current_limit = regulator_set_current_limit_regmap,
	.get_current_limit = regulator_get_current_limit_regmap,
};

static const u32 mt6370_otg_oc_ma[] = {
	500000, 700000, 1100000, 1300000, 1800000, 2100000, 2400000,
};

static const struct regulator_desc mt6370_otg_rdesc = {
	/*
	 * regulator_of_get_init_node() returns NULL unless of_match is set,
	 * which leaves rdev->constraints empty and makes regulator_enable()
	 * fail with -EPERM (no REGULATOR_CHANGE_STATUS).  Matching the node
	 * name makes the core parse regulator-min/max-* from DT.
	 */
	.of_match = "usb-otg-vbus",
	.name = "usb-otg-vbus",
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &mt6370_otg_ops,
	.min_uV = 4425000,
	.uV_step = 25000,
	.n_voltages = 57,
	.vsel_reg = MT6370_REG_CHG_CTRL5,
	.vsel_mask = MT6370_VOBST_MASK,
	.enable_reg = MT6370_REG_CHG_CTRL1,
	.enable_mask = MT6370_OPA_MODE_MASK,
	.curr_table = mt6370_otg_oc_ma,
	.n_current_limits = ARRAY_SIZE(mt6370_otg_oc_ma),
	.csel_reg = MT6370_REG_CHG_CTRL10,
	.csel_mask = MT6370_OTG_OC_MASK,
};

/*
 * The vendor (and upstream) MT6370 TCPC driver patches these registers in
 * mt6370_tcpc_init().  Without them the CC block never samples the CC lines
 * and CC_STATUS stays 0x00, so a Type-C partner sees no Rp from us and
 * never turns on its own upstream port -- which looks exactly like "OTG
 * does not work".  We only need the analogue front-end setup, not the full
 * TCPM state machine, so replay the same patch from the OTG provider.
 */
static const struct reg_sequence mt6370_cc_init[] = {
	REG_SEQ(0xA0, 0x01, 1000),
	REG_SEQ(0x81, 0x38, 0),
	REG_SEQ(0x82, 0x82, 0),
	REG_SEQ(0xBA, 0xFC, 0),
	REG_SEQ(0xBB, 0x50, 0),
	REG_SEQ(0x9E, 0x8F, 0),
	REG_SEQ(0xA1, 0x05, 0),
	REG_SEQ(0xA2, 0x04, 0),
	REG_SEQ(0xA3, 0x4A, 0),
	REG_SEQ(0xA4, 0x01, 0),
	REG_SEQ(0x95, 0x01, 0),
	REG_SEQ(0x80, 0x71, 0),
	REG_SEQ(0x9B, 0x3A, 1000),
};

static int mt6370_otg_probe(struct platform_device *pdev)
{
	struct regulator_config config = { .dev = &pdev->dev };
	struct regulator_dev *rdev;
	int ret;

	config.regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!config.regmap)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "parent regmap unavailable\n");

	ret = regmap_register_patch(config.regmap, mt6370_cc_init,
				    ARRAY_SIZE(mt6370_cc_init));
	if (ret)
		dev_warn(&pdev->dev, "CC front-end init failed: %d\n", ret);

	/*
	 * Hand the DT node to the regulator core, otherwise rdev->dev.of_node
	 * stays NULL and consumers using "vbus-supply = <&usb_otg_vbus>" can
	 * never resolve the supply (they keep returning -EPROBE_DEFER).
	 */
	config.of_node = pdev->dev.of_node;

	rdev = devm_regulator_register(&pdev->dev, &mt6370_otg_rdesc, &config);
	if (IS_ERR(rdev))
		return dev_err_probe(&pdev->dev, PTR_ERR(rdev),
				     "failed to register OTG regulator\n");

	return 0;
}

static const struct of_device_id mt6370_otg_of_match[] = {
	{ .compatible = "mediatek,mt6370-otg-vbus" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6370_otg_of_match);

static struct platform_driver mt6370_otg_driver = {
	.driver = {
		.name = "mt6370-otg-vbus",
		.of_match_table = mt6370_otg_of_match,
	},
	.probe = mt6370_otg_probe,
};
module_platform_driver(mt6370_otg_driver);

MODULE_DESCRIPTION("MT6370 USB OTG boost regulator");
MODULE_LICENSE("GPL");
