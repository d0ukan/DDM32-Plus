/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus faster channel scan (CONFIG_SCAN_FAST, docs/plus.md "Faster channel scan"): the zone scan and the VFO
 * scan move on from a quiet channel as soon as its readings say it is quiet, not after the whole dwell. No RTOS, no
 * hardware: the radio core (radio_scanfast.inc) reads the receiver on its 10 ms tick and hands the readings here.
 *
 *  - Per stop: from FS_SETTLE_MS (30 ms) after the retune, every reading is tested against the scan's own carrier rule with a
 *    margin (radio_scanfast.inc). One reading that is not clearly quiet hands the stop back to the classic rules for
 *    the rest of it (dwell, probe, hold: the scan as in v1.0.0). Once the quiet readings span the mode's time with no
 *    two more than FS_GAP_MS apart, the stop is quiet and the scan moves on. A late reading starts the span again.
 *  - Counters for the PC (debug link section "scanst", scanfast_view.c), kept in both algorithms.
 *  - The switch: fast (default) or classic, held 0 on Home while a zone or VFO scan runs; kept in RAM while the radio
 *    is on. A running scan takes the change at its next stop and its counters start again.
 * Dual watch and the multi-watch ring are not touched (their own timing, radio_mwatch.h). */
#ifndef PLUS_SCANFAST_H
#define PLUS_SCANFAST_H

#include <stdbool.h>
#include <stdint.h>

#define FS_SETTLE_MS	30u	/* a reading this soon after the retune is not used: the receiver's noise reading reads
				 * low (a carrier) for a moment after a frequency change, and a new carrier may show late
				 * (verifier: 30, so a reading lagging 20 ms still shows an FM carrier 1 dB over the line) */
#define FS_SETTLE_RETUNE_MS 25u	/* v1.0.2: ... and at least this long after the retune was done (FS_SETTLE_MS counts
				 * from the step's start; a slow step, reading the channel, ate into it and the readings
				 * met the noise reading's dip: the stop fell back to the classic rules) */
#define FS_GAP_MS	12u	/* readings further apart than this (a late tick) start the span again */
#define FS_SPAN_MS	20u	/* a continuous carrier (FM, P25, YSF, NXDN, dPMR): three readings 10 ms apart (30-50 ms
				 * after the retune) */
#define FS_SPAN_DMR_MS	60u	/* DMR: one whole 60 ms TDMA frame (a direct mode burst is on air 27.5 ms of it) */
#define FS_NOISE_MARGIN	8u	/* a quiet reading's noise is at least this far above the squelch's opening line (2 dB
				 * at the model's 4 a dB; the floor reads about 50 above it, so a quiet zone doesn't mind) */
