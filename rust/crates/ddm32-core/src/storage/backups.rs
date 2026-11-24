// SPDX-License-Identifier: GPL-3.0-or-later
//! Where backups go and where they are found (owner, 2026-11-21):
//!
//! - The user never chooses the backup folder. Every new backup goes to the program's own `backups`
//!   ([`new_backup_folder`]); a release build has no other target and no folder prompt. Choosing a folder exists
//!   only in a developer build (`--features developer`, [`developer::new_backup_folder_in`]).
//! - At start the program finds every verified backup by itself ([`discover`]): its own `backups` first, then,
//!   read only, the older Python and C# programs' `backups` folders on Windows. The newest verified backup that
//!   holds DDM32 Plus is offered for the quick update ([`last_for_quick_update`]); the tool still checks live that it
//!   is the same radio.
//! - Nothing is ever written to, moved out of or deleted from a found folder; program updates (`update::self_update`,
//!   which refuses any file under `backups`) and the uninstaller (`AppPaths::removable_on_uninstall`, the setup's
//!   `uninsneveruninstall`) never delete `backups`.

use super::paths::{is_verified_backup, unique_folder, AppPaths, Env, Os};
use super::settings::Settings;
use crate::app_info;
use crate::radio::codec::Codec;
use crate::radio::flow::{is_ddm32, FileSteps};
use std::path::{Path, PathBuf};
use std::time::SystemTime;

/// A folder that is searched for backups.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BackupRoot {
    pub folder: PathBuf,
    /// "own" (this program's backups), "python", "net" (the older programs', read only) or "chosen" (developer).
    pub origin: &'static str,
}

impl BackupRoot {
    /// Only the program's own `backups` is written to (new backups, install logs).
    pub fn writable(&self) -> bool {
        self.origin == "own"
    }
}

/// Every folder searched for backups, own `backups` first. A release build ignores folders remembered in the
/// settings (only a developer build can have chosen one).
pub fn backup_roots(paths: &AppPaths, settings: &Settings, os: Os, env: Env<'_>) -> Vec<BackupRoot> {
    // own backups, then an old yedekler that could not be moved (read like any other root)
    let mut roots: Vec<BackupRoot> =
        paths.own_backup_roots().into_iter().map(|folder| BackupRoot { folder, origin: "own" }).collect();
    if app_info::DEVELOPER {
        for f in settings.backup_folders() {
            roots.push(BackupRoot { folder: PathBuf::from(f), origin: "chosen" });
        }
    }
    for other in paths.find_other_data(os, env) {
        roots.push(BackupRoot { folder: other.backups, origin: other.program });
    }
    let mut out: Vec<BackupRoot> = Vec::new();
    for r in roots {
        if !out.iter().any(|o| super::paths::same_path(&o.folder, &r.folder)) {
            out.push(r);
        }
    }
    out
}

/// The folder a new backup goes to: `backups/dm32-YYYYMMDD` (or -b, -c ... when it exists). There is no other
/// target in a release build.
pub fn new_backup_folder(paths: &AppPaths, today: &str) -> PathBuf {
    unique_folder(&paths.backups(), &format!("dm32-{today}"))
}

/// A release build never asks the user for a backup folder.
pub const BACKUP_FOLDER_PROMPT: bool = app_info::DEVELOPER;

/// Developer builds only: a backup into a folder the developer chose (remembered in the settings).
#[cfg(feature = "developer")]
pub mod developer {
    use super::*;

    pub fn new_backup_folder_in(chosen: &Path, settings: &Settings, today: &str) -> std::io::Result<PathBuf> {
        settings.remember_backup_folder(chosen)?;
        Ok(unique_folder(chosen, &format!("dm32-{today}")))
    }
}

/// A verified backup found on this PC.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FoundBackup {
    pub folder: PathBuf,
    pub origin: &'static str,
    pub modified: SystemTime,
}

/// The verified backup folders under each root, newest first (by the folder's last write time).
pub fn discover(roots: &[BackupRoot]) -> Vec<FoundBackup> {
    let mut seen: Vec<PathBuf> = Vec::new();
    let mut out = Vec::new();
    for root in roots {
        let Ok(rd) = std::fs::read_dir(&root.folder) else { continue };
        let mut dirs: Vec<PathBuf> = rd.filter_map(Result::ok).map(|e| e.path()).filter(|p| p.is_dir()).collect();
        dirs.sort();
        for d in dirs {
            let full = std::path::absolute(&d).unwrap_or_else(|_| d.clone());
            if seen.iter().any(|s| super::paths::same_path(s, &full)) {
                continue;
            }
            seen.push(full);
            if is_verified_backup(&d) {
                let modified = backup_time(&d);
                out.push(FoundBackup { folder: d, origin: root.origin, modified });
            }
        }
    }
    out.sort_by_key(|b| std::cmp::Reverse(b.modified));
    out
}

/// When a backup was taken: its manifest's `completed_utc` (else `started_utc`), written by the backup tool; the
/// folder's last write time only when neither is there or readable (copying a folder changes its time, not these).
pub fn backup_time(folder: &Path) -> SystemTime {
    let stamp = std::fs::read(folder.join("backup-manifest.json"))
        .ok()
        .and_then(|b| serde_json::from_slice::<serde_json::Value>(&b).ok())
        .and_then(|m| {
            ["completed_utc", "started_utc"].iter().find_map(|k| {
                let t = m.get(*k)?.as_str()?;
                chrono::DateTime::parse_from_rfc3339(t).ok()
            })
        });
    match stamp {
        Some(t) if t.timestamp() > 0 => SystemTime::UNIX_EPOCH + std::time::Duration::from_secs(t.timestamp() as u64),
        _ => std::fs::metadata(folder).and_then(|m| m.modified()).unwrap_or(SystemTime::UNIX_EPOCH),
    }
}

/// BackupFinder.Candidates: the verified backup folders under the given folders, newest first.
pub fn candidates(roots: &[PathBuf]) -> Vec<PathBuf> {
    let r: Vec<BackupRoot> = roots.iter().map(|f| BackupRoot { folder: f.clone(), origin: "own" }).collect();
    discover(&r).into_iter().map(|b| b.folder).collect()
}

/// The newest verified backup (of the first `max`) that holds DDM32 Plus: "the last backup of this radio" for the
/// quick update (the tool checks live that it is the same radio).
pub fn last_for_quick_update(roots: &[PathBuf], codec: &Codec, max: usize) -> Option<PathBuf> {
    candidates(roots)
        .into_iter()
        .take(max)
        .find(|c| FileSteps::detect_backup(c, codec).is_ok_and(|d| is_ddm32(Some(&d))))
}

/// Whether a path lies inside the own `backups` (the only place the program writes backups and logs to).
pub fn inside_own_backups(paths: &AppPaths, p: &Path) -> bool {
    let root = std::path::absolute(paths.backups()).unwrap_or_else(|_| paths.backups());
    let p = std::path::absolute(p).unwrap_or_else(|_| p.to_path_buf());
    p.starts_with(&root)
}
