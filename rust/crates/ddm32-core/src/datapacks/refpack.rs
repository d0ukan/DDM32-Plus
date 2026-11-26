// SPDX-License-Identifier: GPL-3.0-or-later
//! The map / reference pack (type 3, `DPK_REF`; docs/mappack-design.md C.2, E.3): the payload's directory, the
//! firmware's structural validation (the same rules as `ref_validate`, so the programmer never writes a pack the
//! firmware would reject and judges the radio's slots exactly as the firmware does), the slot choice for a write
//! (R3), the release / user-built distinction (S-B) and the rebuild of a user pack for a new release.
//!
//! All arithmetic is in u64 or checked u32, so a crafted pack can never wrap a sum (C.2 S3).

use super::datapack::{self as dp, PackHeader, HEADER_SIZE, REF};
use crate::radio::monitor::crc32;

pub const MAGIC: &[u8; 4] = b"REF1";
/// The directory version firmware 1.1 reads.
pub const DIR_VERSION: u8 = 1;
pub const DIR_HEAD: usize = 16;
pub const DIR_ENTRY: usize = 12;
pub const MAX_SECTIONS: usize = 16;

pub const MAP_META: u16 = 1;
pub const PROVINCES: u16 = 2;
pub const RINGS: u16 = 3;
pub const STEPS: u16 = 4;
pub const PROV_NAMES: u16 = 5;
pub const DIST_POS: u16 = 6;
pub const DIST_FIRST: u16 = 7;
pub const DIST_NAME_FIRST: u16 = 8;
pub const DIST_NAMES: u16 = 9;
pub const RPT_INFO: u16 = 10;
pub const RPT: u16 = 11;
pub const RPT_KEY: u16 = 12;
pub const AIR: u16 = 13;
pub const AIR_FREQ: u16 = 14;
pub const AIR_KEY: u16 = 15;

/// The record size of every known section (C.2 table); a known id with another size is "for another version".
pub const KNOWN: [(u16, u16); 15] = [
    (MAP_META, 32),
    (PROVINCES, 18),
    (RINGS, 8),
    (STEPS, 2),
    (PROV_NAMES, 1),
    (DIST_POS, 4),
    (DIST_FIRST, 2),
    (DIST_NAME_FIRST, 2),
    (DIST_NAMES, 1),
    (RPT_INFO, 32),
    (RPT, 44),
    (RPT_KEY, 10),
    (AIR, 44),
    (AIR_FREQ, 4),
    (AIR_KEY, 10),
];
const MAP_GROUP: [u16; 9] =
    [MAP_META, PROVINCES, RINGS, STEPS, PROV_NAMES, DIST_POS, DIST_FIRST, DIST_NAME_FIRST, DIST_NAMES];
const RPT_GROUP: [u16; 3] = [RPT_INFO, RPT, RPT_KEY];
const AIR_GROUP: [u16; 3] = [AIR, AIR_FREQ, AIR_KEY];
/// turkey_map_data.h TURKEY_MAP_PROVINCES
pub const PROVINCE_COUNT: u32 = 81;
/// air_pack.h AIR_PACK_FREQS
pub const AIR_FREQS_MAX: u32 = 64;
/// RPT_NONE / AIR_NONE: the record counts stay below it
pub const COUNT_NONE: u32 = 0xFFFF;
/// The largest map pack payload (64 KB slot - header).
pub const PAYLOAD_MAX: usize = 0x10000 - HEADER_SIZE;

pub fn expected_rec(id: u16) -> Option<u16> {
    KNOWN.iter().find(|k| k.0 == id).map(|k| k.1)
}

fn le16(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}
fn le32(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}
fn lei16(b: &[u8], o: usize) -> i16 {
    le16(b, o) as i16
}

/// One directory entry.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Entry {
    pub id: u16,
    pub rec: u16,
    pub off: u32,
    pub count: u32,
}

/// The payload's directory head and entries (only the bounds of the directory itself are checked here).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Directory {
    pub version: u8,
    pub coord: u32,
    pub entries: Vec<Entry>,
}

impl Directory {
    pub fn get(&self, id: u16) -> Option<&Entry> {
        self.entries.iter().find(|e| e.id == id)
    }
    pub fn count(&self, id: u16) -> u32 {
        self.get(id).map_or(0, |e| e.count)
    }
    /// The bytes of a section (bounds already checked by [`validate`]; empty when absent or out of bounds).
    pub fn bytes<'a>(&self, payload: &'a [u8], id: u16) -> &'a [u8] {
        let Some(e) = self.get(id) else { return &[] };
        let start = e.off as usize;
        let len = e.rec as usize * e.count as usize;
        payload.get(start..start.saturating_add(len)).unwrap_or(&[])
    }
}