#define FS_RSSI_MARGIN	6	/* ... and (digital modes) its strength this many dB under the DMR strength line */
#define FS_POLL_GUARD_MS 25u	/* after a step made on the tick (to a stop, or back from a priority look to the wait),
				 * the radio core's squelch doesn't open until the retune is this old (the age a classic
				 * stop's first poll has) */
#define FS_RETUNE_GUARD_MS 20u	/* v1.0.2: after a step the poll made (classic, a priority stop), counted from the end
				 * of the retune: the noise reading reads low (a carrier) for about this long after a
				 * frequency change (OpenDMR trusts it from 20 ms). A classic stop's first poll comes 22 ms
				 * or more after a step that took no time, so this only holds back one after a slow step */

enum { FS_M_FM, FS_M_DMR, FS_M_P25, FS_M_OTHER, FS_MODES };	/* P25 counts YSF (the native receiver's modes) */
enum { FS_WAIT, FS_QUIET, FS_CLASSIC };				/* fs_feed's verdict */
enum { FS_ST_OFF, FS_ST_SAMPLING, FS_ST_CLASSIC, FS_ST_DONE };	/* a stop's fast state */

typedef struct {
	uint32_t tuned_ms;	/* the stop's tune (scan_t.tuned_ms; a pause's end tunes anew) */
	uint32_t first_ms;	/* the quiet span: its first and last reading (0: none yet) */
	uint32_t last_ms;
	uint8_t state;		/* FS_ST_* */
} fs_stop_t;

typedef struct {
	bool on;		/* a zone or VFO scan runs */
	bool fast;		/* its algorithm */
	uint16_t stops;		/* stops made (modulo 65536, as every count here) */
	uint16_t passes;	/* complete passes (the first wrap only starts the count) */
	uint16_t pass_stops;	/* the last complete pass: its stops and time */
	uint16_t noticed;	/* stops that probed or held (something heard) */
	uint16_t held;		/* ... of them, those that held (a signal to stop for) */
	uint16_t fast_quiet;	/* stops ended quiet by the fast rule */
	uint16_t fallback;	/* stops the fast rule handed to the classic rules (a reading not clearly quiet) */
	uint16_t late;		/* spans started again after a late reading */
	uint16_t q_n[FS_MODES];	/* quiet stops (not noticed) by mode, and their time from tune to the next tune */
	uint32_t q_ms[FS_MODES];
	uint32_t start_ms, mark_ms;	/* the scan's start; its end (running: now) */
	uint32_t pass_ms;	/* the last complete pass's time */
	/* Where the time goes (v1.0.2, the owner's "a zone I made scans slowly" report): the longest stop since the
	 * counters started, tune to the next tune (where as for the passes, its channel number, FS_M_*, FS_WHY_* bits) */
	uint32_t slow_ms, slow_where;
	uint16_t slow_ch;
	uint8_t slow_mode, slow_why;
	/* the last stop the fast rule handed to the classic rules: its channel, mode and the reading that did it (the
	 * noise reading, the squelch's opening line + FS_NOISE_MARGIN it had to reach, the strength in dBm) */
	uint16_t fb_ch;
	int16_t fb_rssi;
	uint8_t fb_noise, fb_line, fb_mode;
	/* the radio task: the longest gap between two of its 10 ms ticks while scanning and the longest step (reading
	 * the next channel, retuning) the tick made, in ms; stops in another zone than the one scanned (a priority
	 * channel there) and stops that switched the receiver between FM and DMR */
	uint16_t gap_max, step_max, other_zone, switches;
} fs_stats_t;

enum { FS_WHY_NOTICED = 1, FS_WHY_HELD = 2, FS_WHY_FALLBACK = 4, FS_WHY_OTHER_ZONE = 8 };

/* The switch (the UI task); the radio task reads it at every stop */
void fs_set_fast(bool fast);
bool fs_fast_wanted(void);

/* One reading of a stop in its listen phase (since: ms from its tune). quiet: clearly no carrier by the scan's rules.
 * span_ms: FS_SPAN_MS or FS_SPAN_DMR_MS. Pure: the host tests drive it directly */
uint8_t fs_feed(fs_stop_t *s, uint32_t now_ms, bool quiet, uint32_t span_ms);

/* The radio core (the radio task) */
void fs_begin(uint32_t now_ms);			/* a zone or VFO scan starts */
void fs_end(uint32_t now_ms);			/* it ends */
bool fs_fast(void);				/* the running scan's algorithm (taken from the switch at each stop) */
/* A new stop was tuned at tuned_ms (where: the zone sweep's position or the VFO frequency, for the passes); the stop
 * before it lasted from its own tune to tuned_ms, of mode (FS_M_*), noticed / held as the scan's counters say */
void fs_stop_new(uint32_t tuned_ms, uint32_t where, uint8_t prev_mode, bool prev_noticed, bool prev_held);
void fs_count(uint8_t what);			/* a stop's fast verdict: FS_QUIET or FS_CLASSIC (late readings: fs_feed) */
fs_stop_t *fs_cur(void);
/* The stop fs_stop_new just began: its channel number (0 in the VFO), whether it is in another zone than the one
 * scanned, whether its tune switched the receiver between FM and DMR */
void fs_stop_about(uint16_t number, bool other_zone, bool switched);
/* The reading that handed the current stop to the classic rules (after fs_count(FS_CLASSIC)) */
void fs_fallback_reading(uint8_t mode, uint8_t noise, uint8_t line, int16_t rssi);
/* The radio task's timing: the gap since its last tick, the time a step on the tick took (ms) */
void fs_radio_cost(uint32_t gap_ms, uint32_t step_ms);

/* The PC (the debug link task): a copy of the counters (running: mark_ms = now) */
void fs_view(uint32_t now_ms, fs_stats_t *out);

#endif
