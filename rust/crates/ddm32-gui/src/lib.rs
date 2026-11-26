// SPDX-License-Identifier: GPL-3.0-or-later
//! The DDM32 Plus window: the Slint UI (ui/*.slint) and the controller that runs ddm32-core behind it.
//!
//! Every radio step starts only from its own button (one click = one step) and runs in a worker thread; its lines
//! come back to the window as progress. Nothing here retries a failed write. Backups always go to the program's own
//! `backups` and are found by themselves (ddm32_core::storage::backups); a restore may also take another backup folder
//! the user shows (it must be a verified backup).

pub mod channels;
pub mod disclaimer;
pub mod help;
pub mod i18n;
pub mod mappack;
pub mod packs;
pub mod radio_pages;
pub mod restore;
pub mod updates;
pub mod versions;
pub mod window;
pub mod winres;
pub mod worker;

slint::include_modules!();

use ddm32_core::pyjson::{self, Py};
use ddm32_core::radio::cable::CableStatus;
use ddm32_core::radio::codec::Codec;
use ddm32_core::radio::flow::{FileSteps, Installable, RestoreFlow, StepResult, Wizard};
use ddm32_core::radio::port::{RadioAccess, RawSerial, SystemClock};
use ddm32_core::radio::portpick::{self, PortOptions, PortPref, ResolvedPort};
use ddm32_core::radio::progress::{ProgressEvent, ProgressView};
use ddm32_core::radio::tools::ToolContext;
use ddm32_core::radio::{cps, fast_install, tools, ToolError, ToolResult};
use ddm32_core::releases::catalog::{self, Catalog, ReleaseLabel};
use ddm32_core::releases::client::{self, Package};
use ddm32_core::releases::net::HttpReleaseNet;
use ddm32_core::storage::backups::{self, FoundBackup};
use ddm32_core::storage::paths::{real_env, unique_folder};

/// Where the window looks up the environment (LOCALAPPDATA, APPDATA) to find the C# and Python programs' backups:
/// the real one in the program, [`no_env`] in the tests (nothing outside their own data folder is ever read).
pub type EnvLookup = fn(&str) -> Option<String>;

/// An environment with nothing in it: the tests' data folder is the only place backups are looked for.
pub fn no_env(_name: &str) -> Option<String> {
    None
}
use ddm32_core::storage::{today, AppPaths, Os, Settings, Theme as SavedTheme};
use slint::{ComponentHandle, ModelRc, SharedString, VecModel, Weak};
use std::cell::RefCell;
use std::path::{Path, PathBuf};
pub use updates::{newer_program, Installed, Known, UpdateOffer};

/// The controller's state (UI thread only).
pub struct State {
    pub paths: AppPaths,
    pub settings: Settings,
    pub lang: String,
    pub catalog: Option<Catalog>,
    pub shown: Vec<catalog::FirmwareVersion>,
    pub package: Option<PathBuf>,
    pub wizard: Wizard,
    pub restore: RestoreFlow,
    pub found: Vec<FoundBackup>,
    pub quick: Option<PathBuf>,
    pub progress: ProgressView,
    /// A note shown once on Home (the last program update's result).
    pub note: Option<String>,
    /// My channels: the decoded codeplug of the newest verified backup.
    pub codeplug: Option<ddm32_core::codeplug::Codeplug>,
    /// The live view: the stop switch of the running reader and the lines of the poll being read.
    pub live_cancel: Option<std::sync::Arc<std::sync::atomic::AtomicBool>>,
    pub live_once: Vec<String>,
    pub live_poll: Vec<String>,
    pub live_in_poll: bool,
    pub slog_cancel: Option<std::sync::Arc<std::sync::atomic::AtomicBool>>,
    pub slog_poll: Vec<String>,
    pub slog_in_poll: bool,
    pub slog_book: ddm32_core::radio::searchlog::SearchLogBook,
    /// This session's CSV file (searchlog::session_path), the file last written (it or its side file while Excel holds
    /// it), when the session began, and whether the radio's firmware has no search log
    pub slog_file: Option<std::path::PathBuf>,
    pub slog_written: Option<std::path::PathBuf>,
    pub slog_started: Option<ddm32_core::radio::searchlog::Stamp>,
    pub slog_nofw: bool,
    /// The "CSV open in another program" message this page showed (cleared only while it is still the one shown)
    pub slog_lock_note: Option<String>,
    /// The cable picker: the saved choice, what the OS lists now, the port a radio step uses now
    pub port_pref: PortPref,
    pub port_opts: PortOptions,
    pub port_now: ResolvedPort,
    /// What the radio runs as the program knows it (the last install record or a newer verified backup)
    pub known: Known,
    /// The last cable check (Home's cable line)
    pub cable: Option<CableStatus>,
    /// A port listing runs on its worker thread
    pub scan_running: bool,
    /// When the last port listing was applied (a radio step uses it while it is younger than PORT_FRESH)
    pub last_scan: Option<std::time::Instant>,
    /// A radio step that writes to the radio runs (install, restore, pack write): the window never closes then
    pub writing: bool,
    /// When the close button was last refused (a second press within FORCE_CLOSE_WINDOW may offer "Close anyway")
    pub close_refused_at: Option<std::time::Instant>,
    /// The "COM15 not found" line was dismissed for this port
    pub port_missing_ack: Option<String>,
    /// settings.json `auto_check`, read once (the 3 s tick must not read the file)
    pub auto_check: bool,
    /// The backup a running restore writes back (recorded when it is verified)
    pub restore_backup: Option<PathBuf>,
    /// The last failed update check (shown on Home until dismissed or a check succeeds)
    pub check_error: Option<String>,
    /// The start-up question: shown now, waiting for a radio job to end, answered "Later" in this session
    pub prompt: Option<UpdateOffer>,
    pub prompt_pending: bool,
    pub prompt_later: Option<UpdateOffer>,
    /// When the release list was last asked for (the 24 h re-check)
    pub last_check: Option<std::time::Instant>,
    /// The window's 3 s tick: port rescan, the waiting question, the 24 h re-check
    pub tick: Option<slint::Timer>,
    /// The test / alpha version the user said Yes for (its download goes ahead without asking again)
    pub test_ok: Option<String>,
    /// The window's close button was pressed while the live view or the search log ran: they were told to stop, and
    /// the window closes when both have ended (or after CLOSE_FALLBACK)
    pub close_pending: bool,
    pub close_timer: Option<slint::Timer>,
    /// A local release folder loaded (developer builds): its versions stay in the list after a check
    pub local: Option<Catalog>,
    /// A version "Switch to this version" chose that the channel does not offer: in the wizard's list this time
    pub pinned: Option<String>,
    /// The versions page's check runs; the last check's error as it came (the page says it in its own words)
    pub checking: bool,
    pub check_error_raw: Option<String>,
    /// My channels: the filters, the sort, the backup shown
    pub channels: channels::ChannelView,
    /// Health: "read every 30 s" while ticked
    pub health_timer: Option<slint::Timer>,
    /// Data packs: the list, the pages, the packs built or loaded, what the radio holds
    pub packs: packs::PackState,
    /// The update wizard's map pack step
    pub map: mappack::MapState,
    /// The environment the other programs' backups are found through (setup_with_env)
    pub env: EnvLookup,
    /// The DM-32's codec tables the backups are detected with (always Codec::dm32() in the program; the tests set a
    /// synthetic one with [`use_codec`], since the real stock bytes are never in the repository)
    pub codec: &'static Codec,
}

/// The codec tables a backup is detected with (tests: a synthetic stock firmware).
pub fn use_codec(codec: &'static Codec) {
    with(|s| s.codec = codec);
}

/// A settings write that failed (a damaged settings.json refuses the C# keys): appended to `logs/settings.log`, never
/// swallowed. True when the write went through.
pub(crate) fn noted(r: std::io::Result<()>, what: &str) -> bool {
    match r {
        Ok(()) => true,
        Err(e) => {
            let logs = with(|s| s.paths.logs());
            let _ = std::fs::create_dir_all(&logs);
            let line = format!("{} settings.json: {what} not saved: {e}\n", client::now_utc());
            let _ = std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(logs.join("settings.log"))
                .and_then(|mut f| std::io::Write::write_all(&mut f, line.as_bytes()));
            false
        }
    }
}

thread_local! {
    static STATE: RefCell<Option<State>> = const { RefCell::new(None) };
}

pub(crate) fn with<R>(f: impl FnOnce(&mut State) -> R) -> R {
    STATE.with(|s| f(s.borrow_mut().as_mut().expect("controller set up")))
}

/// Whether the controller's state is set up (the screenshot demos draw pages without it).
pub(crate) fn has_state() -> bool {
    STATE.with(|s| s.borrow().is_some())
}

/// The controller's state from outside (the tests drive the close guard with it).
pub fn with_state<R>(f: impl FnOnce(&mut State) -> R) -> R {
    with(f)
}

pub(crate) fn tr(key: &str) -> String {
    with(|s| i18n::t(&s.lang, key))
}

pub(crate) fn trf(key: &str, arg: &str) -> String {
    with(|s| i18n::tf(&s.lang, key, arg))
}

/// The text table behind Tr.t (also used by the screenshot tests).
pub fn install_texts(ui: &AppWindow) {
    ui.global::<Tr>().on_t(|lang, key| SharedString::from(i18n::t(&lang, &key)));
}

fn label_text(l: ReleaseLabel) -> String {
    with(|s| label_in(&s.lang, l))
}

/// The label's text in a language (Latest / Stable / Beta / Test / Alpha / Old).
pub fn label_in(lang: &str, l: ReleaseLabel) -> String {
    i18n::t(
        lang,
        match l {
            ReleaseLabel::Latest => "label_latest",
            ReleaseLabel::Stable => "label_stable",
            ReleaseLabel::Beta => "label_beta",
            ReleaseLabel::TestAlpha => "label_test",
            ReleaseLabel::Legacy => "label_legacy",
        },
    )
}

/// What the radio runs, read from a backup's program image: the DDM32 Plus release whose signed application is in it
/// (with its label), or the kind of firmware.
pub fn installed_line(program: &[u8], all: &[catalog::FirmwareVersion], lang: &str) -> String {
    updates::installed_text(&updates::installed_firmware(program, all, lang), lang)
}

/// A version as the home screen names it: the full version with its prerelease and its label,
/// e.g. "1.0.0-beta.5 (Beta)".
pub fn version_line(fv: &catalog::FirmwareVersion, all: &[catalog::FirmwareVersion], lang: &str) -> String {
    format!("{} ({})", fv.version(), label_in(lang, catalog::label_for(fv, all)))
}

/// `--demo`: example contents; setup() saves nothing for it (main sets it before setup).
pub static DEMO: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// Sets up the window: the state, every callback, the first contents. `fetch` loads the release list in the
/// background when the settings allow it.
pub fn setup(ui: &AppWindow, paths: AppPaths, fetch: bool) {
    setup_with_env(ui, paths, fetch, real_env);
}

