// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal battery monitor for the MT6370 PMIC.
 *
 * The mainline mt6370-charger driver needs a full DT description (IIO
 * channels, IRQs) which this board does not have, so it cannot be used.
 * This driver is deliberately read-only: it only triggers the PMIC ADC
 * (the same way drivers/iio/adc/mt6370-adc.c does) and decodes the charger
 * state register.  It never touches the charge voltage/current settings
 * programmed by the bootloader.
 *
 * Capacity is derived from the open-circuit voltage using the discharge
 * curve that the vendor battery node ("mediatek,bat_gm30",
 * battery0_profile_t0) ships in the device tree.  The terminal voltage is
 * corrected with I*R before the lookup, so changing the screen brightness
 * (which changes the load current, and therefore the IR drop) no longer
 * moves the percentage around.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define MT6370_REG_CHG_CTRL2		0x112
#define MT6370_REG_CHG_ADC		0x121
#define MT6370_REG_USB_STATUS1		0x127
#define MT6370_REG_CHG_STAT		0x14a
#define MT6370_REG_ADC_DATA_H		0x14c

#define MT6370_ADC_START_MASK		BIT(0)
#define MT6370_ADC_IN_SEL_MASK		GENMASK(7, 4)
#define MT6370_ADC_CHAN_VBAT		4
#define MT6370_ADC_CHAN_IBAT		9
#define MT6370_ADC_CONV_TIME_MS		35
#define MT6370_ADC_POLL_TIME_US		1000

#define MT6370_CHG_STAT_MASK		GENMASK(7, 6)
#define MT6370_CHG_STAT_READY		0
#define MT6370_CHG_STAT_PROGRESS	1
#define MT6370_CHG_STAT_DONE		2
#define MT6370_CHG_STAT_FAULT		3
#define MT6370_VBAT_LVL_MASK		BIT(5)

/* USB_STATUS1 bits 6:4: 0 = no VBUS, 1 = VBUS settling, else attached. */
#define MT6370_USB_STAT_MASK		GENMASK(6, 4)
#define MT6370_USB_STAT_NO_VBUS		0
#define MT6370_USB_STAT_SETTLING	1

/* VBAT ADC: 5 mV/LSB.  IBAT ADC: 38.7 mA/LSB on this board. */
#define MT6370_VBAT_LSB_MV		5
#define MT6370_IBAT_LSB_MA_X10		387
#define MT6370_IBAT_MAX_MA		8000

/*
 * Pack resistance (cell + protection FET + wiring), in milliohms.  Used to
 * turn the measured terminal voltage into an open-circuit voltage estimate.
 * The vendor fuel gauge node states FG_METER_RESISTANCE = 75 (mOhm).
 */
#define MT6370_BAT_R_MOHM		75

/* Nominal pack voltage used to turn the design capacity into energy. */
#define MT6370_BAT_NOMINAL_MV		3850

#define MT6370_BAT_SAMPLES		8
#define MT6370_BAT_INTERVAL_MS		10000
#define MT6370_BAT_EMA_DIV		6	/* ~60 s time constant at 10 s updates */
#define MT6370_BAT_CURVE_MAX		110

struct mt6370_bat_point {
	int mv;
	int pct;
};

/* Fallback curve if the vendor battery node is missing or malformed. */
static const struct mt6370_bat_point mt6370_bat_fallback_curve[] = {
	{ 3400,   0 }, { 3450,   1 }, { 3500,   3 }, { 3550,   5 },
	{ 3600,   8 }, { 3650,  13 }, { 3700,  20 }, { 3750,  30 },
	{ 3800,  45 }, { 3850,  55 }, { 3900,  65 }, { 3950,  75 },
	{ 4000,  82 }, { 4050,  88 }, { 4100,  93 }, { 4150,  97 },
	{ 4200, 100 },
};

struct mt6370_battery {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	/* Serialises the shared PMIC ADC block with other readers. */
	struct mutex adc_lock;
	struct delayed_work work;
	int mv;		/* smoothed open-circuit voltage estimate, mV */
	int vterm;	/* last measured terminal voltage, mV */
	int pct;	/* smoothed capacity, percent */
	int cur_ma;	/* battery current, mA, negative while discharging */
	int status;	/* POWER_SUPPLY_STATUS_* */
	bool valid;
	int nr_curve;
	struct mt6370_bat_point curve[MT6370_BAT_CURVE_MAX];
	int design_mah;
};

