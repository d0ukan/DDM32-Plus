// SPDX-License-Identifier: GPL-3.0-or-later
//! The read-only debug link of the running firmware (the pinned tools/rfoundry_debug.py, RadioFoundry MIT; the C#
//! Radio/DebugLink.cs): the frame, its guard and the two readers, health (plus_txdiag --health) and live (plus_live).
//! Held to the recorded conversations of win/tests/DDM32Plus.Core.Tests/Fixtures/live-results.json.

use super::error::{ToolError, ToolResult};
use super::port::{RadioPort, RawSerial};
use super::tools::{Args, ToolContext};
use crate::pytext::py_isspace;
use std::collections::HashSet;
use std::sync::atomic::{AtomicBool, Ordering};

pub const SOF1: u8 = 0xA5;
pub const SOF2: u8 = 0x5A;
pub const CMD_ERROR: u8 = 0xFF;
pub const HEADER: usize = 6;
pub const REQUEST_MAX: usize = 64;
pub const ANSWER_MAX: usize = 1024;
pub const STATUS_OK: u8 = 0;

/// Python's STATUS.get(st, st): the name, the number, or "None".
pub fn status_text(st: Option<u8>) -> String {
    match st {
        None => "None".into(),
        Some(0) => "ok".into(),
        Some(1) => "unknown command".into(),
        Some(2) => "bad argument".into(),
        Some(3) => "busy".into(),
        Some(4) => "unavailable".into(),
        Some(5) => "bad CRC".into(),
        Some(6) => "request too long".into(),
        Some(7) => "refused".into(),
        Some(n) => n.to_string(),
    }
}

/// CRC-16/CCITT-FALSE.
pub fn crc16(data: &[u8]) -> u16 {
    let mut crc: u16 = 0xFFFF;
    for &b in data {
        crc ^= (b as u16) << 8;
        for _ in 0..8 {
            crc = if crc & 0x8000 != 0 { (crc << 1) ^ 0x1021 } else { crc << 1 };
        }
    }
    crc
}

/// SOF A5 5A, command, sequence, payload length u16, payload, CRC-16 u16 over command..payload.
pub fn build(cmd: u8, seq: u8, payload: &[u8]) -> Vec<u8> {
    let mut f = vec![SOF1, SOF2, cmd, seq];
    f.extend_from_slice(&(payload.len() as u16).to_le_bytes());
    f.extend_from_slice(payload);
    let c = crc16(&f[2..]);
    f.extend_from_slice(&c.to_le_bytes());
    f
}

/// (command, payload) of one whole, sound request frame; None for anything else.
pub fn parse_request(f: &[u8]) -> Option<(u8, Vec<u8>)> {
    if f.len() < HEADER + 2 || f[0] != SOF1 || f[1] != SOF2 {
        return None;
    }
    let n = f[4] as usize | (f[5] as usize) << 8;
    if f.len() != HEADER + n + 2 || n > REQUEST_MAX {
        return None;
    }
    if crc16(&f[2..HEADER + n]) != (f[HEADER + n] as u16 | (f[HEADER + n + 1] as u16) << 8) {
        return None;
    }
    Some((f[2], f[HEADER..HEADER + n].to_vec()))
}

fn link_error(message: impl Into<String>, status: Option<u8>) -> ToolError {
    ToolError { py_type: "DebugLinkError".into(), message: message.into(), status }
}

/// The read-only guard of the debug link: only the listed pairs of command and exact one-byte payload pass; anything
/// else, a damaged frame or a baud change is refused before it reaches the port.
pub struct DebugLinkPort {
    pub raw: Box<dyn RawSerial>,
    allowed: HashSet<(u8, u8)>,
    tool: &'static str,
    pub refusals: Vec<String>,
}

impl DebugLinkPort {
    pub fn new(raw: Box<dyn RawSerial>, allowed: &[(u8, u8)], tool: &'static str) -> Self {
        Self { raw, allowed: allowed.iter().copied().collect(), tool, refusals: vec![] }
    }

