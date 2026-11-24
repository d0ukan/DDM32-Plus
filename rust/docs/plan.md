# DDM32 Plus programmer in Rust: the plan

Owner's decision 2026-11-21: the programmer moves from C# / .NET (`win/`) to Rust so it also runs on Linux and macOS.

| Stage | What | Status |
|---|---|---|
| 1 | `ddm32-core` + `ddm32-cli`: every non-UI part of the C# program, held to its fixtures | done (rust-app) |
| 2 | `ddm32-gui`: the window in Slint, tr / en, light and dark themes | done but the first real radio run (rust-app) |
| 3 | packaging: Windows setup, Linux AppImage (+ .deb), macOS .dmg; signing; the self-update hand-over | planned |

## Stage 1: the core (done)

Ported module by module from `win/src/DDM32Plus.Core`, with the C# tests' fixtures as the acceptance tests:

| Module | Held to |
|---|---|
| Ed25519 (`crypto`) | RFC 8032 vectors, 36 Python signatures, the Python verify edge cases |
| signed manifest, versions, labels | the 84 Python manifest cases, the legacy release, every version pair |
| release list, downloads, local folder | the Python catalog result, the C# download / redirect / size tests |
| self-update | the C# swap / rollback / unsafe-zip tests (two Windows-only: files held open) |
| radio tools | the 26 recorded FakeRadio runs: the same bytes in the same order, lines, errors and files |
| guards | the Python guards' 89-frame table and 5 step sequences |
| fake radio, quick update, no-backup install | the C# FakeRadio cross-check (11 runs) and the C# FastInstall tests |
| flows, plans, cable, progress | the C# FlowTests, ProgressTests |
| data packs, CPS writer | datapack.py's results (byte for byte), the C# CpsPack tests |

## Stage 2: the window (Slint)

### Owner's requirements (2026-11-21)

- Simple and minimalist; extremely easy. One obvious button per step, like the C# wizard.
- A home screen with **one big primary action** ("Telsizi güncelle / Update the radio") and a few clear secondary
  ones. A wizard that asks **one thing per screen** in plain Turkish / English; the next step is always obvious.
- No jargon on the main path: sector counts, hashes, plans are under **"Ayrıntılar / Details"**.
- ~~Big readable text (17 px body, 22 px for prompts), generous spacing.~~ Revised by the owner (2026-11-21, later):
  a calm, plain desktop tool, not a "designed" one. Normal text (15 px body, 13 px small, 19 px page titles), normal
  controls (34 px high, 3 px corners), a header row across every page, dense aligned rows; colour only for the one
  accent (blue: main action, selection) and for states (green ok, amber warning, red error, as the bar of a notice).
  Controls stay at least 32 px (WCAG 2.2 asks 24 px); the 150 % / 200 % layout test still checks that nothing overlaps
  or leaves the window.
- **Light and dark themes; light is the default at the first start**; a switch in Settings, remembered in
  `settings.json`, switched live without a restart. Every colour comes from one palette (`ui/theme.slint`); no other
  file names a colour (a test checks it). Every text / background pair of both palettes meets WCAG AA (4.5:1 for text,
  3:1 for focus rings and large accents), including disabled, focus, hover, warning and danger states (a test checks
  it).
