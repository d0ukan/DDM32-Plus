/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus P25 soft probe (docs/p25-soft-probe-design.md): tells within a fraction of a second whether a carrier the
 * P25 search stopped on is P25, from the FM discriminator audio (the HR_C7000's 8 kHz FM RX RAM on the flat receive
 * path, as the NXDN receiver reads it), without handing the frequency to the native P25 receiver first.
 *
 * Our own design (no third-party code): the 24-symbol frame sync is found by a normalised correlation of the samples
 * with the sync pattern (8000 / 4800 = 5/3 samples a symbol, so the 24 symbols span 40 samples); the correlation's
 * peak, refined to a fraction of a sample, gives the symbol timing; a least squares fit on the 24 sync symbols gives
 * the gain (with the polarity) and the offset (the carrier's frequency error); the 32 NID dibits after the sync (the
 * status symbol at 35 dropped) are sliced and the NID is decoded with its BCH code (third_party/p25cai). A NID
 * decoded with few corrections is P25, with its NAC. Receive only, no RTOS, no hardware, no allocation, integer
 * arithmetic (the CK803S has no FPU).
 *
 * RAM: sizeof(p25soft_t) about 330 bytes (the 128-sample ring is 256 of them); the radio core allocates it from the
 * RTOS heap only while a soft probe runs (radio_p25soft.c). CPU: about 25 additions a sample and, only where the
 * correlation is high, a 64-bit division; a NID costs 57 interpolations. */
#ifndef PLUS_P25SOFT_H
#define PLUS_P25SOFT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define P25SOFT_FS		8000u	/* samples a second (the FM RX RAM) */
#define P25SOFT_RING		128u	/* samples kept (a power of two): the sync's 40 and the NID's 58 after it */
#define P25SOFT_SYNC_N		40u	/* samples the 24 sync symbols span */
/* A sync: the correlation coefficient's square at least this (Q16; 0.70, r 0.84). A clean P25 sync reads about 0.88 */
#ifndef P25SOFT_SYNC_R2
#define P25SOFT_SYNC_R2		45875u
#endif
/* A NID decoded with at most this many corrected bits is P25 (BCH(63,16) corrects 11; a random word lands within 6
 * of a codeword about 3 times in 10^7) */
#define P25SOFT_NID_OK_BITS	6u

typedef struct {
	int16_t ring[P25SOFT_RING];	/* input samples, indexed by count & (P25SOFT_RING - 1) */
	uint32_t count;			/* samples in */
	int32_t sum;			/* the last P25SOFT_SYNC_N samples: sum */
	int64_t sum2;			/* ... and sum of squares */
	/* The correlation peak being followed */
	uint32_t cand_at;		/* the window's last sample (count) at the best correlation, 0 = none */
	uint32_t r2_prev, r2_best, r2_before;	/* r^2 (Q16) at the previous sample, the best and the one before it */
	bool cand_neg, r2_prev_sign;	/* the peak's sign (inverted), the previous sample's */
	/* A sync found: its NID waits for its samples */
	uint32_t nid_at;		/* count when the NID's last sample is in, 0 = none */
	int32_t t0_q8;			/* the sync's first symbol start, samples x 256 (the count of the window's start) */
	/* Results */
	uint16_t syncs;			/* syncs taken (a NID was read after each) */
	uint16_t hints;			/* NIDs that decoded, but with more than P25SOFT_NID_OK_BITS corrected */
	uint16_t nids;			/* NIDs decoded with at most P25SOFT_NID_OK_BITS corrected */
	uint16_t nac;			/* the last good NID's */
	uint8_t duid, corrected;
	uint32_t first_nid_count;	/* count when the first good NID was read (timing), 0 = none */
} p25soft_t;

void p25soft_init(p25soft_t *p);
/* Samples in (any polarity, offset and level; a flat discriminator path). Returns true once a good NID was read */
bool p25soft_samples(p25soft_t *p, const int16_t *in, size_t n);
/* A good NID was read: P25 on this frequency (nac, duid) */
static inline bool p25soft_p25(const p25soft_t *p) { return p->nids != 0; }
/* Nothing proven, but a sync was followed by a NID that decoded with too many corrections (a weak P25 signal, or chance):
 * the native receiver should decide */
static inline bool p25soft_hint(const p25soft_t *p) { return p->hints != 0; }

#ifdef __cplusplus
}
#endif
#endif
