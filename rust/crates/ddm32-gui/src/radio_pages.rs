// SPDX-License-Identifier: GPL-3.0-or-later
//! The read-only radio pages: health (plus_txdiag --health), live view (plus_live) and my channels (the codeplug of
//! the newest verified backup). Health and live talk to the running firmware through the debug link's read-only guard
//! (ddm32_core::radio::debug), one button each; my channels reads a backup file only.

use crate::{
    i18n, radio_allowed, reader_ended, tr, trf, with, with_port, worker, App, AppWindow, Fact, Radio, TableRow,
};
use ddm32_core::radio::codec::Codec;
use ddm32_core::radio::debug::{health_tool, live_tool, searchlog_tool, SEARCHLOG_WAITING};
use ddm32_core::radio::live::{self, HealthSummary, LiveSummary, Verdict};
use ddm32_core::radio::port::{RadioAccess, RawSerial, SystemClock};
use ddm32_core::radio::searchlog::{self, SearchGroup, SearchLogBook};
use ddm32_core::radio::tools::ToolContext;
use ddm32_core::radio::{ToolError, ToolResult};
use slint::{ComponentHandle, ModelRc, SharedString, VecModel};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

fn facts(items: Vec<(String, String)>) -> ModelRc<Fact> {
    ModelRc::new(VecModel::from(
        items.into_iter().map(|(l, v)| Fact { label: l.into(), value: v.into() }).collect::<Vec<_>>(),
    ))
}

fn message(ui: &AppWindow, text: &str, kind: &str) {
    let r = ui.global::<Radio>();
    r.set_message(text.into());
    r.set_message_kind(kind.into());
}

pub fn wire(ui: &AppWindow) {
    let r = ui.global::<Radio>();
    let w = ui.as_weak();
    r.on_navigate_home(move || {
        let ui = w.unwrap();
        ui.global::<App>().invoke_navigate(0);
    });
    let w = ui.as_weak();
    r.on_read_health(move || read_health(&w.unwrap()));
    let w = ui.as_weak();
    r.on_live_start(move || live_start(&w.unwrap()));
    r.on_live_stop(move || {
        if let Some(c) = with(|s| s.live_cancel.clone()) {
            c.store(true, Ordering::SeqCst);
        }
    });
    let w = ui.as_weak();
    r.on_slog_start(move || slog_start(&w.unwrap()));
    r.on_slog_stop(move || {
        if let Some(c) = with(|s| s.slog_cancel.clone()) {
            c.store(true, Ordering::SeqCst);
        }
    });
    r.on_slog_folder(move || {
        let dir = with(|s| s.paths.logs());
        let _ = std::fs::create_dir_all(&dir);
        crate::open_folder(&dir);
    });
    let w = ui.as_weak();
    r.on_slog_clear(move || {
        // "Kayıtları temizle" after its confirmation: the table and this session's CSV file are emptied; a running log
        // goes on in a new session file, counting only calls heard from now on
        let ui = w.unwrap();
        let running = ui.global::<Radio>().get_slog_running();
        let file = with(|s| {
            s.slog_book.clear();
            s.slog_started = if running { Some(searchlog::local_stamp()) } else { None };
            s.slog_written = None;
            s.slog_file.take()
        });
        if let Some(f) = file {
            let _ = std::fs::remove_file(searchlog::side_path(&f));
            let _ = std::fs::remove_file(f);
        }
        ui.global::<Radio>().set_slog_confirm(false);
        show_slog(&ui, None);
    });
    crate::channels::wire(ui);
    let w = ui.as_weak();
    r.on_check_cable(move || check_cable(&w.unwrap()));
    let w = ui.as_weak();
    r.on_health_auto_changed(move |on| health_auto(&w.unwrap(), on));
}

// ---- health -----------------------------------------------------------------------------------------------------------

/// Labelled values of a page.
pub type Facts = Vec<(String, String)>;

