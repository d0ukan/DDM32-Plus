// SPDX-License-Identifier: GPL-3.0-or-later
//! What the window says about versions, and the start-up update question (pure parts, tested in tests/updates.rs):
//!
//! - what the radio runs, as the program knows it ([`known_firmware`]): the record of the last verified radio write
//!   (settings.json `installed_sha256` / `installed_firmware`, the C# program's keys, plus `installed_record` with its
//!   time and the backup it came with), or a verified backup taken after it; never more than that;
//! - the version line "Program 3.0.0 · Radio firmware 1.0.1 (from your last install)" (title bar and Home);
//! - Home's firmware line, compared with what the radio runs ([`firmware_status`]);
//! - the program update with the same beta fallback as the firmware list ([`newer_program`]);
//! - the question after the start-up check ([`update_offer`]): "Skip this version" per item is stored in
//!   settings.json, "Later" asks again at the next start (and not again in this session for the same versions); the
//!   firmware part only from the install record or a backup newer than it, never from an older backup;
//! - a failed check is written to `logs/update-check.log` ([`log_check_error`]) and shown on Home.
//!
//! Nothing here touches the radio: a firmware update from the question only opens the update wizard.

use crate::i18n;
use ddm32_core::radio::codec::Codec;
use ddm32_core::radio::flow::StepResult;
use ddm32_core::releases::catalog::{self, AppRelease, Catalog, FirmwareVersion};
use ddm32_core::releases::version::ReleaseVersion;
use ddm32_core::storage::Settings;
use serde_json::{json, Value};
use std::io::Write;
use std::path::{Path, PathBuf};

/// What the radio runs, as the program knows it (never read from the radio here).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Installed {
    /// No verified backup and no install record yet: not read.
    NotRead,
    /// A DDM32 Plus release: its version and label, e.g. ("1.0.1", "Latest"); the label is empty when the version is
    /// not in the list.
    Plus { version: String, label: String },
    /// Known, but not a listed DDM32 Plus: factory firmware, Classic, an unlisted DDM32 Plus (the words, translated).
    Other(String),
    /// A backup whose program image is not recognised.
    Unknown,
    /// The last install or restore stopped after its writes had started: what the radio runs now is not known.
    Interrupted,
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

// ---- the record of the last verified radio write ----------------------------------------------------------------------

/// The C# program's keys (UpdatePage / RestorePage): the installed application's SHA-256 (null after a restore) and
/// "DDM32 Plus 1.0.1" / "stock" / "".
pub const INSTALLED_SHA_KEY: &str = "installed_sha256";
pub const INSTALLED_FIRMWARE_KEY: &str = "installed_firmware";
/// This program's record with its time and the radio it belongs to (the C# program keeps it as an unknown key).
pub const INSTALLED_RECORD_KEY: &str = "installed_record";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RecordKind {
    /// DDM32 Plus installed and verified (new backup, last backup / quick update, or without a backup).
    Install,
    /// A backup's program flash written back and verified (going back to an earlier DDM32 Plus or Classic).
    RestorePrevious,
    /// The factory firmware put back from a backup and verified.
    RestoreStock,
    /// The writes started and did not finish: unknown.
    Interrupted,
}

impl RecordKind {
    fn name(self) -> &'static str {
        match self {
            RecordKind::Install => "install",
            RecordKind::RestorePrevious => "restore_previous",
            RecordKind::RestoreStock => "restore_stock",
            RecordKind::Interrupted => "interrupted",
        }
    }
    fn parse(s: &str) -> Option<Self> {
        Some(match s {
            "install" => RecordKind::Install,
            "restore_previous" => RecordKind::RestorePrevious,
            "restore_stock" => RecordKind::RestoreStock,
            "interrupted" => RecordKind::Interrupted,
            _ => return None,
        })
    }
}

/// What settings.json says the last verified radio write left on the radio.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InstallRecord {
    pub kind: RecordKind,
    /// Seconds since 1970 (UTC); None for a record of the C# program, which keeps no time.
    pub unix: Option<i64>,
    /// The installed application's SHA-256 (an install).
    pub sha256: Option<String>,
    /// The installed version ("1.0.1") as the package said.
    pub version: Option<String>,
    /// The backup the write came with (an install's backup, the restored backup); None for an install without backup.
    pub backup: Option<PathBuf>,
    /// The radio it belongs to, as far as the radio tells: the chips' JEDEC IDs of that backup ("progmem c84014,
    /// q128 ef4018"). Every DM-32UV has the same chips, so this tells the model, not the unit.
    pub radio: Option<String>,
}

