/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus Tools > Cross-band (radio/xband.h, docs/plus.md "Cross-band store and forward"): the form (A and B typed
 * or picked from a zone's channels, each with its mode, then colour code, slot, power, Start), the checks before a start, the
 * attended-operation question, then Home. While it runs the screen is locked: every key but Back is refused with a
 * toast, Back stops it (owner, 2026-11-20); Home's status strip says CROSS and its bottom line what the relay does
 * ("Record A 0:12  Back"). Each page is a list (ui_draw_list). The form edits xband_req() itself (no copy);
 * the picker reads a zone and a channel into the tool screens' shared buffer (ui_scratch.h). Installed as
 * core/ui/ui_xband.c. */
#include "config.h"
#if CONFIG_XBAND || CONFIG_DOTA	/* the band plan: DOTA checks with it too (ui_xband_refusal) */
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_svc.h"
#include "ui_app.h"
#include "fmt.h"
#include "gfx.h"
#include "i18n.h"
#include "board.h"
#include "radio/radio.h"
#include "radio/radio_cwid.h"
#include "radio/tones.h"
#include "radio/xband.h"
#include "ui/ui_scratch.h"
#include "ui/ui_xband.h"
#if CONFIG_REPEATERS
#include "ui/rpt.h"
#endif
#if CONFIG_DOTA
#include "radio/dota.h"
#endif

/* --- The checks before a start (the band plan's part DOTA's frequency check shares) --- */

#if CONFIG_REPEATERS
/* A repeater of the list transmits or listens there (within half a 12.5 kHz channel) */
static bool known_repeater(uint32_t hz)
{
	for (uint32_t i = 0; i < rpt_info.count; i++) {
		const rpt_entry_t *e = &rpt_table[i];
		uint32_t in = (uint32_t)((int32_t)e->rx_hz + (int32_t)e->off * (int32_t)RPT_STEP_HZ);
		if ((hz + 6250u > e->rx_hz && hz < e->rx_hz + 6250u) ||
		    (e->off != 0 && hz + 6250u > in && hz < in + 6250u))
			return true;
	}
	return false;
}
#endif

/* The refusal's text, NULL: allowed */
static const char *why_text(uint8_t why)
{
	static const char *const text[XB_R_COUNT] = {
		[XB_R_SAME] = TRN("A and B are the same"),
		[XB_R_PMR446] = TRN("PMR446: no repeaters allowed"),
		[XB_R_BAND] = TRN("Not an amateur band"),
		[XB_R_REPEATER] = TRN("Repeater frequency"),
		[XB_R_SATELLITE] = TRN("Satellite segment"),
		[XB_R_APRS] = TRN("APRS frequency"),
		[XB_R_MODE] = TRN("Mode pair not supported"),
		[XB_R_CALL] = TRN("Set your callsign (the radio name) in qdmr."),
		[XB_R_ID] = TRN("Set your DMR ID first"),
		[XB_R_OFFSET] = TRN("Repeater channel (RX and TX differ)"),
	};
	return why < XB_R_COUNT && text[why] != NULL ? tr(text[why]) : NULL;
}

/* One frequency: the band plan, then the repeater list's; free: the segments free (Cross-band's #, never DOTA's) */
static uint8_t hz_why(uint32_t hz, bool free)
{
	uint8_t why = xband_check_hz_free(hz, free);
#if CONFIG_REPEATERS
	if (why == XB_R_OK && !free && known_repeater(hz))
		why = XB_R_REPEATER;
#endif
	return why;
}

const char *ui_xband_refusal(uint32_t hz)
{
	return why_text(hz_why(hz, false));
}
#endif

#if CONFIG_XBAND
/* A row: its kind, and in bit 4 the side (A 0, B 1) for the side's rows. The form: A, A mode, B, B mode, then each
 * side's filters and TX values for its mode (DMR: RX CC / TS / TG, TX CC / TS / TG; P25: RX NAC, TX NAC, TX TG; FM:
 * RX Tone, TX Tone, owner 2026-11-25), Power, Start (owner, 2026-11-25: the per-side rows) */
enum { K_FREQ, K_MODE, K_RXCC, K_RXTS, K_RXTG, K_TXCC, K_TXTS, K_TXTG, K_RXNAC, K_TXNAC, K_RXTONE, K_TXTONE, K_POWER, K_GO };
#define ROW(side, k)	((uint8_t)((side) << 4 | (k)))
#define KIND(r)		((r) & 15u)
#define SIDE(r)		((r) >> 4 & 1u)
#define ROWS_MAX	18u

static uint8_t focus, ndigits, page;	/* page 1: the attended-operation question */
static char digits[9];			/* a frequency or a number being typed */
static uint8_t rows[ROWS_MAX], nrows;	/* the form's rows as the modes make it */
static uint16_t form_top;
static uint16_t shown_serial;
static uint8_t was_on;			/* the session ran at the last tick (its end is told once) */

/* Everything before the question: a toast and false when refused */
static bool checks(void)
{
	xband_req_t *r = xband_req();
	char name[CODEPLUG_NAME_LEN + 1];
	codeplug_radio_name(name);
	if (radio_cwid_callsign(name, r->call, sizeof r->call) == 0)
		r->call[0] = '\0';
	uint8_t why = xband_check(r);
#if !CONFIG_NXDN
	if (why == XB_R_OK && (r->mode[0] == XB_MODE_NXDN || r->mode[1] == XB_MODE_NXDN))
		why = XB_R_MODE;	/* no NXDN receiver in this build */
#endif
	for (uint8_t k = 0; why == XB_R_OK && k < 2; k++)
		why = hz_why(r->hz[k], xband_segs_free());	/* the repeater list's (the band plan's passed in xband_check) */
	if (why == XB_R_OK && ui_view()->dmr.own_id == 0)
		why = XB_R_ID;
	const char *t = why_text(why);
	if (t == NULL && (!board_in_band(r->hz[0], true) || !board_in_band(r->hz[1], true)))
		t = TR("Can't transmit there");
#if CONFIG_DOTA
	if (t == NULL && dota_on())
		t = TR("A DOTA transfer runs");
#endif
	if (t != NULL) {
		ui_toast(t);
		return false;
	}
	return true;
}

static void go(void)
{
#if CONFIG_DUAL_WATCH
	if (ui_st.dual.on)
		ui_dual_stop();		/* one channel at a time: the watch ends (the radio core stops its scan too) */
#endif
	if (!xband_start()) {
		ui_toast(TR("Can't transmit there"));
		return;
	}
	was_on = 1;
	ui_go_home();
	ui_toast(TR("Cross-band on: Back stops"));
}

/* --- The form --- */

#if CONFIG_SUBTONE
/* A side's FM sub-tone as the radio's subtone_t (RX gates what is relayed, TX adds it to the carrier; owner,
 * 2026-11-25). Stored on the side as (type, value) so the engine stays free of hal_radio.h */
static subtone_t side_tone(const xband_side_t *x, bool tx)
{
	subtone_t t = { tx ? x->tx_tone_type : x->rx_tone_type, tx ? x->tx_tone_value : x->rx_tone_value };
	return t;
}

static void set_side_tone(xband_side_t *x, bool tx, subtone_t t)
{
	if (tx) {
		x->tx_tone_type = t.type;
		x->tx_tone_value = t.value;
	} else {
		x->rx_tone_type = t.type;
		x->rx_tone_value = t.value;
	}
}
#endif

static void form_rows(void)
{
	const xband_req_t *q = xband_req();
	static const uint8_t dmr[] = { K_RXCC, K_RXTS, K_RXTG, K_TXCC, K_TXTS, K_TXTG }, p25[] = { K_RXNAC, K_TXNAC, K_TXTG };
#if CONFIG_SUBTONE
	static const uint8_t fm[] = { K_RXTONE, K_TXTONE };	/* an FM side: a sub-tone to receive by and one to send */
#endif
	nrows = 0;
	for (uint8_t k = 0; k < 4; k++)
		rows[nrows++] = ROW(k >> 1, k & 1u);	/* A, A mode, B, B mode */
	for (uint8_t sd = 0; sd < 2; sd++) {
		bool d = q->mode[sd] == XB_MODE_DMR, p = q->mode[sd] == XB_MODE_P25;
		const uint8_t *tbl = d ? dmr : p ? p25 : NULL;
		uint8_t n = d ? sizeof dmr : p ? sizeof p25 : 0;
#if CONFIG_SUBTONE
		if (q->mode[sd] == XB_MODE_FM) {
			tbl = fm;
			n = sizeof fm;
		}
#endif
		for (uint8_t i = 0; i < n; i++)
			rows[nrows++] = ROW(sd, tbl[i]);
	}
	rows[nrows++] = K_POWER;
	rows[nrows++] = K_GO;
	if (focus >= nrows)
		focus = (uint8_t)(nrows - 1u);
}

static uint32_t tg_max(uint8_t sd)
{
	return xband_req()->mode[sd] == XB_MODE_P25 ? XB_P25_TG_MAX : XB_TG_MAX;
}

static void sb_hex3(sbuf_t *b, uint16_t v)
{
	for (int sh = 8; sh >= 0; sh -= 4)
		sb_chr(b, "0123456789ABCDEF"[v >> sh & 15u]);
}

static void form_item(uint16_t i, ui_item_t *it)
{
	static const char *const kinds[] = { [K_RXCC] = "RX CC", [K_RXTS] = "RX TS", [K_RXTG] = "RX TG", [K_TXCC] = "TX CC",
					     [K_TXTS] = "TX TS", [K_TXTG] = "TX TG", [K_RXNAC] = "RX NAC", [K_TXNAC] = "TX NAC",
					     [K_RXTONE] = "RX Tone", [K_TXTONE] = "TX Tone" };
	static const char *const modes[XB_MODES] = { "DMR", "FM", "P25", "NXDN" };
	const xband_req_t *q = xband_req();
	uint8_t r = rows[i], k = KIND(r), sd = (uint8_t)SIDE(r);
	const xband_side_t *x = &q->side[sd];
	const char *any = TR("Any");
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	if (k == K_POWER || k == K_GO) {
		sb_str(&b, k == K_POWER ? TR("Power") : TR("Start"));
	} else if (k == K_MODE) {
		sb_str(&b, sd ? TR("B mode") : TR("A mode"));
	} else {
		sb_chr(&b, (char)('A' + sd));
		if (k != K_FREQ) {
			sb_chr(&b, ' ');
			sb_str(&b, kinds[k]);
		}
	}
	sb_init(&b, it->value, sizeof it->value);
	if (i == focus && ndigits != 0) {
		sb_str(&b, digits);
		return;
	}
	switch (k) {
	case K_FREQ:
		if (q->hz[sd] != 0)
			sb_freq(&b, q->hz[sd]);
		else
			sb_str(&b, "---");
		break;
	case K_MODE: sb_str(&b, modes[q->mode[sd] < XB_MODES ? q->mode[sd] : 0]); break;
	case K_RXCC:
	case K_TXCC:
		if (k == K_RXCC && x->rx_cc >= XB_CC_ANY)
			sb_str(&b, any);
		else
			sb_u32(&b, k == K_RXCC ? x->rx_cc : x->tx_cc, 1);
		break;
	case K_RXTS:
	case K_TXTS:
		if (k == K_RXTS && x->rx_ts >= XB_TS_ANY)
			sb_str(&b, any);
		else
			sb_u32(&b, k == K_RXTS ? x->rx_ts : x->tx_ts, 1);
		break;
	case K_RXTG:
	case K_TXTG: {
		uint32_t v = k == K_RXTG ? x->rx_tg : x->tx_tg;
		if (v == 0)
			sb_str(&b, k == K_RXTG ? any : TR("As heard"));
		else
			sb_u32(&b, v, 1);
		break;
	}
	case K_RXNAC:
		if (x->rx_nac >= XB_NAC_ANY) {
			sb_str(&b, any);
			break;
		}
		/* fall through */
	case K_TXNAC: sb_hex3(&b, k == K_RXNAC ? x->rx_nac : x->tx_nac); break;
#if CONFIG_SUBTONE
	case K_RXTONE:
	case K_TXTONE: {
		subtone_t t = side_tone(x, k == K_TXTONE);
		if (t.type == TONE_NONE)
			sb_str(&b, TR("None"));
		else
			sb_tone(&b, &t);
		break;
	}
#endif
	case K_POWER: sb_str(&b, q->power <= 1u ? TR("Low") : q->power == 2u ? TR("Mid") : TR("High")); break;
	default: break;
	}
}

static void xband_draw(void)
{
	ui_list_t l;
	memset(&l, 0, sizeof l);
	l.prefix = TR("Cross-band");
	if (xband_segs_free())
		l.header = TR("Segments free");	/* the band plan's segment refusals are lifted (#) */
	if (page == 1) {
		l.body = TR("Attended only: stay at the radio while it relays. Start?");
		l.hint = TR("OK yes  Back no");
	} else {
		form_rows();
		l.count = nrows;
		l.focus = focus;
		l.top = &form_top;
		l.item = form_item;
		/* the # group last; the shortest form fits 24 characters (9 px each, 220 px): it is never dropped */
		l.hint = xband_segs_free() ? TR(TEXT_ALT_OPEN "OK pick  <> change  # segs: on" TEXT_ALT_NEXT
						"OK pick  <>  # segs: on" TEXT_ALT_NEXT
						"OK  <>  # segs: on" TEXT_ALT_CLOSE) :
					     TR(TEXT_ALT_OPEN "OK pick  <> change  # segs: off" TEXT_ALT_NEXT
						"OK pick  <>  # segs: off" TEXT_ALT_NEXT
						"OK  <>  # segs: off" TEXT_ALT_CLOSE);
	}
	ui_draw_list(ui_view(), &ui_st, &l);
}

/* (v + dir) round n values from 0 */
static uint32_t wrap(uint32_t v, int dir, uint32_t n)
{
	return (v + n + (uint32_t)dir) % n;
}

static void step(uint8_t r, int dir)
{
	xband_req_t *q = xband_req();
	uint8_t sd = (uint8_t)SIDE(r);
	xband_side_t *x = &q->side[sd];
	switch (KIND(r)) {
	case K_FREQ: q->hz[sd] = (uint32_t)((int32_t)q->hz[sd] + dir * 12500); break;
	case K_MODE:
		q->mode[sd] = (uint8_t)wrap(q->mode[sd], dir, XB_MODES);
		if (x->tx_tg > tg_max(sd))
			x->tx_tg = 0;	/* a DMR talkgroup P25 can't send to: as heard */
		break;
	case K_RXCC: x->rx_cc = (uint8_t)wrap(x->rx_cc, dir, XB_CC_ANY + 1u); break;	/* 0-15, Any */
	case K_TXCC: x->tx_cc = (uint8_t)wrap(x->tx_cc, dir, 16); break;
	case K_RXTS: x->rx_ts = (uint8_t)(wrap(x->rx_ts - 1u, dir, 3) + 1u); break;	/* 1, 2, Any */
	case K_TXTS: x->tx_ts = (uint8_t)(3u - x->tx_ts); break;
	case K_RXTG:
	case K_TXTG: {
		uint32_t *v = KIND(r) == K_RXTG ? &x->rx_tg : &x->tx_tg;
		if ((dir > 0 && *v < tg_max(sd)) || (dir < 0 && *v > 0))
			*v = (uint32_t)((int32_t)*v + dir);	/* 0: Any / as heard */
		break;
	}
	case K_RXNAC: x->rx_nac = (uint16_t)wrap(x->rx_nac, dir, XB_NAC_ANY + 1u); break;	/* 000-F7D, Any */
	case K_TXNAC: x->tx_nac = (uint16_t)wrap(x->tx_nac, dir, XB_NAC_ANY); break;
#if CONFIG_SUBTONE
	case K_RXTONE:
	case K_TXTONE: {
		bool tx = KIND(r) == K_TXTONE;
		subtone_t t = side_tone(x, tx);
		set_side_tone(x, tx, tones_step(&t, dir > 0 ? 1 : -1));	/* None, CTCSS low to high, DCS by code */
		break;
	}
#endif
	case K_POWER: q->power = (uint8_t)(wrap(q->power - 1u, dir, 3) + 1u); break;
	default: break;
	}
}

/* The digits a row takes (0: none): a frequency 7, a talkgroup 8, a NAC 3 (hex: 0-9 typed, Left / Right the rest), a
 * colour code 2 */
static uint8_t digits_max(uint8_t r)
{
	static const uint8_t n[] = { [K_FREQ] = 7, [K_RXTG] = 8, [K_TXTG] = 8, [K_RXNAC] = 3, [K_TXNAC] = 3,
				     [K_RXCC] = 2, [K_TXCC] = 2, [K_GO] = 0 };
	return n[KIND(r)];
}

/* A typed frequency in MHz, its first three digits the MHz (every band the relay may use has three): 433 is 433.000,
 * 4335 and 43350 433.500, 145500 145.500, 4335125 433.5125. Fewer than three digits: told, the frequency kept. (Before
 * 2026-11-25 only six or seven digits counted and any other entry was dropped without a word, so the row went back to
 * an older frequency, maybe one refused, while the user believed the typed one was set) */
static void typed(uint8_t r)
{
	uint8_t k = KIND(r), sd = (uint8_t)SIDE(r);
	bool hex = k == K_RXNAC || k == K_TXNAC;
	xband_side_t *x = &xband_req()->side[sd];
	uint32_t n = 0;
	for (uint8_t i = 0; i < ndigits; i++)
		n = n * (hex ? 16u : 10u) + (uint32_t)(digits[i] - '0');
	if (ndigits == 0) {
		/* nothing typed */
	} else if (k == K_FREQ && ndigits >= 3u) {
		for (uint8_t i = ndigits; i < 9u; i++)
			n *= 10u;	/* (7 digits at most: below 10^9) */
		xband_req()->hz[sd] = n;
	} else if (k == K_FREQ) {
		ui_toast(TR("Type the MHz: 433500"));
	} else if ((k == K_RXTG || k == K_TXTG) && n <= tg_max(sd)) {
		*(k == K_RXTG ? &x->rx_tg : &x->tx_tg) = n;	/* 0: Any / as heard */
	} else if (k == K_RXNAC && n <= XB_NAC_ANY) {
		x->rx_nac = (uint16_t)n;
	} else if (k == K_TXNAC && n < XB_NAC_ANY) {
		x->tx_nac = (uint16_t)n;
	} else if ((k == K_RXCC || k == K_TXCC) && n <= 15u) {
		*(k == K_RXCC ? &x->rx_cc : &x->tx_cc) = (uint8_t)n;
	} else {
		ui_toast(TR("Out of range"));	/* the value stays */
	}
	ndigits = 0;
}

static bool xband_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	uint8_t act = ev->action;
	if (page == 1) {
		if (ev->gesture == GESTURE_PRESS && act == ACT_SELECT) {
			page = 0;
			go();
		} else if (ev->gesture == GESTURE_PRESS && act == ACT_BACK) {
			page = 0;
		}
		ui_dirty();
		return true;
	}
	form_rows();
	uint8_t r = rows[focus];
	if (press && (act == ACT_NAV_UP || act == ACT_NAV_DOWN)) {
		if (ndigits)
			typed(r);
		if (act == ACT_NAV_UP && focus > 0)
			focus--;
		else if (act == ACT_NAV_DOWN && focus + 1u < nrows)
			focus++;
	} else if ((press && (act == ACT_NAV_LEFT || act == ACT_NAV_RIGHT)) || (act == ACT_ENC_STEP && ev->value != 0)) {
		ndigits = 0;
		step(r, act == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : act == ACT_NAV_RIGHT ? 1 : -1);
	} else if (ev->gesture == GESTURE_PRESS && act >= ACT_DIGIT_0 && act <= ACT_DIGIT_9 && digits_max(r) != 0) {
		if (ndigits < digits_max(r))
			digits[ndigits++] = (char)('0' + act - ACT_DIGIT_0);
		digits[ndigits] = '\0';
	} else if (ev->gesture != GESTURE_PRESS) {
		return false;
	} else if (act == ACT_CONTEXT) {
		/* # : the segments free or not (owner, 2026-11-25); never while a session runs */
		if (!xband_set_segs_free(!xband_segs_free()))
			ui_toast(TR("Cross-band on: stop it first"));
	} else if (act == ACT_BACK) {
		if (!ndigits)
			return false;	/* leaves */
		ndigits = 0;
	} else if (act != ACT_SELECT) {
		return false;
	} else if (ndigits) {
		typed(r);
	} else if (KIND(r) == K_FREQ) {
		ui_push(SCR_XBAND_PICK, SIDE(r));
	} else if (KIND(r) == K_GO) {
		if (checks())
			page = 1;	/* the question */
	} else {
		step(r, 1);
	}
	ui_dirty();
	return true;
}

