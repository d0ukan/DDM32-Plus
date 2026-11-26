// SPDX-License-Identifier: GPL-3.0-or-later
//! The data pack writer over the running firmware's open-firmware CPS protocol (docs/spec/open-firmware-cps.md 2;
//! docs/plus.md "Loading a pack from the PC"; Radio/CpsPack.cs): the guard, the client and the tool.

use super::error::{ToolError, ToolResult};
use super::monitor::{be32, be32_bytes, crc32};
use super::port::{Clock, RadioPort, RawSerial};
use super::tools::{Args, ToolContext};
use crate::datapacks::datapack::{self as dp, PackHeader, PackSector, HEADER_SIZE, RADIO_ID, REF, SECTOR};
use crate::datapacks::refpack::{self, Rules, Running, SlotChoice, SlotVerdict, UpdateDecision};
use crate::pyjson::{self, Py, PyDict};
use crate::pytext::ascii_decode;

fn refuse(why: impl std::fmt::Display) -> ToolError {
    ToolError::value(format!("refused: {why}"))
}

/// The data pack transport guard: every frame is checked before it goes out. Read-only until [`CpsPackPort::arm`]
/// gives it the plan; then only that plan's sectors, in plan order, each once, with exactly the planned bytes, all
/// inside the data pack region 0xE70000-0xEFFFFF. Allowed:
///   R 9 (radio info); R 1 (flash, physical address) only inside the region, at most 4096 bytes;
///   after arm: C 0, C 1, C 2 (screen text), C 3, C 6 4 (red LED), C 5 (close);
///   X 1 (prepare the next planned sector), X 2 (its planned bytes, inside it), X 3 (commit, once the whole sector
///   was sent).
/// Refused: everything else: W, X 4 / X 7, C 6 0 / C 6 1 (reboots), C 6 2 (save settings), C 6 5..7, D, MMDVM, an
/// address outside the region, a sector out of order or twice, other bytes than planned. The baud rate is fixed.
pub struct CpsPackPort {
    pub raw: Box<dyn RawSerial>,
    plan: Option<Vec<PackSector>>,
    next: usize,
    pending: Option<PackSector>,
    covered: Vec<bool>,
    pub committed: usize,
    pub refusals: Vec<String>,
}

impl CpsPackPort {
    pub fn new(raw: Box<dyn RawSerial>) -> Self {
        Self { raw, plan: None, next: 0, pending: None, covered: vec![], committed: 0, refusals: vec![] }
    }

    pub fn armed(&self) -> bool {
        self.plan.is_some()
    }

    /// The write plan, once: from now on its sectors (and nothing else) may be written, in its order.
    pub fn arm(&mut self, sectors: &[PackSector]) -> ToolResult<()> {
        if self.plan.is_some() {
            return Err(ToolError::value("refused: the pack plan is set once"));
        }
        if sectors.is_empty() {
            return Err(ToolError::value("refused: an empty plan"));
        }
        let mut seen = std::collections::HashSet::new();
        for s in sectors {
            if s.data.len() != SECTOR || s.address % SECTOR as u64 != 0 || !dp::in_region(s.address, SECTOR as u64) {
                return Err(ToolError::value(format!(
                    "refused: sector 0x{:X} is outside the data pack region 0xE70000-0xEFFFFF",
                    s.address
                )));
            }
            if !seen.insert(s.address) {
                return Err(ToolError::value(format!("refused: sector 0x{:X} twice in the plan", s.address)));
            }
        }
        self.plan = Some(sectors.to_vec());
        Ok(())
    }

