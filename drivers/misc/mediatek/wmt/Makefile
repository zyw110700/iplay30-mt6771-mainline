# SPDX-License-Identifier: GPL-2.0-only
#
# MediaTek WMT/STP stack -- built straight out of the vendor tree at
# vendor-connectivity/common/common_main so there is only one copy of the
# code.
#
# $(src) is "." for `make M=drivers/misc/mediatek/wmt` but
# "drivers/misc/mediatek/wmt" for an in-tree build, so paths are anchored
# either on this directory (object lists) or on the absolute module path
# (include flags).

# Paths in wmt_drv-y must be relative to this directory (kbuild prepends
# $(obj)/), while -I paths must survive both the in-tree build (cwd is the
# kernel root) and `make M=` (cwd is this directory), so those are absolute.
WMT_ABS := $(abspath $(srctree)/drivers/misc/mediatek/wmt)
VENDOR  := ../../../../vendor-connectivity/common/common_main
VDBG    := ../../../../vendor-connectivity/common/debug_utility

obj-$(CONFIG_MTK_WMT) += wmt_drv.o

wmt_drv-y := wmt_glue.o \
	     wmt_plat_glue.o \
	     $(VENDOR)/linux/osal.o \
	     $(VENDOR)/core/stp_core.o \
	     $(VENDOR)/core/psm_core.o \
	     $(VENDOR)/core/stp_exp.o \
	     $(VENDOR)/linux/stp_btif.o \
	     $(VENDOR)/core/btm_core.o \
	     $(VENDOR)/core/wmt_core.o \
	     $(VENDOR)/core/wmt_exp.o \
	     $(VENDOR)/core/wmt_ctrl.o \
	     $(VENDOR)/core/wmt_func.o \
	     $(VENDOR)/core/wmt_conf.o \
	     $(VENDOR)/linux/stp_dbg_combo.o \
	     $(VENDOR)/core/wmt_lib.o \
	     $(VENDOR)/core/wmt_ic_soc.o \
	     $(VENDOR)/platform/wmt_plat_alps.o \
	     $(VENDOR)/linux/stp_dbg.o \
	     $(VENDOR)/linux/wmt_dbg.o \
	     $(VDBG)/ring.o

# symbols exported by the BTIF module
KBUILD_EXTRA_SYMBOLS += $(WMT_ABS)/../btif/Module.symvers

ccflags-y += -D CONFIG_MTK_WCN_ARM64
ccflags-y += -D MTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT
ccflags-y += -D WMT_IDC_SUPPORT=0
ccflags-y += -D WMT_PLAT_ALPS=1
# No Android wmt_loader daemon on this port: answer "srh_patch" in kernel.
ccflags-y += -D CFG_WMT_LOCAL_PATCH_SEARCH=1
ccflags-y += -I$(WMT_ABS)/include
ccflags-y += -include $(WMT_ABS)/include/wmt_compat.h
ccflags-y += -I$(WMT_ABS)/$(VENDOR)/include
ccflags-y += -I$(WMT_ABS)/$(VENDOR)/linux/include
ccflags-y += -I$(WMT_ABS)/$(VENDOR)/core/include
ccflags-y += -I$(WMT_ABS)/$(VENDOR)/platform/include
ccflags-y += -I$(WMT_ABS)/$(VENDOR)/../common_detect
ccflags-y += -I$(WMT_ABS)/$(VDBG)
ccflags-y += -I$(WMT_ABS)/../btif/common/inc
# Enables the 0xDB9DB9 unlock of /proc/driver/wmt_dbg so that read-only
# diagnostics (0x8 chipid via STP, 0x19 EMI info, 0x1b CPUPCR) can be used
# on this port.  Without it only 0x7/0x15/0x2e/0x2f/0x32 are accepted.
ccflags-y += -D WMT_DBG_SUPPORT=1
