// SPDX-License-Identifier: GPL-3.0-or-later
//! Every screen in both themes (and both languages), rendered offscreen with Slint's software renderer: the
//! screenshots in rust/docs/screenshots are written by this test. Also: every text / background pair of both palettes
//! meets WCAG AA, the theme switches live on every page, no colour is written outside ui/theme.slint, and every text
//! key the UI asks for exists in both languages. No radio, no network, no file of the user.

use ddm32_gui::{i18n, App, AppWindow, Theme, Tr};
use slint::platform::software_renderer::{MinimalSoftwareWindow, RepaintBufferType};
use slint::platform::{Platform, WindowAdapter};
use slint::{Color, ComponentHandle, Rgb8Pixel};
use std::path::{Path, PathBuf};
use std::rc::Rc;

const W: u32 = 900;
const H: u32 = 720;

struct Offscreen {
    window: Rc<MinimalSoftwareWindow>,
}

impl Platform for Offscreen {
    fn create_window_adapter(&self) -> Result<Rc<dyn WindowAdapter>, slint::PlatformError> {
        Ok(self.window.clone())
    }
}

fn render(window: &MinimalSoftwareWindow) -> Vec<Rgb8Pixel> {
    let mut buf = vec![Rgb8Pixel { r: 0, g: 0, b: 0 }; (W * H) as usize];
    slint::platform::update_timers_and_animations();
    window.request_redraw();
    window.draw_if_needed(|r| {
        r.render(&mut buf, W as usize);
    });
    buf
}

fn save_png(path: &Path, buf: &[Rgb8Pixel]) {
    let f = std::fs::File::create(path).unwrap();
    let mut enc = png::Encoder::new(std::io::BufWriter::new(f), W, H);
    enc.set_color(png::ColorType::Rgb);
    enc.set_depth(png::BitDepth::Eight);
    let mut w = enc.write_header().unwrap();
    let data: Vec<u8> = buf.iter().flat_map(|p| [p.r, p.g, p.b]).collect();
    w.write_image_data(&data).unwrap();
}

fn lum(c: Color) -> f64 {
    let ch = |v: u8| {
        let s = v as f64 / 255.0;
        if s <= 0.03928 {
            s / 12.92
        } else {
            ((s + 0.055) / 1.055).powf(2.4)
        }
    };
    0.2126 * ch(c.red()) + 0.7152 * ch(c.green()) + 0.0722 * ch(c.blue())
}

fn contrast(a: Color, b: Color) -> f64 {
    let (x, y) = (lum(a), lum(b));
    (x.max(y) + 0.05) / (x.min(y) + 0.05)
}

fn shots() -> PathBuf {
    let p = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../docs/screenshots");
    std::fs::create_dir_all(&p).unwrap();
    p
}