    /// Checks one frame; ValueError ("refused: ...") for anything outside the rules above.
    pub fn validate(&mut self, f: &[u8]) -> ToolResult<()> {
        if f.len() < 2 {
            return Err(refuse("short frame"));
        }
        match f[0] {
            0x52 => {
                if f.len() != 8 {
                    return Err(refuse("R frame length"));
                }
                let addr = be32(&f[2..]) as u64;
                let len = u16::from_be_bytes([f[6], f[7]]) as u64;
                if f[1] == 9 {
                    return Ok(());
                }
                if f[1] != 1 {
                    return Err(refuse(format!("R area {}", f[1])));
                }
                if len == 0 || len > SECTOR as u64 || !dp::in_region(addr, len) {
                    return Err(refuse(format!("R 0x{addr:X} +{len}: outside the data pack region 0xE70000-0xEFFFFF")));
                }
                Ok(())
            }
            0x43 => {
                if self.plan.is_none() {
                    return Err(refuse(format!("C {} before the plan", f[1])));
                }
                match f[1] {
                    0 | 1 | 3 | 5 if f.len() == 2 => Ok(()),
                    2 if (8..=7 + 17).contains(&f.len()) => Ok(()),
                    6 if f.len() == 3 && f[2] == 4 => Ok(()),
                    _ => Err(refuse(format!(
                        "C {}{}",
                        f[1],
                        if f.len() > 2 { format!(" {}", f[2]) } else { String::new() }
                    ))),
                }
            }
            0x58 => {
                let Some(plan) = self.plan.as_ref() else {
                    return Err(refuse(format!("X {} before the plan", f[1])));
                };
                match f[1] {
                    1 => {
                        if f.len() != 5 {
                            return Err(refuse("X 1 frame length"));
                        }
                        let a = ((f[2] as u64) << 16 | (f[3] as u64) << 8 | f[4] as u64) * SECTOR as u64;
                        if !dp::in_region(a, SECTOR as u64) {
                            return Err(refuse(format!(
                                "sector 0x{a:X} is outside the data pack region 0xE70000-0xEFFFFF"
                            )));
                        }
                        if let Some(p) = &self.pending {
                            return Err(refuse(format!("sector 0x{a:X} while 0x{:X} is pending", p.address)));
                        }
                        if self.next >= plan.len() || plan[self.next].address != a {
                            return Err(refuse(format!("sector 0x{a:X} is not the next planned sector")));
                        }
                        self.pending = Some(plan[self.next].clone());
                        self.covered = vec![false; SECTOR];
                        self.next += 1;
                        Ok(())
                    }
                    2 => {
                        let Some(p) = &self.pending else { return Err(refuse("X 2 without a prepared sector")) };
                        if f.len() < 9 {
                            return Err(refuse("X 2 frame length"));
                        }
                        let a = be32(&f[2..]) as u64;
                        let n = u16::from_be_bytes([f[6], f[7]]) as usize;
                        if n == 0 || n != f.len() - 8 {
                            return Err(refuse("X 2 length field"));
                        }
                        if a < p.address || a + n as u64 > p.address + SECTOR as u64 || !dp::in_region(a, n as u64) {
                            return Err(refuse(format!("X 2 0x{a:X} +{n}: outside the prepared sector")));
                        }
                        let off = (a - p.address) as usize;
                        if f[8..8 + n] != p.data[off..off + n] {
                            return Err(refuse(format!("X 2 0x{a:X}: other bytes than planned")));
                        }
                        for c in &mut self.covered[off..off + n] {
                            *c = true;
                        }
                        Ok(())
                    }
                    3 => {
                        if f.len() != 2 {
                            return Err(refuse("X 3 frame length"));
                        }
                        let Some(p) = &self.pending else { return Err(refuse("X 3 without a prepared sector")) };
                        if self.covered.iter().any(|c| !c) {
                            return Err(refuse(format!("X 3: sector 0x{:X} was not sent whole", p.address)));
                        }
                        self.pending = None;
                        self.committed += 1;
                        Ok(())
                    }
                    _ => Err(refuse(format!("X {}", f[1]))),
                }
            }
            other => Err(refuse(format!("command 0x{other:02X}"))),
        }
    }
}

impl RadioPort for CpsPackPort {
    fn timeout(&self) -> f64 {
        self.raw.timeout()
    }
    fn set_timeout(&mut self, t: f64) {
        self.raw.set_timeout(t)
    }
    fn baud_rate(&self) -> u32 {
        self.raw.baud_rate()
    }
    fn set_baud_rate(&mut self, baud: u32) -> ToolResult<()> {
        if baud != 115200 {
            return Err(ToolError::value("The data pack transport is fixed at 115200 baud"));
        }
        self.raw.set_baud_rate(baud)
    }
    fn write(&mut self, data: &[u8]) -> ToolResult<Option<usize>> {
        if let Err(e) = self.validate(data) {
            self.refusals.push(e.message.clone());
            self.pending = None; // a refused frame ends the sector: it is never committed
            return Err(e);
        }
        self.raw.write(data)
    }
    fn read(&mut self, n: usize) -> ToolResult<Vec<u8>> {
        self.raw.read(n)
    }
    fn reset_input_buffer(&mut self) -> ToolResult<()> {
        self.raw.reset_input_buffer()
    }
    fn flush(&mut self) -> ToolResult<()> {
        self.raw.flush()
    }
}

fn cps_error(m: impl Into<String>) -> ToolError {
    ToolError::new("CpsError", m)
}

fn hexu(b: &[u8]) -> String {
    hex::encode_upper(b)
}

/// The open-firmware CPS client for data packs (upstream tools/cps_client.py's Cps, the subset docs/plus.md names).
/// A sector write is never repeated: a bad reply ends the run. Reads change nothing and are asked again (twice at
/// most) when a reply is spoiled.
pub struct CpsClient<'a> {
    pub port: &'a mut CpsPackPort,
    clock: &'a dyn Clock,
    pub reply_timeout: f64,
}

pub const BLOCK: usize = 32;
pub const READ_CHUNK: usize = 1024;

impl<'a> CpsClient<'a> {
    pub fn new(port: &'a mut CpsPackPort, clock: &'a dyn Clock) -> Self {
        Self { port, clock, reply_timeout: 2.0 }
    }

