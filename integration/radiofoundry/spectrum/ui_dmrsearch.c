/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > DMR search (radio/dmrsearch.h): find DMR in a range whatever its color code, slot and talkgroup.
 * Left / Right pick the range (around the frequency in use, or a band), OK starts or stops; the screen shows the
 * sweep, the probe or the call being played, and the found list (frequency, color code, slots, repeater or direct,
 * how long ago; the last call's talkgroup and source below). Up / Down pick a find; OK on it puts the VFO there in
 * DMR with its color code and slot. Back stops the search; leaving the screen otherwise stops it after a few
 * seconds (DMRS_IDLE_MS).
 * CONFIG_FIND_SAVE (ui/findsave.h): # saves every find into a new zone "Bulunanlar N"; OK held on a picked find saves
 * that one (into the newest such zone). The list stays as it was while the save runs.
 * CONFIG_ALL_SEARCH (DDM32 Plus): the same screen is Tools > All digital search (ui_allsearch_screen, the flag `all`):
 * the same ranges and keys; the probe and the call show their mode; a find shows its mode and that mode's fields (P25
 * NAC, talkgroup and source; DMR as here; YSF DG-ID and source callsign; NXDN RAN, group and source). OK on a find puts
 * the VFO there in that mode (every mode in the VFO: NAC / DG-ID / RAN Any). No find save there (P25 / DMR only). */
#include "config.h"
#if CONFIG_DMR_SEARCH
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
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
#include "radio/dmrsearch.h"
#include "ui/ui_scratch.h"
#if CONFIG_SEARCH_RANGE
#include "ui/ui_srange.h"
#endif
#if CONFIG_FIND_SAVE
#include "ui/findsave.h"
#endif
#if CONFIG_ALL_SEARCH && CONFIG_VFO_MODES
#include "radio/vfox.h"
#endif

#if CONFIG_SEARCH_RANGE
/* The range and step (DDM32 Plus search ranges, ui_srange.h): kept while the radio is on */
static ui_srange_t rng = UI_SRANGE_INIT;
#else
enum { R_500K, R_1M, R_2M5, R_UHF, R_VHF, R_COUNT };
static const char *const range_names[R_COUNT] = { "+-500 kHz", "+-1 MHz", "+-2.5 MHz", "430-440 MHz", "144-146 MHz" };
static uint8_t range_i = R_1M;
#endif
static uint32_t sel_hz;		/* the find picked with Up / Down, 0 = none */
static uint32_t sel_code, sel_dst;	/* ... and its mode, code and group (a frequency may have more entries) */
#if CONFIG_ALL_SEARCH
static bool all;		/* the screen is Tools > All digital search (its enter sets it) */
#else
#define all false
#endif
/* The search as last copied: the tool screens' shared buffer (ui_scratch.h), refilled by snap() in each draw and key */
#define sv	(ui_scratch.dmrs)

/* The search's state; while finds are being saved, the list as the save began (still in the buffer unless another
 * screen has taken it since: then the save stops, ui_dmrsearch_find) */
static bool snap(void)
{
#if CONFIG_ALL_SEARCH
	if (all) {
		ui_scratch_take(UI_SCRATCH_ALLS);
		return dmrs_snapshot_all(&sv);
	}
#endif
#if CONFIG_FIND_SAVE
	if (ui_findsave_busy() && ui_scratch_mine(UI_SCRATCH_DMRS))
		return true;
#endif
	ui_scratch_take(UI_SCRATCH_DMRS);
	return dmrs_snapshot(&sv);
}

#if CONFIG_FIND_SAVE
bool ui_dmrsearch_find(uint8_t i, findsave_item_t *out)
{
	if (!ui_scratch_mine(UI_SCRATCH_DMRS) || i >= sv.found_n)
		return false;
	const dmrs_found_t *f = &sv.found[i];
	memset(out, 0, sizeof *out);
	out->kind = FINDSAVE_DMR;
	out->hz = f->hz;
	out->cc = f->cc != DMRS_CC_NONE && f->cc <= 15u ? (uint8_t)f->cc : FINDSAVE_CC_NONE;
	out->ts_mask = f->ts_mask;
	return true;
}
#endif

