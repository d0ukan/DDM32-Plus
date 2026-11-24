// SPDX-License-Identifier: GPL-3.0-or-later
//! Going back (the C# RestorePage): the shortcuts "Back to the previous version" (settings.json
//! `restore_previous_backup`, the backup taken right before the last install) and "Back to stock firmware"
//! (`stock_backups`, the backups taken while the radio ran stock firmware), "Choose another backup folder...", and
//! the list of every backup found. Choosing one only detects and plans (files only); the restore itself starts from
//! the Restore button and its Yes / No question (No by default), behind the disclaimer, never by itself.

use crate::{i18n, set_message, tr, with, App, AppWindow};
use ddm32_core::pyjson::PyDict;
use ddm32_core::radio::codec::Codec;
use ddm32_core::radio::flow::FileSteps;
use ddm32_core::storage::paths::is_verified_backup;
use ddm32_core::storage::Settings;
use serde_json::Value;
use slint::ComponentHandle;
use std::path::{Path, PathBuf};

pub const PREVIOUS_KEY: &str = "restore_previous_backup";
pub const STOCK_KEY: &str = "stock_backups";

/// A path saved by an older version under ROOT\yedekler (and the like) mapped to the new folder when it was moved;
/// any other path as it is (the C# AppPaths.MapLegacy).
pub fn map_legacy(root: &Path, path: &str) -> PathBuf {
    let p = PathBuf::from(path);
    if path.is_empty() || p.exists() {
        return p;
    }
    let lower = path.to_lowercase().replace('\\', "/");
    for (old, new) in ddm32_core::app_info::LEGACY_FOLDERS {
        let old_root = format!("{}/", root.join(old).to_string_lossy().to_lowercase().replace('\\', "/"));
        if lower.starts_with(&old_root) {
            let mapped = root.join(new).join(&path[old_root.len()..]);
            return if mapped.exists() { mapped } else { p };
        }
    }
    p
}

/// The backups recorded as taken while the radio ran stock firmware (settings.json `stock_backups`, a list).
pub fn stock_backups(settings: &Settings) -> Vec<String> {
    match settings.load().get(STOCK_KEY) {
        Some(Value::Array(a)) => {
            a.iter().filter_map(Value::as_str).filter(|s| !s.is_empty()).map(String::from).collect()
        }
        _ => vec![],
    }
}

/// Records a backup whose program image is stock firmware (the wizard's check, a planned restore): kept for "Back to
/// stock firmware" (the C# AppState.RememberStockBackup).
pub fn remember_stock_backup(settings: &Settings, folder: &Path) -> std::io::Result<()> {
    let f = folder.display().to_string();
    let mut list = stock_backups(settings);
    if list.contains(&f) {
        return Ok(());
    }
    list.push(f);
    settings.save(STOCK_KEY, Value::Array(list.into_iter().map(Value::String).collect()))
}

/// Records the backup an install came with (after a verified install, or one that stopped after its writes began):
/// "Back to the previous version" restores it.
pub fn remember_previous_backup(settings: &Settings, folder: &Path) -> std::io::Result<()> {
    settings.save(PREVIOUS_KEY, Value::String(folder.display().to_string()))
}

/// The candidates of a shortcut, best first: "previous" the C# key, then this program's install record's backup;
/// "stock" every recorded stock backup.
pub fn candidates(kind: &str, settings: &Settings) -> Vec<String> {
    if kind == "stock" {
        return stock_backups(settings);
    }
    let mut out = Vec::new();
    if let Some(p) = settings.get_string(PREVIOUS_KEY).filter(|p| !p.is_empty()) {
        out.push(p);
    }
    if let Some(rec) = crate::updates::read_record(settings) {
        if rec.kind == crate::updates::RecordKind::Install {
            if let Some(b) = rec.backup {
                out.push(b.display().to_string());
            }
        }
    }
    out
}

/// The backup a shortcut restores: its first candidate that is a verified backup now (old Turkish folder names
/// followed), or None.
pub fn default_backup(kind: &str, settings: &Settings, root: &Path) -> Option<PathBuf> {
    candidates(kind, settings).iter().map(|c| map_legacy(root, c)).find(|c| is_verified_backup(c))
}

/// The page's lines for the chosen backup: its name, the plan's summary, the flow's message.
pub fn status_text(lang: &str, backup: Option<&Path>, plan: Option<&PyDict>, message: &(&str, Vec<String>)) -> String {
    let mut lines = Vec::new();
    if let Some(b) = backup {
        lines.push(i18n::tf(lang, "res_chosen", &b.file_name().unwrap_or_default().to_string_lossy()));
    }
    if let Some(p) = plan {
        let n = p.get("changed_sectors").and_then(|v| v.as_i64()).unwrap_or(0).to_string();
        let first = p.str("first").unwrap_or("-");
        let last = p.str("last").unwrap_or("-");
        lines.push(i18n::tn(lang, "res_plan_summary", &[&n, first, last]));
    }
    if !message.0.is_empty() {
        let args: Vec<&str> = message.1.iter().map(String::as_str).collect();
        lines.push(i18n::tn(lang, message.0, &args));
    }
    lines.join("\n")
}

