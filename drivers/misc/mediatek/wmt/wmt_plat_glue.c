// SPDX-License-Identifier: GPL-2.0-only
/*
 * Platform glue for the MediaTek WMT/STP stack on this port.
 *
 *  - SDIO transport stubs (this port only uses BTIF)
 *  - the "combo stub" power hook
 *  - mtk_wcn_consys_* adapters over drivers/soc/mediatek/mt6771-consys.c
 *  - a placeholder for the non-SOC chip ops table
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/export.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/firmware.h>

#include "osal_typedef.h"
#include "wmt_plat.h"
#include "mtk_wcn_consys_hw.h"
#include "mtk_wcn_cmb_hw.h"
#include "stp_sdio.h"
#include "wmt_core.h"
#include "wmt_detect.h"
#include "wmt_gpio.h"

/* ---------------- SDIO transport stubs ---------------- */

MTK_WCN_STP_SDIO_HIF_INFO g_stp_sdio_host_info;
EXPORT_SYMBOL(g_stp_sdio_host_info);

VOID stp_sdio_retry_flag_ctrl(INT32 flag)
{
}
EXPORT_SYMBOL(stp_sdio_retry_flag_ctrl);

VOID stp_sdio_dump_register(VOID)
{
}
EXPORT_SYMBOL(stp_sdio_dump_register);

INT32 board_sdio_ctrl(UINT32 type, UINT32 on)
{
	return 0;
}
EXPORT_SYMBOL(board_sdio_ctrl);

/* ---------------- combo stub power hook ---------------- */

INT32 mtk_wcn_cmb_hw_pwr_on(VOID)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_cmb_hw_pwr_on);

/* ---------------- consys adapters ---------------- */

/* exported by drivers/soc/mediatek/mt6771-consys.c */
extern int mt6771_consys_power_on(void);
extern void mt6771_consys_power_off(void);

/* vendor WMT core: write every registered ROM patch blob into the EMI window */
extern INT32 mtk_wcn_soc_rom_patch_dwn(UINT32 ip_ver, UINT32 fw_ver);

/* 1 (default): stage the ROMv4 patch in the EMI window; the MCU picks it up
 *              when the WMT reset restarts it.
 * 2:          do not download any ROM patch at all ("bare ROM" A/B mode).
 * 0:          keep the legacy STP fragment download over BTIF.
 * Both 1 and 2 still run the WMT reset and the MCU clock tables; the module
 * can be reloaded to switch modes without touching anything else.
 */
static unsigned int wmt_rom_patch_emi = 1;
module_param_named(rom_patch_emi, wmt_rom_patch_emi, uint, 0644);
MODULE_PARM_DESC(rom_patch_emi, "1 = stage the ROM patch in EMI (default), 2 = no ROM patch, 0 = legacy STP download");

/* The MT6631 boot ROM reads its patch out of the EMI window when it starts
 * running, i.e. when the CONN MTCMOS domain is switched on and the CPU reset
 * is released.  Nothing in the later WMT script (init_table_3) really restarts
 * the core, so staging the blob after mt6771_consys_power_on() is too late:
 * the ROM has already booted.  The EMI window is a reserved DRAM range and is
 * writable while CONNSYS is off, so the default is to stage first.
 * Set to 0 to keep the old "power on, then stage" order for A/B testing.
 */
static unsigned int wmt_patch_before_pwr = 1;
module_param_named(patch_before_pwr, wmt_patch_before_pwr, uint, 0644);
MODULE_PARM_DESC(patch_before_pwr, "1 = write the ROM patch into EMI before releasing MCU reset (default)");

/* A/B knob: also stage the second blob of the ROMv4 patch set
 * (ROMv4_be_patch_1_0_hdr.bin).  Its header has no 0xf0 EMI address marker -
 * the target address is 0x00098022, i.e. the ILM patch slot - so the plain
 * ROMv4 registration path drops it.  With this enabled the blob is written to
 * EMI offset 0x098000 as well, which is where the MCU boot ROM looks for the
 * second half of the set.
 */
