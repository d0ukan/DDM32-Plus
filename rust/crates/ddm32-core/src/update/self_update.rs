// SPDX-License-Identifier: GPL-3.0-or-later
//! Self-update of the installed program (Update/SelfUpdate.cs, the same protocol, so that the C# program's helper
//! can hand over to this program and back):
//!
//! 1. The program downloads the new program zip named in a signed manifest and checks its size and SHA-256
//!    (`releases::client::download_app`), then extracts it safely into updates/app-VERSION/DDM32Plus/ ([`stage`]).
//!    Nothing in the install folder changes yet.
//! 2. It starts the NEW program from that staging folder as the helper
//!    `DDM32Plus --self-update --pid OLD --app-dir INSTALL --staging STAGING --marker FILE --result FILE`
//!    and closes itself.
//! 3. The helper ([`swap`]) waits for the old program to exit and replaces ONLY the files the new program brings:
//!    each one that exists is moved to updates/rollback/ first. The user's data (backups, settings.json,
//!    releases, updates, packs) is never moved, copied over or deleted: a zip that would touch those names is
//!    refused. Then it starts the new program with --update-check FILE; the new program writes the marker once its
//!    window is up.
//! 4. No marker in time: the helper stops the new program, moves its files to updates/failed/, puts the old files
//!    back and starts the old program. An error before the swap leaves everything as it was.
//!
//! In stage 1 nothing here is started by the program itself; the tests drive it with fake processes.

use crate::app_info::{self, EXE_FILE, EXE_NAME, RESERVED_NAMES};
use std::fmt;
use std::path::{Path, PathBuf};
use std::time::Duration;

pub const HELPER_ARG: &str = "--self-update";
pub const CHECK_ARG: &str = "--update-check";
pub const RESULT_FILE: &str = "last-update.json";
pub const ROLLBACK_FOLDER: &str = "rollback";
pub const FAILED_FOLDER: &str = "failed";
pub const START_TIMEOUT: Duration = Duration::from_secs(90);
pub const EXIT_TIMEOUT: Duration = Duration::from_secs(60);

/// A program zip that may not be used, or a failed staging step.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UpdateError(pub String);

impl fmt::Display for UpdateError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}
impl std::error::Error for UpdateError {}

/// What the helper needs from the operating system; the tests pass fakes (nothing is started).
pub trait UpdateProcesses {
    /// Starts exe with args; returns a handle for `stop`.
    fn start(&mut self, exe: &Path, args: &[String]) -> Option<u32>;
    /// True once the old program has exited.
    fn wait_exit(&mut self) -> bool;
    /// True when the new program wrote the marker in time.
    fn wait_marker(&mut self, marker: &Path) -> bool;
    fn stop(&mut self, handle: Option<u32>);
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    Ok,
    RolledBack,
    Aborted,
}

pub fn result_name(o: Outcome) -> &'static str {
    match o {
        Outcome::Ok => "ok",
        Outcome::RolledBack => "rolled_back",
        Outcome::Aborted => "aborted",
    }
}

fn is_reserved(top: &str) -> bool {
    RESERVED_NAMES.iter().any(|r| r.eq_ignore_ascii_case(top))
}

fn unsafe_path(name: &str) -> UpdateError {
    UpdateError(format!("Unsafe or unexpected path in the program zip: '{name}'"))
}

/// Extracts a program zip whose members all sit under DDM32Plus/ (no absolute paths, no '..', no backslash, no
/// drive or stream colon, none of the user's data names); returns the extracted DDM32Plus folder.
pub fn safe_extract(zip_path: &Path, dest: &Path) -> Result<PathBuf, UpdateError> {
    let io = |e: std::io::Error| UpdateError(format!("IOException: {e}"));
    let file = std::fs::File::open(zip_path).map_err(io)?;
    let mut z = zip::ZipArchive::new(file).map_err(|e| UpdateError(format!("Not a program zip: {e}")))?;
    let names: Vec<String> = (0..z.len())
        .map(|i| z.by_index_raw(i).map(|f| f.name().to_string()))
        .collect::<Result<_, _>>()
        .map_err(|e| UpdateError(format!("Not a program zip: {e}")))?;
    for name in &names {
        // PurePosixPath(name).parts: empty and '.' parts drop out
        let parts: Vec<&str> = name.split('/').filter(|p| !p.is_empty() && *p != ".").collect();
        if parts.is_empty()
            || parts[0] != EXE_NAME
            || name.starts_with('/')
            || name.contains('\\')
            || parts.iter().any(|p| *p == ".." || p.contains(':'))
            || (parts.len() > 1 && is_reserved(parts[1]))
        {
            return Err(unsafe_path(name));
        }
    }
    std::fs::create_dir_all(dest).map_err(io)?;
    let root = std::path::absolute(dest).map_err(io)?;
    for (i, name) in names.iter().enumerate() {
        let target = root.join(name);
        let norm = normalize(&target);
        if !norm.starts_with(&root) {
            return Err(unsafe_path(name));
        }
        if name.ends_with('/') {
            std::fs::create_dir_all(&norm).map_err(io)?;
            continue;
        }
        if let Some(parent) = norm.parent() {
            std::fs::create_dir_all(parent).map_err(io)?;
        }
        let mut f = z.by_index(i).map_err(|e| UpdateError(format!("Not a program zip: {e}")))?;
        let mut out = std::fs::File::create(&norm).map_err(io)?;
        std::io::copy(&mut f, &mut out).map_err(io)?;
    }
    let app = dest.join(EXE_NAME);
    if !app.join(EXE_FILE).is_file() {
        return Err(UpdateError(format!("{EXE_FILE} missing in the program zip")));
    }
    Ok(app)
}

