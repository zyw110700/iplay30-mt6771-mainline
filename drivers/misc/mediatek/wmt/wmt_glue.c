// SPDX-License-Identifier: GPL-2.0-only
/*
 * Glue between the vendor WMT/STP stack and mainline:
 *  - the two KERNEL_* debug helpers the vendor code calls
 *  - entry point that starts the stack (added once STP/WMT are wired up)
 */
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include "osal_typedef.h"
#include "wmt_exp.h"
#include "wmt_core.h"
#include "osal.h"
#include <linux/export.h>
#include <linux/module.h>
#include <linux/mutex.h>

void KERNEL_show_stack(struct task_struct *task, unsigned long *sp)
{
	dump_stack();
}
EXPORT_SYMBOL(KERNEL_show_stack);

void KERNEL_dump_thread_state(const char *name)
{
	struct task_struct *t;

	rcu_read_lock();
	for_each_process(t) {
		if (!name || strstr(t->comm, name))
			dump_stack();
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL(KERNEL_dump_thread_state);

static int wmt_inited;

static int wmt_set_hif_btif(void);
extern MTK_WCN_BOOL mtk_wcn_wmt_func_off(ENUM_WMTDRV_TYPE_T type);

static void wmt_power_work_fn(struct work_struct *work);
static DECLARE_WORK(wmt_power_work, wmt_power_work_fn);
static unsigned int wmt_power_action = 1;	/* 1 = on, 0 = off */

static void wmt_power_work_fn(struct work_struct *work)
{
	if (!wmt_inited) {
		pr_err("WMT: trigger before stack init\n");
		return;
	}

	pr_info("WMT: power action %u start\n", wmt_power_action);

	if (wmt_power_action == 1) {
		wmt_set_hif_btif();
		pr_info("WMT: func_on(WIFI) -> %d\n",
			mtk_wcn_wmt_func_on(WMTDRV_TYPE_WIFI));
	} else if (wmt_power_action == 2) {
		/* BT only: exercises the WMT <-> MCU command channel without
		 * pulling in the wlan driver (bench diagnostics).
		 */
		wmt_set_hif_btif();
		pr_info("WMT: func_on(BT) -> %d\n",
			mtk_wcn_wmt_func_on(WMTDRV_TYPE_BT));
	} else {
		pr_info("WMT: func_off -> %d\n",
			mtk_wcn_wmt_func_off(WMTDRV_TYPE_WIFI));
	}

	pr_info("WMT: power action %u done\n", wmt_power_action);
}

/* echo 1 > /sys/module/wmt_drv/parameters/trigger   (power on WiFi)
 * echo 2 > ...                                      (power on BT only)
 * echo 0 > ...                                      (power off WiFi)
 */
static int wmt_trigger_set(const char *val, const struct kernel_param *kp)
{
	unsigned int v;
	int ret = kstrtouint(val, 0, &v);

	if (ret)
		return ret;

	wmt_power_action = v;
	schedule_work(&wmt_power_work);
	return 0;
}

static const struct kernel_param_ops wmt_trigger_ops = {
	.set = wmt_trigger_set,
};

module_param_cb(trigger, &wmt_trigger_ops, NULL, 0200);
MODULE_PARM_DESC(trigger, "1 = power on WiFi (async), 2 = power on BT only, 0 = power off");

/* ---- module entry points ---- */

static unsigned int wmt_start_on;
module_param_named(start_on, wmt_start_on, uint, 0644);
MODULE_PARM_DESC(start_on, "power on the connectivity MCU after init (1 = WiFi)");

extern INT32 stp_drv_init(VOID);
extern INT32 wmt_lib_init(VOID);
extern INT32 wmt_dev_dbg_setup(VOID);
extern INT32 wmt_dev_dbg_remove(VOID);
extern VOID wmt_lib_register_thermal_ctrl_cb(LONG (*query)(VOID));
extern VOID wmt_lib_register_trigger_assert_cb(INT32 (*assert)(VOID));
extern LONG wmt_dev_tm_temp_query(VOID);
extern INT32 wmt_lib_trigger_assert(VOID);
extern MTK_WCN_BOOL mtk_wcn_wmt_func_on(ENUM_WMTDRV_TYPE_T type);
extern INT32 wmt_lib_set_hif(ULONG hifconf);
extern P_WMT_HIF_CONF wmt_lib_get_hif(VOID);
extern P_OSAL_OP wmt_lib_get_free_op(VOID);
extern MTK_WCN_BOOL wmt_lib_put_act_op(P_OSAL_OP pOp);
static inline int wmt_dev_bgw_desense_init(void)
{
	return 0;
}

/* the vendor stack expects these to exist; see wmt_dev.c */
static DEFINE_MUTEX(wmt_i_lock);

/* register BTIF as the WMT transport (mirrors the vendor WMT_IOCTL_HIF_INFO) */
static int wmt_set_hif_btif(void)
{
	P_OSAL_OP pOp;
	P_OSAL_SIGNAL pSignal;
	P_WMT_HIF_CONF pHif;
	int iRet;

	iRet = wmt_lib_set_hif(0x23);	/* STP_BTIF_FULL | (WMT_FM_COMM << 4) */
	if (iRet) {
		pr_err("WMT: wmt_lib_set_hif(BTIF) failed %d\n", iRet);
		return iRet;
	}

	pOp = wmt_lib_get_free_op();
	if (!pOp) {
		pr_err("WMT: no free op for HIF_CONF\n");
		return -1;
	}

	pSignal = &pOp->signal;
	pOp->op.opId = WMT_OPID_HIF_CONF;
	pHif = wmt_lib_get_hif();
	osal_memcpy(&pOp->op.au4OpData[0], pHif, sizeof(WMT_HIF_CONF));
	pOp->op.u4InfoBit = WMT_OP_HIF_BIT;
	pSignal->timeoutValue = 0;

	if (wmt_lib_put_act_op(pOp) == MTK_WCN_BOOL_FALSE) {
		pr_err("WMT: put HIF_CONF op failed\n");
		return -1;
	}

	pr_info("WMT: BTIF registered as WMT transport\n");
	return 0;
}

int wmt_start(void)
{
	int ret;

	if (wmt_inited)
		return 0;

	/* STP core has to exist before wmt_lib_init() (vendor does this in
	 * wmt_dev_init()); without it stp_core_ctx->p_psm is garbage.
	 */
	ret = stp_drv_init();
	if (ret) {
		pr_err("WMT: stp_drv_init failed (%d)\n", ret);
		return ret;
	}

	ret = wmt_lib_init();
	if (ret) {
		pr_err("WMT: wmt_lib_init failed (%d)\n", ret);
		return ret;
	}

	wmt_lib_register_thermal_ctrl_cb(wmt_dev_tm_temp_query);
	wmt_lib_register_trigger_assert_cb(wmt_lib_trigger_assert);
	wmt_dev_bgw_desense_init();

	/* /proc/driver/wmt_dbg: func_ctrl / register / EMI inspection hooks
	 * used on the bench (the vendor calls this from wmt_dev_init()).
	 */
	ret = wmt_dev_dbg_setup();
	if (ret)
		pr_err("WMT: wmt_dev_dbg_setup failed (%d)\n", ret);

	wmt_inited = 1;
	pr_info("WMT: stack initialised\n");

	if (wmt_start_on) {
		wmt_power_action = 1;
		schedule_work(&wmt_power_work);
		pr_info("WMT: power-on queued\n");
	}

	return 0;
}

static int __init wmt_module_init(void)
{
	int ret;

	mutex_lock(&wmt_i_lock);
	ret = wmt_start();
	mutex_unlock(&wmt_i_lock);

	return ret;
}

static void __exit wmt_module_exit(void)
{
	pr_info("WMT: module exit\n");
	wmt_dev_dbg_remove();
}

module_init(wmt_module_init);
module_exit(wmt_module_exit);

/* ---- stubs for the debug-only vendor helpers we do not compile ----
 * /proc/driver/wmt_dbg (linux/wmt_dbg.c) references the power-meter ("step")
 * tracer, the thermal alarm and the APO/temperature setters that live in
 * linux/wmt_dev.c and linux/wmt_step.c.  Those are Android debug features we
 * do not need here; keep the proc interface and let the hooks be no-ops.
 */
INT32 wmt_dev_apo_ctrl(UINT32 enable)
{
	return 0;
}

VOID wmt_dev_set_temp_threshold(INT32 val)
{
}

int connsys_log_alarm_enable(unsigned int sec)
{
	return 0;
}

int connsys_log_alarm_disable(void)
{
	return 0;
}

int wmt_alarm_init(void)
{
	return 0;
}

int wmt_alarm_deinit(void)
{
	return 0;
}

int wmt_alarm_start(unsigned int sec)
{
	return 0;
}

int wmt_alarm_cancel(void)
{
	return 0;
}

void wmt_step_init(void)
{
}

void wmt_step_deinit(void)
{
}

void wmt_step_print_version(void)
{
}


MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek WMT/STP connectivity stack (mainline port)");
