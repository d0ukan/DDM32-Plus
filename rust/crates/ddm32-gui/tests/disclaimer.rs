// SPDX-License-Identifier: GPL-3.0-or-later
//! The disclaimer: the same text and the same version as the C# program (so an acceptance there counts here), shown
//! at the first start after the language and whenever the text changed, every radio step refused until it is
//! accepted, the acceptance saved in the C# format, Help > Disclaimer read only once accepted. No radio (no port is
//! opened: every step is refused before it would look for one), no network.
use ddm32_core::storage::{AppPaths, Settings};
use ddm32_gui::{disclaimer, i18n, App, AppWindow, Radio};
use slint::ComponentHandle;
use std::path::{Path, PathBuf};

/// The version of today's text, as the C# program 2.0.0 computes it (AppState.DisclaimerVersion() over
/// Resources/Strings.tr.resx then Strings.resx, Disc_Title .. Disc_Outro).
const CSHARP_VERSION: &str = "684c0603763f3987";

/// The testing backend, once per test thread (a second init panics).
fn init() {
    thread_local!(static DONE: std::cell::Cell<bool> = const { std::cell::Cell::new(false) });
    if !DONE.with(|d| d.replace(true)) {
        i_slint_backend_testing::init_no_event_loop();
    }
}

fn root(name: &str) -> PathBuf {
    let r = std::env::temp_dir().join(format!("ddm32-disc-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&r);
    std::fs::create_dir_all(&r).unwrap();
    r
}

fn settings_with(root: &Path, v: serde_json::Value) {
    std::fs::write(root.join("settings.json"), serde_json::to_string_pretty(&v).unwrap()).unwrap();
}

fn window(root: &Path) -> AppWindow {
    init();
    let ui = AppWindow::new().unwrap();
    ddm32_gui::setup(&ui, AppPaths::new(root), false);
    ui
}

#[test]
fn version_is_the_csharp_programs() {
    assert_eq!(CSHARP_VERSION, disclaimer::version());
    assert_eq!(16, disclaimer::version().len());
}

/// While the C# program is still in the tree: every text is its resx value byte for byte.
#[test]
fn texts_are_the_csharp_resx_values() {
    let res = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../win/src/DDM32Plus/Resources");
    let (Ok(en), Ok(tr)) =
        (std::fs::read_to_string(res.join("Strings.resx")), std::fs::read_to_string(res.join("Strings.tr.resx")))
    else {
        return; // win/ removed: CSHARP_VERSION above still holds the texts
    };
    let re = regex::Regex::new(r#"(?s)<data name="(Disc_[A-Za-z0-9]+)" xml:space="preserve">\s*<value>(.*?)</value>"#)
        .unwrap();
    let value = |text: &str, key: &str| -> String {
        let c = re.captures_iter(text).find(|c| &c[1] == key).unwrap_or_else(|| panic!("{key} not in the resx"));
        c[2].replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"").replace("&amp;", "&")
    };
    for (key, t, e) in disclaimer::BODY {
        assert_eq!(value(&tr, key), t, "{key} tr");
        assert_eq!(value(&en, key), e, "{key} en");
    }
}

#[test]
fn bold_marks_are_taken_out() {
    assert_eq!(("a b c".to_string(), false), disclaimer::plain("**a** b **c**"));
    assert_eq!(("all bold".to_string(), true), disclaimer::plain("**all bold**"));
    let v = disclaimer::view("en");
    assert!(v.title.starts_with("⚠ DISCLAIMER"));
    assert_eq!(5, v.points.len());
    assert!(v.outro_bold && !v.outro.contains("**"));
    assert!(v.intro.starts_with("DDM32 Plus is unofficial, experimental"));
    assert!(disclaimer::view("tr").points[4].contains("tam yedek"));
}

#[test]
fn accepted_in_the_csharp_program_is_not_asked_again() {
    let root = root("cs");
    settings_with(
        &root,
        serde_json::json!({"language": "en", "window": {"x": 1, "y": 1, "w": 900, "h": 700, "max": false},
            "disclaimer": {"version": CSHARP_VERSION, "accepted": true, "accepted_utc": "2026-11-20T10:00:00Z"}}),
    );
    let ui = window(&root);
    assert!(ui.global::<Radio>().get_disclaimer_ok());
    assert!(!ui.global::<App>().get_disc_open());
    assert_eq!(0, ui.global::<App>().get_page());
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn a_changed_text_or_a_withdrawal_asks_again() {
    for d in [
        serde_json::json!({"version": "0123456789abcdef", "accepted": true, "accepted_utc": "2026-11-01T00:00:00Z"}),
        serde_json::json!({"version": CSHARP_VERSION, "accepted": false, "withdrawn_utc": "2026-11-01T00:00:00Z"}),
    ] {
        let root = root("changed");
        settings_with(&root, serde_json::json!({"language": "tr", "disclaimer": d}));
        let ui = window(&root);
        assert!(!ui.global::<Radio>().get_disclaimer_ok());
        assert!(ui.global::<App>().get_disc_open(), "asked at start");
        assert!(!ui.global::<App>().get_disc_readonly());
        let _ = std::fs::remove_dir_all(&root);
    }
}

#[test]
fn first_start_asks_the_language_then_the_disclaimer() {
    let root = root("first");
    let ui = window(&root);
    let app = ui.global::<App>();
    assert_eq!(6, app.get_page());
    assert!(!app.get_disc_open(), "the language comes first");
    app.invoke_set_lang("en".into());
    app.invoke_navigate(0);
    assert_eq!(0, app.get_page());
    assert!(app.get_disc_open());
    assert_eq!("en", app.get_disc_lang().as_str());
    assert!(app.get_disc_title().starts_with("⚠ DISCLAIMER"));
    // its own language switch, as the C# dialog
    app.invoke_disc_toggle_lang();
    assert_eq!("tr", app.get_disc_lang().as_str());
    assert!(app.get_disc_title().starts_with("⚠ SORUMLULUK"));
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn every_radio_step_is_refused_until_accepted_then_saved_in_the_csharp_format() {
    let root = root("gate");
    settings_with(&root, serde_json::json!({"language": "en"}));
    let ui = window(&root);
    let app = ui.global::<App>();
    let radio = ui.global::<Radio>();
    let refused = i18n::t("en", "disc_not_accepted");
    // "I do not accept": nothing saved, the steps stay off
    app.invoke_disc_reject();
    assert!(!app.get_disc_open());
    assert!(!radio.get_disclaimer_ok());
    assert!(Settings::new(root.join("settings.json")).load().get("disclaimer").is_none());
    // every radio step: the wizard (download, backup, install), restore, pack write, health, live, search log
    type Step<'a> = (&'a str, Box<dyn Fn() + 'a>);
    let steps: Vec<Step<'_>> = vec![
        ("wizard next", Box::new(|| app.invoke_wizard_next())),
        ("backup", Box::new(|| app.invoke_start_backup())),
        ("install", Box::new(|| app.invoke_start_install())),
        ("restore", Box::new(|| app.invoke_restore())),
        ("pack write", Box::new(|| app.invoke_pack_write())),
        ("health", Box::new(|| radio.invoke_read_health())),
        ("live", Box::new(|| radio.invoke_live_start())),
        ("search log", Box::new(|| radio.invoke_slog_start())),
    ];
    for (name, step) in &steps {
        app.set_message("".into());
        radio.set_message("".into());
        step();
        assert_eq!(refused, radio.get_message().as_str(), "{name}");
        assert_eq!(refused, app.get_message().as_str(), "{name}");
        assert!(
            !app.get_busy() && !radio.get_busy() && !radio.get_live_running() && !radio.get_slog_running(),
            "{name}"
        );
    }
    // Home's gate button opens it again; "I accept" does nothing until the box is ticked
    radio.invoke_open_disclaimer();
    assert!(app.get_disc_open() && !app.get_disc_checked());
    app.invoke_disc_accept();
    assert!(app.get_disc_open() && !radio.get_disclaimer_ok());
    app.set_disc_checked(true);
    app.invoke_disc_accept();
    assert!(!app.get_disc_open());
    assert!(radio.get_disclaimer_ok());
    let saved = Settings::new(root.join("settings.json")).load();
    let d = saved["disclaimer"].as_object().unwrap();
    assert_eq!(CSHARP_VERSION, d["version"]);
    assert_eq!(true, d["accepted"]);
    assert!(d["accepted_utc"].as_str().unwrap().ends_with('Z'));
    assert_eq!("en", saved["language"], "the other keys are kept");
    assert!(Settings::new(root.join("settings.json")).disclaimer_accepted(CSHARP_VERSION));
    // Help > Disclaimer: read only now
    app.invoke_menu_action("disclaimer".into());
    assert!(app.get_disc_open() && app.get_disc_readonly());
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn the_help_menu_reaches_it() {
    assert_eq!(ddm32_gui::parse_menu_action("disclaimer"), Some(ddm32_gui::MenuAction::Disclaimer));
    let menu = std::fs::read_to_string(Path::new(env!("CARGO_MANIFEST_DIR")).join("ui/menubar.slint")).unwrap();
    assert!(menu.contains(r#"action: "disclaimer""#));
}

/// Every button that opens a port names the gate in its `enabled` (the window refuses in Rust too, above).
#[test]
fn every_radio_button_is_off_until_accepted() {
    let ui = std::fs::read_to_string(Path::new(env!("CARGO_MANIFEST_DIR")).join("ui/app.slint")).unwrap()
        + &std::fs::read_to_string(Path::new(env!("CARGO_MANIFEST_DIR")).join("ui/radio_pages.slint")).unwrap();
    for callback in [
        "App.start-backup()",
        "App.start-install()",
        "App.restore()",
        "App.pack-write()",
        "App.wizard-next()",
        "Radio.read-health()",
        "Radio.live-start()",
        "Radio.slog-start()",
    ] {
        let at = ui.find(callback).unwrap_or_else(|| panic!("{callback} not found"));
        let before = &ui[..at];
        let enabled = &before[before.rfind("enabled:").unwrap_or_else(|| panic!("{callback}: no enabled"))..];
        assert!(enabled.contains("Radio.disclaimer-ok"), "{callback}: not gated by the disclaimer");
    }
}
