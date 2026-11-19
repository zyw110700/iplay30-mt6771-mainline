/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Stub for the MTK "connectivity build-in adapter".
 *
 * On Android this header pulls in the adaptor glue that turns the WLAN driver
 * into a built-in platform driver.  We build it as a plain module instead, so
 * nothing is needed here.  The include is guarded by
 * !MTK_WCN_BUILT_IN_DRIVER, hence this empty file.
 */
#ifndef _CONNECTIVITY_BUILD_IN_ADAPTER_H
#define _CONNECTIVITY_BUILD_IN_ADAPTER_H

#endif /* _CONNECTIVITY_BUILD_IN_ADAPTER_H */
