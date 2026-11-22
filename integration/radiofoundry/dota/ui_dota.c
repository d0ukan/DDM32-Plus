/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus Tools > DOTA Transfer (docs/dota.md 12): one screen whose pages are the form (role and method first; the
 * sender's or the receiver's rows below, scrolled), its questions, and the transfer (progress, result, the PTT
 * prompt), each drawn as a list (ui_draw_list); and the picker for a channel, channels ticked in any zone (Left /
 * Right: another zone) or a zone. Up / Down pick a row, Left / Right (or the knob) change it, digits type the numbers,
 * OK acts, Back goes back (during a transfer: "Stop the transfer?"). The form is RAM only: it comes back as it was
 * until power off. The session itself is radio/dota.c. Installed as core/ui/ui_dota.c.
 * CONFIG_DATAPACK (docs/dota.md 20): What also offers the RadioID and the air band pack in force (the row under it names
 * the pack, greyed "none" without one); the receiver's waiting page asks "Data pack: RadioID TR, 2026-11-17, 98 KB.
 * Accept?" when a sender offers one (OK yes, Back no). */
#include "config.h"
#if CONFIG_DOTA
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_svc.h"
#include "fmt.h"
#include "i18n.h"
#include "board.h"
#include "radio/radio.h"
#include "radio/dota.h"
#include "radio/xband.h"
#include "aprs/aprs.h"
#include "storage/storage.h"
#include "ui/ui_dota.h"
#include "ui/ui_xband.h"

enum { PG_FORM, PG_RUN };
enum { R_ROLE, R_METHOD, R_WHAT, R_ITEM, R_FSRC, R_FREQ, R_TONE, R_ID, R_POWER, R_CODE, R_GO, R_IMPORT, R_ZNAME,
       R_TXOK };
/* Where the frequency comes from (both roles the same, so two radios pair without typing; docs/dota.md 12.6) */
enum { FS_DEFAULT, FS_CHANNEL, FS_LAST, FS_MANUAL, FS_COUNT };
/* The FM tone's short list (the first is DOTA_CTCSS_DEFAULT) */
static const uint16_t tone_list[] = { DOTA_CTCSS_DEFAULT, 1000, 1230, 1318, 1622 };
/* Questions before a start, asked in this order, and the transfer's own (outside the amateur bands: refused, no
 * question; owner, 2026-11-21) */
#define ASK_BATT	0x01u
#define ASK_READY	0x02u
#define ASK_STOP	0x08u

/* DMR: colour code 1, slot 1 always (not on any DOTA screen) */
static dota_req_t rq = { .method = DOTA_FM, .hz = DOTA_DEFAULT_HZ, .ctcss = DOTA_CTCSS_DEFAULT, .cc = 1, .ts = 1,
			 .code = 10000 };
static uint8_t pg, focus, asks, ndigits, fsrc;
static uint32_t last_hz;	/* the last session's frequency (until power off) */
static char digits[9];		/* a number being typed on the focused row */
static uint32_t shown_serial;
static uint16_t est_s, est_ch;	/* the sender's estimate, made when the form changes */

/* What (R_WHAT): the kinds, then (CONFIG_DATAPACK) the two pack types */
#define WHATS	(4u + 2u * CONFIG_DATAPACK)
static uint8_t what_i(void)
{
#if CONFIG_DATAPACK
	if (rq.kind == DOTA_KIND_PACK)
		return (uint8_t)(3u + rq.pack);	/* DPK_RADIOID 4, DPK_AIR 5 */
#endif
	return rq.kind;
}


static bool sending(void)
{
	return rq.role == DOTA_SEND;
}

static bool dmr(void)
{
	return rq.method == DOTA_DMR;
}

static bool sel_bit(uint16_t n)
{
	return n >= 1u && n <= CODEPLUG_CHANNELS_MAX && ((dota_selection()[(n - 1u) >> 3] >> ((n - 1u) & 7u)) & 1u);
}