/// The directory (magic, head, entries inside the payload), or why it is no directory.
pub fn directory(payload: &[u8]) -> Result<Directory, String> {
    if payload.len() < DIR_HEAD || &payload[..4] != MAGIC {
        return Err("no REF1 directory".into());
    }
    let n = payload[5] as usize;
    if n > MAX_SECTIONS {
        return Err(format!("{n} sections (at most {MAX_SECTIONS})"));
    }
    if DIR_HEAD + n * DIR_ENTRY > payload.len() {
        return Err("the directory is longer than the payload".into());
    }
    let entries = (0..n)
        .map(|i| {
            let o = DIR_HEAD + i * DIR_ENTRY;
            Entry {
                id: le16(payload, o),
                rec: le16(payload, o + 2),
                off: le32(payload, o + 4),
                count: le32(payload, o + 8),
            }
        })
        .collect();
    Ok(Directory { version: payload[4], coord: le32(payload, 8), entries })
}

/// What the firmware a pack is judged for expects (C.2 version items).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Rules {
    pub dir_version: u8,
    /// The firmware's map coordinate id (the release's `ref_pack.coord_crc`). None: not known; then the pack's own
    /// coordinate id stands in for it (only the hint's consistency with it is checked).
    pub coord: Option<u32>,
}

impl Rules {
    pub fn new(coord: Option<u32>) -> Rules {
        Rules { dir_version: DIR_VERSION, coord }
    }
}

/// The features a valid pack carries (a group absent or with zero records is off, C.2 "absent is not a failure").
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Features {
    pub map: bool,
    pub rpt: bool,
    pub air: bool,
}

/// The firmware's verdict on a payload (DPK_OK / DPK_BAD / DPK_OTHER).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Verdict {
    Valid(Features),
    /// Breaks a structural rule (C.2 rules 1-6 apart from the version items).
    Bad(String),
    /// Well formed, but for another firmware version (directory version, a known id's record size, the coordinate
    /// id, a non-zero version hint that disagrees).
    Other(String),
}

impl Verdict {
    pub fn is_valid(&self) -> bool {
        matches!(self, Verdict::Valid(_))
    }
}

/// The version hint the header's `flags` carries for a pack with this coordinate id (B.2).
pub fn hint(coord: u32) -> u16 {
    DIR_VERSION as u16 | ((coord & 0xFF) as u16) << 8
}

/// The firmware's structural validation of a map pack payload (`ref_pack.c ref_validate`, docs/mappack-design.md
/// section I), in the firmware's order: the head's version items (directory version, coordinate id, a non-zero hint
/// that differs) -> Other; the directory's size -> Bad; per known entry another record size -> Other (even after a
/// bad entry), a known id twice, alignment, an offset inside the directory or past the payload, a count past it ->
/// Bad; then overlaps, partial groups and the group rules -> Bad. Unknown section ids are ignored entirely. All or
/// nothing: any failing known section rejects the whole pack.
pub fn validate(payload: &[u8], flags: u16, rules: &Rules) -> Verdict {
    let bad = |s: String| Verdict::Bad(s);
    let other = |s: String| Verdict::Other(s);
    let len = payload.len() as u64;
    if payload.len() > PAYLOAD_MAX {
        return bad(format!("{} bytes: the map slot holds {PAYLOAD_MAX}", payload.len()));
    }
    if payload.len() < DIR_HEAD || &payload[..4] != MAGIC {
        return bad("no REF1 directory".into());
    }
    // the head's version items first
    let coord = le32(payload, 8);
    let fw_coord = rules.coord.unwrap_or(coord);
    let fw_hint = rules.dir_version as u16 | ((fw_coord & 0xFF) as u16) << 8;
    if payload[4] != rules.dir_version {
        return other(format!("directory version {} (this firmware reads {})", payload[4], rules.dir_version));
    }
    if coord != fw_coord {
        return other(format!("map coordinate id 0x{coord:08X} (this firmware: 0x{fw_coord:08X})"));
    }
    if flags != 0 && flags != fw_hint {
        return other(format!("version hint 0x{flags:04X} (this firmware: 0x{fw_hint:04X})"));
    }
    let d = match directory(payload) {
        Ok(d) => d,
        Err(e) => return bad(e),
    };
    let dir_end = (DIR_HEAD + d.entries.len() * DIR_ENTRY) as u64;
    let mut seen: Vec<u16> = Vec::new();
    let mut spans: Vec<(u64, u64)> = Vec::new();
    let mut entry_bad: Option<String> = None;
    for e in &d.entries {
        let Some(r) = expected_rec(e.id) else { continue }; // unknown sections are ignored
        if e.rec != r {
            return other(format!("section {} has {}-byte records (this firmware: {r})", e.id, e.rec));
        }
        let (off, count) = (e.off as u64, e.count as u64);
        if seen.contains(&e.id)
            || !off.is_multiple_of(4)
            || off < dir_end
            || off > len
            || count > (len - off) / r as u64
        {
            entry_bad.get_or_insert_with(|| format!("section {} at {}: misplaced", e.id, e.off));
            continue;
        }
        seen.push(e.id);
        spans.push((off, off + r as u64 * count));
    }
    if let Some(e) = entry_bad {
        return bad(e);
    }
    // no two known sections overlap (empty ones never do)
    for (i, a) in spans.iter().enumerate() {
        for b in &spans[i + 1..] {
            if a.1 > a.0 && b.1 > b.0 && a.0 < b.1 && b.0 < a.1 {
                return bad("two sections overlap".into());
            }
        }
    }
    let present = |g: &[u16]| g.iter().filter(|id| seen.contains(id)).count();
    for (g, what) in [(&MAP_GROUP[..], "map"), (&RPT_GROUP[..], "repeater"), (&AIR_GROUP[..], "airfield")] {
        let n = present(g);
        if n != 0 && n != g.len() {
            return bad(format!("the {what} sections are incomplete"));
        }
    }
    let mut f = Features::default();
    if present(&MAP_GROUP) != 0 {
        if let Err(e) = map_rules(payload, &d, fw_coord) {
            return bad(e);
        }
        f.map = true;
    }
    if present(&RPT_GROUP) != 0 {
        if let Err(e) = rpt_rules(payload, &d) {
            return bad(e);
        }
        f.rpt = d.count(RPT) > 0;
    }
    if present(&AIR_GROUP) != 0 {
        if let Err(e) = air_rules(payload, &d) {
            return bad(e);
        }
        f.air = d.count(AIR) > 0;
    }
    Verdict::Valid(f)
}