fn check_palette(t: &Theme, which: &str) {
    // (text, background, minimum): body text 4.5:1, large text and focus / borders 3:1
    let pairs: Vec<(&str, Color, Color, f64)> = vec![
        ("text on window", t.get_text(), t.get_window(), 4.5),
        ("text on surface", t.get_text(), t.get_surface(), 4.5),
        ("text on surface-alt", t.get_text(), t.get_surface_alt(), 4.5),
        ("muted on window", t.get_text_muted(), t.get_window(), 4.5),
        ("muted on surface", t.get_text_muted(), t.get_surface(), 4.5),
        ("muted on surface-alt", t.get_text_muted(), t.get_surface_alt(), 4.5),
        ("primary-text on primary", t.get_primary_text(), t.get_primary(), 4.5),
        ("primary-text on primary-hover", t.get_primary_text(), t.get_primary_hover(), 4.5),
        ("button-text on button", t.get_button_text(), t.get_button(), 4.5),
        ("button-text on button-hover", t.get_button_text(), t.get_button_hover(), 4.5),
        ("disabled-text on disabled", t.get_disabled_text(), t.get_disabled(), 4.5),
        ("danger on surface", t.get_danger(), t.get_surface(), 4.5),
        ("danger on window", t.get_danger(), t.get_window(), 4.5),
        ("window on danger (danger button)", t.get_window(), t.get_danger(), 4.5),
        ("danger-text on danger-bg", t.get_danger_text(), t.get_danger_bg(), 4.5),
        ("warning-text on warning-bg", t.get_warning_text(), t.get_warning_bg(), 4.5),
        ("success-text on success-bg", t.get_success_text(), t.get_success_bg(), 4.5),
        ("text on success-bg", t.get_text(), t.get_success_bg(), 4.5),
        ("accent-text on window", t.get_accent_text(), t.get_window(), 4.5),
        ("accent-text on surface", t.get_accent_text(), t.get_surface(), 4.5),
        ("success on window (a step done)", t.get_success(), t.get_window(), 4.5),
        ("success on surface", t.get_success(), t.get_surface(), 4.5),
        ("success bar on surface", t.get_success(), t.get_surface(), 3.0),
        ("warning bar on surface", t.get_warning(), t.get_surface(), 3.0),
        ("danger bar on surface", t.get_danger(), t.get_surface(), 3.0),
        ("primary on surface (selected border)", t.get_primary(), t.get_surface(), 3.0),
        ("focus ring on primary (a focused main button)", t.get_focus(), t.get_primary(), 1.5),
        ("focus ring on surface", t.get_focus(), t.get_surface(), 3.0),
        ("focus ring on window", t.get_focus(), t.get_window(), 3.0),
        ("border on surface", t.get_border(), t.get_surface(), 1.4),
    ];
    for (name, fg, bg, min) in pairs {
        let c = contrast(fg, bg);
        assert!(c >= min, "{which}: {name} contrast {c:.2} < {min}");
    }
}

