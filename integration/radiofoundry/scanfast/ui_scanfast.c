/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus faster channel scan, the UI (CONFIG_SCAN_FAST, radio/scanfast.h, docs/plus.md "Faster channel scan"):
 * held 0 on Home while a zone or VFO scan runs switches between the fast scan (the default) and the classic one
 * (v1.0.0's timing), so the owner can compare them on the same zone. Kept in RAM while the radio is on; a running scan
 * takes it at its next stop and its counters (the PC's "scanst" section) start again. A short 0 still skips the
 * channel the scan stopped on (ui_scanx.c); dual watch and multi-watch have their own keys. */
#include "config.h"
#if CONFIG_SCAN_FAST
#include "ui_nav.h"
#include "ui_screens.h"
#include "i18n.h"
#include "radio/scanfast.h"

bool ui_fscan_home_key(const input_event_t *ev)
{
	if (!ui_st.scanning || ui_st.entry || ev->action != ACT_DIGIT_0 || ev->gesture != GESTURE_LONG)
		return false;
	fs_set_fast(!fs_fast_wanted());
	ui_toast(fs_fast_wanted() ? TR("Scan: fast") : TR("Scan: classic (v1.0.0)"));
	return true;
}
#endif