/// Firmware 1.1's map coordinate id (docs/mappack-design.md section I): CRC-32 of its origin, unit and bounds and the
/// charset name. Only for this id does the programmer know the compiled origin / unit / bounds MAP_META must equal.
pub const COORD_1_1: u32 = 0x056B_6A10;
/// turkey_map_data.h of firmware 1.1: LON0_E7, LAT0_E7, UNIT_E7 (i32), XMIN, YMIN, XMAX, YMAX (i16).
pub const META_1_1: ([i32; 3], [i16; 4]) = ([250_000_000, 355_000_000, 10_000], [663, 320, 19_807, 6_599]);

/// MAP_META (docs/mappack-design.md section I): lon0, lat0, unit (i32 at 0, 4, 8), XMIN, YMIN, XMAX, YMAX (i16 at
/// 12..20), then the counts PROVINCES, RINGS, STEPS, DISTRICTS (u16 at 20, 22, 24, 26), 4 zero bytes.
pub const META_PROVINCES: usize = 20;
pub const META_RINGS: usize = 22;
pub const META_STEPS: usize = 24;
pub const META_DISTRICTS: usize = 26;

fn map_rules(p: &[u8], d: &Directory, fw_coord: u32) -> Result<(), String> {
    let meta = d.bytes(p, MAP_META);
    if d.count(MAP_META) != 1 || meta.len() != 32 {
        return Err("MAP_META: one record".into());
    }
    // the origin, unit and bounds the firmware was compiled with (known here for firmware 1.1's coordinate id)
    if fw_coord == COORD_1_1 {
        let (ints, shorts) = META_1_1;
        if (0..3).any(|i| le32(meta, i * 4) as i32 != ints[i]) || (0..4).any(|i| lei16(meta, 12 + i * 2) != shorts[i]) {
            return Err("MAP_META origin, unit or bounds differ from the firmware's".into());
        }
    }
    let (provinces, rings, steps, dists) = (d.count(PROVINCES), d.count(RINGS), d.count(STEPS), d.count(DIST_POS));
    if le16(meta, META_PROVINCES) as u32 != provinces
        || le16(meta, META_RINGS) as u32 != rings
        || le16(meta, META_STEPS) as u32 != steps
        || le16(meta, META_DISTRICTS) as u32 != dists
    {
        return Err("MAP_META counts differ from the sections".into());
    }
    if provinces != PROVINCE_COUNT {
        return Err(format!("{provinces} provinces (81)"));
    }
    if rings >= 65536 || steps >= 65536 || dists >= 65536 {
        return Err("a map count is 65,536 or more".into());
    }
    // rule 5 (checked before the offsets into the blobs are trusted)
    let pnames = d.bytes(p, PROV_NAMES);
    let dnames = d.bytes(p, DIST_NAMES);
    if pnames.last() != Some(&0) || dnames.last() != Some(&0) {
        return Err("a name blob does not end with NUL".into());
    }
    // rule 3
    // rings: their step ranges consecutive and disjoint (ring r starts where ring r-1 ended), so a crafted pack can
    // never make one frame walk shared or overlapping ranges again and again
    let rb = d.bytes(p, RINGS);
    let mut next_step = 0u32;
    for i in 0..rings as usize {
        let r = &rb[i * 8..i * 8 + 8];
        let (first, count) = (le16(r, 4) as u32, le16(r, 6) as u32);
        if count < 1 || first + count > steps {
            return Err(format!("ring {i}: steps {first}+{count} of {steps}"));
        }
        if first != next_step {
            return Err(format!("ring {i}: steps from {first}, not {next_step} (ranges must follow each other)"));
        }
        next_step += count;
    }
    if next_step != steps {
        return Err(format!("the rings cover {next_step} of {steps} steps"));
    }
    let pb = d.bytes(p, PROVINCES);
    let mut next_ring = 0u32;
    for i in 0..provinces as usize {
        let r = &pb[i * 18..i * 18 + 18];
        let (first_ring, ring_count, name) = (le16(r, 0) as u32, r[2] as u32, le16(r, 4) as usize);
        if first_ring + ring_count > rings {
            return Err(format!("province {i}: rings {first_ring}+{ring_count} of {rings}"));
        }
        if first_ring != next_ring {
            return Err(format!(
                "province {i}: rings from {first_ring}, not {next_ring} (ranges must follow each other)"
            ));
        }
        next_ring += ring_count;
        if name >= pnames.len() {
            return Err(format!("province {i}: name offset {name}"));
        }
        if lei16(r, 10) > lei16(r, 14) || lei16(r, 12) > lei16(r, 16) {
            return Err(format!("province {i}: min > max"));
        }
    }
    if next_ring != rings {
        return Err(format!("the provinces cover {next_ring} of {rings} rings"));
    }
    // rule 4
    if d.count(DIST_FIRST) != provinces + 1 || d.count(DIST_NAME_FIRST) != provinces {
        return Err("district index counts".into());
    }
    let first = d.bytes(p, DIST_FIRST);
    let df = |k: usize| le16(first, k * 2) as u32;
    if df(0) != 0 || df(provinces as usize) != dists {
        return Err("DIST_FIRST ends".into());
    }
    let nf = d.bytes(p, DIST_NAME_FIRST);
    let mut last_name = 0usize;
    for k in 0..provinces as usize {
        if df(k + 1) < df(k) {
            return Err(format!("DIST_FIRST decreases at {k}"));
        }
        let off = le16(nf, k * 2) as usize;
        if off < last_name || off >= dnames.len() {
            return Err(format!("DIST_NAME_FIRST {k}: offset {off}"));
        }
        last_name = off;
        let mut pos = off;
        for _ in 0..(df(k + 1) - df(k)) {
            match dnames[pos.min(dnames.len())..].iter().position(|&b| b == 0) {
                Some(z) => pos = pos + z + 1,
                None => return Err(format!("province {k}: fewer district names than districts")),
            }
        }
    }
    Ok(())
}

