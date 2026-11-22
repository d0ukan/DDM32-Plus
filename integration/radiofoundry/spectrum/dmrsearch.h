/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DMR search (docs/plus.md): find DMR traffic anywhere in a frequency range, whatever its color code,
 * timeslot and talkgroup. The board stays in DMR the whole time with the call stack hearing everything (hear all,
 * any color code, either slot). The receiver steps across the range reading the signal strength (a fast sweep);
 * a step well above the noise floor is a hit and is probed: the frequency is held while the modem reports bursts.
 * DMR bursts there: the frequency, color code, slots, repeater (BS sync) or mobile (MS sync) and the last call's
 * talkgroup and source go into the found list; a voice call plays and the sweep goes on once it has ended and a
 * short hang has passed. No DMR: the frequency is skipped for a while. Receive only: PTT never transmits while
 * searching (it stops the search). Pure logic: radio_sm.c tunes and reads the call stack (dmr_call_heard).
 *
 * DDM32 Plus all digital search (CONFIG_ALL_SEARCH, docs/plus.md "All digital search"): the same engine, a second
 * kind (DMRS_KIND_ALL). It sweeps on the FM path and probes a hit in the owner's priority order P25 > DMR > YSF >
 * NXDN, each mode with its own receiver and window (native C4FM receiver for P25 and YSF, the DMR board mode and call
 * stack for DMR, the FM path's software 4FSK modem for NXDN); the first confirmed mode wins and the rest is as above
 * (found list with the mode as a field, the call played, the hang, the resume, skips, revisits). The two kinds share
 * this state (RAM): starting the other kind clears the found list. */
#ifndef PLUS_DMRSEARCH_H
#define PLUS_DMRSEARCH_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#define DMRS_SETTLE_MS		30u	/* readings this long after a retune are the step's */
#define DMRS_READS		3u	/* readings per step, a poll (25 ms) apart: a direct mode burst (30 ms in 60) lands
					 * in one of them; the step's level is their peak */
#if CONFIG_SEARCH_FAST
/* DDM32 Plus faster sweep (docs/plus.md "Search ranges and speed"): the first of the DMRS_READS readings at the poll
 * after the retune (25 ms), the others settled, so a quiet step takes three polls instead of four. A strong settled
 * reading is a hit. A strong first reading alone (a burst caught only there, or the step before's level on a slow
 * reading; half the margin counts there, as a burst that began just before it reads only part of its level) gets
 * DMRS_MORE_READS more settled ones, which cover every phase of the 60 ms frame without it; still
 * nothing: the step before, if it was read so, is read again settled */
#define DMRS_QUICK_MS		20u
#define DMRS_MORE_READS		2u
/* A carrier found again not to be DMR within DMRS_AGAIN_MS of its skip running out (an FM repeater, a beacon) is
 * skipped for DMRS_SKIP_LONG_MS: fewer probes of the same busy channels on every pass */
#define DMRS_SKIP_LONG_MS	30000u
#define DMRS_AGAIN_MS		60000u
#endif
#define DMRS_HIT_DB		10	/* a hit: this far above the floor ... */
#define DMRS_HIT_MIN_DBM	(-115)	/* ... and at least this strong */
#define DMRS_PROBE_MS		600u	/* a hit: bursts within this long are DMR (sync comes once per 360 ms superframe in
					 * a voice call, every 60 ms on a repeater; the rest is the modem's lock) */
#if CONFIG_SEARCH_SURE
/* DDM32 Plus surer search (radio/searchsure.h, docs/plus.md "Search reliability"): a carrier whose level rises and
 * falls as a direct mode one does (TDMA: 30 ms in 60), or that brought a burst already, is held this long: a voice
 * call's sync comes once per 360 ms superframe, so two superframes (one sync lost) plus the stack's start fit */
#define DMRS_PROBE_LONG_MS	1100u
#endif
#define DMRS_MIN_BURSTS		2u	/* bursts with a good check that make it DMR */
#define DMRS_LATE_MS		1500u	/* DMR without a call to play: held this long from the hit for one (late entry
					 * takes the call's LC from the second superframe at worst), then on */
#define DMRS_HANG_MS		2000u	/* a call ended: held this long for a reply, then on */
#define DMRS_SKIP_MS		10000u	/* a carrier that wasn't DMR is not probed again for this long */
#define DMRS_FOUND_MAX		8u
#define DMRS_SKIP_MAX		8u
#define DMRS_REVISIT_STEPS	16u	/* a found frequency in the range is read again after this many steps */
#define DMRS_IDLE_MS		5000u	/* the screen didn't read the search this long (it was left): it stops */
#define DMRS_CC_NONE		0xFFFFu	/* found / heard cc: not named yet (P25 NAC, DMR CC, YSF DG-ID, NXDN RAN) */
/* The found list (owner 2026-11-21): ONE entry per frequency + mode + code + group (dmrsearch.c note). A call counted
 * on the entry a probe holds whose group then turns out another within this long moves to the right entry */
#define DMRS_MOVE_MS		1500u

/* The kinds (view.kind) and the modes a find has (found.mode; view.mode: the one probed or played now) */
#define DMRS_KIND_DMR		0u
#define DMRS_KIND_ALL		1u
enum { DMRS_MODE_DMR, DMRS_MODE_P25, DMRS_MODE_YSF, DMRS_MODE_NXDN, DMRS_MODES };
/* The all digital search's probe windows: a mode not confirmed this long after its receiver took the frequency is
 * not on it (the next mode is tried; after the last the frequency is skipped as a non-digital carrier). P25: a frame
 * every 180 ms (LDU) and the native receiver's lock; YSF: a frame every 100 ms and the FICH; NXDN48: a frame every
 * 80 ms and the software modem's acquisition (radio_nxdn.c); DMR as DMRS_PROBE_MS */
#if CONFIG_SEARCH_SURE
#define DMRS_P25_PROBE_MS	1200u	/* the zone scan's figure for this native receiver (native_lab.c SCAN_P25_PROBE_MS) */
#else
#define DMRS_P25_PROBE_MS	700u
#endif
#define DMRS_YSF_PROBE_MS	700u
#define DMRS_NXDN_PROBE_MS	900u
#define DMRS_VOICE_MS		500u	/* native receiver: voice this recent is a call playing (native_lab.c) */

/* found / heard sync_mask */
#define DMRS_SYNC_BS		1u	/* a repeater's outbound */
#define DMRS_SYNC_MS		2u	/* a mobile: direct mode (MS or TDMA direct sync), or a repeater's input */

typedef enum { DMRS_OFF, DMRS_SWEEP, DMRS_PROBE, DMRS_CALL } dmrs_phase_t;

/* What the receiver heard since the frequency was tuned: the radio task's reading of the call stack */
typedef struct {
	uint16_t bursts;	/* bursts with a good check (the all digital search: frames of the mode probed) */
	uint16_t cc;		/* the color code of the latest burst that names one, DMRS_CC_NONE = none yet (the all
				 * digital search: P25 NAC, YSF DG-ID, NXDN RAN) */
	uint8_t ts_mask;	/* slots with traffic: bit 0 TS1, bit 1 TS2 (named by a repeater or TDMA direct sync) */
	uint8_t sync_mask;	/* DMRS_SYNC_* */
	bool call;		/* a voice call plays */
	bool other;		/* a call heard but not played (encrypted) */
	bool group;		/* the call's: group call (else private) */
	uint32_t dst, src;	/* the call's talkgroup (or called ID) and source, 0 = not known yet (YSF: the source
				 * callsign, dmrs_call_pack) */
	int16_t rssi_dbm;	/* the receiver's level now, 0 = not read (the native receiver's probes) */
} dmrs_heard_t;

typedef struct {
	uint32_t hz;
	uint32_t dst, src;	/* the last call's talkgroup (or called ID) and source, 0 = none heard (YSF: the source
				 * callsign packed, dmrs_call_unpack) */
	uint32_t heard_ms;	/* when it was last heard */
	uint16_t calls;		/* voice calls played there */
	uint16_t cc;		/* DMRS_CC_NONE = not named yet (P25 NAC, DMR color code, YSF DG-ID, NXDN RAN) */
	uint8_t ts_mask;	/* slots with traffic seen there */
	uint8_t sync_mask;	/* DMRS_SYNC_*: repeater and / or mobile */
	uint8_t id;		/* the entry's serial (mod 256), given when it was made: the PC's search log follows an entry
				 * by it with its frequency and mode (searchlog_view.c) */
	uint8_t mode : 2;	/* DMRS_MODE_* (the DMR search: always DMR) */
	bool group : 1;
	bool enc : 1;		/* a call there was encrypted (not played) */
} dmrs_found_t;

typedef struct {
	uint8_t phase;		/* dmrs_phase_t */
	bool playing;		/* a call plays on at_hz */
	uint8_t found_n;
	uint8_t kind;		/* DMRS_KIND_*: whose state this is */
	int16_t floor_dbm;	/* noise floor estimate (a slow average of the quiet steps) */
	uint8_t mode;		/* DMRS_MODE_* probed or played now (PROBE / CALL) */
#if CONFIG_SEARCH_SURE
	uint16_t step_ms;	/* a sweep step's time, averaged (the screen's steps a second), 0 = none yet */
#endif
	uint32_t lo_hz, hi_hz, step_hz;
	uint32_t at_hz;		/* the step being read, or the frequency being probed / played */
	uint32_t sweeps, hits, found_dmr;
	dmrs_found_t found[DMRS_FOUND_MAX];	/* newest first */
} dmrs_view_t;

/* The radio task's side. start: the first frequency to tune. sweep: on every poll while sweeping, with the reading;
 * returns the next frequency to tune (0 = stay) and sets *hit when the step is a hit (the search then holds it in
 * DMRS_PROBE). probe: on every poll while probing or playing, with what the receiver heard since the tune; returns
 * the frequency to tune next (0 = stay). Every return other than 0 is a retune: the caller clears what was heard */
uint32_t dmrs_start(uint32_t lo_hz, uint32_t hi_hz, uint32_t step_hz, uint32_t now_ms);
uint32_t dmrs_sweep(int16_t rssi_dbm, uint32_t now_ms, bool *hit);
uint32_t dmrs_probe(const dmrs_heard_t *h, uint32_t now_ms);
void dmrs_stop(void);
bool dmrs_on(void);
uint8_t dmrs_phase(void);
uint32_t dmrs_at_hz(void);
/* A consistent copy for the UI (and a sign that the screen still looks) */
bool dmrs_snapshot(dmrs_view_t *out);
/* The PC's search log: the state and found entry i, not counted as a look (dmrsearch.c) */
typedef struct {
	uint8_t phase, found_n, mode;
	uint16_t seq;		/* calls counted since power on by either kind (a call moved to another entry counts once) */
	uint32_t at_hz, lo_hz, hi_hz;
	dmrs_found_t f;
} dmrs_peek_t;
bool dmrs_peek(unsigned i, dmrs_peek_t *out);

void radio_req_dmr_search(uint32_t lo_hz, uint32_t hi_hz);	/* lo 0 = stop */

#if CONFIG_ALL_SEARCH
/* The all digital search's side of the same calls. Snapshot and peek of a kind that is not running read as stopped
 * with nothing found (a snapshot then is no look either: the other kind's screen keeps nothing alive) */
uint32_t dmrs_start_all(uint32_t lo_hz, uint32_t hi_hz, uint32_t step_hz, uint32_t now_ms);
bool dmrs_snapshot_all(dmrs_view_t *out);
bool dmrs_peek_all(unsigned i, dmrs_peek_t *out);
uint8_t dmrs_kind(void);
uint8_t dmrs_mode(void);
/* The receiver was set up for what the search wants now (the radio task after a mode change or a native give back;
 * native_lab.c when its receiver took the frequency): the step settles anew, or the probe window starts from now */
void dmrs_retuned(uint32_t now_ms);
/* The native receiver gave the frequency back while the search goes on: the radio task sets the receiver up again
 * (it compares the epoch with the one it saw) */
void dmrs_regained(void);
uint8_t dmrs_epoch(void);
/* The frequency a mode's own receiver must take now, 0 = none: the native receiver (P25 / YSF; *ysf says which), the
 * NXDN software modem */
uint32_t dmrs_native_hz(bool *ysf);
uint32_t dmrs_nxdn_hz(void);
/* A YSF callsign (up to 10 characters: space, A-Z, 0-9, '/', '-', '.'; others read '.') in two numbers, base 40:
 * *lo the first six, *hi the rest; both 0 for an empty one. unpack writes it back, trailing spaces cut */
void dmrs_call_pack(const char *call, uint32_t *hi, uint32_t *lo);
void dmrs_call_unpack(uint32_t hi, uint32_t lo, char out[11]);
/* The modes' names: "DMR", "P25", "YSF", "NXDN" */
const char *dmrs_mode_name(uint8_t mode);

void radio_req_all_search(uint32_t lo_hz, uint32_t hi_hz);	/* lo 0 = stop */
#endif

#endif