/// setup() with the environment the other programs' data folders are found through (the tests pass [`no_env`], so
/// they never read the user's real backups).
pub fn setup_with_env(ui: &AppWindow, paths: AppPaths, fetch: bool, env: EnvLookup) {
    install_texts(ui);
    let _ = paths.ensure();
    let settings = Settings::new(paths.settings_file());
    let first_start = settings.language().is_none();
    let lang = settings.language().unwrap_or_else(|| "tr".into());
    ui.global::<Tr>().set_lang(lang.clone().into());
    ui.global::<Theme>().set_dark(settings.theme() == SavedTheme::Dark);
    let app = ui.global::<App>();
    // this program's channel, or the C# program's include_beta during the hand-over (Settings::channel)
    // decided once and saved, so a key written later (an install record) never changes it
    let channel = if DEMO.load(std::sync::atomic::Ordering::SeqCst) {
        settings.channel()
    } else {
        settings.settle_channel().unwrap_or_else(|_| settings.channel())
    };
    app.set_channel(channel);
    let disclaimer_ok = settings.disclaimer_accepted(&disclaimer::version());
    ui.global::<Radio>().set_disclaimer_ok(disclaimer_ok);
    app.set_auto_check(settings.auto_check());
    app.set_version(ddm32_core::app_info::VERSION.into());
    app.set_key_id(ddm32_core::releases::PUBLIC_KEY_ID.into());
    app.set_data_folder(paths.root.display().to_string().into());
    app.set_backups_folder(paths.backups().display().to_string().into());
    app.set_radio_access(RadioAccess::ENABLED);
    let auto = settings.auto_check();
    STATE.with(|s| {
        *s.borrow_mut() = Some(State {
            catalog: Some(client::cached_catalog(&paths.releases(), None)),
            paths,
            settings,
            lang,
            shown: vec![],
            package: None,
            wizard: Wizard::new(),
            restore: RestoreFlow::new(),
            found: vec![],
            quick: None,
            progress: ProgressView::new(),
            note: None,
            codeplug: None,
            live_cancel: None,
            live_once: vec![],
            live_poll: vec![],
            live_in_poll: false,
            slog_cancel: None,
            slog_poll: vec![],
            slog_in_poll: false,
            slog_book: Default::default(),
            slog_file: None,
            slog_written: None,
            slog_started: None,
            slog_nofw: false,
            slog_lock_note: None,
            port_pref: PortPref::default(),
            port_opts: PortOptions::default(),
            port_now: ResolvedPort::default(),
            known: Known { installed: Installed::NotRead, source: updates::Source::None, may_prompt: false },
            cable: None,
            scan_running: false,
            last_scan: None,
            writing: false,
            close_refused_at: None,
            port_missing_ack: None,
            auto_check: auto,
            restore_backup: None,
            check_error: None,
            prompt: None,
            prompt_pending: false,
            prompt_later: None,
            last_check: None,
            tick: None,
            test_ok: None,
            close_pending: false,
            close_timer: None,
            local: None,
            pinned: None,
            checking: false,
            check_error_raw: None,
            channels: Default::default(),
            health_timer: None,
            packs: Default::default(),
            map: Default::default(),
            env,
            codec: Codec::dm32(),
        })
    });
    with(|s| s.port_pref = s.settings.port_pref());
    app.set_developer(ddm32_core::app_info::DEVELOPER);
    wire(ui);
    radio_pages::wire(ui);
    packs::wire(ui);
    packs::init(ui);
    refresh(ui);
    if first_start {
        // the language first; the disclaimer follows when the first page is left (navigate)
        app.set_page(6);
    } else if !disclaimer_ok {
        // never accepted, or its text changed since: asked before anything else
        open_disclaimer(ui);
    }
    // the window's own close button (and Alt+F4): refused while a step runs, the readers stopped first
    let w = ui.as_weak();
    ui.window().on_close_requested(move || match w.upgrade() {
        Some(ui) => close_requested(&ui),
        None => slint::CloseRequestResponse::HideWindow,
    });
    // the last program update's result, once; old staging folders and zips go
    let updates = with(|s| s.paths.updates());
    // a hand-over started by C# 2.0.0-beta.2 or older wrote its result into the old guncelleme folder
    let legacy_updates = with(|s| s.paths.root.join(ddm32_core::app_info::LEGACY_UPDATES_FOLDER));
    let result = ddm32_core::update::self_update::read_result(&updates)
        .or_else(|| ddm32_core::update::self_update::read_result(&legacy_updates));
    if let Some(r) = result {
        let key = match r.as_str() {
            "ok" => "pu_result_ok",
            "rolled_back" => "pu_result_rolled_back",
            _ => "pu_result_aborted",
        };
        let note = trf(key, ddm32_core::app_info::VERSION);
        with(|s| s.note = Some(note));
        refresh(ui);
    }
    ddm32_core::update::self_update::cleanup(&updates, None);
    if fetch && auto {
        load_catalog(ui.as_weak(), true);
    }
    if fetch {
        start_tick(ui);
    }
}

/// Every 3 s (also driven by tests): the ports the OS lists (a cable plugged in or out, the window back in front;
/// listed on a worker thread, never opened), the update question that waited, and the 24 h re-check while the window
/// stays open (not while anything is under way).
pub fn tick(ui: &AppWindow) {
    request_scan(ui);
    if with(|s| s.prompt_pending) {
        consider_prompt(ui);
    }
    let due = with(|s| s.auto_check && s.last_check.is_some_and(|t| t.elapsed().as_secs() >= updates::RECHECK_SECONDS));
    if due && !busy_state(ui).any() {
        load_catalog(ui.as_weak(), true);
    }
}

fn start_tick(ui: &AppWindow) {
    let timer = slint::Timer::default();
    let w = ui.as_weak();
    timer.start(slint::TimerMode::Repeated, std::time::Duration::from_secs(3), move || {
        if let Some(ui) = w.upgrade() {
            tick(&ui);
        }
    });
    with(|s| s.tick = Some(timer));
}

/// What is under way in the window: the question and the program update's restart wait for all of it.
pub fn busy_state(ui: &AppWindow) -> updates::Busy {
    let app = ui.global::<App>();
    updates::Busy {
        port_held: app.get_menu_radio_locked(),
        wizard_active: app.get_wizard_active(),
        restore_ready: app.get_restore_ready(),
        dialog_open: app.get_confirm_open()
            || app.get_prompt_open()
            || app.get_disc_open()
            || app.get_test_confirm_open()
            || app.get_force_close_open()
            || page_dialog_open(ui)
            || ui.global::<Radio>().get_picker_open(),
    }
}

/// A page's own question is open (restore, download user.csv, write a pack) or the channels' filter picker.
fn page_dialog_open(ui: &AppWindow) -> bool {
    let p = ui.global::<Packs>();
    ui.global::<App>().get_res_confirm_open()
        || p.get_download_confirm_open()
        || p.get_write_confirm_open()
        || ui.global::<App>().get_map_ask_open()
        || ui.global::<Radio>().get_zone_picker_open()
}

/// The OS's serial ports and the cable check, read without opening any port (on the calling thread).
fn list_ports() -> (CableStatus, Vec<portpick::SerialPortEntry>) {
    let status = ddm32_core::radio::cable::check();
    let entries = portpick::list_serial_ports_with(&status);
    (status, entries)
}

/// Lists the ports on a worker thread (a driver being installed can block the device list for seconds) and applies
/// the result in the window's thread. One listing at a time.
pub(crate) fn request_scan(ui: &AppWindow) {
    if with(|s| std::mem::replace(&mut s.scan_running, true)) {
        return;
    }
    let w = ui.as_weak();
    // without an event loop (tests) nothing is applied; a panic in the listing still ends the scan
    worker::spawn(list_ports, move |r| {
        with(|s| s.scan_running = false);
        if let (Some(ui), Ok((status, entries))) = (w.upgrade(), r) {
            apply_port_scan(&ui, &status, &entries);
        }
    });
}

/// How long a port listing is good for a radio step's start.
pub const PORT_FRESH: std::time::Duration = std::time::Duration::from_secs(3);

/// Runs `then` with the port a radio step uses: from the last listing when it is younger than PORT_FRESH, else after
/// a fresh listing on a worker thread (never on the window's thread; the radio buttons are off meanwhile).
pub(crate) fn with_port(ui: &AppWindow, then: fn(&AppWindow, Option<String>)) {
    if with(|s| s.last_scan.is_some_and(|t| t.elapsed() < PORT_FRESH)) {
        let p = with(|s| s.port_now.port.clone());
        then(ui, p);
        return;
    }
    let app = ui.global::<App>();
    app.set_busy(true);
    ui.global::<Radio>().set_busy(true);
    let w = ui.as_weak();
    worker::spawn(list_ports, move |r| {
        let Some(ui) = w.upgrade() else { return };
        ui.global::<App>().set_busy(false);
        ui.global::<Radio>().set_busy(false);
        match r {
            Ok((status, entries)) => {
                apply_port_scan(&ui, &status, &entries);
                let p = with(|s| s.port_now.port.clone());
                then(&ui, p);
            }
            Err(e) => set_message(&ui, &trf("msg_failed", &e), "danger"),
        }
    });
}

/// A port listing's result: the port in use, Home's cable line and what changed in the picker.
pub fn apply_port_scan(ui: &AppWindow, status: &CableStatus, entries: &[portpick::SerialPortEntry]) {
    let opts = portpick::options(entries, Os::current());
    with(|s| {
        s.port_now = portpick::resolve(&s.port_pref, &opts, None);
        if s.port_now.missing.is_none() {
            // the port is back: a later loss is said again
            s.port_missing_ack = None;
        }
        s.port_opts = opts.clone();
        s.cable = Some(status.clone());
        s.last_scan = Some(std::time::Instant::now());
    });
    show_cable(ui);
    show_ports(ui);
}

/// Home's cable line from the last listing.
fn show_cable(ui: &AppWindow) {
    let app = ui.global::<App>();
    if !RadioAccess::ENABLED {
        app.set_radio_text(tr("radio_disabled").into());
        app.set_radio_kind("warning".into());
        return;
    }
    let Some((text, kind)) = with(|s| s.cable.as_ref().map(|st| cable_text(&s.lang, st, &s.port_opts.cables))) else {
        return;
    };
    app.set_radio_text(text.clone().into());
    app.set_radio_kind(kind.into());
    // the health page's cable card says the same
    let r = ui.global::<Radio>();
    r.set_cable_text(text.into());
    r.set_cable_kind(kind.into());
}

/// The picker's texts and lists from the state (in the window's language).
fn show_ports(ui: &AppWindow) {
    let (lang, pref, opts, now, ack) = with(|s| {
        (s.lang.clone(), s.port_pref.clone(), s.port_opts.clone(), s.port_now.clone(), s.port_missing_ack.clone())
    });
    let acknowledged = now.missing.is_some() && ack == now.missing;
    let v = port_view(&lang, &opts, &now, acknowledged);
    let r = ui.global::<Radio>();
    r.set_access(RadioAccess::ENABLED);
    r.set_port_head(v.head.into());
    r.set_port_line(v.line.into());
    r.set_port_note(v.note.clone().into());
    r.set_port_auto(pref.auto || now.missing.is_some());
    r.set_port_selected(now.port.clone().unwrap_or_default().into());
    r.set_port_auto_detail(v.auto_detail.into());
    set_port_rows(&r, v.cables, v.others);
    ui.global::<App>().set_port_missing(if acknowledged { String::new() } else { v.note }.into());
}

fn set_port_rows(r: &Radio<'_>, cables: Vec<(String, String)>, others: Vec<(String, String)>) {
    let rows = |list: Vec<(String, String)>| {
        ModelRc::new(VecModel::from(
            list.into_iter().map(|(n, d)| PortRow { name: n.into(), detail: d.into() }).collect::<Vec<_>>(),
        ))
    };
    r.set_port_cables(rows(cables));
    r.set_port_others(rows(others));
}

/// What the cable picker shows (tests/ports.rs).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PortView {
    /// The header: "Cable: COM14 (auto)"; while a chosen port is gone and not dismissed: "Cable: COM15 not found;
    /// using COM14 (auto)"
    pub head: String,
    /// The radio pages: "Port used: COM14 (auto)", with the fallback said
    pub line: String,
    /// The chosen port is gone: "The saved port COM15 is not there; using Auto." (Home, the picker)
    pub note: String,
    /// Under "Auto (CH340)": the port Auto picks now
    pub auto_detail: String,
    /// (name, detail) of the CH340 cables and of every other port ("Advanced")
    pub cables: Vec<(String, String)>,
    pub others: Vec<(String, String)>,
}

