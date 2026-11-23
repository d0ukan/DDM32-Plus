// SPDX-License-Identifier: GPL-3.0-or-later
//! What the window says about versions, and the start-up update question (pure parts, tested in tests/updates.rs):
//!
//! - what the radio runs, as the program knows it: the newest verified backup's program image ([`Installed`]);
//! - the version line "Program 3.0.0 · Radio firmware 1.0.1" (title bar and Home);
//! - Home's firmware line, compared with what the radio runs ([`firmware_status`]);
//! - the program update with the same beta fallback as the firmware list ([`newer_program`]);
//! - the question after the start-up check ([`update_offer`]): "Skip this version" is stored in settings.json, "Later"
//!   asks again at the next start (and not again in this session for the same versions);
//! - a failed check is written to `logs/update-check.log` ([`log_check_error`]) and shown on Home.
//!
//! Nothing here touches the radio: a firmware update from the question only opens the update wizard.

use crate::i18n;
use ddm32_core::radio::codec::Codec;
use ddm32_core::releases::catalog::{self, AppRelease, Catalog, FirmwareVersion};
use ddm32_core::releases::version::ReleaseVersion;
use std::io::Write;
use std::path::{Path, PathBuf};

/// What the radio runs, as the program knows it (by the newest verified backup; never read from the radio here).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Installed {
    /// No verified backup yet: not read.
    NotRead,
    /// A DDM32 Plus release of the list: its version and label, e.g. ("1.0.1", "Latest").
    Plus { version: String, label: String },
    /// Known, but not a listed DDM32 Plus: factory firmware, Classic, an unlisted DDM32 Plus (the words, translated).
    Other(String),
    /// A backup whose program image is not recognised.
    Unknown,
}

/// What a backup's program image holds: the DDM32 Plus release whose signed application is in it, or the kind of
/// firmware.
pub fn installed_firmware(program: &[u8], all: &[FirmwareVersion], lang: &str) -> Installed {
    use ddm32_core::releases::signed_manifest::sha256_hex;
    const START: usize = 0xC000;
    for fv in all {
        let n = fv.application_size() as usize;
        if program.len() >= START + n && sha256_hex(&program[START..START + n]) == fv.application_sha256() {
            return Installed::Plus {
                version: fv.version().to_string(),
                label: crate::label_in(lang, catalog::label_for(fv, all)),
            };
        }
    }
    match ddm32_core::radio::unit_image::detect(program, Codec::dm32()) {
        Ok(d) => match (d.str("layout"), d.str("firmware")) {
            (Some("stock"), _) => Installed::Other(i18n::tf(lang, "fw_stock", d.str("stock_version").unwrap_or(""))),
            (_, Some("ddm32")) => Installed::Other(i18n::t(lang, "fw_ddm32_unlisted")),
            (_, Some("classic")) => Installed::Other(i18n::t(lang, "fw_classic")),
            _ => Installed::Unknown,
        },
        Err(_) => Installed::Unknown,
    }
}

/// What the radio runs by the first (newest) verified backup in `folders` that holds both program images.
pub fn installed_from_backups(folders: &[PathBuf], all: &[FirmwareVersion], lang: &str) -> Installed {
    folders
        .iter()
        .find_map(|f| ddm32_core::radio::unit_image::saved_images(f).ok())
        .and_then(|imgs| imgs.get("progmem-pass1.bin").map(|p| installed_firmware(p, all, lang)))
        .unwrap_or(Installed::NotRead)
}

/// The bottom line of Home before this version (kept for the facts: "The radio runs (by the last backup): ...").
pub fn installed_text(i: &Installed, lang: &str) -> String {
    match i {
        Installed::NotRead => i18n::t(lang, "installed_none"),
        Installed::Plus { version, label } => i18n::tf(lang, "installed", &format!("DDM32 Plus {version} ({label})")),
        Installed::Other(w) => i18n::tf(lang, "installed", w),
        Installed::Unknown => i18n::tf(lang, "installed", &i18n::t(lang, "fw_unknown")),
    }
}

/// What the radio runs in a few words (About): "1.0.1 (Latest)", "not read yet", "unknown", "Factory firmware 047".
pub fn installed_short(i: &Installed, lang: &str) -> String {
    match i {
        Installed::Plus { version, label } => format!("{version} ({label})"),
        Installed::NotRead => i18n::t(lang, "fw_not_read"),
        Installed::Unknown => i18n::t(lang, "fw_unknown"),
        Installed::Other(w) => w.clone(),
    }
}

fn two(lang: &str, key: &str, a: &str, b: &str) -> String {
    i18n::t(lang, key).replacen("{}", a, 1).replacen("{}", b, 1)
}

/// "Program 3.0.0 · Radio firmware 1.0.1" (Turkish: "Telsiz firmware"); "not read yet" / "unknown" when the program
/// does not know; never a version the program has not seen in a backup.
pub fn version_text(lang: &str, program: &str, i: &Installed) -> String {
    match i {
        Installed::Plus { version, .. } => two(lang, "ver_line", program, version),
        Installed::NotRead => i18n::tf(lang, "ver_line_none", program),
        Installed::Unknown => i18n::tf(lang, "ver_line_unknown", program),
        Installed::Other(w) => two(lang, "ver_line_other", program, w),
    }
}

/// The window's title: the program's name, then the version line.
pub fn title_text(lang: &str, program: &str, i: &Installed) -> String {
    format!("DDM32 Plus - TA2DOU · {}", version_text(lang, program, i))
}

