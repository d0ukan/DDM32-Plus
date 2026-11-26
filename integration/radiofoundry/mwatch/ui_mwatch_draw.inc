// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus multi-watch (CONFIG_MULTI_WATCH, docs/plus.md "Multi-watch"): Home with 3 to 6 watched channels, a
// compact row per letter instead of dual watch's two panes (2 channels keep those). Included by core/ui/ui_screens.c
// before ui_draw_home_dual (tools/plus_features.py prepare_multi_watch); the rows come from ui_dual.c
// (ui_dual_ring_row). Each row:
//   [A▶] 12 COUNTY FM ............ PTT      the letter (filled on the selected row, with the arrow; red while it
//        FM 145.500        Heard 7s ago     transmits, green while a call comes in), number and name, PTT pill;
//                                           mode and frequency, or the caller (green) while receiving; at the
//                                           right Busy (a look in the wait found a signal) or how long ago it heard a call
// Under the rows: TALKING / Receiving on X with the meter, else the ring's period ("Ring 0.9 s", "+P25 3.5 s").
// Large text: one line per letter, as large as fits. Everything is drawn from static buffers: Home is on the UI
// task's deepest path (stack-check.md)

#if CONFIG_AM_RX
#include "radio/radio_am.h"	// DDM32 Plus: a row in the air band says AM
#endif

static char mw_buf[48], mw_fit[48];	// the rows' and the state line's text
static uint32_t mw_newest;		// the newest call's end (its row's age in amber), 0: none
static bool mw_any_rx;			// a call comes in on some row: the others step back (dim)

static const char *mw_mode_word(uint8_t m)
{
	return codeplug_mode_dpmr(m) ? "dPMR" : codeplug_mode_nxdn(m) ? "NXDN" : codeplug_mode_ysf(m) ? "YSF" :
	       codeplug_mode_p25(m) ? "P25" : m == RADIO_MODE_DMR ? "DMR" : "FM";
}

// "0.9 s"
static void mw_sb_secs(sbuf_t *b, uint32_t ms)
{
	uint32_t tenths = (ms + 50u) / 100u;
	sb_u32(b, tenths / 10u, 1);
	sb_chr(b, '.');
	sb_u32(b, tenths % 10u, 1);
	sb_str(b, " s");
}

