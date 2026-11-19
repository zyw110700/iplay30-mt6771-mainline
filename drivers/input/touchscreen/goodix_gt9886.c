// SPDX-License-Identifier: GPL-2.0-only
/*
 * Goodix GT9886-family touchscreen driver
 *
 * Minimal driver for Goodix touch controllers using the 16-bit register
 * map of the GT9886/GT7386 generation (the "gt9xx" driver in this tree
 * uses the older 16-bit map starting at 0x8040/0x814E, which does not
 * match these parts).
 *
 * Register map:
 *   0x8040  command
 *   0x8044  firmware request
 *   0x8240  version (pid string + vid + sensor id + checksum)
 *   0x824E  coordinate/event buffer
 *   0x8050  configuration
 *
 * Event packet (classic gt9xx layout, measured on hardware):
 *   buf[0]	    0x80 | touch count (bit 6: firmware request, bit 5:
 *		  stylus packet)
 *   buf[1..]    8 bytes per contact:
 *		  id, x (le16), y (le16), width/pressure (le16), reserved
 *   For stylus packets four extra bytes follow the contacts.
 *   Then a key byte and a checksum byte; the sum of all packet bytes
 *   must be 0 modulo 256.
 * A contact whose id has bit 7 set is the active stylus; its 16-bit
 * width field carries the pressure.
 * Writing 0 to 0x824E acknowledges the packet and lets the controller
 * report the next event.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>

#define GT_DRV_NAME		"goodix_gt9886"
#define GT_MAX_TOUCH		10
#define GT_BYTES_PER_COORD	8

#define GT_REG_CMD		0x8040
#define GT_REG_REQUEST		0x8044
#define GT_REG_CFG		0x8050
#define GT_REG_VERSION		0x8240
#define GT_REG_COORDS		0x824E
#define GT_REG_PID		0x4535
#define GT_REG_TEST		0x4100

#define GT_EVENT_TOUCH		0x80
#define GT_EVENT_REQUEST	0x40
#define GT_EVENT_PEN		0x20	/* stylus packet carries 4 extra bytes */

#define GT_POLL_INTERVAL_MS	20
#define GT_FRAME_MAX		(1 + GT_BYTES_PER_COORD * GT_MAX_TOUCH + 4 + 2)

/*
 * The stylus packet carries coordinates in panel pixels (1200x1920), unlike
 * the finger path which uses the controller's 12-bit range.  The panel is
 * about 136x216 mm, so both axes are roughly 9 units/mm.
 */
#define GT_PEN_X_MAX		1200
#define GT_PEN_Y_MAX		1920
#define GT_PEN_RES		9

struct gt_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct input_dev *pen_input;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *irq_gpio;
	struct regulator *avdd;
	struct regulator *vddio;
	struct mutex lock;		/* serializes register access */
	struct delayed_work poll_work;
	struct touchscreen_properties props;
	struct touchscreen_properties pen_props;
	unsigned int idle_polls;
	unsigned int good_events;
	unsigned int rx_packets;
	unsigned int bad_checksums;
	unsigned int pen_events;
	unsigned int rep_count;
	u8 last_sum;
	u8 ev_count;
	u8 ev_sum;
	u8 ev_packet[24];
	u8 rep_status;
	u8 rep_id;
	u8 rep_packet[24];
	unsigned int rep_x;
	unsigned int rep_y;
	unsigned int rep_w;
	bool active;
	bool pen_down;
	u8 packet[GT_FRAME_MAX];
	u8 request;
};

static int gt_i2c_read(struct gt_data *ts, u16 reg, u8 *val, int len)
{
	struct i2c_client *client = ts->client;
	u8 addr[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[2] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = sizeof(addr),
			.buf = addr,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = len,
			.buf = val,
		},
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, 2);
	if (ret != 2) {
		dev_err_ratelimited(&client->dev,
				    "read 0x%04x failed: %d\n", reg, ret);
		return ret < 0 ? ret : -EIO;
	}

	return 0;
}