- The radio power-cycle prompts are unmistakable (bold steps in a notice with the amber warning bar: "1. Telsizi
  KAPATIN …").
- Errors say what to do ("… Kabloyu kontrol edip yeniden deneyin. Telsize bir şey yazılmadı.").
- Backups always go to the program's own `backups` and are found by themselves; the user never chooses a folder.
- Pages: Update radio, Backups / going back, Data packs, Settings (language, theme, release channel Latest / Beta /
  Test), About. Self-update through the signed manifest.
- Safety: every serial / flash step behind its own button; all AGENTS.md radio guards stay; tests use fakes only.

### Wireframes

```
Home                                         Update wizard (one question per screen)
+--------------------------------------+     +--------------------------------------+
| DDM32 Plus                           |     | [<- Ana sayfa]  Telsizi güncelle     |
| [ Kablo bulundu: COM14 ...        ]  |     | (Sürüm)(Yedek)(Kontrol)(Kurulum)(Bitti)|
| +----------------------------------+ |     | Kurulumdan önce telsiz nasıl         |
| |        TELSİZİ GÜNCELLE          | |     | korunsun?                            |
| +----------------------------------+ |     | (o) Yeni yedek al (önerilen)         |
| Yeni sürüm hazır: 1.0.0              |     | ( ) Son yedeği kullan (hızlı)        |
| [Yedekler ve geri dönüş][Veri pkt.]  |     | ( ) Yedeksiz kur (riskli)  <- red    |
| [Ayarlar]           [Hakkında]       |     |                                      |
| 3 yedek bulundu (otomatik).          |     |                     [Geri]  [İleri]  |
+--------------------------------------+     +--------------------------------------+

Radio step (backup / install)                Install without backup
+--------------------------------------+     +--------------------------------------+
| +----------------------------------+ |     | (o) Yedeksiz kur (riskli)            |
| | 1. Telsizi KAPATIN.              | |     | +----------------------------------+ |
| | 2. Kabloyu takın.                | |     | | Dikkat: yedek alınmadan ...      | | <- red box
| | 3. Düğmeye basın, telsizi AÇIN.  | |     | +----------------------------------+ |
| +----------------------------------+ |     | [x] Anlıyorum, yedeksiz kurmak ...   |
| [        YEDEĞİ BAŞLAT          ]   |     |            [İleri] -> "Emin misiniz?" |
| ======-------- 40 %                  |     |            [Vazgeç] [Evet, yedeksiz kur]|
| [+ Ayrıntılar]                       |     +--------------------------------------+
+--------------------------------------+
```

The C# wizard's order is kept: version -> backup (new / last / none) -> check (detect + plan, files only) -> install
-> done. A failed install is never retried: before any write the user may try again by hand; after writes started the
install is locked and the page points to "Yedekler ve geri dönüş".

### Done in this run

- `crates/ddm32-gui`: `ui/theme.slint` (the palette), `ui/widgets.slint` (Button, Choice, Notice, Check, Progress),
  `ui/app.slint` (first start: language; home; wizard; backups; data packs; settings; about), `src/lib.rs` (the
  controller on `ddm32-core`, radio steps in a worker thread, lines -> progress), `src/i18n.rs` (tr / en).
- `tests/screens.rs`: renders every screen in both themes offscreen (Slint software renderer), checks that the theme
  switches live on every page, checks WCAG AA for both palettes, no colour outside the theme, every text key in both
  languages; writes `docs/screenshots/*.png` (light and dark, tr; the main screens also en).

### Done in the second run (2026-11-21)

- Home: the owner's DDM32+ logo (light / dark variants from art/mark-src.png, `packaging/art/make_logo.py`), the full
  version with its label ("1.0.0-beta.5 (Beta)"), what the radio runs by its newest verified backup, the backups
  folder with "Aç". Every version list in the C# order (tested per channel).
- The program's own update in Settings and About (signed `app_rs`; Windows: download, stage, swap helper; Linux /
  macOS: the release page).