/// rpt_entry_t (rpt.h, 44 bytes): lat_e5 i32 @0, lon_e5 i32 @4, rx_hz u32 @8, off i16 @12, ctcss10 u16 @14,
/// mode u8 @16, dmr u8 @17, name[11] @18, city[13] @29, prov u8 @42, padding @43.
pub const RPT_MODE: usize = 16;
pub const RPT_PROV: usize = 42;
pub const RPT_NAME: usize = 18;

fn rpt_rules(p: &[u8], d: &Directory) -> Result<(), String> {
    let n = d.count(RPT);
    if d.count(RPT_KEY) != n || n >= COUNT_NONE {
        return Err("RPT / RPT_KEY counts".into());
    }
    if d.count(RPT_INFO) != 1 {
        return Err("RPT_INFO: one record".into());
    }
    let (recs, keys) = (d.bytes(p, RPT), d.bytes(p, RPT_KEY));
    for i in 0..n as usize {
        if keys[i * 10..i * 10 + 10] != rpt_key(&recs[i * 44..i * 44 + 44]) {
            return Err(format!("repeater {i}: its key differs from its record"));
        }
    }
    Ok(())
}

/// A repeater record's key: lat_e5, lon_e5, prov, mode (C.2 section 12).
pub fn rpt_key(rec: &[u8]) -> [u8; 10] {
    let mut k = [0u8; 10];
    k[..8].copy_from_slice(&rec[..8]);
    k[8] = rec[RPT_PROV];
    k[9] = rec[RPT_MODE];
    k
}

