// SPDX-License-Identifier: GPL-3.0-or-later
//! The dialogs (the update question, the cable picker, the no-backup confirmation) are modal for the keyboard too:
//! while one is open, Tab never reaches a control under its shade and Enter / Space run only the dialog's own
//! buttons; the controls under it read as disabled to a screen reader. The question's default button is "Later".
//! Rendered offscreen; no radio, no network.

use ddm32_gui::{App, AppWindow, Modal, Radio, Tr, UpdateOffer};
use i_slint_backend_testing::{AccessibleRole, ElementHandle};
use slint::platform::software_renderer::{MinimalSoftwareWindow, RepaintBufferType};
use slint::platform::{Key, Platform, WindowAdapter, WindowEvent};
use slint::{ComponentHandle, Rgb8Pixel, SharedString};
use std::cell::RefCell;
use std::rc::Rc;

struct Offscreen {
    window: Rc<MinimalSoftwareWindow>,
}

impl Platform for Offscreen {
    fn create_window_adapter(&self) -> Result<Rc<dyn WindowAdapter>, slint::PlatformError> {
        Ok(self.window.clone())
    }
}

fn draw(window: &MinimalSoftwareWindow) {
    let mut buf = vec![Rgb8Pixel { r: 0, g: 0, b: 0 }; 900 * 720];
    slint::platform::update_timers_and_animations();
    window.request_redraw();
    window.draw_if_needed(|r| {
        r.render(&mut buf, 900);
    });
}

fn key(ui: &AppWindow, window: &MinimalSoftwareWindow, k: impl Into<SharedString>) {
    draw(window);
    let text: SharedString = k.into();
    ui.window().dispatch_event(WindowEvent::KeyPressed { text: text.clone() });
    ui.window().dispatch_event(WindowEvent::KeyReleased { text });
    slint::platform::update_timers_and_animations();
}

fn button(ui: &AppWindow, label: &str) -> ElementHandle {
    ElementHandle::find_by_accessible_label(ui, label)
        .find(|e| e.accessible_role() == Some(AccessibleRole::Button))
        .unwrap_or_else(|| panic!("no button {label}"))
}

#[test]
fn dialogs_hold_the_keyboard() {
    let window = MinimalSoftwareWindow::new(RepaintBufferType::NewBuffer);
    slint::platform::set_platform(Box::new(Offscreen { window: window.clone() })).unwrap();
    window.set_size(slint::PhysicalSize::new(900, 720));
    let ui = AppWindow::new().unwrap();
    ddm32_gui::install_texts(&ui);
    ui.global::<Tr>().set_lang("en".into());
    ui.show().unwrap();
    ddm32_gui::demo(&ui);
    let app = ui.global::<App>();
    // every callback a control under the dialogs could run, and the dialogs' own
    let fired: Rc<RefCell<Vec<String>>> = Rc::default();
    let rec = |name: &'static str| {
        let f = fired.clone();
        move || f.borrow_mut().push(name.into())
    };
    {
        let f = fired.clone();
        app.on_navigate(move |p| f.borrow_mut().push(format!("navigate {p}")));
        let f = fired.clone();
        app.on_menu_action(move |a| f.borrow_mut().push(format!("menu {a}")));
    }
    app.on_open_backups_folder(rec("open folder"));
    app.on_prompt_later(rec("later"));
    app.on_prompt_skip_program(rec("skip program"));
    app.on_prompt_skip_firmware(rec("skip firmware"));
    app.on_prompt_update_program(rec("update program"));
    app.on_prompt_update_firmware(rec("update firmware"));
    let radio = ui.global::<Radio>();
    let f = fired.clone();
    radio.on_port_choose(move |p| f.borrow_mut().push(format!("port {p}")));
    radio.on_port_rescan(rec("rescan"));
    let offer = UpdateOffer {
        program: Some(("3.0.0".into(), "3.1.0".into())),
        firmware: Some(("1.0.0".into(), "1.0.1".into())),
    };
    let dialog = ["later", "skip program", "skip firmware", "update program", "update firmware"];

    // the question: Enter at once runs Later (its default button)
    app.set_page(0);
    draw(&window);
    ddm32_gui::show_prompt(&ui, "en", &offer);
    draw(&window);
    assert!(ui.global::<Modal>().get_open());
    key(&ui, &window, Key::Return);
    assert_eq!(vec!["later".to_string()], *fired.borrow(), "Enter is not Later");
    // Tab n times, then Space: always one of the dialog's buttons, never Home's
    for n in 1..14 {
        fired.borrow_mut().clear();
        app.set_prompt_open(false);
        draw(&window);
        ddm32_gui::show_prompt(&ui, "en", &offer);
        draw(&window);
        assert_eq!(Some(false), button(&ui, "Update the radio").accessible_enabled(), "Home's button reads as live");
        assert_eq!(Some(true), button(&ui, "Later").accessible_enabled());
        for _ in 0..n {
            key(&ui, &window, Key::Tab);
        }
        key(&ui, &window, " ");
        let got = fired.borrow().clone();
        assert_eq!(1, got.len(), "Tab x{n}: {got:?}");
        assert!(dialog.contains(&got[0].as_str()), "Tab x{n} reached a control under the dialog: {got:?}");
    }
    app.set_prompt_open(false);
    draw(&window);
    assert!(!ui.global::<Modal>().get_open());
    assert_eq!(Some(true), button(&ui, "Update the radio").accessible_enabled());

    // the cable picker: the same
    for n in 1..12 {
        fired.borrow_mut().clear();
        radio.set_picker_open(true);
        draw(&window);
        for _ in 0..n {
            key(&ui, &window, Key::Tab);
        }
        key(&ui, &window, " ");
        let got = fired.borrow().clone();
        assert!(
            got.iter().all(|g| g.starts_with("port ") || g == "rescan"),
            "Tab x{n} in the picker reached a control under it: {got:?}"
        );
        radio.set_picker_open(false);
        draw(&window);
    }
}