/// The path with `.` and `..` resolved lexically.
fn normalize(p: &Path) -> PathBuf {
    let mut out = PathBuf::new();
    for c in p.components() {
        match c {
            std::path::Component::ParentDir => {
                out.pop();
            }
            std::path::Component::CurDir => {}
            other => out.push(other),
        }
    }
    out
}

/// The new program, extracted into updates_dir/app-VERSION/DDM32Plus.
pub fn stage(zip_path: &Path, updates_dir: &Path, version: &str) -> Result<PathBuf, UpdateError> {
    let target = updates_dir.join(format!("app-{version}"));
    if target.is_dir() {
        std::fs::remove_dir_all(&target).map_err(|e| UpdateError(format!("IOException: {e}")))?;
    }
    let r = safe_extract(zip_path, &target);
    if r.is_err() {
        try_delete(&target);
    }
    r
}

/// The command that starts the staged program as the helper.
pub fn helper_command(
    staged_app: &Path,
    pid: u32,
    app_dir: &Path,
    marker: &Path,
    result: &Path,
) -> (PathBuf, Vec<String>) {
    let s = |p: &Path| p.to_string_lossy().into_owned();
    (
        staged_app.join(EXE_FILE),
        vec![
            HELPER_ARG.into(),
            "--pid".into(),
            pid.to_string(),
            "--app-dir".into(),
            s(app_dir),
            "--staging".into(),
            s(staged_app),
            "--marker".into(),
            s(marker),
            "--result".into(),
            s(result),
        ],
    )
}

pub fn write_marker(path: &Path, version: &str) -> std::io::Result<()> {
    if let Some(d) = std::path::absolute(path)?.parent() {
        std::fs::create_dir_all(d)?;
    }
    let v = serde_json::json!({"started": true, "version": version, "pid": std::process::id()});
    std::fs::write(path, v.to_string())
}

/// Relative paths of every file in the staged program, sorted case-insensitively.
fn staged_files(staging: &Path) -> Vec<PathBuf> {
    fn walk(base: &Path, dir: &Path, out: &mut Vec<PathBuf>) {
        if let Ok(rd) = std::fs::read_dir(dir) {
            for e in rd.filter_map(Result::ok) {
                let p = e.path();
                if p.is_dir() {
                    walk(base, &p, out);
                } else if let Ok(rel) = p.strip_prefix(base) {
                    out.push(rel.to_path_buf());
                }
            }
        }
    }
    let mut out = Vec::new();
    walk(staging, staging, &mut out);
    out.sort_by_key(|p| p.to_string_lossy().to_lowercase());
    out
}

fn top_name(rel: &Path) -> String {
    rel.components().next().map(|c| c.as_os_str().to_string_lossy().into_owned()).unwrap_or_default()
}

