// SPDX-License-Identifier: GPL-3.0-or-later
//! DDM32 Plus user data packs: the port of tools/datapack.py (docs/plus.md "User data packs"; DataPacks/DataPack.cs).
//! A pack is the slot image the radio keeps on its data chip: a 64-byte header (DPK1, type, format, sequence, length,
//! payload CRC-32, date, name, builder tag, header CRC-32), then the payload. Two slots a type in 0xE70000-0xEFFFFF
//! (the one data chip region the firmware may write; AGENTS.md rule 2). The data always comes from files the user
//! downloaded; nothing derived from it is ever in the repository or a release.

use super::air::CH_BASE_KHZ;
use super::radioid::{self, RadioIdSkipped};
use super::PackError;
use crate::pytext::{ascii_decode, ascii_replace, py_strip, rstrip_nul};
use crate::radio::monitor::crc32;
use std::collections::BTreeMap;

pub const MAGIC: &[u8; 4] = b"DPK1";
pub const RADIO_ID: u8 = 1;
pub const AIR: u8 = 2;
/// The map / reference pack (docs/mappack-design.md, `DPK_REF`): map, districts, repeaters, airfields.
pub const REF: u8 = 3;
/// The largest RadioID payload firmware 1.1 takes (192 KB slot - header; docs/mappack-design.md B.1, B.4 N2).
pub const RADIO_ID_PAYLOAD_MAX: usize = 0x30000 - HEADER_SIZE;
pub const HEADER_SIZE: usize = 64;
pub const SECTOR: usize = 4096;
/// 6-bit characters a record's text, a multiple of 4.
pub const ID_CHARS: usize = 16;
/// air_pack.h AIR_PACK_FREQS
pub const AIR_FREQS: usize = 64;
/// The builder tag this program writes (datapack.py writes "datapack.py 1"; the C# program "DDM32Plus-win 1").
pub const TAG: &str = "DDM32Plus-rs 1";
pub const PYTHON_TAG: &str = "datapack.py 1";
/// The region [REGION_START, REGION_END) on the data chip: 0xE70000-0xEFFFFF.
pub const REGION_START: u64 = 0xE70000;
pub const REGION_END: u64 = 0xF00000;

fn perr(s: impl Into<String>) -> PackError {
    PackError(s.into())
}

/// A slot header's fields (datapack.parse).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PackHeader {
    pub ptype: u8,
    pub format: u8,
    pub seq: u32,
    pub length: u32,
    pub crc: u32,
    /// Header bytes 6-7: 0 for types 1 and 2; for type 3 the version hint (bits 0-7 the directory version, bits 8-15
    /// the low byte of the map coordinate CRC), 0 = no hint (docs/mappack-design.md B.2).
    pub flags: u16,
    pub date: String,
    pub name: String,
    pub tag: String,
    pub crc_ok: bool,
}

/// One CPS sector write of a slot image: its physical data chip address and its 4096 bytes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PackSector {
    pub address: u64,
    pub data: Vec<u8>,
}

/// Layout 2 (docs/mappack-design.md A.1): RadioID 192 KB (also towards a 1.0.x radio: the slot starts are the same,
/// the smaller limit is safe for both), map pack 64 KB, air band 16 KB.
pub fn slot_size(ptype: u8) -> Result<usize, PackError> {
    match ptype {
        RADIO_ID => Ok(0x30000),
        REF => Ok(0x10000),
        AIR => Ok(0x4000),
        _ => Err(perr(format!("pack type {ptype}"))),
    }
}

/// Layout 2: RadioID 0xE70000 / 0xEB0000, map pack 0xEA0000 / 0xEE0000, air band 0xEF0000 / 0xEF4000.
pub fn slot_address(ptype: u8, slot: usize) -> Result<u64, PackError> {
    if slot > 1 {
        return Err(perr(format!("slot {slot}")));
    }
    match ptype {
        RADIO_ID => Ok(if slot == 0 { 0xE70000 } else { 0xEB0000 }),
        REF => Ok(if slot == 0 { 0xEA0000 } else { 0xEE0000 }),
        AIR => Ok(if slot == 0 { 0xEF0000 } else { 0xEF4000 }),
        _ => Err(perr(format!("pack type {ptype}"))),
    }
}