    fn read_exactly(&mut self, n: usize, timeout: f64) -> ToolResult<Vec<u8>> {
        self.port.set_timeout(timeout);
        let mut out = Vec::with_capacity(n);
        let end = self.clock.monotonic() + timeout;
        while out.len() < n {
            let got = self.port.read(n - out.len())?;
            let empty = got.is_empty();
            out.extend_from_slice(&got);
            if empty && self.clock.monotonic() >= end {
                break;
            }
        }
        Ok(out)
    }

    pub fn control(&mut self, sub: u8, args: &[u8]) -> ToolResult<()> {
        let mut f = vec![0x43, sub];
        f.extend_from_slice(args);
        self.port.write(&f)?;
        let r = self.read_exactly(1, self.reply_timeout)?;
        if r != [0x2D] {
            return Err(cps_error(format!("C {sub}: reply {}", hexu(&r))));
        }
        Ok(())
    }

    pub fn text(&mut self, y: u8, s: &str) -> ToolResult<()> {
        let t: Vec<u8> = s.chars().map(|c| if (c as u32) < 0x80 { c as u8 } else { b'?' }).collect();
        let mut f = vec![0x43, 2, 0, y, 1, 1, 0];
        f.extend_from_slice(&t[..t.len().min(16)]);
        f.push(0);
        self.port.write(&f)?;
        let r = self.read_exactly(1, self.reply_timeout)?;
        if r != [0x2D] {
            return Err(cps_error(format!("C 2: reply {}", hexu(&r))));
        }
        Ok(())
    }

    /// The write session's opening without "save settings" (C 6 2): the data chip outside the region is not touched.
    pub fn opening(&mut self, line: &str) -> ToolResult<()> {
        self.control(0, &[])?;
        self.control(1, &[])?;
        self.text(0, "DDM32 Plus")?;
        self.text(16, line)?;
        self.control(3, &[])?;
        self.control(6, &[4])
    }

    pub fn close(&mut self) -> ToolResult<()> {
        self.control(5, &[])
    }

    fn drain(&mut self) -> ToolResult<()> {
        self.clock.sleep(0.1);
        self.port.set_timeout(0.1);
        for _ in 0..64 {
            if self.port.read(4096)?.is_empty() {
                break;
            }
        }
        Ok(())
    }

    pub fn read(&mut self, area: u8, addr: u64, n: usize, retries: u32) -> ToolResult<Vec<u8>> {
        let mut attempt = 0;
        loop {
            match self.read_once(area, addr, n) {
                Err(e) if e.py_type == "CpsError" && attempt < retries => {
                    self.drain()?;
                    attempt += 1;
                }
                r => return r,
            }
        }
    }

    fn read_once(&mut self, area: u8, addr: u64, n: usize) -> ToolResult<Vec<u8>> {
        let mut f = vec![0x52, area];
        f.extend_from_slice(&be32_bytes(addr));
        f.extend_from_slice(&[(n >> 8) as u8, n as u8]);
        self.port.write(&f)?;
        let h = self.read_exactly(1, self.reply_timeout)?;
        if h != [0x52] {
            return Err(cps_error(format!("R {area} 0x{addr:X} {n}: reply {}", hexu(&h))));
        }
        let l = self.read_exactly(2, self.reply_timeout)?;
        if l.len() != 2 {
            return Err(cps_error(format!("R {area} 0x{addr:X}: short reply")));
        }
        let ln = (l[0] as usize) << 8 | l[1] as usize;
        if area == 1 && ln != n {
            return Err(cps_error(format!("R {area} 0x{addr:X} {n}: reply length {ln}")));
        }
        let d = self.read_exactly(ln, self.reply_timeout + ln as f64 / 5000.0)?;
        if d.len() != ln {
            return Err(cps_error(format!("R {area} 0x{addr:X}: short reply {} of {ln}", d.len())));
        }
        Ok(d)
    }

    /// Flash bytes [addr, addr + n) in chunks of 1024.
    pub fn read_flash(&mut self, addr: u64, n: usize) -> ToolResult<Vec<u8>> {
        let mut out = Vec::with_capacity(n);
        let mut o = 0;
        while o < n {
            let k = READ_CHUNK.min(n - o);
            out.extend(self.read(1, addr + o as u64, k, 2)?);
            o += k;
        }
        Ok(out)
    }