/// air_field_t (airfield.h, 44 bytes): lat_e5 @0, lon_e5 @4, first u16 @8, count u8 @10, kind u8 @11, code[8] @12,
/// name[23] @20, prov u8 @43.
fn air_rules(p: &[u8], d: &Directory) -> Result<(), String> {
    let n = d.count(AIR);
    if d.count(AIR_KEY) != n || n >= COUNT_NONE {
        return Err("AIR / AIR_KEY counts".into());
    }
    let freqs = d.count(AIR_FREQ);
    let (recs, keys) = (d.bytes(p, AIR), d.bytes(p, AIR_KEY));
    for i in 0..n as usize {
        let r = &recs[i * 44..i * 44 + 44];
        if keys[i * 10..i * 10 + 10] != air_key(r) {
            return Err(format!("airfield {i}: its key differs from its record"));
        }
        let (first, count) = (le16(r, 8) as u32, r[10] as u32);
        if count > AIR_FREQS_MAX || first + count > freqs {
            return Err(format!("airfield {i}: frequencies {first}+{count} of {freqs}"));
        }
    }
    Ok(())
}

/// An airfield record's key: lat_e5, lon_e5, prov (record offset 43), kind (offset 11) (section 15).
pub fn air_key(rec: &[u8]) -> [u8; 10] {
    let mut k = [0u8; 10];
    k[..8].copy_from_slice(&rec[..8]);
    k[8] = rec[43];
    k[9] = rec[11];
    k
}

/// A directory with its sections, 4-byte aligned in the given order (the builder of tools/refpack.py's layout; used
/// for the rebuild and the tests). Sections: (id, record size, bytes).
pub fn assemble(coord: u32, sections: &[(u16, u16, Vec<u8>)]) -> Result<Vec<u8>, String> {
    if sections.len() > MAX_SECTIONS {
        return Err(format!("{} sections", sections.len()));
    }
    let mut out = vec![0u8; DIR_HEAD + sections.len() * DIR_ENTRY];
    out[..4].copy_from_slice(MAGIC);
    out[4] = DIR_VERSION;
    out[5] = sections.len() as u8;
    out[8..12].copy_from_slice(&coord.to_le_bytes());
    for (i, (id, rec, data)) in sections.iter().enumerate() {
        while !out.len().is_multiple_of(4) {
            out.push(0);
        }
        if *rec == 0 || !data.len().is_multiple_of(*rec as usize) {
            return Err(format!("section {id}: {} bytes, records of {rec}", data.len()));
        }
        let o = DIR_HEAD + i * DIR_ENTRY;
        let off = out.len() as u32;
        out[o..o + 2].copy_from_slice(&id.to_le_bytes());
        out[o + 2..o + 4].copy_from_slice(&rec.to_le_bytes());
        out[o + 4..o + 8].copy_from_slice(&off.to_le_bytes());
        out[o + 8..o + 12].copy_from_slice(&((data.len() / *rec as usize) as u32).to_le_bytes());
        out.extend_from_slice(data);
    }
    Ok(out)
}

/// RPT_INFO (rpt_info_t, 32 bytes): count u32, sample u8, date[11], source[16].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RptInfo {
    pub count: u32,
    pub sample: bool,
    pub date: String,
    pub source: String,
}

fn cstr(b: &[u8]) -> String {
    let z = b.iter().position(|&x| x == 0).unwrap_or(b.len());
    b[..z].iter().map(|&c| if c < 0x80 { c as char } else { '?' }).collect()
}

pub fn rpt_info(payload: &[u8]) -> Option<RptInfo> {
    let d = directory(payload).ok()?;
    let b = d.bytes(payload, RPT_INFO);
    (b.len() == 32).then(|| RptInfo {
        count: le32(b, 0),
        sample: b[4] != 0,
        date: cstr(&b[5..16]),
        source: cstr(&b[16..32]),
    })
}

/// A pack built by the release (tools/refpack.py writes the tag "build <version>"); every other tag is a pack a user
/// built (S-B, owner decision 2026-11-26: the repeater source no longer says anything about who built it).
pub fn is_release_built(tag: &str) -> bool {
    tag.strip_prefix("build ").is_some_and(|v| !v.is_empty() && !v.contains(' '))
}

/// One line about a map pack payload (the tool's English text).
pub fn info(payload: &[u8], flags: u16) -> String {
    match directory(payload) {
        Err(e) => format!("map pack: {e}"),
        Ok(d) => {
            let mut s = format!(
                "map pack: directory {}, coordinate id 0x{:08X}, hint 0x{flags:04X}, {} repeaters, {} airfields, {} districts",
                d.version,
                d.coord,
                d.count(RPT),
                d.count(AIR),
                d.count(DIST_POS)
            );
            if let Some(i) = rpt_info(payload) {
                s.push_str(&format!(", repeater data {} {}", i.source, i.date));
            }
            s
        }
    }
}

// --- the radio's slots ------------------------------------------------------------------------------------------

