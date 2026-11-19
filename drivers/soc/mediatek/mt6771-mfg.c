// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6771 MFG (Mali GPU) power bring-up
 *
 * MT6771 has no mainline SPM/power-domain driver, so the Mali GPU cannot be
 * powered up by the generic MediaTek power domain code.  Panfrost only needs
 * the MFG subsystem to be alive before it probes, so this driver:
 *
 *  - runs the vendor MTCMOS power-up sequence for MFG_ASYNC, MFG, CORE0,
 *    CORE1 and MFG_2D once at probe time,
 *  - registers a fixed clock for the MFG PLL/mux that the bootloader already
 *    programmed (the mfgcfg gates are left open).
 *
 * The domain is deliberately *not* modelled as a genpd: genpd is only useful
 * when the domain can be power cycled, and attaching the Mali device to a
 * hand-rolled genpd crashed the kernel inside genpd_add_device().  Since the
 * MTCMOS state is never changed after probe, plain clocks + a one-shot
 * power-up is all panfrost needs.  Panfrost still does its own GPU-internal
 * runtime PM.
 *
 * The register offsets and bit positions come from the vendor
 * clk-mt6771-pg.c driver.
 */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

/* SPM registers, relative to 0x10006000 */
#define SPM_PWR_STATUS		0x0180
#define SPM_PWR_STATUS_2ND	0x0184

/* MTCMOS control bits */
#define PWR_RST_B		BIT(0)
#define PWR_ISO			BIT(1)
#define PWR_ON			BIT(2)
#define PWR_ON_2ND		BIT(3)
#define PWR_CLK_DIS		BIT(4)
#define SRAM_PDN		BIT(8)
#define SRAM_PDN_ACK		BIT(12)

/* MFG sub-domain power control registers */
#define MFG_CORE1_PWR_CON	0x0310
#define MFG_ASYNC_PWR_CON	0x0334
#define MFG_PWR_CON		0x0338
#define MFG_2D_PWR_CON		0x0348
#define MFG_CORE0_PWR_CON	0x034c

/* Power status bits in PWR_STATUS / PWR_STATUS_2ND */
#define MFG_CORE0_STA		BIT(7)
#define MFG_CORE1_STA		BIT(20)
#define MFG_2D_STA		BIT(22)
#define MFG_ASYNC_STA		BIT(23)
#define MFG_STA			BIT(4)

#define MTCMOS_POLL_US		10
#define MTCMOS_TIMEOUT_US	200000

struct mt6771_mfg {
	struct device *dev;
	void __iomem *spm;
};

static int mt6771_mtcmos_on(struct mt6771_mfg *mfg, u32 reg, u32 sta)
{
	void __iomem *con = mfg->spm + reg;
	u32 val;
	int ret;

	/*
	 * Request power.  The vendor writes PWR_ON and PWR_ON_2ND in two
	 * separate accesses; keep that order.
	 */
	val = readl(con) | PWR_ON;
	writel(val, con);
	writel(readl(con) | PWR_ON_2ND, con);

	ret = readl_poll_timeout(mfg->spm + SPM_PWR_STATUS, val,
				 val & sta, MTCMOS_POLL_US, MTCMOS_TIMEOUT_US);
	if (ret)
		return ret;

	ret = readl_poll_timeout(mfg->spm + SPM_PWR_STATUS_2ND, val,
				 val & sta, MTCMOS_POLL_US, MTCMOS_TIMEOUT_US);
	if (ret)
		return ret;

	/* Release clock gating and isolation, then deassert reset. */
	val = readl(con) & ~(PWR_CLK_DIS | PWR_ISO);
	writel(val, con);
	writel(val | PWR_RST_B, con);

	/* Take the SRAMs out of power-down. */
	val = readl(con) & ~SRAM_PDN;
	writel(val, con);

	ret = readl_poll_timeout(con, val, !(val & SRAM_PDN_ACK),
				 MTCMOS_POLL_US, MTCMOS_TIMEOUT_US);
	if (ret)
		dev_warn(mfg->dev, "SRAM PDN ack timeout (reg %08x)\n",
			 readl(con));

	return 0;
}


static int mt6771_mfg_power_up(struct mt6771_mfg *mfg)
{
	static const struct {
		u32 reg;
		u32 sta;
		const char *name;
	} seq[] = {
		{ MFG_ASYNC_PWR_CON, MFG_ASYNC_STA, "async" },
		{ MFG_PWR_CON,       MFG_STA,       "mfg"   },
		{ MFG_CORE0_PWR_CON, MFG_CORE0_STA, "core0" },
		{ MFG_CORE1_PWR_CON, MFG_CORE1_STA, "core1" },
		{ MFG_2D_PWR_CON,    MFG_2D_STA,    "core2" },
	};
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(seq); i++) {
		ret = mt6771_mtcmos_on(mfg, seq[i].reg, seq[i].sta);
		if (ret) {
			dev_err(mfg->dev, "MTCMOS %s power up failed: %d\n",
				seq[i].name, ret);
			return ret;
		}
	}

	return 0;
}

static int mt6771_mfg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6771_mfg *mfg;
	struct clk_hw *clk;
	u32 rate = 650000000;
	int ret;

	mfg = devm_kzalloc(dev, sizeof(*mfg), GFP_KERNEL);
	if (!mfg)
		return -ENOMEM;

	mfg->dev = dev;
	mfg->spm = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mfg->spm))
		return PTR_ERR(mfg->spm);

	of_property_read_u32(dev->of_node, "clock-frequency", &rate);

	/*
	 * Power the subsystem up *before* publishing the clock.  The clock is a
	 * hard dependency of the Mali node, so the clock provider showing up is
	 * what releases panfrost's deferred probe; the MTCMOS domains must
	 * already be on by then.
	 */
	ret = mt6771_mfg_power_up(mfg);
	if (ret)
		return ret;

	clk = devm_clk_hw_register_fixed_rate(dev, "mfg", NULL, 0, rate);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "failed to register mfg clock\n");

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get, clk);
	if (ret)
		return ret;

	dev_info(dev, "MFG powered up at %u Hz\n", rate);

	return 0;
}

static const struct of_device_id mt6771_mfg_of_match[] = {
	{ .compatible = "mediatek,mt6771-mfg" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6771_mfg_of_match);

static struct platform_driver mt6771_mfg_driver = {
	.probe = mt6771_mfg_probe,
	.driver = {
		.name = "mt6771-mfg",
		.of_match_table = mt6771_mfg_of_match,
	},
};
module_platform_driver(mt6771_mfg_driver);

MODULE_DESCRIPTION("MediaTek MT6771 MFG (Mali GPU) power bring-up");
MODULE_LICENSE("GPL");
