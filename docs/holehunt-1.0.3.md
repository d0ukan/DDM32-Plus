# Hole hunt: DDM32 Plus firmware v1.0.3 candidate (2026-11-25)

Scope: branch `ddm25` at 594898f, everything since the 1.0.2 release commits (a4b7bdb firmware 1.0.2, fa379fa
programmer 3.1.0): swprobe, rxgl, issk, scrfix, srchfix, rxnac, toolicons, byline (Rust app), xbmulti, xbfree,
scanslow, the study doc. Each branch had its own implementer and verifier; this pass looks at the whole release.
Nothing here was run on the radio; no serial port was opened.

## Verdict

**GO for a radio-test build** (install with `install_plus.py` after review, then the row order below), **not yet GO
for a public "stable" tag**: every new part is host-tested only, and the cross-band P25 / FM transmit paths and the
scan's P25 learning change what the radio does on the air or what it notices. No blocking defect found in the code.
The fixes on branch `hh103` are documentation and regenerated screenshots only; no code changed.

## Build and tests (merged head 594898f, WSL copy `~/ddm32-hh`, clean `build/radiofoundry-p25`)

| Check | Result |
|---|---|
| `build_radiofoundry.py --plus --stack-check --verify-rebuild` | rc 0, `compiler_warnings` 0, `forced_rebuild_identical` true, no unresolved symbols / high registers |
| FLASH | 897,336 B of 901,120 B: **3,784 B free (99.58 %)** (1.0.2: 884,576 B; +12,760 B this cycle) |
| RAM | 205,216 B of 211,456 B: 6,240 B free (floor 1,500 B) |
| Stacks (margin 128 B included) | radio 316 B free (1.0.2 merge: 324), p25-lab 324, ui 184, start 348, host 576, input 428, audio 1,620, idle 136 (least, unchanged) |
| bin SHA-256 | `730458029e16c9f4e6cec7c87df70f3acf0a925f021ca5b4b8faa660f2619ae8` (not in `PLUS_REVIEWED`; add only after the owner's review) |
| `tools/test_receive_audio.py` | pass |
| `tools/test_fastscan.py` | pass (510 zone checks, 0 failures; the one upstream check that assumes a 150 ms stop is listed, as before) |
| `tools/test_classic.py` | rc 0 (radio core 2,152 checks, faster scan radio core 4,144 checks 0 failures, Turkish catalogue 1,675 texts all translated and drawable, rxgl / rxnac / soft probe / xband / toolicons suites in it) |
| Rust `cargo test` / `clippy --all-targets -D warnings` / `fmt --check` | all pass (no failing test, no clippy warning, fmt clean) |

## What was checked across branches

- **Transmit paths (AGENTS.md rule 3).** No new user TX path. Cross-band keys only for an over the engine sends and the
  CW ID: DMR and FM through `ptt_down` (TX inhibit, battery, band, time-out), P25 through the native transmitter's PTT
  (`radio_sm_tx_check`). `p25_lab_live_begin` refuses the microphone while a P25 session's memory is lent unless the
  ring feeds it (`XB_P25 && !send`), so a P25 session never transmits live audio. `native_lab_request` refuses every
  PTT while `xband_on() || xband_held` unless the radio core itself keys (`xband_native_key`); `xband_held` covers
  `xb_take` to `xb_restore` including the up-to-5 s let-go of a P25 session's channel (80c1493). xband_check still
  requires both frequencies in 144-146 / 430-440 MHz; "segments free" lifts only the repeater, satellite and APRS
  segments (and the repeater list), PMR446 and out-of-band stay refused, DOTA's plan is unchanged.
- **Data chip (rule 2).** Nothing new writes. rxgl reads the lists and contacts (`codeplug_listen_list`,
  `codeplug_contact`) and stores the picked list in the channel record's existing byte through the editor's existing
  save; xband's new fields and "segments free" are RAM only; the soft probe uses heap (456 B for 0.45 s) and the ring.
- **Shared files.** `p25_lab.c`: the xband P25 ring (`xb_lent` 2) and rxgl's `channel_groups` do not overlap
  (`p25_lab_set_groups` refuses while the worker runs; a P25 session's channel goes through `channel_service`'s rxgl
  branch too, but with no list and Normal squelch: every talkgroup, see note N6).
  `native_lab.c`: rxgl in `channel_service`, the soft-probe gate in the search branch, `native_lab_scan_quiet` for the
  scan, the xband PTT gate: independent. `tr.po`: every new `TR()` string of the diff has a msgid; the one duplicate
  msgid ("OK send  Back", identical text) predates this cycle. `.gitignore`: every new tracked file is allowed; no new
  file is ignored.
- **Scan vs the rest.** The P25 learning keys on the zone position and is cleared at each scan start; rxgl changes
  only which talkgroups play, and a take that hears frames of any talkgroup still counts as "not quiet", so a list
  cannot make the scan learn a real P25 carrier as false strength. The soft probe runs only inside the P25 search.
  Cross-band stops a scan when a session starts (`xb_take`).

## Findings

### Fixed on `hh103` (documentation)

- F1. CHANGELOG: the rxnac entry (received NAC on Home) had landed in the released **v1.0.2** section; it is in the
  1.0.3 candidate. Moved to "Unreleased (v1.0.3 candidate)".
- F2. CHANGELOG: the candidate section listed only the scan fixes. Added cross-band modes / side values / segments
  free / typed frequency, P25 RX group list, soft probe (off by default), ISS hold-4, the screen fixes, the sizes.
- F3. docs/plus.md "ISS key on Home" said "v1.0.2 candidate"; it is not in 1.0.2. Now v1.0.3 candidate.
- F4. docs/plus-radio-test.md: rows IK-1 / IK-2 sat without a heading inside the VFO step table; given their own
  section. No radio rows existed for the RX group list, the NAC Any caller line or the screen fixes: added RG-1..3,
  RN-1..2, UI-1, and a pointer to SP-1..SP-4 (docs/p25-soft-probe-design.md).
- F5. Rust app: the Home screenshots in `rust/docs/screenshots` predated the byline (3ff4385); the ones `cargo test`
  writes are committed (40 PNGs, Home header "by d0ukan · TA2DOU").

### For the owner to decide (behaviour; not changed)

- O1. **Cross-band P25 -> P25 sends NAC 293 by default**, not the NAC it heard (0d04dac). With typed frequencies and a
  factory radio on NAC 659 set to selective NAC, the relayed over is not heard there until B's TX NAC is set (or a P25
  channel with NAC 659 is picked for B). Options: default TX NAC "as heard" (the xbmulti behaviour), or keep 293.