unsigned int wmt_rom_patch_extra;
module_param_named(rom_patch_extra, wmt_rom_patch_extra, uint, 0644);
MODULE_PARM_DESC(rom_patch_extra, "1 = also stage ROMv4_be_patch_1_0 at EMI offset 0x98000");

/* The four address bytes of a patch header are passed to the MCU verbatim in
 * the STP "P address" command.  Byte 0 of the blobs on this board is 0x21 /
 * 0x22 (patch set revision), bytes 1..3 are the target address.  The vendor
 * launcher hands the address over with the low byte cleared, which is what
 * the download code expects (0x078000 / 0x098000); keep the raw byte only for
 * A/B testing.
 */
unsigned int wmt_patch_addr_low;
module_param_named(patch_addr_low, wmt_patch_addr_low, uint, 0644);
MODULE_PARM_DESC(patch_addr_low, "1 = pass the low byte of the patch header address verbatim");

/* Bytes to strip from a patch blob before it is streamed over STP.  0 keeps
 * the vendor default (sizeof(struct WMT_PATCH) == 28).  The blobs on this
 * board look like a 48 byte "wmt_rom_patch" header followed by a patch table,
 * so this allows trying 48 (or 30) without a rebuild.
 */
unsigned int wmt_patch_hdr_skip;
module_param_named(patch_hdr_skip, wmt_patch_hdr_skip, uint, 0644);
MODULE_PARM_DESC(patch_hdr_skip, "bytes of patch header to strip before STP download (0 = 28)");

UINT64 mtk_wcn_consys_get_options(VOID)
{
	UINT64 options = OPT_POWER_ON_DLM_TABLE |
			 OPT_SET_MCUCLK_TABLE_3_4 |
			 OPT_SET_WIFI_EXT_COMPONENT |
			 OPT_WIFI_LTE_COEX |
			 OPT_BT_TSSI_FROM_WIFI_CONFIG_NEW_OPID |
			 OPT_INIT_COEX_AFTER_RF_CALIBRATION |
			 OPT_COEX_CONFIG_ADJUST |
			 OPT_SET_OSC_TYPE |
			 OPT_SET_COREDUMP_LEVEL |
			 OPT_WIFI_LTE_COEX_TABLE_2 |
			 OPT_NORMAL_PATCH_DWN_2;

	if (wmt_rom_patch_emi == 1)
		options |= OPT_ROM_PATCH_DWN_TO_EMI;
	else if (wmt_rom_patch_emi == 2)
		options |= OPT_DISABLE_ROM_PATCH_DWN;

	return options;
}
EXPORT_SYMBOL(mtk_wcn_consys_get_options);

INT32 mtk_wcn_consys_hw_deinit(VOID)
{
	mt6771_consys_power_off();
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_deinit);

INT32 mtk_wcn_consys_hw_efuse_paldo_ctrl(UINT32 enable, UINT32 co_clock_type)
{
	/* VCN28 is enabled by the consys power sequence on this port. */
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_efuse_paldo_ctrl);

INT32 mtk_wcn_consys_sleep_info_read_all_ctrl(P_CONSYS_STATE state)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_sleep_info_read_all_ctrl);

/* ---------------- consys helpers we do not implement ---------------- */

INT32 mtk_consys_check_reg_readable_by_addr(SIZE_T addr)
{
	/* The AP can always read the CONNSYS register window here. */
	return 1;
}
EXPORT_SYMBOL(mtk_consys_check_reg_readable_by_addr);

VOID mtk_wcn_force_trigger_assert_debug_pin(VOID)
{
}
EXPORT_SYMBOL(mtk_wcn_force_trigger_assert_debug_pin);

INT32 mtk_wcn_consys_hw_bt_paldo_ctrl(UINT32 enable)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_bt_paldo_ctrl);

/* ---------------- remaining consys / dbg stubs ---------------- */

INT32 mt_combo_plt_exit_deep_idle(UINT32 if_type);
INT32 mt_combo_plt_exit_deep_idle(UINT32 if_type)
{
	return 0;
}
EXPORT_SYMBOL(mt_combo_plt_exit_deep_idle);

INT32 mtk_consys_check_reg_readable(VOID)
{
	return 1;
}
EXPORT_SYMBOL(mtk_consys_check_reg_readable);