/// What the health page shows for a summary: (verdict, its sentence, facts, tasks).
pub fn health_view(lang: &str, s: &HealthSummary) -> (&'static str, String, Facts, Facts) {
    let v = live::verdict(s);
    let (name, key) = match v {
        Verdict::Green => ("green", "health_green"),
        Verdict::Amber => ("amber", "health_amber"),
        Verdict::Red => ("red", "health_red"),
        Verdict::Grey => ("grey", "health_green"),
    };
    let tr = |k: &str| i18n::t(lang, k);
    let turkish = lang != "en";
    let crash = if s.crash_valid {
        format!(
            "{} {} {}",
            s.crash_task.clone().unwrap_or_default(),
            s.crash_pc.clone().unwrap_or_default(),
            live::format_uptime(s.crash_uptime_ms)
        )
    } else {
        tr("f_none")
    };
    let facts = vec![
        (tr("f_uptime"), live::format_uptime(s.uptime_ms)),
        (tr("f_heap"), format!("{} ({})", live::bytes(s.heap_free, turkish), live::bytes(s.heap_min_free, turkish))),
        (
            tr("f_min_task"),
            format!("{} ({})", s.min_task.clone().unwrap_or_else(|| "-".into()), live::bytes(s.min_free, turkish)),
        ),
        (tr("f_reset"), s.reset_cause.clone().unwrap_or_else(|| "-".into())),
        (tr("f_crash"), crash),
    ];
    let tasks = s.tasks.iter().map(|(n, f)| (n.clone(), live::bytes(*f, turkish))).collect();
    (name, tr(key), facts, tasks)
}

fn show_health(ui: &AppWindow, lines: &[String]) {
    let lang = with(|s| s.lang.clone());
    let s = live::health_summary(lines);
    let (v, text, f, t) = health_view(&lang, &s);
    let r = ui.global::<Radio>();
    r.set_health_verdict(v.into());
    r.set_health_text(text.into());
    r.set_health_facts(facts(f));
    r.set_health_tasks(facts(t));
    r.set_health_raw(lines.join("\n").into());
}

/// Runs one read-only debug-link tool in a worker thread; its lines come to `line`, the end to `done`.
fn run_debug(
    ui: &AppWindow,
    tool: &'static str,
    argv: Vec<String>,
    cancel: Arc<AtomicBool>,
    line: impl Fn(&AppWindow, String) + Send + Sync + 'static,
    done: impl FnOnce(&AppWindow, Option<ToolError>) + Send + 'static,
) {
    let w = ui.as_weak();
    let line = Arc::new(line);
    let w2 = w.clone();
    worker::spawn(
        move || {
            let clock = SystemClock::default();
            let line2 = line.clone();
            let print = move |text: &str| {
                for l in text.split('\n') {
                    let l = l.to_string();
                    let w3 = w2.clone();
                    let f = line2.clone();
                    let _ = slint::invoke_from_event_loop(move || {
                        if let Some(ui) = w3.upgrade() {
                            f(&ui, l);
                        }
                    });
                }
            };
            // read-only tools: the read factory (a port only in a build with the radio feature)
            let serial = |p: &str| -> ToolResult<Box<dyn RawSerial>> { RadioAccess::factory(p) };
            let ctx = ToolContext {
                print: &print,
                clock: &clock,
                serial: &serial,
                release_key_hex: None,
                codec: Codec::dm32(),
            };
            // the search log runs all night: the PC stays awake while this thread runs it (dropped at its end)
            let _awake = (tool == "searchlog").then(ddm32_core::sys::KeepAwake::new);
            match tool {
                "health" => health_tool(&argv, &ctx),
                "searchlog" => searchlog_tool(&argv, &ctx, &cancel),
                _ => live_tool(&argv, &ctx, &cancel),
            }
        },
        move |r| {
            // a panic in the reader ends it too: its done callback clears the busy / running state and shows the line
            if let Some(ui) = w.upgrade() {
                done(&ui, r.unwrap_or_else(|p| Err(worker::panic_error(&p))).err());
            }
        },
    );
}

/// Whether the search log holds the port now (Live, Health and the radio steps wait for it; the C# ToolRunner.AnyBusy).
pub fn slog_holds_port() -> bool {
    with(|s| s.slog_cancel.is_some())
}

fn read_health(ui: &AppWindow) {
    if !radio_allowed(ui) {
        return;
    }
    if slog_holds_port() {
        message(ui, &tr("slog_busy"), "warning");
        return;
    }
    with_port(ui, read_health_on);
}