static int gt_i2c_write(struct gt_data *ts, u16 reg, const u8 *val, int len)
{
	struct i2c_client *client = ts->client;
	u8 buf[2 + 8];
	struct i2c_msg msg = {
		.addr = client->addr,
		.flags = 0,
		.len = len + 2,
		.buf = buf,
	};
	int ret;

	if (len > 8)
		return -EINVAL;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;
	memcpy(&buf[2], val, len);

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret != 1) {
		dev_err_ratelimited(&client->dev,
				    "write 0x%04x failed: %d\n", reg, ret);
		return ret < 0 ? ret : -EIO;
	}

	return 0;
}

static int gt_i2c_write8(struct gt_data *ts, u16 reg, u8 val)
{
	return gt_i2c_write(ts, reg, &val, 1);
}

static int gt_release_event(struct gt_data *ts)
{
	u8 zero = 0;

	return gt_i2c_write(ts, GT_REG_COORDS, &zero, 1);
}

static int gt_reset(struct gt_data *ts)
{
	u8 dummy;
	int ret, i;

	for (i = 0; i < 3; i++) {
		gpiod_set_value_cansleep(ts->reset_gpio, 0);
		usleep_range(2000, 3000);
		gpiod_set_value_cansleep(ts->reset_gpio, 1);
		usleep_range(5000, 6000);

		/* The controller answers at 0x4100 once it left reset. */
		ret = gt_i2c_read(ts, GT_REG_TEST, &dummy, 1);
		if (!ret)
			break;

		dev_warn(&ts->client->dev, "i2c test after reset failed: %d\n",
			 ret);
	}

	msleep(95);

	return ret;
}

static void gt_report_pen_up(struct gt_data *ts)
{
	struct input_dev *pen = ts->pen_input;

	if (!ts->pen_down)
		return;

	input_report_key(pen, BTN_TOUCH, 0);
	input_report_key(pen, BTN_TOOL_PEN, 0);
	input_report_key(pen, BTN_STYLUS, 0);
	input_report_key(pen, BTN_STYLUS2, 0);
	input_report_abs(pen, ABS_PRESSURE, 0);
	input_sync(pen);
	ts->pen_down = false;
}

static void gt_report(struct gt_data *ts, const u8 *buf, unsigned int touch_num)
{
	struct input_dev *input = ts->input;
	struct input_dev *pen = ts->pen_input;
	bool pen_present = false;
	unsigned int i;

	/* Snapshot what the parser actually sees, for the debug attribute. */
	ts->rep_count = touch_num;
	ts->rep_status = buf[0];
	memcpy(ts->rep_packet, buf, sizeof(ts->rep_packet));
	if (touch_num) {
		const u8 *p0 = &buf[1];

		ts->rep_id = p0[0];
		ts->rep_x = p0[1] | (p0[2] << 8);
		ts->rep_y = p0[3] | (p0[4] << 8);
		ts->rep_w = p0[5] | (p0[6] << 8);
	}

	for (i = 0; i < touch_num; i++) {
		const u8 *p = &buf[1 + i * GT_BYTES_PER_COORD];
		unsigned int x = p[1] | (p[2] << 8);
		unsigned int y = p[3] | (p[4] << 8);
		unsigned int w = p[5] | (p[6] << 8);

		if (p[0] & 0x80) {
			/*
			 * Active stylus: the 16-bit width field carries the
			 * pressure instead of a contact size.  Report it on
			 * a dedicated pen device, the way tablet styluses
			 * are normally exposed (BTN_TOOL_PEN + ABS_PRESSURE).
			 */
			pen_present = true;
			ts->pen_events++;
			touchscreen_report_pos(pen, &ts->pen_props, x, y, false);
			input_report_abs(pen, ABS_PRESSURE, w);
			input_report_key(pen, BTN_TOUCH, 1);
			input_report_key(pen, BTN_TOOL_PEN, 1);
			continue;
		}

		input_mt_slot(input, p[0] & 0x0f);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, true);
		touchscreen_report_pos(input, &ts->props, x, y, true);
		input_report_abs(input, ABS_MT_TOUCH_MAJOR, min(w, 255u));
	}

	if (pen_present) {
		input_sync(pen);
		ts->pen_down = true;
	} else {
		gt_report_pen_up(ts);
	}

	input_mt_sync_frame(input);
	input_sync(input);

	ts->active = touch_num != 0;
}