/// The record in settings.json: this program's `installed_record`, else a C# install record (`installed_sha256`, no
/// time). A C# `installed_firmware` alone is not a record (the C# backup step writes the user's own note there).
pub fn read_record(settings: &Settings) -> Option<InstallRecord> {
    let s = settings.load();
    let sha = s.get(INSTALLED_SHA_KEY).and_then(Value::as_str).filter(|x| !x.is_empty()).map(str::to_string);
    if let Some(Value::Object(r)) = s.get(INSTALLED_RECORD_KEY) {
        let text = |k: &str| r.get(k).and_then(Value::as_str).filter(|x| !x.is_empty()).map(str::to_string);
        if let Some(kind) = text("kind").and_then(|k| RecordKind::parse(&k)) {
            let rec_sha = text("sha256");
            // the C# program wrote a newer install over ours: its sha differs and it keeps no time
            if kind == RecordKind::Install && sha.is_some() && rec_sha != sha {
                return Some(legacy(sha, &s));
            }
            // the C# program restored a backup over ours: `installed_sha256` null and `installed_firmware` "stock" / ""
            // where our record would have written something else
            if let Some(over) = csharp_restore_over(&s, kind, text("version").as_deref()) {
                return Some(over);
            }
            return Some(InstallRecord {
                kind,
                unix: r.get("unix").and_then(Value::as_i64),
                sha256: rec_sha,
                version: text("version"),
                backup: text("backup").map(PathBuf::from),
                radio: text("radio"),
            });
        }
    }
    sha.map(|sha| legacy(Some(sha), &s))
}

/// What our record would have left in the C# keys: the C# program's restore wrote `installed_sha256: null` and
/// "stock" (factory firmware) or "" (an earlier backup). When the keys say a restore that our record does not, the C#
/// restore is the newer write: a record of it (no time, no backup) is returned.
fn csharp_restore_over(
    s: &serde_json::Map<String, Value>,
    kind: RecordKind,
    version: Option<&str>,
) -> Option<InstallRecord> {
    if !matches!(s.get(INSTALLED_SHA_KEY), Some(Value::Null)) {
        return None;
    }
    let firmware = s.get(INSTALLED_FIRMWARE_KEY).and_then(Value::as_str)?;
    let ours = match kind {
        RecordKind::Install => format!("DDM32 Plus {}", version.unwrap_or("")),
        RecordKind::RestoreStock => "stock".into(),
        RecordKind::RestorePrevious | RecordKind::Interrupted => String::new(),
    };
    let restored = match firmware {
        "stock" => RecordKind::RestoreStock,
        "" => RecordKind::RestorePrevious,
        _ => return None,
    };
    if firmware == ours || (kind == RecordKind::Interrupted && restored == RecordKind::RestorePrevious) {
        return None;
    }
    Some(InstallRecord { kind: restored, unix: None, sha256: None, version: None, backup: None, radio: None })
}

fn legacy(sha: Option<String>, s: &serde_json::Map<String, Value>) -> InstallRecord {
    let version = s
        .get(INSTALLED_FIRMWARE_KEY)
        .and_then(Value::as_str)
        .and_then(|f| f.strip_prefix("DDM32 Plus "))
        .map(|v| v.trim().to_string());
    InstallRecord { kind: RecordKind::Install, unix: None, sha256: sha, version, backup: None, radio: None }
}

fn radio_of(backup: Option<&Path>) -> Option<String> {
    let chips = ddm32_core::radio::unit_image::backup_jedec(backup?).ok()?;
    Some(format!("progmem {}, q128 {}", chips.get("progmem")?, chips.get("q128")?))
}

fn write_record(settings: &Settings, sha: Option<&str>, firmware: &str, rec: &InstallRecord) -> std::io::Result<()> {
    let record = json!({
        "kind": rec.kind.name(),
        "unix": rec.unix,
        "utc": ddm32_core::releases::client::format_utc(rec.unix.unwrap_or(0)),
        "sha256": rec.sha256,
        "version": rec.version,
        "backup": rec.backup.as_ref().map(|b| b.display().to_string()),
        "radio": rec.radio,
    });
    settings.save_all(vec![
        (INSTALLED_SHA_KEY, sha.map_or(Value::Null, |x| Value::String(x.into()))),
        (INSTALLED_FIRMWARE_KEY, Value::String(firmware.into())),
        (INSTALLED_RECORD_KEY, record),
    ])
}

