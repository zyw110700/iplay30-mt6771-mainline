// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal WMT / CONNSYS glue for the vendor MT6631 WLAN driver
 *
 * The vendor driver expects the MediaTek "WMT" stack (drivers/misc/
 * mediatek/wmt, built from vendor-connectivity/common/common_main) to power
 * CONNSYS up, download the ROM patch, restart the MCU, run the RF
 * calibration, register a message callback, and call back into the WLAN
 * driver's HIF probe when WiFi is switched on.
 *
 * This file implements the few WMT entry points the gen3 WLAN driver
 * references that are not exported by wmt_drv.ko:
 *
 *   - mtk_wcn_wmt_wlan_reg/unreg()        exported by wmt_drv.ko: registers
 *                                         the callbacks below with the WMT
 *                                         core, which powers CONNSYS on and
 *                                         then calls wlan_probe_cb(),
 *   - mtk_wcn_wmt_msgcb_reg/unreg()       store the reset notifier (never
 *                                         fired, CONNSYS reset is not
 *                                         supported yet),
 *   - mtk_wcn_wmt_do_reset() etc.         report that a reset is not
 *                                         possible,
 *   - mtk_wcn_wmt_ic_info_get()           chip revision for firmware
 *                                         filename selection,
 *   - mtk_wcn_consys_hw_wifi_paldo_ctrl() no-op, the vendor MT6771
 *                                         implementation is compiled out,
 *   - wmt_plat_read_cpupcr()              CPUPCR register for the reset
 *                                         timeout diagnostics.
 *
 * The CONNSYS power sequence itself stays in the platform driver
 * (drivers/soc/mediatek/mt6771-consys.c) and in the WMT platform glue
 * (drivers/misc/mediatek/wmt/wmt_plat_glue.c).
 */

/*******************************************************************************
 *                    E X T E R N A L   R E F E R E N C E S
 ********************************************************************************
 */
#include "precomp.h"
#include "gl_rst.h"
#include "nic/nic.h"

#include <linux/delay.h>

/* Provided by drivers/soc/mediatek/mt6771-consys.c */
extern int mt6771_consys_power_on(void);
extern void mt6771_consys_power_off(void);
extern bool mt6771_consys_is_powered(void);
extern u32 mt6771_consys_read_cpupcr(void);
extern u32 mt6771_consys_read_chip_id(void);

/* CONNSYS reserved memory, used by the firmware downloader for IEMI/DEMI. */
phys_addr_t gConEmiPhyBase = 0xbfc00000;
EXPORT_SYMBOL(gConEmiPhyBase);

/*******************************************************************************
 *                           P R I V A T E   D A T A
 ********************************************************************************
 */

static PF_WMT_CB apfWmtMsgCb[WMTDRV_TYPE_MAX];

static BOOLEAN fgPowerOffFlag;

/* OPPO's framework hook, not present on mainline: only remembered. */
static set_p2p_mode pfnSetP2pModeHandler;
static BOOLEAN fgWifiDriverLoaded;

/*******************************************************************************
 *                            P U B L I C   F U N C T I O N S
 ********************************************************************************
 */

/*
 * Register the WLAN HIF callbacks with the real WMT stack (wmt_drv.ko).
 *
 * The vendor architecture is: this hook only *registers* the WLAN probe /
 * remove callbacks; the WMT core powers CONNSYS on, downloads the ROM patch,
 * restarts the MCU and runs the RF calibration, and only then calls
 * wlan_probe_cb() so that the WLAN driver downloads its firmware.  The WMT
 * core calls that callback when WiFi is enabled (WMTDRV_TYPE_WIFI func on).
 *
 * We deliberately do not implement mtk_wcn_wmt_wlan_reg()/unreg() here:
 * both are exported by wmt_drv.ko (core/wmt_exp.c) and the kernel resolves
 * the calls in this module against them.  wmt_drv.ko must therefore be
 * loaded before wlan_drv_gen3.ko.
 */
/*
 * mtk_wcn_wmt_wlan_reg() / mtk_wcn_wmt_wlan_unreg() come from wmt_drv.ko
 * (vendor core/wmt_exp.c), see the comment above.
 */

