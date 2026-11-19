// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6771 CPU cluster clock support (ARMPLL_LL / ARMPLL_L + mcucfg mux)
 *
 * The vendor bootloader leaves both clusters running straight from the
 * ARMPLLs (~1.1 GHz little, ~1.2 GHz big).  This driver exposes, per
 * cluster, the ARMPLL as a rate-settable clock and the mcucfg clock mux as
 * the CPU clock itself, with the fixed 26 MHz clock as the second parent.
 * That is exactly the shape the generic mediatek-cpufreq driver expects:
 *
 *     1. clk_set_parent(cpu_clk, clk26m)   - mux the CPUs to 26 MHz
 *     2. clk_set_rate(armpll, target_hz)   - retune the now idle PLL
 *     3. clk_set_parent(cpu_clk, armpll)   - mux the CPUs back
 *
 * Register layout (from the vendor cpufreq platform code):
 *   apmixed 0x1000c000 + 0x204/0x214 : ARMPLL_LL/L CON1
 *        [21:0]  PCW (f_vco = PCW * 26 MHz / 2^14)
 *        [26:24] post divider select (0:/1 1:/2 2:/4 3:/8 else:/16)
 *        [31]    PLL enable / change strobe
 *   mcucfg  0x0c530000 + 0x7a0/0x7a8 : CKDIV1 for each cluster
 *        [21:17] divider select (8:/1 10:/2 11:/4)
 *        [10:9]  clock mux (0: 26 MHz, 1: ARMPLL, 2: MAINPLL, 3: UNIVPLL)
 */

#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define MT6771_APMIXED_BASE	0x1000c000
#define MT6771_MCUCFG_BASE	0x0c530000

#define MT6771_ARMPLL_LL	0x204
#define MT6771_ARMPLL_L		0x214
#define MT6771_CKDIV_LL		0x7a0
#define MT6771_CKDIV_L		0x7a8

#define PLL_CON1_PCW_MASK	GENMASK(21, 0)
#define PLL_CON1_POSDIV_MASK	GENMASK(26, 24)
#define PLL_CON1_EN		BIT(31)

#define CKDIV_DIV_SHIFT		17
#define CKDIV_DIV_MASK		GENMASK(21, 17)
#define CKDIV_MUX_SHIFT		9
#define CKDIV_MUX_MASK		GENMASK(10, 9)
#define CKDIV_MUX_CLK26M	0
#define CKDIV_MUX_ARMPLL	1

#define PLL_REF_HZ		26000000UL
#define PLL_PCW_SCALE		16384U		/* 2^14 */
#define PLL_VCO_MIN_HZ		1500000000ULL
#define PLL_VCO_MAX_HZ		3000000000ULL

enum {
	MT6771_CLK_CPU0_SEL,
	MT6771_CLK_CPU4_SEL,
	MT6771_CLK_ARMPLL_LL,
	MT6771_CLK_ARMPLL_L,
	MT6771_CLK_NR,
};

struct mt6771_cpupll {
	struct clk_hw hw;
	void __iomem *con1;
	void __iomem *ckdiv;
};

static inline struct mt6771_cpupll *to_cpupll(struct clk_hw *hw)
{
	return container_of(hw, struct mt6771_cpupll, hw);
}

static u32 mt6771_posdiv_sel(u32 div)
{
	switch (div) {
	case 1:		return 0;
	case 2:		return 1;
	case 4:		return 2;
	case 8:		return 3;
	default:	return 4;
	}
}

static u32 mt6771_posdiv_val(u32 sel)
{
	static const u32 divs[] = { 1, 2, 4, 8, 16 };

	return divs[sel < ARRAY_SIZE(divs) ? sel : ARRAY_SIZE(divs) - 1];
}

static u32 mt6771_clkdiv_sel(u32 div)
{
	switch (div) {
	case 1:		return 8;
	case 2:		return 10;
	default:	return 11;
	}
}

static u32 mt6771_clkdiv_val(u32 sel)
{
	switch (sel) {
	case 8:		return 1;
	case 10:	return 2;
	case 11:	return 4;
	default:	return 1;
	}
}