UINT32 mtk_consys_get_gps_lna_pin_num(VOID)
{
	return 0;
}
EXPORT_SYMBOL(mtk_consys_get_gps_lna_pin_num);

INT32 mtk_wcn_cmb_hw_state_show(VOID)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_cmb_hw_state_show);

INT32 mtk_wcn_consys_detect_adie_chipid(UINT32 co_clock_type)
{
	/* The chip id is polled by mt6771-consys.c during power on. */
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_detect_adie_chipid);

struct pinctrl *mtk_wcn_consys_get_pinctrl(VOID)
{
	return NULL;
}
EXPORT_SYMBOL(mtk_wcn_consys_get_pinctrl);

VOID mtk_wcn_consys_hang_debug(VOID)
{
}
EXPORT_SYMBOL(mtk_wcn_consys_hang_debug);

UINT32 mtk_wcn_consys_read_cpupcr(VOID)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_read_cpupcr);

INT32 mtk_wcn_consys_sleep_info_restore(VOID)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_sleep_info_restore);

INT32 stp_dbg_soc_core_dump(INT32 dump_sink)
{
	return 0;
}
EXPORT_SYMBOL(stp_dbg_soc_core_dump);

/* ---------------- second batch of platform stubs ---------------- */

INT32 mtk_wcn_cmb_hw_rst(VOID)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_cmb_hw_rst);

INT32 mtk_wcn_consys_co_clock_type(VOID)
{
	return 0;	/* not co-clock on this board (WMT_SOC.cfg) */
}
EXPORT_SYMBOL(mtk_wcn_consys_co_clock_type);

INT32 mtk_wcn_consys_dump_gating_state(P_CONSYS_STATE state)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_dump_gating_state);

INT32 mtk_wcn_consys_hw_pwr_off(UINT32 co_clock_type)
{
	mt6771_consys_power_off();
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_pwr_off);

INT32 mtk_wcn_consys_hw_vcn28_ctrl(UINT32 enable)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_vcn28_ctrl);

UINT32 mtk_wcn_consys_jtag_flag_ctrl(UINT32 en)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_jtag_flag_ctrl);

INT32 mtk_wcn_hif_sdio_f0_readb(MTK_WCN_HIF_SDIO_CLTCTX ctx, UINT32 offset, PUINT8 pvb)
{
	return -1;
}
EXPORT_SYMBOL(mtk_wcn_hif_sdio_f0_readb);

VOID stp_sdio_txdbg_dump(VOID)
{
}
EXPORT_SYMBOL(stp_sdio_txdbg_dump);

LONG wmt_dev_tm_temp_query(VOID)
{
	return 0;
}
EXPORT_SYMBOL(wmt_dev_tm_temp_query);

/* ---------------- antenna / EMI / misc ---------------- */

VOID mtk_wcn_consys_ic_get_ant_sel_cr_addr(PUINT32 default_invert_cr, PUINT32 default_invert_bit)
{
	if (default_invert_cr)
		*default_invert_cr = 0;
	if (default_invert_bit)
		*default_invert_bit = 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_ic_get_ant_sel_cr_addr);

