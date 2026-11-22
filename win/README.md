# DDM32 Plus for Windows (C#, .NET 10, WPF)

The Python-free Windows programmer for DDM32 Plus, replacing the PyQt5 program in `pc/` step by step. Same pages,
same wizard (Hazırlık, Yedek, Kontrol, Yükle, Bitti), Turkish and English.

**Version 2.0.0-beta.2: the programmer for installs (owner's decision 2026-11-21), every page done.** It replaces the Python program
in `pc/` and is published with the firmware from v1.0.0-beta.4: download `DDM32Plus-Setup-2.0.0-beta.2.exe` from the
release, run it, done (see [Install](#install) and the Turkish user guide [docs/kullanim.md](docs/kullanim.md)).

The backup, install, quick update, install without backup, first install from stock and restore tools of
`pc/radiotools` are ported to C# with every guard and held to the Python tools byte for byte on 27 recorded runs
(see [docs/porting-plan.md](docs/porting-plan.md)); the developer build of this program made two real installs on the
owner's radio and a real backup byte-identical to the Python tool's. **Radio access is on in the release build:** each
radio step starts only from its own button, after its own confirmation; every guard of the Python tools stays (a
verified two-pass backup before a write, the plan of changed sectors, only application sectors, boot area, codec
slot and data chip never written by an install, full readback, no retry of a failed write). Data packs came in
2.0.0-beta.1 (RadioID and air band, built on the user's PC; see [Data packs](#data-packs)). New in 2.0.0-beta.2: the
last three pages, **Telsizin durumu** (radio health), **Canlı panel** (live panel) and **Kanallarım** (my channels);
no page says "coming in a later version" any more (see [Health, live panel, channels](#health-live-panel-channels)).

| Language dialog | Main window (Türkçe) | Main window (English) |
|---|---|---|
| ![](docs/screenshots/language-dialog.png) | ![](docs/screenshots/main-tr.png) | ![](docs/screenshots/main-en.png) |

| Step | Türkçe | English |
|---|---|---|
| 1 Hazırlık / Preparation | ![](docs/screenshots/wizard-1-prep-tr.png) | ![](docs/screenshots/wizard-1-prep-en.png) |
| 2 Yedek / Backup | ![](docs/screenshots/wizard-2-backup-tr.png) | ![](docs/screenshots/wizard-2-backup-en.png) |
| 3 Kontrol / Check | ![](docs/screenshots/wizard-3-check-tr.png) | ![](docs/screenshots/wizard-3-check-en.png) |
| 4 Yükle / Install | ![](docs/screenshots/wizard-4-install-tr.png) | ![](docs/screenshots/wizard-4-install-en.png) |
| 5 Bitti / Done | ![](docs/screenshots/wizard-5-done-tr.png) | ![](docs/screenshots/wizard-5-done-en.png) |
| Geri dön / Go back | ![](docs/screenshots/restore-tr.png) | ![](docs/screenshots/restore-en.png) |
| Telsizin durumu / Radio health | ![](docs/screenshots/health-tr.png) | ![](docs/screenshots/health-en.png) |
| Canlı panel / Live panel | ![](docs/screenshots/live-tr.png) | ![](docs/screenshots/live-en.png) |
| Kanallarım / My channels | ![](docs/screenshots/channels-tr.png) | ![](docs/screenshots/channels-en.png) |
| Veri paketleri / Data packs | ![](docs/screenshots/datapacks-tr.png) | ![](docs/screenshots/datapacks-en.png) |

More: [Sürümler](docs/screenshots/versions-tr.png), [Versions](docs/screenshots/versions-en.png). Every
screenshot is the release build 2.0.0-beta.2 on a made-up data folder (a synthetic verified backup with invented
channels, a made-up RadioID list): the check step with that backup chosen, the data pack page after "Paketi
oluştur", health and live panel with `--show health:demo` / `live:demo` (example readings, no radio), channels
from the synthetic backup. The version list is the real signed list from GitHub.

## Install

1. Download `DDM32Plus-Setup-2.0.0-beta.2.exe` from the GitHub release (the release's signed manifest also lists the
   program as `DDM32Plus-win-2.0.0-beta.2.zip`, the file the program's own update uses).
2. Run it. It installs **for your Windows user only**, without an administrator prompt, to
   `%LOCALAPPDATA%\Programs\DDM32 Plus`, with a Start menu entry and (if ticked) a desktop shortcut. Nothing else
   is needed: the .NET runtime is inside the program.
3. **SmartScreen.** The setup and the program are not code-signed yet, so Windows may say "Windows protected your
   PC" on the first run. Click **More info**, then **Run anyway** (Türkçe: **Ek bilgi**, sonra **Yine de
   çalıştır**). This goes away once a code-signing certificate is used.
4. On the first start choose Türkçe or English, read and accept the disclaimer, then use "Telsizi güncelle".

Later versions: the program checks the signed release list on start (switchable in Sürümler) and updates itself
from the manifest's `app_net` entry, after checking its Ed25519 signature, size and SHA-256. Uninstall in Settings >
Apps; the backups folder `backups` stays.

## Layout

```
win/
  DDM32Plus.sln
  Directory.Build.props          version (one place, also AppInfo.Version), warnings are errors
  src/DDM32Plus.Core/            no UI: Ed25519, signed manifest, versions, release list, downloads, channel
                                 labels, self-update swap, paths, settings
  src/DDM32Plus.Core/Radio/      the radio tools (ports of pc/radiotools), their guards, the install / restore
                                 flows' rules, the CH340 check; Data/ = the pinned RAM monitor image and the DM32
                                 codec tables (RadioFoundry, MIT, LICENSES/RadioFoundry-MIT.txt)
  src/DDM32Plus.Core/DataPacks/  the data pack builder (port of tools/datapack.py), the RadioID CSV and AIP readers;
                                 Radio/CpsPack.cs = the CPS client, its guard and the data pack tool
  src/DDM32Plus/                 the WPF program; Resources/Strings.resx (English) + Strings.tr.resx (Turkish)
  tests/DDM32Plus.Core.Tests/    xUnit; Fixtures/python-results.json = results of the Python code (make_fixtures.py),
                                 Fixtures/radio/ = recorded runs of the Python radio tools (make_radio_fixtures.py)
  tools/make_icon.py             the icon (Assets/ddm32plus.ico) from the owner's drawing art/mark-src.png
  installer/DDM32Plus.iss        Inno Setup script (per-user setup)
  docs/                          porting plan, screenshots
```

## Build and test

Needs the .NET 10 SDK (`dotnet --list-sdks` shows 10.0.x). From `win/`:

```
dotnet build DDM32Plus.sln
dotnet test DDM32Plus.sln
```

Both must finish with zero warnings (warnings are errors). The tests use fakes only: no network, no serial port.

The fixture of Python results is made by running the Python code (from the repository root):

```
python win/tests/DDM32Plus.Core.Tests/Fixtures/make_fixtures.py
```

Run it again when `pc/radiotools/signed_manifest.py`, `ed25519.py` or `pc/ddm32_app/releases.py` change; the C# tests
then show any difference.

The radio tools' recordings (on Windows; needs the pinned RadioFoundry tree):

```
set DDM32_UPSTREAM=E:\GIT-REPO\open25-dm32\research\upstream\radiofoundry
python win/tests/DDM32Plus.Core.Tests/Fixtures/make_radio_fixtures.py
```

It also writes `src/DDM32Plus.Core/Radio/Data/` (the RAM monitor image and the codec tables). Run it again when a
radio tool in `pc/radiotools` changes; the replay tests then show where the C# port differs.

## Faster update paths (over an installed DDM32 Plus only)

Owner's decision 2026-11-20 (AGENTS.md rule 4). On the Backup step, besides "Yedek al" (the default):

- **"Bu telsizin son yedeğini kullan (date)"** (quick update): the newest verified backup that holds DDM32 Plus
  (this program's `backups` and the Python program's) is reused. Before anything is written the tool reads the chip
  IDs, the boot area and the codec slot from the radio (only those ranges) and they must equal the backup's; otherwise
  it stops and asks for a new backup. Every application sector below the slot is rewritten.
- **"Yedeksiz yükle"** (install without backup): a red warning, an "Anladım" box, and a second "Emin misiniz?" before
  the install. The 1 MiB program flash is read once from the radio, kept in memory (never saved), and the plan and
  every check come from it.

Both refuse a first install from stock (its codec slot is prepared from the backup) and any radio that does not run
DDM32 Plus, before writing anything. Both keep every rule of the normal install: only application sectors, in plan
order, once; boot area, codec slot and data chip never written (data chip CRCs checked before and after); the whole
program read back before the restart; no retry of a failed write. The tool is `FastInstallTool` (`--quick --backup B`
or `--no-backup`), tested on the C# FakeRadio (`FastInstallTests.cs`).

## Data packs

Page **"Veri paketleri / Data packs"** (docs/plus.md "User data packs" in the firmware): lists the radio keeps in
the one data chip region the firmware may write, **0xE70000-0xEFFFFF**, two slots a type (RadioID 2 x 256 KB at
0xE70000 / 0xEB0000, air band 2 x 16 KB at 0xEF0000 / 0xEF4000). Optional; needs DDM32 Plus v1.0.0-beta.4 or newer.

- **What the radio has:** "Telsizdeki paketleri oku" reads the radio information and both slots of each type over
  the firmware's own open-firmware CPS protocol (`R` area 9 and area 1 reads inside the region; no new protocol, no
  write). "Yedekten göster..." reads the same from a verified backup's `q128-pass1.bin` (files only). The radio stays
  switched on (the CPS talks to the running firmware, not the boot ROM).
- **RadioID pack:** from RadioID.net's `user.csv` that the **user** downloads: "radioid.net'i aç" opens the download
  page in the browser, "user.csv'yi indir" downloads `https://radioid.net/static/user.csv` on that click only (HTTPS,
  radioid.net only). Countries as RadioID.net writes them (default Turkey). The list stays on the user's PC.
- **Air band pack:** from the aerodromes' AIP **AD 2** PDFs the user downloads (in Turkey the DHMI eAIP); the
  **AD 2.18** table of each page is read (PDF text with PdfPig). The aerodrome code comes from the file name
  (`LT_AD_2_LTAC_en.pdf`) or the page.
- **Write:** one confirmation, then the CPS session (`C` 0, screen text, `C` 6 4; no "save settings"), the pack into
  the slot **not in force** with the next sequence number, its sectors as journaled sector writes (prepare, data,
  commit) **payload first, the header's sector last**, every sector read back, then `C` 5 (the radio checks its
  slots) and a check that the new pack is in force. A failure stops at once (no retry); the pack in force stays.
- **The guard** (`CpsPackPort`): reads only inside the region (and the radio information); after the plan is set,
  only the planned sectors, in plan order, each once, with exactly the planned bytes, committed only when whole; no
  reboot, no settings save, no EEPROM / WAV / `W` / `D` command. Tests: `CpsPackTests.cs` (a fake CPS radio as
  `cps_openfw.c` behaves, failures, refusals).
- **Same bytes as `tools/datapack.py`:** the builder (CSV rules, text folding, short names, callsign packing, AD 2.18
  parser, payloads, headers, slot choice, sequence, CPS order) is held to the Python builder on synthetic inputs
  (`Fixtures/make_datapack_fixtures.py` -> `datapack-results.json`, `DataPackTests.cs`). Run the script again when
  `tools/datapack.py`, `iddb.py` or `airfields.py` change. Only this program's builder tag differs
  (`DDM32Plus-win 1`, datapack.py writes `datapack.py 1`); with the same tag the bytes are equal.
- Never commit real RadioID or AIP data; the fixtures are made up.

## Health, live panel, channels

Three pages, ported from `pc/ddm32_app` (health_tab, live_tab, codeplug_tab) in 2.0.0-beta.2; details for the Rust
port in [docs/porting-plan.md](docs/porting-plan.md) "Health, live panel and channels".

- **Telsizin durumu / Radio health:** the CH340 check (Windows' device list, no port), then "Durumu oku" (one click,
  or every 30 s when ticked) reads the radio's health over the firmware's **debug link**: GET_VIEW tasks, crash and
  reset, exactly what `pc/radiotools/plus_txdiag.py --health` sends (`DebugReaders.Health`). The radio stays switched
  on and runs DDM32 Plus. A traffic light (red: crash record; amber: heap low-water under 1 KB or a stack under
  200 B; green), plain lines, the task stacks and the raw output under Details (also appended to
  `backups\health-YYYYMMDD.log`). A card shows the newest verified backup and the firmware it holds (files only).
- **Canlı panel / Live panel:** "Bağlan" opens the port and `DebugReaders.Live` (port of `plus_live.py`) reads every
  1-5 s until "Durdur": GET_VIEW core and tasks, the native modem status (0x21/0) and the P25 zone scan counters
  (0x21/12), crash and reset once. Signal (S-meter, the last 90 readings), state, mode, channel, frequency, P25
  receiver, health, scan counters. Three polls without any answer end the run. No log file (it reads every few
  seconds); the last lines are under Details.
- **The guard** (`DebugLinkPort`): every frame is parsed before it is sent (SOF, length, CRC) and only the pairs of
  command and one-byte payload in the reader's allowlist pass (health: `09/14 09/15 09/17`; live: those and `09/00
  21/00 21/0C`). Anything else, a damaged frame or a baud change is refused before transmission, so no TX, key,
  radio operation, flash, storage or register command can be sent. 115200 baud, DTR/RTS low, as the Python tool.
- **Arama kaydı / Search log** (`SearchLogPage`, `DebugReaders.SearchLog --reconnect`, `SearchLogBook`): GET_VIEW
  p25s / dmrs / alls every second; counts each call once by the radio's entry serials and call sequence; one CSV per
  session (`logs\search-YYYYMMDD-HHMMSS.csv`, BOM, Windows' list separator, formula-safe cells, a `-open.csv` side
  file while Excel holds it); Windows kept awake while it logs; a lost radio is waited for (port tried every 5 s,
  the table kept); a firmware without the search log is told (needs v1.0.0-beta.7). Details: docs/plus.md
  "Search log on the PC".
- **One radio step at a time:** a run on any page (backup, install, restore, data packs, health, live) holds the
  cable; another page's radio button waits (`ToolRunner.AnyBusy`). Closing the window stops the live panel.
- **Kanallarım / My channels:** the codeplug of a backup's data flash image (`q128-pass1.bin`; the newest verified
  backup loads by itself, "Başka bir yedek aç..." takes another), read only: channels 1-1024 in every mode (FM, DMR,
  P25 in Classic r13's RFKV form and the two older forms, NXDN, dPMR, YSF), zones (68 slots, 80 channels each; 16 with
  the old layout byte), contacts, RX group lists. The **P25 NAC is decimal first, as the firmware shows it now**:
  `659 ($293)`, `Herhangi 3966 ($F7E)` / `Any 3966 ($F7E)` (the Python viewer still writes `$293 = 659`). A search
  over every column (`659` and `$293` both find the channel), a zone filter (double-click a zone), a mode filter, all
  columns or the simple five, and "Gösterileni CSV olarak kaydet..." (the rows shown, every column, UTF-8 with BOM,
  as `codeplug.to_csv`). Nothing is written to a radio or a backup.
- **Held to the Python code:** `Fixtures/make_live_fixtures.py` runs `plus_live.run` and `plus_txdiag --health`
  with the pinned `rfoundry_debug.py` DebugLink over an in-memory fake radio (virtual clock) and records every frame,
  answer and printed line (`live-results.json`: 6 live and 5 health runs: ok, unavailable counters, VFO and crash,
  silent radio, noise and late answers, busy / error statuses, a view error, no answer); the C# readers must write the
  same frames and print the same text, and the parsers must give `parsers.live_summary` / `health_summary` /
  `health_tab.verdict` (`LiveTests.cs`). `Fixtures/make_codeplug_fixtures.py` decodes five synthetic images with
  `pc/ddm32_app/codeplug.py` (every mode and P25 form, a two-page RFKV chain, 1024 channels in 68 zones of 80, the
  old 16-channel layout, an empty one) into `codeplug-results.json.gz` (sparse images, every row and CSV);
  `CodeplugTests.cs` holds the C# decoder to it, with the NAC in the new form.
- **Screenshots and demos:** `--show health:demo` and `--show live:demo` show made-up readings (the page says
  "Örnek veri: telsize bağlanılmadı"; no port is opened), `--show channels:FOLDER` a backup's codeplug.

## Radio access and build flags

Three compile-time flags (`Directory.Build.props`); nothing at run time can change them:

| Flag | Release build and setup | What it does |
|---|---|---|
| `RadioAccess` | **true** | compiles the serial port in (`RadioAccess.Factory`): the read-only backup, the data pack read, health, live panel |
| `RadioWrite` | **true** | the tools that write get a port (`RadioAccess.WriteFactory`): install, quick update, install without backup, restore, data pack write. Needs `RadioAccess` |
| `Developer` | false | the developer-only features: the local release folder ("Yerel sürüm klasörü...", `--release-folder`), the backup folder choice, the title "DEVELOPER BUILD" |

Owner's decision 2026-11-21: installs are done with this program, so the release build reaches the radio.
`dotnet build -p:RadioAccess=false` gives a program that opens no port at all (its pages say so). The tests run the
release configuration and also pass with `-p:RadioAccess=false -p:RadioWrite=false`; they never open a real port (a
tool run without an explicit port factory gets none: `ToolContext.NoPort`).

Every guard stays in either build: `ReadOnlyPort` (backup), `InstallPort` (install, restore), `CpsPackPort` (data
packs), `DebugLinkPort` (health, live panel) check every frame before it is sent. Every radio step needs its own click and confirmation; a failed write is
never repeated.

### A developer build

```
dotnet publish src/DDM32Plus/DDM32Plus.csproj -p:PublishProfile=win-x64 -p:Developer=true -p:PublishDir=<new folder>\
```

Title **"DDM32 Plus 2.0.0-beta.2 - DEVELOPER BUILD - radio WRITE on"**; its settings live next to the exe (or in
`DDM32_DATA`). Publish each variant into its own new folder; never delete inside a publish folder (the developer builds
keep their settings next to the exe). The first real backup was compared with the Python tool's as follows (a local
compare; the private folder is never copied into the repository):

```powershell
$py = 'E:\GIT-REPO\open25-dm32\backups\dm32-047-plus-20261120-b'
$cs = 'E:\DDM32-dev-backups\dm32-20261120'          # the folder the developer build wrote
foreach ($f in 'progmem-pass1.bin','progmem-pass2.bin','q128-pass1.bin','q128-pass2.bin','stock-codeplug-pages.raw.bin') {
  $same = (Get-FileHash "$py\$f").Hash -eq (Get-FileHash "$cs\$f").Hash
  '{0,-30} {1}' -f $f, $(if ($same) { 'IDENTICAL' } else { 'DIFFERENT' })
}
```

### A signed release from a local folder (developer builds)

The toolbar entry "Yerel sürüm klasörü..." (only in developer builds) or `DDM32Plus.exe --release-folder PATH` reads
`ddm32-manifest.json`, its `.sig` and the zip it lists from a folder, as `pc/release/make_release.py build` writes
them. The folder stands in for GitHub (`LocalReleaseNet`), and the rest is the downloaded release's own code path:
the signature with the embedded release key, the manifest schema and tag, then on "İndir ve doğrula" the zip's size
and SHA-256 and the application inside. The version is listed in Sürümler and the version picker marked
"yerel / local". The release build ignores both.

`DDM32Plus.exe --show PAGE[:STEP[:BACKUP]]` opens a page (a wizard step, and a verified backup as "Mevcut yedeği
seç..." takes it) for screenshots and demos; it only navigates and reads files.

## Publish (one exe, nothing to install)

```
dotnet publish src/DDM32Plus/DDM32Plus.csproj -p:PublishProfile=win-x64
```

Output: `win/publish/win-x64/DDM32Plus.exe` (about 61 MB): self-contained (the .NET runtime is inside), single file,
compressed, x64. It runs on a stock Windows 10 (1809 or newer) or 11 with nothing installed.

## Setup (Inno Setup)

Install [Inno Setup](https://jrsoftware.org/isinfo.php) 6.3 or newer (7.1 builds it; checked 2026-11-20), publish as above, then:

```
"C:\Program Files\Inno Setup 7\ISCC.exe" installer\DDM32Plus.iss
```

(Inno Setup 6: `"C:\Program Files (x86)\Inno Setup 6\ISCC.exe"`, or under `%LOCALAPPDATA%\Programs` for a per-user Inno install). Output:
`win/publish/installer/DDM32Plus-Setup-<version>.exe`.

The program update zip for the signed manifest (`app_net`): `DDM32Plus-win-<version>.zip` holding only
`DDM32Plus/DDM32Plus.exe` (the published exe; nothing user-specific), given to
`pc/release/make_release.py build ... --app-net-zip ZIP --app-net-version <version>`, which checks it again.

The setup installs **per user** to `%LOCALAPPDATA%\Programs\DDM32 Plus` without an administrator prompt, adds a Start
menu and a desktop shortcut, and appears in Settings > Apps. Uninstalling removes the program, its settings and
downloads but **keeps `backups`** (the radio's backups) and tells the user where it is.

## Where the program keeps things

Everything in its own install folder:

| Path | What |
|---|---|
| `DDM32Plus.exe` | the program |
| `settings.json` | language, disclaimer acceptance, update check, window place |
| `backups\` | the radio's backups and logs (private: calibration, channels). Updates and uninstall never touch it |
| `releases\` | downloaded firmware releases and the verified manifest cache |
| `updates\` | program update staging, `rollback\` (the previous program files), `failed\` |
| `packs\` | data packs: the RadioID list you downloaded (`user.csv`), the packs built (`*.dpk`), the logs of pack reads and writes. Removed on uninstall |

Up to 2.0.0-beta.2 the folders were `yedekler`, `surumler`, `guncelleme` and `paketler`. At start the program moves
each one to its English name (a rename on the same disk; an old folder whose new name already exists stays, and its
backups are still offered).

`DDM32_DATA=<folder>` moves all of these elsewhere (tests, screenshots).

The Python program kept its data in `%LOCALAPPDATA%\DDM32Plus` (before the rename `DDM25Plus`). This program finds
those folders and shows where their backups are (home and "Geri dön" pages); it never moves or deletes them.

## Releases and signatures

- The release list comes from `https://api.github.com/repos/d0ukan/DDM32-Plus/releases` (HTTPS to GitHub hosts only;
  every redirect is checked again). TLS is Windows' own, so no extra certificate bundle is needed.
- A release is listed only when its `ddm32-manifest.json` verifies with the embedded Ed25519 key (key id
  `dda8c4a85c4600df`) and passes the same schema checks as `pc/radiotools/signed_manifest.py`. A firmware zip is used
  only when its size and SHA-256 equal the signed ones and the application inside equals `application_sha256`.
- The first release, `v1.0.0-beta.1`, still uses the old names (`ddm25-manifest.json`, format `ddm25-manifest/1`,
  repo `d0ukan/DDM25-Plus`). Its signature is verified and it is **listed as "Legacy (DDM25)"**, but this program
  does not install it.
- Labels: **Latest** = the newest stable, **Stable** = an older stable, **Beta**, **Test / Alpha** = a beta whose
  version is `-alpha…`, `-test…` or `-dev…` (asks "are you sure?" before use).
- All constants (format, repo, manifest name, key) are in `src/DDM32Plus.Core/Releases/ReleaseConstants.cs`.

### Program updates

The manifest's `app` entry is the Python program's zip. This program updates itself only from an **`app_net`**
entry (same fields as `app`: version, file, url, sha256, size, notes), so neither program can replace itself with the
other. The release tool writes it: `pc/release/make_release.py build ... --app-net-zip ZIP --app-net-version V`
(asset `DDM32Plus-win-V.zip`); the zip must hold `DDM32Plus/DDM32Plus.exe` and nothing named like the user's data
(`backups`, `settings.json`, `releases`, `updates`, the uninstaller). `pc/radiotools/signed_manifest.py` checks
the entry like `app`; programs from before ignore the key, so adding it breaks no published checker.

Update flow: check (on start unless switched off, or the button) → download → SHA-256 and size against the signed
manifest → safe unzip into `updates\app-VERSION\` → the new exe runs as the helper (`--self-update`), waits for the
old program to exit, moves only the files it replaces to `updates\rollback\`, copies the new ones in and starts
the new program; if it does not report back within 90 s, the old files are put back and the old program starts.
`backups` and `settings.json` are never touched.

## SmartScreen

`DDM32Plus.exe` and the setup are not code-signed yet, so Windows SmartScreen warns on the first run ("Windows
protected your PC"). Click **More info**, then **Run anyway** (Türkçe: **Ek bilgi**, sonra **Yine de çalıştır**).
The warning goes away once a code-signing certificate is used.
