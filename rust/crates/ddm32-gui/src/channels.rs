// SPDX-License-Identifier: GPL-3.0-or-later
//! My channels (the C# ChannelsPage): the codeplug of a backup's data flash image, read only. The newest verified
//! backup loads by itself; "Open another backup..." takes another folder. A search over every column, a zone and a
//! mode filter for the channels, all columns or the five simple ones, a sort by any column (numbers as numbers), a
//! double click on a zone shows its channels, and the rows shown saved as CSV. Nothing is ever written to a radio or
//! a backup.

use crate::{i18n, trf, with, AppWindow, Radio, TableRow, ZoneRow};
use ddm32_core::codeplug::{self, Codeplug, Table};
use ddm32_core::storage::backups;
use ddm32_core::storage::paths::{is_verified_backup, real_env};
use ddm32_core::storage::Os;
use slint::{ComponentHandle, ModelRc, SharedString, VecModel};
use std::cmp::Ordering;
use std::collections::HashMap;
use std::path::{Path, PathBuf};

/// The channels page's choices (UI thread).
#[derive(Debug, Clone, Default)]
pub struct ChannelView {
    /// The zone filter: a zone's slot, or None for every channel
    pub zone: Option<i64>,
    /// The mode filter ("DMR", "P25", ...), or None for every mode
    pub mode: Option<String>,
    pub all_columns: bool,
    /// (shown column, ascending)
    pub sort: Option<(usize, bool)>,
    /// The backup shown and whether the user chose it (then the newest one does not replace it)
    pub source: Option<PathBuf>,
    pub chosen: bool,
    pub verified: bool,
    /// The zone slots of the zones table's rows as shown (a double click on row i shows zone_rows[i])
    pub zone_rows: Vec<i64>,
    /// Which picker is open: "zone" or "mode"
    pub picker: String,
}

/// The channels a zone filter and the C# columns: the five simple ones, or every field but the two made from others.
pub const SIMPLE_CHANNEL_KEYS: [&str; 5] = ["number", "name", "freq", "mode", "summary"];
pub const DERIVED_KEYS: [&str; 2] = ["freq", "summary"];
/// The order of the modes in the mode filter (others after them, by name).
pub const MODE_ORDER: [&str; 6] = ["FM", "DMR", "P25", "NXDN", "dPMR", "YSF"];

/// One row of a table as {column key: text}.
pub type Row = HashMap<String, String>;

/// What a table shows: the search, and for the channels the zone, the mode, all columns and the sort.
#[derive(Debug, Clone, Default)]
pub struct Query<'a> {
    pub search: &'a str,
    pub zone: Option<i64>,
    pub mode: Option<&'a str>,
    pub all_columns: bool,
    pub sort: Option<(usize, bool)>,
}

/// The columns a table shows as (key, title).
pub fn shown_columns(cp: &Codeplug, table: Table, all_columns: bool) -> Vec<(&'static str, &'static str)> {
    let cols = cp.columns(table);
    match table {
        Table::Channels if all_columns => cols.iter().filter(|c| !DERIVED_KEYS.contains(&c.0)).copied().collect(),
        Table::Channels => {
            SIMPLE_CHANNEL_KEYS.iter().filter_map(|k| cols.iter().find(|c| c.0 == *k).copied()).collect()
        }
        _ => cols.to_vec(),
    }
}

/// Numbers compare as numbers ("10" after "9"), texts ignoring case (the C# RowComparer).
pub fn compare_cells(a: &str, b: &str) -> Ordering {
    match (a.trim().parse::<f64>(), b.trim().parse::<f64>()) {
        (Ok(x), Ok(y)) => x.partial_cmp(&y).unwrap_or(Ordering::Equal),
        _ => a.to_lowercase().cmp(&b.to_lowercase()),
    }
}

