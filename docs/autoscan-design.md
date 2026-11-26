# Auto airfield scan and auto repeater scan: design (job "autoscan", 2026-11-26)

Base `ddm25` 277d019 (v1.1.0), branch `autoscan`. Revised after the skeptical review (1 BLOCKER, 5 MAJOR, 13 MINOR,
6 NIT) and the owner's decisions of 2026-11-26; section F lists how each finding is resolved. Everything here is
host-tested only; what only the radio can prove is listed in D.5 and `docs/plus-radio-test.md` (AS1..AS12).

Owner's idea: from Home, (1) scan the air band frequencies of the airfields nearest to the GPS position; (2) scan the
nearest repeaters (FM, DMR; NXDN if possible), the list following the position while driving. Budget at the start:
~46 KB program flash free (854,972 of 880 KB), static RAM 206,752 of 211,456 B (4,704 B free).

## Summary

| Item | Decision |
|---|---|
| Engine | **No new scan engine.** Both scans fill the existing **multi-watch ring** (A..F, each its own VFO) through the generalised **air watch** path: the radio core keeps and restores the user's state, saves nothing during it, holds a letter on a signal and waits the Dual watch delay |
| Start | Two Tools entries, **Air scan** / **Repeater scan** (after Battery: no stored hold code moves), On / Off as a value, OK toggles; or a **held digit** (Settings > Keys). No digit by default. A held digit set to a scan works on a VFO too (not while typing a frequency) |
| Selection | 6 nearest; repeaters FM + DMR within **60 km**; airfields within **100 km**, their frequencies round robin (TWR, APP / DEP, AFIS / INFO / CTAF, GND, the rest), **ATIS left out** |
| Refresh | Once a second the UI looks; a refresh at the first fix, then every 30 s at most and only after moving 1 km (repeaters) / 3 km (air). A letter goes when beyond 110 % of the radius or a candidate is 20 % and 2 km nearer than the farthest letter. Never while a call comes in, the ring holds a signal or waits after one, or the radio transmits |
| PTT | While the ring holds or waits on a letter, **PTT talks on that letter** (the one heard); otherwise the selected letter as in multi-watch. FM: the listed input and access tone (as Tools > Repeaters "tune"); DMR: the listed slot (unknown / both: 1) and the user's DMR VFO talk target; air: refused as before (TXDENY_AM). No new TX or band rule |
| Home | The ring's rows; a letter's line 1 is its label: `TA2XYZ 12km`, `LTBA TWR` (line 2: mode and frequency) |
| RAM / flash | measured: **+336 B** RAM (207,088 of 211,456 B), **+5,940 B** flash (860,912 B) over v1.1.0 (D.2, D.3) |
| NXDN | Not in v1: the map pack has no NXDN rows and no RAN field (A.2) |

---

## A. What exists and what the data provides

### A.1 Reused code

| Piece | Where | Reuse |
|---|---|---|
| Multi-watch ring, max 6 letters | `UI_MW_MAX 6`, `RADIO_DUAL_SLOTS`; C..F own VFOs `vfox_x[]` | the scan engine: hop, hold on signal, wait `dual_delay_ms`, look at the others |
| Ring timing | `mwatch/radio_mwatch.h`: fast ring (n > 2) quiet FM / AM stop 50 ms, DMR 100 ms (3 readings) | dwell per letter (C.4) |
| Air watch (UI) | `air/ui_airwatch.inc`: keeps the user's watch in `mw_keep`, fills letters, starts the ring; its end restores | generalised: `mw_air_ring` (any letter kind), `mw_air_finish(user)` |
| Air watch (radio core) | `air/radio_airwatch.inc` `air_vfo`, `air_end`, `air_atis_poll`, `air_request` | letter kinds, replacement, talk back, session end (D.1) |
| Ring rows on Home | `mwatch/ui_mwatch.inc` `mw_row[k].name` (17 B) | the label (kept by `mw_row_read` while a scan runs) |
| Auto repeater | `map/mapnav.c` `ARPT_GAIN_PCT` 20 % / `ARPT_GAIN_M` 2 km, `ARPT_EVERY_MS` 30 s | the same hysteresis and cadence; its offers wait while a scan runs |
| Repeater / airfield readers | `rpt/rpt.c`, `air/airfield.c` on the map pack (`datapack/ref_pack.c`) | the key sections for the passes, a record only for a candidate |
| Hold keys | `sat/ui_hold.inc` | the Home start (no new key code) |

Not reused: the zone / VFO scan and the digital searches (a list of 6 frequencies with their own modes is what the
ring already is).

### A.2 The map pack's data

