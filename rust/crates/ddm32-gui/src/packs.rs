// SPDX-License-Identifier: GPL-3.0-or-later
//! The data packs page (the C# DataPacksPage, docs/plus.md "User data packs"): what the radio holds (a live,
//! read-only read over the CPS protocol, or a verified backup's data chip image), a RadioID pack from the radioid.net
//! list the user downloads (the download only on a click, after a Yes), with the countries and the pack's name; an
//! air band pack from the AIP pages the user adds; a saved .dpk; and the write of one pack (the slot not in force,
//! payload first, header last). Every radio step starts only from its own button and the write asks first (Yes / No,
//! No by default); both are behind the disclaimer and refused while another job holds the port.

use crate::{
    i18n, port_busy, radio_allowed, reader_holds_port, tr, trf, with, with_port, worker, App, AppWindow, Packs,
};
use ddm32_core::datapacks::datapack::{self as dp, AIR, RADIO_ID};
use ddm32_core::datapacks::files::{self as files, AirSource, BuiltPack};
use ddm32_core::datapacks::radioid;
use ddm32_core::radio::cps;
use ddm32_core::storage::paths::is_verified_backup;
use serde_json::Value;
use slint::ComponentHandle;
use std::path::{Path, PathBuf};

pub const AIP_PAGE: &str = "https://aip.dhmi.gov.tr/";
pub const COUNTRIES_KEY: &str = "pack_countries";
pub const RADIOID_NAME_KEY: &str = "pack_radioid_name";
pub const AIR_NAME_KEY: &str = "pack_air_name";
/// The longest pack name the page takes (the C# box's MaxLength).
pub const NAME_MAX: usize = 16;

/// The page's state (UI thread).
#[derive(Debug, Default)]
pub struct PackState {
    pub csv: Option<PathBuf>,
    pub air: Vec<AirSource>,
    pub radioid: Option<(BuiltPack, PathBuf)>,
    pub air_pack: Option<(BuiltPack, PathBuf)>,
    pub radio_text: String,
    pub radioid_error: Option<String>,
    pub air_error: Option<String>,
    /// The pack the Yes of the write question writes (type)
    pub pending: Option<u8>,
    pub sectors_done: usize,
    pub sectors_all: usize,
    pub download_bytes: Option<u64>,
}

/// "12,3 KB" (Turkish) / "12.3 KB"; one decimal at most (the C# "0.#").
pub fn kb(lang: &str, n: u64) -> String {
    let v = (n as f64 / 1024.0 * 10.0).round() / 10.0;
    let mut s = if v.fract() == 0.0 { format!("{v:.0}") } else { format!("{v:.1}") };
    if lang != "en" {
        s = s.replace('.', ",");
    }
    format!("{s} KB")
}

/// "20261124" -> "2026-11-24".
pub fn date(yyyymmdd: &str) -> String {
    if yyyymmdd.len() == 8 && yyyymmdd.is_ascii() {
        format!("{}-{}-{}", &yyyymmdd[..4], &yyyymmdd[4..6], &yyyymmdd[6..])
    } else {
        yyyymmdd.to_string()
    }
}

/// A pack in words: 'RadioID pack "RadioID TR", 2026-11-24: 3120 IDs, 81,6 KB'.
pub fn describe(lang: &str, p: &BuiltPack) -> String {
    let key = if p.ptype == RADIO_ID { "pack_desc_radioid" } else { "pack_desc_air" };
    i18n::tn(lang, key, &[&p.header.name, &date(&p.header.date), &p.count.to_string(), &kb(lang, p.image.len() as u64)])
}