/// After an install run (new backup, last backup / quick update, or without a backup): a verified install records
/// what the radio runs now (the C# keys too); a run that stopped after its writes started records "unknown"; a run
/// that wrote nothing leaves the record as it was.
pub fn record_install_result(
    settings: &Settings,
    r: &StepResult,
    sha256: &str,
    version: &str,
    backup: Option<&Path>,
    now_unix: i64,
) -> std::io::Result<()> {
    if r.passed {
        let rec = InstallRecord {
            kind: RecordKind::Install,
            unix: Some(now_unix),
            sha256: Some(sha256.into()),
            version: Some(version.into()),
            backup: backup.map(Path::to_path_buf),
            radio: radio_of(backup),
        };
        write_record(settings, Some(sha256), &format!("DDM32 Plus {version}"), &rec)
    } else if r.writes_started {
        record_interrupted(settings, backup, now_unix)
    } else {
        Ok(())
    }
}

/// After a restore run: a verified restore records the restored backup (the C# keys: no sha, "stock" or "").
pub fn record_restore_result(
    settings: &Settings,
    r: &StepResult,
    backup: &Path,
    stock: bool,
    now_unix: i64,
) -> std::io::Result<()> {
    if r.passed {
        let rec = InstallRecord {
            kind: if stock { RecordKind::RestoreStock } else { RecordKind::RestorePrevious },
            unix: Some(now_unix),
            sha256: None,
            version: None,
            backup: Some(backup.to_path_buf()),
            radio: radio_of(Some(backup)),
        };
        write_record(settings, None, if stock { "stock" } else { "" }, &rec)
    } else if r.writes_started {
        record_interrupted(settings, Some(backup), now_unix)
    } else {
        Ok(())
    }
}

fn record_interrupted(settings: &Settings, backup: Option<&Path>, now_unix: i64) -> std::io::Result<()> {
    let rec = InstallRecord {
        kind: RecordKind::Interrupted,
        unix: Some(now_unix),
        sha256: None,
        version: None,
        backup: backup.map(Path::to_path_buf),
        radio: radio_of(backup),
    };
    write_record(settings, None, "", &rec)
}

/// What a record says the radio runs: the listed release of its application (or its version when not listed), the
/// restored backup's program image, or unknown after an interrupted write.
pub fn installed_from_record(rec: &InstallRecord, all: &[FirmwareVersion], lang: &str) -> Installed {
    match rec.kind {
        RecordKind::Install => {
            if let Some(fv) = rec.sha256.as_deref().and_then(|sha| all.iter().find(|f| f.application_sha256() == sha)) {
                return Installed::Plus {
                    version: fv.version().into(),
                    label: crate::label_in(lang, catalog::label_for(fv, all)),
                };
            }
            match &rec.version {
                Some(v) => Installed::Plus { version: v.clone(), label: String::new() },
                None => Installed::Other(i18n::t(lang, "fw_ddm32_unlisted")),
            }
        }
        RecordKind::RestorePrevious | RecordKind::RestoreStock => match &rec.backup {
            Some(b) => match installed_from_backups(std::slice::from_ref(b), all, lang) {
                Installed::NotRead => Installed::Unknown,
                i => i,
            },
            // a restore whose backup is not known (the C# program's): factory firmware is still named
            None if rec.kind == RecordKind::RestoreStock => {
                Installed::Other(i18n::tf(lang, "fw_stock", "").trim_end().to_string())
            }
            None => Installed::Unknown,
        },
        RecordKind::Interrupted => Installed::Interrupted,
    }
}

/// Where the program's knowledge of the radio comes from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Source {
    /// The record of the last verified install or restore.
    Install,
    /// A verified backup (newer than the record, or there is no record): it may be older than the radio.
    Backup,
    /// Nothing known.
    None,
}

/// What the radio runs as the program knows it, with its source.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Known {
    pub installed: Installed,
    pub source: Source,
    /// The firmware part of the start-up question may use it: the install record, or a backup newer than the last
    /// install. Never a backup alone (it may be older than an install it does not know of).
    pub may_prompt: bool,
}

