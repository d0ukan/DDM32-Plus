// SPDX-License-Identifier: GPL-3.0-or-later
//! The menu bar: its action strings map to what the window has, and its radio entries lock while any radio job,
//! the live panel or the search log holds the port (the same rule as the buttons). No radio, no network.
use ddm32_gui::{parse_menu_action, App, AppWindow, MenuAction, Radio};
use slint::ComponentHandle;

#[test]
fn actions_parse() {
    assert_eq!(parse_menu_action("page:7"), Some(MenuAction::Page(7)));
    assert_eq!(parse_menu_action("page:10"), Some(MenuAction::Page(10)));
    assert_eq!(parse_menu_action("exit"), Some(MenuAction::Exit));
    assert_eq!(parse_menu_action("github"), Some(MenuAction::GitHub));
    assert_eq!(parse_menu_action("backups-folder"), Some(MenuAction::BackupsFolder));
    assert_eq!(parse_menu_action("logs-folder"), Some(MenuAction::LogsFolder));
    assert_eq!(parse_menu_action("guide"), Some(MenuAction::Guide));
    // the first-start page, unknown pages and junk are not reachable
    for bad in ["page:6", "page:13", "page:-1", "page:x", "", "format"] {
        assert_eq!(parse_menu_action(bad), None, "{bad}");
    }
}

#[test]
fn radio_entries_lock_while_a_job_holds_the_port() {
    i_slint_backend_testing::init_no_event_loop();
    let ui = AppWindow::new().unwrap();
    let app = ui.global::<App>();
    let radio = ui.global::<Radio>();
    assert!(!app.get_menu_radio_locked());
    app.set_busy(true);
    assert!(app.get_menu_radio_locked());
    app.set_busy(false);
    radio.set_busy(true);
    assert!(app.get_menu_radio_locked());
    radio.set_busy(false);
    radio.set_live_running(true);
    assert!(app.get_menu_radio_locked());
    radio.set_live_running(false);
    radio.set_slog_running(true);
    assert!(app.get_menu_radio_locked());
    radio.set_slog_running(false);
    assert!(!app.get_menu_radio_locked());
}

#[test]
fn no_page_is_left_while_the_port_is_held() {
    i_slint_backend_testing::init_no_event_loop();
    let root = std::env::temp_dir().join(format!("ddm32-menu-test-{}", std::process::id()));
    let ui = AppWindow::new().unwrap();
    ddm32_gui::setup_with_env(&ui, ddm32_core::storage::AppPaths::new(&root), false, ddm32_gui::no_env);
    let app = ui.global::<App>();
    let radio = ui.global::<Radio>();
    app.set_page(8); // the live panel
    for lock in 0..4 {
        match lock {
            0 => app.set_busy(true),
            1 => radio.set_busy(true),
            2 => radio.set_live_running(true),
            _ => radio.set_slog_running(true),
        }
        for entry in ["page:0", "page:4", "page:5", "page:9", "page:7"] {
            app.invoke_menu_action(entry.into());
            assert_eq!(app.get_page(), 8, "lock {lock}: {entry} left the page");
        }
        app.invoke_menu_action("exit".into()); // refused with a warning, does not quit
        assert_eq!(radio.get_message_kind(), "warning");
        app.set_busy(false);
        radio.set_busy(false);
        radio.set_live_running(false);
        radio.set_slog_running(false);
    }
    app.invoke_menu_action("page:5".into());
    assert_eq!(app.get_page(), 5);
    let _ = std::fs::remove_dir_all(&root);
}