static int gt_read_packet(struct gt_data *ts)
{
	u8 *buf = ts->packet;
	unsigned int sum = 0, touch_num, extra;
	int i, ret;

	/*
	 * Read the whole event buffer in a single transfer.  The controller
	 * refreshes the pending packet as soon as the status byte has been
	 * read out, so reading the status and the contacts separately makes
	 * the two disagree and the checksum can never match.
	 */
	ret = gt_i2c_read(ts, GT_REG_COORDS, buf, GT_FRAME_MAX);
	if (ret)
		return ret;

	ts->rx_packets++;

	if (buf[0] & GT_EVENT_REQUEST) {
		u8 req = 0;

		if (!gt_i2c_read(ts, GT_REG_REQUEST, &req, 1))
			dev_info_ratelimited(&ts->client->dev,
					     "firmware request %u\n", req);
		gt_i2c_write8(ts, GT_REG_REQUEST, 0);
		return -EAGAIN;
	}

	if (!(buf[0] & GT_EVENT_TOUCH))
		return 0;

	touch_num = buf[0] & 0x0f;
	if (touch_num > GT_MAX_TOUCH) {
		dev_warn_ratelimited(&ts->client->dev,
				     "bad touch count %u\n", touch_num);
		return -EAGAIN;
	}

	/*
	 * A hovering stylus keeps producing empty packets; only push a
	 * release to the input layer if something was actually down.
	 */
	if (!touch_num && !ts->active)
		return 0;

	/*
	 * Stylus packets (status bit 5) carry four extra bytes between the
	 * contact records and the key/checksum pair.
	 */
	extra = (buf[0] & GT_EVENT_PEN) ? 4 : 0;

	for (i = 0; i < touch_num * GT_BYTES_PER_COORD + extra + 3; i++)
		sum += buf[i];
	ts->last_sum = sum & 0xff;

	if (buf[0] & GT_EVENT_TOUCH) {
		memcpy(ts->ev_packet, buf, sizeof(ts->ev_packet));
		ts->ev_count = touch_num;
		ts->ev_sum = sum & 0xff;
	}

	if (sum & 0xff) {
		ts->bad_checksums++;
		dev_warn_ratelimited(&ts->client->dev,
				     "bad pkt %02x %02x %02x %02x %02x %02x %02x %02x n=%u sum=0x%02x\n",
				     buf[0], buf[1], buf[2], buf[3], buf[4],
				     buf[5], buf[6], buf[7], touch_num,
				     sum & 0xff);
		return -EAGAIN;
	}

	gt_report(ts, buf, touch_num);

	if (!(++ts->good_events % 50))
		dev_info(&ts->client->dev, "valid events: %u\n",
			 ts->good_events);

	return 0;
}

static int gt_fetch_event(struct gt_data *ts)
{
	int ret = 0, tries;

	/*
	 * The controller updates the event buffer asynchronously; when a
	 * sample arrives while we read it the checksum will not match.
	 * Re-read a few times before dropping the event.
	 */
	for (tries = 0; tries < 3; tries++) {
		ret = gt_read_packet(ts);
		if (ret != -EAGAIN)
			break;
		usleep_range(1000, 2000);
	}

	return gt_release_event(ts);
}

static bool gt_data_ready(struct gt_data *ts)
{
	if (!ts->irq_gpio)
		return true;

	/* The controller pulls INT low while an event is pending. */
	return gpiod_get_value_cansleep(ts->irq_gpio) == 0;
}

static void gt_poll_work(struct work_struct *work)
{
	struct gt_data *ts = container_of(to_delayed_work(work),
					  struct gt_data, poll_work);

	mutex_lock(&ts->lock);
	/*
	 * Only touch the bus while the controller says it has something,
	 * but keep a slow unconditional probe as a safety net in case the
	 * interrupt line is not wired the way we expect.
	 */
	if (gt_data_ready(ts) || !(ts->idle_polls++ % 10))
		gt_fetch_event(ts);
	mutex_unlock(&ts->lock);

	schedule_delayed_work(&ts->poll_work,
			      msecs_to_jiffies(GT_POLL_INTERVAL_MS));
}

static irqreturn_t gt_irq(int irq, void *data)
{
	struct gt_data *ts = data;

	mutex_lock(&ts->lock);
	gt_fetch_event(ts);
	mutex_unlock(&ts->lock);

	return IRQ_HANDLED;
}

