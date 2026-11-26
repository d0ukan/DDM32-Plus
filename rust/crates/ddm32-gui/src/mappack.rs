// SPDX-License-Identifier: GPL-3.0-or-later
//! The update wizard's map pack step (docs/mappack-design.md E.3, H.3): the release's signed map pack is
//! downloaded (size and SHA-256 from the manifest), then written through the running firmware's CPS protocol into
//! the slot the firmware does not use. Order: on a radio that runs DDM32 Plus the pack goes first (the firmware 1.0.x
//! ignores it, so a failed firmware install leaves a working radio); from the factory firmware the firmware goes
//! first (no CPS before it). A pack the user built is never replaced without their choice (rebuild / keep / use the
//! release's). Every radio step starts only from the user's click, behind the disclaimer, one job at a time.

use crate::updates::Installed;
use crate::{i18n, port_busy, radio_allowed, reader_holds_port, tr, trf, with, with_port, worker, App, AppWindow};
use ddm32_core::datapacks::radioid;
use ddm32_core::releases::catalog::FirmwareVersion;
use ddm32_core::releases::client;
use ddm32_core::releases::net::HttpReleaseNet;
use serde_json::Value;
use slint::ComponentHandle;
use std::path::PathBuf;

/// Where the map pack step stands in the wizard.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MapOrder {
    /// The radio runs DDM32 Plus: the pack before the firmware (step 1).
    PackFirst,
    /// Factory firmware, Classic or not known: the firmware first, the pack after it (step 5).
    FirmwareFirst,
}

/// H.3: pack first only over an installed DDM32 Plus (it has the CPS path the pack is written through).
pub fn order(installed: &Installed) -> MapOrder {
    match installed {
        Installed::Plus { .. } => MapOrder::PackFirst,
        _ => MapOrder::FirmwareFirst,
    }
}

/// The step's state (UI thread).
#[derive(Debug, Default, Clone)]
pub struct MapState {
    /// The version whose pack was written or found on the radio in this session.
    pub done_for: Option<String>,
    /// The user-built pack question of the last run: (keep offered, rebuild offered)
    pub ask: Option<(bool, bool)>,
    /// The last result line ("PASS", "SAME", "KEPT", "ASK")
    pub result: Option<String>,
    /// Notes of the last run (old firmware, a too big RadioID pack)
    pub notes: Vec<String>,
    pub sectors_done: usize,
    pub sectors_all: usize,
}

/// What the tool's lines of one run mean for the step.
pub fn feed(st: &mut MapState, lang: &str, line: &str) {
    if line.starts_with("pack: sector ") {
        st.sectors_done += 1;
        return;
    }
    let Ok(v) = serde_json::from_str::<Value>(line) else { return };
    if let Some(p) = v.get("plan") {
        st.sectors_all = p["sectors"].as_array().map_or(0, Vec::len);
    } else if let Some(a) = v.get("ask") {
        st.ask = Some((a["keep"].as_bool().unwrap_or(false), a["rebuild"].as_bool().unwrap_or(false)));
    } else if let Some(r) = v.get("result").and_then(Value::as_str) {
        st.result = Some(r.to_string());
    } else if let Some(t) = crate::packs::note_text(lang, line) {
        st.notes.push(t);
    }
}

/// The step's text after a run that ended without an error.
pub fn result_text(lang: &str, st: &MapState) -> (String, &'static str) {
    let mut t = match st.result.as_deref() {
        Some("SAME") => i18n::t(lang, "map_same"),
        Some("KEPT") => i18n::t(lang, "map_kept"),
        Some("ASK") => i18n::t(lang, "map_ask_wait"),
        _ => i18n::t(lang, "map_done"),
    };
    for n in &st.notes {
        t.push('\n');
        t.push_str(n);
    }
    (t, if st.result.as_deref() == Some("ASK") { "warning" } else { "success" })
}

/// The selected release in the wizard.
fn selected(ui: &AppWindow) -> Option<FirmwareVersion> {
    let i = ui.global::<App>().get_selected_version() as usize;
    with(|s| s.shown.get(i).cloned())
}

/// The card's flags for the current step (called when the step or the selection changes).
pub fn show(ui: &AppWindow) {
    if !crate::has_state() {
        return;
    }
    let app = ui.global::<App>();
    let fv = selected(ui);
    let has = fv.as_ref().is_some_and(|f| f.entry.ref_pack.is_some());
    let (first, done) = with(|s| {
        (
            order(&s.known.installed) == MapOrder::PackFirst,
            fv.as_ref().is_some_and(|f| s.map.done_for.as_deref() == Some(f.version())),
        )
    });
    app.set_map_available(has);
    app.set_map_first(first);
    app.set_map_done(done);
}

/// "Write the map pack": the gates, then the port.
pub fn write(ui: &AppWindow) {
    if !radio_allowed(ui) || reader_holds_port(ui) || port_busy(ui) {
        return;
    }
    with(|s| s.map.ask = None);
    with_port(ui, write_on);
}

