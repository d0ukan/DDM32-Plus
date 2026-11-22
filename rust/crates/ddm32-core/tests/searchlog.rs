// SPDX-License-Identifier: GPL-3.0-or-later
//! The search log (radio::searchlog), held to the C# SearchLogTests: the firmware's p25s / dmrs / alls sections become
//! rows, counted by the radio's own entries (ID) and its call sequence (seq); the reader on a fake radio.
use ddm32_core::app_info;
use ddm32_core::radio::codec::Codec;
use ddm32_core::radio::debug::{
    build, port_trouble, searchlog_tool, CMD_ERROR, SEARCHLOG_ALLOWED, SEARCHLOG_MISSING_LINE, SEARCHLOG_MISSING_POLLS,
    SEARCHLOG_SILENT_SECONDS, SEARCHLOG_WAITING,
};
use ddm32_core::radio::port::{Clock, RadioPort, RawSerial, VirtualClock};
use ddm32_core::radio::searchlog::{
    self, has_search_log, parse_all, parse_dmr, parse_p25, CsvWritten, SearchGroup, SearchLogBook, Stamp,
};
use ddm32_core::radio::tools::ToolContext;
use ddm32_core::radio::{ToolError, ToolResult};
use std::cell::RefCell;
use std::collections::VecDeque;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};

// 2026-11-21 22:00:00 local
const T0: Stamp = Stamp(1_795_298_400);

fn at(s: i64) -> Stamp {
    Stamp(T0.0 + s)
}

fn poll(pseq: i64, p25: &str, dseq: i64, dmr: &str, uptime: i64) -> Vec<String> {
    vec![
        "--- view p25s (28) status 0".into(),
        format!("uptime_ms={uptime}"),
        "p25s.on=1".into(),
        "p25s.phase=1".into(),
        "p25s.at=433512500".into(),
        "p25s.n=1".into(),
        format!("p25s.seq={pseq}"),
        p25.into(),
        "--- view dmrs (29) status 0".into(),
        format!("uptime_ms={}", uptime + 1),
        "dmrs.on=1".into(),
        "dmrs.phase=1".into(),
        "dmrs.at=438000000".into(),
        "dmrs.n=1".into(),
        format!("dmrs.seq={dseq}"),
        dmr.into(),
    ]
}

fn p(pseq: i64, p25: &str) -> Vec<String> {
    poll(pseq, p25, 0, "", 100000)
}

fn line(b: &SearchLogBook, group: i64) -> SearchGroup {
    let g: Vec<SearchGroup> = b.groups().into_iter().filter(|g| g.group == group).collect();
    assert_eq!(1, g.len(), "one line of group {group}");
    g[0].clone()
}

#[test]
fn fields_are_parsed() {
    assert_eq!("2026-11-21 22:00:00", searchlog::format_time(T0));
    let p = parse_p25("433500000,456,655,1234567,3,10,7", T0).unwrap();
    assert_eq!(
        ("P25", 433500000, "$1C8/456", 655, 1234567, 3, 7),
        (p.mode, p.hz, p.code.as_str(), p.group, p.source, p.calls, p.id)
    );
    assert_eq!(Stamp(T0.0 - 10), p.time);
    let d = parse_dmr("438612500,1,2,1,1,0,2620,2620123,4,0,63", T0).unwrap();
    assert_eq!(
        ("DMR", "CC1", "TS2 RPT", 2620, 2620123, false, 63),
        (d.mode, d.code.as_str(), d.slot.as_str(), d.group, d.source, d.encrypted, d.id)
    );
    assert_eq!("CC?", parse_dmr("438612500,255,0,2,1,1,0,0,0,0,1", T0).unwrap().code);
    assert!(parse_p25("garbage", T0).is_none());
    assert!(parse_dmr("1,2,3", T0).is_none());
    // the format before the entry serial (an internal build): not read
    assert!(parse_p25("433500000,456,655,1234567,3,10", T0).is_none());
    assert!(parse_dmr("438612500,1,2,1,1,0,2620,2620123,4,0", T0).is_none());
}

