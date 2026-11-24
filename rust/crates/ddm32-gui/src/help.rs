// SPDX-License-Identifier: GPL-3.0-or-later
//! The offline help page (the C# InfoDialog.ShowHelp): the eight common situations, in the window's language, shown
//! in the program itself (no browser, no network). The full guide stays one click away on GitHub.

use crate::{i18n, with, App, AppWindow, HelpItem};
use slint::{ComponentHandle, ModelRc, VecModel};

/// The help's sections as (title, body) in a language; section 7 names the backups folder.
pub fn sections(lang: &str, backups: &str) -> Vec<(String, String)> {
    (1..=8)
        .map(|i| {
            let title = i18n::t(lang, &format!("help_{i}_t"));
            let key = format!("help_{i}_b");
            let body = if i == 7 { i18n::tf(lang, &key, backups) } else { i18n::t(lang, &key) };
            (title, body)
        })
        .collect()
}

/// Fills the help page in the window's language.
pub fn show(ui: &AppWindow) {
    let (lang, backups) = with(|s| (s.lang.clone(), s.paths.backups().display().to_string()));
    show_in(ui, &lang, &backups);
}

/// Fills the help page (also the screenshots; no state needed).
pub fn show_in(ui: &AppWindow, lang: &str, backups: &str) {
    let items: Vec<HelpItem> =
        sections(lang, backups).into_iter().map(|(t, b)| HelpItem { title: t.into(), body: b.into() }).collect();
    ui.global::<App>().set_help_items(ModelRc::new(VecModel::from(items)));
}
