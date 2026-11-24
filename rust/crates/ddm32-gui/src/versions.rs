// SPDX-License-Identifier: GPL-3.0-or-later
//! The versions page (the C# VersionsPage): every signed version with its label and its state (on the radio, newer /
//! older than the radio's, downloaded or not, legacy, test), the notes in both languages, "Check for updates" with the
//! last check's time and error, the releases whose signature or format failed, and the program's own update. Network
//! access only from the check button, the start-up / 24 h check or a local release folder; "Switch to this version"
//! only opens the update wizard at that version (every radio step still needs its own click).

use crate::updates::Installed;
use crate::{i18n, label_in, with, App, AppWindow, VerRow};
use ddm32_core::releases::catalog::{self, Catalog, FirmwareVersion, ReleaseLabel};
use ddm32_core::releases::client::{self, Package};
use ddm32_core::releases::version::ReleaseVersion;
use slint::{ComponentHandle, ModelRc, VecModel};

/// The kind of a row (its colour on the page): latest, stable, beta, test, legacy.
pub fn kind_name(l: ReleaseLabel) -> &'static str {
    match l {
        ReleaseLabel::Latest => "latest",
        ReleaseLabel::Stable => "stable",
        ReleaseLabel::Beta => "beta",
        ReleaseLabel::TestAlpha => "test",
        ReleaseLabel::Legacy => "legacy",
    }
}

/// A version's states, in the C# order: on the radio (or newer / older than the radio's), legacy or downloaded / not
/// downloaded, test, local. `downloaded`: the versions whose package on this computer is installable.
pub fn states(
    lang: &str,
    fv: &FirmwareVersion,
    all: &[FirmwareVersion],
    installed: &Installed,
    downloaded: &[String],
) -> Vec<String> {
    let t = |k: &str| i18n::t(lang, k);
    let label = catalog::label_for(fv, all);
    let mut out = Vec::new();
    if let Installed::Plus { version, .. } = installed {
        match (ReleaseVersion::try_parse(version), ReleaseVersion::try_parse(fv.version())) {
            (Some(have), Some(this)) if this == have => out.push(t("state_on_radio")),
            (Some(have), Some(this)) if this > have => out.push(t("state_newer")),
            (Some(_), Some(_)) => out.push(t("state_older")),
            _ if version == fv.version() => out.push(t("state_on_radio")),
            _ => {}
        }
    }
    if label == ReleaseLabel::Legacy {
        out.push(t("state_legacy"));
    } else {
        out.push(t(if downloaded.iter().any(|d| d == fv.version()) {
            "state_downloaded"
        } else {
            "state_not_downloaded"
        }));
    }
    if label == ReleaseLabel::TestAlpha {
        out.push(t("state_test"));
    }
    if fv.is_local() {
        out.push(t("state_local"));
    }
    out
}

/// Every version of the list as the page shows it (newest first, legacy ones too).
pub fn rows(lang: &str, cat: &Catalog, installed: &Installed, downloaded: &[String]) -> Vec<VerRow> {
    cat.firmware
        .iter()
        .map(|fv| {
            let label = catalog::label_for(fv, &cat.firmware);
            VerRow {
                version: fv.version().into(),
                label: label_in(lang, label).into(),
                kind: kind_name(label).into(),
                state: states(lang, fv, &cat.firmware, installed, downloaded).join(", ").into(),
                notes: fv.notes(lang).into(),
                notes_both: format!("TR: {}\nEN: {}", fv.notes("tr"), fv.notes("en")).into(),
                installable: label != ReleaseLabel::Legacy,
            }
        })
        .collect()
}

/// The releases not listed, with the reason (a signature, the manifest's format, a release mismatch), or the line
/// that none failed.
pub fn rejected_text(lang: &str, cat: &Catalog) -> String {
    if cat.rejected.is_empty() {
        return i18n::t(lang, "ver_none_rejected");
    }
    let lines: Vec<String> = cat.rejected.iter().map(|(tag, reason)| format!("{tag}: {reason}")).collect();
    format!("{}\n{}", i18n::t(lang, "ver_rejected"), lines.join("\n"))
}