/// The record against the newest verified backup ((seconds since 1970, what its image holds)): the newer reading
/// wins. A C# record keeps no time: it wins unless the backup holds a newer DDM32 Plus than it.
pub fn known_firmware(record: Option<(&InstallRecord, Installed)>, newest_backup: Option<(i64, Installed)>) -> Known {
    let backup = |i: Installed, may_prompt: bool| Known { installed: i, source: Source::Backup, may_prompt };
    let install = |i: Installed| Known { installed: i, source: Source::Install, may_prompt: true };
    match (record, newest_backup) {
        (Some((rec, ri)), Some((bt, bi))) => match rec.unix {
            Some(u) if bt > u => backup(bi, true),
            Some(_) => install(ri),
            None => match (&ri, &bi) {
                (Installed::Plus { version: r, .. }, Installed::Plus { version: b, .. })
                    if ReleaseVersion::try_parse(b) > ReleaseVersion::try_parse(r) =>
                {
                    backup(bi, true)
                }
                _ => install(ri),
            },
        },
        (Some((_, ri)), None) => install(ri),
        (None, Some((_, bi))) => backup(bi, false),
        (None, None) => Known { installed: Installed::NotRead, source: Source::None, may_prompt: false },
    }
}

/// The bottom line of Home before this version (kept for the facts: "The radio runs (by the last backup): ...").
pub fn installed_text(i: &Installed, lang: &str) -> String {
    match i {
        Installed::NotRead => i18n::t(lang, "installed_none"),
        Installed::Plus { .. } | Installed::Other(_) | Installed::Unknown | Installed::Interrupted => {
            i18n::tf(lang, "installed", &short_words(i, lang))
        }
    }
}

fn plus_words(version: &str, label: &str) -> String {
    if label.is_empty() {
        version.to_string()
    } else {
        format!("{version} ({label})")
    }
}

fn short_words(i: &Installed, lang: &str) -> String {
    match i {
        Installed::Plus { version, label } => format!("DDM32 Plus {}", plus_words(version, label)),
        Installed::NotRead => i18n::t(lang, "fw_not_read"),
        Installed::Unknown => i18n::t(lang, "fw_unknown"),
        Installed::Interrupted => i18n::t(lang, "fw_interrupted"),
        Installed::Other(w) => w.clone(),
    }
}

fn source_words(lang: &str, source: Source) -> String {
    match source {
        Source::Install => i18n::t(lang, "src_install"),
        Source::Backup => i18n::t(lang, "src_backup"),
        Source::None => String::new(),
    }
}

/// What the radio runs in a few words with its source (About): "1.0.1 (Latest) (from your last install)".
pub fn installed_short(k: &Known, lang: &str) -> String {
    let w = match &k.installed {
        Installed::Plus { version, label } => plus_words(version, label),
        other => short_words(other, lang),
    };
    with_source(lang, w, k)
}

fn with_source(lang: &str, text: String, k: &Known) -> String {
    match (&k.installed, k.source) {
        (Installed::NotRead | Installed::Interrupted, _) | (_, Source::None) => text,
        (_, s) => format!("{text} {}", source_words(lang, s)),
    }
}

fn two(lang: &str, key: &str, a: &str, b: &str) -> String {
    i18n::t(lang, key).replacen("{}", a, 1).replacen("{}", b, 1)
}

/// "Program 3.0.0 · Radio firmware 1.0.1 (from your last install)" (Turkish: "Telsiz firmware"); "not read yet" /
/// "unknown" when the program does not know; the source always said, never more claimed than known.
pub fn version_text(lang: &str, program: &str, k: &Known) -> String {
    let base = match &k.installed {
        Installed::Plus { version, .. } => two(lang, "ver_line", program, version),
        Installed::NotRead => i18n::tf(lang, "ver_line_none", program),
        Installed::Unknown => i18n::tf(lang, "ver_line_unknown", program),
        Installed::Interrupted => two(lang, "ver_line_other", program, &i18n::t(lang, "fw_interrupted")),
        Installed::Other(w) => two(lang, "ver_line_other", program, w),
    };
    with_source(lang, base, k)
}

/// The window's title: the program's name, then the version line.
pub fn title_text(lang: &str, program: &str, k: &Known) -> String {
    format!("DDM32 Plus - TA2DOU · {}", version_text(lang, program, k))
}

/// Home's firmware line: the newest version the channel offers against what the radio runs.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FirmwareStatus {
    /// The radio runs this version, and nothing newer is offered.
    UpToDate(String),
    /// (installed, newest offered)
    Newer { installed: String, newest: String },
    /// The radio's version is not known (not read, not a DDM32 Plus version): only the newest is named.
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

