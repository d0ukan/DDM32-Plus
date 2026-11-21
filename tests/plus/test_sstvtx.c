// SPDX-License-Identifier: GPL-3.0-or-later
// DDM32 Plus SSTV send (integration/radiofoundry/sstv/sstv_tx.c): the test picture's audio, made in the radio's
// 32-sample chunks, decoded again by the SSTV receiver (sstv.c) as 8 kHz receive audio. For Robot 36, Martin M1 and
// PD120: the VIS gives the mode, every picture line arrives, the decoded picture matches the card (mean error per
// colour channel against the card averaged over each decoded pixel), the callsign reads back glyph cell by glyph cell,
// the colour bars keep their colours; Robot 36 also through an FM receiver's de-emphasis and 300 Hz high pass. The
// audio: its length within one sample of the mode's timing, the same in any chunk size, no phase jump anywhere (a
// sample never moves more than a 2300 Hz tone can), the VIS's tones and bit times where the specification puts them.
// The card: the callsign's scale and place for 4 to 7 letters, the logo's pixels from the splash art.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sstv.h"
#include "sstv_tx.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MAXS (8000 * 132)
#define AMP 12000
static int16_t audio[MAXS], other[MAXS];
static int ns;

static int generate(int16_t *buf, uint8_t mode, const char *call, uint16_t chunk)
{
	static sstv_tx_t g;
	sstv_tx_init(&g, mode, call);
	int n = 0;
	for (;;) {
		uint16_t want = (uint16_t)(MAXS - n < chunk ? MAXS - n : chunk);
		uint16_t got = sstv_tx_pcm(&g, &buf[n], want, AMP);
		n += got;
		if (got < want || n >= MAXS)
			break;
	}
	CHECK(sstv_tx_done(&g) && sstv_tx_line(&g) == sstv_tx_lines(mode), "%s: not done", sstv_tx_mode_name(mode));
	CHECK(sstv_tx_pcm(&g, buf, 1, AMP) == 0, "samples after the end");
	return n;
}

// --- the decoder's output ---
static uint8_t got[496][SSTV_W][3];
static bool have[496];
static void on_row(void *ctx, const sstv_t *d, uint16_t line, uint8_t sub)
{
	(void)ctx;
	if (line >= 496)
		return;
	for (uint16_t x = 0; x < SSTV_W; x++)
		sstv_rgb(d, sub, x, got[line][x]);
	have[line] = true;
}

static sstv_t dec;
static void decode(void)
{
	memset(have, 0, sizeof have);
	sstv_init(&dec, on_row, NULL);
	for (int i = 0; i < ns; i += 32)
		sstv_audio(&dec, &audio[i], (size_t)(ns - i < 32 ? ns - i : 32));
	int16_t quiet[400] = { 0 };
	for (int i = 0; i < 10; i++)
		sstv_audio(&dec, quiet, 400);
}

// an FM receiver's audio: 750 us de-emphasis and a 300 Hz high pass
static void deemphasis(void)
{
	double a = exp(-1.0 / (8000 * 750e-6)), y = 0, hp_x = 0, hp_y = 0, b = exp(-2 * M_PI * 300 / 8000.0);
	for (int i = 0; i < ns; i++) {
		y = a * y + (1 - a) * audio[i];
		double h = b * (hp_y + y * 3 - hp_x);
		hp_x = y * 3;
		hp_y = h;
		audio[i] = (int16_t)(h > 32767 ? 32767 : h < -32768 ? -32768 : lrint(h));
	}
}

static int picture_lines(uint8_t mode)
{
	return mode == SSTV_TX_PD120 ? 496 : mode == SSTV_TX_MARTIN1 ? 256 : 240;
}
static int rx_mode(uint8_t mode)
{
	return mode == SSTV_TX_PD120 ? SSTV_PD120 : mode == SSTV_TX_MARTIN1 ? SSTV_MARTIN1 : SSTV_ROBOT36;
}

// The card's mean colour over decoded pixel xd of picture line yp
static void card_cell(const sstv_card_t *c, int h, int yp, int xd, double out[3])
{
	int y = yp * 240 / h, x0 = xd * 320 / 240, x1 = (xd + 1) * 320 / 240;
	out[0] = out[1] = out[2] = 0;
	for (int x = x0; x < x1; x++) {
		uint8_t rgb[3];
		sstv_card_rgb(c, (uint16_t)x, (uint16_t)y, rgb);
		for (int k = 0; k < 3; k++)
			out[k] += rgb[k] / (double)(x1 - x0);
	}
}

