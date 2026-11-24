/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > P25 search (radio/p25search.h): find P25 in a range whatever its NAC and talkgroup. Left / Right
 * pick the range (around the frequency in use, or a band), OK starts or stops; the screen shows the sweep, the
 * probe or the call being played, and the found list (frequency, NAC, talkgroup, source, how long ago). Back stops
 * it; leaving the screen otherwise stops it after a few seconds (P25S_IDLE_MS).
 * CONFIG_FIND_SAVE (ui/findsave.h): # saves every find into a new zone "Bulunanlar N"; Up / Down pick a find, OK on it
 * saves that one (into the newest such zone). The list stays as it was while the save runs. */
#include "config.h"
#if CONFIG_P25_SEARCH
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
#include "radio/p25search.h"
#if CONFIG_SEARCH_HYBRID
#include "radio/searchhy.h"
#endif
#include "ui/ui_scratch.h"
#if CONFIG_SEARCH_RANGE
#include "ui/ui_srange.h"
#endif
#if CONFIG_FIND_SAVE
#include "ui/findsave.h"
#endif

#if CONFIG_SEARCH_RANGE
/* The range and step (DDM32 Plus search ranges, ui_srange.h): kept while the radio is on */
static ui_srange_t rng = UI_SRANGE_INIT;
#else
enum { R_500K, R_1M, R_2M5, R_UHF, R_VHF, R_COUNT };
static const char *const range_names[R_COUNT] = { "+-500 kHz", "+-1 MHz", "+-2.5 MHz", "430-440 MHz", "144-146 MHz" };
static uint8_t range_i = R_1M;
#endif
/* The search as last copied: the tool screens' shared buffer (ui_scratch.h), refilled by snap() in each draw and key */
#define sv	(ui_scratch.p25s)
#if CONFIG_FIND_SAVE
static uint32_t sel_hz;		/* the find picked with Up / Down, 0 = none */
static uint32_t sel_key;	/* ... and its NAC and talkgroup (a frequency may have more entries) */
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

static void hexnac(sbuf_t *b, uint16_t nac)
{
	for (int sh = 8; sh >= 0; sh -= 4)
		sb_chr(b, "0123456789ABCDEF"[(nac >> sh) & 15u]);
}

/* Fitting a line to the safe area (DDM32 Plus srchfix): build(b, arg, form) writes the line in a shorter form for each
 * form number (0 = the full one); its text alternates (sb_age: "13m ago" / "13m") count as shorter forms of the same
 * form. The first that is not wider than w is drawn; if even the last is, it is shortened with an ellipsis, never
 * clipped at the edge */
typedef void (*line_fn)(sbuf_t *b, const void *arg, int form);

static void fit_line(const font_t *f, int w, char *out, size_t cap, line_fn build, const void *arg, int forms)
{
	char src[56];
	sbuf_t b;
	for (int form = 0; form < forms; form++) {
		sb_init(&b, src, sizeof src);
		build(&b, arg, form);
		bool more = true;
		for (int lvl = 0; more; lvl++) {
			gfx_text_alt(src, lvl, out, cap, &more);
			if (gfx_text_width(f, out) <= w)
				return;
		}
	}
	gfx_text_fit(f, src, w, out, cap);
}

static const char *skip_sp(const char *s)
{
	while (*s == ' ')
		s++;
	return s;
}

/* "Pass 72  hits 193  floor -84", then one space apart, then "P72 h193 fl-84" (4-digit pass and hits and a 3-digit
 * floor still fit the 220 px of the DM-32's safe area) */
static void pass_line(sbuf_t *b, const void *arg, int form)
{
	const p25s_view_t *v = arg;
	if (form >= 2) {
		sb_str(b, TR("P"));
		sb_u32(b, v->sweeps + 1u, 1);
		sb_str(b, TR(" h"));
		sb_u32(b, v->hits, 1);
		sb_str(b, TR(" fl"));
		sb_i32(b, v->floor_dbm);
		return;
	}
	sb_str(b, TR("Pass "));
	sb_u32(b, v->sweeps + 1u, 1);
	if (form == 0) {
		sb_str(b, TR("  hits "));
	} else {
		sb_chr(b, ' ');
		sb_str(b, skip_sp(TR("  hits ")));
	}
	sb_u32(b, v->hits, 1);
	if (form == 0) {
		sb_str(b, TR("  floor "));
	} else {
		sb_chr(b, ' ');
		sb_str(b, skip_sp(TR("  floor ")));
	}
	sb_i32(b, v->floor_dbm);
}