    fn refuse(&mut self, why: String) -> ToolError {
        self.refusals.push(why.clone());
        ToolError::new("Refused", why)
    }

    pub fn validate(&mut self, data: &[u8]) -> ToolResult<()> {
        let Some((cmd, payload)) = parse_request(data) else {
            return Err(self.refuse(format!("{} refused a damaged frame before transmission", self.tool)));
        };
        if payload.len() != 1 || !self.allowed.contains(&(cmd, payload[0])) {
            return Err(self.refuse(format!(
                "{} refused command 0x{cmd:02x} {} before transmission",
                self.tool,
                hex::encode(&payload)
            )));
        }
        Ok(())
    }
}

impl RadioPort for DebugLinkPort {
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
            return Err(ToolError::new("Refused", "The debug link is fixed at 115200 baud"));
        }
        self.raw.set_baud_rate(baud)
    }
    fn write(&mut self, data: &[u8]) -> ToolResult<Option<usize>> {
        self.validate(data)?;
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

/// One open debug link: a 1 s answer timeout, one try (busy_wait 0).
pub struct DebugLinkClient<'a> {
    port: &'a mut DebugLinkPort,
    clock: &'a dyn super::port::Clock,
    port_name: String,
    timeout: f64,
    buf: Vec<u8>,
    seq: u8,
}

impl<'a> DebugLinkClient<'a> {
    pub fn new(port: &'a mut DebugLinkPort, clock: &'a dyn super::port::Clock, port_name: &str) -> Self {
        Self { port, clock, port_name: port_name.into(), timeout: 1.0, buf: vec![], seq: 0 }
    }

    fn read_frame(&mut self, deadline: f64) -> ToolResult<Option<(u8, u8, Vec<u8>)>> {
        while self.clock.monotonic() < deadline {
            let got = self.port.read(256)?;
            self.buf.extend_from_slice(&got);
            loop {
                let i = self.buf.windows(2).position(|w| w[0] == SOF1 && w[1] == SOF2);
                let Some(i) = i else {
                    if self.buf.len() > 1 {
                        let keep = self.buf.len() - 1;
                        self.buf.drain(..keep); // other protocols' bytes are skipped
                    }
                    break;
                };
                self.buf.drain(..i);
                if self.buf.len() < HEADER {
                    break;
                }
                let n = self.buf[4] as usize | (self.buf[5] as usize) << 8;
                if n > ANSWER_MAX {
                    self.buf.remove(0); // can't be ours: resync past this SOF
                    continue;
                }
                if self.buf.len() < HEADER + n + 2 {
                    break;
                }
                let f: Vec<u8> = self.buf[..HEADER + n + 2].to_vec();
                if crc16(&f[2..HEADER + n]) != (f[HEADER + n] as u16 | (f[HEADER + n + 1] as u16) << 8) {
                    self.buf.remove(0);
                    continue;
                }
                self.buf.drain(..HEADER + n + 2);
                return Ok(Some((f[2], f[3], f[HEADER..HEADER + n].to_vec())));
            }
        }
        Ok(None)
    }

    fn seconds(&self) -> String {
        if self.timeout == self.timeout.floor() {
            format!("{}", self.timeout as i64)
        } else {
            format!("{}", self.timeout)
        }
    }

