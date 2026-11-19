# SPDX-License-Identifier: GPL-2.0-only
#
# MediaTek MT6631 (CONSYS_6771) WLAN driver
#
# This is the vendor "gen3" driver from the MT6771 Android 4.14 kernel
# (wlan/core/gen3), adapted to the mainline kernel.  The original Android
# makefile needed the whole "drivers/misc/mediatek" include farm and Android's
# autoconf.h; this one only points at the headers that are actually used plus
# the small shim/ directory.

MODULE_NAME := wlan_drv_gen3

COMMON_DIR := common/
NIC_DIR    := nic/
MGMT_DIR   := mgmt/
OS_DIR     := os/linux/
HIF_DIR    := os/linux/hif/ahb_sdioLike/

COMMON_OBJS := $(COMMON_DIR)dump.o \
	       $(COMMON_DIR)wlan_lib.o \
	       $(COMMON_DIR)wlan_oid.o \
	       $(COMMON_DIR)wlan_bow.o \
	       $(COMMON_DIR)debug.o \
	       $(COMMON_DIR)wlan_p2p.o \
	       $(COMMON_DIR)fwcfg.o

NIC_OBJS := $(NIC_DIR)nic.o \
	    $(NIC_DIR)nic_tx.o \
	    $(NIC_DIR)nic_rx.o \
	    $(NIC_DIR)nic_pwr_mgt.o \
	    $(NIC_DIR)nic_rate.o \
	    $(NIC_DIR)cmd_buf.o \
	    $(NIC_DIR)que_mgt.o \
	    $(NIC_DIR)nic_cmd_event.o \
	    $(NIC_DIR)p2p_nic.o

OS_OBJS := $(OS_DIR)gl_init.o \
	   $(OS_DIR)gl_kal.o \
	   $(OS_DIR)gl_bow.o \
	   $(OS_DIR)gl_wext.o \
	   $(OS_DIR)gl_wext_priv.o \
	   $(OS_DIR)gl_ate_agent.o \
	   $(OS_DIR)gl_qa_agent.o \
	   $(OS_DIR)gl_hook_api.o \
	   $(OS_DIR)gl_proc.o \
	   $(OS_DIR)gl_rst.o \
	   $(OS_DIR)gl_cfg80211.o \
	   $(OS_DIR)gl_vendor.o \
	   $(OS_DIR)platform.o \
	   $(OS_DIR)gl_p2p.o \
	   $(OS_DIR)gl_p2p_cfg80211.o \
	   $(OS_DIR)gl_p2p_init.o \
	   $(OS_DIR)gl_p2p_kal.o \
	   $(OS_DIR)wmt_shim.o

MGMT_OBJS := $(MGMT_DIR)ais_fsm.o \
	     $(MGMT_DIR)aaa_fsm.o \
	     $(MGMT_DIR)assoc.o \
	     $(MGMT_DIR)auth.o \
	     $(MGMT_DIR)bss.o \
	     $(MGMT_DIR)cnm.o \
	     $(MGMT_DIR)cnm_timer.o \
	     $(MGMT_DIR)cnm_mem.o \
	     $(MGMT_DIR)hem_mbox.o \
	     $(MGMT_DIR)mib.o \
	     $(MGMT_DIR)privacy.o \
	     $(MGMT_DIR)rate.o \
	     $(MGMT_DIR)rlm.o \
	     $(MGMT_DIR)rlm_domain.o \
	     $(MGMT_DIR)rlm_obss.o \
	     $(MGMT_DIR)rlm_protection.o \
	     $(MGMT_DIR)rsn.o \
	     $(MGMT_DIR)saa_fsm.o \
	     $(MGMT_DIR)scan.o \
	     $(MGMT_DIR)scan_fsm.o \
	     $(MGMT_DIR)swcr.o \
	     $(MGMT_DIR)roaming_fsm.o \
	     $(MGMT_DIR)tkip_mic.o \
	     $(MGMT_DIR)hs20.o \
	     $(MGMT_DIR)tdls.o \
	     $(MGMT_DIR)wnm.o \
	     $(MGMT_DIR)qosmap.o \
	     $(MGMT_DIR)wmm.o \
	     $(MGMT_DIR)stats.o \
	     $(MGMT_DIR)p2p_dev_fsm.o \
	     $(MGMT_DIR)p2p_dev_state.o \
	     $(MGMT_DIR)p2p_role_fsm.o \
	     $(MGMT_DIR)p2p_role_state.o \
	     $(MGMT_DIR)p2p_func.o \
	     $(MGMT_DIR)p2p_scan.o \
	     $(MGMT_DIR)p2p_ie.o \
	     $(MGMT_DIR)p2p_rlm.o \
	     $(MGMT_DIR)p2p_assoc.o \
	     $(MGMT_DIR)p2p_bss.o \
	     $(MGMT_DIR)p2p_rlm_obss.o \
	     $(MGMT_DIR)wapi.o

