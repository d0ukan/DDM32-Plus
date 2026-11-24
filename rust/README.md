# DDM32 Plus programmer (Rust)

The program that installs, updates and backs up the DDM32 Plus firmware on a Baofeng DM-32UV, for **Windows, Linux
and macOS**. It replaces the C# / .NET program in `win/` (owner's decision 2026-11-21) and is held byte for byte to it
and to the Python tools in `pc/radiotools`: the same bytes to the radio, the same receipts and backup files, the same
refusals.

Licence: GPL-3.0-or-later (dependencies: [THIRD_PARTY.md](THIRD_PARTY.md)).

## Structure

```
rust/
  Cargo.toml                 workspace (version 3.0.0)
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

## Menu bar

A plain menu bar across the top of the window (every colour from the theme, light and dark): **Dosya / File** (Home,
backups folder, logs folder, Settings, Exit), **Telsiz / Radio** (health, live, search log, update, backups and going
back), **Araçlar / Tools** (my channels, data packs), **Yardım / Help** (user guide, GitHub page, About). It only
navigates or opens folders and links, never starts a radio step; no page is left (and the window is not closed) while a radio step, the live
panel or the search log holds the port; the same gate sits in `navigate()` (`App.menu-radio-locked`, `tests/menu.rs`). It is hidden on the first-start
language screen. Menu bar: ui/menubar.slint.

At the bar's right end the **cable control** ("Kablo: COM14 (otomatik) [Değiştir]" / "Cable: COM14 (auto) [Change]")
shows the port a radio step will use and opens the cable picker: **Auto (CH340)** (the default: the lowest-numbered
CH340 / CH341 cable, VID 1A86 PID 7523 / 5523, as before), each CH340 cable found, and under **Advanced** every other
serial port the OS lists (virtual pairs, com0com by its device or name and tty0tty by name, marked "virtual port: not a
radio", last) with the warning "not a CH340 cable, only if you know your cable". The choice is kept in `settings.json`
(`port`, `port_auto`). When a chosen port is gone the window uses Auto and says so, in the header ("Cable: COM15 not
found; using COM14 (auto)") and as a line on Home, until dismissed; the saved choice stays and is used again when the
port comes back. Ports come only from the OS's lists (`radio::portpick`, `radio::cable`: SetupAPI on Windows,
`serialport`'s list in a `radio` build); the picker never opens a port to probe it. The list is read on a worker thread
(a driver install can block the device list for seconds) when the picker opens, on Rescan and every 3 s (Slint 1.16
has no window-focus or device-change event, so a short poll covers both); a radio step's own click reads it once more
on the window's thread. The port in use is shown on Health, Live, Search log, Data packs, Backups (restore) and the
update wizard's backup and install steps. The choice is locked while a radio job, the live view or the search log
holds the port and from the wizard's backup choice to its install (its backup and install go to the same port). A
files-only build (no `radio` feature) hides the control. Tests: `tests/ports.rs`.

Dialogs (the update question, the cable picker, the no-backup confirmation) are modal for the keyboard too: the
controls under the shade take no focus or click and read as disabled (`widgets.slint` `Modal`, `tests/modal.rs`).

## Versions and the update question

The window's title and Home's bottom bar say "Program 3.0.0 · Radio firmware 1.0.1 (from your last install)"
("Telsiz firmware" in Turkish); About lists both numbers separately. What the radio runs is what the program knows:

- After every **verified** radio write the program records what the radio now runs in `settings.json`: the C#
  program's keys (`installed_sha256`: the installed application's SHA-256, null after a restore; `installed_firmware`:
  "DDM32 Plus 1.0.1", "stock" or "") and its own `installed_record` (kind, time, version, the backup it came with, the
  chips' JEDEC IDs of that backup). Paths: install with a new backup, with the last backup (quick update), without a
  backup, restore of an earlier backup, restore of the factory firmware. A run that stopped after its writes started
  records "unknown: the last install did not finish"; a run that wrote nothing leaves the record.
- The record is used unless a verified backup is newer than it (a fresher reading: "from your last backup; may be
  older"). A C# record keeps no time: it is used unless the newest backup holds a newer DDM32 Plus. The install's
  SHA-256 is mapped to a listed release through the signed list. Before any backup or install: "not read yet".
- The radio identity is only as strong as the radio tells: every DM-32UV has the same chips, so the JEDEC IDs name the
  model, not the unit. The time order decides; the identity is kept for the record.

Home's firmware line compares that with the newest version the channel offers: "up to date (1.0.1)", "a new radio
firmware is ready: 1.0.2 (the radio runs 1.0.1)", hedged when it comes from a backup ("your last backup shows 1.0.1;
the radio may run a newer one"), or only the newest when the radio's version is not known.

After the start-up check (Settings > "check at start") one dialog asks when something is newer: "Program 3.0.0 →
3.1.0" and / or "Radio firmware 1.0.1 → 1.0.2". The firmware item appears only from the install record or a backup
newer than it, never from a backup alone (it may be older than an install it does not know of). Each item has
**Update now** and **Skip this version** (stored per item: `skip_program_version`, `skip_firmware_version`; the other
item stays; the versions list and Settings still allow a skipped version by hand); **Later** (the default button) asks
again at the next start, not again in this session for the same versions. Update now for the program runs the existing
self-update (Windows: download, size and SHA-256 from the signed manifest, stage, helper, restart; Linux / macOS: the
release page opens); for the firmware it opens the update wizard at its first step with that version selected and
never starts a radio step. The question waits while anything is under way (a radio job, a download, the live view or
the search log; the wizard past its first step; a ready restore; another dialog) and comes on the 3 s tick once that is
over; its buttons check again and only close it if something started meanwhile. The program update refuses to start,
and again to restart after its download, while anything of that is under way. While the window stays open the list is
checked again every 24 h (not while anything is under way). A failed check is appended to `logs/update-check.log` and
shown as a small dismissable line on Home. The program update follows the firmware list's rule: on the default
channel, while no stable program build has been published, the betas are offered. Tests: `tests/updates.rs`.

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
packs), `logs/` (search log CSVs, `update-check.log`), `settings.json`. On Windows the older programs' backup folders (`%LOCALAPPDATA%\DDM32Plus`, `DDM25Plus`, and the
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