static void xband_enter(void)
{
	xband_req_t *q = xband_req();
	page = 0;
	ndigits = 0;
	if (q->hz[0] == 0)
		q->hz[0] = ui_view()->ch.rx_hz;
	ui_dirty();
}

const ui_scr_def_t ui_xband_screen = {
	.name = "xband", .flags = SF_FIELD, .key = xband_key, .draw = xband_draw, .enter = xband_enter,
};

/* --- The picker: A or B (arg) from a zone's channels; Left / Right another zone --- */

static uint16_t pick_zone;

static zone_t *pick_z(void)
{
	zone_t *z = &ui_scratch_take(UI_SCRATCH_XBAND)->xband.z;
	return codeplug_zone(pick_zone, z) ? z : NULL;
}

static uint16_t pick_count(void)
{
	zone_t *z = pick_z();
	return z != NULL ? z->count : 0;
}

static void pick_item(uint16_t i, ui_item_t *it)
{
	zone_t *z = pick_z();
	channel_t *ch = &ui_scratch.xband.ch;
	sbuf_t b;
	if (z == NULL || i >= z->count || !codeplug_channel(z->channels[i], ch))
		return;
	sb_init(&b, it->label, sizeof it->label);
	sb_u32(&b, z->channels[i], 1);
	sb_chr(&b, ' ');
	sb_str(&b, ch->name);
	sb_init(&b, it->value, sizeof it->value);
	sb_freq(&b, ch->rx_hz);
	if (ch->tx_hz != 0 && ch->tx_hz != ch->rx_hz)
		it->flags |= UI_ITEM_DIM;	/* a repeater channel: refused */
}