/* The rows of the page now; focus stays on one of them */
static uint8_t rows(uint8_t *r)
{
	uint8_t n = 2;
	r[0] = R_ROLE;
	r[1] = R_METHOD;
	if (sending()) {
		r[n++] = R_WHAT;
		if (rq.kind != DOTA_KIND_CLONE)
			r[n++] = R_ITEM;
	}
	r[n++] = R_FSRC;
	r[n++] = R_FREQ;
	if (!dmr())
		r[n++] = R_TONE;
	if (sending()) {
		if (dmr())
			r[n++] = R_ID;
		r[n++] = R_POWER;
		r[n++] = R_CODE;
	} else {
		r[n++] = R_IMPORT;
		if (rq.imp.mode == DOTA_IMP_NEW)
			r[n++] = R_ZNAME;
		r[n++] = R_TXOK;
	}
	r[n++] = R_GO;
	if (focus >= n)
		focus = (uint8_t)(n - 1u);	/* the rows changed under it */
	return n;
}

/* A frequency source's frequency now, 0 when it has none: the current channel only when it is simplex, may transmit
 * and the band plan allows it (ui_xband_refusal: amateur, no repeater, satellite or APRS frequency) */
static uint32_t src_hz(uint8_t s)
{
	const channel_t *c = &ui_view()->ch;
	if (s == FS_DEFAULT)
		return DOTA_DEFAULT_HZ;
	if (s == FS_CHANNEL)	/* not in dual / multi-watch: the view's channel is the letter the ring is on now */
		return ui_view()->scan != 3 /* SCAN_DUAL */ && c->rx_hz == c->tx_hz && !c->rx_only && ui_xband_refusal(c->rx_hz) == NULL ? c->rx_hz : 0;
	return s == FS_LAST ? last_hz : rq.hz;
}

/* The sender's estimate: how many channels go, about how long (docs/dota.md 8) */
static void estimate(void)
{
	uint16_t ch = 1;
	uint8_t zones = 0;
	zone_t *z = dota_scratch();
#if CONFIG_DATAPACK
	if (rq.kind == DOTA_KIND_PACK) {
		/* a data pack: its bytes at the steady rate, HELLO and END (no commit to speak of) */
		const dpk_info_t *pk = dpk_active(rq.pack);
		est_ch = 0;
		est_s = (uint16_t)(6u + (pk != NULL ? pk->len : 0u) / (dmr() ? 124u : 93u));
		return;
	}
#endif
	if (rq.kind == DOTA_KIND_SELECTED) {
		ch = 0;
		for (uint16_t n = 1; n <= CODEPLUG_CHANNELS_MAX; n++)
			ch = (uint16_t)(ch + sel_bit(n));
	} else if (rq.kind == DOTA_KIND_ZONE) {
		ch = z != NULL && codeplug_zone(rq.zone, z) ? z->count : 0;
	} else if (rq.kind == DOTA_KIND_CLONE) {
		ch = codeplug_channel_count();
		zones = (uint8_t)codeplug_zone_count();
	}
	est_ch = ch;
	/* docs/dota.md 8: 22 B a channel, 20 a zone; about 93 B/s on FM, 124 on DMR; HELLO, END and the commit */
	est_s = (uint16_t)(6u + (22u * ch + 20u * zones) / (dmr() ? 124u : 93u));
}

/* --- Drawing: every page a list (ui_draw_list): the title, a line under it, a wrapped text, the rows --- */

static uint16_t form_top;	/* the form's first row shown */
static dota_view_t rv;		/* the transfer as the run page draws it */