static double luma(const uint8_t *p)
{
	return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
}

// The decoded picture against the card: lines received, mean error, the callsign's cells, the bars
static void compare(uint8_t mode, const char *call, const char *what, double max_mae)
{
	const char *name = sstv_tx_mode_name(mode);
	int h = picture_lines(mode), lines = 0;
	sstv_card_t c;
	sstv_card_init(&c, call);
	double err = 0;
	long cnt = 0;
	for (int l = 0; l < h; l++) {
		if (!have[l])
			continue;
		lines++;
		for (int x = 0; x < (int)SSTV_W; x++) {
			double want[3];
			card_cell(&c, h, l, x, want);
			for (int k = 0; k < 3; k++)
				err += fabs(got[l][x][k] - want[k]);
			cnt += 3;
		}
	}
	err /= cnt ? (double)cnt : 1;
	CHECK(dec.mode == rx_mode(mode), "%s %s: decoded as mode %d", name, what, dec.mode);
	CHECK(dec.state == SSTV_DONE, "%s %s: state %d", name, what, dec.state);
	CHECK(lines == h, "%s %s: %d of %d lines", name, what, lines, h);
	CHECK(err <= max_mae, "%s %s: mean abs error %.1f > %.1f", name, what, err, max_mae);
	// the callsign, cell by cell: set cells bright, clear ones dark (white on dark blue)
	int bad = 0, cells = 0;
	for (int i = 0; i < c.n; i++)
		for (int r = 0; r < 7; r++)
			for (int col = 0; col < 5; col++) {
				int cx = c.x0 + i * 6 * c.scale + col * c.scale + c.scale / 2;
				int cy = c.y0 + r * c.scale + c.scale / 2;
				int yp = (cy * h + 120) / 240, xd = cx * 240 / 320;
				uint8_t want[3];
				sstv_card_rgb(&c, (uint16_t)cx, (uint16_t)cy, want);
				bool on = want[0] > 128;
				double l = luma(got[yp][xd]);
				bad += on ? l < 170 : l > 90;
				cells++;
			}
	CHECK(bad == 0, "%s %s: %d of %d callsign cells wrong", name, what, bad, cells);
	// the bars: their colours in the middle of each
	int yp = 222 * h / 240, worst = 0;
	static const int bars[8][3] = { { 255, 255, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 0, 255, 0 },
					{ 255, 0, 255 }, { 255, 0, 0 }, { 0, 0, 255 }, { 0, 0, 0 } };
	for (int b = 0; b < 8; b++) {
		int xd = (b * 40 + 20) * 240 / 320;
		for (int k = 0; k < 3; k++) {
			int e = abs(got[yp][xd][k] - bars[b][k]);
			worst = e > worst ? e : worst;
		}
	}
	CHECK(worst <= 48, "%s %s: a colour bar %d off", name, what, worst);
	printf("  %-9s %-12s lines %d/%d  mean error %.1f  callsign %s %d/%d cells  bars within %d\n", name, what, lines,
	       h, err, c.call, cells - bad, cells, worst);
}

// Frequency of a stretch of audio by zero crossings (Hz), samples [a, b)
static double tone_hz(const int16_t *s, int a, int b)
{
	// upward crossings, each at its interpolated time
	double t0 = 0, t1 = 0;
	int n = 0;
	for (int i = a + 1; i < b; i++)
		if (s[i - 1] < 0 && s[i] >= 0) {
			double t = i - 1 + (double)-s[i - 1] / (s[i] - s[i - 1]);
			if (n++ == 0)
				t0 = t;
			t1 = t;
		}
	return n < 2 ? 0 : (n - 1) * 8000.0 / (t1 - t0);
}