/// read_health once the port is known (crate::with_port).
fn read_health_on(ui: &AppWindow, found: Option<String>) {
    // the gates again: the listing took a moment
    if !radio_allowed(ui) {
        return;
    }
    if slog_holds_port() {
        message(ui, &tr("slog_busy"), "warning");
        return;
    }
    let Some(p) = found else {
        message(ui, &tr("radio_nocable"), "warning");
        return;
    };
    let r = ui.global::<Radio>();
    r.set_busy(true);
    r.set_health_verdict("grey".into());
    message(ui, "", "info");
    let lines = Arc::new(std::sync::Mutex::new(Vec::<String>::new()));
    let l2 = lines.clone();
    run_debug(
        ui,
        "health",
        vec!["--port".into(), p, "--health".into()],
        Arc::new(AtomicBool::new(false)),
        move |_, l| l2.lock().unwrap().push(l),
        move |ui, err| {
            ui.global::<Radio>().set_busy(false);
            let all = lines.lock().unwrap().clone();
            match err {
                None => {
                    show_health(ui, &all);
                    let t = searchlog::format_time(searchlog::local_stamp());
                    ui.global::<Radio>().set_health_when(trf("health_last_read", &t[11..]).into());
                }
                Some(e) => {
                    ui.global::<Radio>().set_health_raw(all.join("\n").into());
                    message(ui, &trf("msg_failed", &e.record()), "danger");
                }
            }
        },
    );
}

/// "Check the cable": the computer's device list read again on its worker thread (no port is opened); the result
/// shows on this page and on Home.
fn check_cable(ui: &AppWindow) {
    crate::request_scan(ui);
}

/// How often "read every 30 s" reads the health (the C# LiveData.AutoSeconds).
pub const HEALTH_AUTO: std::time::Duration = std::time::Duration::from_secs(30);

/// "Read every 30 s" ticked or not (default off; never kept: it ends when the page is left). Each read is the same
/// read-only step as the button's, and only while nothing else holds the port and the disclaimer is accepted.
pub fn health_auto(ui: &AppWindow, on: bool) {
    ui.global::<Radio>().set_health_auto(on);
    if !on {
        with(|s| s.health_timer = None);
        return;
    }
    let timer = slint::Timer::default();
    let w = ui.as_weak();
    timer.start(slint::TimerMode::Repeated, HEALTH_AUTO, move || {
        if let Some(ui) = w.upgrade() {
            health_auto_tick(&ui);
        }
    });
    with(|s| s.health_timer = Some(timer));
}

/// One tick of "read every 30 s": a read when the health page is open and the port is free (also the tests).
pub fn health_auto_tick(ui: &AppWindow) -> bool {
    let app = ui.global::<App>();
    let r = ui.global::<Radio>();
    let free = app.get_page() == 7
        && r.get_health_auto()
        && !app.get_busy()
        && !r.get_busy()
        && !r.get_live_running()
        && !r.get_slog_running()
        && r.get_disclaimer_ok()
        && with(|s| s.port_now.port.is_some());
    if free {
        read_health(ui);
    }
    free
}

// ---- live view --------------------------------------------------------------------------------------------------------

/// What the live page shows for one poll: (signal 0..1, S-meter, dBm text, facts).
pub fn live_view(lang: &str, s: &LiveSummary) -> (f32, String, String, Vec<(String, String)>) {
    let tr = |k: &str| i18n::t(lang, k);
    let comma = lang != "en";
    let g = &s.signal;
    let n = &s.native;
    let dash = |v: &Option<String>| v.clone().unwrap_or_else(|| "-".into());
    let num = |v: Option<i64>| v.map_or("-".into(), |x| x.to_string());
    let channel = match (&g.channel, g.channel_number) {
        (Some(c), Some(k)) => format!("{k} {c}"),
        (Some(c), None) => c.clone(),
        _ => {
            if g.vfo {
                "VFO".into()
            } else {
                "-".into()
            }
        }
    };
    let facts = vec![
        (tr("f_channel"), channel),
        (tr("f_zone"), dash(&g.zone)),
        (tr("f_freq"), format!("{} / {}", live::format_mhz(g.rx_hz, comma), live::format_mhz(g.tx_hz, comma))),
        (tr("f_mode"), format!("{} / {}", dash(&g.mode), dash(&g.state))),
        (tr("f_noise"), format!("{} / {}", num(g.noise), num(g.squelch))),
        (tr("f_native"), format!("{} ({})", dash(&n.state), dash(&n.reason))),
        (tr("f_sync"), format!("{} / {}", num(n.p25_sync), num(n.faults))),
        (tr("f_uptime"), live::format_uptime(g.uptime_ms)),
    ];
    let dbm = g.rssi_dbm.map_or("-".into(), |d| format!("{d} dBm"));
    (live::dbm_percent(g.rssi_dbm) as f32 / 100.0, live::s_meter(g.rssi_dbm), dbm, facts)
}

