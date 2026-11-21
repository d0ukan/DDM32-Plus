/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Pager and Tools > ACARS (radio/radio_msg.h): the messages heard while the screen is open, newest
 * first, each row shortened ("1234567: text", "TC-JFV H1: text" and its age); OK opens one in full (its address,
 * the rest of its header, how long ago, the text) as a page of text (Up / Down scroll). The decoders listen only
 * while one of these screens is on top (ui_msg_tick, from ui_app_tick); the line above the rows says whether the
 * channel suits. */
#include "config.h"
#if CONFIG_MSG_RX
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "radio/radio.h"
#include "radio/radio_msg.h"
#include "ui/ui_scratch.h"

#define VIEW_ACARS	0x8000u		/* SCR_MSG_VIEW's arg: an ACARS message (else a page), and its id */

/* One message's page: built again in each draw into the tool screens' shared buffer (ui_scratch.h; not on the UI
 * task's stack, which is tight) */
static uint16_t page_first, page_max;

static const char *status_line(uint8_t kind)
{
	const radio_view_t *v = ui_view();
	bool suits = v->ch.mode == RADIO_MODE_FM && (kind == RADIO_MSG_ACARS ? v->am : !v->am);
	uint8_t st = radio_msg_status();
	if (st == RADIO_MSG_ST_ON)
		return TR("Listening on this frequency");
	if (st == RADIO_MSG_ST_WAIT || suits)
		return TR("Waiting for receive audio");
	return kind == RADIO_MSG_PAGER ? TR("Pager: tune an FM channel") :
					 TR("ACARS: tune the air band in AM (131.725, 131.550 MHz)");
}

/* --- Pager --- */

static uint16_t pager_count(void)
{
	return radio_msg_count(RADIO_MSG_PAGER);
}

static void pager_item(uint16_t i, ui_item_t *it)
{
	const radio_pager_msg_t *m = radio_msg_pager((uint8_t)i);
	if (m == NULL)
		return;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sb_u32(&b, m->capcode, 1);
	sb_str(&b, ": ");
	sb_str(&b, m->kind == RADIO_MSG_TONE ? TR("Tone only") : m->text);
	ui_sb_age(it->value, sizeof it->value, ui_st.now_ms - m->ms);
	it->flags |= UI_ITEM_PREVIEW;	/* the start of the message: cut at its end */
	if (m->flags & RADIO_MSG_ERRORS)
		it->flags |= UI_ITEM_WARN;
}

static void pager_select(uint16_t i, bool hold)
{
	(void)hold;
	const radio_pager_msg_t *m = radio_msg_pager((uint8_t)i);
	if (m != NULL)
		ui_push(SCR_MSG_VIEW, m->id);
}

static const char *pager_body(void)
{
	return status_line(RADIO_MSG_PAGER);
}

const ui_scr_def_t ui_pager_screen = {
	.name = "pager", .flags = SF_LIST | SF_FIELD, .prefix = TRN("Pager"), .hint = TRN("OK open  Back"),
	.empty = TRN("No messages yet"), .count = pager_count, .item = pager_item, .select = pager_select,
	.body = pager_body,
};

/* --- ACARS --- */

static uint16_t acars_count(void)
{
	return radio_msg_count(RADIO_MSG_ACARS);
}

static void acars_item(uint16_t i, ui_item_t *it)
{
	const radio_acars_msg_t *m = radio_msg_acars((uint8_t)i);
	if (m == NULL)
		return;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sb_str(&b, m->reg[0] ? m->reg : "?");
	sb_chr(&b, ' ');
	sb_str(&b, m->label);
	sb_str(&b, ": ");
	sb_str(&b, m->text);
	ui_sb_age(it->value, sizeof it->value, ui_st.now_ms - m->ms);
	it->flags |= UI_ITEM_PREVIEW;
	if (m->flags & RADIO_MSG_ERRORS)
		it->flags |= UI_ITEM_WARN;
}

static void acars_select(uint16_t i, bool hold)
{
	(void)hold;
	const radio_acars_msg_t *m = radio_msg_acars((uint8_t)i);
	if (m != NULL)
		ui_push(SCR_MSG_VIEW, (uint16_t)(VIEW_ACARS | m->id));
}

static const char *acars_body(void)
{
	return status_line(RADIO_MSG_ACARS);
}

const ui_scr_def_t ui_acars_screen = {
	.name = "acars", .flags = SF_LIST | SF_FIELD, .prefix = "ACARS", .hint = TRN("OK open  Back"),
	.empty = TRN("No messages yet"), .count = acars_count, .item = acars_item, .select = acars_select,
	.body = acars_body,
};

/* --- one message --- */

static void heard_line(sbuf_t *b, uint32_t ms, uint8_t flags)
{
	sb_str(b, TR("Heard"));
	sb_str(b, ": ");
	sb_age_long(b, ui_st.now_ms - ms);
	if (flags & RADIO_MSG_AGAIN) {
		sb_str(b, ", ");
		sb_str(b, TR("repeated"));
	}
	if (flags & RADIO_MSG_ERRORS) {
		sb_chr(b, '\n');
		sb_str(b, TR("Received with errors"));
	}
	sb_chr(b, '\n');
	sb_str(b, TR("Message"));
	sb_str(b, ": ");
}

