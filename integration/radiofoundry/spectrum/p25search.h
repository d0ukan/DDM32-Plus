/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus P25 search (docs/plus.md): find P25 traffic anywhere in a frequency range, whatever its NAC and
 * talkgroup. The channel receiver steps across the range reading the signal strength (a fast sweep, no audio);
 * a step well above the noise floor is a hit, and the native P25 receiver then probes that frequency with any NAC
 * and any talkgroup. P25 frames there: the call is played and the frequency, NAC, talkgroup and source go into the
 * found list; the sweep goes on once the call has ended. No P25: the frequency is skipped for a while. Receive only:
 * PTT never transmits while searching (it stops the search). Pure logic: radio_sm.c tunes, native_lab.c probes. */
#ifndef PLUS_P25SEARCH_H
#define PLUS_P25SEARCH_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#define P25S_SETTLE_MS		30u	/* a reading this long after a retune is the step's */
#if CONFIG_SEARCH_FAST
/* DDM32 Plus faster sweep (docs/plus.md "Search ranges and speed"): a step's first reading at the poll after the retune
 * (25 ms) gates it. Quiet: on to the next step at once. Strong: read again settled (P25S_SETTLE_MS) before it is a hit;
 * strong then quiet: the first reading may have carried the step before's level (a slow reading), so that step is
 * read again settled. A quiet step costs one poll instead of two */
#define P25S_QUICK_MS		20u
/* A carrier found again not to be P25 within P25S_AGAIN_MS of its skip running out (an FM repeater, a beacon) is
 * skipped for P25S_SKIP_LONG_MS: fewer probes of the same busy channels on every pass */
#define P25S_SKIP_LONG_MS	30000u
#define P25S_AGAIN_MS		60000u
#endif
#define P25S_HIT_DB		10	/* a hit: this far above the floor ... */
#define P25S_HIT_MIN_DBM	(-115)	/* ... and at least this strong */
#define P25S_SKIP_MS		10000u	/* a carrier that wasn't P25 is not probed again for this long */
#define P25S_FOUND_MAX		8u
#define P25S_SKIP_MAX		8u
#define P25S_REVISIT_STEPS	16u	/* a found frequency in the range is read again after this many steps */
#define P25S_IDLE_MS		5000u	/* the screen didn't read the search this long (it was left): it stops */
/* The found list (owner 2026-11-21: how many frequencies, which NAC, which talkgroup have traffic): ONE entry per
 * frequency + NAC + talkgroup with a call counter. A probe's report updates its key's entry (calls, when heard, the
 * last source) and moves it to the top; a new key goes on top and the oldest entry drops when the list is full. A
 * report may not know the NAC (P25S_NAC_NONE: the receiver's NAC after a terminator or a damaged frame) or the
 * talkgroup (0: no link control yet): it updates the entry the probe holds, or the newest one that fits, and starts
 * no entry without a NAC. A call counted on an entry whose talkgroup then turns out another within P25S_MOVE_MS moves
 * to the right one */
#define P25S_NAC_NONE		0xFFFFu
#define P25S_CALL_GAP_MS	3000u	/* heard again after this long: a new call */
#define P25S_MOVE_MS		1500u

typedef enum { P25S_OFF, P25S_SWEEP, P25S_PROBE } p25s_phase_t;

typedef struct {
	uint32_t hz;
	uint16_t nac, tg;
	uint32_t src;
	uint32_t heard_ms;	/* when it was last heard */
	uint16_t calls;
	uint16_t id;		/* the entry's serial, given when it was made (the PC's search log follows an entry by it) */
} p25s_found_t;

typedef struct {
	uint8_t phase;		/* p25s_phase_t */
	uint32_t lo_hz, hi_hz, step_hz;
	uint32_t at_hz;		/* the step being read, or the frequency being probed / played */
	int16_t floor_dbm;	/* noise floor estimate (a slow average of the quiet steps) */
#if CONFIG_SEARCH_SURE
	uint16_t step_ms;	/* a sweep step's time, averaged (the screen's steps a second), 0 = none yet */
#endif
	uint32_t sweeps, hits, probes_p25;
	uint8_t found_n;
	p25s_found_t found[P25S_FOUND_MAX];	/* newest first */
	bool playing;		/* the probe found P25 and the call plays */
} p25s_view_t;

