// SPDX-License-Identifier: GPL-2.0-only
/*
 * HCI transport over the MediaTek STP "BT" task (BTIF).
 *
 * On CONNAC style connectivity chips (MT6631 on MT6771) the Bluetooth
 * controller is reached through the WMT/STP stack instead of a real UART.
 * The vendor tree exposes the BT task as a character device
 * (vendor-connectivity/bt/mt66xx/legacy/stp_chrdev_bt.c) and lets a
 * userspace BT stack speak H4 over it.  This driver does the same thing in
 * the kernel so that the standard BlueZ stack can drive hci0 directly.
 *
 * The STP BT task preserves HCI packet boundaries and carries the H4 packet
 * type as the first byte, so RX strips it and TX prepends it again.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/skbuff.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/firmware.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

/* STP task index used by the vendor stack for BT. */
#define STPBT_TASK_INDX		0
/* ENUM_WMTDRV_TYPE_T */
#define STPBT_WMTDRV_TYPE_BT	0

#define STPBT_RX_BUF_SIZE	2048
#define STPBT_RX_POLL_MS	200

#define stpbt_dbg(fmt, ...)	pr_debug("hci_stpbt: " fmt, ##__VA_ARGS__)
#define stpbt_info(fmt, ...)	pr_info("hci_stpbt: " fmt, ##__VA_ARGS__)
#define stpbt_err(fmt, ...)	pr_err("hci_stpbt: " fmt, ##__VA_ARGS__)

/* Provided by wmt_drv (vendor STP stack). */
extern int mtk_wcn_stp_send_data(const unsigned char *buffer,
				 const unsigned int length,
				 const unsigned char type);
extern int mtk_wcn_stp_receive_data(unsigned char *buffer, unsigned int length,
				    unsigned char type);
extern int mtk_wcn_stp_is_rxqueue_empty(unsigned char type);
extern int mtk_wcn_stp_is_ready(void);
extern int mtk_wcn_stp_register_event_cb(int type, void (*func)(void));
extern int mtk_wcn_wmt_func_on(unsigned int type);
extern int mtk_wcn_wmt_func_off(unsigned int type);

struct stpbt_dev {
	struct hci_dev *hdev;
	struct task_struct *rx_task;
	wait_queue_head_t rx_wq;
	bool rx_pending;
};

static struct stpbt_dev *stpbt;

static bool stpbt_debug;
module_param_named(debug, stpbt_debug, bool, 0644);
MODULE_PARM_DESC(debug, "log every HCI packet (type/opcode/event)");

static bool stpbt_hexdump_evt;
module_param_named(hexdump, stpbt_hexdump_evt, bool, 0644);
MODULE_PARM_DESC(hexdump, "hexdump received HCI events (for vendor length analysis)");

static bool stpbt_trim_conn_req;
module_param_named(trim_conn_req, stpbt_trim_conn_req, bool, 0644);
MODULE_PARM_DESC(trim_conn_req, "trim vendor-extended Connection Request events to 10 bytes");

/*
 * Quirk: this controller keeps inquiry results buffered and only reports them
 * when the inquiry is cancelled.  BlueZ starts an inquiry with a length of 8
 * (82 s on this controller) and would therefore never see a device.  Sending
 * an Inquiry Cancel ourselves a bit after the Inquiry makes the controller
 * flush its results; the core then sees Inquiry Complete and restarts
 * discovery, so results keep coming.  0 = disabled.
 */
static unsigned int stpbt_auto_cancel_ms;
module_param_named(auto_cancel_ms, stpbt_auto_cancel_ms, uint, 0644);
MODULE_PARM_DESC(auto_cancel_ms, "send Inquiry Cancel this many ms after Inquiry (0 = off)");

static void stpbt_cancel_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(stpbt_cancel_work, stpbt_cancel_work_fn);

/*
 * Quirk #2: this controller withholds *asynchronous* HCI events (connection
 * request/complete, inquiry results, ...) until the host sends something.
 * A periodic harmless command keeps the event pipe flowing; without it a
 * connection setup is only reported after the next unrelated command.
 * 0 disables it.
 */
static unsigned int stpbt_kick_ms;
module_param_named(kick_ms, stpbt_kick_ms, uint, 0644);
MODULE_PARM_DESC(kick_ms, "send a harmless HCI command every N ms to flush events (0 = off)");