static void row_text(uint8_t r, bool f, sbuf_t *b, const char **label)
{
	static const char *const what[] = { TRN("One channel"), TRN("Selected channels"), TRN("A zone"),
					    TRN("DOTA Clone (all)"),
#if CONFIG_DATAPACK
					    TRN("RadioID pack"), TRN("Air band pack"),
#endif
	};
	/* By R_*; R_ITEM and R_GO name themselves below */
	static const char *const labels[] = { TRN("Role"), TRN("Method"), TRN("What"), "", TRN("Frequency"), "MHz",
					      TRN("Tone"), TRN("Receiver ID"), TRN("Power"), TRN("Code"), "", TRN("Import"),
					      TRN("Zone name"), TRN("May transmit") };
	static const char *const srcs[] = { TRN("Default"), TRN("Current channel"), TRN("Last used"), TRN("Manual") };
	bool typing = f && ndigits != 0;
	*label = tr(labels[r]);
	switch (r) {
	case R_ROLE: sb_str(b, sending() ? TR("Send") : TR("Receive")); break;
	case R_METHOD: sb_str(b, dmr() ? "DMR" : "FM"); break;
	case R_WHAT: sb_str(b, tr(what[what_i()])); break;
	case R_ITEM:
		*label = tr(what[what_i()]);
		if (rq.kind == DOTA_KIND_ZONE) {
			zone_t *z = dota_scratch();
			if (z != NULL && codeplug_zone(rq.zone, z))
				sb_str(b, z->name);
		} else {
			sb_u32(b, rq.kind == DOTA_KIND_SELECTED ? est_ch : rq.channel, 1);
		}
		break;
	case R_FSRC: sb_str(b, tr(srcs[fsrc])); break;
	case R_FREQ: typing ? sb_str(b, digits) : sb_freq(b, rq.hz); break;
	case R_TONE: {
		subtone_t t = { TONE_CTCSS, rq.ctcss };
		sb_tone(b, &t);
		break;
	}
	case R_ID: typing ? sb_str(b, digits) : sb_u32(b, rq.peer_id, 1); break;
	case R_POWER:
		sb_str(b, rq.power == 0 ? TR("Low") : rq.power == 1 ? TR("Mid") : TR("High"));
		break;
	case R_CODE: typing ? sb_str(b, digits) : rq.code < 10000u ? sb_u32(b, rq.code, 4) : sb_str(b, "____"); break;
	case R_IMPORT: sb_str(b, rq.imp.mode == DOTA_IMP_NEW ? TR("New zone") : TR("Current zone")); break;
	case R_ZNAME: sb_str(b, rq.imp.name); break;
	case R_TXOK: sb_str(b, rq.imp.tx_ok ? TR("On") : TR("Off")); break;
	case R_GO: *label = sending() ? TR("Send") : TR("Ready"); break;
	default: break;
	}
}

#if CONFIG_DATAPACK
/* The pack row: its name, then its date and size, the date alone or the size alone where they don't fit (the size is
 * in the estimate above too); greyed "none" without one */
static void __attribute__((noinline)) pack_row(ui_item_t *it, const char *what)
{
	dpk_hdr_t h;
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	if (!dpk_header(rq.pack, &h)) {
		str_copy(it->label, sizeof it->label, what);
		sb_str(&b, TR("none"));
		it->flags |= UI_ITEM_DIM;	/* nothing to send */
		return;
	}
	char d[40];
	dpk_describe(&h, d, sizeof d);	/* "RadioID TR, 2026-11-17, 98 KB" */
	const char *size = strrchr(d, ',') + 2, *date = size - 12;
	memcpy(it->label, h.name, sizeof h.name);
	it->label[sizeof h.name] = '\0';
	sb_str(&b, TEXT_ALT_OPEN);
	sb_str(&b, date);		/* "2026-11-17, 98 KB" */
	sb_str(&b, TEXT_ALT_NEXT);
	for (unsigned i = 0; i < 10u; i++)
		sb_chr(&b, date[i]);	/* "2026-11-17" */
	sb_str(&b, TEXT_ALT_NEXT);
	sb_str(&b, size);		/* "98 KB" */
	sb_str(&b, TEXT_ALT_CLOSE);
}
#endif