/// The rows of a table under a query, before (`all`) and after the search and mode filter (`shown`, sorted). The
/// zone filter picks `all` (a zone's channels in the zone's order).
pub fn query_rows(cp: &Codeplug, table: Table, q: &Query<'_>) -> (Vec<Row>, Vec<Row>) {
    let all: Vec<HashMap<String, String>> = match (table, q.zone) {
        (Table::Channels, Some(slot)) => match cp.zones.iter().find(|z| z.slot == slot) {
            Some(z) => cp.zone_channels(z).into_iter().map(|c| c.values.clone()).collect(),
            None => cp.rows(table),
        },
        _ => cp.rows(table),
    };
    let mut shown: Vec<HashMap<String, String>> = all
        .iter()
        .filter(|r| codeplug::matches(r, q.search))
        .filter(|r| table != Table::Channels || q.mode.is_none_or(|m| r.get("mode").map(String::as_str) == Some(m)))
        .cloned()
        .collect();
    if let Some((col, asc)) = q.sort {
        if let Some((key, _)) = shown_columns(cp, table, q.all_columns).get(col).copied() {
            let get = |r: &HashMap<String, String>| r.get(key).cloned().unwrap_or_default();
            shown.sort_by(|a, b| {
                let o = compare_cells(&get(a), &get(b));
                if asc {
                    o
                } else {
                    o.reverse()
                }
            });
        }
    }
    (all, shown)
}

/// (column titles, rows of the shown columns) of a table, the rows matching the search (the five simple channel
/// columns, no zone or mode filter, unsorted).
pub fn table_view(cp: &Codeplug, table: Table, search: &str) -> (Vec<String>, Vec<Vec<String>>) {
    let q = Query { search, ..Default::default() };
    let (titles, rows, _) = view(cp, table, &q);
    (titles, rows)
}

/// (titles, cells, total rows before the search) under a query.
pub fn view(cp: &Codeplug, table: Table, q: &Query<'_>) -> (Vec<String>, Vec<Vec<String>>, usize) {
    let cols = shown_columns(cp, table, q.all_columns);
    let (all, shown) = query_rows(cp, table, q);
    let titles = cols.iter().map(|c| c.1.to_string()).collect();
    let rows =
        shown.iter().map(|r| cols.iter().map(|(k, _)| r.get(*k).cloned().unwrap_or_default()).collect()).collect();
    (titles, rows, all.len())
}

/// The columns' widths: fractions of the table's width, and the width in px the table needs when all columns are
/// shown (it then scrolls sideways; 0 = it fits the window).
pub fn widths(cp: &Codeplug, table: Table, all_columns: bool) -> (Vec<f32>, f32) {
    let fit = |w: &[f32]| (w.to_vec(), 0.0);
    match table {
        Table::Channels if all_columns => {
            let px: Vec<f32> = shown_columns(cp, table, true)
                .iter()
                .map(|(k, _)| match *k {
                    "number" | "cc" | "ts" | "bw" => 56.0,
                    "name" | "rx_list" | "contact" => 150.0,
                    "code" => 130.0,
                    _ => 96.0,
                })
                .collect();
            let sum: f32 = px.iter().sum();
            let n = px.len() as f32;
            (px.iter().map(|w| w / sum).collect(), sum + 8.0 * (n - 1.0) + 46.0)
        }
        Table::Channels => fit(&[0.07, 0.22, 0.2, 0.1, 0.41]),
        Table::Zones => fit(&[0.07, 0.25, 0.13, 0.55]),
        Table::Contacts => fit(&[0.07, 0.33, 0.2, 0.2, 0.2]),
        Table::Lists => fit(&[0.07, 0.25, 0.13, 0.55]),
    }
}

pub fn table_of(i: i32) -> Table {
    match i {
        1 => Table::Zones,
        2 => Table::Contacts,
        3 => Table::Lists,
        _ => Table::Channels,
    }
}

/// The modes of the codeplug in the C# order, with their counts.
pub fn modes(cp: &Codeplug) -> Vec<(String, usize)> {
    let mut m: Vec<(String, usize)> = cp.mode_counts().into_iter().collect();
    let rank = |k: &str| MODE_ORDER.iter().position(|x| *x == k).unwrap_or(99);
    m.sort_by(|a, b| rank(&a.0).cmp(&rank(&b.0)).then_with(|| a.0.cmp(&b.0)));
    m
}

fn message(ui: &AppWindow, text: &str, kind: &str) {
    let r = ui.global::<Radio>();
    r.set_message(text.into());
    r.set_message_kind(kind.into());
}