/// One map slot as a firmware judges it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SlotVerdict {
    /// No `DPK1` at the slot start: nothing committed there (B.2: NONE, not BAD).
    Empty,
    /// `DPK1` but a wrong header CRC, type, format, length or payload CRC, or a structural rule broken.
    Bad(String),
    /// Well formed, for another firmware version.
    Other(String),
    Valid,
}

/// The header fields when the slot starts with `DPK1` and the header CRC checks (the "header that checks" of R3).
pub fn checked_header(image: &[u8]) -> Option<PackHeader> {
    dp::parse(image).0
}

pub fn judge(image: &[u8], rules: &Rules) -> SlotVerdict {
    if image.len() < 4 || &image[..4] != dp::MAGIC {
        return SlotVerdict::Empty;
    }
    let (h, payload) = dp::parse(image);
    let Some(h) = h else { return SlotVerdict::Bad("header CRC".into()) };
    if h.ptype != REF || h.format != 1 || h.length as usize > PAYLOAD_MAX {
        return SlotVerdict::Bad(format!("type {} format {} length {}", h.ptype, h.format, h.length));
    }
    if !h.crc_ok {
        return SlotVerdict::Bad("payload CRC".into());
    }
    match validate(&payload, h.flags, rules) {
        Verdict::Valid(_) => SlotVerdict::Valid,
        Verdict::Bad(e) => SlotVerdict::Bad(e),
        Verdict::Other(e) => SlotVerdict::Other(e),
    }
}

/// The slot in force for a firmware with these rules: the valid one with the higher sequence (scan_type).
pub fn in_force(images: &[Vec<u8>], rules: &Rules) -> Option<usize> {
    let mut best: Option<(usize, u32)> = None;
    for (i, img) in images.iter().enumerate() {
        if judge(img, rules) == SlotVerdict::Valid {
            let seq = checked_header(img).map_or(0, |h| h.seq);
            if best.is_none_or(|b| seq > b.1) {
                best = Some((i, seq));
            }
        }
    }
    best.map(|b| b.0)
}

/// The type's state for About: the slot in force, else OTHER when either slot is OTHER, else BAD / NONE (C.2).
pub fn type_state(images: &[Vec<u8>], rules: &Rules) -> &'static str {
    if in_force(images, rules).is_some() {
        return "valid";
    }
    let v: Vec<SlotVerdict> = images.iter().map(|i| judge(i, rules)).collect();
    if v.iter().any(|x| matches!(x, SlotVerdict::Other(_))) {
        "other"
    } else if v.iter().any(|x| matches!(x, SlotVerdict::Bad(_))) {
        "bad"
    } else {
        "none"
    }
}

/// One more than the highest sequence of any slot whose header checks (valid or rejected), so the new pack wins
/// over both (R3).
pub fn next_seq(images: &[Vec<u8>]) -> Result<u32, String> {
    let max = images.iter().filter_map(|i| checked_header(i)).map(|h| h.seq).max().unwrap_or(0);
    max.checked_add(1).ok_or_else(|| "the sequence number is used up".to_string())
}

/// What the running firmware makes of the map slots (R3 "across firmware versions").
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Running {
    /// A firmware before 1.1: it never reads the map slots.
    NoMapPack,
    /// A map pack firmware (1.1 or later). The first list: the coordinate ids of the releases of its version the
    /// program knows; the second: the ids known to belong to other versions. The CPS text names major.minor only, so
    /// a patch or developer build may have an id the program does not know: every id a slot holds that is not known
    /// to be another version's is taken as possible too (it is never overwritten if it could be the one in force).
    MapPack(Vec<u32>, Vec<u32>),
    /// The running version cannot be told: the slot with the higher sequence among the slots whose header and
    /// payload CRC check is kept (E.3).
    Unknown,
}

/// The slots the running firmware may have in force: never overwritten.
pub fn protected(images: &[Vec<u8>], running: &Running) -> Vec<usize> {
    let mut out: Vec<usize> = Vec::new();
    match running {
        Running::NoMapPack => {}
        Running::Unknown => {
            let mut best: Option<(usize, u32)> = None;
            for (i, img) in images.iter().enumerate() {
                if let Some(h) = checked_header(img).filter(|h| h.crc_ok) {
                    if best.is_none_or(|b| h.seq > b.1) {
                        best = Some((i, h.seq));
                    }
                }
            }
            out.extend(best.map(|b| b.0));
        }
        Running::MapPack(coords, foreign) => {
            let mut candidates: Vec<u32> = coords.clone();
            candidates.extend(
                images
                    .iter()
                    .filter_map(|img| checked_header(img).filter(|h| h.crc_ok).map(|_| dp::parse(img).1))
                    .filter_map(|p| directory(&p).ok().map(|d| d.coord))
                    // ids known as other versions' are left out only when the program knows this version's ids
                    .filter(|c| coords.is_empty() || !foreign.contains(c)),
            );
            for c in candidates {
                if let Some(s) = in_force(images, &Rules::new(Some(c))) {
                    if !out.contains(&s) {
                        out.push(s);
                    }
                }
            }
        }
    }
    out.sort();
    out
}

