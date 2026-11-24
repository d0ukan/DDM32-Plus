// SPDX-License-Identifier: GPL-3.0-or-later
//! High-DPI pass: Home, the first two wizard screens and Settings at 150 % and 200 % scaling, light and dark, at the
//! default window size and at the smallest one (what a 1366x768 laptop at 150 % or a 1920x1080 screen at 200 % leaves).
//! Buttons, choices and check boxes must neither overlap nor leave the window. The 150 % / 200 % screenshots in
//! rust/docs/screenshots are written here. Rendered offscreen; no radio, no network.

use ddm32_gui::{App, AppWindow, Theme, Tr};
use i_slint_backend_testing::{AccessibleRole, ElementHandle, ElementRoot};
use slint::platform::software_renderer::{MinimalSoftwareWindow, RepaintBufferType};
use slint::platform::{Platform, WindowAdapter, WindowEvent};
use slint::{ComponentHandle, Rgb8Pixel};
use std::path::Path;
use std::rc::Rc;

struct Offscreen {
    window: Rc<MinimalSoftwareWindow>,
}

impl Platform for Offscreen {
    fn create_window_adapter(&self) -> Result<Rc<dyn WindowAdapter>, slint::PlatformError> {
        Ok(self.window.clone())
    }
}

fn render(window: &MinimalSoftwareWindow, w: u32, h: u32) -> Vec<Rgb8Pixel> {
    let mut buf = vec![Rgb8Pixel { r: 0, g: 0, b: 0 }; (w * h) as usize];
    slint::platform::update_timers_and_animations();
    window.request_redraw();
    window.draw_if_needed(|r| {
        r.render(&mut buf, w as usize);
    });
    buf
}

fn save_png(path: &Path, buf: &[Rgb8Pixel], w: u32, h: u32) {
    let f = std::fs::File::create(path).unwrap();
    let mut enc = png::Encoder::new(std::io::BufWriter::new(f), w, h);
    enc.set_color(png::ColorType::Rgb);
    enc.set_depth(png::BitDepth::Eight);
    let mut wr = enc.write_header().unwrap();
    let data: Vec<u8> = buf.iter().flat_map(|p| [p.r, p.g, p.b]).collect();
    wr.write_image_data(&data).unwrap();
}

#[derive(Debug, Clone, Copy)]
struct Rect {
    x: f32,
    y: f32,
    w: f32,
    h: f32,
}

impl Rect {
    fn overlaps(&self, o: &Rect) -> bool {
        // a shared edge (or a hair of rounding) is no overlap
        let e = 0.5;
        self.x + e < o.x + o.w && o.x + e < self.x + self.w && self.y + e < o.y + o.h && o.y + e < self.y + self.h
    }
}

/// The controls on screen: (role, name, rectangle in logical pixels).
fn controls(ui: &AppWindow) -> Vec<(AccessibleRole, String, Rect)> {
    let mut out = Vec::new();
    for role in [AccessibleRole::Button, AccessibleRole::Checkbox] {
        for e in ui.root_element().query_descendants().match_accessible_role(role).find_all() {
            let (p, s) = (e.absolute_position(), e.size());
            if s.width <= 0.0 || s.height <= 0.0 {
                continue;
            }
            let name = e.accessible_label().map(|l| l.to_string()).unwrap_or_default();
            out.push((role, name, Rect { x: p.x, y: p.y, w: s.width, h: s.height }));
        }
    }
    out
}