    /// (status, data) of one command; DebugLinkError on an error status or no answer.
    pub fn request(&mut self, cmd: u8, payload: &[u8]) -> ToolResult<(u8, Vec<u8>)> {
        if payload.len() > REQUEST_MAX {
            return Err(link_error(format!("request payload {} > {REQUEST_MAX}", payload.len()), None));
        }
        self.seq = self.seq.wrapping_add(1);
        let s = self.seq;
        self.port.write(&build(cmd, s, payload))?;
        let deadline = self.clock.monotonic() + self.timeout;
        let data = loop {
            let Some((rc, rs, data)) = self.read_frame(deadline)? else {
                return Err(link_error(
                    format!(
                        "no answer to command 0x{cmd:02X} within {} s on {} (firmware without the debug link? another program has the port?)",
                        self.seconds(),
                        self.port_name
                    ),
                    None,
                ));
            };
            if rs != s {
                continue; // a late answer to an earlier request
            }
            if rc == CMD_ERROR {
                let est = data.first().copied();
                return Err(link_error(format!("command 0x{cmd:02X}: {}", status_text(est)), est));
            }
            if rc == cmd | 0x80 {
                break data;
            }
        };
        let st = data.first().copied();
        if st != Some(STATUS_OK) {
            return Err(link_error(format!("command 0x{cmd:02X}: {}", status_text(st)), st));
        }
        Ok((0, data[1..].to_vec()))
    }
}

/// Field and state names of the native modem status (pc/radiotools/native_modem.py).
pub const NATIVE_FIELDS: [&str; 37] = [
    "version",
    "state",
    "reason",
    "session",
    "owned",
    "mode",
    "frequency_hz",
    "blocks",
    "duration_ms",
    "lease_ms",
    "rx_count",
    "tx_count",
    "rx_seen",
    "rx_saved",
    "rx_dropped",
    "rx_read",
    "tx_loaded",
    "tx_sent",
    "phy_irqs",
    "tx_slot_irqs",
    "rx_slot_irqs",
    "rf_irqs",
    "abnormal_irqs",
    "duplicate_irqs",
    "ignored_irqs",
    "pa_keys",
    "quiets",
    "faults",
    "rejected",
    "max_gap_us",
    "last_gap_us",
    "restore_failures",
    "started_us",
    "boundary_us",
    "p25_sync",
    "key_requested",
    "keyed",
];
pub fn native_fields() -> &'static [&'static str] {
    &NATIVE_FIELDS
}
pub const NATIVE_STATES: [&str; 7] = ["idle", "armed", "receive", "transmit", "draining", "stopped", "fault"];
pub const NATIVE_REASONS: [&str; 18] = [
    "ok",
    "user_stop",
    "rx_limit",
    "tx_complete",
    "rx_duration",
    "lease_expired",
    "deadline",
    "stalled",
    "late_boundary",
    "early_boundary",
    "underrun",
    "io",
    "rx_overflow",
    "abnormal_slot",
    "denied",
    "interrupted",
    "restore_failed",
    "unexpected_rf",
];
/// The P25 zone scan counters (plus_txdiag.SCAN_FIELDS).
pub const SCAN_FIELDS: [&str; 13] = [
    "version", "stops", "carriers", "cooled", "takes", "synced", "voiced", "ended", "tails", "quiet", "foreign",
    "sync_ms", "voice_ms",
];

/// plus_txdiag --health sends only these: GET_VIEW tasks (20), crash (21), reset (23).
pub const HEALTH_ALLOWED: [(u8, u8); 3] = [(0x09, 0x14), (0x09, 0x15), (0x09, 0x17)];
/// plus_live.ALLOWED: the health views, GET_VIEW core (0), native status (0x21/0), scan counters (0x21/12).
pub const LIVE_ALLOWED: [(u8, u8); 6] =
    [(0x09, 0x00), (0x09, 0x14), (0x09, 0x15), (0x09, 0x17), (0x21, 0x00), (0x21, 0x0C)];