#[test]
fn one_line_per_frequency_code_and_group_with_a_counter() {
    let mut b = SearchLogBook::new();
    assert!(b.add(
        &poll(
            1,
            "p25s.f0=433500000,456,655,1234567,1,30,1",
            2,
            "dmrs.f0=438612500,1,1,1,1,0,2620,2620123,2,5,1",
            100000
        ),
        T0
    ));
    let g = b.groups();
    assert_eq!(2, g.len());
    assert_eq!("DMR", g[0].mode); // heard last comes first
    assert_eq!((1, 2), (g[1].calls(), g[0].calls()));
    assert_eq!(Some(true), b.p25_on);
    assert_eq!(Some(433512500), b.p25_at_hz);
    let same = poll(
        1,
        "p25s.f0=433500000,456,655,1234567,1,31,1",
        2,
        "dmrs.f0=438612500,1,1,1,1,0,2620,2620123,2,6,1",
        100000,
    );
    assert!(!b.add(&same, at(1)));
    // a new call on the same frequency, NAC and group: the same line, counted, the last source kept
    assert!(b.add(&poll(2, "p25s.f0=433500000,456,655,7654321,2,0,1", 2, "", 100000), at(2)));
    let p = b.groups().into_iter().find(|g| g.mode == "P25").unwrap();
    assert_eq!((2, 7654321, Stamp(T0.0 - 30), at(2)), (p.calls(), p.source, p.first, p.last));
    // another talkgroup on that frequency: its own line (another entry)
    assert!(b.add(&poll(5, "p25s.f0=433500000,456,101,1111,3,0,2", 2, "", 100000), at(3)));
    assert_eq!(3, b.groups().len());
    assert_eq!(2, b.frequencies());
    assert_eq!(3, line(&b, 101).calls());
    assert_eq!(2 + 2 + 3, b.total_calls());
    assert_eq!(0, b.missed());
    assert!(!b.add::<String>(&[], at(11)));
    assert_eq!(None, b.dmr_on);
}

#[test]
fn a_call_the_radio_moves_is_one_call() {
    // A2: the radio counts a call on TG 2 (link control late), then moves it to TG 1: 6 then 5 is 5, not 11
    let mut b = SearchLogBook::new();
    b.add(&p(9, "p25s.f0=433500000,456,2,5,5,0,3"), T0);
    b.add(&p(10, "p25s.f0=433500000,456,2,5,6,0,3"), at(1));
    assert_eq!(6, line(&b, 2).calls());
    let mut moved = p(10, "p25s.f0=433500000,456,1,9,1,0,4");
    moved.push("p25s.f1=433500000,456,2,5,5,1,3".into());
    assert!(b.add(&moved, at(2)));
    assert_eq!((5, 1), (line(&b, 2).calls(), line(&b, 1).calls()));
    assert_eq!((6, 0), (b.total_calls(), b.missed()));
}

#[test]
fn an_entry_whose_group_is_filled_in_moves_its_line() {
    let mut b = SearchLogBook::new();
    assert!(b.add(&p(1, "p25s.f0=433500000,456,0,0,1,0,8"), T0));
    assert_eq!(0, b.groups()[0].group);
    assert!(b.add(&p(1, "p25s.f0=433500000,456,655,1234,1,0,8"), at(1)));
    let g = b.groups();
    assert_eq!(1, g.len());
    assert_eq!((655, 1, 1234, T0), (g[0].group, g[0].calls(), g[0].source, g[0].first));
    assert!(b.add(&poll(1, "", 1, "dmrs.f0=438612500,255,0,1,1,0,0,0,1,0,5", 100000), at(2)));
    assert!(b.add(&poll(1, "", 1, "dmrs.f0=438612500,3,2,1,1,0,91,2860001,1,0,5", 100000), at(3)));
    let d = b.groups().into_iter().find(|g| g.mode == "DMR").unwrap();
    assert_eq!(("CC3", 91, 1), (d.code.as_str(), d.group, d.calls()));
    assert_eq!(2, b.groups().len());
}

