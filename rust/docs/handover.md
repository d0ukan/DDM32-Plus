# Moving C# 2.0.x users to the Rust program (hand-over), 2026-11-24

Analysis from the code only (`win/src/DDM32Plus.Core/Update/SelfUpdate.cs`, `win/src/DDM32Plus/App.xaml.cs`,
`win/src/DDM32Plus/Services/AppState.cs`, `win/src/DDM32Plus.Core/Releases/*.cs`, `pc/release/make_release.py`,
`rust/crates/ddm32-core/src/update/self_update.rs`, `rust/crates/ddm32-gui/src/{main,lib}.rs`). Nothing was released,
no C# program was run, no port was opened. What is proven here is proven by host tests; the real swap on a real
Windows PC with an installed C# 2.0.0 is still to be run (see "Still to prove").

## Short answer

Yes. A C# 2.0.x user can be moved to the Rust program through the C# program's own self-update, **without a new C#
release**, provided a signed release lists the Rust Windows zip as its `app_net` entry with a version above 2.0.0
(for example 3.1.0). The C# program only downloads, checks and unpacks that zip and starts the `DDM32Plus.exe` inside
it as the helper; from then on the **Rust** program's helper does the swap, the start check and the rollback, with the
same protocol and the same files as the C# helper. Two Rust-side problems would have broken or hurt this and are fixed
on branch `rsgap1`:

1. The Rust exe needed `vcruntime140.dll` (the Visual C++ Redistributable), which a C# user (self-contained .NET) need
   not have. Without it the staged Rust exe cannot even start as the helper. Fixed: `rust/.cargo/config.toml` links the
   C runtime statically on Windows (`+crt-static`); checked on the built `ddm32-gui.exe` (no `vcruntime140.dll` /
   `api-ms-win-crt-*` import left).
