/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus Tools > SSTV > OK: send the test picture (radio/radio_sstvtx.h, docs/plus.md "SSTV send"). The page shows
 * the picture at half size (the card with the radio's callsign, dithered to the SSTV screen's colours), the mode with
 * its length (Left / Right: Robot 36, Martin M1, PD120), the TX frequency, then what happens. OK sends after the checks
 * the page can make itself (a callsign as the radio name, an FM simplex channel, the band plan: an amateur frequency,
 * no repeater, satellite or APRS frequency, as DOTA and the cross-band use it; the time-out timer longer than the
 * picture); the radio core checks again and keys. While it sends a progress bar and the line; Back stops it at once,
 * so does PTT (the radio core), and leaving the page any other way (ui_sstvtx_tick). Installed as core/ui/ui_sstvtx.c.
 *
 * The preview is drawn only when the page is entered or something else drew the display (ui_strip_draws), like the
 * SSTV receive screen's picture; the lines under it every time. */
#include "config.h"
#if CONFIG_SSTV_TX
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
#include "radio/sstv.h"
#include "radio/sstv_tx.h"
#include "radio/radio_sstvtx.h"
#include "ui/ui_xband.h"

#define PREVIEW_W	(SSTV_TX_CARD_W / 2u)
#define PREVIEW_H	(SSTV_TX_CARD_H / 2u)

static uint8_t mode;		/* SSTV_TX_*, kept while the radio is on */
static bool drawn, asked;	/* the preview is on the display; a start or stop was asked: tell its end */
static uint16_t seq_seen;
static uint32_t strip_seen;

static bool on_air(const radio_sstvtx_info_t *in)
{
	return in->state == SSTVTX_KEYUP || in->state == SSTVTX_SENDING;
}

/* The radio name's callsign into call (empty: none) */
static uint8_t callsign(char call[RADIO_CWID_CALL_MAX + 1])
{
	char name[17];
	ui_svc_name_get(UI_NAME_RADIO, name);
	return radio_cwid_callsign(name, call, RADIO_CWID_CALL_MAX + 1u);
}

static void sstvtx_enter(void)
{
	drawn = false;
	seq_seen = radio_sstvtx_seq();
	ui_dirty();
}

/* Why the radio core refused or stopped (radio_sstvtx.h) */
static const char *why_text(const radio_sstvtx_info_t *in)
{
	switch (in->why) {
	case SSTVTX_R_DENY:
		switch (in->deny) {
		case TXDENY_RX_ONLY: return TR("This channel is RX only");
		case TXDENY_BUSY: return TR("Channel busy");
		default: return TR("Can't transmit there");
		}
	case SSTVTX_R_BUSY: return TR("Not while transmitting");
	case SSTVTX_R_MODE: return TR("SSTV: tune an FM channel");
	case SSTVTX_R_SPLIT: return TR("Repeater channel (RX and TX differ)");
	case SSTVTX_R_CALL: return TR("Set your callsign (the radio name) in qdmr.");
	case SSTVTX_R_TOT: return TR("TX time-out shorter than the picture");
	case SSTVTX_R_NOMEM: return TR("Not enough memory for SSTV");
	case SSTVTX_R_WATCH: return TR("Stop the scan or watch first");
	case SSTVTX_R_BAND: {
		const char *plan = ui_xband_refusal(ui_view()->ch.tx_hz);
		return plan != NULL ? plan : TR("Can't transmit there");
	}
	default: return TR("Can't transmit there");
	}
}

/* The page's own checks before asking the radio core: NULL when it may ask */
static const char *refusal(void)
{
	const radio_view_t *v = ui_view();
	char call[RADIO_CWID_CALL_MAX + 1];
	if (callsign(call) == 0)
		return TR("Set your callsign (the radio name) in qdmr.");
	if (v->scan != 0 || UI_AIR_ON())	/* a scan or a watch: the channel on view is the ring's, not the selected letter */
		return TR("Stop the scan or watch first");
	if (v->ch.mode != RADIO_MODE_FM || v->am)
		return TR("SSTV: tune an FM channel");
	if (v->ch.rx_hz != v->ch.tx_hz)
		return TR("Repeater channel (RX and TX differ)");
	const char *plan = ui_xband_refusal(v->ch.tx_hz);	/* amateur, no repeater / satellite / APRS frequency */
	if (plan != NULL)
		return plan;
	if (v->tot_s != 0 && (uint32_t)v->tot_s * 1000u < sstv_tx_ms(mode) + 100u)
		return TR("TX time-out shorter than the picture");
	return NULL;
}

