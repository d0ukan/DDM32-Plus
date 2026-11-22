/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus Tools > Heard list (heard.h, docs/plus.md "Heard list"): every station heard, newest first, one row each:
 * "P25 77285" and "TG101 $659/1625 x12 3m" (the talkgroup, NAC, calls, how long ago; the NAC, then the talkgroup left out
 * where they do not fit on the row). OK on a
 * row opens it: the mode, destination, source, NAC / colour code and slot / RAN, the frequency, the calls and when;
 * on a P25 or DMR station OK there tunes the VFO to it (CONFIG_VFO_MODES: the VFO settings' own path,
 * ui_svc_vfo_apply: the frequency, the mode, P25 NAC and talkgroup, DMR colour code and slot; a DMR talkgroup then
 * becomes the talk target as the actions' Talk does). Installed as core/ui/ui_heard.c.
 *
 * Fed by ui_app.c (prepare_toolbox): recent_add for every call Recent takes in (a new call), and while a DMR or P25
 * call goes on (the call heard again: the count goes up only after HEARD_GAP_S of quiet). Calls we made are not kept.
 * RAM: the list (16 x 16 B), its count, the open row's copy (16 B) and the tick's view pointer. */
#include "config.h"
#if CONFIG_HEARD
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_svc.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "codeplug/codeplug.h"
#include "ui/heard.h"
#include "ui/ui_heard.h"
#if CONFIG_VFO_MODES
#include "radio/vfox.h"
#endif

static heard_list_t list;
static heard_t open_one;		/* the row OK opened (the list may move under it) */
static const radio_view_t *tick_view;	/* this tick's view: the frequency and DMR's colour code and slot */

static uint32_t now_s(void)
{
	return ui_st.now_ms / 1000u;
}

void ui_heard_view(const radio_view_t *v)
{
	tick_view = v;
}

const heard_list_t *ui_heard_list(void)
{
	return &list;
}

void ui_heard_reset(void)
{
	heard_clear(&list);
}

void ui_heard_recent(const ui_recent_t *r, bool new_call)
{
	uint8_t mode = r->mode == 0 ? (uint8_t)RADIO_MODE_DMR : r->mode;
	if (r->out || (mode != RADIO_MODE_DMR && mode != RADIO_MODE_P25 && mode != RADIO_MODE_YSF &&
		       mode != RADIO_MODE_NXDN && mode != RADIO_MODE_DPMR))
		return;		/* a call we made; FM caller ID's kinds */
	heard_t k;
	memset(&k, 0, sizeof k);
	k.kind = mode;
	if (mode == RADIO_MODE_YSF) {
		if (r->name[0] == '\0')
			return;
		heard_pack_call(r->name, &k);
	} else {
		k.id = r->src & 0xFFFFFFu;
		k.tg = r->dst & 0xFFFFFFu;
		if (!r->group)
			k.kind |= HEARD_PRIV;
		k.nac = r->nac & 0xFFFu;
	}
	const radio_view_t *v = tick_view != NULL ? tick_view : ui_view();
	k.hz = v->ch.rx_hz;
	if (mode == RADIO_MODE_DMR)
		k.nac = (uint32_t)(v->dmr.cc & 15u) | (uint32_t)(v->dmr.ts & 3u) << 4;
	heard_note(&list, &k, new_call, now_s());
}

static const char *mode_name(uint8_t kind)
{
	switch (HEARD_MODE(kind)) {
	case RADIO_MODE_DMR: return "DMR";
	case RADIO_MODE_P25: return "P25";
	case RADIO_MODE_YSF: return "YSF";
	case RADIO_MODE_NXDN: return "NXDN";
	default: return "dPMR";
	}
}

static void hex3(sbuf_t *b, uint32_t v)
{
	for (int sh = 8; sh >= 0; sh -= 4)
		sb_chr(b, "0123456789ABCDEF"[(v >> sh) & 15u]);
}

/* "TG 101 ID 77285", "ID 77285>3101" (a private call), "TA2DOU" (YSF) */
static void who(sbuf_t *b, const heard_t *e)
{
	if (HEARD_MODE(e->kind) == RADIO_MODE_YSF) {
		char call[HEARD_CALL_CHARS + 1];
		heard_unpack_call(e, call);
		sb_str(b, call);
		return;
	}
	if (e->kind & HEARD_PRIV) {
		sb_str(b, "ID ");
		sb_u32(b, e->id, 1);
		sb_str(b, GLYPH_ARROW);
		sb_u32(b, e->tg, 1);
		return;
	}
	sb_str(b, "TG ");
	sb_u32(b, e->tg, 1);
	if (e->id != 0) {
		sb_str(b, " ID ");
		sb_u32(b, e->id, 1);
	}
}

/* "$659", "CC1", "RAN5", "" (YSF) */
static void code(sbuf_t *b, const heard_t *e)
{
	switch (HEARD_MODE(e->kind)) {
	case RADIO_MODE_P25:
		sb_chr(b, '$');
		hex3(b, e->nac);
#if CONFIG_NAC_DEC
		sb_chr(b, '/');		// both forms: "$659/1625"
		sb_u32(b, e->nac, 1);
#endif
		break;
	case RADIO_MODE_DMR:
		sb_str(b, "CC");
		sb_u32(b, e->nac & 15u, 1);
		break;
	case RADIO_MODE_NXDN:
		sb_str(b, "RAN");
		sb_u32(b, e->nac, 1);
		break;
	case RADIO_MODE_DPMR:
		sb_str(b, "CC");
		sb_u32(b, e->nac, 1);
		break;
	default:
		break;
	}
}

/* "3m": the age in one unit, as sb_age's short form */
static void age(sbuf_t *b, uint32_t s)
{
	char unit = s < 60u ? 's' : s < 3600u ? 'm' : s < 86400u ? 'h' : 'd';
	sb_u32(b, s < 60u ? s : s < 3600u ? s / 60u : s < 86400u ? s / 3600u : s / 86400u, 1);
	sb_chr(b, unit);
}

static void count_age(sbuf_t *b, const heard_t *e)
{
	sb_chr(b, 'x');
	sb_u32(b, e->count, 1);
	sb_chr(b, ' ');
	age(b, heard_age_s(e, now_s()));
}

/* The list row's destination: "TG101", ">3102222" (a private call: the unit called), "" (YSF) */
static void dest(sbuf_t *b, const heard_t *e)
{
	if (HEARD_MODE(e->kind) == RADIO_MODE_YSF)
		return;
	if (e->kind & HEARD_PRIV)
		sb_str(b, GLYPH_ARROW);
	else
		sb_str(b, "TG");
	sb_u32(b, e->tg, 1);
	sb_chr(b, ' ');
}

/* --- The list --- */

static uint16_t heard_count(void)
{
	heard_expire(&list, now_s());
	return list.n;
}

static void heard_item(uint16_t i, ui_item_t *it)
{
	if (i >= list.n)
		return;
	const heard_t *e = &list.e[i];
	sbuf_t b;
	/* "P25 77285" (the source; YSF: the callsign) */
	sb_init(&b, it->label, sizeof it->label);
	sb_str(&b, mode_name(e->kind));
	if (HEARD_MODE(e->kind) == RADIO_MODE_YSF) {
		sb_chr(&b, ' ');
		who(&b, e);
	} else if (e->id != 0) {
		sb_chr(&b, ' ');
		sb_u32(&b, e->id, 1);
	}
	/* "TG101 $659/1625 x12 3m", where that does not fit "TG101 x12 3m" (the alternates around the talkgroup and NAC
	 * only: the value's 44 bytes hold them so; an empty last alternate would leave the whole value out) */
	sb_init(&b, it->value, sizeof it->value);
	sb_str(&b, TEXT_ALT_OPEN);
	dest(&b, e);
	if (HEARD_MODE(e->kind) != RADIO_MODE_YSF) {
		code(&b, e);
		sb_chr(&b, ' ');
	}
	sb_str(&b, TEXT_ALT_NEXT);
	dest(&b, e);
	sb_str(&b, TEXT_ALT_CLOSE);
	count_age(&b, e);
}

static void heard_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i >= list.n)
		return;
	open_one = list.e[i];
	ui_push(SCR_HEARD_ONE, 0);
}