/* Same offsets the vendor MT6771 platform uses (see mt6771.h). */
static EMI_CTRL_STATE_OFFSET wmt_emi_state_off = {
	.emi_apmem_ctrl_state = EXP_APMEM_CTRL_STATE,
	.emi_apmem_ctrl_host_sync_state = EXP_APMEM_CTRL_HOST_SYNC_STATE,
	.emi_apmem_ctrl_host_sync_num = EXP_APMEM_CTRL_HOST_SYNC_NUM,
	.emi_apmem_ctrl_chip_sync_state = EXP_APMEM_CTRL_CHIP_SYNC_STATE,
	.emi_apmem_ctrl_chip_sync_num = EXP_APMEM_CTRL_CHIP_SYNC_NUM,
	.emi_apmem_ctrl_chip_sync_addr = EXP_APMEM_CTRL_CHIP_SYNC_ADDR,
	.emi_apmem_ctrl_chip_sync_len = EXP_APMEM_CTRL_CHIP_SYNC_LEN,
	.emi_apmem_ctrl_chip_print_buff_start = EXP_APMEM_CTRL_CHIP_PRINT_BUFF_START,
	.emi_apmem_ctrl_chip_print_buff_len = EXP_APMEM_CTRL_CHIP_PRINT_BUFF_LEN,
	.emi_apmem_ctrl_chip_print_buff_idx = EXP_APMEM_CTRL_CHIP_PRINT_BUFF_IDX,
	.emi_apmem_ctrl_chip_int_status = EXP_APMEM_CTRL_CHIP_INT_STATUS,
	.emi_apmem_ctrl_chip_paded_dump_end = EXP_APMEM_CTRL_CHIP_PAGED_DUMP_END,
	.emi_apmem_ctrl_host_outband_assert_w1 = EXP_APMEM_CTRL_HOST_OUTBAND_ASSERT_W1,
	.emi_apmem_ctrl_chip_page_dump_num = EXP_APMEM_CTRL_CHIP_PAGE_DUMP_NUM,
	.emi_apmem_ctrl_assert_flag = EXP_APMEM_CTRL_ASSERT_FLAG,
};

/*
 * Base of the DRAM region reserved for CONNSYS (DT consys-reserve-memory).
 * This is also what the WLAN driver uses as gConEmiPhyBase, because its
 * RAM-code sections are addressed from the FW view 0xf0000000.
 */
#define WMT_EMI_RESERVED_BASE	0xbfc00000

/*
 * Everything the WMT side hands to the connectivity firmware (its control
 * block, the paged trace/dump offsets, the ROM patch, the patch build time
 * stamps) is addressed from the FW view 0xf0080000 = CONSYS_EMI_FW_PHY_BASE,
 * which the EMI remap puts at reserved_base + 0x80000.  That is exactly where
 * mt6771.h's offsets are anchored: paged dump 0x8400 -> 0xbfc88400, coredump
 * 0x80000 -> 0xbfd00000.
 *
 * Using the reserved base here instead silently writes the ROM patch to
 * 0xbfc78000, i.e. 512 KiB below the place the connectivity ROM actually
 * looks at (0xbfd78000).  The MCU then boots unpatched.
 * Keep it switchable for A/B runs.
 */
#define WMT_EMI_APMEM_SHIFT	0x80000

static unsigned int wmt_emi_ap_shift = WMT_EMI_APMEM_SHIFT;
module_param_named(emi_ap_shift, wmt_emi_ap_shift, uint, 0644);
MODULE_PARM_DESC(emi_ap_shift, "offset of the CONNSYS APMEM window inside the reserved DRAM region (default 0x80000)");

static CONSYS_EMI_ADDR_INFO wmt_emi_info = {
	.emi_phy_addr = 0xf0080000,
	.emi_ap_phy_addr = WMT_EMI_RESERVED_BASE + WMT_EMI_APMEM_SHIFT,
	.paged_trace_off = 0x400,
	.paged_dump_off = 0x8400,
	.full_dump_off = 0x10400,
	.emi_remap_offset = 0x380,
	.p_ecso = &wmt_emi_state_off,
	.emi_size = 0x200000,
	.emi_core_dump_offset = 0x80000,
};

P_CONSYS_EMI_ADDR_INFO mtk_wcn_consys_soc_get_emi_phy_add(VOID)
{
	wmt_emi_info.emi_ap_phy_addr = WMT_EMI_RESERVED_BASE + wmt_emi_ap_shift;
	return &wmt_emi_info;
}
EXPORT_SYMBOL(mtk_wcn_consys_soc_get_emi_phy_add);

INT32 mtk_consys_is_calibration_backup_restore_support(VOID)
{
	return 0;
}
EXPORT_SYMBOL(mtk_consys_is_calibration_backup_restore_support);

INT32 mtk_wcn_consys_hw_state_show(VOID)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_state_show);

INT32 mtk_wcn_consys_hw_wifi_paldo_ctrl(UINT32 enable)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_wifi_paldo_ctrl);

INT32 mtk_wcn_wmt_chipid_query(VOID)
{
	return 0x6771;
}
EXPORT_SYMBOL(mtk_wcn_wmt_chipid_query);