static void stpbt_kick_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(stpbt_kick_work, stpbt_kick_work_fn);

/* H4 Read Local Version: type 0x01, opcode 0x1001, plen 0 */
static u8 stpbt_kick_pkt[4] = { HCI_COMMAND_PKT, 0x01, 0x10, 0x00 };

static void stpbt_kick_work_fn(struct work_struct *work)
{
	if (!stpbt_kick_ms)
		return;

	if (mtk_wcn_stp_is_ready())
		mtk_wcn_stp_send_data(stpbt_kick_pkt, sizeof(stpbt_kick_pkt),
				      STPBT_TASK_INDX);

	schedule_delayed_work(&stpbt_kick_work,
			      msecs_to_jiffies(stpbt_kick_ms));
}

/* H4 Inquiry Cancel: type 0x01, opcode 0x0402, plen 0 */
static u8 stpbt_cancel_pkt[4] = { HCI_COMMAND_PKT, 0x02, 0x04, 0x00 };

static void stpbt_cancel_work_fn(struct work_struct *work)
{
	if (!mtk_wcn_stp_is_ready())
		return;

	pr_info_ratelimited("stpbt: auto Inquiry Cancel\n");
	mtk_wcn_stp_send_data(stpbt_cancel_pkt, sizeof(stpbt_cancel_pkt),
			      STPBT_TASK_INDX);
}

static void stpbt_hexdump(const char *dir, const u8 *buf, int len)
{
	char line[3 * 40 + 1];
	int i, n = min(len, 40);

	for (i = 0; i < n; i++)
		snprintf(line + 3 * i, 4, "%02x ", buf[i]);
	line[3 * n] = '\0';
	pr_info("stpbt %s hex(%d): %s\n", dir, len, line);
}

/* buf[0] is the H4 packet type. */
static void stpbt_dump(const char *dir, const u8 *buf, int len)
{
	if (!stpbt_debug || len < 2)
		return;

	switch (buf[0]) {
	case HCI_COMMAND_PKT:
		pr_info("stpbt %s cmd opcode 0x%04x len %d\n", dir,
			buf[1] | (buf[2] << 8), len);
		break;
	case HCI_EVENT_PKT:
		pr_info("stpbt %s evt 0x%02x len %d\n", dir, buf[1], len);
		break;
	default:
		pr_info("stpbt %s type %u len %d\n", dir, buf[0], len);
		break;
	}
}

static void stpbt_rx_event_cb(void)
{
	struct stpbt_dev *bt = stpbt;

	if (!bt)
		return;

	bt->rx_pending = true;
	wake_up_interruptible(&bt->rx_wq);
}

/*
 * The controller may pack several H4 packets into one STP packet (seen with
 * connection setup: 1d/0c/1d/1d in a single 35 byte payload), so the payload
 * has to be demultiplexed instead of being handed to the core in one piece.
 */
