/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > SSTV (radio/radio_sstv.h): the picture as it arrives, under the status strip, scaled to the
 * display's width (at most SSTV_ROWS_MAX rows), the mode in the title; below it what the decoder is doing ("Waiting
 * for VIS / signal", "line 120/496", the clock offset), Back leaves. The decoder listens only while this screen is on
 * top (ui_msg_tick).
 *
 * The picture is kept nowhere but the frame buffer: this screen clears the display only when it is entered or when
 * something else drew it (ui_strip_draws moved: another screen, the hotspot view), and otherwise draws over its own
 * previous frame: the strip, the new rows, the status lines and the hint. A banner is clipped to the space below the
 * picture. A picture stays until the next one starts (or the screen is left). With CONFIG_SSTV_TX (DDM32 Plus SSTV
 * send) OK opens the send page (ui_sstvtx.c). */
#include "config.h"
#if CONFIG_SSTV
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "radio/radio.h"
#include "radio/radio_msg.h"
#include "radio/radio_sstv.h"

/* static: the UI task's stack is tight */
static radio_sstv_info_t in;
static uint8_t px[RADIO_SSTV_ROW_BYTES];
static char line[48];
static color_t pal[SSTV_COLOURS];
static bool drawn;
static uint8_t img_shown;
static uint32_t strip_seen, seq_seen;

static void sstv_enter(void)
{
	drawn = false;
	for (uint8_t i = 0; i < SSTV_COLOURS; i++) {
		uint8_t c[3];
		sstv_colour(i, c);
		pal[i] = RGB565(c[0], c[1], c[2]);
	}
	ui_dirty();
}

static void draw_row(int x0, int y)
{
	for (unsigned i = 0; i < RADIO_SSTV_ROW_BYTES; i++) {
		gfx_pixel(x0 + 2 * (int)i, y, pal[px[i] >> 4]);
		gfx_pixel(x0 + 2 * (int)i + 1, y, pal[px[i] & 15u]);
	}
}

static const char *first_line(void)
{
	const radio_view_t *v = ui_view();
	uint8_t st = radio_msg_status();
	if (in.nomem)
		return TR("Not enough memory for SSTV");
	if (st == RADIO_MSG_ST_OFF && (v->ch.mode != RADIO_MODE_FM || v->am))
		return TR("SSTV: tune an FM channel");
	if (st != RADIO_MSG_ST_ON)
		return TR("Waiting for receive audio");
	if (in.state == SSTV_IDLE)
		return TR("Waiting for VIS / signal");
	sbuf_t b;
	sb_init(&b, line, sizeof line);
	sb_str(&b, in.state == SSTV_RX ? TR("Line") : in.state == SSTV_DONE ? TR("Received") : TR("Signal lost"));
	sb_chr(&b, ' ');
	sb_u32(&b, in.line, 1);
	sb_chr(&b, '/');
	sb_u32(&b, in.lines, 1);
	return line;
}

static const char *second_line(void)
{
	sbuf_t b;
	sb_init(&b, line, sizeof line);
	if (in.vis_unknown != 0 && in.state != SSTV_RX) {
		sb_str(&b, TR("Mode not decoded: VIS"));
		sb_chr(&b, ' ');
		sb_u32(&b, in.vis_unknown & 0x7Fu, 1);
	} else if (in.state == SSTV_RX && in.ppm != 0) {
		sb_str(&b, TR("Clock"));
		sb_chr(&b, ' ');
		if (in.ppm > 0)
			sb_chr(&b, '+');
		sb_i32(&b, in.ppm);
		sb_str(&b, " ppm");
	} else if (in.state == SSTV_DONE || in.state == SSTV_LOST) {
		sb_str(&b, TR("Waiting for the next picture"));
	}
	return line;
}

static void sstv_draw(void)
{
	const ui_layout_t *L = &ui_layout;
	const font_t *f = L->small;
	int w = gfx_width();
	radio_sstv_info(&in);
	bool full = !drawn || ui_strip_draws != strip_seen;
	if (full)
		gfx_clear(ui_col.bg);
	int top = ui_draw_top(ui_view(), &ui_st, "SSTV", in.mode < SSTV_MODES ? sstv_mode_name(in.mode) : NULL);
	int iy = top + 1, bottom = iy + (int)SSTV_ROWS_MAX, x0 = w > (int)SSTV_W ? (w - (int)SSTV_W) / 2 : 0;
	if (full || in.image != img_shown) {
		gfx_fill(0, iy, w, (int)SSTV_ROWS_MAX, RGB565(0, 0, 0));	/* no picture yet, or a new one */
		img_shown = in.image;
	}
	uint8_t row;
	while (radio_sstv_row(img_shown, &row, px))
		draw_row(x0, iy + row);
	int y = bottom + 2;
	gfx_fill(0, bottom, w, L->hint_y - bottom, ui_col.bg);
	gfx_text(f, 0, y, first_line(), ui_col.fg);
	gfx_text(f, 0, y + f->h + 1, second_line(), ui_col.dim);
#if CONFIG_SSTV_TX
	ui_draw_bottom(&ui_st, TR("OK send  Back"));
#else
	ui_draw_bottom(&ui_st, TR("Back"));
#endif
	gfx_clip(0, bottom, w, gfx_height() - bottom);	/* a banner leaves the picture alone */
	ui_draw_banner(&ui_st);
	gfx_clip_off();
	strip_seen = ui_strip_draws;
	drawn = true;
}

bool ui_sstv_tick(void)
{
	uint32_t q = radio_sstv_seq();
	if (q == seq_seen)
		return false;
	seq_seen = q;
	return true;
}

#if CONFIG_SSTV_TX
/* OK: the send page (DDM32 Plus SSTV send) */
static bool sstv_key(const input_event_t *ev)
{
	if (ev->action == ACT_SELECT && ev->gesture == GESTURE_PRESS) {
		ui_push(SCR_SSTV_TX, 0);
		return true;
	}
	return false;
}
#endif

const ui_scr_def_t ui_sstv_screen = {
	.name = "sstv", .flags = SF_FIELD, .draw = sstv_draw, .enter = sstv_enter,
#if CONFIG_SSTV_TX
	.key = sstv_key,
#endif
};
#endif