/* "158.0062 $40C/1036 TG612 13m ago": the safe area holds 24 small glyphs, so fit_line also tries the age as "13m" and
 * then form 1 (no decimal NAC) and form 2 (no "$" either) */
static void found_line(sbuf_t *b, const void *arg, int form)
{
	const p25s_found_t *f = arg;
	sb_mhz4(b, f->hz);
	sb_str(b, form < 2 ? " $" : " ");
	hexnac(b, f->nac);
#if CONFIG_NAC_DEC
	if (form == 0) {
		sb_chr(b, '/');
		sb_u32(b, f->nac, 1);	// decimal too (some factory radios take the NAC so)
	}
#else
	(void)form;
#endif
	if (f->tg) {
		sb_str(b, " TG");
		sb_u32(b, f->tg, 1);
	}
	sb_chr(b, ' ');
	sb_age(b, ui_st.now_ms - f->heard_ms);
}

/* The search's state; while finds are being saved, the list as the save began (still in the buffer unless another
 * screen has taken it since: then the save stops, ui_p25search_find) */
static bool snap(void)
{
#if CONFIG_FIND_SAVE
	if (ui_findsave_busy() && ui_scratch_mine(UI_SCRATCH_P25S))
		return true;
#endif
	ui_scratch_take(UI_SCRATCH_P25S);
	return p25s_snapshot(&sv);
}

#if CONFIG_FIND_SAVE
static void pick_row(int r)
{
	sel_hz = sv.found[r].hz;
	sel_key = (uint32_t)sv.found[r].nac << 16 | sv.found[r].tg;
}

/* The picked find's row in the snapshot (its frequency's first entry when its key has changed since), -1 = none */
static int sel_row(void)
{
	int first = -1;
	for (uint8_t i = 0; sel_hz != 0 && i < sv.found_n; i++) {
		if (sv.found[i].hz != sel_hz)
			continue;
		if (((uint32_t)sv.found[i].nac << 16 | sv.found[i].tg) == sel_key)
			return i;
		if (first < 0)
			first = i;
	}
	return first;
}

bool ui_p25search_find(uint8_t i, findsave_item_t *out)
{
	if (!ui_scratch_mine(UI_SCRATCH_P25S) || i >= sv.found_n)
		return false;
	const p25s_found_t *f = &sv.found[i];
	memset(out, 0, sizeof *out);
	out->kind = FINDSAVE_P25;
	out->hz = f->hz;
	out->nac = f->nac;
	out->tg = f->tg;
	out->cc = FINDSAVE_CC_NONE;
	return true;
}
#else
static int sel_row(void)
{
	return -1;
}
#endif

static void p25s_draw(void)
{
	char buf[40];
	sbuf_t b;
	bool have = snap();
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR("P25 search"), "") + 2;
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
	if (!have || sv.phase == P25S_OFF) {
		sb_str(&b, TR("Stopped: OK starts"));
	} else if (sv.phase == P25S_SWEEP) {
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
		sb_str(&b, TR("P25 "));
		sb_mhz4(&b, sv.at_hz);
		c = ui_col.rx;
	} else {
		sb_str(&b, TR("Probe "));
		sb_mhz4(&b, sv.at_hz);
		c = ui_col.warn;
	}
	gfx_fill(0, y, gfx_width(), m->h + 2, ui_col.bg);	/* the whole line first: no pixel of the last text (Sweep, Probe, P25) stays */
	gfx_text(m, 0, y, buf, c);
	y += m->h + 2;
	gfx_fill(0, y, gfx_width(), s->h + 3, ui_col.bg);	/* ... and the line below it (Pass, the save line) */
#if CONFIG_FIND_SAVE
	if (ui_findsave_line(buf, sizeof buf)) {
		gfx_text(s, 0, y, buf, ui_col.warn);
	} else