| Record | Fields used | Missing |
|---|---|---|
| RPT (44 B; 252 rows: FM 151, DMR 101) | position, `rx_hz` (output), `off` (1.25 kHz), `ctcss10` (access tone = TX tone), `mode` (FM / DMR / ... \| NARROW), `dmr` = CC \| slots << 4, callsign | RX tone; talkgroup; NXDN RAN; "CC unknown" (the generator writes CC 1 then, F m10) |
| RPT_KEY (10 B) | position, mode | the pass filters by mode without the record |
| AIR (44 B, 74 rows), AIR_FREQ (4 B) | position, `first` / `count`, code; channel, type | many small fields have no frequency |

**NXDN:** the firmware receives NXDN48, but the list has 0 NXDN rows, `rpt_tune_plan` refuses NXDN and the record has
no RAN. Later: a generator change (RAN in the `dmr` byte for NXDN rows) and a third letter kind. Not in this job.

---

## B. UX

### B.1 Start and stop
- Tools > **Air scan** ("Hava tarama", shown on AM-capable radios) and **Repeater scan** ("Röle tarama"): the value
  says On / Off; OK toggles and goes to Home. Starting one ends the other (and an airfield's Watch all).
- Settings > Keys: a digit can hold either (none by default). It works on a channel and on a VFO (the scan's letters
  are VFOs), not while a frequency is being typed. The two new Tools numbers (42, 43) come after Battery (41,
  unchanged), so a stored hold key keeps its tool (verifier fix; they were before it first).
- Refused with a toast: no map pack or none of the kind ("No repeater data: load the map pack" / the airfield twin);
  no GPS or GPS off in Settings ("GPS is off: switch it on in Settings"; the scan never switches it on).
- Stops: any way a watch stops (Back twice, # 5, * 7, the watch key), the same Tools entry or digit again, an airfield's
  Watch all, a zone / VFO scan or dual watch started, DOTA, cross-band, a power off (RAM only). The toast says
  "Repeater scan off" / "Air scan off"; VFOs A / B, the position and the user's watch come back as after the air
  watch. A stopped scan stays stopped (no refresh restarts it).

### B.2 States
| State | Ring | When |
|---|---|---|
| Off | the user's | default, after any stop |
| On, waiting | the user's (untouched) | no fix yet ("... on: waiting for GPS"); fewer than two letters within reach: "Repeater scan: none within 60 km" / "1 repeater only, waiting" (once per count) |
| On, ring | the scan's (2 letters: dual watch's pair; 3..6: the ring) | two or more letters |

Fewer than two after a refresh: the ring ends (the scan's own end: no "off" toast), the user's state comes back, the
scan waits. A fix lost: the letters stay, refreshes pause.

### B.3 Home
The ring's rows; a letter's line 1 is its label (`ELMADAG 12km`, `LTAI TWR`), line 2 its mode and frequency (one-line
layout: label and frequency). The label's distance is rewritten at each refresh. A replaced letter: toast `+YM2KA 9km`.

### B.4 On a hit
As the ring: a signal holds the letter; after it the Dual watch delay is the hang time (with looks at the others).
PTT in the hold or the hang time talks on that letter, and the PTT pill follows it ("Talking on D"). Up / Down step
the selected letter's VFO (until a refresh replaces it); the Change-letter pickers are refused ("The scan picks its
letters: stop it first").

### B.5 Refresh while moving
- Looked at once a second (`ui_autoscan_tick`); due (`as_due`): the first fix, then 30 s since the last selection
  **and** 1 km (repeaters) / 3 km (air) moved; a new map pack at once; a ring that should be there (it failed to
  start) after 30 s.
- Deferred to the next second while a call comes in, any ring or scan holds a signal / waits after one / probes,
  or the radio transmits. The radio core refuses a replacement (RADIO_AIR_REPLACE) in those states too; the UI reads
  the letter's VFO back each second and asks again (`retry`).
- Merge (`as_merge`): letters keep their places; a letter beyond 110 % of the radius (or gone from the pack) goes; a
  new candidate fills a free place, else replaces the farthest letter when 20 % and 2 km nearer. Then FM letters
  before DMR ones (`as_group`). Airfields: merged as airfields, their frequencies then taken again round robin.
- Same count: only the changed letters are asked for (one request each). Another count: the ring is asked for again
  (the selection goes back to A).

---

## C. Radio behaviour per letter

### C.1 FM repeater letter
FM VFO at the output, 12.5 / 25 kHz as listed, **RX tone off** (the listed tone is the access tone), TX = output +
listed offset, TX tone = access tone (a tone not in the CTCSS table: none). An input outside the TX bands stays: the
band check refuses PTT there as on any VFO (no rule of the scan's). Quiet stop 50 ms (fast ring).

### C.2 DMR repeater letter
DMR VFO, **the listed colour code** and **the listed slot** (unknown or both: slot 1), talk target: the user's DMR VFO
target as it was when the scan started (none chosen: talkgroup 9, as every DMR VFO). Not TS Any: that is RX only
(TXDENY_MODE) and would be a new TX limit (review M4); the other slot is not heard. Every talkgroup is heard as on a
VFO. Quiet stop 100 ms.

### C.3 Air band letter
As the air watch: FM VFO in the air band, received in AM, TXDENY_AM. 8.33 kHz channels via `air_freq_hz`.

### C.4 Dwell and order
The ring's own timing. An FM <-> DMR switch costs ~167-169 ms on the owner's radio (scanfast.h); the ring now counts a
stop's dwell from the end of a slow retune (`fsc_now` for MW_FAST, review M5), so the first DMR letter after FM gets
its full window. FM letters before DMR ones: two switches a round. Six letters 4 FM + 2 DMR: ~0.74 s a round
(estimate; radio test AS7).

### C.5 Coexistence
| With | Behaviour |
|---|---|
| Zone / VFO scan, dual watch | the radio core ends the air state first when one is asked for (`air_scan_first`); the UI's end that follows no longer stops it |
| User's dual watch / ring | kept in `mw_keep`, restored at the end |
| Searches, beacon, SSTV TX | refuse while it runs (`UI_AIR_ON`), as for the air watch |
| DOTA, cross-band | they end it before taking the radio (`air_session_end`, counted in `view.dual_stops` so the UI ends its side); fixes the air watch's old hole (review M3) |
| Auto repeater | its offers wait while a scan is on |
| Airfield Watch all | replaces the scan (the scan is off) |
| APRS, eco, emergency | as the ring today |

---

## D. Implementation

### D.1 Files
| File | Change |
|---|---|
| `map/autoscan.{c,h}` (new, portable) | `as_pick_rpt`, `as_pick_fields`, `as_air_letters`, `as_merge`, `as_group`, `as_due`, distances by the key sections |
| `map/ui_autoscan.c` (new) | state, toggle, the 1 s tick, letter requests and labels, retry, toasts |
| `air/ui_airwatch.inc` | `mw_air_ring` (letters with kind bits), `mw_air_finish(user)`, `ui_dual_auto_*` |
| `air/radio_airwatch.inc`, `tools/plus_air.py` | RREQ_AIR_VFO arg: bits 0-2 letter, 3-4 kind (AM / FM / DMR), 5 narrow, 6 replace, 7 ATIS, 8-13 FM tone index + 1 or DMR CC (8-11) / slot (12-13), 14-31 offset in 1.25 kHz signed: one request per letter. `air_talk_back`, `air_session_end`, `air_scan_first`, `air_talk` (4 B in radio_sm_t) |
| `mwatch/ui_mwatch.inc`, `ui_mwatch_draw.inc` | the label kept and drawn; pickers refused |
| `scanfast/radio_scanfast.inc` | `fsc_now` for the ring |
| `dota/radio_dota.inc`, `xband/radio_xband.inc` | `air_session_end` before the take |
| `datapack/ref_pack.{c,h}` | `ref_seq()`: the pack's sequence |
| `map/ui_mapnav.c`, `sat/ui_hold.inc` | auto repeater offers wait; `ui_hold_any` |
| `tools/plus_autoscan.py` (new, last prepare) | `CONFIG_AUTO_SCAN` (needs AIR_WATCH, REPEATERS, GPS, VFO_FM, VFO_DMR, DATAPACK), Tools entries, tick, PTT / scan hooks, hold digit on a VFO |
| `lang/tr.po` | 14 strings |

### D.2 RAM
207,088 B (+336 B over v1.1.0; the first build was +440 B before the trims): the UI state (`as_state_t` 96 B,
candidate and field arrays 96 B, request bits, label and toast buffers), the selection's static work arrays (kept off
the UI task's stack), 4 B in radio_sm_t. Labels live in `mw_row[].name` (0 B). 4,368 B stay free.

### D.3 Flash
860,912 B (+5,940 B over v1.1.0), 40.2 KB stay free.

### D.4 Stack
`--stack-check`: the UI task's deepest path is still Home's draw (1,636 B, 184 B free, unchanged); the tick path
(`ui_autoscan_tick` > `choose` > `as_pick_rpt` > the pack reader) stays below it (its frames 36-108 B; working
arrays static). The radio task unchanged (316 B free).

### D.5 Tests
Host (fakes only, never a COM port):
- `tests/plus/test_autoscan.c` (tools/test_autoscan.py): repeaters and airfield frequencies against brute force at
  9 cities and a grid over the country (places with none and one found), merge rules, a 2 x 70 km drive without
  flapping, FM before DMR, `as_due`; the prepared radio core's hooks (PTT, scan request, DOTA, cross-band) checked in
  the text.
- `tests/plus/test_airwatch_radio.inc` (tools/test_air.py): FM / DMR letters as listed (input, access tone, CC, slot,
  talk target), an input outside the bands refused by the band check, PTT on the held letter and in its hang time,
  else on the selected one, a replacement refused under a call / its wait / TX and taken once quiet, a zone scan asked
  for during it survives the UI's end.
- `tests/plus/test_ui_air.c` (CONFIG_AUTO_SCAN, English and Turkish): the ring with labels, Back twice stops it for
  good, moving refreshes (not while held), none / one within reach, GPS off / no fix, the air scan (no ATIS), a new
  map pack and one without repeaters, DOTA ending it.

Radio only (each serial / flash / RF step needs the owner's approval in chat): real reception on AM, FM and DMR
letters, PTT on a held FM letter reaching the repeater, the DMR letter's slot and target, the mixed ring's round and
the first DMR letter after FM (M5), refresh while driving, GPS loss, SPI read time, battery draw, DOTA during a scan.

---

## E. Owner's decisions (2026-11-26, final)

1. PTT during a scan talks to the letter being heard (held or in its hang time); none held: multi-watch's rule. A
   refresh never swaps a letter under a held / hang signal or a transmission. No new TX or band restriction.
2. 6 nearest; repeaters 60 km, airfields 100 km; FM + DMR together; ATIS excluded; no Home digit by default; no new
   settings menu.

---

## F. Review resolution

| Finding | Resolution |
|---|---|
| **B1** PTT went to the selected letter | radio core `air_talk_back` in `ptt_down`: the held / waited-on letter becomes the selected one before `dual_talk`; the UI's pill follows (`mw_adopt`). Refreshes deferred while held / hang / TX in the UI and refused in the radio core (RADIO_AIR_REPLACE). Tests: radio core, UI |
| **M1** labels not drawn, wiped by apply | `mw_row_read` keeps the name of a VFO letter while a scan runs; `mw_draw_row` draws a VFO letter's label. Stack measured (D.4) |
| **M2** 0 / 1 letters, end detection | three states (B.2); `mw_air_finish(user)`: a user / feature end turns the scan off (`ui_autoscan_ended`), the scan's own end keeps it waiting; one letter is "no ring" |
| **M3** DOTA / cross-band dropped RREQ_AIR_END | `air_session_end` in `dota_take` / `xb_take` (also fixes the airfield watch); counted stop for the UI |
| **M4** TS Any is a new TX limit | listed slot (unknown / both: 1) with the user's DMR VFO target; `DMR_ANY` not needed |
| **M5** FM -> DMR switch ate the DMR window | `fsc_now` counts the ring's stop from a slow retune's end (MW_FAST) |
| m1 map pack changes | `ref_seq()` generation; a new pack: full selection and a new ring; none of the kind: off |
| m2 encoding | one request per letter, 32-bit arg (D.1) |
| m3 race | radio core refuses replacements while held / hang / TX; the UI reads the VFO back and retries |
| m4 RAM | measured (D.2) |
| m5 hold key | the scans' digits work on a VFO (`ui_hold_any`); the hint on VFO letters stays hidden (deferred: Home's hint is on the deepest draw path) |
| m6 input outside TX bands | kept for listening; TX = listed input, refused by the existing band check (owner: no new refusal) |
| m7 replaced letter's state | `ui_dual_auto_set` resets age, receiving mark, pane snapshot; the radio core drops `dual_keep[k]` and the Busy bit |
| m8 user's ring keys | Change-letter pickers refused; Up / Down allowed (until a refresh) |
| m9 zone scan during it | `air_scan_first` in `scan_request` |
| m10 CC guessed as 1 | **deferred**: needs a generator flag (map pack format); documented in A.2 |
| m11 flash estimate | measured (D.3) |
| m12 end toast | by kind |
| m13 busy DMR repeater holds the ring | **deferred**: a per-letter hold cap would be new behaviour; the ring's existing rule applies (owner to decide) |
| n1-n6 | text corrected (C.2, C.4, B.3 distances refreshed, B.4 Up / Down on the selected letter, D.1 dependencies, D.4 dropped) |
