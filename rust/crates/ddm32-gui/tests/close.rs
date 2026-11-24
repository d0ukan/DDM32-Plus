// SPDX-License-Identifier: GPL-3.0-or-later
//! The window's own close button (X, Alt+F4, the OS asking it to close), the Yes / No question for a test / alpha
//! version, the release channel taken over from the C# program, and the self-update start marker. No radio (the
//! readers are stand-ins: only their stop switches exist), no network (the release list is made and signed here).
use ddm32_core::releases::catalog::Catalog;
use ddm32_core::releases::{signed_manifest as sm, CURRENT};
use ddm32_core::storage::{AppPaths, Settings};
use ddm32_gui::{i18n, App, AppWindow, Radio};
use serde_json::json;
use slint::platform::WindowEvent;
use slint::{CloseRequestResponse, ComponentHandle, Model};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

const DISCLAIMER: &str = "684c0603763f3987";

/// The testing backend, once per test thread (a second init panics).
fn init() {
    thread_local!(static DONE: std::cell::Cell<bool> = const { std::cell::Cell::new(false) });
    if !DONE.with(|d| d.replace(true)) {
        i_slint_backend_testing::init_no_event_loop();
    }
}

fn root(name: &str) -> PathBuf {
    let r = std::env::temp_dir().join(format!("ddm32-close-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&r);
    std::fs::create_dir_all(&r).unwrap();
    r
}

/// A window over a data folder whose settings.json is `settings` (the disclaimer accepted unless it says otherwise).
fn window(root: &Path, mut settings: serde_json::Value) -> AppWindow {
    if settings.get("disclaimer").is_none() {
        settings["disclaimer"] =
            json!({"version": DISCLAIMER, "accepted": true, "accepted_utc": "2026-11-24T00:00:00Z"});
    }
    std::fs::write(root.join("settings.json"), settings.to_string()).unwrap();
    init();
    let ui = AppWindow::new().unwrap();
    ddm32_gui::setup(&ui, AppPaths::new(root), false);
    ui
}

#[test]
fn idle_window_closes() {
    let root = root("idle");
    let ui = window(&root, json!({"language": "en"}));
    assert_eq!(CloseRequestResponse::HideWindow, ddm32_gui::close_requested(&ui));
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn a_running_step_refuses_the_close_button() {
    let root = root("busy");
    let ui = window(&root, json!({"language": "en"}));
    let app = ui.global::<App>();
    let radio = ui.global::<Radio>();
    let refused = i18n::t("en", "busy_close");
    // a radio write marked running (install, restore, pack write): every press refused with the plain line (without a
    // write the line also says how to close anyway; a_second_press_offers_close_anyway...)
    ddm32_gui::with_state(|s| s.writing = true);
    // App.busy (the write's own flag) and Radio.busy (a health read beside it)
    for which in 0..2 {
        app.set_message("".into());
        radio.set_message("".into());
        if which == 0 {
            app.set_busy(true);
        } else {
            radio.set_busy(true);
        }
        assert_eq!(CloseRequestResponse::KeepWindowShown, ddm32_gui::close_requested(&ui));
        assert_eq!(refused, app.get_message().as_str());
        assert_eq!(refused, radio.get_message().as_str());
        assert_eq!("warning", radio.get_message_kind().as_str());
        // the window's own close event goes the same way (the handler is set up by setup())
        radio.set_message("".into());
        ui.window().dispatch_event(WindowEvent::CloseRequested);
        assert_eq!(refused, radio.get_message().as_str(), "the close event reached the guard");
        app.set_busy(false);
        radio.set_busy(false);
    }
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn the_live_view_and_the_search_log_are_stopped_before_closing() {
    let root = root("readers");
    let ui = window(&root, json!({"language": "en"}));
    let live = Arc::new(AtomicBool::new(false));
    let slog = Arc::new(AtomicBool::new(false));
    ddm32_gui::with_state(|s| {
        s.live_cancel = Some(live.clone());
        s.slog_cancel = Some(slog.clone());
    });
    assert_eq!(CloseRequestResponse::KeepWindowShown, ddm32_gui::close_requested(&ui));
    assert!(live.load(Ordering::SeqCst) && slog.load(Ordering::SeqCst), "both readers told to stop");
    assert!(ddm32_gui::with_state(|s| s.close_pending && s.close_timer.is_some()));
    assert_eq!(i18n::t("en", "closing_wait"), ui.global::<Radio>().get_message().as_str());
    // the live view ended; the search log still writes its last poll: the window waits
    ddm32_gui::with_state(|s| s.live_cancel = None);
    assert!(!ddm32_gui::reader_ended());
    // the search log ended too: now it closes
    ddm32_gui::with_state(|s| s.slog_cancel = None);
    assert!(ddm32_gui::reader_ended());
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn readers_ending_without_a_close_request_keep_the_window() {
    let root = root("noclose");
    let _ui = window(&root, json!({"language": "en"}));
    assert!(!ddm32_gui::reader_ended());
    let _ = std::fs::remove_dir_all(&root);
}

// ---- the start marker -------------------------------------------------------------------------------------------------

#[test]
fn the_start_marker_is_written_once_the_event_loop_runs() {
    let root = root("marker");
    let marker = root.join("updates").join("update-started.json");
    let args = vec!["--update-check".to_string(), marker.to_string_lossy().into_owned()];
    assert_eq!(Some(marker.clone()), ddm32_gui::start_marker_path(&args));
    assert_eq!(None, ddm32_gui::start_marker_path(&["--demo".to_string()]));
    assert_eq!(None, ddm32_gui::start_marker_path(&["--update-check".to_string()]));
    init();
    ddm32_gui::arm_start_marker(&args);
    assert!(!marker.exists(), "not before the window has run");
    i_slint_backend_testing::mock_elapsed_time(ddm32_gui::MARKER_DELAY + std::time::Duration::from_millis(100));
    // the helpers (C# SelfUpdate.WaitMarker, ddm32-core self_update) only look for the file
    let v: serde_json::Value = serde_json::from_slice(&std::fs::read(&marker).unwrap()).unwrap();
    assert_eq!(true, v["started"]);
    assert_eq!(ddm32_core::app_info::VERSION, v["version"]);
    assert_eq!(std::process::id() as u64, v["pid"].as_u64().unwrap());
    let _ = std::fs::remove_dir_all(&root);
}

// ---- test / alpha confirmation ----------------------------------------------------------------------------------------

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

fn catalog(releases: &[(&str, &str, &str)]) -> Catalog {
    let secret: Vec<u8> = (0..32u8).collect();
    let key = hex::encode(ddm32_core::crypto::ed25519::secret_to_public(&secret));
    let mut cat = Catalog::default();
    for (tag, v, ch) in releases {
        let m = json!({"format": CURRENT.format, "repo": CURRENT.repo, "release": tag,
            "created_utc": "2026-11-24T00:00:00Z", "firmware": [fw_entry(v, ch)]});
        let data = serde_json::to_vec_pretty(&m).unwrap();
        let sig = sm::sign_bytes(&data, &secret);
        cat.add(tag, &sm::verify_bytes(&data, sig.as_bytes(), Some(&key), None).unwrap(), None);
    }
    cat.sort();
    cat
}

#[test]
fn a_test_version_asks_yes_or_no_before_anything_is_downloaded() {
    let root = root("alpha");
    let ui = window(&root, json!({"language": "en", "channel": 2}));
    let app = ui.global::<App>();
    let cat = catalog(&[("v1.0.1", "1.0.1", "stable"), ("v1.1.0-alpha.1", "1.1.0-alpha.1", "beta")]);
    ddm32_gui::catalog_loaded(&ui, Ok(cat), false);
    app.invoke_navigate(1);
    let versions: Vec<String> = (0..app.get_versions().iter().count())
        .map(|i| slint::Model::row_data(&app.get_versions(), i).unwrap().version.to_string())
        .collect();
    let alpha = versions.iter().position(|v| v == "1.1.0-alpha.1").expect("the Test channel offers the alpha");
    app.set_selected_version(alpha as i32);
    app.invoke_wizard_next();
    assert!(app.get_test_confirm_open(), "asked first");
    assert!(app.get_test_confirm_text().starts_with("1.1.0-alpha.1 is a test / alpha version"));
    assert!(!app.get_busy(), "no download started");
    assert_eq!(0, app.get_step());
    // "No" (the default): the question closes, nothing else happens; asking again asks again
    app.set_test_confirm_open(false);
    assert!(!app.get_busy());
    app.invoke_wizard_next();
    assert!(app.get_test_confirm_open());
    assert!(!app.get_busy());
    // the No button has the focus when the question opens (Enter = No), and Yes is the danger button
    let ui_src = std::fs::read_to_string(Path::new(env!("CARGO_MANIFEST_DIR")).join("ui/app.slint")).unwrap();
    let dlg = &ui_src[ui_src.find("if App.test-confirm-open").unwrap()..];
    let dlg = &dlg[..dlg.find("if App.disc-open").unwrap()];
    assert!(dlg.contains(r#"Button { text: Tr.t(Tr.lang, "b_no"); in-dialog: true; default-focus: true;"#));
    assert!(dlg.contains(r#"text: Tr.t(Tr.lang, "ver_test_yes"); kind: "danger"; in-dialog: true;"#));
    let _ = std::fs::remove_dir_all(&root);
}

// ---- the release channel during the hand-over -------------------------------------------------------------------------

#[test]
fn the_channel_follows_the_csharp_include_beta() {
    // (settings.json, channel shown)
    let cases = [
        (json!({"language": "en"}), 0),                        // a fresh install: stable only
        (json!({"language": "en", "include_beta": true}), 1),  // C#: betas shown
        (json!({"language": "en", "include_beta": false}), 0), // C#: betas off
        (json!({"language": "en", "window": {"x": 0}}), 1),    // C# file, include_beta never set (default true)
        (json!({"language": "en", "include_beta": true, "channel": 0}), 0), // this program's own choice wins
        (json!({"language": "en", "channel": 2}), 2),
    ];
    for (i, (settings, want)) in cases.into_iter().enumerate() {
        let root = root(&format!("channel{i}"));
        let ui = window(&root, settings.clone());
        assert_eq!(want, ui.global::<App>().get_channel(), "{settings}");
        let _ = std::fs::remove_dir_all(&root);
    }
    // choosing a channel writes both keys, so the C# program agrees during the transition
    let root = root("channel-set");
    let ui = window(&root, json!({"language": "en", "include_beta": true}));
    for (c, beta) in [(0, false), (1, true), (2, true)] {
        ui.global::<App>().invoke_set_channel(c);
        let s = Settings::new(root.join("settings.json")).load();
        assert_eq!(c as i64, s["channel"].as_i64().unwrap());
        assert_eq!(beta, s["include_beta"].as_bool().unwrap());
        assert_eq!("en", s["language"]);
    }
    let _ = std::fs::remove_dir_all(&root);
}

// ---- "Close anyway" -----------------------------------------------------------------------------------------------------

#[test]
fn a_second_press_offers_close_anyway_but_never_during_a_radio_write() {
    let root = root("force");
    let ui = window(&root, json!({"language": "en"}));
    let app = ui.global::<App>();
    // a backup or a download (no radio write): the first press refuses and says how to close anyway
    app.set_busy(true);
    assert_eq!(CloseRequestResponse::KeepWindowShown, ddm32_gui::close_requested(&ui));
    assert!(!app.get_force_close_open());
    assert!(app.get_message().contains(&i18n::t("en", "busy_close_again")));
    // the second press within 5 s: "Close anyway?" (No has the focus), the window still open
    assert_eq!(CloseRequestResponse::KeepWindowShown, ddm32_gui::close_requested(&ui));
    assert!(app.get_force_close_open());
    app.set_force_close_open(false);
    // a radio write (install, restore, pack write): never, however often it is pressed
    ddm32_gui::with_state(|s| {
        s.writing = true;
        s.close_refused_at = None;
    });
    for _ in 0..3 {
        assert_eq!(CloseRequestResponse::KeepWindowShown, ddm32_gui::close_requested(&ui));
        assert!(!app.get_force_close_open());
        assert_eq!(i18n::t("en", "busy_close"), app.get_message().as_str());
    }
    // and "Yes" itself checks again
    app.set_message("".into());
    assert!(!ddm32_gui::force_close(&ui));
    assert_eq!(i18n::t("en", "busy_close"), app.get_message().as_str(), "a refused Yes says why");
    let ui_src = std::fs::read_to_string(Path::new(env!("CARGO_MANIFEST_DIR")).join("ui/app.slint")).unwrap();
    let dlg = &ui_src[ui_src.find("if App.force-close-open").unwrap()..];
    assert!(dlg.contains(r#"Button { text: Tr.t(Tr.lang, "b_no"); in-dialog: true; default-focus: true;"#));
    let _ = std::fs::remove_dir_all(&root);
}

/// A tool whose worker panicked: the window is not busy any more, no write is marked running, the step gets an error
/// with the panic's text (shown as its failure line).
#[test]
fn a_panicked_tool_clears_the_busy_state() {
    let root = root("panic");
    let ui = window(&root, json!({"language": "en"}));
    let app = ui.global::<App>();
    app.set_busy(true);
    ddm32_gui::with_state(|s| s.writing = true);
    let r = ddm32_gui::worker::caught(|| -> ddm32_core::radio::ToolResult<()> { panic!("boom in the tool") });
    let err = ddm32_gui::tool_ended(&ui, r).expect("an error");
    assert!(err.record().contains("boom in the tool"), "{}", err.record());
    assert!(!app.get_busy());
    assert!(!ddm32_gui::with_state(|s| s.writing));
    assert_eq!(CloseRequestResponse::HideWindow, ddm32_gui::close_requested(&ui), "closing works again");
    let _ = std::fs::remove_dir_all(&root);
}

/// A settings.json that is there but cannot be read: the start-up leaves it byte for byte as it was.
#[test]
fn an_unreadable_settings_file_is_left_as_it_is() {
    let root = root("unreadable");
    let bytes: &[u8] = b"{ \"language\": \"en\", \"disclaimer\": { trunc";
    std::fs::write(root.join("settings.json"), bytes).unwrap();
    init();
    let ui = AppWindow::new().unwrap();
    ddm32_gui::setup(&ui, AppPaths::new(&root), false);
    assert_eq!(0, ui.global::<App>().get_channel());
    assert_eq!(bytes, std::fs::read(root.join("settings.json")).unwrap().as_slice());
    let _ = std::fs::remove_dir_all(&root);
}