fn live_line(ui: &AppWindow, line: String) {
    let ended = with(|s| match live::poll_marker(&line) {
        Some((k, _)) if k == "poll" => {
            s.live_in_poll = true;
            s.live_poll.clear();
            None
        }
        Some(_) => {
            s.live_in_poll = false;
            Some((s.live_poll.clone(), s.live_once.clone(), s.lang.clone()))
        }
        None => {
            if s.live_in_poll {
                s.live_poll.push(line.trim().to_string());
            } else if !line.starts_with("LIVE:") {
                s.live_once.push(line.trim().to_string());
            }
            None
        }
    });
    if let Some((poll, once, lang)) = ended {
        let sum = live::live_summary(&live::parse_blocks(&poll), &live::parse_blocks(&once));
        let (sig, sm, dbm, f) = live_view(&lang, &sum);
        let r = ui.global::<Radio>();
        r.set_live_signal(sig);
        r.set_live_smeter(sm.into());
        r.set_live_dbm(dbm.into());
        r.set_live_facts(facts(f));
        r.set_live_raw(poll.join("\n").into());
    }
}

fn live_start(ui: &AppWindow) {
    if !radio_allowed(ui) {
        return;
    }
    if slog_holds_port() {
        message(ui, &tr("slog_busy"), "warning");
        return;
    }
    with_port(ui, live_start_on);
}

/// live_start once the port is known (crate::with_port).
fn live_start_on(ui: &AppWindow, found: Option<String>) {
    // the gates again: the listing took a moment
    if !radio_allowed(ui) {
        return;
    }
    if slog_holds_port() {
        message(ui, &tr("slog_busy"), "warning");
        return;
    }
    let Some(p) = found else {
        message(ui, &tr("radio_nocable"), "warning");
        return;
    };
    let cancel = Arc::new(AtomicBool::new(false));
    with(|s| {
        s.live_cancel = Some(cancel.clone());
        s.live_once.clear();
        s.live_poll.clear();
        s.live_in_poll = false;
    });
    let r = ui.global::<Radio>();
    r.set_live_running(true);
    message(ui, "", "info");
    let interval = r.get_live_interval().to_string();
    run_debug(ui, "live", vec!["--port".into(), p, "--interval".into(), interval], cancel, live_line, |ui, err| {
        ui.global::<Radio>().set_live_running(false);
        with(|s| s.live_cancel = None);
        // the window's close button waited for this reader
        if reader_ended() {
            return;
        }
        match err {
            None => message(ui, &tr("live_stopped"), "info"),
            Some(e) if e.py_type == "SystemExit" && e.message == "3" => message(ui, &tr("live_silent"), "warning"),
            Some(e) => message(ui, &trf("msg_failed", &e.record()), "danger"),
        }
    });
}

// ---- my channels (channels.rs) -----------------------------------------------------------------------------------

pub use crate::channels::{load_channels, show_table, table_view};