- Radio health, Live view (read-only debug link, the C# DebugLink / LiveData held to live-results.json) and My
  channels (the codeplug viewer held to codeplug-results.json.gz; search, CSV).
- The CH340 missing-driver hint on Windows (SetupAPI / CfgMgr32 device list, no port opened).
- The Windows setup builds with Inno Setup 7 into `rust/dist/`.

### Done 2026-11-24 (v1.1.0 candidate; Rust becomes the only programmer)

- The start-up update question (one dialog; Update now / Later / Skip this version; waits while the port is held;
  24 h re-check), failed checks in `logs/update-check.log` and on Home, Home's firmware line against what the radio
  runs, the program update's beta fallback (`src/updates.rs`, `tests/updates.rs`).
- The cable picker in the menu bar (Auto (CH340), each CH340 cable, Advanced: every other OS port; `settings.json`
  `port` / `port_auto`; fallback to Auto; `ddm32-core` `radio::portpick`, `tests/ports.rs`).
- "Program 3.0.0 · Radio firmware 1.0.1" in the title bar and on Home; About lists both. README "Versions and the
  update question". Host-tested only.
- Verifier fixes (same day): the install record (`installed_sha256` / `installed_firmware` as the C# program, plus
  `installed_record`) after every verified install and restore, used unless a newer backup exists; the source said in
  the version line; the question and the program update wait for the wizard, a ready restore and downloads; per-item
  Skip, Later as the default button; dialogs modal for the keyboard (`tests/modal.rs`); port listing on a worker
  thread; the port choice locked through the wizard's radio steps; a gone port said on Home and in the header.

### Still to do in stage 2

- Done 2026-11-21 (third run): keyboard order, Enter on the primary button, screen reader names, 150 % / 200 %
  scaling (tests `a11y.rs`, `hidpi.rs`; README "Keyboard, screen readers, high DPI"). Still to try by hand: Narrator,
  Orca and VoiceOver on real desktops.
- A first real run on the owner's radio, one approved step at a time, compared with the C# program (backup byte
  compare first, then plan, then install), following firmware/docs/plus-radio-test.md.

## Stage 3: packaging (scripts done 2026-11-21, nothing published)

Scripts: `packaging/linux/build-linux.sh` (AppImage with an appimagetool you supply, `--deb`, `--appdir-only`),
`packaging/macos/build-macos.sh` (.app + .dmg, ad-hoc signed; the workflow's `macos-app` job on rust-app),
`packaging/make_app_zip.py` (the `app_rs` update zip per platform), `pc/release/make_release.py --app-rs-zip
PLATFORM=ZIP` (repeatable) to list them in a signed release. See README "Packages". The macOS .dmg is per CPU
(`macos-aarch64` from macos-latest); a universal `lipo` build is left for later.

| OS | Package | Notes |
|---|---|---|
| Windows | `DDM32Plus-Setup-<version>.exe` (Inno Setup, `packaging/windows/DDM32Plus.iss`) | per user, `%LOCALAPPDATA%\Programs\DDM32 Plus`, the **same AppId** as the C# setup, so it replaces it in place; `backups` is `uninsneveruninstall` |
| Linux | `DDM32Plus-<version>-x86_64.AppImage` (+ optional `.deb`) | AppImage from `target/release/ddm32-gui` with a .desktop file and icon; data in XDG; serial access needs the user in `dialout` (the program explains it) |
| macOS | `DDM32Plus-<version>.dmg` | built by GitHub Actions on macos-latest (universal: x86_64 + aarch64 via `lipo`); an .app bundle; data in Application Support |

Signing:
- Windows: unsigned until a code-signing certificate is bought (SmartScreen "More info > Run anyway", as for the C#
  program). Then signtool in CI with the certificate as a GitHub secret (not added now).
- macOS: unsigned builds need right click > Open; notarisation needs an Apple Developer ID (secret in CI later).
- Releases stay trusted by the Ed25519-signed manifest either way: every program zip and firmware zip is checked
  against it before use.

### The self-update hand-over from the C# program

1. A release's signed manifest gets a new key **`app_rs`**: one asset per platform
   (`windows-x86_64`, `linux-x86_64`, `macos-aarch64`, `macos-x86_64`). The Python and C# checkers ignore unknown keys;
   this program updates itself only from `app_rs` for its own platform (`releases::Catalog::newer_app`).
2. For the hand-over, the same release's **`app_net`** entry carries the Rust program for Windows, packed as the C#
   program expects: `DDM32Plus/DDM32Plus.exe` in the zip, version 3.x (above every 2.x C# version).
3. The C# program downloads and stages it, then starts the staged **new** program as its helper with
   `--self-update --pid --app-dir --staging --marker --result`: the Rust program implements exactly this helper
   (`update::self_update::helper_main`) and the `--update-check FILE` marker, so the swap, the rollback and the user's
   data rules are the same. `backups`, `settings.json`, `releases`, `updates` and `packs` are never touched.
4. From then on the Rust program updates itself from `app_rs`.
