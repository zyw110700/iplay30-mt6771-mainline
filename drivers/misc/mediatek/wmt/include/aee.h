/* SPDX-License-Identifier: GPL-2.0-only */
/* Minimal stand-in for MediaTek's AEE (Android Exception Engine) header. */
#ifndef __WMT_MAINLINE_AEE_H_
#define __WMT_MAINLINE_AEE_H_

#include <linux/kernel.h>

#define DB_OPT_DEFAULT 0
#define DB_OPT_FTRACE  0

#define aee_kernel_warning_api(fmt, ...) \
	pr_warn("WMT: " fmt, ##__VA_ARGS__)

#define aee_kernel_exception_api(...) \
	pr_err("WMT: aee kernel exception\n")

#endif