pub fn wire(ui: &AppWindow) {
    let r = ui.global::<Radio>();
    let w = ui.as_weak();
    r.on_choose_table(move |t| {
        let ui = w.unwrap();
        ui.global::<Radio>().set_table(t);
        with(|s| s.channels.sort = None);
        show_table(&ui);
    });
    let w = ui.as_weak();
    r.on_search_changed(move |_| show_table(&w.unwrap()));
    let w = ui.as_weak();
    r.on_export_csv(move || export_csv(&w.unwrap()));
    let w = ui.as_weak();
    r.on_open_backup(move || {
        let ui = w.unwrap();
        let start = with(|s| s.paths.backups());
        if let Some(folder) = rfd::FileDialog::new().set_directory(&start).pick_folder() {
            open_backup(&ui, &folder);
        }
    });
    let w = ui.as_weak();
    r.on_open_zone_picker(move || open_picker(&w.unwrap(), "zone"));
    let w = ui.as_weak();
    r.on_open_mode_picker(move || open_picker(&w.unwrap(), "mode"));
    let w = ui.as_weak();
    r.on_pick(move |key| pick(&w.unwrap(), &key));
    let w = ui.as_weak();
    r.on_toggle_columns(move |all| {
        let ui = w.unwrap();
        with(|s| {
            s.channels.all_columns = all;
            s.channels.sort = None;
        });
        show_table(&ui);
    });
    let w = ui.as_weak();
    r.on_sort(move |i| sort(&w.unwrap(), i));
    let w = ui.as_weak();
    r.on_zone_activate(move |row| zone_activate(&w.unwrap(), row));
}

/// A column's title clicked: sorted by it, ascending; clicked again: descending.
pub fn sort(ui: &AppWindow, col: i32) {
    if col < 0 {
        return;
    }
    let col = col as usize;
    with(|s| {
        s.channels.sort = match s.channels.sort {
            Some((c, true)) if c == col => Some((c, false)),
            _ => Some((col, true)),
        }
    });
    show_table(ui);
}

/// A double click on the zones table's row: the channels tab with that zone's channels.
pub fn zone_activate(ui: &AppWindow, row: i32) {
    let Some(slot) = with(|s| s.channels.zone_rows.get(row.max(0) as usize).copied()) else { return };
    with(|s| {
        s.channels.zone = Some(slot);
        s.channels.sort = None;
    });
    ui.global::<Radio>().set_table(0);
    show_table(ui);
}

fn open_picker(ui: &AppWindow, which: &str) {
    let lang = with(|s| s.lang.clone());
    let rows: Vec<(String, String)> =
        with(|s| {
            let Some(cp) = s.codeplug.as_ref() else { return vec![] };
            if which == "zone" {
                std::iter::once(("zone:-1".to_string(), i18n::t(&lang, "ch_all_zones")))
                    .chain(cp.zones.iter().map(|z| {
                        (format!("zone:{}", z.slot), format!("{} · {} ({})", z.index, z.name, z.channels.len()))
                    }))
                    .collect()
            } else {
                std::iter::once(("mode:".to_string(), i18n::t(&lang, "ch_all_modes")))
                    .chain(modes(cp).into_iter().map(|(m, n)| (format!("mode:{m}"), format!("{m} ({n})"))))
                    .collect()
            }
        });
    if rows.is_empty() {
        return;
    }
    let selected = with(|s| {
        s.channels.picker = which.to_string();
        if which == "zone" {
            format!("zone:{}", s.channels.zone.unwrap_or(-1))
        } else {
            format!("mode:{}", s.channels.mode.clone().unwrap_or_default())
        }
    });
    let r = ui.global::<Radio>();
    r.set_pick_title(
        i18n::t(&lang, if which == "zone" { "ch_zone_title" } else { "ch_mode" }).trim_end_matches(':').into(),
    );
    r.set_pick_rows(ModelRc::new(VecModel::from(
        rows.into_iter().map(|(k, n)| ZoneRow { key: k.into(), name: n.into() }).collect::<Vec<_>>(),
    )));
    r.set_pick_selected(selected.into());
    r.set_zone_picker_open(true);
}

/// A filter chosen in the picker ("zone:5", "zone:-1", "mode:DMR", "mode:"); the picker closes.
pub fn pick(ui: &AppWindow, key: &str) {
    if let Some(z) = key.strip_prefix("zone:") {
        let slot = z.parse::<i64>().ok().filter(|s| *s >= 0);
        with(|s| s.channels.zone = slot);
    } else if let Some(m) = key.strip_prefix("mode:") {
        with(|s| s.channels.mode = (!m.is_empty()).then(|| m.to_string()));
    }
    ui.global::<Radio>().set_zone_picker_open(false);
    show_table(ui);
}

