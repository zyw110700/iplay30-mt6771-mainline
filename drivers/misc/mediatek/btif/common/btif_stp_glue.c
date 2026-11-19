// SPDX-License-Identifier: GPL-2.0-only
/*
 * Small compatibility layer between the vendor WMT/STP stack (from the
 * MT6771 Android 4.14 tree) and the slightly older BTIF driver we ported
 * from the Nokia MT6771 8.1 kernel.
 *
 * The 4.14 STP glue calls three helpers that the 8.1 BTIF driver does not
 * provide.  They are only used for diagnostics / thread naming there:
 *
 *   mtk_btif_exp_rx_has_pending_data() - logged in the STP timeout handler
 *   mtk_btif_exp_tx_has_pending_data() - same
 *   mtk_btif_exp_rx_thread_get()       - only used to copy ->comm for a log
 *
 * RX data is delivered straight from the BTIF IRQ through the registered
 * callback, so there is never data "pending" behind a reader thread.
 */
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/export.h>
#include <linux/types.h>

#include "mtk_btif_exp.h"

int mtk_btif_exp_rx_has_pending_data(unsigned int id)
{
	return 0;
}
EXPORT_SYMBOL(mtk_btif_exp_rx_has_pending_data);

int mtk_btif_exp_tx_has_pending_data(unsigned int id)
{
	return 0;
}
EXPORT_SYMBOL(mtk_btif_exp_tx_has_pending_data);

struct task_struct *mtk_btif_exp_rx_thread_get(unsigned int id)
{
	/* The caller only reads ->comm to build a thread name. */
	return current;
}
EXPORT_SYMBOL(mtk_btif_exp_rx_thread_get);