static int gt_read_version(struct gt_data *ts)
{
	u8 buf[12];
	int ret;

	ret = gt_i2c_read(ts, GT_REG_VERSION, buf, sizeof(buf));
	if (ret)
		return ret;

	dev_info(&ts->client->dev, "version: %4s vid=%02x%02x sensor=%02x%02x\n",
		 buf, buf[5], buf[4], buf[7], buf[6]);

	return 0;
}

static void gt_power_off(void *data)
{
	struct gt_data *ts = data;

	regulator_disable(ts->avdd);
	regulator_disable(ts->vddio);
}

static void gt_stop_poll(void *data)
{
	struct gt_data *ts = data;

	cancel_delayed_work_sync(&ts->poll_work);
}

static ssize_t debug_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct gt_data *ts = dev_get_drvdata(dev);
	unsigned int i;
	int n = 0;

	n += scnprintf(buf + n, PAGE_SIZE - n,
		       "rx=%u bad=%u ok=%u pen=%u active=%d\n",
		       ts->rx_packets, ts->bad_checksums, ts->good_events,
		       ts->pen_events, ts->active);
	n += scnprintf(buf + n, PAGE_SIZE - n,
		       "last: st=%02x n=%u sum=%02x\n",
		       ts->packet[0], ts->packet[0] & 0x0f, ts->last_sum);
	n += scnprintf(buf + n, PAGE_SIZE - n,
		       "rep: st=%02x n=%u id=%02x x=%u y=%u w=%u\n",
		       ts->rep_status, ts->rep_count, ts->rep_id,
		       ts->rep_x, ts->rep_y, ts->rep_w);
	n += scnprintf(buf + n, PAGE_SIZE - n, "ev: n=%u sum=%02x\n",
		       ts->ev_count, ts->ev_sum);
	n += scnprintf(buf + n, PAGE_SIZE - n, "cur:");
	for (i = 0; i < 24; i++)
		n += scnprintf(buf + n, PAGE_SIZE - n, " %02x", ts->packet[i]);
	n += scnprintf(buf + n, PAGE_SIZE - n, "\nev :");
	for (i = 0; i < 24; i++)
		n += scnprintf(buf + n, PAGE_SIZE - n, " %02x", ts->ev_packet[i]);
	n += scnprintf(buf + n, PAGE_SIZE - n, "\nrep:");
	for (i = 0; i < 24; i++)
		n += scnprintf(buf + n, PAGE_SIZE - n, " %02x", ts->rep_packet[i]);
	n += scnprintf(buf + n, PAGE_SIZE - n, "\n");

	return n;
}
static DEVICE_ATTR_RO(debug);

static struct attribute *gt_attrs[] = {
	&dev_attr_debug.attr,
	NULL,
};
ATTRIBUTE_GROUPS(gt);

