/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus SSTV send (CONFIG_SSTV_TX, docs/plus.md "SSTV send"): the audio of one built-in test picture, made on the
 * fly a few samples at a time (no picture buffer). Installed as core/radio/sstv_tx.{c,h}. Pure integer code, no RTOS,
 * no board: the radio task glue is radio_sstvtx.c, the screen ui_sstvtx.c, the host test tests/plus/test_sstvtx.c
 * (the audio decoded again by the SSTV receiver, sstv.c).
 *
 * The picture (320 x 240, the card): a dark blue card with the DDM32 Plus mark (the splash's, 2x) and the wordmark
 * split as the logo shows it ("DDM32" 4x, a small "+" at its top right, "Plus" under it, both in the accent colour),
 * an accent rule, the callsign large in the middle (a 5 x 7 face scaled up to 8x) and eight colour bars at the bottom
 * (white, yellow, cyan, green, magenta, red, blue, black). Each pixel is worked out when the scan reaches it.
 *
 * Modes (timings from JL Barber, "Proposal for SSTV mode specifications", Dayton 2000; the same tables the receiver's
 * host test encodes with):
 *   Robot 36 (VIS 8): 240 lines of 150 ms: sync 1200 Hz 9 ms, porch 1500 Hz 3 ms, Y 88 ms (320 px), separator 4.5 ms
 *     (1500 Hz on even lines, 2300 Hz on odd), porch 1900 Hz 1.5 ms, chroma 44 ms (160 px: R-Y on even lines, B-Y on
 *     odd). 37.5 s on the air with the lead, the VIS and the tail.
 *   Martin M1 (VIS 44): 256 lines of 446.446 ms: sync 1200 Hz 4.862 ms, then 0.572 ms 1500 Hz before G, B and R
 *     (146.432 ms, 320 px each) and after R. 1 min 56 s.
 *   PD120 (VIS 95): 248 lines of 508.48 ms, each two picture lines: sync 1200 Hz 20 ms, porch 1500 Hz 2.08 ms, Y of
 *     the first, R-Y and B-Y of both (averaged), Y of the second (121.6 ms, 640 px each). 2 min 8 s.
 * Video 1500 Hz (black) .. 2300 Hz (white); YCrCb as ITU-R BT.601 full range (the receiver's inverse).
 * Before the VIS 250 ms of silence (the receiver's squelch opens on the carrier), after the picture 300 ms of 1500 Hz.
 * VIS: 1900 Hz 300 ms, 1200 Hz 10 ms, 1900 Hz 300 ms, the start bit 1200 Hz 30 ms, 7 data bits LSB first and even
 * parity (1100 Hz = 1, 1300 Hz = 0, 30 ms each), the stop bit 1200 Hz 30 ms.
 *
 * Audio: 8 kHz samples from a 32-bit phase accumulator (a frequency change keeps the phase: no clicks) and a 65-entry
 * quarter sine. Time is kept in nanoseconds inside the element being sent (a tone, or one pixel of a scan: every
 * mode's pixel lasts a whole number of ns), so no element's error builds up: each ends within one sample (125 us) of
 * its exact time, the picture's total within one sample of its nominal length.
 *
 * RAM: sstv_tx_t (48 bytes; the radio glue takes it from the heap while it sends). CPU: a table read and a few
 * adds a sample, the card's pixel once per pixel (about every 1.5 to 2.9 samples). */
#ifndef SSTV_TX_H
#define SSTV_TX_H

#include <stdbool.h>
#include <stdint.h>

#define SSTV_TX_FS		8000u	/* samples a second */
#define SSTV_TX_CARD_W		320u	/* the card's own size (each mode scales it to its picture) */
#define SSTV_TX_CARD_H		240u
#define SSTV_TX_CALL_MAX	7u	/* a callsign as radio_cwid_callsign gives it: 4 to 7 letters and digits */

enum { SSTV_TX_ROBOT36, SSTV_TX_MARTIN1, SSTV_TX_PD120, SSTV_TX_MODES };

/* The card's callsign and where it goes */
typedef struct {
	char call[SSTV_TX_CALL_MAX + 1];
	uint8_t n, scale;	/* letters; each 5 x 7 glyph pixel a scale x scale block */
	uint16_t x0, y0;
} sstv_card_t;

typedef struct {
	sstv_card_t card;
	uint8_t mode;
	uint8_t part;		/* the header (silence, VIS), the lines, the tail, done */
	uint8_t el;		/* the element of the part */
	uint16_t line;		/* the transmitted line */
	uint16_t x;		/* the pixel of a scan */
	uint16_t cy0, cy1;	/* the card rows of the line (PD: its two picture lines) */
	uint32_t pos;		/* ns into the element (a scan: into its pixel) */
	uint32_t dur;		/* the element's (pixel's) length, ns */
	uint32_t phase, step;	/* the oscillator: phase and step a sample (2^32 = one cycle); step 0 = silence */
	uint32_t samples;	/* made so far */
} sstv_tx_t;

/* The card for a callsign (upper case letters and digits; others are skipped; at most SSTV_TX_CALL_MAX) */
void sstv_card_init(sstv_card_t *c, const char *call);
/* The card's colour at x, y (0 .. SSTV_TX_CARD_W - 1, 0 .. SSTV_TX_CARD_H - 1): 8-bit RGB */
void sstv_card_rgb(const sstv_card_t *c, uint16_t x, uint16_t y, uint8_t rgb[3]);

/* Start mode (SSTV_TX_*) with the card of call */
void sstv_tx_init(sstv_tx_t *g, uint8_t mode, const char *call);
/* The next samples, at most n, peak amp: how many were written (fewer than n only at the end, then 0) */
uint16_t sstv_tx_pcm(sstv_tx_t *g, int16_t *out, uint16_t n, int16_t amp);
/* The transmitted line being sent (0 .. sstv_tx_lines - 1; sstv_tx_lines once the picture is over) */
uint16_t sstv_tx_line(const sstv_tx_t *g);
bool sstv_tx_done(const sstv_tx_t *g);

/* Mode facts: its name ("Robot 36"), VIS code, transmitted lines, the whole transmission's samples and ms */
const char *sstv_tx_mode_name(uint8_t mode);
uint8_t sstv_tx_vis(uint8_t mode);
uint16_t sstv_tx_lines(uint8_t mode);
uint32_t sstv_tx_samples(uint8_t mode);
uint32_t sstv_tx_ms(uint8_t mode);

#endif