pub const MAX_SILENT_POLLS: u32 = 3;
/// The search log (firmware tools/plus_searchlog.py, tools/plus_alldigital.py) sends only these: GET_VIEW p25s (28),
/// dmrs (29) and alls (30).
pub const SEARCHLOG_ALLOWED: [(u8, u8); 3] = [
    (0x09, crate::radio::searchlog::SECTION_P25),
    (0x09, crate::radio::searchlog::SECTION_DMR),
    (0x09, crate::radio::searchlog::SECTION_ALL),
];
/// The search log runs all night: a radio that does not answer at all for this long (seconds: a menu, a restart, the
/// cable out) ends the run, or with --reconnect closes the port and waits for the radio (SearchLogSilentSeconds).
pub const SEARCHLOG_SILENT_SECONDS: f64 = 60.0;
/// With --reconnect: the port is tried again this often (seconds) while the radio is away.
pub const SEARCHLOG_RETRY_SECONDS: f64 = 5.0;
/// Polls in a row that the radio answered without any search log section (a firmware before v1.0.0-beta.7, Classic,
/// or one built without the searches) before the run ends with exit 4.
pub const SEARCHLOG_MISSING_POLLS: u32 = 3;
/// The line the reader prints before it ends with exit 4.
pub const SEARCHLOG_MISSING_LINE: &str = "NO_SEARCHLOG: this radio's firmware has no search log (needs v1.0.0-beta.7)";
/// The line printed while the radio is away (--reconnect): "WAITING: COM14 ...".
pub const SEARCHLOG_WAITING: &str = "WAITING:";

/// bytes.decode('ascii', 'replace')
pub fn ascii(b: &[u8]) -> String {
    crate::pytext::ascii_decode(b)
}

fn u32le(d: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]])
}

fn rstrip(s: &str) -> &str {
    s.trim_end_matches(py_isspace)
}

fn open(ctx: &ToolContext<'_>, port_name: &str) -> ToolResult<Box<dyn RawSerial>> {
    if port_name.is_empty() || port_name.contains("://") || port_name.starts_with("udp:") {
        return Err(ToolError::value("Explicit local serial port required"));
    }
    let mut raw = (ctx.serial)(port_name)?;
    raw.open()?;
    raw.set_baud_rate(115200)?;
    raw.set_timeout(0.02);
    Ok(raw)
}

/// plus_txdiag.py --port P --health
pub fn health_tool(argv: &[String], ctx: &ToolContext<'_>) -> ToolResult<()> {
    let a = Args::new(argv, &["--port"], &["--health"], &["--port"])?;
    let port_name = a.get("--port").unwrap().to_string();
    let raw = open(ctx, &port_name)?;
    let mut guard = DebugLinkPort::new(raw, &HEALTH_ALLOWED, "plus_txdiag");
    let r = (|| -> ToolResult<()> {
        let mut link = DebugLinkClient::new(&mut guard, ctx.clock, &port_name);
        for (name, section) in [("tasks", 0x14u8), ("crash", 0x15), ("reset", 0x17)] {
            let (code, data) = link.request(0x09, &[section])?;
            (ctx.print)(&format!("--- view {name} ({section}) status {code}"));
            if code == 0 && data.len() >= 5 {
                (ctx.print)(&format!("uptime_ms={}", u32le(&data, 0)));
                (ctx.print)(rstrip(&ascii(&data[5..])));
            }
        }
        Ok(())
    })();
    let _ = guard.raw.close();
    r
}

/// plus_live.py --port P [--interval 1..5] [--count N]: polls until `cancel` is set (the page's "Durdur") or N polls.
/// Three polls without any answer end the run (SystemExit 3, as the Python tool).
pub fn live_tool(argv: &[String], ctx: &ToolContext<'_>, cancel: &AtomicBool) -> ToolResult<()> {
    let a = Args::new(argv, &["--port", "--interval", "--count"], &[], &["--port"])?;
    let port_name = a.get("--port").unwrap().to_string();
    let interval: f64 = a.get("--interval").unwrap_or("2").trim().parse().map_err(|_| ToolError::exit("2"))?;
    if !(1.0..=5.0).contains(&interval) {
        return Err(ToolError::exit("2"));
    }
    let count: i64 = a.get("--count").unwrap_or("0").trim().parse().map_err(|_| ToolError::exit("2"))?;
    if count < 0 {
        return Err(ToolError::exit("2"));
    }
    let raw = open(ctx, &port_name)?;
    let mut guard = DebugLinkPort::new(raw, &LIVE_ALLOWED, "plus_live");
    let shown = format!("{:.3}", interval).trim_end_matches('0').trim_end_matches('.').to_string();
    (ctx.print)(&format!("LIVE: {port_name} open, read only, every {shown} s"));
    let code = {
        let mut link = DebugLinkClient::new(&mut guard, ctx.clock, &port_name);
        poll(&mut link, interval, count, ctx, cancel)
    };
    let _ = guard.raw.close();
    let code = code?;
    if code != 0 {
        return Err(ToolError::exit(code.to_string()));
    }
    Ok(())
}