#[test]
fn every_screen_in_both_themes() {
    let window = MinimalSoftwareWindow::new(RepaintBufferType::NewBuffer);
    slint::platform::set_platform(Box::new(Offscreen { window: window.clone() })).unwrap();
    window.set_size(slint::PhysicalSize::new(W, H));
    let ui = AppWindow::new().unwrap();
    ddm32_gui::install_texts(&ui);
    ui.show().unwrap();
    let theme = ui.global::<Theme>();
    assert!(!theme.get_dark(), "light is the default theme");
    let app = ui.global::<App>();
    // (file name, page, wizard step, backup choice, extra)
    let screens: Vec<(&str, i32, i32, i32, &str)> = vec![
        ("first-start", 6, 0, 0, ""),
        ("home", 0, 0, 0, ""),
        ("home-cable-no-driver", 0, 0, 0, "nodriver"),
        ("home-update-prompt", 0, 0, 0, "prompt"),
        ("home-cable-picker", 0, 0, 0, "picker"),
        ("home-check-error", 0, 0, 0, "check-error"),
        ("home-port-missing", 0, 0, 0, "port-missing"),
        ("home-disclaimer", 0, 0, 0, "disclaimer"),
        ("home-disclaimer-read", 0, 0, 0, "disclaimer-read"),
        ("home-not-accepted", 0, 0, 0, "gate"),
        ("health-not-accepted", 7, 0, 0, "gate"),
        ("update-1-version", 1, 0, 0, ""),
        ("update-2-backup-choice", 1, 1, 0, ""),
        ("update-2-no-backup-warning", 1, 1, 2, "understood"),
        ("update-2-no-backup-confirm", 1, 1, 2, "confirm"),
        ("update-3-backup-power", 1, 2, 0, ""),
        ("update-3-backup-progress", 1, 2, 0, "backup-progress"),
        ("update-4-check", 1, 3, 0, "details"),
        ("update-5-install-power", 1, 4, 0, "progress"),
        ("update-6-done", 1, 5, 0, ""),
        ("backups", 2, 0, 0, ""),
        ("packs", 3, 0, 0, ""),
        ("settings", 4, 0, 0, ""),
        ("about", 5, 0, 0, ""),
        ("health", 7, 0, 0, ""),
        ("live", 8, 0, 0, ""),
        ("channels", 9, 0, 0, ""),
        // a dialog takes the keyboard focus: last, so the wizard shots above keep their focused Next
        ("update-1-test-confirm", 1, 0, 0, "test-confirm"),
    ];
    let dir = shots();
    for lang in ["tr", "en"] {
        ui.global::<Tr>().set_lang(lang.into());
        for (name, page, step, choice, extra) in &screens {
            for dark in [false, true] {
                ddm32_gui::demo(&ui);
                app.set_page(*page);
                app.set_step(*step);
                app.set_backup_choice(*choice);
                app.set_understood(*extra == "understood" || *extra == "confirm");
                app.set_confirm_open(*extra == "confirm");
                app.set_show_details(*extra == "details");
                if *step != 3 {
                    app.set_details("".into());
                }
                app.set_step_done(*step == 3);
                // the progress area as the tool lines leave it (ProgressView): the run's bar, the phase's percent
                // line and the last other tool line
                let (bar, text, info) = match *extra {
                    "progress" => (0.46, i18n::tf(lang, "Prog_Write", "40"), "INSTALLING: 23 application sectors"),
                    "backup-progress" => (0.5, i18n::tf(lang, "Prog_Backup", "50"), "data flash JEDEC ID ef4018"),
                    _ => (0.0, String::new(), ""),
                };
                app.set_progress(bar);
                app.set_busy(false);
                app.set_progress_text(text.into());
                app.set_progress_info(info.into());
                app.set_power_prompt(i18n::t(lang, if *step == 2 { "power_backup" } else { "power_install" }).into());
                app.set_message(if *page == 2 { i18n::t(lang, "msg_restore_plan").into() } else { "".into() });
                app.set_selected_backup(if *page == 2 { 0 } else { -1 });
                app.set_restore_ready(*page == 2);
                if *extra == "nodriver" {
                    let st = ddm32_core::radio::cable::evaluate(&[ddm32_core::radio::cable::PnpDevice {
                        device_id: r"USB\VID_1A86&PID_7523\5&2A1B&0&4".into(),
                        name: "USB2.0-Serial".into(),
                        error_code: 28,
                    }]);
                    let (t, k) = ddm32_gui::cable_text(lang, &st, &[]);
                    app.set_radio_text(t.into());
                    app.set_radio_kind(k.into());
                }
                match *extra {
                    "prompt" => {
                        let offer = ddm32_gui::UpdateOffer {
                            program: Some(("3.0.0".into(), "3.1.0".into())),
                            firmware: Some(("1.0.0".into(), "1.0.1".into())),
                        };
                        ddm32_gui::show_prompt(&ui, lang, &offer);
                    }
                    "picker" => {
                        ui.global::<ddm32_gui::Radio>().set_picker_open(true);
                        ui.global::<ddm32_gui::Radio>().set_picker_advanced(true);
                    }
                    "port-missing" => {
                        app.set_port_missing(i18n::tf(lang, "port_missing", "COM15").into());
                        let auto = i18n::tf(lang, "cable_auto", "COM14");
                        let head =
                            i18n::t(lang, "port_missing_head").replacen("{}", "COM15", 1).replacen("{}", &auto, 1);
                        ui.global::<ddm32_gui::Radio>().set_port_head(i18n::tf(lang, "cable_head", &head).into());
                    }
                    "check-error" => app
                        .set_check_error(i18n::tf(lang, "check_failed", "NetError: api.github.com: timed out").into()),
                    "disclaimer" => {
                        ui.global::<ddm32_gui::Radio>().set_disclaimer_ok(false);
                        ddm32_gui::show_disclaimer(&ui, lang, false);
                    }
                    "disclaimer-read" => ddm32_gui::show_disclaimer(&ui, lang, true),
                    "gate" => ui.global::<ddm32_gui::Radio>().set_disclaimer_ok(false),
                    "test-confirm" => {
                        app.set_test_confirm_text(i18n::tf(lang, "ver_test_confirm", "1.2.0-alpha.1").into());
                        app.set_test_confirm_open(true);
                    }
                    _ => {}
                }
                // switch the theme live on this very page
                theme.set_dark(dark);
                let buf = render(&window);
                // the window background follows the theme at once (Home's bottom corner is its facts bar, a surface)
                let bg = if *page == 0 { theme.get_surface() } else { theme.get_window() };
                let corner = buf[(H as usize - 2) * W as usize + 2];
                let modal = matches!(
                    *extra,
                    "confirm" | "prompt" | "picker" | "disclaimer" | "disclaimer-read" | "test-confirm"
                );
                if !modal {
                    assert_eq!(
                        (bg.red(), bg.green(), bg.blue()),
                        (corner.r, corner.g, corner.b),
                        "{name} {dark}: the theme did not switch"
                    );
                }
                // the header row is a surface bar across the top on every page
                if !matches!(*name, "first-start") && !modal {
                    let s = theme.get_surface();
                    let top = buf[2 * W as usize + W as usize / 2];
                    assert_eq!((s.red(), s.green(), s.blue()), (top.r, top.g, top.b), "{name} {dark}: no header row");
                }
                if lang == "tr"
                    || matches!(
                        *name,
                        "home"
                            | "update-2-backup-choice"
                            | "update-3-backup-power"
                            | "home-update-prompt"
                            | "home-cable-picker"
                            | "home-disclaimer"
                            | "home-disclaimer-read"
                            | "home-not-accepted"
                            | "update-1-test-confirm"
                    )
                {
                    let file = format!("{name}-{}-{lang}.png", if dark { "dark" } else { "light" });
                    save_png(&dir.join(file), &buf);
                }
            }
        }
    }
    theme.set_dark(false);
    check_palette(&theme, "light");
    theme.set_dark(true);
    check_palette(&theme, "dark");
}

