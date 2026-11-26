/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Airfields (CONFIG_AIRFIELDS, docs/plus.md "Airfields"): the Turkish airfields of OurAirports with
 * their air band frequencies (a table in program flash, tools/airfields.py -> airfield_data.c), listed nearest first
 * from the position APRS uses, and what listening to one of them tunes: the VFO in the air band, received in AM, never
 * transmitting (the radio core refuses TX in the air band: TXDENY_AM). Portable (no RTOS): the host tests run it as is.
 * No RAM of its own: the order is walked, never stored (air_next: one pass over the table a step), as Tools >
 * Repeaters does.
 *
 * Order: by the squared equirectangular distance from here (integer, 1e-5 degree, the longitude scaled by the cosine
 * of the pair's middle latitude in Q15, from a 5 degree table), then by the table index (the code): a total order, so
 * walking it forward and back visits every airfield once. No float. Without a position of our own the order is the
 * table's (by code).
 *
 * Provinces (CONFIG_AIR_PROV, DDM32 Plus round 16: Tools > Airfields opens on "All" and the provinces that have
 * airfields): each airfield carries its province's plate, decided by tools/airfields.py from its position with the
 * repeater list's lookup (the map's outlines); the _in walks keep to one province. */
#ifndef PLUS_AIRFIELD_H
#define PLUS_AIRFIELD_H

#include <stdbool.h>
#include <stdint.h>

#define AIR_CODE_MAX	7	/* tools/airfields.py CODE_MAX: "LTAC", "TR-0018" */
#define AIR_NAME_MAX	22	/* tools/airfields.py NAME_MAX */
#define AIR_NONE	0xFFFFu	/* no airfield (air_next's start and end) */
#define AIR_CH_BASE_KHZ	108000u	/* air_freq_t.ch: (channel name in kHz - this) / 5 */

typedef enum { AIR_LARGE, AIR_MEDIUM, AIR_SMALL } air_kind_t;
/* The frequency's use, in the order an airfield lists them (tools/airfields.py TYPES) */
typedef enum {
	AIR_ATIS, AIR_DEL, AIR_GND, AIR_RMP, AIR_TWR, AIR_APP, AIR_DEP, AIR_AFIS, AIR_INFO, AIR_CTAF, AIR_PAR, AIR_SAR,
	AIR_EMR, AIR_OPS, AIR_OTH, AIR_TYPES
} air_type_t;

typedef struct {
	int32_t lat_e5, lon_e5;		/* degrees x 1e5 */
	uint16_t first;			/* its first frequency in air_freqs[] */
	uint8_t count;			/* how many (0: none in the data) */
	uint8_t kind;			/* air_kind_t */
	char code[AIR_CODE_MAX + 1];	/* ICAO code, else OurAirports' ident */
	char name[AIR_NAME_MAX + 1];	/* short name, ISO-8859-9 */
	uint8_t prov;			/* the province's plate 1..81, AIR_PROV_UNKNOWN (tools/airfields.py; was padding) */
} air_field_t;
_Static_assert(sizeof(air_field_t) == 44u, "tools/airfields.py FIELD_BYTES");

#define AIR_PROVS	81u	/* plates 1..81 */
#define AIR_PROV_UNKNOWN 0u	/* air_field_t.prov: not placed in a province */
#define AIR_PROV_ALL	0xFFu	/* the filter of air_next_in / air_rank_in / air_count_in: every airfield */

typedef struct {
	uint16_t ch;			/* (channel name kHz - AIR_CH_BASE_KHZ) / 5: 121.580 -> 2716 */
	uint8_t type;			/* air_type_t */
} air_freq_t;

/* The generated table (airfield_data.c, sorted by code): since 1.1 it goes into the map pack (tools/refpack.py) and
 * the host tests, not into the firmware (the link audit refuses air_fields) */
extern const air_field_t air_fields[];
extern const air_freq_t air_freqs[];
extern const uint16_t air_field_count;

/* The airfields the radio uses: the map pack's (storage/ref_pack.h), UI task only; 0 without a map pack */
uint16_t air_nfields(void);
/* Airfield i copied out of the pack, its strings terminated; one static copy (never hold two), NULL past the end */
const air_field_t *air_get(uint16_t i);
/* Frequency k of the pack's table (air_field_t.first + n), false past its end */
bool air_freq_get(uint32_t k, air_freq_t *f);

/* Where we are, for the order: ok false = no position (the table's order) */
typedef struct {
	bool ok;
	int32_t lat_e5, lon_e5;
} air_here_t;

void air_here_set(air_here_t *h, bool ok, int32_t lat_e7, int32_t lon_e7);
/* The airfield after (dir > 0) or before (dir < 0) cur in the order, among the airfields of province prov (a plate,
 * AIR_PROV_UNKNOWN, or AIR_PROV_ALL: all); cur AIR_NONE: the first (last). AIR_NONE past the end. One pass over the
 * table */
uint16_t air_next_in(const air_here_t *h, uint16_t cur, int dir, uint8_t prov);
/* How many airfields of prov come before i in the order (0: the nearest) */
uint16_t air_rank_in(const air_here_t *h, uint16_t i, uint8_t prov);
/* How many airfields prov has; one pass (AIR_PROV_ALL: the count) */
uint16_t air_count_in(uint8_t prov);
static inline uint16_t air_next(const air_here_t *h, uint16_t cur, int dir)
{
	return air_next_in(h, cur, dir, AIR_PROV_ALL);
}
static inline uint16_t air_rank(const air_here_t *h, uint16_t i) { return air_rank_in(h, i, AIR_PROV_ALL); }
static inline uint16_t air_nearest(const air_here_t *h) { return air_next(h, AIR_NONE, 1); }

/* Metres and the true bearing (0..359) from (lat_e7, lon_e7) to airfield i */
void air_way(uint16_t i, int32_t lat_e7, int32_t lon_e7, uint32_t *meters, uint16_t *bearing);

/* The channel name in kHz ("121.580" -> 121580) and the frequency it tunes: the 25 kHz value, or its 8.33 kHz grid
 * point (x.005 -> x.000, x.010 -> x.00833, x.015 -> x.01667, rounded to the Hz as radio_am_step does) */
uint32_t air_freq_khz(const air_freq_t *f);
uint32_t air_freq_hz(const air_freq_t *f);
bool air_freq_833(const air_freq_t *f);		/* an 8.33 kHz channel name (not on the 25 kHz grid) */
const char *air_type_name(uint8_t type);	/* "TWR", "GND", ... */

/* ui_airfield.c: tune the VFO to airfield i's frequency k as OK in its details does (AM receive, never TX) */
void ui_airfield_tune(uint16_t i, uint8_t k);
#if CONFIG_AIR_WATCH
/* ui_airwatch.inc (DDM32 Plus air watch, item 44b): hz[0..n-1] (2..6) on the multi-watch ring, each letter its own
 * VFO, the user's watch kept until the ring stops; atis: the letters (bit k) that are an ATIS (the radio core holds
 * them 5 s at most, then passes them over a while). False (a toast says why): it can't */
bool ui_dual_air_watch(const uint32_t hz[], uint8_t n, uint8_t atis);
#endif

#endif
