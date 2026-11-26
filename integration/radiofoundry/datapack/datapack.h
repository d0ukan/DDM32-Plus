/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus user data packs (CONFIG_DATAPACK, docs/r14.md 7, docs/plus.md "Data packs"): data the user builds from
 * sources they download themselves (RadioID.net's user list, the aerodromes' own publications), kept on the data
 * chip in the one region the owner opened for it (AGENTS.md rule 2: 0xE70000-0xEFFFFF), never in the firmware.
 *
 * Each pack type has two slots; a slot is a 64-byte header and the payload. A pack is written into the slot not in
 * force (every sector it needs erased first, the payload programmed, read back against its CRC-32) and the header is
 * programmed last: until then the slot does not count, so a power cut, a stop or a bad transfer leaves the pack in
 * force (or none). Of a type's valid slots the higher sequence is in force. No pack: the built-in tables, as before.
 *
 * Every erase and program goes through dpk_erase / dpk_program, which refuse anything outside the region. Reads go
 * through the board's flash driver as the CPS-written ID database's do. Portable (no RTOS): host-tested on the
 * simulated flash. Installed as core/storage/datapack.{c,h}. */
#ifndef PLUS_DATAPACK_H
#define PLUS_DATAPACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DPK_BASE	0xE70000u	/* the region: [DPK_BASE, DPK_END) */
#define DPK_END		0xF00000u
#define DPK_SECTOR	4096u
#define DPK_HDR		64u

enum { DPK_RADIOID = 1, DPK_AIR = 2, DPK_REF = 3, DPK_TYPES = 3 };
/* Why a type has no pack in force (dpk_state): DPK_NONE nothing committed (no "DPK1" at a slot start), DPK_BAD a
 * header or payload that does not check, DPK_BIG a RadioID pack whose header checks but is longer than this layout's
 * slot (a 1.0.x pack over 196,544 bytes: rebuild it), DPK_OTHER a map pack that checks but was built for another
 * firmware version (docs/mappack-design.md C.2). Of two rejected slots OTHER wins, then BIG, then BAD */
enum { DPK_NONE, DPK_OK, DPK_BAD, DPK_BIG, DPK_OTHER };

typedef struct {
	char magic[4];		/* "DPK1" */
	uint8_t type;		/* DPK_RADIOID, DPK_AIR */
	uint8_t fmt;		/* the payload's format version (1) */
	uint16_t flags;		/* 0; map pack: its version hint (DPK_REF_HINT), 0 = no hint */
	uint32_t seq;		/* the higher valid one of the type's slots is in force */
	uint32_t len;		/* payload bytes */
	uint32_t crc;		/* storage_crc32 of the payload */
	char date[8];		/* "YYYYMMDD": the source's date */
	char name[16];		/* "RadioID TR", NUL padded */
	char tag[16];		/* the builder's, NUL padded */
	uint32_t hcrc;		/* storage_crc32 of the 60 bytes before */
} dpk_hdr_t;

/* What the radio keeps of a pack in force (8 bytes a type: the rest of the header stays on the chip, dpk_header) */
typedef struct {
	uint32_t seq, len;
} dpk_info_t;

/* The slots of a type: 0 and 1; their address and size (0 for a type out of range). Layout 2
 * (docs/mappack-design.md A.1): RadioID 0xE70000 / 0xEB0000 (192 KB each, the same starts as 1.0.x), map pack
 * 0xEA0000 / 0xEE0000 (64 KB), air band 0xEF0000 / 0xEF4000 (16 KB); 0xEF8000-0xEFFFFF spare */
uint32_t dpk_slot_addr(uint8_t type, uint8_t slot);
uint32_t dpk_slot_size(uint8_t type);
/* The address of the slot in force for a type (its header), 0 none */
uint32_t dpk_active_addr(uint8_t type);
/* Raw bytes of the region (never outside [DPK_BASE, DPK_END)): the map pack's validation and readers */
bool dpk_raw(uint32_t addr, void *buf, uint32_t len);

/* The map pack's version hint in the header's flags: bits 0-7 the directory version, bits 8-15 the low byte of the
 * map coordinate id; 0 = no hint (older writers) */
#define DPK_REF_HINT(dir_version, coord_id)	((uint16_t)((dir_version) | ((coord_id) & 0xFFu) << 8))

/* Check every slot's header and payload CRC (boot, after a CPS session, after a pack arrived): which is in force */
void dpk_scan(void);
/* The pack in force for a type: its header, NULL if none; dpk_state: DPK_OK, DPK_NONE (empty slots, or not scanned)
 * or DPK_BAD (a slot holds a header or payload that does not check: a broken pack) */
const dpk_info_t *dpk_active(uint8_t type);
uint8_t dpk_state(uint8_t type);
/* The whole header of the pack in force, read from the chip */
bool dpk_header(uint8_t type, dpk_hdr_t *h);
/* Payload bytes of the pack in force */
bool dpk_read(uint8_t type, uint32_t off, void *buf, uint32_t len);

/* Writing a pack: begin (the slot not in force, its sectors erased), any number of puts at any payload offsets (each
 * byte once), commit (payload CRC read back, then the header). false at any step: the old pack stays in force */
typedef struct {
	dpk_hdr_t h;
	uint32_t addr;		/* the slot being written, 0 none */
} dpk_writer_t;
bool dpk_begin(dpk_writer_t *w, const dpk_hdr_t *h);
bool dpk_put(dpk_writer_t *w, uint32_t off, const void *data, uint32_t len);
bool dpk_commit(dpk_writer_t *w);
/* A header's own CRC filled in (the builder and the tests use it); a header that checks */
void dpk_seal(dpk_hdr_t *h);
bool dpk_hdr_ok(const dpk_hdr_t *h, uint8_t type);

/* --- RadioID pack (type DPK_RADIOID, format 1) ---
 * Payload: an 8-byte header (record count 4 bytes LE, record size 1, text characters 1, 2 reserved) and the records
 * sorted by ID: the ID 3 bytes LE, then the text ("TA1ABC Ahmet") 6 bits a character, 4 in 3 bytes, most significant
 * first, alphabet space 0-9 A-Z a-z . (as the CPS's format N, open-firmware-cps.md 7.5), trailing spaces dropped */
#define DPK_ID_HDR	8u
#define DPK_ID_TEXT_MAX	32u
/* "TA1ABC Ahmet" for a DMR ID from the pack in force, cut to len - 1; false (out "") if no pack or not in it */
bool dpk_id_text(uint32_t id, char *out, size_t len);
/* The pack in force as "RadioID TR, 2026-11-17, 98 KB" (About); false (out "") without one */
bool dpk_about(uint8_t type, char *out, size_t len);
/* A pack's header as dpk_about says it (DOTA's question about a pack offered) */
void dpk_describe(const dpk_hdr_t *h, char *out, size_t len);

/* The guarded flash calls (also the tests' handle on the guard): false outside [DPK_BASE, DPK_END) */
bool dpk_erase(uint32_t addr);
bool dpk_program(uint32_t addr, const void *data, uint32_t len);

#endif