static void form_item(uint16_t i, ui_item_t *it)
{
	uint8_t r[12];
	const char *label;
	sbuf_t b;
	rows(r);
	sb_init(&b, it->value, sizeof it->value);
	row_text(r[i], i == focus, &b, &label);
#if CONFIG_DATAPACK
	if (r[i] == R_ITEM && rq.kind == DOTA_KIND_PACK) {
		pack_row(it, label);	/* the pack's name, date and size */
		return;
	}
#endif
	str_copy(it->label, sizeof it->label, label);
}

/* The first question still open (by its ASK_* bit; the form never holds ASK_STOP) */
static const char *question(void)
{
	static const char *const q[] = { TRN("Battery low. Sure?"), TRN("Receiver ready?") };
	unsigned i = (unsigned)__builtin_ctz(asks);
	return tr(q[i < 2u ? i : 1u]);
}

/* n and its noun, singular or plural ("1 channel", "2 channels") */
static void count(sbuf_t *b, uint32_t n, const char *one, const char *many)
{
	sb_u32(b, n, 1);
	sb_str(b, tr(n == 1u ? one : many));
}

static void result_text(const dota_view_t *v, sbuf_t *b)
{
	/* By DOTA_RES_*; NULL: the refusal's reason, or the other side's stop. In "Sent ^" / "Imported ^" the ^ stands for
	 * what went ("12 channels, 1 zone"), where the language puts it */
	static const char *const text[] = {
		TRN("Nothing changed"), TRN("Sent ^"), TRN("Imported ^"), TRN("No answer"),
		NULL, TRN("Nothing changed"), NULL, TRN("No final answer"),
		TRN("Nothing changed"), TRN("Nothing changed"), TRN("Flash write failed"),
		TRN("Channel busy"), TRN("Nothing to send"),
	};
	const char *t = v->result < sizeof text / sizeof text[0] ? text[v->result] : text[0];
	if (v->result == DOTA_RES_REFUSED)
		t = v->why == DOTA_R_VERSION ? TRN("Other version") :
		    v->why == DOTA_R_NOT_READY || v->why == DOTA_R_MODE ? TRN("Not ready") :
		    v->why == DOTA_R_WAIT || v->why == DOTA_R_DECLINED ? TRN("Not accepted") : TRN("Codeplug full");
	else if (v->result == DOTA_RES_PEER_STOPPED)
		t = sending() ? TRN("Receiver stopped") : text[0];
	char what[24];	/* "1024 channels, 68 zones" */
	sbuf_t w;
	sb_init(&w, what, sizeof what);
	if (v->pack)
		sb_str(&w, TR("data pack"));	/* "Sent data pack", "Imported data pack" */
	else
		count(&w, v->ch_added, TRN(" channel"), TRN(" channels"));
	if (v->zones_added != 0) {
		sb_str(&w, ", ");
		count(&w, v->zones_added, TRN(" zone"), TRN(" zones"));
	}
	for (t = tr(t); *t != '\0'; t++)
		*t == '^' ? sb_str(b, what) : sb_chr(b, *t);
}

/* The run page's rows: the receiver's code (and DMR ID) while it waits; else the other side and the progress */
static void run_item(uint16_t i, ui_item_t *it)
{
	sbuf_t b, l;
	it->flags = UI_ITEM_INFO;
	sb_init(&b, it->value, sizeof it->value);
	sb_init(&l, it->label, sizeof it->label);
	if (rv.phase == DOTA_P_WAIT) {
		sb_str(&l, i == 0 ? TR("Code") : "ID");
		sb_u32(&b, i == 0 ? rv.code : ui_view()->dmr.own_id, i == 0 ? 4 : 1);
	} else if (i == 0) {
		sb_str(&l, rv.peer);
		sb_u32(&b, rv.pct, 1);
		sb_str(&b, " %");
	} else if (rv.pack) {
		/* a data pack: "12/98 KB" (its 120-byte frames) */
		uint32_t kb = ((uint32_t)rv.channels * 120u + 1023u) / 1024u;
		sb_u32(&l, kb * rv.pct / 100u, 1);
		sb_chr(&l, '/');
		sb_u32(&l, kb, 1);
		sb_str(&l, " KB");
	} else {
		sb_u32(&l, (uint32_t)rv.channels * rv.pct / 100u, 1);
		sb_chr(&l, '/');
		count(&l, rv.channels, TRN(" channel"), TRN(" channels"));
	}
}