/// One slot as the radio tool's JSON (or a backup's slot state) says it.
pub fn slot_line(lang: &str, s: &Value) -> String {
    let t = |k: &str| i18n::t(lang, k);
    let kind =
        t(if s.get("type").and_then(Value::as_str) == Some("radioid") { "pack_type_radioid" } else { "pack_type_air" });
    let slot = s.get("slot").and_then(Value::as_i64).unwrap_or(0).to_string();
    let addr = s.get("address").and_then(Value::as_str).unwrap_or("").to_string();
    let name = s.get("name").and_then(Value::as_str).unwrap_or("").to_string();
    if !s.get("valid").and_then(Value::as_bool).unwrap_or(false) {
        return i18n::tn(lang, "pack_slot_empty", &[&kind, &slot, &addr]);
    }
    if !s.get("crc_ok").and_then(Value::as_bool).unwrap_or(false) {
        return i18n::tn(lang, "pack_slot_damaged", &[&kind, &slot, &addr, &name]);
    }
    let key = if s.get("in_force").and_then(Value::as_bool).unwrap_or(false) {
        "pack_slot_in_force"
    } else {
        "pack_slot_old"
    };
    let count = s.get("count").and_then(Value::as_i64).unwrap_or(0).to_string();
    let bytes = s.get("bytes").and_then(Value::as_u64).unwrap_or(0) + dp::HEADER_SIZE as u64;
    let seq = s.get("seq").and_then(Value::as_i64).unwrap_or(0).to_string();
    let d = date(s.get("date").and_then(Value::as_str).unwrap_or(""));
    i18n::tn(lang, key, &[&kind, &slot, &addr, &name, &d, &count, &kb(lang, bytes), &seq])
}

/// A backup's slot as the same line (files only).
pub fn slot_state_line(lang: &str, s: &cps::SlotState) -> String {
    let json = ddm32_core::pyjson::dumps(&ddm32_core::pyjson::Py::Dict(cps::slot_json(s)), None);
    slot_line(lang, &serde_json::from_str(&json).unwrap_or(Value::Null))
}

/// Both pack types' slots in a verified backup's data chip image, as lines (or the reason it cannot be read).
pub fn backup_lines(lang: &str, folder: &Path) -> Result<Vec<String>, String> {
    if !is_verified_backup(folder) {
        return Err(i18n::t(lang, "pack_not_verified"));
    }
    let mut lines =
        vec![i18n::tf(lang, "pack_from_backup_of", &folder.file_name().unwrap_or_default().to_string_lossy())];
    for ptype in [RADIO_ID, AIR] {
        let slots = files::slots_from_backup(folder, ptype).map_err(|e| e.0)?;
        lines.extend(cps::describe(ptype, &slots).iter().map(|s| slot_state_line(lang, s)));
    }
    Ok(lines)
}

/// The countries box: "Turkey, Cyprus" -> ["Turkey", "Cyprus"] (empty parts dropped).
pub fn countries(text: &str) -> Vec<String> {
    text.split(',').map(str::trim).filter(|c| !c.is_empty()).map(String::from).collect()
}

/// A pack's name: trimmed, at most 16 characters (the slot header's field), or the fallback.
pub fn pack_name(text: &str, fallback: &str) -> String {
    let t: String = text.trim().chars().take(NAME_MAX).collect();
    if t.trim().is_empty() {
        fallback.to_string()
    } else {
        t.trim().to_string()
    }
}

/// The write question's text (the C# Pack_Confirm): the pack, the port, both slots of its type, the sectors.
pub fn confirm_text(lang: &str, p: &BuiltPack, port: &str) -> String {
    let a = |slot| dp::slot_address(p.ptype, slot).map(|x| format!("0x{x:X}")).unwrap_or_default();
    let slots = format!("{} / {}", a(0), a(1));
    let sectors = p.image.len().div_ceil(dp::SECTOR).to_string();
    i18n::tn(lang, "pack_confirm", &[&describe(lang, p), port, &slots, &sectors])
}

