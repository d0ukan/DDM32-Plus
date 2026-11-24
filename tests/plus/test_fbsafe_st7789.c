// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DDM32 Plus: st7789_write_window_indexed (tools/plus_ramsave.py), RadioFoundry 454a5b76's tests/host/st7789Test.c
// test_window_indexed on upstream's own st7789Test (its command log and checks), which runs first unchanged.
#define main st7789_main
#include "st7789Test.c"
#undef main

// A frame buffer of the safe area only (hal_display_t fb_safe_area): its rows go to the window inside the insets,
// row r of the buffer to panel row y + r, w pixels each from its own stride
static void test_window_indexed(void)
{
	const st7789_t p = { &bus, 240, 320, 0xC0, true, 1 };
	static uint8_t fb[110 * 310];	// 220 x 310: insets 10 left and right, 5 top and bottom
	uint16_t pal[16];
	for (int i = 0; i < 16; i++) {
		pal[i] = (uint16_t)(0x1000u * (unsigned)i + 7u);
	}
	for (int y = 0; y < 310; y++) {
		for (int b = 0; b < 110; b++) {
			fb[y * 110 + b] = (uint8_t)((y & 0x0F) << 4 | (b & 0x0F));
		}
	}
	clear_log();
	st7789_write_window_indexed(&p, fb, pal, 10, 5, 220, 3, 5);
	CHECK(depth == 0 && begins == 1);
	int ca = find(0x2A), ra = find(0x2B), wr = find(0x2C);
	CHECK(ca >= 0 && log_e[ca].data[1] == 10 && log_e[ca].data[3] == 229);		// columns 10 .. 229
	CHECK(ra >= 0 && log_e[ra].data[1] == 8 && log_e[ra].data[3] == 9);		// rows 3, 4 at 5 + 3
	CHECK(wr >= 0 && log_e[wr].n_px == 220u * 2u);
	CHECK(px_log[0] == pal[3] && px_log[1] == pal[0] && px_log[219] == pal[109 & 15] && px_log[220] == pal[4]);
	// The buffer's last rows (310 of them) to the panel's last
	clear_log();
	st7789_write_window_indexed(&p, fb, pal, 10, 5, 220, 300, 310);
	ra = find(0x2B);
	wr = find(0x2C);
	CHECK(ra >= 0 && (log_e[ra].data[0] << 8 | log_e[ra].data[1]) == 305 &&
	      (log_e[ra].data[2] << 8 | log_e[ra].data[3]) == 314 && log_e[wr].n_px == 220u * 10u);
	// A window lower down: clipped to the panel, never past its bottom
	clear_log();
	st7789_write_window_indexed(&p, fb, pal, 10, 15, 220, 300, 310);
	ra = find(0x2B);
	wr = find(0x2C);
	CHECK(ra >= 0 && (log_e[ra].data[2] << 8 | log_e[ra].data[3]) == 319 && log_e[wr].n_px == 220u * 5u);
	// A window that doesn't fit the panel, or nothing to send
	clear_log();
	st7789_write_window_indexed(&p, fb, pal, 30, 5, 220, 0, 10);
	st7789_write_window_indexed(&p, fb, pal, 10, 5, 220, 7, 7);
	CHECK(n_e == 0 && begins == 0);
}

int main(void)
{
	int r = st7789_main();
	failures = 0;
	test_window_indexed();
	if (r != 0 || failures) {
		printf("test_fbsafe_st7789: %d failures\n", failures);
		return 1;
	}
	printf("test_fbsafe_st7789: ok\n");
	return 0;
}