static int mt6370_bat_read_adc(struct mt6370_battery *bat, unsigned int addr,
			       unsigned int *raw)
{
	unsigned int val;
	__be16 be_val;
	int ret;

	mutex_lock(&bat->adc_lock);

	ret = regmap_write(bat->regmap, MT6370_REG_CHG_ADC,
			   MT6370_ADC_START_MASK |
			   FIELD_PREP(MT6370_ADC_IN_SEL_MASK, addr));
	if (ret)
		goto out;

	msleep(MT6370_ADC_CONV_TIME_MS);

	ret = regmap_read_poll_timeout(bat->regmap, MT6370_REG_CHG_ADC, val,
				       !(val & MT6370_ADC_START_MASK),
				       MT6370_ADC_POLL_TIME_US,
				       MT6370_ADC_CONV_TIME_MS * 3 * 1000);
	if (ret)
		goto out;

	ret = regmap_raw_read(bat->regmap, MT6370_REG_ADC_DATA_H,
			      &be_val, sizeof(be_val));
	if (ret)
		goto out;

	*raw = be16_to_cpu(be_val);
out:
	mutex_unlock(&bat->adc_lock);
	return ret;
}

static int mt6370_bat_voltage_avg(struct mt6370_battery *bat, int *mv)
{
	unsigned int raw;
	int i, sum = 0, ret;

	for (i = 0; i < MT6370_BAT_SAMPLES; i++) {
		ret = mt6370_bat_read_adc(bat, MT6370_ADC_CHAN_VBAT, &raw);
		if (ret)
			return ret;
		sum += raw;
	}

	/* raw is a multiple of 5 mV, so average the raw value before scaling. */
	*mv = DIV_ROUND_CLOSEST(sum, MT6370_BAT_SAMPLES) * MT6370_VBAT_LSB_MV;
	return 0;
}

/*
 * Battery current in mA, positive into the pack (charging).
 *
 * The MT6370 reports IBAT as a 12-bit two's complement value in bits 11:0.
 * Sign extension is essential: a 500 mA discharge arrives as 0x0FCC, which
 * read as an unsigned 16-bit value looks like 4044 LSB (~156 A) and would be
 * rejected as implausible, leaving the IR compensation with no current at
 * all.  That was exactly why the percentage collapsed a few percent per
 * minute when the backlight was turned up.
 */
static int mt6370_bat_current_ma(struct mt6370_battery *bat, int *ma)
{
	unsigned int raw;
	int cur, ret;

	ret = mt6370_bat_read_adc(bat, MT6370_ADC_CHAN_IBAT, &raw);
	if (ret)
		return ret;

	cur = sign_extend32(raw, 11) * MT6370_IBAT_LSB_MA_X10;
	cur = DIV_ROUND_CLOSEST(cur, 10);

	if (abs(cur) > MT6370_IBAT_MAX_MA)
		return -EINVAL;

	*ma = cur;
	return 0;
}

/**
 * mt6370_bat_parse_curve() - read the vendor OCV/capacity table from DT
 *
 * The vendor battery node stores 100 points as <capacity voltage current>
 * triples in 0.1 mAh / 0.1 mV units.  Only the voltage column and the last
 * capacity value (used as the design capacity) are interesting here.
 */
static int mt6370_bat_parse_curve(struct mt6370_battery *bat)
{
	struct device_node *np;
	u32 *vals;
	int n, points, i, ret = -ENODEV;

	np = of_find_compatible_node(NULL, NULL, "mediatek,bat_gm30");
	if (!np)
		goto fallback;

	n = of_property_count_elems_of_size(np, "battery0_profile_t0",
					    sizeof(u32));
	if (n < 6)
		goto out_put;

	vals = kmalloc_array(n, sizeof(*vals), GFP_KERNEL);
	if (!vals)
		goto out_put;

	if (of_property_read_u32_array(np, "battery0_profile_t0", vals, n))
		goto out_free;

	points = n / 3;
	if (points > MT6370_BAT_CURVE_MAX)
		points = MT6370_BAT_CURVE_MAX;

	for (i = 0; i < points; i++) {
		bat->curve[i].mv = vals[i * 3 + 1] / 10;
		bat->curve[i].pct = 100 - i * 100 / (points - 1);
	}
	bat->nr_curve = points;

	/* Last capacity entry is the full-charge capacity in 0.1 mAh. */
	bat->design_mah = vals[(points - 1) * 3] / 10;
	if (bat->design_mah < 1000 || bat->design_mah > 20000)
		bat->design_mah = 6000;

	ret = 0;
out_free:
	kfree(vals);
out_put:
	of_node_put(np);
	if (!ret)
		return 0;

fallback:
	memcpy(bat->curve, mt6370_bat_fallback_curve,
	       sizeof(mt6370_bat_fallback_curve));
	bat->nr_curve = ARRAY_SIZE(mt6370_bat_fallback_curve);
	bat->design_mah = 6000;
	return 0;
}

