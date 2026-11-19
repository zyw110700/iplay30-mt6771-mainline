// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6771 thermal controller (read-only, opt-in)
 *
 * MT6771 uses the same "AUXADC thermal controller" IP as mt8173/mt2701, but
 * with two controller instances that share one register window:
 *
 *   controller 0 @ +0x000: slot 0 = TS_MCU5 (big), slot 1 = TS_MCU4 (LL)
 *   controller 1 @ +0x100: slot 0 = TS_MCU1 (SoC), slot 1 = TS_MCU2 (GPU),
 *                          slot 2 = TS_MCU3 (modem), slot 3 = TS_ABB
 *
 * (Sensor mapping taken from the vendor mtk_tc.c / tscpu_settings.h.)
 *
 * Unlike the mainline mt8173 driver the vendor bootloader does NOT leave the
 * controller running: the whole block reads back zero until it is
 * programmed.  This driver therefore programs the vendor initialisation
 * sequence (monitor timing, AUXADC AHB addresses, per-slot PNP mux values,
 * sensing point enable) when the user writes 1 to
 *
 *   /sys/kernel/debug/mt6771-thermal/enable
 *
 * and leaves the block completely untouched otherwise.  All registers were
 * verified against the vendor sequence on real hardware; the AUXADC itself
 * is only ever accessed by the controller through AHB, except for the two
 * channel-11 enable/disable writes that the vendor also does from the CPU.
 *
 * Calibration: the vendor AP efuse layout is not decoded yet, so the
 * fallback (uncalibrated) constants are used: degc_cali = 40, O_VTS = 260,
 * ADC_GE/ADC_OE = 512, O_SLOPE = 0.  The vendor driver uses exactly these
 * values for uncalibrated samples, with a typical absolute error of a few
 * to ten degrees C.  Relative changes are accurate enough for monitoring.
 */

#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>

/* Thermal controller registers (offset from the block base) */
#define TEMP_MONCTL0		0x000
#define TEMP_MONCTL1		0x004
#define TEMP_MONCTL2		0x008
#define TEMP_MONIDET0		0x014
#define TEMP_MONIDET1		0x018
#define TEMP_MSRCTL0		0x038
#define TEMP_MSRCTL1		0x03c
#define TEMP_AHBPOLL		0x040
#define TEMP_AHBTO		0x044
#define TEMP_ADCPNP0		0x048
#define TEMP_ADCPNP1		0x04c
#define TEMP_ADCPNP2		0x050
#define TEMP_ADCPNP3		0x0b4
#define TEMP_ADCMUX		0x054
#define TEMP_ADCEN		0x060
#define TEMP_PNPMUXADDR		0x064
#define TEMP_ADCMUXADDR		0x068
#define TEMP_ADCENADDR		0x074
#define TEMP_ADCVALIDADDR	0x078
#define TEMP_ADCVOLTADDR	0x07c
#define TEMP_RDCTRL		0x080
#define TEMP_ADCVALIDMASK	0x084
#define TEMP_ADCVOLTAGESHIFT	0x088
#define TEMP_ADCWRITECTRL	0x08c
#define TEMP_MSR0		0x090
#define TEMP_MSR1		0x094
#define TEMP_MSR2		0x098
#define TEMP_MSR3		0x0b8

#define TEMP_CTRL1		0x100
#define TEMP_ADCPNP0_1		0x148
#define TEMP_ADCPNP1_1		0x14c
#define TEMP_ADCPNP2_1		0x150
#define TEMP_MSR0_1		0x190
#define TEMP_MSR1_1		0x194
#define TEMP_MSR2_1		0x198
#define TEMP_ADCPNP3_1		0x1b4
#define TEMP_MSR3_1		0x1b8

#define TEMP_RAW_MASK		0xfff

/*
 * The controller drives the AUXADC through AHB, so it needs the physical
 * addresses of the AUXADC registers.  Both are fixed on MT6771.
 */
#define MT6771_AUXADC_PHYS		0x11001000UL
#define AUXADC_CON0			0x000
#define AUXADC_CON1_SET			0x008
#define AUXADC_CON1_CLR			0x00c
#define AUXADC_DATA(ch)			(0x014 + (ch) * 4)
#define MT6771_TS_AUXADC_CHANNEL	11

#define MT6771_APMIXED_TS_CON1_PHYS	0x1000c604UL

