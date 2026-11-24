// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus art (integration/radiofoundry/art): the tables tools/art_build.py generated match the text sources in
// art/, and art.c draws exactly those pixels, in the right colors, inside the right box; the whole boot screen
// (splash_plus.c) keeps the callsign on the bottom line, centred, inside the panel.
//
// From firmware/ (one command; docs/art.md):
//   cc -std=gnu11 -Wall -Wextra -Wshadow -Wconversion -Werror -fsanitize=address,undefined -DART_HOST_TEST
//      -Iintegration/radiofoundry/art -Itests/plus tests/plus/test_art.c integration/radiofoundry/art/art.c
//      integration/radiofoundry/art/art_data.c integration/radiofoundry/art/splash_plus.c -o test_art && ./test_art art
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "art.h"
#include "art_host_ui.h"

#define FB_W	240
#define FB_H	320
#define PAPER	0x1234u		// never an art color: what the frame holds where nothing was drawn

static uint16_t fb[FB_H][FB_W];
static unsigned fills, outside;
static int fails;

void gfx_fill(int x, int y, int w, int h, uint16_t c);
void gfx_fill(int x, int y, int w, int h, uint16_t c)
{
	fills++;
	if (w <= 0 || h <= 0) {
		outside++;	// art.c never sends an empty run
		return;
	}
	for (int yy = y; yy < y + h; yy++)
		for (int xx = x; xx < x + w; xx++) {
			if (xx < 0 || yy < 0 || xx >= FB_W || yy >= FB_H)
				outside++;
			else
				fb[yy][xx] = c;
		}
}

static void clear(void)
{
	for (int y = 0; y < FB_H; y++)
		for (int x = 0; x < FB_W; x++)
			fb[y][x] = PAPER;
	fills = outside = 0;
}

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

// --- the text sources (tools/art_build.py's format) ---

static char src[64 * 1024];

static int load(const char *dir, const char *name)
{
	char path[512];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		printf("cannot read %s\n", path);
		return -1;
	}
	size_t n = fread(src, 1, sizeof src - 1, f);
	fclose(f);
	src[n] = 0;
	return 0;
}

// The next line that is not empty or a ';' comment, without its line end; NULL at the end
static char *next_line(char **p)
{
	while (**p) {
		char *s = *p, *e = strchr(s, '\n');
		if (e != NULL) {
			*e = 0;
			*p = e + 1;
		} else {
			*p = s + strlen(s);
		}
		size_t n = strlen(s);
		while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == ' '))
			s[--n] = 0;
		if (n > 0 && s[0] != ';')
			return s;
	}
	return NULL;
}

static int upper_eq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if ((*a >= 'a' && *a <= 'z' ? *a - 32 : *a) != *b)
			return 0;
	return *a == *b;
}

// Icon names in enum order, as art_data.h spells them (checked against the sources)
static const char *const enum_names[] = {
	"SCAN", "POSITION", "HOTSPOT", "USB_NET", "SFR", "ROAM", "DIAG", "APRS", "SPECTRUM", "FOX", "BEACON", "LONE",
	"ALMANAC", "GOTO", "P25SEARCH", "AIRBAND", "BATTERY", "ZONE_OFFER", "MAP", "SETTINGS", "ZONES", "CONTACTS",
	"MESSAGES", "DUAL", "LANGUAGE", "CHANNELS", "RECENT", "LAST_CALLER", "DOTA", "XBAND", "CW", "SOS",
};

