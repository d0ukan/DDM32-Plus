// SPDX-License-Identifier: GPL-3.0-or-later
//! Keyboard and screen-reader pass over every page: every button, choice, check box and field has an accessible name
//! (in both languages), the primary button of a page holds the focus so Enter runs it, Tab walks the page and shows
//! the focus ring, and a check box toggles with the space bar. Rendered offscreen; no radio, no network.

use ddm32_gui::{App, AppWindow, Theme, Tr};
use i_slint_backend_testing::{AccessibleRole, ElementRoot};
use slint::platform::software_renderer::{MinimalSoftwareWindow, RepaintBufferType};
use slint::platform::{Key, Platform, WindowAdapter, WindowEvent};
use slint::{ComponentHandle, Rgb8Pixel, SharedString};
use std::cell::RefCell;
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

fn key(ui: &AppWindow, k: impl Into<SharedString>) {
    // conditional pages are built lazily: make sure the page on screen exists (and took its focus) first
    ui.window().request_redraw();
    ddm32_gui_test_draw(ui);
    let text: SharedString = k.into();
    ui.window().dispatch_event(WindowEvent::KeyPressed { text: text.clone() });
    ui.window().dispatch_event(WindowEvent::KeyReleased { text });
    slint::platform::update_timers_and_animations();
}

fn ddm32_gui_test_draw(ui: &AppWindow) {
    // a layout pass instantiates the `if` pages; nothing is painted
    let _ = ui.window().size();
    slint::platform::update_timers_and_animations();
    WINDOW.with(|w| {
        if let Some(w) = w.borrow().as_ref() {
            render(w);
        }
    });
}

thread_local! {
    static WINDOW: RefCell<Option<Rc<MinimalSoftwareWindow>>> = const { RefCell::new(None) };
}

/// Opens a page afresh (as the app does: the old page is gone before the new one is built).
fn open_page(ui: &AppWindow, page: i32) {
    ui.global::<App>().set_page(-1);
    ddm32_gui_test_draw(ui);
    ui.global::<App>().set_page(page);
    ddm32_gui_test_draw(ui);
}

fn shift_tab(ui: &AppWindow) {
    let shift: SharedString = Key::Shift.into();
    ui.window().dispatch_event(WindowEvent::KeyPressed { text: shift.clone() });
    key(ui, Key::Tab);
    ui.window().dispatch_event(WindowEvent::KeyReleased { text: shift });
}

fn ring_box(ui: &AppWindow, window: &MinimalSoftwareWindow) -> (usize, usize, usize, usize) {
    let f = ui.global::<Theme>().get_focus();
    let buf = render(window);
    let (mut x0, mut y0, mut x1, mut y1) = (usize::MAX, usize::MAX, 0, 0);
    for (i, p) in buf.iter().enumerate() {
        if (p.r, p.g, p.b) == (f.red(), f.green(), f.blue()) {
            let (x, y) = (i % W as usize, i / W as usize);
            x0 = x0.min(x);
            y0 = y0.min(y);
            x1 = x1.max(x);
            y1 = y1.max(y);
        }
    }
    (x0, y0, x1, y1)
}

/// Pixels painted in the focus-ring colour.
fn ring_pixels(ui: &AppWindow, window: &MinimalSoftwareWindow) -> usize {
    let f = ui.global::<Theme>().get_focus();
    render(window).iter().filter(|p| (p.r, p.g, p.b) == (f.red(), f.green(), f.blue())).count()
}

/// Every element a screen reader can act on, with its role and accessible name.
fn actionable(ui: &AppWindow) -> Vec<(AccessibleRole, String)> {
    let mut out = Vec::new();
    for role in [AccessibleRole::Button, AccessibleRole::Checkbox, AccessibleRole::TextInput] {
        for e in ui.root_element().query_descendants().match_accessible_role(role).find_all() {
            out.push((role, e.accessible_label().map(|l| l.to_string()).unwrap_or_default()));
        }
    }
    out
}