/// search log: --port P [--count N] [--reconnect]. Every second the p25s, dmrs and alls sections, printed as the live
/// reader prints a poll, until `cancel` or N polls (SearchLog.cs). A radio silent for SEARCHLOG_SILENT_SECONDS ends the
/// run with exit 3; with --reconnect (the page's all night run) the port is closed instead and opened again every
/// SEARCHLOG_RETRY_SECONDS ("WAITING:" lines) until the radio answers again (a cable pull, a sleep of the PC, a radio
/// restart). A radio that answers without the search log ends it with exit 4 (SEARCHLOG_MISSING_LINE).
pub fn searchlog_tool(argv: &[String], ctx: &ToolContext<'_>, cancel: &AtomicBool) -> ToolResult<()> {
    let a = Args::new(argv, &["--port", "--count"], &["--reconnect"], &["--port"])?;
    let port_name = a.get("--port").unwrap().to_string();
    let count: i64 = a.get("--count").unwrap_or("0").trim().parse().map_err(|_| ToolError::exit("2"))?;
    if count < 0 {
        return Err(ToolError::exit("2"));
    }
    let reconnect = a.has("--reconnect");
    let (mut polls, mut first) = (0i64, true);
    loop {
        let code = match open(ctx, &port_name) {
            Err(e) if reconnect && port_trouble(&e) => {
                (ctx.print)(&format!("error={}", e.record()));
                3
            }
            Err(e) => return Err(e),
            Ok(raw) => {
                let mut guard = DebugLinkPort::new(raw, &SEARCHLOG_ALLOWED, "searchlog");
                (ctx.print)(&if first {
                    format!("SEARCHLOG: {port_name} open, read only, every 1 s")
                } else {
                    format!("SEARCHLOG: {port_name} open again")
                });
                first = false;
                let r = {
                    let mut link = DebugLinkClient::new(&mut guard, ctx.clock, &port_name);
                    poll_search(&mut link, count, &mut polls, ctx, cancel)
                };
                let _ = guard.raw.close(); // a port that went away closes with an error: closed all the same
                match r {
                    Ok(c) => c,
                    Err(e) if reconnect && port_trouble(&e) => {
                        (ctx.print)(&format!("error={}", e.record()));
                        3
                    }
                    Err(e) => return Err(e),
                }
            }
        };
        if code == 4 {
            (ctx.print)(SEARCHLOG_MISSING_LINE);
        }
        if code != 3 || !reconnect || cancel.load(Ordering::SeqCst) || (count != 0 && polls >= count) {
            if code != 0 && !(code == 3 && reconnect) {
                return Err(ToolError::exit(code.to_string()));
            }
            return Ok(());
        }
        (ctx.print)(&format!(
            "{SEARCHLOG_WAITING} {port_name}: no radio; trying again in {SEARCHLOG_RETRY_SECONDS:.0} s"
        ));
        wait(ctx, cancel, SEARCHLOG_RETRY_SECONDS);
        if cancel.load(Ordering::SeqCst) {
            return Ok(());
        }
    }
}

/// A serial port that went away (a cable pull, a sleep of the PC, a new COM number): worth trying again. A refused
/// frame, a missing port factory or radio access switched off are not.
pub fn port_trouble(e: &ToolError) -> bool {
    e.py_type == "OSError"
}