2. The Rust program wrote the start marker right after creating the window, before the event loop ran: a program
   whose window then failed to come up would still count as "started" and never be rolled back. Fixed: the marker is
   written 1.5 s after the event loop runs (as the C# program does), `ddm32_gui::arm_start_marker`, test
   `tests/close.rs::the_start_marker_is_written_once_the_event_loop_runs`.

Also fixed (not safety): the Rust helper polled `tasklist` 4 times a second without `CREATE_NO_WINDOW`, which flashes a
console window from a window program during the up to 60 s wait for the old program to exit.

## What exactly happens (C# 2.0.0 old program, Rust zip as `app_net`)

The Rust Windows zip (`rust/packaging/make_app_zip.py windows-x86_64 ...`) holds exactly
`DDM32Plus/DDM32Plus.exe` (the Rust window) and `DDM32Plus/ddm32.exe` (the command line).

1. **Offer.** C# `Catalog.NewerApp("2.0.0", include_beta)` returns the first `app_net` entry whose version is above
   2.0.0; a prerelease (`3.1.0-beta.1`) only when `include_beta` is true (the C# default). So publish the hand-over as a
   **stable** version (no `-`), or users who switched betas off never see it. The manifest checks the C# program applies
   to `app_net` (`SignedManifest.Asset`): `version` parses, `file` matches `^[0-9A-Za-z][0-9A-Za-z._-]{0,120}$`, the
   `url` starts with the repo's release prefix and ends with `/file`, `sha256` hex, `size` within the limit. Nothing
   ties `app_net` to a .NET build: any zip passes.
2. **Download and stage** (C# `AppState.StartProgramUpdateAsync`): size and SHA-256 from the signed manifest, then
   `SelfUpdate.Stage` -> `SafeExtract` into `updates\app-3.1.0\`. Its rules: every member under `DDM32Plus/`, no `..`,
   no backslash, no colon, no reserved name (`backups`, `releases`, `updates`, `packs`, `logs`, `settings.json`, the
   uninstaller, the old Turkish names) as the second part, and `DDM32Plus/DDM32Plus.exe` must exist. The Rust zip
   passes: `DDM32Plus/ddm32.exe` is simply extracted next to it.
3. **Helper start.** C# starts `updates\app-3.1.0\DDM32Plus\DDM32Plus.exe --self-update --pid <C#> --app-dir <install>
   --staging <staged> --marker updates\update-started.json --result updates\last-update.json` with that folder as the
   working directory, and closes itself. That exe is the **Rust** program: `main.rs` sees `--self-update` first and runs
   `self_update::helper_main` (same argument names, same parsing: pairs of `--name value`, `--pid` an integer, all five
   required, else exit 2 and nothing changes).
4. **Swap** (Rust `self_update::swap`, a line-for-line port of C# `Swap`): wait up to 60 s for the C# process to exit
   (`tasklist`), delete an old marker, move every installed file the staged program brings (`DDM32Plus.exe`; `ddm32.exe`
   does not exist yet) to `updates\rollback\`, copy the staged files in (`DDM32Plus.exe`, `ddm32.exe`), start
   `<install>\DDM32Plus.exe --update-check updates\update-started.json` and wait up to 90 s for that file.
5. **Start check.** The new (Rust) program writes the marker `{"started": true, "version": "3.1.0", "pid": N}` 1.5 s
   after its event loop runs. Both helpers only test that the file exists.
6. **Result.** Written to `updates\last-update.json` (`ok` / `rolled_back` / `aborted`). The Rust program reads it on
   its start (`self_update::read_result`, also the old `guncelleme` folder) and shows it once on Home. As in the C#
   program the helper writes it only after the marker, so the first start after the swap usually misses it and the
   second start shows it.

The install folder, the data folder and `settings.json` are the same, so the user keeps everything in place:
`backups\`, `releases\`, `updates\`, `packs\`, `logs\` are found with no copy, and the keys carry over:

- `disclaimer`: the Rust program now computes the same version as C# `AppState.DisclaimerVersion()` (SHA-256 over the
  same eight Turkish then English texts, first 16 hex digits: `684c0603763f3987` for today's text) and reads and writes
  the same object, so a C# user who accepted is not asked again (`tests/disclaimer.rs`).
- `channel` / `include_beta`: the Rust program uses `channel` when present, else the C# `include_beta`, else Beta
  when the file is a C# one (a C#-only key such as `window` is present; C#'s default showed betas), else Latest only for
  a fresh install. Choosing a channel writes both keys (`Settings::channel`, `set_channel`).
- `language`, `auto_check`, `port` are shared. C#-only keys (`window`, `installed_firmware`, ...) are kept unused.

## Rollback

| Case | What happens | User left with |
|---|---|---|
| The Rust exe cannot run at all (before the fix: no VC++ runtime) | C#'s `Process.Start` succeeds, the loader fails, C# has already closed; no helper ran | The C# program unchanged; it must be started by hand and offers the update again (loop). Fixed by `+crt-static`. |
| Bad helper arguments | Rust `helper_main` exits 2, nothing written | C# program unchanged, started by hand |
| C# program does not exit in 60 s | `aborted`, nothing changed, the old program is **not** restarted (as C#) | C# still running |
| Moving the old exe aside fails | old files moved back, old program started, `aborted` | C# program |
| Copying the new files fails | copied files deleted, old files moved back, old program started, `rolled_back` | C# program |
| The new window does not write the marker in 90 s (crash at start, window cannot be created, event loop never runs) | new process killed, new files moved to `updates\failed\`, old files moved back, **old C# program started**, `rolled_back` | C# program, result shown once at its next start |
| The new window starts, then crashes later | `ok` (the marker was written) | Rust program; no automatic way back (the C# exe stays in `updates\rollback\` until the next update replaces it) |

Before the marker fix, "window cannot be created by `ui.run()`" was in the last row (marker written too early).
Manual way back in every case: run the C# 2.0.0 setup again (same AppId, same folder; `backups` is never removed).

## What a final C# build would need

Nothing is required for the hand-over itself: C# 2.0.0 already reads `app_net` and starts whatever `DDM32Plus.exe`
the zip carries. A C# 2.0.1 would only be worth it for cosmetics, and is **not** recommended (it adds a release users
must take first):

- the Windows "Apps" entry keeps `DisplayVersion` 2.0.0 and the C# uninstaller log after a self-update (the
  Rust setup, same AppId, fixes both the next time it runs); the C# uninstaller would leave `ddm32.exe` behind because
  its log does not list it (harmless; the Rust setup lists it);
- a C# user who switched betas off sees only a stable hand-over version (see 1. above).

## Release recipe (not done here)

```sh
python rust/packaging/make_app_zip.py windows-x86_64 rust/target/release rust/dist/DDM32Plus-3.1.0-windows-x86_64.zip
python pc/release/make_release.py ... \
  --app-net-zip rust/dist/DDM32Plus-3.1.0-windows-x86_64.zip --app-net-version 3.1.0 \
  --app-rs-zip windows-x86_64=rust/dist/DDM32Plus-3.1.0-windows-x86_64.zip --app-rs-version 3.1.0 ...
```

The same bytes are published twice (`DDM32Plus-win-3.1.0.zip` for `app_net`, the `app_rs` name for Rust users); both
entries carry the same SHA-256. `make_release.py`'s `check_app_net_zip` accepts the Rust zip (it needs
`DDM32Plus/DDM32Plus.exe`, nothing outside `DDM32Plus/`, no reserved or private names).

## Still to prove (owner's PC, no radio needed)

1. A Windows PC with C# 2.0.0 installed by its setup and **no** Visual C++ Redistributable (or a clean VM).
2. A local or draft release as above; in C#: Versions > program update. Expect the Rust window within seconds, Home
   says nothing yet; at the second start "the program was updated". `backups\`, `settings.json` unchanged (hash them
   before), the disclaimer not asked again, the channel Beta if betas were on.
3. Rollback: the same with a Rust build that exits before its event loop (for example a test build that returns from
   `main` early): after 90 s the C# program comes back and says "rolled back"; `updates\failed\` holds the Rust files.

Tests that hold the Rust side: `crates/ddm32-core/tests/storage_update.rs` (swap ok, rollback when no marker,
new files removed on rollback, abort when the old program keeps running, helper arguments and result file),
`crates/ddm32-gui/tests/close.rs` (the marker after the event loop), `crates/ddm32-gui/tests/disclaimer.rs` (the C#
acceptance), `crates/ddm32-gui/tests/close.rs::the_channel_follows_the_csharp_include_beta`.