static void __attribute__((noinline)) mw_draw_row(const radio_view_t *v, const ui_state_t *s, const ui_mw_row_t *r,
						   int y, int h, const font_t *nf, bool two)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *sm = L->small;
	char *buf = mw_buf, *fit = mw_fit;
	sbuf_t b;
	int w = gfx_width();
	bool tx = r->sel && v->state == RS_TX, rx = !tx && r->rx;

	// The tag: the letter, and on the selected row the arrow after it
	int aw = r->sel ? (nf->h / 3 < 3 ? 3 : nf->h / 3) : 0;
	int tw = nf->w + 6 + (aw ? aw + 2 : 0);
	int th = h - 2;
	color_t tbg = ui_col.bg, tfg = ui_col.dim;
	if (tx) {
		tbg = ui_col.tx;
		tfg = ui_col.pill_fg;
	} else if (rx) {
		tbg = ui_col.rx;
		tfg = ui_col.pill_dark_fg;
	} else if (r->sel) {
		tbg = ui_col.fg;
		tfg = ui_col.bg;
	}
	if (tbg != ui_col.bg)
		gfx_round_fill(0, y + 1, tw, th, tbg);
	else
		gfx_rect(0, y + 1, tw, th, r->busy ? ui_col.rx : ui_col.line);
	char letter[2] = { r->letter, 0 };
	int ly = y + 1 + (th - nf->h + 1) / 2;
	gfx_text(nf, 3, ly, letter, tfg);
	if (aw) {
		int ah = (aw * 2 - 1) | 1;
		dual_arrow(3 + nf->w + 2, ly + (nf->h - ah) / 2, ah, aw, tfg);
	}

	// Line 1: number and name (the VFO: its frequency); at the right the PTT pill on the selected row, RX on the
	// receiving one when it isn't selected
	color_t fg = tx ? ui_col.tx : rx ? ui_col.rx : mw_any_rx ? ui_col.dim : ui_col.fg;
	color_t agec = r->heard_ms == mw_newest ? ui_col.warn : ui_col.dim;
	int x0 = tw + sm->w / 2, x_end = w;
	int l1 = two ? nf->h : h;
	int y1 = y + (two ? (h - nf->h - sm->h) / 2 : 0);
	const char *word = tx ? "TX" : r->sel ? "PTT" : rx ? "RX" : NULL;
	if (word != NULL) {
		x_end -= dual_pill_w(word);
		gfx_pill(sm, x_end, y1 + (l1 - sm->h + 1) / 2, sm->h, word, tx ? ui_col.pill_fg : rx ? ui_col.pill_dark_fg : ui_col.bg,
			 tx ? ui_col.tx : rx ? ui_col.rx : ui_col.fg);
		x_end -= sm->w / 2;
	}
	// DDM32 Plus (docs/plus.md "Multi-watch: the heard talk group"): one line, a call coming in: the heard call's talk
	// group ("TG 101", "Pvt 2345") left of the pill, in the green of the call; the name gives way to it
	if (!two && rx && r->heard[0] != '\0')
		x_end = dual_right(sm, x_end, y1, l1, x0 + 3 * sm->w, r->heard, ui_col.rx) - sm->w / 2;
	// One line: the right side says how long ago (or Busy) where the pill isn't
	if (!two && word == NULL && (r->busy || r->heard_ms)) {
		sb_init(&b, buf, sizeof mw_buf);
		if (r->busy)
			sb_str(&b, TR("Busy"));
		else
			sb_age(&b, s->now_ms - r->heard_ms);
		x_end = dual_right(sm, x_end, y1, l1, x0 + 6 * sm->w, buf, r->busy ? ui_col.rx : agec) - sm->w / 2;
	}
	int base = y1 + (l1 - nf->h + 1) / 2;
	int x = x0;
	if (r->vfo) {
		sb_init(&b, buf, sizeof mw_buf);
		// DDM32 Plus auto scan: the letter's label ("TA2XYZ 12km", "LTBA TWR"); line 2 has the frequency, one line
		// both
		if (r->name != NULL && r->name[0] != '\0') {
			sb_str(&b, r->name);
			if (!two) {
				sb_chr(&b, ' ');
				sb_freq(&b, r->rx_hz);
			}
		} else {
			sb_freq(&b, r->rx_hz);
		}
		gfx_text_fit(nf, buf, x_end - x, fit, sizeof mw_fit);
		gfx_text(nf, x, base, fit, fg);
	} else {
		const font_t *mf = dual_num_font(nf);
		sb_init(&b, buf, sizeof mw_buf);
		sb_u32(&b, r->number, 1);
		x = gfx_text(mf, x, base + nf->ascent - mf->ascent, buf, rx || tx ? fg : ui_col.chnum) + mf->w / 2;
		gfx_text_fit(nf, r->name, x_end - x, fit, sizeof mw_fit);
		gfx_text(nf, x, base, fit, fg);
	}
	if (!two)
		return;

	// Line 2: the caller while a call comes in, else the mode and frequency; Busy or the age at the right
	int y2 = y1 + nf->h;
	x_end = w;
	if (r->busy || (r->heard_ms && !rx)) {
		sb_init(&b, buf, sizeof mw_buf);
		if (r->busy) {
			sb_str(&b, TR("Busy"));
		} else {
			sb_str(&b, TEXT_ALT_OPEN);
			sb_str(&b, TR("Heard "));
			sb_age_long(&b, s->now_ms - r->heard_ms);
			sb_str(&b, TEXT_ALT_NEXT);
			sb_age_long(&b, s->now_ms - r->heard_ms);
			sb_str(&b, TEXT_ALT_CLOSE);
		}
		x_end = dual_right(sm, w, y2, sm->h, x0 + 8 * sm->w, buf, r->busy ? ui_col.rx : agec) - sm->w / 2;
	}
	if (rx && r->heard[0] != '\0' && (r->caller == NULL || r->caller[0] == '\0')) {
		gfx_text_fit(sm, r->heard, x_end - x0, fit, sizeof mw_fit);
		gfx_text(sm, x0, y2, fit, ui_col.rx);
		return;
	}
	if (rx && r->caller != NULL && r->caller[0] != '\0') {
		gfx_text_fit(sm, r->caller, x_end - x0, fit, sizeof mw_fit);
		gfx_text(sm, x0, y2, fit, ui_col.rx);
		return;
	}
	// The mode and frequency, then (DDM32 Plus) the mode's own settings, the first form that fits: mode, frequency and
	// the wide details ("P25 145.5000 NAC Any>$293/659 TG 1"), the tight ones ("...NACAny>$293/659 TG1"); then the
	// frequency and the tight details; the mode and the tight details; the tight details alone (they name the mode:
	// NAC, CC TS, DG, RAN, a tone); last the mode and frequency alone, as before
