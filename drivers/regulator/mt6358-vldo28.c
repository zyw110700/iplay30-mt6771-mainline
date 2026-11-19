// SPDX-License-Identifier: GPL-2.0
/*
 * MT6358 VLDO28 regulator (touch supply)
 *
 * The MT6358 PMIC is reachable through the PMIC wrapper, but the full
 * mt6397 MFD/regulator stack needs the PMIC interrupt, which is not wired
 * up in this port yet.  VLDO28 only feeds the touch controller, so provide
 * a small stand-alone regulator for it instead.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>

#include <linux/mfd/mt6358/registers.h>

#define MT6358_VLDO28_EN		BIT(0)
/* VOSEL[9:8]: 0 = off/reserved, 1 = 2.8 V, 3 = 3.0 V */
#define MT6358_VLDO28_VOSEL_MASK	GENMASK(9, 8)
#define MT6358_VLDO28_VOSEL_2V8		BIT(8)
/* Fine tuning, unused here. */
#define MT6358_VLDO28_VOSEL_FINE_MASK	GENMASK(3, 0)

struct mt6358_vldo28 {
	struct regmap *regmap;
};

static int mt6358_vldo28_enable(struct regulator_dev *rdev)
{
	struct mt6358_vldo28 *priv = rdev_get_drvdata(rdev);
	unsigned int ana = 0, con0 = 0, con1 = 0;
	int ret;

	ret = regmap_update_bits(priv->regmap, MT6358_LDO_VLDO28_CON0_0,
				 MT6358_VLDO28_EN, MT6358_VLDO28_EN);
	if (ret)
		return ret;

	/* Read back for bring-up diagnostics. */
	regmap_read(priv->regmap, MT6358_LDO_VLDO28_CON0_0, &con0);
	regmap_read(priv->regmap, MT6358_VLDO28_ANA_CON0, &ana);
	regmap_read(priv->regmap, MT6358_LDO_VLDO28_CON1, &con1);
	dev_info(&rdev->dev, "enabled: CON0=0x%04x ANA=0x%04x CON1=0x%04x\n",
		 con0, ana, con1);

	return 0;
}

static int mt6358_vldo28_disable(struct regulator_dev *rdev)
{
	struct mt6358_vldo28 *priv = rdev_get_drvdata(rdev);

	return regmap_update_bits(priv->regmap, MT6358_LDO_VLDO28_CON0_0,
				  MT6358_VLDO28_EN, 0);
}

static int mt6358_vldo28_is_enabled(struct regulator_dev *rdev)
{
	struct mt6358_vldo28 *priv = rdev_get_drvdata(rdev);
	unsigned int val;
	int ret;

	ret = regmap_read(priv->regmap, MT6358_LDO_VLDO28_CON0_0, &val);
	if (ret)
		return ret;

	return !!(val & MT6358_VLDO28_EN);
}

static const struct regulator_ops mt6358_vldo28_ops = {
	.enable		= mt6358_vldo28_enable,
	.disable	= mt6358_vldo28_disable,
	.is_enabled	= mt6358_vldo28_is_enabled,
};

static const struct regulator_desc mt6358_vldo28_desc = {
	.name		= "vldo28",
	.type		= REGULATOR_VOLTAGE,
	.owner		= THIS_MODULE,
	.ops		= &mt6358_vldo28_ops,
	.n_voltages	= 1,
	.min_uV		= 2800000,
	.fixed_uV	= 2800000,
};

static int mt6358_vldo28_probe(struct platform_device *pdev)
{
	struct regulator_config config = {};
	struct mt6358_vldo28 *priv;
	struct regulator_dev *rdev;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/* The pwrap driver owns the PMIC regmap. */
	priv->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!priv->regmap)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "no parent PMIC regmap\n");

	/* Select 2.8 V before turning the LDO on. */
	ret = regmap_update_bits(priv->regmap, MT6358_VLDO28_ANA_CON0,
				 MT6358_VLDO28_VOSEL_MASK |
				 MT6358_VLDO28_VOSEL_FINE_MASK,
				 MT6358_VLDO28_VOSEL_2V8);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to set VLDO28 voltage\n");

	{
		unsigned int con0 = 0, ana = 0, con1 = 0;

		regmap_read(priv->regmap, MT6358_LDO_VLDO28_CON0_0, &con0);
		regmap_read(priv->regmap, MT6358_VLDO28_ANA_CON0, &ana);
		regmap_read(priv->regmap, MT6358_LDO_VLDO28_CON1, &con1);
		dev_info(&pdev->dev,
			 "probe: CON0=0x%04x ANA=0x%04x CON1=0x%04x\n",
			 con0, ana, con1);
	}

	config.dev = &pdev->dev;
	config.of_node = pdev->dev.of_node;
	config.driver_data = priv;
	config.init_data = of_get_regulator_init_data(&pdev->dev,
						      pdev->dev.of_node,
						      &mt6358_vldo28_desc);

	rdev = devm_regulator_register(&pdev->dev, &mt6358_vldo28_desc,
				       &config);
	if (IS_ERR(rdev))
		return dev_err_probe(&pdev->dev, PTR_ERR(rdev),
				     "failed to register VLDO28\n");

	dev_info(&pdev->dev, "VLDO28 registered (2.8 V)\n");

	return 0;
}

static const struct of_device_id mt6358_vldo28_of_match[] = {
	{ .compatible = "mediatek,mt6358-vldo28" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6358_vldo28_of_match);

static struct platform_driver mt6358_vldo28_driver = {
	.driver = {
		.name = "mt6358-vldo28",
		.of_match_table = mt6358_vldo28_of_match,
	},
	.probe = mt6358_vldo28_probe,
};
module_platform_driver(mt6358_vldo28_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6358 VLDO28 regulator");