/// Example contents for the screenshots (no radio, no file).
pub fn demo(ui: &AppWindow, lang: &str) {
    let r = ui.global::<Radio>();
    let health = [
        "--- view tasks (20) status 0",
        "uptime_ms=5025000",
        "tasks.min_free=452",
        "tasks.min_task=IDLE",
        "tasks.free.IDLE=452",
        "tasks.free.host=1200",
        "tasks.free.ui=884",
        "tasks.free.p25_lab=640",
        "tasks.heap_free=6104",
        "tasks.heap_min_free=4048",
        "--- view crash (21) status 0",
        "uptime_ms=5025001",
        "crash.valid=0",
        "--- view reset (23) status 0",
        "uptime_ms=5025002",
        "reset.cause=power",
    ]
    .map(String::from);
    let (v, text, f, t) = health_view(lang, &live::health_summary(&health));
    r.set_health_verdict(v.into());
    r.set_health_text(text.into());
    r.set_health_facts(facts(f));
    r.set_health_tasks(facts(t));
    r.set_health_raw(health.join("\n").into());
    let poll = [
        "--- view core (0) status 0",
        "uptime_ms=82000",
        "state=rx",
        "mode=p25",
        "vfo=0",
        "zone_name=Zone A",
        "channel_number=4",
        "channel_name=P25 659",
        "rx_hz=434250000",
        "tx_hz=434250000",
        "squelch=3",
        "rssi_dbm=-87",
        "noise=12",
        "--- native (0x21/0) status 0",
        "state_name=receive",
        "reason_name=ok",
        "p25_sync=1",
        "faults=0",
    ]
    .map(String::from);
    let sum = live::live_summary(&live::parse_blocks(&poll), &Default::default());
    let (sig, sm, dbm, f) = live_view(lang, &sum);
    r.set_live_signal(sig);
    r.set_live_smeter(sm.into());
    r.set_live_dbm(dbm.into());
    r.set_live_facts(facts(f));
    r.set_live_raw(poll.join("\n").into());
    // made-up readings: no reader runs, so the buttons show their real states
    r.set_live_running(false);
    r.set_channels_source(i18n::tf(lang, "channels_source", "dm32-20261121").into());
    let cols: Vec<SharedString> = if lang == "en" {
        vec!["No".into(), "Name".into(), "Frequency".into(), "Mode".into(), "Settings".into()]
    } else {
        vec!["No".into(), "Ad".into(), "Frekans".into(), "Mod".into(), "Ayar".into()]
    };
    r.set_table_columns(ModelRc::new(VecModel::from(cols)));
    r.set_table_widths(ModelRc::new(VecModel::from(vec![0.07f32, 0.22, 0.2, 0.1, 0.41])));
    r.set_table_loaded(true);
    r.set_channels_radio(i18n::t(lang, "ch_radio").replacen("{}", "TA1TST", 1).replacen("{}", "2869999", 1).into());
    r.set_zone_text(i18n::tf(lang, "ch_zone", &i18n::t(lang, "ch_all_zones")).into());
    r.set_mode_text(format!("{} {}", i18n::t(lang, "ch_mode"), i18n::t(lang, "ch_all_modes")).into());
    r.set_sort_column(-1);
    r.set_table_min_width(0.0);
    r.set_cable_text(i18n::tf(lang, "radio_ready", "COM14").into());
    r.set_cable_kind("success".into());
    r.set_health_when(i18n::tf(lang, "health_last_read", "21:14:05").into());
    let rows = [
        ["1", "FM Simplex", "145.500", "FM", "Ton RX 67.0 / TX D023I"],
        ["2", "DMR TG91", "438.500 / TX 430.900", "DMR", "CC 1 · TS 2 · Kişi 3"],
        ["4", "P25 659", "434.250", "P25", "NAC 659 ($293) · TG 101"],
        ["5", "P25 Any", "434.250", "P25", "NAC Herhangi 3966 ($F7E) · TG 101 · sadece RX"],
        ["7", "NXDN", "446.10625", "NXDN", "RAN herhangi · sadece RX"],
    ];
    let rows: Vec<TableRow> = rows
        .iter()
        .map(|c| TableRow {
            cells: ModelRc::new(VecModel::from(c.iter().map(|x| SharedString::from(*x)).collect::<Vec<_>>())),
        })
        .collect();
    r.set_table_count(i18n::tn(lang, "ch_shown", &[&rows.len().to_string(), "212"]).into());
    r.set_table_rows(ModelRc::new(VecModel::from(rows)));
}

// ---- search log ---------------------------------------------------------------------------------------------------------

fn short_time(t: searchlog::Stamp) -> String {
    let t = searchlog::format_time(t);
    format!("{}.{} {}", &t[8..10], &t[5..7], &t[11..])
}