static void pick_select(uint16_t i, bool hold)
{
	(void)hold;
	zone_t *z = pick_z();
	channel_t *ch = &ui_scratch.xband.ch;
	uint8_t side = ui_frame()->arg ? 1u : 0u;
	if (z == NULL || i >= z->count || !codeplug_channel(z->channels[i], ch))
		return;
	if (ch->tx_hz != 0 && ch->tx_hz != ch->rx_hz) {
		ui_toast(why_text(XB_R_OFFSET));
		return;
	}
	/* the channel's mode comes with it; one the relay has no use for (YSF, dPMR, ...) is refused */
	uint8_t mode = ch->mode == RADIO_MODE_DMR ? XB_MODE_DMR : ch->mode == RADIO_MODE_FM ? XB_MODE_FM :
		       codeplug_mode_p25(ch->mode) && !codeplug_mode_ysf(ch->mode) ? XB_MODE_P25 :
		       codeplug_mode_nxdn(ch->mode) ? XB_MODE_NXDN : XB_MODES;
	if (mode == XB_MODES) {
		ui_toast(why_text(XB_R_MODE));
		return;
	}
	xband_req_t *q = xband_req();
	xband_side_t *x = &q->side[side];
	q->hz[side] = ch->rx_hz;
	q->mode[side] = mode;
	x->tx_tg = 0;
	if (mode == XB_MODE_DMR) {
		/* a DMR channel brings its colour code, slot and group contact as the side's TX values (heard: as set) */
		contact_t c;
		x->tx_cc = (uint8_t)(ch->color_code & 15u);
		x->tx_ts = ch->timeslot == 2 ? 2 : 1;
		if (codeplug_contact(codeplug_channel_talk_contact(ch), &c) && c.type == CONTACT_GROUP && c.id <= XB_TG_MAX)
			x->tx_tg = c.id;
	} else if (mode == XB_MODE_P25) {
		uint16_t nac = codeplug_p25_tx_nac(ch);	/* a P25 channel its TX NAC (0x293 for one that hears any) */
		if (nac < XB_NAC_ANY)
			x->tx_nac = nac;
	}
#if CONFIG_SUBTONE
	else if (mode == XB_MODE_FM) {
		/* an FM channel brings its CTCSS / DCS: the one it receives by (RX gate) and the one it sends (TX tone) */
		x->rx_tone_type = ch->rx_tone.type;
		x->rx_tone_value = ch->rx_tone.value;
		x->tx_tone_type = ch->tx_tone.type;
		x->tx_tone_value = ch->tx_tone.value;
	}
#endif
	ui_pop();
	ui_dirty();
}

