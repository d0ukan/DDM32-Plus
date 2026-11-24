// SPDX-License-Identifier: GPL-3.0-or-later
//! The program's name, version and the fixed folder names (one place for all of them; AppInfo.cs).

pub const NAME: &str = "DDM32 Plus";
pub const EXE_NAME: &str = "DDM32Plus";

/// The program file inside the install folder and inside a program zip (`DDM32Plus/DDM32Plus.exe` on Windows).
pub const EXE_FILE: &str = if cfg!(windows) { "DDM32Plus.exe" } else { "DDM32Plus" };

/// The program file name the Windows program zips carry (the C# program's update format).
pub const WINDOWS_EXE_FILE: &str = "DDM32Plus.exe";

/// This (Rust) program's version; the workspace Cargo.toml carries the same value (checked by a test).
pub const VERSION: &str = env!("CARGO_PKG_VERSION");

/// The C# (.NET) program this one follows; its last version, for the self-update hand-over.
pub const NET_VERSION: &str = "2.0.0";

pub const AUTHOR: &str = "d0ukan · TA2DOU";
pub const REPO_URL: &str = "https://github.com/d0ukan/DDM32-Plus";

/// The Python program's data folder names under %LOCALAPPDATA% (read only, never moved or deleted).
pub const PYTHON_DATA_NAME: &str = "DDM32Plus";
pub const LEGACY_DATA_NAME: &str = "DDM25Plus";

/// The C# program's per-user install folder under %LOCALAPPDATA% (its `backups`, or `yedekler` up to 2.0.0-beta.2,
/// is read, never moved).
pub const NET_INSTALL_SUBDIR: [&str; 2] = ["Programs", "DDM32 Plus"];

// Folders and files inside the program's data folder (English names since C# 2.0.0-beta.3, owner 2026-11-21)
pub const BACKUPS_FOLDER: &str = "backups";
pub const RELEASES_FOLDER: &str = "releases";
pub const UPDATES_FOLDER: &str = "updates";
pub const PACKS_FOLDER: &str = "packs";
/// The search log's CSV files (one a day; kept on uninstall).
pub const LOGS_FOLDER: &str = "logs";
pub const SETTINGS_FILE: &str = "settings.json";

/// The Turkish names up to C# 2.0.0-beta.2: moved to the English ones at start-up (`AppPaths::ensure`). The Python
/// program's data folder keeps `yedekler`.
pub const LEGACY_BACKUPS_FOLDER: &str = "yedekler";
pub const LEGACY_RELEASES_FOLDER: &str = "surumler";
pub const LEGACY_UPDATES_FOLDER: &str = "guncelleme";
pub const LEGACY_PACKS_FOLDER: &str = "paketler";
pub const PYTHON_BACKUPS_FOLDER: &str = LEGACY_BACKUPS_FOLDER;

/// (old, new) folder names in the data folder.
pub const LEGACY_FOLDERS: [(&str, &str); 4] = [
    (LEGACY_BACKUPS_FOLDER, BACKUPS_FOLDER),
    (LEGACY_RELEASES_FOLDER, RELEASES_FOLDER),
    (LEGACY_UPDATES_FOLDER, UPDATES_FOLDER),
    (LEGACY_PACKS_FOLDER, PACKS_FOLDER),
];

/// Names an update must never replace and an uninstaller must never remove blindly: the user's data (old names too).
pub const RESERVED_NAMES: [&str; 12] = [
    BACKUPS_FOLDER,
    RELEASES_FOLDER,
    UPDATES_FOLDER,
    PACKS_FOLDER,
    LOGS_FOLDER,
    SETTINGS_FILE,
    "unins000.exe",
    "unins000.dat",
    LEGACY_BACKUPS_FOLDER,
    LEGACY_RELEASES_FOLDER,
    LEGACY_UPDATES_FOLDER,
    LEGACY_PACKS_FOLDER,
];

/// Developer-only features (local release folder, a chosen backup folder) exist only with the `developer` feature.
pub const DEVELOPER: bool = cfg!(feature = "developer");