#if UI_DETAIL
	for (uint8_t form = 0; form <= 5u; form++) {
#endif
	sb_init(&b, buf, sizeof mw_buf);
#if CONFIG_AM_RX
	// DDM32 Plus: an FM channel or VFO in the air band is received in AM (radio_am_channel): say so (the air watch)
	bool am = r->mode == RADIO_MODE_FM && radio_am_band(r->rx_hz) && radio_am_capable();
#endif
#if UI_DETAIL
	if (form != 2u && form != 4u) {
#endif
#if CONFIG_AM_RX
	if (am)
		sb_str(&b, "AM");
	else
#endif
	sb_str(&b, mw_mode_word(r->mode));
#if UI_DETAIL
	}
	if (form < 3u || form == 5u) {
		if (form != 2u)
			sb_chr(&b, ' ');
#else
	sb_chr(&b, ' ');
#endif
	sb_freq(&b, r->rx_hz);
#if UI_DETAIL
	}
#if CONFIG_AM_RX
	if (am)
		break;	// no tones in AM: the mode and frequency (form 0's start)
#endif
	if (form == 5u)
		break;
	if (form != 4u)
		sb_chr(&b, ' ');
	ui_sb_detail(&b, &r->detail, form == 0u ? UI_DETAIL_WIDE : UI_DETAIL_TIGHT);
	if (gfx_text_width(sm, buf) <= x_end - x0)
		break;
	}
#endif
	gfx_text_fit(sm, buf, x_end - x0, fit, sizeof mw_fit);
	gfx_text(sm, x0, y2, fit, rx ? ui_col.rx : ui_col.dim);
}