/* s from y, on two lines when one is too narrow (cut at the last space that fits); buf holds the first */
static void text_wrapped(const font_t *f, int y, const char *s, color_t c, char *buf, size_t cap)
{
	int w = gfx_width();
	size_t cut = 0;
	if (gfx_text_width(f, s) > w) {
		for (size_t i = 1; s[i] != '\0' && i < cap; i++) {
			if (s[i] != ' ')
				continue;
			memcpy(buf, s, i);
			buf[i] = '\0';
			if (gfx_text_width(f, buf) > w)
				break;
			cut = i;
		}
	}
	if (cut == 0) {
		gfx_text(f, 0, y, s, c);
		return;
	}
	memcpy(buf, s, cut);
	buf[cut] = '\0';
	gfx_text(f, 0, y, buf, c);
	gfx_text(f, 0, y + f->h + 1, s + cut + 1, c);
}

static void draw_preview(int x0, int y0, const sstv_card_t *card)
{
	for (unsigned y = 0; y < PREVIEW_H; y++)
		for (unsigned x = 0; x < PREVIEW_W; x++) {
			uint8_t rgb[3], c[3];
			sstv_card_rgb(card, (uint16_t)(2u * x), (uint16_t)(2u * y), rgb);
			sstv_colour(sstv_dither(rgb, (uint16_t)x, (uint16_t)y), c);
			gfx_pixel(x0 + (int)x, y0 + (int)y, RGB565(c[0], c[1], c[2]));
		}
}

static void sstvtx_draw(void)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->small;
	const radio_view_t *v = ui_view();
	int w = gfx_width();
	radio_sstvtx_info_t in;
	radio_sstvtx_info(&in);
	bool air = on_air(&in);
	uint8_t m = air ? in.mode : mode;
	bool full = !drawn || ui_strip_draws != strip_seen;
	if (full)
		gfx_clear(ui_col.bg);
	int top = ui_draw_top(v, &ui_st, TR("SSTV send"), sstv_tx_mode_name(m));
	int py = top + 2, px = (w - (int)PREVIEW_W) / 2;
	char call[RADIO_CWID_CALL_MAX + 1], buf[48];
	uint8_t n = callsign(call);
	if (full) {
		sstv_card_t card;
		sstv_card_init(&card, n != 0 ? call : "");
		draw_preview(px, py, &card);
	}
	int y = py + (int)PREVIEW_H + 4;
	gfx_fill(0, y, w, L->hint_y - y, ui_col.bg);
	/* the mode and its length, the callsign */
	sbuf_t b;
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, air ? sstv_tx_mode_name(m) : "< ");
	if (!air) {
		sb_str(&b, sstv_tx_mode_name(m));
		sb_str(&b, " >");
	}
	sb_str(&b, "  ");
	uint32_t s = (sstv_tx_ms(m) + 500u) / 1000u;
	if (s >= 60u) {
		sb_u32(&b, s / 60u, 1);
		sb_chr(&b, ':');
		sb_u32(&b, s % 60u, 2);
	} else {
		sb_u32(&b, s, 1);
		sb_str(&b, " s");
	}
	gfx_text(f, 0, y, buf, ui_col.fg);
	y += f->h + 2;
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, TR("TX"));
	sb_chr(&b, ' ');
	sb_freq(&b, v->ch.tx_hz);
	if (n != 0) {
		sb_str(&b, "  ");
		sb_str(&b, call);
	}
	gfx_text(f, 0, y, buf, n != 0 ? ui_col.fg : ui_col.warn);
	y += f->h + 4;
	/* what happens */
	if (air) {
		int bw = w - 8, bh = f->h;
		uint32_t done = in.lines != 0 ? (uint32_t)in.line * (uint32_t)(bw - 2) / in.lines : 0;
		gfx_rect(4, y, bw, bh, ui_col.line);
		gfx_fill(5, y + 1, (int)done, bh - 2, ui_col.tx);
		y += bh + 3;
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, TR("Sending"));
		sb_chr(&b, ' ');
		sb_u32(&b, in.line, 1);
		sb_chr(&b, '/');
		sb_u32(&b, in.lines, 1);
		gfx_text(f, 0, y, buf, ui_col.tx);
	} else if (in.state == SSTVTX_SENT) {
		gfx_text(f, 0, y, TR("Picture sent"), ui_col.fg);
	} else if (in.state == SSTVTX_STOPPED) {
		text_wrapped(f, y, in.why != SSTVTX_R_NONE ? why_text(&in) : TR("Sending stopped"), ui_col.warn, buf, sizeof buf);
	} else if (in.state == SSTVTX_REFUSED) {
		text_wrapped(f, y, why_text(&in), ui_col.warn, buf, sizeof buf);
	} else {
		gfx_text(f, 0, y, TR("Test picture: OK sends"), ui_col.dim);
	}
	ui_draw_bottom(&ui_st, air ? TR("Back stops") : TR("OK send  <> mode"));
	gfx_clip(0, py + (int)PREVIEW_H, w, gfx_height() - py - (int)PREVIEW_H);	/* a banner leaves the preview */
	ui_draw_banner(&ui_st);
	gfx_clip_off();
	strip_seen = ui_strip_draws;
	drawn = true;
}

