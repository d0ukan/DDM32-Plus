// SPDX-License-Identifier: GPL-3.0-or-later
//! The search log (SearchLog.cs; firmware tools/plus_searchlog.py, docs/plus.md "Search log on the PC"): the radio's
//! P25, DMR and all digital searches keep 8 finds each; read every second over the debug link (GET_VIEW sections p25s =
//! 28, dmrs = 29, alls = 30, read only, not counted as a look). ONE line per mode + frequency + code + group with a
//! call counter, counted by the radio's own entries (searchlog_view.c): each entry has a serial (ID) and a call
//! counter; the log follows an entry by engine (the P25 search, or the DMR search's engine that the all digital search
//! shares) + ID + frequency + mode and adds the change of its counter to the entry's line. A call the radio moves to
//! its right talkgroup (one entry down, another up) is one call; an entry whose code / group is filled in later moves
//! its calls to the right line. X.seq (the engine's calls since power on) or the radio's uptime going back is a
//! restart: the entries start anew, nothing counted is lost. Calls counted on entries the log never saw are `missed`.

use std::collections::{HashMap, HashSet};
use std::io::Write;
use std::path::{Path, PathBuf};

pub const SECTION_P25: u8 = 0x1C;
pub const SECTION_DMR: u8 = 0x1D;
pub const SECTION_ALL: u8 = 0x1E;
pub const CSV_COLUMNS: [&str; 11] = [
    "first_heard",
    "last_heard",
    "mode",
    "mhz",
    "code",
    "slot",
    "group",
    "last_source",
    "caller",
    "encrypted",
    "calls",
];
/// An entry not seen for this many polls is forgotten (it left the radio's list).
pub const FORGET_POLLS: u64 = 10;

/// A local time as seconds since the Unix epoch plus the offset the program shows (the GUI passes the local clock).
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct Stamp(pub i64);

/// One entry of a radio search's found list as one poll shows it (no audio). mode: "P25", "DMR", "YSF" or "NXDN";
/// code: P25 NAC ("$1C8/456"), DMR colour code ("CC1"), YSF DG-ID ("DG0") or NXDN RAN ("RAN1"); slot: DMR slots seen
/// and RPT / DIR; group: P25 / NXDN talkgroup or DMR talkgroup / called ID; caller: the YSF source callsign (empty
/// otherwise); calls: the radio's call counter of the entry; id: its serial (stays while code / group are filled in).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SearchCall {
    pub time: Stamp,
    pub mode: &'static str,
    pub hz: i64,
    pub code: String,
    pub slot: String,
    pub group: i64,
    pub source: i64,
    pub encrypted: bool,
    pub calls: i64,
    pub caller: String,
    pub id: i64,
}

/// One line of the log: every call heard on one mode + frequency + code (NAC / CC / DG-ID / RAN) + group, counted
/// (owner 2026-11-21). first / last: when it was first and last heard; source / caller / slot: the last call's.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SearchGroup {
    pub mode: &'static str,
    pub hz: i64,
    pub code: String,
    pub group: i64,
    pub first: Stamp,
    pub last: Stamp,
    pub slot: String,
    pub source: i64,
    pub caller: String,
    pub encrypted: bool,
    pub calls: i64,
}

impl SearchGroup {
    pub fn calls(&self) -> i64 {
        self.calls
    }
}