#[test]
fn a_radio_restart_loses_nothing_and_counts_nothing_twice() {
    let mut b = SearchLogBook::new();
    b.add(&poll(4, "p25s.f0=433500000,456,101,1111,4,0,12", 0, "", 500000), T0);
    assert!(b.add(&poll(1, "p25s.f0=433500000,456,101,2222,1,0,12", 0, "", 3000), at(10)));
    assert_eq!((5, 1), (line(&b, 101).calls(), b.restarts));
    assert!(!b.add(&poll(1, "p25s.f0=433500000,456,101,2222,1,1,12", 0, "", 4000), at(11)));
    // uptime wrapping after 49.7 days is not a restart; seq wrapping at 65536 is not either
    assert!(!b.add(&poll(1, "p25s.f0=433500000,456,101,2222,1,2,12", 0, "", 0xFFFF_F000), at(12)));
    assert!(!b.add(&poll(1, "p25s.f0=433500000,456,101,2222,1,3,12", 0, "", 1000), at(13)));
    assert_eq!((1, 5), (b.restarts, line(&b, 101).calls()));
    let mut w = SearchLogBook::new();
    w.add(&p(65535, "p25s.f0=433500000,456,101,1,7,0,9"), T0);
    w.add(&p(1, "p25s.f0=433500000,456,101,1,9,0,9"), at(1));
    assert_eq!((0, 9, 0), (w.restarts, line(&w, 101).calls(), w.missed()));
}

#[test]
fn both_lists_and_a_second_search_are_not_counted_twice() {
    let mut b = SearchLogBook::new();
    let first = [
        "p25s.on=0",
        "p25s.seq=2",
        "p25s.f0=433500000,456,655,1234567,2,30,1",
        "alls.on=1",
        "alls.mode=P25",
        "alls.at=433512500",
        "alls.n=2",
        "alls.seq=2",
        "alls.f0=433500000,P25,456,0,0,1,0,655,1234567,1,0,1,",
        "alls.f1=439100000,YSF,0,0,0,0,0,0,0,1,4,2,TA2DOU",
    ];
    assert!(b.add(&first, T0));
    assert_eq!(2, b.groups().len());
    assert_eq!((Some(true), Some(false), Some(433512500)), (b.all_on, b.p25_on, b.all_at_hz));
    // two searches found the same key: two different sets of calls, added
    assert_eq!(3, b.groups().iter().find(|g| g.mode == "P25").unwrap().calls());
    let same = [
        "p25s.seq=2",
        "p25s.f0=433500000,456,655,1234567,2,31,1",
        "alls.on=1",
        "alls.seq=2",
        "alls.f0=433500000,P25,456,0,0,1,0,655,1234567,1,1,1,",
        "alls.f1=439100000,YSF,0,0,0,0,0,0,0,1,5,2,TA2DOU",
    ];
    assert!(!b.add(&same, at(1)));
    assert!(b.add(&["alls.on=1", "alls.seq=3", "alls.f0=439100000,YSF,0,0,0,0,0,0,0,2,0,2,YM2KDZ"], at(2)));
    let y = b.groups().into_iter().find(|g| g.mode == "YSF").unwrap();
    assert_eq!(("YM2KDZ", 2), (y.caller.as_str(), y.calls()));
    assert!(b.add(
        &["alls.on=0", "alls.seq=4", "dmrs.on=1", "dmrs.seq=4", "dmrs.f0=438612500,1,2,1,1,0,2620,2620123,1,0,3"],
        at(3)
    ));
    let g = b.groups();
    let calls = |m: &str| g.iter().find(|g| g.mode == m).unwrap().calls();
    assert_eq!((3, 2, 1), (calls("P25"), calls("YSF"), calls("DMR")));
    assert_eq!((Some(at(3)), None), (b.all_stopped, b.dmr_stopped));
    assert_eq!(0, b.missed());
    let dir = tempfile::tempdir().unwrap();
    let path = searchlog::session_path(dir.path(), T0);
    searchlog::write(&path, &b.groups(), ',').unwrap();
    let text = std::fs::read_to_string(&path).unwrap();
    assert!(text.lines().any(|l| l.ends_with(",YSF,439.1000,DG0,,0,0,YM2KDZ,0,2")));
}

#[test]
fn calls_no_entry_showed_are_told_as_missed() {
    let mut b = SearchLogBook::new();
    b.add(&p(3, "p25s.f0=433500000,456,655,1,3,0,1"), T0);
    b.add(&p(7, "p25s.f0=433500000,456,655,1,4,0,1"), at(1));
    assert_eq!((4, 3), (b.total_calls(), b.missed()));
}

