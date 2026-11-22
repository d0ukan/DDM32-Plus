/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus surer searches (CONFIG_SEARCH_SURE, docs/plus.md "Search reliability (searchfix)"): the pieces the P25 and
 * DMR search engines (p25search.c, dmrsearch.c) share, each with its own copy (static: either search builds without
 * the other). No RTOS, no hardware.
 *
 *  - The floor: learned from the first step (a receiver whose quiet reading sits at -112 dBm made every step a "hit"
 *    against the old -125 dBm start, and only quiet steps moved it, so it never learned: every step was probed). It
 *    falls fast to a lower reading, follows quiet steps slowly and rises only a quarter dB per strong step, so a long
 *    transmission or a cluster of strong neighbours can't lift it far.
 *  - The run: a strong step is not probed at once. The sweep goes on while the next steps read as strong or stronger
 *    (within SURE_RUN_DB of the strongest), then probes the middle of the strongest run: the peak of a carrier whose
 *    lower skirt came first (a radio close by reads strong 12.5-50 kHz off), the middle of a plateau (an overloaded
 *    front end), the half step between two equal steps (a signal on a 6.25 kHz raster, x.xx625, 12.5 kHz steps).
 *    A run of three or more steps is probed on a channel step, never a half step: its one clearly strongest step
 *    (SURE_PEAK_DB over every other: the channel, its neighbours raised by a radio close by), else the step in its
 *    middle (branch dmrs3: an even plateau's middle was a half step, 6.25 kHz off a DMR channel, which the modem can't
 *    decode). A run SURE_CLIMB_MAX steps flat is the band's level raised, not a carrier: the floor takes its level.
 *    Unless one of its steps reads clearly over the rest (beta.7 hole hunt A3: a plateau never hides its strongest
 *    step): that step is probed first, and the floor takes the run's level when the probe ends.
 *  - The skirt: after a probe (or a run whose middle is skipped) the steps up to SURE_SKIRT_HZ above the run that read
 *    SURE_SKIRT_DB under it are that carrier's, not probed again (SURE_SKIRT_MS, the same pass). */
#ifndef PLUS_SEARCHSURE_H
#define PLUS_SEARCHSURE_H

#include <stdbool.h>
#include <stdint.h>

#define SURE_RUN_DB		4	/* steps within this of the run's strongest belong to the run (a direct mode burst
					 * read early reads a few dB short) */
#define SURE_CLIMB_MAX		16u	/* a run this many steps flat (200 kHz at 12.5 kHz) is no carrier but the band's level
					 * raised (a radio close by desensing the receiver, a noisy band): it becomes the floor */
#define SURE_SKIRT_DB		10	/* past the run: this far under it is its skirt ... */
#define SURE_SKIRT_HZ		50000u	/* ... up to this far past it */
#define SURE_SKIRT_MS		4000u	/* a skirt is known this long after the probe ended */
#define SURE_PEAK_DB		3	/* a run's step this far over every other step of it is the one probed */
#define SURE_FLOOR_UP_Q4	4	/* a strong step lifts the floor by 1/4 dB (dBm x 16) */
#define SURE_GONE_MS		200u	/* a probe's level under the hit's half margin this long: off the air (a direct mode
					 * DMR gap is 32.5 ms, so it is a whole over that ended) */

typedef struct {
	bool floor_ok;		/* the floor was learned (the first step) */
	bool climbing;		/* a run is being followed */
	bool skirt_on;
	bool wide;		/* the run is SURE_CLIMB_MAX flat: its level becomes the floor when it ends (dmrs3) */
	uint8_t climbed;
	int16_t run_lvl;	/* the run's strongest */
	int16_t best_lvl, next_lvl;	/* the run's strongest reading and the strongest of its other steps */
	uint32_t best_hz;		/* ... and where (dmrs3) */
	int16_t skirt_lvl;
	uint32_t run_lo, run_hi;
	uint32_t skirt_lo, skirt_hi, skirt_ms, skirt_pass;
} sure_t;

enum { SURE_MORE, SURE_DONE, SURE_WIDE };

/* A step's level into the floor (dBm x 16). strong: the step counted as strong against the floor before it */
static inline void sure_floor(sure_t *u, int32_t *floor_q4, int16_t lvl, bool strong)
{
	int32_t l = (int32_t)lvl * 16;
	if (!u->floor_ok) {
		*floor_q4 = l;
		u->floor_ok = true;
	} else if (l < *floor_q4) {
		*floor_q4 += (l - *floor_q4) / 2;
	} else if (!strong) {
		*floor_q4 += (l - *floor_q4) / 8;
	} else {
		*floor_q4 += SURE_FLOOR_UP_Q4;
	}
}