// The line under the rows: what happens now, else the ring's period
static void __attribute__((noinline)) mw_draw_state(const radio_view_t *v, const ui_state_t *s, int y, uint8_t n,
						     char sel, char rx)
{
	const font_t *sm = ui_layout.small;
	char *buf = mw_buf;
	sbuf_t b;
	int w = gfx_width();
	(void)s;
	sb_init(&b, buf, sizeof mw_buf);
	if (v->state == RS_TX) {
		sb_str(&b, sending_data(v) ? TR("SENDING on ") : TR("TALKING on "));
		sb_chr(&b, sel);
		gfx_text(sm, 0, y, buf, ui_col.tx);
		sb_init(&b, buf, sizeof mw_buf);
		sb_mmss(&b, v->tx_elapsed_ms / 1000u);
		gfx_text(sm, w - gfx_text_width(sm, buf), y, buf, ui_col.fg);
		return;
	}
	if (rx != '-') {
		sb_str(&b, TR("Receiving on "));
		sb_chr(&b, rx);
		int x = gfx_text(sm, 0, y, buf, ui_col.rx) + sm->w;
		if (w - x >= 4 * sm->w)
			draw_meter(x, y + sm->h / 4, w - x, sm->h / 2, v->rssi_dbm);
		return;
	}
	uint32_t p25 = 0, ring = ui_dual_ring_ms(&p25);
	sb_str(&b, TEXT_ALT_OPEN);
	sb_str(&b, TR("Ring "));
	mw_sb_secs(&b, ring);
	if (p25) {
		sb_str(&b, ", P25 +");
		mw_sb_secs(&b, p25);
	}
	sb_str(&b, TEXT_ALT_NEXT);
	sb_str(&b, TR("Ring "));
	mw_sb_secs(&b, ring);
	sb_str(&b, TEXT_ALT_CLOSE);
	gfx_text_fit(sm, buf, w, mw_fit, sizeof mw_fit);
	gfx_text(sm, 0, y, mw_fit, ui_col.dim);
	(void)n;
}

static const char *mw_hint(const radio_view_t *v, const ui_state_t *s)
{
	if (v->state == RS_TX_DENIED)
		return deny_action(v);
	if (s->locked)
		return TR(TEXT_ALT_OPEN "Keys locked: hold * to unlock" TEXT_ALT_NEXT "Locked: hold * to unlock" TEXT_ALT_CLOSE);
#if CONFIG_SATELLITES
	if (!v->vfo) {	// on a VFO letter a held digit types it: no shortcut
		const char *hh = ui_hold_dual_hint();	// "# options  4 ISS  Back stop"; NULL: no digit is set
		if (hh != NULL)
			return hh;
	}
#endif
	return TR("# options  Back stop");
}

// Home with the ring: the rows A..F and the line under them (ui_draw_home_dual adds the banner and the hint, from
// its own frame: this one is on the UI task's deepest path)
static void __attribute__((noinline)) mw_draw_home(const radio_view_t *v, const ui_state_t *s, uint8_t n)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *sm = L->small;
	static ui_mw_row_t r;	// static: Home is on the UI task's deepest path
	int w = gfx_width();
	gfx_clear(ui_col.bg);
	ui_draw_top(v, s, NULL, NULL);
	int top = L->strip_h + 1, bottom = L->hint_y - sm->h - 2;
	int h = (bottom - top) / n;
	// The largest name font a row takes: two lines (name and details) where they fit, else one; large text: one line
	// as large as fits
	const font_t *nf = sm;
	bool two = false;
	if (s->large_text)
		nf = L->large->h + 2 <= h ? L->large : L->medium->h + 2 <= h ? L->medium : sm;
	else if (L->medium->h + sm->h + 2 <= h)
		nf = L->medium, two = true;
	else if (2 * sm->h + 2 <= h)
		two = true;
	else
		nf = L->medium->h + 2 <= h ? L->medium : sm;
	char sel = '-', rx = '-';
	mw_newest = 0;
	mw_any_rx = false;
	for (uint8_t k = 0; k < n; k++) {	// the newest call, a call now
		ui_dual_ring_row(k, v, &r);
		if (r.heard_ms != 0 && (mw_newest == 0 || (int32_t)(r.heard_ms - mw_newest) > 0))
			mw_newest = r.heard_ms;
		mw_any_rx = mw_any_rx || r.rx;
	}
	for (uint8_t k = 0; k < n; k++) {
		ui_dual_ring_row(k, v, &r);
		if (r.sel)
			sel = r.letter;
		if (r.rx)
			rx = r.letter;
		int y = top + k * h;
		mw_draw_row(v, s, &r, y, h, nf, two);
		if (k + 1u < n)
			gfx_hline(0, y + h - 1, w, ui_col.line);
	}
	mw_draw_state(v, s, top + n * h + 1, n, sel, rx);
}