/// The backups the page looks in (this program's, the C# and Python programs' read only).
fn newest_backup_with_codeplug() -> Option<PathBuf> {
    let found = with(|s| {
        let roots = backups::backup_roots(&s.paths, &s.settings, Os::current(), &real_env);
        backups::discover(&roots)
    });
    found.iter().find(|b| b.folder.join("q128-pass1.bin").is_file()).map(|b| b.folder.clone())
}

/// Opening the page: the backup the user chose before, else the newest verified backup (files only), decoded in the
/// window's language.
pub fn load_channels(ui: &AppWindow) {
    let (chosen, source) = with(|s| (s.channels.chosen, s.channels.source.clone()));
    let folder = if chosen { source } else { newest_backup_with_codeplug() };
    message(ui, "", "info");
    match folder {
        Some(f) => {
            if let Err(e) = load_folder(ui, &f) {
                message(ui, &trf("ch_failed", &e), "danger");
            }
        }
        None => {
            with(|s| {
                s.codeplug = None;
                s.channels.source = None;
            });
        }
    }
    show_table(ui);
}

/// "Open another backup...": that folder (or a 16 MiB image file), files only; it stays chosen until the program ends.
pub fn open_backup(ui: &AppWindow, folder: &Path) {
    match load_folder(ui, folder) {
        Ok(()) => {
            with(|s| {
                s.channels.chosen = true;
                s.channels.zone = None;
                s.channels.mode = None;
                s.channels.sort = None;
            });
            message(ui, "", "info");
        }
        Err(e) => message(ui, &trf("ch_failed", &e), "danger"),
    }
    show_table(ui);
}

fn load_folder(_ui: &AppWindow, folder: &Path) -> Result<(), String> {
    let lang = with(|s| s.lang.clone());
    let img = Codeplug::load_image(folder).map_err(|e| e.0)?;
    let cp = Codeplug::decode(&img, codeplug::CodeplugText::for_lang(&lang)).map_err(|e| e.0)?;
    let verified = folder.is_dir() && is_verified_backup(folder);
    with(|s| {
        if s.channels.source.as_deref() != Some(folder) {
            s.channels.zone = None;
            s.channels.mode = None;
            s.channels.sort = None;
        }
        s.codeplug = Some(cp);
        s.channels.source = Some(folder.to_path_buf());
        s.channels.verified = verified;
    });
    Ok(())
}

