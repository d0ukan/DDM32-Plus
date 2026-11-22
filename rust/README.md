# DDM32 Plus programmer (Rust)

The program that installs, updates and backs up the DDM32 Plus firmware on a Baofeng DM-32UV, for **Windows, Linux
and macOS**. It replaces the C# / .NET program in `win/` (owner's decision 2026-11-21) and is held byte for byte to it
and to the Python tools in `pc/radiotools`: the same bytes to the radio, the same receipts and backup files, the same
refusals.

Licence: GPL-3.0-or-later (dependencies: [THIRD_PARTY.md](THIRD_PARTY.md)).

## Structure

```
rust/
  Cargo.toml                 workspace (version 3.0.0-alpha.3)
  crates/ddm32-core/         everything that is not the window
    src/crypto/ed25519.rs        Ed25519 verify (a port of the RFC 8032 reference code, as the C# and Python ones)
    src/releases/                signed manifest checks, version order and labels, release list, downloads, local folder
    src/update/self_update.rs    the self-update swap and rollback (the C# program's protocol, for the hand-over)
    src/storage/                 data folder per OS, settings.json, backups (fixed backups, automatic discovery)
    src/radio/                   RAM monitor host, guards (ReadOnlyPort, InstallPort, CpsPackPort), tools
                                 (backup, install over Plus / from stock / restore, quick update, no-backup install,
                                 restore stock, data pack writer), codec slot writer, detect, flows, progress, cable
    src/datapacks/               data pack format, RadioID CSV, AD 2.18 text / PDF, building packs from files
    data/                        the pinned RadioFoundry RAM monitor and DM-32 codec tables (MIT; SHA-256 checked)
    tests/                       acceptance tests on the C# program's fixtures (win/tests/DDM32Plus.Core.Tests/Fixtures)
  crates/ddm32-cli/          `ddm32`: a small command line (tests, the owner's first real runs)
  crates/ddm32-gui/          the window (Slint): ui/*.slint, src/lib.rs (controller), src/i18n.rs (tr / en)
  docs/plan.md               stage 2 (GUI) design and stage 3 (packaging) plan
  docs/screenshots/          every screen in the light and the dark theme (written by crates/ddm32-gui/tests/screens.rs;
                             the 150 % / 200 % ones by tests/hidpi.rs)
  packaging/windows/         Inno Setup script (per-user install over the C# program, same AppId and folder)
  packaging/linux/           AppImage / .deb script, .desktop file
  packaging/macos/           .app / .dmg script, Info.plist
  packaging/make_app_zip.py  the update zip per platform (app_rs); packaging/art/ the logo and the icon
```

## Radio access

A serial port exists only in a build with the **`radio` feature** (off by default). Without it no code path can open a
port (`RadioAccess::factory` refuses, and the serial code is not compiled in); the tests build without it. Every radio
step starts from its own button or command, the guards sit between every tool and the port, and a failed write is never
retried. See AGENTS.md rules 1-4.

## Search log

The Search log page reads the radio's search finds every second (`radio::debug::searchlog_tool --reconnect`) and counts
each call once (`radio::searchlog`, the C# `SearchLogBook`): one CSV per session (`logs/search-YYYYMMDD-HHMMSS.csv`,
BOM, the Windows list separator or ",", formula-safe cells, a `-open.csv` side file while another program holds it);
the PC kept awake on Windows (`sys::KeepAwake`, a no-op elsewhere); a lost radio waited for; Health, Live and the radio
steps wait while it holds the port. Details: docs/plus.md "Search log on the PC".

## Where the program keeps things

| OS | data folder |
|---|---|
| Windows | the program's own install folder (`%LOCALAPPDATA%\Programs\DDM32 Plus`, as the C# program) |
| Linux | `$XDG_DATA_HOME/DDM32Plus`, else `~/.local/share/DDM32Plus` |
| macOS | `~/Library/Application Support/DDM32Plus` |

Inside it: `backups/` (every backup and every install log: the user never chooses another folder; updates and the
uninstaller never delete it), `releases/` (downloaded releases), `updates/` (program updates), `packs/` (data
packs), `settings.json`. On Windows the older programs' backup folders (`%LOCALAPPDATA%\DDM32Plus`, `DDM25Plus`, and the
C# install folder when this program lives elsewhere) are found and read, never changed. `DDM32_DATA` overrides the
folder (tests, screenshots).

## Keyboard, screen readers, high DPI

- **Keyboard**: Tab / Shift+Tab walk every page in reading order with a 3 px focus ring (`Theme.focus`, AA contrast in
  both themes). The page's safe primary button takes the focus when the page opens, so **Enter** runs it (Home: update,
  wizard: Next, first start: Türkçe, the no-backup confirmation: Cancel). Buttons that write to the radio never take
  the focus on their own. Enter or Space runs a focused button or choice; Space toggles a check box.
