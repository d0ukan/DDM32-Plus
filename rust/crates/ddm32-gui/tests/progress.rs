// SPDX-License-Identifier: GPL-3.0-or-later
//! The progress area during a backup and an install, fed with the recorded tool lines of the C# program's fixtures
//! (no radio): the bar fills over the whole run and the percent line stays a percent line; JEDEC IDs, "boot ROM
//! answered" and the like go to the information line beside it.
use ddm32_core::radio::progress::ProgressView;
use ddm32_gui::{progress_show, App, AppWindow, ProgressShow};
use slint::ComponentHandle;
use std::io::Read;

fn lines(name: &str) -> Vec<String> {
    let f = std::fs::File::open(
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join(format!("../../tests/fixtures/radio/{name}.json.gz")),
    )
    .unwrap();
    let mut s = String::new();
    flate2::read::GzDecoder::new(f).read_to_string(&mut s).unwrap();
    let v: serde_json::Value = serde_json::from_str(&s).unwrap();
    v["lines"].as_array().unwrap().iter().map(|l| l.as_str().unwrap().to_string()).collect()
}

/// (bar values, percent lines, information lines) of one run, as the page would show them line by line.
fn run(name: &str, lang: &str) -> (Vec<f32>, Vec<String>, Vec<String>) {
    let mut view = ProgressView::new();
    let (mut bar, mut text, mut info) = (vec![], vec![], vec![]);
    for l in lines(name) {
        let s = progress_show(&mut view, lang, &l);
        bar.extend(s.value);
        text.extend(s.text);
        info.extend(s.info);
    }
    (bar, text, info)
}

#[test]
fn backup_fills_the_bar_and_keeps_a_percent_line() {
    let (bar, text, info) = run("backup_ok", "tr");
    assert!(bar.len() > 20);
    assert!(bar.windows(2).all(|w| w[0] <= w[1]), "the bar never goes back");
    assert!(bar[0] < 0.05 && (bar.last().unwrap() - 1.0).abs() < 1e-6, "{bar:?}");
    // the first text is the power prompt, every later one a percent of the backup
    assert_eq!("Telsizi şimdi AÇIN.", text[0]);
    assert!(text[1..].iter().all(|t| t.starts_with("Yedek alınıyor") && t.contains('%')), "{text:?}");
    assert_eq!("Yedek alınıyor (iki kez okunur): %100", text.last().unwrap());
    assert!(info.iter().any(|i| i.contains("JEDEC")));
    assert!(info.iter().all(|i| !i.starts_with("progmem pass") && !i.starts_with("q128 pass")));
    let (_, en, _) = run("backup_ok", "en");
    assert_eq!("Taking the backup (read twice): 100%", en.last().unwrap());
}

#[test]
fn install_fills_one_bar_over_its_phases() {
    let (bar, text, info) = run("install_ok", "en");
    assert!(bar.windows(2).all(|w| w[0] <= w[1]), "the bar never goes back: {bar:?}");
    assert!((bar.last().unwrap() - 1.0).abs() < 1e-6);
    assert!(text.iter().any(|t| t == "Comparing the radio with the backup: 100%"));
    assert!(text.iter().any(|t| t == "Writing, each sector read back: 100%"));
    assert_eq!("Reading back and verifying: 100%", text.last().unwrap());
    assert!(info.iter().any(|i| i.starts_with("INSTALLING")));
}

#[test]
fn a_jedec_line_never_replaces_the_percent_on_the_window() {
    i_slint_backend_testing::init_no_event_loop();
    let ui = AppWindow::new().unwrap();
    let app = ui.global::<App>();
    let mut view = ProgressView::new();
    for l in [
        "READY: COM14 open; switch radio OFF then ON normally, cable seated, no keys held. Waiting 600s.",
        "progmem pass 1: 100% (1048576/1048576)",
        "q128 pass 1: 100% (16777216/16777216)",
        "progmem pass 2: 25% (262144/1048576)",
        "data flash JEDEC ID ef4018",
        "{",
    ] {
        progress_show(&mut view, "tr", l).apply(&app);
    }
    assert!((app.get_progress() - 0.5).abs() < 1e-6, "{}", app.get_progress());
    assert_eq!("Yedek alınıyor (iki kez okunur): %50", app.get_progress_text().as_str());
    assert_eq!("data flash JEDEC ID ef4018", app.get_progress_info().as_str());
    assert_eq!(ProgressShow::default(), progress_show(&mut view, "tr", "}"));
}