#if !CONFIG_SEARCH_RANGE
static void range_of(uint8_t r, uint32_t center, uint32_t *lo, uint32_t *hi)
{
	static const uint32_t half[3] = { 500000u, 1000000u, 2500000u };
	if (r <= R_2M5) {
		center -= center % 12500u;
		*lo = center - half[r];
		*hi = center + half[r];
	} else if (r == R_UHF) {
		*lo = 430000000u;
		*hi = 440000000u;
	} else {
		*lo = 144000000u;
		*hi = 146000000u;
	}
}
#endif

static void sb_mhz4(sbuf_t *b, uint32_t hz)
{
	sb_u32(b, hz / 1000000u, 1);
	sb_chr(b, '.');
	sb_u32(b, (hz / 100u) % 10000u, 4);
}

static void pick_row(int r)
{
	sel_hz = sv.found[r].hz;
	sel_code = (uint32_t)sv.found[r].mode << 16 | sv.found[r].cc;
	sel_dst = sv.found[r].dst;
}

/* The picked find's row in the snapshot (its frequency's first entry when its key has changed since), -1 = none */
static int sel_row(void)
{
	int first = -1;
	for (uint8_t i = 0; sel_hz != 0 && i < sv.found_n; i++) {
		if (sv.found[i].hz != sel_hz)
			continue;
		if (((uint32_t)sv.found[i].mode << 16 | sv.found[i].cc) == sel_code && sv.found[i].dst == sel_dst)
			return i;
		if (first < 0)
			first = i;
	}
	return first;
}

/* The search's request: this screen's kind */
static void req_search(uint32_t lo, uint32_t hi)
{
#if CONFIG_ALL_SEARCH
	if (all) {
		radio_req_all_search(lo, hi);
		return;
	}
#endif
	radio_req_dmr_search(lo, hi);
}

#if CONFIG_ALL_SEARCH
static void sb_hex3(sbuf_t *b, uint16_t v)
{
	for (int d = 2; d >= 0; d--)
		sb_chr(b, "0123456789ABCDEF"[(v >> (4 * d)) & 15u]);
}

/* The all digital search's first line after the frequency: "P25 NAC $1C8/456", "YSF DG 0", "NXDN RAN 1" (DMR: as the DMR
 * search's) */
static bool all_line(sbuf_t *b, const dmrs_found_t *f)
{
	if (!all || f->mode == DMRS_MODE_DMR) {
		if (all)
			sb_str(b, " DMR");
		return false;
	}
	sb_chr(b, ' ');
	sb_str(b, dmrs_mode_name(f->mode));
	if (f->cc != DMRS_CC_NONE) {
		sb_str(b, f->mode == DMRS_MODE_P25 ? " NAC " : f->mode == DMRS_MODE_YSF ? " DG " : " RAN ");
		if (f->mode == DMRS_MODE_P25) {
#if CONFIG_NAC_DEC
			sb_chr(b, '$');	// DM32 Plus: both forms, "$1C8/456"
#endif
			sb_hex3(b, f->cc);
#if CONFIG_NAC_DEC
			sb_chr(b, '/');
			sb_u32(b, f->cc, 1);
#endif
		} else
			sb_u32(b, f->cc, 1);
	}
	return true;
}
#endif

/* "433.5000 CC1 TS1+2 RPT 3m" (the all digital search: "433.5000 P25 NAC $1C8/456 3m", "438.7125 DMR CC1 TS2 RPT 3m") */
static void found_line(sbuf_t *b, const dmrs_found_t *f)
{
	sb_mhz4(b, f->hz);
#if CONFIG_ALL_SEARCH
	if (!all_line(b, f))
#endif
	{
		if (f->cc != DMRS_CC_NONE) {
			sb_str(b, " CC");
			sb_u32(b, f->cc, 1);
		}
		if (f->ts_mask & 3u) {
			sb_str(b, " TS");
			sb_str(b, (f->ts_mask & 3u) == 3u ? "1+2" : (f->ts_mask & 1u) ? "1" : "2");
		}
		if (f->sync_mask & DMRS_SYNC_BS)
			sb_str(b, " RPT");
		if (f->sync_mask & DMRS_SYNC_MS)
			sb_str(b, " DIR");
	}
	sb_chr(b, ' ');
	sb_age(b, ui_st.now_ms - f->heard_ms);
}