/// A table row: one mode + frequency + code + group, the most recently heard first on screen.
pub fn slog_cells(c: &SearchGroup) -> Vec<String> {
    vec![
        short_time(c.first),
        short_time(c.last),
        c.mode.to_string(),
        searchlog::mhz(c.hz),
        c.code.clone(),
        c.slot.clone(),
        if c.group == 0 { "-".into() } else { c.group.to_string() },
        if !c.caller.is_empty() {
            c.caller.clone()
        } else if c.source == 0 {
            "-".into()
        } else {
            format!("{}{}", c.source, if c.encrypted { " (enc)" } else { "" })
        },
        c.calls().to_string(),
    ]
}

/// The searches' line: running (and where) / stopped at / off / no answer, then the counts (and calls maybe missed).
pub fn slog_searches(lang: &str, book: &SearchLogBook) -> String {
    let f2 = |key: &str, a: &str, b: &str| i18n::t(lang, key).replacen("{}", a, 1).replacen("{}", b, 1);
    let one = |name: &str, on: Option<bool>, at: Option<i64>, stopped: Option<searchlog::Stamp>| match on {
        Some(true) => f2("slog_on", name, &at.map(searchlog::mhz).unwrap_or_else(|| "-".into())),
        Some(false) => match stopped {
            Some(t) => f2("slog_stopped", name, &searchlog::format_time(t)[11..16]),
            None => i18n::tf(lang, "slog_off", name),
        },
        None => i18n::tf(lang, "slog_noanswer", name),
    };
    let mut text = format!(
        "{}   {}   {}   {}",
        one("P25", book.p25_on, book.p25_at_hz, book.p25_stopped),
        one("DMR", book.dmr_on, book.dmr_at_hz, book.dmr_stopped),
        one(&i18n::t(lang, "slog_all"), book.all_on, book.all_at_hz, book.all_stopped),
        f2("slog_count", &book.total_calls().to_string(), &book.frequencies().to_string())
    );
    if book.missed() > 0 {
        text = format!("{text} {}", i18n::tf(lang, "slog_missed", &book.missed().to_string()));
    }
    if let Some(stats) = slog_stats_line(lang, book) {
        text = format!("{text}\n{stats}");
    }
    text
}

/// The search statistics line of the engine shown ("P25: Fast | pass 4.1 s (161 steps) | ..."); None while no
/// statistics are known (an older firmware sends none).
pub fn slog_stats_line(lang: &str, book: &SearchLogBook) -> Option<String> {
    let st = book.shown_stats()?;
    let dec = if lang == "en" { '.' } else { ',' };
    Some(format!("{}: {}", book.stats_engine?, st.describe(&|k| i18n::t(lang, k), dec)))
}

fn show_slog(ui: &AppWindow, status: Option<String>) {
    let r = ui.global::<Radio>();
    let (rows, searches) = with(|s| {
        let rows: Vec<TableRow> = s
            .slog_book
            .groups()
            .iter()
            .map(|c| TableRow {
                cells: ModelRc::new(VecModel::from(
                    slog_cells(c).into_iter().map(SharedString::from).collect::<Vec<_>>(),
                )),
            })
            .collect();
        (rows, slog_searches(&s.lang, &s.slog_book))
    });
    r.set_slog_rows(ModelRc::new(VecModel::from(rows)));
    r.set_slog_searches(if r.get_slog_running() { searches.into() } else { SharedString::new() });
    if let Some(st) = status {
        r.set_slog_status(st.into());
    }
}