#endif
	if (have && sv.phase != P25S_OFF) {
		fit_line(s, gfx_width(), buf, sizeof buf, pass_line, &sv, 3);
		gfx_text(s, 0, y, buf, ui_col.dim);
	}
	y += s->h + 4;
	gfx_hline(0, y, gfx_width(), ui_col.line);
	y += 3;
	if (!have || sv.found_n == 0) {
		gfx_text(s, 0, y, TR("No P25 found yet"), ui_col.dim);
	}
	int sel = have ? sel_row() : -1;
	for (uint8_t i = 0; have && i < sv.found_n && y + s->h < ui_layout.hint_y; i++) {
		const p25s_found_t *f = &sv.found[i];
		bool picked = (int)i == sel;
		int rows = f->src && y + 2 * s->h + 1 < ui_layout.hint_y ? 2 : 1;
		if (picked)
			gfx_fill(0, y - 1, gfx_width(), rows * (s->h + 1) + 1, ui_col.focus);
		fit_line(s, gfx_width(), buf, sizeof buf, found_line, f, 3);
		gfx_text(s, 0, y, buf, picked ? ui_col.focus_fg : i == 0 && sv.playing ? ui_col.rx : ui_col.fg);
		y += s->h + 1;
		if (rows == 2) {
			sb_init(&b, buf, sizeof buf);
			sb_str(&b, "   ID ");
			sb_u32(&b, f->src, 1);
			gfx_text(s, 0, y, buf, picked ? ui_col.focus_fg : ui_col.dim);
			y += s->h + 1;
		}
	}
	const char *hint = have && sv.phase != P25S_OFF ? TR("OK stop  Back") : TR("OK start  <> range");
#if CONFIG_SEARCH_RANGE
	if (!(have && sv.phase != P25S_OFF))
		hint = TR(TEXT_ALT_OPEN "OK start  <> range  * edit" TEXT_ALT_NEXT "OK start  * range" TEXT_ALT_CLOSE);
#endif
#if CONFIG_FIND_SAVE
	if (sel >= 0)
		hint = TR(TEXT_ALT_OPEN "OK save this one  ^v pick" TEXT_ALT_NEXT "OK save  ^v pick" TEXT_ALT_CLOSE);
	else if (have && sv.found_n != 0)
		hint = sv.phase != P25S_OFF ?
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
	radio_req_p25_search(lo, hi);
	ui_dirty();
}

static bool p25s_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	bool have = snap();
	bool on = have && sv.phase != P25S_OFF;
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
#if CONFIG_SEARCH_HYBRID
	/* DDM32 Plus faster searches (radio/searchhy.h): held 0 switches between the fast algorithm and beta.8's (classic),
	 * so the owner can compare them on the same signal; a running search starts again with it */
	if (ev->gesture == GESTURE_LONG && ev->action == ACT_DIGIT_0) {
#if CONFIG_SEARCH_SOFTP25
		/* DDM32 Plus P25 soft probe (radio/p25search.h): here held 0 goes fast -> fast + soft probe -> classic -> fast;
		 * the soft probe is off until chosen (the DMR and all digital screens keep fast <-> classic) */
		if (sh_fast_wanted() && !p25s_soft_wanted()) {
			p25s_set_soft(true);
		} else {
			p25s_set_soft(false);
			sh_set_fast(!sh_fast_wanted());
		}
		ui_toast(!sh_fast_wanted() ? TR("Search: classic (beta.8)") :
			 p25s_soft_wanted() ? TR("Search: fast + soft probe") : TR("Search: fast"));
#else
		sh_set_fast(!sh_fast_wanted());
		ui_toast(sh_fast_wanted() ? TR("Search: fast") : TR("Search: classic (beta.8)"));
#endif
		if (on)
			restart();
		ui_dirty();
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
#if CONFIG_FIND_SAVE
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
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_CONTEXT) {
		ui_findsave_start(FINDSAVE_P25, 0, have ? sv.found_n : 0, false);	/* # saves them all */
		ui_dirty();
		return true;
	}
	if ((ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG) && ev->action == ACT_SELECT && have &&
	    sel_row() >= 0) {
		uint8_t r = (uint8_t)sel_row();
		ui_findsave_start(FINDSAVE_P25, r, (uint8_t)(r + 1u), true);	/* OK on a find saves that one */
		ui_dirty();
		return true;
	}
#endif
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_SELECT) {
		if (on)
			radio_req_p25_search(0, 0);
		else
			restart();
		ui_dirty();
		return true;
	}
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_BACK) {
#if CONFIG_FIND_SAVE
		sel_hz = 0;
#endif
		radio_req_p25_search(0, 0);	/* and the screen's own Back leaves */
	}
	return false;
}

static void p25s_enter(void)
{
#if CONFIG_SEARCH_RANGE
	ui_srange_close();
#endif
#if CONFIG_FIND_SAVE
	sel_hz = 0;
#endif
	ui_dirty();
}

const ui_scr_def_t ui_p25search_screen = {
	.name = "p25search", .flags = SF_FIELD, .key = p25s_key, .draw = p25s_draw, .enter = p25s_enter,
};
#endif
