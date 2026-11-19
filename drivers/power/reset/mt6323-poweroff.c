// SPDX-License-Identifier: GPL-2.0
/*
 * Power off through MediaTek PMIC
 *
 * Copyright (C) 2018 MediaTek Inc.
 *
 * Author: Sean Wang <sean.wang@mediatek.com>
 *
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/mfd/mt6397/rtc.h>
#include <linux/reboot.h>

#define MT6358_RTC_BASE		0x0588
#define MT6358_RTC_AL_MASK	0x0590
#define MT6358_RTC_AL_MASK_DOW	BIT(4)
#define MT6358_PPCCTL0		0x0a08
#define MT6358_PWRHOLD		BIT(0)
#define MT6358_PSEQ_ELR11	0x0a62
#define MT6358_SDN_DLY_ENB	BIT(10)
#define MT6358_TOP_TMA_KEY	0x03a8
#define MT6358_TMA_KEY_MAGIC	0x9ca7

#define RTC_BBPU_PWREN		BIT(0)
#define RTC_BBPU_CLR		BIT(1)
#define RTC_BBPU_RELOAD		BIT(5)

struct mt6323_pwrc {
	struct device *dev;
	struct regmap *regmap;
	u32 base;
	u32 wrtgr;
};

static void mt6358_dump_rtc(struct mt6323_pwrc *pwrc, const char *tag)
{
	unsigned int bbpu = 0, ppc = 0, al = 0, pdn1 = 0, pdn2 = 0;
	unsigned int pk1 = 0, pk2 = 0, prot = 0;

	regmap_read(pwrc->regmap, pwrc->base + RTC_BBPU, &bbpu);
	regmap_read(pwrc->regmap, MT6358_PPCCTL0, &ppc);
	regmap_read(pwrc->regmap, MT6358_RTC_AL_MASK, &al);
	regmap_read(pwrc->regmap, pwrc->base + 0x2c, &pdn1);
	regmap_read(pwrc->regmap, pwrc->base + 0x2e, &pdn2);
	regmap_read(pwrc->regmap, pwrc->base + 0x28, &pk1);
	regmap_read(pwrc->regmap, pwrc->base + 0x2a, &pk2);
	regmap_read(pwrc->regmap, pwrc->base + 0x34, &prot);

	dev_info(pwrc->dev,
		 "%s: BBPU=0x%04x PPCTL0=0x%04x AL_MASK=0x%04x PDN1=0x%04x PDN2=0x%04x PWRKEY1=0x%04x PWRKEY2=0x%04x PROT=0x%04x\n",
		 tag, bbpu, ppc, al, pdn1, pdn2, pk1, pk2, prot);
}

static int mt6358_write_trigger(struct mt6323_pwrc *pwrc)
{
	unsigned int val;
	int ret;

	regmap_write(pwrc->regmap, pwrc->base + pwrc->wrtgr, 1);

	ret = regmap_read_poll_timeout(pwrc->regmap,
				       pwrc->base + RTC_BBPU, val,
				       !(val & RTC_BBPU_CBUSY),
				       MTK_RTC_POLL_DELAY_US,
				       MTK_RTC_POLL_TIMEOUT);
	if (ret)
		dev_err(pwrc->dev, "failed to write RTC trigger: %d\n", ret);

	return ret;
}

/*
 * The MT6358 does not cut power through RTC_BBPU.  Power is held by
 * PPCCTL0.PWRHOLD; the vendor kernel drops that bit after cleaning up the
 * RTC alarm/LPSD state.  Do the same so a pending alarm cannot immediately
 * restart the board.
 */
static int mt6358_do_pwroff(struct mt6323_pwrc *pwrc)
{
	unsigned int val;
	int ret;

	mt6358_dump_rtc(pwrc, "before");

	regmap_write(pwrc->regmap, pwrc->base + RTC_BBPU,
		     RTC_BBPU_KEY | RTC_BBPU_CLR | RTC_BBPU_PWREN);
	regmap_write(pwrc->regmap, MT6358_RTC_AL_MASK,
		     MT6358_RTC_AL_MASK_DOW);
	mt6358_write_trigger(pwrc);

	ret = regmap_read_poll_timeout(pwrc->regmap,
				       pwrc->base + RTC_BBPU, val,
				       !(val & RTC_BBPU_CLR),
				       MTK_RTC_POLL_DELAY_US,
				       MTK_RTC_POLL_TIMEOUT);
	if (ret)
		dev_warn(pwrc->dev, "RTC CLR did not clear: %d\n", ret);

	/* Disable the PMIC shutdown delay (TMA-protected register). */
	regmap_write(pwrc->regmap, MT6358_TOP_TMA_KEY, MT6358_TMA_KEY_MAGIC);
	regmap_update_bits(pwrc->regmap, MT6358_PSEQ_ELR11,
			   MT6358_SDN_DLY_ENB, 0);
	regmap_write(pwrc->regmap, MT6358_TOP_TMA_KEY, 0);

	regmap_read(pwrc->regmap, pwrc->base + RTC_BBPU, &val);
	regmap_write(pwrc->regmap, pwrc->base + RTC_BBPU,
		     val | RTC_BBPU_KEY | RTC_BBPU_RELOAD);
	mt6358_write_trigger(pwrc);

	dev_info(pwrc->dev, "releasing PWRHOLD\n");
	regmap_update_bits(pwrc->regmap, MT6358_PPCCTL0, MT6358_PWRHOLD, 0);

	/* If PWRHOLD did not cut power, leave some state behind for debugging. */
	mdelay(500);
	mt6358_dump_rtc(pwrc, "after");

	WARN_ONCE(1, "Unable to power off system\n");

	return NOTIFY_DONE;
}