/// Home's firmware line: the newest version the channel offers against what the radio runs.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FirmwareStatus {
    /// The radio runs this version, and nothing newer is offered.
    UpToDate(String),
    /// (installed, newest offered)
    Newer { installed: String, newest: String },
    /// The radio's version is not known (not read, not a listed DDM32 Plus): only the newest is named.
    Newest(String),
    /// No version list.
    NoList,
}

pub fn firmware_status(i: &Installed, offered: &[FirmwareVersion]) -> FirmwareStatus {
    let Some(newest) = offered.first() else { return FirmwareStatus::NoList };
    match i {
        Installed::Plus { version, .. } => {
            match (ReleaseVersion::try_parse(version), ReleaseVersion::try_parse(newest.version())) {
                (Some(have), Some(top)) if top > have => {
                    FirmwareStatus::Newer { installed: version.clone(), newest: newest.version().to_string() }
                }
                (Some(_), Some(_)) => FirmwareStatus::UpToDate(version.clone()),
                _ => FirmwareStatus::Newest(newest.version().to_string()),
            }
        }
        _ => FirmwareStatus::Newest(newest.version().to_string()),
    }
}

/// The line Home shows for a firmware status (the newest with its label, from the full list).
pub fn firmware_status_text(
    lang: &str,
    st: &FirmwareStatus,
    offered: &[FirmwareVersion],
    all: &[FirmwareVersion],
) -> String {
    let named = |v: &str| match offered.iter().find(|f| f.version() == v) {
        Some(f) => crate::version_line(f, all, lang),
        None => v.to_string(),
    };
    match st {
        FirmwareStatus::UpToDate(v) => i18n::tf(lang, "update_uptodate", v),
        FirmwareStatus::Newer { installed, newest } => two(lang, "update_newer", &named(newest), installed),
        FirmwareStatus::Newest(v) => i18n::tf(lang, "update_newest", &named(v)),
        FirmwareStatus::NoList => i18n::t(lang, "update_none"),
    }
}

/// The newer build of this program for this platform that the channel offers: prereleases from Beta on, and on the
/// default channel too while no stable build has been published at all (the firmware list's fallback, `offered`).
pub fn newer_program(cat: &Catalog, current: &str, channel: i32) -> Option<AppRelease> {
    let no_stable = !cat.apps.iter().any(|a| !a.entry.version.contains('-'));
    cat.newer_app(current, channel >= 1 || no_stable).cloned()
}

/// What the start-up question offers: (current, newer) for the program and for the radio firmware.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct UpdateOffer {
    pub program: Option<(String, String)>,
    pub firmware: Option<(String, String)>,
}

impl UpdateOffer {
    pub fn is_empty(&self) -> bool {
        self.program.is_none() && self.firmware.is_none()
    }
}

/// The question after a check: a newer program (not the skipped version), and a newer radio firmware than the radio
/// runs as the program knows it (only when that is a listed DDM32 Plus version; not the skipped version).
pub fn update_offer(
    cat: &Catalog,
    channel: i32,
    current: &str,
    installed: &Installed,
    skip_program: Option<&str>,
    skip_firmware: Option<&str>,
) -> UpdateOffer {
    let program = newer_program(cat, current, channel)
        .map(|a| a.entry.version)
        .filter(|v| Some(v.as_str()) != skip_program)
        .map(|v| (current.to_string(), v));
    let firmware = match firmware_status(installed, &crate::offered(cat, channel)) {
        FirmwareStatus::Newer { installed, newest } if Some(newest.as_str()) != skip_firmware => {
            Some((installed, newest))
        }
        _ => None,
    };
    UpdateOffer { program, firmware }
}

/// Whether the question is shown now.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PromptDecision {
    Show,
    /// A radio job holds the port, or the first-start page is open: asked as soon as that is over.
    Wait,
    /// Nothing new, or "Later" was answered for these very versions in this session.
    No,
}

pub fn prompt_decision(
    offer: &UpdateOffer,
    later: Option<&UpdateOffer>,
    port_held: bool,
    first_start: bool,
) -> PromptDecision {
    if offer.is_empty() || later == Some(offer) {
        PromptDecision::No
    } else if port_held || first_start {
        PromptDecision::Wait
    } else {
        PromptDecision::Show
    }
}

/// The versions "Skip this version" stores: (program, firmware), each only when offered.
pub fn skip_versions(offer: &UpdateOffer) -> (Option<String>, Option<String>) {
    (offer.program.as_ref().map(|p| p.1.clone()), offer.firmware.as_ref().map(|f| f.1.clone()))
}

pub const CHECK_LOG: &str = "update-check.log";

/// Appends a failed check to `logs/update-check.log` ("2026-11-24T08:00:00Z update check failed: ...").
pub fn log_check_error(logs: &Path, when_utc: &str, error: &str) -> std::io::Result<PathBuf> {
    std::fs::create_dir_all(logs)?;
    let file = logs.join(CHECK_LOG);
    let mut f = std::fs::OpenOptions::new().create(true).append(true).open(&file)?;
    let line = error.replace(['\r', '\n'], " ");
    writeln!(f, "{when_utc} update check failed: {line}")?;
    Ok(file)
}

/// Seconds between two automatic checks while the window stays open.
pub const RECHECK_SECONDS: u64 = 24 * 60 * 60;