    /// The radio info (area 9): struct version, radio type (11 = DM-32UV), git, build, flash ID.
    pub fn info(&mut self) -> ToolResult<PyDict> {
        let d = self.read(9, 0, 0x20, 2)?;
        if d.len() < 44 {
            return Err(cps_error(format!("radio info: {} bytes", d.len())));
        }
        let s = |o: usize, n: usize| {
            let b = &d[o..o + n];
            let z = b.iter().position(|&x| x == 0).unwrap_or(b.len());
            ascii_decode(&b[..z])
        };
        let le = |o: usize| u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]]) as i64;
        Ok(PyDict::new()
            .with("struct", le(0))
            .with("radio_type", le(4))
            .with("git", s(8, 16))
            .with("build", s(24, 16))
            .with("flash_id", le(40)))
    }

    fn ack(&mut self, what: &str, sub: u8, timeout: f64) -> ToolResult<()> {
        let r = self.read_exactly(2, timeout)?;
        if r.len() != 2 || r[0] != 0x58 || r[1] != sub {
            return Err(cps_error(format!("{what}: reply {}", hexu(&r))));
        }
        Ok(())
    }

    /// One journaled sector write: prepare, the 4096 bytes in 32-byte blocks, commit. Never repeated.
    pub fn write_sector(&mut self, addr: u64, data: &[u8]) -> ToolResult<()> {
        if data.len() != SECTOR || !addr.is_multiple_of(SECTOR as u64) {
            return Err(ToolError::value("a sector is 4096 bytes on a 4096-byte boundary"));
        }
        let sector = addr / SECTOR as u64;
        self.port.write(&[0x58, 1, (sector >> 16) as u8, (sector >> 8) as u8, sector as u8])?;
        self.ack(&format!("prepare sector 0x{addr:X}"), 1, self.reply_timeout)?;
        for o in (0..SECTOR).step_by(BLOCK) {
            let mut f = vec![0x58, 2];
            f.extend_from_slice(&be32_bytes(addr + o as u64));
            f.extend_from_slice(&[(BLOCK >> 8) as u8, BLOCK as u8]);
            f.extend_from_slice(&data[o..o + BLOCK]);
            self.port.write(&f)?;
            self.ack(&format!("data 0x{:X}", addr + o as u64), 2, self.reply_timeout)?;
        }
        self.port.write(&[0x58, 3])?;
        self.ack(&format!("commit sector 0x{addr:X}"), 3, self.reply_timeout * 3.0)
        // erase, program, journal
    }
}

pub const RADIO_TYPE_DM32: i64 = 11;

/// One slot as the radio holds it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SlotState {
    pub ptype: u8,
    pub slot: usize,
    pub address: u64,
    pub header: Option<PackHeader>,
    pub count: Option<usize>,
    pub in_force: bool,
    /// "empty", "valid", "bad", "other" (a map pack for another firmware version) or "big" (a RadioID pack over
    /// 192 KB that firmware 1.1 does not use; docs/mappack-design.md B.1 rule 4)
    pub state: &'static str,
    /// A map pack's repeater data source (RPT_INFO), if any.
    pub source: Option<String>,
}

/// A slot's image as far as the checks need it: the header, and the payload when the header is valid.
pub fn read_slot(c: &mut CpsClient<'_>, ptype: u8, slot: usize) -> ToolResult<Vec<u8>> {
    let addr = dp::slot_address(ptype, slot).map_err(|e| ToolError::value(e.0))?;
    let mut head = c.read_flash(addr, HEADER_SIZE)?;
    if &head[..4] == dp::MAGIC && u32::from_le_bytes([head[60], head[61], head[62], head[63]]) == crc32(&head[..60], 0)
    {
        let len = u32::from_le_bytes([head[12], head[13], head[14], head[15]]) as usize;
        if len > 0 && len <= dp::slot_size(ptype).unwrap_or(0) - HEADER_SIZE {
            head.extend(c.read_flash(addr + HEADER_SIZE as u64, len)?);
        }
    }
    Ok(head)
}

fn base_state(ptype: u8, img: &[u8]) -> &'static str {
    if img.len() < 4 || &img[..4] != dp::MAGIC {
        return "empty";
    }
    match dp::parse(img).0 {
        None => "bad",
        Some(h) if ptype == RADIO_ID && h.ptype == RADIO_ID && h.length as usize > dp::RADIO_ID_PAYLOAD_MAX => "big",
        Some(h) if h.crc_ok && h.ptype == ptype && h.format == 1 => "valid",
        Some(_) => "bad",
    }
}

/// Both slots of a type; a map pack is judged with the firmware's rules (the pack's own coordinate id when the
/// firmware's is not known).
pub fn describe(ptype: u8, images: &[Vec<u8>]) -> Vec<SlotState> {
    describe_with(ptype, images, &Rules::new(None))
}

pub fn describe_with(ptype: u8, images: &[Vec<u8>], rules: &Rules) -> Vec<SlotState> {
    let active = if ptype == REF { refpack::in_force(images, rules) } else { dp::active_slot(ptype, images) };
    images
        .iter()
        .enumerate()
        .map(|(i, img)| {
            let (h, payload) = dp::parse(img);
            let mut count = h.as_ref().filter(|h| h.crc_ok).and_then(|h| dp::count(h, &payload));
            if ptype == REF && h.as_ref().is_some_and(|h| h.crc_ok) {
                // a map pack's entries: its repeaters
                count = refpack::directory(&payload).ok().map(|d| d.count(refpack::RPT) as usize);
            }
            let state = if ptype == REF {
                match refpack::judge(img, rules) {
                    SlotVerdict::Empty => "empty",
                    SlotVerdict::Valid => "valid",
                    SlotVerdict::Bad(_) => "bad",
                    SlotVerdict::Other(_) => "other",
                }
            } else {
                base_state(ptype, img)
            };
            let source = if ptype == REF { refpack::rpt_info(&payload).map(|r| r.source) } else { None };
            SlotState {
                ptype,
                slot: i,
                address: dp::slot_address(ptype, i).unwrap(),
                header: h,
                count,
                in_force: active == Some(i),
                state,
                source,
            }
        })
        .collect()
}