/*
 * Pick post divider / mcucfg divider so that the VCO stays inside the range
 * used by the vendor tables (1.5 - 3.0 GHz), then compute the PCW.  A few
 * neighbouring PCW values are tried so that the resulting rate rounds back
 * to the exact kHz that is requested (cpufreq compares table entries).
 */
static long mt6771_pll_calc(unsigned long rate, u32 *posdiv_ret,
			    u32 *clkdiv_ret, u32 *pcw_ret)
{
	static const u32 posdivs[] = { 1, 2, 4, 8, 16 };
	static const u32 clkdivs[] = { 1, 2, 4 };
	unsigned long want_khz = DIV_ROUND_CLOSEST(rate, 1000);
	unsigned int c, p, i;

	for (c = 0; c < ARRAY_SIZE(clkdivs); c++) {
		for (p = 0; p < ARRAY_SIZE(posdivs); p++) {
			u64 vco = (u64)rate * posdivs[p] * clkdivs[c];
			u64 div = (u64)PLL_PCW_SCALE * posdivs[p] * clkdivs[c];
			u32 pcw;

			if (vco < PLL_VCO_MIN_HZ || vco > PLL_VCO_MAX_HZ)
				continue;

			pcw = DIV_ROUND_CLOSEST_ULL(vco * PLL_PCW_SCALE,
						    PLL_REF_HZ);

			for (i = 0; i < 3; i++) {
				u32 cand = pcw + i - 1;
				unsigned long out;

				if (cand > PLL_CON1_PCW_MASK)
					continue;

				out = DIV_ROUND_CLOSEST_ULL((u64)cand *
							    PLL_REF_HZ, div);
				if (DIV_ROUND_CLOSEST(out, 1000) != want_khz)
					continue;

				*posdiv_ret = posdivs[p];
				*clkdiv_ret = clkdivs[c];
				*pcw_ret = cand;
				return out;
			}
		}
	}

	return -EINVAL;
}

static unsigned long mt6771_pll_recalc_rate(struct clk_hw *hw,
					    unsigned long parent_rate)
{
	struct mt6771_cpupll *pll = to_cpupll(hw);
	u32 con1 = readl(pll->con1);
	u32 ckdiv = readl(pll->ckdiv);
	u32 pcw = con1 & PLL_CON1_PCW_MASK;
	u32 posdiv = mt6771_posdiv_val((con1 & PLL_CON1_POSDIV_MASK) >> 24);
	u32 clkdiv = mt6771_clkdiv_val((ckdiv & CKDIV_DIV_MASK) >>
				       CKDIV_DIV_SHIFT);

	return DIV_ROUND_CLOSEST_ULL((u64)pcw * PLL_REF_HZ,
				     (u64)PLL_PCW_SCALE * posdiv * clkdiv);
}

static int mt6771_pll_determine_rate(struct clk_hw *hw,
				     struct clk_rate_request *req)
{
	u32 posdiv, clkdiv, pcw;
	long rate;

	rate = mt6771_pll_calc(req->rate, &posdiv, &clkdiv, &pcw);
	if (rate < 0)
		return rate;

	req->rate = rate;

	return 0;
}

static int mt6771_pll_set_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long parent_rate)
{
	struct mt6771_cpupll *pll = to_cpupll(hw);
	u32 posdiv, clkdiv, pcw, val;

	if (mt6771_pll_calc(rate, &posdiv, &clkdiv, &pcw) < 0)
		return -EINVAL;

	/* The caller has muxed the CPUs away from this PLL already. */
	val = readl(pll->ckdiv);
	val &= ~CKDIV_DIV_MASK;
	val |= mt6771_clkdiv_sel(clkdiv) << CKDIV_DIV_SHIFT;
	writel(val, pll->ckdiv);

	val = readl(pll->con1);
	val &= ~PLL_CON1_POSDIV_MASK;
	val |= mt6771_posdiv_sel(posdiv) << 24;
	writel(val, pll->con1);
	udelay(1);

	/* Latch the new PCW; bit 31 doubles as the change strobe. */
	val &= ~PLL_CON1_PCW_MASK;
	val |= pcw | PLL_CON1_EN;
	writel(val, pll->con1);
	udelay(20);

	return 0;
}

static const struct clk_ops mt6771_pll_ops = {
	.recalc_rate	= mt6771_pll_recalc_rate,
	.determine_rate	= mt6771_pll_determine_rate,
	.set_rate	= mt6771_pll_set_rate,
};

