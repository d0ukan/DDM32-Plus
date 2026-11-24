// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DDM32 Plus: the safe-area frame buffer (tools/plus_ramsave.py, after RadioFoundry 454a5b76's tests/host/gfxTest.c
// test_safe_area and gfxFbEdgeTest.c). The same drawing with the whole panel's frame buffer and with the safe area's
// gives the same panel (what flush_indexed and fill_insets sent) and the same RGB565 and packed captures of the whole
// panel. The insets are painted only with the first frame, after gfx_invalidate and when the background changes. Built twice more with PLUS_FB_SAFE_BYTES
// (FBSAFE_EDGE): the array holds exactly the safe area, its last pixel is drawn (AddressSanitizer sees a write past
// it) and the whole panel no longer fits.
#include <stdio.h>
#include <string.h>
#include "ui/gfx.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void d_init(void) {}
static void d_backlight(uint8_t p) { (void)p; }

#ifndef FBSAFE_EDGE
#define SA_W	160
#define SA_H	120
static color_t panel_img[SA_H][SA_W];
static const hal_display_t *panel_d;
static int inset_fills, flushed_rows;

// The panel as display_dm32.c's flush writes it: the whole panel's rows, or the safe area's inside the insets
static void sa_flush(const void *fb, const uint16_t *pal, uint16_t y0, uint16_t y1)
{
	const hal_display_t *d = panel_d;
	int x0 = d->fb_safe_area ? d->inset_left : 0, yo = d->fb_safe_area ? d->inset_top : 0;
	int w = d->fb_safe_area ? d->width - d->inset_left - d->inset_right : d->width, stride = (w + 1) / 2;
	for (int y = y0; y < y1; y++)
		for (int x = 0; x < w; x++)
			panel_img[yo + y][x0 + x] = pal[(((const uint8_t *)fb)[y * stride + x / 2] >> ((x & 1) ? 0 : 4)) & 15];
	flushed_rows += y1 - y0;
}

static void sa_fill_insets(uint16_t c)
{
	const hal_display_t *d = panel_d;
	inset_fills++;
	for (int y = 0; y < SA_H; y++)
		for (int x = 0; x < SA_W; x++)
			if (x < d->inset_left || x >= SA_W - d->inset_right || y < d->inset_top || y >= SA_H - d->inset_bottom)
				panel_img[y][x] = c;
}

#define SA_STEPS	7
typedef struct {
	color_t panel[SA_H][SA_W];
	uint8_t rgb[SA_W * SA_H * 2];
	uint8_t packed[SA_W / 2 * SA_H];
	uint16_t pal[16];
	int fills;
} sa_shot_t;
static sa_shot_t sa_shots[2][SA_STEPS];

static void sa_take(sa_shot_t *t)
{
	memcpy(t->panel, panel_img, sizeof panel_img);
	gfx_read_rgb565(0, t->rgb, sizeof t->rgb);
	gfx_read_packed(0, t->packed, sizeof t->packed);
	for (uint8_t i = 0; i < 16; i++)
		t->pal[i] = gfx_palette(i);
	t->fills = inset_fills;
}

