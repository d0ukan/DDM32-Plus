/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > CW beacon (docs/plus.md, radio/cw_beacon.h): pick the message (Up / Down) and how often
 * (Left / Right), OK starts or stops it. The screen shows the message with the callsign, on air / the next one /
 * why it was refused. Leaving the screen keeps an interval beacon armed; PTT or any radio action stops it. */
#include "config.h"
#if CONFIG_CW_BEACON
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_svc.h"
#include "fmt.h"
#include "i18n.h"
#if CONFIG_AIR_WATCH
/* DDM32 Plus: the UI's air watch (ui_airwatch.inc; weak: a build without the watch screens has none). It can still
 * be set for a moment after its ring stopped (until the UI ends it): the radio core refuses then too */
bool ui_air_watch_on(void) __attribute__((weak));
#define UI_AIR_ON()	(ui_air_watch_on != NULL && ui_air_watch_on())
#else
#define UI_AIR_ON()	false
#endif
#include "gfx.h"
#include "radio/radio.h"
#include "radio/radio_cwid.h"
#include "radio/cw_beacon.h"
#include "radio/morse.h"

static uint8_t kind = CW_BEACON_DE, interval;
static char radio_name[17];	/* read once on entry (the codeplug), not on every draw */
static const char *const kind_names[CW_BEACON_KINDS] = { TRN("DE call K"), TRN("CQ"), TRN("SOS") };
static const char *const interval_names[CW_BEACON_INTERVALS] = { TRN("Once"), "30 s", "60 s", "180 s" };

static void beacon_enter(void)
{
	ui_svc_name_get(UI_NAME_RADIO, radio_name);
	ui_dirty();
}

static int line(int y, const char *label, const char *value, color_t c)
{
	const font_t *f = ui_layout.medium;
	int x = gfx_text(f, 0, y, label, ui_col.dim);
	gfx_text(f, x + f->w, y, value, c);
	return y + f->h + 4;
}

static void beacon_draw(void)
{
	char buf[MORSE_TEXT_MAX + 1], call[RADIO_CWID_CALL_MAX + 1];
	sbuf_t b;
	cw_beacon_view_t st;
	cw_beacon_view(&st);
	const radio_view_t *v = ui_view();
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(v, &ui_st, TR("CW beacon"), "") + 4;
	y = line(y, TR("Message"), tr(kind_names[kind]), ui_col.fg);
	y = line(y, TR("Repeat"), tr(interval_names[interval]), ui_col.fg);
	sb_init(&b, buf, sizeof buf);
	sb_freq(&b, v->ch.tx_hz);
	y = line(y, TR("TX"), buf, ui_col.tx);
	const font_t *f = ui_layout.small;
	if (radio_cwid_callsign(radio_name, call, sizeof call) == 0) {
		gfx_text(f, 0, y, TR("Radio name must be a callsign"), ui_col.warn);
		y += f->h + 2;
		gfx_text(f, 0, y, TR("Settings > My radio"), ui_col.dim);
	} else {
		cw_beacon_text(kind, call, buf, sizeof buf);
		gfx_text(f, 0, y, buf, ui_col.fg);
	}
	y += f->h + 8;
	sb_init(&b, buf, sizeof buf);
	color_t c = ui_col.dim;
	if (st.keying) {
		sb_str(&b, TR("On air"));
		c = ui_col.tx;
	} else if (st.on) {
		sb_str(&b, TR("Next in "));
		int32_t left = (int32_t)(st.next_ms - ui_st.now_ms);
		sb_u32(&b, left > 0 ? (uint32_t)(left + 999) / 1000u : 0u, 1);
		sb_str(&b, " s");
		c = ui_col.warn;
	} else if (st.refused != 0) {
		sb_str(&b, TR("Refused: can't transmit here"));
		c = ui_col.warn;
	} else {
		sb_str(&b, TR("Off"));
	}
	gfx_text(ui_layout.medium, 0, y, buf, c);
	if (st.sent) {
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, TR("Sent "));
		sb_u32(&b, st.sent, 1);
		gfx_text(f, 0, y + ui_layout.medium->h + 2, buf, ui_col.dim);
	}
	ui_draw_bottom(&ui_st, st.on ? TR("OK stop  Back") : TR("OK send  ^v <> pick"));
	ui_draw_banner(&ui_st);
}

static bool beacon_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	cw_beacon_view_t st;
	cw_beacon_view(&st);
	if (press && (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN) && !st.on) {
		kind = (uint8_t)((kind + (ev->action == ACT_NAV_DOWN ? 1u : CW_BEACON_KINDS - 1u)) % CW_BEACON_KINDS);
		ui_dirty();
		return true;
	}
	if (press && (ev->action == ACT_NAV_LEFT || ev->action == ACT_NAV_RIGHT) && !st.on) {
		interval = (uint8_t)((interval + (ev->action == ACT_NAV_RIGHT ? 1u : CW_BEACON_INTERVALS - 1u)) %
				     CW_BEACON_INTERVALS);
		ui_dirty();
		return true;
	}
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_SELECT && !st.on && (ui_view()->scan != 0 || UI_AIR_ON())) {
		ui_toast(TR("Stop the scan or watch first"));	/* the radio core refuses it (it would key the ring's letter) */
		return true;
	}
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_SELECT) {
		radio_req_cw_beacon(st.on ? CW_BEACON_KINDS : kind, interval);
		ui_dirty();
		return true;
	}
	return false;
}

const ui_scr_def_t ui_beacon_screen = {
	.name = "beacon", .flags = SF_FIELD, .key = beacon_key, .draw = beacon_draw, .enter = beacon_enter,
};
#endif