/* The second line: "   TG 2620 from 2620123", "   from TA2DOU", "   Encrypted" */
static void more_line(sbuf_t *b, const dmrs_found_t *f)
{
	sb_str(b, "   ");
#if CONFIG_ALL_SEARCH
	if (f->mode == DMRS_MODE_YSF) {
		char call[11];
		dmrs_call_unpack(f->dst, f->src, call);
		if (call[0] != '\0') {
			sb_str(b, TR("from "));
			sb_str(b, call);
		}
		return;
	}
#endif
	if (f->dst != 0) {
		sb_str(b, f->group ? "TG " : "ID ");
		sb_u32(b, f->dst, 1);
		sb_chr(b, ' ');
	}
	if (f->src != 0) {
		sb_str(b, TR("from "));
		sb_u32(b, f->src, 1);
		sb_chr(b, ' ');
	}
	if (f->enc)
		sb_str(b, TR("Encrypted"));
}

/* "DMR " or, on the all digital search's screen, the mode probed or played and a space */
static void sb_mode(sbuf_t *b)
{
#if CONFIG_ALL_SEARCH
	if (all) {
		sb_str(b, dmrs_mode_name(sv.mode));
		sb_chr(b, ' ');
		return;
	}
#endif
	sb_str(b, "DMR ");
}

static void dmrs_draw(void)
{
	char buf[40];
	sbuf_t b;
	bool have = snap();
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, all ? TR("All digital search") : TR("DMR search"), "") + 2;
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
#if CONFIG_SEARCH_RANGE
	ui_srange_line(&rng, s, gfx_width(), buf, sizeof buf);
	gfx_text(s, 0, y, buf, ui_col.dim);
	y += s->h + 3;
	if (ui_srange_editing()) {	/* the range editor has the page */
		ui_srange_draw(y);
		ui_draw_bottom(&ui_st, ui_srange_hint());
		ui_draw_banner(&ui_st);
		return;
	}
#else
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, TR("Range "));
	sb_str(&b, range_names[range_i]);
	gfx_text(s, 0, y, buf, ui_col.dim);
	y += s->h + 3;
#endif
	sb_init(&b, buf, sizeof buf);
	color_t c = ui_col.dim;
	if (!have || sv.phase == DMRS_OFF) {
		sb_str(&b, TR("Stopped: OK starts"));
	} else if (sv.phase == DMRS_SWEEP) {
		sb_str(&b, TR("Sweep "));
		sb_mhz4(&b, sv.at_hz);
#if CONFIG_SEARCH_SURE
		if (sv.step_ms != 0) {
			/* DDM32 Plus: the steps a second (searchfix), when the line has room for them */
			size_t n = strlen(buf);
			sb_str(&b, "  ");
			sb_u32(&b, (1000u + sv.step_ms / 2u) / sv.step_ms, 1);
			sb_str(&b, "/s");
			if (gfx_text_width(m, buf) > gfx_width())
				buf[n] = '\0';
		}
#endif
		c = ui_col.fg;
	} else if (sv.playing) {
		sb_mode(&b);
		sb_mhz4(&b, sv.at_hz);
		c = ui_col.rx;
	} else {
		sb_str(&b, TR("Probe "));
		if (all)
			sb_mode(&b);
		sb_mhz4(&b, sv.at_hz);
		c = ui_col.warn;
	}
	gfx_text(m, 0, y, buf, c);
	y += m->h + 2;
#if CONFIG_FIND_SAVE
	if (!all && ui_findsave_line(buf, sizeof buf)) {
		gfx_text(s, 0, y, buf, ui_col.warn);
	} else
#endif
	if (have && sv.phase != DMRS_OFF) {
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, TR("Pass "));
		sb_u32(&b, sv.sweeps + 1u, 1);
		sb_str(&b, TR("  hits "));
		sb_u32(&b, sv.hits, 1);
		sb_str(&b, TR("  floor "));
		sb_i32(&b, sv.floor_dbm);
		gfx_text(s, 0, y, buf, ui_col.dim);
	}
	y += s->h + 4;
	gfx_hline(0, y, gfx_width(), ui_col.line);
	y += 3;
	if (!have || sv.found_n == 0)
		gfx_text(s, 0, y, all ? TR("No digital found yet") : TR("No DMR found yet"), ui_col.dim);
	int sel = have ? sel_row() : -1;
	for (uint8_t i = 0; have && i < sv.found_n && y + s->h < ui_layout.hint_y; i++) {
		const dmrs_found_t *f = &sv.found[i];
		bool picked = (int)i == sel;
		bool more = f->dst != 0 || f->src != 0 || f->enc;
		int rows = more && y + 2 * (s->h + 1) <= ui_layout.hint_y ? 2 : 1;
		if (picked)
			gfx_fill(0, y - 1, gfx_width(), rows * (s->h + 1) + 1, ui_col.focus);
		sb_init(&b, buf, sizeof buf);
		found_line(&b, f);
		gfx_text(s, 0, y, buf, picked ? ui_col.focus_fg : i == 0 && sv.playing ? ui_col.rx : ui_col.fg);
		y += s->h + 1;
		if (rows == 2) {
			sb_init(&b, buf, sizeof buf);
			more_line(&b, f);
			gfx_text(s, 0, y, buf, picked ? ui_col.focus_fg : f->enc ? ui_col.warn : ui_col.dim);
			y += s->h + 1;
		}
	}
	const char *hint = sel >= 0 ? TR("OK tune  ^v pick") :
			   have && sv.phase != DMRS_OFF ? TR("OK stop  ^v pick") : TR("OK start  <> range");
