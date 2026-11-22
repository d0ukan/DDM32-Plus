<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# DDM32 Plus (working name until 2026-11-19: DM32 Plus)

Our own build on top of upstream Classic `0.1.0-rc1-r13` (`a13e27d`; until 2026-11-20 Classic r7, `19420686`).
Branch `ddm32` (the r7-based work: branch `plus`), remote `upstream` = `tylerwatt12/Open25-DM32`. Not published under
the Open25-DM32 Classic/Next names; runtime version string `dm32-plus-…`. Licensing: `LICENSING.md` (upstream files
MIT, Plus additions GPL-3.0-or-later, a Plus image GPL-3.0-or-later).

```sh
python3 tools/build_radiofoundry.py --plus --stack-check --verify-rebuild   # build/…/dm32-p25-plus/
python3 tools/build_radiofoundry.py --classic --feature APRS=y               # one-off RAM/flash experiment
make test
```

`--plus` takes the whole Classic preset first, the O3 speed codec and lean build included. Before 2026-11-17 it
did not (it set `classic` after the preset): that build's live P25 encoder needed ~41 ms per 20 ms frame and PTT
stopped after ~300 ms on the mic FIFO overflow (found on the radio with `tools/plus_txdiag.py`, read only).

## On the Classic r13 base (2026-11-20)

Every Plus feature was carried from Classic r7 to r13 (`git log r7..r13`: MIT relicence with the CAI library and
the fixed-point IMBE codec vendored, split RX/TX NAC (PR #14), RFKV-only P25 programming, outside-band RX-only
defaults, P25 capability in VERSION, the independent speaker FIFO on timer 6, P25 transmit timer / cleanup fixes).
Where upstream fixed or redesigned something Plus also touched, upstream's version is kept and Plus sits on top:

- **P25 channel store** (`prepare_p25_records`). Classic r13 keeps a P25 channel as a DMR base record (mode 1,
  receive only) plus an RFKV tag of kind P25 holding the P25 block (squelch, RX NAC, TX NAC with its explicit /
  inherited presence, talkgroup), and refuses every other mode byte (Start new). Plus uses that format for P25, so
  the upstream CPS and Plus read each other's P25 channels. Plus's own modes keep their records (YSF 0xA6, NXDN 0xA7,
  dPMR 0xA8): the tag paths take `RADIO_MODE_P25` only (YSF is a native-modem channel too), and the Start new gate
  ignores the mode bytes Plus reads. `CONFIG_P25_MODE2` now also reads the 0xA5 records of Classic r4-r7 and the
  r7-based Plus (as they read there: Selective squelch, TX NAC Same as RX); saving such a channel writes the RFKV form.
- **Separate RX / TX NAC** (owner's item 38): upstream's (`channel_t.p25_tx_nac_override`: 0 = Same as RX, else TX
  NAC + 1; the editor's "RX NAC" / "TX NAC" rows; `p25_lab_set_channel(rx, tx, tg, id, selective)`). Plus's own
  storage (0x5000 | NAC in an 0xA5 record's TX tone word) and its TX NAC row are gone. Kept on top: the VFO's TX NAC
  (RAM only), Home's VFO row "NAC $293>$659", `#` = Any (RX) / Same as RX (TX) and `# #` decimal entry on both rows,
  the CH_MODE 2 import's TX NAC. Upstream's rules apply: RX Any with Same as RX transmits with $293 (Plus used to make
  such a channel RX only, toast "Any NAC: RX only": gone; the VFO follows the same rule), an explicit TX NAC $F7E /
  $F7F is refused on save (Plus used to read 3966 typed there as Same as RX), unit IDs up to 16,777,214, an empty
  P25 Radio ID is "Default" (the radio's). CH_MODE 2's "hear every talkgroup" is now the Normal squelch (every group
  on the RX NAC, the talkgroup kept for TX) instead of talkgroup Any / RX only.
- **P25 CAI library** (`third_party/p25cai`, locked by `tools/sources.py verify-vendored`; `src/p25/p25.c` / `p25.h`
  are its caller binding). Plus's edits of the old p25.c moved: the unit-to-unit / emergency / other-LCF decode is
  `src/p25/p25_lcx.c` (GPL; the library's own `p25cai_lc_decode`, the frame decoded once more only for an LCF 0x03
  link control), and trunk following frames TSDUs with the library's control channel receiver
  (`p25cai_control_receiver`, as `voice.c` runs it): a TSDU comes whole at its microslot boundary, a protected TSBK
  (P bit) is dropped, "bad" counts dropped TSDUs. Its 140 B share the YSF framer's storage (a union in `p25_lab.c`;
  a trunk receive is never a YSF one).
- **Native P25 guard timer**: r12 gave timer 6 to the speaker FIFO and moved the native lease guard to timer 4, which
  APRS's AFSK owns (Classic refuses APRS with native P25). Plus builds with `HN_GUARD_ON_TIMER5`: the guard takes timer
  5 (`TIMER_BOARD`, unused on the DM-32); Classic keeps timer 4 and its refusal.
- **YSF transmit** (switch off) follows r8/r13's P25 live path: the microphone conditioning and a release wait with
  no timeout.
- **Build**: `--plus` installs its own .c files; a Classic build leaves them out and removes ones a Plus build left
  (the stack check reads every source in the tree). `tools/classic_standard.py`'s writes feed later transforms.

Measured (`--plus --stack-check --verify-rebuild`, zero warnings, forced rebuild identical): FLASH 840,764 B (r7 base:
833,604), RAM 210,584 B of 211,456 (872 B free; r7 base 210,136). What r13 added: the speaker FIFO's fourth block
+320, the C codec's sine table in RAM +1,024 against its smaller context -1,568, a microphone block +320, About's
buffer +240, `channel_t` +16 B per copy (P25 fields no longer a union with DMR's, the squelch, the TX NAC, the mode
tag: about 25 copies, +512), the editor's message buffer +64; Plus's TSDU receiver shares the YSF framer (0). Stacks:
UI 268 B free (r7 base 368: the deepest path is upstream's own RFKV read from the editor, Classic r13 itself has 288),
radio 760, p25 worker 328 (the C codec's synthesis path). Classic r13 (`--classic`) builds with the same RAM as
upstream's and its suites pass.

## RAM budget (DM-32: 211,456 B static RAM, the RTOS heap included)

Classic r7 reproduced bit for bit (`forced_rebuild_identical`): 310,460 B flash, 180,480 B RAM.
Cost of each switch Classic turns off, measured one at a time on top of Classic (2026-11-15):

| Switch | Flash +B | RAM +B |
|---|---:|---:|
| GPS | 1,440 | 400 |
| DMR_GPS (with GPS) | 752 | 0 |
| SFR | 10,400 | 1,600 |
| DMR_ROAMING | 12,364 | 2,336 |
| DUAL_WATCH | 20,044 | 5,480 |
| DMR_SIGNALING | 14,880 | 5,712 |
| DMR_EMERGENCY | 17,712 | 5,840 |
| CRASH_REPORT | 16,588 | 5,984 |
| DMR_SMS | 38,192 | 6,616 |
| HOTSPOT | 34,612 | 11,200 |
| ZONE_SHARE | 52,340 | 12,504 |
| APRS | 40,168 | 15,144 |

Combinations (shared code makes them cheaper than the sum):

| Set | RAM | Free |
|---|---:|---:|
| GPS, DMR_GPS, DUAL_WATCH, DMR_ROAMING, SFR, APRS, GPS_SKY | 206,472 | 4,984 |
| GPS, DMR_GPS, DUAL_WATCH, DMR_ROAMING, SFR, DMR_SMS, DMR_TMS, DMR_EMERGENCY | 197,936 | 13,520 |
| first set + DMR_SMS | overflow by 1,240 | — |

## Plus v1 preset

`GPS DMR_GPS DUAL_WATCH DMR_ROAMING SFR DMR_SMS DMR_TMS DMR_EMERGENCY AM_RX BCAST_FM APRS` on top of
Classic (since 2026-11-19 also `HOTSPOT` with `HOTSPOT_SPLIT=n`: "MMDVM hotspot" below), plus our own `P25_SCAN`, `SPECTRUM`, `FOX`, `CW_BEACON`, `LONE_WORKER`, `ALMANAC` and `NAV`: 462,180 B flash (51.3%),
205,664 B RAM (97.3%, 5.8 KB free), all task
stacks inside their limits, forced rebuild identical, `make test` clean (ASan/UBSan host suites).

How APRS fit: the cable diagnostics are sized for a product. `NT_TRACE_BLOCKS_CFG=1` (TX block trace,
8.4 KB at 192) and `P25_REPLAY_RECORDS=1` (bench replay, 2.3 KB at 64) gave back 10.7 KB; Classic keeps
the full sizes. RTOS heap (40.5 KB, `boards/dm32/freertos_board.h`): task stacks ~31.1 KB + control
blocks, FM ring and queues ~4.6 KB → ~3.8 KB spare by estimate; confirm `tasks.heap_min_free` on the
radio. APRS adds no task.

Largest remaining RAM users: `ucHeap` 41.5 KB, frame buffer 38.4 KB, P25 encoder 7.2 KB, voice 6.9 KB,
decoder 6.1 KB, P25 audio FIFO 5.8 KB, APRS state 7.2 KB + rings 6.2 KB. More digital modes later
need mode-exclusive sharing of these (owner agrees modes run one at a time).

### P25 scan (`CONFIG_P25_SCAN`, ours)

Classic skips P25 channels in a zone scan. Plus scans them on the FM path (the receiver's noise reading
finds the carrier, in a zone scan its signal strength too; the speaker stays muted on a P25 channel). When the
scan holds on one, the native P25 receiver takes it (`native_lab.c` `channel_service`) and gives it back:

| Case | Give back after |
|---|---|
| P25 voice heard, then silence | 1.5 s after the last voice; no cooldown, the scan's own wait for the reply follows |
| Zone scan: no P25 frame of ours at all (analog, another NAC, the carrier already gone) | 1.2 s; that channel is not retaken for 2 s, or for 8 s when the take before it heard none either; the scan moves on at once and passes the channel over meanwhile |
| Zone scan: frames, then none for 0.6 s, never voice (the take landed on a call's tail) | at once, no cooldown: the scan waits there for the reply |
| Frames go on without voice for us (another talkgroup, encrypted); dual watch and multi-watch without voice | 2.5 s; that channel is not retaken for 8 s |
| Just given back | no retake for 0.7 s, so the scan reads the channel again (a call that goes on is retaken) |
| User stops the scan, or presses PTT | the channel stays with the receiver / the transmitter |

Host tests: `tests/classic/test_scan.inc` and `tests/plus/test_zscan.inc` (built as `test_radio_scan` and
`test_radio_mwatch` by `tools/test_classic.py`).
P25 in dual watch uses the same take/give-back: the native receiver takes the P25 channel only
while the watch holds on a signal there (`SCAN_DUAL`, `SCAN_PH_HOLD`).

#### P25 in a zone scan: why calls were missed, and what changed (2026-11-19)

The owner's R3 result: a 3-5 s call was caught only on the 5th zone cycle; once caught, audio and resume were
fine. Found by reading the code and by a host simulation (below); nothing was measured on the radio yet.

| # | Cause | Sure? | Change |
|---|---|---|---|
| 1 | The worker (`p25_lab.c`) starts its receive over 0.5 s after the last valid frame and then reports "no voice heard". The scan's give back read that 0 as "this take never had voice": every call's give back was the no-voice one, with the 8 s cooldown after it. So after each call heard, the same channel was deaf for 8 s (the reply and the next overs lost), and the "1.5 s after the last voice" rule never ran. Dual watch and multi-watch had the same fault | code, reproduced on the host | `native_lab.c` keeps the take's last voice (and last frame) time itself |
| 2 | During a cooldown the scan still stopped on the channel's carrier (FM path, muted) and stayed until the carrier ended plus the resume delay, hearing nothing and scanning nothing else | code, reproduced | the radio core asks `native_lab_scan_stop()`; a channel in its cooldown is passed over |
| 3 | A take on a call's tail (too little left for voice) got the same 2.5 s + 8 s as a foreign carrier | code, reproduced | frames tell them apart: frames then silence = a tail, given back with no cooldown; no frame at all in 1.2 s = not ours, 2 s the first time, 8 s when it repeats |
| 4 | A P25 stop was found only by the FM noise reading. Upstream's gotchas 17.3: on this modem chip the noise reading stayed at its idle value during a DMR call (4FSK, like C4FM) while the strength rose; upstream added the strength for DMR stops only. If our receiver does the same on C4FM, a P25 stop sees nothing within the 150 ms dwell. P25 search finds its hits by strength | likely, **to be confirmed on the radio** (R3b) | a zone scan's P25 (and YSF) stop also counts the strength (DMR's scale: -100 dBm at squelch 5, 3 dB a level); the second poll of it in a row holds the scan |
| 5 | Late entry: voice needs the talkgroup (LDU1) and clear voice (LDU2), so it starts 0.36-0.54 s after the receiver is on the call | by design (`voice.c`: no audio before the encryption state is known) | not changed; with 1-3 it no longer costs a call |

Not changed: FM and DMR stops, the dwell, the hold / resume settings, dual watch and multi-watch stops (they
keep the 2.5 s limit; they gain only fix 1), P25 search, P25 channel receive.

Host simulation (`tests/plus/test_zscan.inc`): the real radio state machine and the real `native_lab.c` on the
fake board; the transmitter (HDU, LDUs, TDU), the FM path's readings and the worker are models. A zone of 8
(once round in 1.2 s) or 16 channels (2.4 s) with one P25 channel, 60 calls a cell at random phase; "caught" =
at least two LDUs played. Single = calls 12-20 s apart; conversation = the next over 1.5-6 s after the last.
Before -> after:

| Zone | Receiver's noise reading | Traffic | 2 s calls | 3 s | 5 s | Air time heard |
|---|---|---|---|---|---|---|
| 8 | follows the carrier | single | 100 -> 100 % | 100 -> 100 % | 100 -> 100 % | 72 -> 73 % |
| 8 | follows the carrier | conversation | 42 -> 100 % | 47 -> 100 % | 55 -> 100 % | 30 -> 80 % |
| 8 | deaf to C4FM (cause 4) | single | 0 -> 100 % | 0 -> 100 % | 0 -> 100 % | 0 -> 71 % |
| 8 | deaf to C4FM | conversation | 0 -> 100 % | 0 -> 100 % | 0 -> 100 % | 0 -> 76 % |
| 8 | around the squelch line | single | 93 -> 100 % | 98 -> 100 % | 100 -> 100 % | 67 -> 72 % |
| 8 | around the squelch line | conversation | 45 -> 100 % | 48 -> 100 % | 60 -> 100 % | 28 -> 77 % |
| 16 | follows the carrier | single | 55 -> 70 % | 100 -> 100 % | 100 -> 100 % | 56 -> 56 % |
| 16 | follows the carrier | conversation | 32 -> 65 % | 43 -> 100 % | 55 -> 100 % | 22 -> 64 % |
| 16 | deaf to C4FM | single | 0 -> 67 % | 0 -> 100 % | 0 -> 100 % | 0 -> 60 % |
| 16 | deaf to C4FM | conversation | 0 -> 65 % | 0 -> 100 % | 0 -> 100 % | 0 -> 68 % |
| 16 | around the squelch line | single | 42 -> 60 % | 90 -> 100 % | 95 -> 100 % | 47 -> 58 % |
| 16 | around the squelch line | conversation | 22 -> 63 % | 40 -> 100 % | 58 -> 100 % | 19 -> 65 % |

What is left is the scan's own round: a 2 s call in a 16-channel zone is over before the scan comes by a third
of the time, and the start of every call is lost for up to one round plus about half a second. The model's
receiver start time (60 ms) is a guess, and so are the three receivers; the radio decides (R3a-R3d).

On the radio, read only: debug link `0x21 [12]` returns a version word (1) and twelve counters (u32 each, counted
modulo 65536 since power-on): zone scan stops on a P25 channel, those with a carrier, those passed over in a
cooldown, takes, takes that heard a frame, takes that played voice, give backs by cause (call ended, tail, no
frame, frames without voice for us), and the last take's milliseconds to its first frame and to its first voice.
`tools/plus_txdiag.py` (lab repo) needs the query allowed and printed. How to read them:
`carriers` far below the calls made = cause 4 is not cured (noise and strength both miss it: check the channel's
squelch level and the strength the Home meter shows); `takes` without `synced` = the probe time (1.2 s) is too
short for this modem's start, compare `sync_ms`; `quiet` high on a channel that only carries our calls = the
same; `cooled` high = calls arriving inside a cooldown.

### Spectrum and close call (`CONFIG_SPECTRUM`, ours)

Tools > Spectrum (tool 9). The channel receiver sweeps a span around the frequency in use (250 kHz, 500 kHz,
1 MHz, 2.5 MHz; 32 points, 64 from 1 MHz up, 30 ms a point) and the screen draws each point's strength as a
bar, its peak hold as a mark, the sweep's floor (median) as a line. Close call: the strongest point at least
12 dB over the floor and -105 dBm or stronger is the hit, named under the plot.

| Key | Does |
|---|---|
| Up / Down, knob | center by a quarter span |
| Left / Right | span |
| OK | VFO to the hit (else the strongest point), sweep ends, Home |
| Back | sweep ends, screen closes |

While it runs the sweep owns the receiver (`radio_sm_poll` returns after it; speaker muted in `outputs`).
Any other user request ends it and tunes the channel again; so does the screen not reading it for 3 s
(Home, timeout). A P25 channel's native receiver waits while it runs. Code:
`integration/radiofoundry/spectrum/` (engine, screen), hooks in `tools/plus_features.py`. Tests:
`tests/plus/test_spectrum.c` (engine) and `test_plus_spectrum_sm` in `tests/classic/test_scan.inc`
(the real state machine). Not on the radio yet: the FD6818's RSSI after a 30 ms settle is unverified.

### Fox hunt (`CONFIG_FOX`, ours)

Tools > Fox hunt (tool 10): the channel in use's strength large in dBm and S units (S9 = -93 dBm), a bar with a
peak mark, and a "vario" tone in the speaker: 400 Hz every 700 ms when weak up to 2.4 kHz every 120 ms at
-45 dBm. Left / Right: virtual attenuator 0/10/20/30 dB (moves the scale so a strong signal near the fox still
changes the tone); OK clears the peak; Back turns the tone off. The tone stops by itself 5 s after the screen
was left. The tone is a new beep (`BEEP_TONE`, any pitch, `beep_set_tone`). Not on a P25 channel (the native
receiver owns it). Tests: `tests/plus/test_fox.c`, and the state machine part in `test_scan.inc`.

### CW beacon and SOS (`CONFIG_CW_BEACON`, ours; transmits)

Tools > CW beacon (tool 11): "DE <call> K", "CQ CQ DE <call> <call> K" or "SOS SOS SOS DE <call>", once or
every 30 / 60 / 180 s, keyed by the radio's CW ID keyer (FM carrier, CW tone keyed; Settings' CW speed and
pitch) on the channel's TX frequency. The callsign is the radio name's (Settings > My radio, 4-7 letters and
digits). Every channel TX check applies (TX inhibit, band, RX only, battery); a P25 channel, a hotspot or the
SFR role refuse it. PTT or any other user request stops it, a message on air is cut at once; an interval
beacon stays armed when the screen is left. Test: `Plus CW beacon` in `test_scan.inc` (once, periodic, PTT
cut, no callsign, TX inhibit, stop).

### Lone worker (`CONFIG_LONE_WORKER`, ours)

Tools > Lone worker (tool 12): Off, 5, 10, 15, 30 or 60 min (Left / Right; RAM only, off after power off). No
key and no PTT for that long: the TX time-out tone every 10 s for 30 s with "Lone worker: press a key", then
the emergency tone every 3 s ("Lone worker: ALARM") and the backlight on, until any key. Runs in the UI task
whatever screen is open (`ui.c`). Test: `tests/plus/test_lone.c`.

**On alarm** (Up / Down on the same screen; RAM only like the time, so **Sound only** after every power on: the
radio never transmits by itself unless the owner chose it for this session). The other choices are offered only
when the build has them and are shown in the warning color with "then the alarm and it TRANSMITS":

- **DMR emergency** (`CONFIG_DMR_EMERGENCY`): when the alarm stage starts, `lone.c` makes the request the
  emergency key makes (`ui_svc_emergency(true)` -> `dmr_emergency_start`), nothing of its own. Upstream then does
  what its emergency settings say (defaults: the emergency alarm CSBK 3 times, then an emergency call to the
  channel's talk target with the microphone keyed by itself for 10 s, then "in emergency" until canceled; the
  settings are changed over the debug link only). Its gates are upstream's: `data_can_send` (a DMR channel, the
  call stack running, not the hotspot, `radio_sm_tx_check` clean: TX inhibit, RX only, band, CC / slot Any, DMR
  search and trunk following; an own ID; not while roaming looks for a site), the data service sends the alarm
  only while PTT is up, and the hot mic call goes through `ptt_down` like a key press (its TX checks and the TOT apply). As
  upstream specifies, an emergency call does not wait for a busy channel.
- **APRS SOS** (`CONFIG_APRS_MSG`): `aprs_sos_set(true)`, what OK on Tools > APRS > SOS calls (that page's
  question is skipped: choosing the action was the consent). The engine sends the Mic-E emergency position at
  its next tick and every 2 minutes, on the APRS frequency with its own gates (callsign, an amateur APRS
  frequency, AFSK, a position, CSMA on a busy frequency).

The request is made at the alarm's beats (every 3 s) until it is accepted, then never again in that alarm: one
emergency / one SOS start per alarm. While it is refused the local alarm goes on and the toast and the Lone
worker screen say why: "Not sent: needs a DMR channel" (an FM or P25 channel selected, RX only, TX inhibit, no
ID...), "SOS not sent: no position", "SOS not sent: APRS not ready"; it goes as soon as the gate opens. Once it
went they say "DMR emergency sent" / "SOS position sent" (the SOS line only after the engine really sent one).
The first beat says "Lone worker: ALARM" (or the reason of a refusal known at once); the outcome shows from the
second beat (3 s).

**Cancel**: any key other than PTT ends the local alarm and cancels what the alarm started, as upstream's own
cancel does (`dmr_emergency_cancel`: the remaining alarm repeats are dropped, the hot mic is released, the radio
leaves emergency, toast "Emergency canceled"; SOS: off, toast "SOS off"). What was already sent stays sent: tell
the listeners by voice. PTT ends the tone only, so the emergency call can be answered with the emergency option
still set; the next other key cancels. An emergency or SOS the user started himself before the alarm is not
started again and is not canceled by that key. The time starts again after the key, as before.

Cost: FLASH 776,128 -> 777,200 B (+1,072), static RAM 208,024 -> 208,024 B (four one-byte states in the word
the timer already used), stacks unchanged (UI 368 B free). Host test: `test_lone.c` built a second time with
`-DLONE_TEST_TX` against upstream's unmodified `ui_svc_emergency.c` and `dmr_emergency.c` with a recording data
service and a fake APRS SOS (Sound only never asks; once per alarm; refusals and their reasons; the gate opening
in the alarm; key cancel, PTT no cancel; the user's own emergency / SOS left alone). Not done: the settings
store (the choice is not kept over power off, on purpose); a P25 or FM emergency.

### Sun & Moon (`CONFIG_ALMANAC`, ours)

Tools > Sun & Moon (tool 13): for the GPS position and the RMC date (now carried to `ui_position_t.date_dmy`),
sunrise / sunset, civil dawn / dusk, day length, and the moon drawn as it looks with its phase, lit fraction and
age. Left / Right: UTC offset in half hours (+03:00 first). The sunrise equation in single precision; checked
against published times within 3 min (Istanbul, Ankara, a polar day) and known 2024 moon phases
(`tests/plus/test_almanac.c`). Needs a GPS fix: DM-32 GPS is unverified upstream.

### Waypoints and Go-To (`CONFIG_NAV`, ours)

Tools > Go-To (tool 14): a compass rose with an arrow to the target, its distance and true bearing, and the time
to get there at the GPS speed. Targets: saved waypoints (OK saves the GPS position as WP1, WP2, ...; 8 kept, the
oldest replaced; OK held deletes the one shown) and every APRS station heard with a position. North up while
standing; heading up (GPS course) from 3 km/h. The DM-32 has no magnetic compass, so standing still the arrow is
relative to true north. Waypoints are in RAM only (gone at power off): persisting them needs a storage record
of their own. Great circle from the APRS engine. Test: `tests/plus/test_nav.c` (Istanbul-Ankara against a double
precision great circle, relative bearing, ETA).

### P25 search (`CONFIG_P25_SEARCH`, ours; receive only)

Tools > P25 search: finds P25 anywhere in a range whatever its NAC and talkgroup. Ranges: the frequency in use
+-500 kHz / +-1 MHz / +-2.5 MHz, 430-440 MHz, 144-146 MHz (Left / Right), 12.5 kHz steps.

| Stage | How |
|---|---|
| Sweep | the channel receiver steps the range, 30 ms a step (+-1 MHz: 161 steps, about 5 s a pass; 430-440 MHz: 801 steps, about 24 s), speaker muted |
| Hit | a step 10 dB over the noise floor (slow average of the quiet steps) and -115 dBm or stronger |
| Probe | the native P25 receiver takes that frequency with NAC F7E (any) and no talkgroup filter; valid P25 frames within 0.7 s (encrypted ones count too) = P25 |
| P25 | the call plays; the found list (8, newest first) gets the frequency, NAC, talkgroup and source; given back 1.5 s after the last frame and the sweep goes on from the next step |
| Not P25 | given back at once; that frequency is not probed again for 10 s |

PTT never transmits here: it is consumed and stops the search. Any other user request ends it and tunes the
channel; the search stops by itself 5 s after its screen was left. Code: `integration/radiofoundry/spectrum/p25search.*`,
`ui_p25search.c`, `native_lab.c` (`search_service`), hooks in `tools/plus_features.py` (`prepare_p25_search`).
The plus build (`CONFIG_SEARCH_SURE`) changes the hit, probe and skip rules: "Search reliability" below.
Tests: `tests/plus/test_p25search.c` (engine) and `test_plus_p25search_sm` in `tests/classic/test_scan.inc` (the real
state machine and native admission: sweep, any-NAC probe, found list, give back, skip, PTT never keys).

**Found list key (2026-11-21, owner: "how many frequencies, which NAC, which group have traffic").** The P25, DMR and
all digital searches keep ONE entry per frequency + mode + code (P25 NAC, DMR colour code, YSF DG-ID, NXDN RAN) +
group (P25 talkgroup, DMR / NXDN talkgroup or called ID; YSF none: its entry holds the last callsign), 8 at most. A
call there updates that entry (calls + 1, heard time, last source) and moves it to the top; a new key goes on top and
the oldest entry drops when the list is full. A report that doesn't know the code or the group yet (P25: the NAC reads
0xFFFF, no link control yet; DMR: no colour code / LC yet) goes to the entry the probe already holds, or the newest
entry of that frequency and mode it fits; the P25 search starts no entry without a NAC. When the group then turns out
to be another one within 1.5 s (`P25S_MOVE_MS`, `DMRS_MOVE_MS`: the call's link control came late, or a reply on the
other slot still carried the last call's talkgroup), the call just counted moves to its own entry. Two entries of one
frequency and mode therefore always differ in a code or group both know. Revisits read a frequency with several
entries once a round. The PC search log (`p25s` / `dmrs` / `alls` sections, `searchlog_view.c`) is unchanged.
Root cause of the beta.7 duplicate (159.125 NAC $178 TG 1 listed with 1 and 2 calls, the PC log flipping between
them): the P25 search keyed by frequency + NAC and took the NAC from `voice.active_nac`, which the decoder resets to
`P25_NAC_ANY` (0xFFFF) after a terminator (TDU) and a frame that failed, while the frame counter still moved; that
report started a second entry for the same frequency, with the talkgroup and source of the last caller (taken even
when no new caller had been heard on this frequency, `native_lab.c` now compares the caller sequence with the take).
Tests: `test_found_key` in `tests/plus/test_p25search.c` and `test_dmrsearch.c`, the key block in
`tests/plus/test_allsearch.c`, the terminator and stale caller steps of `test_plus_p25search_sm`.
The search screens' Up / Down pick keeps the entry's key too (not only its frequency), so every entry of a
frequency can be picked and saved. Budget (`--plus --stack-check --verify-rebuild`: zero warnings, forced rebuild
identical, the least free prvIdleTask 136 B): `ddm25` 8e4647d FLASH 844,896 B, RAM 209,608 B (1,848 free); this
branch FLASH 845,652 B (+756), RAM 209,640 B (+32, 1,816 free).

The channel scan of saved P25 channels (P25 scan above) with NAC F7E, TG 0 and RX only also listens to any NAC and
talkgroup; its dwell is 150 ms a channel (Settings: scan dwell).

### DMR search (`CONFIG_DMR_SEARCH`, ours; receive only)

Tools > DMR search: finds DMR anywhere in a range whatever its color code, timeslot and talkgroup. Same ranges and
keys as P25 search (Left / Right, OK start / stop), 12.5 kHz steps. The board stays in DMR the whole time; the call
stack is set to hear everything (hear all, any color code, either slot: the `CONFIG_DMR_ANY` filters), direct mode
framing.

| Stage | How |
|---|---|
| Sweep | a step is read three times, a poll (25 ms) apart, 30 ms after the retune; its level is the peak (a direct mode burst is on air 30 ms in every 60, so one reading lands in it). About 100 ms a step: +-1 MHz (161 steps) about 16 s a pass, 430-440 MHz (801 steps) about 80 s |
| Hit | the peak 10 dB over the noise floor (slow average of the quiet steps) and -115 dBm or stronger |
| Probe | the frequency is held; the modem's bursts with a good check since the retune are counted before any filter (`dmr_call_heard`, a hook in `rx_burst`) |
| DMR | two bursts within 600 ms (a repeater sends every 60 ms; a direct mode call has a sync once per 360 ms superframe). Noted: frequency, color code (the latest burst that names one), slots with traffic (named by BS or TDMA direct sync), RPT (BS sync seen) and / or DIR (MS sync), the last call's talkgroup and source |
| Call | a voice call plays (any talkgroup); held while it lasts and 2 s after it for a reply, then the sweep goes on from the next step. No call to play within 1.5 s of the hit (an idle repeater, data, an encrypted call, which is marked): on, probed again on the next pass |
| Not DMR | after 600 ms; that frequency is not probed again for 10 s |

The plus build (`CONFIG_SEARCH_SURE`) changes the hit, probe and skip rules of this table and of the all digital
search: "Search reliability" below.

Found list: 8 entries, newest first ("433.5000 CC1 TS2 RPT 3m", then "TG 2620 from 2620123"), kept across range
changes; every 16 steps one find in the range is read again (round robin), so a new call there is caught in about two
seconds. Up / Down pick a find; OK on it stops the search and puts the VFO there in DMR with its color code and its
slot (when one slot was seen). A repeater find puts the VFO on its output frequency, simplex: set the TX offset
before talking through it.

Receive only: while it runs `radio_sm_tx_check` refuses everything (PTT, data and ACKs, an SFR repeater role,
roaming probes) and no APRS job starts. PTT is consumed and stops the search (nothing keyed until the next press);
any other user request, or any retune of the channel (`tune()`), ends it and tunes the channel again; the search
stops by itself 5 s after its screen was left. A P25 channel's native receiver lets go when the search starts and
takes the channel back after it. Code: `integration/radiofoundry/spectrum/dmrsearch.*`, `ui_dmrsearch.c`, hooks in
`tools/plus_features.py` (`prepare_dmr_search`, after `prepare_dmr_any`), two guards in `native_lab.c`. Tests:
`tests/plus/test_dmrsearch.c` (engine) and `tests/plus/test_dmrsearch_sm.inc` (the real state machine with the
simulated modem: hear-all stack, sweep, found list, call played, hang, resume, PTT never keys, stops, P25 channel
kept off; built without the P25 search).

### All digital search (`CONFIG_ALL_SEARCH`, ours; receive only)

Tools > All digital search (Turkish "Tüm dijital arama"; tool 39, after CW decoder; Tools > ISS (40) follows it, then Battery (41); no other tool's number moved):
finds P25, DMR, YSF and NXDN anywhere in a range. Same ranges, step and keys as P25 / DMR search (Left / Right, `*`
and digits open the range editor, OK start / stop, Up / Down pick a find, Back stops). On a hit it probes the modes
in the owner's priority order **P25 > DMR > YSF > NXDN**; the first mode confirmed wins.

It is the DMR search's engine with a second kind (`dmrsearch.c`: the sweep, the hit rule, skip list, revisits, found
list, call, hang and resume are the DMR search's, unchanged) and the DMR search's screen with a flag
(`ui_dmrsearch.c`). The two kinds share the engine's RAM: starting the other kind clears the found list, and a
snapshot or peek of the kind not running reads as stopped with nothing found (so the other screen keeps nothing
alive). Each mode is probed with the receiver this tree already has for it; there is no receiver that hears all four
at once:

| Stage | How |
|---|---|
| Sweep | on the FM path (as P25 search), read as the DMR search reads (three readings a step, so a direct mode DMR burst is caught; with `CONFIG_SEARCH_FAST` its quick first reading) |
| P25 (first) | the native C4FM receiver takes the frequency with NAC and talkgroup Any (the P25 search's take and give back in `native_lab.c`); confirmed by a valid P25 frame (any NAC) within 700 ms of the take |
| DMR | the board in DMR mode, the call stack hearing every color code, slot and talkgroup (the DMR search's `dmr_configure` while `dmrs_on`); confirmed by two bursts with a good check within 600 ms of the setup |
| YSF | the native receiver again, in YSF mode with DG-ID Any (`p25_lab_set_ysf`); confirmed by a frame whose FICH held within 700 ms of the take |
| NXDN | the FM path with the software 4FSK modem (`radio_nxdn_poll` on the probe's frequency, RAN Any); confirmed by frames with a good LICH within 900 ms |
| None | the frequency is skipped as a non-digital carrier (10 s, 30 s when found again soon after with the faster sweep), the sweep goes on |
| Found | as the DMR search: a voice call plays (any NAC / talkgroup / DG-ID / RAN), held while it lasts and 2 s after it; nothing to play within 1.5 s of the confirmation (an idle repeater, a control channel, data, encrypted): on |

A carrier that is none of the four costs the four windows plus the hand-offs, about 3 s, then it is skipped; one of
them is found within its own window after the earlier modes ran out (DMR after about 0.7 s, YSF about 1.4 s, NXDN
about 2.1 s). A frequency already in the found list is probed in its own mode first (the rest in order after it), so
revisits of a DMR, YSF or NXDN find don't wait for P25. A mixed mode frequency gets an entry for each mode.

Found list: 8 entries, newest first, with the mode as a field:

| Mode | First line | Second line |
|---|---|---|
| P25 | `433.5000 P25 NAC $1C8/456 3m` | `TG 655 from 1234567` |
| DMR | `438.7125 DMR CC1 TS2 RPT 3m` | `TG 2620 from 2620123` / Encrypted |
| YSF | `439.1000 YSF DG 0 3m` | `from TA2DOU` (the header's source callsign, packed base 40 into the entry's two numbers: no extra RAM) |
| NXDN | `438.8000 NXDN RAN 5 3m` | `TG 101 from 1234` / Encrypted |

OK on a find stops the search and puts the VFO there in the find's mode (every mode in the VFO): DMR with its color code
and slot as the DMR search does; P25, YSF and NXDN with NAC / DG-ID / RAN Any. No find save on this screen (the find
save writes P25 and DMR channels only).

Receive only, as the DMR search: `radio_sm_tx_check` refuses everything while it runs and no APRS job starts; PTT is
consumed and stops it (during a native P25 / YSF probe `native_lab.c` hands the receiver back first); any other user
request ends it and tunes the channel again, including the DMR search's request (that one then starts with its own
list); it stops by itself 5 s after its screen was left. The FM path's listeners (FM caller ID, pager, dPMR, the
channel's own NXDN) keep off while it runs.

PC search log: a third debug link section `alls` (30), read like `p25s` / `dmrs` (read only, not counted as a look):

```
alls.on=0|1 alls.phase=N alls.mode=P25|DMR|YSF|NXDN alls.at=HZ alls.lo=HZ alls.hi=HZ alls.n=N
alls.fI=HZ,MODE,CODE,TSMASK,SYNCMASK,GROUP,ENC,DST,SRC,CALLS,AGE_S,CALL
```

CODE is the P25 NAC, DMR color code, YSF DG-ID or NXDN RAN (65535 = not known); TSMASK / SYNCMASK as `dmrs` (DMR only);
DST / SRC the talkgroup and source (YSF: 0); CALL the YSF callsign (the last field, empty otherwise). The PC programmers
(C# `SearchLogBook`, Rust `searchlog.rs`) poll all three sections and log a row per new call; the CSV gains a last
column `caller`. The `dmrs` section still prints CC 255 for a color code not named yet.

Cost (plus build): flash 840,664 -> 844,360 B (+3,696), static RAM 209,568 -> 209,592 B (+24; 1,864 B free,
the 1,500 B rule kept; radio task stack 728 B free) (two engine bytes, the native take's kind and caller sequence, the
YSF DG-ID, the screen's flag; the found entry stays 24 bytes: the NAC widened the code to 16 bits, the flags became
bits). Code: `integration/radiofoundry/spectrum/dmrsearch.*` (the kind, the order, the windows), `radio_alls.inc` (the
radio core's side, included into `radio_sm.c`), `ui_dmrsearch.c` (`ui_allsearch_screen`), `native_lab.c` (`all_heard`,
the PTT guard), `p25_lab.c` (`p25_lab_ysf_dgid`), `searchlog_view.c`; hooks in `tools/plus_alldigital.py`. Tests:
`tests/plus/test_allsearch.c` (engine: order and windows, every mode's find, call and hang, skip, the find's mode first,
mixed mode, hand-offs, kinds, callsigns; with and without the faster sweep) and `tests/plus/test_allsearch_sm.inc` (the
real state machine and native admission: FM sweep, native P25 probe found / played / hang / give back, DMR after P25 on
the board's DMR mode, PTT never keys, the other search's request, the idle stop). Radio test rows AD-1.. in
`docs/plus-radio-test.md`.

What only the radio can confirm: the sweep's readings on the FM path catch a DMR direct mode carrier as well as the DMR
search's readings in DMR mode; the FM / DMR board mode switch inside a probe (the DMR stack's start and its first
bursts within 600 ms); the native receiver's lock within 700 ms for P25 and YSF; the NXDN modem's acquisition within
900 ms (NXDN receive itself is still unverified on the radio, docs/nxdn.md).

### Search reliability (`CONFIG_SEARCH_SURE`, branch `searchfix`; needs SEARCH_FAST)

The owner's beta.7 report: P25, DMR and all digital search "skip a lot and miss"; a DMR search over a typed range of
about 436.000-436.500 MHz did not catch the other radio sending DMR direct mode (436.275 or 433.200, CC1, TS1, TG 1,
both radios on LOW power in the same room), and the stepping was slow. Audited end to end (engines, radio core
hooks, native probe) and reproduced in a host model (`tests/plus/test_search_sure.c`, below). Nothing was measured on
the radio: the rows SX-1.. in `docs/plus-radio-test.md` say what to read there.

**Root causes, most likely first** (the model's figures before the fix in brackets):

| # | Cause | Effect |
|---|---|---|
| 1 | **A strong nearby carrier.** A radio a few metres away reads strong 12.5-50 kHz off its frequency too (adjacent channel spill, an overloaded front end: a plateau of equal readings). The sweep goes upwards, so it met the carrier's lower skirt first and probed every strong step one by one: 0.6 s each, not DMR (off frequency), skipped 10 s / 30 s. The 8-entry skip list overflowed on a plateau, so the same steps were probed again every pass, and the real channel was reached seconds later, often after the over had ended | owner's case (436.275 in 436.000-436.500): found in 60 s 169 of 200, median 21.5 s, 57 probes a minute; all digital search 84 of 200 |
| 2 | **The floor could lock up.** It started at -125 dBm and only quiet steps moved it. A receiver whose quiet reading sits at -115 dBm or above (the FD6818's formula has no board correction on the DM-32; other boards read 20-40 dB high) made every step a "hit" against -125: no step was ever quiet, so the floor never learned. Every step was probed and skipped: the sweep stood still ("slow"), the screen's hits climbed, the real signal was met only by chance | floor -112: P25 28 of 200, DMR 71, all digital 0; sweep 0 steps a second |
| 3 | **Signals off the 12.5 kHz grid.** A signal on a 6.25 kHz raster (x.xx625), or any channel when the typed range starts off the grid (436.010 + n x 12.5 kHz is 2.5 kHz off every channel), reads on two steps but decodes on neither: probed off frequency, skipped | half step: P25 0 of 200, DMR 0; owner's case typed from 436.010: 59 of 200 |
| 4 | **Probe windows short for late entry.** A direct mode voice call has a sync once per 360 ms superframe; one sync lost and the stack's start leave 600 ms short. The P25 search's native receiver had 700 ms, the zone scan gives the same receiver 1.2 s (its start is "under 700 ms", the tail is what was lost). A failed probe of a real channel skipped it 10 s, 30 s when it failed again within a minute | part of every row above |
| 5 | **An over ending during the probe** (or before the native receiver's start) left nothing heard: the real channel was skipped for 10-30 s, so the next over there was missed too | short overs |
| 6 | All digital: a hit is probed P25 first (0.7 s and the hand-offs), then DMR after the FM -> DMR switch: a short DMR over was often over before its DMR probe began | DMR direct 156 of 200, median 17 s |

Not a cause: the reading's lag after a retune. The radio task polls every 25 ms; a step is read at the poll after
the retune at the earliest and the FD6818's strength reading is a few ms behind (BK4819-class part); SEARCH_FAST's look
back already covers a slower one (the model's tau 15 ms rows). The 75 ms DMR step is the floor for direct mode with
25 ms polls (three readings so one lands in a 30 ms burst whatever its phase); a faster one would need the 10 ms tick
to read the level (upstream's radio task), left for later.

**What changed** (the engines `p25search.c` / `dmrsearch.c` with `searchsure.h`, both kinds of the DMR engine):

| | Now |
|---|---|
| Floor | learned from the first step (that step makes no hit), falls at once to a lower reading, follows quiet steps slowly, rises only 1/4 dB per strong step (a long transmission or a cluster of strong neighbours can't lift it). A run of 16 equal strong steps (200 kHz flat) is the band's level raised, not a carrier: the floor takes it |
| Run | a strong step is not probed at once: the sweep goes on while the next steps read as strong or stronger (within 4 dB of the run's strongest; a stronger step starts a new run) and then probes **the middle of the strongest run**: the peak of a strong carrier with its neighbours raised, the middle of an overload plateau, the half step between two equal steps (436.28125 at 12.5 kHz). The middle is read again (all three readings) before the probe; quiet there now (the over ended): on, nothing skipped |
| Skirt | after a probe, the steps up to 50 kHz above the run that read 10 dB under it are that carrier's: not probed (4 s, the same pass) |
| Grid | the steps are multiples of the step (436.010-436.500 at 12.5 kHz reads 436.0125 .. 436.2750 ..) |
| DMR probe | the radio core hands the engine the level on every probe poll (`dmrs_heard_t.rssi_dbm`). A carrier whose level rises and falls as a direct mode one does, or that brought a single burst, is held `DMRS_PROBE_LONG_MS` 1.1 s (two superframes and the start); a repeater or a steady carrier 0.6 s as before |
| P25 probe | 1.2 s (the zone scan's figure); the radio core reads the level while the native receiver probes (`p25s_level`) |
| Skips | only a carrier that stayed on air for the whole window with nothing of the mode heard is skipped: a carrier that went off the air (its level under the hit's half margin for 200 ms) is given back at once, never skipped, no other mode tried; one burst / frame heard: never skipped. A skip covers half a step either side (a run's middle may come out half a step off next pass) |
| All digital | a hit whose readings rose and fell (TDMA) is probed as DMR first (P25 can't be it); a steady one in the owner's order P25 > DMR > YSF > NXDN as before. P25 window 1.2 s |
| Screen | the sweep line shows the steps a second ("Sweep 436.2750  13/s"; left out when the line has no room); "Pass N  hits H  floor F" as before. Together the three figures tell the owner's report apart: hits climbing on a quiet band = the floor; steps a second far under 13 (DMR) / 40 (P25) = probes |

Unchanged: receive only (PTT stops the search, nothing keyed), the stop 5 s after the screen was left, the found list
key, the revisits, the PC search log sections (`p25s`, `dmrs`, `alls`), and the engines without the switch (the
model builds both).

**Host model** (`tests/plus/test_search_sure.c`, run by `tools/test_searchfix.py`; the same engines built with
SEARCH_SURE 0 and 1): the radio task's polls (25 ms +-1), the receiver (noise +-1 dB at -124 dBm or -112; selectivity
6.25 kHz 6 dB, 12.5 kHz 30 dB, 25 kHz 45 dB, 37.5 kHz 55 dB, 50 kHz 60 dB, further 80 dB; a reading ceiling of -45 dBm
for a radio in the room; a first-order lag tau 5 ms, 15 in one row), the DMR modem (within 1.5 kHz only; from 60 ms
after a retune, 150 ms after the all digital search's FM -> DMR switch; three headers then a sync every 360 ms, each
caught with probability 0.85; a repeater's every 60 ms), the native P25 receiver (start 250-1000 ms, a frame every 180
ms). The transmitter keys 5-20 s overs with 3-10 s pauses from a random moment (TDMA phase random); 200 transmitters a
row at a random step of +-1 MHz (433-435 MHz) unless the row says; found = in the found list within 60 s.

| Row | Found of 200 | Median s to find | 90 % within s | Probes a minute | Steps a second (no transmitter) |
|---|---|---|---|---|---|
| p25-overs | 184 -> **200** | 8.1 -> **3.7** | 28.0 -> 10.6 | 10.1 -> 12.9 | 40.0 -> 40.0 |
| p25-close | 89 -> **200** | 19.4 -> **4.7** | 99.0 -> 12.9 | 41.8 -> 13.1 | 40.0 -> 40.0 |
| p25-highfloor | 28 -> **200** | 41.8 -> **4.0** | 99.0 -> 10.7 | 67.6 -> 12.3 | 0.0 -> 40.0 |
| p25-half | 0 -> **200** | 99.0 -> **4.0** | 99.0 -> 11.2 | 6.8 -> 12.7 | 40.0 -> 40.0 |
| p25-busy | 145 -> **195** | 18.6 -> **11.7** | 99.0 -> 31.6 | 52.8 -> 29.4 | 6.3 -> 12.7 |
| dmr-owner-436275 | 169 -> **195** | 21.5 -> **2.5** | 99.0 -> 19.2 | 57.4 -> 17.5 | 13.3 -> 13.3 |
| dmr-owner-offgrid | 59 -> **196** | 21.9 -> **2.5** | 99.0 -> 13.8 | 57.8 -> 16.3 | 13.3 -> 13.3 |
| dmr-owner-433200 | 0 -> **0** | 99.0 -> **99.0** | 99.0 -> 99.0 | 57.7 -> 5.4 | 13.3 -> 13.3 |
| dmr-direct | 186 -> **187** | 9.8 -> **9.3** | 35.2 -> 29.9 | 5.8 -> 4.0 | 13.3 -> 13.3 |
| dmr-direct-lag15 | 184 -> **185** | 8.9 -> **9.1** | 31.1 -> 30.6 | 5.5 -> 3.8 | 13.3 -> 13.3 |
| dmr-close | 89 -> **175** | 19.7 -> **12.5** | 99.0 -> 99.0 | 48.8 -> 5.7 | 13.3 -> 13.3 |
| dmr-strong | 184 -> **189** | 12.4 -> **10.2** | 37.6 -> 24.3 | 12.5 -> 5.0 | 13.3 -> 13.3 |
| dmr-highfloor | 71 -> **190** | 35.8 -> **9.5** | 99.0 -> 31.4 | 91.1 -> 4.2 | 0.0 -> 13.3 |
| dmr-half | 0 -> **175** | 99.0 -> **11.8** | 99.0 -> 99.0 | 4.7 -> 4.0 | 13.3 -> 13.3 |
| dmr-busy | 167 -> **186** | 16.5 -> **13.9** | 99.0 -> 39.8 | 41.7 -> 21.5 | 6.8 -> 9.9 |
| dmr-repeater | 194 -> **194** | 9.4 -> **9.1** | 24.8 -> 24.0 | 6.9 -> 4.5 | 13.3 -> 13.3 |
| dmr-steady | 200 -> **200** | 7.5 -> **6.8** | 11.8 -> 10.8 | 14.8 -> 9.2 | 13.3 -> 13.4 |
| all-owner-436275 | 84 -> **197** | 29.8 -> **2.9** | 99.0 -> 26.4 | 14.1 -> 11.4 | 13.3 -> 13.3 |
| all-dmr-direct | 156 -> **191** | 17.0 -> **10.3** | 99.0 -> 31.8 | 4.6 -> 4.2 | 13.3 -> 13.4 |
| all-dmr-close | 22 -> **165** | 34.3 -> **14.9** | 99.0 -> 99.0 | 11.2 -> 4.6 | 13.3 -> 13.3 |
| all-p25 | 131 -> **183** | 17.9 -> **10.0** | 99.0 -> 29.1 | 4.2 -> 3.8 | 13.3 -> 13.3 |
| all-dmr-repeater | 156 -> **176** | 11.8 -> **10.9** | 99.0 -> 99.0 | 4.3 -> 3.5 | 13.3 -> 13.3 |
| all-highfloor | 0 -> **188** | 99.0 -> **9.3** | 99.0 -> 34.1 | 19.0 -> 3.7 | 0.0 -> 13.3 |
| all-busy | 75 -> **149** | 30.0 -> **26.8** | 99.0 -> 99.0 | 15.4 -> 11.2 | 2.3 -> 3.0 |

99 = not within 60 s for that share. Rows: `owner` the owner's case (DMR direct, -30..-15 dBm with the -45 dBm
ceiling, 436.000-436.500; `offgrid` typed from 436.010; `433200` outside the range: never found by either, the range
is applied as typed); `direct` DMR direct mode -110..-60 dBm; `close` -40..-20 dBm in the room; `strong` -70..-50 dBm;
`highfloor` a receiver reading -112 dBm quiet; `half` on a 6.25 kHz raster; `busy` six FM carriers in the range;
`repeater` a DMR repeater; `steady` a direct mode carrier keyed all the time. What still misses: short overs whose
rhythm matches the pass (one receiver: +-1 MHz is a 12 s pass for DMR; the owner's 500 kHz range is 3 s), and in
`close` the band desensed far and wide (80 dB down everywhere) while the over lasts. The model's checks: never fewer
finds than before (3 of 200 for the random overs), never a slower median or sweep, the owner's rows at least 190 of
200 within a 4 s median, the high floor and half step rows found. Unit test of the engines step by step:
`tests/plus/test_searchsure.c` (floor, run middle, half step, plateau, skirt, gone, long window, skips, grid, wide run,
steps a second, all digital DMR first). The real radio state machine's P25, DMR and all digital search suites run with
the switch (`test_radio_searchfix`).

What only the radio can settle (rows SX-*): the floor the FD6818 reads on a quiet band (SX-1), how far a radio in the
room raises the readings around it (SX-6: the run's middle assumes the strongest step is the channel), the DMR modem's
lock within 1.1 s (SX-3, SX-11), the native P25 receiver's within 1.2 s (SX-8).

Cost (`--plus --stack-check --verify-rebuild`: zero warnings, forced rebuild identical, every task fits, radio task
728 B free, the least free prvIdleTask 136 B): `ddm25` dbc0682 FLASH 846,028 B, RAM 209,688 B (1,768 free); this branch
FLASH 848,544 B (+2,516), RAM 209,792 B (+104: the two engines' run, skirt, floor flag and probe levels, the views'
step time; 1,664 free).

### Airband AM and broadcast FM

The DM-32 glue exists upstream but is unverified on the radio (hardware-dm32.md 8.7): AM is the
FD6818's AF type 7, broadcast FM is the RDA5807M on PTA25/26. While broadcast FM plays, a selected
P25 channel's receiver waits (the speaker is the broadcast's).

## More digital modes: feasibility (2026-11-16)

The native modem path that Classic uses for P25 hands the task 144-symbol (288-bit) raw 4FSK blocks every
30 ms from the HR_C7000 (`p25_phy_rx_record`, `native_test.h`): a 4800 symbol/s dibit stream whose sync and
framing are found in software. Any mode with 4800 symbol/s 4FSK can reuse it; deviation differs per mode, so
symbol margins (RX) and TX deviation need checking on the radio.

| Mode | Symbol rate | Voice codec | What it needs here | Verdict |
|---|---|---|---|---|
| YSF (C4FM) | 4800 | DN: AMBE+2 (the DMR vocoder); VW: IMBE full rate | sync/FICH/VCH framing; DN: re-FEC the 49-bit AMBE frames into DMR's 72-bit form for the stock codec; VW: our IMBE | RX feasible first |
| NXDN96 (12.5 kHz) | 4800 | AMBE+2 (EHR) | LICH/SACCH/FACCH framing, scrambler, AMBE re-FEC | RX feasible, but hardly used by amateurs |
| NXDN48 (6.25 kHz) | 2400 | AMBE+2 | what amateur NXDN uses. The C7000 4FSK stream can't run at 2400, but the GPL-3 project `donttellmeimcute/DM32-nxdn-p25` (an Open25 lab fork) keeps the radio in FM mode and runs a software 4FSK modem on the chip's 8 kHz FM RX/TX audio RAM (register recipe in `research/ref/DM32-nxdn-p25-ANALYSIS.md`); its NXDN48 RX works in the field (their issue #4), its TX does not interoperate yet | RX portable (~14 KB flash, ~2.7 KB RAM): next candidate |
| M17 | 4800 | Codec2 3200 | LSF/stream framing (open spec), Golay, convolutional + puncturing; Codec2 is floating point and the CK803S runs soft-float at 168 MHz: decoding 3200 bit/s is estimated at several times the whole CPU (two 512-point FFTs and the synthesis per 10 ms). Voice needs a fixed-point Codec2 (none exists; a large DSP port) | framing, callsign and text messages feasible; voice not without a fixed-point Codec2 |
| D-STAR | 4800 GMSK | AMBE (not AMBE+2) | GMSK demod, a codec the radio doesn't have | no |

RAM: each mode's frame buffers are small (hundreds of bytes); the large items are the voice FIFO (5.8 KB,
shared) and a second vocoder (Codec2). One mode runs at a time, so their state can share one region.

Testing needs a transmitter of that mode (a radio of that mode, an MMDVM hotspot or an SDR); host tests can
prove our encoder and decoder agree, not that they match other radios.

### YSF progress (2026-11-16)

Owner: no YSF/NXDN/M17 transmitter or hotspot for testing, start with YSF. Done, host only:

- `src/ysf/ysf.{c,h}`: sync framer (up to 3 bit errors), FICH decode/encode (K=5 rate 1/2 Viterbi, Golay
  (24,12), CRC-CCITT, 20 x 5 dibit interleave), V/D mode 2 ("DN") voice channel <-> the 49 AMBE+2 parameter
  bits (triplet voting, whitening, 26 x 4 bit interleave). Our code; layout constants from the
  GPL-2.0-or-later MMDVMHost (G4KLX) and MMDVM_CM YSF2DMR (EA7EE).
- `tools/test_ysf_ref.sh` (needs `../research/ref/MMDVMHost` and `MMDVM_CM` clones): our Golay encoder equals
  the reference table for all 4096 words; FICH encodes bit for bit like CYSFFICH both ways and corrects
  errors; the DN voice bits equal YSF2DMR's a/b/c words; and RadioFoundry's `ambeFecEncode` of them gives
  exactly YSF2DMR's 72-bit DMR AMBE frame. So DN audio can go straight into the DMR vocoder path. 0 failures.
- `tests/plus/test_ysf.c` (in `make test`): CRC check value, FICH with errors, triplet voting, the framer
  through noise, a 2-bit-off sync and back-to-back frames.

Wired into Plus (`CONFIG_YSF`, `tools/plus_features.py` `prepare_ysf`):

- Channel mode YSF beside P25: Mode cycles FM -> DMR -> P25 -> YSF -> FM in the channel editor; a YSF channel
  shows "DG-ID" (0 = any, 1..127) and is always RX only (no YSF TX yet). Record: CH_MODE 0xA6, DG-ID in the RX
  group byte. `codeplug_mode_p25()` is true for both ("a native-modem channel"), so scan, dual watch, P25 scan
  give-back, the PTT/TX checks and retune rules apply unchanged; `codeplug_mode_ysf()` tells them apart. Shown as
  "YSF" on Home, the channel list and the debug link.
- Receive: `native_lab.c` tells the worker which (`p25_lab_set_ysf`); `p25_lab.c` feeds the native modem's air
  bits to the YSF framer, decodes the FICH, and for communications frames of V/D mode 2 on the channel's DG-ID
  sends the five voice channels as DMR AMBE frames to `ambeVoiceSink()` (the DMR vocoder streams and switches the
  speaker); the terminator ends the call. Voice also marks the channel heard (P25 scan give-back timing).
- Test: `tests/plus/test_ysf_channel.c` (the real codeplug store: record, RX only, DG-ID limits, P25 unchanged).
  The worker path itself is not host-tested (no fixture for the native worker with the DMR vocoder yet).

Added 2026-11-16 (second pass, with two helper agents):

- Callsigns: the V/D mode 2 data channel (`ysf_vd2_data`: same 20 x 5 interleave and K=5 code as the FICH, CRC,
  whitening) gives FN 1 the source and FN 0 the destination; checked against MMDVMHost's
  `processVDMode2Data` decode. Home's bottom line shows "YSF TA1ABC > ALL" during a YSF call.
- P25 caller (`CONFIG_P25_CALLER`): Home's bottom line shows "P25 TG 655 ID 1234567" during a P25 call (from the
  call's link control; Classic showed nothing).
- VW (Voice FR, DT 3): `src/ysf/ysf_vw.{c,h}`. Each 144-bit section is a P25 channel-coded IMBE frame (same FEC,
  whitening and interleave as P25 LDU voice). FN 0 / FT 1 carries 2 voice sections (3 and 4), every other
  communications frame 5 (MMDVMHost's rule; not checked against a real VW capture). The worker decodes them like
  P25 receive (`p25_imbe_decode`, the same gating, `dm32_imbe_decode`) and plays them on the P25 PCM path.
  Cross-check `tools/test_ysf_vw_ref.sh`: interleave equal to MMDVMHost's and YSF2P25's tables, 14,859 clean and
  29,718 error-injected IMBE frames identical to MMDVMHost's processing: 0 failures (MMDVMHost's own
  `regenerateIMBE` has a write-back bug past 2 errors per word; ours corrects up to the code's limit).
- Robustness: a lost record (time gap) or an unusable record (layout 3) drops the YSF frame in hand and hunts for
  the next sync (`ysf_resync`), so no frame with a hole is played (found by the end-to-end test below).
- End-to-end worker test `tests/plus/test_ysf_worker.c` (`tools/test_ysf_worker.sh`): the real `receive_run()`
  with YSF frames cut into native records of all layouts, through `p25_phy_rx_air`, to the DMR sink.

Still to do for YSF: VW data channel and V/D mode 1, TX, and the radio check: symbol polarity and deviation of
the C7000 raw stream for YSF are unverified (needs a YSF transmitter).

## Plan

1. ~~P25 in scan and dual watch~~ (done, host-tested).
2. ~~APRS~~ (fits after trimming cable diagnostics); heap high-water mark on the radio.
3. ~~Airband AM / broadcast FM~~ (switched on; verify on the radio).
4. ~~Spectrum sweep / close call~~ (done, host-tested; screen look not checked yet).
5. ~~Fox hunt, CW beacon / SOS, lone worker, sun & moon, waypoints and Go-To~~ (done, host-tested).
6. More digital modes: YSF receive (DN) wired and cross-checked; then YSF callsign / VW / TX, NXDN96, M17.
7. Turkish UI (font tool in the lab repo: `tools/turkish_ui.py`).
8. Hardware test pass at the end, each feature checked on the radio (in progress since 2026-11-17).

Next stage (owner's requests from the radio test, 2026-11-17). Built in artifacts/plus-20261117c (7c90427b...),
host-tested, not yet on the radio:

- [built] Home bottom line with the NAC: "P25 TG 101 ID 77285 NAC 659", shorter alternates when it doesn't fit
  ("TG101 ID77285 NAC659", "TG101 ID77285"); p25_lab_p25_caller_line().
- [built] The receiver restarts after a saved channel edit (native_lab.c reselect, test_plus_edit_reselect).
- [built] Dual watch panes name a P25 caller ("TG 101 ID 77285 NAC 659" / "TG101 ID77285", "P25 call" before
  the link control) and a YSF caller ("YSF SRC > DST") instead of "FM signal" (ui_dual.c native_caller).
- [built] The A/B key (SK2 press) starts dual watch when it is off (CONFIG_PLUS_UI).
- [built] Tools > Air band: the VFO on the air band (118.000 MHz the first time, then the last air band frequency
  used), AM receive (CONFIG_PLUS_UI). Home already shows "AM" and "Air Band AM".
- [built, part 1] Faster P25 search: every 16 sweep steps one frequency of the found list in the range is read
  again (round robin), so a new call on a known channel is caught in under a second instead of a whole pass
  (test_p25search). Still to measure on the radio before changing: a pass's time, a coarse 25 kHz sweep with an
  exact-step probe, a shorter P25S_SETTLE_MS.
- [built 2026-11-19, host-tested, not yet on the radio] P25 in zone scan (R3, worked but missed: a 3-5 s call
  was caught only on the 5th zone cycle). Causes and changes in "P25 in a zone scan" above: the give back forgot
  the call's voice when the worker started over, so every call ended in the 8 s cooldown (also in dual watch and
  multi-watch); the scan still stopped, muted, on a channel in its cooldown; a take on a call's tail was treated
  as a foreign carrier; and a P25 stop was found by the FM noise reading only (now the strength too, as DMR
  stops). Host simulation, 8-channel zone: a conversation's 3 s overs caught 47 % before, 100 % after; with a
  noise reading deaf to C4FM 0 % before, 100 % after. Counters over the debug link (`0x21 [12]`). FLASH
  776,128 -> 776,852 B (+724), RAM 208,024 -> 208,072 B (+48: the counters 24 B, the take's voice / frame times
  and flags). Radio checks R3a-R3e; the 1.2 s probe time and the strength threshold are the two figures to
  confirm there.

Second round (owner: Turkish and every remaining OnurP25 catalogue item, 2026-11-17). Built in
artifacts/plus-20261117e (b4ab8353...), host-tested, not yet on the radio (checks N7-N12 in plus-radio-test.md):

- [built] Turkish UI (CONFIG_LANG_TR, default language): integration/radiofoundry/lang/tr.po, every string; owner
  rule: technical names stay English (RX, TX, VOX, Fox hunt, Dual watch, Talkgroup, Lone worker, Squelch...).
  Ğ ğ İ ı Ş ş composed into the fonts in their ISO-8859-9 slots (tools/turkish_ui.py), only in CONFIG_LANG_TR builds.
- [built] Tools > Last call (CONFIG_CALL_REPLAY): the last 8 s of the last P25 call (IMBE frames, 4.4 KB); the RTOS
  heap went from 40.5 to 36.5 KB (measured min free 8,144 B before).
- [built] Tools > Map (CONFIG_MAP, docs/map.md): Natural Earth 5.1.1 (public domain), 81 il, 20 KB.
- [built] Map districts and repeaters (`CONFIG_MAP`, `prepare_map`, docs/map.md "Districts"; owner 2026-11-19;
  parallel job): the 973 districts (ilçe) as points with their names from GeoNames TR.zip of 2026-11-19
  (CC BY 4.0, "District data: GeoNames (CC BY 4.0)"; SHA-256 pinned, `tools/turkey_districts.py`: admin1 codes
  mapped to plates by name, official names and per-province counts enforced, 973 = the official total; no district
  boundaries: geoBoundaries TUR ADM2 is OSM / ODbL, GADM non-commercial). Zoom 0..6 (two closer levels), district
  dots from zoom 3, names from zoom 4, labels never overlap (a 16 x 24 cell grid on the stack, provinces first),
  "GeoNames" in the corner; under the map "34 İstanbul / Kadıköy" (nearest district centre). Repeaters from the
  existing `rpt_table` (no copy) as triangles; # picks the one nearest the map centre (# again: the next), its
  callsign, frequency and way from the centre under the map; OK opens its Tools > Repeaters details (Back returns
  to the map). Data 12,114 B (map data 32.3 KB of the generator's 60 KB cap). FLASH 755,052 -> 769,440 B
  (+14,388), RAM 208,024 -> 208,024 B; every task's deepest stack path unchanged. Host test test_map.c (counts,
  names, every centre in or within 12 km of its province, lookups, drawing, label grid). Radio checks MD1-MD8.
- [built] Our splash and Tools icons (CONFIG_PLUS_ART, docs/art.md), 3.1 KB.
- [built] NXDN48 receive channels (CONFIG_NXDN, docs/nxdn.md): no NXDN transmitter yet to prove it.
- [open] Not possible here: D-STAR (no codec), M17 voice (Codec2 too heavy), OTA updates (vendor servers), "10 W".

Third round (owner, 2026-11-17): 1 DMR scanner, 2 DMR any CC/TG/TS, 3 call history, 4 signal quality during a call,
5 encrypted call notice, 6 repeater list and nearest repeater (built: "Repeater list" below), 7 APRS position/message/SOS TX, 8 a PC application
(last). RAM came first:

- [built] RAM headroom (`prepare_ram_trim`, Plus only): three task stacks cut to the stack check's worst case plus
  margin (p25-lab 8,192 -> 5,632 B, dmr 2,048 -> 1,280 B, gps 1,536 -> 1,024 B; the radio's R12 high-water marks
  were well inside), the RTOS heap shrunk by the same 3,840 B (its measured free room unchanged), the debug link
  log ring 2,048 -> 1,024 B. Static RAM 207,864 -> 203,000 B (8.4 KB free). Not done (kept in reserve): sharing
  the AMBE codec state (13.3 KB) with the P25 voice decoder and FIFO (12.7 KB). They are never heard at the same
  time, but YSF uses both (DN via AMBE, VW via IMBE) and dual watch switches between them, so it needs an owner
  hand-over and a full radio retest; worth it only when the next large feature needs the room.
- [built] DMR any (`CONFIG_DMR_ANY`): channel editor Color code 0-15 then "Any", Timeslot 1 > 2 > "Any", RX Group
  List "Any (all groups)" (the last entry in the picker). Stored in the free high bits of a DMR record's color code
  byte (0x80 any CC, 0x40 any TS, 0x20 every talkgroup; the upstream CPS shows and writes only the low nibble, so a
  CPS edit of such a channel drops "any"). Any CC: the modem's CC filter off, Home shows "CC*" until a call names
  its CC. Any TS: no slot lock even through a repeater, the first slot heard holds the call, the other slot stays
  out until it ends, Home shows "TS*" then the call's slot. Both make the channel RX only (toast) and TX is refused
  (TXDENY_MODE). Every talkgroup is the upstream hear-all filter, so TX to the contact still works. Tests:
  tests/plus/test_dmr_any.c (record), test_dmr_any_call.c (upstream's call fixture).
- [built] Call history and encrypted notice (`CONFIG_CALL_LOG`): upstream's Recent list (DMR calls heard and made,
  12) holds 20 and takes P25, YSF and NXDN calls: "P25 77285 / TG 101 NAC 659 5m", "YSF DL1ABC", "NXDN 1234 / TG
  10". P25 calls come from p25_lab.c `call_track` (a new call after a 1.5 s gap or another TG/NAC; the source fills
  in at LDU1; encrypted when no clear-voice superframe, LDU2 ALGID 0x80, arrives in the first 0.5 s, and clear again
  if one does). An encrypted P25 call shows DMR's toast "Encrypted: not played" once and is marked "Encrypted" in
  the list. RAM only (lost at power off), ~0.4 KB. Non-DMR entries have no PTT/actions, and Last caller, call back
  and the talkgroup picker skip them (they would be wrong DMR targets). Test: test_ysf_worker.c test_call_log.
- [built] Signal quality during a call (`CONFIG_SIGNAL_QUALITY`, `prepare_signal_quality`; parallel job): a bit
  error rate estimate over about the last second (radio/sigq.c: four 250 ms slots of corrected / FEC-protected bits)
  from the corrections the voice FEC already counts. P25: the 9 IMBE frames of every voice superframe on the channel,
  admitted or not (137 protected bits each; a superframe that failed to decode counts 15 per frame, the most the
  FEC corrects); Home's caller line ends with it in every form ("P25 TG 101 ID 77285 NAC 659 BER 1.2%" |
  "TG101 ID77285 NAC659 BER1.2%" | "TG101 ID77285 BER1.2%"). DMR: the AMBE+2 frames the voice sink decodes (47
  protected bits; an uncorrectable frame counts 7), shown before the level on the call screen ("BER 1.2%  -87
  dBm"); upstream already showed the level and meter there. P25 level: while the native receiver owns the channel
  the radio task skipped its poll, so Home's dBm and meter kept the last FM reading; the native maintenance now reads
  the FD6818 RSSI as the poll does while a call is heard (never keyed). The estimate is honest only up to the FEC's
  limit (3 errors per Golay word): beyond about 10 % it reads low. Not in the call history yet. Test:
  tests/plus/test_sigq.c. RAM +152 B, flash +912 B.
- [built] DMR scanner (`CONFIG_DMR_SEARCH`, "DMR search" above; parallel job): Tools > DMR search sweeps a range
  with the board in DMR and the call stack hearing every color code, slot and talkgroup; a carrier with two good
  bursts in 600 ms is DMR and goes into the found list (frequency, CC, slots, RPT / DIR, last talkgroup and source);
  a voice call plays and the sweep resumes 2 s after it; OK on a find puts the VFO there in DMR (a repeater find:
  simplex on its output, with a toast to set the TX offset). Receive only. 584 B static RAM.
- [built] FM caller ID (`CONFIG_FM_ID`, `prepare_fm_id`; parallel job): MDC1200 (PTT ID op 0x01 and emergency op
  0x00, unit ID in hex as Motorola radios show it), FleetSync I (fleet-unit) and DTMF decoded from an FM channel's
  receive audio. Home's bottom line shows "MDC 1A2B" (+ "Emergency"), "FleetSync 107-1772" for 5 s, or "DTMF 1234#"
  while digits arrive and 4 s after the last; each ID (a DTMF string once it has ended) joins Recent with its own
  mode (0x80-0x83), no PTT or actions. Audio: the APRS receive ring (HR_C7000 8 kHz FM RX RAM, 2 KB from the heap),
  ours whenever an FM channel receives (not TX, AM, broadcast FM, spectrum, P25 search; 300 ms after the channel
  settles), squelch open or not, because an ID at key-up arrives while the squelch opens. On the APRS frequency APRS
  keeps the ring and hands its blocks over (`radio_fmid_yield` / `radio_fmid_audio` in aprs.c). **Side effect: while
  the ring runs, FM audio plays through the HR_C7000's FM playback (as on the APRS frequency), not the FD6818 direct
  path** (radio check F1), and the 2 KB ring is held while an FM channel listens (heap margin, R12c / F7). DSP
  (integer, `integration/radiofoundry/fmid/fmid.c`): band pass + x[n]x[n-4] discriminator + bit clock for the
  1200/1800 Hz FFSK both share; MDC sync, deinterleave, majority-logic FEC, CRC; FleetSync sync, CRC-15 + parity
  (checked on two published on-air blocks); Goertzel DTMF on 17 ms blocks. Under 1 % CPU, +392 B static RAM,
  +4.3 KB flash, UI stack unchanged. On / off: Settings > Talk & listen > FM caller ID ("FM caller ID on / off"
  below; Off keeps FM audio on the direct path). Host test: tests/plus/test_fmid.c. Radio rows F1-F7.
- [built] SFR role on the radio (`CONFIG_SFR_EDIT`, `prepare_sfr_edit`; parallel job): channel editor row "SFR"
  after Battery saver, DMR channels only, cycles Off > User > ERDM user > Repeater > Repeater (ERDM style). With a
  role set, "SFR inbound slot" (TS1 / TS2: the slot users talk on, the repeat goes out on the other) and, for the
  two ERDM roles, "SFR outbound CC" (Channel's = the channel's color code, or 0-15, like the Color code spinner).
  Framing and hang time are not offered: a new role gets the codeplug defaults (auto framing: TS sync for the
  standard roles, BS for ERDM; 1 s hang), and a CPS-set framing or hang stays as stored when only the role or slot
  changes. Encoding is upstream's sfrChannelEncode (the inverse of sfrChannelDecode) into channel_t.sfr_flag1 /
  sfr_a / sfr_b; the open-firmware encoder now writes them (0x26 bit 1, 0x25, 0x2A). A channel without a role
  clears a record that had one (Off: bit and both bytes 0) and leaves any other record's bytes alone, so a
  CPS-made record nobody touched round-trips byte for byte (the CPS view of the VFO record keeps its bytes too,
  `codeplug_openfw_sfr`). A role needs one frequency (RX = TX): on a two-frequency channel the row reads "Needs one
  frequency" and OK only toasts. A repeater role also needs TX: on an RX only channel the cycle skips both repeater
  roles (toast "Repeater needs TX: RX only is on"). Choosing a repeater role toasts "Repeater role: transmits by
  itself": the radio then repeats unattended (licence conditions; upstream marks standalone SFR experimental).
  Tools > Single-freq repeater without a role now points to the editor's SFR row. +8 B static RAM, +888 B flash, UI
  stack unchanged (368 B free). Host tests: tests/plus/test_sfr_edit.c (every role through encoder, flash, decoder,
  sfrChannelDecode and radio_sfr_channel; CPS byte patterns; Off; two frequencies), test_ui_sfr.c (the rows in the
  UI simulator). Radio rows SR1-SR6.
- [built] Satellites (`CONFIG_SATELLITES`, `prepare_satellites`; parallel job; the queued "Satellite mode" below
  without TLE entry on the radio; the ISS APRS digipeater is its own item below): Tools > Satellites lists every FM satellite's next pass
  over the GPS position, soonest first (rise in local time, highest elevation, minutes; "now" while under way), and
  for the one in focus the live azimuth, elevation, range, rise / set countdown and the Doppler corrected RX and TX
  (with the shift in kHz). OK on a pass under way or due within 10 minutes follows it in the VFO: RX on the
  downlink, TX on the uplink, FM wide, the uplink CTCSS (# switches SO-50's 74.4 Hz arming tone), both moved again
  whenever the correction rounds to another step (1 kHz VHF, 2.5 kHz UHF: about 7-9 retunes a pass), never while
  transmitting; it ends 30 s after the set, on OK again, or when the VFO is changed by hand (or a channel chosen),
  which the screen reports. Nothing keys the transmitter. (Round 3 changed the moves to a hysteresis, an uplink
  move alone to TX only and the end at the set to the nominal frequencies, and added the pass alarm: "Satellites:
  pass alarm and Doppler tracking" below.) Left / Right: the UTC offset (+03:00 at first, as Sun &
  Moon). Elements: `integration/radiofoundry/sat/tle.txt` (standard three-line sets, CelesTrak's amateur.txt can
  go in whole) and `sat_freq.txt` beside it (catalog number, name, downlink, uplink, CTCSS, arming tone; at most 8),
  compiled in by `tools/sat_tle.py` on every build (checksums verified), so a refresh is an edit and a build. The shipped sets
  are CelesTrak's (ISS, SO-50, AO-91, PO-101, epoch 17.11.2026, fetched 18.11.2026; the agent's made-up samples were
  replaced at the merge): refresh before each release; the
  screen shows the elements' age and turns it to the warning color after 14 days. SGP4 (radio/sgp4.c, near-earth
  branch, WGS-72, written from Hoots & Roehrich 1980 and Vallado et al. 2006) reproduces that paper's verification
  output for 00005, 06251 and 28057 to 5e-9 km; deep-space sets are refused. Look angles from the WGS-84 observer,
  TEME turned by GMST; the pass search steps by how fast the sub-satellite point can reach the visible circle, then
  bisects rise and set to 2 s and finds the top by ternary search (100 passes checked against a 1 s brute force:
  rise / set within 1 s, top within 0.5 degree; 65 propagations a pass on average). Runs in the radio task at its
  poll, only while the screen draws or a pass is followed: once a second the live look, in between two
  propagations of the search a poll (one on a P25 channel, at the native receiver's maintenance poll;
  radio/sat_svc.c). Cost of one propagation with look angles (soft float): about
  150 double multiplies, 110 adds, 21 divides and 35 libm calls (18 sin/cos, 6 sqrt, 6 fmod, 2 atan2, 2 acos, asin),
  estimated 1-2 ms on the CK803S (the screen shows the measured slowest, top right). Static RAM +320 B, flash
  +26 KB (mostly the double-precision libm). Stack: the radio task's satellite path is 1,888 B of its 3,072 (the
  deepest stays the native path, 2,044 B; free 832 B), the screen's draw about 1,100 B of the UI task's 2,016
  (deepest unchanged, 1,452 B). Host test: tests/plus/test_sat.c. Radio rows U1-U8.
- [built] Pager and ACARS receive (`CONFIG_MSG_RX`, `prepare_messages`; parallel job; needs FM_ID): Tools > Pager
  decodes POCSAG (ITU-R M.584: 512 / 1200 / 2400 bit/s found by themselves, BCH(31,21) correcting 2 bits + parity,
  numeric and alphanumeric, tone only) on the FM channel in use; Tools > ACARS (radios with AM receive) decodes the
  air band's 2400 bit/s MSK blocks (registration, flight ID, label, block, text; BCS = CRC-16 0x8408, odd parity).
  Each lists its last six messages, newest first, shortened ("1234567: text", "TC-JFV H1: text", age); OK shows one
  in full (Up/Down scroll). **Runs only while one of its screens is open** (a decoder needs the audio path changed:
  the pager the flat path, ACARS the receive ring in AM; and nobody reads a list nobody looks at): the screen tells
  the radio task each UI tick (`radio_msg_want`). Pager audio: FM caller ID's (its ring, or APRS's on the APRS
  frequency; `radio_msg_audio` in radio_fmid.c), with the flat receive path on while the screen listens
  (`board_nxdn_flat`: FD6818 HPF/LPF/de-emphasis and HR_C7000 FM band-pass off, back on when the screen or the
  channel is left), **so the speaker plays flat, brighter FM audio meanwhile**. ACARS audio: an air band channel
  (AM) takes the APRS receive ring itself (FM caller ID, APRS and NXDN never listen there): the FD6818's AF output
  (its AM demodulator) stays on the HR_C7000's input, which copies it to its 8 kHz RX RAM and plays an open squelch
  through its own FM playback, as on the APRS frequency. Whether the HR_C7000 passes AM audio to the RX RAM as it
  does FM is the one thing only the radio can tell (M4). DSP (integer, `integration/radiofoundry/msg/`): POCSAG with
  six slicers (three rates, each a level slicer for flat / de-emphasized audio and an edge slicer for high passed
  audio; codewords from both, the one with fewer errors taken), preamble-gated sync; ACARS with a 1800 Hz I/Q mixer,
  a three-sample differential detector and Gardner timing, sync looked for in all four bit/tone mappings (the
  mapping is not assumed). Host tests (tests/plus/test_pocsag.c, test_acars.c, own encoders): every rate flat and
  high passed, 512/1200 through de-emphasis, 8 dB SNR at 512/1200, clock offsets, clipping, FEC; ACARS four mappings,
  tilt, 15.6 dB, BCS / parity errors; no false message from 30 s noise, voice or random data. About 1 % CPU while a
  screen listens, nothing otherwise. +1,496 B static RAM (two lists of 6 x 80-character messages, the message page),
  decoder state (~220 B) and, for ACARS, the 2 KB ring from the heap while a screen listens; +7.3 KB flash; UI
  stack unchanged. RAM only: gone at power off. Radio rows M1-M7.
- [built] APRS messages and SOS (`CONFIG_APRS_MSG`, `prepare_aprs_msg`; parallel job; item 7 above, position TX was
  upstream's already: "Send position now", beacons): three new rows at the end of Tools > APRS. **Messages**: the
  APRS messages to us (the engine keeps the last 4, RAM only), newest first as "FROM: text" and age, unread ones in
  the warning color (the row says "2 new"); OK opens one (from, how long ago, the text; opening marks it read), #
  or its Reply opens the composer to the sender. **New message**: To (callsign-SSID) and Text typed with the
  radio's own text entry (the channel name editor's keypad letters, ABC/123/abc on #; its buffer grew from 16 to
  67 characters, a long text shows whole above the row), or Canned text: QSL, 73, OK, QRV?, On my way, Call me,
  Wait, Thanks (English: APRS text is ASCII, so not translated); Send hands them to the engine's
  `aprs_send_message` (msgno, ack, 3 tries at 30 / 60 / 120 s; `|`, `~`, `{` replaced, the addressee upper-cased).
  The line above the rows and toasts say how it went: "Sending to X, try n of 3", "Delivered to X" (acked),
  "Not delivered to X: no ack"; the engine's gates first (the same why-texts as Tools > APRS: callsign, amateur
  frequency). **Acks and messages are heard only while the channel is the APRS frequency with APRS receive on**
  (upstream's receive path; the composer says so while it waits). **SOS (emergency)**: a confirmation page ("Send
  an emergency position now and every 2 minutes until turned off?" OK = yes, Back = no); then the Mic-E position
  with message bits 000 = "Emergency" (APRS101 ch. 10: Kenwood / Yaesu radios alarm, aprs.fi shows Emergency),
  comment "SOS", our symbol, at once and every 120 s on the configured path through the engine's RF queue (CSMA,
  keying, RF power as for beacons), until the row is turned off (OK on it); the row reads "On, next m:ss" in the
  warning color, each one toasts "SOS position sent". RAM only: off after power-off. It needs the beacon's gates
  (callsign, an amateur APRS frequency, AFSK) and a position (GPS / computer / fixed), else the confirmation toasts
  why and nothing goes out; a gate lost while on makes it wait. RF only (not DMR, not APRS-IS). A Mic-E Emergency
  heard from another station toasts "EMERGENCY: <call>". Mic-E encoder (`integration/radiofoundry/aprsmsg/
  aprs_plus.c`): latitude digits, message bits, N/S, longitude offset and W/E in the destination (frames to it
  instead of APZRDF: `queue_frame_to` in aprs.c), longitude / speed / course + 28 with APRS101's +80 / +400 shifts
  for printable bytes, optional altitude "xxx}"; reproduces APRS101's own example byte for byte. Host test:
  tests/plus/test_aprs_msg.c (upstream's aprsRadioTest suite again with the feature in, then encoder round trips
  through the engine's parser in all quadrants and longitude bands, SOS gates / timing / off through the fake radio's
  decoded tones, composer, 3 tries, ack, inbox). +464 B static RAM (text entry +51, screens' buffers ~390, engine
  24), +5.0 KB flash, every task's stack unchanged (UI 368 B free). Radio rows AP1-AP10.
- [built] SSTV receive (`CONFIG_SSTV`, `prepare_sstv`; parallel job; needs MSG_RX): Tools > SSTV draws slow-scan TV
  pictures from the FM channel in use, mainly the ISS's (PD120 on 145.800 MHz FM) and the common ham modes: Robot 36
  and 72, PD50 / 90 / 120 / 180 / 240, Martin M1 / M2, Scottie S1 / S2 / DX (one decoder core, a table per mode
  family). The VIS code picks the mode; a picture tuned into late (VIS missed) starts on its line syncs (four sync
  pulses of a mode's length at its line time, nothing else of the sync tone between). The picture fills the
  display's width under the status strip (320 x 256 shown 240 x 192, PD120's 640 x 496 as 240 x 186), line by line
  as it arrives; the mode is in the title, below it "Line n/N" and the clock offset, then "Received" or "Signal
  lost" (ten syncs missed in a row) and "Waiting for the next picture". The picture stays until the next one starts
  or Back. **Runs only while the screen is open**, like the pager and through the same code (`radio_msg_want`
  RADIO_MSG_SSTV): an FM channel FM caller ID listens on, its audio (`radio_msg_audio`), the flat receive path on
  (`board_nxdn_flat`; the speaker plays flat audio meanwhile, as for the pager); a retune of up to 10 kHz (Satellites'
  Doppler steps on an ISS pass) keeps the picture going, a bigger one starts the decoder again. DSP (integer,
  `integration/radiofoundry/sstv/`): DC block, mix down by 1700 Hz (an 80-entry table), 15-tap 900 Hz low pass on
  I/Q, the phase step between samples (atan table) as the frequency; VIS from a smoothed frequency (leader, start
  bit edge, 10 bits averaged over their middle 20 ms, even parity); each line's sync found near where a least
  squares line through all syncs so far puts it (sync tone over the pulse, none after it), so a sender or receiver
  clock off by a few tenths of a percent gives a straight picture; every colour scan sampled straight to 240 (chroma
  120) pixels, each the mean frequency over its share; YCrCb to RGB (BT.601). The display has 16 palette entries for
  the whole screen: pixels are dithered (4 x 4 ordered) to 12 colours (R and B two levels, G three), the theme's
  black and white among them. The picture lives only in the frame buffer: the screen draws over its own last frame
  (strip, new rows, status, hint) and clears only when entered or when something else drew the display (another
  screen, the hotspot view: the picture so far is lost then); a banner is clipped below the picture. Host test
  (tests/plus/test_sstv.c, own encoder from the published timings): every mode with its VIS (mean error 0.5-3.5 of
  255), Robot 36 / PD120 / Martin M1 / Scottie S1 also with 12 dB SNR white noise (10-22), +-0.3 % sample clock
  (the fit reads +-3000 ppm, no slant), +-8 Hz, de-emphasis with a 300 Hz high pass, all together, and without the
  VIS (starts 4-8 lines in); no start from 30 s of noise or voice-like audio or a VIS with bad parity. CPU: about
  30 multiplies and a divide a sample (17.5 ns a sample on the host; estimated ~1 % of the CK803S), only while the
  screen listens. +248 B static RAM (the screen's row and text buffers, the info), the decoder (1,100 B) and a
  three-row ring from the heap while the screen listens (1,472 B); +7.6 KB flash; UI and radio task stacks' deepest
  paths unchanged. Radio rows SV1-SV7.
- [built] SSTV send (`CONFIG_SSTV_TX`, `tools/plus_sstvtx.py`; owner's request 2026-11-22): Tools > SSTV > **OK**
  opens "SSTV send": one built-in test picture, a clean card (320 x 240) in dark blue with the DDM32 Plus mark (the
  splash's, 2x), the wordmark split as the logo shows it ("DDM32" white, a small "+" at its top right and "Plus" under
  it in the accent orange: the splash art's own pixels, `art_data.c`), an accent rule, **the radio's callsign** large
  (the radio name's callsign as CW ID / APRS take it, `radio_cwid_callsign`; a 5 x 7 face up to 8x) and eight colour
  bars. The page shows it at half size, the mode with its length (Left / Right: **Robot 36** 37 s, the default;
  Martin M1 1:56; PD120 2:08), the TX frequency with the callsign. **OK sends**, nothing else does (never by itself).
  Checks on the page: a callsign as the radio name ("Set your callsign (the radio name) in qdmr."), an FM channel,
  simplex (RX = TX: no repeater), the band plan DOTA and the cross-band use (`ui_xband_refusal`: 144-146 / 430-440 MHz
  only, not a repeater segment or a repeater of the list, the satellite segments 145.8-146.0 / 435-438, APRS 144.800
  or PMR446), the time-out timer longer than the picture. The radio core checks again (`radio_sstvtx.inc`: every
  check a PTT press gets, `radio_sm_tx_check`: TX inhibit, receive only, band, battery, busy channel lockout; FM,
  simplex, the callsign, the time-out; nothing else on the air: PTT, APRS, DOTA, cross-band, CW, hotspot, SFR) and
  keys the channel's TX frequency with its bandwidth, CTCSS / DCS and power, as PTT would. While it sends: a progress
  bar and "Sending n/240", the TX LED and timer as for PTT; **Back stops it at once** (unkeyed, the page stays: "Sending
  stopped"), so do **PTT** (that press keys no voice), any request that moves the radio (a channel, zone or VFO change,
  a scan; the page's own keys do nothing meanwhile), leaving the page another way, the time-out timer (it still counts
  and cuts) and a critical battery. At the end: "Picture sent".
  Audio: the APRS / DOTA tone path (`hal_afsk.h` gains `tx_pcm`: `hrc_afsk.c` plays 8 kHz PCM through the HR_C7000's
  prompt FIFO into the FM modulator with the microphone muted, its timer 4 tops the FIFO up every 2 ms in 32-sample
  chunks; `tx_pcm` hands it a generator instead of the Bell 202 modulator, at the AFSK level 50: about 3 kHz peak
  deviation at 25 kHz). The generator (`integration/radiofoundry/sstv/sstv_tx.c`, from the published timings: JL
  Barber, Dayton 2000) makes each sample when the FIFO asks: a 32-bit phase accumulator and a quarter sine (a
  frequency change keeps the phase), time kept in nanoseconds inside the tone or pixel being sent (every mode's pixel
  is a whole number of ns: Robot 36 275 us, Martin M1 457.6 us, PD120 190 us), so nothing drifts: each element ends
  within one sample of its exact time, the whole within one sample of the nominal length; the card's pixel is worked
  out when the scan reaches it (no picture buffer). Sent: 250 ms of silence (the receiver's squelch), the VIS (1900 Hz
  300 ms, 1200 Hz 10 ms, 1900 Hz 300 ms, start bit, 7 bits LSB first and even parity at 1100 / 1300 Hz, stop bit, 30
  ms each), the lines (Robot 36: sync 1200 Hz 9 ms, porch 1500 Hz 3 ms, Y 88 ms, separator 4.5 ms at 1500 / 2300 Hz on
  even / odd lines, porch 1900 Hz 1.5 ms, R-Y / B-Y 44 ms; Martin M1: sync 4.862 ms, G B R 146.432 ms each with 0.572
  ms gaps; PD120: sync 20 ms, porch 2.08 ms, Y, R-Y, B-Y (two lines averaged), Y, 121.6 ms each), 300 ms of 1500 Hz.
  Video 1500 (black) .. 2300 Hz (white), YCrCb as BT.601 full range. Host tests (`tools/test_sstvtx.py`):
  tests/plus/test_sstvtx.c (the audio in 32-sample chunks decoded again by our SSTV receiver, sstv.c: the VIS gives
  the mode, every line arrives, mean error against the card 3-8 of 255, every callsign glyph cell and colour bar
  right, also through an FM receiver's de-emphasis; the length within a sample of the nominal, no phase jump, the
  VIS's tones and bit times), tests/plus/test_ui_sstvtx.c (the page in English and Turkish: modes, every refusal,
  sending, Back, sent, the radio core's refusals, leaving the page; layout; no PTT transmission),
  tests/plus/test_sstvtx_radio.inc (the real radio core: keyed on the channel, the very samples of the picture played
  whole, unkeyed once; each refusal keys nothing; Back, PTT, a channel step and the time-out stop it at once). +40 B
  static RAM (1,624 B free), 48 B of heap while sending, +6.3 KB flash; every task's deepest stack path unchanged (UI
  184 B, radio 728 B free; the generator runs on the tone path's interrupt, a few frames deep). Radio rows ST1-ST8.
- [built] DMR ID database (`CONFIG_ID_DB`, `prepare_id_db`; parallel job): a DMR caller no codeplug contact names
  shows as "CALLSIGN Name" ("TA1ABC Ahmet") instead of the bare ID, offline, from a table in **program flash**
  compiled into the image (nothing is written to the data flash or anywhere else). The radio task's `find_name`
  (radio_sm.c) names every caller the screens show, so the call screen's big line (the ID stays on the line below),
  the Home banner, Recent, Last caller / the CONTEXT hint and dual watch all get it. Order: the codeplug contact
  first (the owner's own name wins), then this database, then upstream's CPS-uploaded database in the data flash
  (left as it was; it fills IDs from elsewhere if one is ever uploaded). The talker alias is not a rival: the call
  screen shows it on its own line in quotes as before. Groups are never looked up. Settings > Advanced > About
  ends with "ID database: 8694, 2026-11-17, RadioID.net" (count, the download's date; "6, sample" for the sample).
  No on/off setting (it would need a settings field; a contact with the ID overrides a database name).
  **Data**: RadioID.net's DMR user database (https://radioid.net/static/user.csv, the public amateur registry:
  RADIO_ID, CALLSIGN, FIRST_NAME, LAST_NAME, CITY, STATE, COUNTRY). Attribution: "DMR ID data: RadioID.net"
  (generated file header, this entry). It holds registered amateurs' names: keep it in builds for the owner's own
  radio; check RadioID.net's terms before publishing a build that contains it. **Refresh** (like the TLEs): download
  user.csv into `research-data/user.csv` (git-ignored; or point `IDDB_CSV` at it), build; `tools/iddb.py` turns it
  into `build/iddb/iddb_data.c` at every prepare and prints "ID database: N IDs, ... bytes". Countries:
  `IDDB_COUNTRY=Turkey,Cyprus` for the build (default Turkey; "Turkey" matches RadioID's "Turkiye" / "Türkiye",
  case and accents ignored); by hand `python3 tools/iddb.py user.csv --country Turkey --prefix TA --stats`. Without
  the download the build uses `integration/radiofoundry/iddb/sample.csv` (six fictional IDs 9990001-9990006) and
  says "ID database: SAMPLE ..." in the build output: a release build needs the download. **Format**
  (`tools/iddb.py`, `iddb.h`): records sorted by ID in blocks of 64; an index of each block's first ID and offset
  (8 B a block) is binary searched, then the block walked (O(log n) + 64); a record is the LEB128 gap from the
  previous ID (1 B for Turkey's dense 2860001-2868888), the callsign base-37 in 4 B (A-Z 0-9, up to 6 characters;
  longer ones skipped and counted) and a 2-byte number into a table of distinct names. Name: the first word of the
  first name (else the last name), folded to ASCII (ç ğ ı İ ö ş ü to c g i I o s u: the large call-screen font sizes
  have no Latin-1 glyphs, turkish_ui.py), cut to 10 characters, an all-capitals or all-lower-case name in title
  case; the first row of a duplicate ID wins; IDs 1..0xFFFFFF. Size (RadioID.net 2026-11-17): Turkey 8,694 IDs,
  1,794 names, **78,076 B** (records 60,858, names 16,130, index 1,088); Turkey + Cyprus 8,796 IDs, 79,558 B; the
  generator refuses more than 120 KB (all of Germany alone is 150 KB). Build: FLASH 623,080 -> 701,828 B (+78,748;
  sample only +772), RAM 207,184 -> 207,232 B (+48: About's text buffer); the lookup uses no RAM besides its result,
  every task's deepest stack path unchanged (radio 828 B free, UI 368 B). Host test (tests/plus/test_iddb.c via
  tools/test_classic.py): the generator from a synthetic CSV (224 IDs in four blocks, gaps of 1-3 LEB128 bytes,
  IDs 1 and 0xFFFFFF, duplicates, Turkish / accented / long / empty names, callsigns with digits, country and
  prefix filters, broken rows), every ID back through the C lookup with its callsign and folded name, neighbours
  and dropped rows not found, the text cut at every length and to the radio's 16 characters; then the table the
  prepare made, walked record by record against the lookup. Radio rows ID1-ID4.
- [built] RAM reclaim (`prepare_ram_trim`, `prepare_startup_stack`; Plus only, 2026-11-18): static RAM 207,184 ->
  203,736 B (3,448 B back, 7.7 KB free), flash +892 B; the Classic image is byte for byte unchanged. Behaviour,
  audio paths and every other task stack unchanged.
  1. Start-up stack in the heap (2,044 B): crt0 to `vTaskStartScheduler` ran on a 2 KB `.stack` of its own after
     .bss, dead once the first task runs (main() never returns; interrupts switch to their own stack, portasm.S).
     Now the RTOS heap is the board's (`boards/dm32/board.c` `ucHeap`, section `.plus_heap` last in .bss,
     `configAPPLICATION_ALLOCATED_HEAP`) and `__stack_top` is 16 B under its end (`platform/hrc7000/common.ld`),
     below heap_4's end marker. heap_4 hands out its one free block from the low end and nothing is freed before
     the scheduler, so the boot's allocations (start-up, idle and timer task stacks, the timer queue: about 5 KB of
     32 KB) never reach main()'s 2 KB; after that the heap may use them. Guards: the start-up task checks first
     (`vPortGetHeapStats`: no free yet, least free >= 2,112 B) and stops with a crash record otherwise; the build
     audits the layout (`rfoundry_dm32.json` audits `plus_startup_stack_*`); host test `tests/plus/test_startup_heap.c`
     (in `make test`) runs the kernel's own heap_4.c: the boot's sequence passes with main()'s bytes untouched, and in
     3,000 random sequences (a third with frees) the check never passes once the heap wrote into them.
  2. Home's placeholder view until the radio core's first tick (`ui_app.c` `view_none`, 720 B, all zero, read only
     through `const radio_view_t *`) is `const`: flash.
  3. Host link task stack 2,924 -> 2,240 B (684 B; `PLUS_HOST_TASK_WORDS` 560): static deepest 1,428 B in this build
     (upstream's 731 words cover the web view and MMDVM hotspot chains, compiled out here), 616 B free above the
     port overhead and margin (gps keeps 472). The heap shrank by the same 684 B, so its free room is unchanged.
  Not done (looked at): sharing the AMBE codec state (13.3 KB) with the P25 voice decoder and FIFO (12.7 KB). YSF
  uses both in one session (DN through the AMBE codec in the audio task, VW through IMBE in the worker), the AMBE
  decode runs behind the worker in another task and its reset rewrites both state blocks, and a FIFO overwritten by
  codec state indexes its ring out of bounds; it needs a hand-over (`hrc_vocoder_park`, the codec's `ready` cleared,
  IMBE re-init, FIFO reset) and a full radio retest, so it stays the reserve for a feature that needs 10 KB. Smaller
  ones for later, each needing a call-graph review: about 20 static `zone_t` copies in the UI (180 B each) and four
  UI `radio_view_t` snapshots (720 B each) that could share one buffer. Risks: an upstream change that allocates or
  frees more before the scheduler would stop every boot at the start-up check (crash record names app.c; build
  without `-DPLUS_STARTUP_STACK_IN_HEAP` or with a larger heap); the host link stack has no radio high-water mark
  yet. Radio rows RR1-RR6.
- [built] FM caller ID on / off (`prepare_fm_id`; parallel job): Settings > Talk & listen > "FM caller ID", On by
  default (as built), Off. Off: `radio_fmid.c` never takes the APRS receive ring for itself, so FM audio stays on
  the FD6818's direct path (radio check F1's side effect gone) and no MDC / FleetSync / DTMF line is shown or logged.
  While Tools > Pager or SSTV (or a pager message) is open, the ring is taken for that screen anyway
  (`radio_msg_wants_fm`: they hear FM caller ID's audio): the pager / SSTV decoders get the audio, the ID decoders
  stay quiet, and Back gives the direct path back. A change takes effect at the next radio poll
  (`radio_fmid_enable`, from `ui_svc_apply` at start-up and on the change). Stored in a spare bit of an existing
  settings byte, `settings_t.cp_flags` bit 7 (`SETTINGS_CP_NO_FM_ID`, set = off): upstream uses only bit 0 there
  ("Use anyway", changed by app.c through a mask), so the record keeps upstream's size and layout, an older record
  and a factory reset (`settings_defaults`) read On, and Classic firmware ignores the bit; the CPS never reads the
  settings ring. (No earlier Plus feature stored a setting; this is the first, and the way for small ones.) With
  the ISS item below: static RAM +24 B, flash +1,048 B, UI and radio stacks unchanged (368 / 828 B free). Host
  tests: `tests/plus/test_fmid_gate.c` (the gate; a fake ring playing DTMF: on, off, off with the screen open, a
  channel that doesn't suit, on again) and `tests/plus/test_fmid_set.c` (the actual UI storage service and settings
  store on the simulated DM32 flash: On in a new radio, Off and On kept across power cycles, "Use anyway" not
  disturbed, factory defaults On). Radio rows SM1-SM3.
- [built] ISS APRS digipeater from Satellites (`prepare_iss_aprs`, `ui_sat.c`; parallel job; needs SATELLITES and
  APRS): on Tools > Satellites with the ISS in focus (or followed) and above the horizon, # sends one APRS position
  beacon through its digipeater: 145.825 MHz FM simplex plus the uplink Doppler of the moment (`sat_doppler_tx`,
  rounded to 1 kHz as the tracking does; at most about +-3.5 kHz), 1200 Bd AFSK, 25 kHz (wide) deviation, path
  "ARISS", our callsign and position (GPS, host or fixed; the same report as the beacons, comment included). Toast
  "Sent via ISS (ARISS)"; hint row "OK track  # APRS via ISS" (round 3; "OK alarm ..." with no pass to track). The engine (`aprs_iss_send`, patched into
  core/aprs/aprs.c) checks callsign, position, an amateur frequency the radio transmits on, AFSK, at least 30 s
  since the last accepted press, and one at a time; the radio task queues the frame at its next tick (`K_ISS`, built
  there as the beacons are, not on the UI stack) and the RF job sends it alone on its own frequency (CSMA there,
  then back to the channel), even while the APRS region frequency is blocked; the ordinary beacons keep their
  frequency and path. Only on the key press, never automatic (the engine's usual three tries after a busy channel
  apply, on the Doppler of the press). The ISS's digipeat comes back through the normal APRS receive only when the
  radio listens on 145.825 (APRS receives on the APRS frequency: region custom 145.825 and a channel there); nothing
  more is built for it. Host test `tests/plus/test_aprs_iss.c` (in `make test`): upstream's APRS radio suite again
  with it built in, then the frame keyed on 145.828 MHz (Doppler +3 kHz) with path ARISS, our callsign and the
  position, the radio back on its channel; no callsign / no position / not a ham band refused with nothing keyed;
  30 s between sends; one at a time; alone in its transmission, an ordinary beacon after it on 144.390 with its own
  path; it goes while the region frequency is blocked. Radio rows SM4-SM5.
- [built] Satellites: pass alarm and Doppler tracking (part of `CONFIG_SATELLITES`; `prepare_sat_alarm` hooks the
  alarm into `ui_app_tick`; round 3, parallel job "sat"):
  - **Pass alarm.** On Tools > Satellites, OK on a satellite with nothing to track (or 0 on any) opens the "Pass
    alarm" page: For (a satellite, or Any satellite), Before AOS (2, 5 or 10 min), Min. elevation (0, 10, 20, 30,
    45 degrees: the pass's top at least that); Up / Down a row, Left / Right (or the knob) its value, OK arms it or
    turns it off, Back the list; a change while armed arms it again. It sounds on any screen: N minutes before the
    rise two short 1 kHz beeps (`BEEP_TOT_WARN`) and a banner "ISS in 5 min, max 47°" with the backlight on (the
    info banner, 8 s, as the low battery warning: a 1.8 s toast is too easy to miss; over a banner that waits for
    the user, the zone offer, a toast instead); at the rise one short 1.4 kHz beep (`BEEP_SCAN_HOLD`) and "ISS AOS
    now, max 47°"; then it is off (one pass; arm again for the next). Armed late (inside the lead), the warning
    comes at once with the minutes left. RAM only: off after a power cycle. The page shows the pass found ("SO-50
    13:01 23°", "Tone at 12:56"), "Alarm: searching…", "Alarm: none in 36 h" (looked again 18 h later), or "Armed:
    waits for GPS"; the list's status line shows "Alarm 12:56: SO-50 23°" when nothing else is there.
    Cost (radio/sat_svc.c, radio task): the pass is found once, a propagation a radio poll (the screen's own search
    result is taken when it is the next pass from now: then at once); a pass under way or below the elevation is
    skipped by searching on from 5 s after its set; for Any, every satellite is searched and the soonest kept (a
    satellite stops once it can't beat it). Then only the clock (the GPS's UTC, once a second) and an integer
    comparison once a second until it is due: no SGP4 (the host test counts the live look: zero while waiting). A
    clock jump of over a minute or a move of over 0.2 degree looks for the pass again. Found in 58-77 polls for one
    satellite, 350-430 for Any (25 ms polls: 2-11 s) in the host test.
  - **Doppler tracking** (the existing OK on a pass under way or due within 10 min, now "OK track"): the status
    line "Tracking Doppler"; the live lines show the azimuth and elevation, the range and range rate ("2345 km
    -5.43 km/s", positive going away) and RX / TX with each shift in kHz (TX: the uplink tone after it; " kHz"
    dropped there when the line is full). Changes to the tracking: (1) **hysteresis**: the VFO moves once the
    correction is more than 5/8 of a step off (`sat_follow_hz`: 625 Hz VHF, 1,562 Hz UHF), to the nearest step, so
    a GPS clock correction of a second can't move it back and forth over a step's edge; the step stays a policy (1
    kHz VHF, 2.5 kHz UHF: FM is that tolerant, about 8-12 moves a pass), not the hardware's: the FD6818 synthesizer
    is set in 10 Hz units (registers 0x38 / 0x39); (2) an **uplink-only move** asks only `radio_req_vfo_tx` (the
    receiver is not retuned; `radio_req_vfo_set` moves a split's TX along, so RX is always followed by its TX); (3)
    **at LOS** (30 s after the set) the VFO goes **back to the nominal downlink and uplink** (437.800 / 145.990 for
    the ISS), ready for the next pass, if it is still where the tracking put it (status "Over: VFO on nominal");
    OK during the pass ends it with the VFO left on the corrected frequencies ("Tracking off: VFO stays"), a hand
    change ends it with the VFO the user's (as before). Never a request while PTT is held or the radio transmits
    (the restore at LOS waits for the end of the transmission too); the same VFO requests the Repeaters screen uses
    (`radio_req_vfo_set` / `radio_req_vfo_tx`, mode, bandwidth, tone); nothing keys the transmitter (the service
    links against no PTT call: the host test's fake radio has none).
  - Frequencies: from `sat_freq.txt` (no new table): ISS 437.800 down / 145.990 up 67.0 Hz (the cross-band FM
    repeater; 145.800 is the ISS's voice / SSTV downlink, not a repeater pair), SO-50 436.795 / 145.850 67.0 (74.4
    arming), AO-91 145.960 / 435.250 67.0, PO-101 145.900 / 437.500 141.3. The repository records no current
    status for any of them beyond "check against AMSAT's current status before release" (sat_freq.txt); AO-27 is
    not in the shipped elements and is not added.
  - Screen fit on the DM-32 (220 px between the insets, the big fonts): the passes' length column is left out
    where it doesn't fit, "no pass soon" in the small font, "AOS in" / "LOS in" for "rises in" / "sets in", the
    slowest propagation moved from the clock line (no room) to the alarm page ("SGP4 at most 1.5 ms").
  - Host tests (in `make test`): `tests/plus/test_sat_svc.c` (the service with a fake GPS and radio core: the
    alarm for one satellite at 2 / 5 / 10 min and with a 30 degree floor, and for any satellite at 0 and 40
    degrees, each pass against a one-second brute-force scan, the warning and the rise each within a second, armed
    late, armed during a pass, the screen's result taken, off, none that high, armed before the GPS time; the
    tracking of each shipped satellite over a real pass: the first request sequence, every move a whole number of
    steps, the VFO never more than the hold plus a second's drift off the true Doppler, no request at all through
    40 s of PTT at the top, back to nominal after the set (waiting for PTT), a hand change and OK leaving the VFO
    alone; `sat_follow_hz` steps and hysteresis); `tests/plus/test_sat_doppler.c` with `tests/plus/sat_doppler_ref.py`
    (an independent Python SGP4, checked against Vallado's verification output to 5e-9 km, and frames the other
    way round on the same tle.txt: 2,491 samples of the four satellites over two days from Ankara, the radio's
    Doppler at 145.8 MHz within 0.5 Hz of the reference (limit 100 Hz), elevation within 0.0001 degree);
    `tests/plus/test_ui_sat.c` (26 flows in the simulator at the DM-32's geometry: the list, the live lines,
    tracking and stopping, the alarm page and each row, arm, change, off, Back, 0, the banner and the tones on
    Home, the toast over a zone offer, no GPS time, and the layout check on the list and the page).
  - FLASH +4,032 B (777,924 -> 781,956), RAM +144 B (208,072 -> 208,216: the alarm's search state and pass 68 B,
    the view's alarm 12 B, the request and the event 14 B, the banner text 40 B, the page 5 B, alignment); stacks unchanged
    (radio 824 B free, UI 368 B free; from `sat_service` 1,788 B deep, `alarm_job` 1,676 B, `ui_sat_alarm` 300 B
    under `ui_app_tick`, the screen's draw 1,028 B). Radio rows PA1-PA7.
- [built] Satellites: ISS notifications and auto track (part of `CONFIG_SATELLITES`; `tools/plus_satauto.py`
  `prepare_sat_auto`, `integration/radiofoundry/sat/` sat_svc.c, ui_sat.c, radio_sathold.inc; owner's request "ISS
  automatic tracking, automatic frequency and an ISS notification pop-up"; branch issauto). Built on the pass alarm and
  the Doppler tracking above; one alarm at a time, as before.
  - **The pass alarm page** has two more rows: **Every pass** (Off / On; "Her geçiş") and **Auto track** (Off / On /
    APRS; "Oto. izleme"). Every pass is for the ISS or Any satellite (what the settings can keep): turning it On with
    another satellite in For picks the ISS, moving For to another satellite turns it Off. OK arms ("Armed for every
    pass") or turns it off as before; a change while armed arms it again. With Every pass the alarm is not off after
    the rise: the radio task looks for the next qualifying pass at once (the existing search: the pass under way is
    skipped) and announces every pass the same way (two beeps and "ISS in 5 min, max 47°" N minutes before, a beep and
    "ISS AOS now, max 47°" at the rise). The page's line reads "Kept across power cycles" instead of "Off after a power
    cycle"; with Auto track on, the line under it says what it listens to ("Tracks ISS 145.800, RX", "Tracks ISS
    145.825, RX", or "Tracks the downlink, RX" for another satellite or Any).
  - **Stored** (the settings store has room: spare bits, no new field, the record's size and layout upstream's):
    Every pass with its lead and lowest elevation in `settings_t.cp_flags` bits 1-6 (`SETTINGS_CP_SAT_NOTE`: bits 1-2
    for whom, 0 off, 1 ISS, 2 any; bits 3-6 an index of the 3 x 5 lead / elevation choices, 0 = 5 min, 0°; next to
    "Use anyway" bit 0, which app.c changes through a mask, and FM caller ID bit 7), Auto track in `settings_t.rs_flags`
    bits 6-7 (`SETTINGS_RS_SAT_AUTO`: 0 off, 1 the downlink, 2 the ISS's APRS); the radio core's own rs_flags write
    (`save_settings` in radio_sm.c) now keeps those two bits. An older record and a factory reset read 0 = off; Classic
    firmware ignores the bits (its own rs_flags write clears auto track). Through the UI storage service
    (`UI_SET_SAT_NOTE`, `UI_SET_SAT_AUTO`). At power on the first UI tick (`ui_sat_alarm`) arms the stored alarm again;
    it finds its pass once the GPS has the time and a position (needs the GPS on, as the alarm always did).
  - **Auto track** (needs an armed alarm, one pass or every pass: no silent mode; the notifications are the alarm's):
    at the rise, when the UI says the radio is free (each UI tick `sat_ui_idle`: Home or the Satellites screen open,
    radio idle, no scan or watch, no hotspot, nothing transmitting) and nothing is tracked, the radio task asks the
    radio core to hold the user's state (`radio_req_sat_hold`, new request `RREQ_SAT_HOLD`, taken before upstream's
    "anything the user does stops X" checks so it stops nothing). The radio core refuses unless it is free too (no
    PTT, monitor, call, scan or watch, DMR / P25 / All digital search, P25 or DMR trunk following, DOTA, cross-band,
    APRS sending, CW beacon, hotspot, air watch, a ring letter's C..F VFO in use); taken, it notes the position and
    the VFOs where a power off notes them (`persist`), then notes nothing more (`persist` returns at once) and refuses
    TX (`TXDENY_RX_ONLY`: auto track is receive only, whatever the screen). Only once the hold is taken (one to three
    seconds) does the VFO move: the existing tracking on the satellite's downlink alone, simplex, FM wide, no tones
    (ISS: 145.800 voice / SSTV, or 145.825 with APRS; another satellite its downlink), Doppler corrected in the same
    steps and hysteresis; the banner "ISS tracking, max 47°" (no second tone). It ends 30 s after the set, on any key
    (`ui_sat_auto_key`, first in `ui_app_key`: the key is used for that, toast "Auto track off: radio back"), on PTT
    (refused, the end once released), the screen's OK, a change of the VFO from elsewhere, or the elements failing; the
    hold's end puts the VFO in use and the position back as the radio's start takes them (`radio_sm_init`'s way: the
    channel, or the VFO with its frequencies, mode, width, tones, talk target). A power off during it comes back as
    before it (nothing was noted). Not started in a later second of the same pass (only at the rise); a refused hold
    moves nothing.
  - **No pop-up while transmitting**: `ui_sat_alarm` takes no event while the radio transmits (or PTT is refused); the
    banner and tone come after the release, the minutes left counted again then (the event keeps the rise).
  - Host tests (in `make test`): `tests/plus/test_sat_svc.c` adds every pass (three consecutive ISS passes at 10° or
    more, each warning and rise within a second of the one-second scan, armed again at once for the next), auto
    track from a channel on 145.800 and on 145.825 (the hold first, the set-up a second later: FM, wide, no tones,
    simplex; the VFO within the hold plus a second's drift of the true Doppler all pass, TX = RX; "tracking" after the
    rise's event; 30 s after the set the hold released and the channel back, the alarm armed for the next pass), a
    key ending it at once and PTT ending it on release (the split VFO / channel back, nothing more that pass), and no
    start off Home or busy, with PTT across the rise, with the radio core refusing (asked, let go, nothing moved) or a
    pass already tracked by hand; `tests/plus/test_sathold_radio.inc` (`test_radio_sathold`: upstream's and Classic's
    radio suites with it built in, then the hold from a channel and from a split VFO with a tone, nothing noted, TX
    refused and PTT denied, a reboot meanwhile, refused with PTT, monitor, a call or a scan, and the radio core's
    settings save keeping the auto track bits); `tests/plus/test_sat_set.c` (the two settings through the UI storage
    service and settings store across power cycles, next to "Use anyway", FM caller ID and the radio core's rs_flags
    bits, defaults off); `tests/plus/test_ui_sat.c` adds the page's rows, Every pass stored / changed / off, the ISS
    picked, auto track stored, the stored alarm armed at power on, idle only on Home, the banner held back during TX,
    the tracking banner, a key and PTT ending an auto track, and the layout check on the page with both rows on.
  - FLASH +2,624 B (848,544 -> 851,168), RAM +24 B (209,792 -> 209,816: the hold flag, the UI's idle word, the cancel,
    the auto state, the alarm's options request, the page's two values and the power-on flag, alignment; the alarm's
    options and the auto track's wait and top sit in what was padding); stacks unchanged (radio 728 B free, UI 184 B
    free, the deepest paths not these). Radio rows PA8-PA15.

- [built] Tools > ISS and the AOS pop-up (part of `CONFIG_SATELLITES`; `tools/plus_satauto.py` `prepare_iss_tool` and
  the pop-up hunk of `prepare_sat_auto`; `integration/radiofoundry/sat/` ui_sat.c `ui_iss_screen`, sat_svc.c
  `sat_auto_start` / `sat_auto_mode`; owner's request 2026-11-22; branch issmenu). Reuses the satellites' code: the
  pass search, the live look, the pass alarm page's rows and the auto track's hold and return; Tools > Satellites stays
  for all satellites.
  - **Tools > ISS** ("ISS" in both languages) is tool 40, after All digital search: only Battery (now 41, last by
    design) and the zone offer's 0 move one on, every other tool keeps its number. One page (240 x 320, small font
    where a line is long): "AOS in 1h05  47°" ("LOS in 5:12  47°" in the rx color under way), "AOS 14:27  LOS 14:38"
    (local time, the Satellites screen's offset; "Above now, max 47°" under way), the live "Az 123°  El 23.4°", the
    two downlinks "> 145.800 voice / SSTV" and "145.825 APRS" (the one in use marked), "RX 145.803 +2.6 kHz" (that one
    with the Doppler of now; the tuned frequency during a held track), a line, then the pass alarm page's rows from
    Before AOS on (Before AOS, Min. elevation, Every pass, Auto track) in the small font, and a status line
    ("Tracking, back at LOS", "Alarm AO-91 armed" for another satellite's alarm, "OK: track in the VFO", the ISS alarm
    "Alarm 14:27: ISS 47°", or "Track from AOS-10 min"). **The rows always edit the ISS's alarm** (For is the ISS):
    Every pass On arms the ISS's notifications at once (kept across power cycles as on the alarm page), Off disarms
    them (only the ISS's alarm); a change of the other rows while Every pass is on arms it again with the change; Auto
    track is the same stored setting. Another satellite's (or Any satellite's) alarm is shown on the status line and
    left alone by the other rows; Every pass On replaces it (one alarm at a time) with the toast "Every ISS pass: other
    alarm off" ("Her ISS geçişi: diğer alarm kapalı"). **OK**: within 10 min of AOS or during a pass, asks for the held
    track (below; "ISS tracking, max 47°" once the radio holds, else a refusal toast); any track running: stop it
    ("Auto track off: radio back" or "Tracking off: VFO stays"); otherwise the toast "Track from AOS-10 min". **#**:
    APRS via ISS as on the Satellites screen (the same gates). **1**: the other downlink (145.800 / 145.825), switched
    under a running held track too (the next second's tune). Hint "OK track  # APRS  1 freq" ("OK stop ..." while
    tracking). While a held track runs, keys on this page stay the page's (they do not end it as on other screens);
    PTT and monitor still do. The page counts as idle for the automatic auto track (as Home and Satellites).
  - **The held track** (`sat_auto_start(idx, mode)`, a UI request to the service): the auto track's machinery started
    by the user. The pass: the screens' search result when there is one, else the last alarm rise's pass (the service
    keeps the rise's satellite, top, AOS and LOS: what the pop-up offers from Home, where no screen searched); only a
    pass under way or due within 10 min. Then the radio core is asked to hold the user's state, and only once it does
    does anything move: the downlink alone (ISS 145.800, or 145.825), simplex, FM wide, no tones, Doppler corrected in
    the same steps; 30 s after the set (or a key off Tools > ISS, PTT, a hand change) the radio back exactly where it
    was (`radio_sathold.inc`'s end). **The UI says "tracking" only on the service's word**: the existing
    `SAT_ALARM_TRACK` event ("ISS tracking, max 47°" banner) once held; a refusal is a new event `SAT_ALARM_NOTRACK`
    with the reason, shown as a toast: "Can't track: no pass now" (no pass within 10 min, after LOS, or no GPS clock),
    "Can't track: a track runs" (a pass tracked by hand from Tools > Satellites goes on untouched), "Can't track: radio
    busy" (the radio core refused the hold within 3 s: scan, search, call, DOTA, cross-band, hotspot ...; nothing
    moved). `sat_auto_mode` switches a held ISS track's downlink while it runs. The Satellites screen's own OK track
    (437.800 / 145.990 split, TX allowed) is unchanged.
  - **TX during a held (or auto) track**: PTT is refused by the radio core (`TXDENY_RX_ONLY`, "RX only"), and PTT ends
    the track once released (the radio back as before). No APRS frame of the radio's own keys meanwhile: beacons
    (smart / timed), the SOS position, digipeats and message acks wait in the queue until the hold ends
    (`aprs_can_start` checks `sat_held`). The one exception is the frame the user asks for with **# APRS via ISS** on
    Tools > ISS or Satellites: `aprs_rf_next_iss` moves it to the head of the queue and it is sent alone on
    145.825 MHz + uplink Doppler (CSMA there first), then the radio returns to the tracked downlink; the service tells
    PTT apart from the radio's own transmissions (`sat_service`'s `SAT_TX_PTT` / `SAT_TX_OWN`), so that frame does not
    end the track. The ISS frame's own gates (callsign, position, 30 s apart) are as before.
  - **The AOS pop-up** (Home only): with the pass alarm armed (notifications on: one pass or Every pass), Auto track
    Off and Home open (no menu, editor or typed frequency), the rise's banner reads "ISS overhead, max 47°" ("ISS
    tepede, maks 47°") with "OK: track" ("OK: takip") under it, the rise's beep as before, up 60 s instead of 8
    (countdown), and Home's hint row reads "OK track  Back" ("OK takip  Geri") meanwhile. It takes two key presses
    only: OK asks for the held track on that satellite's downlink (the ISS: the one chosen on Tools > ISS, 145.800 at
    power on) and hides it, the service's word following as above; Back hides it. Every other key, a long press and
    the knob are Home's as always (the pop-up stays up). On any other screen the rise gives the plain 8 s "ISS AOS
    now, max 47°" banner with no key taken. Not while transmitting (as every satellite banner: it waits for the end
    of TX); over a zone offer it is the toast as before. With Auto track On or APRS the rise's banner is "ISS AOS now"
    as before and the auto track starts by itself.
  - **Fixed: the Satellites screen's hint without a GPS fix**: in the "Waiting for the GPS time / position" state the
    hint was "Back" alone, so its left part (where every other state names OK) was empty although OK opens the pass
    alarm page there (sat_key: nothing to track). It now reads "OK alarm  Back".
  - Host tests (in `make test`): `tests/plus/test_sat_svc.c` adds the held track (15 min early refused with
    SAT_NO_PASS; 5 min before the rise from a channel without the UI's idle word: the hold, 145.800 with Doppler,
    simplex, "tracking"; switched to 145.825 under way; the VFO within the hold plus a second's drift of the true
    Doppler; 30 s after the set the channel back), refused over a pass tracked by hand (that one goes on), and the
    pop-up's path from Home with no screen looked at (the alarm's own pass, two passes back to back, the radio's own
    frame for 3 s not ending the track, OK after LOS refused, the radio core busy refused with nothing moved);
    `tests/plus/test_aprs_iss.c` adds the hold on the real radio core and APRS engine (a beacon held back, the ISS
    frame past it on 145.825 + Doppler with the hold kept, the beacon after the hold); `tests/plus/test_ui_sat.c`
    (now built with Turkish) adds Tools > ISS in English and Turkish (the page, layout checks: plain, a held track,
    above now, no GPS time, Every pass on, another satellite's alarm; OK asks for the track with no toast and "ISS
    tracking" only on the service's word, refusals as toasts, 1 then OK on 145.825, too early, OK stops a held track
    with the keys not taken by the auto track, Every pass on / changed / off stored, AO-91's alarm shown and left alone
    and then replaced with the toast, auto track row, # below the horizon) and the pop-up (shown with its line, beep
    and Home's hint, OK on 145.800 or 145.825 with the service's word or refusal, Back hides it, another key, a long
    OK and the knob left to Home, the plain banner and no key taken off Home, up 60 s and gone after, held back during
    TX and actionable after, no pop-up with Auto track On), and the fixed hint.
  - FLASH +3,688 B (857,584 -> 861,272), RAM +16 B (209,856 -> 209,872: the alarm rise's pass and the held track's
    flag in the service, the downlink switch, the pop-up byte; the alarm text moved out of its function); stacks
    unchanged (radio 728 B free, UI 184 B free). Radio rows IS-1-IS-11 with IS-4b, IS-6b and IS-6c.

- [built] P25 Phase 1 trunk following (`CONFIG_P25_TRUNK`, `prepare_p25_trunk`; parallel job; receive only):
  Tools > P25 trunk (tool 24, before Battery) follows a trunked system from the **selected channel**: a P25 channel
  on the control channel's frequency with its NAC (or Any) and the talkgroup to follow (Any or Left / Right on the
  screen: every group). A Tools screen rather than a channel option: no codeplug or CPS change, and the screen is
  where the system and the grants are shown; like the P25 search it owns the receiver while open (leaving it stops
  after 5 s, Back at once) and any other request ends it. **How**: the native P25 receiver runs on the control
  channel with any talkgroup; the framer (src/p25/p25.c, only while asked) also takes TSDUs (DUID 7) and returns
  each 196-bit TSBK block, which the worker decodes (trunk/p25_tsbk.c: deinterleave, 1/2-rate trellis by Viterbi,
  CRC-16/GSM, last-block flag ends the TSDU) into an 8-entry queue. The radio task (native_lab.c `trunk_service`)
  hands them to the follower (trunk/p25trunk.c): IDEN_UP / IDEN_UP_VU / IDEN_UP_TDMA keep the channel plan (16
  identifiers: base, spacing, TDMA slots), NET_STS_BCST / RFSS_STS_BCST the WACN, system ID, RFSS and site,
  GRP_V_CH_GRANT / _UPDT / _UPDT_EXP the grants (standard MFID only; vendor and protected TSBKs ignored). A grant for
  a followed group on an FDMA channel this radio receives moves the receiver: the worker is stopped and the channel
  receive starts again by itself on the new frequency (`channel_rx_hz`); the call plays through the normal P25 voice
  path (Home caller line, call log, replay), and the receiver returns to the control channel on a terminator
  (TDU/TDULC), 1 s without a frame after voice, or 1.5 s with no frame at all after the retune (3 s if it never got
  there). Not followed (shown in the grant list): other groups ("-"), an encrypted grant (service option bit, "ENC";
  a call whose voice turns out encrypted plays muted with the existing encrypted notice), Phase 2 TDMA channels
  ("P2"), an identifier not yet heard ("?"), a frequency outside VHF/UHF ("band": 700/800 MHz systems can be watched,
  not followed). **No preemption**: the one receiver can't hear the control channel on a voice channel, so a grant
  for another group never interrupts a call; after a call its grant updates for the same group and channel are held
  off 2 s (10 s after a retune that found no call) so a call's late updates don't pull the receiver back. Control
  channel lost (no valid TSBK for 3 s): "Searching" on the same frequency (no hunting of other control channels).
  **Receive only**: nothing is ever sent (no registration, affiliation or ISP request); PTT is denied as on an RX-only
  channel (TX denied banner, nothing keyed) and the following goes on. Screen: CC frequency and the groups, state
  (Searching / Control locked / Voice f), WACN / SYS / NAC, Site rfss-site, TSBK/s and failed blocks, the call now or
  last (TG, source unit from the grant or the voice's link control, age), then the grants heard newest first (TG,
  frequency or channel, why not followed, age). The ID database is not used: P25 unit IDs are not DMR IDs. Retune:
  in the host model the receiver runs on the voice channel 3 radio task turns after the grant reaches the radio task;
  on the radio that is the worker letting go (<= 5 ms polls), the native session's stop and start (FM restore, then
  DMR mode and the PLL on the new frequency) and the first frame sync: about 0.2-0.4 s to audio is expected
  (unmeasured: radio row TK7). Build: RAM 203,784 -> 204,496 B (+712: the follower and the screen's copy 2 x 184 B,
  the plan 128 B, the TSBK queue 80 B), FLASH 702,740 -> 708,748 B (+6,008); radio task stack 828 B free (the
  follower runs off the deepest path, `noinline`), UI 368, p25-lab 912: unchanged. Classic (builds, no warnings):
  the framer, voice and worker changes are compiled out; only the Tools and screen lists gain the hidden entry.
  Host tests (tools/test_classic.py):
  tests/plus/test_p25trunk.c (the decoder against the test's own encoder written from the standard's state table,
  constellation and interleave order plus a second greedy decoder, CRC-16/GSM vectors, 1-4 errors corrected, noise
  never accepted; framer 1/3-block TSDUs, NAC filter, off for conventional receive; plan for 800 / 700 / UHF / VHF /
  TDMA; grants; the follower: grant > voice > terminator / fade / no call / no retune > back, hold-offs, late-entry
  source, lost > searching, every group, idle stop), tests/plus/test_trunk_worker.c (TSDUs through the real channel
  worker, PHY record mapping and framer: every TSBK out in order, a damaged block counted, voice and terminator
  counted; conventional receive frames none), tests/plus/test_trunk_sm.inc in test_radio (the real radio state
  machine and native admission: control channel taken, grant > voice channel > terminator > back, PTT denied, another
  request ends it, idle stop gives the P25 channel back, an FM channel can't start it). Unverified: no real control
  channel was heard (the trellis table and interleave are the standard's as decoders print them, cross-checked two
  ways, but not against a capture); GRP_V_CH_GRANT_UPDT_EXP's channel (T) as the voice frequency follows OP25.
  Radio rows TK1-TK8.

- [built] DMR Tier III trunk following (`CONFIG_DMR_TRUNK`, `prepare_dmr_trunk`; parallel job; receive only):
  Tools > DMR trunk (tool 25, right after P25 trunk) follows an ETSI TS 102 361-4 trunked site from the **selected
  channel**: a DMR channel on the site's control channel (TSCC) frequency with its color code (or Any); the groups
  followed are the channel's talkgroup (its contact) and its RX group list, or every group (RX group list Any, or
  Left / Right on the screen). Same model and UX as Tools > P25 trunk: it owns the receiver while open (leaving the
  screen stops it after 5 s, Back at once), any other request ends it, PTT is denied. **How CSBKs reach the CPU**: the
  HR_C7000 does the BPTC(196,96) decoding and the CRC check of data bursts itself and hands every CSBK / MBC header /
  MBC continuation to the driver as 12 octets with its CRC verdict (bad CSBKs and headers are dropped in the driver,
  `hrc7000_dmr.c`); the call stack (`dmr_call.c rx_burst`) passes them after its color code and slot filters. No
  software BPTC runs on the radio. While following, a new hook (`dmr_call_set_trunk_rx`) takes them before the data
  service: nothing is answered (no radio check / call alert ack) and they don't light the data RX indication. MBC
  continuation blocks carry no CRC the chip can check per block, so with `CONFIG_DMR_TRUNK` the driver passes them
  with the chip's flag (as it does data blocks) instead of dropping them. **Decoding** (`dmrtrunk/dmr_csbk.c`,
  the radio task): the CRC-CCITT is checked again in software (CSBK mask 0xA5A5, MBC header 0xAAAA; failures counted as
  "bad"), standard FID 0x00 only (vendor CSBKs and protected ones are ignored). C_ALOHA (25) locks the TSCC and gives
  the system identity code (model Tiny / Small / Large / Huge, net, site; shown +1 as the standard numbers them);
  C_BCAST (40) is counted (Ann_WD_TSCC, Vote_Now, Site params, Adjacent site parsed, not acted on) and a Chan_Freq
  announcement's MBC continuation (absolute channel parameters, CDEFTYPE 0: LPCN, TX / RX MHz + 125 Hz steps) fills
  the channel plan (16 LPCNs); TV_GRANT (49), BTV_GRANT (50) and PV_GRANT (48) give LPCN (12 bits) + slot, target,
  source and the emergency flag; a grant with LPCN 0xFFF sent as an MBC takes its channel from the continuation
  (absolute grant). The plan's "RX" frequency is the downlink (the standard names them for the MS); when the TSCC's
  own frequency shows up as a "TX" one, the plan is read the other way round. **Following** (`dmrtrunk/dmrtrunk.c`):
  a grant for a followed group whose frequency is known and in this radio's bands moves the receiver
  (`radio_sm.c dmrtk_tune`: `rx()` on the payload frequency, the stack reconfigured with the granted slot locked as on
  a repeater, the channel's color code, every group heard, the modem's slot timing restarted); the call plays through
  the normal DMR voice path (Home caller line with the ID database name, call log, replay). Back to the TSCC (both
  slots) on P_CLEAR on the payload channel, 1.5 s after the call's voice ended (terminator or fade; another over in
  that hang is heard), 1.5 s with no voice at all after the retune, or at once when the voice turns out encrypted
  without a key (existing privacy handling: muted; listed "ENC"). Its grants are held off 1 s after a call (10 s after
  a miss or an encrypted call) so the site's repeated grants don't pull the receiver back. Private calls (PV_GRANT)
  are followed with every group only. Not followed (listed with why): another group ("-"), a private call ("priv"),
  an LPCN whose frequency is unknown ("?", shown as "ch n": no Chan_Freq / absolute parameters heard, e.g. a system
  whose radios carry a fixed channel plan), outside the bands ("band"), heard during or just after a call ("busy").
  **No preemption** (one receiver). TSCC lost (no Tier III CSBK for 3 s): "Searching" on the same frequency.
  **Receive only**: `radio_sm_tx_check` answers TXDENY_RX_ONLY while following, so PTT, data, the emergency alarm and an
  SFR repeater role key nothing; no registration, random access or acknowledgement is ever sent. Screen: CC frequency
  and the groups, state (Searching / Control locked / Voice f TSn), the system (model, net, site or "System unknown"),
  CSBK/s, CRC failures and LPCNs known, the call now or last (TG or ID, source ID with its ID database name, ENC, age),
  then the grants (target, frequency or "ch n", /slot, EM, why, age). **Not in scope**: Motorola Capacity Plus /
  Connect Plus, Hytera XPT and other vendor trunking are not ETSI Tier III (vendor FIDs, other opcodes): such a
  control channel stays "Searching" and lists nothing. Retune: in the host model the receiver is on the payload
  channel 25 ms of radio task time after the grant burst reaches the stack (the stack's next 10 ms tick decodes it,
  the next 25 ms radio poll retunes); on the radio add the HR_C7000's burst report (<= 30 ms), the PLL and the modem's
  slot sync on the payload outbound (one or two bursts, 30-60 ms): about 0.1-0.15 s to the first payload burst when
  the voice header is heard; joining a call already running needs its embedded LC (late entry, 0.36-0.72 s).
  Unmeasured: radio row DT7. Build: RAM 204,520 -> 205,248 B (+728: the follower's view and the screen's copy 2 x
  212 B, the channel plan 192 B, MBC headers 22 B, timers), FLASH 710,224 -> 715,552 B (+5,328); stacks unchanged
  (radio 828 B free: the request and the retune are `noinline` helpers off radio_sm_request's frame; UI 368; dmr 800).
  Classic (builds, no warnings): compiled out, only the hidden Tools entry. Host tests (tools/test_classic.py):
  tests/plus/test_dmrtrunk.c (CRC-16/GSM catalogue vector and the CSBK / MBC masks; BPTC(196,96) from the test's own
  encoder written from the standard's matrix, equal to upstream's encoder for 200 random blocks, upstream's decoder
  correcting every single error; C_ALOHA system codes of all four models, C_BCAST types, grants, P_CLEAR, MBC
  channel parameters; the follower: lock, plan, grant > payload slot > voice > hang / P_CLEAR > back, hold-offs, no
  call, encrypted, absolute grant with and without its continuation, band, other / private / RX group list / every
  group, lost, plan orientation, idle stop), tests/plus/test_dmrtrunk_sm.inc in test_radio (the real radio state
  machine and call stack with the simulated modem: TSCC on both slots, CSBKs and an MBC through the stack to the
  follower, another group listed, the grant > 438.5125 MHz with slot 2 locked in 25 ms, the call played, PTT denied
  (TXDENY_RX_ONLY) with nothing keyed, terminator + hang and P_CLEAR > back, no transmission at all, another request
  ends it, every group, idle stop, an FM channel can't start it). Unverified: no real Tier III site was heard; the
  CSBK field layouts follow TS 102 361-4 as decoders (DSD-FME, SDRTrunk) print them, but the MBC continuation's
  layout (reserved octet 1, LPCN at bit 16) and RX-as-downlink are the least certain; whether the HR_C7000 reports MBC
  blocks with data type 4 / 5 as expected is unchecked. Radio rows DT1-DT8.

- [built] dPMR receive channels (`CONFIG_DPMR`, `prepare_dpmr`, docs/dpmr.md; parallel job; receive only): a
  channel mode "dPMR" (Mode cycles ... NXDN -> dPMR -> FM; record CH_MODE 0xA8) with a "CC" row: the colour /
  channel code 0-63 (TS 102 658 Table 6.1 numbering) or Any (the default; dPMR446 radios of TS 102 490 V1.2.1 take
  their code from the channel). **How**: dPMR's PHY is NXDN48's (2400 symbols/s 4FSK, +-1050 / +-350 Hz, RRC 0.2 x
  sin(x)/x), so the NXDN path is reused whole: FM mode, flat receive audio, NXDN's software demodulator, its 20 ms
  voice pacing into the stock AMBE+2 vocoder; src/dpmr is the frame layer, written from ETSI TS 102 490 / TS 102 658:
  FS1 / FS2 / FS3 sync in both polarities, the header (HI0 / HI1: header type, called and own 24-bit IDs, mode,
  format; CRC8, (12,8) Hamming, 12 x 10 interleave, x^9 + x^5 + 1 scrambler; the two copies combined word by word),
  the superframe's CCH (frame number, the IDs in quarters, mode, slow data; CRC7, 12 x 6 interleave) for late entry,
  the colour code in frames 2 / 4 and the header, the end frame. The four 72-bit voice blocks of a frame are DMR
  AMBE+2 air frames and go to `ambeVoiceSink()` as NXDN's do. Data calls and voice+data frames marked as data are
  reported, not played; no field marks encryption in dPMR, so nothing is shown as encrypted. Home: "dPMR" and the
  caller line "dPMR 1234567 > 7654321 CC 16" (IDs as TS 102 658 annex A dial strings, "CID n" for dPMR446 common IDs,
  "ALL"); call history "dPMR 1234567" / "> 7654321 CC 16 5m"; PTT denied (RX only); scan and dual watch as NXDN
  channels. **RAM**: the dPMR receiver shares NXDN's receiver object (a union in radio_nxdn.c; one listens at a time,
  the poll of the other mode stops it first; `_Static_assert` that it fits): +120 B static (Home's caller line 80, the
  caller and the call-history copy), FLASH 710,236 -> 716,344 B (+6,108); stacks unchanged (radio 828 B free, UI 368,
  audio 1608). Host tests: tests/plus/test_dpmr.c (NXDN's suite first, then the coding against an independent
  encoder and whole transmissions through the 4FSK modulator: noise, carrier / clock / deviation error, de-emphasis,
  inverted, late entry, foreign CC, data, back to back callers; noise, speech-like FM and NXDN48 audio give no call),
  test_dpmr_channel.c (the record), test_ui_dpmr.c (9 editor flows). Unverified: no dPMR signal heard yet; the
  scrambler's bit order and the voice block layout follow dsdcc's dPMR decoder (docs/dpmr.md "What the radio must
  confirm"). Radio rows DP1-DP6.
- [built] Repeater list and nearest repeater (`CONFIG_REPEATERS`, `prepare_repeaters`; owner's item 6 "Röle listesi
  ve en yakın röle"; parallel job, 2026-11-19): Tools > Repeaters (tool 26, after DMR trunk; Battery and the zone
  offer move one on) lists the built-in repeaters **nearest first** from the position APRS uses
  (`aprs_position_now`: GPS, else the computer's, else the fixed one; its ambiguity setting applies), each row the
  callsign, distance and compass direction ("12.3 km NE"), RX frequency, mode (DMR: CC and slots, FM: the tone) and
  city. Without a position: "No position: sorted by frequency" and the table's order. Up / Down / the knob pick
  (round from the end to the start), OK the details (RX / TX, offset or "Simplex", tone and bandwidth or CC / slot,
  city, Maidenhead locator, distance, bearing in degrees), OK again **tunes the VFO**, # opens the nearest one's
  details at once (the quick action; the screen also opens on the nearest), Back from the details to the list.
  **Since item 37 (below) the screen opens on a province list first**; the list described here is its "All" row
  ("Hepsi") and each province's list. Tuning uses the requests the VFO's own settings use, in order: `radio_req_vfo_set(RX)` (opens the VFO),
  `radio_req_mode`, then FM: bandwidth (25 kHz, 12.5 kHz when the list says so), the access tone as the TX CTCSS and
  no RX tone; DMR: colour code and slot (TS2 when only TS2 is linked, else TS1; the VFO's talk group stays), then
  `radio_req_vfo_tx(RX + offset)` (0 = simplex, so an old split never stays) and Home. Refused with a toast while
  transmitting, outside the radio's bands, for C4FM / D-STAR / P25 / NXDN / M17 entries ("Info only") or in a build
  without that VFO mode; a TX outside the bands tunes RX only, simplex, and says so. Nothing is written to any
  flash. **Order without RAM**: the table is sorted by RX frequency once (generator); the screen keeps only the
  picked and the top row's entries (5 B); the next / previous entry in the order is found by one pass over the
  table (`rpt_next`: integer squared equirectangular distance in 1e-5 degree, the longitude scaled by cos(latitude)
  in Q15, ties by table index: a total order, so walking forward and back visits each entry once); a page draw is
  rows x n key computations (8 x 100 here), the true distance and bearing (`aprs_distance_m` / `aprs_bearing_deg`)
  only for the rows drawn. A static `uint16_t` index array was not chosen: it would cost 2 B an entry (200 B now,
  1 KB for an owner list of 500) and need re-sorting as the position moves. **Data**: (1) RadioID.net's repeater list
  https://radioid.net/static/rptrs.json (downloaded 2026-11-18 into `research-data/rptrs.json`, git-ignored; served
  openly with no login, the same registry the ID database uses; RadioID.net shows no licence or terms page for the
  dumps, so as with user.csv: fine for the owner's own radio, check with RadioID.net before publishing a build that
  contains it; attribution "Repeater data: RadioID.net"): country Turkiye, status ACTIVE: 109 DMR rows, 100 kept, 9
  duplicates (same callsign and frequency), all placed. It has **no coordinates**: the city is looked up in the
  committed gazetteer `integration/radiofoundry/rpt/places.csv` (81 province centres, 48 towns / districts and nine
  old names and misspellings seen in the data; approximate town centres, a few km at worst, not repeater sites):
  a whole word of the city first (a town before its province: "Mersin Silifke" is Silifke), then a word starting
  with a name of 4+ letters ("Ordualtinordu" is Ordu); no match: kept without a position (listed last, no distance).
  (2) The owner's own list `research-data/repeaters.csv` (or `RPT_CSV`), for FM and other repeaters no open source
  lists: columns (header names, any order) `callsign` (required, up to 10), `city`, `lat` / `lon` (decimal degrees)
  or `locator` (4 / 6 character Maidenhead, its centre), `rx_mhz` (required: the repeater's output), `tx_mhz` or
  `offset_mhz` (neither: simplex), `ctcss` (Hz), `mode` (required: FM, NFM, DMR, C4FM / YSF / Fusion, D-STAR, P25,
  NXDN, M17), `cc` (DMR, default 1), `ts` (1, 2, 1+2), `bw` (25 or 12.5); `#` lines are comments; its rows win a
  duplicate. Both present: merged ("own+RadioID.net"). Neither: the committed fictional sample
  (`integration/radiofoundry/rpt/sample.csv`, eight TA0RPx entries, not real stations) and "Repeaters: SAMPLE ..."
  in the build output and "Sample list: not real repeaters" on the screen. **Owner's list since 2026-11-19 (owner:
  "analog röleleri de ekleyelim")**: TRAC's current repeater page trac.org.tr/tr/role-bilgileri and its per-repeater
  JSON (trac.org.tr/ajax/relay/N: coordinates, tone, CC) converted by `research-data/trac/to_csv.py` (git-ignored,
  with the raw files) into `research-data/repeaters.csv`: 174 entries, 3 under maintenance and 18 APRS digipeaters
  (144.800 simplex) left out, comma decimals and one "439275" fixed, 152 kept (151 FM with their CTCSS, 1 DMR; the
  site name is the "callsign", the branch the city; 57 without valid coordinates placed by the gazetteer). With
  RadioID.net: 252 repeaters (DMR 101, FM 151), 11,120 B. TRAC states no licence: the owner's own radio only, never
  in a published build. **Sources checked, not used** (before that): TRAC's
  role (repeater) lists (trac.org.tr and branch pages, e.g. old qsl.net copies from 2003): web pages, no licence,
  not current; RepeaterBook: its API terms forbid bulk extraction, mirroring and redistribution without written
  permission; HearHam (hearham.com/api/repeaters/v1, has coordinates; in Turkey only ~65 DMR repeaters, the same ones):
  no licence or terms stated, so not used; OpenRepeater: no Turkish data found. **Refresh**: download rptrs.json
  into `research-data/` (or point `RPT_JSON` at it; `RPT_COUNTRY=Turkey,Cyprus` for more countries), edit
  `research-data/repeaters.csv`, build; `tools/rpt_list.py` makes `build/rpt/rpt_data.c` at every prepare and prints
  "Repeaters: N (DMR n, ...), n without a position, n skipped (reasons), bytes, date". By hand: `python3
  tools/rpt_list.py --radioid research-data/rptrs.json --csv my.csv --stats`. Rows refused and counted: callsign
  (empty, over 10, not A-Z 0-9 - /), frequency (not a number, not on 1.25 kHz, outside 1-1300 MHz), offset, tone
  (outside 67.0-254.1 Hz), mode, DMR (CC over 15, slot not 1 / 2 / 1+2), position (out of range, bad locator),
  bandwidth; RadioID rows not ACTIVE. **Format** (`rpt.h` `rpt_entry_t`, 44 B): latitude / longitude in 1e-5
  degree (int32, `RPT_NOPOS`), RX in Hz, TX - RX in 1.25 kHz (int16, ±40 MHz), CTCSS in 0.1 Hz, mode | narrow, DMR
  CC | slots << 4, callsign (10) and city (12, folded to ASCII like the ID database: ç ğ ı İ ö ş ü to c g i I o s
  u, capitals to title case), NUL terminated, the province's plate (one byte in the former tail padding, item 37
  below); sorted by RX, then callsign. Size: 100 repeaters **4,432 B**; the
  generator refuses more than 20 KB (`--max-bytes`; ~460 entries). Build: FLASH 721,812 -> 730,384 B (+8,572:
  table 4,432, math 1,088, screen and strings ~3.1 KB), RAM 207,416 -> 207,424 B (+8); every task's deepest stack
  path unchanged (UI 368 B free, radio 828). Host test (tests/plus/test_rpt.c via tools/test_classic.py): the
  generator from a synthetic CSV and RadioID JSON (positions from lat / lon, a locator, a town, a province, a word's
  start and none; Turkish folding; lower-case callsigns; offsets both ways and as TX; a tone with a comma; 12.5 kHz;
  DMR CC / slots; RadioID's country spellings, status and duplicates; eleven bad rows counted by reason; the byte
  limit), every field of the table back exactly; the order nearest first from Ankara against a double precision
  great circle, from Van and Bursa, the unplaced last, the walk forward and back and every rank; without a position
  the table's order; Istanbul -> Ankara 351 km and its bearing; locators (Ankara KM69kw, a CSV locator back as
  itself); the tune values (offset sign, tone, narrow, simplex, CC / TS, both or unknown slots -> TS1, C4FM
  refused); then the shipped table's invariants. Radio rows RP1-RP6.

- [built] Repeaters by province (`CONFIG_RPT_PROV`, `prepare_rpt_prov`, needs REPEATERS and MAP; owner's round 3
  item 37 "Röleler ekranı il il"; parallel job, 2026-11-19): Tools > Repeaters now opens on a first level, a list in
  the small font like Tools: **"Hepsi (252)"** ("All repeaters" in English: the whole list exactly as before,
  nearest first or by frequency), then **only the provinces that have repeaters, in plate order, with their count**
  ("01 Adana (13)", "06 Ankara (8)", "34 İstanbul (15)"), then **"Bilinmeyen (n)"** ("Unknown") only when some
  entries have no province. Up / Down / the knob pick (round), OK opens that province's list: the same rows, nearest
  first from the position APRS uses or by frequency without one, Up / Down round within the province, OK the details
  and OK again tunes the VFO, # that province's nearest; the title reads "Repeaters Adana". Back from the details
  returns to the province's list, Back there to the first level (the cursor still on that province), Back there
  leaves the screen. # on the first level opens the nearest repeater's details in Hepsi (Back: Hepsi's list, Back:
  the first level). Unchanged: Tools > Map's pick opens that entry's details straight away, Up / Down there walk the
  whole list, Back returns to the map; the auto repeater's offer tunes through the same `ui_rpt_tune`. Names: the
  map's own table (`turkey_map_data.h` by plate, ISO-8859-9 like the map's labels: "İstanbul", "Şanlıurfa" with the
  Turkish glyphs), the official names unshortened: the longest row, "03 Afyonkarahisar (123)", is 23 characters and
  the list's font has 24 on the 240 px screen (26 / 25 on the smaller classes), checked for all 81 names in the host
  test; a longer label would be shortened in the middle by the list itself. **Province of each repeater** (decided at
  build time by `tools/rpt_list.py`, a plate byte 1..81, 0 unknown, in `rpt_entry_t`'s former tail padding: the entry
  stays **44 B**, `_Static_assert`ed, no flash per entry): (1) the position inside a province outline, the map's
  even-odd test (`map_province_at`) on the same simplified outlines parsed from `turkey_map_data.c`, so the list
  agrees with where the map draws the triangle; outside every outline (a simplified coast, or a wrong position):
  (2) a district centre (`turkey_district_data.c`, 973 GeoNames points that carry their province) within 10 km,
  (3) the city text naming a province (a whole word; Afyon, Antep, İçel, Maraş, Urfa; or a word starting with a name
  of 4+ letters, "Ordualtinordu"), (4) a district centre within 30 km; without a position only (3); else unknown.
  The build line says "...; 60 provinces, unknown 0" and the generated table carries the distribution in a comment;
  `tools/rpt_list.py --provinces` prints it. **Owner's data (252 repeaters)**: 60 provinces, **unknown 0**; placed by
  outline 240, district 3 (Kadıköy, Alanya, Erdemli shores), city text 9 (positions out at sea: e.g. BAHCECIK "Kocaeli"
  at 40.68 N 25.94 E, BULANCAK "Giresun" 50 km off the coast); İzmir 19, İstanbul 15, Adana 13, Aydın 11, Bursa 10,
  Kocaeli 10, Ankara 8, Antalya 8. **12 entries where the position and the city text name different provinces**
  (TRAC's "city" is the club branch that runs the repeater, so several are real: SOKEGUMUSD and DILEKTEPE "Izmir" stand
  in Aydın, HAVARADARE "Canakkale" in Balıkesir; some look like wrong coordinates in TRAC's data: GOLKOYGOLC "Ordu" at
  37.66 N 40.42 E is in Diyarbakır (latitude and longitude swapped?), YILDIZTEPE "Malatya" at 31.36 E is in Konya,
  SANALTEPE "Nevsehir" in Adana, HARPUT "Elazig" in Tunceli; also YARASLIKAR "Konya" in Ankara, HASANDAGI "Elazig" in
  Tunceli, RESULOSMAN "Kilis" in Kahramanmaraş, GIJGIJTEPE "Tokat" in Sivas, KARAGOLTEP "Ordu" in Giresun): the
  position wins (the map draws them there too); fix their lat / lon in `research-data/repeaters.csv` and rebuild to
  move them. **RAM**: 16 B static (which of the 83 groups have entries, a bit each, filled by one pass when the screen
  opens; the first level's pick, top row and filter): the first level's row count and labels walk those bits, a row's
  count is one pass over the table when drawn (rows x n = 10 x 252 byte compares); a province's list filters the
  same walk (`rpt_next_in` / `rpt_rank_in`, one pass a step, as before). Strings: "All repeaters" -> "Hepsi" (its own
  msgid: "All" is already "Tümü" on another screen), "Unknown" -> "Bilinmeyen", "^v pick  OK list  # nearest";
  tr.po 100 %. Build: FLASH 820,956 -> 821,964 B (+1,008), RAM 209,744 -> 209,760 B (+16); UI stack 368 B free,
  radio 832 B, as before. Host tests (tools/test_rptprov.py from tools/test_classic.py): every province's own label
  point is placed in it by the outline test; Ankara, İstanbul (a text naming Ankara loses to the position), Adana,
  İzmir; Kadıköy and Alanya shores by district; out at sea by the text; old names, Turkish capitals, a word's start,
  "Vanadium" not Van, no position, Berlin and Lefkoşa unknown; a generated table's province bytes, counts and
  comment; the sample (one unknown); the research-data lists when present (the distribution and the 12 differing
  entries printed). tests/plus/test_rpt.c: the fixture's provinces (Ankara 6, Istanbul 34 from a locator, Şanlıurfa
  63, Ordu 52, Mersin 33, Bursa 16, Van 65 from the gazetteer, no position 0) and, for the fixture and the shipped
  table, every province's walk forward and back is the whole order filtered, ranks from 0, counts adding up.
  tests/plus/test_ui_rptprov.c (10 flows in the simulator): the first level (All 9, 01 Adana 2, 06 Ankara 5, 34, Unknown
  1; no empty province, no "(0)"), the order by opening each row and round both ways; every label's width; Adana's
  list holds only Adana's, nearest first, round within it, # Adana's nearest (and Ceyhan's from Ceyhan); by frequency
  without a position; tune from a province (the FM request sequence, the VFO's frequencies); the Unknown group; All
  as before (order, round, #); Back details -> list -> first level (cursor kept) -> Tools, and reopened from the top;
  # on the first level; the map's pick -> details -> Down (whole list) -> Back to the map -> details again -> tune.
  Radio rows RP7-RP11.

- [built] NAC in decimal too (`CONFIG_NAC_DEC`, `prepare_nac_decimal`; owner's finding 2026-11-19): the NAC is a
  12-bit value; this firmware always showed and took it in hex, while the owner's factory P25 radio takes it in
  decimal: "293" typed there is $125 on the air, which P25 search and Home showed as "125" (hex, no mark), and only
  NAC $125 here then talked to it. Now the channel editor's NAC row reads "$293 = 659"; while the NAC is edited, # once
  is Any (F7E) as before, # again types it in decimal ("[293] = $125", 0-4095, past 4095 starts over), # there goes
  back to hex digits. P25 search ("$125/293"), P25 trunk and Home's P25 caller line ("NAC $125/293", the tight form
  "$125") mark hex with "$" and add the decimal. No RAM; +~0.5 KB flash. UI flows in tests/plus/test_ui_any.c.
  Radio row NC1.
- [built] NAC in both forms everywhere (`CONFIG_NAC_DEC`; owner 2026-11-21): wherever the radio shows a P25 NAC it
  shows hex marked with "$" and the decimal, `$659/1625`, in the tight forms too (no screen shows one form alone any
  more, except the last-resort row below):
  - Home's P25 caller line: `P25 TG 101 ID 77285 NAC $293/659` | `TG101 ID77285 NAC$293/659` | `TG101 ID77285`
    (the tight form had the hex alone); dual watch and multi-watch panes: `TG 101 ID 77285 NAC $293/659` |
    `TG101 ID77285` (`integration/radiofoundry/p25_lab.c`).
  - Home's VFO row (`tools/plus_p25nac.py`): `VFO P25 NAC $456/1110 TG 99` | `P25 NAC ...` | `NAC$456/1110 TG99`,
    with a TX NAC of its own `NAC$293/659>$659/1625 TG99`; Any reads `NAC Any $F7E/3966` in the wide forms and
    `NACAny` in the tight one. Only when a TX NAC of its own does not fit even so (Large text) a fourth form keeps
    round 15's decimals, `NAC659>1625 TG99`. Its buffer 96 -> 144 B (+48 B static RAM; static RAM 209,656 B, 1,800 B free).
  - Recent (the call history), a P25 row: `TG 202 $293/659 1s` (the "$" marks the NAC; the word "NAC" gave its four
    bytes to the decimal so the row's 44-byte value still offers the NAC at both of its first levels; the room is
    counted from the age as written, not its longest).
  - Tools > Heard list: the row `TG101 $659/1625 x12 3m`, where that does not fit (the DM-32's rows) `TG101 x12 3m`
    as before (the alternates now wrap only the talkgroup and NAC, so the value's 44 bytes hold them; the third,
    `x12 3m` alone, is gone: an empty last alternate would leave the whole value out), the station's page `$659/1625  433.500000`, the VFO tune toast
    `VFO 433.500000 $659/1625 TG 101`.
  - Tools > All digital search, a P25 find: `433.5000 P25 NAC $1C8/456 3m`.
  - Tools > P25 trunk, the system line: `WACN BEE00 SYS 1C8 NAC $1C8/456` (the "$" was missing); where that is too
    wide the space after NAC goes first, then the WACN: `SYS 1C8 NAC$1C8/456` (the DM-32's width; before, the line
    ran off the screen there).
  - Unchanged, already both: P25 search's found list (`433.5000 $125/293 TG101 3m`), the channel editor's and VFO
    settings' RX / TX NAC rows (`659 ($293)`, typed `[293] ($125)`, hex digits `$2[9]3 (659)`, Any
    `Any 3966 ($F7E)`; the entry stays decimal first, # # hex), the toasts naming 3966 / 3967 ($F7E / $F7F).
  No screen besides these shows a NAC (Last calls, scan, multi-watch rows and diagnostics have none). +376 B flash,
  +48 B static RAM. Host tests: `tests/plus/test_p25x_worker.c` (the caller line's forms), `test_ui_p25x.c` (Recent),
  `test_ui_p25nac.c`, `test_ui_vfox.c` (Home's VFO row), `test_ui_toolbox.c` (Heard list, P25 trunk), with the
  layout checks in English and Turkish. Radio row NC2.
- [built] RAM for the hotspot (`prepare_audio_mux`, `PLUS_AUDIO_MUX_NO_HOST`; Plus only, 2026-11-19): upstream's
  hotspot as Classic measured it (+11.2 KB) overflowed the Plus image by 7,120 B. Two exact cuts, no sharing:
  1. The split duplex extension off (`CONFIG_HOTSPOT_SPLIT=n` in the Plus preset; its default follows HOTSPOT): the
     two-radios-as-one-duplex-repeater role (`hotspotSplit.c` 3,552 B, the BS outbound state in `dmr_glue.c` 664 B)
     is not a hotspot feature. The hotspot itself then costs 4,168 B (`.ram_lo` voice ring and modem event queue
     1,680, data bursts 1,728, uplink queue 352, MMDVM framer 272, the rest small).
  2. The shared PCM stream (`core/usbaudio/audio_mux.c`) is built here only for APRS's receive ring; its host audio
     side (the 2 KB receive ring to a host, the 2 KB transmit jitter buffer and the 320 B host microphone block,
     4,480 B with the stats) has one user, USRP over the network (`usrp_net.c`, `CONFIG_NET_USRP`), which needs the
     IP gateway this build leaves out. Nothing else calls `audio_mux_host_*()` / `audio_mux_fm_pull()`, so the
     host side's state never left "off" and its buffers were never read or written; now they are compiled out, the
     API stays (no samples, zero counts), the radio's side (the primary user's blocks, the speaker route, APRS's ring,
     start / stop) is the same code. A build with USRP refuses the macro (`#error`). Host test
     `tests/plus/test_audio_mux.c` (in `make test`) runs the same checks against the Plus file and upstream's: user
     blocks and speaker output, APRS's ring sample-exact, the stream kept and stopped, and only upstream passes host
     audio; the USRP refusal is checked too.
  Static RAM 207,424 -> 207,112 B with the hotspot in (4,344 B free, was 4,032 B); RTOS heap unchanged (34,900 B; the
  hotspot allocates nothing and adds no task: the host link task already runs). Flash 731,376 -> 747,568 B. Classic's
  image is byte for byte the same (checked against a build from the baseline sources).
  Not done, again (looked at for this): sharing the AMBE codec state (`dec_state` 6,144 + `enc_state` 7,168 B) with
  the P25 lab's `voice` (6,916) / `fifo` (5,788) / `rp_buf` (4,400). The users: the AMBE decoder runs in the audio
  task behind `ambeVoiceAudioBlock`'s queue (DMR, NXDN and dPMR through `ambeVoiceSink`, YSF DN), and a new call's
  `voc->reset()` there rewrites both blocks at a time the radio task does not choose; the encoder in the same task
  for DMR TX; `voice` (IMBE context, P25 RX and TX, YSF VW, the call replay's decoder) and `fifo` in the P25 worker
  and the audio task, `voice`'s caller fields read by the radio task (Home's caller line, P25 search, call log) in
  any mode; `rp_buf` kept across channel changes for Tools > Last call. YSF needs both codecs in one session. A safe
  overlay needs an owner switched only with both tasks quiesced (`hrc_vocoder_park` and its `busy` wait, the
  codec's `ready` cleared, `p25_lab_channel_stop` and the worker idle, IMBE re-init, FIFO reset, the caller fields
  moved out of `voice`), YSF left out of the sharing (so YSF needs its own 12.7 KB: no gain while YSF is built) and
  checks of the real AMBE codec after each hand-over, which only the radio can run (the codec is the owner's stock
  bytes, not in the host tests). With 4.3 KB free that risk buys nothing today: it stays the reserve for a feature
  that needs ~10 KB, and would come with YSF's own memory decided first. Radio row RS1 soaks the mode switches
  around the hotspot instead.
- [built] MMDVM hotspot (`CONFIG_HOTSPOT`, `prepare_hotspot`; 2026-11-19): upstream's hotspot (docs/user/14-hotspot.md)
  on the programming cable. The DM-32 has no USB: the host link is UART0 on the cable (CH340), 115200 8N1, and
  hotspot mode starts by itself when MMDVM frames arrive (Settings > Connections > Hotspot allowed, on by default).
  Tools > Hotspot shows On / Off and its page now says the cable ("Start MMDVMHost, Pi-Star or WPSD on a computer on
  the programming cable (115200 baud): hotspot mode starts by itself."; "on USB" on radios with USB). The radio is a
  simplex DMR modem: DMR only, timeslot 2 only, RX = TX frequency (or `Duplex=1` with equal frequencies for MMDVM
  SFR, built in with `CONFIG_SFR`); D-Star / YSF / P25 / NXDN / FM modes refused, POCSAG accepted and never sent.
  **Use**: MMDVMHost (`MMDVM.ini`: `[Modem] Protocol=uart`, `UARTPort=/dev/ttyUSB0` for the CH340 on Linux / a
  Raspberry Pi, `COMn` on Windows, `UARTSpeed=115200`; `[General] Duplex=0`; `[Info] RXFrequency=TXFrequency`;
  `[DMR] Enable=1`, `ColorCode=1`; `[DMR Network] Slot1=0 Slot2=1`; every other mode off), Pi-Star or WPSD with the
  cable's serial port as an MMDVM modem on that port at 115200 and the same settings. Other MMDVM-protocol hosts
  (BlueDV, DroidStar) are untried. The host port is exclusive: our cable tools (`tools/plus_txdiag.py`,
  `tools/p25_rxmode.py`, CPS) need MMDVMHost stopped; on the radio side the debug link still works between MMDVM
  frames. Leaving: OK or hold Back on the hotspot screen, Tools > Hotspot > Stop hotspot, or 20 s without a frame
  (a UART can't tell when the host closes the port). **Other modes**: entering hands the channel back like any
  request; on a P25 or YSF channel the native receiver stays off while hotspot mode runs and takes the channel
  again after it (`native_lab.c`: `channel_service` does not select with `sm->hotspot`, and in hotspot mode every
  request, PTT too, is the radio core's: nothing keys a P25 transmission; on a P25 channel PTT does nothing);
  NXDN / dPMR listen only in FM board mode and stop; P25 / DMR search and trunk following, spectrum, the CW beacon
  and APRS give way as upstream's requests do (`busy`). **Limits**: the transmitter keys for every network call on
  its own (unattended operation: the owner's licence conditions and the band plan's hotspot frequencies apply);
  power follows MMDVMHost's `RFLevel` over the radio's levels (100 % = the highest, ~5 W VHF / ~4 W UHF): use a
  low level (RFLevel 10-25), a proper antenna or a dummy load, and keep long network sessions short (no duty
  limit in the firmware; the handheld's PA heats in long calls).
  Stack: the host link task's MMDVM paths are 1,172 B deep at most (`mmdvm_poll` 1,060 B under the task's frame),
  under the CPS chain's 1,436 B: the 2,240 B stack stays (608 B free). Host tests: upstream's `mmdvmTest`,
  `hotspotSfrTest`, `hotspotDataTest`, `hotspotUplinkTest` on the transformed core and `radioCoreTest`'s hotspot
  session (in `tools/test_classic.py`), and `test_plus_hotspot_p25` (tests/classic/test_radio.inc: P25 channel ->
  hotspot -> PTT -> back, three rounds; fails without the native_lab.c guards). USB network: out of scope (no USB);
  PPP over the cable (upstream's stand-in for it on this platform) needs the IP gateway; measured on top of this
  build: with upstream's default services (TCP, HTTP, rigctld, APRS-IS, KISS, mDNS, TFTP, SNMP; USRP off, as it
  needs the host audio side cut above) +18,616 B static RAM (overflow by 14,272 B); the gateway and PPP alone, every
  service off, +5,040 B (overflow by 696 B). Neither fits without the codec-state sharing above. Radio rows HS1, RS1.
- [built] Multi-watch, 2 to 6 channels (`CONFIG_MULTI_WATCH`, `prepare_multi_watch`, integration/radiofoundry/mwatch;
  owner: "A ve B iki kanalı aynı anda dinleyebiliyor, bunu 3-4, hatta 5-6 yapabilir miyiz", chose "ayarlanabilir
  2-6"; 2026-11-19): **Settings > Talk & listen > Watch channels** 2..6 (2, the default, is upstream's dual watch
  exactly: its panes, keys, talk back and rules). With 3..6, dual watch (`* 7`, hold SK2, the A/B key) watches a
  ring of channels **A..F**: A the channel you are on, B..F the next channels of A's zone (then later zones), as B's
  default. **Radio core** (`radio_mwatch.inc`): dual watch's pair grown to `dual[0..n-1]`; the one receiver steps
  A > B > C ... with the scan's dwell per channel; a signal holds its channel, the wait after it (`Dual watch delay`)
  stays for the reply and looks at the other channels in turn (a signal found marks that row Busy), then the ring
  goes on from the next one; PTT transmits on the selected channel (tuned first) and the ring goes on after (a P25 letter, or the native receiver
  holding another letter, too since `mwfix`: section "Dual watch and multi-watch: the TX target");
  hotspot mode, a stop, a channel / zone / VFO change act as on dual watch. A P25 (or YSF) channel is listened on
  the FM path like the others while quiet; a signal there is handed to the native P25 receiver as in dual watch
  and zone scan (`native_lab.c` scan hold: the radio core waits while it has the channel) and holds the ring up to
  2.5 s without voice for us, then the wait. NXDN / dPMR channels as in dual watch. **UI** (`ui_mwatch.inc`): dual
  watch's two panes stay: pane A, and pane B holds the selected one of B..F, so every dual watch rule for the
  selected channel (Up / Down, the knob, quick settings, talk target, VFO key, Find, zone keys) acts on the selected
  letter; selecting a letter outside the panes swaps it in. **Home** (240x320): a row per channel, the letter in a
  box (filled with ▶ on the selected one, which has the PTT pill; red TX, green RX while a call comes in, outline
  green while Busy), number and name, mode and frequency or the caller (green) on the receiving row, "Heard 7s ago"
  at the right (amber on the newest call); under the rows "Receiving on C" with the meter, "TALKING on C", else
  "Ring 0.9 s" (and ", P25 +3.5 s" with a P25 channel in the ring). Large text: one line per channel. A toast "P25
  slows the ring" when the ring has a P25 channel. **Keys** (dual-watch.md's, extended): the A/B key (SK2 press)
  and # 1 select the next letter (A > B > ... > A); Left / Right the next / previous on maps that have them; #
  opens 1 Talk on <next>, 2 Change A, 3 Change B, 4 Caller, 5 Stop, then **6 Change C, 7 Change D, 8 Change E, 9
  Change F** (numbers 1-5 as dual watch's); a long OK on a Change row talks on that letter. The picker dims the
  channels other letters have, with their letter; every letter can be a VFO (A / B upstream's two; C..F one each of
  their own since "Every mode in the VFO" below; before that only A and the selected letter). Stop
  (Back twice, `* 7`, hold SK2, # 5): "Watch off: on C", the radio stays on the selected one. Talk back works with
  2 channels only (it is off in the ring). **Kept**: the count in `settings_t.dual_state` bits 2-4 (count - 2) and
  the selected letter's place in bits 5-7, next to dual watch's own bits (an older record and a factory reset read
  2); A and the selected (or B) channel as dual watch keeps them; C..F are found again as the next free channels of
  the zone after power off (no room in the record for four more channel numbers). **Ring period** (host model:
  the radio state machine with the native P25 receiver, tests/plus/test_mwatch_radio.inc; quiet channels, the
  default dwell 150 ms, one stop = the dwell rounded up to the 25 ms poll; Home's "Ring" is this estimate,
  `radio_mwatch.h`):

  | Channels | FM only | DMR only | FM + 1 P25 | A P25 signal without voice for us |
  |---|---|---|---|---|
  | 2 (dual watch) | 0.30 s | 0.30 s | 0.30 s | + 3.5-3.6 s |
  | 3 | 0.45 s | 0.45 s | 0.45 s | + 3.5-3.6 s |
  | 4 | 0.60 s | 0.60 s | 0.60 s | + 3.5-3.6 s |
  | 5 | 0.75 s | 0.75 s | 0.75 s | + 3.5-3.6 s |
  | 6 | 0.90 s | 0.90 s | 0.90 s | + 3.5-3.6 s |

  On the radio each FM <-> DMR switch and each HR_C7000 retune adds its own time (not modelled; MW2 measures it).
  **Limits**: one receiver: a call on one channel is missed while the receiver is on another (each channel is
  heard once a round; a call is caught within one round of starting, its first syllables can be lost); a call
  holds the ring, so other channels go unheard during it (the looks in the wait only mark them Busy); P25 slows the
  ring as above. **RAM**: +912 B static (208,024 of 211,456 B, 3,432 B free): per channel 72 B, no channel_t or
  view per channel (radio core 34 B for each of C..F: the reference 6 B and its changes for now 28 B, `dual_keep`;
  UI 38 B for each of A..F: the reference 6, the last call's time 4, Home's number, mode, frequency and name 28),
  so 6 channels cost 136 + 228 B; shared: the ring request and its copies 3 x 36 B, the codeplug
  read buffers (zone 180 B, channel 68 B: Home's rows are read once when the ring changes, not per frame) and the
  row text 2 x 48 B; FLASH 747,716 -> 754,952 B (+7,236). Stacks unchanged (UI 368 B free, radio 824: the ring's
  drawing is static, noinline, the banner and hint drawn from dual watch's own frame). Host tests:
  tests/plus/test_mwatch_radio.inc (upstream's radio suite and the Classic / Plus P25 suites again with it built in,
  then order, dwell, hold, wait and looks, Busy, PTT on the selected, refusals, the stop, a P25 channel, the period
  table), test_mwatch_set.c (the setting across power cycles, beside dual watch's bits), test_ui_mwatch.c
  (upstream's UI flows with it built in fail exactly where they fail without; 18 ring flows: 4 rows, select C, the
  A/B key round, # 1, # 6..9, long OK, D's channel from # 7, Up skips the others, a call on C, PTT on C, stop,
  power off, 6 channels, back to 2, the setting). Radio rows MW1-MW8.
- [built] Tone finder (`CONFIG_TONE_FIND`, `prepare_fm_decoders`, integration/radiofoundry/fmdec; owner's round-3
  item 1; parallel job fmdec): **Tools > Tone finder** names the sub-audible tone of the FM signal received on the
  channel or VFO in use: "CTCSS 88.5 Hz" or "DCS 023N" large, a bar for how firmly, the frequency and RSSI, "Searching..."
  / "No tone", the same DCS signal's other standard names ("= DCS 047I": every inverted standard code is a normal
  standard one turned, the well-known equivalent pairs), and once the signal is gone "Last: ..." (kept). **OK** opens
  Write tone for the tone heard (or the last one): RX and TX / RX only / TX only. On a channel the stored channel is
  read and written back with only its tones changed through the channel editor's own call (`ui_svc_write_channel`,
  the codeplug write path unchanged; "Saved channel 4"; temporary changes dropped as after an editor save); in the VFO
  it is the VFO's tone (`radio_req_tone`, kept as any VFO setting). DMR / digital / AM channels: "FM only".
  **Hardware first**: the FD6818 has a detector for the one tone or code it is set up for (0x51 / 0x07 / 0x08, the
  verdict in 0x0C: the radio's squelch); upstream exposes no tone scan, and stepping it through 50 tones and 208 codes
  would take a minute with the squelch closed, so the tone is found in **software** on FM caller ID's receive audio
  (the APRS receive ring, `radio_msg.c` RADIO_MSG_TONEF, as the pager and SSTV: its ring, or APRS's on the APRS
  frequency; FM caller ID turned off still gives it to the screen). The FD6818's normal FM path has a **300 Hz high
  pass** (0x2B = 0xC000) whose job is to remove the sub-audible tone, so the screen turns the **flat receive path**
  on while it is open (`board_nxdn_flat`: 0x2B = 0x0700 and the HR_C7000's FM band-pass off, back on after; the
  speaker plays flat, brighter audio meanwhile, as for the pager); the host test shows a 4-pole 300 Hz high pass
  leaves nothing to find. Experimental: the screen also shows what the FD6818's 0x68-0x6A read ("FD6818: CTCSS 88.5
  Hz" and the raw words), the registers the DM-1801 Pro's stock firmware reads and where the BK4819 (the FD6818's
  likely relative) puts its own CTCSS frequency measurement and received DCS word; read only, four times a second
  while the screen is open (`dm32_css.inc`, `TONEF_CHIP_READ`); if TF5 shows they work, a later build can use them.
  **DSP** (integer, `tonefind.c`): an 80-tap Blackman low pass (-6 dB at 350 Hz, -69 dB at 750 Hz) computed every
  8th sample to 1 kHz; CTCSS: Goertzel on the 50 standard tones over 0.5 s blocks, a tone named when it holds 15 %
  of the block's power (voice or noise alone: 5 % at most) and beats every other by 6 dB (neighbours 2.3-3 Hz apart
  are 13.5 dB down at 0.5 s), locked after two blocks in a row (1.0-1.5 s), released after two without; DCS: a bit
  clock on the 134.4 bit/s edges, each bit read by an edge slicer (coupling capacitors bend runs of equal bits) and
  by the sign of the bit's sum, each into its slot of the 23-bit period (a leaky sum: one or two wrong readings don't
  flip it), all 23 rotations of the slots' word tried normal and inverted against the Golay(23,12) words, the name
  kept: standard before non-standard, N before I, then the lowest code; locked after two periods (about 0.5 s); a
  DCS word in the last 2 s keeps CTCSS out (DCS spectral lines and its turn-off tail near 67 Hz). **The receive
  audio's sign** is not known on the DM-32: `TONEF_DCS_INVERT` (0) flips the DCS polarity if TF3 reads 023I for a
  D023N sender. Host tests: tests/plus/test_tonefind.c (every CTCSS tone with voice and noise at 0.35-0.75 kHz,
  +-0.3 %, levels x0.05 to x4 with clipping: 50/50 found in 1.0 s, worst 3.5 s with 0.8 kHz rms noise or 0.35 kHz;
  +-0.6 % never read as the neighbour; every DCS code N and I through 20 Hz and 5 Hz coupling, +-0.5 % bit rate, an
  inverted receive sign: 104/104 each, mean 0.45-0.6 s, worst 1.9 s; heavy noise found right or not at all; voice and
  noise alone 30 s: nothing; the DCS turn-off tail not read as CTCSS), test_fmdec_glue.c (through radio_msg.c and
  radio_fmid.c with a fake ring: flat path on and off, decoder freed, the last tone kept), test_ui_fmdec.c (the
  screens in the UI simulator: found / last / searching / DCS equivalents, the write into channel 4 read back, RX
  only, the VFO, DMR refused, listening only while on top). CPU about 0.2 M multiplies a second, only while the screen
  is open; RAM: about 0.7 KB of heap while it listens. Radio rows TF1-TF7.
- [built] DTMF / SelCall decoder (`CONFIG_SELCALL`, `prepare_fm_decoders`; owner's round-3 item 5; parallel job
  fmdec): **Tools > DTMF / SelCall** lists the last six sequences heard on the FM channel in use, newest first: "DTMF
  1234#  446.00625", "ZVEI 12345  438.500" (the frequency, cut first when the row is short) and how long ago; OK
  shows a row whole; **#** picks the tone set looked for: Auto (all), ZVEI1, ZVEI2, CCIR, EIA (DTMF always). The list
  is RAM only and stays after the screen closes (until power off); the same sequence again within 3 s refreshes its
  row. Runs only while the screen is open (RADIO_MSG_SELCALL, FM caller ID's audio, **not** the flat path: the tones
  are in the voice band, the speaker sounds as usual). No Home toast while the screen is closed (FM caller ID already
  shows DTMF there; SelCall in the background would keep the decoder and the ring running all the time). **DTMF**:
  FM caller ID's own decoder (fmid.c: Goertzel on 17 ms blocks, twist 8 dB, a digit after two blocks) as a second
  instance, fed sample by sample so that a digit's block is still at hand: the pair must hold 60 % of it (talk-off: a
  vowel with harmonics on a DTMF pair holds less); a string ends 1.5 s after its last digit. **SelCall** (integer,
  `selcall.c`): zero crossings with hysteresis, interpolated to 1/256 sample, in 5 ms frames; a frame is one tone
  when its periods spread under 1/8; frames within 3 % make a tone (one frame off and back is still it), its
  frequency the mean over all its periods. A sequence (4-12 tones, ended by 150 ms without one) is read by each set:
  every tone within 2 % of the set's, the inner tones 0.8-1.3 of its length (EIA 0.6-1.6), the smallest error wins
  and letters cost extra. The sets share tones within a few Hz (ZVEI 3 1270 / CCIR 3 1275 Hz, CCIR 5 = EIA 6 =
  1446 Hz): their lengths (70 / 100 / 33 ms) tell them apart. ZVEI1 and ZVEI2 differ only in A-F (970 Hz is ZVEI1's
  C and ZVEI2's repeat, 2600 Hz the other way round), so a call of digits is "ZVEI"; a repeated digit or a set's own
  letter names it. The repeat tone ('E') is read as the digit before. A tone lost to noise (a gap over 20 ms, or a
  tone too short to count right before or after) refuses the call rather than show it a digit short; a whistle 5 %
  or more off every set's tones right before or after a call is left out. Tone tables as Wikipedia's "Selective
  calling" and multimon-ng list them. Host tests: tests/plus/test_selcall.c (each set 96 calls: fixed, random with
  20 dB noise and +-8 % lengths, 12-21 dB SNR with +-0.8 % frequency and levels, a 600 ms first tone, six tones with
  letters, the set picked: 96/96 decoded in every set, never wrong, 0.16-0.18 s after the last tone; checked over 40
  seeds: no wrong or short decode; a picked set never hears another's calls (0/60); a whistle and a tone around the
  call 28/30, a tone lost in the middle 30/30 refused; DTMF 56/56 strings with +4 / -6 dB twist, 45-120 ms tones,
  40-300 ms gaps, +-1.5 %, noise, de-emphasis; 70 s of voice-like audio with whistles and of noise: nothing; over 80
  seeds one talk-off digit came out of FM caller ID's decoder and was refused here), test_fmdec_glue.c (with FM caller
  ID off the ring is still taken, no flat path, a ZVEI call and a DTMF string listed with the frequency, a repeat
  merged, CCIR picked hears no ZVEI, the list kept after Back), test_ui_fmdec.c. CPU about 1 % while the screen is
  open; RAM: about 0.75 KB of heap while it listens. Radio rows SC1-SC6.
  Both items: static RAM 208,072 -> 208,496 B (+424: the SelCall list 168 B, the tone finder's result, the screens'
  buffers and the channel copy for the write), FLASH 777,924 -> 787,448 B (+9,524); UI and radio task stacks
  unchanged (368 / 824 B free).
- [built] Every mode in the VFO (`CONFIG_VFO_MODES`, `tools/plus_vfox.py`, `integration/radiofoundry/vfox`; owner's
  items 21, 21b and 22 of round 3, 2026-11-19; host-tested, not yet on the radio). The VFO, with no channel saved,
  is FM, DMR, P25, YSF, NXDN, dPMR or **AM on any frequency**, each with the parameters a channel of that mode has:
  P25 NAC (hex, `#` decimal, `#` Any F7E), talkgroup (Any = 0) and unit ID; YSF DG-ID (0 any, 1-127); NXDN RAN (0 any,
  1-63); dPMR CC (Any or 0-63); DMR color code 0-15 or Any, timeslot 1 / 2 / Any, RX group list None or "Any (all
  groups)" (the VFO has no group lists; its talk target stays upstream's). **UI**: Quick settings' Mode tile (4) in
  the VFO cycles FM > DMR > P25 > YSF > NXDN > dPMR > AM > FM, only the modes the build (and the board's AM receiver)
  has, toast "VFO: P25" (FM > DMR is upstream's toggle as before; in the air band and the broadcast band upstream's
  rule and toast stay). `#` in Quick settings (or `#` on the VFO row of Zones while in the VFO) opens **VFO settings**:
  the channel editor itself (`ui_menus.c` rows, editors, checks: no second editor) on a copy of the VFO in use, showing
  only Mode, RX, TX and that mode's rows, plus "Save as new channel" (the channel editor's New from current on the VFO
  as it is: P25 / YSF / NXDN / dPMR / DMR Any carried into a channel record in the Plus formats; AM off the air band
  becomes FM with a toast). Each change goes to the radio at once (`ui_svc_vfo_apply` > `radio_req_vfo_apply`, a
  `channel_t` handed over under the view lock like the multi-watch ring; RREQ_VFO_APPLY), so nothing waits for a save
  (hold OK says so). Example: 145.575 typed on Home, `* 4 4` (P25), `#`, Talkgroup 99, NAC 4 5 6 (or `#` 1 1 1 0).
  Home's VFO row shows the parameters: "VFO P25 NAC $456 TG 99" > "P25 NAC $456 TG 99" > "NAC456 TG99", "NAC Any",
  "VFO YSF DG-ID 12", "VFO NXDN RAN 5", "VFO dPMR CC 7", "VFO AM" (the chip "AM" off the air band). **Radio core**
  (`radio_vfox.inc`): RREQ_MODE in the VFO also takes P25 / YSF / NXDN / dPMR / AM (`vfox_set_mode`: P25 starts with
  NAC Any, talkgroup Any and the radio's DMR ID as its unit ID; the others Any), and leaving them for FM / DMR clears
  their fields, so the other jobs' VFO requests (Repeaters, Satellites, DMR search, the tone finder:
  `radio_req_vfo_set` / `_tx` / `_mode` / `_bandwidth` / `_tone`) work unchanged on a VFO in any mode; Copy to VFO
  takes a P25 / YSF channel too, with its parameters, and a DMR channel's Any. The native receiver takes a P25 or YSF
  VFO like a channel of that mode (`native_lab.c`: the `!view.vfo` exclusion removed; the cooldown key of a VFO is
  its own, not the zone position it left); NXDN / dPMR listen on the FM path as channels do; AM is FM through the
  board's AM demodulator (`channel_t.am`, in the struct's tail padding: no RAM; `radio_am_channel`), AM squelch and
  TXDENY_AM as in the air band. The board's `rx_am` has no frequency limit besides the bands; whether the FD6818's AM
  demodulator works on UHF / 2 m is for the radio (VX9). **TX gates** (`vfox_fix`, then `radio_sm_tx_check`
  unchanged): P25 only with a NAC (not Any; since `CONFIG_P25_TXNAC` the TX NAC, else the RX NAC), a talkgroup and a
  unit ID (`codeplug_p25_valid`), and only in 144-148 /
  420-450 MHz; YSF, NXDN, dPMR and AM receive only; DMR CC / TS Any refused (TXDENY_MODE) as on a channel.
  **Multi-watch / dual watch**: a VFO slot in a native mode is tuned, held and handed to the native receiver exactly
  like a channel slot (the P25 hold, the give back). Every letter A..F may be a VFO: A / B use upstream's two VFOs
  (saved as before), C..F one more each (VFOs 2..5, RAM only: `vfox_slot_t`, 32 B, in `radio_sm_t`; while one is in
  use VFO A / B waits in a park slot, so upstream's `vfo_idx`, its swap and its persistence only ever see 0 / 1). The
  VFO key on the selected letter, and the VFO row of # 6..9's picker for a letter outside the panes, make a letter its
  own VFO; the picker's channel rows make it a channel again. PTT and every VFO request act on the selected letter
  (tuned first, as before). After power off C..F's VFOs are gone: the ring comes back with B in pane 1 if a C..F VFO
  was there (`pane_code`, `mw_save`), and the radio powers up on VFO A / B. **Item 22**: Settings > Keys already lets
  SK1 / SK2 press and hold take "Dual watch" (`KEYFN_DUAL_WATCH`, upstream), which with Watch channels 3..6 turns the
  ring on and off; in multi-watch builds it is named "Dual/multi-watch". No new setting: SK2 hold stays its default.
  **Settings record** (no new field; `settings_t` stays 392 B, upstream's layout; `vfox_store` / `vfox_load` after
  upstream's own persist / restore, VFO A = idx 0, B = idx 1):

  | Byte (VFO A / B) | Field | Upstream | With a VFO in P25 / YSF / NXDN / dPMR | AM | DMR |
  |---|---|---|---|---|---|
  | 20 bits 0-3 / 256 | `vfo_mode` low nibble / `vfo2_mode` | mode 0 FM, 1 DMR | 2 P25, 3 YSF, 4 NXDN, 5 dPMR | 6 | 1 |
  | 28-29 / 252-253 | `vfo_rx_tone` / `vfo2_rx_tone` | RX tone value | talkgroup / DG-ID / RAN / dPMR CC (16 bits) | tone | tone |
  | 30-31 / 254-255 | `vfo_tx_tone` / `vfo2_tx_tone` | TX tone value | unit ID bits 0-15 | tone | tone |
  | 25 / 261 | `vfo_rx_tone_type` / `vfo2_rx_tone_type` | 0-3 | 0x80 \| unit ID bits 16-22 | type | type |
  | 26 / 262 | `vfo_tx_tone_type` / `vfo2_tx_tone_type` | 0-3 | 0x80 \| NAC bits 0-6 | type | type |
  | 23 bits 0-3 / 259 bits 0-3 | `vfo_cc` / `vfo2_cc` color code | 0-15 | NAC bits 7-10 | CC | CC |
  | 24 bits 0-1 / 260 bits 0-1 | `vfo_ts` / `vfo2_ts` slot | 1 / 2 | bit 0 unit ID bit 23, bit 1 NAC bit 11 | slot | slot |
  | 40-43 bits 11-13 / 251 bits 0-2 | `unused_dmr_id` / `vfo2_talk` bits 24-26 | ignored / 0 | 0 | flags | bit 0 CC Any, 1 TS Any, 2 every group (VFO A: bits 11-13 of the Plus word, tag 0x50 in byte 43, shared with the priority channel in bits 0-10 and 16-22) |

  Why these: a native mode has no use for the FM tones or the DMR color code / slot, so its 52 bits (NAC 12,
  talkgroup 16, unit ID 24) go there; the FM tones of a VFO saved in a native mode are therefore not kept over a power
  cycle (they are in RAM while it runs), its DMR talk target is. Upstream firmware (Classic, or an older Plus) reading
  such a record: an unknown mode reads as FM, both tone type bytes are invalid (bit 7) so no tone, color code and slot
  stay in range. `unused_dmr_id` is read by nothing upstream; an older record may hold an own DMR ID there (under
  2^24), so ours carry the Plus tag 0x50 in the top byte (the same word as the priority channel) and anything else is left alone. `vfo2_talk` bits 24-30 are ignored by
  upstream (it takes bits 0-23 and 31) and were always written 0. Appending fields was not chosen: settings_t has
  about six copies in static RAM and several on task stacks (the CPS chain and the UI's storage job among them), and
  parallel jobs appending would shift each other's offsets. An older record loads unchanged and saves back byte for
  byte (test). C..F's VFOs: no safe settings room for four more VFOs (32 B each), so RAM only, as the owner allowed.
  Cost: FLASH 777,924 -> 782,092 B (+4,168), static RAM 208,072 -> 208,512 B (+440: C..F's slots and the park 5 x 32,
  their published copy for the UI 4 x 32, the VFO settings' hand-over 68, Home's text 72, toast 12), every task's
  deepest stack path unchanged (UI 368 B free, radio 824). Host tests: `tests/plus/test_vfox_set.c` (layout offsets,
  each mode on VFO A and B across power cycles with edge values, an older record unchanged and re-saved byte for byte,
  what upstream reads back, the compact slots), `tests/plus/test_vfox_radio.inc` (the real radio state machine and
  native admission after upstream's radio suite, the Classic / Plus P25 suites and the multi-watch ring run again with
  it built in: P25 VFO 145.575 NAC $456 TG 99 taken by the native receiver, the TX gates, YSF, NXDN, dPMR, AM on UHF, DMR
  Any, each mode kept across a restart and an older record, Copy to VFO of a P25 channel, a P25 VFO in dual watch, C..F
  as P25 / YSF / NXDN / dPMR VFOs in a six-letter ring with the P25 hold on C, A / B's saved VFOs untouched),
  `tests/plus/test_ui_vfox.c` (42 flows in the upstream simulator: the Mode tile's cycle with and without an AM
  receiver, VFO settings rows, TG 99, NAC 456 hex and decimal, Any, unit ID, Home's line, YSF / NXDN / dPMR / DMR Any,
  Save as new channel, the Zones row, C..F's VFOs by the VFO key and the picker, D / E / F in YSF / NXDN / dPMR, the
  Dual/multi-watch key). Not done: a VFO band scan in a native mode stops on carriers muted (the native receiver takes
  only a zone scan's or a watch's hold); P25 trunk following still needs a saved channel. Radio rows VX1-VX12.
- [built] P25 emergency and unit-to-unit calls (`CONFIG_P25_EXTRAS`, `prepare_p25_extras`; round 3 item 4; parallel
  job): the link control's emergency bit and unit-to-unit voice are decoded and shown. **Before**: `decode_lc`
  (src/p25/p25.c) took only LCF 0x00 with MFID 0/1; an LDU1 carrying LCF 0x03 ("Unit to Unit Voice Channel User")
  came back with `metadata_valid` 0, `voice.c` dropped the call's identity and permission, so **every voice frame of a
  private call was muted**, Home kept the last group caller (or nothing), and the call history got no entry. The
  emergency bit (service options bit 7) was decoded into `service_options` but never looked at. Formats (TIA-102.AABF,
  as MMDVMHost's `P25Data.cpp` codes them): LCF 0x00 octet 2 service options, 4-5 talkgroup, 6-8 source; LCF 0x03
  octet 2 service options, 3-5 the called unit, 6-8 the source. **Now**:
  - Emergency (either format): Home's caller line leads with "EMERGENCY" (Turkish "ACİL DURUM") instead of "P25"
    ("EMERGENCY TG 101 ID 77285 NAC $659/1625" | "EMERGENCY TG101 ID77285 ..."), and while the call comes in the
    activity line's second row says EMERGENCY in the warning colour (instead of "Receiving"; the word too, P5). Dual
    watch panes and multi-watch rows use the same line (their 48-byte buffer keeps the forms that fit:
    "EMERGENCY TG101 ID77285"). The call history marks the entry (warning colour, "EMERGENCY"; a call's emergency
    sticks once any LDU1 of it had the bit). No full-screen alarm (DMR's emergency screen is DMR's).
  - Unit to unit: "P25 Private 77285 > 1234 NAC $659/1625" | "Private 77285>1234 ..." for the talkgroup part; the call
    history logs it to its unit ("Private > 1234", narrow rows "> 1234"); a call to another unit is a new call.
  - **Which unit-to-unit calls play** (decision): on a channel with no talkgroup filter (talkgroup Any, P25 search, a
    trunk receive) every one, as Any hears every talkgroup; on a talkgroup channel only one for the channel's own
    P25 Radio ID (`p25_id`, the worker's `own_unit`); a channel without an ID plays none. The others show (Home,
    history) and stay muted, as calls on another talkgroup always did. A real radio plays private calls to itself
    whatever group it is on, and a scanner hears everything: this is both.
  - **Found on the way, fixed**: an LDU1 whose link control is another format that passed its FEC (a vendor's talker
    alias blocks, LCF 0x15 / 0x17 with MFID 0x90, a source ID extension, ...) also dropped the call's permission,
    muting that whole superframe (0.36 s every time it came). Now such an LC (`lc_other`, P bit clear) says nothing
    of the call and the LCs around it hold; damage (FEC failure) and a protected LC (P bit) mute as before. A new
    call still starts with an HDU, a TDU, another NAC or 0.5 s without records, which reset the permission.
  - **Talker alias: not built.** The pinned OP25 (`research/protocol/op25`, 3f44d43) decodes Motorola talker alias
    only from Phase 2 TDMA MAC messages (opcodes 0x91 header / 0x95 blocks, MFID 0x90, `tk_p25.py` 1289-1307); its
    FDMA link control path (`decode_fdma_lcw`) has no LCF 0x15 / 0x17 case ("unhandled"), so where the sequence
    number, block number and alias length sit in a Phase 1 LDU1 word is not shown anywhere in the tree (the TDMA
    header is "converted to FDMA format" as 17 octets, not a 9-octet LC). Its `mot_talker_alias` class stops at
    bytes: `decode_mot_alias` prints the encoded and decoded octets and never makes text (the character encoding is
    not established), the CRC check's span is ad hoc, and there are no test vectors or captures. No Harris alias
    anywhere in the tree; MMDVMHost and DM32-nxdn-p25 have none. Needed before building it: a reference for the FDMA
    LC layout of the header and blocks, the character set, and recorded calls with known aliases to test against.
    What the radio does with alias LCs now: plays through them (above), shows nothing for them.
  RAM +24 B (the receive's called unit and own unit ID 8 B, the caller's unit and kind 5 B, the call history's unit
  4 B, alignment; the entry's emergency flag sits in a spare byte of `ui_recent_t`), FLASH +1,376 B with the CH_MODE 2
  reading below (777,924 -> 779,300). Every task's deepest stack path unchanged (UI 368 B free, radio 824, P25
  worker 912). Without the switch the decode is the old one (checked). Host tests (`tools/test_classic.py`
  `p25_extras_tests`): `tests/plus/test_p25x.c` (built with the switch and without): LDU1s from the real encoder with
  their LC rewritten by `tests/plus/p25_lc_build.h`, written from MMDVMHost's formulation (Hamming (10,6,3)
  equations, the bit spans and status symbols; it reproduces the encoder's frames bit for bit for 300 group LCs),
  the emergency bit, unit to unit (target, source, service options), a bit error in all 24 Hamming words and 6 RS
  symbol errors, vendor / protected / damaged LCs told apart; `test_p25x_worker.c`: calls through the real channel
  worker (frames, PHY records, FEC, IMBE): what plays on a talkgroup channel, without an ID, on an Any channel, the
  Home and pane lines exactly (the longest IDs whole), the call history fields, a talker alias LC in a call no
  longer muting, a protected LC still muting; `test_ui_p25x.c`: the history rows in the real UI on the DM-32's
  narrow screen and a full 240x320 one. Standalone, not in `make test` (needs the reference tree, as
  `test_ysf_ref.sh`): `tools/test_p25lc_ref.sh` builds MMDVMHost's own `CP25Data` LDU1 coder and checks 500 calls
  both ways (our frames decoded by it, its LDU1 link control decoded by us: LCF, emergency, destination, source;
  passed 500/500 each way, 2026-11-19). Radio rows PX1-PX7.
- [built] P25 channels of CH_MODE 2 (`CONFIG_P25_MODE2`, `prepare_p25_mode2`; round 3 item 18; read compatibility).
  **Since the Classic r13 base** (see "On the Classic r13 base"): the switch also reads Classic r4-r7's 0xA5 records;
  a saved channel is written in Classic r13's RFKV form, not 0xA5; a TX NAC unlike RX is the channel's TX NAC; "hear
  every talkgroup" is the Normal squelch. The text below is the r7-base design:
  the owner's channels 18-21 ("159.64375", "159.8825", "159.88125", "159.4975") are records with mode byte 2, which
  Plus (and Classic) hid. **Where they came from**: not Classic r7. Every Classic release in the upstream history of
  `firmware/` (r4 `fdced2e` ... r7 `1942068`, `tools/classic_channels.py`) writes and reads P25 channels only as mode
  0xA5 (NAC in CH_CC's low nibble and CH_RX_GROUP, talkgroup in the contact word, ID in CH_DMR_ID) and rejects any
  other mode; `RADIO_MODE_P25 = 2` there is the in-memory `channel_t.mode`, never a record byte. The record matches,
  byte for byte, the format of the DM32-nxdn-p25 multi-mode derivative (`research/ref/DM32-nxdn-p25`, patches 0001
  `codeplug.c` `encode_digital` and 0004 web codeplug editor `records.js`; its docs "open-firmware-cps.md 7.3.3.1"):
  mode 2 = P25, the tone words = RX NAC / TX NAC (here 0x0293 both), the contact word = talkgroup (1 or 612), CH_DMR_ID
  with LFLAG1 bit 7 = unit ID (1), CH_FLAG2 bit 6 = hear every talkgroup (clear), CH_CC written 0. Its web CPS /
  flasher (dm32-web-cps-flasher) talks the same CPS protocol Classic answers, so it could write them while Classic
  ran. The leftover CH_RX_GROUP 0x93 fits a Classic 0xA5 record (NAC $293: CH_CC 0xD2, CH_RX_GROUP 0x93, talkgroup 1,
  ID 1, Classic's new-channel defaults) that the derivative's editor read as FM (an unknown mode reads as FM there),
  kept the RX group byte and saved as P25 with its default NAC $293 and CH_CC 0. Not proven which tool, but the
  format is certain. **The NAC field is hex**: in that format the tone word is the NAC as a 12-bit binary number,
  and its editor, CSV and on-radio entry all take three hex digits ("293" = $293, Pi-Star's default); Classic's
  editor too ("$293"). $293 is 659 in decimal. The owner's factory radio takes the NAC in decimal: "293" typed there
  is $125 on the air (NAC in decimal too, above). So "293" on the two radios were two different NACs: to talk to the
  factory radio's "293" this radio needs $125 ("125" in hex, or "293" with Plus's decimal entry: # # 2 9 3); for this
  radio's $293 the factory radio would need "659". **Mapping** (codeplug.c
  `p25_mode2`, after the common fields): P25; NAC = the RX tone word (over $FFF or $F7F: Any, as the derivative reads
  it); TX tone word $F7E and up: its default $293; talkgroup = contact word, 0 with "hear every talkgroup" (Any);
  unit ID from CH_DMR_ID when LFLAG1 bit 7 (over 9,999,999: none); tones, colour code, slot, SFR bytes cleared;
  12.5 kHz. Plus had one NAC for RX and TX, so a record it can't send as written read as **RX only**: TX NAC unlike
  RX, hear every talkgroup, no talkgroup, or no unit ID (the derivative then sends the radio's DMR ID; Plus wants it
  set). Since `CONFIG_P25_TXNAC` (separate RX / TX NAC, below) a TX NAC unlike RX is kept as the channel's TX NAC
  and no longer makes it RX only (RX Any with the derivative's default TX $293 transmits with $293, as the
  derivative does); the other three cases stay RX only. The owner's four: P25, NAC $293, TG 1 / 612 / 612 / 1, ID 1, TX allowed by the record (159 MHz is outside
  Plus's P25 TX bands, 144-148 / 420-450 MHz, so PTT is refused there anyway). **Nothing is written**: reading changes
  no byte; the data chip is not touched by any tool. Saving such a channel on the radio (channel editor Save, or
  saving temporary changes) writes it as a Plus P25 record (0xA5, NAC in CH_CC / CH_RX_GROUP, tone words 0xFFFF):
  that is `codeplug_openfw_encode`'s only P25 format, and an edit is the user's choice; the derivative's TX NAC (when
  it differed) was not kept (with `CONFIG_P25_TXNAC` it is: in the 0xA5 record's TX tone word). The derivative's NXDN (3) and P25 trunking (4) records stay hidden. PC app
  (`pcapp/dm32plus_app/codeplug.py`, outside this tree): a ready diff with tests is in the job's scratch folder
  (`p25x/pcapp-codeplug.diff`): mode 2 shown as P25 with the same mapping and "(CH_MODE 2 biçimi)" next to the NAC.
  Host tests (`tests/plus/test_p25_mode2.c`, built with the switch and without; `tools/test_classic.py` compares
  the two): the owner's two records' bytes decode to P25 / NAC $293 / TG 1 and 612 / ID 1 / TX allowed; 12 NAC cases
  of the derivative's special values; hear every talkgroup, no ID, an ID over 7 digits, no talkgroup, the record's own
  RX only / 25 kHz / skip / power; the record in a codeplug read after a reboot with no write, then saved the editor's
  way as 0xA5 and read back the same; without the switch both records are not channels; FM, DMR, 0xA5-0xA8 and other
  modes: 3,724 records (the encoder's of every kind under 11 mode bytes, and random ones) decode identically in both
  builds. Radio rows CM1-CM3.
- [built] Separate RX NAC and TX NAC (`CONFIG_P25_TXNAC`, `tools/plus_p25nac.py`; owner's item 38, 2026-11-19;
  host-tested, not yet on the radio). **Since the Classic r13 base** the TX NAC, its storage (the RFKV P25 block), its
  editor row and its TX rule are upstream's (PR #14); `CONFIG_P25_TXNAC` keeps the VFO's TX NAC, Home's ">TX NAC"
  and the decimal / `#` entry on top (see "On the Classic r13 base", host tests `tools/test_p25nac.py`). The 0xA5
  tone-word format below was never written on the owner's radio (it ran bb8d8e20, before item 38) and is no longer
  read or written. The r7-base design: A P25 channel had one NAC for both directions, and NAC Any forbade TX. Now it
  has an **RX NAC** (the receiver's filter, Any allowed) and a **TX NAC** ("Same as RX" by default; never Any). Uses:
  (a) a repeater whose input and output NACs differ; (b) RX Any + a fixed TX NAC: hear everything, transmit with the
  right NAC.
  - **Record** (0xA5, `codeplug.c`): the RX NAC stays where the NAC was (CH_CC low nibble, CH_RX_GROUP; CH_CC high
    nibble 0xD). The TX NAC goes into the **TX tone word** (0x22-0x23), which a P25 record has no use for, as
    `0x5000 | NAC`. Any other value there means "same as RX": 0xFFFF (what Plus has always written), 0, or a CTCSS /
    DCS word left over from the FM channel it was. A tagged $F7E / $F7F also reads as "same". So every existing record
    reads exactly as before and saves back byte for byte (test below). Why this word and this tag: bit 15 clear and
    bit 14 set is "no tone" in every tone decoder that may meet the record. In `codeplug.c tone()` (Plus, Classic r7,
    the DM32-nxdn-p25 derivative) such a word is neither DCS (bit 15) nor CTCSS (bit 14 must be clear); the same in the
    upstream web CPS's `getTone` (`site/static/js/codeplug/fields.js`). In RAM the TX NAC is
    `channel_t.p25_txnac` (0 = same, else 0x1000 | NAC). It sits in the struct's tail padding, so `channel_t`
    stays 68 bytes (asserted by the test). `codeplug_p25_tx_nac()` gives the effective one.
  - **What other software makes of such a record**:
    - **Classic r7, or a Plus without the switch**: a channel with RX $293 / TX $659 reads as NAC $293 with no tone.
      It would transmit with $293, and its save writes 0xFFFF, so the TX NAC goes back to "same".
    - **RX Any + TX NAC** (TX allowed) is not a channel there: it fails Classic's P25 rule (Any only RX only), like
      any record Classic can't use. Nothing is misread as a tone.
    - **The upstream web CPS** reads every 0xA5 record as FM (it doesn't know P25), so it shows no tone.
    - **The PC app**: a ready diff with tests is in the job's scratch folder (`p25nac/pcapp-codeplug.diff`). It adds a
      "TX NAC" column ("RX ile aynı" or "$659 = 1625") and the summary "NAC $293 = 659 > TX $659 = 1625 · TG 101".
      The CH_MODE 2 rows show the TX NAC there instead of making the channel RX only.
  - **TX rule** (`codeplug_p25_valid`): a P25 channel transmits when the effective TX NAC is not Any ($F7E / $F7F),
    and it has a talkgroup and a unit ID; otherwise only RX only is valid. So RX Any + TX $659 transmits with $659,
    and RX Any with "same" still only listens. The P25 TX bands (144-148 / 420-450 MHz) and every other gate are
    unchanged (`radio_sm_tx_check`).
  - **Transmitter**: `native_lab.c` arms the channel worker with `p25_lab_set_channel(RX NAC, ...)`, then
    `p25_lab_set_tx_nac(effective TX NAC)`. The receive (`p25_lab_channel_rx`) filters on the RX NAC (Any = every
    NAC). The live transmission (`p25_lab.c live_run`) builds its `p25_call` with the TX NAC, so the NID of the HDU,
    every LDU and the terminator carry it. `p25_lab_set_channel` resets the TX NAC to the RX one, so the other
    callers transmit as before: P25 search (RX only), trunk following (its control channel's NAC), the bench
    commands. A saved TX NAC edit on the channel in use restarts the receiver (`selected_rx` notes the TX NAC too),
    so the next PTT sends the new one. YSF TX is not affected (the TX NAC is P25's only).
  - **Channel editor**: the NAC row is now "RX NAC", unchanged ("$293 = 659"; # Any, # # decimal). Below it is a
    new "TX NAC" row, P25 only (hidden on YSF / NXDN / dPMR). It shows "Same as RX" ("RX ile aynı") or "$659 = 1625".
    OK opens it on its value or on "[Same as RX]". Hex digits and the knob work as on the RX NAC; # = Same as RX;
    # # = decimal entry, as on the RX NAC (from "[Same as RX]" one # is decimal, as the RX NAC from Any). $F7E / $F7F typed (3966 / 3967) mean Same as RX. The rules:
    - RX NAC Any, no TX NAC: the channel becomes RX only ("Any NAC: RX only", as before).
    - RX NAC Any while a TX NAC is set: the channel keeps TX.
    - A TX NAC set on an RX-only channel with RX Any, a talkgroup and an ID: TX is on again (toast "TX with the TX NAC" /
      "TX, TX NAC ile").
    - TX NAC back to Same as RX with RX Any: RX only again.

    A mode change clears the TX NAC. **Home**: the VFO row shows the TX NAC only when it differs, in each of its
    forms: "VFO P25 NAC $293>$659 TG 101" > "P25 NAC $293>$659 TG 101" > "NAC293>659 TG101". On the DM-32's width,
    next to the step, the tight form is the one drawn ("NACAny>659 TG99" with RX Any). Its buffer grew from 72 to
    96 B. A channel's Home line has no NAC (the caller line shows the NAC heard, as before).
  - **VFO**: VFO settings show the TX NAC row (the channel editor's rows). The VFO's TX rule follows the effective
    TX NAC (`vfox_fix`), Copy to VFO carries it, and a mode change clears it. It is **RAM only**: `settings_t` has no
    13 free bits for VFO A in a native mode (its 52 bits went to the NAC, talkgroup and unit ID; the talk target, the
    priority channel and the DMR flags use the rest of the spare words). So after power off, VFO A / B come back with
    "Same as RX" (with RX Any: RX only until a TX NAC is set again). The C..F VFOs and the park slot keep it in
    `vfox_slot_t` (bits 0-7 in the spare byte, bits 8-12 above the bandwidth in its byte). That slot stays 32 B.
  - **CH_MODE 2** records (`p25_mode2`): a TX NAC unlike the RX NAC is kept, not read as RX only (above).
  - **Cost**: FLASH 820,956 -> 822,028 B (+1,072), static RAM 209,744 -> 209,776 B (+32: Home's VFO row buffer
    +24, the worker's TX NAC 2, alignment; `selected_rx` and `channel_t` keep their sizes). Every task's deepest stack
    path is unchanged (UI 368 B free, radio 832, P25 worker 908).

  Host tests (`tools/test_p25nac.py`, from `tools/test_classic.py`):
  - `tests/plus/test_p25nac.c`, built with the switch and without. 5,878 records (the encoder's FM / DMR / P25 /
    YSF records with 18 tone words under 10 mode bytes, and random ones) decode and save back the same in both
    builds, except the tagged P25 ones and CH_MODE 2. The explicit cases: an existing record re-saved byte for byte;
    RX $293 / TX $659 (record bytes, back, then "same" again); TX $000 / $FFF / $F80 / $293 / $125; RX Any + TX $659
    with and without a talkgroup / ID; tagged $F7E / $F7F read as "same"; a leftover CTCSS word kept; YSF never has
    one; flash across a reboot; codeplug_set_channel refuses RX Any + same and takes RX Any + TX. Without the
    switch: what Classic reads.
  - `test_p25_mode2.c` again with the switch: 12 NAC cases with the TX NAC, saved as 0xA5 with it.
  - `tests/plus/test_p25nac_worker.c`: the actual channel worker transmits (fake modem and microphone). Our framer
    receives every frame: HDU, LDU1 / LDU2 and the terminator all carry the TX NAC, and the LDU1 link control carries
    the talkgroup and the ID. Cases: RX $293 / TX $659, RX Any / TX $659, the same NAC, no TX NAC call (search /
    trunk), TX $000. The receive filter stays the RX NAC.
  - `tests/plus/test_p25nac_radio.inc`: the real radio core after the Classic suite, with the VFO modes built in. The
    receiver gets the RX NAC and the worker the TX NAC; PTT arms for RX $293 / TX $659 and for RX Any / TX $659. Any +
    same is refused (TXDENY_MODE, or RX_ONLY), and so are no talkgroup / no ID. A saved TX NAC edit restarts the
    receiver with the new TX NAC. The VFO: vfox_fix's rule, never Any, the C..F slots round trip, Copy to VFO, YSF
    clears it.
  - `tests/plus/test_ui_p25nac.c`: 33 flows in the upstream simulator. Channel editor: the rows, TX NAC hex /
    digits / decimal (from Same as RX the first # is decimal, as on the RX NAC at Any; from a value # # ) / back to
    Same / 3966, saved and reopened, RX NAC hex / decimal / Any; RX Any then TX NAC (TX on), saved, back to Same (RX
    only); TX NAC then RX Any (keeps TX); TG Any stays RX only; YSF hides the row. VFO settings: the row, the TX
    rule, Home "NACAny>659 TG99" / "NAC293>659 TG99" / no ">" when the same, the unit ID row below, Save as new
    channel with it, YSF clears it; C's own VFO in multi-watch.
  - The radio suite above also runs the Classic, multi-watch and VFO radio suites again with the switch built in.

  Radio rows NX1-NX8.
- [built, switch off by default] YSF transmit, Voice FR / VW (`CONFIG_YSF_TX`, `prepare_ysf_tx`, docs/ysf-nxdn-tx.md;
  round 3 item 19; parallel job "ysftx"). Feasibility first: YSF VW feasible (the native modem's 4800 sym/s stream is
  payload-agnostic, YSF uses P25's dibit map, VW voice is P25's IMBE codeword; the deviation is the DMR calibration,
  ~±1.94 kHz, between YSF normal ±2.7 and half ±1.35 kHz: the open radio question); YSF DN feasible on paper but not
  built (the stock AMBE+2 encoder inside a native TX session never ran, DN receive audio is unheard yet, a DN/VW
  channel setting would be needed); NXDN48 not feasible on the native modem (fixed 4800 sym/s, symbol doubling keeps
  DMR's deviation, ~1.85x NXDN48's) and the FM TX RAM route is unproven; NXDN96 possible in principle, not built.
  Built: a YSF channel whose record is not RX only transmits VW (header with CSD1 = "**********" + our callsign, the
  FN 0 / FT 1 sub-header with CSD3 and 2 IMBE frames, 5 IMBE frames a frame after it, terminator; DG-ID from the
  channel; Dev bit 0) through P25's live path (`ysf_live_run`: the same mic FIFO, encoder, blocks, preamble, sealing
  block, time-out, lease and drain); the callsign is the radio name's (as the CW beacon's), else PTT is refused
  ("Radio name must be a callsign", TXDENY_NO_CALL); all of P25's gates apply (inhibit, RX only, 144-148 / 420-450
  MHz, battery, busy lockout); without the switch a YSF channel is refused (TXDENY_MODE) and the worker refuses a YSF
  transmission. Also: YSF receive names the caller from a header (VW calls showed none). Switch off in the Plus
  preset until the owner approves on-air tests; `--feature YSF_TX=y` builds it: FLASH 788,512 -> 791,112 B (+2,600),
  RAM 208,472 -> 208,480 B (+8, the callsign); the default build (switch off) 788,784 B (+272: the header callsign on
  receive), RAM unchanged. Stacks: UI 368 B and radio 824 B free as before, p25-lab 912 -> 908 B. Host tests: test_ysf_tx.c (framing through our own receiver),
  test_ysf_tx_worker.c (the real worker with a fake modem and mic, encoder up to 18 ms a frame: the queue never
  under 4 blocks; built with and without the switch), test_ysftx_radio.inc (the radio core's gates, both builds),
  test_ysf_channel.c (both builds); tools/test_ysf_tx_ref.sh: 1,098 frames bit for bit as MMDVMHost encodes / reads
  them. Radio rows YT1-YT6.
- [built] APRS stations on the map (`CONFIG_MAP`, `prepare_mapnav`, docs/map.md "APRS stations, auto repeater,
  track"; owner's round 3 item 7; parallel job): Tools > Map draws upstream's own heard list (the APRS engine keeps 12
  stations with position, symbol and comment: **no copy, no ring**) as dots with the callsign from zoom 3 (the label
  grid: provinces, then stations, then districts). **0** picks the station nearest the map's centre, 0 again the next
  (walked by distance, round to the nearest; the pick is a callsign hash, so the list reordering as stations are heard
  does not move it); under the map "TA1ABC-9 2.8 km NE 3m ago"; **OK** the details "/> 376 m Hello mobile" (symbol,
  altitude, comment); **OK again** the APRS message composer to it (`CONFIG_APRS_MSG`, To filled in, Back returns to
  the map); **#** switches to the repeater pick, 0 back. The engine's station entry now keeps the altitude its parser
  already decodes (an int16 in the struct's tail padding: `sizeof` stays 64). Host tests test_mapnav.c (order, walk,
  wrap, a station gone, the comment), test_aprs_map.c (upstream's APRS radio suite again with it, the altitude through
  the engine's receive path, the pick over the engine's own list), test_ui_mapnav.c (map flows). Radio rows MN1-MN4.
- [built] Auto repeater (`CONFIG_AUTO_RPT`, `prepare_mapnav`; owner's round 3 item 8; parallel job): Tools > Auto
  repeater (tool 27, after Repeaters; Battery and the zone offer move one on), On / Off, RAM only (Off at power on).
  With a GPS fix, at most every 30 s, the nearest FM repeater (DMR on a DMR channel) of the built-in table with RX and
  TX in the radio's bands; the first is offered at once, another only when it is **20 % and more than 2 km nearer**
  than the one offered before (or the channel's mode changed); never twice the same one in a row, none for the one
  the radio is on. The offer is a banner over any screen for 8 s (a new banner kind): "Nearer: SIVRITEPE 145.700
  12.3 km NE" / "# tunes the VFO"; **#** tunes it through Tools > Repeaters' own tune (`ui_rpt_tune`: the same
  requests and checks) and goes Home; Back hides it; it waits while another banner is up or while transmitting. It
  never tunes without the key and never transmits. Host tests: test_mapnav.c (hysteresis over a synthetic table:
  first offer, pacing, no repeat, 20 % without 2 km and 2 km without 20 %, no flip-flop back, mode change, already
  tuned, no fix, no bands, off, ms wrap; mutation-checked), test_ui_mapnav.c (the row, on / off, the exact tune
  request sequence for DMR and FM, Back, no spam over 5 minutes, moving, waiting for a banner and for TX). Radio rows
  MN5-MN9.
- [built] Track on the map (`CONFIG_MAP_TRACK`, `prepare_mapnav`; owner's round 3 item 9; parallel job): with a GPS
  fix the last 48 positions (a point every 30 s once 50 m moved, as int16 offsets of 0.001 degree like the map data;
  the first point since a clear kept exactly) drawn as a dotted line in the TX colour. **9** "Back to start": the
  first point saved once as the waypoint START and Tools > Go-To opened on it; **7 held** clears the track (7 alone
  says so). RAM only. Host tests: test_mapnav.c (30 s / 50 m / cell rules, jitter on a cell edge, the wrap and its
  order, the start kept, out of range; drawn through the map renderer into a framebuffer: inside the view, clipped,
  dotted), test_ui_mapnav.c (points while moving, START once, Go-To, 7 / 7 held). Radio rows MN10-MN12.
  **All three**: FLASH 777,924 -> 783,100 B (+5,176), static RAM 208,072 -> 208,304 B (+232 of the 600 B budget:
  track ring 192 + 24, auto repeater 14, map pick 2); every task's deepest stack path unchanged (UI 368 B free, radio
  824). New files: integration/radiofoundry/map/mapnav.{c,h}, ui_mapnav.c; small additions to ui_map.c, rpt.c
  (`rpt_nearest_if`), ui_rpt.c (`ui_rpt_tune`), ui_aprsmsg.c (`ui_aprsmsg_compose_to`), nav.c (`nav_rename`),
  ui_goto.c (opened on a target).

- [built] Priority channel and temporary skip (`CONFIG_SCAN_PRIO`, `prepare_scanx`, integration/radiofoundry/scanx; owner's
  round 3 items 2 and 3; parallel job). **Upstream had both halves already, so they are completed, not duplicated**:
  its priority (`scan_cfg_t.priority`, only the debug link's `set.scan_priority` turns it on, no UI) revisits the zone
  scan's *start channel* after every 2 others; its nuisance delete (`scan_t.skip`, a bitmap of the zone's 80
  positions, or 8 VFO frequencies; cleared when the scan ends, RAM only) is # 1 while scanning. **Priority channel**:
  - Setting: Settings > Talk & listen > **Priority channel** (the last row): Off or the channel's number; OK takes the
    channel on view, OK again Off. Also # **5 Set as priority** while scanning (Priority off when on it) and the zone
    editor's channel actions (Zones > zone > # on a channel > **7 Set as priority**, 6 without zone sharing).
    **Stored** in `settings_t.unused_dmr_id`, an old own-ID override upstream ignores ("kept for the layout"): the
    record's layout and size stay upstream's. Format (`radio/scanprio.h`): channel number bits 0-10, the zone it was
    set from bits 16-22 (a hint), the tag 0x50 in bits 24-31; an old ID there (at most 24 bits, radioCoreTest writes
    1234567) or anything else reads as Off. With a channel set, upstream's start-channel revisits are off.
  - Zone scan: the priority channel is the next stop once it hasn't been on the receiver for **2 s**
    (`SCANPRIO_EVERY_MS`), then every 2 s: one stop with the scan's own dwell, probe, hold and wait, so a signal there
    holds it exactly as a scan stop. It may be in another zone (the stop reads that zone, as dual watch does for a
    channel of another zone; the next stop reads the scanned zone again; it is looked for in the scanned zone first,
    then in the zone it was set from). One of the scanned zone keeps its own turn (never fewer stops), and gets no
    extra stop when the sweep reaches it within 3 stops anyway.
  - **Look-back policy (decided)**: during a call (HOLD, also a probe) never; between overs (the resume wait after a
    signal) one look of one dwell when it and a dwell back fit in what is left of the wait (dual watch's rule for its
    own looks): a signal there holds the priority channel (the wait's channel is left), nothing: back to the wait,
    which ends when it would have (not lengthened). No look with the monitor key down, while transmitting (the radio
    core doesn't run the scan then), or while the native P25 receiver holds a channel (the radio core waits). PTT
    during a look talks on the wait's channel (the scan was still on its wait). Common radios' "look-back during a
    call" (a short audio gap every N s) is not done: it would cut the call heard.
  - Dual watch / multi-watch: the same every 2 s and between overs. A priority channel outside the ring is an extra
    stop (the ring's letter stays where it was and the ring goes on from the next; the ring's own wait looks are off
    while the receiver is on it); a priority letter of the ring takes over the wait on another letter at a look (a
    plain ring look only marks Busy). PTT always talks on the selected letter (dual watch's rule). Limits: the panes /
    rows don't name a call on a priority channel outside the ring (its audio plays; the strip says DUAL P); Home's
    "Ring 0.9 s" leaves the priority stops out; a radio with a second receiver (none in Plus) watches without priority.
  - P25 (and YSF) priority channel: the same take / give back as zone scan (`native_lab.c`, "P25 in a zone scan"), the
    second strong poll holds, the native receiver takes it in the hold. Host model (the zone scan model of
    test_zscan.inc, 60 single 2 s calls, receiver noise follows the carrier): zone of 16 (2.4 s round) caught 70 %
    with the P25 channel as priority (70 % without: the round is barely longer than 2 s; 67-70 % between runs); zone of 32 (4.8 s round)
    58 % against 32 %.
  - Marks: Home's channel number "12P" on the priority channel; the strip's word "SCAN P" / "DUAL P" while one is set.
  **Temporary skip**: **0** on Home while a zone or VFO scan has stopped (hold, probe, the wait) skips that channel
  (frequency) until the scan stops: toast "Skipped until the scan stops" (0 while it still steps: "Nothing to skip
  yet", nothing skipped; 0 is free there: Find has no channel 0 and no frequency starts with 0; without a scan 0 is
  Find's as before). Home's hint while scanning: "0 skip  # more  Back stop". # 1 still skips too. **# 4 Clear skips**
  (with the count) undoes every skip of this scan (`RSCAN_UNSKIP`), the priority channel's included. Skipping the
  priority channel ends its priority stops for this scan. Gone when the scan stops and at power off (RAM only). The
  P25 scan is the zone scan, so the same: a skip while the native receiver holds the P25 channel hands it back and
  the scan leaves it. Dual watch keeps upstream's rule (no skip: both channels were picked).
  RAM +88 B static (of the 150 B budget; the objects: radio core state 28 B and its copy of the setting 4, UI copy 4, toast 40, strip
  word 16, 92 B, some of it in alignment gaps), FLASH 788,512 -> 791,480 B (+2,968). Every task's deepest stack path unchanged (UI 368 B free, radio 832).
  Host tests: `tests/plus/test_scanx_radio.inc` (built as `test_radio_scanx` by `tools/test_classic.py`: upstream's
  radio suite, the Classic / Plus P25 suites, the zone scan model and multi-watch's run again first with it built in
  and no priority set; then the setting's word and the settings record across a boot, intervals in a zone scan of
  another zone's channel (2250 ms) and of the zone's own, Off adds nothing, a hit while stepping, between overs: no
  look in a call, the look's time and the wait not lengthened, a look that finds a signal, a reply after a look, PTT
  during a look; multi-watch: a channel outside the ring every 2250 ms with the ring in order, a hit there, a ring
  letter taking over the wait (Off: Busy only), no retune while transmitting; skip and Clear skips, skips gone at a
  scan stop, the priority channel's own skip; the P25 rates above, a skip while the native receiver holds the channel)
  and `tests/plus/test_ui_scanx.c` (upstream's UI flows first, failing exactly as without; then 18 flows: 0 at a stop,
  in the wait, while stepping, without a scan, the hint, # lists 5, # 1, # 4, # 5 and the marks, Priority off, the
  setting on / off, the zone editor, a power cycle, DUAL P). Radio rows PS1-PS9.

Round 3 "misc" job (owner's items 10, 11, 14, 15, 17b; 2026-11-19; parallel job, `tools/plus_misc.py` via
`prepare_misc`). All five together: FLASH 788,512 -> 803,940 B (+15,428: airfields 8.0 KB with their 4.2 KB table,
marine 2.6 KB, battery 2.5 KB, QSO log 1.2 KB, the Turkish strings and the hooks), static RAM 208,472 -> 208,784 B
(+312 of the 600 B budget: QSO marks and frequencies 104, battery history 112, the screens' state and toast texts 96);
every task's deepest stack path unchanged (UI 368 B free, radio 824, host link 608). Host tests: `tools/test_misc.py`
(run at the end of `tools/test_classic.py`).

- [built] Airfields (`CONFIG_AIRFIELDS`; needs AM_RX, APRS, VFO): Tools > Airfields (tool 28, after Auto repeater;
  Turkish "Havalimanları") lists the Turkish airfields **nearest first** from the position APRS uses
  (`aprs_position_now`: GPS, else the computer's, else the fixed one, as Tools > Repeaters), each "LTAC Esenboğa" and
  "26.0 km NE  TWR 118.100" (the tower, else the first frequency); without a position "No position: by code" and the
  table's order. Up / Down / the knob pick (round), OK the details: kind, distance and bearing in degrees, every
  frequency ("ATIS 123.600", "GND 121.900", "TWR 118.100", "APP 119.100"; ATIS, DEL, GND, RMP, TWR, APP, DEP, then
  others), an "8.33" mark on 8.33 kHz channel names, "AM, RX only", "Not for navigation"; OK on a frequency **tunes
  the VFO the way Tools > Air band does** (`radio_req_vfo(true)`, `radio_req_vfo_set`), toast "LTAC TWR 118.100: AM, RX
  only", Home: AM because the frequency is in the air band (`radio_am.h`), and receive only because the radio core
  refuses TX there whatever the VFO says (`radio_sm_tx_check`: TXDENY_AM); refused while transmitting. # opens the
  nearest one's details. **Data**: OurAirports (https://ourairports.com/data/, the daily dumps at
  https://davidmegginson.github.io/ourairports-data/ `airports.csv` and `airport-frequencies.csv`, downloaded
  2026-11-19 into `research-data/ourairports/`, git-ignored, SHA-256 pinned in `tools/airfields.py`). **Licence**: the
  ourairports-data repository is under the Unlicense (a public domain dedication; GitHub's licence field, checked
  2026-11-19); ourairports.com/data calls the data public domain with no guarantee of accuracy (that page itself could
  not be fetched on 2026-11-19: its TLS certificate had expired). Attribution kept anyway: "Airfield data: OurAirports
  (public domain)" (table header). Selection: `iso_country` TR, large and medium airports always (13 without a
  frequency in the data are listed with "No frequencies in the data"), small ones only with a usable frequency;
  heliports, closed fields, balloon / seaplane bases never. Frequencies: 118.000-136.990 MHz as channel names on the 25
  kHz grid or 8.33 kHz names (x.005, x.010, x.015, x.030, ...); an 8.33 kHz name tunes its grid point (121.580 ->
  121.575 MHz, x.010 -> +8.333 kHz, x.015 -> +16.667 kHz, rounded as `radio_am_step` does, so Up / Down on the 8.33
  step stays on the grid); refused and counted: 28 outside the band (military UHF, "25.125", a PMR 433 entry), off the
  grid, 1 duplicate; types normalised from their last word ("Gokcen TWR" -> TWR, "Clearance" -> DEL, "APP/DEP" -> APP).
  Result: 79 airfields, 188 frequencies, **4,228 B** (generator cap 12 KB). Committed table
  `integration/radiofoundry/air/airfield_data.{c,h}` (names ISO-8859-9 like the district table); `python3 -I
  tools/airfields.py --check` proves it is the pinned download's, `--list` prints it for review. Refresh: download the
  two files, update the pins and the date in `tools/airfields.py`, run it, review `--list`. Order without RAM: walked
  like Tools > Repeaters' (`air_next`), the key the squared equirectangular distance at the pair's middle latitude
  (integer, a 5 degree cosine table, no float): within 0.3 % of the great circle from five places across the country.
  Host tests: `tests/plus/test_airfield.c` (the shipped table's invariants and grid points; the order from Kızılay,
  Esenboğa, Taksim, Antalya and Van against a double precision great circle; walk forward and back, ranks; no
  position), the generator on a synthetic pair in `tools/test_misc.py` (selection, every refusal reason, types, names,
  pins refused, `--check`, the byte cap), UI flows in `tests/plus/test_ui_misc.c` (list, details, the exact requests
  `vfo:1 vfo_set:118675000`, an 8.33 kHz name -> 121575000, #, no position, no frequencies, refused while
  transmitting). Radio rows AF1-AF5.
- [built] Marine VHF (`CONFIG_MARINE`; needs VFO): Tools > Marine VHF (tool 29; "Deniz VHF") lists the international
  maritime mobile channels in channel order (01-28, 60-88, 87B, 88B), each with its ship / coast frequency ("157.100 /
  161.700" duplex, "156.800" one frequency, "156.300 / -" ships only) and its use ("Distress, calling" for 16 and "DSC
  data, no voice" for 70 in the warning colour, "Public corr., port", "Port ops, intership", "Intership only",
  "Intership, SAR" (06), "Bridge to bridge" (13), "On board, 1 W" (15, 17), "Navigation, 1 W" (75, 76), "AIS data, no
  voice"). Opens on 16; Up / Down / the knob (Left / Right ten on, on radios that have them), # back to 16. OK **listens**: the VFO on the coast
  station frequency (the ship one for the intership-only channels), FM, 25 kHz, no tones, simplex, through the requests
  Tools > Repeaters' tune uses (`radio_req_vfo_set`, `_mode`, `_bandwidth`, `_tone` RX and TX none, `_vfo_tx(0)`),
  toast "Ch 16 156.800: RX only", Home. 70 and AIS are never tuned ("Ch 70 is DSC data: not tuned for voice").
  **Table**: `integration/radiofoundry/marine/marine.c`, written from ITU Radio Regulations Appendix 18 (Rev. WRC-19;
  the table ITU-R M.1084 Annex 4 refers to): 57 channels plus AIS 1 / 2 (161.975 / 162.025 MHz, called 87B / 88B); a
  duplex channel's coast station sends 4.6 MHz above its ship station; 87 and 88 single frequency since WRC-12. Not
  listed: the four digit channels (1019, 2019 ...: one side of a duplex channel alone, in some countries "19A" /
  "19B"; their frequencies are the two shown), 2006 and national 12.5 kHz interleaved channels. **TX impossible**: the
  radio core refuses TX with the RX only refusal (`radio_sm_tx_check` -> TXDENY_RX_ONLY, the banner a channel's RX
  only flag gives) **while the VFO's RX or TX frequency is within 1.25 kHz of any ship or coast frequency of the
  table** (`marine_hz`). The rule is the frequency's, not a flag: the VFO has no stored RX only flag (its frequencies
  are kept in the settings, its RX only state is not), so a flag would be lost at power off; this way a VFO tuned here
  stays receive only after a TX split, a mode change, a power cycle, or when the frequency is typed by hand. A codeplug
  channel on such a frequency is the codeplug's business (not refused); 12.5 kHz beside a channel (156.8125) is not a
  table frequency. Placed after the air band check and after TX inhibit (TX inhibit is still reported first). Host
  tests: `tests/plus/test_marine.c` (Appendix 18's structure: 33 duplex 4.6 MHz apart, 22 one frequency, 4 ship only,
  every channel number's frequency, 16 / 70 / 06 / 13 / 22 / 28 / 60 / 87 / AIS, what listening tunes, `marine_hz`'s
  window), `tests/plus/test_marine_sm.inc` (the real radio state machine after upstream's whole radioCoreTest suite
  built with the rule: the screen's requests, every listenable channel and every table frequency refused with nothing
  keyed on a board that transmits 136-174 MHz, a split onto and away from a marine frequency, 156.8125 and 145.500
  keying, a codeplug channel on 156.800 keying, TX inhibit first, after a reboot still refused), UI flows (16, a
  duplex channel, 70 and AIS refused, the request sequence). Radio rows MV1-MV4.
- [built] QSO log (`CONFIG_QSO_LOG`; needs CALL_LOG, DMR): **\*** on a Recent row marks it as a QSO (a star before
  the row) or takes the mark away (toasts "Marked as QSO" / "QSO mark removed"); on Recent * no longer opens the quick
  settings (it still does everywhere else); hint "PTT talk  * QSO  # more". **Automatic mark**: a heard call is marked
  when we transmitted on the same frequency (the channel's RX frequency) within 60 s before or after it: any
  transmission the radio view shows as TX (FM and DMR; a P25 one as far as the native path shows it there: radio row
  QL2), and a DMR call we made (its own Recent entry); a mark the user set
  or removed is never changed again by the rule. Each entry also keeps the frequency it was heard (or made) on. The
  marks live beside Recent (`integration/radiofoundry/qso/qso_log.c`: three 32-bit masks, 20 frequencies;
  `ui_recent_t` unchanged) and move with recent_add's moves (the same station again moves up with its mark; a new one
  starts unmarked; the oldest drops out). **RAM only** (as Recent, gone at power off): no safe storage area was looked
  for or invented. **Debug link (read only)**: `0x21 [14]` returns the history. Every change to Recent in the UI task
  happens between `qso_log_begin` / `qso_log_end` (one sequence word; also the * key and the UI's reset), the host
  link task copies only while none is under way and tries again a tick later (4 tries, then "unavailable"). Reply
  (little endian; the status byte first as every 0x21 reply: 0x00 OK, else DL_ST_UNAVAILABLE and nothing more):

  | Offset | Bytes | Field |
  |---|---|---|
  | 0 | 1 | status (0 = OK) |
  | 1 | 1 | version (1) |
  | 2 | 1 | n, entries that follow (0..20), newest first |
  | 3 | 4 | the radio's uptime now, ms (wall time = the PC's time now - age) |
  | 7 | 4 | the automatic mark's window, s (60) |
  | 11 + 36 k | 4 | entry k: age, ms (uptime now - the call's last time) |
  | +4 | 1 | mode: 0 DMR, 2 P25, 3 YSF, 4 NXDN, 5 dPMR; FM caller ID 0x80 MDC1200, 0x81 MDC1200 emergency, 0x82 FleetSync, 0x83 DTMF |
  | +5 | 1 | flags: 0x01 a call we made (DMR), 0x02 to a talkgroup, 0x04 QSO, 0x08 the mark (or its absence) is the user's, 0x10 encrypted, 0x20 P25 emergency, 0x40 a DMR private call to us |
  | +6 | 2 | P25 NAC (12 bits), NXDN RAN, dPMR colour code; 0 otherwise |
  | +8 | 4 | source: DMR / P25 / NXDN / dPMR ID (a call we made: our ID), MDC / FleetSync unit; YSF 0 |
  | +12 | 4 | destination: talkgroup or called ID (FleetSync fleet) |
  | +16 | 4 | frequency, Hz (the channel's RX frequency when the entry was made; 0 not known) |
  | +20 | 16 | name: the caller's name or callsign as the radio shows it (codeplug contact, ID database "TA1ABC Ahmet", YSF callsign, DTMF digits), NUL padded, not terminated when 16 long; ASCII / ISO-8859-9 |

  At most 11 + 20 x 36 = 731 bytes. Host tests: `tests/plus/test_qso.c` (the moves with ui_app.c's recent_add and the
  hooks, the mark following its entry, a dropped one not passing its mark on, the automatic mark before / after / not
  on another frequency / not past 60 s / a DMR call we made / the user's choice kept, the export byte for byte, a full
  list in one reply, refused while a change is under way, too small a buffer), the UI flow (* on Recent, the export
  after it, * elsewhere still the quick settings). The PC app's export is another job's. Radio rows QL1-QL4.
- [built] Battery graph and time left (`CONFIG_BATT_GRAPH`): Tools > Battery (tool 41 since Tools > ISS; it was 30 when added and stays the last tool, "Pil") on a radio with a
  battery opens a page with the charge level and voltage ("84%  7.90 V"), "Time left: 4 h 20 min" or "Time left: --"
  with why ("Collecting: needs 30 min", "Charging", "Not steady"), and the last 3.2 hours as a line graph (scaled to its
  own range, 0.2 V at least; labels top and bottom; "-3.2 h" ... "now"); a radio that measures its supply keeps
  upstream's Supply page. **History**: one sample every 2 minutes, the mean of the voltage readings in those 2 minutes
  (one a second at most, `radio_view_t.battery_mv`, which the radio core reads once a second and not right after TX),
  one byte each in 10 mV steps above 6.00 V (6.01-8.55 V), 96 samples in a ring (3.2 h), taken in the UI task's tick
  whatever screen is open. **Estimate** (integer and Q8 fixed point, only when the page draws): a least squares line
  through the newest samples back to 60 minutes or back to a rise of 30 mV or more between two samples (a charger:
  only the discharge after it counts), at least 30 minutes of them; the line's level now down to the empty level (the
  DM-32's 0 %, 6.40 V) at its slope. "--" while that is not known: under 30 minutes since the start or the last rise,
  rising now or a line that does not fall, or readings more than 30 mV RMS around the line (a load that keeps
  changing). Linear: the Li-ion knee near empty makes the last part go faster than shown. RAM only (the history starts
  again at power on). Host tests: `tests/plus/test_battgraph.c` (the 2 minute means, one reading a second, a gap, the
  byte limits, the ring; straight discharges of 100 / 300 / 200 mV an hour with and without noise against the exact
  time, a slow one, already empty, charging, a charge in the middle, a changing load, a sawtooth over the RMS limit),
  the UI flow (collecting, the graph, "4 h" on a falling voltage, "Charging" when it rises). Radio rows BG1-BG3.
- [built] PC live panel, firmware side (`CONFIG_PC_LIVE`): two read-only additions for the PC app's live panel.
  (1) **Debug link `0x21 [13]`**: the spectrum sweep while Tools > Spectrum runs one, else status DL_ST_UNAVAILABLE
  only. It never starts a sweep or retunes: `spectrum_export` (spectrum.c) copies the engine's state under its
  sequence word straight into the reply, **without counting as a read** (`spectrum_snapshot` does `reads++`, which keeps
  the sweep alive; a PC asking must not keep one alive that the screen left: the sweep still stops 3 s after its
  screen was left) and without a copy of the view on the host task's stack; tried again a tick later while the radio
  task was writing (4 tries). Reply (little endian):

  | Offset | Bytes | Field |
  |---|---|---|
  | 0 | 1 | status (0 = OK) |
  | 1 | 1 | version (1) |
  | 2 | 1 | on (1) |
  | 3 | 1 | points (32, or 64 from a 1 MHz span) |
  | 4 | 1 | hit: the close call's point, 0xFF none |
  | 5 | 4 | center_hz |
  | 9 | 4 | step_hz (point i is at center + (i - points / 2) x step) |
  | 13 | 4 | sweeps completed since the start |
  | 17 | 2 | floor_dbm (int16, the median of the last full sweep) |
  | 19 | 2 | hit_dbm (int16, 0 without a hit) |
  | 21 + 2 i | 2 | dbm[i] (int16; 0 = not read yet in the first sweep; "unknown" readings are stored as -140) |

  At most 21 + 128 = 149 bytes. (2) **GET_VIEW** core section: a new key `plus_mode=am|dpmr|nxdn|ysf|p25|dmr|fm`, the
  mode really received: `am` whenever the radio receives in AM (the air band, `radio_view_t.am`), else the channel's
  (or VFO's) mode as `mode=` names it. A new key does not change the view's version (`debuglink_view.c`: "keys are
  stable (DL_VIEW_VERSION is bumped when one changes meaning or goes away); new keys may appear anywhere in a section"),
  so `view=` stays as it was. Host test: `tests/plus/test_spectrum_export.c` (the bytes of a sweep with a hit, no hit,
  nothing yet, too small a buffer; a PC asking every 100 ms does not stop the idle stop after 3 s, the screen's
  snapshots do; stopped stays stopped). Radio rows PL1-PL2.
- [built] CW decoder and CW practice (`CONFIG_CW_RX`, `prepare_cw_rx`; owner's round 3 item 6; parallel job; needs
  MSG_RX; receive and local sound only). **Tools > CW decoder** (tool 38; All digital search and ISS came after it later) decodes Morse sent as a
  keyed **audio tone** (MCW: a CW ID, a beacon, a practice net) on the FM or AM channel in use. **Limit**: FM and AM
  have no BFO, so a plain CW carrier (A1A, as on HF) gives no tone to decode (on FM it only quiets the noise); the tone
  must lie within 400-1000 Hz. The screen: "18 WPM  712 Hz" and a mark lamp, a status line ("Listening: MCW tone
  400-1000 Hz", "Decoding", "CW: tune an FM or AM channel", "Waiting for receive audio"), then the text as lines that
  scroll up, newest at the bottom (the last 160 characters, RAM only; characters are numbered from power-on so a line
  keeps its characters as the ring moves: a word may break at a line's end). OK clears, # opens CW practice, Back
  leaves. **Runs only while the screen is on top**, through the pager's code (`radio_msg_want` RADIO_MSG_CW = 6;
  fmdec's tone finder / SelCall are 4 / 5): on an FM channel FM caller ID's audio (`radio_msg_audio`; the normal
  receive path, not the flat one: MCW is a voice band tone, so the speaker sounds as usual), on an AM channel the
  receive ring itself as ACARS does (`acars_service`; FM <-> AM on the same frequency restarts it on the other path);
  FM caller ID Off in Settings still lends its ring (`radio_msg_wants_fm`). DSP (integer, `integration/radiofoundry/cw/
  cwdec.c`, a 16 ms block of 128 samples at a time): 25 Goertzel bins 400-1000 Hz, 25 Hz apart (Q12 coefficients, the
  block shifted to +-1023 so nothing overflows); a block is a tone when its peak stands 12 times over the mean of the
  bins 100 Hz and more away, and counts only beside another tone block on the same bin (a tone holds still, a voice's
  harmonics don't): the gate then stays open 3 s; the tone from the averaged power shape by a parabola (to 1-2 Hz,
  drift followed block by block; a lone block elsewhere never moves it). The block mixed down by the tone (64-entry
  sine), I/Q through two one-pole low passes (2 ms each from 15 WPM up, 4 ms below, 8 ms below 8 WPM), the magnitude
  once a millisecond, keyed one block late so the gate and tone are known first. Adaptive threshold: mark level
  (fast up, follows marks down, sinks towards the space level over 6 dots so a mark after a fade still keys), space
  level (spaces only, never the start of a mark); key down above half way, up below 3/8, never within 3x / 2x the
  space level (where the noise's envelope rarely reaches); a change must hold a quarter dot (3-15 ms). Timing 5-40
  WPM: the last 8 marks split by 2-means into dots and dashes (else the running averages), the averages kept 2-4.5
  apart; the unit is half the dash-dot difference, so a keyer's weight cancels and is added back to each space;
  character end at 2 units, word space at 5 units or between the two kinds of long gap when the recent ones show both
  (Farnsworth spacing: 1.5 x their mean when all are 4.5-6.5 units). A transmission's start (and after the gate
  closes or a pause of more than 14 units) is **learned**: marks and spaces kept (40) until the marks split and three
  character gaps have come, then decoded with that speed, so the first characters (a short CW ID) come out right; the
  first ones appear together. ITU-R M.1677-1 letters, figures and punctuation; prosigns <AR> <KN> <SK> <AS> <KA> <VE>
  <BK> <CL> <SOS> <HH> (7+ dots), BT as "="; unknown "*". CPU about 1 % of the core while the screen listens.
  **CW practice** (# on the decoder): Koch method (G4FON / LCWO order, 41 characters, KMURESNAPTLWI.JZ=FOY,VG5/Q92H38B?
  47C1D60X): Groups (five) or Characters (one at a time), from the first n (2-41; the newest drawn a third of the time
  and more), Speed 5-40 WPM, Tone 400-1000 Hz in 50 Hz steps (Up / Down pick, Left / Right change; RAM only). OK plays;
  the answer is typed with the radio's keypad letters as its text entry has them (a **simplified own entry**, not the
  APRS composer's screen: 2 ABC ... 9 WXYZ cycling within 1 s then the digit, a held key its digit, 1 . , ? / = 1,
  0 0; Right ends a letter, Left / Back delete); OK checks: characters right = sent less the edit distance (wrong,
  missed, extra), "Right: KMURE" / "Sent: KMURE" (Characters mode adds its dots and dashes), "Score 12/15  80 %";
  Characters mode plays the next a second after the check; OK held plays it again (* stays the radio's quick
  settings), # zeroes the score. **Sound path, local only**: the radio has no local-only sidetone (the CW ID / beacon
  keyer's tone is the FM modulator's `tx_tone`, sounding only while transmitting), so the practice is "listen, then
  answer" with **the beep generator the key beeps use**, made sample exact: `beep.c` gained BEEP_MORSE, whose segments
  come from a text set with `beep_set_morse` (each element faded 2 ms as any beep), at the key beep level (50 when key
  beeps are off). The page hands the text to the radio task (`radio/cw_train.h`, a sequence lock); the radio task's
  poll plays it with `beep_play` and nothing else: no `tx()`, no PTT, no `tx_tone()`; while the radio transmits (or is
  a hotspot) the request is refused ("Not while transmitting"); key beeps are left out while it plays (they would cut
  it). Cost: FLASH 821,508 -> 831,520 B (+10,012), static RAM 209,744 -> 210,096 B (+352 of the 400 B budget: text ring
  160 + info and counters ~30, practice page ~95, the hand-over ~24, beep Morse ~35); the decoder (cwdec_t, ~640 B)
  from the RTOS heap while its screen is open; every task's deepest stack path unchanged (UI 368 B free, radio 832,
  audio 1,616). Host tests: tests/plus/test_cwdec.c (own keyer and table: random text 5 / 12 / 20 / 30 WPM, and 8, 35,
  each with 15 % element jitter at 6 dB SNR, heavy (+0.3) and light (-0.25) weight, fading -14 dB at 18 dB (trough +4
  dB), -6 dB flutter, tone drift 0.6-2.5 Hz/s, tones 400-1000 Hz, Farnsworth gaps, everything at once: CER 0-0.6 %
  over three texts each, limit 2 %; CER by SNR at 20 WPM: 0 % to +4 dB, 2 % at +1 dB, 56 % at -2 dB; prosigns in a QSO;
  speed changes 15->30, 30->12 (1 s pause), 25->8, 8->25 (gate closed); a lone CW ID; no text from 60 s of noise or a
  held tone; the radio glue), tests/plus/test_cw_train.c (Koch draw, score, timing, hand-over; BEEP_MORSE's PCM: every
  mark and gap within 2 samples of its units at 5-40 WPM, decoded back, mixed into voice, cut to 12, replaced),
  tests/plus/test_cw_radio.inc (upstream's radio suite again with it built in, then the practice through the real
  radio core and beep stream: decoded and timed to the sample, 24 groups at 5-40 WPM and 400-1000 Hz, **no TX request
  of any kind**: no tx(), no FM TX tone or keying, no DTMF, no PTT, never RS_TX; refused while transmitting; key beeps
  left out; level 0), tests/plus/test_ui_cw.c (Tools entry, the screens, the decoder wanted only while on top, every
  key, scrolling, refusal, layout; sim TX count 0). Radio rows CW1-CW10.

Queued by the owner (2026-11-18), with the feasibility notes from the discussion:

- [built, radio check DT1-DT8: "DMR Tier III trunk following" above] DMR Tier I / II / III. Tier I (446 MHz
  direct) and Tier II (repeaters, two slots) are upstream's DMR and work now. Tier III: receive-only trunk following
  (decode the control channel's CSBK grants, follow the voice channel, return) is the part a radio can do;
  transmitting on a trunked system needs registration and is out. Mainly useful where such a system may lawfully be
  heard.
- [built, radio check TK1-TK8: "P25 Phase 1 trunk following" above] P25 Phase 1 trunk following: the native P25
  receiver already decodes the 4800 Bd C4FM; add TSBK decoding
  on the control channel (IDEN_UP for the channel plan, group voice grants), follow a grant to its voice channel,
  return at the end. Receive only. Phase 2 (TDMA, H-DQPSK 6000 sym/s) is not possible on this hardware.
- [built except TLE loading on the radio: "Satellites" and "ISS APRS digipeater" above] Satellite mode: pass prediction from TLEs (SGP4, fixed point or soft float, offline) with the GPS position,
  next passes for ISS / FM birds, live Doppler correction of RX/TX during a pass, the ISS APRS digipeater 145.825.
  TLE entry: typed, or loaded over the cable (no network on the radio).
- [built, radio check SV1-SV7: "SSTV receive" above] SSTV receive (Robot 36 / PD120 from the ISS, FM audio at
  8 kHz): image drawn into the 4-bit palette framebuffer (grey levels or a coarse palette); RAM for one line at a
  time. Done as Tools > SSTV (`CONFIG_SSTV`, third round above): 12 modes, VIS or line sync start, slant fitted,
  12 dithered colours, the lines in progress only.
- [built, radio check M4-M6] ACARS on the air band (131.550 etc., AM audio, 2400 bit/s MSK on 1200/2400 Hz):
  message list. Done as Tools > ACARS (`CONFIG_MSG_RX`, third round above). AM audio reaches the CPU through the
  same HR_C7000 RX RAM path as FM when the receive ring runs (the FD6818's AF output feeds the HR_C7000 whatever
  its type); unverified on the radio, as are the BCS variant and the bit/tone mapping (the decoder tries all four).
- [built, radio check M1-M3] POCSAG (512/1200/2400 Bd FSK on FM audio): pager message list. Done as Tools > Pager
  (`CONFIG_MSG_RX`); it hears FM caller ID's audio but needs its own DSP (NRZ, not FFSK), with the flat receive
  path while its screen is open.
- [built, radio test open] SFR on the radio: the channel editor's SFR rows (`CONFIG_SFR_EDIT`, third round above)
  set a one-frequency DMR channel's role (User, ERDM user, Repeater, Repeater ERDM style), its inbound slot and the
  ERDM outbound color code; the Codeplug Editor is no longer needed. Upstream marks standalone SFR experimental
  (host and SDR bench tested, not reliable end to end). The repeater roles transmit by themselves (unattended
  repeater: licence conditions apply); the user roles are ordinary talking. Radio rows SR1-SR6.
- [built, radio rows BR1-BR3] Name and public build (`tools/plus_brand.py`, `prepare_brand`; owner's request
  2026-11-19): "Name and public build" below.

Round 13 "ideas 1" (owner's items 23, 28, 30 and 33; parallel job, 2026-11-20; `tools/plus_fieldx.py`, our code in
integration/radiofoundry/fieldx/ installed into core/ui/). Host-tested, not yet on the radio (rows I23-*, I28-*,
I30-*, I33-* in plus-radio-test.md):

- [built] Search finds saved as channels (`CONFIG_FIND_SAVE`, item 23; needs ZONE_EDIT and P25_SEARCH or DMR_SEARCH).
  On Tools > P25 search and Tools > DMR search **#** saves every find into a new zone **"Bulunanlar N"** (N one more
  than the highest such zone; the name is not translated, it is codeplug data); **Save this one**: P25 search, Up /
  Down pick a find (the DMR screen's pick, new on P25) and OK; DMR search, OK held on a picked find (OK alone still
  tunes the VFO). It goes into the newest "Bulunanlar" zone, or a new one when there is none or it is full. **The
  writes are the zone editor's own**: `ui_svc_zone_edit` with ZONE_OP_NEW for the zone, then one ZONE_OP_ADD_NEW a
  find, the channel editor's "Copy here" path (a free channel number written and put at the zone's end in one storage
  job, checked before anything is written; the foreign codeplug question applies as to any edit). One step a UI tick
  (`ui_findsave.c`), the next only once the zone editor's queue is empty and the step is seen in the codeplug (the
  zone found again by its name: an index can move). A find already in that zone (same mode, RX frequency and NAC /
  colour code) is skipped; the zone's capacity (`codeplug_zone_capacity`), a codeplug with no free channel, no room
  for another zone and a refused write end the save with the count so far. Toasts: "Bulunanlar 2: 5 saved, 1 there",
  "Saved to / Already in Bulunanlar 2", "Zone full: 3 saved", "Codeplug full: 3 saved", "Stopped: 3 saved" (after the
  refusal's own toast), "No room for another zone", "Nothing found to save"; "Saving 3 of 8" on the search screen
  meanwhile, whose list stays as it was until the save has ended. Channels (`findsave.h`): named "P25 433.500" /
  "DMR 438.7125" (a fourth decimal only when needed), simplex on the frequency found, 12.5 kHz, the radio's power.
  **P25**: the NAC found (TX NAC Same as RX), Normal squelch (every talkgroup on that NAC: "TG Any"), the talkgroup
  heard kept as the TX talkgroup. **Rule for TX**: RX only unless a talkgroup was heard (the system's own, from a voice
  call's link control), our own P25 unit ID is known (the radio's default, `codeplug_p25_unit_id(0)`; the editor's
  Radio ID "Default"), the frequency is in 144-148 / 420-450 MHz (the editor's outside-band rule) and in the radio's
  TX bands; never with NAC $F7E / $F7F. That is `codeplug_p25_valid`'s own rule (TX needs a talkgroup and an ID) made
  safe: a found system is not ours, and the source ID heard is the other radio's, never used as ours. **DMR**: the
  colour code found (Any when none was named), the one slot seen (Any when both or none), RX group list Any, no TX
  contact, always RX only (the search never learns a talkgroup we may use; pick a contact in the editor to talk).
  RAM: 54 B (the save's state 14, its toast 36, P25 search's pick 4); the zone and channel copies live on the UI
  stack only inside noinline readers, never with a zone_op_t (the UI task's deepest path is still upstream's).
  Not changed: the P25 search's find rows were already wider than the DM-32's 220 px safe area with the NAC in
  decimal ("433.5000 $1C8/456 TG655 1s ago": the end is cut on the screen); left for the screen's owner.
- [built] Find my friend (`CONFIG_NAV_FOLLOW`, item 28; needs NAV and APRS). **5** on Tools > Map's APRS station pick
  (0) or **OK** on Tools > APRS > Stations heard opens Go-To following that station: "Follow TA1ABC-9", the rose,
  distance and bearing, and "Heard 3m ago"; once a second the station is looked up in upstream's heard list (no copy)
  and a newer position taken, so the arrow moves live. Not heard for **10 minutes or more**: "STALE: heard 12m ago" in
  the warning colour, the last position kept (also when the station drops out of the 12-entry list); "No position
  from it yet" for one heard without a position. **Back** (toast "Following ended", Go-To closes), **Up / Down /
  the knob** (another target: the list again) or **OK** (a new waypoint) end it; Tools > Go-To opened any other way
  is not following. The age is the station's (any packet heard); a status packet without a position moves it but not
  the position. RAM: 28 B (one callsign, position and time, 24; the poll time, 4). `follow.c` (pure), `ui_goto.c`.
- [built] Locator calculator (`CONFIG_LOCATOR`, item 30; needs NAV and APRS_MSG, whose text entry it uses). **#** on
  Go-To opens the page; # there types a 4 or 6 character Maidenhead locator with the radio's text entry (titled
  "Locator"; keypad letters, a held digit is the digit; any case, written back as "KM69kw"); the page shows the
  centre's latitude / longitude ("39.9375 N  32.8750 E"), the distance and bearing from where we are
  (`aprs_position_now`: the GPS fix, else the computer's, else the fixed position, as Tools > Repeaters) and our own
  locator; **OK** saves the centre as a waypoint named after the locator (Go-To's list, RAM only); a bad one is
  refused ("Not a locator: like KM69 or KM69kw"). The existing locator code only goes from a position to a locator
  (Tools > Repeaters' `rpt_locator`, Tools > Position's grid page); the way back, locator to centre, is new
  (`locator.c`, integer 1e-7 degree) and encodes exactly as those do. RAM: 7 B (the text typed).
- [built] One-key SOS (`CONFIG_SOS_KEY`, item 33; needs KEY_ASSIGN). Settings > Keys offers **"SOS"** for a side
  key's press and long press (`KEYFN_SOS`, appended as value 17: stored values never move; input action `ACT_SOS`);
  **not assigned by default**. Tools > **SOS key** picks the actions: APRS SOS, DMR emergency, CW SOS (once), each
  On / Off, **RAM only, all off after power on** (no settings bit: the VFO / priority words are taken). The key opens
  a confirmation page listing the actions chosen ("Send SOS now?", "It TRANSMITS"); **OK** or **a long press of the
  same key** starts them (set on the press with the long press left at Default, the long press sends SOS too),
  **Back** cancels; with nothing chosen it says so and sends nothing. Like the emergency key it works with the keys
  locked (the page's OK is then locked; the key held confirms). The actions are the existing calls with all their
  gates, as the lone worker's: the CW beacon's "SOS SOS SOS DE <call>" once (`radio_req_cw_beacon`), `aprs_sos_set`
  (Tools > APRS > SOS: the Mic-E emergency position now and every 2 minutes), `ui_svc_emergency` (the emergency
  key's). **Order**: the CW beacon is cut by any other radio request and keys the channel out of DMR, so it goes
  first; the APRS SOS is turned on once the CW is on air (the engine waits for the channel), the DMR emergency once the
  CW message has ended; without CW both start at once. The page and toasts say what was sent or why not ("CW SOS
  sent", "CW SOS not sent: can't transmit here", "DMR emergency sent", "Not sent: needs a DMR channel", "SOS not
  sent: no position", "SOS not sent: APRS not ready"; each SOS position's own toast is the APRS page's). The key
  again while one runs offers **"Stop SOS?"** (OK or the key held): the CW cut, the emergency canceled, the APRS SOS
  off; what went out stays sent; an emergency or APRS SOS that was on before (the user's own, the lone worker's) is
  left alone. RAM: 17 B (`sos.c` 15, the page 2).
- Tests: tests/plus/test_findsave.c (names, the P25 / DMR rules, the TX rule's every condition, duplicates, zone
  names; with and without DMR_ANY), test_follow.c (update, other stations, kept position, the 10 minute edge, the
  millisecond wrap), test_locator.c (published centres JN58td, FN31pr, IO91wm, KM69kw, 4 characters, the corners,
  refusals; the encoder against a double precision reference over the world and back into the subsquare; nav_way's
  distance / bearing between centres within 0.5 % / 1 degree of a double great circle), test_sos.c (recording fakes:
  **nothing asked to transmit before sos_start, nor with every action off**; the order, once each, refusals and their
  events, Stop, the user's own left alone; with and without the CW beacon), test_sos_key.c (`keyassign.c`: appended
  value, both slots, the long press, a build without it reads Default), test_ui_fieldx.c (19 flows through the real
  UI in upstream's simulator with the real zone_edit.c: zones and channels as written, duplicates, zone full,
  codeplug full, no room for a zone; follow from the heard list and the map, live, stale, gone, the endings; the
  locator page, typing, centre, distance, waypoint, refusal; Tools > SOS key, nothing chosen, Back, OK, the key held,
  locked keys, Stop, a refusal), run by tools/test_fieldx.py from tools/test_classic.py.
- Cost (all four): FLASH 841,040 -> 852,472 B (+11,432, strings and Turkish included), static RAM 210,584 ->
  210,688 B (+104 of the 120 B budget: item 23 54, item 28 28, item 30 7, item 33 17, less alignment), every task's
  deepest stack path unchanged (UI 268 B free, radio 760, p25-lab 328).

Round 13 "ideas 2" (a static RAM reclaim, then owner's items 24, 26, 27, 29 and 34; parallel job, 2026-11-20;
`tools/plus_toolbox.py`, our code in integration/radiofoundry/common/ and integration/radiofoundry/toolbox/ installed
into core/ui/). Host-tested, not yet on the radio (rows RR13-*, I24-*, I26-*, I27-*, I29-*, I34-* in
plus-radio-test.md):

- [built] RAM reclaim, round 13. **One buffer for the six tool screens' copies** (`core/ui/ui_scratch.c`, a union of
  `spectrum_view_t` 280 B, `dmrs_view_t` 228, `p25s_view_t` 204, `dmrtk_view_t` 212, `p25tk_view_t` 184 and the
  pager / ACARS message page 184): each of Tools > Spectrum, DMR search, P25 search, DMR trunk, P25 trunk and the
  message page refills it at the start of its own draw and in its key handler (a fresh snapshot; the spectrum's OK
  now tunes to the hit of a fresh copy instead of the last draw's), and only the screen on top draws or gets keys.
  What must outlive a draw stays in each screen's own statics (span, range, the find picked, the page's scroll). The
  one reader outside its screen is the find save (`ui_findsave.c`, item 23), which reads the P25 / DMR search list
  frozen as the save began: `ui_scratch_take` / `ui_scratch_mine` name who filled the buffer last, the search screen
  keeps its frozen list only while it still owns it, and when another screen has taken it the save stops at the next
  find ("Stopped: n saved"; before the first step: "Stopped", no zone made); every channel already written is whole.
  **Debug log ring 1,024 -> 512 B** (`PLUS_DLOG_RING_BYTES`; about half a dozen lines between two reads of the debug
  link, `tools/plus_live.py` polls far more often). Heap and task stacks unchanged. Measured: static RAM 210,688 ->
  209,168 B (-1,520: the six copies' 1,292 B -> the union's 280 and its owner byte, about -1,010; the ring -512), **2,288 B free**; FLASH
  +168 B; every task's deepest stack path unchanged.
- [built] Heard list (`CONFIG_HEARD`, item 24; needs CALL_LOG). Tools > **Heard list** ("Duyulanlar"): every station
  heard on P25, DMR, YSF, NXDN and dPMR, one row each, newest first: "P25 77285" and "TG101 $659/1625 x12 3m" (the
  talkgroup or the unit called, the NAC / colour code / RAN, the calls, how long ago; the NAC, then the talkgroup
  left out where the row is too narrow). Entries merge by (mode, source, destination); a private call and another
  mode are other entries. Fed where Recent is fed (ui_app.c `recent_add`: every new call; while a DMR or P25 call goes
  on, `ui_heard_recent(.., false)`: its last-heard time, and another call only after 5 s of quiet), with the
  frequency and DMR's colour code / slot of the view it was heard on; calls we made and FM caller ID's kinds are not
  kept. 16 entries of 16 B (`heard.h`): 24-bit source and destination (a YSF callsign packed into those 48 bits as 8
  characters of 6 bits), an 8-bit saturating count, the frequency, 20-bit last-heard seconds (an entry older than
  about 6 days is dropped before that clock wraps) and the 12-bit NAC; the brief's 12 B had no room for both the
  talkgroup and the frequency Tune VFO needs. **OK** opens one: mode, destination and source, NAC / colour code and
  slot / RAN, frequency, calls, last heard; on a P25 or DMR one **OK tunes the VFO** (`CONFIG_VFO_MODES`: the VFO
  settings' own path, `radio_req_vfo(true)` then `ui_svc_vfo_apply`: the frequency, the mode, P25 NAC and talkgroup
  (Selective, the VFO's own rule; TX only as `vfox_fix` allows), DMR colour code and slot; a DMR talkgroup then becomes
  the talk target as the actions' Talk does, "Talking to TG 2620"). RAM only, empty after power on. RAM: 280 B (the
  list 260 with its count, the open row 16, the tick's view pointer 4).
- [built] Spectrum waterfall (`CONFIG_SPECTRUM_WF`, item 26; needs SPECTRUM). Tools > Spectrum shows the last 40
  finished sweeps under the bars (the bars take the upper 45 %), newest on top, each point's level above the sweep's
  floor in 3 dB steps (16 levels, 4 bits) drawn in the screen's own colours (background, line, idle, dim, RX, warning,
  TX; the 16-colour frame buffer has no room for a palette of its own); a new center or span starts the rows over.
  **\*** turns it off and on (a toast says so; RAM only, on at power on; `*` no longer opens quick settings on this
  screen, as on Recent). **Its 1,280 B buffer comes from the RTOS heap** while the screen is on top with the
  waterfall on: taken on entry (or with *), given back on Back, on OK (tune) and at the first UI tick with another
  screen on top (`ui_spectrum_wf_tick`), taken again when the screen is back on top (the rows start over). When the
  heap has no room the bars are drawn alone with "No room" (tried again at the next entry or *). Not the frame buffer:
  the screen is cleared at every redraw and toasts / banners are drawn over it, so the history cannot live there.
  **Heap**: the radio read `tasks.heap_min_free` 1,640 B with FM caller ID's 2 KB ring running (RR1, 2026-11-18),
  and the heap was enlarged by 2,048 B after that, so about 3.6 KB should stay free with the ring; the waterfall takes
  1,288 B with heap_4's header. During a sweep FM caller ID gives its ring back (radio_sm.c: `fmid && !spectrum_on()`,
  `radio_fmid_poll(false)` -> `ring_stop` -> `hrc_fm_rx_stop` frees it), unless APRS receive owns it (an FM channel
  on the APRS frequency: `aprs_poll`'s `fm_rx` is not gated by the sweep), so the worst case keeps the ring and
  still leaves about 2.3 KB. The pager / SSTV / CW decoders' heap blocks belong to their own screens and are not
  taken while the spectrum is on top. A host model of the whole RTOS heap would only repeat estimates; the radio row
  I26-3 reads `heap_min_free` with the waterfall open. RAM: 14 B (the ring's state and pointer, the switch, the
  no-room flag).
- [built] Coverage map (`CONFIG_MAP_COVERAGE`, item 27; needs MAP_TRACK). While the track records (a point every
  30 s once 50 m from the last), each point also keeps one byte, the signal of the channel or VFO in use when it was
  taken (`radio_view_t.rssi_dbm` + 141, clamped to -140 .. -1 dBm; 0 when the receiver gave no reading), in
  `ui_mapnav_cov[48]` beside the ring (the same slot: the track's head and n, `track_slot`). On Tools > Map **\***
  turns Coverage on / off (RAM only): every track point a dot in its signal's colour, on the S-meter scale of VHF /
  UHF (S9 = -93 dBm, 6 dB a unit): **green S7 and up (-105 dBm), yellow S5-S6 (-117 dBm and up), red weaker**, grey
  no reading; the legend "S7+ S5 <S5" in those colours under the map beside the scale. RAM: 49 B.
- [built] GPS dashboard (`CONFIG_GPS_DASH`, item 29; needs GPS). Tools > **GPS**: the speed large (km/h), the heading
  as one of 16 compass points and degrees (only from 3 km/h), the altitude, the trip distance and the top speed since
  the reset, the fix (2D / 3D) and the satellites used. **OK held** resets the trip and the top speed. The trip counts
  whatever screen is open (the GPS read once a second, `ui_gpsdash_tick`): a fix 25 m or more from the last counted
  point adds the straight way there (integer equirectangular distance), so standing still adds nothing; a step
  faster than 400 km/h is a jump (not counted, counting goes on from there); a GPS speed above 300 km/h is not the
  top speed. RAM only. RAM: 24 B (the trip meter 20, the poll time 4).
- [built] Clock tools (`CONFIG_CLOCK_TOOLS`, item 34). Tools > **Clock** ("Saat"), four rows (Up / Down): **Stopwatch**
  (OK start / stop, # a lap while it runs: the newest three laps with their own times; # while stopped: reset);
  **Countdown** (Left / Right or the knob set it while stopped, a minute a step, 10 s under a minute, held five steps,
  up to 99 min; OK start / stop); **Alarm** at a time of day, local (Left / Right a minute, held 10; OK on / off;
  one shot: off once it has rung); **UTC offset** (half an hour a step, +03:00 at first). The title shows the local
  time. **Time of day**: the board's RTC has no driver in upstream, so the clock is the GPS's UTC time (taken once a
  second while there is a fix; the receiver keeps the last fix's time after it is lost, so no time is taken then) run
  on by the millisecond clock; without a GPS time since power on the screen says "No clock: needs GPS" and the alarm
  cannot ring. The alarm rings when its minute begins (a clock moved on by up to 10 minutes at once still rings; a
  bigger jump does not, rather than ringing for a time that never was; the minute it was set in does not ring until
  the next day). **The countdown's end and the alarm sound and show a banner on any screen**, through the satellite
  pass alarm's own path in `ui_app_tick` (an info banner, or a toast over a banner that waits for the user; the
  backlight on): "Countdown done" with the TX time-out warning tone, "Alarm 07:30" with the scan hold tone. RAM only,
  all off at power on. RAM: 90 B (the state 56, its text 24, focus and times).
- Tests: tests/plus/test_toolbox.c (the heard list's merge, order, counting, the gap rule, saturation, the oldest
  dropped, expiry across the 20-bit clock, YSF callsigns; the waterfall's levels, colour slots, row ring and wrap, a
  new point count, no buffer; coverage's byte and its S-meter thresholds; the trip meter's still / moving / jump /
  outage / spike cases, the compass; the stopwatch and laps, the countdown's end once, the alarm's minute with the
  offset, jumps, the next day, no clock), test_ui_toolbox.c (10 flows through the real UI in upstream's simulator,
  tests/plus/ui_sim_toolbox.c: the six screens one on top of another and back, each from its own copy and none of
  another's text, the spectrum's OK after them; a find save stopped by another screen before and during it; the heard
  list from DMR calls and P25 / YSF entries, Tune VFO for DMR and P25, none for YSF; the waterfall's heap block
  counted through entry, *, another screen, Back and OK, its rows drawn, the allocation refused; coverage from moving
  GPS fixes with three signal levels, the legend; the GPS dashboard; the clock's stopwatch, the countdown's banner on
  Home, the alarm's on another screen from a running GPS time), run by tools/test_toolbox.py from
  tools/test_classic.py.
- Cost (step 2, all five): FLASH 852,640 -> 862,860 B (+10,220, strings and Turkish included), static RAM 209,168 ->
  209,672 B (+504 of the 600 B budget), **1,784 B free**; every task's deepest stack path unchanged (UI 268 B free,
  radio 760, p25-lab 328). The whole job: FLASH 852,472 -> 862,860 B, RAM 210,688 -> 209,672 B (768 -> 1,784 B free).

## DOTA Transfer (`CONFIG_DOTA`, ours; branch `dota`)

Radio-to-radio transfer of channels, zones, contacts and RX group lists over FM (AFSK 1200, CTCSS) or DMR (data
service). Design, owner's decisions and test plan: [dota.md](dota.md). Code in `integration/radiofoundry/dota/`,
hooks in `tools/plus_dota.py`, host tests in `tools/test_dota.py` (run from `tools/test_classic.py`).

Budget, measured after each phase 2 step (`--plus --stack-check --verify-rebuild`: zero warnings, forced rebuild
identical, every task inside its stack). Limits for the job: FLASH headroom >= 12 KB (<= 888,832 B of 901,120),
static RAM free >= 1,500 B (<= 209,956 B); from the rebase onto `ddm25` 3032f91 (CONFIG_HOME_FREQ, +344 B) on, the
owner relaxed the flash rule to >= 11,500 B free (<= 889,620 B).

| Step | What | FLASH B | RAM B | RAM free | Stacks: UI / radio free |
|---|---|---:|---:|---:|---|
| base | ddm25 at 1c391c7 | 864,036 | 209,672 | 1,784 | 268 / 760 |
| 0 | `audio_mux.c`'s APRS ring compiled out (`PLUS_AUDIO_MUX_NO_APRS`; the DM-32 reads APRS audio from its own AFSK receive path) | 863,704 (-332) | 207,616 (-2,056) | 3,840 | 268 / 760 |
| 1 | `CONFIG_DOTA`, the protocol (`dota_proto.c`: frames, manifest, both ends' ARQ). Built, not linked until the radio core calls it (object: 4,487 B text) | 863,704 (0) | 207,616 (0) | 3,840 | 268 / 760 |
| 2 | The records, format version 1 (`dota_rec.c`). Not linked yet (object: 2,560 B text) | 863,704 (0) | 207,616 (0) | 3,840 | 268 / 760 |
| 3 | The import (`dota_import.c`, `codeplug_dota.inc`: staging without in-use bits, one commit, undo); `codeplug_set_channel` gains a staging twin. Import not linked yet (object: 2,404 B text) | 863,760 (+56) | 207,616 (0) | 3,840 | 268 / 760 |
| 4-5 | The session (`dota.c`: preparation, windows, answers, FM over AX.25 and the AFSK path with `tx_more`, DMR over chained UDP, sounds), the radio core's side (`radio_dota.inc`: the session channel, PTT refused, requests held, keying), hooks in `aprs.c`, `dmrDataService.c`, `hrc_afsk.c`, `radio_dm32.c`, `beep.c`. `dota_buf` 1,740 B | 878,356 (+14,596) | 209,632 (+2,016) | 1,824 | 268 / 760 |
| 6 | Tools > DOTA Transfer and its picker (`ui_dota.c`: the form, questions, run page and PTT prompt as `ui_draw_list` pages), 43 Turkish entries, the PTT prompt's path. First measured at 892,012 B (over the limit); trimmed to fit: shorter texts and reused catalog strings, table-driven results, the power as a class, list pages instead of hand drawing. Flash headroom left: 12,580 B (292 B above the job's limit) | 888,540 (+10,184) | 209,648 (+16) | 1,808 | 216 / 760 |
| 7 | Docs: `dota.md` sections 13.1 (sound paths), 17 (the owner's answers), 19 (as built); radio rows D-* in `plus-radio-test.md` | 888,540 (0) | 209,648 (0) | 1,808 | 216 / 760 |
| 7b | Review: the stack check names the storage job and the session's transport calls (`plus_dota.STACK_EDGES`; the job's chain 1,988 B on the start task, 376 B free beyond the margin); singular / plural result texts with the counts where the language puts them; `radio_dota.inc` includes `hal_time.h`; host tests `test_dota_fm.c` (AFSK through audio, CTCSS, noise) and `test_dota_radio.inc` (the radio core) | 888,652 (+112) | 209,648 (0) | 1,808 | 196 / 760 |
| rebase | onto `ddm25` 3032f91 (beta.2 prep, README, CONFIG_HOME_FREQ: base now 864,380 B / 209,672 B, UI stack 268 free); DOTA's share unchanged (+24,616 B flash, -16 B RAM). DOTA's screens are lists and use none of Home's layout fields (`freq_y`). Free flash: 12,124 B | 888,996 | 209,656 | 1,800 | 196 / 760 |
| ux | Pairing without typing (owner): the frequency source row (Default 433.450 / Current channel / Last used / Manual), the tone from a short list, DMR CC 1 / TS 1 fixed and hidden. Public build (the owner's radio now): 810,388 B (+164), RAM 209,616 B (1,840 free); owner build below | 889,160 (+164) | 209,664 | 1,792 | 200 / 760 |

## Name and public build

The product's name on the radio is **DDM32 Plus**. The internal names stay (`CONFIG_*`, `--plus`, `prepare_*`, file
names, `dm32-plus-…` version string, `docs/plus*.md`).

| Where | What |
|---|---|
| Splash (`CONFIG_PLUS_ART`) | wordmark "DDM32 Plus" (`art/wordmark.txt`, 79x12, 158 px wide at 2x); under board and version, on the bottom line, **TA2DOU** in the tiny font, centred, in the mark's teal (mono: fg) |
| Settings > About | first line "DDM32 Plus · d0ukan · TA2DOU", then "Based on Open25-DM32 Classic" in place of Classic r13's "Firmware: Open25-DM32 Classic" and its issue tracker lines (a Plus image is not Classic; its bugs are not upstream's); "DDM32 Plus: GPL-3.0-or-later" right above upstream's "License: MIT, (c) 2026 Open25-DM32 contributors"; upstream's other lines unchanged ("Based on RadioFoundry", its MIT line, its authors and third-party list); with the repeater list, "Repeater data: RadioID.net, TRAC" ("sample" without a download) at the end |

Names and callsign live in `art.h` (`ART_PRODUCT`, `ART_AUTHOR`, `ART_CALLSIGN`) and are not translated; "Repeater
data: " is (Turkish "Röle verisi: "). Cost on the r13 base: +276 B flash, 0 B RAM (About's buffer is Classic r13's
688 B with the ID database; its text with every line, a 5-digit ID count and a 12-character release name: English
518 B, Turkish 540 B, the terminator included; on the r7 base it was +352 B flash, +16 B RAM). `tests/plus/test_art.c`
draws the whole boot screen (`splash_plus.c` on stand-ins,
`tests/plus/art_host_ui.h`) on 240x320, 160x128 and mono: callsign on the bottom line, centred within 1 px, inside
the panel, nothing else on its line, the footer and the art above it.

**Public build**: `python3 tools/build_radiofoundry.py --plus --public --stack-check --verify-rebuild` →
`build/dm32-p25-plus-public/` (logs `*-plus-public.log`; the owner's `dm32-p25-plus/` is never touched). The same
firmware with `CONFIG_ID_DB=n`: nothing generated from `research-data/user.csv` (RadioID.net users) is compiled, and
About has no "ID database" line. The repeater list ships (RadioID.net repeaters + the owner's TRAC list). Guards:
`--public` needs `--plus` and refuses `--feature ID_DB=…`; `plus_brand.public_command` refuses a make command whose
last `CONFIG_ID_DB` is not `n`; after the link `plus_brand.public_audit` refuses an ELF holding any `iddb_*` symbol
(manifest: `public_enabled`, `audits.public_id_db_absent`). `make test` runs `tools/plus_brand.py --self-test`:
the refusals on made-up inputs, and the tree's public ELF when there is one. 2026-11-19: owner build 833,956 B flash,
210,152 B RAM; public 755,184 B flash, 210,104 B RAM (the 78 KB table and its About line gone). 2026-11-20 on the
r13 base: owner build 841,040 B flash, 210,584 B RAM (872 B free), bin sha256 `9d1daa59…58ce`; public 762,264 B
flash, 210,536 B RAM, bin sha256 `ff6b2606…68cd`; both zero warnings, forced rebuild identical, stack check ok.

**Codec and private data** (checked 2026-11-19 on both r7-based images, read only; the r13 numbers below): the build never opens
`research/originals` (only `--with-local-stock` would, and Plus never uses it; manifest `codec_slot`: "reserved; no
stock AMBE merged") nor `backups/`. The linker's FLASH region ends at `__codec_slot` (0x030E8000), the image ends at
0x030D79A4 (public 0x030C45F0), and `tools/install_plus.py` writes only sectors below 0xE8000; the codec in the slot is
the radio's own, put there by the Firmware Utility. Byte search: every 32-byte window of the stock codec code (047
and 049, 83,456 B) at every 4 bytes: none in either image; of its tables (12,152 B): 57 windows, all one 256-byte
stretch that equals our IMBE codec's `cos_table` (open-source `imbe_vocoder` `math_sub.cc`, a Q15 cosine table both
carry); every 32-byte window of the backup's data chip (calibration, codeplug): none. On the r13 base (2026-11-20, map
and size only, the byte search not repeated): `__codec_slot` still 0x030E8000, the image ends at 0x030D9550 (public
0x030C6198), `__codec_*` symbols as `tools/install_plus.py` expects.

## Round 14 APRS job (branch `r14-aprs`, ROADMAP rows 47 and 49)

Owner items of 2026-11-20 (feasibility and costs in `docs/r14.md` on branch `r14`), one commit each, on top of item
48 (`r14-48`, cherry-picked). Patches in `tools/plus_aprs.py` (called last from `plus_features.prepare`), host tests
in `tools/test_aprsx.py` (run by `tools/test_classic.py`), Turkish in `tr.po`'s last block.

**Item 47, Mic-E beacons (`CONFIG_APRS_MICE`, needs `APRS_MSG`).** Settings gets an **APRS** group (the owner chose
Settings over Tools > APRS; it is the 11th group, after Advanced and without a digit, so upstream's flows that open
the others by their digits stay as they are): "Beacon format" Standard / Mic-E and, only while Mic-E is chosen,
"Mic-E message" Off duty / En route / In service / Returning / Committed / Special / Priority (M0-M6; Turkish Görev
dışı, Yolda, Hizmette, Dönüyor, Görevde, Özel, Öncelikli). Turning Mic-E on starts at En route (the owner's default)
or the message chosen before. Emergency is not offered: it stays the SOS's (`aprs_sos_set`). The engine
(`build_beacon` in `aprs.c`) builds the beacon with the existing encoder `aprs_mice_build` (the SOS's) from the same
position, speed, course, altitude, symbol and codeplug comment, and queues it to the Mic-E destination
(`queue_frame_to`); every beacon that goes through `build_beacon` follows the setting: "Send position now", the
interval, SmartBeaconing, after PTT, boot, distance and the ISS digipeater shot. The APRS-IS copy (`own_to_is`) gets
the same destination ("CALL>TY0S5P,TCPIP*:`..."). Messages, acks, status and DMR positions are unchanged. Stored in
three spare bits the older firmware masks off (`settings_aprs_mice`: `aprs_flags2` bits 6-7, `aprs_misc` bit 7; 0 =
standard, 1..7 = M0..M6), taken and saved with the other APRS settings (`aprs_set_cfg`, which turns anything past M6
into the standard format). The Settings rows read and write through two light engine calls (`aprs_mice`,
`aprs_set_mice`: no copy of the configuration on the UI task's stack; going through `ui_svc_aprs_set_cfg` first cost
156 B of the UI stack's 196 B in the stack check). Position ambiguity does not apply to Mic-E beacons. Cost (public
build, on item 48): +824 B flash, +16 B static RAM (the beacon's destination, the configuration byte); stacks
unchanged (UI 196 B free).

Tests: `tests/plus/test_aprs_mice.c` runs upstream's `aprsRadioTest` with it built in (all pass: the standard format
stays the default), then 1,610 encoder round trips through the engine's own parser (every message M0-M6 over latitudes
and longitudes in all four quadrants and every band of the Mic-E longitude code, the poles and 179 59.99, speeds
0-799 kn with the shifted codes and >799, courses 1-360 and unknown, altitudes, comments: destination and information
field decode back), the beacons on the air through the fake radio decoded back (manual for each message, automatic
every 60 s, SmartBeaconing from moving host NMEA with speed and course, the APRS-IS line), and the setting stored and
read back with the neighbouring APRS bits intact. UI: Settings > APRS in English and Turkish with the layout check.

**Item 49, the APRS station page (`CONFIG_APRS_STATION`, needs `APRS_RX`, `MAP` and `NAV_FOLLOW`).** Tools > APRS >
Stations heard: OK opens the station's page (`integration/radiofoundry/aprsx/ui_aprsx.c`, `SCR_APRS_STATION`; the
list's hint "OK details  Back"; before, OK started Go-To, which is now one key on the page). Lines, cut to the
240 px screen: the callsign (medium font); the symbol by name and its two characters with the distance and direction
from our GPS position ("Car /> 2.8 km NE"; "No GPS fix" / "No position heard" otherwise); the position in degrees
and the Maidenhead locator (`loc_encode`, CONFIG_LOCATOR); altitude, speed in km/h and heading as a compass point when
the station sent them; its comment without the "/A=" altitude (`mapnav_comment`); "Heard: 3m ago" and "Direct
(or Via digi), 5 packets"; under a line, the message we sent it last ("> QSL (delivered)": the composer's addressee
and text, the engine's sending / delivered / not delivered) and its messages in our inbox ("< text", unread ones in
the warning colour, newest first, as many as fit; another station's are not shown), or "No messages with it".
Symbol names (Turkish in `tr.po`): APRS101 appendix 2's common primary-table symbols (police, digipeater, gateway,
aircraft, house, motorcycle, car, balloon, camper, bus, helicopter, boat, person, weather, ambulance, bicycle, fire
truck, hospital, jeep, truck, laptop, repeater, van); on the alternate table or with an overlay only digipeater,
gateway, house, car and weather keep their name; others show the two characters only. Keys: OK Go-To follows the
station (`ui_follow_start`, as the list's OK did; Back returns to the page); 0 opens Tools > Map centred on it at zoom
6 or closer with it picked and its details under the map (`ui_map_show_station` in `ui_map.c`; Back steps out of the
details, the pick, then the map); # the composer to it (CONFIG_APRS_MSG); Up / Down the station before / after it in
the list (round). The page names its station by the callsign's hash (2 B) and looks it up in the engine's list at each
draw (no copy kept); one that drops out of the list shows "Not heard any more" and its keys do nothing. Drawn again
once a second (`ui_aprsx_tick` in `ui_app.c`) so the age moves on. Its working copies (the station, one inbox
message, two 48-character lines) live in the tool screens' shared buffer (`ui_scratch.h`, `UI_SCRATCH_APRSX`; it is
larger already), so the page costs 7 B of static RAM (the hash, the tick, the map's centring flag) and nothing of
the UI stack beyond what the stack check already counted. Cost (public build, on item 47): +3,124 B flash (the page,
its texts and the 23 symbol names in two languages, the map's centring; above the 1.5-2.5 KB estimate), +8 B static
RAM; stacks unchanged (UI 196 B free). The map also keeps other labels off the picked station's name now (the layout
check found a district name under it at zoom 6). Not done from the design: the
"how often" figure (the list keeps only the packet count) and the station's own Mic-E status (not kept in the list).

**The empty SOS page (after item 48).** With no action chosen, the SOS key's page read "No SOS actions chosen" in the
medium font (252 px, wider than the 240 px screen; Turkish 240 px) over "Tools > SOS key picks them", stale since the
actions moved to Settings > Keys > SOS key. Now "No SOS actions" / "SOS eylemi yok" over "Pick: Settings > Keys" /
"Seç: Ayarlar > Tuşlar", and the toast on OK "No SOS actions: Settings > Keys"; `test_ui_aprsx` (English and Turkish)
and the round 13 flow check its layout.

**Found, not fixed (upstream).** A host test that feeds the same `$GPRMC...\r\n` sentence through
`aprs_host_feed` (the KISS / NMEA host port) every few seconds sees the host position go stale after a few
sentences: later lines are no longer taken as NMEA. Likely the `\n` after the `\r` that ended a sentence goes to the
KISS decoder, which then holds a byte, and a `$` is only taken as NMEA while that decoder is empty
(`aprs_host.c`, `aprs_host_feed`); not confirmed. A host program that sends CR LF NMEA through the KISS
port (APRS position from the PC) may be hit. `tests/plus/test_aprs_mice.c` feeds `aprs_nmea` directly instead.

## Round 14: air band and spectrum (branch `r14-air`)

ROADMAP rows 44 and 45, the owner's decisions in `docs/r14.md` (branch `r14`). Hooks in `tools/plus_air.py`, host
tests in `tools/test_air.py` (run from `tools/test_classic.py`). Budget per commit, both profiles
(`--stack-check --verify-rebuild`: zero warnings, forced rebuild identical, every task inside its stack):

| Step | What | Public FLASH B | Public RAM B (free) | Owner FLASH B | Owner RAM B (free) | Stacks: UI / radio free |
|---|---|---:|---:|---:|---:|---|
| base | `ddm25` 22b4049 | 810,224 | 209,608 (1,848) | 888,996 | 209,656 (1,800) | 196 / 760 |
| 44a | the airfield table (67 rows out: -484 B of table), "+N", the short "No frequencies" | 809,912 (-312) | 209,608 (1,848) | 888,684 (-312) | 209,656 (1,800) | 196 / 760 |
| 44b | air watch: the UI (`ui_airwatch.inc`, `mw_keep` 54 B), the radio core's two requests and flag, # on the airfield page, 3 Turkish entries | 811,712 (+1,800) | 209,664 (1,792) | 890,484 (+1,800) | 209,712 (1,744) | 196 / 760 |
| 45 | 3D waterfall (`wf3d.c`, the third view on *), 1 Turkish entry; multi-watch rows in the air band say "AM" | 812,500 (+788) | 209,664 (1,792) | 891,272 (+788) | 209,712 (1,744) | 196 / 760 |
| rebase | onto `ddm25` 42a0ff4 (dota-ux, r14-aprs: base 814,356 / 209,640 B public, 893,132 / 209,688 B owner, UI 200 B free); this branch's share | 816,636 (+2,280) | 209,696 (1,760) | 895,408 (+2,276) | 209,744 (1,712) | 200 / 760 |

The three items together: FLASH +2,276 B, static RAM +56 B, no stack path deeper (UI 196 B, radio 760 B free beyond the margin). Public build: 88,620 B flash free, 1,792 B RAM free (rule >= 1,500 B: kept). **Owner build: 9,848 B flash free, under the owner's >= 11,500 B rule** (the base had 624 B above it; docs/r14.md: every item but 48 needs the reclaim of owner question Q1 there), 1,744 B RAM free.
After the rebase onto 42a0ff4: public 84,484 B flash and **1,760 B RAM free**; owner 5,712 B flash free (the owner runs the public build; the ID database moves to a data-chip pack), 1,712 B RAM free.

### 44a. The airfield table (owner: "D", 2026-11-20)

- **Only removed, nothing added**: the entries of the pinned OurAirports data that are not in that aerodrome's own
  official publication (its AD 2.18 table), compared by the owner's research outside this repository. The
  publication is copyrighted: no frequency was added, changed or given another service from it. `tools/airfields.py`
  `EXCLUDE`: 61 entries (code, channel name, reason), applied by code and channel name whatever the type (LTAJ's
  "TWR/APP 120.100" goes as both rows). 58 are "not in the aerodrome's official publication" (among them 119.100 at
  LTAD Etimesgut and LTAE Akıncı: Esenboğa approach's); TR-0018 Bolu's 122.100 and 123.300 are unofficial (no published
  aerodrome); TR-0040 Manavgat's 128.025 is Antalya approach's. The list must match the pinned data: an exclusion that
  matches nothing fails the run (a refresh reviews it), as `--check` does for the table. Result: **74 airfields, 122
  frequencies, 3,744 B** (was 79 / 188 / 4,228; 67 rows removed, the duplicate counted before was one of them). Five
  small airfields are no longer listed: the selection keeps a small field only with a frequency (LTBK Gaziemir, LTBP
  Yalova, LTBV Bodrum-İmsık, TR-0018 Bolu, TR-0040 Manavgat); 18 large / medium ones now say "No frequencies in the
  data" (13 before).
- **Every frequency of an airfield**: `FREQ_MAX` 10 -> 64 (`air_field_t.count` is a `uint8_t`; the generator refuses
  more than 255; the 12 KB byte cap stays). Today's most is LTAI Antalya with 7. The details page scrolls (its rows
  follow the picked one) and Up / Down wrap; the list line is unchanged in height.
- **"+N" on the list line** when the service shown (the tower, else the first frequency) has more frequencies:
  "TWR 125.000 +1" at LTAI, none at LTAB's one tower. No new text.
- **Fixed on the way**: an airfield's page without frequencies drew "No frequencies in the data" wider than the
  panel (the layout check found it once TR-0004, without frequencies, became the last airfield by code); it now
  has the shorter alternate "No frequencies" ("Frekans yok"); the toast keeps the long text. One Turkish entry.
- Tests: `tools/test_air.py` (the list well formed: 61 entries, codes as the table has them, channel names of the band,
  once each, a reason each; applied to a synthetic pair: removed whatever the type, counted, a small field left without
  a frequency dropped, an unmatched one reported, bad entries refused; none of them in the committed table);
  `tests/plus/test_airfield.c` (the shipped table: 74 / 122); `tests/plus/test_ui_misc.c` (its airfield flows on the
  new table: LTAB GND 120.200 tuned, LTFJ's DEL first, TR-0004 last by code, Antalya "TWR 125.000 +1", LTAB without
  "+"); `tests/plus/test_ui_air.c` (English and Turkish: "+1", Antalya's seven frequencies scrolled to the last and
  tuned, Etimesgut without 119.100, Kastamonu without frequencies, layout). Radio rows AW1-AW2.

### 44b. Air watch (`CONFIG_AIR_WATCH`; needs MULTI_WATCH, VFO_MODES, AIRFIELDS)

- **Use**: Tools > Airfields, OK on an airfield, **#** ("OK listen  # watch all"; Turkish "OK dinle  # tümünü izle"):
  **all** its frequencies go on the multi-watch ring, letters A..F, **each letter its own VFO** (VFOs 0..5): the
  services in the airfield's list order, then **the ATIS last**, a channel name used by two services once (LTAH's TWR /
  APP 122.100 is one). Toast "LTAI: Air watch on", Home shows the ring's rows ("VFO 121.9000" ...). **More than six**
  (Antalya: ATIS, GND, TWR x2, APP x3 = seven): the first six of that order (so the ATIS is the one left over) and the
  toast says so: "LTAI: 6/7 frequencies watched" (Turkish "LTAI: 6/7 frekans izleniyor"). **The ATIS letter** (marked
  for the radio core: RREQ_AIR_VFO's arg | `RADIO_AIR_ATIS`) holds the ring **5 s at most** (`AIR_ATIS_HOLD_MS`, from
  the first poll of the hold, through the wait and its looks), then the ring goes on and every ATIS letter is
  **passed over for 60 s** (`AIR_ATIS_COOL_MS`: nothing heard there, the ring's 50 ms quiet stop): an ATIS recording
  never stops, so held like any signal it would keep the one receiver there for good. Any other letter is held as
  always (a long tower exchange is heard to its end). The VFOs are FM
  VFOs in the air band, so each letter is **received in AM** (`radio_am_channel`), its squelch on the signal strength
  (`radio_am_noise`), and **never transmits** (TXDENY_AM). The ring is multi-watch's own: the one receiver hops
  (150 ms a quiet letter: 0.9 s round six), a signal holds it on its letter, the Dual watch delay's wait looks at the
  others and the ring goes on. Two frequencies make dual watch's pair of two VFOs (LTAB: GND 120.200, TWR 132.000);
  one is tuned as OK does. Refused while transmitting ("Can't tune while transmitting"), without AM receive, outside
  the radio's bands; an airfield without frequencies says so. Only from the airfield's page: the typed list in Tools >
  Air band (docs/r14.md 44a, owner question 3) is not built.
- **Leaving** is how any watch stops: Back twice, # 5 Stop, * 7, the watch key. Toast "Air watch off" and **everything
  as before**: the radio core brings VFOs A / B and the position back from the settings as its start does
  (`radio_sm_init`'s way), then the user's watch is asked for again as it was kept (off; or dual watch / the ring with
  its letters, selected letter and pane 1's letter). Keys used during it (Up / Down on a letter, Change C, the VFO key)
  change the air ring only and go with it. **Nothing of it is saved**: the UI's `state_save` / `mw_save` and the radio
  core's `persist` return while it runs, so a power off during it comes back exactly as before it. Not kept: the
  contents of C..F's own VFOs (RAM only; emptied as a power off empties them), so a user ring with a C..F VFO letter
  gets that letter back as a fresh VFO. A Watch channels change made during it applies after it.
- **How**: the UI (`integration/radiofoundry/air/ui_airwatch.inc`, included in `ui_dual.c` after `ui_mwatch.inc`)
  keeps the user's watch (`mw_keep`, 54 B; the airfield page's two toast buffers became one), asks the radio core for each letter's VFO (`ui_svc_air_vfo` ->
  `radio_req_air_vfo`, RREQ_AIR_VFO: arg the VFO, arg2 Hz; the queue keeps their order before the ring's RREQ_MWATCH),
  then starts the ring through multi-watch's own `apply`; `ui_dual_tick` finds the watch off and ends it
  (`ui_svc_air_end` -> RREQ_AIR_END). The radio core (`radio_airwatch.inc`): the first RREQ_AIR_VFO stops a scan or the
  user's watch (back on its selected channel), notes the position and VFOs A / B (`persist`) and sets `sm.air`; each
  VFO becomes a fresh VFO at the frequency (the one in use, the other of A / B, a parked one, or a C..F slot); RREQ_AIR_END
  stops the ring, reloads VFOs A / B from the settings (`restore_vfo` + `vfox_load`), empties C..F, and selects the
  saved position (`select_position`, then the VFO or the channel tuned). Requests while transmitting or in hotspot mode
  are refused, as RREQ_MWATCH is. The ATIS limit (`air_atis_poll`, called by `scan_service` before the scan engine):
  `sm.air_atis` (a bit per VFO, set by RREQ_AIR_VFO, cleared at the end), `sm.air_held_ms` (the hold's start on an
  ATIS letter; any other stop or a new one starts it over) and `sm.air_cool_ms` (passed over until): 12 B with the
  padding. The mark follows the letter's VFO for the watch.
- **Bug fixed after beta.6** (owner: Esenboğa shows four frequencies, # watched only three, A..C): no ring limit, the
  ATIS left out on purpose (a documented deviation from "all", made before the 5 s limit existed). Fixed as above: Esenboğa's four on
  A..D (GND, TWR, APP, the ATIS on D). Cost: FLASH +440 B (841,104), RAM +16 B (209,584 of 211,456: 1,872 free).
- **Bug fixed after the beta.7 candidate** (owner: Esenboğa's four on A..D, but "received in FM, not AM"). The radio
  core was already right: every letter is tuned through `tune()`, which puts an FM VFO in the air band on the board's
  AM demodulator (`rx_am`) with the AM squelch; the host test now models the DM-32's demodulator (rx() plays FM,
  rx_am() AM, refused outside FM mode) and checks it on **every** stop, while a letter holds a call, around a refused
  PTT and back in FM after the end. What said FM: (1) **Home's status bar** in any dual watch / ring read the selected
  pane's mode as `DMR ? "DMR" : "FM"` (upstream `ui_screens.c`), so the air watch showed "FM DUAL" over its AM rows;
  (2) a **VFO pane** of dual watch's pair read "VFO FM" (an airfield with two frequencies). Both now follow the radio
  core's rule (`dual_pane_am`: an FM pane in the air band on a radio that receives AM says AM; `tools/plus_air.py`).
  (3) A real FM stop: with a **priority channel** set (Settings, CONFIG_SCAN_PRIO) the ring added a stop on it every
  2 s and held it on a signal, so the user's FM channel was heard in the middle of the air watch. The air watch's ring
  never takes priority stops or looks now (`scx_kind_ok` while `sm.air`, `radio_scanprio.inc`); the user's watch and
  scans keep them. Cost: FLASH +116 B (844,892), RAM +0 B (209,608 of 211,456: 1,848 free).
- Tests: `tests/plus/test_airwatch_radio.inc` (run by `tools/test_air.py` after upstream's radio suite, the Classic /
  Plus P25 suites, the multi-watch ring and every mode in the VFO again with it built in): four air frequencies on the
  letters' VFOs and the ring over them, each tuned on its own frequency in AM through the board's AM receiver, TX
  refused; a signal on one letter holds the ring there (2 s) and the ring moves on after; nothing noted while it runs;
  the end back on the channel and VFO A as before, C..F empty; a power off during it back as before; the user's ring
  stopped on its selected channel and noted there; an ATIS letter (Esenboğa's four, the ATIS on D on the air for
  good: held 5 s, then passed over 20 s with A..C visited and D's stops quiet ones, a tower call on A held its whole
  8 s, D held again after the 60 s; the end forgets the mark); refusals (TX, off the band, a seventh VFO, an end
  without it).
  `tests/plus/test_ui_air.c` (English and Turkish, the radio core's side faked in `ui_sim_air.c` on the simulator's
  VFOs): Antalya's seven: the first six on A..F (each VFO's frequency, the receiver on A, E on its own), the toast
  "LTAI: 6/7 frequencies watched", no ATIS among them; Esenboğa's four on A..D, the ATIS on D marked, Home's four
  rows (owner's beta.6 case); Back twice: the
  radio back on its channel, the watch off, the settings untouched throughout; the user's ring of four with C
  selected kept and back with # 5 Stop; two frequencies the pair; one tuned; a channel name on two services once;
  refusals; a power off during it doesn't bring it back; layout of the airfield page and Home. `test_ui_airprov.c`
  (Antalya from its province: 6/7) and `test_ui_datapack.c` (the pack's five for LTAB, its ATIS on E). Radio rows
  AW3-AW9.

### 45. 3D waterfall (`CONFIG_SPECTRUM_WF3D`; needs SPECTRUM_WF)

- **Use**: Tools > Spectrum, **\*** cycles **waterfall > 3D > bars alone > waterfall** (owner: a third view, the flat
  waterfall stays; RAM only, the waterfall at power on, the view chosen kept for the next entry). Toasts "3D
  waterfall" ("3B şelale"), "Waterfall off", "Waterfall on"; without heap room "Waterfall: no room" as before. The 3D
  view takes 13 / 20 of the area under the header (the flat one 11 / 20) and the bars stay above it.
- **Picture** (`integration/radiofoundry/toolbox/wf3d.{c,h}`, installed as `core/ui/wf3d.*`): the waterfall's newest
  24 rows as a ridge plot, the newest in front at the bottom and full width (its points exactly under the bars),
  each older row further back: scaled by 16 / (16 + depth) towards the centre and raised towards a vanishing line
  above the box, placed so the last row's highest peak just reaches the top. A point's level (wf_level: 3 dB a step
  above the sweep's floor) lifts its row by up to 9 / 20 of the box at the front. Back to front (painter's order):
  each row first clears the area under its ridge down to its own base, which hides the rows behind there, then draws
  the ridge, each segment in the colour slot of its higher level (the waterfall's eight slots; slot 0, the background,
  is drawn in slot 1's colour so a quiet row still shows as a line). It "flows" one row a sweep, as the waterfall does.
- **RAM 0**: it draws from the waterfall's own heap rows (`wf_t.buf`, 1,280 B, only while the screen is open), keeps
  nothing (every point worked out as it is drawn), and its locals are a few integers; the colour table (16 B) is on the
  stack of `wf3d_view` only while it draws. CPU: about two 1-pixel `gfx_vline` calls a column a row (24 x ~190
  columns), once a redraw.
- Tests: `tests/plus/test_wf3d.c` (into a test frame buffer, ASan / UBSan): nothing without a buffer or rows; nothing
  outside the box; a quiet front row is the bottom line in slot 1's colour from the first point to the last, at the
  bars' x; a level-15 peak lifts it by the amplitude in the TX slot's colour, a level-8 one by 8 / 15 in slot 4's; the
  area under a peak cleared; 24 rows each a line, higher and narrower going back (the last under 3 / 5 of the
  width); a wall in front hides row 1's line and not the last row's above it; 40 rows kept draw exactly as their
  newest 24; every row a wall: the last row's peaks on the box's top row (within 2 px), none above; a narrow (1 px
  bars) and a too-short box. `tests/plus/test_ui_wf3d.c` (English and Turkish, `ui_sim_toolbox.c`): * from the
  waterfall to 3D keeps the same heap block (no second allocation), draws the strong point's ridge in the TX colour
  low in the box (the newest in front; the waterfall's newest is on top), new sweeps drawn; * to the bars gives the block back and draws no rows; * to
  the waterfall takes it again; Back gives it back and 3D comes back on the next entry; without heap: "No room" and
  the toasts; layout of each view. The existing waterfall flows (`test_ui_toolbox.c`) run as before in their own build
  without the switch. Radio rows WF3-1 - WF3-3.
- **Also in this commit (item 44b's Home)**: a multi-watch row of an FM channel or VFO in the air band now says "AM"
  ("AM 124.425") where it said "FM": it is received in AM (`ui_mwatch_draw.inc`; the air watch's rows are all such).

## User data packs (`CONFIG_DATAPACK`, ours; branch `datapack`, round 14)

Data the user builds from sources they download themselves (RadioID.net's user list; the aerodromes' own
publications), kept on the data chip in **0xE70000-0xEFFFFF**, the one region AGENTS.md rule 2 opens to the firmware
(owner, 2026-11-20; checked erased in the owner's backups). Design: `docs/r14.md` section 7. Code:
`integration/radiofoundry/datapack/`, hooks in `tools/plus_datapack.py`, host tests in `tools/test_datapack.py`.

- Store (`core/storage/datapack.c`): per type two slots (RadioID 2 x 256 KB at 0xE70000 / 0xEB0000, air band 2 x 16 KB
  at 0xEF0000 / 0xEF4000); a slot is a 64-byte header (`DPK1`, type, format, sequence, length, payload CRC-32, date,
  name, builder tag, header CRC-32) and the payload. Written into the slot not in force: its sectors erased, the
  payload programmed (any order), read back against its CRC, the header programmed last. The higher valid sequence is
  in force; checked at boot (`dpk_scan` after `storage_init`). Every erase / program goes through `dpk_erase` /
  `dpk_program`, which refuse anything outside the region. RAM: 2 x 40 B (what is kept of the pack in force).

| Step | What | Public FLASH B | Public RAM B (free) | Owner FLASH B |
|---|---|---:|---:|---:|
| base | `ddm25` da7577c | 810,388 | 209,616 (1,840) | 889,160 |
| 1 | the store, boot scan | 810,928 (+540) | 209,696 (1,760) | 889,700 |
| 2-3 | RadioID from the pack (a caller's name: the codeplug contact, then the pack, then the built-in table, then the CPS-uploaded database; binary search on the chip, no RAM copy); Settings > About: "RadioID pack: RadioID TR, 2026-11-17, 98 KB" / "none" / "damaged, not used", the same for the air band pack; the airfield screens ask `air_count` / `air_freq` (`core/ui/air_pack.c`): a pack that names an airfield replaces its frequency list, the table answers for the others | 812,204 (+1,276) | 209,824 (1,632) | 890,964 |

- RadioID pack payload (format 1): an 8-byte header (count, record size, text characters) and records sorted by ID:
  3 bytes LE, then 16 characters 6 bits each ("TA1ABC Ahmet"), 15 bytes a record (Turkey's ~8,700 IDs: ~130 KB).
- Air band pack payload (format 1): a 4-byte header (count), per airfield its code (8 bytes), its frequency count and
  the frequencies as `air_freq_t` (channel 2 bytes, type 1). Airfields the table lacks are not added (format 1:
  frequencies only).
| 4 | Loading from the PC: the CPS session's close (`C` 5) checks the slots again; the builder `tools/datapack.py` (RadioID pack from the user's radioid.net `user.csv`, air band pack from the aerodromes' AD 2.18 pages the user downloads, `info`) | 812,208 (+4) | 209,824 (1,632) | 890,968 |
| rebase | onto `main` 42a0ff4 (r14-aprs merged): base public 814,356 B, RAM 209,640 B (1,816 free) | | | |
| 5a | **The built-in ID database leaves program flash in every profile** (owner, 2026-11-21: the RadioID pack replaces it; `CONFIG_ID_DB=n` in `build_radiofoundry.py`, `tools/iddb.py` stays as the pack builder's source). RAM: the kept pack state cut to 8 bytes a type (the rest of the header is read from the chip when About draws), and **Settings > About's text is built in the heap while the page draws** instead of a 752-byte static buffer ("No room" if the heap has none) | 816,232 (+1,876 over main) | 209,040 (**2,416**: +600 over main) | 816,232: the same image |
| rebase | onto `main` 16a9f46 (r14-air merged: base public 816,636 B, RAM 209,696 B, 1,760 free). The airfield screens take both sides: r14-air's "+N", every frequency (up to 64) and the air watch (#) all ask `air_count` / `air_freq`, so an air band pack's list is what the line, the details, OK and the watch use (the ATIS left out of the watch as before); "+N" keeps the shown service's type, not the pack's one copy `air_freq` hands back. `air_pack.c` also stops at an airfield whose list would run past the payload (a pack that checks but is malformed is never read beyond its end) | 818,500 (+1,864 over main) | 209,096 (**2,360**: +600 over main) | 818,500: the same image |
| UI | simulator flows with packs in force (`tests/plus/test_ui_datapack.c`, English and Turkish, layout-checked): About with a RadioID pack ("RadioID pack: RadioID TR, 2026-11-20, 1 KB", "Air band pack: none") and with a damaged air band pack ("damaged, not used", the table again); Tools > Airfields with an air band pack naming LTAB ("TWR 132.000 +1", its five frequencies, OK on one only the pack has); the air watch on the pack's frequencies (four letters, its ATIS left out) | 818,500 (tests only) | 209,096 (2,360) | |
| 5b | **DOTA content type "data pack"** (`docs/dota.md` section 20): Tools > DOTA Transfer sends the RadioID or the air band pack in force to another radio; the receiver is asked "Data pack: RadioID TR, 2026-11-17, 98 KB. Accept?" (the sender's HELLOs give ~20 s), writes the slot not in force in storage jobs (`dpk_begin` / `dpk_put` / `dpk_commit`) and switches only when it is whole and checked; any failure leaves the old pack. HELLO carries the 64-byte header after the 21-byte manifest (older builds drop it: "No answer"); 120-byte chunks as channels records; the receiver keeps a 32-frame window instead of a bitmap (up to 2,240 frames). `dpk_describe` (About's text for any header); the slot scan takes its result in one go (another task never sees a half scan). Fixed on the way: a DMR receiver's refusal went to ID 0 | 820,796 (+2,296) | 209,072 (**2,384**: -24) | 820,796: the same image |

**Profiles after 5a.** `--plus` and `--plus --public` build the same image (the same bytes; only the output directory,
the manifest's "kind" text and the public build's audit that no ID database code is in the ELF differ). The public
profile still has to differ only by that audit and by refusing `--feature ID_DB=y`; a `--plus` developer build can
still turn the old table on with `--feature ID_DB=y` (not for release).

**Loading a pack from the PC (for the Windows programmer).** Through the firmware's own open-firmware CPS protocol
(`docs/spec/open-firmware-cps.md` 2), never our flash tools: flash area 1 takes physical addresses and the pack
region lies inside the window the CPS may write (0x720000-0xEFFFFF), so the CPS's journaled sector writes (prepare
sector, write, commit; power-cut safe) carry it.

1. Start the CPS session as for a codeplug write (`C` 0 ...).
2. Read both slots' first 64 bytes of the pack's type (area 1: RadioID 0xE70000 / 0xEB0000, air band 0xEF0000 /
   0xEF4000) and pick the slot **not** in force: `datapack.inactive_slot`; the new sequence is `datapack.next_seq`.
3. Build the slot image (`datapack.header(type, payload, date, name, seq)` + payload; `datapack.py` writes `.dpk`
   images with sequence 0: set it from step 2 and recompute the header CRC with `header()`).
4. Write it sector by sector in `datapack.cps_plan(slot, image)` order: the payload's sectors first, **the header's
   sector last** (its rewrite is the commit; until then the slot does not count, and the pack in force stays).
5. Close the session (`C` 5): the radio checks the slots and the new pack is in force (Settings > About shows it).

The data never comes from us: the user downloads RadioID.net's user list and the State's AIP pages (in Turkey the
DHMI eAIP, AD 2.18) themselves, and the programmer builds the pack on their PC. Nothing derived from them is ever in
this repository, a release or the firmware.

**Loading from another radio (DOTA, step 5b; `docs/dota.md` section 20).** Both radios: Tools > DOTA Transfer, the same
method and frequency (the defaults pair without typing). The sender: Role Send, What "RadioID pack" or "Air band
pack" (the row under it names the pack in force, greyed "none" without one), the receiver's code, Send. The receiver:
Role Receive, Ready as for channels (the import rows do not matter for a pack; "New zone" still wants a name, so pick
"Current zone"), then answer "Data pack: <name>, <date>, <size>. Accept?" with OK within about 20 s. Deviation from
`docs/r14.md` 7.6: the receiver has no "What: Data pack" of its own; it is asked about whatever pack a sender offers,
and a sender of channels is received as before. Nothing reaches the codeplug; the new pack replaces the one of its
type only when every byte arrived and checked, and About shows it at once.

## Last calls (`CONFIG_LAST_CALLS`, ours; branch `r14-xband`, ROADMAP item 43)

Tools > Last call is now a list of the **last 8 calls** heard, newest first, from every mode (DMR, P25, YSF, NXDN,
dPMR, and the FM caller IDs MDC1200 / FleetSync / DTMF) and from every watched channel: one row a call, "B DMR Joe"
(the multi-watch letter A-F, or dual watch's A / B; the mode; the caller's name, else its ID or callsign) and
"TG2862 2m" (the talkgroup, or the unit a private call went to; how long ago; the talkgroup gives way where the row is
short). Encrypted calls say so. Owner's decisions (docs/r14.md on `r14`, 2026-11-20): 8 calls; **no stored audio for
the new modes**; the P25 replay stays as it was for the latest P25 call: OK on the newest P25 call (shown "> TG101")
plays it again on the P25 channel in use, OK again stops it, the same toasts as before; OK on any other row says "No
audio kept for this call". Design and code:

- `integration/radiofoundry/lastcall/lastcall.{c,h}` (portable, installed as `core/ui/`): 8 entries of 16 bytes
  (source and destination 24 bits each, or a YSF callsign / DTMF string's first 8 characters at 6 bits each; the
  letter; private / encrypted / text flags; the UI clock's ms; NAC / RAN / colour code | slot; the mode). One entry a
  call: a call going on updates its entry (P25's source arrives with its first LDU1), the same station heard again
  after 5 s of quiet is a call of its own, a new call naming the same known source within those 5 s is the same call
  (a caller reported twice), one whose source is still unknown never is (a P25 reply on the same talkgroup).
- `ui_lastcall.c`: fed at exactly the heard list's hooks in `ui_app.c` (recent_add for a new call, the DMR and P25
  "call goes on" spots, the tick's view), so every mode Recent already logs arrives through one path; the caller's
  name is looked up in Recent when the row is drawn (no copy). The letter comes from `ui_dual_letter_here()` (added to
  `ui_dual.c`): the ring's `mw_tuned()` or dual watch's `tuned_pane()` on the view of that tick (the ring's own
  `mw_rx` follows a tick later, after Recent is fed).
- Reuse, not duplication: the heard list keeps a station once with its count; Recent keeps 20 stations with their
  names; this list keeps the last calls in order and points back to Recent for the names. The r14 study's shared
  multi-mode audio ring is not built (no audio for the new modes).

Cost (public build, `--plus --public --stack-check --verify-rebuild`): FLASH 810,224 -> 812,416 B (+2,192), static
RAM 209,608 -> 209,744 B (+136; **1,712 B free**); stacks unchanged (UI 196 B, radio 760 B free beyond the margin).
Owner build (`--plus`): 888,996 -> 891,192 B (+2,196; **9,928 B free: below the 11,500 B owner rule**, as docs/r14.md
foresaw for every r14 item without a reclaim), RAM 209,792 B (1,664 free). Both zero warnings, forced rebuild
identical.

Tests: `tests/plus/test_lastcall.c` (the list's rules: calls and continuations, the gap, the P25 source rule, stations
told apart by letter / mode / private / destination, the oldest dropped, texts with `*` and `#`, the 16-byte entry)
and `tests/plus/test_ui_lastcall.c` (12 flows, English and Turkish, through the real UI in upstream's simulator: a DMR
call through Recent's own feed, MDC / FleetSync / DTMF through `radio_fmid_logged`, P25 / YSF / NXDN / dPMR as
`call_log_update` hands them over, the oldest dropped; letters A-F on a six-channel ring and A / B in dual watch;
Tools > Last call; the P25 row's replay refused off a P25 channel, stopped while playing, not offered for an older or
encrypted P25 call; empty list; RAM only; the layout on every page), run by `tools/test_xband.py` from
`tools/test_classic.py`. Radio rows: `docs/plus-radio-test.md` "Round 14 xband".

## Cross-band store and forward (`CONFIG_XBAND`, ours; branch `r14-xband`, ROADMAP item 46)

**Real-time cross-band is impossible on this radio** (docs/r14.md 46: one FD6818 transceiver with one synthesizer,
both LNAs off while it transmits). Tools > Cross-band is therefore a store-and-forward relay between two frequencies
A and B: an over heard on one is recorded whole, then transmitted on the other (A to B and B to A); while nothing is
heard the receiver goes between A and B, a second on each (held while bursts are heard). **DMR only.**

| Mode | Can | Cannot |
|---|---|---|
| DMR | relay whole overs between two simplex frequencies in the amateur bands, the same colour code and slot on both: the over's AMBE+2 air frames are kept as heard (9 bytes a 20 ms frame) and sent again through the call stack's voice source, so the voice is bit for bit what was heard (no decode / encode); sent as a new call from our own DMR ID to the talkgroup (or unit) it was heard for; up to **37.98 s an over** (the memory: 1,899 frames); longer overs are cut with the alert tone and "!" on Home (the 3-minute rule is in the engine too, but the memory always cuts first) | keep the caller's own ID (we transmit, so the call is ours), relay encrypted calls (no clear voice), hear the other side while it sends, catch the start of an over that begins while it listens on the other frequency (up to about a second can be lost: the dwell, then DMR late entry), different colour codes or slots on A and B (one pair of settings) |
| FM | - | **not built.** As PCM an over needs 16 KB/s: the memory holds 1 s. Through the AMBE codec (the r14 study's idea) it needs the owner's stock codec to encode FM audio, which no host test can run (the owner's condition was "only if it works in host tests"), and the FM audio-to-encoder path has never been measured. Through our own IMBE codec it would need the P25 worker's codec context and FIFO, which are the very memory the recording uses |
| P25 | - | **not built.** Its frames come through the P25 worker, whose buffers are the recording's memory (both cannot run at once); the worker has no path to transmit stored IMBE frames (its transmitter encodes the microphone live), and without the worker's buffers only the call replay's 4.4 KB (8 s) would be left |

Owner's rules (2026-11-20) and how they are kept:

- **Attended only**: the question "Attended only: stay at the radio while it relays. Start?" before every start.
- **The screen is locked; only the stop key works**: while a session runs every key but Back is refused with "Cross-band
  on: Back stops" (`ui_xband_lock`, first in `ui_app_key`: the emergency / SOS key too); Back stops it at once (off the
  air at once, `dmr_end`). The radio core drops every request but beeps, settings, the volume and power off; PTT gets
  the deny tone and never keys. DOTA cannot take the radio meanwhile (`radio_dota.inc`), nor APRS beacons, nor a
  codeplug reload.
- **A clear CROSS mark**: "CROSS" in the status strip of every screen (warn colour), and Home's bottom line says what the
  relay does ("Listen A", "Record A 0:12", "Send B 0:12", "CW ID", "!" when cut) whatever Settings says of hints.
- **Refused frequencies** (`xband_check_hz`, IARU Region 1 band plan): outside 144-146 / 430-440 MHz; PMR446
  446.000-446.200 MHz (its own refusal: no repeaters there, the radio is not type-approved); the repeater segments
  (2 m inputs 145.000-145.1875 and outputs 145.600-145.7875, 70 cm inputs 431.050-431.825 and outputs 438.650-439.425,
  each with half a 12.5 kHz channel around it); the satellite segments (145.800-146.000, 435-438 MHz); APRS 144.800;
  A = B; any input or output of the repeater list (Tools > Repeaters' table); a channel whose RX and TX differ when
  picked from a zone; frequencies the radio cannot transmit on.
- **CW ID every 10 minutes**: the callsign (the radio name, `radio_cwid_callsign`: refused to start without one) in
  Morse on each frequency transmitted on since the last ID: after the first over, then again once 10 minutes have passed
  and there was traffic (as the SFR repeater's ID); the CW beacon's keyer (FM carrier, the settings' speed and tone).
- **At most 3 minutes an over**: cut at 9,000 frames or at the memory's end, whichever comes first (here the memory).

Design: `integration/radiofoundry/xband/` - `xband.c` (portable: the rules, the state machine LISTEN / RECORD / SEND /
ID, the frame store over up to three memory pieces), `radio_xband.inc` (the radio core: the session channel, the
voice sink and source wrapped for the session, keying through `ptt_down` so every gate of a user's transmission holds
- TX inhibit, the battery, the band, the radio's time-out - the CW ID through `cw_begin`, the request filter, the
restore), `ui_xband.c` (the form, the picker, the question, the lock, Home's line). The memory: the P25 worker's `voice`
(6,916 B), `fifo` (5,788 B) and the call replay's `rp_buf` (4,400 B), lent while the worker is idle (`p25_lab_lend`;
the session's DMR channel stops a P25 receive first, 2 s at most, else it ends "no memory") and given back zeroed
before the user's channel returns (`p25_lab_give_back`); while lent, every worker start refuses. The cost of that: no
P25 / YSF receive during a session (it is DMR anyway) and the call replay's last P25 call is gone after one.
Deviations from docs/r14.md: no separate duty limit (the owner's rules name the 3-minute over; the radio's own time-out
applies to each transmission too); no ID at the very end (the stop must be immediate); the ID on each frequency
actually used rather than always both.

Cost (public build, `--plus --public --stack-check --verify-rebuild`, on top of last calls): FLASH 812,416 ->
820,032 B (+7,616, strings and Turkish included), static RAM 209,744 -> 209,904 B (+160; **1,552 B free**: the
picker's zone and channel share the tool screens' buffer, the session's memory is the P25 worker's, the engine's own
state is packed); every task's deepest stack path unchanged (UI 196 B, radio 760 B, start 348 B free beyond the margin;
the stack check follows the wrapped voice sink and source and the engine's io through `plus_xband.STACK_EDGES`).
Owner build (`--plus`): FLASH 891,192 -> 898,804 B (+7,612; **2,316 B free: below the 11,500 B owner rule**, as
docs/r14.md foresaw without a reclaim), RAM 209,792 -> 209,952 B (1,504 B free). Both zero warnings, forced rebuild
identical. Round 14 xband in all (items 43 and 46): public +9,808 B flash / +296 B RAM, owner +9,808 B / +296 B.

Rebased onto `ddm25` da7577c (DOTA pairing without typing: base public 810,388 B / 209,616 B, owner 889,160 B /
209,664 B); `radio_dota.inc`'s take is unchanged there, so the guard that keeps DOTA off a cross-band session stands
as it was. 8 B more of static RAM taken back for the owner build (the radio core's time is `hal_millis()`, the session
counters 8-bit). Now: public FLASH 820,184 B, RAM 209,904 B (**1,552 B free**); owner FLASH 898,960 B (2,160 B free;
the owner build's flash is not a blocker any more: the owner's radio runs the public build and the ID database moves
to a data pack), RAM 209,952 B (1,504 B free). Round 14 xband over the new base: public +9,796 B flash / +288 B RAM,
owner +9,800 B / +288 B. Both zero warnings, forced rebuild identical, stacks unchanged.

Rebased again onto `ddm25` 42a0ff4 (round 14 APRS job: items 48, 47, 49; the shared tool-screen buffer now also holds
the APRS station page's copy, the cross-band picker's zone and channel beside it). Public build: the base with last
calls and cross-band compiled out (`--feature LAST_CALLS=n --feature XBAND=n`) 814,976 B / 209,640 B; with them
**824,132 B flash, 209,928 B static RAM: 1,528 B free** (round 14 xband +9,156 B / +288 B); zero warnings, forced
rebuild identical, stacks unchanged (UI 200 B, radio 760 B free). **The owner build (`--plus`, with the ID database) no
longer links: FLASH overflows by 1,784 B** with the APRS job and this one together; it waits for the ID database's
move to a data pack (coordinator, 2026-11-20).

Rebased onto `ddm25` 8242a43 (round 14 air items and the user data packs: the built-in ID database gone, so `--plus`
and `--plus --public` build the same image; About's text buffer on the heap; DOTA can send a data pack). The data
pack sessions are DOTA sessions taken through the same `dota_take`, so `radio_dota.inc`'s guard (no DOTA while a
cross-band session runs) covers them, and Tools > Cross-band refuses to start while any DOTA session runs
(`dota_on()`). The last calls' hook into `ui_dual.c` now anchors right after multi-watch's part (air watch's follows
it). Both builds: **830,568 B flash, 209,368 B static RAM: 2,088 B free** (the base with both compiled out 821,412 B / 209,072 B: round 14 xband +9,156 B / +296 B);
zero warnings, forced rebuild identical, stacks unchanged (UI 184 B, radio 760 B, start 348 B free: as the base).

Tests: `tests/plus/test_xband.c` (the rules at every segment's edge; an over recorded and sent in order; B to A, a
private call; the dwell, held by bursts; a key-up and an encrypted call not relayed; a recording whose end never comes;
the cut at the memory and at 3 minutes, one beep; a refused transmission; one cut short on the air; the CW ID's first
time, the 10 minutes, a refused ID; the stop on the air, the battery, no memory), `tests/plus/test_xband_radio.inc`
(the real radio core and DMR call stack on the simulated modem, after upstream's and Classic's radio suites: the
session channel, PTT refused, requests dropped, a call heard on A sent on B with every voice burst as heard, the
header to the talkgroup from our ID, the terminator, the CW ID in FM on B and DMR back, the dwell, the stop restoring
the FM channel and the memory, a refused transmission, a P25 worker that keeps its memory), `tests/plus/test_ui_xband.c`
(10 flows, English and Turkish: the form, every refusal, the picker, the question, Home while it relays, the lock, the
stop, an end by itself; the layout). Radio rows: `docs/plus-radio-test.md` "Round 14 xband", XB-*.

## DMR receive audio diagnostics (`CONFIG_DMR_AUDIO_DIAG`, ours; branch `fix-dmr-audio`)

Owner's report on v1.0.0-beta.3 (public build): received DMR audio sounds crackly / scratchy and artificial; our
transmit is heard clean. Whether beta.2 or earlier builds did the same is not known.

**What changed since beta.2, against the receive audio path.** None of it runs in that path during a call:

| Change | Why it is not the cause |
|---|---|
| DOTA step 0, `PLUS_AUDIO_MUX_NO_APRS` | the ring was written only when `aprs_on`, set only through `rx_by_stream()`, which needs `board.audio_tap`; the DM-32 has none, so the ring was dead code before (`core/aprs/aprs.c` `rx_by_stream`) |
| DOTA amplifier (`AMP_DOTA`, `speaker_click`, `afsk_monitor`) | only with `afsk_tx` (a DOTA FM transmission) or a click asked by a session (`radio_dm32.c` patch in `tools/plus_dota.py`) |
| DOTA radio core, data service hook, beeps | `dota_request` / `dota_rf_poll` return at once without a session; the UDP hook only for DOTA's port |
| Home frequency (`CONFIG_HOME_FREQ`) | one text row drawn by the UI task (priority 1, below the audio and DMR tasks at 5) |
| Data packs (main after beta.3) | ID lookups run in the radio task (priority 4) on the data chip's own SPI bus with a mutex: no interrupt lock, nothing the audio task waits for |
| Last calls, cross-band (main after beta.3) | last calls are fed in the UI task, RAM only; cross-band wraps the voice sink only while a session runs (`radio_xband.inc` `xb_take` / `xb_give`) |

**The path itself** (unchanged since Classic r12): `dmr_call` (DMR task, priority 5) -> `ambeVoice.c` `sink_frame`
(FEC, a queue of 8 frames, playback after 3) -> the audio task (priority 5, woken by the microphone interrupt every
20 ms) runs `rx_block` -> the stock AMBE+2 decoder from the codec slot -> one 160-sample block into the speaker queue
(4 slots) -> a 2 ms timer pushes 32-sample chunks into the HR_C7000 prompt FIFO while AUDIO_CONTROL b31 says there is
room (`build_radiofoundry.py` r12 patch on `audio_pcm.c`).

**Ranked candidates.**

1. **No margin when the speaker starts** (most likely; a design weakness, host-tested). The speaker route turns on
   inside the first voice block's callback and empties the queue; the next block is pushed into an empty FIFO at once,
   so it and the queue hold exactly one block. Any later block that comes out of the decoder later than the first one
   leaves the FIFO empty: a click / crackle, until enough late blocks have built a margin. A block's latency varies
   with the decoder and with the DMR task (same priority, time sliced) handling a burst every 60 ms.
   `tests/plus/test_dmraudio_pcm.c` reproduces it on the real driver: first block 2 ms, later ones 2 / 7 ms -> the
   FIFO runs dry for 4 ms (mode 0); with one block of prefill, never. The FIFO has no empty flag and may replay stale
   chunks when it runs dry (`modem-hrc7000.md` 8.4: "Flushing stale audio: fill with 0x7F bytes"): that would sound
   rough rather than silent.
2. **Feeding by the b31 flag instead of a clock.** P25 on this FIFO sounded "choppy, accelerated" with this feeding
   and "works perfectly" paced at one chunk per 4 ms (lab `PROGRESS.md` 2026-11-06, `results/playback-pacing.json`:
   legacy 546 full refusals and 86 pushes within 500 us; paced none). DMR still uses the b31 feeding (lab
   `docs/receive-audio.md`: "Normal non-P25 audio retains legacy FIFO service"). That P25 test changed two things at
   once, so it does not prove which one helped.
3. **Frames lost or concealed** (`un`, `cc`, `ls`, `bd` counters): an underrun of the AMBE queue repeats the last frame
   (robotic); `bd` / `be` high means a weak or distorted signal, not our audio path.
4. **Decoder arguments.** `vocoder.c` calls the stock decoder with flags 0 and `r3 = D[0x1166]`; the vendor wrapper
   passes "a flags word" not documented (`codec-dm32.md`: "Audio quality against the vendor firmware has not been
   compared"). Only a comparison with another radio on the same transmission can point here; nothing is changed from
   a guess (clean room: no stock disassembly).
5. **Clipping**: `pk` near 32767 on normal speech would mean the decoded level clips at the DAC (codec DAC gain 0 dB).

**What is built.** `tools/plus_dmraudio.py` (one line in `plus_features.prepare`, one switch), the driver part in
`integration/radiofoundry/dmraudio/pcm_diag.inc` (included by `audio_pcm.c` after `pcm_playback.inc`):

- Tools > Diagnostics, page one, five lines first (debug text, not translated; at most 26 characters, counters stop
  at a cap):

  | Line | Fields |
  |---|---|
  | `dv m1 in150 pl147` | FIFO feed mode; AMBE frames from the air; frames played |
  | `dv cc2 un1 q4 pk12000` | blocks concealed; AMBE queue underruns; the queue's most; the call's peak sample |
  | `dv bd3 ls4 be56` | frames FEC could not fix; frames lost; bit errors fixed |
  | `sp gp7/4.0 fu300 dr1` | times the FIFO ran dry / the longest (ms), by an 8 kHz play model; FIFO full refusals; blocks dropped (each call's first block is dropped by the route change, as before) |
  | `au bl2.0-8.0 sl1 ov2` | block latency min-max (microphone block complete -> speaker block queued, voice blocks only, ms); blocks 15 ms late or more; microphone blocks the audio task missed |

- On that page with a channel shown: **0** steps the feed mode m0 -> m1 -> m2 -> m3 -> m0, **9** zeroes the counters
  (the voice queue's, the FIFO's, the DMR glue's). m0 = as shipped; m1 = prefill (after the queue empties, the FIFO
  waits for 2 queued blocks or 25 ms); m2 = P25's pacing for every stream; m3 = both. RAM only: power-on is m0.
- Costs (public build on 743c1b7): FLASH 830,592 -> 831,984 B (+1,392), static RAM 209,368 -> 209,432 B (+64; 2,024 B
  free). m1 adds 20 ms of delay at the start of each over and may cut up to 20 ms more of its tail (the route turns
  off when the AMBE queue ends, which empties the speaker queue, as before).

Tests: `tests/plus/test_dmraudio_pcm.c` (the real generated driver and audio task against a prompt FIFO model: m0 runs
dry after a fast first block, m1 and m3 do not, the counters match the model, a one-block stream still plays, the
mode applied while running, the reset), `tests/plus/test_ui_dmraudio.c` (the real formatter and digit handler cut out
of the generated `diag_dm32.c`; 4 flows, English and Turkish, layout). Radio rows: `docs/plus-radio-test.md` "DMR
receive audio", DA-*. Next: the default becomes the mode the rows show clean (one line), or the counters point at 3-5.
**Round 2 (branch `fix-dmr-audio-2`, after DA-1..3 on v1.0.0-beta.4).** The owner's counters, each a ~30 s run after
9 (`in` 177-219 frames: about 4 s of voice actually received):

| Mode | rx | in / pl | cc / un | bd / ls / be | pk | sp gp | fu | dr | au bl / sl / ov |
|---|---|---|---|---|---|---|---|---|---|
| m0 | -103 dBm | 219 / 215 | 19 / 3 | 4 / 15 / 490 | 1118 | 0 | 1676 | 999 | 0.2-4.6 / 0 / 0 |
| m1 | -99 | 177 / 177 | 3 / 3 | 0 / 3 / 331 | 1014 | 0 | 1808 | 905 | same |
| m2 | -100 | 177 / 177 | 3 / 3 | 0 / 3 / 306 | 1064 | 2 / 2.0 ms | 0 | 577 | same |
| m3 | -98 | 177 / 177 | 3 / 3 | 0 / 3 / 313 | 836 | 2 / 2.1 ms | 0 | 440 | same |

Heard: m0 and m3 cleaner than m1 and m2, none clean. What this says:

- **Candidate 1 (the FIFO running dry) is not supported**: `gp0` in m0 and m1; the block latency (0.2-4.6 ms) is well
  inside its 20 ms; no microphone block lost. The FIFO is deeper than the model's two chunks, or the first block is not
  the fastest; either way it does not run dry on this radio.
- **`dr` was not a loss**: on a DMR channel the PCM stream keeps running between calls with the speaker route off; the
  FIFO feed returns at once, the 4-slot queue fills, and every later block (50 a second) went to `play_diag[8]`
  ("queue full"). `dr` now counts only blocks lost to a full queue while the speaker plays (`spk_routed`). **`fu`** is
  normal for the b31 feed: each 2 ms tick pushes until the FIFO refuses once, so a refusal per tick while voice is
  queued; the paced modes never fill it (`fu0`).
- **The signal**: about 2 bit errors fixed per frame (`be` 306-490 over 177-219 frames) at -98..-103 dBm. The FEC fixes
  errors only in the protected parts (C0 Golay 24,12 and C1 Golay 23,12: 47 of the 72 channel bits); 2.3 corrected
  errors in 47 bits is a raw bit error rate near 5 %, so the 25 unprotected bits (C2, C3) carry about one error a frame
  that nothing fixes: that alone sounds warbly / robotic on AMBE+2. A DMR receiver is usually near 5 % BER only around
  -115..-120 dBm, so at -100 dBm either the signal is weaker than the reading says or the receiver is off frequency /
  distorted. **This is now the first candidate.** `cc19` / `ls15` in the weakest run (m0, -103 dBm) fit it.
- **The level**: `pk` 836-1118 of 32,767 on speech, about -30 dBFS. Nothing in our path scales the decoded samples:
  `vocoder.c` `decode_run` copies the codec's output (flags 0, `r3 = D[0x1166]`, the stock wrapper's values,
  `codec-dm32.md` "Calling convention"); the decoder is set up with `D[0x116A] = 128`, `D[0x116C] = -20` exactly as the
  stock init routine does (`codec-dm32.md` C, steps 1 and 4: their meaning is not documented; the encoder gets the same
  pair); `ambeVoice.c` and `audio_pcm.c` move the samples unchanged; the codec DAC is at 0 dB (`GCR_DACL` 0x80,
  `hrc7000_power_on_init`; the DM-32 never changes it, its volume knob is analog after the amplifier). FM never passes
  the DAC (the FD6818's audio goes straight to the amplifier, `hardware-dm32.md` 9), so there is no digital FM level to
  compare. Beeps go the same digital way at `level x 160` (`beep.c`, default level 50: 8,000 peak, about -12 dBFS),
  so decoded speech sits about 18 dB under the beeps. Whether stock plays it louder (a gain after its decoder, or other
  `dec_set` values later) cannot be seen without reading stock code (clean room). How loud the far end's speech is also
  matters: a quiet transmitter decodes quietly.

**Added in round 2:**

- Key **8** on the page: a digital gain on the decoded voice, x1 -> x2 -> x4 -> x8 -> x1, saturating at the 16-bit limits
  (`ambeVoice.c` `rx_gain`, in the audio callback after `rx_block`, before the beeps are mixed in: the beeps keep their
  level; it also applies to NXDN / dPMR / YSF voice on the same sink). RAM only, x1 at power-on up to beta.5 (x2 since
  beta.6, below); 9 keeps it. x1 leaves
  every sample as it was.
- A sixth line `dv cl<clipped> po<peak out>`: samples the gain saturated (until 9) and the latest call's peak after the
  gain. The first line shows the gain: `dv m0 x1 in.. pl..`.
- `dr` counts only real losses (above).

Costs (public build on bb6f554, the owner profile the same): FLASH 837,952 -> 838,352 B (+400), static RAM 209,544 ->
209,560 B (+16; 1,896 B free). Tests: `tests/plus/test_dmraudio_gain.c` (the generated `ambeVoice.c` with the
real FEC: x1 unchanged, x2 / x4 exact, x8 saturating with every clipped sample counted, the call's peak after the gain,
the beeps mixed after it, the reset keeping the gain); `test_dmraudio_pcm.c` gains the idle-stream case (drops not
counted) and the held-FIFO case (counted); `test_ui_dmraudio.c` 5 flows (key 8 added), English and Turkish. Radio rows
DA-7..DA-11.

**Result (owner's radio, v1.0.0-beta.5, 2026-11-21).** `m0 x4` sounded bad, `m3 x2` and `m3 x4` clean; the owner
chose x2 (x4 would leave FM, which has no digital gain, much quieter than the digital modes). So the feeding was the cause (candidates 1 and 2 together: prefill and the P25 pacing for every stream)
and the decoded voice was also too quiet. Since v1.0.0-beta.6 both are the power-on default: `dv m3 x2`
(`pd_mode` in `pcm_diag.inc`, `rx_gain_shift` in `tools/plus_dmraudio.py`). The keys still step them for comparison
(RAM only, back to m3 x2 at power-on). The gain also applies to NXDN / dPMR / YSF voice on the same sink, and P25's
decoded voice gets the same x2, fixed (`src/p25/voice.c`, before the sink, plus builds only). FM cannot: its audio goes
from the FD6818 straight to the amplifier and never passes the DAC (above); its level is the volume knob.

## Round 15: channels, zones and DMR rows (branch `r15-channels`, `CONFIG_CHAN_EXTRAS`)

The owner's reports after v1.0.0-beta.3 (`tools/plus_chux.py`, tests `tools/test_chux.py`). One switch for the job; it
needs `ZONE_EDIT` and `CHANNEL_EDIT`.

### 1. Zone size ("I can add a 27th channel, but on Home I can only go up to 26")

The limits are the codeplug's: **80 channels a zone** (`codeplug_zone_capacity()`; 16 in a codeplug that still has
the old 16-channel zone records), **68 zones**, **1024 channels**. Nothing between the codeplug and Home stops below
them, measured on host: the real codeplug, its RFKV records and `zone_edit.c` over a simulated flash grow a zone of FM,
DMR and P25 channels one at a time from 26 to 80, every one read back, after a power cycle too, the 81st refused
(`tests/plus/test_zone_cap.c`; also all 68 zones x 80 checked once); the real radio state machine steps Home through 26,
then 27 added underneath (as a zone edit's write arrives), then 80 positions both ways, and keeps position 80 over a
reboot (`tests/plus/test_zone_radio.inc`). No host path gives 26, so the radio's case is one of: a channel the radio
cannot read (Home's `find_channel` skips it silently, and the zone list hid it: its row vanished), or Up / Down set to
the zone or the talk target (Settings > Keys), or dual / multi-watch skipping the positions the other watched channels
hold. What changed:

- the zone list's **Add channel** row shows `26/80` (dimmed at the capacity, and OK there refuses at once), the channel
  editor's Zones list `26/80` beside each zone the channel is not in, Zones > Create New Zone `5/68`;
- a full zone refuses with **"Zone full: max 80 channels"** (Add channel, Copy here, the channel editor's Zones; the
  last one's toast was not translated);
- a zone position whose channel cannot be read now **shows** in the zone list as `27 ? Ch 123`, "Can't be read"
  (warning colour) instead of vanishing, so the user sees why Home skips it and can remove it (# menu).

Radio test row Z-1 (`docs/plus-radio-test.md`) narrows the owner's case down.

#### Above 80 channels a zone? (owner's question; not changed)

Measured on the public build (`--plus --public`, base 743c1b7: RAM 209,368 B, 2,088 B free; rule >= 1,500 B free).
A `zone_t` is 180 B (name 17 + count 2 + 80 x 2); every extra channel costs 2 B in each copy. Static copies in the image
(map file): the radio core's `sm.zone` and `buf` (radio_sm.c), 12 in `ui_dual.c` (dual / multi-watch, `mw_z`), 8 in
`ui_menus.c` (zone list cache, the zone editor's `ze_z`, `chz_z`, Find, ...), 2 in `ui_find.c`, 1 each in
`ui_app.c`, `ui_svc_radio.c` and `zone_edit.c` (`work`): **27 copies, about 54 B per channel**; the tool screens'
shared buffer (`ui_scratch`, the cross-band picker) grows too if a zone becomes its largest member. Stack copies: the
codeplug's zone read buffer (176 B, every task that reads a zone, the UI task among them: 184 B free), `zone_t` locals
in the radio core (select_in_zone, RREQ_ZONE, persist), scan priority (3) and Find & save (3, UI task).

| Per zone | Static RAM | Free (rule 1,500) | UI stack | Data chip record | CPS tools |
|---:|---:|---:|---|---|---|
| 80 (today) | 0 | 2,088 | 184 B free | 0xB0, 68 slots at EE 0x8030-0xAEF0 | qdmr, upstream web CPS, our viewer: 80 |
| 90 | +540 | ~1,550 | ~140 B | does not fit the record | lost past 80 |
| 100 | +1,080 | ~1,010 (fails) | ~100 B | does not fit the record | lost past 80 |
| 128 | +2,590 | negative (does not link) | overflow likely | does not fit | lost past 80 |
| 250 | +9,180 | negative | overflow | does not fit | lost past 80 |

The format is the hard limit, not RAM: the open-firmware zone record is a fixed 0xB0 bytes (16 name + 80 x 2), 68 of
them packed from 0x8030 to 0xAEF0 with the layout byte at 0x806F (`docs/spec/open-firmware-cps.md` 7.3.4); qdmr, the
upstream web CPS and our programmer's read-only viewer (`pc/ddm32_app/codeplug.py`, `zone_capacity = 80`) all write or
read exactly that, and the 16-channel legacy layout is told apart by that byte. More channels would need a second
record type (an extension area or RFKV entries keyed by zone slot with a check of the base record, as the P25 tags do),
which the CPS tools would neither show nor keep: a CPS write of the zone would leave a stale extension behind. DOTA's
zone record also caps `total` at `CODEPLUG_ZONE_MAX` (a protocol change with other radios). Flash cost of a larger
constant is negligible (< 200 B).

Recommendation: **keep 80**. If the owner needs more in one place, the cheap path is two zones (Zones > Copy, or a
"continued" zone the zone step reaches next), not bigger records. If 100 is ever wanted on our own format, first fold
`ui_dual.c`'s 12 static zones into one shared buffer (-1,980 B at 80), which makes 100 fit with ~1,100 B margin left of
the rule's; 128 and 250 need on-demand reads instead of whole-zone copies everywhere.

### 2. DMR channel rows ("RX group list 'Any' doesn't work sensibly")

Any was digital monitor in disguise: the radio core set the call layer's `hear_all` for it, so such a channel also
played private calls between other stations, Home showed DMon whatever the user's toggle said, and the toggle could not
turn it off (item 3). Any is now its own filter, `dmr_listen_filter_t.any_group`: **every group call on the channel's
slot** (the talk target, the reply window and all call as before), nothing else; digital monitor is the user's alone.
In the channel editor:

| Row | Now |
|---|---|
| RX list (was "RX group list": the value didn't fit) | Any reads `All TGs TS1` (`All TGs` with timeslot Any); the picker offers None, **Any: all TGs**, then the lists (Any used to come after all 76 list slots); picking Any says "Any: every TG on this slot", None "None: only the TX contact's TG" |
| Mode | a channel made DMR gets the first group contact as its TX contact when it has none, colour code 1 and timeslot 1 when it has none |
| TX contact | an RX only DMR channel saves without one (before: "Pick a TX contact first" for every DMR channel) |
| Direct mode | on: "Direct: TX on the RX frequency" |
| Color code, Timeslot, Own ID | unchanged (Any colour code / timeslot make the channel RX only, as before; Own ID: empty is the radio's) |

The VFO's RX list row (VFO settings) shows the same text. Tests: `tests/plus/test_any_group.c` in upstream's DMR call
fixture (Any plays another talkgroup, not a private call between others, ours yes; digital monitor plays both),
`tests/plus/test_ui_chux.c` (made DMR, the picker order, both one-liners, the row, save, RX only without a contact,
Direct mode; English and Turkish, the layout).

### 3. Digital monitor ("I turn it on and off and the screen still says DMon")

Home drew DMon for `v->dmr.hear_all || s->hear_all`, and a channel with RX list Any kept `hear_all` on whatever the
toggle said. Since item 2 `hear_all` is the user's again (the toggle, the monitor key held, Settings > digital monitor
by default); the channel's own Any gets its own chip on Home's DMR row, **AllTG** (Turkish "TümG"; the normal colour:
nothing to turn off), and DMon (warning colour) wins when both are on. The toggle (monitor key, quick tile) says in one
line what it does: "Digital monitor on: every call", "Digital monitor off", and on a channel that hears every
talkgroup anyway "Monitor off: channel hears all TGs". Home's hint "Digital monitor  Back off" now shows only for the
user's monitor. Tests: `tests/plus/test_ui_chux.c` (AllTG without DMon, DMon on and off with their lines, a channel
without Any; English and Turkish, the layout).

### 4. Delete channel

Before, a channel could only leave a zone (# > Remove from zone); its record stayed. Now **Delete channel** is the last
row of the channel editor (Channels > OK on a channel; not on a new, unsaved one or on VFO settings) and the last entry
of a zone's channel menu (#, after Remove from zone / Set as priority: entry 7 in the public build, the hint says
1-7 or 1-8). It asks first, **No focused**: "Delete channel?", the name, "Leaves zones: n", "Can't be undone.". Yes
posts one zone editor job (`ZONE_OP_DROP` -> `zone_edit_drop_channel`, so the foreign codeplug question and the
failure toast are the zone editor's): the channel leaves every zone that holds it (a zone left empty stays: its name
is the user's), then `codeplug_delete_channel` clears its in-use bit and its RFKV record (P25); the slot is free for
the next new channel, which is in no zone. "Deleted <name>"; from the editor the editor closes too. Refused: the channel
in use ("In use: switch to another channel first": the radio would have to move first) and while cross-band runs
("Cross-band on: stop it first"; the screen is locked then anyway, and a session keeps frequencies, not channel
numbers). A channel dual watch's other side or the scan priority holds is handled as a channel deleted by the CPS:
the radio core moves on when it reads the codeplug again. The host simulator can delete a built-in channel for the
flows. Tests: `tests/plus/test_zone_cap.c` (the real codeplug: out of two zones and "All channels", the P25 tag gone,
the free slot reused in no zone, an empty zone kept, unknown numbers refused, after a power cycle),
`tests/plus/test_ui_chux.c` (No keeps it, Yes from the zone menu and from the editor, the channel in use refused, no
row on a new channel; English and Turkish, the layout).

### 5. P25 NAC in decimal (owner: "I want to type the decimal value")

`CONFIG_NAC_DEC` (`plus_features.prepare_nac_decimal`) now puts decimal first. The channel editor's and VFO settings'
RX NAC / TX NAC rows read **`659 ($293)`**; OK opens the entry in decimal: digits build the value (`[293] ($125)` while
typed; a digit that would pass 4095 is refused, "NAC goes 0 to 4095", the value kept), Up / Down and the knob step it by
one (wrapping 0-4095), Back takes the last digit back (an untouched value: Back leaves the row as before). The special
values: **RX Any** is `#` as before and reads `Any 3966 ($F7E)`; **TX Same as RX** is `#` on the TX NAC as before;
`#` again on either switches to the hex digits (`$2[9]3 (659)`, the cursor and the knob as before; `#` there back to
decimal). OK refuses an RX NAC 3967 ($F7F: "3967 ($F7F) is not a NAC: # is Any") and a TX NAC 3966 / 3967 ($F7E /
$F7F: "TX NAC can't be 3966 or 3967") and keeps the row open; before, those reached the save and got the general
"Check P25 NAC, talkgroup and ID". Home's VFO row (`tools/plus_p25nac.py`) shows the NACs in decimal too:
`P25 NAC 1110 TG 99`, `NAC 659>1625` with a TX NAC of its own (since 2026-11-21 both forms there too:
`NAC $456/1110`, see "NAC in both forms everywhere"). P25 search and P25 trunk keep listing both ways.
Upstream's own builds (no `NAC_DEC`) are unchanged. Updated with it: `tests/plus/test_ui_any.c`, `test_ui_vfox.c`,
`test_ui_p25nac.c` (their typed NACs and expected rows now decimal), and `integration/radiofoundry/vfox/ui_vfox.inc`
(the flag is `ced_nac_hex` now). New flows in `tests/plus/test_ui_chux.c` (English and Turkish).

## Round 15: search ranges and speed, the multi-watch ring (branch `r15-scan`)

The owner's requests after using v1.0.0-beta.3 on the radio. Code: `tools/plus_scan.py` (the hunks into upstream
files), `integration/radiofoundry/srange/` (installed into `core/radio`, `core/ui`), and our own search screens and
engines (`spectrum/ui_p25search.c`, `ui_dmrsearch.c`, `p25search.*`, `dmrsearch.*`). Host tests: `tools/test_scan.py`
(run by `tools/test_classic.py`). Radio rows: `docs/plus-radio-test.md` "Round 15 scan".

**Cost of the three** (public build on e5d8cbf, with the repeater lists of `research-data/`; the `--plus` build is the
same): FLASH 834,352 -> 837,952 B (+3,600), static RAM 209,488 -> 209,544 B (+56), **1,912 B free**. Zero warnings,
forced rebuilds identical, every task's stack fits. Each item's own figures below were measured on 743c1b7 with the
sample repeater list (the flash there is about 10.7 KB lower; RAM the same).

### 1. Search ranges (`CONFIG_SEARCH_RANGE`; needs P25_SEARCH or DMR_SEARCH)

Tools > P25 search and DMR search take any range of the receiver's bands and ten steps:

| Key (search screen) | Does |
|---|---|
| Left / Right | the presets as before (+-500 kHz, +-1 MHz, +-2.5 MHz around the frequency in use, 430-440, 144-146 MHz), then the manual range once one is set |
| * | opens the range editor ("Search range, MHz") on the range swept now |
| 0-9 | opens it typing Lower |

| Editor row | Left / Right (and the knob) | Held Left / Right (DM-32: long Left / Right) | Digits |
|---|---|---|---|
| Lower | - / + one step | - / + 1 MHz | type it (positional, as the VFO entry: "4 3 3 5" = 433.5); OK goes on to Upper |
| Upper | - / + one step | - / + 1 MHz | type it; OK applies the range |
| Step | 2.5, 5, 6.25, 10, 12.5, 20, 25, 50, 100 kHz, 1 MHz (round) | | |
| Move (the width shown) | the whole range - / + one step | - / + 1 MHz | |

OK applies (the range becomes the manual one; a running search starts again on it, a stopped one waits for OK);
Back deletes the last digit typed, else leaves the editor unchanged; Up / Down move between rows (a typed value is
taken first). Refused, with a toast and the editor kept: Upper not above Lower; a range not inside one of the
transmit bands (136-174, 400-480 MHz: the FD6818 the searches step; the air and broadcast bands are other receivers);
an end moved out of the bands is simply not moved. The range line shows "Range +-1 MHz  12.5k" (shorter forms when it
is too wide: without "Range ", or a long manual range alone).

**Remembered**: each search keeps its own preset, manual range and step in RAM while the radio is on (leaving the
screen and coming back finds them). Not across a power off: `settings_t` has no room (docs above, "Settings record"),
and the owner's request did not need a stored record; a later round can add it to a data pack style record.

**How the step reaches the radio**: `radio_req_p25_search(lo, hi)` and `radio_req_dmr_search(lo, hi)` keep their
form (no change for the simulators and other callers): the start frequency's last decimal digit carries the step
(`radio/srange.h`; every frequency the FD6818 takes is a multiple of 10 Hz). Digit 0 is 12.5 kHz, the step the
searches always had, so an untagged request sweeps as before. The radio core (`RREQ_P25_SEARCH`,
`RREQ_DMR_SEARCH`) passes the decoded start and step to `p25s_start` / `dmrs_start`; the engines already took any
step. * on the two search screens opens the editor instead of quick settings (`ui_app.c`'s QUICK exceptions).

Host tests: `tests/plus/test_ui_scan.c` (`tests/plus/ui_sim_scan.c` records the requests and decodes them as the
radio core does), 8 flows in English and Turkish with the layout checked on the screen and the editor: the presets and
the range line; typing (a digit opens the editor, OK on, OK applies, nothing asked before OK); Back deleting digits
and leaving; every step both ways, 6.25 kHz and 1 MHz in the request; one step, held Left / Right and the DM-32's
long Left / Right on Lower, Upper and Move; the refusals; a running search started again on an applied range, the
range kept across leaving the screen and then among the presets; the DMR search's range and step.
`tests/plus/test_scan_radio.inc`, after upstream's radio suite and the Classic / Plus P25 suites (the searches' own
state machines among them) built with the switch: the P25 search requested with a tagged start steps the receiver
by 25 kHz, 1 MHz (round to the start), 6.25 kHz, and by 12.5 kHz untagged.

Cost (public build): FLASH 819,832 -> 822,584 B (+2,752), static RAM 209,368 -> 209,408 B (+40: the two ranges
2 x 12 B, the editor 16 B), free 2,048 B. The ID database build (`--plus`) is the same size (the database is a data
pack since round 14).

### 2. Faster searches (`CONFIG_SEARCH_FAST`; needs P25_SEARCH or DMR_SEARCH)

**What a step cost, and what bounds it** (read from the code, measured in the host model below; nothing measured on
the radio yet). The radio task polls every 25 ms (`RADIO_POLL_MS`); a sweep step retunes the FD6818 at one poll and
reads its strength (register 0x67, one SPI read: no time worth counting) at a later one:

| | Before | Bound by |
|---|---|---|
| P25 search, a quiet step | 50 ms (2 polls): the reading must be `P25S_SETTLE_MS` = 30 ms after the retune, and the next poll is 25 ms after it. The old "30 ms a step" in "P25 search" above was the settle, not the step: a +-1 MHz pass (161 steps) is 8.0 s, 430-440 MHz 40 s | the settle: the PLL's lock and the strength reading's filter, neither measured on this radio (hardware-dm32.md 8.5: "the PLL lock time after each retune is not measured yet") |
| DMR search, a quiet step | 100 ms (4 polls): 3 readings from 30 ms on, a poll apart (a direct mode burst is on air 30 ms in every 60: three readings 25 ms apart always meet one) | the settle and the 60 ms TDMA frame |
| A hit that isn't P25 / DMR | P25: the native receiver's take and `P25S_PROBE_MS` 0.7 s for a frame with a NID (an LDU is 180 ms; the worker's start); DMR: 0.6 s for two bursts with a good check (sync once per 360 ms superframe in a call); then skipped 10 s (8 frequencies) | the frame sync windows: not shortened (a NID needs a whole frame, a DMR sync a burst; cutting them loses calls) |

**What changed** (only with the switch; the engines without it are byte for byte as before, the host model builds
both):

- **A quick reading gates the step.** A step's first reading is taken at the poll after the retune (`P25S_QUICK_MS` /
  `DMRS_QUICK_MS` 20 ms: the next poll even with a millisecond of jitter). Quiet: on to the next step at once. A quiet
  P25 step is one poll (25 ms), a quiet DMR step three (the quick reading and two settled ones: 75 ms).
- **Only settled readings make a hit.** P25: a strong quick reading is read again on the same step at the next poll
  (now 50 ms settled) before it counts. DMR: the quick reading only marks a step (with half the hit's margin, as a
  burst that began just before it reads part of its level), which then gets two more settled readings: together the
  settled ones cover every phase of the 60 ms frame.
- **A slow reading is caught by a look back.** If the receiver's reading lags the retune, the level read quickly on
  step k may be step k-1's: strong at once, quiet settled. The engine then reads step k-1 again, settled (P25: two
  polls; DMR: three readings), and goes on from k. So a signal is not lost when the reading is slower than 25 ms, only
  its step costs a few polls more; a strong step already known (skipped) gets no look back.
- **Busy carriers that are not P25 / DMR are probed less**: one found again within 60 s of its skip running out is
  skipped for 30 s (`P25S_SKIP_LONG_MS`, `DMRS_SKIP_LONG_MS`) instead of 10.

**Host model** (`tests/plus/test_search_model.c`, run by `tools/test_scan.py`; both engines built from the same source
with the switch off and on): the radio task's polls (25 ms +-1 ms), the receiver's strength reading through a
first-order lag of time constant tau after each retune (0, 5, 10, 15 ms), noise +-1 dB at -124 dBm, a neighbour 12.5
kHz away 60 dB down; 200 signals per row at a random step of +-1 MHz, level -112 to -60 dBm, random TDMA phase:

| | Before | After |
|---|---|---|
| P25, a quiet step / a +-1 MHz pass / 430-440 MHz | 50 ms / 8.0 s / 40.1 s | **25 ms / 4.0 s / 20.0 s** |
| DMR, a quiet step / a +-1 MHz pass / 430-440 MHz | 100 ms / 16.1 s / 80.1 s | **75 ms / 12.1 s / 60.1 s** |
| A busy FM carrier in the range, probes in 3 min (P25 / DMR) | 12 / 10 | 7 / 6 |
| P25 found, tau 0 / 5 / 10 / 15 ms | 200 / 200 / 200 / 200 | 200 / 200 / 200 / 200 |
| DMR repeater found, tau 0 / 5 / 10 / 15 ms | 200 / 200 / 200 / 200 | 200 / 200 / 200 / 200 |
| DMR direct mode found, tau 0 / 5 / 10 / 15 ms | 200 / 200 / 198 / 198 | 200 / 200 / 200 / 197 |
| Hits on another frequency (each a wasted probe), every row | 0 | 0 |

The weakest direct mode signals (-112 dBm, 12 dB over the floor) are at the noise's mercy at tau 15 ms in both: a case
either way. The model's checks: a quiet P25 step <= 26 ms and DMR <= 76 ms, fewer probes of the busy carrier, every
signal found up to tau 10 ms (and never fewer than before there), no hit elsewhere. The P25 and DMR searches' state
machine tests (`test_plus_p25search_sm`, `test_plus_dmrsearch_sm`) pass with the switch on (`tools/test_scan.py`'s
radio suites), and `tests/plus/test_scan_radio.inc` checks a quiet step is one poll in the radio core.

**To confirm on the radio** (rows SF-*): the quick reading assumes the FD6818's strength follows a retune within about
25 ms; if it is slower, the look back keeps the finds and only the steps near a signal cost more (the pass time shows
it). The revisits of found channels are unchanged (every 16 steps), so a find is re-read about every 0.4 s (P25) /
1.2 s (DMR) instead of 0.8 s / 1.6 s.

Cost (public build): FLASH 822,584 -> 823,184 B (+600), static RAM 209,408 -> 209,424 B (+16: the engines' stage,
the step before and the look back's start), free 2,032 B.

### 3. The multi-watch ring (`CONFIG_MWATCH_FAST`; needs MULTI_WATCH)

The owner misses calls with 6 channels. **Why** (read from the code, reproduced in the host model):

| # | Cause | Change (ring of 3..6 only; dual watch's pair keeps upstream's timing) |
|---|---|---|
| 1 | Every quiet stop listens the whole scan dwell (150 ms, Settings: scan dwell) although the receiver's reading is ready at the settle (`SCAN_SETTLE_MS` 50 ms): 6 channels took 0.9 s a round, so a call waited up to 0.9 s (and its first syllables) before the ring came to it | a stop with nothing heard since the settle (no carrier by the noise reading, nor sync, nor a signal to hold) ends at the first reading after the settle: 50 ms; a DMR stop at the third (100 ms), as direct mode bursts are on air 30 ms in 60. Anything heard keeps the dwell, the probe and the hold as before; a signal still holds at once |
| 2 | **A carrier on a P25 channel that brings no frame for us (an FM user on that frequency, another NAC) froze the ring**: the native receiver gave it back after 2.5 s with an 8 s cooldown, but the ring then stayed on the channel's FM path, holding on the carrier (the zone scan's pass-over of a channel in its cooldown was for zone scans only), until the carrier ended plus the wait. Host model: a 20 s carrier there, and the ring's A was visited **0 times** in those 20 s | the ring gives a P25 channel back by the zone scan's rules (`native_lab.c` `scan_by_frames`): no frame of ours in 1.2 s: back, 2 s cooldown (8 s when it repeats); frames then none for 0.6 s (a call's tail): back without a cooldown; frames without voice for us: 2.5 s and the cooldown. A channel in its cooldown is passed over at once. Host model: A visited **54 times** in the same 20 s |
| 3 | A P25 stop was found by the FM noise reading only (on this modem the reading may stay put on C4FM, "P25 in a zone scan" cause 4) | the strength counts too, on the DMR squelch scale, as in a zone scan: the second strong poll holds |
| 4 | The cooldown's key was the zone position only; a ring may hold VFOs (C..F) | `native_lab_scan_key`: the zone position or 0xFFF0 + the VFO's index, as the give back names it |

Not changed: the hold, the wait after a signal and its looks at the others, Busy, PTT on the selected letter, a DMR
channel's probe and late entry, the P25 hand-over to the native receiver (scan_allows: a hold on the ring), voice
given back 1.5 s after the last. One receiver: a call on one channel is still missed while the receiver is on
another, only for much less time.

**Host model** (`tests/plus/test_mwatch_radio.inc`: the real radio state machine, native receiver and the fake
board; run without the switch by `tools/test_classic.py`'s suites, with it by `tools/test_scan.py`):

| Ring | Before | After |
|---|---|---|
| 2 channels (dual watch), any mode | 0.30 s | 0.30 s (unchanged) |
| 3 / 4 / 5 / 6 channels, FM (or P25, NXDN, dPMR, AM on the FM path) | 0.45 / 0.60 / 0.75 / 0.90 s | **0.15 / 0.20 / 0.25 / 0.30 s** |
| 3 / 4 / 5 / 6 channels, DMR | 0.45 / 0.60 / 0.75 / 0.90 s | **0.30 / 0.40 / 0.50 / 0.60 s** |
| Time to hold on a call starting at a random moment, 3 / 4 / 6 FM channels: mean (worst) | 161 (375) / 275 (500) / 427 (825) ms | **86 (150) / 130 (200) / 163 (300) ms** |
| A P25 channel with a 2 s signal without voice for us: the ring held there | 3.6 s | **1.3 s** |
| A 20 s foreign carrier on a P25 channel of 6: rounds the others got | **0** (frozen) | **54** |

Home's "Ring 0.3 s" follows (`radio_mwatch.h` `radio_mw_ring_ms(n, n_dmr)`: the DMR letters counted; ", P25 +2.5 s"
for a P25 letter: the hold alone, no wait after it). On the radio each FM <-> DMR switch and each retune adds its
own time (not modelled; rows MR-*). Air watch (AM VFOs on the ring) and cross-band (which stops any watch) are
unchanged; air watch's suite passes with the switch (`tools/test_scan.py`).

Host tests with the switch: the multi-watch suite (order, a 50 ms stop, hold and wait with looks, PTT, refusals, a
P25 channel on the FM path, the period table against the estimate, the catch time, the foreign carrier) after
upstream's radio suite and the Classic / Plus P25 suites; air watch's and every mode in the VFO's suites;
`tests/plus/test_ui_mwfast.c` (Home's ring line with 4 and 6 letters, dual watch's panes without it, English and
Turkish, the layout). The test fixture's frame time is now cleared between the multi-watch cases
(`mw_start_radio`): a stale one from the suites before had made a take look like a call's tail.

Cost (public build): FLASH 823,184 -> 823,428 B (+244), static RAM unchanged (209,424 B, free 2,032 B). In the same
commit: the search range editor lets PTT, the monitor and the emergency key through (item 1's editor had taken every
key while open).

## Round 16: airfields by province, spectrum in colour (branch `r16-airprov`)

The owner's requests after using v1.0.0-beta.4 on the radio. Code: `tools/plus_airprov.py` (the switches),
`integration/radiofoundry/air/` (the airfield table, its walks and screen), `tools/airfields.py` (the province of each
airfield). Host tests: `tools/test_airprov.py` (run by `tools/test_classic.py`). Radio rows: `docs/plus-radio-test.md`
"Round 16 airprov".

### 1. Airfields by province (`CONFIG_AIR_PROV`; needs AIRFIELDS and MAP)

Tools > Airfields now opens as Tools > Repeaters does (item 37): a first level "All airfields (74)", then the provinces
that have airfields in plate order with their counts ("01 Adana (2)", "06 Ankara (5)", ... "76 Iğdır (1)": the map's
names, only provinces with airfields, "Unknown" last if an airfield had no province). OK opens that province's list:
the same rows (distance and direction, the tower or first frequency, "+N"), nearest first from the position APRS uses
(by code without one), Up / Down round within the province, # its nearest; OK on an airfield its details, listening
and air watch (#) as before. "All" is the whole list as before. Back: details -> the province's list -> the first
level (its focus kept) -> out. # on the first level: the nearest airfield's details in "All". The air band data pack
(`air_count` / `air_freq`) still answers every airfield's frequencies (the province only filters the walk).

- **Province of each airfield, at build time** (`tools/airfields.py`): the repeater list's own lookup
  (`tools/rpt_list.py` `Provinces`: the map's province outlines, then a district centre within 10 km, the
  municipality naming a province, a district centre within 30 km). One byte in what was `air_field_t`'s tail padding
  (44 B a row as before, `_Static_assert`), so the table costs no flash. **Deviation**: on the pinned data the
  outlines alone put two airports in the neighbouring province: Sabiha Gökçen (Pendik, İstanbul) inside the
  simplified Kocaeli outline and Çukurova (near Tarsus, Mersin) inside Adana's. OurAirports' own `iso_region` (ISO
  3166-2:TR, whose numbers are the plates) is now checked: where it differs and a district centre of that province is
  within 30 km (`REGION_KM`: a border the simplified outlines do not follow), the region wins; farther, the position
  stays and the build line reports it (none on the pinned data). Result: 74 airfields in 54 provinces, unknown 0 (71
  by outline, 1 by a district centre: Ordu-Giresun off the simplified coast, 2 by region). The generated table's
  comment line gives the distribution; `--list` prints each airfield's plate.
- **Walks**: `air_next_in` / `air_rank_in` / `air_count_in` keep to one province (`AIR_PROV_ALL`: all; `air_next` /
  `air_rank` are the All wrappers), one pass over the table a step as before: no RAM for the order.
- RAM: the first level's state (which groups have airfields: 11 B, the filter, the picked and top rows), as Tools >
  Repeaters keeps its own.

Host tests: synthetic OurAirports rows (inside an outline, off the coast, at both borders where the region wins, a
region far from the position kept and reported, no region), the generated rows and comment line, the committed table
(every airfield in a province, 14 known places); `tests/plus/test_airprov.c` (every province's walk from Kızılay and
Antalya and without a position: only its airfields, each once, nearest first, rank = place, backward the reverse; All
as before); `tests/plus/test_ui_airprov.c` in upstream's simulator with the data chip, English and Turkish, the layout
check on every page: the first level (counts, plate order, no empty province, round), every province label fitting a
row, Ankara's list (only its five, round, # its nearest), Antalya ("+1", every frequency, OK listens), air watch from a
province, an air band pack's frequencies in a province's list, no position (by code), All, Back and #. The screens'
older flows (`test_ui_misc`, `test_ui_air`, `test_ui_datapack`) are built without the switch and pass unchanged.

Cost (public build on bb6f554): FLASH 837,952 -> 838,908 B (+956), static RAM 209,544 -> 209,560 B (+16), free
1,896 B. The `--plus` build: the same figures.

### 2. The spectrum in colour (`CONFIG_SPECTRUM_COLOR`; needs SPECTRUM_WF)

Tools > Spectrum's waterfall and 3D view (item 45) drew each level in one of the theme's few colours (background,
line, idle, dim, RX, warning twice, TX: mostly greys). On a colour panel both views now draw the waterfall's 8 slots
(`wf_slot`: levels 1..15 above the floor, 3 dB each) in a signal strength ramp (`wfall.c` `wf_ramp`):

| Slot (levels) | 1 (1-3) | 2 (4-5) | 3 (6-7) | 4 (8-9) | 5 (10-12) | 6 (13-14) | 7 (15) |
|---|---|---|---|---|---|---|---|
| Dark and high contrast themes | dark blue (16,40,168) | blue (16,120,255) | cyan (0,216,232) | the theme's RX green | yellow (248,232,0) | orange (255,128,0) | the theme's TX red |
| Light theme (a light background) | pale blue (168,204,255) | blue (32,104,232) | teal (0,160,192) | the theme's RX green | dark yellow (200,168,0) | orange (232,104,0) | the theme's TX red |

The flat waterfall uses the same ramp (it was not coloured before either). A mono panel keeps the old slots (any
non-zero colour is ink there, so its picture is the same either way). The light ramp is chosen by the background's
brightness, so an accent or the high contrast theme needs nothing more.

- **The 16 colour palette** (the DM-32's bpp 4 panel, `gfx.c`): the screen's own solid colours are 8 (background,
  text, dim, line, idle, RX, warning, the status strip's grey), and anti-aliased text edges take entries while more
  than four are free. The ramp adds 5 colours of its own (green and red are the theme's RX and TX) plus TX red, which
  the screen does not otherwise draw. Counted in the simulator with the ramp drawn where the waterfall is (last): all
  16 entries taken (8 solids, 4 text edges, 4 ramp colours), orange and red fell to the nearest entry. Now each frame with the waterfall on first draws the
  ramp and the screen's own colours on one pixel (then the background over it, `wf_reserve`), so they hold their
  entries: 15 of 16 in use (dark and light themes, flat and 3D view), every level in its exact colour; the text edges
  take the nearest existing grey (the upstream rule once the palette is short; the dark theme's greys suit it).
- Host tests: `tests/plus/test_wfcolor.c` (each theme's ramp: background, RX and TX reused, the eight distinct, at most
  five added, the hues running blue > cyan > green > yellow > orange > red, the weakest dark on black and light on
  white; the 3D view drawn into a test frame buffer with every level's ridge in its slot's colour);
  `tests/plus/test_ui_wfcolor.c` (Tools > Spectrum in the simulator's 16 colour frame buffer, dark and light,
  English and Turkish: the newest waterfall row has every point in its level's exact ramp colour, the 3D view draws
  every slot, the colours on the screen counted, bars alone draw none of the ramp, the layout check). The existing
  3D view flows (`test_ui_wf3d`, built without the switch) pass unchanged.

Cost (public build): FLASH 838,908 -> 839,056 B (+148), static RAM 209,560 -> 209,560 B (+0), free 1,896 B.
The `--plus` build: the same figures. **Both items** (public build on bb6f554): FLASH 837,952 -> 839,056 B
(+1,104), static RAM 209,544 -> 209,560 B (+16), **1,896 B free**. Zero warnings, forced rebuilds identical,
every task's stack fits.

## Round 16 polish (branch `r16-polish`)

Small finishing work toward a near-final product. Code: `tools/plus_polish.py` (hunks into upstream files), host tests
`tools/test_polish.py` (run by `tools/test_classic.py`).

### 1. The Turkish catalogue complete

**Cause.** `tools/i18n.py` (upstream) extracts `TR()` / `TRN()` texts from core's `.c` and `.h` files only. Texts in
the `.inc` files the `.c` files include never reached `tr.po`, so `--sync` would not add them and the radio showed
them in English; they were translated only where a `.c` file happened to use the same English. Missing were the
multi-watch ring's line under the rows ("Ring 0.6 s", `ui_mwatch_draw.inc`), its # menu ("Talk on B", "Change C",
"Stop, stay on B") and toasts ("Talking on C", "P25 slows the ring", "Only a selected channel can be the VFO",
`ui_mwatch.inc`). Besides those, a few texts were drawn without `TR()` at all: upstream's zone toasts ("Zone full",
"Give it a name first", "Couldn't save: try again") and its "In Zones" list (prefix and hint), the caller line before
the first link control ("P25 call", "YSF call", NXDN's "call"), the battery's time left ("4 h 20 min",
`ui_battgraph.c`), About's "Based on ..." lines and the repeater data source "sample"; and the cross-band form's new
"Slot" row had no entry.

**Change.** `plus_polish.py` makes `tools/i18n.py` read `*.inc` as well and wraps those texts in `TR()` / `TRN()`; 18
entries appended to `tr.po` (one block, branch `r16-polish`) and one typo fixed ("Airbandnda" -> "Airband'de").
Turkish: "Döngü 0.6 s", "Konuş: B", "Değiş: C", "Durdur, kal: B", "Konuşulan: C" (the letter after a colon, so no
suffix has to agree with it), "4 sa 20 dk", "P25 çağrısı", "Bölgelerde", "Open25-DM32 Classic tabanlı".

**Test.** `tools/test_polish.py` runs i18n.py's own extraction (with the `.inc` files) against `tr.po` in the prepared
tree: every text the radio can show must be in the catalogue, translated, not fuzzy and drawable (`check_translation`:
Latin-5 bytes, the `{...|...}` alternates, leading / trailing spaces, the ending colon). A new text without its
Turkish now fails `make classic-test`. `tests/plus/test_ui_mwfast.c` checks the ring line by its number in Turkish.

**Turkish fit.** The simulator writes every text it shortens with an ellipsis to `SIM_FIT_LOG` when that is set
(`ui_sim.c` `gfx_fit_trace`). Run over every UI test (English and Turkish) the shortened Turkish texts of rounds 14-15
were three, now shorter: the multi-watch # menu's "Değiştir: C" ("Değiş: C"), DOTA's "Bir bölge" ("Bölge") and the
reply window hint "# arayan  Geri yanıtı bitir" ("# arayan  Geri bitir"). The rest shortened in both languages alike,
by design: long airfield and channel names (cut in the middle), "No frequencies in the data" beside a distance. The
layout check found nothing on any screen in either language.

Cost (public build = `--plus` build, on 01e8507): FLASH 837,952 -> 838,312 B (+360: the `TR()` lookups), static
RAM unchanged (209,544 B, 1,912 B free); stacks unchanged (UI 184, radio 760, start 348 B free). Zero warnings,
forced rebuilds identical.

### 2. `tools/datapack.py`: the aerodrome code next to underscores

`code_of` searched `\bLT[A-Z]{2}\b` in the file name; in `LT_AD_2_LTAC_en.pdf` the underscore before `LTAC` is a word
character, so there was no word boundary and the build stopped with "no aerodrome code". Now a code is any `LTxx` not
touching a letter or digit, the last one in the file name, else the last one in the page text before its AD 2.18
heading (as `AirText.CodeOf` in the Windows programmer). Tests in `tools/test_datapack.py`.

### 4. Text size option (ROADMAP item 40): measured, not built

Asked: Settings > Display "Text size" Normal / Large, the next larger fonts across lists and menus, built only if it
fits (public static RAM free >= 1,500 B, the layout check clean on every screen or Large limited to the screens where
it fits). **Not built: the layout does not fit.** The measurement and a proposal:

**Fonts.** The DM-32 build has `FONT_AA`, `FONT_BIG` and `FONT_XL`: lists, labels and the status strip use JetBrains
Mono 9x18 (`ui_layout.small`, 24 characters across the 220 px safe area), small print 8x16 (`tiny`), Home's channel
12x24 / 16x32. "One size up" is small 12x24 (18 characters a line) and tiny 9x18. Both fonts are already in the image,
so **no font flash**; a size between (10x20 / 11x22) would be a new 2 bpp font of about 4-5 KB.

**Flash, RAM, stack (estimated from the parts, not built).** The switch is a few lines in `ui_layout_init` and a
re-layout when the setting changes, one Display row and its Turkish: about 300-500 B flash. Static RAM: 0-1 B
(`ui_layout` already holds the font pointers). Stack: none (no new call depth). The setting needs one stored bit:
`settings_t.ui_flags` is full (all 8 bits used), so it would take a spare bit of another field as the Plus settings
already do (for example `accent`'s or `dual_state`'s free bits), which is a codeplug-independent change of the settings
record and needs its own test. The budget would fit (1,880 B free after this branch).

**Layout (the deciding part).** The simulator draws one size up when `SIM_TEXT_LARGE` is set (`plus_polish.py`,
host only: the firmware is unchanged). Every Plus UI test (about 30 binaries, English and Turkish) was run both ways:

| | Normal | Large |
|---|---:|---:|
| Layout check problems (text outside the screen, overlaps) | 0 | **59 distinct, on about 40 screens in 14 suites** |
| Distinct texts the screens shorten with an ellipsis | 92 | **490** |
| UI test binaries whose own checks pass | all | none (texts they look for are cut) |

Where it breaks: every page drawn line by line rather than as a list (`ui_draw_text_page` style and hand-placed
labels): the battery page ("Time left: 4 h 40 min" runs off the right), the search range editor's key lines, the P25 /
DMR search pages, the airfield details ("Seyrüsefer için değil"), the marine header, the clock, the GPS dashboard,
locator, follow, SOS page, the DMR audio diagnostics, the 3D waterfall's readout and its "No room" against the range,
the multi-watch rows ("467.375 MHz  TS1 CC1"), the cross-band and DOTA pages' "Henüz çağrı duyulmadı". List screens
(Tools, Settings, channel editor, last calls) stay inside the screen but cut most values: "Single-f…epeater", "USB
…work", "TX z…şımı", "dPM…321", in Turkish more than in English. Upstream's own gallery check (`uiSim --layout`) does not
build in the Plus host tree (the Plus screens need their own simulator wrappers), so upstream's screens were seen only
through the Plus suites that open them.

**Proposal** (for the owner to decide; a round of its own):

1. **Large lists only.** Apply the larger font to `ui_draw_list` rows (Tools, Settings, Zones, Channels, the editors,
   Recent, last calls, heard list) and leave pages, Home and the status strip as they are. Lists already shorten
   values safely; the cost is the shortened values, which need shorter Turkish for the common rows
   and a value column that gives way to the label. Pages stay readable as now. Estimated 0.5 KB flash, one bit stored.
2. Make each page that overflows fit its own lines (wrap or shorten through `gfx_text_fit`) before Large covers it:
   about 40 screens, each with its layout check in both sizes; worth it only screen by screen, starting with the most
   used (battery, search, airfield details).
3. Alternatively a 10x20 or 11x22 anti-aliased font (+4-5 KB flash, generated with upstream's `tools/fontconv.py`
   from the same JetBrains Mono): 20-21 characters a line instead of 18, far fewer cuts, but the same page work.

Already there: Settings > Display > Large text makes Home's channel and talk target up to twice as big
(`CONFIG_LARGE_SCALE`); the proposal above is for everything else.

## DOTA frequency: amateur bands only (branch `dotafreq`)

Owner, 2026-11-21: on Tools > DOTA Transfer a frequency can still be typed (Frequency source Manual), but only an
amateur band frequency is accepted, with the same rules as the cross-band (section "Cross-band store and forward").
One definition serves both: the band plan `xband_check_hz` (`radio/xband.c`) and its toasts with the repeater list's
check (`ui_xband_refusal`, `ui_xband.c`), built with `CONFIG_XBAND` or `CONFIG_DOTA` (the rest of the cross-band only
with `CONFIG_XBAND`). Design text: [dota.md](dota.md) 12.6.

| Frequency | DOTA (typed, stepped, any source at Send / Ready) | Toast (Turkish) |
|---|---|---|
| 144.000-146.000, 430.000-440.000 MHz (edges included), outside the rows below | accepted | |
| below 144, 146-430, above 440 MHz (162.550, 143.9875, 429.990, 440.0125, 146.0125) | refused | "Not an amateur band" ("Amatör bant değil") |
| 446.000-446.200 MHz (PMR446) | refused | "PMR446: no repeaters allowed" ("PMR446: röle yasak") |
| 144.800 MHz +- 6.25 kHz (APRS) | refused | "APRS frequency" ("APRS frekansı") |
| 145.000-145.1875 and 145.600-145.7875 MHz (RV48-RV63 inputs / outputs), 431.050-431.825 and 438.650-439.425 MHz (RU inputs / outputs), each +- 6.25 kHz; a frequency of the repeater list | refused | "Repeater frequency" ("Röle frekansı") |
| 145.800-146.000 and 435.000-438.000 MHz (satellites; 145.825 ISS APRS among them) | refused | "Satellite segment" ("Uydu bölümü") |
| APRS's own frequency (Settings > APRS), when the plan allows it | refused at Send / Ready | "APRS frequency" |

- **Typed** (6 digits kHz or 7 digits with 100 Hz, OK or Up / Down): a refused frequency is not taken; the toast says
  why and the frequency and its source stay as they were.
- **Left / Right** on the frequency: never out of the amateur bands (the toast, the frequency stays); a step into a
  refused segment inside them is taken, so the user can step across it, and Send / Ready refuses it.
- **Current channel** is offered only when it passes the same check (it was the upstream `aprs_amateur_hz`, 144-148,
  222-225 and 420-450 MHz). Default 433.450 and Last used pass by construction; Send / Ready checks every source.
- **The question "Not amateur band: send?" is gone** (its Turkish entry too): outside the bands DOTA sends nothing.
  The receiver (it answers on the air) is checked the same way.
- Host tests: `tests/plus/test_ui_dota.c` (`freq_typed`: 19 typed frequencies, each refused or started on exactly
  that frequency; `freq_stepped`: the band edges, a step into the satellite segment refused at Send for sender and
  receiver; `start_refusals`: APRS's own frequency), built with the real `xband.c` / `ui_xband.c`; the band plan's own
  table stays in `tests/plus/test_xband.c`. Radio rows D-B1 to D-B3 in [plus-radio-test.md](plus-radio-test.md).

Budget (`--plus --stack-check --verify-rebuild`: zero warnings, forced rebuild identical, every task inside its
stack, the least free prvIdleTask 136 B as before):

| | FLASH B | RAM B (free) |
|---|---:|---:|
| `ddm25` 6d846e3 | 840,656 | 209,568 (1,888) |
| this branch | 840,656 (+0: the shared check's code is about what the question, its Turkish and the ISS compare took) | 209,568 (1,888) |

Also fixed on the way: `tests/plus/test_dmrsearch.c` expected the search log's range from an earlier search
(433.000 MHz); the search running there starts at 432.500 MHz.

## Dual watch and multi-watch: the TX target (branch `mwfix`)

Owner's report on the v1.0.0-beta.7 candidate (f75f319a), triple watch with A DMR 436.275 CC1 TS1 TG1, B P25 145.500
TG1 RX NAC Any (F7E) TX NAC $293, C FM 433.500 no tone; a DMR radio on A's settings, a P25 radio on B's:

1. P25 on B sometimes not heard (sometimes it is; after power off/on it sometimes works again);
2. PTT on the P25 letter does not transmit inside dual / multi-watch (it does with the watch off);
3. sometimes the screen still shows TALKING / TX after PTT is released;
4. **with C (FM) selected, PTT transmitted P25 on B** (the P25 radio heard it): the wrong channel and mode.

**The TX target rule** (now true in every case): PTT transmits on the letter Home shows with the PTT pill, the
selected one (`dual_sel`), with that letter's own frequency, power, tones, CC / TS / talk group or NAC / talk group.
With 2 channels upstream's talk back may move the selection to the pane a call just came in on: the toast "Talk back on
B" says so and the PTT pill moves with it, so the rule is the same (talk back is off in the ring of 3..6). The receiver
being on another letter (a quiet stop of the ring, a look in the wait, a call held there) never changes the target: the
radio first tunes the selected letter, then keys it.

**Root causes** (read from the code, each reproduced in the host model on the real radio state machine and native
receiver before the fix):

| # | Report | Cause | Fix |
|---|---|---|---|
| 1 | 4, 2 | The native P25 owner sees PTT before the radio core (`radio_task.c`: `native_lab_request` first) and decided by the channel **the receiver happened to be on** (`sm->view.ch`), not the selected one. Ring on (or a call held on) B with C selected: it kept PTT and keyed P25 on B with B's NAC (report 4); ring on B quiet with C selected: PTT swallowed, nothing keyed. The radio core's own rule (`dual_talk`: tune the selected letter first) never ran | `native_lab.c`: PTT in dual watch / the ring asks the radio core first (`radio_sm_dual_away`, `tools/plus_mwfix.py`); the receiver on another letter: the native receiver lets that letter go (the request waits, `NATIVE_REQUEST_WAIT`), the radio core tunes the selected one (`dual_talk`), then a P25 / YSF letter is keyed by the native transmitter, an FM / DMR one by the radio core. A selected letter that can't be read ends the watch and refuses the PTT (never the letter underneath) |
| 2 | 2 | B selected, the ring on A or C: the radio core tuned B (a P25 channel) and keyed it as **FM** (an analog carrier on 145.500 the P25 radio ignores); its release went to the native owner (the view was P25 now), so the radio core never unkeyed it (`tx_off`) until the ring's next retune | 1 above sends it to the native transmitter; besides, `ptt_down` refuses (TXDENY_MODE) instead of keying FM on a P25 / YSF letter whatever reaches it (`tools/plus_mwfix.py`) |
| 3 | 2 | B selected, the ring on B but on a quiet stop (not holding a signal): the native receiver may take a ring letter only while the ring holds a signal there (`scan_allows`), so PTT took nothing and keyed nothing; a take that came later started receive only (`live_tx_pending` cleared) | `scan_allows`: PTT on the selected letter takes it whatever the ring does there (a quiet stop, the settle after a give back, a cooldown); a take while PTT is held keys at once |
| 4 | 3, 1 | The native receiver shows its own state in the view (`channel_view`: TX while its transmission arms, runs or drains). When it gave a ring letter back in the poll right after its transmission ended (the call's voice ended over 1.5 s before: a reply longer than that), no `channel_view` ran in between (the session's cleanup returns early), so **TX stayed in the view**: Home showed TALKING after release, the radio core took it for its own transmission, so the ring stood still (`scan_talked` every poll), the retune after the give back waited for the end of that TX, and nothing was heard on any letter, B included, until the time-out (3 min by default; never with the time-out off) or a power cycle. The same with TX denied | `native_lab.c` `native_view_clear`: a TX / TX denied / RX the native receiver showed is cleared whenever it hands the channel back (`restore_channel`, `channel_let_go`) |
| 5 | (1) | A take of the ring made for PTT and no reply afterwards would have been given back as a foreign carrier, with the 8 s cooldown (fast ring: 2 s, 8 s when it repeats): the reply to our over unheard | a take that transmitted waits 2.5 s after the release for the reply, then goes back to the ring without a cooldown (`scan_after_tx`) |
| 6 | 3 | A P25 transmission the native worker refused while PTT was held showed nothing (idle) | shown as TX denied (Busy) until release |

Not changed: the ring's order, dwell, hold, wait and looks, Busy, the P25 hand-over to the native receiver on a
signal and its give back by the zone scan's rules, talk back with 2 channels, the native P25 transmitter itself, the
TX checks (`radio_sm_tx_check`, the P25 TX bands). One receiver: a call on one letter is missed while the receiver is
on another (the ring's limit, above).

**Host tests** (`tests/plus/test_mwatch_radio.inc`, run without and with the faster ring by `tools/test_classic.py`
and `tools/test_scan.py`, and inside the VFO-modes, priority-channel and air watch suites): each PTT goes the way the
radio task sends it (`classic_dispatch`: the native owner first, its wait kept). The owner's exact A DMR / B P25 / C FM
(PTT on each letter with the receiver parked on each letter, and on B held by a P25 call: the reply right after the
other station's over); 2..6 letters in two mixed patterns (`DPFPDF`, `FDPDFP`), every letter selected, the receiver on
every letter and held by a call on every P25 letter: 227 PTT cases per build. Each checks the letter keyed (FM / DMR
through the board, P25 through the native transmitter), its frequency, mode, power, FM tone or P25 NAC (RX Any, TX
$293) and talk group, the ring holding 2 s while PTT is down, then on release: the indicator cleared, the native
receiver let go, the ring going on, the board retuned, and a new P25 call heard again. An RX-only FM and P25 letter
refused and cleared; a TX / TX denied left in the view at the give back cleared (report 3's order played by hand: the
fixture's worker lets go inside its own pump). With the code before the fix the same tests fail thousands of checks (the
owner's setup: PTT on A or C from B and on B from anywhere keyed the wrong letter or nothing; the stale TX froze the
ring and B was not heard again).

Not host-tested: YSF / NXDN / dPMR letters (YSF has no transmitter in this build: refused as on a channel; NXDN /
dPMR receive only), VFO letters (they go through the same `dual_here` / `dual_talk`), the real worker's timing.
Radio rows MW9-MW16 in [plus-radio-test.md](plus-radio-test.md).


**Home's details** (part 2 of the same job; with VFO_MODES and NAC_DEC, i.e. every Plus build: `UI_DETAIL`,
`integration/radiofoundry/mwatch/ui_detail.inc`, `tools/plus_mwfix.py`): each channel row says its mode's own
settings, the first form that fits the 240 px row (the simulator's layout checks, English and Turkish, normal and
Large text):

| Where | Before | Now |
|---|---|---|
| Single channel, the chips row of a P25 / YSF / NXDN / dPMR channel | FM's "No tone 12.5k High SQ3" | `NAC $293/659 TG 42` (the owner's B: `NAC Any>$293/659 TG 1`), `DG-ID 12`, `RAN 5`, `CC 7`, then the power and squelch where they fit; tight `NAC$293/659 TG42`. FM (tones) and DMR (CC, TS, talk group) rows had theirs already; the VFO has them on its own row (round 15) |
| Dual watch panes (2 letters), a P25 / YSF / NXDN / dPMR pane | row 2 the frequency, row 3 "No tone 12.5k"; a P25 VFO pane said "VFO FM" | P25 as a DMR pane: row 2 what PTT talks to (`TG 42`), row 3 the frequency and the NAC (`145.5000 MHz  NAC Any>$293/659`); YSF / NXDN / dPMR (receive only) the frequency on row 2 and `DG-ID 12`, `RAN 5`, `CC 7` on row 3 |
| Multi-watch rows (3..6), line 2 | `P25 145.5000` | `P25 145.5000 NAC Any>$293/659 TG 1`, tight `...NACAny>$293/659 TG1`; then the frequency and the tight details (`467.375 CC1 TS1 TG3100`), the mode and the tight details (the selected row, beside its PTT pill: `DMR CC1 TS1 TG3100`), the tight details alone, last the mode and frequency as before. FM `T88.5 R100.0` / `T88.5` / `No tone`; DMR `CC1 TS1 TG 91` (Any `CC* TS*`, a private talk target `ID 1234`); YSF `DG-ID 12`, NXDN `RAN 5`, dPMR `CC 7`. A VFO letter's row follows its VFO as it is now (before, the mode it had when it became a VFO) |
| Large text | | unchanged: one line per letter / pane, the name only |

Not done (no room on the row or in RAM): NXDN's group (the channel keeps the RAN only), dPMR's own ID (the radio's),
the DMR talk group of a VFO letter (the radio core's, not in the VFO copy: CC and TS only). RAM: 4 B per ring row
(the three packed values in the row's padding and 4 more) and the row the drawing fills: +32 B static. Host tests:
`tests/plus/test_ui_mwdetail.c` (the formatter for every mode and form; Home with 4 and 6 letters, a P25 VFO letter,
dual watch with a P25 VFO pane; English and Turkish, normal and Large text, the layout checks), `test_ui_vfox.c` and
`test_ui_mwatch.c` unchanged; the single channel's chips row is covered by the formatter only (the simulator has no
P25 channel to put on Home). Radio rows HD1-HD4.

**Review round** (an independent check of the two commits above found these; fixed on the same branch):

| # | Found | Fix |
|---|---|---|
| B1 | **A refused PTT became an unstoppable P25 transmission**: the radio core keys by itself (the 1750 Hz burst, the DMR emergency hot mic, VOX, cross-band: `ptt_down`) and on a P25 letter refuses it (TXDENY_MODE) but sets `sm->ptt`; the native side took `sm->ptt` as its PTT (`scan_allows`, the take's `live_tx_pending`), keyed P25 on the selected letter and, the release never reaching it, held it until the time-out (never with the time-out off) | `native_lab.c` `native_ptt`: set and cleared only by its own PTT branch; only it lets the native receiver take a letter for PTT, arms a transmission or passes `eligible`; a native transmission ends as soon as `native_ptt` or `sm->ptt` goes; while the radio core holds PTT itself the native side leaves the view (its TX denied) alone |
| S1 | The PTT pill and the radio's selected letter could disagree: during TX the radio drops a selection (RREQ_DUAL / RREQ_MWATCH) but the UI moved the pill anyway (the keys, talk back) | `apply()` refuses a new selection while TX / TX denied ("Not while transmitting", the pill stays); `mw_tick` asks the radio again for the pill's letter after a transmission if they still differ (once a second at most); talk back waits through TX denied too |
| S2 | The 1750 Hz burst checked the mode of the letter the ring was on, then `dual_talk` keyed the selected one: 1750 Hz on a DMR (or P25) carrier | `tone_burst` tunes the selected letter first (`dual_to_selected`), then checks it is FM |
| S2b | SSTV send keyed the letter the ring was on; DOTA and cross-band ended the watch there; DOTA's "Current channel" frequency was the ring's letter | SSTV tunes the selected letter first; DOTA and cross-band end the watch on the selected letter; DOTA's "Current channel" source has no frequency while a watch runs (another source must be picked) |
| S3 | Listening only: P25 frames without voice for us within 2.5 s (a late entry still waiting for its link control) were given back with the **8 s** cooldown, so most of a call of ours could be skipped | frames without voice: taken again after 2 s (the zone scan's retry); the whole cooldown only when it happens again within 10 s (another talk group's conversation). Host model, ring of 3 / 6, the call starting while the receiver is elsewhere: late entry heard after 250 / 450 ms; frames 1.5 s before voice: 1.5 s; frames 3 s without voice then voice: 4.8 / 5.0 s (before: over 10 s, failing) |
| M2 | A DMR row showed only the channel's contact; YSF / NXDN / dPMR values were shown as talk targets although this build can't transmit them | a DMR letter in a pane shows the talk target PTT uses there now (a target for now, Call back: dual watch's snapshot); YSF DG-ID, NXDN RAN, dPMR CC are shown as what the channel receives (pane row 3, not row 2's "PTT talks to") |
| M3 | With 2 letters, talk back moves the PTT pill and now P25 really goes out there | a short beep with the "Talk back on B" toast (the pill moves with it); the setting's default unchanged (off), the owner decides |

**Named by its letters** (owner 2026-11-22): the watch is Dual / Triple / Quad / 5x / 6x watch (Turkish Çift / Üçlü / Dörtlü / Beşli / Altılı izleme) by the ring's letters (else Settings > Watch channels): the status strip's word (DUAL, TRIPLE, QUAD, 5X, 6X; ÇİFT, ÜÇLÜ, DÖRTLÜ, BEŞLİ, ALTILI, where it fits), Quick settings tile 7 ("Quad watch" / "Quad"), Tools' row, the # screen's title, the toast when it starts ("Quad watch on" / "Dörtlü izleme açık"); its end says "Watch off: on C" as before. The settings rows keep their names ("Dual watch delay" and "talk back", "Watch channels", the key function "Dual/multi-watch"). The Turkish "Dual watch" entries are now "Çift izleme".

Host tests added: the burst and the hot mic from every letter of the owner's A / B / C and every ring position, and with a P25 call holding B, the time-out off (no native transmission, the burst only on an FM letter, nothing left on the air; the hot-mic cases fail with the code before); a selection asked during TX (radio side), the UI's refusal and the pill moving after it (`test_ui_mwdetail.c`); RS_RX left at the give back; the listen-only cases above (the 3 s case fails with the 8 s cooldown); the zone scan's frames-without-voice rule (`tests/classic/test_scan.inc`: 2 s, then 8 s); the names (EN toasts and strip words; the layout checks in EN and TR, normal and Large text).

**Second review round** (branch `mwfix2`):

| # | Found | Fix |
|---|---|---|
| S1 | The once-a-second re-ask of the pill's letter (RREQ_MWATCH) makes the native receiver let a letter go (`channel_let_go`): with a mismatch that stays (the radio refuses the ring), a P25 call held on a letter was cut every second | asked again twice at most, never while a call comes in (RS_RX); then the pill takes the radio's letter ("Talking on C"). The radio choosing another letter itself (the debug link) is taken at once, without a request back (`mw_adopt`). And a selection (the A/B key, a re-ask) whose ring still has the letter a P25 call is held on no longer lets the native receiver go: the radio core doesn't retune for it (`radio_sm_dual_keeps`), the call plays on, PTT goes to the new letter |
| S2 | The DMR emergency (alarm, hot mic, its talk target, `data_can_send`) works on the channel the radio is on: in the ring, the letter it happens to be on | the emergency key while a watch runs: the selected letter DMR: the watch ends there first; else refused, toast "Emergency: select a DMR letter" ("Acil durum: DMR harfi seçin"). ARS and text replies still answer on the letter the data came in on |
| M1 | A safety net for any TX / TX denied left in the view | in the ring's poll, TX / TX denied with PTT up and nothing keyed (burst, roger, sub-audio tail, VOX, DMR keying or TX, CW, the hot mic) is cleared, and a retune that waited for it is done |
| M3 | The S3 retry's cost | a P25 conversation on another talk group on a ring letter takes it 2.5 s, then 2 s free, then again (the other letters get about 45 % of the ring until the 8 s cooldown applies, from the second time within 10 s) |
| M5 | Ring PTT on VFO letters untested | host test (VFO modes build): C a P25 VFO (145.575, NAC $456, TG 99) keyed natively with its NAC from every ring position; D a YSF VFO refused (TX denied, nothing keyed) |

Host tests added: a selection during a held P25 call (`test_mwatch_radio.inc`; fails without the fix), the debug link's selection and the UI flows (`test_ui_mwdetail.c`: the radio selecting A, the emergency key on a DMR and an FM letter), the safety net (`test_mwatch_radio.inc`), the VFO letters (`test_vfox_radio.inc`).

Budget (`--plus --stack-check --verify-rebuild`: zero warnings, forced rebuild identical, every task inside its stack;
radio task 728 B free (720 after the review round, 716 after the second), UI 184 B: the radio task's deepest path is still the P25 search's give back):

| | FLASH B | RAM B (free) |
|---|---:|---:|
| `ddm25` dc6376d | 857,584 | 209,856 (1,600) |
| the TX target fix | 858,176 (+592) | 209,856 (1,600; the one new flag sits in padding) |
| Home's details | 859,524 (+1,348) | 209,888 (1,568; +32 B, the 1,500 B floor kept) |
| review round and the names | 860,584 (+1,060) | 209,904 (1,552; +16 B: the # screen's title) |
| second review (on `ddm25` 415b519: 864,272 / 209,916) | 865,032 (+760) | 209,920 (1,536; +4 B) |