type GroupKey = (&'static str, i64, String, i64);
/// An entry: engine + serial + mode + frequency (a serial wraps; a hot entry may outlive its number's next use).
type EntryKey = (char, i64, &'static str, i64);

#[derive(Debug)]
struct Entry {
    key: Option<GroupKey>, // its key at the last poll
    row: Option<GroupKey>, // its line (None: none yet, or its calls went back to 0)
    first: Stamp,          // when it was first seen
    calls: i64,            // the radio's counter at the last poll
    counted: i64,          // what this log has counted of it (on row)
    seen: u64,
}

/// The search log (SearchLog.cs): ONE line per mode + frequency + code + group with a call counter, counted by the
/// radio's entries and its call sequence (module comment).
#[derive(Debug, Default)]
pub struct SearchLogBook {
    groups: HashMap<GroupKey, SearchGroup>,
    entries: HashMap<EntryKey, Entry>,
    seqs: HashMap<char, i64>,
    balance: HashMap<char, i64>,
    debt: HashMap<char, i64>,
    uptime: Option<i64>,
    poll: u64,
    pub p25_on: Option<bool>,
    pub dmr_on: Option<bool>,
    pub all_on: Option<bool>,
    pub p25_at_hz: Option<i64>,
    pub dmr_at_hz: Option<i64>,
    pub all_at_hz: Option<i64>,
    /// When each search was seen to stop (it ran at the poll before; None while it runs or never ran).
    pub p25_stopped: Option<Stamp>,
    pub dmr_stopped: Option<Stamp>,
    pub all_stopped: Option<Stamp>,
    /// Radio restarts seen (uptime or a search's call sequence went back).
    pub restarts: u32,
}

fn fields(text: &str, n: usize) -> Option<Vec<i64>> {
    let parts: Vec<&str> = text.split(',').collect();
    if parts.len() < n {
        return None;
    }
    parts[..n].iter().map(|p| p.trim().parse::<i64>().ok()).collect()
}

/// p25s.fI=HZ,NAC,TG,SRC,CALLS,AGE_S,ID
pub fn parse_p25(text: &str, now: Stamp) -> Option<SearchCall> {
    let v = fields(text, 7)?;
    if v[0] <= 0 {
        return None;
    }
    let age = v[5].clamp(0, 86400);
    Some(SearchCall {
        time: Stamp(now.0 - age),
        mode: "P25",
        hz: v[0],
        code: if (0..0xFFFF).contains(&v[1]) { nac(v[1]) } else { "$?".to_string() },
        slot: String::new(),
        group: v[2],
        source: v[3],
        encrypted: false,
        calls: v[4],
        caller: String::new(),
        id: v[6],
    })
}

/// dmrs.fI=HZ,CC,TSMASK,SYNCMASK,GROUP,ENC,DST,SRC,CALLS,AGE_S,ID
pub fn parse_dmr(text: &str, now: Stamp) -> Option<SearchCall> {
    let v = fields(text, 11)?;
    if v[0] <= 0 {
        return None;
    }
    let age = v[9].clamp(0, 86400);
    let code = if (0..=15).contains(&v[1]) { format!("CC{}", v[1]) } else { "CC?".to_string() };
    Some(SearchCall {
        time: Stamp(now.0 - age),
        mode: "DMR",
        hz: v[0],
        code,
        slot: slots(v[2], v[3]),
        group: v[6],
        source: v[7],
        encrypted: v[5] != 0,
        calls: v[8],
        caller: String::new(),
        id: v[10],
    })
}

/// DMR slots seen and repeater / direct: "TS2 RPT", "TS1+2", "DIR".
fn slots(ts_mask: i64, sync_mask: i64) -> String {
    let slot = match ts_mask & 3 {
        1 => "TS1",
        2 => "TS2",
        3 => "TS1+2",
        _ => "",
    };
    let tag = if sync_mask & 1 != 0 {
        "RPT"
    } else if sync_mask & 2 != 0 {
        "DIR"
    } else {
        ""
    };
    format!("{slot} {tag}").trim().to_string()
}

/// alls.fI=HZ,MODE,CODE,TSMASK,SYNCMASK,GROUP,ENC,DST,SRC,CALLS,AGE_S,ID,CALL (MODE P25 / DMR / YSF / NXDN; CODE the
/// NAC, colour code, DG-ID or RAN, 65535 not known; CALL the YSF callsign, the last field).
pub fn parse_all(text: &str, now: Stamp) -> Option<SearchCall> {
    let parts: Vec<&str> = text.splitn(13, ',').collect();
    if parts.len() < 12 {
        return None;
    }
    let mode: &'static str = match parts[1].trim() {
        "P25" => "P25",
        "DMR" => "DMR",
        "YSF" => "YSF",
        "NXDN" => "NXDN",
        _ => return None,
    };
    let nums: Vec<&str> = std::iter::once(parts[0]).chain(parts[2..12].iter().copied()).collect();
    let v = fields(&nums.join(","), 11)?;
    if v[0] <= 0 {
        return None;
    }
    let age = v[9].clamp(0, 86400);
    let known = (0..65535).contains(&v[1]);
    let code = match mode {
        "P25" if known => nac(v[1]),
        "P25" => "$?".to_string(),
        "DMR" if (0..=15).contains(&v[1]) => format!("CC{}", v[1]),
        "DMR" => "CC?".to_string(),
        "YSF" if known => format!("DG{}", v[1]),
        "YSF" => "DG?".to_string(),
        _ if known => format!("RAN{}", v[1]),
        _ => "RAN?".to_string(),
    };
    Some(SearchCall {
        time: Stamp(now.0 - age),
        mode,
        hz: v[0],
        code,
        slot: if mode == "DMR" { slots(v[2], v[3]) } else { String::new() },
        group: v[6],
        source: v[7],
        encrypted: v[5] != 0,
        calls: v[8],
        caller: parts.get(12).map(|c| c.trim().to_string()).unwrap_or_default(),
        id: v[10],
    })
}

/// Whether one poll's lines come from a firmware with the search log (beta.7 and later: an X.seq line).
pub fn has_search_log<S: AsRef<str>>(lines: &[S]) -> bool {
    lines.iter().any(|l| {
        let s = l.as_ref().trim_start();
        s.starts_with("p25s.seq=") || s.starts_with("dmrs.seq=") || s.starts_with("alls.seq=")
    })
}

fn stopped(before: Option<bool>, now: Option<bool>, at: Option<Stamp>, t: Stamp) -> Option<Stamp> {
    match now {
        Some(true) => None,
        Some(false) if before == Some(true) => Some(t),
        _ => at,
    }
}

impl SearchLogBook {
    pub fn new() -> Self {
        Self::default()
    }

    /// Every line, the most recently heard first.
    pub fn groups(&self) -> Vec<SearchGroup> {
        let mut v: Vec<SearchGroup> = self.groups.values().cloned().collect();
        v.sort_by(|a, b| b.last.cmp(&a.last).then(a.hz.cmp(&b.hz)).then(a.group.cmp(&b.group)));
        v
    }

    /// Distinct frequencies with a call.
    pub fn frequencies(&self) -> usize {
        let mut v: Vec<(&str, i64)> = self.groups.values().map(|g| (g.mode, g.hz)).collect();
        v.sort();
        v.dedup();
        v.len()
    }

    /// Every call counted.
    pub fn total_calls(&self) -> i64 {
        self.groups.values().map(SearchGroup::calls).sum()
    }

    /// Calls the radio counted that no entry showed (it found more than 8 between two polls).
    pub fn missed(&self) -> i64 {
        self.balance.values().filter(|b| **b > 0).sum()
    }

    /// "Kayıtları temizle": the lines go; the radio's entries stay known, so only calls heard from now on count.
    pub fn clear(&mut self) {
        self.groups.clear();
        for e in self.entries.values_mut() {
            e.row = None;
            e.counted = 0;
        }
        self.balance.clear();
        self.debt.clear();
    }

    /// One poll's lines (all three sections, with the reader's uptime_ms lines); true when a line was added, changed
    /// or removed.
    pub fn add<S: AsRef<str>>(&mut self, lines: &[S], now: Stamp) -> bool {
        let mut kv: HashMap<&str, &str> = HashMap::new();
        for l in lines {
            let l = l.as_ref().trim();
            if let Some((k, v)) = l.split_once('=') {
                if !k.is_empty() {
                    kv.insert(k, v);
                }
            }
        }
        let flag = |k: &str| kv.get(k).map(|v| *v == "1");
        let num = |k: &str| kv.get(k).and_then(|v| v.parse::<i64>().ok());
        let (p, d, a) = (flag("p25s.on"), flag("dmrs.on"), flag("alls.on"));
        self.p25_stopped = stopped(self.p25_on, p, self.p25_stopped, now);
        self.dmr_stopped = stopped(self.dmr_on, d, self.dmr_stopped, now);
        self.all_stopped = stopped(self.all_on, a, self.all_stopped, now);
        (self.p25_on, self.dmr_on, self.all_on) = (p, d, a);
        self.p25_at_hz = num("p25s.at");
        self.dmr_at_hz = num("dmrs.at");
        self.all_at_hz = num("alls.at");
        self.poll += 1;

        let mut seq_now: Vec<(char, i64)> = vec![];
        if let Some(s) = num("p25s.seq") {
            seq_now.push(('P', s & 0xFFFF));
        }
        if let Some(s) = num("dmrs.seq").or_else(|| num("alls.seq")) {
            seq_now.push(('D', s & 0xFFFF));
        }
        let up = num("uptime_ms");
        // a restart: the radio's uptime went back (not its 49.7 day wrap), or a search's call sequence went back
        let mut restart =
            matches!((up, self.uptime), (Some(u), Some(u0)) if u < u0 && !(u0 > 0xF000_0000 && u < 0x1000_0000));
        for (eng, s) in &seq_now {
            if let Some(s0) = self.seqs.get(eng) {
                if s != s0 && ((s - s0) & 0xFFFF) >= 0x8000 {
                    restart = true;
                }
            }
        }
        if restart {
            self.entries.clear();
            self.seqs.clear();
            self.restarts += 1;
        }
        if up.is_some() {
            self.uptime = up;
        }

        let mut changed = false;
        let mut delta: HashMap<char, i64> = HashMap::new();
        let mut seen: HashSet<EntryKey> = HashSet::new();
        let mut calls: Vec<(char, SearchCall)> = vec![];
        for i in 0..8 {
            let found = [
                ('P', kv.get(format!("p25s.f{i}").as_str()).and_then(|t| parse_p25(t, now))),
                ('D', kv.get(format!("dmrs.f{i}").as_str()).and_then(|t| parse_dmr(t, now))),
                ('D', kv.get(format!("alls.f{i}").as_str()).and_then(|t| parse_all(t, now))),
            ];
            for (eng, c) in found {
                // the same entry twice in one poll (two entries of one serial, frequency and mode): the newest only
                if let Some(c) = c.filter(|c| c.id >= 0 && seen.insert((eng, c.id, c.mode, c.hz))) {
                    calls.push((eng, c));
                }
            }
        }
        // a counter one down is the call the radio moved to another entry when another entry of the engine rose in
        // this poll or the engine counted no new call; otherwise (its serial used again, seq moved) a new entry
        let rising: HashSet<char> = calls
            .iter()
            .filter(|(eng, c)| self.entries.get(&(*eng, c.id, c.mode, c.hz)).map_or(c.calls > 0, |e| c.calls > e.calls))
            .map(|(eng, _)| *eng)
            .collect();
        let counting: HashSet<char> = seq_now
            .iter()
            .filter(|(eng, s)| self.seqs.get(eng).is_some_and(|s0| s0 != s))
            .map(|(eng, _)| *eng)
            .collect();
        for (eng, c) in &calls {
            changed |= self.take(*eng, c, &mut delta, rising.contains(eng) || !counting.contains(eng));
        }
        let poll = self.poll;
        self.entries.retain(|_, e| poll - e.seen <= FORGET_POLLS);
        for (eng, s) in seq_now {
            // never below 0, so it never hides a later real miss; a call the entries showed before seq had it (the
            // radio counted it between printing the two) is owed for one poll only
            if let Some(s0) = self.seqs.get(&eng) {
                let owed = self.debt.get(&eng).copied().unwrap_or(0);
                let raw = self.balance.get(&eng).copied().unwrap_or(0) + owed + ((s - s0) & 0xFFFF)
                    - delta.get(&eng).copied().unwrap_or(0);
                self.balance.insert(eng, raw.max(0));
                self.debt.insert(eng, if raw < 0 && owed == 0 { raw } else { 0 });
            }
            self.seqs.insert(eng, s);
        }
        changed
    }

    /// One entry of a poll into its line; true when a line changed. A line exists only while it has calls: none for
    /// an entry without a call yet, and a line whose calls went back to 0 (its call moved to another talkgroup) goes
    /// once no entry with calls is on it.
    fn take(&mut self, eng: char, c: &SearchCall, delta: &mut HashMap<char, i64>, move_ok: bool) -> bool {
        let id: EntryKey = (eng, c.id, c.mode, c.hz);
        let key: GroupKey = (c.mode, c.hz, c.code.clone(), c.group);
        // the serial used again by a new entry: its key is not a fill-in of the old one's, or its counter went down by
        // more than the one call the radio moves to another talkgroup. The old entry's calls stay on their line.
        let known = self.entries.get(&id).is_some_and(|e| {
            (c.calls >= e.calls || (c.calls == e.calls - 1 && move_ok))
                && e.key.as_ref().is_none_or(|k| *k == key || fill_in(k, &key))
        });
        if !known {
            if c.calls == 0 && c.group == 0 && c.source == 0 && c.caller.is_empty() {
                return false; // nothing known yet: taken when it has something
            }
            self.entries.insert(id, Entry { key: None, row: None, first: c.time, calls: 0, counted: 0, seen: 0 });
        }
        let poll = self.poll;
        let (row, prev_calls, prev_counted, mut first) = {
            let e = self.entries.get_mut(&id).unwrap();
            e.seen = poll;
            e.key = Some(key.clone());
            (e.row.clone(), e.calls, e.counted, e.first)
        };
        let d = c.calls - prev_calls;
        *delta.entry(eng).or_default() += d;
        let counted = (prev_counted + d).max(0);
        let mut changed = false;
        let mut on_line = row.clone();
        if let Some(old) = row.filter(|old| *old != key || counted == 0) {
            // the radio filled in the code / group, or the entry's last call moved away: it leaves its line
            let others = self.entries.iter().any(|(k, o)| *k != id && o.row.as_ref() == Some(&old) && o.counted > 0);
            if let Some(og) = self.groups.get_mut(&old) {
                og.calls -= prev_counted;
                first = first.min(og.first); // heard first under the half known key
                if og.calls <= 0 && !others {
                    self.groups.remove(&old);
                    for o in self.entries.values_mut().filter(|o| o.row.as_ref() == Some(&old)) {
                        o.row = None;
                    }
                }
            }
            on_line = None;
            changed = true;
        }
        let e = self.entries.get_mut(&id).unwrap();
        e.calls = c.calls;
        e.first = first;
        if counted == 0 {
            e.row = None;
            e.counted = 0;
            return changed; // no line until it has a call
        }
        let was = if on_line.is_some() { prev_counted } else { 0 };
        e.row = Some(key.clone());
        e.counted = counted;
        let g = self.groups.entry(key).or_insert_with(|| SearchGroup {
            mode: c.mode,
            hz: c.hz,
            code: c.code.clone(),
            group: c.group,
            first,
            last: c.time,
            slot: c.slot.clone(),
            source: c.source,
            caller: c.caller.clone(),
            encrypted: c.encrypted,
            calls: 0,
        });
        if counted != was || on_line.is_none() {
            g.calls += counted - was;
            g.first = g.first.min(first);
            changed = true;
        }
        if c.time > g.last
            || c.source != g.source
            || c.caller != g.caller
            || (c.encrypted && !g.encrypted)
            || (!c.slot.is_empty() && c.slot != g.slot)
        {
            g.last = g.last.max(c.time);
            g.first = g.first.min(c.time);
            g.source = c.source;
            g.caller = c.caller.clone();
            if !c.slot.is_empty() {
                g.slot = c.slot.clone();
            }
            g.encrypted |= c.encrypted;
            changed = true;
        }
        changed
    }
}

/// A code the radio has not named yet ("CC?", "$?", "DG?", "RAN?").
fn unknown(code: &str) -> bool {
    code.ends_with('?')
}

/// The radio filled in an entry's code and / or group: the old key had an unknown field and the known ones agree
/// (anything else is another entry).
fn fill_in(old: &GroupKey, now: &GroupKey) -> bool {
    old.0 == now.0
        && old.1 == now.1
        && (unknown(&old.2) || old.3 == 0)
        && (old.2 == now.2 || unknown(&old.2))
        && (old.3 == now.3 || old.3 == 0)
}

/// The local clock as a Stamp (seconds of local time since 1970-01-01 00:00 local).
pub fn local_stamp() -> Stamp {
    Stamp(chrono::Local::now().naive_local().and_utc().timestamp())
}

/// A P25 NAC in both forms the radio shows: "$659/1625" (hex, decimal; owner 2026-11-21).
pub fn nac(v: i64) -> String {
    format!("${v:03X}/{v}")
}

pub fn mhz(hz: i64) -> String {
    format!("{:.4}", hz as f64 / 1e6)
}

/// MHz with the given decimal separator (the CSV's: "433,5000" for Turkish Excel).
pub fn mhz_with(hz: i64, dec: char) -> String {
    mhz(hz).replace('.', &dec.to_string())
}

/// "2026-11-21 22:00:00" from a local Stamp.
pub fn format_time(t: Stamp) -> String {
    let days = t.0.div_euclid(86400);
    let secs = t.0.rem_euclid(86400);
    let (y, m, d) = civil(days);
    format!("{y:04}-{m:02}-{d:02} {:02}:{:02}:{:02}", secs / 3600, secs / 60 % 60, secs % 60)
}

/// Days since 1970-01-01 to (year, month, day) (H. Hinnant's civil_from_days).
fn civil(z: i64) -> (i64, i64, i64) {
    let z = z + 719468;
    let era = z.div_euclid(146097);
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    (if m <= 2 { y + 1 } else { y }, m, d)
}

/// One CSV cell: text Excel would run as a formula (starting with = + - @, a tab or CR) gets a leading apostrophe; a
/// cell holding the separator, a quote or a line break is quoted.
pub fn csv_cell(value: &str, sep: char) -> String {
    let mut v = value.to_string();
    if v.starts_with(['=', '+', '-', '@', '\t', '\r']) {
        v.insert(0, '\'');
    }
    if v.contains([sep, '"', '\n', '\r']) {
        format!("\"{}\"", v.replace('"', "\"\""))
    } else {
        v
    }
}

pub fn csv_header(sep: char) -> String {
    CSV_COLUMNS.join(&sep.to_string())
}

pub fn csv_row(g: &SearchGroup, sep: char, dec: char) -> String {
    [
        format_time(g.first),
        format_time(g.last),
        g.mode.to_string(),
        mhz_with(g.hz, dec),
        g.code.clone(),
        g.slot.clone(),
        g.group.to_string(),
        g.source.to_string(),
        g.caller.clone(),
        if g.encrypted { "1" } else { "0" }.to_string(),
        g.calls().to_string(),
    ]
    .iter()
    .map(|c| csv_cell(c, sep))
    .collect::<Vec<_>>()
    .join(&sep.to_string())
}

/// The log's file for a session started at `started`: folder/search-YYYYMMDD-HHMMSS.csv, never one that exists (a
/// "-2", "-3" ... after the name then). One file per session: a program restart never overwrites an earlier session's
/// file, and a run past midnight stays in the file it began in.
pub fn session_path(folder: &Path, started: Stamp) -> PathBuf {
    let t = format_time(started);
    let name = format!("search-{}-{}", t[..10].replace('-', ""), t[11..].replace(':', ""));
    let mut path = folder.join(format!("{name}.csv"));
    let mut n = 2;
    while path.exists() || side_path(&path).exists() {
        path = folder.join(format!("{name}-{n}.csv"));
        n += 1;
    }
    path
}

/// Where the rows go while another program (Excel) holds the log's file: name-open.csv beside it.
pub fn side_path(path: &Path) -> PathBuf {
    let stem = path.file_stem().map(|s| s.to_string_lossy().to_string()).unwrap_or_default();
    path.with_file_name(format!("{stem}-open.csv"))
}

/// What `write` did: the file written, and whether the log's own file was held by another program (Excel) so the rows
/// went to `side_path` instead.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CsvWritten {
    pub path: PathBuf,
    pub locked: bool,
}