fn slog_line(ui: &AppWindow, line: String) {
    let done = with(|s| match live::poll_marker(&line) {
        Some((k, _)) if k == "poll" => {
            s.slog_in_poll = true;
            s.slog_poll.clear();
            None
        }
        Some((k, _)) if k == "end" => {
            s.slog_in_poll = false;
            Some(s.slog_poll.clone())
        }
        _ => {
            if s.slog_in_poll {
                s.slog_poll.push(line.trim().to_string());
            } else if line.starts_with("NO_SEARCHLOG") {
                s.slog_nofw = true;
            }
            None
        }
    });
    if done.is_none() && line.starts_with(SEARCHLOG_WAITING) {
        let p = with(|s| s.port_now.port.clone()).unwrap_or_default();
        show_slog(ui, Some(trf("slog_waiting", &p)));
        return;
    }
    let Some(poll) = done else { return };
    let now = searchlog::local_stamp();
    let (changed, path, lines, stats) = with(|s| {
        let changed = s.slog_book.add(&poll, now);
        let started = *s.slog_started.get_or_insert(now);
        let logs = s.paths.logs();
        let path = s.slog_file.get_or_insert_with(|| searchlog::session_path(&logs, started)).clone();
        let stats: Vec<(&str, searchlog::SearchStats)> =
            s.slog_book.all_stats_seen().into_iter().map(|(n, st)| (n, st.clone())).collect();
        (changed, path, s.slog_book.groups(), stats)
    });
    let stats: Vec<(&str, &searchlog::SearchStats)> = stats.iter().map(|(n, st)| (*n, st)).collect();
    if changed {
        let (sep, dec) = searchlog::csv_format();
        match searchlog::write_with_stats(&path, &lines, sep, dec, &stats) {
            Ok(w) => {
                if w.locked {
                    let note = trf("slog_locked", &w.path.display().to_string());
                    message(ui, &note, "warning");
                    with(|s| s.slog_lock_note = Some(note));
                } else if let Some(note) = with(|s| s.slog_lock_note.take()) {
                    // the file is free again: only the lock message goes, never another page's warning
                    if ui.global::<Radio>().get_message() == note.as_str() {
                        message(ui, "", "info");
                    }
                }
                with(|s| s.slog_written = Some(w.path));
            }
            Err(e) => message(ui, &trf("msg_failed", &e.to_string()), "danger"),
        }
    }
    let file = with(|s| s.slog_written.clone());
    let t = searchlog::format_time(now);
    let mut status = trf("slog_running", &t[11..]);
    if let Some(f) = file {
        status = format!("{status}  {}", trf("slog_file", &f.display().to_string()));
    }
    show_slog(ui, Some(status));
}

fn slog_start(ui: &AppWindow) {
    if !radio_allowed(ui) {
        return;
    }
    with_port(ui, slog_start_on);
}

/// slog_start once the port is known (crate::with_port).
fn slog_start_on(ui: &AppWindow, found: Option<String>) {
    // the disclaimer again (the listing took a moment); the busy check below covers the readers
    if !radio_allowed(ui) {
        return;
    }
    let Some(p) = found else {
        message(ui, &tr("radio_nocable"), "warning");
        return;
    };
    // one radio step at a time (the C# ToolRunner.AnyBusy): not while a backup, install, restore, pack write, Health or
    // Live holds the port
    if ui.global::<App>().get_busy()
        || ui.global::<Radio>().get_busy()
        || with(|s| s.slog_cancel.is_some() || s.live_cancel.is_some())
    {
        message(ui, &tr("slog_busy_other"), "warning");
        return;
    }
    let cancel = Arc::new(AtomicBool::new(false));
    with(|s| {
        s.slog_cancel = Some(cancel.clone());
        s.slog_poll.clear();
        s.slog_in_poll = false;
        s.slog_nofw = false;
        // the session (its CSV file) begins with its first start; a stop and a start again go on in the same file
        s.slog_started.get_or_insert_with(searchlog::local_stamp);
    });
    let r = ui.global::<Radio>();
    r.set_slog_running(true);
    r.set_slog_status(tr("slog_connecting").into());
    message(ui, "", "info");
    // --reconnect: a cable pulled, a sleep of the PC or a radio restart is waited for, the table kept
    run_debug(ui, "searchlog", vec!["--port".into(), p, "--reconnect".into()], cancel, slog_line, |ui, err| {
        ui.global::<Radio>().set_slog_running(false);
        let nofw = with(|s| {
            s.slog_cancel = None;
            s.slog_nofw
        });
        show_slog(ui, Some(tr("live_stopped")));
        // the window's close button waited for this reader (the CSV was written in this thread, between events)
        if reader_ended() {
            return;
        }
        match err {
            _ if nofw => message(ui, &tr("slog_nofw"), "warning"),
            None => {}
            Some(e) if e.py_type == "SystemExit" && e.message == "4" => message(ui, &tr("slog_nofw"), "warning"),
            Some(e) if e.py_type == "SystemExit" && e.message == "3" => message(ui, &tr("live_silent"), "warning"),
            Some(e) => message(ui, &trf("msg_failed", &e.record()), "danger"),
        }
    });
}