INT32 stp_sdio_wake_up_ctrl(MTK_WCN_HIF_SDIO_CLTCTX ctx)
{
	return 0;
}
EXPORT_SYMBOL(stp_sdio_wake_up_ctrl);

INT32 mtk_wcn_cmb_stub_reg(P_CMB_STUB_CB p_stub_cb)
{
	return 0;
}

INT32 mtk_wcn_cmb_stub_unreg(VOID)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_cmb_stub_unreg);
EXPORT_SYMBOL(mtk_wcn_cmb_stub_reg);

/* no combo LDO/PMU enable GPIOs on this board */
GPIO_CTRL_INFO gpio_ctrl_info;
EXPORT_SYMBOL(gpio_ctrl_info);

/* ---------------- third batch ---------------- */

INT32 mtk_wcn_cmb_hw_init(P_PWR_SEQ_TIME pPwrSeqTime)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_cmb_hw_init);

VOID mtk_wcn_consys_clock_fail_dump(VOID)
{
}
EXPORT_SYMBOL(mtk_wcn_consys_clock_fail_dump);

UINT32 mtk_wcn_consys_get_adie_chipid(VOID)
{
	return 0x6631;
}
EXPORT_SYMBOL(mtk_wcn_consys_get_adie_chipid);

/*
 * ROMv4 (MT6631) patch staging.
 *
 * The vendor code can either push the ROM patch over STP in 1000 byte
 * fragments (the legacy path) or drop the blob into the CONNSYS EMI window and
 * let the boot ROM pick it up when the MCU starts
 * (mtk_wcn_soc_rom_patch_dwn(), used by other SoCs of the same family).  The
 * legacy path makes this ROM assert, so we stage the blob here instead.
 *
 * On mt6768 the vendor calls this from polling_consys_chipid() *after* the
 * chip id is readable and relies on a real `consys_ic_hw_reset_bit_set` to
 * restart the core so that the ROM sees the patch.  This port has no such
 * hardware reset step (init_table_3 only programs the MCU's clock/HIF
 * registers), so the blob has to be in EMI before consys_power_on() releases
 * the reset - see patch_before_pwr.
 */
static void wmt_stage_rom_patch(void)
{
	INT32 ret;

	if (wmt_rom_patch_emi != 1)
		return;

	pr_info("WMT: staging ROM patch into EMI\n");
	ret = mtk_wcn_soc_rom_patch_dwn(0, 0);
	if (ret)
		pr_err("WMT: ROM patch staging into EMI failed (%d)\n", ret);
	else
		pr_info("WMT: ROM patch staged\n");
}

/*
 * Single entry point for "CONNSYS on" so that the ROM patch is always in DRAM
 * before the MCU starts executing (see patch_before_pwr above).
 */
static INT32 wmt_consys_power_on(void)
{
	INT32 ret;

	if (wmt_patch_before_pwr)
		wmt_stage_rom_patch();

	ret = mt6771_consys_power_on();

	if (!ret && !wmt_patch_before_pwr)
		wmt_stage_rom_patch();

	return ret;
}

INT32 mtk_wcn_consys_hw_rst(UINT32 co_clock_type)
{
	mt6771_consys_power_off();
	return wmt_consys_power_on();
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_rst);

INT32 mtk_wcn_consys_read_irq_info_from_dts(PINT32 irq_num, PUINT32 irq_flag)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_read_irq_info_from_dts);

INT32 mtk_wcn_hif_sdio_wmt_control(WMT_SDIO_FUNC_TYPE func_type, MTK_WCN_BOOL is_on)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_hif_sdio_wmt_control);

INT32 stp_sdio_rw_retry(ENUM_STP_SDIO_HIF_TYPE_T type, UINT32 retry_limit,
		MTK_WCN_HIF_SDIO_CLTCTX clt_ctx, UINT32 offset, PUINT32 pData,
		UINT32 len)
{
	return 0;
}
EXPORT_SYMBOL(stp_sdio_rw_retry);

UINT32 wmt_dev_get_blank_state(VOID)
{
	return 0;
}
EXPORT_SYMBOL(wmt_dev_get_blank_state);