pub fn wire(ui: &AppWindow) {
    let p = ui.global::<Packs>();
    let w = ui.as_weak();
    p.on_read_radio(move || read_radio(&w.unwrap()));
    let w = ui.as_weak();
    p.on_from_backup(move || {
        let ui = w.unwrap();
        let start = with(|s| s.paths.backups());
        if let Some(f) = rfd::FileDialog::new().set_directory(&start).pick_folder() {
            from_backup(&ui, &f);
        }
    });
    p.on_open_radioid(|| crate::open_folder(Path::new(radioid::DOWNLOAD_PAGE)));
    p.on_open_aip(|| crate::open_folder(Path::new(AIP_PAGE)));
    let w = ui.as_weak();
    p.on_download(move || {
        let ui = w.unwrap();
        let p = ui.global::<Packs>();
        p.set_download_confirm_text(trf("pack_download_confirm", radioid::DOWNLOAD_URL).into());
        p.set_download_confirm_open(true);
    });
    let w = ui.as_weak();
    p.on_download_yes(move || download(&w.unwrap()));
    let w = ui.as_weak();
    p.on_choose_csv(move || {
        let ui = w.unwrap();
        if let Some(f) = rfd::FileDialog::new().add_filter("user.csv", &["csv"]).pick_file() {
            set_csv(&ui, &f);
        }
    });
    let w = ui.as_weak();
    p.on_build_radioid(move || build_radioid(&w.unwrap()));
    let w = ui.as_weak();
    p.on_add_pdf(move || {
        let ui = w.unwrap();
        if let Some(fs) = rfd::FileDialog::new().add_filter("AIP AD 2 (PDF)", &["pdf", "txt"]).pick_files() {
            add_sources(&ui, fs);
        }
    });
    let w = ui.as_weak();
    p.on_clear_pdf(move || {
        let ui = w.unwrap();
        with(|s| {
            s.packs.air.clear();
            s.packs.air_error = None;
        });
        show(&ui);
    });
    let w = ui.as_weak();
    p.on_build_air(move || build_air(&w.unwrap()));
    let w = ui.as_weak();
    p.on_load_dpk(move || {
        let ui = w.unwrap();
        let dir = with(|s| s.paths.packs());
        if let Some(f) =
            rfd::FileDialog::new().set_directory(&dir).add_filter("DDM32 Plus (*.dpk)", &["dpk"]).pick_file()
        {
            load_dpk(&ui, &f);
        }
    });
    let w = ui.as_weak();
    p.on_write_radioid(move || ask_write(&w.unwrap(), RADIO_ID));
    let w = ui.as_weak();
    p.on_write_air(move || ask_write(&w.unwrap(), AIR));
    let w = ui.as_weak();
    p.on_write_yes(move || write_yes(&w.unwrap()));
}

/// The page's first contents: the countries and names the C# program saved (settings.json `pack_*`), and a
/// user.csv downloaded before into the packs folder.
pub fn init(ui: &AppWindow) {
    let (countries, rid, air, csv) = with(|s| {
        (
            s.settings.get_string(COUNTRIES_KEY).unwrap_or_else(|| "Turkey".into()),
            s.settings.get_string(RADIOID_NAME_KEY).unwrap_or_else(|| "RadioID TR".into()),
            s.settings.get_string(AIR_NAME_KEY).unwrap_or_else(|| "TR aerodromes".into()),
            Some(s.paths.packs().join("user.csv")).filter(|p| p.is_file()),
        )
    });
    let p = ui.global::<Packs>();
    p.set_countries(countries.into());
    p.set_radioid_name(rid.into());
    p.set_air_name(air.into());
    with(|s| s.packs.csv = csv);
    show(ui);
}

fn result(ui: &AppWindow, text: &str, kind: &str) {
    let p = ui.global::<Packs>();
    p.set_result(text.into());
    p.set_result_kind(kind.into());
}

/// Every text of the page from the state.
pub fn show(ui: &AppWindow) {
    let p = ui.global::<Packs>();
    let lang = with(|s| s.lang.clone());
    with(|s| {
        let st = &s.packs;
        let csv_text = match (&st.csv, st.download_bytes) {
            (_, Some(n)) => i18n::tf(&lang, "pack_downloading", &kb(&lang, n)),
            (Some(c), None) => {
                let len = std::fs::metadata(c).map(|m| m.len()).unwrap_or(0);
                i18n::tn(&lang, "pack_csv", &[&c.display().to_string(), &kb(&lang, len)])
            }
            (None, None) => i18n::t(&lang, "pack_no_csv"),
        };
        p.set_csv_text(csv_text.into());
        p.set_csv_ready(st.csv.is_some());
        let air_list = if st.air.is_empty() {
            i18n::t(&lang, "pack_no_pdf")
        } else {
            st.air
                .iter()
                .map(|a| {
                    let freqs: Vec<String> = a
                        .freqs
                        .iter()
                        .take(6)
                        .map(|(t, khz)| {
                            format!(
                                "{} {}",
                                ddm32_core::datapacks::air::type_name(*t),
                                ddm32_core::datapacks::air::mhz(*khz)
                            )
                        })
                        .collect();
                    let more = if a.freqs.len() > 6 { " ..." } else { "" };
                    i18n::tn(
                        &lang,
                        "pack_air_source",
                        &[
                            a.code.as_deref().unwrap_or("????"),
                            &a.freqs.len().to_string(),
                            &a.path.file_name().unwrap_or_default().to_string_lossy(),
                            &format!("{}{more}", freqs.join(" ")),
                        ],
                    )
                })
                .collect::<Vec<_>>()
                .join("\n")
        };
        let air_list = match &st.air_error {
            Some(e) => format!("{air_list}\n{e}"),
            None => air_list,
        };
        p.set_air_list(air_list.into());
        p.set_air_ready(!st.air.is_empty());
        let rid = match (&st.radioid_error, &st.radioid) {
            (Some(e), _) => e.clone(),
            (None, Some((pk, file))) => {
                let mut t = describe(&lang, pk);
                if let Some(sk) = &pk.skipped {
                    t.push('\n');
                    t.push_str(&i18n::tn(
                        &lang,
                        "pack_skipped",
                        &[&sk.callsign.to_string(), &sk.id.to_string(), &sk.duplicate.to_string()],
                    ));
                }
                format!("{t}\n{}", file.display())
            }
            _ => String::new(),
        };
        p.set_radioid_result(rid.into());
        let air = match (&st.air_error, &st.air_pack) {
            (None, Some((pk, file))) => format!("{}\n{}", describe(&lang, pk), file.display()),
            _ => String::new(),
        };
        p.set_air_result(air.into());
        let chosen: Vec<String> =
            [&st.radioid, &st.air_pack].into_iter().flatten().map(|(pk, _)| describe(&lang, pk)).collect();
        p.set_chosen_text(
            if chosen.is_empty() { i18n::t(&lang, "pack_nothing_built") } else { chosen.join("\n") }.into(),
        );
        p.set_radioid_pack(st.radioid.is_some());
        p.set_air_pack(st.air_pack.is_some());
        p.set_downloading(st.download_bytes.is_some());
        p.set_radio_text(
            if st.radio_text.is_empty() { i18n::t(&lang, "pack_not_read") } else { st.radio_text.clone() }.into(),
        );
    });
}