const ui_scr_def_t ui_heard_screen = {
	.name = "heard", .flags = SF_LIST | SF_FIELD, .prefix = TRN("Heard list"), .hint = TRN("OK open  Back"),
	.empty = TRN("Nothing heard yet"), .count = heard_count, .item = heard_item, .select = heard_select,
};

/* --- One station --- */

static bool can_tune(const heard_t *e)
{
	uint8_t m = HEARD_MODE(e->kind);
	return CONFIG_VFO_MODES && e->hz != 0 && (m == RADIO_MODE_P25 || m == RADIO_MODE_DMR);
}

static void one_draw(void)
{
	const heard_t *e = &open_one;
	char buf[40];
	sbuf_t b;
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR("Heard list"), mode_name(e->kind)) + 4;
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	sb_init(&b, buf, sizeof buf);
	who(&b, e);
	gfx_text(m, 0, y, buf, ui_col.fg);
	y += m->h + 4;
	sb_init(&b, buf, sizeof buf);
	code(&b, e);
	if (HEARD_MODE(e->kind) == RADIO_MODE_DMR) {
		uint32_t ts = (e->nac >> 4) & 3u;
		if (ts == 1u || ts == 2u) {
			sb_str(&b, " TS");
			sb_u32(&b, ts, 1);
		}
	}
	if (e->hz != 0) {
		if (HEARD_MODE(e->kind) != RADIO_MODE_YSF)
			sb_str(&b, "  ");
		sb_freq(&b, e->hz);
	}
	gfx_text(s, 0, y, buf, ui_col.dim);
	y += s->h + 4;
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, TR("Calls "));
	sb_u32(&b, e->count, 1);
	sb_str(&b, e->count >= 255u ? "+  " : "  ");
	sb_str(&b, TR("Last "));
	age(&b, heard_age_s(e, now_s()));
	gfx_text(s, 0, y, buf, ui_col.dim);
	ui_draw_bottom(&ui_st, can_tune(e) ? TR("OK tune VFO  Back") : TR("Back"));
	ui_draw_banner(&ui_st);
}