/// Where a new map pack goes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SlotChoice {
    Write {
        slot: usize,
        seq: u32,
    },
    /// The only slot the rules leave already holds exactly this payload: nothing to write.
    AlreadyThere {
        slot: usize,
    },
}

/// The refusal when both slots may be the running firmware's: why, and what the user can do.
pub const BOTH_IN_USE: &str = "both map slots may be in use by the running firmware; nothing is written. The program      does not know the map pack id of the radio's firmware version: check for updates online (the release list names      the ids) and try again, or install the firmware again first";

/// The code of the refusal when the only slot left would destroy a RadioID pack over 192 KB (its tail lies in that
/// map slot); the tool writes it only with --allow-radioid-loss.
pub const RADIOID_LOSS: &str = "radioid_loss";

/// R3: never the slot in force for the running firmware; of the rest, the slot the target firmware does not use
/// (rejected or empty, or the older valid one), never its only valid pack; sequence above every checked header.
/// Nothing to write only when the identical pack is the one the target firmware has in force. `avoid`: map slots
/// whose RadioID partner holds a pack over 192 KB (B.4: the map slot is that pack's tail); another slot is taken when
/// the rules allow, else the write is refused with [`RADIOID_LOSS`] unless `allow_loss`.
/// `images` must be both slots read in full (the caller stops before this when a read fails).
pub fn choose_slot(
    images: &[Vec<u8>],
    running: &Running,
    target: &Rules,
    payload: &[u8],
    avoid: &[usize],
    allow_loss: bool,
) -> Result<SlotChoice, String> {
    if images.len() != 2 {
        return Err("both map slots must be read".into());
    }
    let same = |s: usize| {
        let (h, p) = dp::parse(&images[s]);
        h.is_some_and(|h| h.crc_ok) && p == payload
    };
    let target_slot = in_force(images, target);
    if let Some(t) = target_slot.filter(|t| same(*t)) {
        return Ok(SlotChoice::AlreadyThere { slot: t });
    }
    let keep = protected(images, running);
    // the slots that may be written: never the running firmware's; of two, not the one the target has in force (its
    // only valid pack). When the running firmware holds the other slot, the target's pack there is replaced: the
    // running firmware does not use it (the pack-first case).
    let mut free: Vec<usize> = (0..2).filter(|s| !keep.contains(s)).collect();
    if free.len() == 2 {
        free.retain(|s| Some(*s) != target_slot);
    }
    let preferred = |cands: &[usize]| -> usize {
        // an empty slot first, else the lower header sequence
        if let Some(s) = cands.iter().find(|s| judge(&images[**s], target) == SlotVerdict::Empty) {
            return *s;
        }
        *cands.iter().min_by_key(|s| (checked_header(&images[**s]).map_or(0, |h| h.seq), **s)).unwrap()
    };
    if free.is_empty() {
        return Err(BOTH_IN_USE.into());
    }
    let safe: Vec<usize> = free.iter().copied().filter(|s| !avoid.contains(s)).collect();
    let slot = if !safe.is_empty() {
        preferred(&safe)
    } else if allow_loss {
        preferred(&free)
    } else {
        return Err(format!(
            "{RADIOID_LOSS}: the map slot left is the tail of a RadioID pack over 192 KB; writing ends that pack \
             (rebuild it under 192 KB first, or allow it)"
        ));
    };
    Ok(SlotChoice::Write { slot, seq: next_seq(images)? })
}

/// The map slot (0, 1) whose bytes are the tail of a RadioID slot's old 256 KB layout: map A 0xEA0000 lies in RadioID
/// A (0xE70000), map B 0xEE0000 in RadioID B (0xEB0000).
pub fn radioid_partner(map_slot: usize) -> usize {
    map_slot
}

// --- the update step (E.3 S-B) -----------------------------------------------------------------------------------

/// What the update step does with the release's map pack.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum UpdateDecision {
    /// The pack in force already has the release pack's CRC and length.
    Same,
    /// No pack, a release-built one or a rejected one: the release pack is written.
    WriteRelease,
    /// A user-built pack is in force: never replaced without the user's choice.
    AskUserBuilt { name: String, tag: String, keep_ok: bool, rebuild_ok: bool },
}

/// The pack the radio has in force for the update step's purpose: the running firmware's slot when it reads map
/// packs, else the higher sequence among the slots whose CRCs check.
pub fn in_force_for_update(images: &[Vec<u8>], running: &Running) -> Option<usize> {
    match running {
        Running::MapPack(..) => {
            let p = protected(images, running);
            p.into_iter().max_by_key(|s| checked_header(&images[*s]).map_or(0, |h| h.seq))
        }
        _ => protected(images, &Running::Unknown).first().copied(),
    }
}