INT32 wmt_dev_patch_put(osal_firmware **ppPatch)
{
	if (!ppPatch || !*ppPatch)
		return -1;

	release_firmware(*ppPatch);
	*ppPatch = NULL;
	return 0;
}
EXPORT_SYMBOL(wmt_dev_patch_put);

/* ---------------- fourth batch ---------------- */

INT32 mt_combo_plt_enter_deep_idle(UINT32 if_type)
{
	return 0;
}
EXPORT_SYMBOL(mt_combo_plt_enter_deep_idle);

INT32 mtk_consys_is_ant_swap_enable_by_hwid(VOID)
{
	return 0;
}
EXPORT_SYMBOL(mtk_consys_is_ant_swap_enable_by_hwid);

VOID mtk_consys_set_mcif_mpu_protection(MTK_WCN_BOOL enable)
{
}
EXPORT_SYMBOL(mtk_consys_set_mcif_mpu_protection);

INT32 mtk_wcn_consys_hw_pwr_on(UINT32 co_clock_type)
{
	return wmt_consys_power_on();
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_pwr_on);

UINT32 mtk_wcn_consys_soc_chipid(VOID)
{
	return 0x6771;
}
EXPORT_SYMBOL(mtk_wcn_consys_soc_chipid);

PUINT8 stp_dbg_soc_id_to_task(UINT32 id)
{
	return (PUINT8) "wmt";
}
EXPORT_SYMBOL(stp_dbg_soc_id_to_task);

/*
 * RX wakeup.  STP's wmt_ctrl_rx() sleeps in wmt_dev_rx_timeout() and is woken
 * by this callback from the transport RX thread.  Both used to be stubs; the
 * empty callback plus a timeout stub that returned 0 made every received
 * packet look like a 2 s timeout, so STP never saw the MCU's replies.
 */
P_OSAL_EVENT gpRxEvent;
UINT32 u4RxFlag;
static atomic_t gRxCount = ATOMIC_INIT(0);

VOID wmt_dev_rx_event_cb(VOID)
{
	u4RxFlag = 1;
	atomic_inc(&gRxCount);
	if (gpRxEvent != NULL) {
		if (unlikely(!gpRxEvent->waitQueue.head.next))
			pr_err("BADWAITQ wmt rx event wq=%px\n",
			       &gpRxEvent->waitQueue);
		else
			wake_up_interruptible(&gpRxEvent->waitQueue);
	}
}
EXPORT_SYMBOL(wmt_dev_rx_event_cb);

VOID wmt_dev_send_cmd_to_daemon(UINT32 cmd)
{
}
EXPORT_SYMBOL(wmt_dev_send_cmd_to_daemon);

PINT8 wmt_uart_port_desc;
EXPORT_SYMBOL(wmt_uart_port_desc);

/* ---------------- fifth batch ---------------- */

static void __iomem *wmt_emi_va;
static DEFINE_MUTEX(wmt_emi_lock);

PUINT8 mtk_wcn_consys_emi_virt_addr_get(UINT32 offset)
{
	mutex_lock(&wmt_emi_lock);
	if (!wmt_emi_va)
		wmt_emi_va = ioremap(0xbfc00000, 0x200000);
	mutex_unlock(&wmt_emi_lock);

	if (!wmt_emi_va)
		return NULL;
	if (offset >= 0x200000)
		return NULL;
	return (PUINT8)(wmt_emi_va + offset);
}
EXPORT_SYMBOL(mtk_wcn_consys_emi_virt_addr_get);

INT32 mtk_consys_dump_osc_state(P_CONSYS_STATE state)
{
	return 0;
}
EXPORT_SYMBOL(mtk_consys_dump_osc_state);

INT32 mtk_wcn_cmb_hw_deinit(VOID)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_cmb_hw_deinit);

INT32 mtk_wcn_cmb_hw_pwr_off(VOID)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_cmb_hw_pwr_off);

INT32 mtk_wcn_consys_hw_init(VOID)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_init);

VOID stp_dbg_soc_read_debug_crs(VOID)
{
}
EXPORT_SYMBOL(stp_dbg_soc_read_debug_crs);