pub fn slot_json(s: &SlotState) -> PyDict {
    let h = s.header.as_ref();
    let mut d = PyDict::new()
        .with("type", dp::type_name(s.ptype))
        .with("slot", s.slot)
        .with("address", pyjson::hex(s.address as i64))
        .with("valid", h.is_some())
        .with("crc_ok", h.is_some_and(|h| h.crc_ok))
        .with("in_force", s.in_force)
        .with("seq", h.map(|h| h.seq as i64))
        .with("name", h.map(|h| h.name.clone()))
        .with("date", h.map(|h| h.date.clone()))
        .with("bytes", h.map(|h| h.length as i64))
        .with("count", s.count)
        .with("tag", h.map(|h| h.tag.clone()))
        .with("state", s.state);
    if s.ptype == REF {
        d = d
            .with("source", s.source.clone())
            .with("user_built", h.map(|h| !refpack::is_release_built(&h.tag)))
            .with("flags", h.map(|h| h.flags as i64));
    }
    d
}

fn line(d: PyDict) -> String {
    pyjson::dumps(&Py::Dict(d), None)
}

/// The firmware version the CPS info's `git` field names ("ddm32-plus-1.1." cut to 15 characters, E.3): (major,
/// minor), or None when it is not a DDM32 Plus version string.
pub fn running_version(git: &str) -> Option<(u32, u32)> {
    let rest = git.strip_prefix("ddm32-plus-")?;
    let mut it = rest.split('.');
    let major = it.next()?.parse().ok()?;
    let minor: String = it.next()?.chars().take_while(char::is_ascii_digit).collect();
    Some((major, minor.parse().ok()?))
}

/// "1.1.0=0x1A2B3C4D,1.2.0=0x..." (the coordinate ids of the releases the program knows) -> [(version, id)].
pub fn parse_known_coords(text: &str) -> ToolResult<Vec<(String, u32)>> {
    let mut out = Vec::new();
    for part in text.split(',').map(str::trim).filter(|p| !p.is_empty()) {
        let (v, c) = part.split_once('=').ok_or_else(|| ToolError::exit("2"))?;
        out.push((v.trim().to_string(), parse_coord(c).ok_or_else(|| ToolError::exit("2"))?));
    }
    Ok(out)
}

/// "0x1A2B3C4D" (or plain hex) -> the coordinate id.
pub fn parse_coord(text: &str) -> Option<u32> {
    let t = text.trim();
    let t = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")).unwrap_or(t);
    if t.is_empty() || t.len() > 8 {
        return None;
    }
    u32::from_str_radix(t, 16).ok()
}

/// What the running firmware makes of the map slots, from its version and the known coordinate ids (R3).
pub fn running_rules(version: Option<(u32, u32)>, known: &[(String, u32)]) -> Running {
    match version {
        None => Running::Unknown,
        Some(v) if v < (1, 1) => Running::NoMapPack,
        Some((ma, mi)) => {
            let ours = |ver: &str| {
                let mut it = ver.split(['.', '-']);
                it.next().and_then(|x| x.parse::<u32>().ok()) == Some(ma)
                    && it.next().and_then(|x| x.parse::<u32>().ok()) == Some(mi)
            };
            let mut coords: Vec<u32> = known.iter().filter(|(v, _)| ours(v)).map(|(_, c)| *c).collect();
            coords.sort();
            coords.dedup();
            // ids only other versions use (a patch build of this version may share one: then it is not foreign)
            let mut foreign: Vec<u32> =
                known.iter().filter(|(v, c)| !ours(v) && !coords.contains(c)).map(|(_, c)| *c).collect();
            foreign.sort();
            foreign.dedup();
            Running::MapPack(coords, foreign)
        }
    }
}