#[test]
fn keyboard_and_screen_reader() {
    let window = MinimalSoftwareWindow::new(RepaintBufferType::NewBuffer);
    slint::platform::set_platform(Box::new(Offscreen { window: window.clone() })).unwrap();
    window.set_size(slint::PhysicalSize::new(W, H));
    WINDOW.with(|w| *w.borrow_mut() = Some(window.clone()));
    let ui = AppWindow::new().unwrap();
    ddm32_gui::install_texts(&ui);
    ui.show().unwrap();
    ui.window().dispatch_event(WindowEvent::WindowActiveChanged(true));
    let app = ui.global::<App>();
    // record what the buttons ask for instead of running the controller
    let asked: Rc<RefCell<Vec<String>>> = Rc::default();
    {
        let (a, w) = (asked.clone(), ui.as_weak());
        app.on_navigate(move |p| {
            a.borrow_mut().push(format!("navigate {p}"));
            if let Some(ui) = w.upgrade() {
                ui.global::<App>().set_page(p);
            }
        });
        let (a, w) = (asked.clone(), ui.as_weak());
        app.on_set_lang(move |l| {
            a.borrow_mut().push(format!("lang {l}"));
            if let Some(ui) = w.upgrade() {
                ui.global::<Tr>().set_lang(l);
            }
        });
        let a = asked.clone();
        app.on_wizard_next(move || a.borrow_mut().push("next".into()));
        let a = asked.clone();
        app.on_set_auto_check(move |v| a.borrow_mut().push(format!("auto {v}")));
    }

    // 1. accessible names on every page, wizard step and the confirmation, in both languages and both themes
    let screens: Vec<(i32, i32, i32, bool)> = vec![
        (6, 0, 0, false),
        (0, 0, 0, false),
        (1, 0, 0, false),
        (1, 1, 0, false),
        (1, 1, 2, false),
        (1, 1, 2, true),
        (1, 2, 0, false),
        (1, 3, 0, false),
        (1, 4, 0, false),
        (1, 5, 0, false),
        (2, 0, 0, false),
        (3, 0, 0, false),
        (4, 0, 0, false),
        (5, 0, 0, false),
        (7, 0, 0, false),
        (8, 0, 0, false),
        (9, 0, 0, false),
        (11, 0, 0, false),
        (12, 0, 0, false),
    ];
    for lang in ["tr", "en"] {
        ui.global::<Tr>().set_lang(lang.into());
        for dark in [false, true] {
            ui.global::<Theme>().set_dark(dark);
            for (page, step, choice, confirm) in &screens {
                ddm32_gui::demo(&ui);
                app.set_page(*page);
                app.set_step(*step);
                app.set_backup_choice(*choice);
                app.set_understood(*choice == 2);
                app.set_confirm_open(*confirm);
                app.set_step_done(*step == 3);
                app.set_busy(false);
                let found = actionable(&ui);
                let buttons = found.iter().filter(|(r, _)| *r == AccessibleRole::Button).count();
                assert!(buttons > 0, "{lang} page {page} step {step}: no button for a screen reader");
                for (role, label) in &found {
                    assert!(!label.trim().is_empty(), "{lang} page {page} step {step}: a {role:?} has no name");
                }
                // the progress bar tells its value
                for e in ui
                    .root_element()
                    .query_descendants()
                    .match_accessible_role(AccessibleRole::ProgressIndicator)
                    .find_all()
                {
                    assert!(e.accessible_value().is_some(), "page {page}: progress without a value");
                }
            }
        }
    }
    app.set_confirm_open(false);
    ui.global::<Theme>().set_dark(false);
    ui.global::<Tr>().set_lang("tr".into());

    // 2. first start: the Turkish button holds the focus, Enter picks it
    asked.borrow_mut().clear();
    open_page(&ui, 6);
    key(&ui, Key::Return);
    assert_eq!(*asked.borrow(), vec!["lang tr".to_string(), "navigate 0".into()]);

    // 3. Home: Enter on the big update button opens the wizard (it took the focus when Home opened)
    asked.borrow_mut().clear();
    open_page(&ui, 0);
    assert!(ring_pixels(&ui, &window) > 100, "Home: the primary button shows no focus ring");
    key(&ui, Key::Return);
    assert_eq!(*asked.borrow(), vec!["navigate 1".to_string()]);
    // the wizard's Next button holds the focus at once
    asked.borrow_mut().clear();
    app.set_step(0);
    key(&ui, Key::Return);
    assert_eq!(*asked.borrow(), vec!["next".to_string()]);

    // 4. Home again: Tab moves on to Backups, the space bar runs it; Shift+Tab goes back
    asked.borrow_mut().clear();
    open_page(&ui, 0);
    key(&ui, Key::Tab);
    key(&ui, " ");
    assert_eq!(*asked.borrow(), vec!["navigate 2".to_string()]);
    asked.borrow_mut().clear();
    open_page(&ui, 0);
    let start = ring_box(&ui, &window);
    key(&ui, Key::Tab);
    key(&ui, Key::Tab);
    shift_tab(&ui);
    key(&ui, Key::Backtab);
    assert_eq!(ring_box(&ui, &window), start, "Shift+Tab does not walk back to the primary button");
    key(&ui, Key::Return);
    assert_eq!(*asked.borrow(), vec!["navigate 1".to_string()]);

    // 5. Tab reaches something on every page and the ring is drawn
    for (page, step) in [(1, 0), (1, 1), (2, 0), (3, 0), (4, 0), (5, 0), (7, 0), (8, 0), (9, 0), (11, 0), (12, 0)] {
        ddm32_gui::demo(&ui);
        open_page(&ui, page);
        app.set_step(step);
        key(&ui, Key::Tab);
        assert!(ring_pixels(&ui, &window) > 40, "page {page} step {step}: Tab shows no focus ring");
        // and walking the whole page never gets stuck on one element
        let mut seen = std::collections::HashSet::new();
        for _ in 0..30 {
            key(&ui, Key::Tab);
            seen.insert(render(&window).iter().map(|p| p.r as u64 + p.g as u64 * 3 + p.b as u64 * 7).sum::<u64>());
        }
        assert!(seen.len() > 1, "page {page}: Tab does not move");
    }

    // 6. a check box toggles with the space bar (Settings: the automatic check)
    ddm32_gui::demo(&ui);
    open_page(&ui, 4);
    let before = app.get_auto_check();
    let check = ui
        .root_element()
        .query_descendants()
        .match_accessible_role(AccessibleRole::Checkbox)
        .find_first()
        .expect("Settings has a check box");
    assert_eq!(check.accessible_checked(), Some(before));
    asked.borrow_mut().clear();
    let mut toggled = false;
    for n in 1..30 {
        asked.borrow_mut().clear();
        open_page(&ui, 4);
        for _ in 0..n {
            key(&ui, Key::Tab);
        }
        key(&ui, " ");
        if asked.borrow().iter().any(|a| a.starts_with("auto ")) {
            toggled = true;
            break;
        }
    }
    assert!(toggled, "the check box cannot be reached and toggled from the keyboard");
}