/// The pack types this program knows, in the order it lists them.
pub const TYPES: [u8; 3] = [RADIO_ID, REF, AIR];

/// The three-way type name of the tool's JSON ("radioid" / "ref" / "air"; docs/mappack-design.md F.5).
pub fn type_name(ptype: u8) -> &'static str {
    match ptype {
        RADIO_ID => "radioid",
        REF => "ref",
        AIR => "air",
        _ => "unknown",
    }
}

/// True when [address, address + length) lies inside the data pack region.
pub fn in_region(address: u64, length: u64) -> bool {
    length > 0 && address >= REGION_START && length <= REGION_END && address <= REGION_END - length
}

fn le32(b: &[u8]) -> u32 {
    u32::from_le_bytes([b[0], b[1], b[2], b[3]])
}

/// The header (64 bytes): datapack.header(ptype, payload, date, name, seq, tag), with `flags` in bytes 6-7 (0 for
/// types 1 and 2; a map pack's version hint is carried from its .dpk, docs/mappack-design.md S-A).
pub fn header(
    ptype: u8,
    payload: &[u8],
    date: &str,
    name: &str,
    seq: u32,
    tag: &str,
    flags: u16,
) -> Result<Vec<u8>, PackError> {
    if date.chars().count() != 8 || !date.chars().all(|c| c.is_ascii_digit()) {
        return Err(perr(format!("date '{date}': YYYYMMDD")));
    }
    let size = slot_size(ptype)?;
    if payload.len() > size - HEADER_SIZE {
        return Err(perr(format!("{} bytes: the slot holds {}", payload.len(), size - HEADER_SIZE)));
    }
    let mut h = vec![0u8; HEADER_SIZE];
    h[..4].copy_from_slice(MAGIC);
    h[4] = ptype;
    h[5] = 1; // format 1
    h[6..8].copy_from_slice(&flags.to_le_bytes());
    h[8..12].copy_from_slice(&seq.to_le_bytes());
    h[12..16].copy_from_slice(&(payload.len() as u32).to_le_bytes());
    h[16..20].copy_from_slice(&crc32(payload, 0).to_le_bytes());
    h[20..28].copy_from_slice(date.as_bytes());
    let n = ascii_replace(name);
    let k = n.len().min(16);
    h[28..28 + k].copy_from_slice(&n[..k]);
    let t = ascii_replace(tag);
    let k = t.len().min(16);
    h[44..44 + k].copy_from_slice(&t[..k]);
    let c = crc32(&h[..60], 0);
    h[60..64].copy_from_slice(&c.to_le_bytes());
    Ok(h)
}

/// The slot image of a new pack: the header with sequence seq (and flags), then the payload.
pub fn image(
    ptype: u8,
    payload: &[u8],
    date: &str,
    name: &str,
    seq: u32,
    tag: &str,
    flags: u16,
) -> Result<Vec<u8>, PackError> {
    let mut v = header(ptype, payload, date, name, seq, tag, flags)?;
    v.extend_from_slice(payload);
    Ok(v)
}

/// datapack.parse: the header's fields (None when it is no pack) and the payload.
pub fn parse(image: &[u8]) -> (Option<PackHeader>, Vec<u8>) {
    if image.len() < HEADER_SIZE || &image[..4] != MAGIC || le32(&image[60..]) != crc32(&image[..60], 0) {
        return (None, vec![]);
    }
    let length = le32(&image[12..]);
    let crc = le32(&image[16..]);
    let end = image.len().min(HEADER_SIZE + length as usize);
    let payload = image[HEADER_SIZE..end].to_vec();
    let h = PackHeader {
        ptype: image[4],
        format: image[5],
        seq: le32(&image[8..]),
        length,
        crc,
        flags: u16::from_le_bytes([image[6], image[7]]),
        date: ascii_decode(&image[20..28]),
        name: ascii_decode(rstrip_nul(&image[28..44])),
        tag: ascii_decode(rstrip_nul(&image[44..60])),
        crc_ok: payload.len() as u32 == length && crc32(&payload, 0) == crc,
    };
    (Some(h), payload)
}

