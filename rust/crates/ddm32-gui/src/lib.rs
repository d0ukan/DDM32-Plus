// SPDX-License-Identifier: GPL-3.0-or-later
//! The DDM32 Plus window: the Slint UI (ui/*.slint) and the controller that runs ddm32-core behind it.
//!
//! Every radio step starts only from its own button (one click = one step) and runs in a worker thread; its lines
//! come back to the window as progress. Nothing here retries a failed write. Backups always go to the program's own
//! `backups` and are found by themselves (ddm32_core::storage::backups); the user never chooses a folder.

pub mod i18n;
pub mod radio_pages;

slint::include_modules!();

use ddm32_core::datapacks::files::{self as packs, BuiltPack};
use ddm32_core::pyjson::{self, Py};
use ddm32_core::radio::codec::Codec;
use ddm32_core::radio::flow::{FileSteps, Installable, RestoreFlow, StepResult, Wizard};
use ddm32_core::radio::port::{cable_ports, RadioAccess, RawSerial, SystemClock};
use ddm32_core::radio::progress::{ProgressEvent, ProgressView};
use ddm32_core::radio::tools::ToolContext;
use ddm32_core::radio::{cps, fast_install, tools, ToolError, ToolResult};
use ddm32_core::releases::catalog::{self, Catalog, ReleaseLabel};
use ddm32_core::releases::client::{self, Package};
use ddm32_core::releases::net::HttpReleaseNet;
use ddm32_core::storage::backups::{self, FoundBackup};
use ddm32_core::storage::paths::{real_env, unique_folder};
use ddm32_core::storage::{today, AppPaths, Os, Settings, Theme as SavedTheme};
use slint::{ComponentHandle, ModelRc, SharedString, VecModel, Weak};
use std::cell::RefCell;
use std::path::{Path, PathBuf};

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
    pub pack: Option<PathBuf>,
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
}

thread_local! {
    static STATE: RefCell<Option<State>> = const { RefCell::new(None) };
}

pub(crate) fn with<R>(f: impl FnOnce(&mut State) -> R) -> R {
    STATE.with(|s| f(s.borrow_mut().as_mut().expect("controller set up")))
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
    use ddm32_core::releases::signed_manifest::sha256_hex;
    const START: usize = 0xC000;
    for fv in all {
        let n = fv.application_size() as usize;
        if program.len() >= START + n && sha256_hex(&program[START..START + n]) == fv.application_sha256() {
            return i18n::tf(lang, "installed", &format!("DDM32 Plus {}", version_line(fv, all, lang)));
        }
    }
    let what = match ddm32_core::radio::unit_image::detect(program, Codec::dm32()) {
        Ok(d) => match (d.str("layout"), d.str("firmware")) {
            (Some("stock"), _) => i18n::tf(lang, "fw_stock", d.str("stock_version").unwrap_or("")),
            (_, Some("ddm32")) => i18n::t(lang, "fw_ddm32_unlisted"),
            (_, Some("classic")) => i18n::t(lang, "fw_classic"),
            _ => i18n::t(lang, "fw_unknown"),
        },
        Err(_) => i18n::t(lang, "fw_unknown"),
    };
    i18n::tf(lang, "installed", &what)
}

/// A version as the home screen names it: the full version with its prerelease and its label,
/// e.g. "1.0.0-beta.5 (Beta)".
pub fn version_line(fv: &catalog::FirmwareVersion, all: &[catalog::FirmwareVersion], lang: &str) -> String {
    format!("{} ({})", fv.version(), label_in(lang, catalog::label_for(fv, all)))
}