INT32 stp_sdio_retry_flag_get(VOID)
{
	return 0;
}
EXPORT_SYMBOL(stp_sdio_retry_flag_get);

/* ---------------- wmt_dev / wmt_detect minimal versions ---------------- */

INT32 wmt_dev_rx_timeout(P_OSAL_EVENT pEvent)
{
	UINT32 ms = pEvent->timeoutValue;
	LONG lRet = 0;

	gpRxEvent = pEvent;
	if (ms != 0)
		lRet = wait_event_interruptible_timeout(gpRxEvent->waitQueue,
				0 != u4RxFlag, msecs_to_jiffies(ms));
	else
		lRet = wait_event_interruptible(gpRxEvent->waitQueue,
				u4RxFlag != 0);

	u4RxFlag = 0;
	if (atomic_dec_return(&gRxCount)) {
		pr_info("WMT: gRxCount != 0 (%d), reset it!\n",
			atomic_read(&gRxCount));
		atomic_set(&gRxCount, 0);
	}

	return lRet;
}
EXPORT_SYMBOL(wmt_dev_rx_timeout);

INT32 wmt_dev_patch_get(PUINT8 pPatchName, osal_firmware **ppPatch)
{
	const struct firmware *fw = NULL;
	int ret;

	if (!pPatchName || !ppPatch)
		return -1;

	ret = request_firmware(&fw, (const char *)pPatchName, NULL);
	if (ret || !fw) {
		pr_info("WMT: request_firmware(%s) failed %d\n", pPatchName, ret);
		return -1;
	}

	*ppPatch = (osal_firmware *)fw;
	return 0;
}
EXPORT_SYMBOL(wmt_dev_patch_get);

MTK_WCN_BOOL wmt_dev_get_early_suspend_state(VOID)
{
	return MTK_WCN_BOOL_FALSE;
}
EXPORT_SYMBOL(wmt_dev_get_early_suspend_state);

UINT8 wmt_dev_is_close(VOID)
{
	return 0;
}
EXPORT_SYMBOL(wmt_dev_is_close);

ENUM_WMT_CHIP_TYPE wmt_detect_get_chip_type(VOID)
{
	/* MT6771 keeps the connectivity subsystem inside the SoC. */
	return WMT_CHIP_TYPE_SOC;
}
EXPORT_SYMBOL(wmt_detect_get_chip_type);

/* Debug aid: the MCU writes its log into the EMI ring buffer at offset 0x400
 * of the CONNSYS APMEM window and the AP cannot write that region through
 * /dev/mem (the mapping faults), so there is no way to get a clean log per
 * boot from user space.  Writing 1 here zeroes the 32 KiB log window from the
 * kernel before the power-on worker runs the MCU.
 */
static unsigned int wmt_emi_clear_log_val;

static int wmt_emi_clear_log_set(const char *val, const struct kernel_param *kp)
{
	unsigned long v;
	void __iomem *va;

	if (kstrtoul(val, 0, &v))
		return -EINVAL;
	if (!v)
		return 0;

	va = (void __iomem *)mtk_wcn_consys_emi_virt_addr_get(0x400);
	if (!va)
		return -ENOMEM;

	memset_io(va, 0, 0x8000);
	pr_info("wmt: cleared CONNSYS EMI log window (32 KiB)\n");
	return 0;
}

static const struct kernel_param_ops wmt_emi_clear_log_ops = {
	.set = wmt_emi_clear_log_set,
	.get = param_get_uint,
};

module_param_cb(emi_clear_log, &wmt_emi_clear_log_ops,
		&wmt_emi_clear_log_val, 0644);
MODULE_PARM_DESC(emi_clear_log, "write 1 to zero the MCU EMI log window before power-on");

/* Placeholder for the mt6632 (non-SOC) ops table; MT6771 selects the SOC
 * table through wmt_detect_get_chip_type().
 */
WMT_IC_OPS wmt_ic_ops_mt6632;
WMT_IC_OPS wmt_ic_ops_mt6628;
WMT_IC_OPS wmt_ic_ops_mt6620;
WMT_IC_OPS wmt_ic_ops_mt6630;
EXPORT_SYMBOL(wmt_ic_ops_mt6632);
