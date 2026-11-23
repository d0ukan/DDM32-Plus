// SPDX-License-Identifier: GPL-3.0-or-later
//! settings.json in the data folder (Storage/Settings.cs): a JSON object written atomically (temporary file, then
//! replace). A missing or damaged file reads as empty settings; unknown keys are kept.

use serde_json::{Map, Value};
use std::path::{Path, PathBuf};

pub const LANGUAGE_KEY: &str = "language";
pub const AUTO_CHECK_KEY: &str = "auto_check";
pub const INCLUDE_BETA_KEY: &str = "include_beta";
pub const DISCLAIMER_KEY: &str = "disclaimer";
pub const THEME_KEY: &str = "theme";
pub const BACKUP_FOLDERS_KEY: &str = "backup_folders";
/// The serial port the user chose in the cable picker, and whether the automatic choice (the CH340 cable) is used.
pub const PORT_KEY: &str = "port";
pub const PORT_AUTO_KEY: &str = "port_auto";
/// A version the start-up update question was told to skip ("Skip this version"); it can still be chosen by hand.
pub const SKIP_PROGRAM_KEY: &str = "skip_program_version";
pub const SKIP_FIRMWARE_KEY: &str = "skip_firmware_version";

#[derive(Debug, Clone)]
pub struct Settings {
    pub file_path: PathBuf,
}

/// The GUI theme: light by default (owner, 2026-11-21), dark when chosen in the settings.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Theme {
    Light,
    Dark,
}

impl Settings {
    pub fn new(path: impl AsRef<Path>) -> Settings {
        Settings { file_path: path.as_ref().to_path_buf() }
    }

    pub fn load(&self) -> Map<String, Value> {
        std::fs::read(&self.file_path)
            .ok()
            .and_then(|b| serde_json::from_slice::<Value>(&b).ok())
            .and_then(|v| match v {
                Value::Object(o) => Some(o),
                _ => None,
            })
            .unwrap_or_default()
    }

    pub fn save(&self, key: &str, value: Value) -> std::io::Result<()> {
        let mut s = self.load();
        s.insert(key.to_string(), value);
        self.write(&s)
    }

    fn write(&self, s: &Map<String, Value>) -> std::io::Result<()> {
        if let Some(dir) = self.file_path.parent() {
            std::fs::create_dir_all(dir)?;
        }
        let tmp = self.file_path.with_extension("tmp");
        let text = serde_json::to_string_pretty(&Value::Object(s.clone())).expect("JSON") + "\n";
        std::fs::write(&tmp, text)?;
        std::fs::rename(&tmp, &self.file_path)
    }

    pub fn get_string(&self, key: &str) -> Option<String> {
        self.load().get(key).and_then(Value::as_str).map(str::to_string)
    }

    pub fn get_bool(&self, key: &str, fallback: bool) -> bool {
        self.load().get(key).and_then(Value::as_bool).unwrap_or(fallback)
    }

    /// "tr" or "en", or None before the first-start question.
    pub fn language(&self) -> Option<String> {
        self.get_string(LANGUAGE_KEY).filter(|l| l == "tr" || l == "en")
    }

    pub fn set_language(&self, lang: &str) -> std::io::Result<()> {
        self.save(LANGUAGE_KEY, Value::String(lang.into()))
    }

    pub fn auto_check(&self) -> bool {
        self.get_bool(AUTO_CHECK_KEY, true)
    }

    pub fn set_auto_check(&self, v: bool) -> std::io::Result<()> {
        self.save(AUTO_CHECK_KEY, Value::Bool(v))
    }

    pub fn include_beta(&self) -> bool {
        self.get_bool(INCLUDE_BETA_KEY, true)
    }

    pub fn set_include_beta(&self, v: bool) -> std::io::Result<()> {
        self.save(INCLUDE_BETA_KEY, Value::Bool(v))
    }

    /// Light unless "dark" was chosen.
    pub fn theme(&self) -> Theme {
        match self.get_string(THEME_KEY).as_deref() {
            Some("dark") => Theme::Dark,
            _ => Theme::Light,
        }
    }