/// The picker's texts. `acknowledged`: the user dismissed the "not found" line for this port.
pub fn port_view(lang: &str, opts: &PortOptions, now: &ResolvedPort, acknowledged: bool) -> PortView {
    let mut name = now.port.clone().unwrap_or_else(|| i18n::t(lang, "cable_none"));
    if now.not_ch340 {
        name = i18n::tf(lang, "port_not_ch340", &name);
    }
    if now.auto && now.port.is_some() {
        name = i18n::tf(lang, "cable_auto", &name);
    }
    let note = now.missing.as_deref().map(|m| i18n::tf(lang, "port_missing", m)).unwrap_or_default();
    let mut line = i18n::tf(lang, "port_line", &name);
    if !note.is_empty() {
        line = format!("{line}. {note}");
    }
    let head_name = match (&now.missing, acknowledged) {
        (Some(m), false) if now.port.is_none() => i18n::tf(lang, "port_missing_none", m),
        (Some(m), false) => i18n::t(lang, "port_missing_head").replacen("{}", m, 1).replacen("{}", &name, 1),
        _ => name,
    };
    let auto_now = portpick::resolve(&PortPref::default(), opts, None).port;
    PortView {
        head: i18n::tf(lang, "cable_head", &head_name),
        line,
        note,
        auto_detail: match auto_now {
            Some(p) => i18n::tf(lang, "port_auto_d", &p),
            None => i18n::t(lang, "port_auto_none"),
        },
        cables: opts.cables.iter().map(|c| (c.clone(), i18n::t(lang, "port_cable_d"))).collect(),
        others: opts
            .others
            .iter()
            .map(|o| (o.name.clone(), i18n::t(lang, if o.virtual_port { "port_virtual_d" } else { "port_other_d" })))
            .collect(),
    }
}

/// The picker's choice: "" is Auto. Saved at once (settings.json `port`, `port_auto`). Refused while the port is
/// held or the update wizard is between its radio steps (its backup and install go to the same port).
pub fn choose_port(ui: &AppWindow, name: &str) -> bool {
    if ui.global::<App>().get_port_locked() {
        return false;
    }
    let pref = if name.is_empty() { PortPref::default() } else { PortPref::fixed(name) };
    with(|s| {
        s.port_pref = pref.clone();
        s.port_missing_ack = None;
        s.port_now = portpick::resolve(&pref, &s.port_opts, None);
        s.settings.set_port_pref(&pref)
    })
    .ok();
    show_ports(ui);
    request_scan(ui);
    true
}

fn wire(ui: &AppWindow) {
    let app = ui.global::<App>();
    let w = ui.as_weak();
    app.on_navigate(move |p| navigate(&w.unwrap(), p));
    let w = ui.as_weak();
    app.on_refresh(move || refresh(&w.unwrap()));
    let w = ui.as_weak();
    app.on_set_theme(move |dark| {
        let ui = w.unwrap();
        ui.global::<Theme>().set_dark(dark);
        with(|s| s.settings.set_theme(if dark { SavedTheme::Dark } else { SavedTheme::Light })).ok();
    });
    let w = ui.as_weak();
    app.on_set_lang(move |lang| {
        let ui = w.unwrap();
        ui.global::<Tr>().set_lang(lang.clone());
        with(|s| {
            s.lang = lang.to_string();
            s.settings.set_language(&lang)
        })
        .ok();
        refresh(&ui);
    });
    let w = ui.as_weak();
    app.on_set_channel(move |c| {
        let ui = w.unwrap();
        ui.global::<App>().set_channel(c);
        // `channel` and, for the C# program during the hand-over, `include_beta`
        with(|s| s.settings.set_channel(c)).ok();
        refresh(&ui);
    });
    app.on_set_auto_check(move |v| {
        with(|s| {
            s.auto_check = v;
            s.settings.set_auto_check(v)
        })
        .ok();
    });
    let w = ui.as_weak();
    app.on_wizard_next(move || wizard_next(&w.unwrap()));
    let w = ui.as_weak();
    app.on_wizard_back(move || {
        let ui = w.unwrap();
        let app = ui.global::<App>();
        let step = app.get_step();
        let back = match step {
            1 => 0,
            2 | 4 if app.get_backup_choice() != 0 => 1,
            2 => 1,
            3 => 1,
            4 => 3,
            s => s,
        };
        set_step(&ui, back);
    });
    let w = ui.as_weak();
    app.on_choose_backup(move |c| {
        let ui = w.unwrap();
        let app = ui.global::<App>();
        app.set_backup_choice(c);
        if c != 2 {
            app.set_understood(false);
        }
    });
    let w = ui.as_weak();
    app.on_confirm_no_backup(move || {
        let ui = w.unwrap();
        ui.global::<App>().set_confirm_open(false);
        let ok = with(|s| s.wizard.choose_no_backup(true).is_ok());
        if ok {
            set_step(&ui, 4);
        }
    });
    let w = ui.as_weak();
    app.on_start_backup(move || start_backup(&w.unwrap()));
    let w = ui.as_weak();
    app.on_start_install(move || start_install(&w.unwrap()));
    let w = ui.as_weak();
    app.on_restore_plan(move || restore_plan(&w.unwrap()));
    let w = ui.as_weak();
    app.on_restore(move || restore::ask(&w.unwrap()));
    let w = ui.as_weak();
    app.on_res_confirm_yes(move || {
        let ui = w.unwrap();
        ui.global::<App>().set_res_confirm_open(false);
        start_restore(&ui);
    });
    let w = ui.as_weak();
    app.on_res_previous(move || {
        restore::shortcut(&w.unwrap(), "previous");
    });
    let w = ui.as_weak();
    app.on_res_stock(move || {
        restore::shortcut(&w.unwrap(), "stock");
    });
    let w = ui.as_weak();
    app.on_res_other(move || {
        let ui = w.unwrap();
        let start = with(|s| s.paths.backups());
        if let Some(f) = rfd::FileDialog::new().set_directory(&start).pick_folder() {
            restore::other_folder(&ui, &f);
        }
    });
    let w = ui.as_weak();
    app.on_ver_check(move || versions::check_now(&w.unwrap()));
    let w = ui.as_weak();
    app.on_ver_switch(move || {
        versions::switch(&w.unwrap());
    });
    app.on_help_online(|| open_folder(Path::new(&format!("{GITHUB_URL}#readme"))));
    let w = ui.as_weak();
    app.on_app_check(move || {
        let ui = w.unwrap();
        ui.global::<App>().set_app_update_text(tr("pu_checking").into());
        load_catalog(ui.as_weak(), false);
    });
    let w = ui.as_weak();
    app.on_app_update(move || app_update(&w.unwrap()));
    let w = ui.as_weak();
    app.on_menu_action(move |a| menu_action(&w.unwrap(), &a));
    let w = ui.as_weak();
    app.on_disc_accept(move || accept_disclaimer(&w.unwrap()));
    let w = ui.as_weak();
    app.on_disc_reject(move || {
        // nothing is saved: the radio steps stay off, and Home says how to read it again
        w.unwrap().global::<App>().set_disc_open(false);
    });
    let w = ui.as_weak();
    app.on_disc_toggle_lang(move || {
        let ui = w.unwrap();
        let app = ui.global::<App>();
        let lang = if app.get_disc_lang() == "en" { "tr" } else { "en" };
        show_disclaimer(&ui, lang, app.get_disc_readonly());
    });
    let w = ui.as_weak();
    app.on_force_close_yes(move || {
        force_close(&w.unwrap());
    });
    let w = ui.as_weak();
    app.on_map_write(move || mappack::write(&w.unwrap()));
    let w = ui.as_weak();
    app.on_map_choose(move |c| mappack::choose(&w.unwrap(), &c));
    let w = ui.as_weak();
    app.on_test_confirm_yes(move || {
        let ui = w.unwrap();
        let app = ui.global::<App>();
        app.set_test_confirm_open(false);
        let i = app.get_selected_version() as usize;
        with(|s| s.test_ok = s.shown.get(i).map(|f| f.version().to_string()));
        wizard_next(&ui);
    });
    app.on_open_backups_folder(move || {
        let dir = with(|s| s.paths.backups());
        open_folder(&dir);
    });
    let w = ui.as_weak();
    app.on_dismiss_check_error(move || {
        with(|s| s.check_error = None);
        w.unwrap().global::<App>().set_check_error("".into());
    });
    let w = ui.as_weak();
    app.on_dismiss_port_missing(move || {
        let ui = w.unwrap();
        with(|s| s.port_missing_ack = s.port_now.missing.clone());
        show_ports(&ui);
    });
    let w = ui.as_weak();
    app.on_prompt_later(move || prompt_later(&w.unwrap()));
    let w = ui.as_weak();
    app.on_prompt_skip_program(move || prompt_skip(&w.unwrap(), true));
    let w = ui.as_weak();
    app.on_prompt_skip_firmware(move || prompt_skip(&w.unwrap(), false));
    let w = ui.as_weak();
    app.on_prompt_update_program(move || prompt_update_program(&w.unwrap()));
    let w = ui.as_weak();
    app.on_prompt_update_firmware(move || prompt_update_firmware(&w.unwrap()));
    let r = ui.global::<Radio>();
    let w = ui.as_weak();
    r.on_open_disclaimer(move || open_disclaimer(&w.unwrap()));
    let w = ui.as_weak();
    r.on_port_choose(move |name| {
        choose_port(&w.unwrap(), &name);
    });
    let w = ui.as_weak();
    r.on_port_rescan(move || request_scan(&w.unwrap()));
}

/// Closes the question; what it asked is not asked again in this session.
fn close_prompt(ui: &AppWindow) -> Option<UpdateOffer> {
    ui.global::<App>().set_prompt_open(false);
    with(|s| {
        let offer = s.prompt.take();
        s.prompt_later.clone_from(&offer);
        offer
    })
}

/// Anything under way besides the question itself (a radio job, a download, the wizard, a ready restore, another
/// dialog): the question's buttons do nothing then but close it (it is asked again later).
fn blocked_besides_prompt(ui: &AppWindow) -> bool {
    let mut b = busy_state(ui);
    let app = ui.global::<App>();
    b.dialog_open = app.get_confirm_open()
        || app.get_disc_open()
        || app.get_test_confirm_open()
        || page_dialog_open(ui)
        || ui.global::<Radio>().get_picker_open();
    b.any()
}

/// Something started meanwhile: the question closes without an answer and is asked again once it is over.
fn postpone_prompt(ui: &AppWindow) {
    ui.global::<App>().set_prompt_open(false);
    with(|s| {
        s.prompt = None;
        s.prompt_pending = true;
    });
}

pub fn prompt_later(ui: &AppWindow) {
    close_prompt(ui);
}

/// "Skip this version" for the program (`program`) or the firmware: stored in settings.json; the question stays
/// open for the other item, if any.
pub fn prompt_skip(ui: &AppWindow, program: bool) {
    let Some(offer) = with(|s| s.prompt.clone()) else { return };
    let (version, rest) = updates::skip_item(&offer, program);
    if let Some(v) = version {
        with(
            |s| {
                if program {
                    s.settings.set_skip_program_version(&v)
                } else {
                    s.settings.set_skip_firmware_version(&v)
                }
            },
        )
        .ok();
    }
    if rest.is_empty() {
        ui.global::<App>().set_prompt_open(false);
        with(|s| {
            s.prompt = None;
            s.prompt_later = Some(rest);
        });
    } else {
        let lang = with(|s| s.lang.clone());
        show_prompt(ui, &lang, &rest);
        with(|s| s.prompt = Some(rest));
    }
}

/// "Update now" for the program: the program update card (Settings) and the signed self-update; nothing while
/// anything else is under way.
pub fn prompt_update_program(ui: &AppWindow) {
    if blocked_besides_prompt(ui) {
        postpone_prompt(ui);
        return;
    }
    close_prompt(ui);
    navigate(ui, 4);
    app_update(ui);
}