#[test]
fn a_cleared_log_counts_only_new_calls() {
    let mut b = SearchLogBook::new();
    b.add(&p(3, "p25s.f0=433500000,456,655,1,3,0,1"), T0);
    b.clear();
    assert!(b.groups().is_empty());
    assert!(!b.add(&p(3, "p25s.f0=433500000,456,655,1,3,1,1"), at(1)));
    assert!(b.groups().is_empty());
    assert!(b.add(&p(4, "p25s.f0=433500000,456,655,1,4,0,1"), at(2)));
    assert_eq!(1, b.total_calls());
}

#[test]
fn the_sessions_csv_is_the_log_itself() {
    let dir = tempfile::tempdir().unwrap();
    let mut b = SearchLogBook::new();
    b.add(&p(1, "p25s.f0=433500000,456,655,1234567,1,0,1"), T0);
    let path = searchlog::session_path(dir.path(), T0);
    assert_eq!(dir.path().join("search-20261121-220000.csv"), path);
    assert_eq!(CsvWritten { path: path.clone(), locked: false }, searchlog::write(&path, &b.groups(), ',').unwrap());
    b.add(&p(2, "p25s.f0=433500000,456,655,7654321,2,0,1"), at(5));
    searchlog::write(&path, &b.groups(), ',').unwrap();
    let bytes = std::fs::read(&path).unwrap();
    assert_eq!([0xEF, 0xBB, 0xBF], bytes[..3]); // a BOM: Excel reads the Turkish letters
    let text = String::from_utf8(bytes[3..].to_vec()).unwrap();
    let lines: Vec<&str> = text.lines().collect();
    assert_eq!(searchlog::csv_header(','), lines[0]);
    assert_eq!(2, lines.len()); // one line, not one per call
    assert_eq!("2026-11-21 22:00:00,2026-11-21 22:00:05,P25,433.5000,$1C8/456,,655,7654321,,0,2", lines[1]);
    assert!(!path.with_extension("csv.tmp").exists());
    // a program restart: a new session never takes an earlier session's file, even in the same second
    assert_eq!(dir.path().join("search-20261121-220000-2.csv"), searchlog::session_path(dir.path(), T0));
}

#[test]
fn csv_cells_fit_excel() {
    assert_eq!("'-TA1ABC", searchlog::csv_cell("-TA1ABC", ','));
    assert_eq!("'=HYPERLINK(1)", searchlog::csv_cell("=HYPERLINK(1)", ';'));
    assert_eq!("'+1", searchlog::csv_cell("+1", ','));
    assert_eq!("'@x", searchlog::csv_cell("@x", ','));
    assert_eq!("\"a;b\"", searchlog::csv_cell("a;b", ';'));
    assert_eq!("a,b", searchlog::csv_cell("a,b", ';'));
    assert_eq!("\"say \"\"hi\"\"\"", searchlog::csv_cell("say \"hi\"", ','));
    assert_eq!("$1C8/456", searchlog::csv_cell("$1C8/456", ','));
    let g = SearchGroup {
        mode: "YSF",
        hz: 439100000,
        code: "DG0".into(),
        group: 0,
        first: T0,
        last: T0,
        slot: String::new(),
        source: 0,
        caller: "=CMD|x".into(),
        encrypted: false,
        calls: 2,
    };
    assert_eq!(
        "2026-11-21 22:00:00;2026-11-21 22:00:00;YSF;439.1000;DG0;;0;0;'=CMD|x;0;2",
        searchlog::csv_row(&g, ';')
    );
    assert!([',', ';', '\t'].contains(&searchlog::list_separator()));
}

#[cfg(windows)]
#[test]
fn an_open_csv_goes_to_a_side_file_until_it_is_free() {
    use std::os::windows::fs::OpenOptionsExt;
    let dir = tempfile::tempdir().unwrap();
    let mut b = SearchLogBook::new();
    b.add(&p(1, "p25s.f0=433500000,456,655,1,1,0,1"), T0);
    let path = searchlog::session_path(dir.path(), T0);
    searchlog::write(&path, &b.groups(), ',').unwrap();
    let held = {
        // Excel holds it: no delete / rename sharing
        let _f = std::fs::OpenOptions::new().read(true).share_mode(1).open(&path).unwrap();
        b.add(&p(2, "p25s.f0=433500000,456,655,1,2,0,1"), at(1));
        searchlog::write(&path, &b.groups(), ',').unwrap()
    };
    assert_eq!(CsvWritten { path: searchlog::side_path(&path), locked: true }, held);
    assert!(held.path.to_string_lossy().ends_with("-open.csv"));
    assert!(std::fs::read_to_string(&held.path).unwrap().lines().any(|l| l.ends_with(",2")));
    assert_eq!(CsvWritten { path: path.clone(), locked: false }, searchlog::write(&path, &b.groups(), ',').unwrap());
    assert!(!held.path.exists());
}