- O2. The cross-band P25 NAC rows take 0-9 only; A-F needs Left / Right one value a press (0x1C8: type 190, then 56
  presses). The picker brings a P25 channel's TX NAC but not its RX NAC, and a DMR channel's TX values but not its RX
  filters (RX stays Any). P25 has no RX TG filter. Ask whether the picker should bring the RX side too.
- O3. Cross-band DMR RX colour code and slot now default to **Any** (1.0.2: the shared CC 1 / TS 1): a session records
  any DMR over on that frequency, any CC and slot.
- O4. Scan P25 learning: while a channel's false strength is learned (2 min, renewed by takes), a real P25 signal less
  than 10 dB over that reading is noticed only through the noise reading. A very weak real P25 channel may be passed
  over for up to 2 minutes at a time. Rows SS-4 / SS-5 measure it.
- O5. FLASH has 3,784 B left. The next feature of any size needs a saving first (or a feature dropped).
- O6. The Rust app byline ("by d0ukan · TA2DOU", 3ff4385) is in `ddm25` but belongs to the next programmer release,
  not the firmware's notes. The byline commit left the 40 Home screenshots under `rust/docs/screenshots` stale
  (`cargo test` regenerates them with the byline); refreshed on `hh103` (F5).

### Notes (no action needed now)

- N1. A P25 cross-band over ends 1.5 s after the last frame (XB_REC_QUIET_MS: the ring has no end-of-call), so P25
  relays start that much later than DMR ones.
- N2. rxgl reads one list and up to 32 contacts at each take of a P25 channel with a list ("a few ms"); scan and
  multi-watch take often. Row RG-3 checks it is not noticeable.
- N3. `p25_lab_give_back` clears the whole audio FIFO inside a critical section (a few KB of memset); once per session.
- N4. The static stack graph's deepest radio-task chain now runs through `xb_key > xb_native_ptt > native_lab_request`
  (radio 316 B free); a static upper bound, still above the 128 B margin.
- N5. BER on Home on a NAC Any channel is dropped only in the last (narrowest) form, as documented.
- N6. A cross-band P25 session's channel is built in RAM by `xb_channel` (`listen_list` 0 after `memset` in
  `xb_take`, `p25_squelch` 0): the native receiver's take runs rxgl's `p25_rxgl_selective` / `p25_rxgl_apply`, which
  give "not selective" and no groups, so every talkgroup is recorded (as the xbmulti design says).

## Release-note callouts

1. Everything new is host-tested only; not run on a radio.
2. Cross-band now relays P25 -> P25, NXDN -> DMR and DMR / NXDN -> FM; P25 sends with the side's TX NAC (293 unless
   set or brought by a picked channel), not the heard NAC. DMR RX is Any CC / slot by default.
3. Cross-band # ("segments free") lifts repeater / satellite / APRS segment refusals for that session form only (RAM,
   off at power-on); outside amateur bands and PMR446 still refused.
4. Soft P25 probe is off by default (held 0 on the P25 search screen to choose it).
5. Scan: P25 zones should no longer take ~1.9 s a channel; a weak real P25 signal may be passed for up to 2 minutes
   while the false strength is learned. Mixed DMR / FM / P25 zones should now catch DMR calls.
6. P25 channels can use the codeplug's RX group lists (edited in the CPS or a DOTA pack, picked on the radio).

## Radio test order (each step only with the owner's approval in chat)

1. Install (backup or quick-update path per rule 4), boot, Home on FM, DMR, P25 channels; UI-1.
2. Scan, receive only: SS-1 (on 1.0.2 first, if still wanted as evidence), SS-2, SS-4, SS-5, SS-6, SS-7, SS-3.
3. P25 receive: RN-1, RN-2, RG-1, RG-2, RG-3.
4. ISS: IK-1, IK-2 (GPS time and TLEs needed).
5. Soft probe, receive only: SP-1, SP-2, SP-3, SP-4.
6. Cross-band (transmits; licence and the question): XB-1, XB-6 (refusals, no TX), XB-14 (segments free), XB-15
   (side values), XB-9 / XB-10 (DMR -> FM), XB-11 (NXDN -> DMR / FM, needs a source), XB-12 then XB-13 (P25 -> P25,
   set B's TX NAC to the factory radio's NAC first, see O1).
