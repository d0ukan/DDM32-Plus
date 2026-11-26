/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus auto airfield scan and auto repeater scan (CONFIG_AUTO_SCAN, docs/autoscan-design.md): the portable part
 * (no RTOS, no heap; the host tests run it as is on the simulated map pack). Installed as core/ui/autoscan.{c,h}.
 *
 * What it picks from the map pack in force (UI task only, as every reader of the pack):
 *   - repeaters: FM and DMR with a position and their RX in the radio's bands, within AS_RPT_RADIUS_M, nearest
 *     first, at most AS_MAX;
 *   - airfields: the nearest AS_MAX within AS_AIR_RADIUS_M that have a frequency to watch (any but ATIS); their
 *     frequencies round robin (each field's best first, then the second ones ...), best: TWR, then APP / DEP, then
 *     AFIS / INFO / CTAF, then GND, then the rest; ATIS left out; a frequency once.
 * The refresh keeps what it can (as_merge): a letter goes only when it is out of reach (AS_KEEP_PCT of the radius) or
 * a candidate is AS_GAIN_PCT and AS_GAIN_M nearer than the farthest letter (the auto repeater's rule): no flapping at
 * a boundary. The airfield scan merges airfields, then takes their frequencies again.
 *
 * The UI glue (ui_autoscan.c) and the ring (ui_airwatch.inc) are declared at the end. */
#ifndef PLUS_AUTOSCAN_H
#define PLUS_AUTOSCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"

#define AS_MAX		6u	/* letters: the multi-watch ring's most (UI_MW_MAX) */
#define AS_RPT_RADIUS_M	60000u	/* owner 2026-11-26 */
#define AS_AIR_RADIUS_M	100000u
#define AS_KEEP_PCT	110u	/* a letter stays while within this % of the radius */
#define AS_EVERY_MS	30000u	/* a refresh at most this often (the auto repeater's ARPT_EVERY_MS) ... */
#define AS_RPT_MOVE_M	1000u	/* ... and only after moving this far (repeaters) */
#define AS_AIR_MOVE_M	3000u	/* (airfields) */
#define AS_GAIN_PCT	20u	/* a candidate replaces the farthest letter only this much nearer ... */
#define AS_GAIN_M	2000u	/* ... and by more than this (ARPT_GAIN_PCT, ARPT_GAIN_M) */
#define AS_FIELD_MAX	1024u	/* airfield index limit of an id (field << 6 | frequency k) */

enum { AS_OFF, AS_AIR, AS_RPT };		/* the scan's kind */
enum { AS_M_AM, AS_M_FM, AS_M_DMR };		/* a letter's mode: the ring has the FM ones (A..) before the DMR ones */

typedef struct {
	uint16_t id;		/* repeater index; airfield: its index (as_pick_fields) or field << 6 | frequency k */
	uint8_t mode;		/* AS_M_* */
	uint32_t d_m;		/* metres from here, UINT32_MAX: gone from the pack */
} as_pick_t;

typedef bool (*as_band_fn)(uint32_t hz);	/* the radio receives hz (the band and its mode) */

/* The repeaters within radius_m of (lat, lon), nearest first (ties: the table's order), at most max; how many */
uint8_t as_pick_rpt(int32_t lat_e7, int32_t lon_e7, uint32_t radius_m, as_band_fn band, as_pick_t out[], uint8_t max);
/* The airfields with a frequency to watch within radius_m, nearest first (id: the airfield's index); how many */
uint8_t as_pick_fields(int32_t lat_e7, int32_t lon_e7, uint32_t radius_m, as_band_fn band, as_pick_t out[],
		       uint8_t max);
/* The letters from fields[0..nf) in their order, round robin (id: field << 6 | k, d_m the field's); how many */
uint8_t as_air_letters(const as_pick_t fields[], uint8_t nf, as_band_fn band, as_pick_t out[], uint8_t max);
/* The airfields of letters l[0..n) in order of first appearance (id: the field), their distances from (lat, lon) */
uint8_t as_letter_fields(const as_pick_t l[], uint8_t n, int32_t lat_e7, int32_t lon_e7, as_pick_t out[]);
/* Metres from (lat, lon) to repeater i / airfield f by the key section; UINT32_MAX when the pack has no such one */
uint32_t as_rpt_dist(uint16_t i, int32_t lat_e7, int32_t lon_e7);
uint32_t as_field_dist(uint16_t f, int32_t lat_e7, int32_t lon_e7);

/* The refresh: cur[0..n) (distances measured now) against cand[0..nc) (nearest first). A letter farther than keep_m
 * goes; a candidate not in cur fills a free place, else replaces the farthest letter (its place) when AS_GAIN_PCT and
 * AS_GAIN_M nearer. Returns the new count (cur rewritten, the kept letters in their places, compacted) */
uint8_t as_merge(as_pick_t cur[], uint8_t n, const as_pick_t cand[], uint8_t nc, uint32_t keep_m);
/* The ring's order: AM / FM letters before DMR ones, each group in its order (stable) */
void as_group(as_pick_t l[], uint8_t n);

/* The scan's state (ui_autoscan.c keeps one, RAM only) */
typedef struct {
	uint8_t kind;		/* AS_OFF, AS_AIR, AS_RPT */
	uint8_t n;		/* letters chosen (a ring from 2) */
	uint8_t pend;		/* bit k: letter k asked for, not seen taken yet (ui_autoscan.c reads the VFO back) */
	uint8_t said;		/* the "too few" count last said (0xFF none) */
	bool picked;		/* a selection was made, at (lat, lon) */
	int32_t lat_e7, lon_e7;
	uint32_t next_ms;	/* the next refresh at the earliest */
	uint32_t gen;		/* the map pack's sequence it was made from */
	as_pick_t l[AS_MAX];	/* the letters A.. */
	uint32_t hz[AS_MAX];	/* each letter's RX as asked */
} as_state_t;

enum { AS_DUE_NO, AS_DUE_YES, AS_DUE_FULL };
/* A refresh now? FULL: the map pack changed (the letters name another pack's rows: everything again); YES: the first
 * selection, or AS_EVERY_MS since the last and moved AS_*_MOVE_M, or (stale: letters chosen that no ring of the scan
 * has) AS_EVERY_MS since the last. No fix: NO (the letters stay) */
uint8_t as_due(const as_state_t *s, bool fix, int32_t lat_e7, int32_t lon_e7, uint32_t now_ms, uint32_t gen,
	       bool stale);

#if CONFIG_AUTO_SCAN
/* --- The UI task's side --- */
/* ui_autoscan.c: Tools > Air scan / Repeater scan and their held digits: on (kind), off (the same kind again) */
void ui_autoscan_toggle(uint8_t kind);
uint8_t ui_autoscan_on(void);		/* the kind running, AS_OFF none */
bool ui_autoscan_tick(uint32_t now_ms);	/* every UI tick (ui_app.c): once a second; true: draw again */
void ui_autoscan_ended(void);		/* the ring stopped some other way (the user, DOTA ...): off, toast */
/* ui_airwatch.inc: the ring of the scan (kind): letter k its own VFO on hz[k], arg[k] (UI_AIR_* kind bits), the user's
 * watch kept (as the air watch). False (a toast says why): it can't */
bool ui_dual_auto_ring(const uint32_t hz[], const uint32_t arg[], uint8_t n, uint8_t kind);
void ui_dual_auto_set(uint8_t k, uint32_t hz, uint32_t arg);	/* letter k replaced (UI_AIR_REPLACE) */
void ui_dual_auto_label(uint8_t k, const char *label);	/* Home's label of letter k */
bool ui_dual_auto_has(uint8_t k, uint32_t hz, uint32_t arg);	/* the radio's VFO k is letter (hz, arg): RX, TX, tone ... */
void ui_dual_auto_end(void);	/* the scan's own end (fewer than two letters, map pack gone): the user's watch back */
uint8_t ui_dual_auto_kind(void);	/* the scan kind the ring runs for, AS_OFF none (or the airfield watch) */
#endif

#endif