#[test]
fn the_reader_sends_only_its_three_views() {
    assert_eq!([(0x09, 28), (0x09, 29), (0x09, 30)], SEARCHLOG_ALLOWED);
    assert!(app_info::RESERVED_NAMES.contains(&app_info::LOGS_FOLDER));
}

#[test]
fn all_digital_fields_are_parsed() {
    let p = parse_all("433500000,P25,456,0,0,1,0,655,1234567,3,10,4,", T0).unwrap();
    assert_eq!(
        ("P25", 433500000, "$1C8/456", "", 655, 1234567, 3, "", 4),
        (p.mode, p.hz, p.code.as_str(), p.slot.as_str(), p.group, p.source, p.calls, p.caller.as_str(), p.id)
    );
    assert_eq!(Stamp(T0.0 - 10), p.time);
    let d = parse_all("438612500,DMR,1,2,1,1,0,2620,2620123,4,0,5,", T0).unwrap();
    assert_eq!(("DMR", "CC1", "TS2 RPT", 2620, 2620123), (d.mode, d.code.as_str(), d.slot.as_str(), d.group, d.source));
    let y = parse_all("439100000,YSF,0,0,0,0,0,0,0,2,5,6,TA2DOU/P", T0).unwrap();
    assert_eq!(
        ("YSF", "DG0", "", 0, 0, 2, "TA2DOU/P"),
        (y.mode, y.code.as_str(), y.slot.as_str(), y.group, y.source, y.calls, y.caller.as_str())
    );
    let n = parse_all("438800000,NXDN,5,0,0,1,1,101,1234,1,0,7,", T0).unwrap();
    assert_eq!(("NXDN", "RAN5", 101, 1234, true), (n.mode, n.code.as_str(), n.group, n.source, n.encrypted));
    assert_eq!("RAN?", parse_all("438800000,NXDN,65535,0,0,0,0,0,0,0,0,1,", T0).unwrap().code);
    assert_eq!("$?", parse_all("438800000,P25,65535,0,0,0,0,0,0,0,0,1,", T0).unwrap().code);
    assert_eq!("", parse_all("438800000,YSF,0,0,0,0,0,0,0,0,0,1", T0).unwrap().caller);
    assert!(parse_all("438800000,FM,0,0,0,0,0,0,0,0,0,1,", T0).is_none());
    assert!(parse_all("438800000,P25,1,2", T0).is_none());
    assert!(parse_all("0,P25,0,0,0,0,0,0,0,0,0,1,", T0).is_none());
}

#[test]
fn firmware_without_the_search_log_is_told() {
    assert!(has_search_log(&["uptime_ms=5", "p25s.on=0", "p25s.seq=0"]));
    assert!(!has_search_log(&["uptime_ms=5", "p25s.on=0", "p25s.n=0"]));
    assert!(!has_search_log(&["p25s.built=0", "dmrs.built=0", "alls.built=0"]));
    assert!(!has_search_log::<&str>(&[]));
}

// ---- the reader on a fake radio ----

type View = Box<dyn Fn(u8) -> Option<String>>;

/// A fake radio answering GET_VIEW with a fixed text (None: "bad argument"), or going away / silent on cue.
struct FakeSearch {
    view: View,
    pending: VecDeque<u8>,
    opens: usize,
    closes: usize,
    gone: bool,
    silent: bool,
}

impl FakeSearch {
    fn new(view: View) -> Rc<RefCell<Self>> {
        Rc::new(RefCell::new(FakeSearch {
            view,
            pending: VecDeque::new(),
            opens: 0,
            closes: 0,
            gone: false,
            silent: false,
        }))
    }
}

struct Port(Rc<RefCell<FakeSearch>>);