/// "Show from a backup...": what a verified backup's data chip holds (files only).
pub fn from_backup(ui: &AppWindow, folder: &Path) {
    let lang = with(|s| s.lang.clone());
    let text = match backup_lines(&lang, folder) {
        Ok(lines) => lines.join("\n"),
        Err(e) => i18n::tf(&lang, "pack_failed", &e),
    };
    with(|s| s.packs.radio_text = text);
    show(ui);
}

fn set_csv(ui: &AppWindow, f: &Path) {
    with(|s| {
        s.packs.csv = Some(f.to_path_buf());
        s.packs.radioid_error = None;
    });
    show(ui);
}

/// "Download user.csv" after its Yes: radioid.net only, into the packs folder, on a worker thread.
fn download(ui: &AppWindow) {
    let p = ui.global::<Packs>();
    p.set_download_confirm_open(false);
    if with(|s| s.packs.download_bytes.is_some()) {
        return;
    }
    let dest = with(|s| {
        s.packs.download_bytes = Some(0);
        s.paths.packs().join("user.csv")
    });
    let _ = std::fs::create_dir_all(dest.parent().unwrap_or(Path::new(".")));
    show(ui);
    let w = ui.as_weak();
    let w2 = w.clone();
    let d2 = dest.clone();
    worker::spawn(
        move || {
            let mut last = 0u64;
            let mut progress = |n: u64, _all: u64| {
                if n - last >= 256 * 1024 {
                    last = n;
                    let w3 = w2.clone();
                    let _ = slint::invoke_from_event_loop(move || {
                        if let Some(ui) = w3.upgrade() {
                            with(|s| {
                                if s.packs.download_bytes.is_some() {
                                    s.packs.download_bytes = Some(n)
                                }
                            });
                            show(&ui);
                        }
                    });
                }
            };
            files::RadioIdDownloader::real().download(&d2, Some(&mut progress)).map_err(|e| e.0)
        },
        move |r| {
            let r = r.and_then(|x| x);
            let Some(ui) = w.upgrade() else { return };
            with(|s| s.packs.download_bytes = None);
            match r {
                Ok(()) => {
                    set_csv(&ui, &dest);
                    result(&ui, "", "info");
                }
                Err(e) => {
                    show(&ui);
                    result(&ui, &trf("pack_failed", &e), "danger");
                }
            }
        },
    );
}

fn packs_dir() -> PathBuf {
    let d = with(|s| s.paths.packs());
    let _ = std::fs::create_dir_all(&d);
    d
}

