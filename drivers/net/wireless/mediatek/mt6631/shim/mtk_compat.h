/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Small compatibility layer between the MT6631 vendor driver (written for
 * Linux 4.14 / Android) and the mainline kernel.
 *
 * Only mechanical API renames live here.  Anything that changes the driver's
 * behaviour is patched at the call site instead, so this header stays easy to
 * audit.
 */
#ifndef _MTK_COMPAT_H
#define _MTK_COMPAT_H

#include <linux/version.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/sched/clock.h>
#include <linux/timekeeping.h>
#include <linux/time64.h>
#include <linux/io.h>
#include <linux/timer.h>
#include <linux/netdevice.h>
#include <linux/interrupt.h>
#include <linux/sched/debug.h>

/*
 * struct timeval was reduced to a userspace type in 5.6.  A few debug and
 * /proc paths in the vendor driver still use it; on 64-bit the layout is the
 * classic one and the uapi header only defines it for userspace, so there is
 * no clash with the kernel headers.
 */
#ifndef _MTK_STRUCT_TIMEVAL
#define _MTK_STRUCT_TIMEVAL
struct timeval {
	long tv_sec;
	long tv_usec;
};
#endif

static inline void mtk_do_gettimeofday(struct timeval *tv)
{
	struct timespec64 ts;

	ktime_get_real_ts64(&ts);
	tv->tv_sec = (long)ts.tv_sec;
	tv->tv_usec = (long)(ts.tv_nsec / NSEC_PER_USEC);
}
#define do_gettimeofday(tv) mtk_do_gettimeofday(tv)

/* get_monotonic_boottime() is gone; the vendor code always passes a timespec. */
static inline void mtk_get_monotonic_boottime(struct timespec64 *ts)
{
	*ts = ktime_to_timespec64(ktime_get_boottime());
}

#ifndef ioremap_nocache
#define ioremap_nocache(addr, size) ioremap((addr), (size))
#endif

#ifndef del_timer_sync
#define del_timer_sync(t) timer_delete_sync(t)
#endif

#ifndef del_timer
#define del_timer(t) timer_delete(t)
#endif

/* netif_rx_ni() always ran in process context; netif_rx() is the replacement. */
#define netif_rx_ni(skb) netif_rx(skb)

#ifndef VERIFY_READ
#define VERIFY_READ 0
#define VERIFY_WRITE 1
#endif

/*
 * strncpy() was removed from the kernel in 6.9.  strscpy_pad() is the closest
 * replacement: it always NUL terminates and zero pads the rest, exactly what
 * the vendor code relies on.
 */
static inline char *mtk_strncpy(char *dest, const char *src, size_t count)
{
	if (count == 0)
		return dest;
	strscpy_pad(dest, src, count);
	return dest;
}
#define strncpy mtk_strncpy

/*
 * MODULE_SUPPORTED_DEVICE() was removed; it never did anything on modern
 * kernels anyway.
 */
#ifndef MODULE_SUPPORTED_DEVICE
#define MODULE_SUPPORTED_DEVICE(name)
#endif

/*
 * The framebuffer blank notifier was removed from the kernel.  The vendor
 * code only uses it to gate scan results during screen off, so define the
 * historic value: the notifier is never called any more, which disables the
 * feature.
 */
#ifndef FB_EVENT_BLANK
#define FB_EVENT_BLANK 0x09
#endif

/*
 * show_stack() is not exported to modules (and took a third argument since
 * 6.2).  sched_show_task() is the closest exported equivalent: it prints the
 * task state and stack usage, which is what the halt-lock timeout diagnostics
 * are after.
 */
#ifndef KERNEL_show_stack
#define KERNEL_show_stack(task, stack) sched_show_task(task)
#endif

#endif /* _MTK_COMPAT_H */