static int stpbt_deliver(struct stpbt_dev *bt, u8 *buf, int len)
{
	struct sk_buff *skb;
	u8 type;

	if (len < 2)
		return -EINVAL;

	type = buf[0];
	if (type != HCI_COMMAND_PKT && type != HCI_ACLDATA_PKT &&
	    type != HCI_SCODATA_PKT && type != HCI_EVENT_PKT &&
	    type != HCI_ISODATA_PKT) {
		stpbt_err("bad H4 packet type %u (len %d)\n", type, len);
		return -EINVAL;
	}

	if (type == HCI_EVENT_PKT && len >= 3) {
		u8 evt = buf[1];
		u8 plen = buf[2];

		if (stpbt_hexdump_evt && stpbt_debug)
			stpbt_hexdump("rx", buf, len);

		if (evt == HCI_EV_INQUIRY_COMPLETE)
			cancel_delayed_work(&stpbt_cancel_work);

		if (evt == HCI_EV_CONN_REQUEST &&
		    plen > sizeof(struct hci_ev_conn_request)) {
			if (stpbt_debug)
				stpbt_hexdump("rx", buf, len);
			/*
			 * This controller appends vendor data to the standard
			 * Connection Request event (24 bytes instead of 10);
			 * the HCI core drops such packets, which makes the
			 * adapter unreachable for incoming connections.
			 */
			if (stpbt_trim_conn_req) {
				pr_info_ratelimited("stpbt: trimming conn_req %u -> %zu\n",
						    plen,
						    sizeof(struct hci_ev_conn_request));
				plen = sizeof(struct hci_ev_conn_request);
				buf[2] = plen;
				len = 3 + plen;
			}
		}
	}

	skb = bt_skb_alloc(len - 1, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	skb_put_data(skb, buf + 1, len - 1);
	hci_skb_pkt_type(skb) = type;
	hci_recv_frame(bt->hdev, skb);

	return 0;
}

/* Length of the H4 packet starting at buf, or the whole remaining buffer. */
static int stpbt_h4_len(const u8 *buf, int len)
{
	int plen;

	if (len < 1)
		return 0;

	switch (buf[0]) {
	case HCI_COMMAND_PKT:
		if (len < 4)
			return len;
		plen = 4 + buf[3];
		break;
	case HCI_ACLDATA_PKT:
		if (len < 5)
			return len;
		plen = 5 + (buf[3] | (buf[4] << 8));
		break;
	case HCI_SCODATA_PKT:
		if (len < 4)
			return len;
		plen = 4 + buf[3];
		break;
	case HCI_EVENT_PKT:
		if (len < 3)
			return len;
		plen = 3 + buf[2];
		break;
	case HCI_ISODATA_PKT:
		if (len < 5)
			return len;
		plen = 5 + (buf[3] | (buf[4] << 8));
		break;
	default:
		return len;
	}

	return min(plen, len);
}

static int stpbt_recv_packet(struct stpbt_dev *bt, u8 *buf, int len)
{
	int off = 0;
	int ret = 0;

	while (off < len) {
		int plen = stpbt_h4_len(buf + off, len - off);

		if (plen <= 0)
			break;

		ret = stpbt_deliver(bt, buf + off, plen);
		off += plen;
	}

	return ret;
}

static int stpbt_rx_thread(void *arg)
{
	struct stpbt_dev *bt = arg;
	u8 *buf;
	int len;

	buf = kmalloc(STPBT_RX_BUF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	while (!kthread_should_stop()) {
		if (mtk_wcn_stp_is_rxqueue_empty(STPBT_TASK_INDX)) {
			wait_event_interruptible_timeout(bt->rx_wq,
				bt->rx_pending || kthread_should_stop(),
				msecs_to_jiffies(STPBT_RX_POLL_MS));
			bt->rx_pending = false;
			continue;
		}

		len = mtk_wcn_stp_receive_data(buf, STPBT_RX_BUF_SIZE,
					       STPBT_TASK_INDX);
		if (len > 0) {
			stpbt_dump("rx", buf, len);
			stpbt_recv_packet(bt, buf, len);
			continue;
		}
		if (len < 0) {
			stpbt_err("stp receive failed (%d)\n", len);
			msleep(20);
		} else {
			/* Queue raced with us, wait for the next hint. */
			wait_event_interruptible_timeout(bt->rx_wq,
				bt->rx_pending || kthread_should_stop(),
				msecs_to_jiffies(STPBT_RX_POLL_MS));
			bt->rx_pending = false;
		}
	}

	kfree(buf);
	return 0;
}

/*
 * MediaTek vendor init.
 *
 * The stock BT HAL runs a "preload script" after the firmware finished
 * downloading and before the stack takes over.  Without it the controller
 * keeps its boot defaults: the BD address stays 00:00:46:07:88:01 and LE
 * scanning never reports anything (BR/EDR works, LE advertising works, but
 * no LE Advertising Report event is ever generated).  The radio/sleep
 * values live in the per-device BT NVRAM record (112 bytes, first six bytes
 * are the BD address); the stock HAL copies that record into its state
 * struct at offset 8, so btinit[N] == record[N - 8].
 *
 * The values only take effect once the controller is reset afterwards: a
 * plain HCI Reset commits the shadowed configuration.  Without it the boot
 * defaults stay active and LE scanning never reports anything.  This is why
 * the script runs from ->post_init() (after the core finished its own init
 * sequence) and ends with an explicit HCI Reset; ->setup() runs too early,
 * the controller firmware is still busy with its own start-up then.
 */
static bool stpbt_vendor_init = true;
module_param_named(vendor_init, stpbt_vendor_init, bool, 0644);
MODULE_PARM_DESC(vendor_init, "replay the MediaTek BT preload script from /lib/firmware/BT_Addr");

#define STPBT_NVRAM_RECORD	0x30

static void stpbt_vendor_cmd(struct hci_dev *hdev, const char *name, u16 opcode,
			     const void *param, u32 plen)
{
	int err = __hci_cmd_sync_status(hdev, opcode, plen, param,
					HCI_INIT_TIMEOUT);

	if (err)
		stpbt_err("vendor cmd %s (0x%04x) failed: %d\n", name, opcode, err);
	else if (stpbt_debug)
		stpbt_info("vendor cmd %s (0x%04x) ok\n", name, opcode);
}

static int stpbt_post_init(struct hci_dev *hdev)
{
	const struct firmware *fw;
	struct sk_buff *skb;
	u8 pcm[4], radio[8], txp[6], sleep_to[7];
	u8 ssp_debug = 0x01;
	u8 lktype = 0x01;
	int err;

	if (!stpbt_vendor_init)
		return 0;

	err = request_firmware(&fw, "BT_Addr", &hdev->dev);
	if (err) {
		stpbt_err("BT_Addr firmware missing (%d), LE scan will not work\n", err);
		return 0;
	}

	if (fw->size < STPBT_NVRAM_RECORD) {
		stpbt_err("BT_Addr too short (%zu), skipping vendor init\n", fw->size);
		release_firmware(fw);
		return 0;
	}

	/* Set_PCM */
	memcpy(pcm, fw->data + 0x08, sizeof(pcm));
	/* Set_Radio */
	memcpy(radio, fw->data + 0x0c, 6);
	memcpy(radio + 6, fw->data + 0x24, 2);
	/* Set_TX_Power_Offset */
	memcpy(txp, fw->data + 0x1b, 3);
	memcpy(txp + 3, fw->data + 0x26, 3);
	/* Set_Sleep_Timeout */
	memcpy(sleep_to, fw->data + 0x12, sizeof(sleep_to));

	stpbt_vendor_cmd(hdev, "Set_Local_BD_Addr", 0xfc1a, fw->data, 6);
	stpbt_vendor_cmd(hdev, "Set_PCM", 0xfc72, pcm, sizeof(pcm));
	stpbt_vendor_cmd(hdev, "Set_Radio", 0xfc79, radio, sizeof(radio));
	stpbt_vendor_cmd(hdev, "Set_TX_Power_Offset", 0xfc93, txp, sizeof(txp));
	stpbt_vendor_cmd(hdev, "Set_Sleep_Timeout", 0xfc7a, sleep_to,
			 sizeof(sleep_to));
	stpbt_vendor_cmd(hdev, "Set_SSP_Debug_Mode", 0x1804, &ssp_debug, 1);
	/* Legacy (PIN) pairing: without this the controller completes the
	 * pairing but rejects the stored key later with Authentication
	 * Failed (0x05), which blocks HIDP (gamepads, keyboards). */
	stpbt_vendor_cmd(hdev, "Set_LinkKeyType", 0xfc1b, &lktype, 1);
	stpbt_vendor_cmd(hdev, "commit reset", HCI_OP_RESET, NULL, 0);

	/* The core read the address during its init, before the reset above
	 * could latch ours, so refresh the cached copy to keep mgmt/userspace
	 * in sync with what the controller now reports. */
	skb = __hci_cmd_sync(hdev, HCI_OP_READ_BD_ADDR, 0, NULL,
			     HCI_INIT_TIMEOUT);
	if (!IS_ERR(skb) && skb->len >= 7 && !skb->data[0]) {
		hci_dev_lock(hdev);
		bacpy(&hdev->bdaddr, (bdaddr_t *)&skb->data[1]);
		hci_dev_unlock(hdev);
	}
	if (!IS_ERR(skb))
		kfree_skb(skb);

	release_firmware(fw);
	return 0;
}

static int stpbt_open(struct hci_dev *hdev)
{
	struct stpbt_dev *bt = hci_get_drvdata(hdev);
	int ret;

	ret = mtk_wcn_wmt_func_on(STPBT_WMTDRV_TYPE_BT);
	if (ret < 0) {
		stpbt_err("WMT BT power on failed (%d)\n", ret);
		return ret;
	}

	stpbt_info("BT function on, STP ready=%d\n", mtk_wcn_stp_is_ready());

	if (stpbt_kick_ms)
		schedule_delayed_work(&stpbt_kick_work,
				      msecs_to_jiffies(stpbt_kick_ms));

	bt->rx_pending = false;
	bt->rx_task = kthread_run(stpbt_rx_thread, bt, "stpbt_rx");
	if (IS_ERR(bt->rx_task)) {
		ret = PTR_ERR(bt->rx_task);
		bt->rx_task = NULL;
		stpbt_err("failed to start rx thread (%d)\n", ret);
		return ret;
	}

	return 0;
}

static int stpbt_close(struct hci_dev *hdev)
{
	struct stpbt_dev *bt = hci_get_drvdata(hdev);

	cancel_delayed_work_sync(&stpbt_kick_work);

	if (bt->rx_task) {
		kthread_stop(bt->rx_task);
		bt->rx_task = NULL;
	}

	mtk_wcn_wmt_func_off(STPBT_WMTDRV_TYPE_BT);

	return 0;
}

static int stpbt_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	u8 type = hci_skb_pkt_type(skb);
	int ret;

	if (stpbt_auto_cancel_ms && type == HCI_COMMAND_PKT && skb->len >= 3 &&
	    (skb->data[0] | (skb->data[1] << 8)) == HCI_OP_INQUIRY)
		schedule_delayed_work(&stpbt_cancel_work,
				      msecs_to_jiffies(stpbt_auto_cancel_ms));

	/* STP carries the H4 type byte as part of the payload. */
	skb_push(skb, 1);
	skb->data[0] = type;
	stpbt_dump("tx", skb->data, skb->len);

	ret = mtk_wcn_stp_send_data(skb->data, skb->len, STPBT_TASK_INDX);

	kfree_skb(skb);

	if (ret <= 0) {
		if (ret < 0)
			stpbt_err("stp send failed (%d)\n", ret);
		return -EAGAIN;
	}

	return 0;
}

static int __init stpbt_init(void)
{
	struct stpbt_dev *bt;
	int ret;

	bt = kzalloc(sizeof(*bt), GFP_KERNEL);
	if (!bt)
		return -ENOMEM;

	init_waitqueue_head(&bt->rx_wq);

	bt->hdev = hci_alloc_dev();
	if (!bt->hdev) {
		kfree(bt);
		return -ENOMEM;
	}

	bt->hdev->bus = HCI_VIRTUAL;
	bt->hdev->open = stpbt_open;
	bt->hdev->close = stpbt_close;
	bt->hdev->send = stpbt_send;
	bt->hdev->post_init = stpbt_post_init;
	hci_set_drvdata(bt->hdev, bt);

	ret = mtk_wcn_stp_register_event_cb(STPBT_TASK_INDX, stpbt_rx_event_cb);
	if (ret) {
		stpbt_err("failed to register STP BT event cb (%d)\n", ret);
		hci_free_dev(bt->hdev);
		kfree(bt);
		return ret;
	}

	ret = hci_register_dev(bt->hdev);
	if (ret < 0) {
		stpbt_err("hci_register_dev failed (%d)\n", ret);
		mtk_wcn_stp_register_event_cb(STPBT_TASK_INDX, NULL);
		hci_free_dev(bt->hdev);
		kfree(bt);
		return ret;
	}

	stpbt = bt;
	stpbt_info("registered %s\n", bt->hdev->name);
	return 0;
}

static void __exit stpbt_exit(void)
{
	struct stpbt_dev *bt = stpbt;

	if (!bt)
		return;

	stpbt = NULL;
	hci_unregister_dev(bt->hdev);
	mtk_wcn_stp_register_event_cb(STPBT_TASK_INDX, NULL);
	hci_free_dev(bt->hdev);
	kfree(bt);
}

module_init(stpbt_init);
module_exit(stpbt_exit);

MODULE_AUTHOR("iPlay30 mainline port");
MODULE_DESCRIPTION("MediaTek STP/BTIF HCI transport (CONNAC/MT6631)");
MODULE_LICENSE("GPL");