fn wait(ctx: &ToolContext<'_>, cancel: &AtomicBool, seconds: f64) {
    let end = ctx.clock.monotonic() + seconds;
    while !cancel.load(Ordering::SeqCst) {
        let left = end - ctx.clock.monotonic();
        if left <= 0.0 {
            break;
        }
        ctx.clock.sleep(left.min(0.05));
    }
}

/// The polls of one open port: 0 after `count` polls (all of the run, counted in `polls`) or a stop, 3 when the radio
/// was silent for SEARCHLOG_SILENT_SECONDS, 4 when it answered SEARCHLOG_MISSING_POLLS times in a row without the
/// search log.
pub fn poll_search(
    link: &mut DebugLinkClient<'_>,
    count: i64,
    polls: &mut i64,
    ctx: &ToolContext<'_>,
    cancel: &AtomicBool,
) -> ToolResult<i32> {
    use crate::radio::searchlog::{has_search_log, SECTION_ALL, SECTION_DMR, SECTION_P25};
    let mut heard = ctx.clock.monotonic();
    let mut missing = 0u32;
    while (count == 0 || *polls < count) && !cancel.load(Ordering::SeqCst) {
        *polls += 1;
        let n = *polls;
        let started = ctx.clock.monotonic();
        (ctx.print)(&format!("=== poll {n}"));
        let mut text = vec![];
        let codes = [
            print_view_into(link, "view p25s", SECTION_P25, ctx, Some(&mut text))?,
            print_view_into(link, "view dmrs", SECTION_DMR, ctx, Some(&mut text))?,
            print_view_into(link, "view alls", SECTION_ALL, ctx, Some(&mut text))?,
        ];
        (ctx.print)(&format!("=== end {n}"));
        if codes.iter().any(|&c| c != -1) {
            heard = ctx.clock.monotonic();
        } else if ctx.clock.monotonic() - heard >= SEARCHLOG_SILENT_SECONDS {
            (ctx.print)(&format!("No answer from the radio for {SEARCHLOG_SILENT_SECONDS:.0} s; stopping."));
            return Ok(3);
        }
        // answered (ok, unknown command, bad argument: not busy) and no search log in it: an older firmware
        missing = if codes.iter().all(|c| matches!(c, 0..=2)) && !has_search_log(&text) { missing + 1 } else { 0 };
        if missing >= SEARCHLOG_MISSING_POLLS {
            return Ok(4);
        }
        if count == 0 || *polls < count {
            wait(ctx, cancel, (1.0 - (ctx.clock.monotonic() - started)).max(0.0));
        }
    }
    Ok(0)
}

/// plus_live.run on an open (guarded) link: 0 after count polls or a stop, 3 when the radio went silent.
pub fn poll(
    link: &mut DebugLinkClient<'_>,
    interval: f64,
    count: i64,
    ctx: &ToolContext<'_>,
    cancel: &AtomicBool,
) -> ToolResult<i32> {
    print_view(link, "view crash", 0x15, ctx)?;
    print_view(link, "view reset", 0x17, ctx)?;
    let (mut n, mut silent) = (0i64, 0u32);
    while (count == 0 || n < count) && !cancel.load(Ordering::SeqCst) {
        n += 1;
        let started = ctx.clock.monotonic();
        (ctx.print)(&format!("=== poll {n}"));
        let codes = [
            print_view(link, "view core", 0x00, ctx)?,
            print_view(link, "view tasks", 0x14, ctx)?,
            print_native(link, ctx)?,
            print_scan(link, ctx)?,
        ];
        (ctx.print)(&format!("=== end {n}"));
        silent = if codes.iter().all(|&c| c == -1) { silent + 1 } else { 0 };
        if silent >= MAX_SILENT_POLLS {
            (ctx.print)(&format!("No answer from the radio for {silent} polls; stopping."));
            return Ok(3);
        }
        if count == 0 || n < count {
            let end = ctx.clock.monotonic() + (interval - (ctx.clock.monotonic() - started)).max(0.0);
            while !cancel.load(Ordering::SeqCst) {
                let left = end - ctx.clock.monotonic();
                if left <= 0.0 {
                    break;
                }
                ctx.clock.sleep(left.min(0.05));
            }
        }
    }
    Ok(0)
}