static void test_safe_area(void)
{
	static hal_display_t sd;	// gfx keeps the pointer
	const font_t *fonts[] = { &font_8x16, &font_aa_8x16, &font_12x24 };
	for (int safe = 0; safe < 2; safe++) {
		memset(&sd, 0, sizeof sd);
		sd.width = SA_W; sd.height = SA_H; sd.bpp = 4;
		sd.inset_left = 5; sd.inset_right = 4; sd.inset_top = 3; sd.inset_bottom = 2;	// odd left: the other nibble
		sd.init = d_init; sd.set_backlight = d_backlight;
		sd.flush_indexed = sa_flush;
		sd.fb_safe_area = safe;
		sd.fill_insets = sa_fill_insets;
		panel_d = &sd;
		inset_fills = 0;
		memset(panel_img, 0, sizeof panel_img);
		CHECK(gfx_init(&sd) && gfx_width() == 151 && gfx_height() == 115);
		gfx_fb_rect_t r;
		gfx_fb_rect(&r);
		CHECK(safe ? r.safe_area && r.x == 5 && r.y == 3 && r.w == 151 && r.h == 115 && r.stride == 76 :
			     !r.safe_area && r.x == 0 && r.y == 0 && r.w == 160 && r.h == 120 && r.stride == 80);
		// 1: the first frame, nothing drawn (the frame buffer's zeros, palette entry 0 black, as at boot: gfx_init
		// itself leaves the array as the run before left it)
		gfx_clear(0);
		gfx_flush();
		sa_take(&sa_shots[safe][0]);
		// 2: a screen: background, boxes, text in three fonts, icons
		gfx_clear(RGB565(16, 16, 16));
		gfx_round_fill(2, 2, 60, 20, RGB565(0, 128, 0));
		gfx_rect(0, 0, 151, 115, RGB565(200, 200, 200));
		for (int k = 0; k < 3; k++)
			gfx_text(fonts[k], 4, 24 + 26 * k, "Ag 145.500 TA2", RGB565(255, 255, 255));
		gfx_icon_battery(120, 4, 24, 12, 64, RGB565(255, 255, 255), RGB565(0, 200, 0));
		gfx_icon_bars(100, 4, 16, 12, 4, 3, RGB565(255, 255, 0), RGB565(64, 64, 64));
		gfx_flush();
		sa_take(&sa_shots[safe][1]);
		// 3: part of it changed
		gfx_fill(4, 24, 140, 18, RGB565(16, 16, 16));
		gfx_text(&font_aa_8x16, 4, 24, "TX 7.4 s", RGB565(255, 0, 0));
		gfx_flush();
		sa_take(&sa_shots[safe][2]);
		// 4: another background, drawing reaching past every edge (clipped to the safe area)
		gfx_clear(RGB565(0, 64, 128));
		gfx_fill(-10, -10, 400, 4, RGB565(255, 255, 0));
		gfx_fill(-10, 111, 400, 40, RGB565(255, 128, 0));
		gfx_vline(-1, 0, 200, RGB565(1, 2, 3));
		gfx_vline(150, -5, 200, RGB565(9, 9, 9));
		gfx_text(&font_8x16, -3, 50, "EDGE TEXT WIDER THAN THE SCREEN", RGB565(255, 255, 255));
		gfx_text(&font_aa_12x24, 100, 100, "LOW", RGB565(0, 255, 0));
		gfx_pixel(150, 114, RGB565(255, 0, 0));
		gfx_pixel(151, 114, RGB565(0, 255, 0));	// outside: nothing
		gfx_pixel(0, -1, RGB565(0, 255, 0));
		gfx_flush();
		sa_take(&sa_shots[safe][3]);
		// 5: more than 16 colors (nearest colors, reused entries) and a clip window
		gfx_clip(10, 10, 60, 40);
		for (int k = 0; k < 24; k++)
			gfx_fill(k * 3, k * 2, 30, 30, RGB565(k * 10, 255 - k * 9, (k * 37) & 255));
		gfx_clip_off();
		gfx_flush();
		sa_take(&sa_shots[safe][4]);
		// 6: the same background again (no inset fill), a narrow band changed
		gfx_clear(RGB565(0, 64, 128));
		gfx_fill(0, 90, 151, 1, RGB565(255, 0, 255));
		gfx_fill(0, 95, 151, 2, RGB565(0, 255, 255));
		gfx_flush();
		sa_take(&sa_shots[safe][5]);
		// 7: gfx_invalidate (the panel may have lost its memory): everything again, the insets once
		int fills = inset_fills;
		memset(panel_img, 0x5A, sizeof panel_img);
		gfx_invalidate();
		gfx_flush();
		CHECK(inset_fills == fills + (safe ? 1 : 0));
		gfx_flush();
		CHECK(inset_fills == fills + (safe ? 1 : 0));
		sa_take(&sa_shots[safe][6]);
	}
	for (int k = 0; k < SA_STEPS; k++) {
		const sa_shot_t *a = &sa_shots[0][k], *b = &sa_shots[1][k];
		for (int i = 0; i < SA_W * SA_H; i++)
			if (a->panel[i / SA_W][i % SA_W] != b->panel[i / SA_W][i % SA_W]) {
				printf("step %d: the panels differ first at x %d y %d (%04x / %04x)\n", k + 1, i % SA_W, i / SA_W,
				       a->panel[i / SA_W][i % SA_W], b->panel[i / SA_W][i % SA_W]);
				break;
			}
		CHECK(memcmp(a->panel, b->panel, sizeof a->panel) == 0);
		CHECK(memcmp(a->rgb, b->rgb, sizeof a->rgb) == 0);
		CHECK(memcmp(a->packed, b->packed, sizeof a->packed) == 0);
		CHECK(memcmp(a->pal, b->pal, sizeof a->pal) == 0);
		// what the panel shows is what the RGB565 capture says
		for (int y = 0; y < SA_H; y++)
			for (int x = 0; x < SA_W; x++)
				CHECK(b->panel[y][x] == (color_t)(b->rgb[(y * SA_W + x) * 2] | b->rgb[(y * SA_W + x) * 2 + 1] << 8));
	}
	CHECK(sa_shots[1][3].panel[119][159] == RGB565(0, 64, 128) && sa_shots[1][3].panel[3 + 114][5 + 150] == RGB565(255, 0, 0));
	// The insets painted with the first frame, then only when the background changed
	CHECK(sa_shots[1][0].fills == 1);
	for (int k = 1; k < SA_STEPS - 1; k++)
		CHECK(sa_shots[1][k].fills - sa_shots[1][k - 1].fills ==
		      (sa_shots[1][k].panel[0][0] != sa_shots[1][k - 1].panel[0][0]));
	CHECK(sa_shots[0][SA_STEPS - 1].fills == 0);
	// Not for a depth without a palette entry for the background, nor without the board's fill: the whole panel then
	gfx_fb_rect_t r;
	memset(&sd, 0, sizeof sd);
	sd.width = SA_W; sd.height = SA_H; sd.bpp = 16; sd.inset_left = 5; sd.fb_safe_area = true;
	sd.fill_insets = sa_fill_insets;
	CHECK(gfx_init(&sd));
	gfx_fb_rect(&r);
	CHECK(!r.safe_area && r.w == SA_W);
	sd.bpp = 4; sd.flush_indexed = sa_flush; sd.fill_insets = NULL;
	CHECK(gfx_init(&sd));
	gfx_fb_rect(&r);
	CHECK(!r.safe_area && r.w == SA_W);
	// Insets of zero: nothing to keep out, the whole panel
	sd.fill_insets = sa_fill_insets; sd.inset_left = 0;
	CHECK(gfx_init(&sd));
	gfx_fb_rect(&r);
	CHECK(!r.safe_area && r.w == SA_W);
}

