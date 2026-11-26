# Map / reference data pack: design (job "mappack", 2026-11-26)

Design only. Nothing here is built yet; no firmware, tool or programmer code changed with this document. Base:
`ddm25` 580a16d (v1.0.6). All numbers marked "estimate" must be confirmed by the build (`--plus --public
--stack-check --verify-rebuild`) or on the radio (section G).

**Revision 2 (2026-11-26)** after the skeptical review of ae8102a ("ready to implement: NO"). Changes, by finding:

| Finding | What changed | Where |
|---|---|---|
| B1 (blocker) DOTA stops refusing repeater frequencies without a pack | a compact **repeater frequency set stays in program flash** (~0.9-1.3 KB), used only by the TX refusals (`known_repeater`) of cross-band **and DOTA**, independent of the pack; cross-band no longer refuses Start without a pack | C.7, D, F, G T9/T15, H.4, H.5 |
| S1 bad type-3 pack with valid CRCs taking over | structural validation is part of `slot_ok` for type 3 and runs in `dpk_commit` **before** the header is programmed; all-or-nothing per slot | C.2, C.3 |
| S2 cross-task race | the scan validates only (raw slot reads, never the UI window); the UI owns its directory copy and reloads it lazily when the generation (active slot + seq) changes | C.3 |
| S3 record sanitising | NUL forced on copied strings; offsets, counts and `first + count` sums checked in u32 | C.2, C.5, G T5 |
| S4 map redraw rate during RX | the map layer is redrawn only when its own key changes; measured during RX | C.4, G T10 / MP3 |
| S5 boot time | both slots are CRC-checked: ~110-130 ms with two map packs | C.4, G MP10 |
| S6 release gate | a public release refuses repeater data from a source the owner has not cleared (default until H.5 is answered: no "own" = TRAC data) for both the pack and the flash set | E.2, E.5, H.5 |
| N1-N5 | DOTA / programmer consistency for a > 192 KB RadioID pack; old-programmer PASS that erases a map slot; `min_firmware` meaning; implementation traps; `ref_pack` inside the firmware manifest entry; CRC coordinate id | B.4, C.2, E.2, E.3, F.5 |

Owner decisions of 2026-11-26 this design implements:

1. RadioID pack slots shrink from 2 x 256 KB to 2 x 192 KB (the owner's Turkey RadioID pack is ~98 KB).
2. The constant data of the Turkey map, the repeater table, the airfield table (and the marine table, see C.6 for why
   this design keeps marine in program flash) leave program flash for a new data pack in the freed data-chip space.
   Fonts and the Turkish text table stay in program flash.
3. Later (not this job): "Auto airfield scan" and "Repeater scan" from GPS. The format below keeps both cheap (C.4).

Current design read first: `docs/plus.md` "User data packs" (line 3630+), `docs/r14.md` 7 (branch `r14`, cefcba4),
`docs/dota.md` 20, `integration/radiofoundry/datapack/datapack.{c,h}`, `air_pack.c`, `tools/datapack.py`, the Rust
programmer's `rust/crates/ddm32-core/src/datapacks/datapack.rs` and `radio/cps.rs`.

---

## Summary

| Item | Proposal |
|---|---|
| Layout | **Interleaved**: RadioID A 0xE70000 (192 KB), map pack A 0xEA0000 (64 KB), RadioID B 0xEB0000 (192 KB), map pack B 0xEE0000 (64 KB), air band A/B 0xEF0000 / 0xEF4000 unchanged, 0xEF8000-0xEFFFFF (32 KB) free. Both RadioID slot headers stay exactly where 1.0.6 has them |
| Migration | **No firmware writes at boot or on update.** An existing RadioID pack (either slot) of up to 196,544 B payload is accepted unchanged: the owner does **not** reload RadioID. Map pack slots read "none" until the programmer (or DOTA) writes one. Downgrade to 1.0.6 keeps RadioID and air band packs valid |
| Map pack | One new pack type `DPK_REF` = 3, format 1: a directory of up to 16 sections (map meta, provinces, rings, steps, province names, district positions / indexes / names, repeater info / records / key, airfields / frequencies / key). Read straight from the chip, no RAM copy; a 128-byte read window for sequential streams |
| Size | ~50.7 KB payload, 13 sectors; slot payload max 65,472 B: **~14.8 KB headroom** (22.6 % of the slot, +29 % over today's data) |
| Flash | Gross 47,163 B leaves program flash; reader + "no data" states ~3.0-4.5 KB and the repeater frequency set ~0.9-1.3 KB come back (estimate): **net ~42 KB freed (range 41.4-43.3 KB)** |
| RAM | ~400 B static (window 136 B, UI directory copy 100 B, kept pack state 12 B, name buffers 48 B, three static record scratch copies 92 B) of the ~5.2 KB free |
| TX refusals | cross-band and DOTA refuse listed repeater frequencies **with or without a map pack** (a frequency set in program flash, C.7) |
| Marine | **Stays in program flash** (590 B): it feeds the radio core's TX refusal (`marine_hz`); moving it would put a SPI read on the PTT path and lose the refusal without a pack. Owner question H.1 |
| Release | The firmware build makes `ddm32-ref-<version>.dpk` from the same generator outputs; it ships as a **separate signed release asset** (a `ref_pack` object inside the firmware's entry of `ddm32-manifest.json`). The firmware zip keeps its three files, so older programmers still install the firmware. A public release refuses repeater data the owner has not cleared (H.5) |
| Programmer | Rust 3.2: third pack row "Harita paketi"; writes it in the inactive slot through the CPS path, header last, as RadioID / air band today; install / update flow writes it automatically |
| DOTA | Works as content type "data pack" with type 3: ~423 frames, FM ~9 min, DMR ~7 min |

---

## A. Data-chip layout

The region stays **0xE70000-0xEFFFFF** (AGENTS.md rule 2, owner decision 2026-11-20). Every slot is 4 KB aligned
(`DPK_SECTOR`, datapack.h:23). Nothing outside the region is read for packs or written; calibration (factory, read
only), settings ring 0x711000, journal 0x719000, the open-firmware codeplug image 0x700000 / 0x720000 and the native
codeplug 0xF00000 (`boards/dm32/board.c` flash_map, lines 214-224 in the build tree) are never touched. `dpk_erase` /
`dpk_program` keep refusing anything outside `[DPK_BASE, DPK_END)` (datapack.c:33-62).

### A.1 Proposed layout (layout 2)

| Slot | Start | End (excl.) | Size | Payload max | Notes |
|---|---|---|---:|---:|---|
| RadioID A | **0xE70000** | 0xEA0000 | 192 KB (0x30000) | 196,544 B | same start as 1.0.6 slot A |
| Map pack A | **0xEA0000** | 0xEB0000 | 64 KB (0x10000) | 65,472 B | was the tail (bytes 192-256 KB) of 1.0.6 slot A |
| RadioID B | **0xEB0000** | 0xEE0000 | 192 KB (0x30000) | 196,544 B | same start as 1.0.6 slot B |
| Map pack B | **0xEE0000** | 0xEF0000 | 64 KB (0x10000) | 65,472 B | was the tail of 1.0.6 slot B |
| Air band A | 0xEF0000 | 0xEF4000 | 16 KB | 16,320 B | unchanged |
| Air band B | 0xEF4000 | 0xEF8000 | 16 KB | 16,320 B | unchanged |
| Free | 0xEF8000 | 0xF00000 | **32 KB** (8 sectors) | | spare inside the region |

Total 576 KB = 384 + 128 + 32 + 32. Free: 32 KB at the top.

### A.2 Why interleaved and not "RadioID A, RadioID B, map A, map B"

The compact alternative (RadioID A 0xE70000, B 0xEA0000, map A 0xED0000, map B 0xEE0000) gives the same sizes but moves
the RadioID B header from 0xEB0000 to 0xEA0000. The interleaved one wins on every compatibility point at no cost:

| Point | Interleaved (proposed) | Compact |
|---|---|---|
| RadioID pack in 1.0.6 slot B after the update | kept (header still at a slot start) | lost: 0xEA0000 holds slot A's tail or 0xFF, the user reloads |
| Older programmers (Rust 3.0/3.1, retired C# 2.0) writing a RadioID pack <= 192 KB | correct: they use 0xE70000 / 0xEB0000 (datapack.rs:70, tools/datapack.py:31), the same slots | wrong: their slot B is inside the new B payload / map slots |
| Downgrade to 1.0.6 | RadioID A and B both still valid (<= 192 KB packs) | slot B lost |
| Map pack size | 64 KB each (fixed by the 256 KB spacing of the old B header) | 64 KB each |

The price: map pack slots cannot grow past 64 KB without moving a RadioID header. With ~14.8 KB headroom (A.3) that is
fine for the data planned; if the repeater list grows a lot later, the repeater sections can move into a pack type of
their own in the free 32 KB (2 x 16 KB) without touching this layout (open question H.7).

### A.3 Map pack size

| Section | Bytes | Source |
|---|---:|---|
| directory (16 B head + 15 x 12 B entries) | 196 | new |
| map meta | 32 | new (origin, unit, bounds, counts) |
| provinces (81 x 18 B) | 1,458 | turkey_map_provinces |
| rings (85 x 8 B) | 680 | turkey_map_rings |
| steps (8,715 x 2 B) | 17,430 | turkey_map_steps |
| province names | 617 | turkey_map_names |
| district positions (973 x 4 B) | 3,892 | turkey_district_pos |
| district first (82 x 2 B) | 164 | turkey_district_first |
| district name first (81 x 2 B) | 162 | turkey_district_name_first |
| district names | 7,896 | turkey_district_names |
| repeater info | 32 | rpt_info |
| repeaters (252 x 44 B) | 11,088 | rpt_table |
| repeater key (252 x 10 B) | 2,520 | new (C.4) |
| airfields (74 x 44 B) | 3,256 | air_fields |
| air frequencies (122 x 4 B) | 488 | air_freqs |
| airfield key (74 x 10 B) | 740 | new (C.4) |
| **total** | **50,651** | + up to ~60 B of 4-byte alignment padding |

Slot payload max 65,472 B: **headroom ~14,800 B** (22.6 % of the slot; 29 % on top of the data). That holds e.g.
+250 repeaters (+13.5 KB with their key) or district data growth. Slot image ~50.7 KB = **13 sectors**.

### A.4 Address constants

`datapack.c` loses its two macros (datapack.c:11-14) for one table:

```c
/* layout 2 (docs/mappack-design.md A.1): per type its two slot starts and its slot size */
static const struct { uint32_t a, b, size; } slots[DPK_TYPES] = {
	[DPK_RADIOID - 1] = { 0xE70000u, 0xEB0000u, 0x30000u },
	[DPK_AIR - 1]     = { 0xEF0000u, 0xEF4000u, 0x4000u },
	[DPK_REF - 1]     = { 0xEA0000u, 0xEE0000u, 0x10000u },
};
_Static_assert(...)	/* every slot inside [DPK_BASE, DPK_END), 4 KB aligned, no two slots overlap */
```

The host test checks the same invariants at run time (G.1 T1).

---

## B. Migration of an installed radio

### B.1 Rule

1. **The firmware never writes the pack region on its own** (not at boot, not on update, not to "convert" a layout).
   Writes stay as today: only a CPS session's journaled sector writes from the programmer, or a DOTA receive after the
   user's Yes (`dpk_begin` / `dpk_put` / `dpk_commit`). So the update cannot corrupt anything by itself.
2. On first boot of the new firmware `dpk_scan` (datapack.c:126) reads the slots at the **new** addresses with the
   **new** sizes. Per type the valid slot with the higher sequence is in force, as now.
3. RadioID: the slot starts are unchanged, only the size check tightens (`h->len <= slot_size - 64`,
   datapack.c:72). A 1.0.6 pack in slot A or B whose payload is <= 196,544 B (**the owner's 98 KB pack: yes**) passes
   the header CRC and payload CRC exactly as before and stays in force. **The owner does not reload RadioID.**
4. A RadioID pack between 196,545 B and 261,056 B (possible in 1.0.6, e.g. a multi-country pack): magic, type and header
   CRC check but the length does not fit. New state **`DPK_BIG`** (instead of the generic "damaged"): About says
   "RadioID pack: too big for this firmware, rebuild it" ("RadioID paketi bu yazılım için büyük, yeniden oluşturun"),
   the pack is not used, the built-in fallbacks answer (codeplug contacts, CPS-written database). Nothing is written.
5. Map pack slots on an upgraded radio: 0xEA0000 and 0xEE0000 are the tails of the old 256 KB slots. With a <= 192 KB
   old pack they are erased (0xFF) -> state NONE, "Map pack: none" until one is written (D).

### B.2 Proof: no old header is misread as a new one

A header is only ever read at a slot start (`slot_ok` reads `dpk_slot_addr(type, slot)`, datapack.c:93-94; the
programmer's `read_slot` likewise, cps.rs:412-414). So only what lies at each **new** slot start matters:

| New slot start | What a 1.0.6 radio may have there | Result in layout 2 |
|---|---|---|
| 0xE70000 RadioID A | old slot A header | read as RadioID (same type, same place): valid if <= 196,544 B, else `DPK_BIG` |
| 0xEB0000 RadioID B | old slot B header | the same |
| 0xEA0000 map A | old slot A payload at offset 196,544 (only if the old pack is that big) or 0xFF | 0xFF: NONE. Payload bytes: RadioID records (3-byte ID + 6-bit text); to be taken as a map pack they would have to hold `DPK1`, type 3, format 1, a length <= 65,472 **and** a header CRC-32 over 60 bytes that matches, **and** a payload CRC-32 that matches, **and** a valid directory (C.2). Chance below 2^-64 for data not built for it |
| 0xEE0000 map B | old slot B payload at offset 196,544 or 0xFF | the same |
| 0xEF0000 / 0xEF4000 air | air band headers | unchanged |

**The task's worry "old slot B header inside a new slot's payload" does not arise in this layout**: the old B header
is still a header at a slot start of the same type. In the compact alternative (A.2) it would have been inside the
new RadioID B payload; there too it would never be read as a header (headers are read only at slot starts), but the pack
would be lost.

Type confusion is excluded by `h->type == type` (datapack.c:71): a RadioID header can never count as a map pack and
the other way round. No extra layout-version field is needed. The reserved `flags` field (datapack.h:34) stays 0.

One small fix comes with it: `dpk_hdr_ok` computes `dpk_slot_size(type) - DPK_HDR` (datapack.c:72), which wraps to
0xFFFFFFC0 for a type the build does not know (slot size 0). All callers pass a known type today (DOTA checks
`h.type <= DPK_TYPES` first, dota.c:836), but the new code checks `dpk_slot_size(type) > DPK_HDR` before the
subtraction.

And a semantic cleanup: a slot whose first 4 bytes are not `DPK1` is **NONE** (nothing committed there: the header is
written last), not BAD; BAD stays for `DPK1` with a wrong CRC / format / length. Today any non-0xFF magic is BAD
(datapack.c:96-101). This keeps an upgraded radio with old data in the map tails from showing "Map pack: damaged".

### B.3 Downgrade 1.1 -> 1.0.6 (after layout 2 was used)

| What 1.0.6 does | Effect |
|---|---|
| reads RadioID at 0xE70000 / 0xEB0000 with a 256 KB limit | a layout-2 RadioID pack (<= 192 KB by construction) is valid there: kept |
| reads air band at 0xEF0000 / 0xEF4000 | unchanged: kept |
| never reads 0xEA0000 / 0xEE0000 | map packs ignored; 1.0.6 has the tables in program flash: map, repeaters, airfields work |
| 1.0.6 or an old programmer writes a RadioID pack <= 192 KB | erases only the sectors of header + payload (datapack.c:239, datapack.rs:184): the map slots are not touched |
| 1.0.6 or an old programmer writes a RadioID pack > 192 KB | it overwrites (part of) a map slot. Back on 1.1 that RadioID pack is `DPK_BIG` (not used) and the overwritten map slot fails its CRC: the other map slot is used, or "Map pack: damaged, reload". **No silent corruption**, only the need to reload |

DOTA between versions: a 1.0.6 receiver refuses a type-3 HELLO (`h.type > DPK_TYPES`, dota.c:836) without writing;
a 1.1 receiver refuses a RadioID pack > 196,544 B from a 1.0.6 sender (`dpk_hdr_ok` length check) without writing.

### B.4 A big old RadioID pack and the map slots (N1, N2)

- **Writing a map pack over a `DPK_BIG` RadioID pack's tail (N1).** On a 1.1 radio a RadioID pack > 196,544 B is not
  used anyway (B.1 rule 4). One rule for both writers: **the map pack may be written**; its slot erase destroys that
  pack's tail. The firmware (`dpk_begin(DPK_REF)`, DOTA receive) does not refuse; the programmer 3.2 does not refuse
  either but warns first: "Your RadioID pack is too big for 1.1 and is not used; writing the map pack ends it for
  1.0.6 too. Rebuild it under 192 KB." (Revision 1 had the programmer refuse; that was inconsistent with DOTA.) The
  only loss is for a later downgrade to 1.0.6, where that big pack would have been valid.
- **An old programmer writing a > 192 KB RadioID pack to a 1.1 radio (N2).** Rust 3.0/3.1 and the retired C# 2.0
  allow 256 KB (datapack.rs:58, tools/datapack.py:30). The inactive RadioID slot's write then runs into the next map
  slot (from 0xEA0000 or 0xEE0000), **possibly the map pack in force**, and the old programmer reports PASS (it checks
  its own slot only). On 1.1: that RadioID pack is `DPK_BIG` (not used), the damaged map slot fails its CRC, the other
  map slot is used if valid, otherwise "Map pack: damaged, not used" and the user reloads it. Nothing silent, but a
  loss the old programmer does not report. Documented for 1.1 in the release notes ("use programmer 3.2 or later");
  3.2 refuses a RadioID pack > 196,544 B for every firmware.

---

## C. Pack format and how the firmware reads it

### C.1 One pack type with a directory (chosen) vs several types

| | One type `DPK_REF` with sections (chosen) | Separate types (map, repeaters, airfields) |
|---|---|---|
| Slots | 2 x 64 KB | 6 slots; with 4 KB granularity at least 2 x (36 + 16 + 8) KB = 120 KB, little headroom each; no room left for the layout in A.1 |
| User | one "Harita paketi" row, one write, one DOTA send | three rows, three writes; partial states (map without repeaters) visible |
| Release | one asset | three assets |
| Licence split (repeater data, H.5) | a section can be left out (count 0) and the rest still works; a user-built repeater section can replace it later | natural |

One type is chosen. Each feature checks only its own sections, so a pack without repeater sections still gives the map
and the airfields (H.5).

### C.2 Payload format 1 (all little-endian, every section 4-byte aligned)

Directory head (16 B):

| Off | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `REF1` |
| 4 | 1 | directory version (1) |
| 5 | 1 | section count n (<= 16) |
| 6 | 2 | reserved 0 |
| 8 | 4 | map coordinate id: CRC-32 (`storage_crc32`) of the compiled coordinate constants as 7 LE int32 (`TURKEY_MAP_LON0_E7`, `LAT0_E7`, `UNIT_E7`, `XMIN`, `YMIN`, `XMAX`, `YMAX`) followed by the charset text; the firmware computes the same at build time (a `static const` from the builder) and treats a mismatch as "for another version" (revision 1 had an XOR, which two different origins can share) |
| 12 | 4 | reserved 0 |

Then n entries of 12 B: `id` u16, `rec` u16 (record size, 1 for blobs), `off` u32 (from payload start), `count` u32.

| id | Section | rec | count | Firmware 1.1 use |
|---:|---|---:|---|---|
| 1 | MAP_META | 32 | 1 | origin, unit, XMIN..YMAX, PROVINCES, RINGS, STEPS, DISTRICTS (checked against compiled constants where the code depends on them) |
| 2 | PROVINCES | 18 | 81 | `turkey_map_province_t` |
| 3 | RINGS | 8 | 85 | `turkey_map_ring_t` |
| 4 | STEPS | 2 | 8,715 | `int8_t[2]` |
| 5 | PROV_NAMES | 1 | 617 | NUL-separated, ISO-8859-9 |
| 6 | DIST_POS | 4 | 973 | `turkey_map_pt_t` |
| 7 | DIST_FIRST | 2 | 82 | `uint16_t` |
| 8 | DIST_NAME_FIRST | 2 | 81 | `uint16_t` |
| 9 | DIST_NAMES | 1 | 7,896 | NUL-separated |
| 10 | RPT_INFO | 32 | 1 | `rpt_info_t` (count, sample, date, source: About's "Repeater data: ...") |
| 11 | RPT | 44 | n | `rpt_entry_t` (rpt.h:31-41), sorted by RX as today |
| 12 | RPT_KEY | 10 | n | lat_e5 i32, lon_e5 i32, prov u8, mode u8 (same order as 11) |
| 13 | AIR | 44 | n | `air_field_t` (airfield.h:34-43) |
| 14 | AIR_FREQ | 4 | n | `air_freq_t` |
| 15 | AIR_KEY | 10 | n | lat_e5, lon_e5, prov, kind |

The records keep the exact C struct layouts the firmware has today (`_Static_assert` sizes in rpt.h:42 and
airfield.h:44; the csky and host ABIs agree for these field types), so the readers copy a record into the same struct
and the existing math (rpt.c, airfield.c, map.c) keeps its code paths.

Versioning: the pack header's `fmt` (datapack.h:33) is 1; the directory version is 1. **Unknown section ids are
ignored** (a later pack can add e.g. a richer repeater record for NXDN RAN without breaking 1.1). A known id with a
different `rec` makes that feature "pack for another version" (treated as no data for that feature). A new `fmt` value
is refused by 1.1 (`h->fmt == 1u`, datapack.c:71): reserved for an incompatible change.

**Structural validation** (`ref_validate(slot_addr, len)`), all arithmetic in **uint32_t** (map.c:215 loops with a
`uint16_t i`, so a `first + count` that wraps 16 bits must be refused, not trusted):

1. Directory: magic, version 1, `n <= 16`, every entry `off % 4 == 0`, `off + rec * count <= len` (u32, overflow
   checked: `count <= (len - off) / rec`), no two known sections overlap; a known id with a wrong `rec` fails.
2. MAP_META counts equal the section counts; coordinate id equal (C.2 head); provinces == 81; rings, steps,
   districts each < 65,536 (the firmware's index types).
3. Every ring: `first + count <= steps`, `count >= 1`. Every province: `first_ring + ring_count <= rings`,
   `name < PROV_NAMES` length, `min <= max`.
4. Districts: DIST_FIRST has 82 entries, non-decreasing, `first[0] == 0`, `first[81] == DIST_POS count`; DIST_NAME_FIRST
   offsets non-decreasing and `< DIST_NAMES` length; walking each province's names from its offset finds exactly
   `first[p+1] - first[p]` NUL-terminated names inside the blob.
5. Name blobs (5, 9) end with NUL.
6. Repeaters: RPT and RPT_KEY counts equal, < 65,535 (`RPT_NONE`); each key equals its record's lat / lon / prov / mode
   (one pass, 13.6 KB, ~15 ms). Airfields: AIR and AIR_KEY counts equal, < 65,535; every `first + count <= AIR_FREQ`
   count, `count <= 64` (`AIR_PACK_FREQS`).

**All or nothing per slot (S1):** a slot whose sections present all validate is a valid slot; **any known section that
fails makes the whole slot BAD**, even if the map part alone would be fine (simple and predictable: a builder bug or a
crafted pack never mixes). A section that is absent is not a failure: that feature is just off (a release pack without
repeater sections, H.5). Of two valid slots the higher sequence wins, as for the other types, so a bad newer pack
never displaces a good older one.

Where it runs (S1): (a) in `slot_ok` for type 3, after the payload CRC, so `scan_type` only ever picks a validated slot;
(b) in `dpk_commit` for type 3, **after the payload CRC read-back and before the header is programmed**: a DOTA
transfer of a pack that checks by CRC but not by structure is never committed (the receiver ends "Bad pack", the old
pack stays); (c) in the programmer 3.2 (`refpack.rs`) before it writes, and in `tools/refpack.py` after it builds. The
CPS path cannot be validated before its header sector arrives (the firmware does not see the session's plan), so for
it (a) is the guard: a bad slot is simply never in force.

Validation reads raw slot bytes through `rd()` (datapack.c:38-42) in its own 64-byte buffer, as `payload_crc` does:
never `dpk_read` (which reads the slot **in force**, not the one being checked) and never the UI window (S2). Cost
~4 KB of small sections + the 13.6 KB repeater pass: **~20 ms per slot**. This matters because a pack can arrive over
DOTA from any radio; reads can never leave the slot anyway (`dpk_read` bounds, datapack.c:148-153).

**Sanitising copied records (S3):** every reader that copies a record out forces the string ends, whatever the pack
holds: `rpt_entry_t.name[RPT_NAME_MAX] = 0`, `.city[RPT_CITY_MAX] = 0` (rpt.h:39-40); `air_field_t.code[AIR_CODE_MAX]
= 0`, `.name[AIR_NAME_MAX] = 0` (airfield.h:39-40); `rpt_info_t.date[10] = 0`, `.source[15] = 0`; names copied from the
blobs stop at the caller's buffer size and are always terminated. `air_freq_t.type` is clamped as air_pack.c:74 does.
Indexes taken from a record (a ring's `first`, a province's `name`) were range-checked by the validation; the readers
still bound every loop by the validated counts kept in the directory copy.

### C.3 Reading without a RAM copy

New file `integration/radiofoundry/datapack/ref_pack.{c,h}` (installed as `core/storage/ref_pack.c`):

- `ref_validate(slot_addr, len)` called from `slot_ok` and `dpk_commit` for type 3 (C.2). It writes **no shared
  state**: it only answers valid / not valid. The scan then publishes `cur[]` as today (scan_type's "found first,
  then taken" order, datapack.c:118-123).
- **Publication without a race (S2).** `scan_type` runs in other tasks: the DOTA storage job on the start-up task
  (dota.c:266-300) and the CPS close (`dpk_scan` after `end_session`, patched in by plus_datapack.py:84-86), while the
  UI task may be drawing. So the directory the readers use is **owned by the UI task**: a 100-byte copy (per needed
  section its offset u32 and count u16, the feature bits, and its generation = the slot address and `seq` it was
  loaded from). At the start of every UI use (`ref_has`, the first read of a draw), the UI compares
  `dpk_active(DPK_REF)` (slot, seq) with its generation; on a change it reloads the directory (196 B, one `dpk_read`)
  and drops the window. No other task ever writes the UI copy; the scan task only writes `cur[]`, which is already
  published in an order where a reader sees the old pack or the new one, never a half state.
- What a draw in progress sees when a new pack takes over mid-draw: it keeps reading the **old** slot through its own
  cached directory (the old slot is not erased by a commit; it is erased only when the next pack starts writing into
  it, a whole transfer later). Even in that unlikely case (two writes within one draw), every loop is bounded by the
  cached validated counts and every read by `rd` stays inside the region: at worst one frame draws wrong lines.
- `bool ref_has(uint8_t feature)`.
- Record readers: `ref_province(p, turkey_map_province_t *)`, `ref_ring(r, turkey_map_ring_t *)`,
  `ref_rpt(i, rpt_entry_t *)`, `ref_rpt_key(i, ref_key_t *)`, `ref_air(i, air_field_t *)`, `ref_air_freq(k, air_freq_t *)`,
  `ref_dist_pos(d, turkey_map_pt_t *)`, `ref_dist_first(p)`, name copy-out `ref_name(section, off, char *out, size)`.
- A **128-byte read window** (`dpk_win_read`): a static buffer with its tag (slot address, sequence, offset): a read
  inside the window is a `memcpy`; a miss refills 128 bytes with one `board.flash->read`. Used for the sequential
  streams (steps, key passes, name walks). Small records read once (province 18 B, ring 8 B) go through the window too;
  it is invalidated when the slot or sequence changes (a new pack took over), so no stale bytes after a write.
- **UI task only.** Every reader of these tables runs in the UI task (map, lists, auto repeater tick, cross-band form);
  the DOTA sender keeps using `dpk_read` directly (dota.c:456). The window is not locked; a comment and a debug assert
  name the owning task. A future reader in another task gets its own window.

### C.4 The hot paths and why there are key sections

SPI read path (build tree): `board.flash->read` -> `flash_spi0.c` `rd()` (takes the data-chip mutex, lines 83-92) ->
`spi_nor_read` (`CMD_READ` 0x03, 4-byte command, spi_nor.c:111-119) -> `hrc_ssi_xfer` (ssi.c:55-71), which moves **one
byte at a time with three status polls per byte**, SCLK = APB / 4 = 21 MHz (flash_spi0.c:62), CPU 168 MHz
(clock_hz.h:13). No measured figure exists in the docs; **estimate: ~0.8-1.2 us per byte (0.38 us on the wire plus the
polls) and ~6-10 us per call** (mutex, CS GPIO, command bytes). The program-flash tables today are read through XIP
with cache: effectively free. G.2 R3 measures it.

| Path | Today | From the pack (estimate, 1 us/B + 8 us/call) |
|---|---|---|
| Map draw, whole country (`map_draw`, map.c:202-232: every step of every province in view) | XIP | 34.9 KB steps through the window: ~35 ms + 273 refills x 8 us = ~37 ms; provinces / rings ~2 ms. **A 2-byte read per step without a window: 8,715 x ~10 us = ~90 ms**, hence the window |
| District labels (`map_districts_draw`, map.c:372-393) | XIP | names 7.9 KB + positions 3.9 KB: ~13 ms when drawn |
| "You are in" (`map_province_near`, map.c:277-320, every map redraw, ui_map.c:557) | XIP | only provinces whose box is near: typically 1-3 KB of steps, ~3 ms |
| Repeater list redraw (`rpt_next_in` = one pass a row, rpt.c:43-61; ~10 rows + `rpt_rank_in`) | XIP | with the full 44-B records: 11 KB a pass, ~12 passes = **~135 ms a key press**. With RPT_KEY (2.5 KB a pass, lat/lon/prov is all `key()` and `in_prov()` read, rpt.c:20-41): **~30 ms**. Rows then read their 44-B record once each (~0.5 ms) |
| Auto repeater (`arpt_check` every `ARPT_EVERY_MS` = 30 s, mapnav.h:104; `rpt_nearest_if` over all 252, rpt.c:63-80) | XIP | key pass 2.5 ms; the full record (for `arpt_suitable`) read only for an entry nearer than the best so far: ~3-6 ms every 30 s |
| Cross-band / DOTA frequency check (`known_repeater`, ui_xband.c:37-48) | XIP | **stays XIP**: the frequency set in program flash (C.7), a binary search |
| Airfield list redraw (74 airfields) | XIP | AIR_KEY passes: ~1 ms each, ~12 per redraw: ~12 ms |
| Air band pack match (`air_pack.c` `find`, compares `air_fields[i].code`) | XIP + chip | the code from `ref_air` (one 44-B read) |
| Boot / CPS close / DOTA commit: CRC + validation of **every** map slot holding a header (`scan_type` checks both slots, datapack.c:111-117) | none | per slot ~50.7 KB CRC (`payload_crc`, 64-B reads, datapack.c:76-88: 792 calls, ~55-60 ms) + validation ~20 ms = ~75-80 ms; **two map packs (the normal state after the second write): ~150 ms more boot time**; one pack ~75-80 ms. (Revision 1 said 55-65 ms for one slot only.) RadioID packs already cost ~100 ms each the same way. Possible later saving, not in this job: check the slot in force first and the other only when it would win (higher seq) |

Key sections cost 3,260 B of pack space, no program flash beyond their reader. They are also exactly what the later
**Repeater scan** and **Auto airfield scan** need: a nearest-N selection is one key pass (2.5 KB, ~3 ms) with an N x 2 B
index list in RAM; refreshing it every 30 s while driving costs ~3-15 ms. Modes for "FM / DMR / NXDN only" filtering
are in the key (`mode` byte) without reading the full record. Nothing in this design blocks either feature.

Window size: 128 B, not 256: the per-call overhead (~8 us) is small next to the per-byte cost, so a bigger window
saves under 2 ms on a whole-country draw and costs 128 B of RAM.

**Redraw rate (S4).** The UI core marks the screen dirty on every radio view-serial change (ui_app.c:2772-2776 in the
build tree: frequent while receiving: RSSI, caller, call state) and at least every `REFRESH_MS` = 1 s (ui_app.c:95,
2796), and a dirty screen is drawn whole. On the map that would re-read the whole country from the chip on every
change: ~37 ms of reads (+ the ~3 ms province test + districts at close zoom, ~53 ms in the worst case) several times
a second during a call. Rule for the implementation:

- The map screen keeps a **map key**: centre, zoom, the province drawn bright, the picked repeater / station, district
  labels on / off, the track length and coverage flag, and the map pack generation (C.3).
- The **map layer** (borders, province names, districts, repeater triangles: everything read from the pack) is read
  from the chip only when the key changed since the last drawn frame **or** the screen was entered / uncovered. Between
  such frames a dirty caused only by the radio view redraws the parts that do not need the pack (status strip, the
  "here" / info line, our position mark) - via the UI core's partial redraw if it has one, else by keeping the map
  layer's pixels: the implementer checks first whether the frame buffer survives between draws of the same screen
  (`gfx` / `ui_app.c`); if it does not, the fallback is to redraw the map layer at most once per `REFRESH_MS` while the
  key is unchanged and to draw the cheap parts on the other dirties (stale-for-a-second map pixels under unchanged
  borders are invisible). Which of the two is possible is an implementation check, named here so it is not missed.
- T10 and MP3 measure this **during RX traffic** (a DMR call with RadioID names being looked up), not on a quiet radio.

**Mutex contention.** The data-chip mutex (flash_spi0.c:36-51) is taken **per read call**, not per draw, so another
task waits at most one window refill (128 B, ~0.15 ms) for a map reader. The radio task does read the same chip during
calls: `dpk_id_text` (datapack.c:161-199, a binary search of ~14 small reads per new caller) and the CPS-written ID
database; each of its reads may wait one refill: negligible. Storage jobs hold the mutex for a sector erase (~45 ms
typical, up to 400 ms, W25Q128 class): a map draw that meets a settings save stalls that long once (visible, no fault).
The radio task never reads the map pack (marine and the TX refusal set stay in flash, C.6 / C.7), so the PTT / TX path
gets no new SPI dependency.

### C.5 API changes in the users (pointer -> copy)

The tables are used through pointers into const memory today (`&rpt_table[i]`, `&air_fields[i]`,
`map_province_name()` returning a `const char *` into flash). With the chip:

- `rpt_info.count` -> `rpt_count()` (0 without RPT data); `&rpt_table[i]` -> `ref_rpt(i, &e)` into one **static**
  scratch entry per module (not on the stack: the UI task's deepest path had **184 B free** in the last stack check,
  docs/plus.md 3821; a 44-B local in a deep draw path could break the margin). The stack check (`--stack-check`)
  stays the gate.
- `air_field_count` -> `air_nfields()`; `&air_fields[i]` -> `ref_air(i, &f)`. `air_freq(i, k)` returns a pointer
  (air_pack.c:62-78); with the table gone its fallback (`&air_freqs[first + k]`) becomes a copy into **one static
  scratch** `air_freq_t` (the pack branch already does this, air_pack.c:67). So **no caller may hold two `air_freq`
  results at once**; the implementer checks every caller (ui_airfield.c, ui_airwatch.inc, the "+N" type logic) and
  copies the value where two are compared.
- `map_province_name(p)` -> `map_province_name(p, char *buf, size_t n)` (callers: ui_map.c:248, 443, 445; ui_rpt.c:224,
  244; ui_airfield.c:223, 242); `map_district_name(d)` likewise (ui_map.c:445-447 compares the two: two static 24-B
  buffers); `map_districts_draw` hands each name from the window walk to its callback in a 24-B buffer.

### C.6 Marine stays in program flash (deviation from decision 2; owner question H.1)

`marine_table` (590 B) is read by `marine_hz()` (marine.h:55-56), which the radio core calls on every TX request of
the VFO: `if (sm->view.vfo && (marine_hz(ch->rx_hz) || marine_hz(ch->tx_hz)))` -> TXDENY_RX_ONLY (patched in by
tools/plus_misc.py:110). Moving it would (a) remove the marine receive-only refusal on a radio without a map pack,
(b) put a data-chip read (mutex, possibly behind a 400 ms erase) on the PTT path in the radio task, (c) save only ~300 B
net after its reader and fallback. The owner's rule "no unrequested restrictions" also rules out the stricter fallback
"refuse TX on all of 156-162 MHz without a pack". Recommendation: keep marine in flash. If the owner still wants it
moved, the TX check needs its own fallback decision.

### C.7 The repeater frequency set stays in program flash (B1)

`known_repeater()` (ui_xband.c:37-48) is not only the cross-band check: `hz_why()` -> `ui_xband_refusal()`
(ui_xband.c:69-82) is also **DOTA's band-plan check** (ui_dota.c:117 the source channel, :460 a typed frequency,
:479-489 `begin()` on Send / Ready). The band-plan segments in xband.c:21-30 cover only RV48-RV63 and the RU segments;
repeaters outside them are refused only through the list. If the list lived only in the pack, a radio without a map
pack would silently stop refusing those frequencies for DOTA; failing closed instead would block receiving the map
pack itself over DOTA. So the TX refusals keep their own data in program flash, **independent of the pack**:

- `rpt_freqs.c`, generated by `tools/rpt_list.py` from the same rows as the pack's RPT section, at every prepare:
  every repeater's output (`rx_hz`) and, when `off != 0`, its input (`rx_hz + off x 1.25 kHz`); sorted, duplicates
  removed; two `uint16_t` arrays in 1.25 kHz steps: VHF from 136 MHz (up to 174 MHz: 30,400 steps) and UHF from 400 MHz
  (up to 480 MHz: 64,000 steps); frequencies outside both bands are dropped (the radio cannot transmit there, and
  `xband_check_hz` / the band check refuse them already).
- Size: 252 repeaters, 101 DMR + 151 FM, at most 504 frequencies, ~350-450 after merging shared ones: **~0.7-0.9 KB
  data + ~150-250 B code** (binary search for the nearest entry, then the same +-6,250 Hz test as today) = **~0.9-1.3
  KB**. Revision 1's saving shrinks by that.
- `known_repeater(hz)` uses only this set, for cross-band **and** DOTA, with or without a pack, with the same result as
  today (the same rows, the same tolerance). `rpt_freqs` holds only frequencies: no names, places or positions.
- A host test proves equality: for every frequency from 136 to 480 MHz in 625 Hz steps, `known_repeater` on the set
  equals the old loop over `rpt_table` (G T15).
- It is derived from the same repeater rows, so it carries the same licence question as the pack's repeater sections
  (H.5): the release gate (E.2) applies to it as well.

---

## D. No pack / damaged pack

Principle: **no crash, no wrong data, Home and boot unaffected.** Every user path asks `ref_has(MAP|RPT|AIR)` first;
without the feature it shows one clear state. Texts (EN / TR, added to the Turkish table):

- "No map data. Load the map pack (PC programmer)." / "Harita verisi yok. Harita paketini yükleyin (PC programı)."
- "No repeater data: load the map pack" / "Röle verisi yok: harita paketini yükleyin"
- "No airfield data: load the map pack" / "Meydan verisi yok: harita paketini yükleyin"
- Settings > About: "Map pack: DDM32 TR 1.1.0, 2026-11-19, 50 KB" / "none" / "damaged, not used" / "for another
  version" (`dpk_about` / `dpk_describe`, datapack.c:201-229, as for RadioID and air band).

| Place | Today (code) | Without MAP / RPT / AIR data |
|---|---|---|
| Tools > Map | ui_map.c:539 `map_draw_screen` | the map area shows the "No map data" text; GPS position, our track (CONFIG_MAP_TRACK), coverage and APRS stations still draw over an empty frame (they need no table); keys work |
| Map: province / district line "here" | ui_map.c:441-447 | omitted (lat/lon / locator line only) |
| Map: repeater triangles, OK / context pick | ui_map.c:307-327, 700-702 | not drawn; context key toast "No repeater data: load the map pack" (replaces "No repeaters in this build") |
| APRS station "show on map" | ui_aprsx.c:270 | opens the map as above |
| Tools > Repeaters | ui_rpt.c:340-341, 631 | the existing empty path, new text |
| Auto repeater (banner on Home) | ui_mapnav.c:41-66, `arpt_check` | toggling on: toast "No repeater data: load the map pack", stays off; Home itself unchanged |
| Tools > Airfields, air watch (#) | ui_airfield.c:486 (`air_field_count == 0`) | the existing empty path, new text; the air watch cannot start (no frequencies) |
| Air band data pack (user's AIP pack) | air_pack.c:19-46 | not applied (it names airfields of the table); About still shows it |
| Cross-band and DOTA repeater-frequency refusal | ui_xband.c:73 `known_repeater`, used by ui_dota.c:117, 460, 479-489 | **unchanged with or without a pack**: the frequency set in program flash (C.7). Cross-band starts and DOTA sends / receives as today (so a map pack can be received over DOTA by a radio that has none) |
| DOTA "What: Map pack" | ui_dota.c:130 | greyed "none", Send: "Nothing to send" (as the other packs) |
| Tools > Marine VHF, marine TX refusal | marine.c | unaffected (stays in flash) |
| Locator (Tools > Locator), Trip meter, GPS dashboard | fieldx/ui_locator.c, toolbox/tripm.c, ui_gpsdash.c | **unaffected**: checked, none of them reads the moved tables (they use only GPS math) |
| Home, boot, codeplug, scan, all radio modes | | unaffected; boot adds the pack CRC check (~60 ms) only when a pack exists |

Damaged pack (CRC or validation fails): the same as none, About says "damaged, not used"; if the other slot holds a
valid older pack, that one is used (two-slot rule).

---

## E. Where the pack comes from and how it reaches the radio

### E.1 Built by the firmware build, from the same sources

- New `tools/refpack.py` (Python stdlib): reads the generator outputs the firmware links today:
  `integration/radiofoundry/map/turkey_map_data.c` and `turkey_district_data.c` (committed, generated by
  `tools/turkey_map.py` / `tools/turkey_districts.py` from pinned downloads), `integration/radiofoundry/air/
  airfield_data.c` (committed, `tools/airfields.py`) and `build/rpt/rpt_data.c` (generated at every prepare by
  `tools/rpt_list.py`, plus_features.py:4053-4081). It parses the C arrays (the parser `rpt_list._c_array` already does
  this for the map tables, rpt_list.py:86) and writes the sections with explicit `struct.pack` layouts.
- Cross-check: a host test compiles the same `.c` tables with gcc and serialises the arrays from C; the bytes must equal
  the Python output section for section (G.1 T3). So the pack is exactly what the firmware would have had in flash.
- `tools/build_radiofoundry.py --plus [--public]` writes `build/dm32-p25-plus*/ddm32-ref.dpk` (sequence 0, as
  `datapack.py` images; name "DDM32 TR <version>", date = the newest source date, tag "build <version>"), records its
  SHA-256 and size in `rfoundry_dm32.json`, and `--verify-rebuild` also requires the pack to rebuild byte-identical
  (no build time inside).
- The firmware no longer links the four data `.c` files; a new link audit (like `plus_brand.public_audit` for `iddb_*`)
  refuses an ELF holding `turkey_map_steps`, `turkey_district_names`, `rpt_table` or `air_fields`.

### E.2 Release: a separate signed asset

The firmware zip **must stay at its three files**: the Rust programmer refuses any other zip content
(`client.rs:400-410`, `PACKAGE_FILES` releases/mod.rs:72), so a pack inside the zip would make programmer 3.1.0 refuse
the whole release. Instead:

- `pc/release/make_release.py` takes `--ref-pack build/.../ddm32-ref.dpk` and adds the pack **inside the firmware's
  entry** (N5: the pack belongs to that firmware version; a manifest listing several firmware versions keeps each
  one's pack with it): `"firmware": [{..., "ref_pack": {"file": "ddm32-ref-1.1.0.dpk", "url": ..., "sha256": ...,
  "size": ..., "pack_type": 3, "pack_format": 1, "date": "20261119", "min_firmware": "1.1.0"}}]`. The manifest is
  signed as a whole (Ed25519, make_release.py:337-340), so the pack's SHA-256 is covered by the signature. `--verify`
  (line 350+) checks the asset like the others.
- `min_firmware` means **the oldest firmware that uses the pack** (N3), not the oldest that may receive it: writing it
  to an older firmware is allowed and harmless (1.0.6 never reads 0xEA0000 / 0xEE0000), which is what the pack-first
  update order needs (H.3).
- `pc/radiotools/signed_manifest.py` `check_manifest` (line 113) and the Rust `signed_manifest.rs` `check_manifest`
  (line 230) validate the optional object. Older programmers read only the keys they know inside a firmware entry, so
  they ignore it and still install the firmware (risk H.2). **To prove before release**: a test feeds a manifest with
  the new object to programmer 3.1.0's parser (its own test suite at tag 3.1.0) and to the Python one, and both accept
  it; if either rejects unknown keys in a firmware entry, the object moves to the top level (revision 1's place).
- **Release gate (S6).** By default the build reads the owner's `research-data/` (plus_features.py:4053-4081): with
  the TRAC CSV present, `rpt_info.source` is "own list" or "own+RadioID.net" (rpt_list.py:442), so a public pack (and the
  frequency set of C.7) would carry the owner's TRAC data. `make_release.py` therefore checks the pack's RPT_INFO
  source and the build manifest's recorded repeater source against an allowed list that encodes the owner's H.5
  answer. **Until the owner answers, the default refuses "own" in a public release** (the coordinator's recommendation,
  option (a); not a decision taken here). It also refuses a pack whose header or structure does not check, and
  "sample" repeater data. `tools/build_radiofoundry.py --public` records the source in `rfoundry_dm32.json` so the gate
  can check the firmware's frequency set too.

### E.3 Rust programmer 3.2 writes it

Same CPS path as RadioID / air band (docs/plus.md 3667-3679): start the session, read both slot headers of type 3,
pick the inactive slot, set the next sequence, write the payload sectors, **the header sector last**, read each sector
back, close with `C 5` so the firmware rescans.

- `datapack.rs`: `REF = 3`; `slot_size`: RadioID **0x30000** (also when talking to a 1.0.6 radio: the stricter limit is
  safe for both layouts since the slot starts are the same), REF 0x10000, AIR 0x4000; `slot_address`: REF 0xEA0000 /
  0xEE0000.
- `cps.rs`: `read` lists three types; `write` accepts type 3 (cps.rs:480-485 today accepts only 1 and 2); before writing
  type 3 it validates the structure (`refpack.rs`, C.2), **warns** (does not refuse, B.4 N1) when a RadioID slot holds a
  pack > 196,544 B, refuses a RadioID pack > 196,544 B for every firmware (B.4 N2), and tells the user when the
  firmware is older than 1.1 that the map pack takes effect after the update (the CPS info `git` field carries
  `RFOUNDRY_VERSION` "ddm32-plus-1.x" cut to 15 characters: cps_openfw.c memcpy, build_radiofoundry.py:645; to be
  confirmed on the radio). A map pack written to a 1.0.6 radio is harmless (1.0.6 ignores 0xEA0000), so the check is for
  the user, not for safety.
- Install / update flow: after a firmware install of a release with `ref_pack`, a second step "Harita paketi" writes the
  pack (skipped when the radio's map pack in force has the same CRC and length). Order: H.3.
- GUI `packs.rs`: a third row "Harita paketi" with the slot lines and a "Yükle" button (from the release folder or a
  `.dpk` file).

Write time over CPS (estimate): per sector 4 KB data in 32-B blocks with an ACK each, read back, commit; ~1.5-2 s a
sector at 115,200 baud -> **13 sectors ~20-30 s**. Radio-measured in G.2.

### E.4 DOTA

Content type "data pack" (docs/dota.md 20) carries type 3 unchanged: `dota_req_t.pack = DPK_REF`, the What list gains
"Map pack" ("Harita paketi"), the receiver's question names it. 50,651 B / 120 B = **423 frames** (limit 2,240). At the
steady rates of docs/dota.md 8 (FM ~93 B/s, DMR ~124 B/s): **FM ~9 min, DMR ~7 min**, plus 13 sector erases. The PC
path is the better one; DOTA is for a radio without a PC nearby.

### E.5 Licence and source of the data (nothing private in a release)

| Data | Source (THIRD_PARTY.md 59-64) | Licence | In the release pack |
|---|---|---|---|
| Province outlines, names | Natural Earth 5.1.1 admin-1 (pinned SHA-256, tools/turkey_map.py) | public domain | yes |
| Districts | GeoNames TR.zip of 2026-11-19 (pinned SHA-256, tools/turkey_districts.py) | **CC BY 4.0**: attribution "District data: GeoNames (CC BY 4.0)" must travel with the pack: About keeps the line (compiled, `TURKEY_DISTRICT_ATTRIBUTION`), the release notes and THIRD_PARTY.md name the `.dpk` asset | yes |
| Airfields, frequencies | OurAirports of 2026-11-19 (pinned, tools/airfields.py) | Unlicense (public domain) | yes |
| Repeaters (pack sections 10-12) **and the frequency set in program flash (C.7)** | RadioID.net `rptrs.json` + the owner's TRAC list (`research-data/`, git-ignored) | RadioID.net: data (c) RadioID.net, credited, never committed; **TRAC: no licence stated** | **owner decision H.5**; enforced by the release gate (E.2), default until answered: no "own" (TRAC) data in a public release |

The docs contradict each other on the repeater data today, and this design does not resolve that: docs/plus.md:2201
says the TRAC list is "the owner's own radio only, never in a published build", docs/plus.md:3381 says "The repeater
list ships (RadioID.net repeaters + the owner's TRAC list)", and THIRD_PARTY.md:64 says an image carrying it "is for the
owner's radio until TRAC's permission is clear". Moving the list into a separate downloadable `.dpk` makes it more
visible, not less. The frequency set of C.7 is derived from the same rows (frequencies only, no names), so the same
question covers it. Options in H.5.

Nothing private enters the pack: no backup, calibration, codeplug, RadioID user list or AIP data; `refpack.py` reads
only the four table sources above. `make_release.py`'s existing private-file guards (backup folders, manifests, raw
images, lines 169-174) apply to the new asset too.

---

## F. Code changes

### F.1 Firmware (`integration/radiofoundry/`, installed by the `tools/plus_*.py` prepares)

| File | Change |
|---|---|
| datapack/datapack.h | `DPK_REF = 3`, `DPK_TYPES = 3`; `DPK_BIG` state; `dpk_win_read` |
| datapack/datapack.c | slot table (A.4); `dpk_hdr_ok` unknown-type fix; NONE vs BAD by magic; `DPK_BIG`; `slot_ok` and `dpk_commit` call `ref_validate` for type 3 (before the header write); the 128-B window |
| datapack/ref_pack.{c,h} (new) | `ref_validate` (raw slot reads), the UI-owned directory copy with its generation, feature bits, record / stream / name readers with sanitising (C.2, C.3) |
| rpt/rpt_freqs.h + generated `build/rpt/rpt_freqs.c` (new, program flash) | the TX-refusal frequency set and its lookup (C.7) |
| datapack/air_pack.c | codes from `ref_air` instead of `air_fields[i].code` |
| map/map.c, map.h | every table access through ref_pack (map_draw, inside, map_province_at / _near, names, districts); names copied out; `map_ok()` |
| map/turkey_map_data.h, turkey_district_data.h | stay for types and the coordinate constants; counts become pack values; the `.c` files are no longer installed into the firmware (host tests keep them) |
| map/ui_map.c | "No map data" state; repeater overlay via `ref_rpt`; name buffers; the map key and layer-redraw rule (C.4, S4) |
| map/mapnav.c, mapnav.h, ui_mapnav.c | auto repeater via key + `ref_rpt`; toggle refused without RPT |
| rpt/rpt.c, rpt.h, ui_rpt.c | `rpt_count()`, key-based `key()` / `in_prov()`, `ref_rpt` for rows, info, tune, add channel; empty text |
| air/airfield.c, airfield.h, ui_airfield.c, ui_airwatch.inc | `air_nfields()`, key-based order, `ref_air` |
| xband/ui_xband.c | `known_repeater` on the flash frequency set (C.7); used unchanged by DOTA through `ui_xband_refusal` |
| dota/dota.c, dota.h, ui_dota.c | third pack type in What (`WHATS` / `what_i`, ui_dota.c:51-56, today "3 + rq.pack" for two types), labels, "Bad pack" result when `dpk_commit` refuses the structure |
| marine/* | none (C.6) |
| lang (Turkish table, tools/turkish_ui.py) | ~8 new strings |

Tools: `tools/plus_datapack.py` (install ref_pack; About lines, see F.5), `tools/plus_features.py` `prepare_map`
(2204), `prepare_repeaters` (4053: also generates and installs `rpt_freqs.c`), `prepare_mapnav` (4691),
`tools/plus_air.py`, `tools/plus_airprov.py` (stop installing the data `.c`), `tools/rpt_list.py` (the frequency set
output), `tools/plus_brand.py` (About's repeater source from the pack, lines 56-60; the link audit),
`tools/plus_polish.py:37` (same line), `tools/build_radiofoundry.py` (pack output, repeater source in the manifest,
verify-rebuild), `tools/refpack.py` (new), `tools/datapack.py` (slot sizes, type 3: lines 30-31),
`pc/release/make_release.py` (asset, release gate), `pc/radiotools/signed_manifest.py`. `.gitignore` allowlist entries
for every new file.

### F.5 Implementation traps found by the review (N4)

- `cps.rs` `slot_json` (cps.rs:448) and the `plan` line (cps.rs:554) print every non-RadioID type as `"air"`; the
  write filter (cps.rs:480-485) accepts only types 1-2. Both need a three-way type name ("radioid" / "air" / "ref").
- Settings > About (plus_datapack.py:71-75) labels every non-RadioID type "Air band pack: "; it needs a label per type
  ("Map pack: " / "Harita paketi: ") and the `DPK_BIG` and "for another version" texts. `ABOUT_BYTES` reserves
  `112u * CONFIG_DATAPACK` (plus_datapack.py:62) for two lines; raise it to ~180 B for three lines with the longest
  Turkish state text (heap only while About draws).
- ui_dota.c:51-56: `WHATS` and `what_i` map "3 + rq.pack" for two types; the third needs its own row and label.
- `air_freq()` returns a pointer to one static copy once the table is gone (C.5): callers must not hold two results.
- The Rust and Python `inactive_slot` / `best` (datapack.rs:159-174) do not check the structure; for type 3 they must
  treat a structurally bad slot as invalid, like the firmware, or they could pick the good slot as "inactive" and
  overwrite it.

### F.2 Rust programmer (`firmware/rust`)

`ddm32-core/src/datapacks/datapack.rs` (slots, REF), `datapacks/refpack.rs` (new: directory check for `info` and before
writing), `datapacks/mod.rs`, `radio/cps.rs` (three types, guards of E.3), `releases/signed_manifest.rs`,
`releases/client.rs`, `releases/local.rs`, `releases/mod.rs` (optional `ref_pack` asset; zip unchanged), the update
flow (`radio/plan.rs` and the GUI's update page), `ddm32-gui/src/packs.rs` + its Slint page and texts, `ddm32-cli`
(`pack read` lists three types, `pack write` takes the `.dpk`). The retired C# programmer is not changed (H.2).

### F.3 Size of the job (estimate)

Firmware ~25 files touched (~1,200 lines changed), tools ~12 files (~500 lines, `refpack.py` ~250), Rust ~14 files
(~800 lines), tests ~22 files (~1,500 lines), docs 5 (plus.md section, dota.md 20, plus-radio-test.md rows,
THIRD_PARTY.md, CHANGELOG).

### F.4 Program flash and RAM

| | Bytes | Note |
|---|---:|---|
| Leaves flash: map + districts | 32,299 | sums of the 1.0.4 map: 17,430 + 7,896 + 3,892 + 1,458 + 680 + 617 + 326 |
| Leaves flash: repeaters | 11,120 | rpt_table 11,088 + rpt_info 32 |
| Leaves flash: airfields | 3,744 | air_fields 3,256 + air_freqs 488 |
| **Gross** | **47,163** | marine 590 stays |
| Added: datapack.c type 3, window, states | ~500 | estimate |
| Added: ref_pack.c (validation, readers) | ~1,000-1,400 | estimate |
| Added: call sites (map, rpt, air, mapnav, xband, dota) pointer -> copy | ~800-1,400 | estimate |
| Added: "no data" states, About line, strings EN + TR | ~600-900 | estimate |
| Added: total of the reader side | ~3,000-4,500 | (validation grew with S1 / S3; still inside this range by estimate) |
| Added: the repeater frequency set + lookup (C.7, B1) | ~900-1,300 | data ~0.7-0.9 KB, code ~0.15-0.25 KB |
| **Net freed** | **~42 KB (41.4-43.3 KB)** | 47,163 - (3,900-5,800); 1.0.6 has 5,052 B free (CHANGELOG v1.0.6), so ~47 KB free after |

Static RAM: window 128 + tag 8, UI directory copy ~100, `cur[]` grows by one type 12, name buffers 2 x 24, static
record scratch rpt 44 + air 44 + air_freq 4: **~390 B**, of the ~5.2 KB free. No heap beyond About's ~70 B more while it
draws. Stack: no new deep locals (static scratch; validation uses a 64-byte buffer like `payload_crc`, on the task
that scans: start-up task, CPS, boot); the stack check must show every task's margin unchanged.

---

## G. Test plan

### G.1 Host tests (simulated 16 MiB data chip, as `tests/plus/test_datapack.c` and `test_dota_pack.c` do)

| # | Test | File |
|---|---|---|
| T1 | layout: every slot inside the region, aligned, no overlap; `dpk_erase` / `dpk_program` refuse 0xE6F000 and 0xF00000 | test_datapack.c (line 101's addresses updated) |
| T2 | migration fixtures built with the **1.0.6 layout**: (a) 98 KB RadioID pack in old A -> in force, unchanged; (b) in old B with higher seq -> in force; (c) both; (d) 200 KB pack in old B -> `DPK_BIG`, A used if valid; (e) old A > 192 KB: map A tail holds its bytes -> map A NONE (not BAD); the image's bytes outside the region compared before / after: unchanged; no erase / program call happens during the scan | test_datapack.c |
| T3 | `refpack.py` output == C serialisation of the compiled tables (gcc), section by section; deterministic (two runs identical) | tools/test_refpack.py (new) |
| T4 | map pack: written through `dpk_begin/put/commit`, in force, then a second write into the other slot; power cut at 4 points -> old pack in force | test_refpack.c (new) |
| T5 | validation: each structural rule of C.2 broken once with valid CRCs (section past the payload, `off + rec * count` overflowing u32, overlapping sections, ring `first + count` past steps and wrapping 16 bits, province ring past rings, province name offset past its blob, DIST_FIRST decreasing, `first[81] != DIST_POS` count, a district name offset past the blob, a province with fewer names than districts, a blob without its final NUL, RPT / RPT_KEY count mismatch, a key differing from its record, air `first + count` past freqs, air count > 64, unknown section id ignored, known id with wrong rec, wrong coordinate CRC, fmt 2) -> **the whole slot BAD**, no out-of-slot read (read hook asserts). Records with no NUL in name / city / code / date / source come out NUL-terminated (S3) | test_refpack.c |
| T5b | slot choice (S1): newer slot structurally bad + older good -> older in force; newer bad in the map part only -> older in force (all or nothing); a slot without RPT sections -> valid, MAP and AIR on, RPT off; DOTA receive of a CRC-valid but structurally bad pack -> `dpk_commit` refuses before the header write, the header sector stays erased, old pack in force | test_refpack.c, test_dota_pack.c |
| T5c | race (S2): the UI's directory copy reloads when a commit from another "task" (simulated between two reads of one draw) changes the generation; a draw in progress stays bounded by its cached counts | test_refpack.c |
| T6 | map functions on the pack equal the flash tables: `map_draw` line list, `map_province_at` / `_near` on a grid of 5,000 points, district near / names, labels | test_map.c (both builds) |
| T7 | repeaters: `rpt_next_in` / `rank` / `count` / `nearest_if` order identical to the flash table for 200 random positions and every province filter; `arpt_check` sequence on a recorded drive | test_rpt.c, test_mapnav.c |
| T8 | airfields: order, provinces, air band pack match by code | test_airfield.c, test_airprov.c |
| T9 | no pack / damaged pack / pack without RPT sections: every UI place of section D (map, here line, repeater overlay and pick, Repeaters, auto repeater toggle, Airfields, air watch, DOTA What row, About with three lines and the `DPK_BIG` text), EN + TR, layout-checked; **cross-band and DOTA still refuse a listed repeater's input and output without any pack** (B1) | test_ui_map.c (new), test_ui_rpt*.c, test_ui_airprov.c, test_ui_xband.c, test_ui_dota.c, test_ui_datapack.c |
| T10 | read counting: a whole-country `map_draw` makes <= 300 flash reads; a repeater list redraw reads <= 40 KB (the window and key sections are used); **on the map screen, 20 radio view-serial changes with the map key unchanged read the pack at most once per `REFRESH_MS`** (S4) | test_refpack.c, test_ui_map.c |
| T15 | the flash frequency set (C.7) equals the old `known_repeater` loop over `rpt_table` for every frequency 136-480 MHz in 625 Hz steps, for the sample list and a generated 400-repeater list | test_rpt.c, tools/test_rptprov.py |
| T16 | release gate (S6): `make_release.py` refuses a public pack or build manifest whose repeater source is not in the allowed list (default: "own" refused), a sample pack, a pack that fails structure; accepts a RadioID.net-only pack | pc/release tests |
| T11 | DOTA: a map pack over FM with 10 % loss; a 1.0.6-style receiver (DPK_TYPES 2) refuses type 3 without writing; a 1.1 receiver refuses a 200 KB RadioID pack | test_dota_pack.c |
| T12 | marine TX refusal unchanged (no pack in the image) | test_marine.c |
| T13 | link audit refuses an ELF with `rpt_table` etc. | plus_brand.py self-test |
| T14 | Rust: slot table, REF plan (header sector last, 13 sectors), structure check before writing, warning (not refusal) when a RadioID slot holds > 196,544 B, refusal of such a RadioID pack, three-type read JSON with "ref", `inactive_slot` treating a structurally bad slot as invalid, manifest with / without `ref_pack` in the firmware entry, **3.1.0's parser accepting the new manifest**, zip still three files; Python `datapack.py` the same | ddm32-core/tests/datapacks.rs, manifest.rs, releases.rs, cli tests; tools/test_datapack.py |

### G.2 Owner radio rows (each serial / flash / RF step needs the owner's approval in chat, AGENTS.md rule 1)

| Row | Step | Expect | Radio-only |
|---|---|---|---|
| MP1 | Update 1.0.6 -> 1.1 with the existing RadioID pack, no map pack yet | boots normally; About: RadioID pack unchanged; Map pack: none; Map / Repeaters / Airfields show the "load the map pack" texts; Home, scan, calls normal | yes |
| MP2 | Programmer writes the map pack | 13 sectors, PASS; About: "Map pack: DDM32 TR 1.1.0, ..."; time noted (estimate 20-30 s) | yes |
| MP3 | Tools > Map whole country, zoom, districts; **quiet, then during a DMR call with names (RX traffic)** | draws like 1.0.6; keys stay responsive during the call; draw time by stopwatch / live panel (estimate ~40-55 ms of reads per map-layer redraw, not per radio update) | yes (SPI speed, redraw rate) |
| MP4 | Tools > Repeaters scroll through a province and All | no visible lag (estimate ~30 ms a key) | yes |
| MP5 | Auto repeater on while moving | offers as in 1.0.6 | yes |
| MP6 | Airfields, air watch, with the user's air band pack | its frequencies still win | yes |
| MP7 | Cross-band: a listed repeater's output as A; DOTA with a listed repeater's frequency; **both repeated before the map pack is loaded (MP1 state)** | refused "Repeater frequency" each time | yes |
| MP8 | VFO on 156.800 (marine 16), PTT | refused as before | yes |
| MP9 | Write the map pack again | the other slot, seq + 1, in force | yes |
| MP10 | Boot time with both map slots written (after MP9) | at most ~0.15-0.2 s longer than 1.0.6 (estimate ~150 ms for two map packs) | yes |
| MP11 | DOTA: send the map pack to a second radio (if available) | ~7-9 min, accepted, in force | yes |
| MP12 (optional) | Downgrade to 1.0.6 and back | RadioID kept both ways; map pack in force again on 1.1 | yes; owner decides if worth it |

What stays host-only until these rows run: the SPI timing estimates, the CPS write time, the boot-time cost, the DOTA
time, and the `git` field check of E.3.

---

## H. Risks and open questions for the owner

| # | Question / risk | Proposal |
|---|---|---|
| H.1 | Marine table: decision 2 says move it; it feeds the radio core's TX refusal (C.6) and saves ~300 B net | keep it in program flash |
| H.2 | **Update with an older programmer (3.1.0, or the retired C# 2.0)**: it installs 1.1 firmware but not the pack (it ignores `ref_pack`); map, repeaters, airfields and auto repeater say "load the map pack" until programmer 3.2 writes it (the cross-band / DOTA refusals keep working, C.7). An old programmer writing a > 192 KB RadioID pack can erase a map slot and still report PASS (B.4 N2) | coordinator's recommendation: release 1.1.0 with programmer 3.2.0 and a programmer self-update prompt; release notes "update the programmer first". Old programmers writing RadioID <= 192 KB keep working (same slot starts) |
| H.3 | **Install order** | coordinator's recommendation, matching this design: update from 1.0.x with programmer 3.2: **pack first, then firmware** (1.0.6 ignores the map slot, so a failed firmware install leaves a working 1.0.6; `min_firmware` means "needed to use it", E.2); first install from stock: firmware first (no CPS before), then the pack |
| H.4 | (revision 1: cross-band without a map pack) | **closed by B1**: the TX refusals use the flash frequency set (C.7); cross-band and DOTA behave the same with or without a pack. No owner question left here |
| H.5 | **Repeater data in a public release (pack sections and the flash frequency set)**: TRAC states no licence; docs contradict (docs/plus.md:3381 vs :2201, THIRD_PARTY.md:64; E.5). The release gate (E.2) enforces the answer; until then it refuses "own" (TRAC) data in a public release, which differs from what 1.0.x releases shipped | (a) public release with RadioID.net repeaters only, the TRAC part built on the user's PC from their own copy (coordinator's recommendation); (b) keep what 1.0.x ships (both); (c) no repeaters in the release at all, all user-built (then the public frequency set is empty and only the band-plan segments refuse). The format supports all three |
| H.6 | RadioID reload | not needed for the owner (98 KB). Only a pack > 196,544 B (multi-country) must be rebuilt; About says so (`DPK_BIG`) |
| H.7 | 64 KB map slot is fixed by the layout; headroom ~14.8 KB | enough for the planned scans; if repeaters grow past ~+250, split them into a pack type of their own in the free 32 KB later |
| H.8 | SPI read speed is an estimate (byte-polled SSI, no measured figure) | if MP3 / MP4 show lag, enlarge the window or add a key for districts; worst case still a draw well under 0.2 s |
| H.9 | Data-chip mutex stalls a map draw during a settings save (up to 0.4 s); the radio task's RadioID lookups wait at most one 128-B read for a map reader | accepted (UI only); the radio task never reads the map pack |
| H.9b | Whether the UI core keeps the frame buffer between draws of one screen decides how the map avoids re-reading on every radio update (C.4, S4) | implementation check first; fallback: map layer at most once per second while its key is unchanged |
| H.9c | Boot time grows ~150 ms with two map packs (S5) | accepted, or later check the losing slot only when it could win |
| H.10 | Firmware-version check in the programmer uses the CPS info `git` text (15 characters) | confirm its content on the radio (read-only info command) before relying on it; it guards UX only |
| H.11 | Version name: the map pack belongs to firmware 1.1.0 (a new minor version, since the release now has two parts) | owner picks the version |