/// "Update now" for the radio firmware: the update wizard at its first step with that version selected. It never
/// starts a radio step: the backup and every step after it start only from the user's own click.
pub fn prompt_update_firmware(ui: &AppWindow) {
    if blocked_besides_prompt(ui) {
        postpone_prompt(ui);
        return;
    }
    let version = close_prompt(ui).and_then(|o| o.firmware.map(|f| f.1));
    open_wizard_at(ui, version.as_deref());
}

/// The update wizard at its first step with this version selected (no radio step).
pub fn open_wizard_at(ui: &AppWindow, version: Option<&str>) {
    navigate(ui, 1);
    if let Some(v) = version {
        if let Some(i) = with(|s| s.shown.iter().position(|f| f.version() == v)) {
            ui.global::<App>().set_selected_version(i as i32);
        }
    }
}

/// After a check: the one question when something is newer (or later, when anything is under way).
pub fn consider_prompt(ui: &AppWindow) {
    let app = ui.global::<App>();
    if app.get_prompt_open() {
        return;
    }
    let offer = with(|s| {
        let cat = s.catalog.clone().unwrap_or_default();
        updates::update_offer(
            &cat,
            app.get_channel(),
            ddm32_core::app_info::VERSION,
            &s.known,
            s.settings.skip_program_version().as_deref(),
            s.settings.skip_firmware_version().as_deref(),
        )
    });
    let later = with(|s| s.prompt_later.clone());
    match updates::prompt_decision(&offer, later.as_ref(), busy_state(ui).any(), app.get_page() == 6) {
        updates::PromptDecision::No => with(|s| s.prompt_pending = false),
        updates::PromptDecision::Wait => with(|s| s.prompt_pending = true),
        updates::PromptDecision::Show => {
            let lang = with(|s| s.lang.clone());
            show_prompt(ui, &lang, &offer);
            with(|s| {
                s.prompt_pending = false;
                s.prompt = Some(offer);
            });
        }
    }
}

/// Fills the question's texts and opens it.
pub fn show_prompt(ui: &AppWindow, lang: &str, offer: &UpdateOffer) {
    let app = ui.global::<App>();
    let two = |k: &str, a: &str, b: &str| i18n::t(lang, k).replacen("{}", a, 1).replacen("{}", b, 1);
    let (p, pd) = match &offer.program {
        Some((cur, new)) => (
            two("prompt_program", cur, new),
            i18n::t(lang, if cfg!(windows) { "prompt_program_d" } else { "prompt_program_d_web" }),
        ),
        None => (String::new(), String::new()),
    };
    let (f, fd) = match &offer.firmware {
        Some((cur, new)) => {
            let fv = if has_state() { with(|s| s.catalog.as_ref().and_then(|c| c.find(new).cloned())) } else { None };
            (two("prompt_firmware", cur, new), updates::firmware_detail(lang, fv.as_ref()))
        }
        None => (String::new(), String::new()),
    };
    app.set_prompt_program(p.into());
    app.set_prompt_program_detail(pd.into());
    app.set_prompt_firmware(f.into());
    app.set_prompt_firmware_detail(fd.into());
    app.set_prompt_open(true);
}

/// What an entry of the menu bar does (the strings the menu's `action` callback carries).
#[derive(Debug, PartialEq, Eq)]
pub enum MenuAction {
    Page(i32),
    BackupsFolder,
    LogsFolder,
    Guide,
    GitHub,
    Disclaimer,
    /// "lang:tr" / "lang:en": the window's language at once
    Lang(String),
    /// A signed release from a local folder (developer builds only; refused otherwise)
    LocalRelease,
    Exit,
}

pub const GITHUB_URL: &str = "https://github.com/d0ukan/DDM32-Plus";

/// The pages: 0 home .. 10 search log, 11 versions, 12 help (6, the first start, is never a menu target).
pub const LAST_PAGE: i32 = 12;

pub fn parse_menu_action(a: &str) -> Option<MenuAction> {
    Some(match a {
        "backups-folder" => MenuAction::BackupsFolder,
        "logs-folder" => MenuAction::LogsFolder,
        "guide" => MenuAction::Guide,
        "github" => MenuAction::GitHub,
        "disclaimer" => MenuAction::Disclaimer,
        "exit" => MenuAction::Exit,
        "lang:tr" => MenuAction::Lang("tr".into()),
        "lang:en" => MenuAction::Lang("en".into()),
        "local-release" if ddm32_core::app_info::DEVELOPER => MenuAction::LocalRelease,
        _ => {
            MenuAction::Page(a.strip_prefix("page:")?.parse().ok().filter(|p| (0..=LAST_PAGE).contains(p) && *p != 6)?)
        }
    })
}

fn menu_action(ui: &AppWindow, a: &str) {
    let Some(action) = parse_menu_action(a) else { return };
    let app = ui.global::<App>();
    match action {
        MenuAction::Page(p) => {
            // navigate() itself refuses while a job, the live panel or the search log holds the port
            navigate(ui, p);
        }
        MenuAction::BackupsFolder => {
            let dir = with(|s| s.paths.backups());
            let _ = std::fs::create_dir_all(&dir);
            open_folder(&dir);
        }
        MenuAction::LogsFolder => {
            let dir = with(|s| s.paths.logs());
            let _ = std::fs::create_dir_all(&dir);
            open_folder(&dir);
        }
        MenuAction::Guide => open_folder(Path::new(&format!("{GITHUB_URL}#readme"))),
        MenuAction::GitHub => open_folder(Path::new(GITHUB_URL)),
        MenuAction::Disclaimer => open_disclaimer(ui),
        MenuAction::Lang(l) => app.invoke_set_lang(l.into()),
        MenuAction::LocalRelease => {
            // The menu's drop-down is its own popup window, still closing while this click runs: a folder dialog
            // opened here never showed on Windows (owner, 2026-11-25). Open it once the popup is gone.
            let w = ui.as_weak();
            slint::Timer::single_shot(std::time::Duration::from_millis(150), move || {
                let Some(ui) = w.upgrade() else { return };
                if let Some(f) = rfd::FileDialog::new().set_title(tr("m_local_release")).pick_folder() {
                    load_local_release(&ui, &f);
                }
            });
        }
        MenuAction::Exit => {
            if app.get_menu_radio_locked() {
                set_message(ui, &tr("m_exit_busy"), "warning");
                ui.global::<Radio>().set_message(tr("m_exit_busy").into());
                ui.global::<Radio>().set_message_kind("warning".into());
            } else {
                window::save(ui);
                let _ = slint::quit_event_loop();
            }
        }
    }
}

pub(crate) fn open_folder(dir: &Path) {
    let cmd = if cfg!(windows) {
        "explorer"
    } else if cfg!(target_os = "macos") {
        "open"
    } else {
        "xdg-open"
    };
    let _ = std::process::Command::new(cmd).arg(dir).spawn();
}

pub(crate) fn set_message(ui: &AppWindow, text: &str, kind: &str) {
    let app = ui.global::<App>();
    app.set_message(text.into());
    app.set_message_kind(kind.into());
}

fn navigate(ui: &AppWindow, page: i32) {
    let app = ui.global::<App>();
    // a radio step, the live panel or the search log holds the port: leaving its page would hide its Stop button
    if app.get_menu_radio_locked() {
        return;
    }
    // leaving the first-start (language) page: the disclaimer comes next when it was not accepted yet
    let from_first_start = app.get_page() == 6 && page != 6;
    set_message(ui, "", "info");
    clear_progress(&app);
    app.set_details("".into());
    app.set_show_details(false);
    if page != 7 && ui.global::<Radio>().get_health_auto() {
        // "read every 30 s" belongs to the health page: leaving it ends it
        radio_pages::health_auto(ui, false);
    }
    if page == 1 {
        with(|s| {
            s.wizard = Wizard::new();
            s.package = None;
            s.test_ok = None;
            s.pinned = None;
        });
        app.set_backup_choice(0);
        app.set_understood(false);
        set_step(ui, 0);
    }
    if page == 9 {
        radio_pages::load_channels(ui);
    }
    // a planned restore belongs to the Backups page: leaving it (or opening it afresh) drops the plan, so it never
    // holds the update question or the program update back from another page
    app.set_restore_ready(false);
    if page == 2 {
        app.set_selected_backup(-1);
        app.set_res_status("".into());
    }
    if page == 3 {
        packs::show(ui);
        ui.global::<Packs>().set_result("".into());
    }
    if page == 11 {
        app.set_ver_note("".into());
    }
    if page == 12 {
        help::show(ui);
    }
    app.set_page(page);
    refresh(ui);
    if from_first_start && !ui.global::<Radio>().get_disclaimer_ok() {
        open_disclaimer(ui);
    }
}

// ---- the disclaimer ---------------------------------------------------------------------------------------------------

/// Opens the disclaimer: to be accepted (the box ticked, then "I accept") while it is not, read only once it is.
pub fn open_disclaimer(ui: &AppWindow) {
    let lang = with(|s| s.lang.clone());
    let accepted = ui.global::<Radio>().get_disclaimer_ok();
    show_disclaimer(ui, &lang, accepted);
}

/// Fills the dialog in a language and opens it (also the screenshot tests; no state needed).
pub fn show_disclaimer(ui: &AppWindow, lang: &str, readonly: bool) {
    let app = ui.global::<App>();
    let v = disclaimer::view(lang);
    app.set_disc_lang(lang.into());
    app.set_disc_title(v.title.into());
    app.set_disc_intro(v.intro.into());
    app.set_disc_points(ModelRc::new(VecModel::from(v.points.into_iter().map(SharedString::from).collect::<Vec<_>>())));
    app.set_disc_outro(v.outro.into());
    app.set_disc_outro_bold(v.outro_bold);
    if !app.get_disc_open() {
        app.set_disc_checked(false);
    }
    app.set_disc_readonly(readonly);
    app.set_disc_open(true);
}

/// "I accept" with the box ticked: saved as the C# program saves it (settings.json `disclaimer`), the radio steps open.
fn accept_disclaimer(ui: &AppWindow) {
    let app = ui.global::<App>();
    if !app.get_disc_checked() || app.get_disc_readonly() {
        return;
    }
    let saved = with(|s| s.settings.accept_disclaimer(&disclaimer::version(), &client::now_utc()));
    if let Err(e) = saved {
        // not saved: the steps stay off rather than open without a record
        set_message(ui, &trf("msg_failed", &e.to_string()), "danger");
        app.set_disc_open(false);
        return;
    }
    ui.global::<Radio>().set_disclaimer_ok(true);
    app.set_disc_open(false);
    set_message(ui, "", "info");
    ui.global::<Radio>().set_message("".into());
    if with(|s| s.prompt_pending) {
        consider_prompt(ui);
    }
}

/// Every radio step asks this first: refused (and said on screen) while the disclaimer is not accepted.
pub(crate) fn radio_allowed(ui: &AppWindow) -> bool {
    if ui.global::<Radio>().get_disclaimer_ok() {
        return true;
    }
    let text = tr("disc_not_accepted");
    set_message(ui, &text, "warning");
    let r = ui.global::<Radio>();
    r.set_message(text.into());
    r.set_message_kind("warning".into());
    false
}

// ---- the window's close button ----------------------------------------------------------------------------------------

/// How long the close button waits for the live view and the search log to stop before it closes anyway (their files
/// are written in the window's own thread, between two events, so none is ever left half written).
pub const CLOSE_FALLBACK: std::time::Duration = std::time::Duration::from_secs(10);