/// Sets up the window: the state, every callback, the first contents. `fetch` loads the release list in the
/// background when the settings allow it.
pub fn setup(ui: &AppWindow, paths: AppPaths, fetch: bool) {
    install_texts(ui);
    let _ = paths.ensure();
    let settings = Settings::new(paths.settings_file());
    let first_start = settings.language().is_none();
    let lang = settings.language().unwrap_or_else(|| "tr".into());
    ui.global::<Tr>().set_lang(lang.clone().into());
    ui.global::<Theme>().set_dark(settings.theme() == SavedTheme::Dark);
    let app = ui.global::<App>();
    app.set_channel(settings.load().get("channel").and_then(|v| v.as_i64()).unwrap_or(0) as i32);
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
            pack: None,
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
        })
    });
    wire(ui);
    radio_pages::wire(ui);
    refresh(ui);
    if first_start {
        app.set_page(6);
    }
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
        load_catalog(ui.as_weak());
    }
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
        with(|s| s.settings.save("channel", serde_json::json!(c))).ok();
        refresh(&ui);
    });
    app.on_set_auto_check(move |v| {
        with(|s| s.settings.set_auto_check(v)).ok();
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
    app.on_restore(move || start_restore(&w.unwrap()));
    let w = ui.as_weak();
    app.on_pack_radioid(move || pack_radioid(&w.unwrap()));
    let w = ui.as_weak();
    app.on_pack_air(move || pack_air(&w.unwrap()));
    let w = ui.as_weak();
    app.on_pack_write(move || pack_write(&w.unwrap()));
    let w = ui.as_weak();
    app.on_app_check(move || {
        let ui = w.unwrap();
        ui.global::<App>().set_app_update_text(tr("pu_checking").into());
        load_catalog(ui.as_weak());
    });
    let w = ui.as_weak();
    app.on_app_update(move || app_update(&w.unwrap()));
    app.on_open_backups_folder(move || {
        let dir = with(|s| s.paths.backups());
        open_folder(&dir);
    });
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

fn set_message(ui: &AppWindow, text: &str, kind: &str) {
    let app = ui.global::<App>();
    app.set_message(text.into());
    app.set_message_kind(kind.into());
}

fn navigate(ui: &AppWindow, page: i32) {
    let app = ui.global::<App>();
    if app.get_busy() {
        return;
    }
    set_message(ui, "", "info");
    clear_progress(&app);
    app.set_details("".into());
    app.set_show_details(false);
    if page == 1 {
        with(|s| {
            s.wizard = Wizard::new();
            s.package = None;
        });
        app.set_backup_choice(0);
        app.set_understood(false);
        set_step(ui, 0);
    }
    if page == 9 {
        radio_pages::load_channels(ui);
    }
    if page == 2 {
        app.set_selected_backup(-1);
        app.set_restore_ready(false);
    }
    refresh(ui);
    app.set_page(page);
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
    // the cable (listing ports never opens one)
    if !RadioAccess::ENABLED {
        app.set_radio_text(tr("radio_disabled").into());
        app.set_radio_kind("warning".into());
    } else {
        let (text, kind) = with(|s| cable_text(&s.lang, &ddm32_core::radio::cable::check(), &cable_ports()));
        app.set_radio_text(text.into());
        app.set_radio_kind(kind.into());
    }
    // versions
    let rows = with(|s| {
        let cat = s.catalog.clone().unwrap_or_default();
        s.shown = offered(&cat, channel);
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
    app.set_update_text(match rows.first() {
        Some(f) => trf("update_available", &version_line(f, &all, &lang)).into(),
        None => tr("update_none").into(),
    });
    if app.get_selected_version() as usize >= model.len() {
        app.set_selected_version(0);
    }
    app.set_versions(ModelRc::new(VecModel::from(model)));
    // backups, found by themselves
    let (found, quick) = with(|s| {
        let roots = backups::backup_roots(&s.paths, &s.settings, Os::current(), &real_env);
        s.found = backups::discover(&roots);
        let dirs: Vec<PathBuf> = roots.iter().map(|r| r.folder.clone()).collect();
        s.quick = backups::last_for_quick_update(&dirs, Codec::dm32(), 8);
        (s.found.clone(), s.quick.clone())
    });
    app.set_backups_text(trf("backups_found", &found.len().to_string()).into());
    // what the radio runs, by the newest verified backup (files only)
    let installed = found
        .iter()
        .find_map(|b| ddm32_core::radio::unit_image::saved_images(&b.folder).ok())
        .map(|imgs| installed_line(&imgs["progmem-pass1.bin"], &all, &lang))
        .unwrap_or_else(|| tr("installed_none"));
    app.set_installed_text(installed.into());
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
}

/// The newer build of this program for this platform that the channel offers (prereleases from Beta on).
pub fn newer_program(cat: &Catalog, current: &str, channel: i32) -> Option<catalog::AppRelease> {
    cat.newer_app(current, channel >= 1).cloned()
}

/// Downloads the new program (size and SHA-256 from the signed manifest), stages it, starts the staged program as
/// the helper that swaps the files after this program has closed, and closes. Windows only: on Linux and macOS the
/// package (AppImage, .app) is replaced from the download page.
fn app_update(ui: &AppWindow) {
    let app = ui.global::<App>();
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
    std::thread::spawn(move || {
        use ddm32_core::update::self_update as su;
        let r = (|| -> Result<(PathBuf, Vec<String>), String> {
            let zip =
                client::download_app(&rel.entry, &updates, &HttpReleaseNet::real(), None).map_err(|e| e.to_string())?;
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
        })();
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = w.upgrade() else { return };
            let app = ui.global::<App>();
            app.set_busy(false);
            match r.and_then(|(helper, args)| {
                std::process::Command::new(&helper).args(&args).spawn().map_err(|e| e.to_string()).map(|_| ())
            }) {
                Ok(()) => {
                    app.set_app_update_text(tr("pu_restarting").into());
                    let _ = slint::quit_event_loop();
                }
                Err(e) => app.set_app_update_text(trf("pu_failed", &e).into()),
            }
        });
    });
}

fn load_catalog(w: Weak<AppWindow>) {
    let releases = with(|s| s.paths.releases());
    std::thread::spawn(move || {
        let r = client::load_catalog(&releases, &HttpReleaseNet::real(), None, None);
        let _ = slint::invoke_from_event_loop(move || {
            if let (Ok(cat), Some(ui)) = (r, w.upgrade()) {
                with(|s| s.catalog = Some(cat));
                refresh(&ui);
            }
        });
    });
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
}

pub(crate) fn port() -> Option<String> {
    ddm32_core::radio::cable::check().port.or_else(|| cable_ports().into_iter().next())
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
    let app = ui.global::<App>();
    match app.get_step() {
        0 => {
            let i = app.get_selected_version() as usize;
            let Some(fv) = with(|s| s.shown.get(i).cloned()) else { return };
            if catalog::needs_confirmation(catalog::label_for(
                &fv,
                &with(|s| s.catalog.clone().unwrap_or_default().firmware),
            )) {
                set_message(ui, &tr("ch_test_d"), "warning");
            }
            app.set_busy(true);
            set_message(ui, &tr("msg_downloading"), "info");
            let releases = with(|s| s.paths.releases());
            let w = ui.as_weak();
            std::thread::spawn(move || {
                let net = HttpReleaseNet::real();
                let r = match &fv.source {
                    Some(local) => client::download_firmware(&fv, &releases, local.as_ref(), None, None),
                    None => client::download_firmware(&fv, &releases, &net, None, None),
                };
                let r = r.map_err(|e| e.to_string());
                let _ = slint::invoke_from_event_loop(move || {
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
                });
            });
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
    let r: Result<(bool, String), String> = with(|s| {
        s.wizard.start_detect().map_err(|e| e.0.to_string())?;
        let backup = s.wizard.backup_dir.clone().unwrap_or_default();
        match FileSteps::detect_backup(&backup, Codec::dm32()) {
            Ok(d) => s.wizard.detect_finished(Some(d), ""),
            Err(e) => {
                s.wizard.detect_finished(None, &e.record());
                return Err(e.record());
            }
        }
        let stock = s.wizard.detection.as_ref().and_then(|d| d.str("layout")) == Some("stock");
        s.wizard.start_plan().map_err(|_| i18n::t(&s.lang, s.wizard.message.0))?;
        let plus = s.package.clone().unwrap_or_default();
        let mode = s.wizard.mode().unwrap_or("install_plus");
        match FileSteps::plan(mode, &backup, &plus, None, Codec::dm32()) {
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
    let app = ui.global::<App>();
    app.set_busy(true);
    clear_progress(&app);
    with(|s| s.progress = ProgressView::new());
    let w = ui.as_weak();
    std::thread::spawn(move || {
        let clock = SystemClock::default();
        let w2 = w.clone();
        let print = move |line: &str| {
            let line = line.to_string();
            let w3 = w2.clone();
            let _ = slint::invoke_from_event_loop(move || {
                let Some(ui) = w3.upgrade() else { return };
                let show = with(|s| progress_show(&mut s.progress, &s.lang, &line));
                show.apply(&ui.global::<App>());
            });
        };
        let serial = move |p: &str| -> ToolResult<Box<dyn RawSerial>> {
            if writes {
                RadioAccess::write_factory(p)
            } else {
                RadioAccess::factory(p)
            }
        };
        let ctx =
            ToolContext { print: &print, clock: &clock, serial: &serial, release_key_hex: None, codec: Codec::dm32() };
        let r = match tool {
            "backup" => tools::backup_tool(&argv, &ctx),
            "install_plus" => tools::install_tool(&argv, &ctx),
            "install_firstboot" => tools::restore_stock_tool(&argv, &ctx),
            "fast_install" => fast_install::fast_install_tool(&argv, &ctx),
            "datapack" => cps::data_pack_tool(&argv, &ctx),
            other => Err(ToolError::value(format!("unknown tool {other}"))),
        };
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = w.upgrade() else { return };
            let app = ui.global::<App>();
            app.set_busy(false);
            if r.is_ok() && app.get_progress() > 0.0 {
                // a tool's last percent line may stop short of 100 (the readback prints every 25 %)
                app.set_progress(1.0);
            }
            done(&ui, r.err());
        });
    });
}

fn s(p: &Path) -> String {
    p.to_string_lossy().into_owned()
}

/// A read-only reader (the search log or the live view) holds the radio's port: the radio steps wait (said on screen).
/// On Linux / macOS a second open of the same tty would succeed and the two streams would mix.
fn reader_holds_port(ui: &AppWindow) -> bool {
    let held = with(|st| st.slog_cancel.is_some() || st.live_cancel.is_some());
    if held {
        set_message(ui, &tr("slog_busy"), "warning");
    }
    held
}

fn start_backup(ui: &AppWindow) {
    if reader_holds_port(ui) {
        return;
    }
    let Some(port) = port() else {
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
    if reader_holds_port(ui) {
        return;
    }
    let Some(port) = port() else {
        set_message(ui, &tr("radio_nocable"), "warning");
        return;
    };
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
        let none = with(|st| {
            st.wizard.install_finished(&r);
            st.wizard.path == ddm32_core::radio::flow::BackupPath::None
        });
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
    let app = ui.global::<App>();
    let i = app.get_selected_backup();
    let Some(b) = with(|st| st.found.get(i as usize).cloned()) else { return };
    app.set_restore_ready(false);
    let r: Result<(), String> = with(|st| {
        let d = FileSteps::detect_backup(&b.folder, Codec::dm32()).map_err(|e| e.record())?;
        let kind = if d.str("layout") == Some("stock") { "stock" } else { "previous" };
        st.restore.choose(kind, &b.folder).map_err(|e| e.0.to_string())?;
        st.restore.start_detect().map_err(|e| e.0.to_string())?;
        st.restore.detect_finished(Some(d), "");
        let (_, _, mode) = st.restore.tool().ok_or_else(|| i18n::t(&st.lang, st.restore.message.0))?;
        st.restore.start_plan().map_err(|e| e.0.to_string())?;
        let p = FileSteps::restore_plan(mode, &b.folder).map_err(|e| e.record())?;
        let safe = FileSteps::safe(&p);
        st.restore.plan_finished(Some(p), "", safe);
        if safe {
            Ok(())
        } else {
            Err("unsafe plan".into())
        }
    });
    match r {
        Ok(()) => {
            app.set_restore_ready(true);
            set_message(ui, &tr("msg_restore_plan"), "info");
        }
        Err(e) => set_message(ui, &trf("msg_restore_cannot", &e), "warning"),
    }
}

fn start_restore(ui: &AppWindow) {
    if reader_holds_port(ui) {
        return;
    }
    let Some(port) = port() else {
        set_message(ui, &tr("radio_nocable"), "warning");
        return;
    };
    let r = with(|st| -> Result<_, String> {
        st.restore.port = Some(port.clone());
        let (tool, extra, _) = st.restore.tool().ok_or("no tool")?;
        let backup = st.restore.backup_dir.clone().unwrap_or_default();
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
        with(|st| st.restore.restore_finished(&r));
        if r.passed {
            set_message(ui, &tr("msg_restore_done"), "success");
        } else if r.writes_started {
            set_message(ui, &trf("msg_install_stopped", &r.error), "danger");
        } else {
            set_message(ui, &trf("msg_install_not_started", &r.error), "warning");
        }
        refresh(ui);
    });
}

fn pack_done(ui: &AppWindow, r: Result<BuiltPack, String>, name: &str) {
    let app = ui.global::<App>();
    match r {
        Ok(p) => {
            let dir = with(|st| st.paths.packs());
            let _ = std::fs::create_dir_all(&dir);
            let file = unique_folder(&dir, &format!("{name}-{}.dpk", today()));
            if let Err(e) = std::fs::write(&file, &p.image) {
                set_message(ui, &trf("msg_pack_failed", &e.to_string()), "danger");
                return;
            }
            with(|st| st.pack = Some(file));
            app.set_pack_text(trf("msg_pack_ready", &ddm32_core::datapacks::datapack::info(&p.image)).into());
            app.set_pack_ready(true);
            set_message(ui, "", "info");
        }
        Err(e) => {
            app.set_pack_ready(false);
            set_message(ui, &trf("msg_pack_failed", &e), "danger");
        }
    }
}

fn pack_radioid(ui: &AppWindow) {
    let Some(csv) = rfd::FileDialog::new().add_filter("RadioID user.csv", &["csv"]).pick_file() else { return };
    let r = packs::radio_id(&csv, &[], "RadioID", None).map_err(|e| e.0);
    pack_done(ui, r, "radioid");
}

fn pack_air(ui: &AppWindow) {
    let Some(files) = rfd::FileDialog::new().add_filter("AIP AD 2", &["pdf", "txt"]).pick_files() else { return };
    let r = files
        .iter()
        .map(|f| packs::read_air_source(f))
        .collect::<Result<Vec<_>, _>>()
        .and_then(|src| packs::air_pack(&src, "Air band", None))
        .map_err(|e| e.0);
    pack_done(ui, r, "air");
}

fn pack_write(ui: &AppWindow) {
    if reader_holds_port(ui) {
        return;
    }
    let Some(port) = port() else {
        set_message(ui, &tr("radio_nocable"), "warning");
        return;
    };
    let Some(pack) = with(|st| st.pack.clone()) else { return };
    let argv = vec!["write".into(), "--port".into(), port, "--pack".into(), s(&pack)];
    run_tool(ui, "datapack", argv, true, move |ui, err| match err {
        None => set_message(ui, &tr("msg_pack_written"), "success"),
        Some(e) => set_message(ui, &trf("msg_failed", &e.record()), "danger"),
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
    app.set_update_text(i18n::tf(&lang, "update_available", "1.0.0-beta.5 (Beta)").into());
    app.set_backups_text(i18n::tf(&lang, "backups_found", "3").into());
    app.set_installed_text(i18n::tf(&lang, "installed", "DDM32 Plus 1.0.0-beta.4 (Beta)").into());
    app.set_app_update_text(i18n::tf(&lang, "pu_available", "3.0.0-beta.1").into());
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
            name: "dm32-047-20261113".into(),
            detail: "…/DDM25Plus/yedekler/dm32-047-20261113".into(),
            origin: "python".into(),
            kind: "".into(),
        },
    ])));
    app.set_backups_folder("C:\\Users\\…\\AppData\\Local\\Programs\\DDM32 Plus\\backups".into());
    app.set_pack_text(i18n::tf(&lang, "msg_pack_ready", "type 1 format 1 sequence 0 \"RadioID\" 2 IDs").into());
    app.set_pack_ready(true);
    app.set_version(ddm32_core::app_info::VERSION.into());
    app.set_key_id(ddm32_core::releases::PUBLIC_KEY_ID.into());
    app.set_data_folder("C:\\Users\\…\\AppData\\Local\\Programs\\DDM32 Plus".into());
}