/// One query, printed: (status, data). A radio error or no answer is reported, never raised.
fn query(
    link: &mut DebugLinkClient<'_>,
    name: &str,
    cmd: u8,
    arg: u8,
    ctx: &ToolContext<'_>,
) -> ToolResult<(i32, Option<Vec<u8>>)> {
    let label = if cmd != 0x09 { format!("0x{cmd:02x}/{arg}") } else { arg.to_string() };
    match link.request(cmd, &[arg]) {
        Ok((code, data)) => {
            (ctx.print)(&format!("--- {name} ({label}) status {code}"));
            Ok((code as i32, Some(data)))
        }
        Err(e) if e.py_type == "DebugLinkError" => {
            let code = e.status.map_or(-1, |s| s as i32);
            (ctx.print)(&format!("--- {name} ({label}) status {code}"));
            (ctx.print)(&format!("error={}", e.record()));
            Ok((code, None))
        }
        Err(e) => Err(e),
    }
}

fn print_view(link: &mut DebugLinkClient<'_>, name: &str, section: u8, ctx: &ToolContext<'_>) -> ToolResult<i32> {
    print_view_into(link, name, section, ctx, None)
}

/// print_view, the view's text lines also kept in `sink` (the search log's check for its sections).
fn print_view_into(
    link: &mut DebugLinkClient<'_>,
    name: &str,
    section: u8,
    ctx: &ToolContext<'_>,
    sink: Option<&mut Vec<String>>,
) -> ToolResult<i32> {
    let (code, data) = query(link, name, 0x09, section, ctx)?;
    if let Some(d) = data.filter(|d| code == 0 && d.len() >= 5) {
        (ctx.print)(&format!("uptime_ms={}", u32le(&d, 0)));
        let text = ascii(&d[5..]);
        let text = rstrip(&text);
        if !text.is_empty() {
            (ctx.print)(text);
            if let Some(sink) = sink {
                sink.extend(text.split('\n').map(str::to_string));
            }
        }
    }
    Ok(code)
}

fn print_native(link: &mut DebugLinkClient<'_>, ctx: &ToolContext<'_>) -> ToolResult<i32> {
    let (code, data) = query(link, "native", 0x21, 0x00, ctx)?;
    let Some(d) = data.filter(|_| code == 0) else { return Ok(code) };
    let fields = native_fields();
    if d.len() == 4 * fields.len() {
        let v: Vec<u32> = (0..fields.len()).map(|i| u32le(&d, 4 * i)).collect();
        for (f, x) in fields.iter().zip(&v) {
            (ctx.print)(&format!("{f}={x}"));
        }
        if (v[1] as usize) < NATIVE_STATES.len() {
            (ctx.print)(&format!("state_name={}", NATIVE_STATES[v[1] as usize]));
        }
        if (v[2] as usize) < NATIVE_REASONS.len() {
            (ctx.print)(&format!("reason_name={}", NATIVE_REASONS[v[2] as usize]));
        }
    } else {
        (ctx.print)(&format!("raw={}", hex::encode(&d)));
    }
    Ok(code)
}

fn print_scan(link: &mut DebugLinkClient<'_>, ctx: &ToolContext<'_>) -> ToolResult<i32> {
    let (code, data) = query(link, "scan", 0x21, 0x0C, ctx)?;
    let Some(d) = data.filter(|_| code == 0) else { return Ok(code) };
    if d.len() == 4 * SCAN_FIELDS.len() {
        for (i, f) in SCAN_FIELDS.iter().enumerate() {
            (ctx.print)(&format!("{f}={}", u32le(&d, 4 * i)));
        }
    } else {
        (ctx.print)(&format!("raw={}", hex::encode(&d)));
    }
    Ok(code)
}