#if CONFIG_VFO_MODES
/* The VFO to the station: its frequency and mode, P25's NAC and talkgroup (Selective: the VFO's own rule), DMR's colour
 * code and slot; then DMR's talkgroup as the talk target (Home says "Talking to TG ..."), else a toast of our own */
static void __attribute__((noinline)) tune(const heard_t *e)
{
	static char msg[40];
	const radio_view_t *v = ui_view();
	uint8_t m = HEARD_MODE(e->kind);
	channel_t ch;
	memset(&ch, 0, sizeof ch);
	strcpy(ch.name, "VFO");
	ch.rx_hz = ch.tx_hz = e->hz;
	ch.bw = RADIO_BW_12K5;
	ch.power = CODEPLUG_POWER_DEFAULT;
	ch.color_code = 1;
	ch.timeslot = 1;
	vfox_set_mode(&ch, m, v->dmr.own_id);
	if (m == RADIO_MODE_P25) {
		ch.p25_nac = (uint16_t)e->nac;
		ch.p25_tg = (e->kind & HEARD_PRIV) || e->tg > 0xFFFFu ? 0u : (uint16_t)e->tg;	/* 0: every talkgroup */
	} else {
		uint32_t ts = (e->nac >> 4) & 3u;
		ch.color_code = (uint8_t)(e->nac & 15u);
		ch.timeslot = ts == 1u || ts == 2u ? (uint8_t)ts : 1u;
	}
	vfox_fix(&ch);
	radio_req_vfo(true);
	if (!ui_svc_vfo_apply(&ch)) {
		ui_toast(TR("VFO: not in this build"));
		return;
	}
	if (m == RADIO_MODE_DMR && !(e->kind & HEARD_PRIV) && e->tg != 0) {
		ui_target_t t;
		memset(&t, 0, sizeof t);
		t.id = e->tg;
		t.group = true;
		ui_talk(&t, false);	/* Home, "Talking to TG 2620" */
		return;
	}
	sbuf_t b;
	sb_init(&b, msg, sizeof msg);
	sb_str(&b, "VFO ");
	sb_freq(&b, e->hz);
	if (m == RADIO_MODE_P25) {
		sb_str(&b, " $");
		hex3(&b, e->nac);
#if CONFIG_NAC_DEC
		sb_chr(&b, '/');
		sb_u32(&b, e->nac, 1);
#endif
		if (ch.p25_tg != 0) {
			sb_str(&b, " TG ");
			sb_u32(&b, ch.p25_tg, 1);
		}
	}
	ui_go_home();
	ui_toast(msg);
}
#endif

static bool one_key(const input_event_t *ev)
{
	if (ev->gesture == GESTURE_PRESS && ev->action == ACT_SELECT) {
#if CONFIG_VFO_MODES
		if (can_tune(&open_one))
			tune(&open_one);
#endif
		return true;
	}
	return false;
}

static void one_enter(void)
{
	ui_dirty();
}

const ui_scr_def_t ui_heard_one_screen = {
	.name = "heard-one", .flags = SF_FIELD, .key = one_key, .draw = one_draw, .enter = one_enter,
};
#endif