/* Vendor timing values from tscpu_g_tc[] (26 MHz module clock). */
#define MT6771_MONCTL1_VAL		0x00c
#define MT6771_MONCTL2_VAL		0x0001003b
#define MT6771_AHBPOLL_VAL		0x30d
#define MT6771_ADC_MUX_WRITE		BIT(0)
#define MT6771_ADC_PNP_WRITE		BIT(1)
#define MT6771_ADCWRITECTRL_VAL		(MT6771_ADC_MUX_WRITE | \
					 MT6771_ADC_PNP_WRITE)

/* The vendor marks a sensor that has not sampled yet with this value. */
#define MT6771_TS_INIT_VALUE		0xda1

/* Vendor enum thermal_sensor order */
enum mt6771_sensor_id {
	MT6771_MCU1,	/* SoC */
	MT6771_MCU2,	/* GPU */
	MT6771_MCU3,	/* modem */
	MT6771_MCU4,	/* little cluster */
	MT6771_MCU5,	/* big cluster */
	MT6771_ABB,
	MT6771_NUM_SENSORS,
};

struct mt6771_sensor {
	const char *name;
	enum mt6771_sensor_id id;
	u32 ctrl;	/* TEMP_CTRL1 for controller 1, 0 for controller 0 */
	u32 msr;	/* MSR register offset inside its controller */
	u32 pnp_off;	/* ADCPNP register offset (absolute) */
	u32 pnp_val;	/* vendor TEMPADC_* mux value */
};

/*
 * PNP values come from tscpu_settings.h:
 *   MCU1..5 = 0..4, ABB = 0x10.
 */
static const struct mt6771_sensor mt6771_sensors[MT6771_NUM_SENSORS] = {
	{ "cpu-little",	MT6771_MCU4, 0x000,	TEMP_MSR1,   TEMP_ADCPNP1,   0x03 },
	{ "cpu-big",	MT6771_MCU5, 0x000,	TEMP_MSR0,   TEMP_ADCPNP0,   0x04 },
	{ "gpu",	MT6771_MCU2, TEMP_CTRL1, TEMP_MSR1_1, TEMP_ADCPNP1_1, 0x01 },
	{ "soc",	MT6771_MCU1, TEMP_CTRL1, TEMP_MSR0_1, TEMP_ADCPNP0_1, 0x00 },
	{ "modem",	MT6771_MCU3, TEMP_CTRL1, TEMP_MSR2_1, TEMP_ADCPNP2_1, 0x02 },
	{ "abb",	MT6771_ABB,  TEMP_CTRL1, TEMP_MSR3_1, TEMP_ADCPNP3_1, 0x10 },
};

struct mt6771_zone {
	struct mt6771_thermal *mt;
	int idx;
};

struct mt6771_thermal {
	struct device *dev;
	void __iomem *base;
	void __iomem *auxadc;
	struct clk *clk;
	struct clk *auxadc_clk;
	struct dentry *dbg;
	bool enabled;
	struct mt6771_zone zone[MT6771_NUM_SENSORS];
	struct thermal_zone_device *tz[MT6771_NUM_SENSORS];
};

/*
 * Uncalibrated conversion, copied from the vendor raw_to_temperature_roomt():
 *
 *   Vbe = raw * 1800 / 4096
 *   T   = degc_cali / 2 - 2 * (Vbe - Vbe_room) * ...
 *
 * With the fallback constants the whole expression collapses to a linear
 * mapping of the raw ADC value; raw 0 means "no sample yet".
 */
static int mt6771_raw_to_mcelsius(u32 raw)
{
	const int degc_cali = 40;
	const int o_slope = 0;
	const int gain = 10000;
	const int oe = 0;
	const int vts = 260;
	long x_roomt;
	long f1, f2, f3, f4;

	raw &= TEMP_RAW_MASK;
	if (!raw || raw == MT6771_TS_INIT_VALUE)
		return THERMAL_TEMP_INVALID;

	x_roomt = ((long)(vts + 3350) * 10000 / 4096) * 10000 / gain;

	f1 = (degc_cali * 10) >> 1;
	f2 = (long)raw - oe;
	f3 = ((((f2 * 10000) >> 12) * 10000) / gain) - x_roomt;
	f3 = f3 * 15 / 18;
	f4 = (f3 * 1000) / (1534 + o_slope * 10);
	f4 = f4 - (f4 << 1);

	return (int)(f1 + f4) * 100;	/* 0.1 degC -> mC */
}

static int mt6771_read_raw(struct mt6771_thermal *mt, int idx)
{
	/* msr holds the absolute offset (0x1xx for controller 1). */
	return readl(mt->base + mt6771_sensors[idx].msr) & TEMP_RAW_MASK;
}