HIF_OBJS := $(HIF_DIR)arm.o \
	    $(HIF_DIR)ahb.o \
	    $(HIF_DIR)sdio_bus_driver.o \
	    $(HIF_DIR)ahb_cqdma.o

# MT6771 uses the CQ-DMA HIF channel whose window is given by the second "reg"
# entry of the wifi node (0x10212180, length 0x6c == CQ_DMA_HIF_LENGTH); with
# CONFIG_OF the driver ioremaps that entry directly.  The hardcoded
# CQ_DMA_HIF_BASE above is only used in the non-DT build, and its printed value
# in HifCqdmaRegDump is likewise just a label, not the address that was read.
# ahb_pdma.c (AP_DMA_HIF_BASE 0x11000080) is the UART AP-DMA controller on this
# SoC, not the CONNSYS HIF DMA.

# The vendor plat_priv.c (os/linux/plat/mt6771) only overrides kalBoostCpu(),
# and gl_kal.c already provides a __weak implementation, so it is not built.

obj-$(CONFIG_MT6631_WLAN) += $(MODULE_NAME).o
$(MODULE_NAME)-objs += $(COMMON_OBJS) $(NIC_OBJS) $(OS_OBJS) $(MGMT_OBJS)
$(MODULE_NAME)-objs += $(HIF_OBJS)

ccflags-y += -DMT6631 -DLINUX -DDBG=0
ccflags-y += -DMTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT
ccflags-y += -DCONFIG_ANDROID=0
ccflags-y += -DCFG_ENABLE_EMI_MPU=0
ccflags-y += -DCFG_SUPPORT_WIFI_FW_LOG_UI=1
ccflags-y += -DCFG_SUPPORT_AEE=0
ccflags-y += -DBUILD_QA_DBG=0
ccflags-y += -DCFG_MET_PACKET_TRACE_SUPPORT=0
ccflags-y += -DWMT_IDC_SUPPORT=0
ccflags-y += -DCFG_SUPPORT_PASSPOINT=0
ccflags-y += -DCFG_HS20_DEBUG=0
ccflags-y += -DCFG_ENABLE_GTK_FRAME_FILTER=0
ccflags-y += -DCFG_SUPPORT_DEBUG_FS=0
ccflags-y += -DWLAN_INCLUDE_PROC
ccflags-y += -DCFG_SUPPORT_AGPS_ASSIST=1
ccflags-y += -DCFG_SUPPORT_TSF_USING_BOOTTIME=1
ccflags-y += -DARP_MONITER_ENABLE=1
ccflags-y += -DFW_CFG_SUPPORT -DENABLED_IN_ENGUSERDEBUG
ccflags-y += -DDRIVER_BUILD_DATE='"mainline"'
ccflags-y += -DCONFIG_MTK_COMBO_CHIP_CONSYS_6771

# $(src) is "." under `make M=drivers/net/wireless/mediatek/mt6631` and the
# module path for an in-tree build, so anchor the include paths absolutely.
WLAN_ABS := $(abspath $(srctree)/drivers/net/wireless/mediatek/mt6631)
ccflags-y += -I$(WLAN_ABS) -I$(WLAN_ABS)/include
ccflags-y += -I$(WLAN_ABS)/include/nic -I$(WLAN_ABS)/include/mgmt
ccflags-y += -I$(WLAN_ABS)/os -I$(WLAN_ABS)/os/linux/include
ccflags-y += -I$(WLAN_ABS)/os/linux/hif/ahb_sdioLike/include
ccflags-y += -I$(WLAN_ABS)/shim
