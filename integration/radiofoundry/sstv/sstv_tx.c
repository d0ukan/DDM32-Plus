/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus SSTV send: the test picture's audio (sstv_tx.h). Written for this project from the published mode
 * timings; no code from other encoders. */
#include "config.h"
#if CONFIG_SSTV_TX
#include "radio/sstv_tx.h"
#include "ui/art_data.h"	/* the splash mark and the wordmark (DDM32 Plus art, generated from the art/ text files) */

#define NS_SAMPLE	125000u		/* 1 / 8000 s */
#define MS		1000000u	/* ns */
#define STEP_HZ		536871u		/* phase step of 1 Hz: 2^32 / 8000 */
#define STEP_BLACK	(1500u * STEP_HZ)
#define STEP_LEVEL	1684301u	/* 800 / 255 Hz a level */

enum { P_HEAD, P_LINES, P_TAIL, P_DONE };
enum { K_TONE, K_SEP, K_SCAN };
/* what a scan sends: Y of the line's first / second picture line, R-Y, B-Y (PD: of both lines), Robot's chroma (R-Y on
 * even lines, B-Y on odd), R, G, B */
enum { C_Y0, C_Y1, C_RY2, C_BY2, C_ROBOT, C_R, C_G, C_B };

typedef struct {
	uint8_t kind, ch;
	uint16_t arg;		/* a tone's Hz, a scan's pixels */
	uint32_t ns;		/* a tone's length, a scan pixel's */
} el_t;

typedef struct {
	const char *name;
	uint8_t vis, nel;
	uint16_t lines, h;	/* transmitted lines, picture lines */
	const el_t *el;
} txmode_t;

static const el_t robot36[] = {
	{ K_TONE, 0, 1200, 9 * MS }, { K_TONE, 0, 1500, 3 * MS }, { K_SCAN, C_Y0, 320, 275000 },
	{ K_SEP, 0, 0, 4500000 }, { K_TONE, 0, 1900, 1500000 }, { K_SCAN, C_ROBOT, 160, 275000 },
};
static const el_t martin1[] = {
	{ K_TONE, 0, 1200, 4862000 }, { K_TONE, 0, 1500, 572000 }, { K_SCAN, C_G, 320, 457600 },
	{ K_TONE, 0, 1500, 572000 }, { K_SCAN, C_B, 320, 457600 }, { K_TONE, 0, 1500, 572000 },
	{ K_SCAN, C_R, 320, 457600 }, { K_TONE, 0, 1500, 572000 },
};
static const el_t pd120[] = {
	{ K_TONE, 0, 1200, 20 * MS }, { K_TONE, 0, 1500, 2080000 }, { K_SCAN, C_Y0, 640, 190000 },
	{ K_SCAN, C_RY2, 640, 190000 }, { K_SCAN, C_BY2, 640, 190000 }, { K_SCAN, C_Y1, 640, 190000 },
};
#define N(a) (uint8_t)(sizeof a / sizeof a[0])
static const txmode_t modes[SSTV_TX_MODES] = {
	[SSTV_TX_ROBOT36] = { "Robot 36", 8, N(robot36), 240, 240, robot36 },
	[SSTV_TX_MARTIN1] = { "Martin M1", 44, N(martin1), 256, 256, martin1 },
	[SSTV_TX_PD120] = { "PD120", 95, N(pd120), 248, 496, pd120 },
};

/* The header: 250 ms of silence, then the VIS (leader, break, leader, start bit, 7 bits, parity, stop bit) */
#define HEAD_N		14u
#define TAIL_MS		300u
#define LEAD_MS		250u

/* sin(2 pi i / 256) x 32767 for i = 0 .. 64 */
static const int16_t quarter[65] = {
	0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512,
	10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868,
	19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319,
	26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113,
	31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767,
};

static int32_t sine(uint32_t phase)
{
	uint8_t i = (uint8_t)(phase >> 24), q = i & 63u;
	switch (i >> 6) {
	case 0: return quarter[q];
	case 1: return quarter[64u - q];
	case 2: return -quarter[q];
	default: return -quarter[64u - q];
	}
}

/* --- The card --- */