/// The data pack tool (like the other radio tools: argv, printed lines, the port only from ctx.serial):
///   read --port COMx [--known-coords V=ID,...]
///       the radio info and both slots of each pack type (read only)
///   write --port COMx --pack FILE.dpk [--coord ID] [--known-coords V=ID,...]
///       the pack into the slot not in force (docs/plus.md, steps 1-5; a map pack by docs/mappack-design.md E.3 R3)
///   update-ref --port COMx --pack RELEASE.dpk --coord ID [--known-coords ...] [--choice keep|release|rebuild]
///       the update step's map pack (E.3 S-B): nothing when the radio has it already; a user-built pack in force is
///       never replaced without --choice (the tool says "ASK" and writes nothing)
pub fn data_pack_tool(argv: &[String], ctx: &ToolContext<'_>) -> ToolResult<()> {
    if argv.is_empty() || !(argv[0] == "read" || argv[0] == "write" || argv[0] == "update-ref") {
        return Err(ToolError::exit("2"));
    }
    let a = Args::new(
        &argv[1..],
        &["--port", "--pack", "--line", "--coord", "--known-coords", "--choice"],
        &["--allow-radioid-loss"],
        &["--port"],
    )?;
    let port_name = a.get("--port").ok_or_else(|| ToolError::exit("2"))?.to_string();
    let coord = match a.get("--coord") {
        Some(c) => Some(parse_coord(c).ok_or_else(|| ToolError::exit("2"))?),
        None => None,
    };
    let known = parse_known_coords(a.get("--known-coords").unwrap_or(""))?;
    let choice = a.get("--choice").map(str::to_string);
    if choice.as_deref().is_some_and(|c| !["keep", "release", "rebuild"].contains(&c)) {
        return Err(ToolError::exit("2"));
    }
    let mut pack: Option<(Vec<u8>, PackHeader)> = None;
    if argv[0] != "read" {
        // every check of the file before the port is opened
        let path = std::path::PathBuf::from(a.get("--pack").ok_or_else(|| ToolError::exit("2"))?);
        let image = std::fs::read(&path)?;
        let fname = path.file_name().unwrap_or_default().to_string_lossy().into_owned();
        let built = crate::datapacks::files::check_image(&image, &fname).map_err(|e| ToolError::value(e.0))?;
        let h = built.header;
        if h.ptype == REF {
            if let refpack::Verdict::Bad(e) | refpack::Verdict::Other(e) =
                refpack::validate(&image[HEADER_SIZE..], h.flags, &Rules::new(coord))
            {
                return Err(ToolError::value(format!("{fname}: not for this firmware: {e}")));
            }
        }
        if argv[0] == "update-ref" && (h.ptype != REF || coord.is_none()) {
            return Err(ToolError::value("update-ref takes a map pack and its release's --coord"));
        }
        pack = Some((image, h));
    }
    let mut raw = (ctx.serial)(&port_name)?;
    raw.open()?;
    raw.set_baud_rate(115200)?;
    raw.reset_input_buffer()?;
    let mut guard = CpsPackPort::new(raw);
    let opts = RefOptions { coord, known, choice, allow_loss: a.has("--allow-radioid-loss") };
    let result = data_pack_run(&mut guard, &argv[0], pack, a.get("--line").unwrap_or("Data pack"), &opts, ctx);
    let _ = guard.raw.close();
    result
}

struct RefOptions {
    coord: Option<u32>,
    known: Vec<(String, u32)>,
    choice: Option<String>,
    allow_loss: bool,
}