static void pager_page(sbuf_t *b, uint16_t id)
{
	static const uint16_t rates[3] = { 512, 1200, 2400 };
	const radio_pager_msg_t *m = NULL;
	for (uint8_t i = 0; i < radio_msg_count(RADIO_MSG_PAGER) && m == NULL; i++)
		if (radio_msg_pager(i)->id == id)
			m = radio_msg_pager(i);
	if (m == NULL) {
		sb_str(b, TR("Message deleted"));	/* no longer kept: newer ones took its place */
		return;
	}
	sb_str(b, "Capcode: ");
	sb_u32(b, m->capcode, 1);
	sb_chr(b, '\n');
	sb_str(b, TR("Function"));
	sb_str(b, ": ");
	sb_u32(b, m->function, 1);
	sb_str(b, ", ");
	sb_u32(b, rates[m->rate < 3u ? m->rate : 0], 1);
	sb_str(b, " bit/s\n");
	heard_line(b, m->ms, m->flags);
	sb_str(b, m->kind == RADIO_MSG_TONE ? TR("Tone only") : m->text);
	if (m->flags & RADIO_MSG_CUT)
		sb_str(b, "...");
}

static void acars_page(sbuf_t *b, uint16_t id)
{
	const radio_acars_msg_t *m = NULL;
	for (uint8_t i = 0; i < radio_msg_count(RADIO_MSG_ACARS) && m == NULL; i++)
		if (radio_msg_acars(i)->id == id)
			m = radio_msg_acars(i);
	if (m == NULL) {
		sb_str(b, TR("Message deleted"));
		return;
	}
	sb_str(b, TR("Aircraft"));
	sb_str(b, ": ");
	sb_str(b, m->reg[0] ? m->reg : "?");
	if (m->flight[0]) {
		sb_chr(b, '\n');
		sb_str(b, TR("Flight"));
		sb_str(b, ": ");
		sb_str(b, m->flight);
	}
	sb_chr(b, '\n');
	sb_str(b, TR("Label"));
	sb_str(b, ": ");
	sb_str(b, m->label);
	sb_str(b, "  ");
	sb_str(b, TR("Block"));
	sb_chr(b, ' ');
	sb_chr(b, m->block >= ' ' ? m->block : '?');
	sb_chr(b, '\n');
	heard_line(b, m->ms, m->flags);
	sb_str(b, m->text);
	if (m->flags & (RADIO_MSG_CUT | RADIO_MSG_MORE))
		sb_str(b, "...");
}

static void view_draw(void)
{
	uint16_t arg = ui_frame()->arg;
	bool ac = (arg & VIEW_ACARS) != 0;
	sbuf_t b;
	char *page = ui_scratch_take(UI_SCRATCH_MSG)->msg_page;
	sb_init(&b, page, UI_SCRATCH_MSG_PAGE);
	if (ac)
		acars_page(&b, (uint16_t)(arg & ~VIEW_ACARS));
	else
		pager_page(&b, arg);
	page_max = (uint16_t)ui_draw_text_page(ui_view(), &ui_st, ac ? "ACARS" : TR("Pager"), page, &page_first);
}

/* Up / Down and the knob scroll a line, as on About; Back leaves */
static bool view_key(const input_event_t *ev)
{
	int dir = ev->action == ACT_NAV_UP ? -1 : ev->action == ACT_NAV_DOWN ? 1 :
		ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : 0;
	if (dir == 0 || (ev->gesture != GESTURE_PRESS && ev->gesture != GESTURE_REPEAT && ev->gesture != GESTURE_LONG &&
			ev->action != ACT_ENC_STEP))
		return false;
	int n = ev->action == ACT_ENC_STEP ? (ev->value < 0 ? -ev->value : ev->value) : 1;
	int f = page_first + dir * n;
	page_first = (uint16_t)(f < 0 ? 0 : f > page_max ? page_max : f);
	ui_dirty();
	return true;
}

static void view_enter(void)
{
	page_first = 0;
}

const ui_scr_def_t ui_msgview_screen = {
	.name = "msgview", .flags = SF_FIELD, .key = view_key, .draw = view_draw, .enter = view_enter,
};

bool ui_msg_tick(uint8_t scr, uint16_t arg)
{
	static uint32_t seen;
	uint8_t kind = scr == SCR_PAGER ? RADIO_MSG_PAGER : scr == SCR_ACARS ? RADIO_MSG_ACARS :
		       scr == SCR_MSG_VIEW ? ((arg & VIEW_ACARS) ? RADIO_MSG_ACARS : RADIO_MSG_PAGER) :
		       CONFIG_SSTV && scr == SCR_SSTV ? RADIO_MSG_SSTV : 0;
#if CONFIG_TONE_FIND || CONFIG_SELCALL
	if (kind == 0)
		kind = ui_fmdec_kind(scr);	/* Tools > Tone finder, DTMF / SelCall (ui_fmdec.c) */
#endif
#if CONFIG_CW_RX
	if (kind == 0 && scr == SCR_CW)
		kind = RADIO_MSG_CW;	/* Tools > CW decoder (ui_cw.c); not its practice page */
#endif
	radio_msg_want(kind);
	bool again = false;
#if CONFIG_SSTV_TX
	again = ui_sstvtx_tick(scr);	/* DDM32 Plus SSTV send: its progress; leaving the page stops it */
#endif
#if CONFIG_SSTV
	again = (kind == RADIO_MSG_SSTV && ui_sstv_tick()) || again;	/* a row or a status change of SSTV */
#endif
#if CONFIG_TONE_FIND || CONFIG_SELCALL
	again = again || ((kind == RADIO_MSG_TONEF || kind == RADIO_MSG_SELCALL) && ui_fmdec_tick());
#endif
#if CONFIG_CW_RX
	again = ui_cw_tick(scr) || again;	/* the decoder's text and info, the practice page's timing */
#endif
	uint32_t q = radio_msg_seq();
	if (q == seen)
		return again;
	seen = q;
	return kind != 0;
}
#endif