/* Whether a strong step at hz belongs to the carrier probed last (on the same pass: the next pass comes to it from
 * below again and follows its run anew) */
static inline bool sure_skirt(const sure_t *u, uint32_t hz, int16_t lvl, uint32_t now_ms, uint32_t pass)
{
	if (!u->skirt_on || now_ms - u->skirt_ms >= SURE_SKIRT_MS || pass != u->skirt_pass)
		return false;
	/* only above the run: the sweep goes on upwards from it (a revisit's probe goes back to where the sweep was, below:
	 * the climb from there finds the run again) */
	return hz > u->skirt_hi && hz - u->skirt_hi <= SURE_SKIRT_HZ && lvl <= u->skirt_lvl - SURE_SKIRT_DB;
}

/* The run ended (probed or passed over): its steps are a skirt from now */
static inline void sure_set_skirt(sure_t *u, uint32_t now_ms, uint32_t pass)
{
	u->skirt_pass = pass;
	u->skirt_on = true;
	u->skirt_lo = u->run_lo;
	u->skirt_hi = u->run_hi;
	u->skirt_lvl = u->run_lvl;
	u->skirt_ms = now_ms;
	u->climbing = false;
}

/* A step's level while following a run (strong: it is a strong step and no skirt). SURE_MORE: read the next step */
static inline int sure_climb(sure_t *u, uint32_t hz, int16_t lvl, bool strong)
{
	if (!u->climbing) {
		u->climbing = true;
		u->climbed = 0;
		u->run_lvl = lvl;
		u->run_lo = u->run_hi = hz;
		u->best_lvl = lvl;
		u->best_hz = hz;
		u->next_lvl = INT16_MIN;
		u->wide = false;
		return SURE_MORE;
	}
	if (!strong || lvl < u->run_lvl - SURE_RUN_DB)
		return SURE_DONE;	/* weaker: the run is behind */
	if (lvl > u->run_lvl + SURE_RUN_DB) {
		u->run_lvl = lvl;	/* stronger: a new run starts here (and may go on as far again) */
		u->run_lo = u->run_hi = hz;
		u->climbed = 0;
		u->best_lvl = lvl;
		u->best_hz = hz;
		u->next_lvl = INT16_MIN;
	} else {
		u->run_hi = hz;
		if (lvl > u->best_lvl) {
			u->next_lvl = u->best_lvl;
			u->best_lvl = lvl;
			u->best_hz = hz;
		} else if (lvl > u->next_lvl) {
			u->next_lvl = lvl;
		}
	}
	if (++u->climbed < SURE_CLIMB_MAX)
		return SURE_MORE;
	u->wide = true;
	return SURE_WIDE;
}

/* A run has ended (probed, passed over, or quiet when its target was read again): a SURE_WIDE run's level is the
 * band's, raised: the floor from now (until quiet steps pull it down again). True when the floor changed */
static inline bool sure_wide_floor(sure_t *u, int32_t *floor_q4)
{
	if (!u->wide)
		return false;
	u->wide = false;
	*floor_q4 = (int32_t)u->run_lvl * 16;
	return true;
}

/* One step of the run reads clearly over all its others (SURE_PEAK_DB): a carrier, not just the band raised */
static inline bool sure_peaked(const sure_t *u)
{
	return u->best_lvl - u->next_lvl >= SURE_PEAK_DB;
}

/* The frequency the run is probed on (step: the sweep's). One step: it. Two: the half step between them (a signal on
 * a 6.25 kHz raster reads on both). Three or more: the clearly strongest step, else the step in the middle (of the two
 * in the middle of an even run, the one nearer the strongest reading): always a channel step */
static inline uint32_t sure_target(const sure_t *u, uint32_t step)
{
	uint32_t span = u->run_hi - u->run_lo;
	if (step == 0u || span <= step) {
		uint32_t mid = u->run_lo + span / 2u;
		return mid - mid % 10u;
	}
	if (sure_peaked(u))
		return u->best_hz;
	uint32_t n = span / step;	/* steps above run_lo */
	uint32_t lo = u->run_lo + n / 2u * step;
	if (n % 2u == 0u)
		return lo;
	return u->best_hz > lo ? lo + step : lo;
}

#endif