fn data_pack_run(
    guard: &mut CpsPackPort,
    cmd: &str,
    pack: Option<(Vec<u8>, PackHeader)>,
    line_text: &str,
    opts: &RefOptions,
    ctx: &ToolContext<'_>,
) -> ToolResult<()> {
    let mut c = CpsClient::new(guard, ctx.clock);
    let info = c.info()?;
    (ctx.print)(&line(PyDict::new().with("radio", info.clone())));
    if info.get("radio_type").and_then(Py::as_i64) != Some(RADIO_TYPE_DM32) {
        let t = info.get("radio_type").and_then(Py::as_i64).map_or("None".into(), |t| t.to_string());
        return Err(ToolError::new("RadioError", format!("radio type {t}: not a DM-32UV running DDM32 Plus")));
    }
    let git = info.get("git").and_then(Py::as_str).unwrap_or("").to_string();
    let running = running_rules(running_version(&git), &opts.known);
    if cmd == "read" {
        let rules = match &running {
            Running::MapPack(c, _) if c.len() == 1 => Rules::new(Some(c[0])),
            _ => Rules::new(None),
        };
        for ptype in dp::TYPES {
            let images = vec![read_slot(&mut c, ptype, 0)?, read_slot(&mut c, ptype, 1)?];
            for s in describe_with(ptype, &images, &rules) {
                (ctx.print)(&line(PyDict::new().with("slot", slot_json(&s))));
            }
        }
        (ctx.print)(&line(PyDict::new().with("result", "READ")));
        return Ok(());
    }
    let (image, ph) = pack.unwrap();
    if ph.ptype != REF {
        return write_pack(&mut c, &image, &ph, line_text, ctx);
    }
    let target = Rules::new(target_coord(opts.coord, &running, &image[HEADER_SIZE..])?);
    if let refpack::Verdict::Bad(e) | refpack::Verdict::Other(e) =
        refpack::validate(&image[HEADER_SIZE..], ph.flags, &target)
    {
        return Err(ToolError::new("PackError", format!("ref_other: not for the radio's firmware: {e}")));
    }
    if running == Running::NoMapPack {
        // harmless (1.0.x never reads the map slots), said for the user (E.3)
        (ctx.print)(&line(PyDict::new().with("note", "ref_after_update")));
    }
    // a RadioID pack over 192 KB has its tail in the map slot of its side (B.4): that slot is avoided, said first
    let mut avoid = Vec::new();
    for slot in 0..2 {
        let head = read_slot(&mut c, RADIO_ID, slot)?;
        if base_state(RADIO_ID, &head) == "big" {
            (ctx.print)(&line(PyDict::new().with("warning", "radioid_big").with("slot", slot)));
            avoid.push(refpack::radioid_partner(slot));
        }
    }
    let before = vec![read_slot(&mut c, REF, 0)?, read_slot(&mut c, REF, 1)?];
    for s in describe_with(REF, &before, &target) {
        (ctx.print)(&line(PyDict::new().with("slot", slot_json(&s))));
    }
    let mut payload = image[HEADER_SIZE..].to_vec();
    let (mut name, mut tag) = (ph.name.clone(), ph.tag.clone());
    if cmd == "update-ref" {
        match refpack::update_decision(&before, &running, &image, &target) {
            UpdateDecision::Same => {
                (ctx.print)(&line(PyDict::new().with("result", "SAME")));
                return Ok(());
            }
            UpdateDecision::WriteRelease => {}
            UpdateDecision::AskUserBuilt { name: un, tag: ut, keep_ok, rebuild_ok } => match opts.choice.as_deref() {
                None => {
                    (ctx.print)(&line(
                        PyDict::new().with(
                            "ask",
                            PyDict::new()
                                .with("name", un)
                                .with("tag", ut)
                                .with("keep", keep_ok)
                                .with("rebuild", rebuild_ok),
                        ),
                    ));
                    (ctx.print)(&line(PyDict::new().with("result", "ASK")));
                    return Ok(());
                }
                Some("keep") => {
                    (ctx.print)(&line(PyDict::new().with("result", "KEPT").with("valid", keep_ok)));
                    return Ok(());
                }
                Some("rebuild") => {
                    let s = refpack::in_force_for_update(&before, &running).unwrap_or(0);
                    let (uh, up) = dp::parse(&before[s]);
                    let uh = uh.ok_or_else(|| ToolError::value("the user's pack cannot be read"))?;
                    payload = refpack::rebuild(&payload, ph.flags, &up, &target)
                        .map_err(|e| ToolError::new("PackError", format!("the pack cannot be rebuilt: {e}")))?;
                    name = uh.name;
                    tag = dp::TAG.to_string();
                }
                _ => {} // "release": the release pack as it is
            },
        }
    }
    let choice = refpack::choose_slot(&before, &running, &target, &payload, &avoid, opts.allow_loss)
        .map_err(|e| ToolError::new("PackError", e))?;
    let (slot, seq) = match choice {
        SlotChoice::AlreadyThere { slot } => {
            (ctx.print)(&line(PyDict::new().with("result", "SAME").with("slot", slot)));
            return Ok(());
        }
        SlotChoice::Write { slot, seq } => (slot, seq),
    };
    let pe = |e: crate::datapacks::PackError| ToolError::new("PackError", e.0);
    let new = dp::image(REF, &payload, &ph.date, &name, seq, &tag, ph.flags).map_err(pe)?;
    let w = Written { ptype: REF, slot, seq, name: &name, date: &ph.date, rules: target };
    write_into(&mut c, &w, &new, line_text, ctx, &|after: &[Vec<u8>]| refpack::in_force(after, &target) == Some(slot))
}

/// The coordinate id a map pack is written for: the release's (--coord), else the running firmware's when the
/// program knows exactly one for its version; with several, the pack's own when it is one of them; else the pack's
/// own (a firmware before 1.1, or not known). A pack for another id than the running firmware's is refused.
pub fn target_coord(coord: Option<u32>, running: &Running, payload: &[u8]) -> ToolResult<Option<u32>> {
    if coord.is_some() {
        return Ok(coord);
    }
    let own = refpack::directory(payload).ok().map(|d| d.coord);
    match running {
        Running::MapPack(c, _) if c.len() == 1 => Ok(Some(c[0])),
        Running::MapPack(c, _) if c.len() > 1 => match own.filter(|o| c.contains(o)) {
            Some(o) => Ok(Some(o)),
            None => Err(ToolError::new(
                "PackError",
                "ref_other: the map pack is for none of the coordinate ids of the radio's firmware version",
            )),
        },
        _ => Ok(None),
    }
}