/// The check's line and its kind: checking, the failed check (the list from the copies on this computer), the
/// signed versions counted, or the copies on this computer (no check in this session yet).
pub fn status(lang: &str, checking: bool, error: Option<&str>, cat: &Catalog) -> (String, &'static str) {
    if checking {
        return (i18n::t(lang, "ver_checking"), "info");
    }
    if let Some(e) = error {
        return (i18n::tf(lang, "ver_check_failed", e), "danger");
    }
    match cat.source.as_str() {
        "github" | "local" => (i18n::tf(lang, "ver_checked", &cat.firmware.len().to_string()), "success"),
        _ if !cat.firmware.is_empty() => (i18n::t(lang, "ver_cached"), "info"),
        _ => (String::new(), "info"),
    }
}

/// "Last check: 2026-11-24 10:02 UTC" from the list's own time or settings.json `last_check_utc` (the C# key).
pub fn last_check(lang: &str, utc: Option<&str>) -> String {
    match utc.filter(|u| !u.is_empty()) {
        Some(u) => i18n::tf(lang, "ver_last_check", &pretty_utc(u)),
        None => i18n::t(lang, "ver_never"),
    }
}

/// "2026-11-24T10:02:33Z" -> "2026-11-24 10:02 UTC"; anything else as it is.
pub fn pretty_utc(u: &str) -> String {
    let b = u.as_bytes();
    if u.len() >= 16 && b[10] == b'T' {
        format!("{} {} UTC", &u[..10], &u[11..16])
    } else {
        u.to_string()
    }
}

/// The page's contents from the state (also after a check, a language or channel change, a local release).
pub fn show(ui: &AppWindow) {
    let app = ui.global::<App>();
    let (lang, cat, installed, checking, error, saved_utc, releases) = with(|s| {
        (
            s.lang.clone(),
            s.catalog.clone().unwrap_or_default(),
            s.known.installed.clone(),
            s.checking,
            s.check_error_raw.clone(),
            s.settings.get_string("last_check_utc"),
            s.paths.releases(),
        )
    });
    let downloaded: Vec<String> =
        client::list_packages(&releases, None).into_iter().filter(Package::installable).map(|p| p.version).collect();
    let rows = rows(&lang, &cat, &installed, &downloaded);
    let old = app.get_ver_selected();
    if old >= rows.len() as i32 {
        app.set_ver_selected(-1);
    }
    app.set_ver_rows(ModelRc::new(VecModel::from(rows)));
    let (text, kind) = status(&lang, checking, error.as_deref(), &cat);
    app.set_ver_checking(checking);
    app.set_ver_status(text.into());
    app.set_ver_status_kind(kind.into());
    app.set_ver_last_check(last_check(&lang, cat.checked_utc.as_deref().or(saved_utc.as_deref())).into());
    app.set_ver_rejected(rejected_text(&lang, &cat).into());
    app.set_ver_key(i18n::tf(&lang, "ver_key", ddm32_core::releases::PUBLIC_KEY_ID).into());
    // above the list: Home's firmware line (the newest against what the radio runs) and the new program's notes
    let channel = app.get_channel();
    let notes = crate::newer_program(&cat, ddm32_core::app_info::VERSION, channel)
        .and_then(|a| a.entry.notes.as_ref().map(|n| n.for_lang(&lang).to_string()))
        .filter(|n| !n.is_empty())
        .map(|n| i18n::tf(&lang, "ver_new_program_notes", &n));
    let banner = [Some(app.get_update_text().to_string()).filter(|t| !t.is_empty()), notes]
        .into_iter()
        .flatten()
        .collect::<Vec<_>>()
        .join("\n");
    app.set_ver_banner(banner.into());
}

/// "Check for updates": the signed list asked for now (the button waits while it runs); the start-up question is not
/// asked from here.
pub fn check_now(ui: &AppWindow) {
    if with(|s| std::mem::replace(&mut s.checking, true)) {
        return;
    }
    show(ui);
    crate::start_catalog_check(ui.as_weak(), false);
}

/// "Switch to this version": the update wizard at that version (a version the channel does not offer is added to
/// the wizard's list for this time). A legacy version is only explained. Never a radio step.
pub fn switch(ui: &AppWindow) -> bool {
    let app = ui.global::<App>();
    let i = app.get_ver_selected();
    let Some(fv) = with(|s| s.catalog.as_ref().and_then(|c| c.firmware.get(i.max(0) as usize).cloned())) else {
        return false;
    };
    if i < 0 {
        return false;
    }
    if fv.is_legacy() {
        let lang = with(|s| s.lang.clone());
        app.set_ver_note(i18n::tf(&lang, "ver_legacy_note", fv.version()).into());
        app.set_ver_note_kind("warning".into());
        return false;
    }
    let version = fv.version().to_string();
    crate::open_wizard_pinned(ui, &version);
    true
}