/// datapack.next_seq: one more than the valid sequences the radio's two slots hold.
pub fn next_seq(images: &[Vec<u8>]) -> Result<u32, PackError> {
    let mut max = 0u32;
    for img in images {
        if let (Some(h), _) = parse(img) {
            if h.crc_ok && h.seq > max {
                max = h.seq;
            }
        }
    }
    if max == u32::MAX {
        return Err(perr("the sequence number is used up"));
    }
    Ok(max + 1)
}

fn best(ptype: u8, images: &[Vec<u8>]) -> Option<usize> {
    let mut best: Option<(usize, u32)> = None;
    for (i, img) in images.iter().enumerate() {
        if let (Some(h), _) = parse(img) {
            if h.crc_ok && h.ptype == ptype && best.is_none_or(|b| h.seq > b.1) {
                best = Some((i, h.seq));
            }
        }
    }
    best.map(|b| b.0)
}

/// datapack.inactive_slot: the slot a new pack goes into, the one not in force (0 when neither holds a valid pack).
pub fn inactive_slot(ptype: u8, images: &[Vec<u8>]) -> usize {
    best(ptype, images).map_or(0, |s| 1 - s)
}

/// The slot (0, 1) in force, or None: the valid pack of the type with the higher sequence.
pub fn active_slot(ptype: u8, images: &[Vec<u8>]) -> Option<usize> {
    best(ptype, images)
}

/// datapack.cps_plan: the sector writes for a slot image, in order: the payload's sectors, then the header's sector
/// last (its rewrite is the commit). Every sector lies inside the region, or this refuses.
pub fn cps_plan(slot_address: u64, image: &[u8]) -> Result<Vec<PackSector>, PackError> {
    let mut padded = vec![0xFFu8; image.len().div_ceil(SECTOR) * SECTOR];
    padded[..image.len()].copy_from_slice(image);
    let mut secs: Vec<PackSector> = padded
        .chunks(SECTOR)
        .enumerate()
        .map(|(i, c)| PackSector { address: slot_address + (i * SECTOR) as u64, data: c.to_vec() })
        .collect();
    if secs.is_empty() {
        return Err(perr("empty image"));
    }
    for s in &secs {
        if !in_region(s.address, SECTOR as u64) || s.address % SECTOR as u64 != 0 {
            return Err(perr(format!("sector 0x{:X}: outside the data pack region 0xE70000-0xEFFFFF", s.address)));
        }
    }
    let first = secs.remove(0);
    secs.push(first);
    Ok(secs)
}

/// The plan for a pack of a type into one of its slots: cps_plan after the slot size check.
pub fn plan_for_slot(ptype: u8, slot: usize, image: &[u8]) -> Result<Vec<PackSector>, PackError> {
    let size = slot_size(ptype)?;
    if image.len() > size {
        return Err(perr(format!("{} bytes: the slot holds {size}", image.len())));
    }
    cps_plan(slot_address(ptype, slot)?, image)
}

// --- RadioID (format 1) ---

fn code6(c: char) -> u32 {
    match c {
        ' ' => 0,
        '0'..='9' => 1 + (c as u32 - '0' as u32),
        'A'..='Z' => 11 + (c as u32 - 'A' as u32),
        'a'..='z' => 37 + (c as u32 - 'a' as u32),
        '.' => 63,
        _ => 0,
    }
}

/// datapack.radioid_payload: entries (id, callsign, name) -> an 8-byte header, then records sorted by ID (3 bytes
/// LE, the text 6 bits a character).
pub fn radioid_payload(entries: &[(i64, String, String)], chars: usize) -> Result<Vec<u8>, PackError> {
    if !chars.is_multiple_of(4) || !(4..=32).contains(&chars) {
        return Err(perr("text characters: 4..32, a multiple of 4"));
    }
    let mut list = entries.to_vec();
    list.sort();
    let rec = 3 + chars * 3 / 4;
    let mut out = Vec::with_capacity(8 + list.len() * rec);
    out.extend_from_slice(&(list.len() as u32).to_le_bytes());
    out.extend_from_slice(&[rec as u8, chars as u8, 0, 0]);
    let mut last = 0i64;
    for (rid, call, name) in &list {
        let rid = *rid;
        if rid <= 0 || rid > 0xFFFFFF || rid <= last {
            return Err(perr(format!("ID {rid}: out of range or repeated")));
        }
        last = rid;
        let folded = radioid::fold(py_strip(&format!("{call} {name}")));
        let mut text: Vec<char> = folded.chars().take(chars).collect();
        text.resize(chars, ' ');
        out.extend_from_slice(&[rid as u8, (rid >> 8) as u8, (rid >> 16) as u8]);
        for k in (0..chars).step_by(4) {
            let mut v = 0u32;
            for &c in &text[k..k + 4] {
                v = v << 6 | code6(c);
            }
            out.extend_from_slice(&[(v >> 16) as u8, (v >> 8) as u8, v as u8]);
        }
    }
    Ok(out)
}