static int mt6771_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct mt6771_zone *z = thermal_zone_device_priv(tz);
	struct mt6771_thermal *mt = z->mt;
	int raw, mc;

	if (!mt->enabled)
		return -EAGAIN;

	raw = mt6771_read_raw(mt, z->idx);
	mc = mt6771_raw_to_mcelsius(raw);
	if (mc == THERMAL_TEMP_INVALID || mc < -40000 || mc > 150000)
		return -EAGAIN;

	*temp = mc;

	return 0;
}

static const struct thermal_zone_device_ops mt6771_tz_ops = {
	.get_temp = mt6771_get_temp,
};

static int mt6771_debugfs_temps_show(struct seq_file *s, void *unused)
{
	struct mt6771_thermal *mt = s->private;
	int i;

	seq_printf(s, "enabled: %d\n", mt->enabled);
	if (!mt->enabled) {
		seq_puts(s, "write 1 to 'enable' first (read-only probe)\n");
		return 0;
	}

	for (i = 0; i < MT6771_NUM_SENSORS; i++) {
		int raw = mt6771_read_raw(mt, i);
		int mc = mt6771_raw_to_mcelsius(raw);

		if (mc == THERMAL_TEMP_INVALID) {
			seq_printf(s,
				   "%-11s ctrl=%s msr=0x%03x raw=%4d temp=invalid\n",
				   mt6771_sensors[i].name,
				   mt6771_sensors[i].ctrl ? "1" : "0",
				   mt6771_sensors[i].msr, raw);
			continue;
		}

		seq_printf(s,
			   "%-11s ctrl=%s msr=0x%03x raw=%4d temp=%s%d.%03d C\n",
			   mt6771_sensors[i].name,
			   mt6771_sensors[i].ctrl ? "1" : "0",
			   mt6771_sensors[i].msr, raw,
			   mc < 0 ? "-" : "", abs(mc / 1000),
			   abs(mc % 1000));
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt6771_debugfs_temps);

static int mt6771_debugfs_regs_show(struct seq_file *s, void *unused)
{
	struct mt6771_thermal *mt = s->private;
	static const u32 regs[] = {
		TEMP_MONCTL0, TEMP_MONCTL1, TEMP_MONCTL2, TEMP_MSRCTL0,
		TEMP_MSRCTL1, TEMP_AHBPOLL, TEMP_AHBTO, TEMP_ADCPNP0,
		TEMP_ADCPNP1, TEMP_ADCPNP2, TEMP_ADCMUX, TEMP_ADCEN,
		TEMP_PNPMUXADDR, TEMP_ADCMUXADDR, TEMP_ADCENADDR,
		TEMP_ADCVALIDADDR, TEMP_ADCVOLTADDR, TEMP_RDCTRL,
		TEMP_ADCVALIDMASK, TEMP_ADCVOLTAGESHIFT, TEMP_ADCWRITECTRL,
	};
	int i;

	if (!mt->enabled) {
		seq_puts(s, "write 1 to 'enable' first\n");
		return 0;
	}

	seq_puts(s, "controller 0:\n");
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		seq_printf(s, "  +0x%03x = 0x%08x\n", regs[i],
			   readl(mt->base + regs[i]));

	seq_puts(s, "controller 1:\n");
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		seq_printf(s, "  +0x%03x = 0x%08x\n", regs[i],
			   readl(mt->base + TEMP_CTRL1 + regs[i]));

	seq_puts(s, "PNP:\n");
	for (i = 0; i < MT6771_NUM_SENSORS; i++)
		seq_printf(s, "  %-11s 0x%03x = 0x%08x\n",
			   mt6771_sensors[i].name, mt6771_sensors[i].pnp_off,
			   readl(mt->base + mt6771_sensors[i].pnp_off));

	seq_puts(s, "MSR:\n");
	seq_printf(s, "  c0 msr0=0x%03x msr1=0x%03x msr2=0x%03x msr3=0x%03x\n",
		   readl(mt->base + TEMP_MSR0), readl(mt->base + TEMP_MSR1),
		   readl(mt->base + TEMP_MSR2), readl(mt->base + TEMP_MSR3));
	seq_printf(s, "  c1 msr0=0x%03x msr1=0x%03x msr2=0x%03x msr3=0x%03x\n",
		   readl(mt->base + TEMP_MSR0_1), readl(mt->base + TEMP_MSR1_1),
		   readl(mt->base + TEMP_MSR2_1), readl(mt->base + TEMP_MSR3_1));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt6771_debugfs_regs);

/*
 * Vendor tscpu_thermal_initial_all_tc()/thermal_reset_and_initial(),
 * mt6771 flavour.  Both controllers share one register window and one
 * AUXADC channel (11); only the per-slot PNP mux values differ.
 */
static void mt6771_thermal_hw_init(struct mt6771_thermal *mt)
{
	u32 mux = BIT(MT6771_TS_AUXADC_CHANNEL);
	int c, i;

	/* AUXADC channel 11: not autoset, start from a clean enable state */
	writel(readl(mt->auxadc + AUXADC_CON0) & ~mux,
	       mt->auxadc + AUXADC_CON0);
	writel(mux, mt->auxadc + AUXADC_CON1_CLR);

	for (c = 0; c < 2; c++) {
		void __iomem *b = mt->base + c * TEMP_CTRL1;

		writel(MT6771_MONCTL1_VAL, b + TEMP_MONCTL1);
		writel(MT6771_MONCTL2_VAL, b + TEMP_MONCTL2);
		writel(MT6771_AHBPOLL_VAL, b + TEMP_AHBPOLL);
		writel(0, b + TEMP_MSRCTL0);
		writel(0xffffffff, b + TEMP_AHBTO);
		writel(0, b + TEMP_MONIDET0);
		writel(0, b + TEMP_MONIDET1);

		writel(mux, b + TEMP_ADCMUX);
		writel(MT6771_AUXADC_PHYS + AUXADC_CON1_CLR,
		       b + TEMP_ADCMUXADDR);
		writel(mux, b + TEMP_ADCEN);
		writel(MT6771_AUXADC_PHYS + AUXADC_CON1_SET,
		       b + TEMP_ADCENADDR);
		writel(MT6771_AUXADC_PHYS + AUXADC_DATA(MT6771_TS_AUXADC_CHANNEL),
		       b + TEMP_ADCVALIDADDR);
		writel(MT6771_AUXADC_PHYS + AUXADC_DATA(MT6771_TS_AUXADC_CHANNEL),
		       b + TEMP_ADCVOLTADDR);
		writel(0, b + TEMP_RDCTRL);
		writel(BIT(5) | 12, b + TEMP_ADCVALIDMASK);
		writel(0, b + TEMP_ADCVOLTAGESHIFT);

		for (i = 0; i < MT6771_NUM_SENSORS; i++) {
			if (mt6771_sensors[i].ctrl != c * TEMP_CTRL1)
				continue;
			writel(mt6771_sensors[i].pnp_val,
			       mt->base + mt6771_sensors[i].pnp_off);
		}

		writel(MT6771_APMIXED_TS_CON1_PHYS, b + TEMP_PNPMUXADDR);
		writel(MT6771_ADCWRITECTRL_VAL, b + TEMP_ADCWRITECTRL);
	}

	writel(mux, mt->auxadc + AUXADC_CON1_SET);

	/* 2 sensing points on controller 0, 4 on controller 1 */
	writel(BIT(2) - 1, mt->base + TEMP_MONCTL0);
	writel(BIT(4) - 1, mt->base + TEMP_CTRL1 + TEMP_MONCTL0);
}

static void mt6771_thermal_hw_stop(struct mt6771_thermal *mt)
{
	writel(0, mt->base + TEMP_MONCTL0);
	writel(0, mt->base + TEMP_CTRL1 + TEMP_MONCTL0);
	writel(BIT(MT6771_TS_AUXADC_CHANNEL), mt->auxadc + AUXADC_CON1_CLR);
}

static int mt6771_thermal_enable(struct mt6771_thermal *mt, bool on)
{
	int i, ret;

	if (on == mt->enabled)
		return 0;

	if (on) {
		/*
		 * The clock providers may not have been ready at probe time;
		 * retry now that we are actually asked to touch the block.
		 */
		if (IS_ERR(mt->clk))
			mt->clk = devm_clk_get_optional(mt->dev, "therm-main");
		if (IS_ERR(mt->auxadc_clk))
			mt->auxadc_clk = devm_clk_get_optional(mt->dev, "auxadc");

		if (!IS_ERR_OR_NULL(mt->clk)) {
			ret = clk_prepare_enable(mt->clk);
			if (ret)
				return ret;
		}
		if (!IS_ERR_OR_NULL(mt->auxadc_clk)) {
			ret = clk_prepare_enable(mt->auxadc_clk);
			if (ret) {
				clk_disable_unprepare(mt->clk);
				return ret;
			}
		}

		mt6771_thermal_hw_init(mt);
		mt->enabled = true;

		for (i = 0; i < MT6771_NUM_SENSORS; i++) {
			mt->zone[i].mt = mt;
			mt->zone[i].idx = i;
			mt->tz[i] = thermal_tripless_zone_device_register(
				mt6771_sensors[i].name, &mt->zone[i],
				&mt6771_tz_ops, NULL);
			if (IS_ERR(mt->tz[i])) {
				dev_warn(mt->dev, "zone %s: %ld\n",
					 mt6771_sensors[i].name,
					 PTR_ERR(mt->tz[i]));
				mt->tz[i] = NULL;
			}
		}
	} else {
		for (i = 0; i < MT6771_NUM_SENSORS; i++) {
			if (mt->tz[i])
				thermal_zone_device_unregister(mt->tz[i]);
			mt->tz[i] = NULL;
		}

		mt6771_thermal_hw_stop(mt);
		mt->enabled = false;

		if (!IS_ERR_OR_NULL(mt->auxadc_clk))
			clk_disable_unprepare(mt->auxadc_clk);
		if (!IS_ERR_OR_NULL(mt->clk))
			clk_disable_unprepare(mt->clk);
	}

	return 0;
}

static int mt6771_enable_get(void *data, u64 *val)
{
	struct mt6771_thermal *mt = data;

	*val = mt->enabled;
	return 0;
}

static int mt6771_enable_set(void *data, u64 val)
{
	struct mt6771_thermal *mt = data;

	return mt6771_thermal_enable(mt, val != 0);
}

DEFINE_DEBUGFS_ATTRIBUTE(mt6771_enable_fops, mt6771_enable_get,
			 mt6771_enable_set, "%llu\n");

static int mt6771_thermal_probe(struct platform_device *pdev)
{
	struct mt6771_thermal *mt;

	mt = devm_kzalloc(&pdev->dev, sizeof(*mt), GFP_KERNEL);
	if (!mt)
		return -ENOMEM;

	mt->dev = &pdev->dev;
	mt->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mt->base))
		return PTR_ERR(mt->base);

	/*
	 * The controller itself reaches the AUXADC over AHB; this mapping is
	 * only used for the channel-11 autoset/enable bits, exactly like the
	 * vendor driver does from the CPU.
	 */
	mt->auxadc = devm_ioremap(&pdev->dev, MT6771_AUXADC_PHYS, 0x1000);
	if (!mt->auxadc)
		return -ENOMEM;

	/*
	 * The vendor DT has "clocks = <&infracfg_ao 10>" without a
	 * clock-names entry, so fetch the first (unnamed) clock as well as
	 * the AUXADC gate.  Both are enabled only when the probe is armed
	 * through debugfs.
	 */
	mt->clk = devm_clk_get_optional(&pdev->dev, "therm-main");
	if (IS_ERR(mt->clk))
		dev_warn(&pdev->dev, "no thermal clock: %ld\n",
			 PTR_ERR(mt->clk));
	mt->auxadc_clk = devm_clk_get_optional(&pdev->dev, "auxadc");
	if (IS_ERR(mt->auxadc_clk))
		dev_warn(&pdev->dev, "no auxadc clock: %ld\n",
			 PTR_ERR(mt->auxadc_clk));

	mt->dbg = debugfs_create_dir("mt6771-thermal", NULL);
	debugfs_create_file("enable", 0644, mt->dbg, mt,
			    &mt6771_enable_fops);
	debugfs_create_file("temps", 0444, mt->dbg, mt,
			    &mt6771_debugfs_temps_fops);
	debugfs_create_file("regs", 0444, mt->dbg, mt,
			    &mt6771_debugfs_regs_fops);

	platform_set_drvdata(pdev, mt);

	dev_info(&pdev->dev,
		 "probe ready, enable via /sys/kernel/debug/mt6771-thermal/enable\n");

	return 0;
}

static void mt6771_thermal_remove(struct platform_device *pdev)
{
	struct mt6771_thermal *mt = platform_get_drvdata(pdev);

	mt6771_thermal_enable(mt, false);
	debugfs_remove(mt->dbg);
}

static const struct of_device_id mt6771_thermal_of_match[] = {
	{ .compatible = "mediatek,mt6771-thermal" },
	{ }
};

static struct platform_driver mt6771_thermal_driver = {
	.probe = mt6771_thermal_probe,
	.remove = mt6771_thermal_remove,
	.driver = {
		.name = "mt6771-thermal",
		.of_match_table = mt6771_thermal_of_match,
	},
};
module_platform_driver(mt6771_thermal_driver);

MODULE_DESCRIPTION("MT6771 AUXADC thermal controller (read-only)");
MODULE_LICENSE("GPL");