static bool sstvtx_key(const input_event_t *ev)
{
	if (ev->gesture != GESTURE_PRESS && ev->gesture != GESTURE_REPEAT)
		return false;
	radio_sstvtx_info_t in;
	radio_sstvtx_info(&in);
	if (on_air(&in)) {
		if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS) {
			radio_req_sstv_tx(SSTV_TX_MODES);	/* off the air at once; a second Back leaves */
			asked = true;
		}
		return true;	/* nothing else while it sends */
	}
	if (ev->action == ACT_NAV_LEFT || ev->action == ACT_NAV_RIGHT) {
		mode = (uint8_t)((mode + (ev->action == ACT_NAV_RIGHT ? 1u : SSTV_TX_MODES - 1u)) % SSTV_TX_MODES);
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && ev->gesture == GESTURE_PRESS) {
		const char *why = refusal();
		if (why != NULL) {
			ui_toast(why);
		} else {
			radio_req_sstv_tx(mode);
			asked = true;
		}
		ui_dirty();
		return true;
	}
	return false;
}

bool ui_sstvtx_tick(uint8_t scr)
{
	uint16_t q = radio_sstvtx_seq();
	if (scr != SCR_SSTV_TX) {
		radio_sstvtx_info_t in;
		radio_sstvtx_info(&in);
		if (on_air(&in) && q != seq_seen) {
			seq_seen = q;	/* (once: the stop's own change is seen by the next look) */
			radio_req_sstv_tx(SSTV_TX_MODES);	/* the page was left (not by Back): off the air */
		}
		return false;
	}
	if (q == seq_seen)
		return false;
	seq_seen = q;
	radio_sstvtx_info_t in;
	radio_sstvtx_info(&in);
	if (asked && !on_air(&in)) {
		asked = false;
		ui_toast(in.state == SSTVTX_SENT ? TR("Picture sent") : in.state == SSTVTX_REFUSED ? why_text(&in) :
			 in.why != SSTVTX_R_NONE ? why_text(&in) : TR("Sending stopped"));
	}
	return true;
}

const ui_scr_def_t ui_sstvtx_screen = {
	.name = "sstv-tx", .flags = SF_FIELD, .key = sstvtx_key, .draw = sstvtx_draw, .enter = sstvtx_enter,
};
#endif
