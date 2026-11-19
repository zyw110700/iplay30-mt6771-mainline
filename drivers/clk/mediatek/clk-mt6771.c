// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6771 clock support (bring-up subset)
 *
 * The MT6771 vendor DT describes the INFRA_AO clock controller as
 * "mediatek,infracfg_ao" with #clock-cells = 1.  Only the gate clocks
 * needed during early bring-up are registered here; IDs, register
 * offsets and gate bits come from the vendor clk-mt6771.c driver.
 *
 * The vendor gate parents live in topckgen (uart_sel, msdc50_hclk_sel,
 * msdc50_0_sel, ...).  Those muxes are left in the state programmed by
 * the bootloader; the gates are registered against the fixed 26 MHz
 * clock so that the UART baud clock stays correct.
 */

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#include "clk-gate.h"
#include "clk-mtk.h"

/* INFRA_AO clock IDs (vendor DT binding, include/dt-bindings/clock/mt6771-clk.h) */
#define CLK_INFRA_UART0			21
#define CLK_INFRA_UART1			22
#define CLK_INFRA_UART2			23
#define CLK_INFRA_UART3			24
#define CLK_INFRA_MSDC0			29
#define CLK_INFRA_MSDC1			30
#define CLK_INFRA_MSDC2			31
#define CLK_INFRA_MSDC0_SCK		32
#define CLK_INFRA_MSDC1_SCK		41
#define CLK_INFRA_MSDC2_SCK		42
#define CLK_INFRA_AP_DMA		43
#define CLK_INFRA_I2C4			58
#define CLK_INFRA_UNIPRO_SCK		62
#define CLK_INFRA_I2C5			69
#define CLK_INFRA_I2C5_ARBITER		70
#define CLK_INFRA_I2C5_IMM		71
#define CLK_INFRA_I2C1_ARBITER		72
#define CLK_INFRA_I2C1_IMM		73
#define CLK_INFRA_I2C2_ARBITER		74
#define CLK_INFRA_I2C2_IMM		75
#define CLK_INFRA_I2C6			88
#define CLK_INFRA_USB			91
#define CLK_INFRA_I2C7			99
#define CLK_INFRA_I2C8			100
#define CLK_INFRA_ICUSB			8
#define CLK_INFRA_BTIF			27
#define CLK_INFRA_THERM			10
#define CLK_INFRA_AUXADC		36
#define CLK_INFRA_I2C0			11
#define CLK_INFRA_I2C1			12
#define CLK_INFRA_I2C2			13
#define CLK_INFRA_I2C3			14

/* The vendor clock namespace is sparse and extends past 200. */
#define CLK_INFRA_NR_CLK		256

static const struct mtk_gate_regs infra0_cg_regs = {
	.set_ofs = 0x80,
	.clr_ofs = 0x84,
	.sta_ofs = 0x90,
};

static const struct mtk_gate_regs infra1_cg_regs = {
	.set_ofs = 0x88,
	.clr_ofs = 0x8c,
	.sta_ofs = 0x94,
};

static const struct mtk_gate_regs infra2_cg_regs = {
	.set_ofs = 0xa4,
	.clr_ofs = 0xa8,
	.sta_ofs = 0xac,
};

static const struct mtk_gate_regs infra3_cg_regs = {
	.set_ofs = 0xc0,
	.clr_ofs = 0xc4,
	.sta_ofs = 0xc8,
};

#define GATE_INFRA0(_id, _name, _shift)				\
	GATE_MTK(_id, _name, "clk26m", &infra0_cg_regs, _shift,	\
		 &mtk_clk_gate_ops_setclr)
#define GATE_INFRA1(_id, _name, _shift)				\
	GATE_MTK(_id, _name, "clk26m", &infra1_cg_regs, _shift,	\
		 &mtk_clk_gate_ops_setclr)
#define GATE_INFRA2(_id, _name, _shift)				\
	GATE_MTK(_id, _name, "clk26m", &infra2_cg_regs, _shift,	\
		 &mtk_clk_gate_ops_setclr)