/// The window's close button, Alt+F4 or the OS asking the window to close (the C# OnClosing): refused while a radio
/// step (backup, install, restore, pack write, health read) or a download runs; the live view and the search log are
/// told to stop and the window closes once they have ended.
pub fn close_requested(ui: &AppWindow) -> slint::CloseRequestResponse {
    let app = ui.global::<App>();
    let r = ui.global::<Radio>();
    let downloading = with(|s| s.packs.download_bytes.is_some());
    if app.get_busy() || r.get_busy() || downloading {
        let writing = with(|s| s.writing);
        let again = with(|s| s.close_refused_at.is_some_and(|t| t.elapsed() < FORCE_CLOSE_WINDOW));
        if !writing && again {
            // the second press: "Close anyway?" (No by default); never while the radio is written
            app.set_force_close_open(true);
            return slint::CloseRequestResponse::KeepWindowShown;
        }
        with(|s| s.close_refused_at = Some(std::time::Instant::now()));
        let text = if writing { tr("busy_close") } else { format!("{} {}", tr("busy_close"), tr("busy_close_again")) };
        set_message(ui, &text, "warning");
        r.set_message(text.into());
        r.set_message_kind("warning".into());
        return slint::CloseRequestResponse::KeepWindowShown;
    }
    let readers: Vec<_> = with(|s| [s.live_cancel.clone(), s.slog_cancel.clone()].into_iter().flatten().collect());
    window::save(ui);
    if readers.is_empty() {
        return slint::CloseRequestResponse::HideWindow;
    }
    for c in &readers {
        c.store(true, std::sync::atomic::Ordering::SeqCst);
    }
    r.set_message(tr("closing_wait").into());
    r.set_message_kind("info".into());
    let first = with(|s| !std::mem::replace(&mut s.close_pending, true));
    if first {
        let timer = slint::Timer::default();
        timer.start(slint::TimerMode::SingleShot, CLOSE_FALLBACK, || {
            let _ = slint::quit_event_loop();
        });
        with(|s| s.close_timer = Some(timer));
    }
    slint::CloseRequestResponse::KeepWindowShown
}

/// A second press of the close button within this time, while a step that does not write to the radio runs, asks
/// "Close anyway?".
pub const FORCE_CLOSE_WINDOW: std::time::Duration = std::time::Duration::from_secs(5);

/// "Close anyway": only when no radio write runs (checked again here); the half-done backup or download is never used
/// (a backup without its verified manifest is not one).
pub fn force_close(ui: &AppWindow) -> bool {
    let app = ui.global::<App>();
    app.set_force_close_open(false);
    if with(|s| s.writing) {
        let text = tr("busy_close");
        set_message(ui, &text, "warning");
        let r = ui.global::<Radio>();
        r.set_message(text.into());
        r.set_message_kind("warning".into());
        return false;
    }
    // a RadioID download cut short leaves no half file behind
    if with(|s| s.packs.download_bytes.is_some()) {
        let part = with(|s| s.paths.packs().join("user.csv"));
        let _ = std::fs::remove_file(ddm32_core::releases::net::partial_path(&part));
    }
    window::save(ui);
    let _ = slint::quit_event_loop();
    true
}

/// A reader (live view, search log) ended: when the close button waits for it and nothing else runs, the window
/// closes now. True when it does.
pub fn reader_ended() -> bool {
    let close = with(|s| s.close_pending && s.live_cancel.is_none() && s.slog_cancel.is_none());
    if close {
        // the place was saved when the close button was pressed
        let _ = slint::quit_event_loop();
    }
    close
}

// ---- the self-update start marker -------------------------------------------------------------------------------------

/// How long after the window's event loop starts the marker is written (the C# program waits 1.5 s too): the window
/// is up and drawing, not only created.
pub const MARKER_DELAY: std::time::Duration = std::time::Duration::from_millis(1500);

/// `--update-check FILE`: the file the update helper waits for (90 s), or None.
pub fn start_marker_path(args: &[String]) -> Option<PathBuf> {
    let i = args.iter().position(|a| a == ddm32_core::update::self_update::CHECK_ARG)?;
    args.get(i + 1).filter(|m| !m.is_empty()).map(PathBuf::from)
}

/// Started by the update helper (`--update-check FILE`): writes the start marker once the event loop has run for
/// MARKER_DELAY. Without it the helper (this program's or the C# program's) puts the old program back after 90 s.
pub fn arm_start_marker(args: &[String]) {
    if let Some(marker) = start_marker_path(args) {
        slint::Timer::single_shot(MARKER_DELAY, move || {
            let _ = ddm32_core::update::self_update::write_marker(&marker, ddm32_core::app_info::VERSION);
        });
    }
}

/// offered(), plus a version "Switch to this version" chose that the channel does not offer (never a legacy one),
/// in its place in the order.
pub fn offered_with(cat: &Catalog, channel: i32, pinned: Option<&str>) -> Vec<catalog::FirmwareVersion> {
    let mut list = offered(cat, channel);
    if let Some(fv) = pinned.and_then(|v| cat.find(v)).filter(|f| !f.is_legacy()) {
        if !list.iter().any(|f| f.version() == fv.version()) {
            list.push(fv.clone());
            list.sort_by_key(|f| std::cmp::Reverse(f.key()));
        }
    }
    list
}

/// The update wizard at its first step with `version` selected, added to the list when the channel does not offer
/// it (the versions page's "Switch to this version"). No radio step.
pub fn open_wizard_pinned(ui: &AppWindow, version: &str) {
    navigate(ui, 1);
    if ui.global::<App>().get_page() != 1 {
        return;
    }
    with(|s| s.pinned = Some(version.to_string()));
    refresh(ui);
    if let Some(i) = with(|s| s.shown.iter().position(|f| f.version() == version)) {
        ui.global::<App>().set_selected_version(i as i32);
    }
}

/// The versions the chosen channel offers (Latest: stable only; Beta: stable and beta; Test: all), never legacy.
pub fn offered(cat: &Catalog, channel: i32) -> Vec<catalog::FirmwareVersion> {
    let pick = |ch: i32| -> Vec<catalog::FirmwareVersion> {
        cat.firmware
            .iter()
            .filter(|f| !f.is_legacy())
            .filter(|f| match catalog::label_for(f, &cat.firmware) {
                ReleaseLabel::TestAlpha => ch >= 2,
                ReleaseLabel::Beta => ch >= 1,
                _ => true,
            })
            .cloned()
            .collect()
    };
    let list = pick(channel);
    // No stable release yet (every release is a beta): offer the betas, as the C# program does, instead of an empty
    // list that reads like a network error. Test / alpha builds still need their own channel.
    if list.is_empty() && channel < 1 {
        return pick(1);
    }
    list
}

fn refresh(ui: &AppWindow) {
    let app = ui.global::<App>();
    let channel = app.get_channel();
    // the cable and the port in use: the last listing now, a new one on its worker thread (never opening a port)
    show_cable(ui);
    show_ports(ui);
    request_scan(ui);
    // versions
    let rows = with(|s| {
        let cat = s.catalog.clone().unwrap_or_default();
        s.shown = offered_with(&cat, channel, s.pinned.as_deref());
        s.shown.clone()
    });
    let all = with(|s| s.catalog.clone().unwrap_or_default().firmware);
    let lang = with(|s| s.lang.clone());
    let model: Vec<VersionRow> = rows
        .iter()
        .map(|f| VersionRow {
            version: f.version().into(),
            label: label_text(catalog::label_for(f, &all)).into(),
            notes: f.notes(&lang).into(),
            local: f.is_local(),
        })
        .collect();
    if app.get_selected_version() as usize >= model.len() {
        app.set_selected_version(0);
    }
    app.set_versions(ModelRc::new(VecModel::from(model)));
    // backups, found by themselves
    let (found, quick) = with(|s| {
        let roots = backups::backup_roots(&s.paths, &s.settings, Os::current(), &s.env);
        s.found = backups::discover(&roots);
        let dirs: Vec<PathBuf> = roots.iter().map(|r| r.folder.clone()).collect();
        s.quick = backups::last_for_quick_update(&dirs, s.codec, 8);
        (s.found.clone(), s.quick.clone())
    });
    app.set_backups_text(trf("backups_found", &found.len().to_string()).into());
    // what the radio runs: the record of the last verified install / restore, or a verified backup newer than it
    // (files only), and the newest version against it
    let newest_backup = found.iter().find_map(|b| {
        let imgs = ddm32_core::radio::unit_image::saved_images(&b.folder).ok()?;
        let t = b.modified.duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs() as i64);
        Some((t, updates::installed_firmware(imgs.get("progmem-pass1.bin")?, &all, &lang)))
    });
    let record = with(|s| updates::read_record(&s.settings));
    let known = updates::known_firmware(
        record.as_ref().map(|r| (r, updates::installed_from_record(r, &all, &lang))),
        newest_backup,
    );
    app.set_installed_text(updates::installed_text(&known.installed, &lang).into());
    let program = ddm32_core::app_info::VERSION;
    app.set_version_line(updates::version_text(&lang, program, &known).into());
    app.set_title_text(format!("{}{}", updates::title_text(&lang, program, &known), dev_title(&lang)).into());
    app.set_installed_firmware(updates::installed_short(&known, &lang).into());
    let status = updates::firmware_status(&known.installed, &rows);
    app.set_update_text(updates::firmware_status_text(&lang, &status, known.source, &rows, &all).into());
    with(|s| s.known = known);
    app.set_check_error(with(|s| s.check_error.clone()).unwrap_or_default().into());
    // the program's own update
    let channel = app.get_channel();
    let (text, available) = with(|s| match &s.catalog {
        Some(cat) if cat.source != "cache" || !cat.apps.is_empty() => {
            match newer_program(cat, ddm32_core::app_info::VERSION, channel) {
                Some(a) => (i18n::tf(&s.lang, "pu_available", &a.entry.version), true),
                None => (i18n::tf(&s.lang, "pu_current", ddm32_core::app_info::VERSION), false),
            }
        }
        _ => (i18n::t(&s.lang, "pu_unknown"), false),
    });
    app.set_app_update_text(text.into());
    app.set_app_update_available(available);
    if let Some(note) = with(|s| s.note.clone()) {
        app.set_update_text(format!("{note}\n{}", app.get_update_text()).into());
    }
    app.set_quick_available(quick.is_some());
    app.set_quick_backup(
        quick.map(|q| q.file_name().unwrap_or_default().to_string_lossy().into_owned()).unwrap_or_default().into(),
    );
    let brows: Vec<BackupRow> = found
        .iter()
        .map(|b| BackupRow {
            name: b.folder.file_name().unwrap_or_default().to_string_lossy().into_owned().into(),
            detail: b.folder.display().to_string().into(),
            origin: b.origin.into(),
            kind: "".into(),
        })
        .collect();
    app.set_backups(ModelRc::new(VecModel::from(brows)));
    if app.get_page() == 11 {
        versions::show(ui);
    }
}

/// The developer build's mark in the title (the C# Dev_Title / Dev_TitleWrite); nothing in a release build.
pub fn dev_title(lang: &str) -> String {
    if !ddm32_core::app_info::DEVELOPER {
        return String::new();
    }
    let key = if RadioAccess::WRITES_ENABLED {
        "dev_title_write"
    } else if RadioAccess::ENABLED {
        "dev_title"
    } else {
        "dev_title_off"
    };
    i18n::t(lang, key)
}

/// A signed release in a local folder (developer builds only): verified like a downloaded one, its versions added to
/// the list (and kept there after a check); the versions page says what was loaded.
pub fn load_local_release(ui: &AppWindow, folder: &Path) {
    if !ddm32_core::app_info::DEVELOPER {
        return;
    }
    let releases = with(|s| s.paths.releases());
    let folder = folder.to_path_buf();
    let w = ui.as_weak();
    worker::spawn(
        move || ddm32_core::releases::local::load_local(&folder, &releases, None).map_err(|e| e.0),
        move |r| {
            let Some(ui) = w.upgrade() else { return };
            local_release_loaded(&ui, r.and_then(|x| x));
        },
    );
}