fn write_pack(
    c: &mut CpsClient<'_>,
    image: &[u8],
    ph: &PackHeader,
    line_text: &str,
    ctx: &ToolContext<'_>,
) -> ToolResult<()> {
    let ptype = ph.ptype;
    let pe = |e: crate::datapacks::PackError| ToolError::new("PackError", e.0);
    let before = vec![read_slot(c, ptype, 0)?, read_slot(c, ptype, 1)?];
    for s in describe(ptype, &before) {
        (ctx.print)(&line(PyDict::new().with("slot", slot_json(&s))));
    }
    let (slot, seq) = if ptype == RADIO_ID { radioid_slot(&before)? } else { (dp::inactive_slot(ptype, &before), 0) };
    let seq = if ptype == RADIO_ID { seq } else { dp::next_seq(&before).map_err(pe)? };
    let payload = &image[HEADER_SIZE..];
    let target = dp::image(ptype, payload, &ph.date, &ph.name, seq, &ph.tag, ph.flags).map_err(pe)?;
    let w = Written { ptype, slot, seq, name: &ph.name, date: &ph.date, rules: Rules::new(None) };
    write_into(c, &w, &target, line_text, ctx, &|after: &[Vec<u8>]| dp::active_slot(ptype, after) == Some(slot))
}

/// A RadioID write's slot (N1): a slot whose header checks but whose pack is over 192 KB (a 1.0.x pack: only its
/// header is read) counts as a valid pack of its sequence, since a 1.0.x firmware may have it in force; the new pack
/// goes into the other slot, with a sequence above both.
fn radioid_slot(images: &[Vec<u8>]) -> ToolResult<(usize, u32)> {
    let live = |img: &Vec<u8>| -> Option<u32> {
        let h = dp::parse(img).0?;
        let big = h.ptype == RADIO_ID && h.length as usize > dp::RADIO_ID_PAYLOAD_MAX;
        (h.ptype == RADIO_ID && (h.crc_ok || big)).then_some(h.seq)
    };
    let mut best: Option<(usize, u32)> = None;
    for (i, img) in images.iter().enumerate() {
        if let Some(seq) = live(img) {
            if best.is_none_or(|b| seq > b.1) {
                best = Some((i, seq));
            }
        }
    }
    let max = images.iter().filter_map(live).max().unwrap_or(0);
    let seq = max.checked_add(1).ok_or_else(|| ToolError::new("PackError", "the sequence number is used up"))?;
    Ok((best.map_or(0, |b| 1 - b.0), seq))
}

/// The slot a write goes to, what the PASS line names, and the rules the slot lines after the write judge with.
struct Written<'a> {
    ptype: u8,
    slot: usize,
    seq: u32,
    name: &'a str,
    date: &'a str,
    rules: Rules,
}

/// The plan for one slot, the write (payload sectors, the header's sector last, each read back) and the check that
/// the new pack is the one in force afterwards.
fn write_into(
    c: &mut CpsClient<'_>,
    w: &Written<'_>,
    target: &[u8],
    line_text: &str,
    ctx: &ToolContext<'_>,
    in_force: &dyn Fn(&[Vec<u8>]) -> bool,
) -> ToolResult<()> {
    let (ptype, slot) = (w.ptype, w.slot);
    let pe = |e: crate::datapacks::PackError| ToolError::new("PackError", e.0);
    let plan = dp::plan_for_slot(ptype, slot, target).map_err(pe)?;
    (ctx.print)(&line(
        PyDict::new().with(
            "plan",
            PyDict::new()
                .with("type", dp::type_name(ptype))
                .with("slot", slot)
                .with("address", pyjson::hex(dp::slot_address(ptype, slot).map_err(pe)? as i64))
                .with("seq", w.seq as i64)
                .with("bytes", target.len())
                .with("sectors", Py::List(plan.iter().map(|p| Py::Str(pyjson::hex(p.address as i64))).collect())),
        ),
    ));
    c.port.arm(&plan)?;
    c.opening(line_text)?;
    let r = (|| -> ToolResult<()> {
        for (i, p) in plan.iter().enumerate() {
            c.write_sector(p.address, &p.data)?;
            let back = c.read_flash(p.address, SECTOR)?;
            if back != p.data {
                return Err(ToolError::new(
                    "VerifyError",
                    format!("sector 0x{:X}: read back differs (not written again)", p.address),
                ));
            }
            (ctx.print)(&format!("pack: sector {}/{} 0x{:X} written and read back", i + 1, plan.len(), p.address));
        }
        c.close()?;
        Ok(())
    })();
    if let Err(e) = r {
        // leave the CPS screen; the pack in force is unchanged (its header was not written)
        let _ = c.close();
        return Err(e);
    }
    let after = vec![read_slot(c, ptype, 0)?, read_slot(c, ptype, 1)?];
    for s in describe_with(ptype, &after, &w.rules) {
        (ctx.print)(&line(PyDict::new().with("slot", slot_json(&s))));
    }
    if after[slot] != target || !in_force(&after) {
        return Err(ToolError::new("VerifyError", "the new pack is not the one in force after the write"));
    }
    (ctx.print)(&line(
        PyDict::new()
            .with("result", "PASS")
            .with("slot", slot)
            .with("seq", w.seq as i64)
            .with("sectors", plan.len())
            .with("name", w.name.to_string())
            .with("date", w.date.to_string()),
    ));
    Ok(())
}