static bool pick_key(const input_event_t *ev)
{
	uint16_t n = codeplug_zone_count();
	if (n == 0 || ev->gesture != GESTURE_PRESS || (ev->action != ACT_NAV_LEFT && ev->action != ACT_NAV_RIGHT))
		return false;
	pick_zone = (uint16_t)((pick_zone + (ev->action == ACT_NAV_RIGHT ? 1u : n - 1u)) % n);
	ui_frame()->focus = 0;
	ui_dirty();
	return true;
}

static void pick_enter(void)
{
	pick_zone = ui_view()->zone_index;
}

static const char *pick_title(void)
{
	return ui_frame()->arg ? "B" : "A";
}

const ui_scr_def_t ui_xband_pick_screen = {
	.name = "xband-pick", .flags = SF_LIST | SF_FIELD, .prefix = TRN("Cross-band"),
	.hint = TRN("OK pick  <> zone  Back"), .count = pick_count, .item = pick_item, .select = pick_select,
	.key = pick_key, .enter = pick_enter, .title = pick_title,
};

/* --- While it runs --- */

bool ui_xband_lock(const input_event_t *ev)
{
	if (!xband_on())
		return false;
	if (ev->gesture != GESTURE_PRESS && ev->gesture != GESTURE_LONG)
		return true;	/* the rest of a press, a repeat, the knob: nothing */
	if (ev->action == ACT_BACK) {
		xband_stop();
		ui_go_home();
		ui_toast(TR("Cross-band off"));
	} else {
		ui_toast(TR("Cross-band on: Back stops"));
	}
	return true;
}