INT_32 mtk_wcn_wmt_msgcb_reg(enum ENUM_WMTDRV_TYPE eType, PF_WMT_CB pCb)
{
	if (eType >= WMTDRV_TYPE_MAX)
		return -EINVAL;

	apfWmtMsgCb[eType] = pCb;
	return 0;
}

INT_32 mtk_wcn_wmt_msgcb_unreg(enum ENUM_WMTDRV_TYPE eType)
{
	if (eType >= WMTDRV_TYPE_MAX)
		return -EINVAL;

	apfWmtMsgCb[eType] = NULL;
	return 0;
}

/*
 * CONNSYS reset is not implemented yet.  Report failure so that the WLAN
 * driver's reset state machine does not wait for a callback that will never
 * come, and so that a firmware assert ends in a driver halt instead of a
 * silent hang.
 */
INT_32 mtk_wcn_wmt_assert_timeout(enum ENUM_WMTDRV_TYPE type, UINT32 reason,
				  INT_32 timeout)
{
	DBGLOG(INIT, WARN, "wmt shim: assert type %d reason 0x%x not supported\n",
	       type, reason);
	return -ENOTSUPP;
}

INT_32 mtk_wcn_wmt_do_reset(enum ENUM_WMTDRV_TYPE type)
{
	DBGLOG(INIT, WARN, "wmt shim: reset type %d not supported\n", type);
	return -ENOTSUPP;
}

INT_32 mtk_wcn_set_connsys_power_off_flag(INT_32 value)
{
	fgPowerOffFlag = !!value;
	return fgPowerOffFlag;
}

UINT_32 mtk_wcn_wmt_ic_info_get(enum ENUM_WMT_CHIPINFO_TYPE_T type)
{
	switch (type) {
	case WMTCHIN_CHIPID:
		return 0x6771;
	case WMTCHIN_HWVER:
		/* A-die revision, as reported by the CONNSYS MCU id register */
		return mt6771_consys_read_chip_id() & BITS(0, 7);
	case WMTCHIN_FWVER:
		return 0;
	default:
		return 0;
	}
}

UINT32 wmt_plat_read_cpupcr(VOID)
{
	return mt6771_consys_read_cpupcr();
}

/*
 * The vendor MT6771 implementation of this hook is compiled out
 * (consys_hw_wifi_vcn33_ctrl() is behind "#if 0"), so the rails are kept on
 * from power-on until the module is removed.
 */
INT_32 mtk_wcn_consys_hw_wifi_paldo_ctrl(UINT_32 enable)
{
	return 0;
}

int wifi_reset_start(VOID)
{
	return 0;
}

int wifi_reset_end(ENUM_RESET_STATUS_T status)
{
	return 0;
}

/*******************************************************************************
 *            O P P O - s p e c i f i c   F R A M E W O R K   H O O K S
 ********************************************************************************
 */

void register_set_p2p_mode_handler(set_p2p_mode handler)
{
	pfnSetP2pModeHandler = handler;
}

/*
 * OPPO lets userspace push the NVRAM/CFG/WLANCfg buffers into the driver
 * through this hook.  On this port the buffers come from the module's own
 * defaults and from /lib/firmware, so the handlers are only remembered.  If
 * the config ever needs to be pushed from userspace, this is the place to
 * call the handler from a debugfs write.
 */
static file_buf_handler apfnFileBufHandler[ENUM_BUF_TYPE_NUM];
static PVOID apvFileBufCtx[ENUM_BUF_TYPE_NUM];

VOID register_file_buf_handler(file_buf_handler handler, PVOID ctx, UINT_8 ucType)
{
	if (ucType >= ENUM_BUF_TYPE_NUM) {
		DBGLOG(INIT, ERROR, "register_file_buf_handler: bad type %u\n",
		       ucType);
		return;
	}

	apfnFileBufHandler[ucType] = handler;
	apvFileBufCtx[ucType] = ctx;
}

void update_driver_loaded_status(uint8_t loaded)
{
	fgWifiDriverLoaded = !!loaded;
}
