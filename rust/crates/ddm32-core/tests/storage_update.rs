// SPDX-License-Identifier: GPL-3.0-or-later
#![allow(clippy::type_complexity, clippy::assertions_on_constants)]
//! The data folder per OS, the backups' fixed home and their automatic discovery, settings.json, and the self-update
//! swap and rollback with fake processes (the C# StorageTests, SelfUpdateTests, FlowTests' backup finder, plus the
//! owner's 2026-11-21 rules: no backup folder prompt in a release build, automatic discovery, updates and the
//! uninstaller never delete `backups`).
mod common;
use common::*;
use ddm32_core::app_info::{self, EXE_FILE};
use ddm32_core::radio::codec::Codec;
use ddm32_core::storage::backups::{self, backup_roots, discover, new_backup_folder, BACKUP_FOLDER_PROMPT};
use ddm32_core::storage::paths::{count_backups, unique_folder};
use ddm32_core::storage::{AppPaths, Os, Settings, Theme};
use ddm32_core::update::self_update::{self as su, Outcome, UpdateProcesses};
use std::path::{Path, PathBuf};

fn env_of(pairs: Vec<(&'static str, String)>) -> impl Fn(&str) -> Option<String> {
    move |n: &str| pairs.iter().find(|(k, _)| *k == n).map(|(_, v)| v.clone())
}

#[test]
fn old_turkish_folders_move_to_the_english_names() {
    let t = tmp();
    let root = t.path().join("Programs").join("DDM32 Plus");
    make_backup(&root.join("yedekler").join("dm32-20261121"), true);
    std::fs::create_dir_all(root.join("surumler").join("1.0.0-beta.4")).unwrap();
    std::fs::create_dir_all(root.join("paketler")).unwrap();
    std::fs::write(root.join("paketler").join("user.csv"), "x").unwrap();
    let p = AppPaths::new(&root);
    p.ensure().unwrap();
    assert!(!root.join("yedekler").exists() && !root.join("surumler").exists());
    assert!(p.backups().join("dm32-20261121").join("backup-manifest.json").is_file());
    assert!(p.releases().join("1.0.0-beta.4").is_dir());
    assert_eq!("x", std::fs::read_to_string(p.packs().join("user.csv")).unwrap());
    assert_eq!(vec![p.backups()], p.own_backup_roots());
    // both names present: the old one stays and is still searched
    make_backup(&root.join("yedekler").join("dm32-20261120"), true);
    p.ensure().unwrap();
    assert_eq!(vec![p.backups(), root.join("yedekler")], p.own_backup_roots());
    assert!(app_info::RESERVED_NAMES.contains(&"yedekler") && app_info::RESERVED_NAMES.contains(&"backups"));
}

#[test]
fn the_data_folder_per_os() {
    let t = tmp();
    let exe = t.path().join("Programs").join("DDM 32 Plus");
    let none = |_: &str| None;
    // Windows: next to the program, as the C# program
    let p = AppPaths::detect(&exe, Os::Windows, &none).unwrap();
    p.ensure().unwrap();
    assert_eq!(exe.join("backups"), p.backups());
    assert_eq!(exe.join("settings.json"), p.settings_file());
    assert!(p.backups().is_dir() && p.releases().is_dir() && p.updates().is_dir());
    // Linux: XDG data dir, else ~/.local/share
    let home = t.path().join("home");
    let e = env_of(vec![("HOME", home.to_string_lossy().into())]);
    assert_eq!(home.join(".local/share/DDM32Plus"), AppPaths::detect(&exe, Os::Linux, &e).unwrap().root);
    let e = env_of(vec![
        ("HOME", home.to_string_lossy().into()),
        ("XDG_DATA_HOME", t.path().join("xdg").to_string_lossy().into()),
    ]);
    assert_eq!(t.path().join("xdg").join("DDM32Plus"), AppPaths::detect(&exe, Os::Linux, &e).unwrap().root);
    // macOS: ~/Library/Application Support
    let e = env_of(vec![("HOME", home.to_string_lossy().into())]);
    assert_eq!(home.join("Library/Application Support/DDM32Plus"), AppPaths::detect(&exe, Os::MacOs, &e).unwrap().root);
    // DDM32_DATA overrides (tests, screenshots)
    let e = env_of(vec![("DDM32_DATA", t.path().join("data").to_string_lossy().into())]);
    assert_eq!(t.path().join("data"), AppPaths::detect(&exe, Os::Windows, &e).unwrap().root);
}

fn make_backup(folder: &Path, verified: bool) {
    std::fs::create_dir_all(folder).unwrap();
    std::fs::write(folder.join("backup-manifest.json"), if verified { "{\"backup_verified\": true}" } else { "{}" })
        .unwrap();
}

fn tree(dir: &Path) -> Vec<PathBuf> {
    let mut out = vec![];
    fn walk(d: &Path, out: &mut Vec<PathBuf>) {
        if let Ok(rd) = std::fs::read_dir(d) {
            for e in rd.flatten() {
                out.push(e.path());
                if e.path().is_dir() {
                    walk(&e.path(), out);
                }
            }
        }
    }
    walk(dir, &mut out);
    out.sort();
    out
}

#[test]
fn the_older_programs_backup_folders_are_found_and_left_alone() {
    let t = tmp();
    let local = t.path().join("Local");
    make_backup(&local.join("DDM25Plus/yedekler/dm32-20261113"), true);
    make_backup(&local.join("DDM25Plus/yedekler/dm32-20261114"), false);
    make_backup(&local.join("DDM32Plus/yedekler/dm32-20261120"), true);
    std::fs::create_dir_all(local.join("DDM32Plus/yedekler/not-a-backup")).unwrap();
    // the C# program's install folder, when this program lives elsewhere (a portable copy)
    make_backup(&local.join("Programs/DDM32 Plus/yedekler/dm32-20261121"), true);
    let before = tree(&local);
    let e = env_of(vec![("LOCALAPPDATA", local.to_string_lossy().into())]);
    let p = AppPaths::new(t.path().join("portable"));
    let found = p.find_other_data(Os::Windows, &e);
    assert_eq!(3, found.len());
    let d32 = found.iter().find(|f| f.folder.ends_with("DDM32Plus")).unwrap();
    assert_eq!((1, 1, "python"), (d32.backup_folders, d32.verified_backups, d32.program));
    let d25 = found.iter().find(|f| f.folder.ends_with("DDM25Plus")).unwrap();
    assert_eq!((2, 1), (d25.backup_folders, d25.verified_backups));
    assert_eq!("net", found.iter().find(|f| f.folder.ends_with("DDM32 Plus")).unwrap().program);
    assert_eq!(before, tree(&local), "found folders are read only");
    // the program's own folder (installed over the C# program) is never reported
    let own = AppPaths::new(local.join("Programs/DDM32 Plus"));
    assert_eq!(2, own.find_other_data(Os::Windows, &e).len());
    // only on Windows (the older programs ran there only)
    assert!(p.find_other_data(Os::Linux, &e).is_empty());
    assert!(p.find_other_data(Os::Windows, &|_| None).is_empty());
}

#[test]
fn no_backup_folder_prompt_in_a_release_build() {
    // the release configuration (the tests) has no folder choice at all: every backup goes to the own backups
    assert!(!BACKUP_FOLDER_PROMPT);
    let t = tmp();
    let p = AppPaths::new(t.path());
    p.ensure().unwrap();
    let f = new_backup_folder(&p, "20261121");
    assert_eq!(p.backups().join("dm32-20261121"), f);
    assert!(backups::inside_own_backups(&p, &f));
    std::fs::create_dir_all(&f).unwrap();
    assert_eq!(p.backups().join("dm32-20261121-b"), new_backup_folder(&p, "20261121"));
    // a folder remembered in the settings (by a developer build) is ignored by a release build
    let s = Settings::new(p.settings_file());
    let chosen = t.path().join("elsewhere");
    make_backup(&chosen.join("dm32-1"), true);
    s.remember_backup_folder(&chosen).unwrap();
    let roots = backup_roots(&p, &s, Os::Linux, &|_| None);
    assert_eq!(1, roots.len());
    assert_eq!(p.backups(), roots[0].folder);
    assert!(roots[0].writable());
}

#[test]
fn backups_are_discovered_automatically_newest_first() {
    let t = tmp();
    let p = AppPaths::new(t.path().join("app"));
    p.ensure().unwrap();
    let local = t.path().join("Local");
    make_backup(&p.backups().join("dm32-20261120"), true);
    std::thread::sleep(std::time::Duration::from_millis(30));
    make_backup(&local.join("DDM32Plus/yedekler/dm32-20261113"), true);
    std::thread::sleep(std::time::Duration::from_millis(30));
    make_backup(&p.backups().join("dm32-20261121"), true);
    make_backup(&p.backups().join("broken"), false);
    let e = env_of(vec![("LOCALAPPDATA", local.to_string_lossy().into())]);
    let s = Settings::new(p.settings_file());
    let roots = backup_roots(&p, &s, Os::Windows, &e);
    assert_eq!(vec!["own", "python"], roots.iter().map(|r| r.origin).collect::<Vec<_>>());
    assert!(!roots[1].writable());
    let found = discover(&roots);
    let names: Vec<String> =
        found.iter().map(|b| b.folder.file_name().unwrap().to_string_lossy().into_owned()).collect();
    assert_eq!(vec!["dm32-20261121", "dm32-20261113", "dm32-20261120"], names);
    assert_eq!("python", found[1].origin);
    assert_eq!((3, 2), count_backups(&p.backups()));
}

#[test]
fn the_quick_update_finds_the_last_ddm32_backup_by_itself() {
    let r = RadioInputs::new();
    let t = tmp();
    let own = t.path().join("backups");
    let python = t.path().join("python-yedekler");
    std::fs::create_dir_all(&own).unwrap();
    assert_eq!(None, backups::last_for_quick_update(&[own.clone(), python.clone()], Codec::dm32(), 8));
    // a stock backup is not offered; the DDM32 Plus backup is
    write_backup(&own.join("dm32-20261113"), &r.stock, &r.data, true);
    let taken = python.join("dm32-20261120");
    write_backup(&taken, &r.program, &r.data, true);
    std::fs::create_dir_all(python.join("not-a-backup")).unwrap();
    assert_eq!(
        Some(taken),
        backups::last_for_quick_update(
            &[own.clone(), python.clone(), python.clone(), t.path().join("missing")],
            Codec::dm32(),
            8
        )
    );
    assert_eq!(2, backups::candidates(&[own, python]).len());
}

#[test]
fn updates_and_the_uninstaller_never_delete_the_backups() {
    assert!(app_info::RESERVED_NAMES.contains(&app_info::BACKUPS_FOLDER));
    assert!(app_info::RESERVED_NAMES.contains(&app_info::PACKS_FOLDER));
    let t = tmp();
    let p = AppPaths::new(t.path());
    assert!(!p.removable_on_uninstall().contains(&p.backups()));
    // the update cleanup touches only its own folder
    make_backup(&p.backups().join("dm32-1"), true);
    std::fs::create_dir_all(p.updates().join("app-1.0.0")).unwrap();
    su::cleanup(&p.updates(), None);
    assert!(p.backups().join("dm32-1/backup-manifest.json").is_file());
    // the Windows setup marks backups uninsneveruninstall and removes only program folders
    let iss =
        std::fs::read_to_string(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../packaging/windows/DDM32Plus.iss"))
            .unwrap();
    assert!(iss.contains(r#"Name: "{app}\backups"; Flags: uninsneveruninstall"#));
    let del = &iss[iss.find("[UninstallDelete]").unwrap()..iss.find("[Code]").unwrap()];
    assert!(!del.lines().any(|l| !l.trim_start().starts_with(';') && l.contains("backups")));
}

#[test]
fn settings_round_trip_and_damage() {
    let t = tmp();
    let s = Settings::new(t.path().join("settings.json"));
    assert_eq!(None, s.language());
    assert!(s.auto_check());
    assert_eq!(Theme::Light, s.theme(), "light is the default theme");
    s.set_language("en").unwrap();
    s.set_auto_check(false).unwrap();
    s.set_theme(Theme::Dark).unwrap();
    assert_eq!(Some("en".into()), s.language());
    assert!(!s.auto_check());
    assert_eq!(Theme::Dark, s.theme());
    assert!(!t.path().join("settings.tmp").exists());
    assert!(!s.disclaimer_accepted("v1"));
    s.accept_disclaimer("v1", "2026-11-20T00:00:00Z").unwrap();
    assert!(s.disclaimer_accepted("v1"));
    assert!(!s.disclaimer_accepted("v2"));
    s.withdraw_disclaimer("v1", "2026-11-20T00:00:01Z").unwrap();
    assert!(!s.disclaimer_accepted("v1"));
    std::fs::write(&s.file_path, "{ damaged").unwrap();
    assert_eq!(None, s.language());
    s.set_language("tr").unwrap();
    assert_eq!(Some("tr".into()), s.language());
    s.save("language", serde_json::json!("xx")).unwrap();
    assert_eq!(None, s.language());
}

/// The release channel: this program's `channel`, else the C# program's `include_beta` (default true when the file is
/// a C# one), else stable only; choosing writes both keys.
#[test]
fn channel_takes_over_include_beta() {
    let t = tmp();
    let path = t.path().join("settings.json");
    let s = Settings::new(&path);
    let with = |text: &str| {
        std::fs::write(&path, text).unwrap();
        s.channel()
    };
    assert_eq!(0, s.channel(), "no file: a fresh install");
    assert_eq!(0, with(r#"{"language": "tr", "theme": "dark", "port": "COM14"}"#), "this program's keys only");
    // this program writes the C# install record too: it is not a sign of a C# file
    assert_eq!(0, with(r#"{"language": "en", "installed_sha256": "ab", "installed_firmware": "DDM32 Plus 1.0.1"}"#));
    assert_eq!(1, with(r#"{"include_beta": true}"#));
    assert_eq!(0, with(r#"{"include_beta": false, "window": {}}"#));
    for k in ddm32_core::storage::settings::NET_ONLY_KEYS {
        assert_eq!(1, with(&format!(r#"{{"language": "en", "{k}": 1}}"#)), "{k}: a C# file, include_beta default");
    }
    assert_eq!(0, with(r#"{"include_beta": true, "channel": 0}"#), "the own choice wins");
    assert_eq!(2, with(r#"{"channel": 7}"#), "clamped");
    std::fs::write(&path, r#"{"language": "en", "disclaimer": {"version": "x", "accepted": true}}"#).unwrap();
    for (c, beta) in [(1, true), (0, false), (2, true)] {
        s.set_channel(c).unwrap();
        assert_eq!(c, s.channel());
        assert_eq!(beta, s.include_beta());
        assert_eq!(Some("en".into()), s.language());
        assert!(s.disclaimer_accepted("x"));
    }
}

/// A fresh install decides its channel once (Latest) and keeps it after its first install record.
#[test]
fn the_channel_is_settled_once() {
    let t = tmp();
    let path = t.path().join("settings.json");
    let s = Settings::new(&path);
    s.set_language("en").unwrap();
    assert_eq!(0, s.settle_channel().unwrap());
    assert_eq!(Some(0), s.load().get("channel").and_then(|v| v.as_i64()), "saved at the first start");
    // the first install writes the C# keys (and any key a C# program might write later)
    s.save("installed_sha256", serde_json::json!("ab")).unwrap();
    s.save("installed_firmware", serde_json::json!("DDM32 Plus 1.0.1")).unwrap();
    s.save("window", serde_json::json!({"x": 1})).unwrap();
    assert_eq!(0, s.channel());
    assert_eq!(0, s.settle_channel().unwrap());
    // a C# file without `channel`: decided from it once, then kept
    std::fs::write(&path, r#"{"language": "en", "window": {}}"#).unwrap();
    assert_eq!(1, s.settle_channel().unwrap());
    s.save("include_beta", serde_json::json!(false)).unwrap();
    assert_eq!(1, s.channel(), "settled: a later include_beta does not change it");
}

/// A settings.json that is there but cannot be read is never rewritten by the start-up (byte for byte).
#[test]
fn settling_never_overwrites_an_unreadable_file() {
    let t = tmp();
    let path = t.path().join("settings.json");
    let s = Settings::new(&path);
    for bytes in [&b"{ \"language\": \"tr\", trunc"[..], &[0xFF, 0xFE, 0x00, 0x7B][..], &b"{}"[..], &b""[..]] {
        std::fs::write(&path, bytes).unwrap();
        assert_eq!(0, s.settle_channel().unwrap());
        assert_eq!(bytes, std::fs::read(&path).unwrap().as_slice());
    }
}

/// A C# settings file keeps `port` without `port_auto`: the user's last chosen port, a fixed choice here.
#[test]
fn the_csharp_port_is_the_first_fixed_choice() {
    let t = tmp();
    let path = t.path().join("settings.json");
    let s = Settings::new(&path);
    std::fs::write(&path, r#"{"port": "COM14"}"#).unwrap();
    let p = s.port_pref();
    assert!(!p.auto);
    assert_eq!(Some("COM14".to_string()), p.port);
    std::fs::write(&path, r#"{"port": "COM14", "port_auto": true}"#).unwrap();
    assert!(s.port_pref().auto, "this program's own choice wins");
    std::fs::write(&path, r#"{}"#).unwrap();
    assert!(s.port_pref().auto);
}

/// A backup's age: the manifest's completed_utc (else started_utc), not the folder's time (copying changes that).
#[test]
fn backup_age_comes_from_its_manifest() {
    use std::time::{Duration, SystemTime};
    let t = tmp();
    let b = t.path().join("dm32-old");
    std::fs::create_dir_all(&b).unwrap();
    let at = |s: i64| SystemTime::UNIX_EPOCH + Duration::from_secs(s as u64);
    std::fs::write(
        b.join("backup-manifest.json"),
        r#"{"backup_verified": true, "started_utc": "2026-11-13T10:00:00Z", "completed_utc": "2026-11-13T10:05:00Z"}"#,
    )
    .unwrap();
    assert_eq!(at(1_794_564_300), backups::backup_time(&b));
    std::fs::write(
        b.join("backup-manifest.json"),
        r#"{"backup_verified": true, "started_utc": "2026-11-13T10:00:00+00:00"}"#,
    )
    .unwrap();
    assert_eq!(at(1_794_564_000), backups::backup_time(&b));
    // none (or damaged): the folder's time
    std::fs::write(b.join("backup-manifest.json"), r#"{"backup_verified": true, "completed_utc": "yesterday"}"#)
        .unwrap();
    let folder = std::fs::metadata(&b).unwrap().modified().unwrap();
    assert_eq!(folder, backups::backup_time(&b));
    // discover orders by it: an old backup copied today stays older than a newer one
    let n = t.path().join("dm32-new");
    std::fs::create_dir_all(&n).unwrap();
    std::fs::write(
        n.join("backup-manifest.json"),
        r#"{"backup_verified": true, "completed_utc": "2026-11-20T09:00:00Z"}"#,
    )
    .unwrap();
    std::fs::write(
        b.join("backup-manifest.json"),
        r#"{"backup_verified": true, "completed_utc": "2026-11-13T10:05:00Z"}"#,
    )
    .unwrap();
    let found = discover(&[backups::BackupRoot { folder: t.path().to_path_buf(), origin: "own" }]);
    assert_eq!(vec![n, b], found.into_iter().map(|f| f.folder).collect::<Vec<_>>());
}

#[test]
fn unknown_settings_keys_are_kept() {
    let t = tmp();
    let path = t.path().join("settings.json");
    std::fs::write(&path, "{\"window_geometry\": \"abc\", \"port\": \"COM14\"}").unwrap();
    let s = Settings::new(&path);
    s.set_language("tr").unwrap();
    assert_eq!(Some("COM14".into()), s.get_string("port"));
    assert_eq!(Some("abc".into()), s.get_string("window_geometry"));
}

#[test]
fn unique_folder_letters() {
    let t = tmp();
    assert_eq!(t.path().join("x"), unique_folder(t.path(), "x"));
    std::fs::create_dir(t.path().join("x")).unwrap();
    assert_eq!(t.path().join("x-b"), unique_folder(t.path(), "x"));
}

// --- the self-update swap ------------------------------------------------------------------------------------------

struct Up {
    t: tempfile::TempDir,
    app: PathBuf,
    staging: PathBuf,
    marker: PathBuf,
}

fn make_app(folder: &Path, tag: &str) -> PathBuf {
    std::fs::create_dir_all(folder).unwrap();
    std::fs::write(folder.join(EXE_FILE), format!("exe {tag}")).unwrap();
    std::fs::write(folder.join("lib.dll"), format!("lib {tag}")).unwrap();
    folder.to_path_buf()
}

impl Up {
    fn new() -> Up {
        let t = tmp();
        let app = make_app(&t.path().join("Programs").join("DDM32 Plus"), "old");
        let staging = make_app(&app.join("updates/app-9.9.9/DDM32Plus"), "new");
        let marker = app.join("updates/started.json");
        std::fs::create_dir_all(app.join("backups/dm32-20261120")).unwrap();
        std::fs::write(app.join("backups/dm32-20261120/backup-manifest.json"), "{}").unwrap();
        std::fs::write(app.join("settings.json"), "{\"language\": \"tr\"}").unwrap();
        Up { t, app, staging, marker }
    }
    fn read(&self, rel: &str) -> String {
        std::fs::read_to_string(self.app.join(rel)).unwrap()
    }
    fn user_data_untouched(&self) {
        assert_eq!("{}", self.read("backups/dm32-20261120/backup-manifest.json"));
        assert_eq!("{\"language\": \"tr\"}", self.read("settings.json"));
    }
    fn swap(&self, p: &mut Procs) -> Outcome {
        su::swap(&self.app, &self.staging, p, &self.marker, &mut |_| {})
    }
}

#[derive(Default)]
struct Procs {
    no_start: bool,
    not_exited: bool,
    started: Vec<(PathBuf, Vec<String>, String)>,
    stopped: Vec<Option<u32>>,
}

impl UpdateProcesses for Procs {
    fn start(&mut self, exe: &Path, args: &[String]) -> Option<u32> {
        self.started.push((exe.to_path_buf(), args.to_vec(), std::fs::read_to_string(exe).unwrap_or_default()));
        if let Some(i) = args.iter().position(|a| a == su::CHECK_ARG) {
            if !self.no_start {
                su::write_marker(Path::new(&args[i + 1]), "9.9.9").unwrap();
            }
        }
        Some(self.started.len() as u32)
    }
    fn wait_exit(&mut self) -> bool {
        !self.not_exited
    }
    fn wait_marker(&mut self, m: &Path) -> bool {
        m.is_file()
    }
    fn stop(&mut self, h: Option<u32>) {
        self.stopped.push(h);
    }
}

#[test]
fn update_ok_keeps_old_for_rollback() {
    let u = Up::new();
    let mut p = Procs::default();
    assert_eq!(Outcome::Ok, u.swap(&mut p));
    assert_eq!("exe new", u.read(EXE_FILE));
    assert_eq!("lib new", u.read("lib.dll"));
    assert_eq!("exe old", u.read(&format!("updates/rollback/{EXE_FILE}")));
    let last = p.started.last().unwrap();
    assert_eq!(vec![su::CHECK_ARG.to_string(), u.marker.to_string_lossy().into_owned()], last.1);
    assert_eq!("exe new", last.2);
    u.user_data_untouched();
}

#[test]
fn update_rollback_when_new_does_not_start() {
    let u = Up::new();
    let mut p = Procs { no_start: true, ..Default::default() };
    assert_eq!(Outcome::RolledBack, u.swap(&mut p));
    assert_eq!("exe old", u.read(EXE_FILE));
    assert_eq!("lib old", u.read("lib.dll"));
    assert_eq!("exe new", u.read(&format!("updates/failed/{EXE_FILE}")));
    assert_eq!(vec![Some(1)], p.stopped);
    assert_eq!("exe old", p.started.last().unwrap().2);
    assert!(p.started.last().unwrap().1.is_empty());
    u.user_data_untouched();
}

#[test]
fn update_new_files_are_removed_on_rollback() {
    let u = Up::new();
    std::fs::write(u.staging.join("extra.dll"), "new only").unwrap();
    assert_eq!(Outcome::RolledBack, u.swap(&mut Procs { no_start: true, ..Default::default() }));
    assert!(!u.app.join("extra.dll").exists());
    assert_eq!("exe old", u.read(EXE_FILE));
}

#[test]
fn update_abort_when_old_program_keeps_running() {
    let u = Up::new();
    let mut p = Procs { not_exited: true, ..Default::default() };
    assert_eq!(Outcome::Aborted, u.swap(&mut p));
    assert_eq!("exe old", u.read(EXE_FILE));
    assert!(p.started.is_empty());
    assert!(!u.app.join("updates/rollback").exists());
}

/// Windows only: a file another program holds open (no sharing) cannot be moved or read.
#[cfg(windows)]
fn locked(path: &Path, write: bool) -> std::fs::File {
    use std::os::windows::fs::OpenOptionsExt;
    std::fs::OpenOptions::new().read(true).write(write).share_mode(0).open(path).unwrap()
}

#[cfg(windows)]
#[test]
fn update_cannot_move_files_aside() {
    let u = Up::new();
    let mut p = Procs::default();
    {
        let _l = locked(&u.app.join("lib.dll"), false);
        assert_eq!(Outcome::Aborted, u.swap(&mut p));
    }
    assert_eq!("exe old", u.read(EXE_FILE));
    assert_eq!("lib old", u.read("lib.dll"));
    assert_eq!("exe old", p.started.last().unwrap().2);
    u.user_data_untouched();
}

#[cfg(windows)]
#[test]
fn update_copy_failure_puts_old_back() {
    let u = Up::new();
    let mut p = Procs::default();
    {
        let _l = locked(&u.staging.join("lib.dll"), true);
        assert_eq!(Outcome::RolledBack, u.swap(&mut p));
    }
    assert_eq!("exe old", u.read(EXE_FILE));
    assert_eq!("lib old", u.read("lib.dll"));
    assert_eq!("exe old", p.started.last().unwrap().2);
    u.user_data_untouched();
}

#[test]
fn update_previous_rollback_copy_is_replaced() {
    let u = Up::new();
    make_app(&u.app.join("updates/rollback"), "older");
    assert_eq!(Outcome::Ok, u.swap(&mut Procs::default()));
    assert_eq!("exe old", u.read(&format!("updates/rollback/{EXE_FILE}")));
}

#[test]
fn update_staging_without_exe() {
    let u = Up::new();
    std::fs::remove_file(u.staging.join(EXE_FILE)).unwrap();
    assert_eq!(Outcome::Aborted, u.swap(&mut Procs::default()));
    assert_eq!("exe old", u.read(EXE_FILE));
}

#[test]
fn update_staging_that_would_touch_user_data_is_refused() {
    for name in ["yedekler", "settings.json", "YEDEKLER"] {
        let u = Up::new();
        if name.ends_with(".json") {
            std::fs::write(u.staging.join(name), "{}").unwrap();
        } else {
            std::fs::create_dir_all(u.staging.join(name).join("x")).unwrap();
            std::fs::write(u.staging.join(name).join("x/backup-manifest.json"), "evil").unwrap();
        }
        let mut p = Procs::default();
        assert_eq!(Outcome::Aborted, u.swap(&mut p), "{name}");
        assert!(p.started.is_empty());
        assert_eq!("exe old", u.read(EXE_FILE));
        u.user_data_untouched();
    }
}

#[test]
fn update_helper_main_writes_result_shown_once() {
    let u = Up::new();
    let result = u.app.join("updates/last-update.json");
    let s = |p: &Path| p.to_string_lossy().into_owned();
    let args: Vec<String> = vec![
        "--pid".into(),
        "123".into(),
        "--app-dir".into(),
        s(&u.app),
        "--staging".into(),
        s(&u.staging),
        "--marker".into(),
        s(&u.marker),
        "--result".into(),
        s(&result),
    ];
    let code = su::helper_main(&args, &mut |pid| {
        assert_eq!(123, pid);
        Box::new(Procs::default())
    });
    assert_eq!(0, code);
    let v: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&result).unwrap()).unwrap();
    assert_eq!("ok", v["result"]);
    assert_eq!(Some("ok".into()), su::read_result(&u.app.join("updates")));
    assert_eq!(None, su::read_result(&u.app.join("updates")));
    assert_eq!(2, su::helper_main(&["--pid".into(), "x".into()], &mut |_| Box::new(Procs::default())));
    drop(u.t);
}

#[test]
fn update_helper_command_starts_the_staged_program() {
    let u = Up::new();
    let (exe, args) = su::helper_command(&u.staging, 42, &u.app, &u.marker, Path::new("r.json"));
    assert_eq!(u.staging.join(EXE_FILE), exe);
    let s = |p: &Path| p.to_string_lossy().into_owned();
    assert_eq!(
        vec![
            su::HELPER_ARG.to_string(),
            "--pid".into(),
            "42".into(),
            "--app-dir".into(),
            s(&u.app),
            "--staging".into(),
            s(&u.staging),
            "--marker".into(),
            s(&u.marker),
            "--result".into(),
            "r.json".into()
        ],
        args
    );
}

fn zip_file(t: &Path, names: &[&str]) -> PathBuf {
    static N: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    let z = t.join(format!("app-{}.zip", N.fetch_add(1, std::sync::atomic::Ordering::SeqCst)));
    let members: Vec<(&str, &[u8])> = names.iter().map(|n| (*n, b"x".as_slice())).collect();
    std::fs::write(&z, zip_of(&members)).unwrap();
    z
}

#[test]
fn update_good_zip_stages() {
    let t = tmp();
    let exe = format!("DDM32Plus/{EXE_FILE}");
    let staged =
        su::stage(&zip_file(t.path(), &[&exe, "DDM32Plus/README.txt"]), &t.path().join("updates"), "3.1.0").unwrap();
    assert!(staged.join(EXE_FILE).is_file());
    assert_eq!("app-3.1.0", staged.parent().unwrap().file_name().unwrap());
}

#[test]
fn update_unsafe_zips_are_refused() {
    let exe = format!("DDM32Plus/{EXE_FILE}");
    let cases: Vec<Vec<&str>> = vec![
        vec![&exe, "../evil.exe"],
        vec![&exe, "/abs.txt"],
        vec!["Other/DDM32Plus.exe"],
        vec!["DDM32Plus/../../x"],
        vec!["DDM32Plus/readme.txt"],
        vec!["DDM32Plus/C:evil"],
        vec![&exe, "DDM32Plus\\..\\evil.exe"],
        vec![&exe, "DDM32Plus/yedekler/x/backup-manifest.json"],
        vec![&exe, "DDM32Plus/settings.json"],
    ];
    for names in cases {
        let t = tmp();
        let updates = t.path().join("updates");
        assert!(su::stage(&zip_file(t.path(), &names), &updates, "3.1.0").is_err(), "{names:?}");
        assert!(!updates.join("app-3.1.0").exists());
        assert!(!t.path().join("evil.exe").exists());
    }
}

#[test]
fn update_cleanup_keeps_running_copy_and_rollback() {
    let t = tmp();
    let u = t.path().join("u");
    std::fs::create_dir_all(u.join("app-1.0.0/DDM32Plus")).unwrap();
    std::fs::create_dir_all(u.join("app-2.0.0/DDM32Plus")).unwrap();
    std::fs::create_dir_all(u.join("rollback")).unwrap();
    std::fs::write(u.join("x.zip"), "z").unwrap();
    su::cleanup(&u, Some(&u.join("app-2.0.0/DDM32Plus")));
    assert!(!u.join("app-1.0.0").exists());
    assert!(u.join("app-2.0.0").exists());
    assert!(u.join("rollback").exists());
    assert!(!u.join("x.zip").exists());
}
