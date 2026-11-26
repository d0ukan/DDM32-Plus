/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus repeater list (CONFIG_REPEATERS, docs/plus.md): a table in program flash that tools/rpt_list.py makes at
 * every prepare (RadioID.net's DMR repeaters and / or the owner's CSV; a few fictional entries without either), and
 * the math Tools > Repeaters needs: the order nearest first from a position (or by frequency without one), the
 * distance and bearing, the Maidenhead locator, and what "tune the VFO" sets. Portable (no RTOS), so the host tests
 * run it as is. No RAM of its own: the order is walked, never stored (rpt_next: one pass over the table a step).
 *
 * Provinces (CONFIG_RPT_PROV: Tools > Repeaters opens on "All" and the provinces that have repeaters): each entry
 * carries its province's plate, decided by the generator; the _in walks keep to one province.
 *
 * Order: by the squared equirectangular distance from here (integer, 1e-5 degree, the longitude scaled by
 * cos(latitude) in Q15; within the list's few hundred km it orders as the great circle does), then by the table
 * index (RX frequency): a total order, so walking it forward and back visits every entry once. Entries without a
 * position come after all others; without a position of our own the order is the table's (by frequency). */
#ifndef PLUS_RPT_H
#define PLUS_RPT_H

#include <stdbool.h>
#include <stdint.h>

#define RPT_NAME_MAX	10	/* tools/rpt_list.py NAME_MAX */
#define RPT_CITY_MAX	12	/* tools/rpt_list.py CITY_MAX */
#define RPT_NOPOS	INT32_MIN	/* lat_e5 / lon_e5: the repeater's position is not known */
#define RPT_NONE	0xFFFFu	/* no entry (rpt_next's start and end) */
#define RPT_STEP_HZ	1250u	/* off's unit */

typedef enum { RPT_FM, RPT_DMR, RPT_C4FM, RPT_DSTAR, RPT_P25, RPT_NXDN, RPT_M17, RPT_MODES } rpt_mode_t;
#define RPT_MODE_MASK	0x0Fu
#define RPT_NARROW	0x80u	/* mode: FM in 12.5 kHz (else 25 kHz) */
#define RPT_TS_BOTH	3u	/* dmr >> 4: 0 not known, 1, 2, 3 both */

typedef struct {
	int32_t lat_e5, lon_e5;		/* degrees x 1e5, RPT_NOPOS: not known */
	uint32_t rx_hz;			/* the repeater's output: what the radio receives */
	int16_t off;			/* TX - RX in RPT_STEP_HZ, 0 simplex */
	uint16_t ctcss10;		/* the access tone, Hz x 10, 0 none */
	uint8_t mode;			/* rpt_mode_t | RPT_NARROW */
	uint8_t dmr;			/* colour code | slots << 4 */
	char name[RPT_NAME_MAX + 1];	/* the callsign */
	char city[RPT_CITY_MAX + 1];
	uint8_t prov;			/* the province's plate 1..81, RPT_PROV_UNKNOWN (tools/rpt_list.py; was padding) */
} rpt_entry_t;
_Static_assert(sizeof(rpt_entry_t) == 44u, "tools/rpt_list.py ENTRY_BYTES");

#define RPT_PROVS	81u	/* plates 1..81 */
#define RPT_PROV_UNKNOWN 0u	/* rpt_entry_t.prov: not placed in a province */
#define RPT_PROV_ALL	0xFFu	/* the filter of rpt_next_in / rpt_rank_in / rpt_count_in: every entry */

typedef struct {
	uint32_t count;
	uint8_t sample;			/* 1: the committed fictional sample */
	char date[11];			/* "2026-11-19", "sample" */
	char source[16];		/* "RadioID.net", "own list", "own+RadioID.net", "sample" */
} rpt_info_t;

/* The table tools/rpt_list.py generates (build/rpt/rpt_data.c), sorted by RX frequency: since 1.1 it goes into the
 * map pack (tools/refpack.py) and the host tests, not into the firmware (the link audit refuses rpt_table) */
extern const rpt_info_t rpt_info;
extern const rpt_entry_t rpt_table[];

/* The list the radio uses: the map pack's repeater sections (storage/ref_pack.h), UI task only. No map pack, or one
 * without repeaters: 0 entries ("load the map pack") */
uint16_t rpt_count(void);
/* Entry i copied out of the pack, its strings terminated whatever the pack holds; one static copy (the next call
 * overwrites it: never hold two), NULL past the end */
const rpt_entry_t *rpt_get(uint16_t i);
/* The pack's repeater info (count, sample, date, source), strings terminated; false (zeroed) without it */
bool rpt_info_get(rpt_info_t *out);
bool rpt_sample(void);		/* the pack's list is the fictional sample (rpt_info_t.sample) */
/* Repeater i listens or transmits within half a 12.5 kHz channel of hz: the TX refusal (cross-band, DOTA) */
bool rpt_entry_near(const rpt_entry_t *e, uint32_t hz);

/* The TX refusal set in program flash (generated rpt_freqs.c, docs/mappack-design.md C.7): every listed repeater's
 * output and input (frequencies only), whatever map pack the radio has. True when a repeater of the set transmits or
 * listens within +-6,250 Hz of hz, exactly as rpt_entry_near over the rows it was made from */
bool rpt_freq_listed(uint32_t hz);
#define RPT_FQ_VHF_BASE	135993750u	/* the steps' zero: 136 MHz - 6,250 Hz (an entry on the rim still matches) */
#define RPT_FQ_UHF_BASE	399993750u
extern const uint16_t rpt_fq_vhf[], rpt_fq_uhf[];	/* sorted steps of RPT_STEP_HZ above the base */
extern const uint16_t rpt_fq_vhf_n, rpt_fq_uhf_n;

/* Where we are, for the order: ok false = no position (the table's order) */
typedef struct {
	bool ok;
	int32_t lat_e5, lon_e5;
	uint16_t cos_q15;
} rpt_here_t;

void rpt_here_set(rpt_here_t *h, bool ok, int32_t lat_e7, int32_t lon_e7);
/* The entry after (dir > 0) or before (dir < 0) cur in the order, among the entries of province prov (a plate,
 * RPT_PROV_UNKNOWN, or RPT_PROV_ALL: all); cur RPT_NONE: the first (last). RPT_NONE past the end. One pass over the
 * table */
uint16_t rpt_next_in(const rpt_here_t *h, uint16_t cur, int dir, uint8_t prov);
/* How many entries of prov come before i in the order (0: the nearest) */
uint16_t rpt_rank_in(const rpt_here_t *h, uint16_t i, uint8_t prov);
/* How many entries prov has; one pass (RPT_PROV_ALL: the count) */
uint16_t rpt_count_in(uint8_t prov);
static inline uint16_t rpt_next(const rpt_here_t *h, uint16_t cur, int dir)
{
	return rpt_next_in(h, cur, dir, RPT_PROV_ALL);
}
static inline uint16_t rpt_rank(const rpt_here_t *h, uint16_t i) { return rpt_rank_in(h, i, RPT_PROV_ALL); }
static inline uint16_t rpt_nearest(const rpt_here_t *h) { return rpt_next(h, RPT_NONE, 1); }
/* The nearest entry with a position that ok accepts (in the order's sense); RPT_NONE without one or without our
 * position. One pass over the table */
uint16_t rpt_nearest_if(const rpt_here_t *h, bool (*ok)(const rpt_entry_t *e, void *ctx), void *ctx);

/* Metres and the true bearing (0..359) from (lat_e7, lon_e7) to the entry; false when it has no position */
bool rpt_way(const rpt_entry_t *e, int32_t lat_e7, int32_t lon_e7, uint32_t *meters, uint16_t *bearing);
/* The 6 character Maidenhead locator of the entry ("KM69kw"), out at least 7 bytes; "" without a position */
void rpt_locator(const rpt_entry_t *e, char *out);
const char *rpt_mode_name(const rpt_entry_t *e);	/* "FM", "DMR", "C4FM", ... */

/* What tuning the VFO to the entry sets (FM and DMR only: the radio has no other of these modes) */
typedef struct {
	uint32_t rx_hz, tx_hz;		/* TX = RX + offset (= RX: simplex) */
	bool dmr;			/* false: FM */
	bool narrow;			/* FM 12.5 kHz */
	uint16_t ctcss10;		/* FM: the TX tone, 0 none (the receiver's tone stays off) */
	uint8_t cc, ts;			/* DMR: colour code and slot (1 or 2; not known or both: 1) */
} rpt_tune_t;
bool rpt_tune_plan(const rpt_entry_t *e, rpt_tune_t *out);

/* ui_rpt.c: opens Tools > Repeaters on entry i's details; Back there returns to the screen that called it (the map) */
void ui_rpt_open(uint16_t i);
/* ui_rpt.c: tune the VFO to entry i as OK in its details does (the auto repeater's offer, CONFIG_AUTO_RPT) */
void ui_rpt_tune(uint16_t i);

#endif