/// The line Home shows for a firmware status (the newest with its label, from the full list), hedged when the
/// radio's version comes from a backup.
pub fn firmware_status_text(
    lang: &str,
    st: &FirmwareStatus,
    source: Source,
    offered: &[FirmwareVersion],
    all: &[FirmwareVersion],
) -> String {
    let named = |v: &str| match offered.iter().find(|f| f.version() == v) {
        Some(f) => crate::version_line(f, all, lang),
        None => v.to_string(),
    };
    let backup = source == Source::Backup;
    match st {
        FirmwareStatus::UpToDate(v) => {
            i18n::tf(lang, if backup { "update_uptodate_backup" } else { "update_uptodate" }, v)
        }
        FirmwareStatus::Newer { installed, newest } => {
            two(lang, if backup { "update_newer_backup" } else { "update_newer" }, &named(newest), installed)
        }
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
/// runs as the program knows it (only from the install record or a backup newer than it, only when that is a DDM32
/// Plus version; not the skipped version).
pub fn update_offer(
    cat: &Catalog,
    channel: i32,
    current: &str,
    known: &Known,
    skip_program: Option<&str>,
    skip_firmware: Option<&str>,
) -> UpdateOffer {
    let program = newer_program(cat, current, channel)
        .map(|a| a.entry.version)
        .filter(|v| Some(v.as_str()) != skip_program)
        .map(|v| (current.to_string(), v));
    let firmware = match firmware_status(&known.installed, &crate::offered(cat, channel)) {
        FirmwareStatus::Newer { installed, newest } if known.may_prompt && Some(newest.as_str()) != skip_firmware => {
            Some((installed, newest))
        }
        _ => None,
    };
    UpdateOffer { program, firmware }
}

/// The firmware part's detail line: the wizard opens; a release with a map pack says the pack goes with it (and a
/// map pack of a newer kind than this program reads says to update the program first; docs/mappack-design.md H.2).
pub fn firmware_detail(lang: &str, fv: Option<&FirmwareVersion>) -> String {
    let mut d = crate::i18n::t(lang, "prompt_firmware_d");
    if let Some(f) = fv {
        if f.entry.ref_pack.is_some() {
            d.push(' ');
            d.push_str(&crate::i18n::t(lang, "prompt_firmware_map"));
        } else if f.entry.ref_pack_newer {
            d.push(' ');
            d.push_str(&crate::i18n::t(lang, "prompt_firmware_map_newer"));
        }
    }
    d
}

/// Whether the question is shown now.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PromptDecision {
    Show,
    /// Something is under way (a radio job, the live view or the search log holds the port; the wizard is past its
    /// first step; a restore is ready; a download runs; another dialog is open) or the first-start page is open:
    /// asked as soon as that is over.
    Wait,
    /// Nothing new, or "Later" was answered for these very versions in this session.
    No,
}

pub fn prompt_decision(
    offer: &UpdateOffer,
    later: Option<&UpdateOffer>,
    blocked: bool,
    first_start: bool,
) -> PromptDecision {
    if offer.is_empty() || later == Some(offer) {
        PromptDecision::No
    } else if blocked || first_start {
        PromptDecision::Wait
    } else {
        PromptDecision::Show
    }
}

/// What blocks the question (and the program update's restart): the window's state in a few flags.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Busy {
    /// A radio job, a download, the live view or the search log (App.menu-radio-locked).
    pub port_held: bool,
    /// The wizard is past its first step and not finished.
    pub wizard_active: bool,
    /// A restore is planned and ready to run.
    pub restore_ready: bool,
    /// Another dialog is open (the no-backup confirmation, the cable picker, the question itself).
    pub dialog_open: bool,
}

impl Busy {
    pub fn any(&self) -> bool {
        self.port_held || self.wizard_active || self.restore_ready || self.dialog_open
    }
}

/// The wizard's steps 1-4 (backup choice, backup, check, install) are under way; step 0 (version) and 5 (done) not.
pub fn wizard_active(page: i32, step: i32) -> bool {
    page == 1 && (1..=4).contains(&step)
}

/// "Skip this version" for one item: the version stored, the rest of the question.
pub fn skip_item(offer: &UpdateOffer, program: bool) -> (Option<String>, UpdateOffer) {
    let mut rest = offer.clone();
    let v = if program { rest.program.take() } else { rest.firmware.take() };
    (v.map(|x| x.1), rest)
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

/// Seconds since 1970 now (the record's time).
pub fn now_unix() -> i64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs() as i64)
}