/// "Build the pack" (RadioID): the countries and the name saved (the C# keys), the pack built on a worker thread and
/// saved as packs/radioid-DATE.dpk.
pub fn build_radioid(ui: &AppWindow) {
    let p = ui.global::<Packs>();
    let Some(csv) = with(|s| s.packs.csv.clone()) else { return };
    let list = countries(&p.get_countries());
    let name = pack_name(&p.get_radioid_name(), "RadioID");
    with(|s| {
        let _ = s.settings.save(COUNTRIES_KEY, Value::String(p.get_countries().trim().to_string()));
        let _ = s.settings.save(RADIOID_NAME_KEY, Value::String(name.clone()));
    });
    p.set_working(true);
    result(ui, &tr("pack_busy_read"), "info");
    let dir = packs_dir();
    let w = ui.as_weak();
    worker::spawn(
        move || -> Result<(BuiltPack, PathBuf), String> {
            let pk = files::radio_id(&csv, &list, &name, None).map_err(|e| e.0)?;
            let file = dir.join(format!("radioid-{}.dpk", pk.header.date));
            std::fs::write(&file, &pk.image).map_err(|e| e.to_string())?;
            Ok((pk, file))
        },
        move |r| {
            let Some(ui) = w.upgrade() else { return };
            ui.global::<Packs>().set_working(false);
            result(&ui, "", "info");
            built(&ui, RADIO_ID, r.and_then(|x| x));
        },
    );
}

/// A built (or loaded) pack, or its error, into the page.
pub fn built(ui: &AppWindow, ptype: u8, r: Result<(BuiltPack, PathBuf), String>) {
    let lang = with(|s| s.lang.clone());
    with(|s| {
        let st = &mut s.packs;
        match (ptype, r) {
            (RADIO_ID, Ok(x)) => {
                st.radioid = Some(x);
                st.radioid_error = None;
            }
            (RADIO_ID, Err(e)) => {
                st.radioid = None;
                st.radioid_error = Some(i18n::tf(&lang, "pack_failed", &e));
            }
            (_, Ok(x)) => {
                st.air_pack = Some(x);
                st.air_error = None;
            }
            (_, Err(e)) => {
                st.air_pack = None;
                st.air_error = Some(i18n::tf(&lang, "pack_failed", &e));
            }
        }
    });
    show(ui);
}

/// "Add AD 2 PDFs...": each page read on a worker thread (the same page again replaces itself).
pub fn add_sources(ui: &AppWindow, paths: Vec<PathBuf>) {
    ui.global::<Packs>().set_working(true);
    result(ui, &tr("pack_busy_read"), "info");
    let w = ui.as_weak();
    worker::spawn(
        move || paths.iter().map(|f| files::read_air_source(f).map_err(|e| e.0)).collect::<Vec<_>>(),
        move |r| {
            let Some(ui) = w.upgrade() else { return };
            ui.global::<Packs>().set_working(false);
            result(&ui, "", "info");
            let lang = with(|s| s.lang.clone());
            let r = r.unwrap_or_else(|e| vec![Err(e)]);
            with(|s| {
                let mut failed = Vec::new();
                for x in r {
                    match x {
                        Ok(src) => {
                            s.packs.air.retain(|a| !a.path.eq(&src.path));
                            s.packs.air.push(src);
                        }
                        Err(e) => failed.push(i18n::tf(&lang, "pack_failed", &e)),
                    }
                }
                s.packs.air_error = (!failed.is_empty()).then(|| failed.join("\n"));
            });
            show(&ui);
        },
    );
}

/// "Build the pack" (air band): the name saved, the pack saved as packs/air-DATE.dpk.
pub fn build_air(ui: &AppWindow) {
    let p = ui.global::<Packs>();
    let name = pack_name(&p.get_air_name(), "Aerodromes");
    let sources = with(|s| {
        let _ = s.settings.save(AIR_NAME_KEY, Value::String(name.clone()));
        s.packs.air.clone()
    });
    if sources.is_empty() {
        return;
    }
    let dir = packs_dir();
    let r = files::air_pack(&sources, &name, None).map_err(|e| e.0).and_then(|pk| {
        let file = dir.join(format!("air-{}.dpk", pk.header.date));
        std::fs::write(&file, &pk.image).map_err(|e| e.to_string())?;
        Ok((pk, file))
    });
    built(ui, AIR, r);
}

/// "Choose a .dpk file...": a whole, checked pack (this program's, the C# program's or datapack.py's).
pub fn load_dpk(ui: &AppWindow, f: &Path) {
    match files::load(f) {
        Ok(pk) => {
            let t = pk.ptype;
            built(ui, t, Ok((pk, f.to_path_buf())));
            result(ui, "", "info");
        }
        Err(e) => result(ui, &trf("pack_failed", &e.0), "danger"),
    }
}