/// The helper's work: Ok, RolledBack or Aborted. Only the staged program's own files change.
pub fn swap(
    app_dir: &Path,
    staging: &Path,
    procs: &mut dyn UpdateProcesses,
    marker: &Path,
    log: &mut dyn FnMut(&str),
) -> Outcome {
    let exe = app_dir.join(EXE_FILE);
    let updates = app_dir.join(app_info::UPDATES_FOLDER);
    let rollback = updates.join(ROLLBACK_FOLDER);
    let failed = updates.join(FAILED_FOLDER);
    if !staging.join(EXE_FILE).is_file() {
        log("aborted: the staged program has no executable");
        return Outcome::Aborted;
    }
    let files = staged_files(staging);
    if files.iter().any(|f| is_reserved(&top_name(f))) {
        log("aborted: the staged program would replace user data; nothing changed");
        return Outcome::Aborted;
    }
    if !procs.wait_exit() {
        log("aborted: the old program did not exit; nothing changed");
        return Outcome::Aborted;
    }
    if marker.is_file() {
        let _ = std::fs::remove_file(marker);
    }

    // 1. Move the files the update replaces aside (rollback copy)
    let mut moved: Vec<PathBuf> = Vec::new();
    try_delete(&rollback);
    for rel in &files {
        let cur = app_dir.join(rel);
        if !cur.is_file() {
            continue;
        }
        let keep = rollback.join(rel);
        let r = keep.parent().map_or(Ok(()), std::fs::create_dir_all).and_then(|_| std::fs::rename(&cur, &keep));
        if let Err(e) = r {
            log(&format!("aborted: cannot move the program files aside ({e}); nothing changed"));
            move_back(app_dir, &rollback, &moved);
            procs.start(&exe, &[]);
            return Outcome::Aborted;
        }
        moved.push(rel.clone());
    }

    // 2. Copy the new files in
    let mut copied: Vec<PathBuf> = Vec::new();
    for rel in &files {
        let dst = app_dir.join(rel);
        let r = dst.parent().map_or(Ok(()), std::fs::create_dir_all).and_then(|_| {
            if dst.exists() {
                return Err(std::io::Error::new(std::io::ErrorKind::AlreadyExists, "exists"));
            }
            std::fs::copy(staging.join(rel), &dst).map(|_| ())
        });
        if let Err(e) = r {
            log(&format!("copy failed ({e}); putting the old program back"));
            // a partial copy of this file is removed too (File.Copy leaves none behind)
            // (the old file of this name was moved aside in step 1, so whatever is there now is the new one)
            try_delete_file(&dst);
            for c in &copied {
                try_delete_file(&app_dir.join(c));
            }
            move_back(app_dir, &rollback, &moved);
            procs.start(&exe, &[]);
            return Outcome::RolledBack;
        }
        copied.push(rel.clone());
    }

    // 3. Start the new program and wait for its marker
    let handle = procs.start(&exe, &[CHECK_ARG.to_string(), marker.to_string_lossy().into_owned()]);
    if procs.wait_marker(marker) {
        log("ok: the new program started");
        return Outcome::Ok;
    }
    log("the new program did not start in time; rolling back");
    procs.stop(handle);
    try_delete(&failed);
    for rel in &copied {
        let cur = app_dir.join(rel);
        let bad = failed.join(rel);
        let r = bad.parent().map_or(Ok(()), std::fs::create_dir_all).and_then(|_| std::fs::rename(&cur, &bad));
        if r.is_err() {
            try_delete_file(&cur);
        }
    }
    move_back(app_dir, &rollback, &moved);
    procs.start(&exe, &[]);
    Outcome::RolledBack
}

fn move_back(app_dir: &Path, rollback: &Path, moved: &[PathBuf]) {
    for rel in moved {
        let dst = app_dir.join(rel);
        try_delete_file(&dst);
        let _ = std::fs::rename(rollback.join(rel), &dst);
    }
}

struct HelperArgs {
    pid: u32,
    app_dir: PathBuf,
    staging: PathBuf,
    marker: PathBuf,
    result: PathBuf,
}

fn parse_args(argv: &[String]) -> Option<HelperArgs> {
    if !argv.len().is_multiple_of(2) {
        return None;
    }
    let mut map = std::collections::HashMap::new();
    for pair in argv.chunks(2) {
        if !pair[0].starts_with("--") {
            return None;
        }
        map.insert(pair[0].as_str(), pair[1].clone());
    }
    let pid_text = map.get("--pid")?;
    if pid_text.is_empty() || !pid_text.chars().all(|c| c.is_ascii_digit()) {
        return None;
    }
    Some(HelperArgs {
        pid: pid_text.parse().ok()?,
        app_dir: map.get("--app-dir")?.into(),
        staging: map.get("--staging")?.into(),
        marker: map.get("--marker")?.into(),
        result: map.get("--result")?.into(),
    })
}

/// The helper's entry: `DDM32Plus --self-update ARGS` (argv without the --self-update itself). Exit code 0 when the
/// new program runs, 1 when not, 2 for bad arguments.
pub fn helper_main(argv: &[String], processes: &mut dyn FnMut(u32) -> Box<dyn UpdateProcesses>) -> i32 {
    let Some(a) = parse_args(argv) else { return 2 };
    let mut lines: Vec<String> = Vec::new();
    let mut procs = processes(a.pid);
    let result = swap(&a.app_dir, &a.staging, procs.as_mut(), &a.marker, &mut |t: &str| {
        lines.push(format!("{} {t}", crate::storage::local_clock().0));
    });
    let write = || -> std::io::Result<()> {
        if let Some(d) = std::path::absolute(&a.result)?.parent() {
            std::fs::create_dir_all(d)?;
        }
        let v = serde_json::json!({
            "result": result_name(result), "log": lines, "finished": crate::storage::local_clock().1,
        });
        std::fs::write(&a.result, v.to_string())
    };
    let _ = write();
    if result == Outcome::Ok {
        0
    } else {
        1
    }
}

