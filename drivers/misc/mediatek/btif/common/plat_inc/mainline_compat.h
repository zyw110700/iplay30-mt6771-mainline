/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Small compatibility layer so the vendor BTIF driver (written against the
 * MediaTek 4.4 "mt-plat" headers) builds against mainline.
 *
 * The vendor tree provides <mtk_io.h>, <sync_write.h>, <mtk_lpae.h> and
 * <linux/irqchip/mtk-gic-extend.h>; mainline has none of them.  Only a
 * handful of helpers from them are actually used by the BTIF driver.
 */
#ifndef __BTIF_MAINLINE_COMPAT_H_
#define __BTIF_MAINLINE_COMPAT_H_

#include <linux/io.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/timekeeping.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>

/*
 * "Synchronous" register writes: the MediaTek bus needs the store to be
 * visible before the next access, which a following read-back guarantees.
 * The vendor's sync_write.h implements exactly that.  The vendor driver
 * passes plain unsigned long addresses, so take that type here.
 */
static inline void mt_reg_sync_writeb(u8 v, unsigned long a)
{
	writeb(v, (void __iomem *)a);
	(void)readb((void __iomem *)a);
}

static inline void mt_reg_sync_writew(u16 v, unsigned long a)
{
	writew(v, (void __iomem *)a);
	(void)readw((void __iomem *)a);
}

static inline void mt_reg_sync_writel(u32 v, unsigned long a)
{
	writel(v, (void __iomem *)a);
	(void)readl((void __iomem *)a);
}

/* dma_zalloc_coherent() was folded into dma_alloc_coherent() + __GFP_ZERO */
#define dma_zalloc_coherent(dev, size, handle, flag) \
	dma_alloc_coherent((dev), (size), (handle), (flag) | __GFP_ZERO)

/*
 * The mainline MT6771 clock driver does not model CLK_INFRA_BTIF (id 27)
 * yet, so devm_clk_get("btifc") can fail.  Treat clocks as optional and
 * use these wrappers instead of the raw clk_* helpers.
 */
static inline int btif_clk_prepare(struct clk *c)
{
	return c ? clk_prepare(c) : 0;
}

static inline void btif_clk_unprepare(struct clk *c)
{
	if (c)
		clk_unprepare(c);
}

static inline int btif_clk_enable(struct clk *c)
{
	return c ? clk_enable(c) : 0;
}

static inline void btif_clk_disable(struct clk *c)
{
	if (c)
		clk_disable(c);
}

/* vendor GIC debug helper (irqchip/mtk-gic-extend.h): debug only */
static inline void mt_irq_dump_status(int irq)
{
}

/* vendor 36-bit ("4G") DMA address support, nothing to do on mainline */
static inline int enable_4G(void)
{
	return 0;
}

/* <linux/time.h> no longer provides these to kernel code */
struct timeval {
	long tv_sec;
	long tv_usec;
};

static inline void do_gettimeofday(struct timeval *tv)
{
	struct timespec64 ts;

	ktime_get_real_ts64(&ts);
	tv->tv_sec = ts.tv_sec;
	tv->tv_usec = ts.tv_nsec / 1000;
}

/* strncpy() was removed from linux/string.h; the vendor driver relies on
 * the classic semantics (zero padded destination).
 */
static inline char *strncpy(char *dest, const char *src, size_t n)
{
	size_t i;

	for (i = 0; i < n && src[i]; i++)
		dest[i] = src[i];
	for (; i < n; i++)
		dest[i] = '\0';
	return dest;
}

#endif /* __BTIF_MAINLINE_COMPAT_H_ */