#define GATE_INFRA3(_id, _name, _shift)				\
	GATE_MTK(_id, _name, "clk26m", &infra3_cg_regs, _shift,	\
		 &mtk_clk_gate_ops_setclr)

static const struct mtk_gate infra_clks[] = {
	GATE_INFRA0(CLK_INFRA_ICUSB, "infra_icusb", 8),
	GATE_INFRA0(CLK_INFRA_BTIF, "infra_btif", 31),
	GATE_INFRA0(CLK_INFRA_THERM, "infra_therm", 10),
	GATE_INFRA0(CLK_INFRA_UART0, "infra_uart0", 22),
	GATE_INFRA0(CLK_INFRA_UART1, "infra_uart1", 23),
	GATE_INFRA0(CLK_INFRA_UART2, "infra_uart2", 24),
	GATE_INFRA0(CLK_INFRA_UART3, "infra_uart3", 25),
	GATE_INFRA0(CLK_INFRA_I2C0, "infra_i2c0", 11),
	GATE_INFRA0(CLK_INFRA_I2C1, "infra_i2c1", 12),
	GATE_INFRA0(CLK_INFRA_I2C2, "infra_i2c2", 13),
	GATE_INFRA0(CLK_INFRA_I2C3, "infra_i2c3", 14),
	GATE_INFRA1(CLK_INFRA_MSDC0, "infra_msdc0", 2),
	GATE_INFRA1(CLK_INFRA_MSDC1, "infra_msdc1", 4),
	GATE_INFRA1(CLK_INFRA_MSDC2, "infra_msdc2", 5),
	GATE_INFRA1(CLK_INFRA_MSDC0_SCK, "infra_msdc0_sck", 6),
	GATE_INFRA1(CLK_INFRA_MSDC1_SCK, "infra_msdc1_sck", 16),
	GATE_INFRA1(CLK_INFRA_MSDC2_SCK, "infra_msdc2_sck", 17),
	GATE_INFRA1(CLK_INFRA_AP_DMA, "infra_apdma", 18),
	GATE_INFRA1(CLK_INFRA_AUXADC, "infra_auxadc", 10),
	GATE_INFRA2(CLK_INFRA_UNIPRO_SCK, "infra_unipro_sck", 11),
	GATE_INFRA2(CLK_INFRA_USB, "infra_usb", 1),
	GATE_INFRA2(CLK_INFRA_I2C4, "infra_i2c4", 7),
	GATE_INFRA2(CLK_INFRA_I2C5, "infra_i2c5", 18),
	GATE_INFRA2(CLK_INFRA_I2C5_ARBITER, "infra_i2c5_arbiter", 19),
	GATE_INFRA2(CLK_INFRA_I2C5_IMM, "infra_i2c5_imm", 20),
	GATE_INFRA2(CLK_INFRA_I2C1_ARBITER, "infra_i2c1_arbiter", 21),
	GATE_INFRA2(CLK_INFRA_I2C1_IMM, "infra_i2c1_imm", 22),
	GATE_INFRA2(CLK_INFRA_I2C2_ARBITER, "infra_i2c2_arbiter", 23),
	GATE_INFRA2(CLK_INFRA_I2C2_IMM, "infra_i2c2_imm", 24),
	GATE_INFRA3(CLK_INFRA_I2C6, "infra_i2c6", 6),
	GATE_INFRA3(CLK_INFRA_I2C7, "infra_i2c7", 22),
	GATE_INFRA3(CLK_INFRA_I2C8, "infra_i2c8", 23),
};

static struct device *mt6771_diag_dev;
static void mt6771_diag_dump_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(mt6771_diag_work, mt6771_diag_dump_work_fn);