static int mt6323_do_pwroff(struct sys_off_data *data)
{
	struct mt6323_pwrc *pwrc = data->cb_data;
	unsigned int val;
	int ret;

	if (pwrc->base == MT6358_RTC_BASE)
		return mt6358_do_pwroff(pwrc);

	regmap_write(pwrc->regmap, pwrc->base + RTC_BBPU, RTC_BBPU_KEY);
	regmap_write(pwrc->regmap, pwrc->base + pwrc->wrtgr, 1);

	ret = regmap_read_poll_timeout(pwrc->regmap,
					pwrc->base + RTC_BBPU, val,
					!(val & RTC_BBPU_CBUSY),
					MTK_RTC_POLL_DELAY_US,
					MTK_RTC_POLL_TIMEOUT);
	if (ret)
		dev_err(pwrc->dev, "failed to write BBPU: %d\n", ret);

	/* Wait some time until system down, otherwise, notice with a warn */
	mdelay(1000);

	WARN_ONCE(1, "Unable to power off system\n");

	return NOTIFY_DONE;
}

static int mt6323_pwrc_probe(struct platform_device *pdev)
{
	struct mt6397_chip *mt6397_chip = dev_get_drvdata(pdev->dev.parent);
	struct mt6323_pwrc *pwrc;
	struct resource *res;
	int ret;

	pwrc = devm_kzalloc(&pdev->dev, sizeof(*pwrc), GFP_KERNEL);
	if (!pwrc)
		return -ENOMEM;

	pwrc->dev = &pdev->dev;

	if (of_device_is_compatible(pdev->dev.of_node,
				    "mediatek,mt6358-poweroff")) {
		/*
		 * The MT6358 sits behind the PMIC wrapper.  The pwrap driver
		 * has already created the PMIC regmap and the bootloader has
		 * initialized the wrapper, so just take the parent regmap.
		 */
		pwrc->regmap = dev_get_regmap(pdev->dev.parent, NULL);
		if (!pwrc->regmap)
			return dev_err_probe(pwrc->dev, -ENODEV,
					     "no parent PMIC regmap\n");
		pwrc->base = MT6358_RTC_BASE;
		pwrc->wrtgr = RTC_WRTGR_MT6358;
	} else {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		if (!res)
			return -EINVAL;

		pwrc->base = res->start;
		pwrc->regmap = mt6397_chip->regmap;
		pwrc->wrtgr = RTC_WRTGR_MT6323;
	}

	ret = devm_register_sys_off_handler(pwrc->dev,
					    SYS_OFF_MODE_POWER_OFF,
					    SYS_OFF_PRIO_DEFAULT,
					    mt6323_do_pwroff,
					    pwrc);
	if (ret)
		return dev_err_probe(pwrc->dev, ret, "failed to register power-off handler\n");

	dev_info(pwrc->dev, "registered power-off handler (base 0x%x)\n",
		 pwrc->base);

	return 0;
}

static const struct of_device_id mt6323_pwrc_dt_match[] = {
	{ .compatible = "mediatek,mt6323-pwrc" },
	{ .compatible = "mediatek,mt6358-poweroff" },
	{},
};
MODULE_DEVICE_TABLE(of, mt6323_pwrc_dt_match);

static struct platform_driver mt6323_pwrc_driver = {
	.probe          = mt6323_pwrc_probe,
	.driver         = {
		.name   = "mt6323-pwrc",
		.of_match_table = mt6323_pwrc_dt_match,
	},
};

module_platform_driver(mt6323_pwrc_driver);

MODULE_DESCRIPTION("Poweroff driver for MT6323 PMIC");
MODULE_AUTHOR("Sean Wang <sean.wang@mediatek.com>");