#if CONFIG_DATAPACK
/* The receiver's question: "Data pack: RadioID TR, 2026-11-17, 98 KB. Accept?" (false: none now) */
static bool __attribute__((noinline)) pack_question(sbuf_t *b)
{
	dpk_hdr_t h;
	char d[40];
	if (!dota_pack_offer(&h))
		return false;
	dpk_describe(&h, d, sizeof d);
	for (const char *t = TR("Data pack: ^. Accept?"); *t != '\0'; t++)
		*t == '^' ? sb_str(b, d) : sb_chr(b, *t);
	return true;
}
#endif

static void dota_draw(void)
{
	char head[24], body[64];
	uint8_t r[12];
	sbuf_t b;
	ui_list_t l;
	memset(&l, 0, sizeof l);
	l.prefix = TR("DOTA Transfer");
	l.hint = TR("OK yes  Back no");
	sb_init(&b, head, sizeof head);
	if (pg == PG_RUN) {
		dota_view(&rv);
		bool ended = !dota_on() || rv.phase == DOTA_P_DONE || rv.phase == DOTA_P_FAILED;
		l.prefix = sending() ? TR("Send") : TR("Receive");
		l.name = dmr() ? "DMR" : "FM";
		sb_freq(&b, rq.hz);
		if (!dmr()) {
			subtone_t t = { TONE_CTCSS, rq.ctcss };
			sb_chr(&b, ' ');
			sb_tone(&b, &t);
		}
		l.header = head;
		l.item = run_item;
		l.count = ended ? 0u : rv.phase != DOTA_P_WAIT || dmr() ? 2u : 1u;
		sb_init(&b, body, sizeof body);
		if ((asks & ASK_STOP) || rv.ptt_prompt) {
			sb_str(&b, TR("Stop the transfer?"));
#if CONFIG_DATAPACK
		} else if (!sending() && rv.asking && !ended && pack_question(&b)) {
			/* the hint asks OK yes / Back no */
#endif
		} else if (ended) {
			result_text(&rv, &b);
			l.hint = TR("Back");
		} else {
			sb_str(&b, rv.phase == DOTA_P_WAIT ? TR("Waiting for sender") : rv.phase == DOTA_P_PREP ? TR("Preparing") :
				   rv.phase == DOTA_P_WRITING ? TR("Writing") : rv.keyed ? TR("On air") :
				   rv.asking ? TR("Waiting for answer") : "");
			l.hint = rv.phase == DOTA_P_WRITING ? "" : TR("Back stop");
		}
		l.body = body;
	} else if (asks != 0) {
		l.body = question();
	} else {
		if (sending()) {
			sb_str(&b, TR("About "));
			sb_mmss(&b, est_s);
			l.header = head;
		}
		l.count = rows(r);
		l.focus = focus;
		l.top = &form_top;
		l.item = form_item;
		l.hint = TR("OK pick  <> change");
	}
	ui_draw_list(ui_view(), &ui_st, &l);
}

/* --- Keys --- */

