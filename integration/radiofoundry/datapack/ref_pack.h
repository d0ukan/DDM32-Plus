/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus map pack (data pack type DPK_REF, docs/mappack-design.md C): the Turkey map, the districts, the repeater
 * list and the airfields, read straight from the data chip (no RAM copy) instead of program flash.
 *
 * Payload format 1 (little-endian, every section 4-byte aligned): a 16-byte directory head ("REF1", directory
 * version, section count n <= 16, 2 reserved, the map coordinate id, 4 reserved), then n entries of 12 bytes (id u16,
 * record size u16, offset u32 from the payload's start, count u32), then the sections. Unknown ids are ignored.
 *
 * ref_validate checks a slot's payload (raw reads of that slot only, no shared state): DPK_OK, DPK_BAD (a rule of
 * C.2 broken) or DPK_OTHER (built for another firmware: directory version, a known section with another record size,
 * another coordinate id, a non-zero flags hint that disagrees). datapack.c calls it in slot_check (boot, CPS close,
 * DOTA commit) and in dpk_commit before the header is programmed.
 *
 * The readers belong to the UI task (the map, the lists, the auto repeater tick, the cross-band and DOTA forms): it
 * owns the directory copy and two read windows; ref_has / ref_count reload the directory when the pack in force
 * changed (its slot or sequence), the other readers read only through the copy, so one use (a draw) reads one pack.
 * Installed as core/storage/ref_pack.{c,h}. */
#ifndef PLUS_REF_PACK_H
#define PLUS_REF_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REF_DIR_VERSION	1u
#define REF_SECTIONS	16u	/* at most in a directory */
#define REF_HEAD	16u
#define REF_ENTRY	12u

/* Section ids (their record size: ref_rec_size) */
enum {
	REF_S_META = 1, REF_S_PROV, REF_S_RING, REF_S_STEP, REF_S_PNAME, REF_S_DPOS, REF_S_DFIRST, REF_S_DNFIRST,
	REF_S_DNAME, REF_S_RINFO, REF_S_RPT, REF_S_RKEY, REF_S_AIR, REF_S_AFREQ, REF_S_AKEY, REF_IDS = 15
};
/* Features: each needs its own sections whole (a pack without repeater sections still has the map) */
enum { REF_MAP = 1, REF_RPT = 2, REF_AIR = 4 };

/* A key record (RPT_KEY, AIR_KEY): what the walks need without the 44-byte record */
typedef struct {
	int32_t lat_e5, lon_e5;
	uint8_t prov;		/* the plate 1..81, 0 unknown */
	uint8_t mode;		/* RPT_KEY: the record's mode byte; AIR_KEY: its kind */
} ref_key_t;

uint16_t ref_rec_size(uint8_t id);
/* CRC-32 of the compiled map coordinate constants (7 LE int32) and the charset text: the directory head's map id */
uint32_t ref_coord_id(void);
/* The header flags a pack for this firmware carries: DPK_REF_HINT(REF_DIR_VERSION, ref_coord_id()) */
uint16_t ref_hint(void);
/* The payload at pa (the slot's address + DPK_HDR), len bytes, its header's flags: DPK_OK, DPK_BAD or DPK_OTHER */
uint8_t ref_validate(uint32_t pa, uint32_t len, uint16_t flags);

/* --- UI task only --- */
bool ref_has(uint8_t feature);
/* The pack in force's sequence (0: none): a new pack (CPS close, a DOTA commit) has another (DDM32 Plus auto scan) */
uint32_t ref_seq(void);
uint16_t ref_count(uint8_t id);		/* a section's count, 0 without it (also reloads like ref_has) */
/* Record i of a section, whole (the record window) */
bool ref_rec(uint8_t id, uint32_t i, void *out);
/* Bytes of a section from offset off (the stream window: steps, names, key passes) */
bool ref_bytes(uint8_t id, uint32_t off, void *out, uint32_t n);
/* Bytes straight from the chip (no window): a few records read once */
bool ref_direct(uint8_t id, uint32_t off, void *out, uint32_t n);
bool ref_key(uint8_t id, uint32_t i, ref_key_t *k);	/* REF_S_RKEY / REF_S_AKEY */
/* A NUL-terminated name from a blob section at off, cut to n - 1 and terminated; returns the bytes the name takes in
 * the blob with its NUL (where the next one starts), 0 when off is not inside the blob */
uint32_t ref_name(uint8_t id, uint32_t off, char *out, size_t n);

/* Record layouts the validation checks (rpt.h rpt_entry_t, airfield.h air_field_t; _Static_asserts there) */
#define REF_RPT_MODE	16u	/* rpt_entry_t.mode */
#define REF_RPT_PROV	42u	/* rpt_entry_t.prov */
#define REF_AIR_FIRST	8u	/* air_field_t.first (u16), .count (u8) at 10, .kind (u8) at 11 */
#define REF_AIR_PROV	43u	/* air_field_t.prov */

#endif