static void audio_checks(uint8_t mode)
{
	const char *name = sstv_tx_mode_name(mode);
	int n = generate(audio, mode, "TA2DOU", 32);
	CHECK(n == (int)sstv_tx_samples(mode), "%s: %d samples, the timing says %u", name, n, (unsigned)sstv_tx_samples(mode));
	// the nominal length: lead 250 ms + VIS 910 ms + lines + tail 300 ms
	double line_ms = mode == SSTV_TX_ROBOT36 ? 150.0 : mode == SSTV_TX_MARTIN1 ? 446.446 : 508.48;
	double want = (250 + 910 + sstv_tx_lines(mode) * line_ms + 300) * 8.0;
	CHECK(fabs(n - want) <= 1.0, "%s: %d samples, want %.1f", name, n, want);
	CHECK(sstv_tx_ms(mode) == (uint32_t)(n / 8), "%s: ms", name);
	// any chunk size gives the same audio
	int n7 = generate(other, mode, "TA2DOU", 7);
	CHECK(n7 == n && memcmp(audio, other, (size_t)n * sizeof audio[0]) == 0, "%s: chunks of 7 differ", name);
	// no phase jump: no step larger than the highest tone (2300 Hz) can make at full amplitude
	double lim = AMP * 2 * M_PI * 2300 / 8000 * 1.02 + 4;
	int worst = 0, peak = 0;
	for (int i = 2000; i < n; i++) {	/* (after the silent lead) */
		int d = abs(audio[i] - audio[i - 1]);
		worst = d > worst ? d : worst;
		peak = abs(audio[i]) > peak ? abs(audio[i]) : peak;
	}
	CHECK(worst <= lim && peak <= AMP && peak >= AMP - 2, "%s: largest step %d (limit %.0f), peak %d", name, worst, lim,
	      peak);
	// the lead is silent; the VIS's tones where the specification puts them (centres of each part)
	bool quiet = true;
	for (int i = 0; i < 2000; i++)
		quiet = quiet && audio[i] == 0;
	CHECK(quiet, "%s: the lead is not silent", name);
	uint8_t vis = sstv_tx_vis(mode);
	struct { double ms, len, hz; } parts[13] = { { 250, 300, 1900 }, { 550, 10, 1200 }, { 560, 300, 1900 },
						      { 860, 30, 1200 } };
	int ones = 0;
	for (int b = 0; b < 8; b++) {
		int one = b < 7 ? (vis >> b) & 1 : ones & 1;
		ones += b < 7 ? one : 0;
		parts[4 + b].ms = 890 + 30 * b;
		parts[4 + b].len = 30;
		parts[4 + b].hz = one ? 1100 : 1300;
	}
	parts[12].ms = 1130;
	parts[12].len = 30;
	parts[12].hz = 1200;
	for (int k = 0; k < 13; k++) {
		// the middle of the part, leaving 2 ms at each end
		int a = (int)((parts[k].ms + 2) * 8), b = (int)((parts[k].ms + parts[k].len - 2) * 8);
		double f = tone_hz(audio, a, b);
		double tol = parts[k].len < 20 ? 60 : 12;
		CHECK(fabs(f - parts[k].hz) <= tol, "%s: VIS part %d %.0f Hz, want %.0f", name, k, f, parts[k].hz);
	}
	// the first line's sync: 1200 Hz right after the stop bit
	double sync_ms = mode == SSTV_TX_PD120 ? 20 : mode == SSTV_TX_MARTIN1 ? 4.862 : 9;
	double f = tone_hz(audio, 1160 * 8 + 4, (int)((1160 + sync_ms) * 8) - 4);
	CHECK(fabs(f - 1200) <= 40, "%s: first sync %.0f Hz", name, f);
	printf("  %-9s %d samples (%.3f s), steps within %d of %.0f, VIS %u and its tones as specified\n", name, n,
	       n / 8000.0, worst, lim, vis);
}