pub fn show_table(ui: &AppWindow) {
    let r = ui.global::<Radio>();
    let table = table_of(r.get_table());
    let search = r.get_search().to_string();
    let lang = with(|s| s.lang.clone());
    let out = with(|s| {
        let cp = s.codeplug.as_ref()?;
        let ch = &s.channels;
        let q = Query {
            search: &search,
            zone: ch.zone,
            mode: ch.mode.as_deref(),
            all_columns: ch.all_columns,
            sort: ch.sort,
        };
        let (titles, rows, total) = view(cp, table, &q);
        let zone_rows = if table == Table::Zones {
            query_rows(cp, table, &q)
                .1
                .iter()
                .filter_map(|r| r.get("index")?.parse::<i64>().ok())
                .filter_map(|i| cp.zones.iter().find(|z| z.index == i).map(|z| z.slot))
                .collect()
        } else {
            vec![]
        };
        let (w, min) = widths(cp, table, ch.all_columns && table == Table::Channels);
        let zone_text = match ch.zone.and_then(|slot| cp.zones.iter().find(|z| z.slot == slot)) {
            Some(z) => i18n::tf(&lang, "ch_zone", &format!("{} · {}", z.index, z.name)),
            None => i18n::tf(&lang, "ch_zone", &i18n::t(&lang, "ch_all_zones")),
        };
        let mode_text = format!(
            "{} {}",
            i18n::t(&lang, "ch_mode"),
            ch.mode.clone().unwrap_or_else(|| i18n::t(&lang, "ch_all_modes"))
        );
        let name = ch.source.as_ref().and_then(|p| p.file_name()).map(|n| n.to_string_lossy().into_owned());
        let source = name.map(|n| {
            let mut t = i18n::tf(&lang, "channels_source", &n);
            if !ch.verified {
                t.push_str(&i18n::t(&lang, "ch_unverified"));
            }
            t
        });
        let modes_line = modes(cp).iter().map(|(m, n)| format!("{m} {n}")).collect::<Vec<_>>().join(", ");
        let radio = format!(
            "{}{}",
            i18n::t(&lang, "ch_radio")
                .replacen("{}", if cp.radio_name.is_empty() { "-" } else { &cp.radio_name }, 1)
                .replacen("{}", &if cp.dmr_id != 0 { cp.dmr_id.to_string() } else { "-".into() }, 1),
            if modes_line.is_empty() { String::new() } else { format!("  ·  {modes_line}") }
        );
        Some((titles, rows, total, zone_rows, w, min, zone_text, mode_text, source, radio, ch.sort, ch.all_columns))
    });
    let Some((titles, rows, total, zone_rows, w, min, zone_text, mode_text, source, radio, sort, all)) = out else {
        with(|s| s.channels.zone_rows.clear());
        r.set_table_loaded(false);
        r.set_channels_source("".into());
        r.set_channels_radio("".into());
        r.set_table_count("".into());
        r.set_table_columns(ModelRc::default());
        r.set_table_rows(ModelRc::default());
        r.set_table_min_width(0.0);
        return;
    };
    with(|s| s.channels.zone_rows = zone_rows);
    r.set_table_loaded(true);
    r.set_channels_source(source.unwrap_or_default().into());
    r.set_channels_radio(radio.into());
    r.set_zone_text(zone_text.into());
    r.set_mode_text(mode_text.into());
    r.set_all_columns(all);
    r.set_sort_column(sort.map_or(-1, |(c, _)| c as i32));
    r.set_sort_ascending(sort.is_none_or(|(_, a)| a));
    r.set_table_count(i18n::tn(&lang, "ch_shown", &[&rows.len().to_string(), &total.to_string()]).into());
    r.set_table_columns(ModelRc::new(VecModel::from(titles.into_iter().map(SharedString::from).collect::<Vec<_>>())));
    r.set_table_widths(ModelRc::new(VecModel::from(w)));
    r.set_table_min_width(min);
    let rows: Vec<TableRow> = rows
        .into_iter()
        .map(|cells| TableRow {
            cells: ModelRc::new(VecModel::from(cells.into_iter().map(SharedString::from).collect::<Vec<_>>())),
        })
        .collect();
    r.set_table_rows(ModelRc::new(VecModel::from(rows)));
}

/// The rows the tab shows now (search, filters, sort) with all their columns, as CSV (Excel opens it).
fn export_csv(ui: &AppWindow) {
    let r = ui.global::<Radio>();
    let table = table_of(r.get_table());
    let search = r.get_search().to_string();
    let Some(csv) = with(|s| {
        s.codeplug.as_ref().map(|cp| {
            let ch = &s.channels;
            let q = Query {
                search: &search,
                zone: ch.zone,
                mode: ch.mode.as_deref(),
                all_columns: ch.all_columns,
                sort: ch.sort,
            };
            let (_, rows) = query_rows(cp, table, &q);
            codeplug::to_csv(cp.columns(table), &rows)
        })
    }) else {
        return;
    };
    let name = ["kanallar.csv", "bolgeler.csv", "kisiler.csv", "rx-gruplari.csv"][r.get_table().clamp(0, 3) as usize];
    let Some(path) = rfd::FileDialog::new().set_file_name(name).add_filter("CSV", &["csv"]).save_file() else { return };
    match codeplug::write_csv(&path, &csv) {
        Ok(()) => message(ui, &trf("csv_saved", &path.display().to_string()), "success"),
        Err(e) => message(ui, &trf("msg_failed", &e.to_string()), "danger"),
    }
}

/// The zone picker as the screenshots show it (no backup needed).
pub fn demo_picker(ui: &AppWindow, lang: &str) {
    let r = ui.global::<Radio>();
    let rows = [
        ("zone:-1", i18n::t(lang, "ch_all_zones")),
        ("zone:0", "1 · Simplex (12)".to_string()),
        ("zone:1", "2 · DMR TR (24)".to_string()),
        ("zone:2", "3 · P25 (6)".to_string()),
    ];
    r.set_pick_title(i18n::t(lang, "ch_zone_title").into());
    r.set_pick_rows(ModelRc::new(VecModel::from(
        rows.iter().map(|(k, n)| ZoneRow { key: (*k).into(), name: n.clone().into() }).collect::<Vec<_>>(),
    )));
    r.set_pick_selected("zone:1".into());
    r.set_zone_picker_open(true);
}