    pub fn set_theme(&self, t: Theme) -> std::io::Result<()> {
        self.save(THEME_KEY, Value::String(if t == Theme::Dark { "dark" } else { "light" }.into()))
    }

    /// Folders a developer chose for backups (developer builds only; a release build never asks for a folder and
    /// ignores this key, see storage::backups).
    pub fn backup_folders(&self) -> Vec<String> {
        match self.load().get(BACKUP_FOLDERS_KEY) {
            Some(Value::Array(a)) => a
                .iter()
                .map(|x| match x {
                    Value::String(s) => s.clone(),
                    Value::Null => String::new(),
                    other => other.to_string(),
                })
                .filter(|s| !s.is_empty())
                .collect(),
            _ => vec![],
        }
    }

    pub fn remember_backup_folder(&self, folder: &Path) -> std::io::Result<()> {
        let mut list = self.backup_folders();
        if list.iter().any(|x| super::paths::same_path(Path::new(x), folder)) {
            return Ok(());
        }
        list.push(folder.to_string_lossy().into_owned());
        self.save(BACKUP_FOLDERS_KEY, Value::Array(list.into_iter().map(Value::String).collect()))
    }

    /// The cable picker's choice: automatic (the CH340 cable, the default) or one port by name.
    pub fn port_pref(&self) -> crate::radio::portpick::PortPref {
        let s = self.load();
        let port = s.get(PORT_KEY).and_then(Value::as_str).map(str::trim).filter(|p| !p.is_empty()).map(str::to_string);
        let auto = s.get(PORT_AUTO_KEY).and_then(Value::as_bool).unwrap_or(true);
        crate::radio::portpick::PortPref { auto: auto || port.is_none(), port }
    }

    pub fn set_port_pref(&self, pref: &crate::radio::portpick::PortPref) -> std::io::Result<()> {
        let mut s = self.load();
        s.insert(PORT_AUTO_KEY.into(), Value::Bool(pref.auto));
        match &pref.port {
            Some(p) => s.insert(PORT_KEY.into(), Value::String(p.clone())),
            None => s.remove(PORT_KEY),
        };
        self.write(&s)
    }

    /// The program version the start-up question skips (None: ask for every newer one).
    pub fn skip_program_version(&self) -> Option<String> {
        self.get_string(SKIP_PROGRAM_KEY).filter(|v| !v.is_empty())
    }

    pub fn set_skip_program_version(&self, version: &str) -> std::io::Result<()> {
        self.save(SKIP_PROGRAM_KEY, Value::String(version.into()))
    }

    /// The radio firmware version the start-up question skips.
    pub fn skip_firmware_version(&self) -> Option<String> {
        self.get_string(SKIP_FIRMWARE_KEY).filter(|v| !v.is_empty())
    }

    pub fn set_skip_firmware_version(&self, version: &str) -> std::io::Result<()> {
        self.save(SKIP_FIRMWARE_KEY, Value::String(version.into()))
    }

    /// The disclaimer was accepted for this text version.
    pub fn disclaimer_accepted(&self, version: &str) -> bool {
        match self.load().get(DISCLAIMER_KEY) {
            Some(Value::Object(d)) => {
                d.get("version").and_then(Value::as_str) == Some(version)
                    && d.get("accepted").and_then(Value::as_bool) == Some(true)
            }
            _ => false,
        }
    }

    pub fn accept_disclaimer(&self, version: &str, now_utc: &str) -> std::io::Result<()> {
        self.save(DISCLAIMER_KEY, serde_json::json!({"version": version, "accepted": true, "accepted_utc": now_utc}))
    }

    pub fn withdraw_disclaimer(&self, version: &str, now_utc: &str) -> std::io::Result<()> {
        self.save(DISCLAIMER_KEY, serde_json::json!({"version": version, "accepted": false, "withdrawn_utc": now_utc}))
    }
}