/// One of the user-built pack question's answers ("rebuild", "keep", "release").
pub fn choose(ui: &AppWindow, choice: &str) {
    ui.global::<App>().set_map_ask_open(false);
    if !radio_allowed(ui) || reader_holds_port(ui) || port_busy(ui) {
        return;
    }
    with(|s| s.map.ask = None);
    let choice = choice.to_string();
    match choice.as_str() {
        "rebuild" => with_port(ui, |ui, p| run_with(ui, p, Some("rebuild"))),
        "keep" => with_port(ui, |ui, p| run_with(ui, p, Some("keep"))),
        "release" => with_port(ui, |ui, p| run_with(ui, p, Some("release"))),
        _ => {}
    }
}

fn write_on(ui: &AppWindow, found: Option<String>) {
    run_with(ui, found, None);
}

fn set_text(ui: &AppWindow, text: &str, kind: &str) {
    let app = ui.global::<App>();
    app.set_map_text(text.into());
    app.set_map_kind(kind.into());
}

/// The download (a worker thread), then the data pack tool's update-ref.
fn run_with(ui: &AppWindow, found: Option<String>, choice: Option<&'static str>) {
    if !radio_allowed(ui) || reader_holds_port(ui) || port_busy(ui) {
        return;
    }
    let Some(port) = found else {
        set_text(ui, &tr("radio_nocable"), "warning");
        return;
    };
    let Some(fv) = selected(ui).filter(|f| f.entry.ref_pack.is_some()) else { return };
    let releases = with(|s| s.paths.releases());
    set_text(ui, &tr("map_downloading"), "info");
    ui.global::<App>().set_busy(true);
    let w = ui.as_weak();
    let fv2 = fv.clone();
    worker::spawn(
        move || -> Result<PathBuf, String> {
            let r = match &fv2.source {
                Some(local) => client::download_ref_pack(&fv2, &releases, local.as_ref(), None),
                None => client::download_ref_pack(&fv2, &releases, &HttpReleaseNet::real(), None),
            };
            r.map_err(|e| e.to_string())
        },
        move |r| {
            let Some(ui) = w.upgrade() else { return };
            ui.global::<App>().set_busy(false);
            match r.and_then(|x| x) {
                Ok(file) => run_tool(&ui, &port, &fv, file, choice),
                Err(e) => set_text(&ui, &trf("map_download_failed", &e), "danger"),
            }
        },
    );
}

/// The argv of one update-ref run.
pub fn argv(
    port: &str,
    file: &std::path::Path,
    coord: u32,
    known: &[String],
    line: &str,
    choice: Option<&str>,
) -> Vec<String> {
    let mut a: Vec<String> = vec![
        "update-ref".into(),
        "--port".into(),
        port.into(),
        "--pack".into(),
        file.to_string_lossy().into_owned(),
        "--coord".into(),
        format!("0x{coord:08X}"),
        "--line".into(),
        line.into(),
    ];
    a.extend(known.iter().cloned());
    if let Some(c) = choice {
        a.push("--choice".into());
        a.push(c.into());
    }
    a
}

fn run_tool(ui: &AppWindow, port: &str, fv: &FirmwareVersion, file: PathBuf, choice: Option<&str>) {
    if !radio_allowed(ui) || reader_holds_port(ui) || port_busy(ui) {
        return;
    }
    let coord = fv.entry.ref_pack.as_ref().map_or(0, |r| r.coord_crc);
    let line = radioid::fold(&tr("map_screen_line"));
    let a = argv(port, &file, coord, &crate::packs::known_coords_args(), &line, choice);
    with(|s| s.map = MapState { done_for: s.map.done_for.clone(), ..Default::default() });
    set_text(ui, &tr("map_writing"), "info");
    let version = fv.version().to_string();
    crate::run_tool_lines(ui, "datapack", a, true, tool_line, move |ui, err| {
        let lang = with(|s| s.lang.clone());
        match err {
            None => {
                let (ask, st) = with(|s| (s.map.ask, s.map.clone()));
                if let Some((keep, rebuild)) = ask {
                    let app = ui.global::<App>();
                    app.set_map_ask_keep(keep);
                    app.set_map_ask_rebuild(rebuild);
                    app.set_map_ask_text(
                        i18n::t(&lang, if keep { "map_ask_text" } else { "map_ask_text_other" }).into(),
                    );
                    app.set_map_ask_open(true);
                } else {
                    with(|s| s.map.done_for = Some(version.clone()));
                }
                let (t, k) = result_text(&lang, &st);
                set_text(ui, &t, k);
            }
            Some(e) => set_text(ui, &i18n::tf(&lang, "map_failed", &e.record()), "danger"),
        }
        show(ui);
    });
}

fn tool_line(ui: &AppWindow, line: &str) {
    let v = with(|s| {
        let lang = s.lang.clone();
        feed(&mut s.map, &lang, line);
        if s.map.sectors_all == 0 {
            0.0
        } else {
            s.map.sectors_done as f32 / s.map.sectors_all as f32
        }
    });
    if v > 0.0 {
        ui.global::<App>().set_progress(v.min(1.0));
    }
}