/* 5 x 7 glyphs of 0-9 and A-Z: a row a byte, bit 4 the leftmost pixel */
static const uint8_t glyphs[36][7] = {
	{ 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
	{ 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
	{ 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
	{ 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
	{ 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
	{ 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },	/* A B */
	{ 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E }, { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C },	/* C D */
	{ 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },	/* E F */
	{ 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F }, { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },	/* G H */
	{ 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C },	/* I J */
	{ 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },	/* K L */
	{ 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 },	/* M N */
	{ 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },	/* O P */
	{ 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },	/* Q R */
	{ 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },	/* S T */
	{ 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },	/* U V */
	{ 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A }, { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },	/* W X */
	{ 0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04 }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },	/* Y Z */
};

/* The layout (card pixels) */
#define MARK_X		14
#define MARK_Y		12
#define MARK_S		2
#define WORD_X		104
#define WORD_Y		20
#define WORD_S		4
#define DDM32_COLS	46	/* wordmark columns 0..45: "DDM32" */
#define PLUS_C0		48	/* 48..53: the superscript "+" */
#define PLUS_COLS	6
#define PLUS_ROWS	6
#define PLUS_X		(WORD_X + DDM32_COLS * WORD_S + 4)
#define LUS_C0		59	/* 59..86: "Plus" */
#define LUS_COLS	28
#define LUS_X		(WORD_X + (DDM32_COLS - LUS_COLS) * WORD_S)	/* right aligned under "DDM32" */
#define LUS_Y		(WORD_Y + ART_WORD_H * WORD_S + 8)
#define RULE_Y		132
#define CALL_TOP	138
#define CALL_H		62
#define BARS_Y		204

static const uint8_t navy[3] = { 16, 28, 64 }, white[3] = { 255, 255, 255 };
static const uint8_t bars[8][3] = { { 255, 255, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 0, 255, 0 },
				    { 255, 0, 255 }, { 255, 0, 0 }, { 0, 0, 255 }, { 0, 0, 0 } };

static void put(uint8_t rgb[3], const uint8_t c[3])
{
	rgb[0] = c[0];
	rgb[1] = c[1];
	rgb[2] = c[2];
}

static void put565(uint8_t rgb[3], uint16_t v)
{
	rgb[0] = (uint8_t)((v >> 11) << 3 | v >> 13);
	rgb[1] = (uint8_t)(((v >> 5) & 63u) << 2 | ((v >> 9) & 3u));
	rgb[2] = (uint8_t)((v & 31u) << 3 | ((v >> 2) & 7u));
}

static bool word_px(int r, int c)
{
	return (art_word_bits[r * ART_WORD_STRIDE + c / 8] >> (7 - c % 8)) & 1u;
}

void sstv_card_init(sstv_card_t *c, const char *call)
{
	uint8_t n = 0;
	for (const char *p = call; p != 0 && *p != '\0' && n < SSTV_TX_CALL_MAX; p++)
		if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z'))
			c->call[n++] = *p;
	c->call[n] = '\0';
	c->n = n;
	uint16_t w = n != 0 ? (uint16_t)(6u * n - 1u) : 1u;	/* glyph pixels across, one between letters */
	uint16_t s = (uint16_t)((SSTV_TX_CARD_W - 12u) / w);
	c->scale = (uint8_t)(s > 8u ? 8u : s < 1u ? 1u : s);
	c->x0 = (uint16_t)((SSTV_TX_CARD_W - w * c->scale) / 2u);
	c->y0 = (uint16_t)(CALL_TOP + (CALL_H - 7u * c->scale) / 2u);
}

void sstv_card_rgb(const sstv_card_t *c, uint16_t x, uint16_t y, uint8_t rgb[3])
{
	uint16_t accent = art_mark_rgb[0];
	if (y >= BARS_Y) {
		put(rgb, bars[x / (SSTV_TX_CARD_W / 8u) & 7u]);
		return;
	}
	put(rgb, navy);
	if (y < RULE_Y) {
		/* the mark */
		if (x >= MARK_X && x < MARK_X + ART_MARK_W * MARK_S && y >= MARK_Y && y < MARK_Y + ART_MARK_H * MARK_S) {
			unsigned sx = (x - MARK_X) / MARK_S, sy = (y - MARK_Y) / MARK_S;
			unsigned k = (art_mark_bits[sy * ART_MARK_STRIDE + sx / 4u] >> (6u - 2u * (sx & 3u))) & 3u;
			if (k != 0) {
				if (art_mark_fg[k - 1u])
					put(rgb, white);
				else
					put565(rgb, art_mark_rgb[k - 1u]);
			}
			return;
		}
		/* "DDM32", its "+", "Plus" */
		if (y >= WORD_Y && y < WORD_Y + ART_WORD_H * WORD_S) {
			int r = (y - WORD_Y) / WORD_S;
			if (x >= WORD_X && x < WORD_X + DDM32_COLS * WORD_S) {
				if (word_px(r, (x - WORD_X) / WORD_S))
					put(rgb, white);
				return;
			}
			if (x >= PLUS_X && x < PLUS_X + PLUS_COLS * WORD_S && r < PLUS_ROWS) {
				if (word_px(r, PLUS_C0 + (x - PLUS_X) / WORD_S))
					put565(rgb, accent);
				return;
			}
		}
		if (y >= LUS_Y && y < LUS_Y + ART_WORD_H * WORD_S && x >= LUS_X && x < LUS_X + LUS_COLS * WORD_S) {
			if (word_px((y - LUS_Y) / WORD_S, LUS_C0 + (x - LUS_X) / WORD_S))
				put565(rgb, accent);
		}
		return;
	}
	if (y < RULE_Y + 2u) {
		if (x >= 12u && x < SSTV_TX_CARD_W - 12u)
			put565(rgb, accent);
		return;
	}
	/* the callsign */
	unsigned cell = 6u * c->scale;
	if (c->n == 0 || x < c->x0 || y < c->y0 || y >= c->y0 + 7u * c->scale)
		return;
	unsigned i = (x - c->x0) / cell, col = (x - c->x0) % cell / c->scale, row = (y - c->y0) / c->scale;
	if (i >= c->n || col >= 5u)
		return;
	char ch = c->call[i];
	unsigned g = ch >= 'A' ? 10u + (unsigned)(ch - 'A') : (unsigned)(ch - '0');
	if ((glyphs[g][row] >> (4u - col)) & 1u)
		put(rgb, white);
}

/* --- The transmission --- */

static const txmode_t *mode_of(const sstv_tx_t *g)
{
	return &modes[g->mode < SSTV_TX_MODES ? g->mode : 0];
}

/* The header's element i: Hz (0: silence) and ns */
static uint16_t head_el(uint8_t vis, uint8_t i, uint32_t *ns)
{
	*ns = 30u * MS;
	switch (i) {
	case 0: *ns = LEAD_MS * MS; return 0;
	case 1: case 3: *ns = 300u * MS; return 1900;
	case 2: *ns = 10u * MS; return 1200;
	case 4: case 13: return 1200;
	case 12: {
		uint8_t ones = 0;
		for (uint8_t b = 0; b < 7u; b++)
			ones = (uint8_t)(ones + ((vis >> b) & 1u));
		return ones & 1u ? 1100 : 1300;	/* even parity */
	}
	default: return (vis >> (i - 5u)) & 1u ? 1100 : 1300;
	}
}

/* The step of the scan pixel the sender has reached */
static uint32_t pixel_step(const sstv_tx_t *g, const el_t *e)
{
	uint16_t nx = e->arg < SSTV_TX_CARD_W ? 2u : 1u;	/* Robot's chroma: two card pixels to one */
	uint16_t cx = (uint16_t)((uint32_t)g->x * SSTV_TX_CARD_W / e->arg);
	bool pair = e->ch == C_RY2 || e->ch == C_BY2;
	uint16_t sum[3] = { 0, 0, 0 };
	for (uint16_t j = 0; j < (pair ? 2u : 1u); j++)
		for (uint16_t i = 0; i < nx; i++) {
			uint8_t c[3];
			sstv_card_rgb(&g->card, (uint16_t)(cx + i), e->ch == C_Y1 || j != 0 ? g->cy1 : g->cy0, c);
			sum[0] = (uint16_t)(sum[0] + c[0]);
			sum[1] = (uint16_t)(sum[1] + c[1]);
			sum[2] = (uint16_t)(sum[2] + c[2]);
		}
	uint16_t div = (uint16_t)(nx * (pair ? 2u : 1u));
	int32_t r = sum[0] / div, gr = sum[1] / div, b = sum[2] / div;
	int32_t y = (77 * r + 150 * gr + 29 * b + 128) >> 8, v;
	uint8_t ch = e->ch == C_ROBOT ? ((g->line & 1u) ? C_BY2 : C_RY2) : e->ch;
	switch (ch) {
	case C_RY2: v = ((r - y) * 183 + 32896) >> 8; break;	/* 128 + (R - Y) / 1.402 */
	case C_BY2: v = ((b - y) * 144 + 32896) >> 8; break;	/* 128 + (B - Y) / 1.772 */
	case C_R: v = r; break;
	case C_G: v = gr; break;
	case C_B: v = b; break;
	default: v = y; break;
	}
	if (v > 255)
		v = 255;
	return STEP_BLACK + (uint32_t)v * STEP_LEVEL;
}

/* The card rows of the line about to be sent */
static void line_rows(sstv_tx_t *g)
{
	const txmode_t *m = mode_of(g);
	uint32_t y = m->h > m->lines ? 2u * g->line : g->line;
	g->cy0 = (uint16_t)(y * SSTV_TX_CARD_H / m->h);
	g->cy1 = (uint16_t)((y + 1u) * SSTV_TX_CARD_H / m->h);
	if (g->cy1 >= SSTV_TX_CARD_H)
		g->cy1 = SSTV_TX_CARD_H - 1u;
}

/* The element g->part / g->el begins: its length and its first step */
static void begin(sstv_tx_t *g)
{
	const txmode_t *m = mode_of(g);
	uint16_t hz = 0;
	switch (g->part) {
	case P_HEAD:
		hz = head_el(m->vis, g->el, &g->dur);
		break;
	case P_LINES: {
		const el_t *e = &m->el[g->el];
		g->dur = e->ns;
		if (e->kind == K_SCAN) {
			g->x = 0;
			g->step = pixel_step(g, e);
			return;
		}
		hz = e->kind == K_SEP ? ((g->line & 1u) ? 2300u : 1500u) : e->arg;
		break;
	}
	case P_TAIL:
		g->dur = TAIL_MS * MS;
		hz = 1500;
		break;
	default:
		g->dur = 1;
		g->step = 0;
		return;
	}
	g->step = (uint32_t)hz * STEP_HZ;
}

/* The element (or the scan's pixel) is over: on to the next */
static void advance(sstv_tx_t *g)
{
	const txmode_t *m = mode_of(g);
	if (g->part == P_LINES) {
		const el_t *e = &m->el[g->el];
		if (e->kind == K_SCAN && ++g->x < e->arg) {
			g->step = pixel_step(g, e);
			return;
		}
		if (++g->el >= m->nel) {
			g->el = 0;
			if (++g->line >= m->lines)
				g->part = P_TAIL;
			else
				line_rows(g);
		}
	} else if (g->part == P_HEAD) {
		if (++g->el >= HEAD_N) {
			g->el = 0;
			g->part = P_LINES;
			g->line = 0;
			line_rows(g);
		}
	} else {
		g->part = P_DONE;
	}
	begin(g);
}

void sstv_tx_init(sstv_tx_t *g, uint8_t mode, const char *call)
{
	sstv_card_init(&g->card, call);
	g->mode = mode < SSTV_TX_MODES ? mode : SSTV_TX_ROBOT36;
	g->part = P_HEAD;
	g->el = 0;
	g->line = 0;
	g->x = 0;
	g->cy0 = g->cy1 = 0;
	g->pos = 0;
	g->phase = 0;
	g->samples = 0;
	begin(g);
}

uint16_t sstv_tx_pcm(sstv_tx_t *g, int16_t *out, uint16_t n, int16_t amp)
{
	uint16_t k = 0;
	while (k < n && g->part != P_DONE) {
		out[k++] = g->step != 0 ? (int16_t)((sine(g->phase) * amp) >> 15) : 0;
		g->phase += g->step;
		g->pos += NS_SAMPLE;
		while (g->part != P_DONE && g->pos >= g->dur) {
			g->pos -= g->dur;
			advance(g);
		}
		g->samples++;
	}
	return k;
}

uint16_t sstv_tx_line(const sstv_tx_t *g)
{
	return g->part == P_HEAD ? 0 : g->part == P_LINES ? g->line : mode_of(g)->lines;
}

bool sstv_tx_done(const sstv_tx_t *g)
{
	return g->part == P_DONE;
}

const char *sstv_tx_mode_name(uint8_t mode)
{
	return mode < SSTV_TX_MODES ? modes[mode].name : "";
}

uint8_t sstv_tx_vis(uint8_t mode)
{
	return mode < SSTV_TX_MODES ? modes[mode].vis : 0;
}

uint16_t sstv_tx_lines(uint8_t mode)
{
	return mode < SSTV_TX_MODES ? modes[mode].lines : 0;
}

uint32_t sstv_tx_samples(uint8_t mode)
{
	if (mode >= SSTV_TX_MODES)
		return 0;
	const txmode_t *m = &modes[mode];
	uint64_t ns = 0, line = 0;
	for (uint8_t i = 0; i < HEAD_N; i++) {
		uint32_t d;
		head_el(m->vis, i, &d);
		ns += d;
	}
	for (uint8_t i = 0; i < m->nel; i++)
		line += (uint64_t)m->el[i].ns * (m->el[i].kind == K_SCAN ? m->el[i].arg : 1u);
	ns += line * m->lines + (uint64_t)TAIL_MS * MS;
	return (uint32_t)((ns + NS_SAMPLE - 1u) / NS_SAMPLE);	/* every sample whose time falls inside */
}

uint32_t sstv_tx_ms(uint8_t mode)
{
	return sstv_tx_samples(mode) / (SSTV_TX_FS / 1000u);
}
#endif