#if CONFIG_SEARCH_RANGE
	if (sel < 0 && !(have && sv.phase != DMRS_OFF))
		hint = TR(TEXT_ALT_OPEN "OK start  <> range  * edit" TEXT_ALT_NEXT "OK start  * range" TEXT_ALT_CLOSE);
#endif
#if CONFIG_FIND_SAVE
	if (all) {
		/* the all digital search saves nothing: the hints above */
	} else if (sel >= 0)
		hint = TR(TEXT_ALT_OPEN "OK tune  hold OK save  ^v pick" TEXT_ALT_NEXT "OK tune  hold OK save"
			  TEXT_ALT_CLOSE);
	else if (have && sv.found_n != 0)
		hint = sv.phase != DMRS_OFF ?
		       TR(TEXT_ALT_OPEN "OK stop  ^v pick  # save all" TEXT_ALT_NEXT "OK stop  # save all" TEXT_ALT_CLOSE) :
		       TR(TEXT_ALT_OPEN "OK start  ^v pick  # save all" TEXT_ALT_NEXT "OK start  # save all" TEXT_ALT_CLOSE);
#endif
	ui_draw_bottom(&ui_st, hint);
	ui_draw_banner(&ui_st);
}

static void restart(void)
{
	uint32_t lo, hi;
	if (ui_view()->scan == 3 /* SCAN_DUAL: dual / multi-watch, the air watch */ || UI_AIR_ON()) {
		ui_toast(TR("Stop the scan or watch first"));	/* the radio core refuses it over a watch (hole hunt B3) */
		return;
	}
#if CONFIG_SEARCH_RANGE
	lo = ui_srange_request(&rng, ui_view()->ch.rx_hz, &hi);	/* the step rides on lo (srange.h) */
#else
	range_of(range_i, ui_view()->ch.rx_hz, &lo, &hi);
#endif
	req_search(lo, hi);
	ui_dirty();
}

#if CONFIG_ALL_SEARCH
/* OK on a P25 / YSF / NXDN find: the VFO there in that mode, listening to every NAC / DG-ID / RAN (the VFO's defaults
 * for the mode, radio/vfox.h); false when this build's VFO has no such mode */
static bool tune_other(const dmrs_found_t *f)
{
#if CONFIG_VFO_MODES
	static const uint8_t mode_of[DMRS_MODES] = { RADIO_MODE_DMR, RADIO_MODE_P25, RADIO_MODE_YSF, RADIO_MODE_NXDN };
	channel_t c = ui_view()->ch;
	c.rx_hz = c.tx_hz = f->hz;
	c.bw = RADIO_BW_12K5;
	vfox_set_mode(&c, mode_of[f->mode], 0);
	c.p25_tg = 0;		/* YSF DG-ID / NXDN RAN: any; P25: every talkgroup */
	c.p25_squelch = 0;
	radio_req_vfo_set(f->hz);
	radio_req_vfo_apply(&c);
	return true;
#else
	(void)f;
	return false;
#endif
}
#endif

/* OK on a find: the search stops and the VFO goes there in DMR, with the color code and the slot when known (the all
 * digital search: in the find's mode) */