/// "Write the RadioID pack" / "Write the air band pack": the question first (nothing is written before Yes).
pub fn ask_write(ui: &AppWindow, ptype: u8) {
    if !radio_allowed(ui) {
        return;
    }
    let lang = with(|s| s.lang.clone());
    let Some(text) = with(|s| {
        let pk = if ptype == RADIO_ID { s.packs.radioid.as_ref() } else { s.packs.air_pack.as_ref() };
        let port = s.port_now.port.clone().unwrap_or_else(|| "-".into());
        pk.map(|(p, _)| confirm_text(&lang, p, &port))
    }) else {
        return;
    };
    with(|s| s.packs.pending = Some(ptype));
    let p = ui.global::<Packs>();
    p.set_write_confirm_text(text.into());
    p.set_write_confirm_open(true);
}

fn write_yes(ui: &AppWindow) {
    ui.global::<Packs>().set_write_confirm_open(false);
    if !radio_allowed(ui) || reader_holds_port(ui) || port_busy(ui) {
        return;
    }
    with_port(ui, write_on);
}

/// The write once the port is known (crate::with_port): the gates again, then the data pack tool (writes).
fn write_on(ui: &AppWindow, found: Option<String>) {
    if !radio_allowed(ui) || reader_holds_port(ui) || port_busy(ui) {
        return;
    }
    let Some(port) = found else {
        result(ui, &tr("radio_nocable"), "warning");
        return;
    };
    let Some(file) = with(|s| {
        let ptype = s.packs.pending.take()?;
        let pk = if ptype == RADIO_ID { s.packs.radioid.as_ref() } else { s.packs.air_pack.as_ref() };
        pk.map(|(_, f)| f.clone())
    }) else {
        return;
    };
    with(|s| {
        s.packs.sectors_done = 0;
        s.packs.sectors_all = 0;
        s.packs.radio_text.clear();
    });
    ui.global::<Packs>().set_progress(0.0);
    result(ui, &tr("pack_writing"), "info");
    let line = radioid::fold(&tr("pack_screen_line"));
    let argv = vec![
        "write".into(),
        "--port".into(),
        port,
        "--pack".into(),
        file.to_string_lossy().into_owned(),
        "--line".into(),
        line,
    ];
    crate::run_tool_lines(ui, "datapack", argv, true, tool_line, move |ui, err| {
        match err {
            None => {
                ui.global::<Packs>().set_progress(1.0);
                result(ui, &tr("pack_write_done"), "success");
            }
            Some(e) => result(ui, &trf("pack_write_failed", &e.record()), "danger"),
        }
        show(ui);
    });
}

/// "Read the radio's packs": the radio information and both slots of each type (read only, no power cycle).
pub fn read_radio(ui: &AppWindow) {
    if !radio_allowed(ui) || reader_holds_port(ui) || port_busy(ui) {
        return;
    }
    with_port(ui, read_on);
}

fn read_on(ui: &AppWindow, found: Option<String>) {
    if !radio_allowed(ui) || reader_holds_port(ui) || port_busy(ui) {
        return;
    }
    let Some(port) = found else {
        result(ui, &tr("radio_nocable"), "warning");
        return;
    };
    with(|s| s.packs.radio_text.clear());
    result(ui, &tr("pack_reading"), "info");
    show(ui);
    let argv = vec!["read".into(), "--port".into(), port];
    crate::run_tool_lines(ui, "datapack", argv, false, tool_line, |ui, err| {
        match err {
            None => result(ui, &tr("pack_read_done"), "success"),
            Some(e) => result(ui, &trf("pack_failed", &e.record()), "danger"),
        }
        show(ui);
    });
}

