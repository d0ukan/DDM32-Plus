# Hole hunt: DDM32 Plus firmware v1.0.4 candidate (2026-11-25)

Scope: branch `ddm25` at 38c3826, everything since the 1.0.3 test build `9763158` (the build on the owner's radio
now): scanseed (P25 false strength learned in one take, kept per zone), holdkeys (digits 0-9 held on Home open a tool
from Settings > Keys; settings record 392 -> 404 B), nacfmt (received NAC as "$hex/dec" on Home and the watch panes,
"P:" for unit-to-unit). Each branch had its own implementer and verifier; this pass looks at the whole release.
Nothing here was run on the radio; no serial port was opened.

## Verdict

**GO for a radio-test build** after the fixes on `hh104` and with VERSION bumped to 1.0.4 (it still says 1.0.3: the
radio's About would not tell the two builds apart). **Not GO for a public tag** until the radio rows below pass: every
part is host-tested only. Two defects fixed here: scanseed's 6 dB drop rule made the scan slower than 1.0.3 as soon
as the strength readings jitter by 2-3 dB (finding 3; would have been blocking: it defeats the release's own speedup),
and the empty-hold toast pointed to qdmr (finding 1). The rest are documentation fixes.

## Build and tests

WSL copies without rust/pc/win, clean `build/radiofoundry-p25`, `build_radiofoundry.py --plus --public --stack-check
--verify-rebuild`.

| Check | merged head 38c3826 (`~/ddm32-hh104`) | with the `hh104` fixes (`~/ddm32-hh104b`) |
|---|---|---|
| build | rc 0, 0 warnings, forced rebuild identical | rc 0, 0 warnings, forced rebuild identical |
| FLASH (of 901,120 B) | 899,144 B, 1,976 B free | 899,156 B, **1,964 B free** (1.0.3: 897,336) |
| RAM (of 211,456 B) | 205,560 B | 205,560 B (1.0.3: 205,216) |
| Stack free | start 312, host 564, radio 316, ui 184, p25-lab 324, input 428, audio 1,620, idle 136 (least) | same |
| bin SHA-256 | - | `eee37f90...bcd78cd6` (not in `PLUS_REVIEWED`; VERSION still 1.0.3, see finding 2) |
| `test_receive_audio.py` | pass | pass |
| `test_fastscan.py` | pass (zone checks 824, 0 failures) | pass (new jitter row: 6 slow stops at the start, 0 later, 0 after a restart; 1.0.3 build 10 / 20 / 10) |
| `test_classic.py` | rc 0 (Turkish catalogue 1,675 texts all translated and drawable) | CLASSIC2 |

## What was checked across branches

- **Settings record 392 -> 404 B.** Upstream's loader (`settings_init`) starts from `settings_defaults` and copies
  `min(stored, page_len)` bytes per page, so a 1.0.3 record (page 1 156 B) loads every field and leaves `hold_tool` at
  its default (4 = ISS); the load is counted `SETTINGS_UPGRADED`, a statistic only (no toast, no reset, no save until a
  setting changes). Downgrade: 1.0.3 copies 156 of the 168 B and later saves its 156 B page: only the hold keys are lost.
  Nothing else knows the size: `SETTINGS_PAGES` derives from `sizeof`, no CPS / hostlink command dumps the record, DOTA
  never sends settings (docs/dota.md), and no PC program (pc/, rust/, win/) parses the settings area (backups are whole
  flash images). The upstream radio-core factory reset byte count (432 -> 444) and the static assert are patched and
  pass. Keeping `settings_format` at 1 departs from pc/README "raise it when the stored settings layout changes"; it is
  defensible (no field moves, downgrade loses only the hold keys) and documented in docs/plus.md; release notes must say
  it.
- **Storage rule 2.** The hold keys are written only through `ui_svc_setting_set` -> `settings_begin/settings_end`
  (the existing store, its own CRC and commit). No data-chip access added. scanseed keeps its table in RAM only.
- **TX paths.** Untouched: the diff has no change in the PTT, keying, native TX or DOTA paths. A hold can open a tool
  that can transmit (CW beacon, SSTV, Cross-band, DOTA, Fox hunt) exactly as the Tools list does; nothing keys from a hold.
- **Hold vs the held-0 fast / classic switch.** `ui_home_key` asks `ui_fscan_home_key` first: while a scan runs a held 0
  still switches fast / classic whatever "0 hold" says; the short 0 skip is untouched. Documented now (docs/plus.md,
  CHANGELOG, radio row IK-9).
- **Hold hint vs the nacfmt caller line.** Different rows: the caller line is the P25 caller row (`\x1d` forms); the
  hint row's "# name" is the last caller from Recent. No shared buffer; `home_hint` 56 B, built in the UI tick (not a
  draw). The dual / ring hint reads only the cache (`ui_hold_hint(false)`).
- **tool_open from Home.** Checked every case: each pushes a screen, toasts, or (Scan, Air band, Last call replay) calls
  `ui_go_home` first; none pops the stack, so nothing pops Home. Air band from a hold moves the radio to the VFO (as from
  Tools); after that a held digit types it.
- **scanseed vs zone / codeplug changes.** The kept table is dropped on another zone, a codeplug serial change, or a
  due time further than 120 s away. Fast / classic share it (the switch does not clear it).
- **Stacks.** No task lost margin through a draw path; the 12 B bigger `settings_t` costs the start task 36 B and the
  host task 12 B (docs/plus.md said "deepest paths unchanged": corrected with the numbers).
- **Turkish.** New texts: the hold rows (" uzun", existing), the dual hint template ("# seçenekler  %  Geri durdur |
  # seç  %  Geri dur"), the toast (" boş: Ayarlar > Tuşlar", this pass). `test_classic.py`'s catalogue check passes.

## Findings

| # | Severity | Finding | Action |
|---|---|---|---|
| 1 | defect (UX, small) | A held digit with no tool (None, or a tool this radio lacks) still said "Speed dial N is empty: set it in qdmr"; qdmr cannot set a hold key, Settings > Keys can | fixed on `hh104`: "Speed dial N is empty: Settings > Keys" / "Hızlı arama N boş: Ayarlar > Tuşlar" (`tools/plus_satauto.py`, `tr.po`) |
| 2 | release blocker for the radio build (process) | `VERSION` is still 1.0.3: About / splash would show 1.0.3 on the 1.0.4 candidate | lead: bump before the radio build (not done here: release step) |
| 3 | **defect (scan, fixed)** | scanseed dropped a learned level when a reading was more than 6 dB **under** it, while the level is the stop's strongest reading. The model had no per-reading jitter. A throwaway model run with jitter added: +-2 dB already gave classic 22 slow stops at the start (5 expected); +-3 dB 31 at the start and 51 in 60-300 s (1.0.3: 20), i.e. slower than 1.0.3; +-4 dB classic 1,720 ms a stop from 30 s. Per-channel false strengths 5-10 dB apart with a 20-50 ms lag after a retune did not matter on their own | fixed on `hh104`: drop at `FS_P25_DROP_DB` 10 dB, refresh only within +-6 dB of the level (a level the readings sit 6-10 dB under expires after 120 s as in 1.0.3). Model +-3 dB: 6 slow at the start, 0 later, 0 after a restart; +-4 dB (10 dB drop alone): 8-10 / 0-3; foreign-carrier row 16 of 16 at jitter 0-5. New permanent model row (jitter +-3 dB). Jitter wider than +-4 dB stays a radio question (SS-8) |
| 4 | idea | On a NAC-Any channel Home's caller line now almost never shows BER (a 5-digit ID form with "$hex/dec" and BER is 30 characters; the row holds about 24) | CHANGELOG says so now; owner to decide if BER matters more than the decimal NAC there |
| 5 | idea | Settings > Keys lists all 41 Tools names, also SOS key (never a Tools row) and Battery without a voltmeter; picking them gives the toast | documented (CHANGELOG, docs/plus.md, IK-9); filtering the list is a later change, not a release fix |
| 6 | idea | The hint names only the first digit set ("4 ISS+"): the owner has to remember the others; dual watch cuts the name to 3 letters ("7 Sat") | as designed; release notes should say "+ = more digits set, see Settings > Keys" |
| 7 | idea | The scan speedup's evidence (one take per channel, no new takes after a restart) is visible only with `plus_live.py --scan-stats` and the native counters; by ear the owner can only tell "the first pass is quicker" | radio row SS-8 lists the reads |
| 8 | docs | CHANGELOG had per-branch costs but no total and no nacfmt cost; the IK-5 toast text; held 0 while scanning; downgrade note | fixed on `hh104` |
| 9 | flash | 1,976 B left of 901,120 B on the merged head, 1,964 B after this pass's fixes (+12 B): above 1 KB, but the next feature of holdkeys' size (+1,376 B) would leave under 600 B | callout |

## Release notes must say

- Host-tested only; the radio test rows below.
- Hold keys: Settings > Keys, last ten rows; default unchanged (4 = ISS); "+" in the hint means more digits are set;
  a held 0 during a scan stays fast / classic; downgrading to 1.0.3 forgets the hold keys, nothing else.
- NAC on Home: "$hex/dec"; on NAC-Any channels BER usually no longer fits on that line.
- Scan: the first pass over a P25 zone is about half as slow, later passes and restarts of the same zone stay quick;
  a codeplug change or another zone learns again.

## Owner radio test order (each serial step with the owner's approval)

1. **IK-0 before installing**: note language, theme, brightness, Settings > Keys rows, VFO A frequency and mode.
2. Install (backup or reuse path per AGENTS.md rule 4); check IK-0 after: all as before, "4 hold" = ISS.
3. RN-1 / RN-2 (NAC "$hex/dec", P: for unit-to-unit) with the other radio on NAC Any.
4. SS-8 then SS-5 (scan: first pass, restart, 3 minutes; every P25 over still caught). If native `takes` keeps growing
   after the first pass, finding 3 is the suspect: send the `--scan-stats` lines.
5. IK-1, IK-2 (ISS default unchanged), IK-3 .. IK-9 (hold keys), last IK-8 to restore the defaults.