static void tune_find(const dmrs_found_t *f)
{
	req_search(0, 0);
	sel_hz = 0;
#if CONFIG_ALL_SEARCH
	if (f->mode != DMRS_MODE_DMR) {
		if (!tune_other(f))
			ui_toast(TR("VFO: not in this build"));
		ui_go_home();
		return;
	}
#endif
	radio_req_vfo_set(f->hz);
	radio_req_mode(RADIO_MODE_DMR);
	if (f->cc != DMRS_CC_NONE)
		radio_req_dmr_color_code((uint8_t)f->cc);
	if ((f->ts_mask & 3u) == 1u || (f->ts_mask & 3u) == 2u)
		radio_req_dmr_timeslot((f->ts_mask & 1u) ? 1u : 2u);
	ui_go_home();
	/* A repeater's output: the VFO is simplex there, so PTT would talk on the output; say so */
	if (f->sync_mask & DMRS_SYNC_BS)
		ui_toast(TR("Repeater: set the TX offset"));
}

static bool dmrs_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	bool have = snap();
	bool on = have && sv.phase != DMRS_OFF;
#if CONFIG_SEARCH_RANGE
	/* The range editor: it takes every key while open; an applied range restarts a running search on it */
	int ek = ui_srange_key(ev);
	if (ek != UI_SRANGE_NONE) {
		if (ek == UI_SRANGE_APPLIED && on)
			restart();
		return true;
	}
	/* * opens it; a digit opens it typing Start */
	if (ev->gesture == GESTURE_PRESS && (ev->action == ACT_QUICK ||
					      (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9))) {
		ui_srange_open(&rng, ui_view()->ch.rx_hz,
			       ev->action == ACT_QUICK ? 0 : (char)('0' + (ev->action - ACT_DIGIT_0)));
		return true;
	}
#endif
	if (press && (ev->action == ACT_NAV_LEFT || ev->action == ACT_NAV_RIGHT)) {
#if CONFIG_SEARCH_RANGE
		ui_srange_turn(&rng, ev->action == ACT_NAV_RIGHT ? 1 : -1);
#else
		range_i = (uint8_t)((range_i + (ev->action == ACT_NAV_RIGHT ? 1u : R_COUNT - 1u)) % R_COUNT);
#endif
		if (on)
			restart();
		ui_dirty();
		return true;
	}
	if (press && (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN)) {
		/* Down from nothing picks the newest; Up past the newest picks nothing again */
		int r = have ? sel_row() : -1;
		r += ev->action == ACT_NAV_DOWN ? 1 : -1;
		if (!have || r < 0 || sv.found_n == 0)
			sel_hz = 0;
		else
			pick_row(r < sv.found_n ? r : sv.found_n - 1);
		ui_dirty();
		return true;
	}
#if CONFIG_FIND_SAVE
	if (!all && ev->gesture == GESTURE_PRESS && ev->action == ACT_CONTEXT) {
		ui_findsave_start(FINDSAVE_DMR, 0, have ? sv.found_n : 0, false);	/* # saves them all */
		ui_dirty();
		return true;
	}
	if (!all && ev->gesture == GESTURE_LONG && ev->action == ACT_SELECT) {
		int r = have ? sel_row() : -1;
		if (r >= 0)	/* OK held on a find saves that one (OK alone tunes it) */
			ui_findsave_start(FINDSAVE_DMR, (uint8_t)r, (uint8_t)(r + 1), true);
		ui_dirty();
		return true;
	}
#endif
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_SELECT) {
		int r = have ? sel_row() : -1;
		if (r >= 0)
			tune_find(&sv.found[r]);
		else if (on)
			req_search(0, 0);
		else
			restart();
		ui_dirty();
		return true;
	}
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_BACK) {
		sel_hz = 0;
		req_search(0, 0);	/* and the screen's own Back leaves */
	}
	return false;
}

static void dmrs_enter(void)
{
#if CONFIG_SEARCH_RANGE
	ui_srange_close();
#endif
#if CONFIG_ALL_SEARCH
	all = false;
#endif
	sel_hz = 0;
	ui_dirty();
}

const ui_scr_def_t ui_dmrsearch_screen = {
	.name = "dmrsearch", .flags = SF_FIELD, .key = dmrs_key, .draw = dmrs_draw, .enter = dmrs_enter,
};

#if CONFIG_ALL_SEARCH
static void alls_enter(void)
{
	dmrs_enter();
	all = true;
}

const ui_scr_def_t ui_allsearch_screen = {
	.name = "allsearch", .flags = SF_FIELD, .key = dmrs_key, .draw = dmrs_draw, .enter = alls_enter,
};
#endif
#endif