#[test]
fn scaled_150_and_200_percent() {
    let window = MinimalSoftwareWindow::new(RepaintBufferType::NewBuffer);
    slint::platform::set_platform(Box::new(Offscreen { window: window.clone() })).unwrap();
    let ui = AppWindow::new().unwrap();
    ddm32_gui::install_texts(&ui);
    ui.show().unwrap();
    let app = ui.global::<App>();
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../docs/screenshots");
    std::fs::create_dir_all(&dir).unwrap();
    // (name, page, wizard step, dialog); a dialog is checked over an empty page (the page under its shade is not
    // reachable while it is open)
    let screens = [
        ("home", 0, 0, ""),
        ("update-1-version", 1, 0, ""),
        ("update-2-backup-choice", 1, 1, ""),
        ("settings", 4, 0, ""),
        ("update-prompt", -1, 0, "prompt"),
        ("cable-picker", -1, 0, "picker"),
        ("disclaimer", -1, 0, "disclaimer"),
        ("disclaimer-read", -1, 0, "disclaimer-read"),
        ("test-confirm", -1, 0, "test-confirm"),
        ("home-not-accepted", 0, 0, "gate"),
    ];
    for lang in ["tr", "en"] {
        ui.global::<Tr>().set_lang(lang.into());
        for (scale, tag) in [(1.5f32, "150"), (2.0, "200")] {
            ui.window().dispatch_event(WindowEvent::ScaleFactorChanged { scale_factor: scale });
            for (lw, lh) in [(900u32, 720u32), (640, 560)] {
                let (w, h) = ((lw as f32 * scale) as u32, (lh as f32 * scale) as u32);
                window.set_size(slint::PhysicalSize::new(w, h));
                for (name, page, step, dialog) in screens {
                    for dark in [false, true] {
                        ui.global::<Theme>().set_dark(dark);
                        ddm32_gui::demo(&ui);
                        app.set_page(-1);
                        render(&window, w, h);
                        app.set_page(page);
                        app.set_step(step);
                        app.set_backup_choice(0);
                        app.set_busy(false);
                        if dialog == "prompt" {
                            let offer = ddm32_gui::UpdateOffer {
                                program: Some(("3.0.0".into(), "3.1.0".into())),
                                firmware: Some(("1.0.0".into(), "1.0.1".into())),
                            };
                            ddm32_gui::show_prompt(&ui, lang, &offer);
                        }
                        match dialog {
                            "disclaimer" => ddm32_gui::show_disclaimer(&ui, lang, false),
                            "disclaimer-read" => ddm32_gui::show_disclaimer(&ui, lang, true),
                            "test-confirm" => {
                                let text = ddm32_gui::i18n::tf(lang, "ver_test_confirm", "1.2.0-alpha.1");
                                app.set_test_confirm_text(text.into());
                                app.set_test_confirm_open(true);
                            }
                            "gate" => ui.global::<ddm32_gui::Radio>().set_disclaimer_ok(false),
                            _ => {}
                        }
                        if dialog == "picker" {
                            ui.global::<ddm32_gui::Radio>().set_picker_open(true);
                            ui.global::<ddm32_gui::Radio>().set_picker_advanced(true);
                        }
                        let buf = render(&window, w, h);
                        let ls = ui.window().size().to_logical(scale);
                        assert!(
                            (ls.width - lw as f32).abs() < 1.0 && (ls.height - lh as f32).abs() < 1.0,
                            "{name}: the window is {ls:?} logical, not {lw}x{lh}"
                        );
                        let mut found = controls(&ui);
                        assert!(!found.is_empty(), "{name}: no controls found");
                        // a page's middle part may scroll: what lies beyond its viewport is clipped, not cut. The
                        // header above it and the wizard's button row below it do not scroll.
                        if let Some(view) = ElementHandle::find_by_element_type_name(&ui, "ScrollView").next() {
                            let (p, s) = (view.absolute_position(), view.size());
                            let view = Rect { x: p.x, y: p.y, w: s.width, h: s.height };
                            found.retain(|(_, n, r)| {
                                let fixed_row = matches!(n.as_str(), "İleri" | "Next" | "Geri" | "Back");
                                let clipped = r.y >= view.y - 0.5 && r.y + r.h > view.y + view.h + 0.5;
                                fixed_row || !clipped
                            });
                        }
                        for (i, (_, a, ra)) in found.iter().enumerate() {
                            for (_, b, rb) in &found[i + 1..] {
                                assert!(!ra.overlaps(rb), "{lang} {tag}% {lw}x{lh} {name}: '{a}' overlaps '{b}'");
                            }
                            let inside = ra.x >= -0.5
                                && ra.y >= -0.5
                                && ra.x + ra.w <= lw as f32 + 0.5
                                && ra.y + ra.h <= lh as f32 + 0.5;
                            assert!(inside, "{lang} {tag}% {lw}x{lh} {name}: '{a}' leaves the window: {ra:?}");
                            // a desktop tool's normal control: well above WCAG 2.2's 24 px target minimum
                            assert!(ra.w >= 32.0 && ra.h >= 32.0, "{name}: '{a}' is smaller than a target: {ra:?}");
                        }
                        if lang == "tr" && lw == 900 {
                            let file = format!("{name}-{}-{lang}-{tag}.png", if dark { "dark" } else { "light" });
                            save_png(&dir.join(file), &buf, w, h);
                        }
                        if lang == "tr" && lw == 640 {
                            if let Ok(out) = std::env::var("DDM32_SHOT_DIR") {
                                let file =
                                    format!("{name}-{}-{lang}-{tag}-small.png", if dark { "dark" } else { "light" });
                                save_png(&Path::new(&out).join(file), &buf, w, h);
                            }
                        }
                    }
                }
            }
        }
    }
}