/// One line of the data pack tool (read or write): a slot, the radio's build, the plan, a written sector.
pub fn tool_line(ui: &AppWindow, line: &str) {
    let lang = with(|s| s.lang.clone());
    let parse = |l: &str| serde_json::from_str::<Value>(l).ok();
    if line.starts_with("{\"slot\"") {
        if let Some(v) = parse(line) {
            let l = slot_line(&lang, &v["slot"]);
            with(|s| {
                if !s.packs.radio_text.is_empty() {
                    s.packs.radio_text.push('\n');
                }
                s.packs.radio_text.push_str(&l);
            });
        }
    } else if line.starts_with("{\"radio\"") {
        if let Some(v) = parse(line) {
            let r = &v["radio"];
            let t = i18n::tn(
                &lang,
                "pack_radio_info",
                &[r["git"].as_str().unwrap_or("-"), r["build"].as_str().unwrap_or("-")],
            );
            with(|s| s.packs.radio_text = t);
        }
    } else if line.starts_with("{\"plan\"") {
        if let Some(v) = parse(line) {
            let p = &v["plan"];
            let n = p["sectors"].as_array().map_or(0, Vec::len);
            with(|s| s.packs.sectors_all = n);
            let text = i18n::tn(
                &lang,
                "pack_plan",
                &[
                    &p["slot"].as_i64().unwrap_or(0).to_string(),
                    p["address"].as_str().unwrap_or("-"),
                    &n.to_string(),
                    &p["seq"].as_i64().unwrap_or(0).to_string(),
                ],
            );
            result(ui, &text, "info");
        }
    } else if line.starts_with("pack: sector ") {
        let v = with(|s| {
            s.packs.sectors_done += 1;
            if s.packs.sectors_all == 0 {
                0.0
            } else {
                s.packs.sectors_done as f32 / s.packs.sectors_all as f32
            }
        });
        ui.global::<Packs>().set_progress(v.min(1.0));
    }
    show(ui);
}

/// Example contents for the screenshots (no file, no radio).
pub fn demo(ui: &AppWindow, lang: &str) {
    let p = ui.global::<Packs>();
    let slot = |t: &str, n: i64, addr: &str, valid: bool, force: bool, name: &str, count: i64| {
        serde_json::json!({"type": t, "slot": n, "address": addr, "valid": valid, "crc_ok": valid, "in_force": force,
            "seq": 3, "name": name, "date": "20261121", "bytes": 81520, "count": count})
    };
    let lines = [
        i18n::tn(lang, "pack_radio_info", &["1.0.1-ddm32", "2026-11-24"]),
        slot_line(lang, &slot("radioid", 0, "0xE70000", true, true, "RadioID TR", 3120)),
        slot_line(lang, &slot("radioid", 1, "0xEA0000", false, false, "", 0)),
        slot_line(lang, &slot("air", 0, "0xED0000", true, true, "TR aerodromes", 41)),
    ];
    p.set_radio_text(lines.join("\n").into());
    p.set_csv_text(i18n::tn(lang, "pack_csv", &["…/DDM32 Plus/packs/user.csv", &kb(lang, 48_211_000)]).into());
    p.set_csv_ready(true);
    p.set_countries("Turkey, Cyprus".into());
    p.set_radioid_name("RadioID TR".into());
    p.set_radioid_result(
        i18n::tn(lang, "pack_desc_radioid", &["RadioID TR", "2026-11-24", "3120", &kb(lang, 83_520)]).into(),
    );
    p.set_air_list(
        i18n::tn(
            lang,
            "pack_air_source",
            &["LTAC", "7", "LT_AD_2_LTAC_en.pdf", "TWR 118.100 GND 121.900 ATIS 126.200 ..."],
        )
        .into(),
    );
    p.set_air_ready(true);
    p.set_air_name("TR aerodromes".into());
    p.set_air_result("".into());
    p.set_chosen_text(
        i18n::tn(lang, "pack_desc_radioid", &["RadioID TR", "2026-11-24", "3120", &kb(lang, 83_520)]).into(),
    );
    p.set_radioid_pack(true);
    p.set_air_pack(false);
    p.set_result("".into());
    p.set_progress(0.0);
    p.set_working(false);
    p.set_downloading(false);
    p.set_download_confirm_open(false);
    p.set_write_confirm_open(false);
    let _ = ui.global::<App>();
}

/// A small RadioID pack for the screenshots (made in memory; the count says what a real list holds).
pub fn demo_pack() -> BuiltPack {
    let entries = [(2869999i64, "TA1TST".to_string(), "Test".to_string())];
    let payload = dp::radioid_payload(&entries, dp::ID_CHARS).expect("a valid entry");
    let image = dp::image(RADIO_ID, &payload, "20261124", "RadioID TR", 0, dp::TAG).expect("a valid pack");
    let header = dp::parse(&image).0.expect("its own header");
    BuiltPack { ptype: RADIO_ID, image, header, count: 3120, skipped: None }
}