pub fn update_decision(images: &[Vec<u8>], running: &Running, release_image: &[u8], target: &Rules) -> UpdateDecision {
    let Some(s) = in_force_for_update(images, running) else { return UpdateDecision::WriteRelease };
    let (h, payload) = dp::parse(&images[s]);
    let Some(h) = h else { return UpdateDecision::WriteRelease };
    let (rh, rp) = dp::parse(release_image);
    if rh.as_ref().is_some_and(|r| r.crc == h.crc && r.length == h.length) {
        return UpdateDecision::Same;
    }
    if is_release_built(&h.tag) {
        return UpdateDecision::WriteRelease;
    }
    let keep_ok = validate(&payload, h.flags, target).is_valid();
    let rebuild_ok = rh.is_some_and(|r| rebuild(&rp, r.flags, &payload, target).is_ok());
    UpdateDecision::AskUserBuilt { name: h.name, tag: h.tag, keep_ok, rebuild_ok }
}

/// "Rebuild your pack for this version" (E.3 S-B choice 1): the release pack's sections with the repeaters of both
/// the release and the user's pack in force (the user's own rows are already in it; their CSV is not read again).
/// Records equal in frequency and callsign are taken once, the user's copy winning; sorted by RX as rpt_list.py
/// sorts (stable). The result is validated with the target rules.
pub fn rebuild(
    release_payload: &[u8],
    release_flags: u16,
    user_payload: &[u8],
    target: &Rules,
) -> Result<Vec<u8>, String> {
    if !validate(release_payload, release_flags, target).is_valid() {
        return Err("the release pack does not check".into());
    }
    let rd = directory(release_payload)?;
    let ud = directory(user_payload)?;
    for id in RPT_GROUP {
        let e = ud.get(id).ok_or_else(|| "the user's pack has no repeater sections".to_string())?;
        if Some(e.rec) != expected_rec(id) {
            return Err("the user's pack has repeater records of another version".into());
        }
    }
    let span = |d: &Directory, p: &[u8], id: u16| -> Result<Vec<u8>, String> {
        let e = d.get(id).copied().unwrap_or(Entry { id, rec: 1, off: 0, count: 0 });
        let end = e.off as u64 + e.rec as u64 * e.count as u64;
        if end > p.len() as u64 {
            return Err(format!("section {id} runs past the payload"));
        }
        Ok(p[e.off as usize..end as usize].to_vec())
    };
    let user_recs = span(&ud, user_payload, RPT)?;
    let rel_recs = span(&rd, release_payload, RPT)?;
    let key = |r: &[u8]| (le32(r, 8), cstr(&r[RPT_NAME..RPT_NAME + 11]));
    let mut recs: Vec<Vec<u8>> = user_recs.chunks(44).map(<[u8]>::to_vec).collect();
    for r in rel_recs.chunks(44) {
        if !recs.iter().any(|u| key(u) == key(r)) {
            recs.push(r.to_vec());
        }
    }
    recs.sort_by_key(|r| le32(r, 8));
    if recs.len() as u32 >= COUNT_NONE {
        return Err("too many repeaters".into());
    }
    let keys: Vec<u8> = recs.iter().flat_map(|r| rpt_key(r)).collect();
    let mut info = span(&ud, user_payload, RPT_INFO)?;
    if info.len() != 32 {
        return Err("RPT_INFO".into());
    }
    info[..4].copy_from_slice(&(recs.len() as u32).to_le_bytes());
    let mut sections: Vec<(u16, u16, Vec<u8>)> = Vec::new();
    for e in &rd.entries {
        let data = match e.id {
            RPT_INFO => info.clone(),
            RPT => recs.concat(),
            RPT_KEY => keys.clone(),
            id => span(&rd, release_payload, id)?,
        };
        sections.push((e.id, e.rec, data));
    }
    for id in RPT_GROUP {
        if rd.get(id).is_none() {
            let data = match id {
                RPT_INFO => info.clone(),
                RPT => recs.concat(),
                _ => keys.clone(),
            };
            sections.push((id, expected_rec(id).unwrap(), data));
        }
    }
    let out = assemble(rd.coord, &sections)?;
    match validate(&out, release_flags, target) {
        Verdict::Valid(_) if out.len() <= PAYLOAD_MAX => Ok(out),
        Verdict::Valid(_) => Err(format!("{} bytes: the map slot holds {PAYLOAD_MAX}", out.len())),
        Verdict::Bad(e) | Verdict::Other(e) => Err(e),
    }
}

/// A short digest of a payload (CRC-32), as the firmware compares packs.
pub fn payload_crc(payload: &[u8]) -> u32 {
    crc32(payload, 0)
}