static void mt6771_dump_msdc_node(struct device *dev, const char *path)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct device_link *link;
	const char *status = NULL;

	np = of_find_node_by_path(path);
	if (!np) {
		dev_info(dev, "DIAG: %s: node not found\n", path);
		return;
	}

	of_property_read_string(np, "status", &status);
	dev_info(dev, "DIAG: %s: available=%d status=%s\n", path,
		 of_device_is_available(np), status ? status : "(none)");

	pdev = of_find_device_by_node(np);
	if (!pdev) {
		dev_info(dev, "DIAG: %s: no platform device\n", path);
	} else {
		dev_info(dev, "DIAG: %s: dev=%s driver=%s links_status=%d\n",
			 path, dev_name(&pdev->dev),
			 pdev->dev.driver ? pdev->dev.driver->name : "(unbound)",
			 pdev->dev.links.status);
		list_for_each_entry(link, &pdev->dev.links.suppliers, c_node)
			dev_info(dev,
				 "DIAG: %s: supplier=%s link_status=%d flags=0x%x\n",
				 path, dev_name(link->supplier), link->status,
				 link->flags);
	}
	of_node_put(np);
}

static void mt6771_dump_dt(struct device *dev)
{
	const char *model = NULL;

	if (of_root)
		of_property_read_string(of_root, "model", &model);
	dev_info(dev, "DIAG: of_root model=%s\n", model ? model : "(none)");
	mt6771_dump_msdc_node(dev, "/msdc@11230000");
	mt6771_dump_msdc_node(dev, "/msdc@11240000");
}

static void mt6771_diag_dump_work_fn(struct work_struct *work)
{
	mt6771_dump_dt(mt6771_diag_dev);
}

static void mt6771_force_i2c_parent_clk26m(struct device *dev)
{
	void __iomem *base;
	u32 sel;

	base = ioremap(0x10000000, 0x1000);
	if (!base) {
		dev_warn(dev, "cannot map topckgen, I2C parent left as-is\n");
		return;
	}

	sel = readl(base + 0xc0) & 0x3;
	if (sel != 0) {
		/* CK_CFG_8_CLR: clear i2c_sel[1:0], CK_CFG_8_SET: parent 0 */
		writel(0x3, base + 0xc8);
		writel(0x0, base + 0xc4);
		/* CLK_CFG_UPDATE1 bit 1 commits TOP_MUX_I2C */
		writel(BIT(1), base + 0x8);
		readl(base + 0x8);
	}

	dev_info(dev, "i2c_sel was %u, now %u (clk26m)\n", sel,
		 readl(base + 0xc0) & 0x3);
	iounmap(base);
}

static int clk_mt6771_infra_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data;
	struct device_node *node = pdev->dev.of_node;
	int r;

	/*
	 * The gate clocks below are modelled as children of clk26m, but LK
	 * leaves i2c_sel on univpll_d5_d2 so the real bus clock is many times
	 * faster than what i2c-mt65xx computes.  Switch the mux to clk26m so
	 * hardware and model agree (26 MHz / clock-div = 5.2 MHz).
	 */
	mt6771_force_i2c_parent_clk26m(&pdev->dev);

	clk_data = mtk_alloc_clk_data(CLK_INFRA_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	r = mtk_clk_register_gates(&pdev->dev, node, infra_clks,
				   ARRAY_SIZE(infra_clks), clk_data);
	if (r)
		return r;

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		dev_err(&pdev->dev, "could not register clock provider: %d\n", r);
	else
		dev_info(&pdev->dev, "registered %zu INFRA_AO gate clocks\n",
			 ARRAY_SIZE(infra_clks));

	mt6771_diag_dev = &pdev->dev;
	mt6771_dump_dt(&pdev->dev);
	schedule_delayed_work(&mt6771_diag_work, msecs_to_jiffies(20000));

	return r;
}

static const struct of_device_id of_match_clk_mt6771[] = {
	{ .compatible = "mediatek,infracfg_ao" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6771);

static struct platform_driver clk_mt6771_drv = {
	.probe = clk_mt6771_infra_probe,
	.driver = {
		.name = "clk-mt6771-infra",
		.of_match_table = of_match_clk_mt6771,
	},
};

static int __init clk_mt6771_init(void)
{
	return platform_driver_register(&clk_mt6771_drv);
}
arch_initcall(clk_mt6771_init);

MODULE_DESCRIPTION("MediaTek MT6771 clock support (bring-up subset)");
MODULE_LICENSE("GPL");
