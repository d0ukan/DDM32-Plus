// SPDX-License-Identifier: GPL-3.0-or-later
//! The start-up update question and the version texts: what the radio runs (the record of the last verified install
//! or restore against the newest backup), the program update's beta fallback, "Later" / "Skip this version" per item,
//! the question waiting while anything is under way, its buttons never starting a radio step, a failed check written
//! to logs and shown on Home, and the line "Program 3.0.0 · Radio firmware 1.0.1 (from your last install)". No radio,
//! no network (the release list is made here and signed with a test key; install results are made up).
use ddm32_core::radio::flow::StepResult;
use ddm32_core::releases::catalog::Catalog;
use ddm32_core::releases::{signed_manifest as sm, CURRENT};
use ddm32_core::storage::{AppPaths, Settings};
use ddm32_gui::updates::{
    self, firmware_status, firmware_status_text, known_firmware, prompt_decision, update_offer, version_text,
    FirmwareStatus, InstallRecord, Installed, Known, PromptDecision, RecordKind, Source, UpdateOffer,
};
use ddm32_gui::{offered, App, AppWindow, Radio};
use serde_json::json;
use slint::ComponentHandle;
use std::path::{Path, PathBuf};

fn fw_entry(version: &str, channel: &str) -> serde_json::Value {
    let app = format!("app {version}").into_bytes();
    let name = format!("ddm32-plus-{version}.zip");
    json!({
        "version": version, "channel": channel, "file": name,
        "url": format!("{}v{version}/{name}", CURRENT.url_prefix()),
        "sha256": sm::sha256_hex(b"zip"), "size": 3,
        "application_sha256": sm::sha256_hex(&app), "application_size": app.len(),
        "settings_format": 1, "codeplug_format": 1, "min_compatible_version": version,
        "notes": {"tr": "", "en": ""},
    })
}

/// The application SHA-256 of a test release.
fn app_sha(version: &str) -> String {
    sm::sha256_hex(format!("app {version}").as_bytes())
}

/// A release: (tag, firmware (version, channel), program version for this platform).
type Release<'a> = (&'a str, Option<(&'a str, &'a str)>, Option<&'a str>);