static int mt6370_bat_capacity(struct mt6370_battery *bat, int mv)
{
	int i;

	if (mv >= bat->curve[0].mv)
		return 100;

	for (i = 1; i < bat->nr_curve; i++) {
		const struct mt6370_bat_point *lo = &bat->curve[i - 1];
		const struct mt6370_bat_point *hi = &bat->curve[i];

		if (mv >= hi->mv) {
			int dmv = lo->mv - hi->mv;
			int dpct = hi->pct - lo->pct;

			if (dmv <= 0)
				return hi->pct;

			return lo->pct + (lo->mv - mv) * dpct / dmv;
		}
	}

	return 0;
}

static int mt6370_bat_status(struct mt6370_battery *bat, int mv)
{
	unsigned int chg, usb;
	int ret;

	ret = regmap_read(bat->regmap, MT6370_REG_CHG_STAT, &chg);
	if (ret)
		return ret;

	ret = regmap_read(bat->regmap, MT6370_REG_USB_STATUS1, &usb);
	if (ret)
		return ret;

	usb = FIELD_GET(MT6370_USB_STAT_MASK, usb);
	chg = FIELD_GET(MT6370_CHG_STAT_MASK, chg);

	if (usb == MT6370_USB_STAT_NO_VBUS)
		return POWER_SUPPLY_STATUS_DISCHARGING;

	switch (chg) {
	case MT6370_CHG_STAT_PROGRESS:
		return POWER_SUPPLY_STATUS_CHARGING;
	case MT6370_CHG_STAT_DONE:
		return POWER_SUPPLY_STATUS_FULL;
	case MT6370_CHG_STAT_READY:
		/* VBUS present but the charger is idle: treat a high pack as full. */
		return mv >= 4100 ? POWER_SUPPLY_STATUS_FULL
				  : POWER_SUPPLY_STATUS_NOT_CHARGING;
	default:
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
	}
}

static long mt6370_bat_energy_full(struct mt6370_battery *bat)
{
	return (long)bat->design_mah * MT6370_BAT_NOMINAL_MV;
}

static void mt6370_bat_refresh(struct mt6370_battery *bat)
{
	int mv_term, mv_ocv, cur, target, status, pct, ret;

	ret = mt6370_bat_voltage_avg(bat, &mv_term);
	if (ret)
		return;

	if (mt6370_bat_current_ma(bat, &cur))
		cur = 0;

	status = mt6370_bat_status(bat, mv_term);
	if (status < 0)
		return;

	/*
	 * Normalise the current direction from the charger state.  The PMIC
	 * sign convention is not something we want to depend on, and while on
	 * AC but idle the reading is close to zero anyway.
	 */
	if (status == POWER_SUPPLY_STATUS_DISCHARGING)
		cur = -abs(cur);
	else if (status == POWER_SUPPLY_STATUS_CHARGING)
		cur = abs(cur);

	/*
	 * Convert the measured terminal voltage into an open-circuit voltage.
	 * The pack voltage is Vterm = OCV + I*R, so OCV = Vterm - I*R for
	 * both directions (I is negative while discharging).  Removing that
	 * term is what keeps the percentage independent of backlight
	 * brightness and CPU load.
	 */
	mv_ocv = mv_term - DIV_ROUND_CLOSEST(cur * MT6370_BAT_R_MOHM, 1000);

	if (!bat->valid) {
		bat->mv = mv_ocv;
		bat->pct = mt6370_bat_capacity(bat, mv_ocv);
		bat->valid = true;
	} else {
		bat->mv = (bat->mv * (MT6370_BAT_EMA_DIV - 1) + mv_ocv) /
			  MT6370_BAT_EMA_DIV;
		target = mt6370_bat_capacity(bat, bat->mv);
		pct = bat->pct;

		if (status == POWER_SUPPLY_STATUS_FULL ||
		    (status != POWER_SUPPLY_STATUS_DISCHARGING &&
		     mv_term >= 4300)) {
			/* Charger says "done": the pack really is full. */
			pct = 100;
		} else if (status == POWER_SUPPLY_STATUS_DISCHARGING) {
			/* On battery the estimate may move both ways. */
			if (target > pct + 1)
				pct++;
			else if (target < pct - 1)
				pct--;
		} else if (target > pct + 1) {
			/* Charging can only raise the state of charge. */
			pct++;
		}

		bat->pct = clamp(pct, 0, 100);
	}

	bat->cur_ma = cur;
	bat->vterm = mv_term;
	bat->status = status;
}