/// The end of a local release load (also the tests): the versions added, or the reason; the versions page opens.
pub fn local_release_loaded(ui: &AppWindow, r: Result<Catalog, String>) {
    let lang = with(|s| s.lang.clone());
    let (text, kind) = match r {
        Ok(cat) if !cat.firmware.is_empty() => {
            let names: Vec<String> = cat.firmware.iter().map(|f| f.version().to_string()).collect();
            with(|s| {
                let mut all = s.catalog.clone().unwrap_or_default();
                all.add_local(&cat);
                s.catalog = Some(all);
                s.local = Some(cat);
            });
            (i18n::tf(&lang, "local_loaded", &names.join(", ")), "success")
        }
        Ok(cat) => {
            let why: Vec<String> = cat.rejected.iter().map(|(t, r)| format!("{t}: {r}")).collect();
            (i18n::tf(&lang, "local_failed", &why.join("; ")), "warning")
        }
        Err(e) => (i18n::tf(&lang, "local_failed", &e), "warning"),
    };
    navigate(ui, 11);
    let app = ui.global::<App>();
    app.set_ver_note(text.into());
    app.set_ver_note_kind(kind.into());
    versions::show(ui);
}

/// Downloads the new program (size and SHA-256 from the signed manifest), stages it, starts the staged program as
/// the helper that swaps the files after this program has closed, and closes. Windows only: on Linux and macOS the
/// package (AppImage, .app) is replaced from the download page.
fn app_update(ui: &AppWindow) {
    let app = ui.global::<App>();
    // never over a radio job, the live view, the search log, the wizard between its steps or a ready restore
    if blocked_besides_prompt(ui) {
        app.set_app_update_text(tr("pu_blocked").into());
        return;
    }
    let Some(rel) =
        with(|s| s.catalog.as_ref().and_then(|c| newer_program(c, ddm32_core::app_info::VERSION, app.get_channel())))
    else {
        return;
    };
    if !cfg!(windows) {
        let url = format!("{}/releases/tag/{}", ddm32_core::app_info::REPO_URL, rel.release);
        app.set_app_update_text(trf("pu_other_os", &url).into());
        let opener = if cfg!(target_os = "macos") { "open" } else { "xdg-open" };
        let _ = std::process::Command::new(opener).arg(&url).spawn();
        return;
    }
    app.set_busy(true);
    app.set_app_update_text(tr("pu_downloading").into());
    let updates = with(|s| s.paths.updates());
    let w = ui.as_weak();
    worker::spawn(
        move || {
            use ddm32_core::update::self_update as su;
            (|| -> Result<(PathBuf, Vec<String>), String> {
                let zip = client::download_app(&rel.entry, &updates, &HttpReleaseNet::real(), None)
                    .map_err(|e| e.to_string())?;
                let staged = su::stage(&zip, &updates, &rel.entry.version).map_err(|e| e.0)?;
                let exe = std::env::current_exe().map_err(|e| e.to_string())?;
                let app_dir = exe.parent().ok_or("no program folder")?.to_path_buf();
                Ok(su::helper_command(
                    &staged,
                    std::process::id(),
                    &app_dir,
                    &updates.join("started.json"),
                    &updates.join(su::RESULT_FILE),
                ))
            })()
        },
        move |r| {
            // a panic in the download or the staging ends here too (its text shown)
            let r = r.and_then(|x| x);
            let Some(ui) = w.upgrade() else { return };
            let app = ui.global::<App>();
            app.set_busy(false);
            // the program closes for the swap: not if anything started meanwhile (the download is redone later)
            if blocked_besides_prompt(&ui) {
                app.set_app_update_text(tr("pu_blocked").into());
                return;
            }
            match r.and_then(|(helper, args)| {
                std::process::Command::new(&helper).args(&args).spawn().map_err(|e| e.to_string()).map(|_| ())
            }) {
                Ok(()) => {
                    app.set_app_update_text(tr("pu_restarting").into());
                    let _ = slint::quit_event_loop();
                }
                Err(e) => app.set_app_update_text(trf("pu_failed", &e).into()),
            }
        },
    );
}

/// Asks GitHub for the signed release list in the background. A failure is never swallowed: it is written to
/// `logs/update-check.log` and shown on Home (dismissable). `ask`: the start-up / 24 h check, which may ask the one
/// update question; the Check button only refreshes the texts.
/// The versions page's "Check for updates" (crate::versions::check_now).
pub(crate) fn start_catalog_check(w: Weak<AppWindow>, ask: bool) {
    load_catalog(w, ask);
}

fn load_catalog(w: Weak<AppWindow>, ask: bool) {
    let releases = with(|s| {
        s.last_check = Some(std::time::Instant::now());
        s.paths.releases()
    });
    worker::spawn(
        move || client::load_catalog(&releases, &HttpReleaseNet::real(), None, None).map_err(|e| e.0),
        move |r| {
            let Some(ui) = w.upgrade() else { return };
            catalog_loaded(&ui, r.and_then(|x| x), ask);
        },
    );
}

/// The end of a release-list check (also driven by tests with a made-up result): the list replaces the cached one
/// and, for the start-up check, the one question is considered; a failure goes to logs/update-check.log and Home.
pub fn catalog_loaded(ui: &AppWindow, r: Result<Catalog, String>, ask: bool) {
    match r {
        Ok(mut cat) => {
            let mut pending_note = None;
            with(|s| {
                // a local release folder stays in the list (developer builds)
                if let Some(local) = &s.local {
                    cat.add_local(local);
                }
                if let Some(t) = &cat.checked_utc {
                    let r = s.settings.save("last_check_utc", serde_json::Value::String(t.clone()));
                    pending_note = Some(r);
                }
                s.catalog = Some(cat);
                s.check_error = None;
                s.check_error_raw = None;
                s.checking = false;
            });
            if let Some(r) = pending_note {
                noted(r, "last_check_utc");
            }
            refresh(ui);
            versions::show(ui);
            if ask {
                consider_prompt(ui);
            }
        }
        Err(e) => {
            let logs = with(|s| s.paths.logs());
            let _ = updates::log_check_error(&logs, &client::now_utc(), &e);
            let text = trf("check_failed", &e);
            with(|s| {
                s.check_error = Some(text);
                s.check_error_raw = Some(e.clone());
                s.checking = false;
            });
            refresh(ui);
            versions::show(ui);
        }
    }
}

fn set_step(ui: &AppWindow, step: i32) {
    let app = ui.global::<App>();
    app.set_step(step);
    app.set_step_done(false);
    clear_progress(&app);
    match step {
        2 => app.set_power_prompt(tr("power_backup").into()),
        4 => app.set_power_prompt(tr("power_install").into()),
        _ => {}
    }
    if step < 3 {
        app.set_details("".into());
    }
    set_message(ui, "", "info");
    mappack::show(ui);
}

/// What Home says about the cable: Windows' device list (a missing CH340 driver is found without opening a port),
/// else the serial ports of CH340 cables. (text, notice kind)
pub fn cable_text(
    lang: &str,
    status: &ddm32_core::radio::cable::CableStatus,
    ports: &[String],
) -> (String, &'static str) {
    use ddm32_core::radio::cable::CableState;
    let code = status.error_code.to_string();
    match status.state {
        CableState::Ok => (i18n::tf(lang, "radio_ready", status.port.as_deref().unwrap_or("")), "success"),
        CableState::NoDriver => (i18n::tf(lang, "cable_nodriver", &code), "warning"),
        CableState::Problem => (i18n::tf(lang, "cable_problem", &code), "warning"),
        CableState::NoPort => (i18n::t(lang, "cable_noport"), "warning"),
        CableState::NoCable => match ports.first() {
            Some(p) => (i18n::tf(lang, "radio_ready", p), "success"),
            None => (i18n::t(lang, "radio_nocable"), "info"),
        },
    }
}

fn wizard_next(ui: &AppWindow) {
    if !radio_allowed(ui) {
        return;
    }
    let app = ui.global::<App>();
    match app.get_step() {
        0 => {
            let i = app.get_selected_version() as usize;
            let Some(fv) = with(|s| s.shown.get(i).cloned()) else { return };
            // a test / alpha version: Yes / No first, No by default (the C# VersionsPage.OnSwitch)
            let test = catalog::needs_confirmation(catalog::label_for(
                &fv,
                &with(|s| s.catalog.clone().unwrap_or_default().firmware),
            ));
            if test && with(|s| s.test_ok.as_deref() != Some(fv.version())) {
                app.set_test_confirm_text(trf("ver_test_confirm", fv.version()).into());
                app.set_test_confirm_open(true);
                return;
            }
            app.set_busy(true);
            set_message(ui, &tr("msg_downloading"), "info");
            let releases = with(|s| s.paths.releases());
            let w = ui.as_weak();
            worker::spawn(
                move || {
                    let net = HttpReleaseNet::real();
                    let r = match &fv.source {
                        Some(local) => client::download_firmware(&fv, &releases, local.as_ref(), None, None),
                        None => client::download_firmware(&fv, &releases, &net, None, None),
                    };
                    r.map_err(|e| e.to_string())
                },
                move |r| {
                    // a panic in the download ends here too (busy cleared, its text shown)
                    let r = r.and_then(|x| x);
                    let Some(ui) = w.upgrade() else { return };
                    ui.global::<App>().set_busy(false);
                    match r {
                        Ok(folder) => {
                            let p = Package::new(&folder, None);
                            with(|s| {
                                s.package = Some(folder);
                                s.wizard
                                    .set_package(Some(Installable {
                                        sha256: p.sha256.clone(),
                                        installable: p.installable(),
                                    }))
                                    .ok();
                            });
                            set_step(&ui, 1);
                        }
                        Err(e) => set_message(&ui, &trf("msg_download_failed", &e), "danger"),
                    }
                },
            );
        }
        1 => match app.get_backup_choice() {
            0 => set_step(ui, 2),
            1 => {
                let q = with(|s| s.quick.clone());
                if let Some(q) = q {
                    with(|s| s.wizard.use_quick_backup(&q)).ok();
                    check(ui);
                }
            }
            _ => app.set_confirm_open(true),
        },
        2 => check(ui),
        3 => set_step(ui, 4),
        _ => {}
    }
}

/// Detection and plan (files only), then the plain-words summary; the numbers go under Details.
fn check(ui: &AppWindow) {
    set_step(ui, 3);
    let app = ui.global::<App>();
    let mut stock_note = None;
    let r: Result<(bool, String), String> = with(|s| {
        s.wizard.start_detect().map_err(|e| e.0.to_string())?;
        let backup = s.wizard.backup_dir.clone().unwrap_or_default();
        match FileSteps::detect_backup(&backup, s.codec) {
            Ok(d) => s.wizard.detect_finished(Some(d), ""),
            Err(e) => {
                s.wizard.detect_finished(None, &e.record());
                return Err(e.record());
            }
        }
        let stock = s.wizard.detection.as_ref().and_then(|d| d.str("layout")) == Some("stock");
        if stock {
            // kept for "Back to stock firmware" (the C# RememberStockBackup)
            stock_note = Some(restore::remember_stock_backup(&s.settings, &backup));
        }
        s.wizard.start_plan().map_err(|_| i18n::t(&s.lang, s.wizard.message.0))?;
        let plus = s.package.clone().unwrap_or_default();
        let mode = s.wizard.mode().unwrap_or("install_plus");
        match FileSteps::plan(mode, &backup, &plus, None, s.codec) {
            Ok(p) => {
                let safe = FileSteps::safe(&p);
                let text = pyjson::dumps2(&Py::Dict(p.clone()));
                s.wizard.plan_finished(Some(p), "", safe);
                if !safe {
                    return Err("unsafe plan".into());
                }
                Ok((stock, text))
            }
            Err(e) => {
                s.wizard.plan_finished(None, &e.record(), true);
                Err(e.record())
            }
        }
    });
    if let Some(n) = stock_note {
        noted(n, "stock_backups");
    }
    match r {
        Ok((stock, details)) => {
            app.set_summary(tr(if stock { "msg_check_stock" } else { "msg_check_ok" }).into());
            app.set_details(details.into());
            app.set_step_done(true);
        }
        Err(e) => {
            app.set_summary("".into());
            set_message(ui, &trf("msg_check_failed", &e), "danger");
        }
    }
}

