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
#include "i18n.h"
#include "board.h"
#include "radio/radio.h"
#include "radio/radio_cwid.h"
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

/* One frequency: the band plan (xband_check_hz), then the repeater list's */
static uint8_t hz_why(uint32_t hz)
{
	uint8_t why = xband_check_hz(hz);
#if CONFIG_REPEATERS
	if (why == XB_R_OK && known_repeater(hz))
		why = XB_R_REPEATER;
#endif
	return why;
}

const char *ui_xband_refusal(uint32_t hz)
{
	return why_text(hz_why(hz));
}
#endif

#if CONFIG_XBAND
enum { R_A, R_AM, R_B, R_BM, R_CC, R_TS, R_POWER, R_GO, R_ROWS };
#define FREQ_ROW(r)	((r) == R_A || (r) == R_B)
#define SIDE(r)		((r) >= R_B ? 1u : 0u)	/* A / B of a frequency or mode row */

static uint8_t focus, ndigits, page;	/* page 1: the attended-operation question */
static char digits[8];			/* a frequency being typed on A / B */
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
		why = hz_why(r->hz[k]);	/* the repeater list's (the band plan's passed in xband_check) */
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

static void row_text(uint8_t r, bool f, sbuf_t *b, const char **label)
{
	static const char *const labels[R_ROWS] = { "A", TRN("A mode"), "B", TRN("B mode"), TRN("Color code"), TRN("Slot"),
						    TRN("Power"), TRN("Start") };
	static const char *const modes[XB_MODES] = { "DMR", "FM", "P25", "NXDN" };
	const xband_req_t *q = xband_req();
	*label = FREQ_ROW(r) ? labels[r] : tr(labels[r]);
	switch (r) {
	case R_A:
	case R_B:
		if (f && ndigits != 0)
			sb_str(b, digits);
		else if (q->hz[SIDE(r)] != 0)
			sb_freq(b, q->hz[SIDE(r)]);
		else
			sb_str(b, "---");
		break;
	case R_AM:
	case R_BM: sb_str(b, modes[q->mode[SIDE(r)] < XB_MODES ? q->mode[SIDE(r)] : 0]); break;
	case R_CC: sb_u32(b, q->cc, 1); break;
	case R_TS: sb_u32(b, q->ts, 1); break;
	case R_POWER: sb_str(b, q->power <= 1u ? TR("Low") : q->power == 2u ? TR("Mid") : TR("High")); break;
	default: break;
	}
}

static void form_item(uint16_t i, ui_item_t *it)
{
	const char *label;
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	row_text((uint8_t)i, i == focus, &b, &label);
	str_copy(it->label, sizeof it->label, label);
}

static void xband_draw(void)
{
	ui_list_t l;
	memset(&l, 0, sizeof l);
	l.prefix = TR("Cross-band");
	if (page == 1) {
		l.body = TR("Attended only: stay at the radio while it relays. Start?");
		l.hint = TR("OK yes  Back no");
	} else {
		l.count = R_ROWS;
		l.focus = focus;
		l.top = &form_top;
		l.item = form_item;
		l.hint = TR("OK pick  <> change");
	}
	ui_draw_list(ui_view(), &ui_st, &l);
}

static void step(uint8_t r, int dir)
{
	xband_req_t *q = xband_req();
	switch (r) {
	case R_A:
	case R_B: q->hz[SIDE(r)] = (uint32_t)((int32_t)q->hz[SIDE(r)] + dir * 12500); break;
	case R_AM:
	case R_BM: q->mode[SIDE(r)] = (uint8_t)((q->mode[SIDE(r)] + XB_MODES + (unsigned)dir) % XB_MODES); break;
	case R_CC: q->cc = (uint8_t)((q->cc + 16 + dir) % 16); break;
	case R_TS: q->ts = (uint8_t)(3 - q->ts); break;
	case R_POWER: q->power = (uint8_t)((q->power + 2u + (unsigned)dir) % 3u + 1u); break;
	default: break;
	}
}

/* A typed frequency in MHz, its first three digits the MHz (every band the relay may use has three): 433 is 433.000,
 * 4335 and 43350 433.500, 145500 145.500, 4335125 433.5125. Fewer than three digits: told, the frequency kept. (Before
 * 2026-11-25 only six or seven digits counted and any other entry was dropped without a word, so the row went back to
 * an older frequency, maybe one refused, while the user believed the typed one was set) */
static void typed(uint8_t r)
{
	uint32_t n = 0;
	for (uint8_t i = 0; i < ndigits; i++)
		n = n * 10u + (uint32_t)(digits[i] - '0');
	if (FREQ_ROW(r) && ndigits >= 3u) {
		for (uint8_t i = ndigits; i < 9u; i++)
			n *= 10u;	/* (7 digits at most: below 10^9) */
		xband_req()->hz[SIDE(r)] = n;
	} else if (FREQ_ROW(r) && ndigits != 0) {
		ui_toast(TR("Type the MHz: 433500"));
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
	if (press && (act == ACT_NAV_UP || act == ACT_NAV_DOWN)) {
		if (ndigits)
			typed(focus);
		if (act == ACT_NAV_UP && focus > 0)
			focus--;
		else if (act == ACT_NAV_DOWN && focus + 1u < R_ROWS)
			focus++;
	} else if ((press && (act == ACT_NAV_LEFT || act == ACT_NAV_RIGHT)) || (act == ACT_ENC_STEP && ev->value != 0)) {
		step(focus, act == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : act == ACT_NAV_RIGHT ? 1 : -1);
	} else if (ev->gesture == GESTURE_PRESS && act >= ACT_DIGIT_0 && act <= ACT_DIGIT_9 && FREQ_ROW(focus)) {
		if (ndigits < 7u)
			digits[ndigits++] = (char)('0' + act - ACT_DIGIT_0);
		digits[ndigits] = '\0';
	} else if (ev->gesture != GESTURE_PRESS) {
		return false;
	} else if (act == ACT_BACK) {
		if (!ndigits)
			return false;	/* leaves */
		ndigits = 0;
	} else if (act != ACT_SELECT) {
		return false;
	} else if (ndigits) {
		typed(focus);
	} else if (FREQ_ROW(focus)) {
		ui_push(SCR_XBAND_PICK, SIDE(focus));
	} else if (focus == R_GO) {
		if (checks())
			page = 1;	/* the question */
	} else {
		step(focus, 1);
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
	q->hz[side] = ch->rx_hz;
	q->mode[side] = mode;
	if (ch->mode == RADIO_MODE_DMR) {
		q->cc = (uint8_t)(ch->color_code & 15u);	/* a DMR channel brings its colour code and slot */
		q->ts = ch->timeslot == 2 ? 2 : 1;
	}
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