static void test_icons(const char *dir)
{
	CHECK(ART_ICON_COUNT == sizeof enum_names / sizeof enum_names[0], "icon count %d", ART_ICON_COUNT);
	CHECK(ART_ICON_SCAN == 0 && ART_ICON_SPECTRUM == 8 && ART_ICON_SOS == ART_ICON_COUNT - 1, "enum order");
	if (load(dir, "icons.txt") != 0) {
		fails++;
		return;
	}
	char *p = src, *line;
	int id = -1, row = 0, seen = 0;
	static uint8_t want[ART_ICON_COUNT][ART_ICON_H][ART_ICON_W];
	while ((line = next_line(&p)) != NULL) {
		if (strncmp(line, "icon ", 5) == 0) {
			CHECK(id < 0 || row == ART_ICON_H, "icon %d has %d rows", id, row);
			id++;
			row = 0;
			CHECK(id < ART_ICON_COUNT && upper_eq(line + 5, enum_names[id]), "icon %d is %s", id, line + 5);
			if (id >= ART_ICON_COUNT)
				return;
			seen++;
			continue;
		}
		CHECK(id >= 0 && row < ART_ICON_H && strlen(line) == ART_ICON_W, "bad row '%s'", line);
		if (id < 0 || row >= ART_ICON_H)
			return;
		for (int c = 0; c < ART_ICON_W; c++)
			want[id][row][c] = line[c] == '#';
		row++;
	}
	CHECK(seen == ART_ICON_COUNT, "%d icons in icons.txt", seen);

	for (int i = 0; i < ART_ICON_COUNT; i++) {
		int ink = 0;
		// The table itself
		for (int r = 0; r < ART_ICON_H; r++)
			for (int c = 0; c < ART_ICON_W; c++) {
				int bit = (art_icon_bits[i][r] >> (15 - c)) & 1;
				CHECK(bit == want[i][r][c], "icon %s table row %d col %d", enum_names[i], r, c);
				ink += bit;
			}
		CHECK(ink > 8 && ink < ART_ICON_W * ART_ICON_H - 8, "icon %s ink %d", enum_names[i], ink);
		for (int j = 0; j < i; j++)
			CHECK(memcmp(art_icon_bits[i], art_icon_bits[j], sizeof art_icon_bits[i]) != 0,
			      "icons %s and %s are the same", enum_names[i], enum_names[j]);
		// Drawn: the source pixels in the color, nothing else touched, one fill a run at most
		const int ox = 37, oy = 101;
		const uint16_t col = (uint16_t)(0xF800u ^ (unsigned)i);
		clear();
		art_icon((unsigned)i, ox, oy, col);
		CHECK(outside == 0, "icon %s drew outside", enum_names[i]);
		CHECK(fills <= (unsigned)ink, "icon %s took %u fills", enum_names[i], fills);
		int bad = 0;
		for (int y = 0; y < FB_H; y++)
			for (int x = 0; x < FB_W; x++) {
				int in = x >= ox && x < ox + ART_ICON_W && y >= oy && y < oy + ART_ICON_H;
				uint16_t expect = in && want[i][y - oy][x - ox] ? col : PAPER;
				bad += fb[y][x] != expect;
			}
		CHECK(bad == 0, "icon %s: %d pixels differ", enum_names[i], bad);
	}

	// Out of range: nothing drawn. Partly off screen: gfx clips (here: counted as outside), art.c still only fills
	clear();
	art_icon(ART_ICON_COUNT, 0, 0, 1);
	art_icon(0xFFFFFFFFu, 0, 0, 1);
	CHECK(fills == 0, "an unknown icon drew");
	clear();
	art_icon(ART_ICON_BATTERY, -8, -8, 1);
	CHECK(fills > 0 && outside > 0, "an icon off the corner");
}

static int mark_src[ART_MARK_H][ART_MARK_W];
static uint16_t mark_rgb_src[3];
static int mark_ink_src[3], mark_fg_src[3];