/// A signed release list.
fn catalog(releases: &[Release<'_>]) -> Catalog {
    let secret: Vec<u8> = (0..32u8).collect();
    let key = hex::encode(ddm32_core::crypto::ed25519::secret_to_public(&secret));
    let platform = ddm32_core::releases::platform_key();
    let mut cat = Catalog::default();
    for (tag, fw, app) in releases {
        let mut m = json!({"format": CURRENT.format, "repo": CURRENT.repo, "release": tag,
            "created_utc": "2026-11-24T00:00:00Z", "firmware": fw.map(|(v, c)| vec![fw_entry(v, c)]).unwrap_or_default()});
        if let Some(v) = app {
            let name = format!("DDM32Plus-{v}.zip");
            m["app_rs"] = json!({platform.clone(): {"version": v, "file": name,
                "url": format!("{}{tag}/{name}", CURRENT.url_prefix()), "sha256": sm::sha256_hex(b"z"), "size": 1}});
        }
        let data = serde_json::to_vec_pretty(&m).unwrap();
        let sig = sm::sign_bytes(&data, &secret);
        cat.add(tag, &sm::verify_bytes(&data, sig.as_bytes(), Some(&key), None).unwrap(), None);
    }
    cat.sort();
    cat
}

fn plus(v: &str) -> Installed {
    Installed::Plus { version: v.into(), label: "Latest".into() }
}

fn from_install(v: &str) -> Known {
    Known { installed: plus(v), source: Source::Install, may_prompt: true }
}

fn temp(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("ddm32-updates-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

fn record(kind: RecordKind, unix: Option<i64>, version: Option<&str>) -> InstallRecord {
    InstallRecord {
        kind,
        unix,
        sha256: version.map(app_sha),
        version: version.map(str::to_string),
        backup: None,
        radio: None,
    }
}

#[test]
fn installed_against_the_newest() {
    let cat = catalog(&[("v1.0.1", Some(("1.0.1", "stable")), None), ("v1.0.0", Some(("1.0.0", "stable")), None)]);
    let rows = offered(&cat, 0);
    assert_eq!(FirmwareStatus::UpToDate("1.0.1".into()), firmware_status(&plus("1.0.1"), &rows));
    assert_eq!(
        FirmwareStatus::Newer { installed: "1.0.0".into(), newest: "1.0.1".into() },
        firmware_status(&plus("1.0.0"), &rows)
    );
    // a beta newer than the channel's newest stable: up to date, never "a new version is ready"
    assert_eq!(FirmwareStatus::UpToDate("1.1.0-beta.1".into()), firmware_status(&plus("1.1.0-beta.1"), &rows));
    for i in [Installed::NotRead, Installed::Unknown, Installed::Interrupted, Installed::Other("Factory 047".into())] {
        assert_eq!(FirmwareStatus::Newest("1.0.1".into()), firmware_status(&i, &rows));
    }
    assert_eq!(FirmwareStatus::NoList, firmware_status(&plus("1.0.1"), &[]));
    let en = |st: &FirmwareStatus, src| firmware_status_text("en", st, src, &rows, &cat.firmware);
    assert_eq!(
        "The radio firmware is up to date (1.0.1).",
        en(&firmware_status(&plus("1.0.1"), &rows), Source::Install)
    );
    assert_eq!(
        "A new radio firmware is ready: 1.0.1 (Latest) (the radio runs 1.0.0)",
        en(&firmware_status(&plus("1.0.0"), &rows), Source::Install)
    );
    // from a backup: hedged
    assert_eq!(
        "A new radio firmware is ready: 1.0.1 (Latest) (your last backup shows 1.0.0; the radio may run a newer one)",
        en(&firmware_status(&plus("1.0.0"), &rows), Source::Backup)
    );
    assert_eq!(
        "By your last backup the radio firmware is up to date (1.0.1); the backup may be older than the radio.",
        en(&firmware_status(&plus("1.0.1"), &rows), Source::Backup)
    );
    assert_eq!("Newest radio firmware: 1.0.1 (Latest)", en(&firmware_status(&Installed::NotRead, &rows), Source::None));
}

#[test]
fn the_record_against_the_backup() {
    // installed 1.0.1 after the backup (which holds 1.0.0): the record wins, no reinstall nag
    let rec = record(RecordKind::Install, Some(2000), Some("1.0.1"));
    let k = known_firmware(Some((&rec, plus("1.0.1"))), Some((1000, plus("1.0.0"))));
    assert_eq!(from_install("1.0.1"), k);
    let cat = catalog(&[("v1.0.1", Some(("1.0.1", "stable")), None), ("v1.0.0", Some(("1.0.0", "stable")), None)]);
    assert_eq!(None, update_offer(&cat, 0, "3.0.0", &k, None, None).firmware);
    // a backup taken after the last install is a fresher reading (and may prompt)
    let k = known_firmware(Some((&rec, plus("1.0.1"))), Some((3000, plus("1.0.0"))));
    assert_eq!(Known { installed: plus("1.0.0"), source: Source::Backup, may_prompt: true }, k);
    // a C# record keeps no time: it wins unless the backup holds a newer DDM32 Plus
    let cs = record(RecordKind::Install, None, Some("1.0.1"));
    assert_eq!(Source::Install, known_firmware(Some((&cs, plus("1.0.1"))), Some((9, plus("1.0.0")))).source);
    assert_eq!(Source::Backup, known_firmware(Some((&cs, plus("1.0.0"))), Some((9, plus("1.0.1")))).source);
    // only a backup: shown hedged, never a firmware question (it may be older than an install it does not know of)
    let k = known_firmware(None, Some((9, plus("1.0.0"))));
    assert_eq!(Known { installed: plus("1.0.0"), source: Source::Backup, may_prompt: false }, k);
    assert_eq!(None, update_offer(&cat, 0, "3.0.0", &k, None, None).firmware);
    assert_eq!(
        Known { installed: Installed::NotRead, source: Source::None, may_prompt: false },
        known_firmware(None, None)
    );
    // after a first install from stock the backup says factory firmware, the record DDM32 Plus
    let k = known_firmware(Some((&rec, plus("1.0.1"))), Some((1000, Installed::Other("Factory firmware 047".into()))));
    assert_eq!(plus("1.0.1"), k.installed);
}

/// Each install / restore path with a made-up result: what settings.json then says (the C# program's keys too).
#[test]
fn every_radio_write_path_is_recorded() {
    let dir = temp("record");
    let s = Settings::new(dir.join("settings.json"));
    let ok = StepResult::new(true, true, false, "");
    let not_started = StepResult::new(false, false, false, "no answer");
    let stopped = StepResult::new(false, true, false, "write failed");
    let backup = dir.join("dm32-20261124");
    let get = |k: &str| s.load().get(k).cloned().unwrap_or(serde_json::Value::Null);

    // nothing written yet: no record (a C# backup note alone is not one)
    s.save("installed_firmware", json!("my note")).unwrap();
    assert_eq!(None, updates::read_record(&s));

    // 1. install with a new backup, 2. with the last backup (quick update): the backup is kept with the record
    for (n, b) in [(1, Some(backup.as_path())), (2, Some(backup.as_path())), (3, None::<&Path>)] {
        updates::record_install_result(&s, &ok, &app_sha("1.0.1"), "1.0.1", b, 1000 + n).unwrap();
        assert_eq!(json!(app_sha("1.0.1")), get("installed_sha256"));
        assert_eq!(json!("DDM32 Plus 1.0.1"), get("installed_firmware"));
        let r = updates::read_record(&s).unwrap();
        assert_eq!((RecordKind::Install, Some(1000 + n)), (r.kind, r.unix));
        assert_eq!(b.map(Path::to_path_buf), r.backup);
        assert_eq!(Some("1.0.1".to_string()), r.version);
    }
    // a run that wrote nothing leaves the record
    updates::record_install_result(&s, &not_started, &app_sha("1.0.2"), "1.0.2", None, 2000).unwrap();
    assert_eq!(Some(1003), updates::read_record(&s).unwrap().unix);
    // a run that stopped after its writes started: unknown
    updates::record_install_result(&s, &stopped, &app_sha("1.0.2"), "1.0.2", None, 2001).unwrap();
    let r = updates::read_record(&s).unwrap();
    assert_eq!(RecordKind::Interrupted, r.kind);
    assert_eq!(serde_json::Value::Null, get("installed_sha256"));
    assert_eq!(Installed::Interrupted, updates::installed_from_record(&r, &[], "en"));
    // 4. restore of an earlier backup, 5. restore of the factory firmware (C#: no sha, "" / "stock")
    updates::record_restore_result(&s, &ok, &backup, false, 3000).unwrap();
    let r = updates::read_record(&s).unwrap();
    assert_eq!((RecordKind::RestorePrevious, Some(backup.clone())), (r.kind, r.backup.clone()));
    assert_eq!((serde_json::Value::Null, json!("")), (get("installed_sha256"), get("installed_firmware")));
    // the restored backup is not readable here: unknown, never a guess
    assert_eq!(Installed::Unknown, updates::installed_from_record(&r, &[], "en"));
    updates::record_restore_result(&s, &ok, &backup, true, 3001).unwrap();
    assert_eq!(RecordKind::RestoreStock, updates::read_record(&s).unwrap().kind);
    assert_eq!(json!("stock"), get("installed_firmware"));
    updates::record_restore_result(&s, &stopped, &backup, true, 3002).unwrap();
    assert_eq!(RecordKind::Interrupted, updates::read_record(&s).unwrap().kind);

    // the C# program's record (no time), and a C# install over ours (its sha differs): read as the C# record
    let cs = Settings::new(dir.join("cs.json"));
    cs.save("installed_sha256", json!(app_sha("1.0.0"))).unwrap();
    cs.save("installed_firmware", json!("DDM32 Plus 1.0.0")).unwrap();
    assert_eq!(record(RecordKind::Install, None, Some("1.0.0")), updates::read_record(&cs).unwrap());
    updates::record_install_result(&cs, &ok, &app_sha("1.0.1"), "1.0.1", None, 5000).unwrap();
    cs.save("installed_sha256", json!(app_sha("1.0.2"))).unwrap();
    cs.save("installed_firmware", json!("DDM32 Plus 1.0.2")).unwrap();
    let r = updates::read_record(&cs).unwrap();
    assert_eq!((None, Some("1.0.2".to_string())), (r.unix, r.version));

    // the record names the listed release by its application's SHA-256 (with its label), else its version only
    let cat = catalog(&[("v1.0.1", Some(("1.0.1", "stable")), None)]);
    let r = record(RecordKind::Install, Some(1), Some("1.0.1"));
    assert_eq!(plus("1.0.1"), updates::installed_from_record(&r, &cat.firmware, "en"));
    let r = record(RecordKind::Install, Some(1), Some("1.0.9"));
    assert_eq!(
        Installed::Plus { version: "1.0.9".into(), label: String::new() },
        updates::installed_from_record(&r, &cat.firmware, "en")
    );
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn the_program_update_falls_back_to_betas_like_the_firmware() {
    let betas = catalog(&[("v1.1.0-beta.1", None, Some("3.1.0-beta.1"))]);
    assert_eq!("3.1.0-beta.1", updates::newer_program(&betas, "3.0.0", 0).unwrap().entry.version);
    let both = catalog(&[("v1.0.1", None, Some("3.0.1")), ("v1.1.0-beta.1", None, Some("3.1.0-beta.1"))]);
    assert_eq!("3.0.1", updates::newer_program(&both, "3.0.0", 0).unwrap().entry.version);
    assert_eq!("3.1.0-beta.1", updates::newer_program(&both, "3.0.0", 1).unwrap().entry.version);
    assert!(updates::newer_program(&both, "3.0.1", 0).is_none());
    assert!(updates::newer_program(&Catalog::default(), "3.0.0", 0).is_none());
}

#[test]
fn skip_and_later() {
    let cat =
        catalog(&[("v1.0.2", Some(("1.0.2", "stable")), Some("3.1.0")), ("v1.0.1", Some(("1.0.1", "stable")), None)]);
    let k = from_install("1.0.1");
    let offer = update_offer(&cat, 0, "3.0.0", &k, None, None);
    assert_eq!(
        UpdateOffer {
            program: Some(("3.0.0".into(), "3.1.0".into())),
            firmware: Some(("1.0.1".into(), "1.0.2".into()))
        },
        offer
    );
    assert_eq!(None, update_offer(&cat, 0, "3.0.0", &k, Some("3.1.0"), None).program);
    assert_eq!(None, update_offer(&cat, 0, "3.0.0", &k, None, Some("1.0.2")).firmware);
    assert!(update_offer(&cat, 0, "3.0.0", &k, Some("3.0.5"), Some("1.0.1")).firmware.is_some());
    // skip per item: the other stays
    let (v, rest) = updates::skip_item(&offer, true);
    assert_eq!((Some("3.1.0".to_string()), None, true), (v, rest.program.clone(), rest.firmware.is_some()));
    let (v, rest) = updates::skip_item(&rest, false);
    assert_eq!(Some("1.0.2".to_string()), v);
    assert!(rest.is_empty());
    assert_eq!(None, update_offer(&cat, 0, "3.0.0", &from_install("1.0.2"), None, None).firmware);
    assert_eq!(PromptDecision::Show, prompt_decision(&offer, None, false, false));
    assert_eq!(PromptDecision::No, prompt_decision(&offer, Some(&offer), false, false));
    let older = UpdateOffer { program: Some(("3.0.0".into(), "3.0.9".into())), firmware: None };
    assert_eq!(PromptDecision::Show, prompt_decision(&offer, Some(&older), false, false));
    assert_eq!(PromptDecision::Wait, prompt_decision(&offer, None, true, false));
    assert_eq!(PromptDecision::Wait, prompt_decision(&offer, None, false, true));
    assert_eq!(PromptDecision::No, prompt_decision(&UpdateOffer::default(), None, true, false));
    // what blocks it
    assert!(!updates::Busy::default().any());
    for b in [
        updates::Busy { port_held: true, ..Default::default() },
        updates::Busy { wizard_active: true, ..Default::default() },
        updates::Busy { restore_ready: true, ..Default::default() },
        updates::Busy { dialog_open: true, ..Default::default() },
    ] {
        assert!(b.any());
    }
    assert!(!updates::wizard_active(1, 0) && !updates::wizard_active(1, 5) && !updates::wizard_active(4, 2));
    assert!((1..=4).all(|s| updates::wizard_active(1, s)));
}

#[test]
fn version_line_says_where_it_comes_from() {
    assert_eq!(
        "Program 3.0.0 · Radio firmware 1.0.1 (from your last install)",
        version_text("en", "3.0.0", &from_install("1.0.1"))
    );
    assert_eq!(
        "Program 3.0.0 · Telsiz firmware 1.0.1 (son kurulumunuza göre)",
        version_text("tr", "3.0.0", &from_install("1.0.1"))
    );
    let backup = Known { installed: plus("1.0.0"), source: Source::Backup, may_prompt: false };
    assert_eq!(
        "Program 3.0.0 · Radio firmware 1.0.0 (from your last backup; may be older)",
        version_text("en", "3.0.0", &backup)
    );
    let none = Known { installed: Installed::NotRead, source: Source::None, may_prompt: false };
    assert_eq!("Program 3.0.0 · Radio firmware: not read yet", version_text("en", "3.0.0", &none));
    assert_eq!("Program 3.0.0 · Telsiz firmware: henüz okunmadı", version_text("tr", "3.0.0", &none));
    let broke = Known { installed: Installed::Interrupted, source: Source::Install, may_prompt: true };
    assert_eq!(
        "Program 3.0.0 · Radio firmware: unknown: the last install did not finish",
        version_text("en", "3.0.0", &broke)
    );
    let stock = Known {
        installed: Installed::Other("Factory firmware DM32.01.01.047".into()),
        source: Source::Backup,
        may_prompt: false,
    };
    assert_eq!(
        "Program 3.0.0 · Radio firmware: Factory firmware DM32.01.01.047 (from your last backup; may be older)",
        version_text("en", "3.0.0", &stock)
    );
    assert_eq!(
        "DDM32 Plus - TA2DOU · Program 3.0.0 · Radio firmware 1.0.1 (from your last install)",
        updates::title_text("en", "3.0.0", &from_install("1.0.1"))
    );
    assert_eq!("1.0.1 (Latest) (from your last install)", updates::installed_short(&from_install("1.0.1"), "en"));
    let cat = catalog(&[("v1.0.1", Some(("1.0.1", "stable")), None)]);
    let mut program = vec![0xFFu8; 0x100000];
    let app = b"app 1.0.1";
    program[0xC000..0xC000 + app.len()].copy_from_slice(app);
    assert_eq!(plus("1.0.1"), updates::installed_firmware(&program, &cat.firmware, "en"));
    assert_eq!(Installed::Unknown, updates::installed_firmware(&vec![0xFFu8; 0x100000], &cat.firmware, "en"));
    assert_eq!(Installed::NotRead, updates::installed_from_backups(&[], &cat.firmware, "en"));
}

#[test]
fn a_failed_check_is_logged() {
    let root = temp("log");
    let logs = root.join("logs");
    let f = updates::log_check_error(&logs, "2026-11-24T08:00:00Z", "NetError: timed out\nsecond line").unwrap();
    updates::log_check_error(&logs, "2026-11-24T09:00:00Z", "HTTP 403").unwrap();
    assert_eq!(
        "2026-11-24T08:00:00Z update check failed: NetError: timed out second line\n\
         2026-11-24T09:00:00Z update check failed: HTTP 403\n",
        std::fs::read_to_string(&f).unwrap()
    );
    let _ = std::fs::remove_dir_all(&root);
}

fn window(name: &str) -> (AppWindow, PathBuf, Settings) {
    i_slint_backend_testing::init_no_event_loop();
    let root = temp(name);
    let paths = AppPaths::new(&root);
    let settings = Settings::new(paths.settings_file());
    settings.set_language("en").unwrap();
    // accepted: an open disclaimer holds the question back (disclaimer_holds_the_question below)
    settings.accept_disclaimer(&ddm32_gui::disclaimer::version(), "2026-11-24T00:00:00Z").unwrap();
    let ui = AppWindow::new().unwrap();
    ddm32_gui::setup(&ui, paths, false);
    (ui, root, settings)
}

/// The window's flow with made-up check results: an error on Home and in logs, the question, Later, Skip per item,
/// and the wait while anything is under way (the 3 s tick shows it afterwards).
#[test]
fn the_window_asks_once_and_waits() {
    let (ui, root, settings) = window("flow");
    let app = ui.global::<App>();
    ddm32_gui::catalog_loaded(&ui, Err("NetError: no route to host".into()), true);
    assert!(app.get_check_error().contains("no route to host"), "{}", app.get_check_error());
    let log = std::fs::read_to_string(root.join("logs").join(updates::CHECK_LOG)).unwrap();
    assert!(log.contains("update check failed: NetError: no route to host"));
    assert!(!app.get_prompt_open());
    app.invoke_dismiss_check_error();
    assert_eq!("", app.get_check_error().as_str());

    let newer = |v: &str| catalog(&[("v9.0.0", None, Some(v))]);
    ddm32_gui::catalog_loaded(&ui, Ok(newer("9.0.0")), true);
    assert!(app.get_prompt_open());
    assert_eq!(format!("Program {} → 9.0.0", ddm32_core::app_info::VERSION), app.get_prompt_program().to_string());
    app.invoke_prompt_later();
    assert!(!app.get_prompt_open());
    ddm32_gui::catalog_loaded(&ui, Ok(newer("9.0.0")), true);
    assert!(!app.get_prompt_open(), "Later: not again in this session");
    assert_eq!(None, settings.skip_program_version());

    // anything under way: it waits, and the 3 s tick asks once it is over
    type Set = fn(&AppWindow, bool);
    let states: [(&str, Set); 5] = [
        ("busy", |ui, v| ui.global::<App>().set_busy(v)),
        ("search log", |ui, v| ui.global::<Radio>().set_slog_running(v)),
        ("restore ready", |ui, v| ui.global::<App>().set_restore_ready(v)),
        ("picker", |ui, v| ui.global::<Radio>().set_picker_open(v)),
        ("wizard step 2", |ui, v| {
            let app = ui.global::<App>();
            app.set_page(if v { 1 } else { 0 });
            app.set_step(if v { 2 } else { 0 });
        }),
    ];
    for (i, (name, set)) in states.iter().enumerate() {
        let v = format!("9.{}.0", i + 1);
        set(&ui, true);
        ddm32_gui::catalog_loaded(&ui, Ok(newer(&v)), true);
        assert!(!app.get_prompt_open(), "{name}: asked while under way");
        ddm32_gui::tick(&ui);
        assert!(!app.get_prompt_open(), "{name}: the tick asked while under way");
        set(&ui, false);
        ddm32_gui::tick(&ui);
        assert!(app.get_prompt_open(), "{name}: not asked once free");
        app.invoke_prompt_later();
    }
    // Skip this version: stored, never asked again for it
    ddm32_gui::catalog_loaded(&ui, Ok(newer("9.9.0")), true);
    assert!(app.get_prompt_open());
    app.invoke_prompt_skip_program();
    assert!(!app.get_prompt_open());
    assert_eq!(Some("9.9.0".to_string()), settings.skip_program_version());
    ddm32_gui::catalog_loaded(&ui, Ok(newer("9.9.0")), true);
    assert!(!app.get_prompt_open());
    let _ = std::fs::remove_dir_all(&root);
}

/// "Update now" for the firmware: the wizard at its first step with that version selected, nothing started; per item
/// skip keeps the other item; nothing at all while anything is under way.
#[test]
fn update_now_never_starts_a_radio_step() {
    let (ui, root, settings) = window("now");
    let app = ui.global::<App>();
    let radio = ui.global::<Radio>();
    // the radio runs 1.0.0 by the record of a verified install (now: newer than any backup)
    let ok = StepResult::new(true, true, false, "");
    updates::record_install_result(&settings, &ok, &app_sha("1.0.0"), "1.0.0", None, updates::now_unix()).unwrap();
    let cat = || {
        catalog(&[
            ("v9.0.0", Some(("1.0.2", "stable")), Some("9.0.0")),
            ("v1.0.1", Some(("1.0.1", "stable")), None),
            ("v1.0.0", Some(("1.0.0", "stable")), None),
        ])
    };
    ddm32_gui::catalog_loaded(&ui, Ok(cat()), true);
    assert!(
        app.get_version_line().ends_with("Radio firmware 1.0.0 (from your last install)"),
        "{}",
        app.get_version_line()
    );
    assert!(app.get_prompt_open());
    assert_eq!("Radio firmware 1.0.0 → 1.0.2", app.get_prompt_firmware().as_str());
    // skip the program only: the firmware item stays
    app.invoke_prompt_skip_program();
    assert!(app.get_prompt_open());
    assert_eq!("", app.get_prompt_program().as_str());
    // a radio job started meanwhile: Update now does nothing but close; asked again later
    app.set_busy(true);
    app.invoke_prompt_update_firmware();
    assert!(!app.get_prompt_open());
    assert_eq!(0, app.get_page());
    app.set_busy(false);
    ddm32_gui::tick(&ui);
    assert!(app.get_prompt_open());
    app.invoke_prompt_update_firmware();
    assert!(!app.get_prompt_open());
    assert_eq!((1, 0), (app.get_page(), app.get_step()), "the wizard at its first step");
    let i = app.get_selected_version() as usize;
    assert_eq!("1.0.2", ddm32_gui::with_shown(|v| v[i].version().to_string()));
    assert!(!app.get_busy() && !radio.get_busy() && !app.get_step_done(), "no radio step started");
    assert_eq!("", app.get_message().as_str());
    // the program update is refused while the wizard is under way (no download, no restart)
    app.set_step(2);
    app.invoke_app_update();
    assert!(!app.get_busy());
    assert_eq!(ddm32_gui::i18n::t("en", "pu_blocked"), app.get_app_update_text().as_str());
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn disclaimer_holds_the_question() {
    i_slint_backend_testing::init_no_event_loop();
    let root = std::env::temp_dir().join(format!("ddm32-prompt-disc-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    let paths = AppPaths::new(&root);
    Settings::new(paths.settings_file()).set_language("en").unwrap();
    let ui = AppWindow::new().unwrap();
    ddm32_gui::setup(&ui, paths, false);
    let app = ui.global::<App>();
    assert!(app.get_disc_open(), "not accepted yet: asked first");
    ddm32_gui::catalog_loaded(&ui, Ok(catalog(&[("v9.0.0", None, Some("9.0.0"))])), true);
    assert!(!app.get_prompt_open(), "the update question waits behind the disclaimer");
    app.set_disc_checked(true);
    app.invoke_disc_accept();
    assert!(!app.get_disc_open());
    assert!(app.get_prompt_open(), "and comes once it is accepted");
    let _ = std::fs::remove_dir_all(&root);
}

/// A C# restore after our install: `installed_sha256` null and `installed_firmware` "stock" / "" supersede our record
/// (what the radio runs is the restored firmware or not known, never our older install).
#[test]
fn a_csharp_restore_supersedes_our_record() {
    let dir = temp("csrestore");
    let s = Settings::new(dir.join("settings.json"));
    let ok = StepResult::new(true, true, false, "");
    let all = catalog(&[("v1.0.1", Some(("1.0.1", "stable")), None)]).firmware;
    for (firmware, kind) in [("stock", RecordKind::RestoreStock), ("", RecordKind::RestorePrevious)] {
        updates::record_install_result(&s, &ok, &app_sha("1.0.1"), "1.0.1", None, 1000).unwrap();
        assert_eq!(RecordKind::Install, updates::read_record(&s).unwrap().kind);
        // the C# RestorePage after a verified restore
        s.save("installed_sha256", serde_json::Value::Null).unwrap();
        s.save("installed_firmware", json!(firmware)).unwrap();
        let r = updates::read_record(&s).unwrap();
        assert_eq!((kind, None, None), (r.kind, r.unix, r.sha256.clone()), "{firmware:?}");
        let shown = updates::installed_from_record(&r, &all, "en");
        if kind == RecordKind::RestoreStock {
            assert_eq!(Installed::Other("Factory firmware".into()), shown);
        } else {
            assert_eq!(Installed::Unknown, shown);
        }
    }
    // our own restore records stay as they are (their C# keys agree with them)
    let backup = dir.join("dm32-x");
    updates::record_restore_result(&s, &ok, &backup, true, 2000).unwrap();
    assert_eq!((RecordKind::RestoreStock, Some(2000)), {
        let r = updates::read_record(&s).unwrap();
        (r.kind, r.unix)
    });
    updates::record_restore_result(&s, &ok, &backup, false, 3000).unwrap();
    assert_eq!(Some(3000), updates::read_record(&s).unwrap().unix);
    // a C# stock restore over our previous-version restore
    s.save("installed_firmware", json!("stock")).unwrap();
    let r = updates::read_record(&s).unwrap();
    assert_eq!((RecordKind::RestoreStock, None), (r.kind, r.unix));
    // a C# backup note (sha kept) does not count as a restore
    updates::record_install_result(&s, &ok, &app_sha("1.0.1"), "1.0.1", None, 4000).unwrap();
    s.save("installed_firmware", json!("my note")).unwrap();
    assert_eq!(Some(4000), updates::read_record(&s).unwrap().unix);
}

/// A fresh install stays on Latest after its first install record (the record writes C# keys too).
#[test]
fn a_fresh_install_keeps_latest_after_an_install() {
    let (ui, root, settings) = window("fresh-channel");
    assert_eq!(0, ui.global::<App>().get_channel());
    let ok = StepResult::new(true, true, false, "");
    updates::record_install_result(&settings, &ok, &app_sha("1.0.0"), "1.0.0", None, updates::now_unix()).unwrap();
    drop(ui);
    let ui = AppWindow::new().unwrap();
    ddm32_gui::setup(&ui, AppPaths::new(&root), false);
    assert_eq!(0, ui.global::<App>().get_channel());
    assert_eq!(Some(0), settings.load().get("channel").and_then(|v| v.as_i64()), "settled at the first start");
    let _ = std::fs::remove_dir_all(&root);
}