/// The Yes / No question before the restore (the C# Res_Confirm): the chosen backup's full path, the page's lines,
/// the short disclaimer.
pub fn confirm_text(lang: &str, backup: &Path, status: &str) -> String {
    i18n::tn(
        lang,
        "res_confirm",
        &[&i18n::tf(lang, "res_chosen", &backup.display().to_string()), status, &i18n::t(lang, "disc_short")],
    )
}

/// Detects and plans a restore of `folder` (files only) as `kind` ("previous", "stock"; None: from the backup's own
/// layout). The Restore button opens when the plan is safe. True when it is.
pub fn choose(ui: &AppWindow, kind: Option<&str>, folder: &Path) -> bool {
    let app = ui.global::<App>();
    app.set_restore_ready(false);
    set_message(ui, "", "info");
    let detected = FileSteps::detect_backup(folder, Codec::dm32());
    let stock = detected.as_ref().ok().and_then(|d| d.str("layout")) == Some("stock");
    if stock {
        let _ = with(|s| remember_stock_backup(&s.settings, folder));
    }
    let kind = kind.unwrap_or(if stock { "stock" } else { "previous" });
    let ok = with(|st| {
        if st.restore.choose(kind, folder).is_err() || st.restore.start_detect().is_err() {
            return false;
        }
        match detected {
            Ok(d) => st.restore.detect_finished(Some(d), ""),
            Err(e) => {
                st.restore.detect_finished(None, &e.record());
                return false;
            }
        }
        let Some((_, _, mode)) = st.restore.tool() else { return false };
        if st.restore.start_plan().is_err() {
            return false;
        }
        match FileSteps::restore_plan(mode, folder) {
            Ok(p) => {
                let safe = FileSteps::safe(&p);
                st.restore.plan_finished(Some(p), "", safe);
                safe
            }
            Err(e) => {
                st.restore.plan_finished(None, &e.record(), true);
                false
            }
        }
    });
    show_status(ui);
    app.set_restore_ready(ok);
    if ok {
        set_message(ui, &tr("msg_restore_plan"), "info");
    }
    ok
}

/// The chosen backup's lines from the flow.
pub fn show_status(ui: &AppWindow) {
    let text = with(|s| {
        status_text(&s.lang, s.restore.backup_dir.as_deref(), s.restore.plan_data.as_ref(), &s.restore.message)
    });
    ui.global::<App>().set_res_status(text.into());
}

/// "Back to the previous version" / "Back to stock firmware": the recorded backup planned, or the reason there is
/// none (the C# Res_NoBackup + Res_NoPrevious / Res_NoStock).
pub fn shortcut(ui: &AppWindow, kind: &str) -> bool {
    if ui.global::<App>().get_busy() {
        return false;
    }
    let found = with(|s| default_backup(kind, &s.settings, &s.paths.root));
    ui.global::<App>().set_selected_backup(-1);
    match found {
        Some(folder) => choose(ui, Some(kind), &folder),
        None => {
            ui.global::<App>().set_restore_ready(false);
            ui.global::<App>().set_res_status("".into());
            let why = tr(if kind == "stock" { "res_no_stock" } else { "res_no_previous" });
            set_message(ui, &format!("{}\n{why}", tr("res_no_backup")), "warning");
            false
        }
    }
}

/// "Choose another backup folder...": a verified backup only (else said so); then planned by its own layout (or as
/// stock when the stock shortcut was the last choice, as C#).
pub fn other_folder(ui: &AppWindow, folder: &Path) -> bool {
    if !is_verified_backup(folder) {
        set_message(ui, &tr("not_verified_backup"), "warning");
        return false;
    }
    ui.global::<App>().set_selected_backup(-1);
    let last_stock = with(|s| s.restore.kind.as_deref() == Some("stock"));
    choose(ui, if last_stock { Some("stock") } else { None }, folder)
}

/// The Restore button: the Yes / No question first (nothing starts before Yes).
pub fn ask(ui: &AppWindow) {
    if !crate::radio_allowed(ui) || !ui.global::<App>().get_restore_ready() {
        return;
    }
    let Some(backup) = with(|s| s.restore.backup_dir.clone()) else { return };
    let lang = with(|s| s.lang.clone());
    let status = ui.global::<App>().get_res_status().to_string();
    let app = ui.global::<App>();
    app.set_res_confirm_text(confirm_text(&lang, &backup, &status).into());
    app.set_res_confirm_open(true);
}