static void card_checks(void)
{
	static const struct { const char *call; uint8_t n, scale; } t[] = {
		{ "TA2DOU", 6, 8 }, { "K1AB", 4, 8 }, { "TA2DOUX", 7, 7 }, { "ta-2", 1, 8 }, { "", 0, 8 },
	};
	for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
		sstv_card_t c;
		sstv_card_init(&c, t[i].call);
		CHECK(c.n == t[i].n && c.scale == t[i].scale, "card %s: %u letters at %u", t[i].call, c.n, c.scale);
		unsigned w = c.n ? (6u * c.n - 1u) * c.scale : 0;
		CHECK(c.n == 0 || (c.x0 + w <= 320 && c.x0 >= 6 && (320 - w - c.x0) - c.x0 <= 1),
		      "card %s: not centred (x0 %u, w %u)", t[i].call, c.x0, w);
		CHECK(c.y0 + 7u * c.scale <= 204 && c.y0 >= 134, "card %s: rows %u..", t[i].call, c.y0);
	}
	// the logo from the splash art: a white pixel of "D", the accent "+", "Plus", the mark's colours; bars; background
	sstv_card_t c;
	sstv_card_init(&c, "TA2DOU");
	uint8_t p[3];
	sstv_card_rgb(&c, 104 + 1, 20 + 1, p);	/* "D"'s top left stroke */
	CHECK(p[0] == 255 && p[1] == 255 && p[2] == 255, "D not white");
	sstv_card_rgb(&c, 104 + 46 * 4 + 4 + 2 * 4 + 1, 20 + 2 * 4 + 1, p);	/* the "+" bar's middle (column 50, row 2) */
	CHECK(p[0] > 200 && p[1] > 100 && p[1] < 140 && p[2] < 20, "+ not in the accent (%u %u %u)", p[0], p[1], p[2]);
	sstv_card_rgb(&c, 104 + 18 * 4 + 1, 20 + 48 + 8 + 1, p);	/* "P" of "Plus", top left (column 59) */
	CHECK(p[0] > 200 && p[1] > 100 && p[1] < 140, "Plus not in the accent (%u %u %u)", p[0], p[1], p[2]);
	sstv_card_rgb(&c, 97, 50, p);	/* between the mark and the wordmark: the card */
	CHECK(p[0] == 16 && p[1] == 28 && p[2] == 64, "background (%u %u %u)", p[0], p[1], p[2]);
	sstv_card_rgb(&c, 10, 239, p);
	CHECK(p[0] == 255 && p[1] == 255 && p[2] == 255, "first bar white");
	sstv_card_rgb(&c, 300, 239, p);
	CHECK(p[0] == 0 && p[1] == 0 && p[2] == 0, "last bar black");
	int mark_white = 0, mark_accent = 0;
	for (int y = 12; y < 12 + 112; y++)
		for (int x = 14; x < 14 + 76; x++) {
			sstv_card_rgb(&c, (uint16_t)x, (uint16_t)y, p);
			mark_white += p[0] == 255 && p[1] == 255 && p[2] == 255;
			mark_accent += p[0] > 200 && p[1] > 100 && p[1] < 140 && p[2] < 20;
		}
	CHECK(mark_white > 2000 && mark_accent > 50, "mark: %d white, %d accent pixels", mark_white, mark_accent);
	// names and facts
	CHECK(strcmp(sstv_tx_mode_name(SSTV_TX_ROBOT36), sstv_mode_name(SSTV_ROBOT36)) == 0 &&
	      strcmp(sstv_tx_mode_name(SSTV_TX_MARTIN1), sstv_mode_name(SSTV_MARTIN1)) == 0 &&
	      strcmp(sstv_tx_mode_name(SSTV_TX_PD120), sstv_mode_name(SSTV_PD120)) == 0, "mode names");
	CHECK(sstv_tx_vis(SSTV_TX_ROBOT36) == sstv_mode_vis(SSTV_ROBOT36) && sstv_tx_vis(SSTV_TX_MARTIN1) ==
	      sstv_mode_vis(SSTV_MARTIN1) && sstv_tx_vis(SSTV_TX_PD120) == sstv_mode_vis(SSTV_PD120), "VIS codes");
	CHECK(sstv_tx_ms(SSTV_TX_ROBOT36) / 1000 == 37 && sstv_tx_ms(SSTV_TX_MARTIN1) / 1000 == 115 &&
	      sstv_tx_ms(SSTV_TX_PD120) / 1000 == 127, "durations %u %u %u", (unsigned)sstv_tx_ms(0),
	      (unsigned)sstv_tx_ms(1), (unsigned)sstv_tx_ms(2));
	CHECK(sstv_tx_mode_name(SSTV_TX_MODES)[0] == 0 && sstv_tx_samples(SSTV_TX_MODES) == 0, "an unknown mode");
}

int main(void)
{
	printf("Plus SSTV send: the card\n");
	card_checks();
	printf("Plus SSTV send: the audio\n");
	for (uint8_t m = 0; m < SSTV_TX_MODES; m++)
		audio_checks(m);
	printf("Plus SSTV send: round trip through the SSTV receiver\n");
	static const double limit[SSTV_TX_MODES] = { 12, 10, 10 };
	for (uint8_t m = 0; m < SSTV_TX_MODES; m++) {
		ns = generate(audio, m, "TA2DOU", 32);
		decode();
		compare(m, "TA2DOU", "as sent", limit[m]);
	}
	ns = generate(audio, SSTV_TX_ROBOT36, "TA2DOUX", 32);
	deemphasis();
	decode();
	compare(SSTV_TX_ROBOT36, "TA2DOUX", "de-emphasis", 14);
	printf("Plus SSTV send: %d failures\n", fails);
	return fails != 0;
}