#[test]
fn no_colour_outside_the_theme_and_every_text_exists() {
    let ui_dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("ui");
    let hex = regex_lite(r"#[0-9a-fA-F]{3,8}\b");
    let key = regex_lite(r#"Tr\.t\(Tr\.lang, "([a-z_0-9]+)"\)"#);
    for entry in std::fs::read_dir(&ui_dir).unwrap() {
        let p = entry.unwrap().path();
        if p.extension().is_none_or(|e| e != "slint") {
            continue;
        }
        let text = std::fs::read_to_string(&p).unwrap();
        let name = p.file_name().unwrap().to_string_lossy().into_owned();
        if name != "theme.slint" {
            let code: String =
                text.lines().filter(|l| !l.trim_start().starts_with("//")).collect::<Vec<_>>().join("\n");
            assert!(hex(&code).is_empty(), "{name}: colours outside the theme: {:?}", hex(&code));
            for word in ["Colors.", "color: white", "color: black", "background: white", "background: black"] {
                assert!(!code.contains(word), "{name}: {word}");
            }
        }
        for k in key(&text) {
            assert!(i18n::TEXTS.iter().any(|(x, _, _)| *x == k), "{name}: text key {k} is missing");
        }
    }
    // every Rust-side key too
    let src = std::fs::read_to_string(Path::new(env!("CARGO_MANIFEST_DIR")).join("src/lib.rs")).unwrap();
    let rkey = regex_lite(r#"\btrf?\("([a-z_0-9]+)""#);
    for k in rkey(&src) {
        assert!(i18n::TEXTS.iter().any(|(x, _, _)| *x == k), "lib.rs: text key {k} is missing");
    }
    for (k, t, e) in i18n::TEXTS {
        assert!(!t.is_empty() && !e.is_empty(), "{k}: a language is empty");
        assert_eq!(t.matches("{}").count(), e.matches("{}").count(), "{k}: placeholders differ");
    }
}

/// A tiny regex finder for the test (std only would be clumsy): the captured group 1, or the whole match.
fn regex_lite(pattern: &str) -> impl Fn(&str) -> Vec<String> {
    let re = regex::Regex::new(pattern).unwrap();
    move |s: &str| {
        re.captures_iter(s).map(|c| c.get(1).unwrap_or_else(|| c.get(0).unwrap()).as_str().to_string()).collect()
    }
}