static void step(uint8_t r, int dir)
{
	switch (r) {
	case R_ROLE: rq.role = !rq.role; break;
	case R_METHOD: rq.method = !rq.method; break;
	case R_WHAT: {
		uint8_t w = (uint8_t)((what_i() + WHATS + (unsigned)dir) % WHATS);
		rq.kind = w < 4u ? w : DOTA_KIND_PACK;
		rq.pack = (uint8_t)(w < 4u ? 0u : w - 3u);
		break;
	}
	case R_TONE: {
		unsigned i = 0, n = sizeof tone_list / sizeof tone_list[0];
		while (i < n && tone_list[i] != rq.ctcss)
			i++;
		rq.ctcss = tone_list[(i + n + (unsigned)dir) % n];
		break;
	}
	case R_FSRC: {
		/* the next source that has a frequency now (Manual always has: the one shown) */
		uint8_t s = fsrc;
		do
			s = (uint8_t)((s + FS_COUNT + dir) % FS_COUNT);
		while (src_hz(s) == 0);
		fsrc = s;
		rq.hz = src_hz(s);
		break;
	}
	case R_POWER: rq.power = (uint8_t)((rq.power + 3 + dir) % 3); break;
	case R_IMPORT: rq.imp.mode = !rq.imp.mode; break;
	case R_TXOK: rq.imp.tx_ok = !rq.imp.tx_ok; break;
	case R_FREQ: {
		/* within the amateur bands only: a step out of them is refused (the segments refused inside them are passed
		 * over; Send / Ready names them) */
		uint32_t hz = (uint32_t)((int32_t)rq.hz + dir * 12500);
		uint8_t why = xband_check_hz(hz);
		if (why == XB_R_BAND || why == XB_R_PMR446) {
			ui_toast(ui_xband_refusal(hz));
			break;
		}
		rq.hz = hz;
		fsrc = FS_MANUAL;
		break;
	}
	default: break;
	}
	estimate();
}

/* The digits typed on the focused row: 433500 / 4335125 for a frequency (one the band plan refuses: its toast, the
 * frequency stays), the ID, the four-digit code */
static void typed(uint8_t r)
{
	uint32_t n = 0;
	for (uint8_t i = 0; i < ndigits; i++)
		n = n * 10u + (uint32_t)(digits[i] - '0');
	if (r == R_FREQ && (ndigits == 6 || ndigits == 7)) {
		n *= ndigits == 6 ? 1000u : 100u;
		const char *why = ui_xband_refusal(n);
		if (why != NULL) {
			ui_toast(why);
		} else {
			rq.hz = n;
			fsrc = FS_MANUAL;
		}
	} else if (r == R_ID)
		rq.peer_id = n <= 16776415u ? n : 0;
	else if (r == R_CODE)
		rq.code = ndigits == 4 ? (uint16_t)n : 10000u;
	ndigits = 0;
}

/* OK on Send / Ready: the checks, then the questions (or the start) */
static void begin(void)
{
	const radio_view_t *v = ui_view();
	char call[CODEPLUG_NAME_LEN + 1];
	const char *why = NULL, *plan = ui_xband_refusal(rq.hz);
	aprs_cfg_t c;
	codeplug_radio_name(call);
	call[AX25_CALL_LEN] = '\0';
	aprs_get_cfg(&c);
	if (!aprs_callsign_ok(call))
		why = TR("Set your callsign (the radio name) in qdmr.");
	else if (dmr() && v->dmr.own_id == 0)
		why = TR("Set your DMR ID first");
	else if (plan != NULL)
		why = plan;	/* the band plan's toast (the cross-band's): any source, Left / Right too */
	else if (!board_in_band(rq.hz, true))
		why = TR("Can't transmit there");
	else if (rq.hz == aprs_cfg_freq(&c))
		why = TR("APRS frequency");
	else if (sending() && (rq.code >= 10000u || (dmr() && rq.peer_id == 0)))
		why = TR("Code / ID needed");
#if CONFIG_DATAPACK
	else if (sending() && rq.kind == DOTA_KIND_PACK && dpk_active(rq.pack) == NULL)
		why = TR("Nothing to send");
#endif
	else if (!sending() && rq.imp.mode == DOTA_IMP_NEW && rq.imp.name[0] == '\0')
		why = TR("Zone name needed");
	if (why != NULL) {
		ui_toast(why);
		return;
	}
	strcpy(rq.call, call);
	asks = (uint8_t)((ui_st.battery_pct != 0 && ui_st.battery_pct < DOTA_LOW_BATTERY_PCT ? ASK_BATT : 0u) |
			 (sending() ? ASK_READY : 0u));
	if (asks == 0)
		asks = 0x80u;	/* no question: start at once */
}

