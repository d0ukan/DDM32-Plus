/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus VFO step key (CONFIG_VFO_STEPKEY, docs/plus.md "VFO step key"): on Home with the VFO selected, a short
 * SK2 (the A/B key) takes the VFO's tuning step to the next value of Settings > Talk > VFO step (Auto, 2.5, 5, 6.25,
 * 8.33, 10, 12.5, 20, 25, 50, 100 kHz, then Auto again) and says it in a toast ("Step 12.5 kHz"). The step is the same
 * setting as the menu row: it goes to the radio core (ui_svc_apply UI_SET_VFO_STEP), which keeps it in the settings
 * (vfo_step_hz); no new settings bit, and the only RAM is the toast's text.
 * Nothing else changes: on a channel, in dual or multi-watch (the A/B key selects the other letter there, and the
 * VFO letters share this one step), while a scan runs, or while a frequency is being typed, SK2 does what it did. Held
 * SK2 (dual watch on / off) is untouched, so the watch can still be started from the VFO. Home's hint row names the key
 * ("SK2 step"); it is not shown when Settings > Keys has taken SK2's press for something else. */
#include "config.h"
#if CONFIG_VFO_STEPKEY
#include "board.h"
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_svc.h"
#include "fmt.h"
#include "font.h"
#include "i18n.h"
#include "input/keyassign.h"

/* The menu row's values (ui_menus.c v_step), 10 Hz units: 0 Auto, 833 the 8.33 kHz air band grid */
static const int16_t vs_set[] = { 0, 250, 500, 625, 833, 1000, 1250, 2000, 2500, 5000, 10000 };
#define VS_N	((int)(sizeof vs_set / sizeof vs_set[0]))

static char vs_msg[24];	/* "Step 12.5 kHz", "Step Auto" (Turkish "Adim Otomatik") */

/* Home's hint for the VFO with the key named, NULL when SK2's press is no longer the A/B key */
const char *ui_vfostep_hint(void)
{
	if (board.side_key_count < 2)
		return NULL;
	int32_t fn = ui_svc_setting_get(UI_SET_KEY_FIRST + 2);	/* the second side key's press: SK2 */
	if (fn != KEYFN_DEFAULT && fn != KEYFN_AB)
		return NULL;
	return TR(TEXT_ALT_OPEN "0-9 freq  * Quick  Up/Dn  SK2 step" TEXT_ALT_NEXT "0-9 freq  * Quick  SK2 step" TEXT_ALT_NEXT
		  "0-9 freq  SK2 step" TEXT_ALT_CLOSE);
}

/* The short A/B press on Home: true when it stepped the VFO */
bool ui_vfostep_key(const input_event_t *ev)
{
	const radio_view_t *v = ui_view();
	if (ev->gesture != GESTURE_PRESS || !v->vfo || ui_st.entry || ui_st.scanning || ui_st.dual.on)
		return false;
	int i = 0;	/* where the step in use is in the list: Auto, or the one that matches; else the next is Auto */
	if (!v->vfo_step_auto) {
		int32_t now10 = v->vfo_step_hz == 8333u ? 833 : (int32_t)(v->vfo_step_hz / 10u);
		for (i = VS_N - 1; i > 0 && vs_set[i] != now10; i--)
			;
		if (i == 0)
			i = VS_N - 1;	/* not in the list (a build's other value): on to Auto */
	}
	i = (i + 1) % VS_N;
	ui_svc_apply(UI_SET_VFO_STEP, vs_set[i]);
	sbuf_t b;
	sb_init(&b, vs_msg, sizeof vs_msg);
	sb_str(&b, TR("Step "));
	if (i == 0) {	/* Auto: the radio core picks the width's step (Home's place row shows it at once) */
		sb_str(&b, TR("Auto"));
		ui_toast(vs_msg);
		return true;
	}
	uint32_t hz = vs_set[i] == 833 ? 8333u : (uint32_t)vs_set[i] * 10u;
	sb_u32(&b, hz / 1000u, 1);
	if (hz % 1000u) {
		sb_chr(&b, '.');
		sb_u32(&b, (hz % 1000u) / 10u, 2);
		if (b.p[-1] == '0') {	/* 12.50 -> 12.5 */
			b.p[-1] = 0;
			b.p--;
		}
	}
	sb_str(&b, " kHz");
	ui_toast(vs_msg);
	return true;
}
#endif
