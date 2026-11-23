// SPDX-License-Identifier: GPL-3.0-or-later
//! The start-up update question and the version texts: what the radio runs against the newest version, the program
//! update's beta fallback, "Later" / "Skip this version", a failed check written to logs and shown on Home, and the
//! line "Program 3.0.0 · Radio firmware 1.0.1". No radio, no network (the release list is made here and signed with a
//! test key).
use ddm32_core::releases::catalog::Catalog;
use ddm32_core::releases::{signed_manifest as sm, CURRENT};
use ddm32_core::storage::{AppPaths, Settings};
use ddm32_gui::updates::{
    self, firmware_status, firmware_status_text, prompt_decision, update_offer, version_text, FirmwareStatus,
    Installed, PromptDecision, UpdateOffer,
};
use ddm32_gui::{offered, App, AppWindow, Radio};
use serde_json::json;
use slint::ComponentHandle;

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
    // not known: only the newest is named
    for i in [Installed::NotRead, Installed::Unknown, Installed::Other("Factory firmware 047".into())] {
        assert_eq!(FirmwareStatus::Newest("1.0.1".into()), firmware_status(&i, &rows));
    }
    assert_eq!(FirmwareStatus::NoList, firmware_status(&plus("1.0.1"), &[]));
    let en = |st: &FirmwareStatus| firmware_status_text("en", st, &rows, &cat.firmware);
    assert_eq!("The radio firmware is up to date (1.0.1).", en(&firmware_status(&plus("1.0.1"), &rows)));
    assert_eq!(
        "A new radio firmware is ready: 1.0.1 (Latest) (the radio runs 1.0.0)",
        en(&firmware_status(&plus("1.0.0"), &rows))
    );
    assert_eq!("Newest radio firmware: 1.0.1 (Latest)", en(&firmware_status(&Installed::NotRead, &rows)));
    assert_eq!(
        "Telsiz firmware güncel (1.0.1).",
        firmware_status_text("tr", &firmware_status(&plus("1.0.1"), &rows), &rows, &cat.firmware)
    );
}

#[test]
fn the_program_update_falls_back_to_betas_like_the_firmware() {
    // only a beta program published: the default channel offers it (as the firmware list does)
    let betas = catalog(&[("v1.1.0-beta.1", None, Some("3.1.0-beta.1"))]);
    assert_eq!("3.1.0-beta.1", updates::newer_program(&betas, "3.0.0", 0).unwrap().entry.version);
    // a stable program exists: the default channel stays on stable builds
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
    let offer = update_offer(&cat, 0, "3.0.0", &plus("1.0.1"), None, None);
    assert_eq!(
        UpdateOffer {
            program: Some(("3.0.0".into(), "3.1.0".into())),
            firmware: Some(("1.0.1".into(), "1.0.2".into()))
        },
        offer
    );
    // "Skip this version": that version is not asked for again; a newer one is
    assert_eq!(None, update_offer(&cat, 0, "3.0.0", &plus("1.0.1"), Some("3.1.0"), None).program);
    assert_eq!(None, update_offer(&cat, 0, "3.0.0", &plus("1.0.1"), None, Some("1.0.2")).firmware);
    assert!(update_offer(&cat, 0, "3.0.0", &plus("1.0.1"), Some("3.0.5"), Some("1.0.1")).firmware.is_some());
    assert!(update_offer(&cat, 0, "3.0.0", &plus("1.0.1"), Some("3.1.0"), Some("1.0.2")).is_empty());
    assert_eq!((Some("3.1.0".into()), Some("1.0.2".into())), updates::skip_versions(&offer));
    // the radio already runs the newest, or its firmware is not known: no firmware question
    assert_eq!(None, update_offer(&cat, 0, "3.0.0", &plus("1.0.2"), None, None).firmware);
    assert_eq!(None, update_offer(&cat, 0, "3.0.0", &Installed::NotRead, None, None).firmware);
    // "Later": not again in this session for these very versions; a newer one asks again
    assert_eq!(PromptDecision::Show, prompt_decision(&offer, None, false, false));
    assert_eq!(PromptDecision::No, prompt_decision(&offer, Some(&offer), false, false));
    let older = UpdateOffer { program: Some(("3.0.0".into(), "3.0.9".into())), firmware: None };
    assert_eq!(PromptDecision::Show, prompt_decision(&offer, Some(&older), false, false));
    // never while a radio job holds the port, nor over the first-start page: it waits
    assert_eq!(PromptDecision::Wait, prompt_decision(&offer, None, true, false));
    assert_eq!(PromptDecision::Wait, prompt_decision(&offer, None, false, true));
    assert_eq!(PromptDecision::No, prompt_decision(&UpdateOffer::default(), None, true, false));
}