fn clear_progress(app: &App<'_>) {
    app.set_progress(0.0);
    app.set_progress_text("".into());
    app.set_progress_info("".into());
}

/// What one tool line changes in the progress area: the bar (0..1), the percent line, the information line.
/// `None` leaves that part as it is, so a JEDEC or "boot ROM answered" line never replaces the percent.
#[derive(Debug, Default, Clone, PartialEq)]
pub struct ProgressShow {
    pub value: Option<f32>,
    pub text: Option<String>,
    pub info: Option<String>,
}

impl ProgressShow {
    pub fn apply(&self, app: &App<'_>) {
        if let Some(v) = self.value {
            app.set_progress(v);
        }
        if let Some(t) = &self.text {
            app.set_progress_text(t.into());
        }
        if let Some(i) = &self.info {
            app.set_progress_info(i.into());
        }
    }
}

/// A tool line turned into what the progress area shows, in a language (the bar is the run's overall percent).
pub fn progress_show(view: &mut ProgressView, lang: &str, line: &str) -> ProgressShow {
    match view.feed(line) {
        ProgressEvent::Percent { key, phase, overall } => ProgressShow {
            value: Some(overall as f32 / 100.0),
            text: Some(i18n::tf(lang, key, &phase.to_string())),
            info: None,
        },
        ProgressEvent::Ready => ProgressShow { text: Some(i18n::t(lang, "power_now")), ..Default::default() },
        ProgressEvent::Info(s) => ProgressShow { info: Some(s), ..Default::default() },
        ProgressEvent::Ignore => ProgressShow::default(),
    }
}

/// Runs one radio tool in a worker thread; its lines feed the progress bar; `done` gets the result in the UI thread.
fn run_tool(
    ui: &AppWindow,
    tool: &'static str,
    argv: Vec<String>,
    writes: bool,
    done: impl FnOnce(&AppWindow, Option<ToolError>) + Send + 'static,
) {
    run_tool_lines(ui, tool, argv, writes, progress_line, done);
}

/// A tool line into the wizard's / restore's progress area.
fn progress_line(ui: &AppWindow, line: &str) {
    let show = with(|s| progress_show(&mut s.progress, &s.lang, line));
    show.apply(&ui.global::<App>());
}

/// run_tool with its own handler of the tool's lines (the data pack tool's slots, plan and sectors).
pub(crate) fn run_tool_lines(
    ui: &AppWindow,
    tool: &'static str,
    argv: Vec<String>,
    writes: bool,
    on_line: fn(&AppWindow, &str),
    done: impl FnOnce(&AppWindow, Option<ToolError>) + Send + 'static,
) {
    // one radio job at a time, whatever called: refused (as a step that did not start) while another holds the port
    if port_busy(ui) {
        done(ui, Some(ToolError::value("another radio job holds the port; nothing was started")));
        return;
    }
    let app = ui.global::<App>();
    app.set_busy(true);
    clear_progress(&app);
    with(|s| {
        s.progress = ProgressView::new();
        s.writing = writes;
    });
    if writes {
        // the radio is written from now on: "Close anyway?" no longer applies
        app.set_force_close_open(false);
    }
    let w = ui.as_weak();
    let w2 = w.clone();
    worker::spawn(
        move || {
            let clock = SystemClock::default();
            let print = move |line: &str| {
                let line = line.to_string();
                let w3 = w2.clone();
                let _ = slint::invoke_from_event_loop(move || {
                    let Some(ui) = w3.upgrade() else { return };
                    on_line(&ui, &line);
                });
            };
            let serial = move |p: &str| -> ToolResult<Box<dyn RawSerial>> {
                if writes {
                    RadioAccess::write_factory(p)
                } else {
                    RadioAccess::factory(p)
                }
            };
            let ctx = ToolContext {
                print: &print,
                clock: &clock,
                serial: &serial,
                release_key_hex: None,
                codec: Codec::dm32(),
            };
            match tool {
                "backup" => tools::backup_tool(&argv, &ctx),
                "install_plus" => tools::install_tool(&argv, &ctx),
                "install_firstboot" => tools::restore_stock_tool(&argv, &ctx),
                "fast_install" => fast_install::fast_install_tool(&argv, &ctx),
                "datapack" => cps::data_pack_tool(&argv, &ctx),
                other => Err(ToolError::value(format!("unknown tool {other}"))),
            }
        },
        move |r| {
            let Some(ui) = w.upgrade() else { return };
            let err = tool_ended(&ui, r);
            done(&ui, err);
        },
    );
}

/// The end of a radio tool, also after a panic in its worker (then an error with the panic's text): the window is
/// not busy any more and no write runs; the tool's error, if any, for the step's own message.
pub fn tool_ended(ui: &AppWindow, r: Result<ToolResult<()>, String>) -> Option<ToolError> {
    let app = ui.global::<App>();
    app.set_busy(false);
    with(|s| s.writing = false);
    let r = r.unwrap_or_else(|panic| Err(worker::panic_error(&panic)));
    if r.is_ok() && app.get_progress() > 0.0 {
        // a tool's last percent line may stop short of 100 (the readback prints every 25 %)
        app.set_progress(1.0);
    }
    r.err()
}

fn s(p: &Path) -> String {
    p.to_string_lossy().into_owned()
}

/// A read-only reader (the search log or the live view) holds the radio's port: the radio steps wait (said on screen).
/// On Linux / macOS a second open of the same tty would succeed and the two streams would mix.
/// Another radio job holds the port now (a step, a health read, the live view, the search log): a Yes that comes
/// late (a question left open) starts nothing; said on screen.
pub(crate) fn port_busy(ui: &AppWindow) -> bool {
    let r = ui.global::<Radio>();
    let busy = ui.global::<App>().get_busy() || r.get_busy() || r.get_live_running() || r.get_slog_running();
    if busy {
        set_message(ui, &tr("slog_busy_other"), "warning");
        r.set_message(tr("slog_busy_other").into());
        r.set_message_kind("warning".into());
    }
    busy
}

pub(crate) fn reader_holds_port(ui: &AppWindow) -> bool {
    let held = with(|st| st.slog_cancel.is_some() || st.live_cancel.is_some());
    if held {
        set_message(ui, &tr("slog_busy"), "warning");
    }
    held
}

fn start_backup(ui: &AppWindow) {
    if !radio_allowed(ui) || reader_holds_port(ui) || port_busy(ui) {
        return;
    }
    with_port(ui, start_backup_on);
}

/// start_backup once the port is known (crate::with_port).
fn start_backup_on(ui: &AppWindow, found: Option<String>) {
    // the gates again: the listing took a moment, the disclaimer or a reader may have changed meanwhile; nothing of
    // the wizard moves before they pass
    if !radio_allowed(ui) || reader_holds_port(ui) || port_busy(ui) {
        return;
    }
    let Some(port) = found else {
        set_message(ui, &tr("radio_nocable"), "warning");
        return;
    };
    let out = with(|st| backups::new_backup_folder(&st.paths, &today()));
    let ok = with(|st| st.wizard.set_port(Some(&port)).is_ok() && st.wizard.start_backup(&out).is_ok());
    if !ok {
        return;
    }
    set_message(ui, &tr("msg_backup_running"), "info");
    let argv = vec!["--port".into(), port, "--out".into(), s(&out), "--installed-firmware".into(), "unknown".into()];
    run_tool(ui, "backup", argv, false, move |ui, err| {
        let r = StepResult::backup(&out, err.as_ref());
        with(|st| st.wizard.backup_finished(&r));
        if r.passed {
            ui.global::<App>().set_step_done(true);
            ui.global::<App>().set_progress(1.0);
            set_message(ui, &tr("msg_backup_done"), "success");
        } else if r.timeout {
            set_message(ui, &tr("msg_backup_timeout"), "warning");
        } else {
            set_message(ui, &trf("msg_backup_failed", &r.error), "danger");
        }
        refresh(ui);
    });
}

fn start_install(ui: &AppWindow) {
    if !radio_allowed(ui) || reader_holds_port(ui) || port_busy(ui) {
        return;
    }
    with_port(ui, start_install_on);
}

/// start_install once the port is known (crate::with_port).
fn start_install_on(ui: &AppWindow, found: Option<String>) {
    // the gates again: the listing took a moment, the disclaimer or a reader may have changed meanwhile; nothing of
    // the wizard moves before they pass
    if !radio_allowed(ui) || reader_holds_port(ui) || port_busy(ui) {
        return;
    }
    let Some(port) = found else {
        set_message(ui, &tr("radio_nocable"), "warning");
        return;
    };
    // what goes on the radio, recorded after a verified install (settings.json, the C# program's keys too)
    let (pkg_sha, pkg_version) = with(|st| {
        let p = Package::new(&st.package.clone().unwrap_or_default(), None);
        (p.sha256, p.version)
    });
    let (tool, argv, out) = match with(|st| -> Result<_, String> {
        st.wizard.set_port(Some(&port)).map_err(|e| e.0.to_string())?;
        // the install log goes to backups too (never anywhere the user picks)
        let out = unique_folder(&st.paths.backups(), &format!("install-{}", today()));
        let plus = st.package.clone().unwrap_or_default();
        let (tool, argv) = st.wizard.install_command(&s(&plus), &s(&out)).map_err(|e| e.0.to_string())?;
        st.wizard.start_install().map_err(|e| e.0.to_string())?;
        Ok((tool, argv, out))
    }) {
        Ok(x) => x,
        Err(e) => {
            set_message(ui, &trf("msg_failed", &e), "danger");
            return;
        }
    };
    set_message(ui, &tr("msg_installing"), "info");
    run_tool(ui, tool, argv, true, move |ui, err| {
        let r = StepResult::install(&out, err.as_ref());
        let mut prev_note = None;
        let none = with(|st| {
            st.wizard.install_finished(&r);
            let none = st.wizard.path == ddm32_core::radio::flow::BackupPath::None;
            let backup = if none { None } else { st.wizard.backup_dir.clone() };
            if let Some(b) = backup.as_deref().filter(|_| r.passed || r.writes_started) {
                // "Back to the previous version" restores it (the C# restore_previous_backup)
                prev_note = Some(restore::remember_previous_backup(&st.settings, b));
            }
            let _ = updates::record_install_result(
                &st.settings,
                &r,
                &pkg_sha,
                &pkg_version,
                backup.as_deref(),
                updates::now_unix(),
            );
            none
        });
        if let Some(n) = prev_note {
            noted(n, "restore_previous_backup");
        }
        if r.passed {
            set_step(ui, 5);
        } else if r.writes_started {
            set_message(
                ui,
                &trf(if none { "msg_install_stopped_none" } else { "msg_install_stopped" }, &r.error),
                "danger",
            );
            ui.global::<App>().set_step_done(true);
        } else {
            set_message(ui, &trf("msg_install_not_started", &r.error), "warning");
        }
        refresh(ui);
    });
}

fn restore_plan(ui: &AppWindow) {
    let i = ui.global::<App>().get_selected_backup();
    let Some(b) = with(|st| st.found.get(i as usize).cloned()) else { return };
    restore::choose(ui, None, &b.folder);
}

fn start_restore(ui: &AppWindow) {
    if !radio_allowed(ui) || reader_holds_port(ui) || port_busy(ui) {
        return;
    }
    with_port(ui, start_restore_on);
}