static void mt6370_bat_work(struct work_struct *work)
{
	struct mt6370_battery *bat =
		container_of(work, struct mt6370_battery, work.work);
	int old_status = bat->status;

	mt6370_bat_refresh(bat);

	if (bat->valid && old_status != bat->status)
		dev_info(bat->dev, "status %d, vbat %d mV, %d mA, %d%%\n",
			 bat->status, bat->mv, bat->cur_ma, bat->pct);

	schedule_delayed_work(&bat->work,
			      msecs_to_jiffies(MT6370_BAT_INTERVAL_MS));
}

static int mt6370_bat_get_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct mt6370_battery *bat = power_supply_get_drvdata(psy);
	long energy_full = mt6370_bat_energy_full(bat);

	if (!bat->valid)
		mt6370_bat_refresh(bat);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = bat->status;
		return 0;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		return 0;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = bat->pct;
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		if (bat->pct >= 95)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		else if (bat->pct >= 70)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
		else if (bat->pct >= 40)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
		else if (bat->pct >= 10)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
		else
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = bat->vterm * 1000;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = 3400 * 1000;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = bat->cur_ma * 1000;
		return 0;
	case POWER_SUPPLY_PROP_POWER_NOW:
		/* mV * mA == uW */
		val->intval = bat->vterm * bat->cur_ma;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = bat->design_mah * 1000;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = bat->design_mah * 1000;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		val->intval = (long)bat->design_mah * 1000 * bat->pct / 100;
		return 0;
	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
		val->intval = energy_full;
		return 0;
	case POWER_SUPPLY_PROP_ENERGY_FULL:
		val->intval = energy_full;
		return 0;
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		val->intval = energy_full * bat->pct / 100;
		return 0;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = POWER_SUPPLY_HEALTH_GOOD;
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "iPlay 30 battery";
		return 0;
	default:
		return -EINVAL;
	}
}

static enum power_supply_property mt6370_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
	POWER_SUPPLY_PROP_ENERGY_FULL,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static const struct power_supply_desc mt6370_bat_desc = {
	.name		= "mt6370-battery",
	.type		= POWER_SUPPLY_TYPE_BATTERY,
	.properties	= mt6370_bat_props,
	.num_properties	= ARRAY_SIZE(mt6370_bat_props),
	.get_property	= mt6370_bat_get_property,
};

static int mt6370_bat_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct power_supply_config cfg = {};
	struct mt6370_battery *bat;

	bat = devm_kzalloc(dev, sizeof(*bat), GFP_KERNEL);
	if (!bat)
		return -ENOMEM;

	bat->dev = dev;
	bat->regmap = dev_get_regmap(dev->parent, NULL);
	if (!bat->regmap)
		return dev_err_probe(dev, -ENODEV, "failed to get parent regmap\n");

	mutex_init(&bat->adc_lock);
	INIT_DELAYED_WORK(&bat->work, mt6370_bat_work);
	cfg.drv_data = bat;

	mt6370_bat_parse_curve(bat);

	/* Fill the cache before registering, so the first poll sees real data. */
	mt6370_bat_refresh(bat);

	bat->psy = devm_power_supply_register(dev, &mt6370_bat_desc, &cfg);
	if (IS_ERR(bat->psy))
		return dev_err_probe(dev, PTR_ERR(bat->psy),
				     "failed to register battery\n");

	platform_set_drvdata(pdev, bat);

	schedule_delayed_work(&bat->work, msecs_to_jiffies(1000));

	dev_info(dev, "registered: vbat/ocv %d mV, %d mA, %d%%, %d mAh, %d curve points\n",
		 bat->valid ? bat->mv : -1, bat->cur_ma,
		 bat->valid ? bat->pct : -1, bat->design_mah, bat->nr_curve);

	return 0;
}

static void mt6370_bat_remove(struct platform_device *pdev)
{
	struct mt6370_battery *bat = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&bat->work);
}

static const struct of_device_id mt6370_bat_of_match[] = {
	{ .compatible = "mediatek,mt6370-battery" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6370_bat_of_match);

static struct platform_driver mt6370_bat_driver = {
	.driver = {
		.name = "mt6370-battery",
		.of_match_table = mt6370_bat_of_match,
	},
	.probe = mt6370_bat_probe,
	.remove = mt6370_bat_remove,
};
module_platform_driver(mt6370_bat_driver);

MODULE_DESCRIPTION("MT6370 battery monitor");
MODULE_LICENSE("GPL");