static void go(void)
{
	const radio_view_t *v = ui_view();
	if (!sending()) {
		rq.code = (uint16_t)((ui_st.now_ms * 2654435761u ^ v->battery_mv * 40503u) % 10000u);
		rq.imp.zone = v->zone_index;
	}
	if (dota_start(&rq)) {
		last_hz = rq.hz;
		pg = PG_RUN;
		shown_serial = 0;
	}
}

static bool run_key(const input_event_t *ev)
{
	dota_view_t v;
	dota_view(&v);
	bool live = dota_on() && v.phase != DOTA_P_DONE && v.phase != DOTA_P_FAILED;
	if (ev->gesture != GESTURE_PRESS)
		return ev->action != ACT_BACK;
#if CONFIG_DATAPACK
	if (!sending() && v.asking && live && !(asks & ASK_STOP) && !v.ptt_prompt &&
	    (ev->action == ACT_SELECT || ev->action == ACT_BACK)) {
		dota_pack_answer(ev->action == ACT_SELECT);	/* the sender's next HELLO is taken or refused */
		ui_dirty();
		return true;
	}
#endif
	if ((asks & ASK_STOP) || v.ptt_prompt) {
		if (ev->action == ACT_SELECT)
			dota_stop();
		else if (ev->action == ACT_BACK)
			dota_ptt_prompt_clear();
		else
			return true;
		asks = 0;
	} else if (ev->action == ACT_BACK) {
		if (!live) {
			pg = PG_FORM;
			return false;	/* leaves */
		}
		if (v.phase != DOTA_P_WRITING)
			asks = ASK_STOP;
	}
	ui_dirty();
	return true;
}

static bool dota_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	uint8_t act = ev->action;
	if (pg == PG_RUN)
		return run_key(ev);
	if (asks != 0) {
		if (ev->gesture == GESTURE_PRESS && act == ACT_SELECT) {
			asks &= (uint8_t)(asks - 1u);	/* the next question */
			if (asks == 0)
				go();
		} else if (ev->gesture == GESTURE_PRESS && act == ACT_BACK) {
			asks = 0;
		}
		ui_dirty();
		return true;
	}
	uint8_t list[12], n = rows(list), r = list[focus];
	if (press && (act == ACT_NAV_UP || act == ACT_NAV_DOWN)) {
		if (ndigits)
			typed(r);
		if (act == ACT_NAV_UP && focus > 0)
			focus--;
		else if (act == ACT_NAV_DOWN && focus + 1u < n)
			focus++;
	} else if ((press && (act == ACT_NAV_LEFT || act == ACT_NAV_RIGHT)) || (act == ACT_ENC_STEP && ev->value != 0)) {
		step(r, act == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : act == ACT_NAV_RIGHT ? 1 : -1);
	} else if (ev->gesture == GESTURE_PRESS && act >= ACT_DIGIT_0 && act <= ACT_DIGIT_9 &&
		   (r == R_FREQ || r == R_ID || r == R_CODE)) {
		if (ndigits < (r == R_CODE ? 4u : 8u))
			digits[ndigits++] = (char)('0' + act - ACT_DIGIT_0);
		digits[ndigits] = '\0';
	} else if (ev->gesture != GESTURE_PRESS) {
		return false;
	} else if (act == ACT_BACK) {
		if (ndigits)
			ndigits = 0;
		else
			return false;	/* leaves */
	} else if (act != ACT_SELECT) {
		return false;
	} else if (ndigits) {
		typed(r);
	} else if (r == R_ITEM && rq.kind != DOTA_KIND_PACK) {	/* a pack: no picker */
		ui_push(SCR_DOTA_PICK, rq.kind == DOTA_KIND_ZONE ? 2u : rq.kind == DOTA_KIND_SELECTED ? 1u : 0u);
	} else if (r == R_ZNAME) {
		ui_text_edit(rq.imp.name, CODEPLUG_NAME_LEN, ui_dirty, 1);
	} else if (r == R_GO) {
		begin();
		if (asks == 0x80u) {
			asks = 0;
			go();
		}
	} else {
		step(r, 1);
	}
	ui_dirty();
	return true;
}