/// The last helper result, once (the file is removed after reading).
pub fn read_result(updates_dir: &Path) -> Option<String> {
    let p = updates_dir.join(RESULT_FILE);
    let text = std::fs::read(&p).ok()?;
    let v: serde_json::Value = serde_json::from_slice(&text).ok()?;
    std::fs::remove_file(&p).ok()?;
    v.get("result").and_then(|r| r.as_str()).map(str::to_string)
}

/// Old staging folders and zips (not the one running now). Rollback copies are kept; nothing outside
/// `updates_dir` is touched.
pub fn cleanup(updates_dir: &Path, keep: Option<&Path>) {
    let Ok(rd) = std::fs::read_dir(updates_dir) else { return };
    let keep_full = keep.map(|k| std::path::absolute(k).unwrap_or_else(|_| k.to_path_buf()));
    for e in rd.filter_map(Result::ok) {
        let p = e.path();
        let name = e.file_name().to_string_lossy().into_owned();
        if p.is_dir() {
            if !name.starts_with("app-") {
                continue;
            }
            let full = std::path::absolute(&p).unwrap_or_else(|_| p.clone());
            if keep_full.as_ref().is_some_and(|k| k.starts_with(&full)) {
                continue;
            }
            try_delete(&p);
        } else {
            let lower = name.to_lowercase();
            if lower.ends_with(".zip") || lower.ends_with(".partial") {
                try_delete_file(&p);
            }
        }
    }
}

fn try_delete(dir: &Path) {
    if dir.is_dir() {
        let _ = std::fs::remove_dir_all(dir);
    }
}

fn try_delete_file(file: &Path) {
    if file.is_file() {
        let _ = std::fs::remove_file(file);
    }
}

/// The real process functions (used by the program's `--self-update` entry, stage 2).
pub struct RealProcesses {
    pub old_pid: u32,
    children: Vec<(u32, std::process::Child)>,
}

impl RealProcesses {
    pub fn new(old_pid: u32) -> Self {
        Self { old_pid, children: vec![] }
    }
}

impl UpdateProcesses for RealProcesses {
    fn start(&mut self, exe: &Path, args: &[String]) -> Option<u32> {
        let mut cmd = std::process::Command::new(exe);
        cmd.args(args);
        if let Some(d) = exe.parent() {
            cmd.current_dir(d);
        }
        let child = cmd.spawn().ok()?;
        let id = child.id();
        self.children.push((id, child));
        Some(id)
    }

    fn wait_exit(&mut self) -> bool {
        let end = std::time::Instant::now() + EXIT_TIMEOUT;
        while std::time::Instant::now() < end {
            if !process_alive(self.old_pid) {
                return true;
            }
            std::thread::sleep(Duration::from_millis(250));
        }
        false
    }

    fn wait_marker(&mut self, marker: &Path) -> bool {
        let end = std::time::Instant::now() + START_TIMEOUT;
        while std::time::Instant::now() < end {
            if marker.is_file() {
                return true;
            }
            std::thread::sleep(Duration::from_millis(500));
        }
        false
    }

    fn stop(&mut self, handle: Option<u32>) {
        if let Some(h) = handle {
            if let Some((_, c)) = self.children.iter_mut().find(|(id, _)| *id == h) {
                let _ = c.kill();
            }
        }
        std::thread::sleep(Duration::from_secs(1));
    }
}

/// Whether a process id still runs (best effort, no extra dependency).
fn process_alive(pid: u32) -> bool {
    if cfg!(windows) {
        let mut cmd = std::process::Command::new("tasklist");
        cmd.args(["/FI", &format!("PID eq {pid}"), "/NH"]);
        // the helper is a window program: without this every poll (4 a second) would flash a console window
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            const CREATE_NO_WINDOW: u32 = 0x0800_0000;
            cmd.creation_flags(CREATE_NO_WINDOW);
        }
        cmd.output().map(|o| String::from_utf8_lossy(&o.stdout).contains(&pid.to_string())).unwrap_or(false)
    } else {
        Path::new(&format!("/proc/{pid}")).exists()
            || std::process::Command::new("kill").args(["-0", &pid.to_string()]).status().is_ok_and(|s| s.success())
    }
}