impl RadioPort for Port {
    fn timeout(&self) -> f64 {
        0.0
    }
    fn set_timeout(&mut self, _t: f64) {}
    fn baud_rate(&self) -> u32 {
        115200
    }
    fn set_baud_rate(&mut self, _b: u32) -> ToolResult<()> {
        Ok(())
    }
    fn write(&mut self, d: &[u8]) -> ToolResult<Option<usize>> {
        let mut r = self.0.borrow_mut();
        if r.gone {
            return Err(ToolError::os("the port is closed"));
        }
        if r.silent {
            return Ok(Some(d.len()));
        }
        let frame = match (r.view)(d[6]) {
            None => build(CMD_ERROR, d[3], &[2]),
            Some(text) => {
                let mut body = vec![0, 0x50, 0x40, 0x01, 0x00, 0xFF];
                body.extend_from_slice(text.as_bytes());
                build(d[2] | 0x80, d[3], &body)
            }
        };
        r.pending.extend(frame);
        Ok(Some(d.len()))
    }
    fn read(&mut self, n: usize) -> ToolResult<Vec<u8>> {
        let mut r = self.0.borrow_mut();
        let k = n.min(r.pending.len());
        Ok(r.pending.drain(..k).collect())
    }
    fn reset_input_buffer(&mut self) -> ToolResult<()> {
        self.0.borrow_mut().pending.clear();
        Ok(())
    }
    fn flush(&mut self) -> ToolResult<()> {
        Ok(())
    }
}

impl RawSerial for Port {
    fn open(&mut self) -> ToolResult<()> {
        let mut r = self.0.borrow_mut();
        if r.gone {
            return Err(ToolError::os("could not open port 'COM14'"));
        }
        r.opens += 1;
        Ok(())
    }
    fn close(&mut self) -> ToolResult<()> {
        self.0.borrow_mut().closes += 1;
        Ok(())
    }
}

fn beta7() -> View {
    Box::new(|s| {
        Some(
            match s {
                0x1C => "p25s.on=1\np25s.seq=3\np25s.f0=433500000,456,655,1,3,0,1\n",
                0x1D => "dmrs.on=0\ndmrs.seq=0\n",
                _ => "alls.on=0\nalls.seq=0\n",
            }
            .to_string(),
        )
    })
}

fn args(a: &[&str]) -> Vec<String> {
    a.iter().map(|s| s.to_string()).collect()
}

/// Runs the reader on `radio`; `on_line` sees every printed line with the radio and the clock.
fn run(
    radio: &Rc<RefCell<FakeSearch>>,
    clock: &VirtualClock,
    argv: &[&str],
    cancel: &AtomicBool,
    on_line: &dyn Fn(&str, &mut FakeSearch, &VirtualClock),
) -> (ToolResult<()>, Vec<String>) {
    let lines = RefCell::new(Vec::<String>::new());
    let print = |s: &str| {
        for l in s.split('\n') {
            lines.borrow_mut().push(l.to_string());
            on_line(l, &mut radio.borrow_mut(), clock);
        }
    };
    let r2 = radio.clone();
    let serial = move |_: &str| -> ToolResult<Box<dyn RawSerial>> { Ok(Box::new(Port(r2.clone()))) };
    let ctx = ToolContext { print: &print, clock, serial: &serial, release_key_hex: None, codec: Codec::dm32() };
    let r = searchlog_tool(&args(argv), &ctx, cancel);
    let out = lines.borrow().clone();
    (r, out)
}

fn count(lines: &[String], prefix: &str) -> usize {
    lines.iter().filter(|l| l.starts_with(prefix)).count()
}

#[test]
fn an_older_firmware_is_told_and_the_run_ends() {
    let olds: Vec<View> = vec![
        Box::new(|_| Some(String::new())),
        Box::new(|s| Some(if s == 0x1C { "p25s.on=0\np25s.n=0\n".into() } else { String::new() })),
        Box::new(|_| Some("p25s.built=0\n".into())),
        Box::new(|_| None),
    ];
    for old in olds {
        let radio = FakeSearch::new(old);
        let clock = VirtualClock::default();
        let (r, lines) =
            run(&radio, &clock, &["--port", "COM14", "--reconnect"], &AtomicBool::new(false), &|_, _, _| {});
        let e = r.unwrap_err();
        assert_eq!(("SystemExit", "4"), (e.py_type.as_str(), e.message.as_str()));
        assert!(lines.iter().any(|l| l == SEARCHLOG_MISSING_LINE));
        assert_eq!(SEARCHLOG_MISSING_POLLS as usize, count(&lines, "=== end "));
        assert_eq!((1, 1), (radio.borrow().opens, radio.borrow().closes));
    }
}