const char *ui_xband_home(const char *bottom)
{
	static char line[24];	/* "Record A 0:12 !  Back" */
	if (!xband_on())
		return bottom;
	xband_view_t v;
	xband_view(&v);
	sbuf_t b;
	sb_init(&b, line, sizeof line);
	static const char *const words[] = { TRN("Starting"), TRN("Listen"), TRN("Record"), TRN("Send"), "CW ID" };
	sb_str(&b, v.phase < 5u ? tr(words[v.phase]) : "");
	sb_chr(&b, ' ');
	sb_chr(&b, v.side ? 'B' : 'A');
	if (v.phase == XB_P_RECORD || v.phase == XB_P_SEND) {
		sb_chr(&b, ' ');
		sb_mmss(&b, (uint32_t)v.frames * XB_FRAME_MS / 1000u);
	}
	if (v.cut)
		sb_str(&b, " !");
	sb_str(&b, "  ");
	sb_str(&b, TR("Back"));	/* the start's toast said: Back stops */
	return line;
}

bool ui_xband_tick(uint8_t top, uint32_t now)
{
	(void)now;
	xband_view_t v;
	xband_view(&v);
	bool on = xband_on();
	if (was_on && !on) {
		/* it ended by itself (the battery, no memory) or by the stop key: say so once */
		static const char *const why[] = { NULL, NULL, TRN("Cross-band off: battery low"),
						   TRN("Cross-band off: no memory"), TRN("Cross-band off: refused") };
		if (v.ended < sizeof why / sizeof why[0] && why[v.ended] != NULL)
			ui_toast(tr(why[v.ended]));
	}
	bool moved = v.serial != shown_serial || (was_on != 0) != on;	/* Home's strip and bottom line follow */
	was_on = on;
	shown_serial = v.serial;
	return moved && top == SCR_HOME;
}
#endif