/// Writes every line of the log to `path` (UTF-8 with a BOM, so Excel reads the Turkish letters; `sep` between the
/// cells), whole, through a temporary file, so a crash never leaves half a file. While another program holds the file
/// (Excel locks an open CSV on Windows) the rows go to `side_path`; once the file is free again it is written and the
/// side file goes.
pub fn write(path: &Path, lines: &[SearchGroup], sep: char, dec: char) -> std::io::Result<CsvWritten> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let mut text = String::from("\u{feff}");
    text.push_str(&csv_header(sep));
    text.push_str("\r\n");
    for g in lines {
        text.push_str(&csv_row(g, sep, dec));
        text.push_str("\r\n");
    }
    let tmp = path.with_extension("csv.tmp");
    {
        let mut f = std::fs::File::create(&tmp)?;
        f.write_all(text.as_bytes())?;
        f.sync_all()?;
    }
    let side = side_path(path);
    if std::fs::rename(&tmp, path).is_err() {
        std::fs::rename(&tmp, &side)?; // held too: the error goes to the caller
        return Ok(CsvWritten { path: side, locked: true });
    }
    if side.exists() {
        let _ = std::fs::remove_file(&side); // still open in Excel: it stays, the log's own file is whole
    }
    Ok(CsvWritten { path: path.to_path_buf(), locked: false })
}

/// The CSV separator Excel expects here: Windows' list separator (";" in Turkish, "," in English), else ",".
pub fn list_separator() -> char {
    crate::sys::list_separator().filter(|c| matches!(c, ',' | ';' | '\t')).unwrap_or(',')
}

/// The CSV's separator and decimal separator for Excel: the list separator and, with it, the decimal separator of
/// the regional settings (";" and "," in Turkish, so 433,5000 is a number); "," and "." otherwise (csv_format_of).
pub fn csv_format() -> (char, char) {
    csv_format_of(list_separator(), crate::sys::decimal_separator())
}

/// csv_format from a list separator and a decimal separator (None: not known).
pub fn csv_format_of(sep: char, dec: Option<char>) -> (char, char) {
    match dec {
        Some(d @ (',' | '.')) if sep != ',' => (sep, d),
        _ => (sep, '.'),
    }
}