#[test]
fn version_line_never_claims_what_is_not_known() {
    assert_eq!("Program 3.0.0 · Radio firmware 1.0.1", version_text("en", "3.0.0", &plus("1.0.1")));
    assert_eq!("Program 3.0.0 · Telsiz firmware 1.0.1", version_text("tr", "3.0.0", &plus("1.0.1")));
    assert_eq!("Program 3.0.0 · Radio firmware: not read yet", version_text("en", "3.0.0", &Installed::NotRead));
    assert_eq!("Program 3.0.0 · Telsiz firmware: henüz okunmadı", version_text("tr", "3.0.0", &Installed::NotRead));
    assert_eq!("Program 3.0.0 · Radio firmware: unknown", version_text("en", "3.0.0", &Installed::Unknown));
    assert_eq!(
        "Program 3.0.0 · Radio firmware: Factory firmware DM32.01.01.047",
        version_text("en", "3.0.0", &Installed::Other("Factory firmware DM32.01.01.047".into()))
    );
    assert_eq!(
        "DDM32 Plus - TA2DOU · Program 3.0.0 · Radio firmware 1.0.1",
        updates::title_text("en", "3.0.0", &plus("1.0.1"))
    );
    // a backup's program image: the release whose application is in it, or nothing claimed
    let cat = catalog(&[("v1.0.1", Some(("1.0.1", "stable")), None)]);
    let mut program = vec![0xFFu8; 0x100000];
    let app = b"app 1.0.1";
    program[0xC000..0xC000 + app.len()].copy_from_slice(app);
    assert_eq!(plus("1.0.1"), updates::installed_firmware(&program, &cat.firmware, "en"));
    assert_eq!(Installed::Unknown, updates::installed_firmware(&vec![0xFFu8; 0x100000], &cat.firmware, "en"));
    assert_eq!(Installed::NotRead, updates::installed_from_backups(&[], &cat.firmware, "en"));
}

#[test]
fn a_failed_check_is_logged_and_shown() {
    let root = std::env::temp_dir().join(format!("ddm32-updates-log-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    let logs = root.join("logs");
    let f = updates::log_check_error(&logs, "2026-11-24T08:00:00Z", "NetError: timed out\nsecond line").unwrap();
    updates::log_check_error(&logs, "2026-11-24T09:00:00Z", "HTTP 403").unwrap();
    let text = std::fs::read_to_string(&f).unwrap();
    assert_eq!(
        "2026-11-24T08:00:00Z update check failed: NetError: timed out second line\n\
         2026-11-24T09:00:00Z update check failed: HTTP 403\n",
        text
    );
    let _ = std::fs::remove_dir_all(&root);
}

/// The window's flow with a made-up check result: an error on Home and in logs, the question, Later, Skip, and the
/// wait while a radio job holds the port.
#[test]
fn the_window_asks_once_and_waits_for_the_port() {
    i_slint_backend_testing::init_no_event_loop();
    let root = std::env::temp_dir().join(format!("ddm32-updates-flow-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    let paths = AppPaths::new(&root);
    let settings = Settings::new(paths.settings_file());
    settings.set_language("en").unwrap();
    let ui = AppWindow::new().unwrap();
    ddm32_gui::setup(&ui, paths.clone(), false);
    let app = ui.global::<App>();

    // a failed check: never swallowed
    ddm32_gui::catalog_loaded(&ui, Err("NetError: no route to host".into()), true);
    assert!(app.get_check_error().contains("no route to host"), "{}", app.get_check_error());
    let log = std::fs::read_to_string(paths.logs().join(updates::CHECK_LOG)).unwrap();
    assert!(log.contains("update check failed: NetError: no route to host"));
    assert!(!app.get_prompt_open());
    app.invoke_dismiss_check_error();
    assert_eq!("", app.get_check_error().as_str());

    // a newer program: one question; a good check clears the error
    let newer = ddm32_gui::updates::newer_program(
        &catalog(&[("v9.0.0", None, Some("9.0.0"))]),
        ddm32_core::app_info::VERSION,
        0,
    );
    assert!(newer.is_some());
    ddm32_gui::catalog_loaded(&ui, Err("x".into()), false);
    ddm32_gui::catalog_loaded(&ui, Ok(catalog(&[("v9.0.0", None, Some("9.0.0"))])), true);
    assert_eq!("", app.get_check_error().as_str());
    assert!(app.get_prompt_open());
    assert_eq!(format!("Program {} → 9.0.0", ddm32_core::app_info::VERSION), app.get_prompt_program().to_string());
    // Later: closed, not asked again in this session for the same version
    app.invoke_prompt_later();
    assert!(!app.get_prompt_open());
    ddm32_gui::catalog_loaded(&ui, Ok(catalog(&[("v9.0.0", None, Some("9.0.0"))])), true);
    assert!(!app.get_prompt_open());
    assert_eq!(None, settings.skip_program_version());

    // a newer one while the radio is busy: it waits, and comes when the port is free
    app.set_busy(true);
    ddm32_gui::catalog_loaded(&ui, Ok(catalog(&[("v9.1.0", None, Some("9.1.0"))])), true);
    assert!(!app.get_prompt_open());
    app.set_busy(false);
    ui.global::<Radio>().set_slog_running(true);
    ddm32_gui::consider_prompt(&ui);
    assert!(!app.get_prompt_open());
    ui.global::<Radio>().set_slog_running(false);
    ddm32_gui::consider_prompt(&ui);
    assert!(app.get_prompt_open());
    // Skip this version: stored, never asked again for 9.1.0
    app.invoke_prompt_skip();
    assert!(!app.get_prompt_open());
    assert_eq!(Some("9.1.0".to_string()), settings.skip_program_version());
    ddm32_gui::catalog_loaded(&ui, Ok(catalog(&[("v9.1.0", None, Some("9.1.0"))])), true);
    assert!(!app.get_prompt_open());
    let _ = std::fs::remove_dir_all(&root);
}