int main(void)
{
	test_safe_area();
	if (fails) {
		printf("test_fbsafe_gfx: %d failed\n", fails);
		return 1;
	}
	printf("test_fbsafe_gfx: ok (whole-panel and safe-area frame buffers identical on the panel and in captures)\n");
	return 0;
}

#else	// FBSAFE_EDGE: the array sized by PLUS_FB_SAFE_BYTES, exactly a safe area of EDGE_W x EDGE_H
#ifndef PLUS_FB_SAFE_BYTES
#error "FBSAFE_EDGE needs PLUS_FB_SAFE_BYTES"
#endif
#define STRIDE	((EDGE_SW + 1) / 2)
static uint8_t last_row[STRIDE];
static int last_row_sent;
static uint16_t last_pal[16];
static void d_fill_insets(uint16_t c) { (void)c; }
static void d_flush_indexed(const void *fb, const uint16_t *pal, uint16_t y0, uint16_t y1)
{
	if (y1 == EDGE_SH) {
		memcpy(last_row, (const uint8_t *)fb + (size_t)(EDGE_SH - 1) * STRIDE, STRIDE);
		memcpy(last_pal, pal, sizeof last_pal);
		last_row_sent++;
	}
}

int main(void)
{
	_Static_assert(STRIDE * EDGE_SH == PLUS_FB_SAFE_BYTES, "the safe area must be exactly PLUS_FB_SAFE_BYTES");
	static hal_display_t disp = {
		.width = EDGE_W, .height = EDGE_H, .bpp = 4, .inset_left = EDGE_L, .inset_right = EDGE_W - EDGE_L - EDGE_SW,
		.inset_top = EDGE_T, .inset_bottom = EDGE_H - EDGE_T - EDGE_SH, .init = d_init,
		.flush_indexed = d_flush_indexed, .set_backlight = d_backlight, .fill_insets = d_fill_insets,
	};
	// The whole panel no longer fits the array
	CHECK(!gfx_init(&disp) && gfx_width() == 0);
	disp.fb_safe_area = true;
	CHECK(gfx_init(&disp));
	gfx_fb_rect_t r;
	gfx_fb_rect(&r);
	CHECK(r.safe_area && r.w == EDGE_SW && r.h == EDGE_SH && r.stride == STRIDE);
	const color_t bg = RGB565(0, 0, 255), red = RGB565(255, 0, 0);
	gfx_clear(bg);
	gfx_fill(0, 0, EDGE_SW, EDGE_SH, bg);
	gfx_pixel(EDGE_SW - 1, EDGE_SH - 1, red);
	gfx_pixel(EDGE_SW - 2, EDGE_SH - 1, red);
	CHECK(gfx_get_pixel(EDGE_SW - 1, EDGE_SH - 1) == red && gfx_get_pixel(EDGE_SW - 3, EDGE_SH - 1) == bg);
	gfx_flush();
	CHECK(last_row_sent == 1);
	if (EDGE_SW & 1) {	// the last byte's high nibble holds the last pixel
		CHECK(last_pal[last_row[STRIDE - 1] >> 4] == red && last_pal[last_row[STRIDE - 2] & 15] == red);
	} else {
		CHECK(last_pal[last_row[STRIDE - 1] & 15] == red && last_pal[last_row[STRIDE - 1] >> 4] == red);
	}
	// The packed capture of the whole panel reads past nothing
	static uint8_t whole[(EDGE_W + 1) / 2 * EDGE_H + 8];
	gfx_read_packed(0, whole, sizeof whole);
	CHECK(whole[sizeof whole - 1] == 0);
	if (fails) {
		printf("test_fbsafe_edge %dx%d: %d failed\n", EDGE_SW, EDGE_SH, fails);
		return 1;
	}
	printf("test_fbsafe_edge: ok (a %d x %d safe area in %u B)\n", EDGE_SW, EDGE_SH, (unsigned)PLUS_FB_SAFE_BYTES);
	return 0;
}
#endif
