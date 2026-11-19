/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Forced-include compatibility header for the MediaTek WMT/STP stack.
 *
 * The vendor code was written against a 4.4 kernel; this fills in the few
 * things that no longer exist in mainline.  It is added with -include so the
 * vendor sources themselves stay untouched.
 */
#ifndef __WMT_MAINLINE_COMPAT_H_
#define __WMT_MAINLINE_COMPAT_H_

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/debug.h>
#include <linux/timekeeping.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/types.h>

/* <linux/time.h> no longer exposes struct timeval to kernel code */
struct timeval {
	long tv_sec;
	long tv_usec;
};

/* strncpy() was removed from linux/string.h */
static inline char *strncpy(char *dest, const char *src, size_t n)
{
	size_t i;

	for (i = 0; i < n && src[i]; i++)
		dest[i] = src[i];
	for (; i < n; i++)
		dest[i] = '\0';
	return dest;
}

/* KERNEL_show_stack()/KERNEL_dump_thread_state() live in wmt_glue.c */
void KERNEL_show_stack(struct task_struct *task, unsigned long *sp);
void KERNEL_dump_thread_state(const char *name);

/* from_timer() was replaced by timer_container_of() */
#ifndef from_timer
#define from_timer(var, callback_timer, timer_fieldname) \
	container_of(callback_timer, typeof(*var), timer_fieldname)
#endif

/* rtc_time_to_tm() -> rtc_time64_to_tm(), ioremap_nocache() -> ioremap() */
#include <linux/rtc.h>
#define rtc_time_to_tm(t, tm)	rtc_time64_to_tm(t, tm)
#define ioremap_nocache(addr, size)	ioremap(addr, size)

/* timer API rename */
#define del_timer(t)		timer_delete(t)
#define del_timer_sync(t)	timer_delete_sync(t)

#endif /* __WMT_MAINLINE_COMPAT_H_ */