/* The radio task's side. start: the first frequency to tune. sweep: called on every poll while sweeping with the
 * reading; returns the next frequency to tune (0 = stay), and sets *hit when the current step is a hit (the
 * search then waits in P25S_PROBE for p25s_probe_done) */
uint32_t p25s_start(uint32_t lo_hz, uint32_t hi_hz, uint32_t step_hz, uint32_t now_ms);
uint32_t p25s_sweep(int16_t rssi_dbm, uint32_t now_ms, bool *hit);
void p25s_stop(void);
/* The receiver was tuned to p25s_at_hz() just now (the radio task, after something else had it): settle again */
void p25s_retuned(uint32_t now_ms);
/* Changes whenever the search moves to a new frequency outside the sweep's own steps (after a probe) */
uint32_t p25s_epoch(void);
bool p25s_on(void);
uint8_t p25s_phase(void);
uint32_t p25s_at_hz(void);
/* The native receiver's side: a probe of p25s_at_hz() found P25 (playing, with what it heard so far) or ended.
 * found: the call is noted (frequency, NAC, talkgroup, source); done: back to sweeping from the next step,
 * a probe that heard no P25 marks the frequency to skip. Both return the frequency to tune next (0 when off) */
void p25s_probe_found(uint16_t nac, uint16_t tg, uint32_t src, uint32_t now_ms);
uint32_t p25s_probe_done(bool was_p25, uint32_t now_ms);
#if CONFIG_SEARCH_SURE
/* DDM32 Plus surer search (radio/searchsure.h): the level while the native receiver probes (the radio task reads it on
 * every poll; 0 = none), and whether the probe's carrier went off the air (its level under the hit's half margin for
 * SURE_GONE_MS): the probe is given back at once then and the frequency is not skipped */
void p25s_level(int16_t rssi_dbm, uint32_t now_ms);
bool p25s_carrier_gone(uint32_t now_ms);
#endif
#if CONFIG_SEARCH_SOFTP25
/* DDM32 Plus P25 soft probe (docs/p25-soft-probe-design.md, radio/p25soft.h, radio/radio_p25soft.h): with the fast
 * algorithm and the soft probe chosen (held 0 on the P25 search screen: fast -> fast + soft probe -> classic), a hit
 * is first read in software on the FM path for P25SOFT_WINDOW_MS. A good NID: P25, its NAC goes into the found list
 * at once and the native receiver takes the frequency as before (it plays the call). Nothing P25-like on a carrier
 * well over the floor: not P25, skipped as a probe that heard nothing (without the native receiver's 1.2 s). Anything
 * else (a weak carrier, a doubtful NID, no heap, no ring): the native receiver probes as before. Off (the default):
 * nothing here runs and the search is the fast or classic one, unchanged */
enum { P25S_SOFT_P25, P25S_SOFT_NONE, P25S_SOFT_UNSURE };
void p25s_set_soft(bool on);		/* the screen's choice (the UI task), taken by the next start */
bool p25s_soft_wanted(void);
bool p25s_soft_pending(void);		/* a probe waits for the soft verdict: the native receiver must not take it */
int16_t p25s_probe_margin(void);	/* the probed carrier's level over the floor, dB */
/* The soft probe's verdict (radio task). Returns the frequency to tune next when it ended the probe (NONE), else 0 */
uint32_t p25s_soft_verdict(uint8_t verdict, uint16_t nac, uint32_t now_ms);
#endif
/* A consistent copy for the UI */
bool p25s_snapshot(p25s_view_t *out);
/* The PC's search log: the state and found entry i, not counted as a look (p25search.c) */
typedef struct {
	uint8_t phase, found_n;
	uint16_t seq;		/* calls counted since power on (a call moved to another entry counts once) */
	uint32_t at_hz, lo_hz, hi_hz;
	p25s_found_t f;
} p25s_peek_t;
bool p25s_peek(unsigned i, p25s_peek_t *out);

void radio_req_p25_search(uint32_t lo_hz, uint32_t hi_hz);	/* lo 0 = stop */

#endif
