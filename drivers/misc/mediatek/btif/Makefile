# SPDX-License-Identifier: GPL-2.0-only
#
# MediaTek BTIF driver (mainline port)

obj-$(CONFIG_MTK_BTIF) += btif_drv.o

btif_drv-y := common/mtk_btif.o \
	      common/mtk_btif_exp.o \
	      common/btif_dma_plat.o \
	      common/btif_plat.o \
	      common/btif_stp_glue.o

ccflags-y += -I$(src)/common/inc
ccflags-y += -I$(src)/common/plat_inc