- **Screen readers** (UI Automation / AT-SPI / NSAccessibility through Slint's AccessKit): every button, choice, check
  box and field has a role and a name in both languages; notices read as text, the progress bar reports 0-100.
- **Scaling**: the layout is held at 150 % and 200 % at the default window and at the smallest one (640 x 560 logical),
  so text wraps and buttons, choices and notices grow with it; pages that do not fit scroll. Screenshots:
  `docs/screenshots/*-150.png`, `*-200.png`.
- Tests: `crates/ddm32-gui/tests/a11y.rs` (names, Enter / Tab / Shift+Tab / Space, focus ring on every page) and
  `tests/hidpi.rs` (no control overlaps another or leaves the window, every target at least 44 x 44).

## Build and test

Rust 1.88 or newer (tested with 1.100.0).

```sh
cd rust
cargo test                                   # all tests (no serial port, no network)
cargo clippy --all-targets -- -D warnings
cargo fmt --all --check
cargo build --release -p ddm32-cli -p ddm32-gui                                       # no radio access
cargo build --release -p ddm32-cli -p ddm32-gui --features ddm32-cli/radio,ddm32-gui/radio   # the installed program
```

Linux: the window's font lookup links fontconfig, so a build needs `pkg-config` and `libfontconfig-dev`
(`sudo apt install pkg-config libfontconfig-dev`); the window uses Wayland or X11 through winit (loaded at run time). The tests read the C# program's fixtures from `../win/tests/DDM32Plus.Core.Tests/Fixtures`.

Command line: `ddm32 version | paths | backups | detect --backup DIR | plan ... | releases | download VERSION |
pack radioid|air|info ... | tool backup|install_plus|restore_stock|fast_install|datapack ARGS` (`ddm32 --help`).
The window: `ddm32-gui` (`--demo` shows example contents without radio or network).

## Packages

Every script builds with radio access (`--features radio`) unless told `--no-radio`, writes only to `rust/dist/`
(git-ignored) and publishes nothing. Each also makes the platform's **update zip** for a signed release
(`packaging/make_app_zip.py`: `DDM32Plus/DDM32Plus[.exe]` + `DDM32Plus/ddm32[.exe]`, deterministic), which
`pc/release/make_release.py build ... --app-rs-zip PLATFORM=ZIP --app-rs-version V` lists under `app_rs` (repeat
`--app-rs-zip` once per platform; PLATFORM is `windows-x86_64`, `linux-x86_64`, `linux-aarch64`, `macos-aarch64`,
`macos-x86_64`, as `releases::platform_key()` names it). `ddm32 verify-manifest DIR [--public-key HEX]` checks such a
release the way this program reads it, including unpacking this platform's zip with the self-update's extractor.
The Windows program replaces itself from its zip; on Linux and macOS the program shows the new version and opens the
release page (an AppImage, a .deb or a .dmg is replaced by the user).

**Windows** (per user, over the C# program, keeps `backups`): build the window with `--features radio`, then
`"C:\Program Files\Inno Setup 7\ISCC.exe" rust\packaging\windows\DDM32Plus.iss`, and for the update zip
`python rust\packaging\make_app_zip.py windows-x86_64 rust	arget
elease rust\dist\DDM32Plus-VERSION-windows-x86_64.zip`.
Unsigned: SmartScreen asks "More info > Run anyway".

**Linux**: `rust/packaging/linux/build-linux.sh [--deb] [--appdir-only] [--no-build] [--no-radio]`

- AppImage `DDM32Plus-VERSION-ARCH.AppImage`: needs `appimagetool` (from https://github.com/AppImage/appimagetool,
  your choice of release) on PATH or in `$APPIMAGETOOL`; the script never downloads tools. `--appdir-only` stops at
  `rust/dist/AppDir` (AppRun, `ddm32plus.desktop`, icon, `usr/bin/DDM32Plus` and `usr/bin/ddm32`).
  `./DDM32Plus-*.AppImage ddm32 ARGS` runs the command line.
- `--deb`: `ddm32plus_VERSION_ARCH.deb` (`3.0.0~alpha.1` so Debian orders prereleases as SemVer): the program in
  `/opt/ddm32plus`, `/usr/bin/DDM32Plus` and `/usr/bin/ddm32`, the menu entry and the icon. Removing it never touches
  `~/.local/share/DDM32Plus` (backups).
- The serial cable needs the user in the `dialout` group (`sudo usermod -aG dialout $USER`, then log in again); no
  package changes groups.
- Build needs `pkg-config libfontconfig-dev`; at run time fontconfig, and Wayland or X11 (with libxkbcommon).

**macOS**: `rust/packaging/macos/build-macos.sh [--no-build] [--no-radio]` on a Mac (Xcode command line tools):
`rust/dist/DDM32 Plus.app` (Info.plist from `packaging/macos/Info.plist`, icon from `packaging/art/ddm32plus.png`)
and `DDM32Plus-VERSION-macos-ARCH.dmg` (the app and an Applications link). The GitHub workflow builds it in the
`macos-app` job on pushes to `rust-app` only, with no secret, and keeps the .dmg and the zip as workflow artifacts
(the `linux-packages` job does the same for the .deb, the AppDir and the zip). Signed ad hoc only (no Developer ID,
no notarisation): the first start needs right click > Open (or System Settings > Privacy & Security > Open
Anyway). The cable appears as `/dev/cu.usbserial-*` or `/dev/cu.wchusbserial*` (CH340; recent macOS has the driver).