/// datapack.radioid_from_csv: the payload, how many IDs, and what was skipped.
pub fn radioid_from_csv(csv: &[u8], countries: &[String]) -> Result<(Vec<u8>, usize, RadioIdSkipped), PackError> {
    let (rows, skipped) = radioid::read(csv, countries)?;
    let entries: Vec<(i64, String, String)> = rows.iter().map(|r| (r.id, r.call.clone(), r.name.clone())).collect();
    Ok((radioid_payload(&entries, ID_CHARS)?, rows.len(), skipped))
}

// --- Air band (format 1) ---

/// datapack.air_payload: {code: [(type index, kHz)]} -> a 4-byte header, per airfield its code (8), its count (1) and
/// the frequencies (channel 2, type 1).
pub fn air_payload(fields: &BTreeMap<String, Vec<(i64, i64)>>) -> Result<Vec<u8>, PackError> {
    if fields.len() > 0xFFFF {
        return Err(perr(format!("{} airfields", fields.len())));
    }
    let mut out = vec![fields.len() as u8, (fields.len() >> 8) as u8, 0, 0];
    // BTreeMap<String> iterates in code point order, as Python's sorted() does
    for (code, list) in fields {
        let freqs: Vec<&(i64, i64)> = list.iter().take(AIR_FREQS).collect();
        if code.is_empty() || code.chars().count() > 8 || code.chars().any(|c| c as u32 > 0x7F) {
            return Err(perr(format!("code '{code}'")));
        }
        let mut c = [0u8; 8];
        c[..code.len()].copy_from_slice(code.as_bytes());
        out.extend_from_slice(&c);
        out.push(freqs.len() as u8);
        for &&(t, khz) in &freqs {
            let ch = (khz - CH_BASE_KHZ).div_euclid(5); // Python's // floors
            if !(0..=0xFFFF).contains(&ch) || !(0..=0xFF).contains(&t) {
                return Err(perr(format!("frequency {khz} kHz")));
            }
            out.extend_from_slice(&[ch as u8, (ch >> 8) as u8, t as u8]);
        }
    }
    Ok(out)
}

/// datapack.info: what a slot image holds, in English (the tool's own text).
pub fn info(image: &[u8]) -> String {
    let (h, payload) = parse(image);
    let Some(h) = h else { return "not a data pack".into() };
    let mut lines = vec![format!(
        "type {} format {} sequence {} \"{}\" date {} {} bytes, payload CRC {}, built by {}",
        h.ptype,
        h.format,
        h.seq,
        h.name,
        h.date,
        h.length,
        if h.crc_ok { "ok" } else { "WRONG" },
        h.tag
    )];
    if h.ptype == RADIO_ID && payload.len() >= 8 {
        lines.push(format!("{} IDs, {} characters a record ({} bytes)", le32(&payload), payload[5], payload[4]));
    }
    if h.ptype == AIR && payload.len() >= 4 {
        lines.push(format!("{} airfields", u16::from_le_bytes([payload[0], payload[1]])));
    }
    if h.ptype == REF {
        lines.push(super::refpack::info(&payload, h.flags));
    }
    lines.join("\n")
}

/// How many entries a pack's payload holds (IDs or airfields), or None (also for a map pack: it has sections).
pub fn count(h: &PackHeader, payload: &[u8]) -> Option<usize> {
    if h.ptype == RADIO_ID && payload.len() >= 8 {
        Some(le32(payload) as usize)
    } else if h.ptype == AIR && payload.len() >= 4 {
        Some(u16::from_le_bytes([payload[0], payload[1]]) as usize)
    } else {
        None
    }
}