static int gt_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gt_data *ts;
	int ret;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	i2c_set_clientdata(client, ts);
	mutex_init(&ts->lock);

	ts->avdd = devm_regulator_get_optional(dev, "avdd");
	if (IS_ERR(ts->avdd)) {
		if (PTR_ERR(ts->avdd) != -ENODEV)
			return dev_err_probe(dev, PTR_ERR(ts->avdd),
					     "failed to get avdd\n");
		ts->avdd = NULL;
	}

	ts->vddio = devm_regulator_get_optional(dev, "vddio");
	if (IS_ERR(ts->vddio)) {
		if (PTR_ERR(ts->vddio) != -ENODEV)
			return dev_err_probe(dev, PTR_ERR(ts->vddio),
					     "failed to get vddio\n");
		ts->vddio = NULL;
	}

	ret = regulator_enable(ts->avdd);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable avdd\n");

	ret = regulator_enable(ts->vddio);
	if (ret) {
		regulator_disable(ts->avdd);
		return dev_err_probe(dev, ret, "failed to enable vddio\n");
	}

	ret = devm_add_action_or_reset(dev, gt_power_off, ts);
	if (ret)
		return ret;

	ts->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ts->reset_gpio),
				     "failed to get reset gpio\n");

	ts->irq_gpio = devm_gpiod_get_optional(dev, "irq", GPIOD_IN);
	if (IS_ERR(ts->irq_gpio))
		return dev_err_probe(dev, PTR_ERR(ts->irq_gpio),
				     "failed to get irq gpio\n");

	msleep(10);

	ret = gt_reset(ts);
	if (ret)
		return dev_err_probe(dev, ret, "controller does not answer\n");

	ret = gt_read_version(ts);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read version\n");

	/* Drop whatever event the controller queued while booting. */
	gt_release_event(ts);

	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;

	ts->input->name = "Goodix GT9886 Touchscreen";
	ts->input->id.bustype = BUS_I2C;
	ts->input->dev.parent = dev;

	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, 4095, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, 4095, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0, 4095, 0, 0);
	input_set_abs_params(ts->input, ABS_PRESSURE, 0, 4095, 0, 0);
	/*
	 * libinput needs a non-zero resolution for tablet tools and uses it
	 * for the physical size of the touchscreen.  The finger path reports
	 * 0..4095 over the 136 x 216 mm panel, so ~30 and ~19 units/mm.
	 */
	input_abs_set_res(ts->input, ABS_MT_POSITION_X, 30);
	input_abs_set_res(ts->input, ABS_MT_POSITION_Y, 19);

	touchscreen_parse_properties(ts->input, true, &ts->props);

	/*
	 * The stylus is exposed as its own input device, the way tablet
	 * pens normally are, so userspace gets BTN_TOOL_PEN + ABS_PRESSURE
	 * instead of an extra multitouch slot.
	 */
	ts->pen_props = ts->props;

	ts->pen_input = devm_input_allocate_device(dev);
	if (!ts->pen_input)
		return -ENOMEM;

	ts->pen_input->name = "Goodix GT9886 Pen";
	ts->pen_input->id.bustype = BUS_I2C;
	ts->pen_input->dev.parent = dev;

	input_set_abs_params(ts->pen_input, ABS_X, 0, GT_PEN_X_MAX, 0, 0);
	input_set_abs_params(ts->pen_input, ABS_Y, 0, GT_PEN_Y_MAX, 0, 0);
	input_set_abs_params(ts->pen_input, ABS_PRESSURE, 0, 4095, 0, 0);
	input_abs_set_res(ts->pen_input, ABS_X, GT_PEN_RES);
	input_abs_set_res(ts->pen_input, ABS_Y, GT_PEN_RES);
	input_set_capability(ts->pen_input, EV_KEY, BTN_TOUCH);
	input_set_capability(ts->pen_input, EV_KEY, BTN_TOOL_PEN);
	input_set_capability(ts->pen_input, EV_KEY, BTN_STYLUS);
	input_set_capability(ts->pen_input, EV_KEY, BTN_STYLUS2);

	ret = input_mt_init_slots(ts->input, GT_MAX_TOUCH + 1,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return dev_err_probe(dev, ret, "failed to init slots\n");

	ret = input_register_device(ts->input);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register input\n");

	ret = input_register_device(ts->pen_input);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register pen input\n");

	INIT_DELAYED_WORK(&ts->poll_work, gt_poll_work);
	ret = devm_add_action_or_reset(dev, gt_stop_poll, ts);
	if (ret)
		return ret;

	schedule_delayed_work(&ts->poll_work,
			      msecs_to_jiffies(GT_POLL_INTERVAL_MS));

	if (client->irq > 0) {
		ret = devm_request_threaded_irq(dev, client->irq, NULL, gt_irq,
						IRQF_ONESHOT,
						GT_DRV_NAME, ts);
		if (ret)
			dev_warn(dev, "no irq (%d), polling only\n", ret);
	}

	dev_info(dev, "GT9886 ready: %ux%u\n", ts->props.max_x,
		 ts->props.max_y);

	return 0;
}

static const struct of_device_id gt_of_match[] = {
	{ .compatible = "goodix,gt9886" },
	{ .compatible = "goodix,gt7386" },
	{ }
};
MODULE_DEVICE_TABLE(of, gt_of_match);

static const struct i2c_device_id gt_i2c_id[] = {
	{ GT_DRV_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, gt_i2c_id);

static struct i2c_driver gt_driver = {
	.driver = {
		.name = GT_DRV_NAME,
		.of_match_table = gt_of_match,
		.dev_groups = gt_groups,
	},
	.probe = gt_probe,
	.id_table = gt_i2c_id,
};
module_i2c_driver(gt_driver);

MODULE_DESCRIPTION("Goodix GT9886 touchscreen driver");
MODULE_LICENSE("GPL");