/// start_restore once the port is known (crate::with_port).
fn start_restore_on(ui: &AppWindow, found: Option<String>) {
    // the gates again: the listing took a moment, the disclaimer or a reader may have changed meanwhile
    if !radio_allowed(ui) || reader_holds_port(ui) || port_busy(ui) {
        return;
    }
    let Some(port) = found else {
        set_message(ui, &tr("radio_nocable"), "warning");
        return;
    };
    let r = with(|st| -> Result<_, String> {
        st.restore.port = Some(port.clone());
        let (tool, extra, _) = st.restore.tool().ok_or("no tool")?;
        let backup = st.restore.backup_dir.clone().unwrap_or_default();
        st.restore_backup = Some(backup.clone());
        let out = unique_folder(&st.paths.backups(), &format!("restore-{}", today()));
        st.restore.start_restore().map_err(|e| e.0.to_string())?;
        let mut argv = vec![
            "--port".into(),
            port.clone(),
            "--backup".into(),
            s(&backup),
            "--out".into(),
            s(&out),
            "--wait".into(),
            "600".into(),
        ];
        argv.extend(extra.iter().map(|x| x.to_string()));
        Ok((tool, argv, out))
    });
    let (tool, argv, out) = match r {
        Ok(x) => x,
        Err(e) => {
            set_message(ui, &trf("msg_failed", &e), "danger");
            return;
        }
    };
    ui.global::<App>().set_restore_ready(false);
    set_message(ui, &tr("power_install"), "warning");
    let tool: &'static str = if tool == "install_firstboot" { "install_firstboot" } else { "install_plus" };
    run_tool(ui, tool, argv, true, move |ui, err| {
        let r = StepResult::install(&out, err.as_ref());
        with(|st| {
            st.restore.restore_finished(&r);
            if let Some(b) = st.restore_backup.take() {
                let _ = updates::record_restore_result(
                    &st.settings,
                    &r,
                    &b,
                    tool == "install_firstboot",
                    updates::now_unix(),
                );
            }
        });
        if r.passed {
            set_message(ui, &tr("msg_restore_done"), "success");
        } else if r.writes_started {
            set_message(ui, &trf("msg_install_stopped", &r.error), "danger");
        } else {
            set_message(ui, &trf("msg_install_not_started", &r.error), "warning");
        }
        restore::show_status(ui);
        refresh(ui);
    });
}

/// Example contents for the screenshots and the demo mode (no file, no network, no radio).
pub fn demo(ui: &AppWindow) {
    let app = ui.global::<App>();
    let lang = ui.global::<Tr>().get_lang().to_string();
    let t = |k: &str| SharedString::from(i18n::t(&lang, k));
    app.set_radio_text(i18n::tf(&lang, "radio_ready", "COM14").into());
    app.set_radio_kind("success".into());
    app.set_radio_access(true);
    let known = Known {
        installed: Installed::Plus { version: "1.0.0".into(), label: i18n::t(&lang, "label_stable") },
        source: updates::Source::Install,
        may_prompt: true,
    };
    let st = updates::FirmwareStatus::Newer { installed: "1.0.0".into(), newest: "1.0.1".into() };
    app.set_update_text(updates::firmware_status_text(&lang, &st, known.source, &[], &[]).into());
    app.set_backups_text(i18n::tf(&lang, "backups_found", "3").into());
    app.set_installed_text(updates::installed_text(&known.installed, &lang).into());
    app.set_version_line(updates::version_text(&lang, ddm32_core::app_info::VERSION, &known).into());
    app.set_title_text(
        format!("{}{}", updates::title_text(&lang, ddm32_core::app_info::VERSION, &known), dev_title(&lang)).into(),
    );
    app.set_installed_firmware(updates::installed_short(&known, &lang).into());
    app.set_port_missing("".into());
    app.set_check_error("".into());
    app.set_prompt_open(false);
    app.set_disc_open(false);
    app.set_test_confirm_open(false);
    app.set_force_close_open(false);
    app.set_res_confirm_open(false);
    ui.global::<Radio>().set_zone_picker_open(false);
    ui.global::<Radio>().set_health_auto(false);
    ui.global::<Radio>().set_disclaimer_ok(true);
    demo_ports(ui, &lang);
    app.set_app_update_text(i18n::tf(&lang, "pu_available", "3.1.0").into());
    app.set_app_update_available(true);
    radio_pages::demo(ui, &lang);
    app.set_versions(ModelRc::new(VecModel::from(vec![
        VersionRow {
            version: "1.0.0".into(),
            label: t("label_latest"),
            notes: "Kararlı sürüm / stable".into(),
            local: false,
        },
        VersionRow { version: "1.1.0-beta.2".into(), label: t("label_beta"), notes: "Beta".into(), local: false },
        VersionRow { version: "1.2.0-alpha.1".into(), label: t("label_test"), notes: "Test".into(), local: false },
    ])));
    app.set_quick_available(true);
    app.set_quick_backup("dm32-20261120".into());
    app.set_summary(t("msg_check_ok"));
    app.set_details("{\n  \"mode\": \"install_plus\",\n  \"changed_sectors\": 23,\n  \"boot_area_unchanged\": true,\n  \"data_chip_written\": false\n}".into());
    app.set_backups(ModelRc::new(VecModel::from(vec![
        BackupRow {
            name: "dm32-20261121".into(),
            detail: "…/DDM32 Plus/backups/dm32-20261121".into(),
            origin: "own".into(),
            kind: "".into(),
        },
        BackupRow {
            name: "dm32-20261120".into(),
            detail: "…/DDM32 Plus/backups/dm32-20261120".into(),
            origin: "own".into(),
            kind: "".into(),
        },
        BackupRow {
            name: "dm32-demo-20260101".into(),
            detail: "…/DDM25Plus/yedekler/dm32-demo-20260101".into(),
            origin: "python".into(),
            kind: "".into(),
        },
    ])));
    app.set_backups_folder("C:\\Users\\…\\AppData\\Local\\Programs\\DDM32 Plus\\backups".into());
    packs::demo(ui, &lang);
    help::show_in(ui, &lang, "C:\\Users\\…\\AppData\\Local\\Programs\\DDM32 Plus\\backups");
    app.set_res_status(
        format!(
            "{}\n{}",
            i18n::tf(&lang, "res_chosen", "dm32-20261121"),
            i18n::tn(&lang, "res_plan_summary", &["23", "0x0C000", "0x22000"])
        )
        .into(),
    );
    demo_versions(ui, &lang);
    app.set_version(ddm32_core::app_info::VERSION.into());
    app.set_key_id(ddm32_core::releases::PUBLIC_KEY_ID.into());
    app.set_data_folder("C:\\Users\\…\\AppData\\Local\\Programs\\DDM32 Plus".into());
}

/// A page by its `--show` name (the C# names, and this program's own).
pub fn show_page_key(key: &str) -> Option<i32> {
    Some(match key {
        "home" => 0,
        "update" => 1,
        "restore" | "backups" => 2,
        "packs" => 3,
        "settings" => 4,
        "about" => 5,
        "health" => 7,
        "live" => 8,
        "channels" => 9,
        "searchlog" => 10,
        "versions" => 11,
        "help" => 12,
        _ => return None,
    })
}

/// `--show PAGE[:demo|:FOLDER]` (screenshots, demos; the C# MainWindow.ShowFromArgs): opens a page; health:demo and
/// live:demo show made-up readings, channels:FOLDER that backup's codeplug. Navigation and file reads only: a radio
/// step still needs its own button. False when the page is unknown.
pub fn show_from_args(ui: &AppWindow, spec: &str) -> bool {
    let (key, rest) = match spec.split_once(':') {
        Some((k, r)) => (k, Some(r)),
        None => (spec, None),
    };
    let Some(page) = show_page_key(key) else { return false };
    navigate(ui, page);
    let lang = with(|s| s.lang.clone());
    match (key, rest) {
        ("health" | "live", Some("demo")) => {
            radio_pages::demo(ui, &lang);
            // made-up readings only: no reader runs, so nothing holds the port
            ui.global::<Radio>().set_live_running(false);
        }
        ("channels", Some(folder)) if !folder.is_empty() => channels::open_backup(ui, Path::new(folder)),
        _ => {}
    }
    true
}

/// Example versions page (screenshots, demo): a newer stable, the one on the radio, a beta, a test build, a legacy one.
pub fn demo_versions(ui: &AppWindow, lang: &str) {
    let app = ui.global::<App>();
    let t = |k: &str| i18n::t(lang, k);
    let row = |v: &str, label: &str, kind: &str, st: Vec<String>, notes: &str| VerRow {
        version: v.into(),
        label: t(label).into(),
        kind: kind.into(),
        state: st.join(", ").into(),
        notes: notes.into(),
        notes_both: format!("TR: {notes}\nEN: {notes}").into(),
        installable: kind != "legacy",
    };
    app.set_ver_rows(ModelRc::new(VecModel::from(vec![
        row(
            "1.2.0-alpha.1",
            "label_test",
            "test",
            vec![t("state_newer"), t("state_not_downloaded"), t("state_test")],
            "P25 search engine test",
        ),
        row("1.1.0-beta.2", "label_beta", "beta", vec![t("state_newer"), t("state_downloaded")], "Beta: channels view"),
        row("1.0.1", "label_latest", "latest", vec![t("state_newer"), t("state_not_downloaded")], "DMR audio fix"),
        row(
            "1.0.0",
            "label_stable",
            "stable",
            vec![t("state_on_radio"), t("state_downloaded")],
            "First stable release",
        ),
        row("1.0.0-beta.3", "label_legacy", "legacy", vec![t("state_older"), t("state_legacy")], ""),
    ])));
    app.set_ver_selected(2);
    app.set_ver_checking(false);
    app.set_ver_status(i18n::tf(lang, "ver_checked", "5").into());
    app.set_ver_status_kind("success".into());
    app.set_ver_last_check(versions::last_check(lang, Some("2026-11-24T09:41:00Z")).into());
    app.set_ver_banner(
        i18n::t(lang, "update_newer")
            .replacen("{}", &format!("1.0.1 ({})", label_in(lang, ReleaseLabel::Latest)), 1)
            .replacen("{}", "1.0.0", 1)
            .into(),
    );
    app.set_ver_rejected(
        format!("{}\nv0.9.9-test: signature does not verify\nv0.9.8: no signed manifest", t("ver_rejected")).into(),
    );
    app.set_ver_key(i18n::tf(lang, "ver_key", ddm32_core::releases::PUBLIC_KEY_ID).into());
    app.set_ver_note("".into());
    app.set_ver_details(false);
}

/// Example cable picker contents (screenshots, demo): two CH340 cables, Auto on COM14, a USB serial adapter and a
/// com0com pair under Advanced.
pub fn demo_ports(ui: &AppWindow, lang: &str) {
    let entries = [
        portpick::SerialPortEntry::new("COM14", true, false),
        portpick::SerialPortEntry::new("COM15", true, false),
        portpick::SerialPortEntry::new("COM3", false, false),
        portpick::SerialPortEntry::new("COM12", false, true),
        portpick::SerialPortEntry::new("COM13", false, true),
    ];
    let opts = portpick::options(&entries, Os::Windows);
    let now = portpick::resolve(&PortPref::default(), &opts, None);
    let v = port_view(lang, &opts, &now, false);
    let r = ui.global::<Radio>();
    r.set_access(true);
    r.set_port_head(v.head.into());
    r.set_port_line(v.line.into());
    r.set_port_note(v.note.into());
    r.set_port_auto(true);
    r.set_port_selected(now.port.unwrap_or_default().into());
    r.set_port_auto_detail(v.auto_detail.into());
    set_port_rows(&r, v.cables, v.others);
    r.set_picker_open(false);
    r.set_picker_advanced(false);
}

/// The versions the wizard offers now (tests: which one "Update now" selected).
pub fn with_shown<R>(f: impl FnOnce(&[catalog::FirmwareVersion]) -> R) -> R {
    with(|s| f(&s.shown))
}