static void dota_enter(void)
{
	const radio_view_t *v = ui_view();
	pg = dota_on() ? PG_RUN : PG_FORM;	/* back to a transfer that runs */
	if (rq.channel == 0)
		rq.channel = v->channel_number;
	estimate();
	ui_dirty();
}

/* The UI's tick: the storage jobs a receiver asks for, a redraw when the transfer moved */
bool ui_dota_tick(uint8_t top, uint32_t now)
{
	(void)now;
	if (dota_job_wanted() && !storage_job_busy())
		(void)storage_job_post(dota_job, NULL);
	if (top != SCR_DOTA || pg != PG_RUN)
		return false;
	dota_view_t v;
	dota_view(&v);
	if (v.serial == shown_serial)
		return false;
	shown_serial = v.serial;
	return true;
}

const ui_scr_def_t ui_dota_screen = {
	.name = "dota", .flags = SF_FIELD, .key = dota_key, .draw = dota_draw, .enter = dota_enter,
};

/* --- The picker: a channel of a zone (arg 0), channels ticked in any zone (1), a zone (2) --- */

static uint16_t pick_zone;

static zone_t *pick_z(void)
{
	zone_t *z = dota_scratch();
	return z != NULL && codeplug_zone(ui_frame()->arg == 2u ? 0u : pick_zone, z) ? z : NULL;
}

static uint16_t pick_count(void)
{
	zone_t *z = pick_z();
	return ui_frame()->arg == 2u ? codeplug_zone_count() : z != NULL ? z->count : 0;
}

static void pick_item(uint16_t i, ui_item_t *it)
{
	zone_t *z = dota_scratch();
	channel_t *ch = (channel_t *)((uint8_t *)z + sizeof(zone_t));
	sbuf_t b;
	bool on;
	sb_init(&b, it->label, sizeof it->label);
	if (ui_frame()->arg == 2u) {
		if (z == NULL || !codeplug_zone(i, z))
			return;
		sb_str(&b, z->name);
		on = i == rq.zone;
	} else {
		if ((z = pick_z()) == NULL || i >= z->count)
			return;
		uint16_t n = z->channels[i];
		sb_u32(&b, n, 1);
		sb_chr(&b, ' ');
		if (codeplug_channel(n, ch))
			sb_str(&b, ch->name);
		on = ui_frame()->arg == 1u ? sel_bit(n) : n == rq.channel;
	}
	strcpy(it->value, on ? "*" : "");
}

static void pick_select(uint16_t i, bool hold)
{
	(void)hold;
	zone_t *z = pick_z();
	if (ui_frame()->arg == 2u) {
		rq.zone = i;
	} else if (z != NULL && i < z->count) {
		uint16_t n = z->channels[i];
		if (ui_frame()->arg == 0u)
			rq.channel = n;
		else if (n >= 1u && n <= CODEPLUG_CHANNELS_MAX)
			dota_selection()[(n - 1u) >> 3] ^= (uint8_t)(1u << ((n - 1u) & 7u));
	}
	estimate();
	if (ui_frame()->arg != 1u)
		ui_pop();
	ui_dirty();
}

static bool pick_key(const input_event_t *ev)
{
	uint16_t n = codeplug_zone_count();
	/* Left / Right: another zone's channels */
	if (ui_frame()->arg == 2u || n == 0 || ev->gesture != GESTURE_PRESS ||
	    (ev->action != ACT_NAV_LEFT && ev->action != ACT_NAV_RIGHT))
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

const ui_scr_def_t ui_dota_pick_screen = {
	.name = "dota-pick", .flags = SF_LIST | SF_FIELD, .count = pick_count, .item = pick_item, .select = pick_select,
	.key = pick_key, .enter = pick_enter,
};
#endif