#[test]
fn a_gone_radio_is_waited_for_with_the_table_kept() {
    let radio = FakeSearch::new(beta7());
    let clock = VirtualClock::default();
    let ends = RefCell::new(0);
    let waits = RefCell::new(0);
    let (r, lines) = run(
        &radio,
        &clock,
        &["--port", "COM14", "--count", "4", "--reconnect"],
        &AtomicBool::new(false),
        &|l, f, _| {
            if l.starts_with("=== end ") {
                *ends.borrow_mut() += 1;
                if *ends.borrow() == 2 {
                    f.gone = true; // the cable is pulled after the second poll
                }
            }
            if l.starts_with(SEARCHLOG_WAITING) {
                *waits.borrow_mut() += 1;
                if *waits.borrow() == 3 {
                    f.gone = false; // and back after three tries
                }
            }
        },
    );
    r.unwrap();
    assert_eq!(3, count(&lines, "=== end ")); // poll 3 met the pulled cable (no end), poll 4 the radio again
    assert_eq!(4, count(&lines, "=== poll "));
    assert_eq!(2, radio.borrow().opens);
    assert!(lines.iter().any(|l| l == "SEARCHLOG: COM14 open again"));
    assert!(count(&lines, SEARCHLOG_WAITING) >= 3);
    // without --reconnect a pulled cable ends the run with its error
    let r2 = FakeSearch::new(beta7());
    r2.borrow_mut().gone = true;
    let (r, _) = run(&r2, &clock, &["--port", "COM14"], &AtomicBool::new(false), &|_, _, _| {});
    assert!(port_trouble(&r.unwrap_err()));
}

#[test]
fn a_silent_radio_is_waited_for_after_a_minute() {
    let radio = FakeSearch::new(beta7());
    radio.borrow_mut().silent = true;
    let clock = VirtualClock::default();
    let t0 = clock.monotonic();
    let waited = RefCell::new(None::<f64>);
    let (r, _) = run(
        &radio,
        &clock,
        &["--port", "COM14", "--count", "30", "--reconnect"],
        &AtomicBool::new(false),
        &|l, f, c| {
            if l.starts_with(SEARCHLOG_WAITING) && waited.borrow().is_none() {
                *waited.borrow_mut() = Some(c.monotonic() - t0);
                f.silent = false;
            }
        },
    );
    r.unwrap();
    let w = waited.borrow().unwrap();
    assert!((SEARCHLOG_SILENT_SECONDS..SEARCHLOG_SILENT_SECONDS + 5.0).contains(&w), "{w}");
    assert_eq!(2, radio.borrow().opens);
    // without --reconnect: exit 3 after the same minute
    let r2 = FakeSearch::new(beta7());
    r2.borrow_mut().silent = true;
    let (r, _) = run(&r2, &VirtualClock::default(), &["--port", "COM14"], &AtomicBool::new(false), &|_, _, _| {});
    let e = r.unwrap_err();
    assert_eq!(("SystemExit", "3"), (e.py_type.as_str(), e.message.as_str()));
}

#[test]
fn reconnect_stops_on_cancel_while_waiting() {
    let radio = FakeSearch::new(beta7());
    radio.borrow_mut().gone = true;
    let cancel = AtomicBool::new(false);
    let (r, _) = run(&radio, &VirtualClock::default(), &["--port", "COM14", "--reconnect"], &cancel, &|l, _, _| {
        if l.starts_with(SEARCHLOG_WAITING) {
            cancel.store(true, Ordering::SeqCst);
        }
    });
    r.unwrap();
    assert_eq!(0, radio.borrow().opens);
    assert!(!port_trouble(&ToolError::no_port_given()));
    assert!(!port_trouble(&ToolError::value("Refused")));
    assert!(!port_trouble(&ToolError::radio_disabled()));
}