static uint16_t rgb565(int r, int g, int b)
{
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void test_mark(const char *dir)
{
	if (load(dir, "mark.txt") != 0) {
		fails++;
		return;
	}
	char *p = src, *line;
	int row = 0, w = 0, h = 0;
	while ((line = next_line(&p)) != NULL) {
		int r, g, b;
		char k, role[8];
		if (sscanf(line, "size %d %d", &w, &h) == 2)
			continue;
		if (sscanf(line, "color %c %d %d %d %7s", &k, &r, &g, &b, role) == 5) {
			CHECK(k >= 'a' && k <= 'c', "color %c", k);
			if (k >= 'a' && k <= 'c') {
				mark_rgb_src[k - 'a'] = rgb565(r, g, b);
				mark_ink_src[k - 'a'] = strcmp(role, "paper") != 0;
				mark_fg_src[k - 'a'] = strcmp(role, "fg") == 0;
			}
			continue;
		}
		CHECK(row < ART_MARK_H && strlen(line) == ART_MARK_W, "mark row %d", row);
		if (row >= ART_MARK_H)
			return;
		for (int c = 0; c < ART_MARK_W; c++)
			mark_src[row][c] = line[c] == '.' ? 0 : line[c] - 'a' + 1;
		row++;
	}
	CHECK(w == ART_MARK_W && h == ART_MARK_H && row == ART_MARK_H, "mark size %dx%d, %d rows", w, h, row);
	for (int i = 0; i < 3; i++) {
		CHECK(art_mark_rgb[i] == mark_rgb_src[i], "mark color %d", i);
		CHECK(art_mark_ink[i] == mark_ink_src[i], "mark mono %d", i);
		CHECK(art_mark_fg[i] == mark_fg_src[i], "mark fg role %d", i);
		CHECK(art_mark_rgb[i] != PAPER, "mark color %d is the test paper", i);
	}
	CHECK(art_accent() == art_mark_rgb[0], "accent");

	for (int scale = 1; scale <= 2; scale++)
		for (int mono = 0; mono <= 1; mono++) {
			const int ox = 10, oy = 20;
			const uint16_t fg = 0x0001, bg = 0x0002;
			clear();
			art_mark(ox, oy, scale, mono, fg, bg);
			CHECK(outside == 0, "mark drew outside");
			int bad = 0;
			for (int y = 0; y < FB_H; y++)
				for (int x = 0; x < FB_W; x++) {
					int sx = (x - ox) / scale, sy = (y - oy) / scale;
					int in = x >= ox && y >= oy && sx < ART_MARK_W && sy < ART_MARK_H;
					int v = in ? mark_src[sy][sx] : 0;
					uint16_t expect = v == 0 ? PAPER : mono ? (mark_ink_src[v - 1] ? fg : bg) : mark_fg_src[v - 1] ? fg : mark_rgb_src[v - 1];
					bad += fb[y][x] != expect;
				}
			CHECK(bad == 0, "mark scale %d mono %d: %d pixels differ", scale, mono, bad);
		}
	// The scale is kept to 1..4
	clear();
	art_mark(0, 0, 0, false, 1, 2);
	int ink0 = 0;
	for (int y = 0; y < FB_H; y++)
		for (int x = 0; x < FB_W; x++)
			ink0 += fb[y][x] != PAPER && (x >= ART_MARK_W || y >= ART_MARK_H);
	CHECK(ink0 == 0, "scale 0 drew past 1x");
}

static void test_wordmark(const char *dir)
{
	if (load(dir, "wordmark.txt") != 0) {
		fails++;
		return;
	}
	static int want[ART_WORD_H][ART_WORD_W];
	char *p = src, *line;
	int row = 0, w = 0, h = 0, split = -1;
	while ((line = next_line(&p)) != NULL) {
		if (sscanf(line, "size %d %d", &w, &h) == 2 || sscanf(line, "split %d", &split) == 1)
			continue;
		CHECK(row < ART_WORD_H && strlen(line) == ART_WORD_W, "wordmark row %d", row);
		if (row >= ART_WORD_H)
			return;
		for (int c = 0; c < ART_WORD_W; c++)
			want[row][c] = line[c] == '#';
		row++;
	}
	CHECK(w == ART_WORD_W && h == ART_WORD_H && row == ART_WORD_H && split == ART_WORD_SPLIT, "wordmark header");
	CHECK(3 + ART_WORD_W * 2 <= FB_W, "the wordmark fits a 240 px panel at 2x (the splash's scale)");
	for (int scale = 1; scale <= 3 && 3 + ART_WORD_W * scale <= FB_W; scale++) {	// 3x only while it fits
		const int ox = 3, oy = 200;
		clear();
		art_wordmark(ox, oy, scale, 0x00AA, 0x0BB0);
		CHECK(outside == 0, "wordmark drew outside");
		int bad = 0, fg = 0, acc = 0;
		for (int y = 0; y < FB_H; y++)
			for (int x = 0; x < FB_W; x++) {
				int sx = (x - ox) / scale, sy = (y - oy) / scale;
				int in = x >= ox && y >= oy && sx < ART_WORD_W && sy < ART_WORD_H;
				uint16_t expect = in && want[sy][sx] ? (sx >= ART_WORD_SPLIT ? 0x0BB0 : 0x00AA) : PAPER;
				bad += fb[y][x] != expect;
				fg += fb[y][x] == 0x00AA;
				acc += fb[y][x] == 0x0BB0;
			}
		CHECK(bad == 0 && fg > 0 && acc > 0, "wordmark scale %d: %d pixels differ", scale, bad);
	}
}

static void test_splash(void)
{
	for (int k = 1; k <= 2; k++) {
		clear();
		int h = art_splash(FB_W / 2, 10, k, false, 0x0003, 0x0004);
		CHECK(h == art_splash_h(k) && h == (ART_MARK_H + 6 + ART_WORD_H) * k, "splash height %d", h);
		CHECK(outside == 0, "splash drew outside");
		int x0 = FB_W, x1 = -1, y0 = FB_H, y1 = -1;
		for (int y = 0; y < FB_H; y++)
			for (int x = 0; x < FB_W; x++)
				if (fb[y][x] != PAPER) {
					x0 = x < x0 ? x : x0;
					x1 = x > x1 ? x : x1;
					y0 = y < y0 ? y : y0;
					y1 = y > y1 ? y : y1;
				}
		CHECK(y0 >= 10 && y1 < 10 + h, "splash rows %d..%d", y0, y1);
		CHECK(x1 - x0 + 1 <= art_splash_w(k), "splash columns %d..%d", x0, x1);
		CHECK(abs((x0 + x1) - (FB_W - 1)) <= 2 * k, "splash not centered: %d..%d", x0, x1);
	}
	// Mono: only fg and bg
	clear();
	art_splash(FB_W / 2, 0, 1, true, 0x0003, 0x0004);
	int other = 0;
	for (int y = 0; y < FB_H; y++)
		for (int x = 0; x < FB_W; x++)
			other += fb[y][x] != PAPER && fb[y][x] != 0x0003 && fb[y][x] != 0x0004;
	CHECK(other == 0, "mono splash has %d pixels of other colors", other);
}

// --- the whole boot screen (splash_plus.c) on stand-ins for gfx, the layout and sbuf (art_host_ui.h) ---

static int panel_w = FB_W, panel_h = FB_H, panel_bpp = 4;
static const font_t f_tiny = { 8, 16 }, f_small = { 9, 18 }, f_medium = { 12, 24 };
ui_layout_t ui_layout = { &f_tiny, &f_small, &f_medium };
ui_colors_t ui_col = { 0x0004, 0x0003, 0x0005 };

#define MAX_TEXTS	16
static struct {
	const font_t *f;
	int x, y, w;
	color_t c;
	char s[48];
} texts[MAX_TEXTS];
static int n_texts;

int gfx_width(void) { return panel_w; }
int gfx_height(void) { return panel_h; }
int gfx_bpp(void) { return panel_bpp; }

void gfx_clear(color_t c)
{
	for (int y = 0; y < FB_H; y++)
		for (int x = 0; x < FB_W; x++)
			fb[y][x] = x < panel_w && y < panel_h ? c : PAPER;
}

void gfx_hline(int x, int y, int w, color_t c)
{
	gfx_fill(x, y, w, 1, c);
}

int gfx_text_width(const font_t *f, const char *s)
{
	return f->w * (int)strlen(s);
}

// Each glyph a solid cell, so the text's box is what the frame shows
int gfx_text(const font_t *f, int x, int y, const char *s, color_t fg)
{
	int w = gfx_text_width(f, s);
	if (n_texts < MAX_TEXTS) {
		texts[n_texts].f = f;
		texts[n_texts].x = x;
		texts[n_texts].y = y;
		texts[n_texts].w = w;
		texts[n_texts].c = fg;
		snprintf(texts[n_texts].s, sizeof texts[n_texts].s, "%s", s);
		n_texts++;
	}
	if (w > 0)
		gfx_fill(x, y, w, f->h, fg);
	if (x < 0 || y < 0 || x + w > panel_w || y + f->h > panel_h)
		outside++;
	return x + w;
}

void sb_init(sbuf_t *b, char *buf, size_t len)
{
	b->p = buf;
	b->end = buf + len - 1;
	*b->p = 0;
}

void sb_str(sbuf_t *b, const char *s)
{
	while (*s && b->p < b->end)
		*b->p++ = *s++;
	*b->p = 0;
}

static int find_text(const char *s)
{
	for (int i = 0; i < n_texts; i++)
		if (strstr(texts[i].s, s) != NULL)
			return i;
	return -1;
}

static void test_splash_screen(int w, int h, int bpp, const char *line1, const char *line2)
{
	clear();
	n_texts = 0;
	panel_w = w;
	panel_h = h;
	panel_bpp = bpp;
	bool drawn = ui_draw_splash_plus("DM-32UV", "dm32-plus-0.1.0-dev", line1, line2);
	CHECK(drawn, "%dx%d: splash not drawn", w, h);
	if (!drawn)
		return;
	CHECK(outside == 0, "%dx%d: splash drew outside the panel", w, h);
	// The callsign: tiny font, bottom line, centred, inside the panel, in the accent color (mono: fg)
	int c = find_text(ART_CALLSIGN);
	CHECK(c >= 0, "%dx%d: no %s on the splash", w, h, ART_CALLSIGN);
	if (c < 0)
		return;
	const font_t *f = texts[c].f;
	CHECK(f == ui_layout.tiny, "%dx%d: callsign not in the tiny font", w, h);
	CHECK(strcmp(texts[c].s, ART_CALLSIGN) == 0, "%dx%d: callsign line is \"%s\"", w, h, texts[c].s);
	CHECK(texts[c].y + f->h == h, "%dx%d: callsign at y %d, not on the bottom line", w, h, texts[c].y);
	CHECK(abs(2 * texts[c].x + texts[c].w - w) <= 1, "%dx%d: callsign not centred (x %d, width %d)", w, h,
	      texts[c].x, texts[c].w);
	CHECK(texts[c].x >= 0 && texts[c].x + texts[c].w <= w, "%dx%d: callsign outside the panel", w, h);
	CHECK(texts[c].c == (bpp == 1 ? ui_col.fg : art_accent()), "%dx%d: callsign color %04x", w, h, texts[c].c);
	// The footer (board and version) right above it, and nothing else on the callsign's line
	int ft = find_text("dm32-plus");
	CHECK(ft >= 0, "%dx%d: no footer", w, h);
	if (ft < 0)
		return;
	CHECK(texts[ft].y + texts[ft].f->h <= texts[c].y, "%dx%d: footer overlaps the callsign", w, h);
	int stray = 0;
	for (int y = texts[c].y; y < h; y++)
		for (int x = 0; x < w; x++)
			stray += fb[y][x] != ui_col.bg && (x < texts[c].x || x >= texts[c].x + texts[c].w);
	CHECK(stray == 0, "%dx%d: %d other pixels on the callsign's line", w, h, stray);
	// The boot text lines and the art stay above the footer
	for (int i = 0; i < n_texts; i++)
		if (i != c && i != ft)
			CHECK(texts[i].y + texts[i].f->h <= texts[ft].y, "%dx%d: \"%s\" runs into the footer", w, h, texts[i].s);
	for (int y = texts[ft].y; y < h; y++)
		for (int x = 0; x < w; x++)
			if (fb[y][x] != ui_col.bg && fb[y][x] != ui_col.dim && fb[y][x] != texts[c].c)
				stray++;
	CHECK(stray == 0, "%dx%d: the art runs into the footer", w, h);
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "art";
	test_icons(dir);
	test_mark(dir);
	test_wordmark(dir);
	test_splash();
	test_splash_screen(240, 320, 4, "TA0XXX", "P25 RX");	// DM-32UV
	test_splash_screen(240, 320, 4, "", "");
	test_splash_screen(160, 128, 4, "Line one", "Line two");	// 1x art, the lines left out where they don't fit
	test_splash_screen(160, 128, 1, "", "");			// mono
	panel_w = 100;
	panel_h = 60;
	CHECK(!ui_draw_splash_plus("X", "v", "", ""), "a panel too small for the art still drew it");
	printf("art: %d icons, %d bytes of tables: %s\n", ART_ICON_COUNT, ART_DATA_BYTES, fails ? "FAILED" : "ok");
	return fails != 0;
}
