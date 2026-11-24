// SPDX-License-Identifier: GPL-3.0-or-later
//! DDM32 Plus: the window. `--self-update ...` and `--update-check FILE` are the self-update protocol (the same as the
//! C# program's, so either program can hand over to the other); `--demo` shows example contents (no radio, no network);
//! `--show PAGE[:demo|:FOLDER]` opens a page (screenshots; the C# --show); `--release-folder PATH` loads a signed
//! release from a local folder (developer builds only).
#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]

use ddm32_core::update::self_update as su;
use ddm32_gui::AppWindow;
use slint::ComponentHandle;

fn arg_after<'a>(args: &'a [String], name: &str) -> Option<&'a str> {
    let i = args.iter().position(|a| a == name)?;
    args.get(i + 1).map(String::as_str).filter(|v| !v.is_empty())
}

fn main() -> Result<(), slint::PlatformError> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.first().map(String::as_str) == Some(su::HELPER_ARG) {
        let code = su::helper_main(&args[1..], &mut |pid| Box::new(su::RealProcesses::new(pid)));
        std::process::exit(code);
    }
    let ui = AppWindow::new()?;
    let paths =
        ddm32_core::storage::AppPaths::for_this_program().unwrap_or_else(|| ddm32_core::storage::AppPaths::new("."));
    let demo = args.iter().any(|a| a == "--demo");
    ddm32_gui::DEMO.store(demo, std::sync::atomic::Ordering::SeqCst);
    ddm32_gui::setup(&ui, paths, !demo);
    if demo {
        ddm32_gui::demo(&ui);
    } else {
        // the size and place the window had (settings.json `window`, shared with the C# program)
        ddm32_gui::window::restore(&ui);
    }
    // --show health:demo, live:demo, channels:FOLDER, versions, ... : navigation and file reads only
    if let Some(spec) = arg_after(&args, "--show") {
        ddm32_gui::show_from_args(&ui, spec);
    }
    // --release-folder PATH: a developer build's local release (refused in a release build)
    if let Some(folder) = arg_after(&args, "--release-folder") {
        if ddm32_core::app_info::DEVELOPER {
            ddm32_gui::load_local_release(&ui, std::path::Path::new(folder));
        }
    }
    // started by the update helper (`--update-check FILE`, this program's or the C# program's): the start marker is
    // written 1.5 s after the event loop runs, so a window that cannot come up makes the helper roll back
    ddm32_gui::arm_start_marker(&args);
    ui.run()
}