static int mt6771_cpupll_probe(struct platform_device *pdev)
{
	static const char * const cpu0_parents[] = { "clk26m", "armpll_ll" };
	static const char * const cpu4_parents[] = { "clk26m", "armpll_l" };
	struct clk_hw_onecell_data *clk_data;
	struct device *dev = &pdev->dev;
	struct mt6771_cpupll *ll, *l;
	struct clk_init_data init = {};
	void __iomem *apmixed, *mcucfg;
	int ret;

	apmixed = devm_ioremap(dev, MT6771_APMIXED_BASE, 0x1000);
	mcucfg = devm_ioremap(dev, MT6771_MCUCFG_BASE, 0x2000);
	if (!apmixed || !mcucfg)
		return -ENOMEM;

	clk_data = devm_kzalloc(dev, struct_size(clk_data, hws,
						 MT6771_CLK_NR), GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;
	clk_data->num = MT6771_CLK_NR;

	ll = devm_kzalloc(dev, sizeof(*ll), GFP_KERNEL);
	l = devm_kzalloc(dev, sizeof(*l), GFP_KERNEL);
	if (!ll || !l)
		return -ENOMEM;

	ll->con1 = apmixed + MT6771_ARMPLL_LL;
	ll->ckdiv = mcucfg + MT6771_CKDIV_LL;
	l->con1 = apmixed + MT6771_ARMPLL_L;
	l->ckdiv = mcucfg + MT6771_CKDIV_L;

	init.ops = &mt6771_pll_ops;
	init.flags = CLK_IS_CRITICAL;

	init.name = "armpll_ll";
	ll->hw.init = &init;
	ret = clk_hw_register(dev, &ll->hw);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register armpll_ll\n");
	clk_data->hws[MT6771_CLK_ARMPLL_LL] = &ll->hw;

	init.name = "armpll_l";
	l->hw.init = &init;
	ret = clk_hw_register(dev, &l->hw);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register armpll_l\n");
	clk_data->hws[MT6771_CLK_ARMPLL_L] = &l->hw;

	clk_data->hws[MT6771_CLK_CPU0_SEL] =
		clk_hw_register_mux(dev, "cpu0_sel", cpu0_parents, 2,
				    CLK_SET_RATE_NO_REPARENT,
				    mcucfg + MT6771_CKDIV_LL,
				    CKDIV_MUX_SHIFT, 2, 0, NULL);
	if (IS_ERR(clk_data->hws[MT6771_CLK_CPU0_SEL]))
		return dev_err_probe(dev, PTR_ERR(clk_data->hws[MT6771_CLK_CPU0_SEL]),
				     "failed to register cpu0_sel\n");

	clk_data->hws[MT6771_CLK_CPU4_SEL] =
		clk_hw_register_mux(dev, "cpu4_sel", cpu4_parents, 2,
				    CLK_SET_RATE_NO_REPARENT,
				    mcucfg + MT6771_CKDIV_L,
				    CKDIV_MUX_SHIFT, 2, 0, NULL);
	if (IS_ERR(clk_data->hws[MT6771_CLK_CPU4_SEL]))
		return dev_err_probe(dev, PTR_ERR(clk_data->hws[MT6771_CLK_CPU4_SEL]),
				     "failed to register cpu4_sel\n");

	ret = of_clk_add_hw_provider(dev->of_node, of_clk_hw_onecell_get,
				     clk_data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add clock provider\n");

	dev_info(dev, "armpll_ll at %lu Hz, armpll_l at %lu Hz\n",
		 clk_hw_get_rate(&ll->hw), clk_hw_get_rate(&l->hw));

	return 0;
}

static const struct of_device_id mt6771_cpupll_of_match[] = {
	{ .compatible = "mediatek,mt6771-cpupll" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6771_cpupll_of_match);

static struct platform_driver mt6771_cpupll_driver = {
	.driver = {
		.name = "mt6771-cpupll",
		.of_match_table = mt6771_cpupll_of_match,
	},
	.probe = mt6771_cpupll_probe,
};
module_platform_driver(mt6771_cpupll_driver);

MODULE_DESCRIPTION("MediaTek MT6771 CPU cluster clock support");
MODULE_LICENSE("GPL");
