/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus faster searches (CONFIG_SEARCH_HYBRID, docs/plus.md "Faster searches (beta.9 candidate)"): what the P25,
 * DMR and all digital search engines (p25search.c, dmrsearch.c) share. One search runs at a time, so one state serves
 * the three (the engine that started last owns it). No RTOS, no hardware.
 *
 *  - Measurement: per search, counters the PC reads over the debug link (searchlog_view.c, the X.st line): passes and
 *    the last pass's time and steps, probes and their outcomes, time spent probing and sweeping, the time from the
 *    carrier first read above the floor to its find, and how long a found probe waited for its mode's first frame.
 *    Kept in both algorithms, so the owner can compare them on the same signal.
 *  - The algorithm switch: "fast" (the hybrid below) or "classic" (beta.8), chosen on the search screens (held 0) and
 *    latched when a search starts.
 *  - The candidate memory (fast): carriers the sweep read strong, with their level and when they were first and last
 *    seen. One left undecided (its carrier went off the air during the probe, something of a mode was heard but not
 *    enough, or the over ended before the probe began) is read again between sweep steps, strongest first, for
 *    SH_WATCH_MS: a wide range doesn't wait a whole pass for the carrier's next over. */
#ifndef PLUS_SEARCHHY_H
#define PLUS_SEARCHHY_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

enum { SH_NONE, SH_P25, SH_DMR, SH_ALL };	/* the engine (owner) */
enum { SH_END_FOUND, SH_END_NOSYNC, SH_END_PARTIAL, SH_END_GONE };	/* how a probe ended */

#define SH_CANDS	8u
#define SH_NEAR_HZ	25000u	/* a strong step this near a candidate is that carrier (its skirt: 12.5-25 kHz) */
#define SH_FORGET_MS	60000u	/* a candidate not read strong this long is forgotten (first seen starts anew) */
#define SH_WATCH_MS	30000u	/* an undecided carrier is read again between steps this long after it was last seen */
#define SH_REJECT_MS	120000u	/* fast: a steady carrier found not to be digital twice in a row is skipped this long
				 * (classic: 30 s); a TDMA-looking DMR carrier never gets more than classic's */

typedef struct {
	uint8_t owner;		/* SH_*: whose counters these are */
	bool fast;		/* the running search's algorithm */
	bool probing, synced;
	uint16_t passes, pass_steps, cur_steps;
	uint16_t probes, found, nosync, partial, gone, skipped, early, watch;
	uint16_t sync_last, sync_max;	/* ms from a found probe's start (its mode's window) to the first frame */
	uint32_t start_ms, mark_ms, pass_t0, pass_ms, probe_ms, sweep_ms;
	uint32_t first_ms;	/* from the start to the first find, 0 = none */
	uint32_t seen_ms;	/* the last find: from its carrier first read strong to the find, 0 = none */
} sh_stats_t;

/* The screen's choice (the UI task), taken by the next search start */
void sh_set_fast(bool fast);
bool sh_fast_wanted(void);

/* The engines (the radio task) */
void sh_start(uint8_t owner, uint32_t now_ms);
bool sh_fast(void);
void sh_step(uint32_t now_ms);			/* a sweep step done */
void sh_pass(uint32_t now_ms);			/* the sweep wrapped */
void sh_seen(uint32_t hz, int16_t lvl, uint32_t now_ms);	/* a step read strong (a candidate) */
void sh_unsettled(uint32_t hz, uint32_t now_ms);	/* a run's target read quiet before its probe: watched */
void sh_skipped(void);				/* a target passed over: in the skip list */
void sh_probe(bool watch, uint32_t now_ms);	/* a probe starts (watch: a watched carrier read again) */
void sh_sync(uint32_t waited_ms);		/* the probe's first frame of its mode (once a probe) */
void sh_found(uint32_t hz, uint32_t now_ms);	/* the probe confirmed its mode (once a probe) */
void sh_end(uint8_t how, bool early, uint32_t hz, uint32_t now_ms);	/* the probe ended (SH_END_*) */
/* fast: the strongest watched carrier in lo..hi not read again this round and not skipped (skip: the engine's skip
 * list), 0 = none */
uint32_t sh_watch_pick(uint32_t lo, uint32_t hi, uint32_t now_ms, bool (*skip)(uint32_t hz, uint32_t now_ms));
/* The PC (the debug link task): a copy of owner's counters (running: with the phase running now added, mark_ms = now;
 * stopped: as at the last probe or sweep change), false when another engine owns them */
bool sh_view(uint8_t owner, bool running, uint32_t now_ms, sh_stats_t *out);

#endif
