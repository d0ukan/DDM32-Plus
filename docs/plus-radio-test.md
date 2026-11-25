<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# DM32 Plus: radio test plan (unit .047, COM15)

Every step that opens the serial port, writes flash, restarts the radio or transmits needs the owner's explicit
approval in chat for that step (AGENTS.md hard rule 1). Receive tests come first; transmit tests last and only on
frequencies the owner's licence allows.

## 0. Decisions and inputs

| Item | State |
|---|---|
| Owner decision: install our own build with normal TX, codeplug storage and CPS (AGENTS.md rule 3 today allows only the official Classic for that) | **given 2026-11-16; installed and verified 2026-11-17** |
| Plus package | `artifacts/plus-20261116/` (`rfoundry_dm32.bin` SHA-256 `95517e66…869761`, manifest: plus, rebuild identical) |
| Installer | `tools/install_plus.py` (application sectors below 0xE8000 only; boot, codec slot and data chip must read back unchanged) |
| Radio now | Classic r7 from the Firmware Utility, Classic codeplug ("Start new") with the 433.500 MHz P25 channel |

## 1. Back up the radio as it is (read only)

```sh
python -u tools/dm32_readonly_backup.py --port COM15 --out backups/dm32-047-classic-20261117 \
  --installed-firmware "Open25-DM32 Classic 0.1.0-rc1-r7 (Firmware Utility)" --wait 120
```
Pass: `VERIFIED` (both chips read twice, identical). This backup is the install basis and the way back to Classic.

**Then go straight to step 3 without letting the radio boot normally**: Classic may write settings when it runs,
and the installer compares the radio with this backup byte for byte (a difference stops it before any write; a
new backup is then needed). After `VERIFIED`, leave the radio off until the installer says `READY`.

## 2. Plan (no port)

```sh
python tools/install_plus.py --port COM15 --backup backups/dm32-047-classic-20261117 \
  --plus artifacts/plus-20261116 --out backups/install-plus-plan --plan-only
```
Pass: the codec slot is found and its CRC holds; every changed sector is below 0xE8000; boot and slot unchanged.

## 3. Install

```sh
python -u tools/install_plus.py --port COM15 --backup backups/dm32-047-classic-20261117 \
  --plus artifacts/plus-20261116 --out backups/install-plus-<time> --wait 600
```
At `READY`: radio off, then on normally. Pass: `VERIFIED` then `PASS`; receipt `installation_verified`,
`codec_slot_verified`, `data_sector_crcs_unchanged`, `restart_sent` all true.

## 4. Boot

| Check | Pass |
|---|---|
| Boot screen, then Home | no reboot loop; About shows `dm32-plus-0.1.0-dev` |
| The Classic channels are still there | the 433.500 MHz P25 channel (NAC 659, TG 101) is listed |
| Tools menu | Spectrum, Fox hunt, CW beacon, Lone worker, Sun & Moon, Go-To, P25 search present |

## 5. Receive tests (owner's P25 radio on 433.500 MHz, NAC 659, TG 101)

| # | Test | Pass |
|---|---|---|
| R1 | P25 channel, 5 calls | all heard; Home bottom line "P25 TG 101 ID …" during each |
| R2 | Same channel with NAC Any and TG Any (editor: # key) | becomes RX only (toast); all calls heard |
| R3 | P25 scan: a zone with the P25 channel and an FM channel | scan stops on each P25 call, resumes after it (2026-11-18: it did, but a 3-5 s call was caught only on the 5th zone cycle; rebuilt 2026-11-19, docs/plus.md "P25 in a zone scan": R3a-R3e) |
| R3a | Catch rate, single calls. A zone of 8 channels (FM and DMR ones that are quiet, the P25 channel among them, squelch 5), zone scan on, scan resume as it is. Power the radio off and on first (the counters start at 0). The other radio: 20 calls of about 3 s on 433.500 (NAC 659, TG 101), 15 s apart. Count the calls heard (two words or more) | 18 of 20 or more; each heard call starts within about 2 s of the key-up; the scan goes on about 3.5 s after a call (1.5 s + the 2 s resume delay) |
| R3b | Right after R3a (no power cycle), read the counters: `python tools/plus_txdiag.py --port <port> --scan` (debug link `0x21 [12]`, read only; the tool in the lab repo needs that query added; the serial step needs its own approval) | `carriers` >= 18 and `takes` >= 18 (fewer `carriers` than calls: the stop did not see the C4FM carrier, write down the Home meter's dBm during a call and the channel's squelch level); `synced` and `voiced` about equal to `takes`; `quiet` 0-2, `foreign` 0; `sync_ms` (the last take's time to its first frame) under 700 and `voice_ms` under 1100. `sync_ms` near or over 1200, or `quiet` about as high as `takes`: the 1.2 s probe time is too short for the modem's start, report the figures |
| R3c | Catch rate, conversation: 20 calls of about 3 s, the next one 2-4 s after the last ended; then the counters again | 18 of 20 or more (before the change every second one was lost to the 8 s cooldown); the scan stays on the P25 channel between the overs; counters: `ended` rises by the number of pauses longer than 1.5 s, `cooled` and `quiet` stay about where they were |
| R3d | Short calls: 10 calls of about 2 s, 15 s apart, in the 8-channel zone; then the same in a zone of 16 | 8 channels: 9 of 10 or more; 16 channels: about 6-7 of 10 (the scan's round is 2.4 s there: a limit of scanning, not a fault) |
| R3e | Not ours on the P25 channel: an FM radio keyed on 433.500 for 20 s, then (5 s after it stops) one P25 call; and the P25 radio on another talkgroup or NAC for 10 s (NAC Any / TG Any off) | during the FM carrier the scan stops there for about 1.2 s, goes on, comes back about 2 s later, then only every 8 s or so, with no audio and no stuck stop; FM and DMR channels of the zone are still scanned meanwhile; the P25 call after it is heard; another talkgroup holds the channel 2.5 s without audio, then the scan goes on (another NAC: as the FM carrier, 1.2 s); no crash record (`--health`) |
| R3f | The same fault in dual watch and multi-watch (the give back forgot the call's voice): dual watch FM + P25, two P25 calls of 3 s, 3 s apart; then N3 / MW3 again | both calls heard (before: the second fell in the 8 s cooldown after the first); panes, callers and the ring as before |
| R4 | P25 search, range +-1 MHz | 433.5000 found with NAC 659 and TG 101; calls heard; sweep resumes |
| R5 | Spectrum, 500 kHz span, while the other radio transmits | a peak at 433.500; the close call hit names it; OK tunes the VFO there |
| R6 | Fox hunt on 433.500, walk away / closer | the tone pitch and rate follow the signal; the attenuator moves the scale |
| R7 | Dual watch with the P25 channel as one side | P25 calls heard on that side |
| R8 | Broadcast FM (a local station) and air band AM (if in range) | audio |
| R9 | GPS outdoors: Tools > Position, Sun & Moon, Go-To | a fix; sunrise/sunset plausible for the day; a saved waypoint shows distance and bearing |
| R10 | APRS receive on 144.800 MHz | stations listed |
| R11 | Lone worker 5 min, no keys | warnings at 5:00, alarm 30 s later, any key stops it |
| R12 | One hour of use, then the debug link task view (read only) | no reset; `tasks.heap_min_free` > 1 KB (the RAM budget estimate was 3.8 KB) |

## 6. Transmit tests (owner's licence, owner's frequencies)

| # | Test | Pass |
|---|---|---|
| T1 | FM channel, short call to the owner's other radio | heard |
| T2 | CW beacon "DE <call> K" once | the other radio hears the Morse; PTT during a periodic beacon cuts it |
| T3 | P25 TX on the 433.500 channel (Classic's TX path) to the owner's P25 radio | heard there |

## 6b. Pending on the radio (owner: test later; do not forget)

Installed 2026-11-18: c9dacee7 (artifacts/plus-20261118: everything below plus trimmed stacks, DMR any and the antenna splash mark).

Results of the first pass (2026-11-17): R1-R8, R11, R12, T1-T3 pass; R9 GPS (outdoors) and R10 APRS (needs
traffic) open. Build 7c90427b (artifacts/plus-20261117c) is installed but its new items are untested:

| # | Test | Pass |
|---|---|---|
| N1 | P25 call on the 433.500 channel | Home bottom line "P25 TG 101 ID 77285 NAC 659" (or the short form) |
| N2 | Edit the channel to NAC Any and save, stay on it; the other radio on NAC 293 calls | heard without leaving the channel |
| N3 | Dual watch (long SK2) with the P25 channel; a P25 call | the pane names "TG 101 ID 77285 NAC 659", not "FM signal" |
| N4 | Dual watch off, SK2 press (A/B) | dual watch starts |
| N5 | Tools > Air band | VFO on 118.000 MHz, "AM" shown; later it returns to the last air band frequency |
| N6 | P25 search +-1 MHz, 433.500 found once, then a new call | caught within about a second |
| N7 | Boot | our splash (antenna mark, "DM32 Plus"), then Home in Turkish. **Passed 2026-11-18 on c9dacee7** (language switch back to English also works) |
| N8 | Menus in Turkish: Settings, Tools, channel editor | readable, no cut words; Ğ ğ İ ı Ş ş look right; technical names (RX, TX, Fox hunt, Dual watch, Talkgroup) in English; Settings > Display > Language can switch to English |
| N9 | Tools list | an icon before each entry; clear at 16 px |
| N10 | Tools > Son çağrı (Last call) on the P25 channel after a call | the last ~8 s play again; pressing it again stops; a live call cuts the replay |
| N11 | Tools > Harita (Map), outdoors with a GPS fix | Turkey outline, the cross at our position, the il name below; zoom and pan keys |
| N12 | Build b4ab8353 (artifacts/plus-20261117e): make an NXDN channel (channel editor Mode: FM > DMR > P25 > YSF > NXDN, RAN 0 = any) | saved, shown as NXDN, RX only; PTT refused. With an NXDN48 transmitter (none yet): calls heard, Home "NXDN <id> > TG <tg>"; without one: no false calls on noise, FM channels unchanged |
| R12b | One hour of use on this build, then `tools/plus_txdiag.py --health` | heap_min_free > 1 KB (the heap gave 4 KB to the replay; it was 8,144 B before) |
| R12c | Build with the trimmed stacks (docs/plus.md, third round): an hour with P25 calls, a DMR call, GPS on, then `--health` | no reset, no crash record; tasks.free.p25_lab, dmr, gps each > 200 B; heap_min_free about as R12b |
| D1 | DMR channel, editor: Color code past 15 | "Any" (Tümü), toast "RX only", saved; Home shows CC* |
| D2 | Same, Timeslot | 1 > 2 > Any; Home TS*; a call on either slot is heard and Home names its slot, back to TS* after |
| D3 | RX Group List > last entry "Any (all groups)" | calls to any talkgroup heard; PTT still goes to the contact (if RX only is off) |
| D4 | A channel with Any CC or Any TS, PTT | refused (no TX) |
| H1 | Three P25 calls from the other radio, then Menu > Recent | three entries "P25 77285", "TG 101 NAC 659" and their age, newest first (a call within 1.5 s of the last counts as the same) |
| H2 | OK / PTT on a P25 entry; Last caller | nothing sent; Last caller goes to the newest DMR entry (or opens Recent if there is none) |
| H3 | An encrypted P25 call (if one can be made) | toast "Encrypted: not played" (Şifreli: çalınmadı), no audio, entry marked Encrypted |
| Q1 | P25 call on the 433.500 channel, the other radio close by | Home bottom line ends "BER 0.0%" (or a few tenths); the dBm above it moves with the signal during the call (it used to keep the FM reading from before the call) |
| Q2 | Same, the other radio farther away / its antenna off / ours off | BER rises (several %) as audio starts to break up, dBm falls; updates about twice a second; nothing shown once the call ends |
| Q3 | P25 call while the dBm reading is live: listen for clicks or dropouts compared with build c9dacee7 | no new audio gaps (the level read must not disturb the native receiver) |
| Q4 | DMR call (any DMR channel) | call screen's signal line "BER x.x%  -87 dBm", the meter shorter; BER rises with a weak signal |
| Q5 | Dual watch with the P25 channel, a P25 call | pane shows the caller as before (no BER there); no change to N3 |
| S1 | Tools > DMR search, +-1 MHz around 433.500; the other radio calls in DMR direct mode on 433.500 (any CC, TS, TG) | within a pass (about 16 s) "DMR 433.5000", the call heard; found entry "433.5000 CC<n> DIR" and "TG <tg> from <id>"; a pass with nobody on air finds nothing (no false finds on noise) |
| S2 | After S1's call ends | the sweep goes on about 2 s after the last over; a new call on 433.500 is caught within about 2 s (revisit) |
| S3 | A local DMR repeater's output in range (or a hotspot in repeater mode) | entry with RPT, its CC and the slot(s) in use (TS1 / TS2 / TS1+2); an idle repeater (beacon / hang) is listed without a call and the sweep moves on after about 1.5 s |
| S4 | During the search: PTT (sweeping, and again while a found call plays) | no TX (red LED off, the other radio hears nothing), the search stops, the channel is back; the next PTT press works normally on the channel |
| S5 | Up / Down to a find, OK | VFO on that frequency in DMR with its CC (and TS when one slot was seen); its calls are heard there; a repeater find also toasts "Repeater: set the TX offset" |
| S6 | Start the search on the P25 433.500 channel, then Back | the P25 receiver is off while searching (no P25 audio), and receives again on the channel after Back |
| S7 | An analog FM carrier in range (e.g. a handheld on 433.450) | probed about 0.6 s, not listed, not probed again for 10 s; also measure: a pass's time, how soon a DMR carrier is seen after the hit (to shorten DMRS_PROBE_MS) |
| R9 | GPS outdoors: Position, Sun & Moon, Go-To | as in section 5 |
| R10 | APRS on 144.800 when there is traffic | stations listed |
| F1 | FM caller ID build: an FM channel, 10 min idle (squelch closed, then opened with monitor), then a normal voice call | no MDC / FleetSync / DTMF line on Home, nothing new in Recent; the voice sounds as before (level, quality; the speaker now plays FM through the HR_C7000's FM playback as on the APRS frequency); no new click when the squelch opens or closes |
| F2 | The other radio on FM sends DTMF "123#" while keyed (its keypad during PTT) | Home's bottom line "DTMF 123#" as the digits come, gone about 4 s after the last; Recent gains "DTMF 123#" (no PTT / actions on it) |
| F3 | MDC1200 PTT ID from a Motorola radio (or an MDC1200 recording played into the other radio's microphone) | "MDC xxxx" (four hex digits, as the sender's radio shows its ID) for about 5 s at key-up and again at release; one Recent entry |
| F4 | MDC1200 emergency, same source | "MDC xxxx Emergency" on Home; the Recent entry says "Emergency" in the warning color |
| F5 | FleetSync I PTT ID from a Kenwood radio (or a recording) | "FleetSync fff-uuuu" (fleet 100-349, unit 1000-4999) for about 5 s; one Recent entry. FleetSync II (2400 baud) is not decoded |
| F6 | The APRS channel (144.800) with APRS receive on | APRS stations still decode (R10); DTMF sent there still shows on Home |
| F7 | FM > NXDN > DMR channel and back, dual watch FM + DMR, a zone scan over FM channels; then `tools/plus_txdiag.py --health` | no stuck or missing audio, an NXDN channel still starts; heap_min_free still > 1 KB (the ring takes 2 KB of heap while an FM channel listens) |
| SR1 | SFR build: a one-frequency DMR channel (e.g. a copy of the 433.450 direct channel), editor row "SFR" after Battery saver; OK steps it | Off > User > ERDM user > Repeater > Repeater (ERDM style) > Off; "SFR inbound slot" TS1 / TS2 shows once a role is set, "SFR outbound CC" (Channel's / 0-15) only for the ERDM roles; Back without saving leaves the channel as it was |
| SR2 | Choose Repeater (or Repeater ERDM style) | toast "Repeater role: transmits by itself"; on an RX only channel both repeater roles are skipped with "Repeater needs TX: RX only is on"; on a two-frequency channel (or TX edited away from RX) the row reads "Needs one frequency" and OK only toasts "SFR needs one frequency: RX = TX" |
| SR3 | Licence permitting, low power, short range: our channel set to Repeater (TS1 in), saved and in use; the other radio talks on the same frequency in DMR direct mode, TS1, same CC | Home says "SFR repeater" / "Repeating"; the red LED flashes for the repeated slot; a third receiver (or SDR) hears the call twice (TS1 direct, TS2 repeated about 90 ms later); our PTT refused. Tools > Single-freq repeater: "Repeater", "In TS1, out TS2", "Repeating a call" / "Hang time" / "Listening" |
| SR4 | The reverse: our channel set to User (TS1 in), the other radio (one with SFR, e.g. another DM32 Plus or a RadioFoundry radio) as the repeater on the same frequency | our PTT goes out on TS1 and is repeated; the repeat of other users is heard; Tools > Single-freq repeater: "User of a repeater", "Talk on TS1, hear TS1 and TS2" |
| SR5 | Tools > Single-freq repeater on a channel without a role | the text points to the channel editor's SFR row (not the Codeplug Editor) |
| SR6 | CPS round trip: read the codeplug with the PC Codeplug Editor after SR1-SR4 (role, slot, ERDM CC set on the radio); set a role with the Codeplug Editor on another channel (e.g. framing BS, hang 4 s), write, then on the radio change only that channel's name and save, and read back | the CPS shows what the radio set; the CPS-made channel keeps its framing and hang after the radio's edit; Off set on the radio clears the role in the CPS |
| U1 | Satellites (elements in integration/radiofoundry/sat/tle.txt, CelesTrak epoch 17.11.2026: refresh if older than a week). Tools > Satellites indoors without a fix, then outdoors | "Waiting for the GPS time" / "...position" first; with a fix the clock line (local time, UTC+03:00, "TLE n d"), then within about 10 s every satellite's next pass (rise time, top elevation, minutes), soonest first |
| U2 | Compare the list with an online prediction (Heavens-Above, AMSAT pass predictor or N2YO) for the same place | rise and set within a minute, top elevation within 2 degrees (fresh elements: under a week old) |
| U3 | After a minute with the screen open, 0 (the pass alarm page), bottom line | "SGP4 at most n.n ms", the slowest propagation (estimated 1-2 ms; on the DM-32 the clock line has no room for it): note it; over 5 ms means the search should do one propagation a poll |
| U4 | Open the screen on the P25 channel (idle, then during a call), during an FM call and a DMR call; scroll it for a while | the list fills on the P25 channel too (slower there); no audio gaps or clicks, keys and the display respond as before (the math runs in the radio task) |
| U5 | During a real pass (ISS 437.800 or SO-50 436.795 downlink): focus it, compare azimuth / elevation with the antenna direction that works best | the live azimuth and elevation match the strongest direction; "LOS in m:ss" counts down; RX / TX show the Doppler shift (UHF about +-10 kHz at the horizons) |
| U6 | OK on that pass (or up to 10 min before it) | toast "VFO tracks the pass"; Home: VFO on the corrected downlink, FM wide, TX split to the corrected uplink with CTCSS 67.0; the RX frequency steps by 2.5 kHz (UHF) a few times during the pass and the downlink stays audible across it |
| U7 | While following: PTT (on a licensed, working transponder, briefly) / turn the VFO by hand / choose a channel | TX on the corrected uplink with the tone, no retune while keyed; a hand change or a channel ends following ("Stopped: VFO changed"), the VFO stays where it was put |
| U8 | SO-50 followed, # | toast "Uplink tone: arming", TX tone 74.4 Hz (shown on the TX line); # again back to 67.0; 30 s after the set following ends on its own, the VFO back on 436.795 / 145.850 |
| PA1 | Pass alarm page: Tools > Satellites with a GPS fix, a satellite whose next pass is more than 10 min away in focus (hint "OK alarm"), OK; Up / Down, Left / Right on each row; OK; Back | page "Pass alarm": For (that satellite, then the others and "Any satellite"), Before AOS 2 / 5 / 10 min, Min. elevation 0 / 10 / 20 / 30 / 45°, "Off: OK arms it"; after OK toast "Pass alarm armed", within a few seconds the pass ("SO-50 13:01 23°", the same as the list) and "Tone at hh:mm" (rise minus the lead); Back: the list, the status line "Alarm hh:mm: SO-50 23°" when that satellite is in focus |
| PA2 | Armed for the ISS (or Any satellite) with Before AOS 2 min; Back to Home, the display left to go dark | at the tone time two short beeps, the backlight on and the banner "ISS in 2 min, max NN°" (about 8 s); at the rise one short high beep and "ISS AOS now, max NN°"; both within a minute of an online prediction (as U2); the page then reads "Off: OK arms it" |
| PA3 | Armed, then the radio switched off and on | the alarm is off (RAM only): the page reads "Off: OK arms it", nothing sounds at the pass |
| PA4 | Any satellite, Min. elevation 45°, armed on the P25 channel and again during an FM and a DMR call; the screen closed after | "Alarm: searching…" for a few seconds, then the pass (or "Alarm: none in 36 h"); no audio gaps, clicks or key lag while it searches or waits (one propagation a radio poll, then a comparison a second) |
| PA5 | Tracking a real pass (U6), Satellites screen open, Home now and then | "Tracking Doppler"; the range rate goes from about -7 to +7 km/s through 0 at the top; RX / TX shifts in kHz shrink and change sign; Home: the VFO moves in whole steps (2.5 kHz UHF, 1 kHz VHF), about 8-12 times a pass, never back and forth; when only the uplink moves the downlink audio has no gap |
| PA6 | Tracking, PTT held about 20 s across the top of the pass (TX: owner approval and an authorised frequency; a working transponder only) | the TX frequency stays put while keyed (no retune); within a second or two after release the VFO catches up (a step or two at once) |
| PA7 | A tracked pass left to end; another tracked and stopped with OK during the pass | 30 s after LOS the VFO back on the nominal downlink and uplink (ISS 437.800 / 145.990), status "Over: VFO on nominal"; with OK: toast "Tracking off: VFO stays", the VFO stays on the last corrected frequencies |
| PA8 | ISS notifications: Tools > Satellites, 0 on the ISS (page "Pass alarm" / "Geçiş alarmı"), rows For ISS, Before AOS 5 min, Min. elevation 10°, Every pass On ("Her geçiş: Açık"), OK; Back to Home | toast "Armed for every pass" / "Her geçiş için kuruldu"; the page's line "Kept across power cycles" / "Kapat-açta kalır"; at each qualifying ISS pass two beeps and "ISS in 5 min, max NN°" / "ISS AOS'a 5 dk, maks NN°", one beep and "ISS AOS now, max NN°" / "ISS şimdi AOS, maks NN°" at the rise; after the pass the page shows the next one (no re-arming by hand) |
| PA9 | PA8 armed, the radio switched off and on (GPS on, outdoors) | the page shows Every pass On, ISS, 5 min, 10° again and the next pass after the GPS fix; the next pass is announced. Every pass Off (or OK = off), off and on again: nothing armed |
| PA10 | Every pass with For on SO-50: Every pass On picks ISS; For moved to SO-50 again | For jumps to ISS ("Every pass" is for the ISS or Any satellite only); moving For to SO-50 turns Every pass Off |
| PA11 | Auto track On ("Oto. izleme: Açık"), PA8 armed, the radio left on Home on a channel (e.g. a 70 cm simplex channel) through an ISS pass | at the rise the AOS beep and banner, a second later "ISS tracking, max NN°" / "ISS izleniyor, maks NN°"; Home shows the VFO on 145.800 (± up to about 3.5 kHz, moving 1 kHz steps during the pass), FM wide, no tone; the ISS's voice / SSTV heard if active; 30 s after the set the radio back on the same channel, zone and mode as before (no key pressed) |
| PA12 | As PA11 with Auto track APRS ("Oto. izleme: APRS") | the VFO on 145.825 (± Doppler) during the pass; APRS packets via the ISS heard (and decoded by the APRS receive if the APRS frequency is 145.825); back to the channel after the set |
| PA13 | During an auto track: any key (e.g. Down, or OK); in another pass PTT | the key: toast "Auto track off: radio back" / "Oto. izleme kapalı: telsiz geri" and the radio at once back on the previous channel / VFO; PTT: no transmission (RX only refusal shown), the radio back on the previous channel after PTT is released; the next PTT transmits normally on the user's channel |
| PA14 | Auto track while busy: at the rise the radio (a) in a menu or another Tools screen, (b) scanning, (c) receiving a call, (d) in DMR / P25 search, cross-band or DOTA | no auto track (the AOS banner and beep only, the radio stays where it is); an auto track never starts while transmitting, and no banner appears while PTT is held (it comes after the release) |
| PA15 | Auto track from the VFO (a split VFO with CTCSS, narrow); a power cycle during an auto track | after the set (or a key) the VFO back with its own RX / TX, tone and width; switched off during the track and on again: the radio on the channel / VFO used before the track |
| IS-1 | Satellites hint without GPS: Tools > Satellites indoors (no fix yet) | "Waiting for the GPS time"; the hint row reads "OK alarm  Back" / "OK alarm  Geri" (before: only "Back"); OK opens the pass alarm page |
| IS-2 | Tools > ISS (the tool after All digital search, before Battery; Battery's number is one higher than before, every other tool unchanged) with a GPS fix, English then Turkish | title "ISS"; "AOS in 1h05  47°" / "AOS'a 1h05  47°", "AOS hh:mm  LOS hh:mm" (the same as Tools > Satellites' ISS row and an online predictor), Az / El, "> 145.800 voice / SSTV" ("ses / SSTV"), "145.825 APRS", "RX 145.80x +n.n kHz", rows Before AOS / Min. elevation / Every pass / Auto track with the values of the pass alarm page, hint "OK track  # APRS  1 freq" / "OK takip  # APRS  1 frek"; nothing cut off |
| IS-3 | On Tools > ISS: 1, 1 | the ">" mark moves to 145.825 APRS and back; the RX line follows (145.825 + Doppler) |
| IS-4 | On Tools > ISS: Down Down (Every pass), Right; Back; Tools > Satellites, 0; switch off and on | Every pass On arms the ISS's notifications at once (status line "Alarm hh:mm: ISS NN°"); the pass alarm page shows For ISS, Every pass On; after the power cycle still armed. Every pass Left on Tools > ISS: off (and off after a power cycle) |
| IS-4b | Arm SO-50's (or AO-91's) pass alarm on Tools > Satellites; open Tools > ISS; change Before AOS; then Every pass Right | the status line "Alarm SO-50 armed" / "Alarm SO-50 kurulu"; Before AOS changes nothing of SO-50's alarm (Satellites' alarm page still shows it); Every pass On: toast "Every ISS pass: other alarm off" / "Her ISS geçişi: diğer alarm kapalı", now the ISS's every-pass alarm |
| SK-1 | Tools > ISS with the GPS time and a fix (or a typed position, SK-5), a pass under way or the next one: key 2; English then Turkish | title "Sky ISS" / "Gökyüzü ISS"; a circle with two inner rings and a cross, N at the top (Turkish K), E right, S bottom, W left; the track a line from a filled green square (rise) to an open square (set), bowing toward the centre the higher the pass; under it "AOS hh:mm  LOS hh:mm", "Max NN°  AOS in m:ss" ("LOS in" under way), "Az ..  El .."; hint row "Back" / "Geri"; compare the rise and set directions and the top elevation with an online predictor for the same pass (a few degrees) |
| SK-2 | During a pass, SK-1 left open for a minute | the yellow dot moves along the line about once a second, the Az / El numbers with it; before the rise there is no dot, the time to the rise counts down; at the set the dot ends at the open square |
| SK-3 | Tools > Satellites, Down to another satellite, key 2; Back; key 0 | the plot for that satellite's pass (title with its name); Back returns to the list with the same row focused; 0 is still the pass alarm page; the hint row reads "OK alarm  2 sky  <> UTC" / "OK alarm  2 gök  <> UTC" (and "OK track  # APRS  2 sky" with the ISS in focus), the same at any time and without GPS time |
| SK-4 | Auto track On, an ISS pass (as PA11), at the rise open Tools > ISS and key 2; Back; then on Tools > Satellites key 2 and Back | the plot is shown with the track going on (Home later still shows the VFO on 145.800, back on the channel 30 s after the set); on Tools > Satellites key 2 and Back do not end the track (any other key there still does); the AOS pop-up and tones come as before |
| SK-7 | A typed position set (Settings > Location), GPS on but no fix yet (indoors), Tools > Satellites; then take the radio outdoors until it has a fix, and cover the antenna again | the title reads "Satellites Pos: typed" until the first fix, then "Pos: GPS" (the passes move to the real place once); after the fix is lost it stays "Pos: GPS" and the passes stay (no jump back to the typed place); a power cycle starts again from the typed one |
| SK-5 | GPS off or indoors without a fix, a typed position set (Settings > Location) but the GPS time present; Tools > Satellites and the plot | passes are listed and the plot is drawn from the typed position (altitude taken as 0); with neither a fix nor a typed position "Waiting for a GPS position" and "Needs a GPS fix or a typed position"; with no GPS time at all "Waiting for the GPS time" (the radio has no clock of its own) |
| SK-6 | The plot with Settings > Display > Large text on, English and Turkish; a satellite with "no pass soon" (PO-101 if the elements give none) | the circle, N / E / S / W and the three text lines all fit, nothing cut off or overlapping; "no pass soon" instead of a plot |
| SK-8 | Indoors with no GPS time (the clock not running), Tools > ISS; look for the row under Auto track; Down to it; OK; Back | a row "Show orbit" / "Yörüngeyi göster" with ">" under Auto track, visible at once (not only after a wait); with it in focus the bottom hint reads "OK orbit  # APRS  1 freq"; OK opens the plot page with "Waiting for the GPS time"; Back returns to the ISS page |
| SK-9 | As SK-8 with the GPS time and a fix; also key 2 from the page; then Settings > Display > Large text on and again | OK on Show orbit and key 2 both open the same plot; Back returns to the page; the ISS page looks the same with Large text on (only Home changes) and the row stays reachable with Up / Down (Up from the first row wraps to it) |
| SK-10 | Auto track On, an ISS pass in progress with the track held, Tools > ISS: Down to Show orbit, OK; Back | the plot opens and the track goes on (Home later still shows the VFO on 145.800); Back returns to the ISS page; OK on another row still stops the track |
| SK-11 | Tools > Satellites with no GPS time (indoors), then with it | its bottom hint names "2 sky" ("2 gök") in both states and does not change over time; without GPS time the two lines "Waiting for the GPS time" / "Needs a GPS fix or a typed position" are all inside the screen (the second wraps); key 2 shows the plot's waiting message |
| IS-5 | On Tools > ISS with the next pass more than 10 min away: OK | toast "Track from AOS-10 min" / "AOS-10 dk'dan takip"; nothing tuned |
| IS-6 | On Tools > ISS within 10 min of AOS (or during a pass), the radio on a 70 cm channel: OK; Back to Home; leave it through the pass | within about 2 s the banner "ISS tracking, max NN°" / "ISS izleniyor, maks NN°" (no toast before it); Home on the VFO 145.800 (± Doppler, 1 kHz steps), FM wide, no tone, PTT refused (receive only); 30 s after the set the radio back on the same channel, zone and mode (no key pressed) |
| IS-6b | Refusals: OK on Tools > ISS during a pass (a) while the radio scans or dual-watches, (b) while Tools > Satellites tracks the ISS by hand (OK there first) | (a) toast "Can't track: radio busy" / "Takip olmaz: telsiz meşgul" within about 4 s, the scan goes on, nothing tuned; (b) toast "Can't track: a track runs", the hand track goes on |
| IS-6c | During a held track (IS-6) with a GPS fix and the callsign set, APRS beacons on (a short interval): wait two beacon intervals; then # on Tools > ISS while the ISS is above the horizon | no beacon is sent during the track (no TX LED); # keys once on 145.825 + Doppler (path ARISS) and the radio is back on the tracked 145.800 + Doppler right after, the track going on; the held beacon goes on the APRS frequency after the track ends |
| IS-7 | As IS-6, on Tools > ISS during the track: 1, then OK | the VFO moves to 145.825 (± Doppler) within about a second and keeps following; OK: toast "Auto track off: radio back", the channel back at once |
| IS-8 | Pop-up: Every pass On (IS-4), Auto track Off, the radio on Home on a channel at the rise (Tools > ISS not opened since power on) | one beep and the banner "ISS overhead, max NN°" / "ISS tepede, maks NN°" with "OK: track" / "OK: takip", a 30 s countdown and Home's hint row "OK track  Back" / "OK takip  Geri"; OK: the pop-up goes, within about 2 s "ISS tracking, max NN°", then as IS-6 (the VFO on the downlink chosen on Tools > ISS) and the channel back 30 s after the set; at the next pass the same again |
| IS-9 | Pop-up: at another rise, Back; at a third, Down, the knob, a long OK; at a fourth, nothing for 30 s; at a fifth, in a menu (e.g. Settings) | Back: the banner goes, nothing tuned; Down / knob / long OK do what they do on Home (channel, volume, ...) and the pop-up stays; after 30 s it goes by itself; in a menu: the plain "ISS AOS now, max NN°" banner for 8 s, OK and Back are the menu's |
| IS-10 | Pop-up while transmitting: PTT held (authorised frequency) across the rise, then released | no beep and no banner during TX; after the release the beep and the pop-up; OK tracks as IS-8 |
| IS-11 | Pop-up with Auto track On (PA11) | no pop-up: "ISS AOS now, max NN°", then the auto track starts by itself as before |
| IS-12 | Emergency during a held / auto track (holehunt B1): the radio on a DMR channel with an ID before the track (IS-6 or PA11), then the emergency key (long press) while it tracks; repeat from an FM channel; repeat with SOS / the lone worker set up (Tools) | DMR channel: the track ends at once (the channel back, no "Auto track off" toast needed) and the emergency goes out there (TX LED, hot mic) as on Home; cancel with the key. FM channel: the channel back, then exactly what the emergency key does on that FM channel outside a track (the emergency fails, nothing keyed); SOS / lone worker fall back as on an FM channel. Never an emergency or TX on 145.800 / 145.825 |
| IS-13 | A held track whose end is lost (holehunt B6; cannot be forced on the radio: watch for it): after any track (LOS, a key, PTT) check Home | the radio back on its channel within about 2 s in every case; PTT transmits normally and a channel change is still there after a power cycle (saving works again) |
| IS-14 | # APRS via ISS during a held APRS track mid pass while 145.825 is busy (holehunt B8): # on Tools > ISS, the frame waits for a clear channel | the frame goes out once the channel is clear (TX LED once), even if that takes many seconds; the VFO does not step meanwhile and catches up the Doppler within a second or two after the frame |
| IS-15 | An emergency running (DMR channel, emergency key held, then left "in emergency") across an ISS rise with the pass alarm armed: (a) auto track Off, (b) auto track On; then SK1 set to hold = Emergency and held during a held track | (a) the plain "ISS AOS now" banner, no pop-up, OK not taken; (b) no auto track starts, the radio stays on the emergency channel and still transmits on PTT; SK1 held during a track: the track ends and the emergency goes on from the user's channel (as IS-12) |
| IS-16 | Emergency key (long press) during the DMR search, the all digital search (sweep and while a DMR carrier is probed), the P25 search's sweep and DMR trunk following, each started from a DMR channel with an ID; repeat the DMR search from an FM channel | within about 0.3 s the search / following ends, the channel is tuned again (its frequency and color code) and the emergency goes out there (TX LED, hot mic); from the FM channel the channel is tuned again and the emergency fails (SOS / lone worker fall back); never TX on a search frequency |
| M1 | Pager build: Tools > Pager on an FM channel with no pager traffic, 5 min; then Back and a normal voice call | the line above the list says "Listening on this frequency" within a second (not "Waiting for receive audio"), no message appears; while the screen is open FM audio sounds flatter / brighter (the flat path), after Back it sounds as before (F1) |
| M2 | A POCSAG transmitter in range (a local paging network, or a POCSAG test signal at 512, 1200 and 2400 bit/s from a signal generator / SDR transmitter into a dummy load) with Tools > Pager open on its frequency | each page listed as "capcode: text" with its age; OK shows capcode, function, rate, text; numeric pages show digits, alphanumeric pages text; no "Received with errors" on a strong signal. Note which rates decode and the weakest level that still does |
| M3 | During M2: the pager's signal at 512 bit/s (most affected by the audio path) | decodes; if not, also try with the screen opened before the transmission starts (the flat path is set when the screen opens) |
| M4 | ACARS: Tools > Air band, 131.725 (Europe) or 131.550 MHz, near an airport, then Tools > ACARS | "Listening on this frequency" (the receive ring runs in AM: AM audio reaches the HR_C7000's RX RAM); blocks listed as "registration label: text" within minutes of traffic (the ACARS bursts are audible as short buzzes). "Waiting for receive audio" that stays: the AM audio does not reach the RX RAM (report it) |
| M5 | During M4: OK on blocks | registration (e.g. TC-JFV), flight ID on downlinks (e.g. TK1234), label, block, text that reads sensibly. "Received with errors" on every block (parity right, BCS wrong) means the BCS variant assumed (CRC-16 0x8408, start 0) is wrong: report it |
| M6 | During M4: the voice on the air band channel with the ACARS screen open, then Back | AM voice still plays (now through the HR_C7000's FM playback while the ring runs; level and clarity as before), back to the direct path after Back |
| M7 | Pager screen open, then switch to an NXDN channel, a DMR channel, the APRS frequency and back; then `tools/plus_txdiag.py --health` | the NXDN channel still receives (its flat path not undone by the pager's), APRS still decodes, no stuck audio; heap_min_free > 1 KB (ACARS: 2 KB ring + ~220 B decoder while its screen is open) |
| AP1 | APRS messages build, gate: callsign cleared (radio name empty / N0CALL), Tools > APRS > New message, To and Text filled, Send; same with SOS (emergency) > Send SOS | toast "Set your callsign (the radio name) in qdmr." (or the line above the rows says so); nothing keyed (red LED off, the other radio hears nothing); SOS row stays "Off" |
| AP2 | Callsign set, a channel on the APRS frequency (144.800) with APRS receive on. New message: To = the owner's other radio's APRS call (or a station that answers, e.g. an iGate-covered one / ANSRVR), Text typed with the keypad (# switches ABC/123/abc, Back deletes); Send | one AFSK burst on 144.800; the line reads "Sending to X, try 1 of 3", toast "Message sent, try 1 of 3"; the other radio (or aprs.fi > Messages for an iGate-covered station) shows the text from our call |
| AP3 | The receiving station acks (Kenwood / Yaesu / APRSdroid do by themselves) | "Delivered to X", toast "Message delivered", no further tries; New message row value "Delivered" |
| AP4 | Send to a call that does not exist / the other radio off | tries at about 0, 30 and 90 s ("try 2 of 3", "try 3 of 3"), then about 2 min later "Not delivered to X: no ack" and the toast; nothing more keyed. A second Send while it is still trying: toast "Wait: the last message is still going" |
| AP5 | Same as AP2 but the channel NOT on the APRS frequency | the message goes out (the job retunes to 144.800 and back) but the ack is not heard: the line adds "Acks are heard on the APRS frequency only" and it ends "Not delivered" (expected; upstream receives APRS only on its frequency) |
| AP6 | Canned text: New message > Canned text > "QSL" (and 1-8 digits) | Text row shows QSL; Send delivers "QSL" (AP3) |
| AP7 | The other radio sends us a message (to our call-SSID, e.g. TA1ABC-7) while on the APRS frequency | toast "New APRS message"; Messages row "1 new" (warning color); Messages lists "FROM: text" with its age, unread in the warning color; OK opens From / age / text (row now read); # (or Reply) opens New message with To = the sender and an empty text; we ack it automatically (the other radio shows delivered) |
| AP8 | SOS: Tools > APRS > SOS (emergency), OK | the confirmation page "Send an emergency position now and every 2 minutes until turned off?"; Back leaves it Off (nothing keyed). OK on "Send SOS" without a position (indoors, no fixed position): toast "No position: GPS or a fixed one", still Off |
| AP9 | SOS with a position (GPS fix or Tools > APRS > Position fixed): OK on "Send SOS" | toast "SOS on: sending", one burst at once, then every 2 min; the row reads "On, next m:ss" (warning color), toast "SOS position sent" each time. The other radio (Kenwood / Yaesu: emergency alarm) or aprs.fi (an iGate in range) shows our position as **Emergency** with comment "SOS", within about 0.01' of the true position |
| AP10 | SOS row OK while on; then power off / on with it on | toast "SOS off", no more bursts (watch 5 min); after a power cycle the row is Off and nothing goes out. Also: the other radio sends a Mic-E Emergency (its own SOS) while we are on the APRS frequency: toast "EMERGENCY: <its call>" |
| SV1 | SSTV build: Tools > SSTV on an FM channel with no signal (squelch closed, then opened with monitor), 5 min; then a voice call on it | "Waiting for VIS / signal" within a second (not "Waiting for receive audio"; on an AM or digital channel "SSTV: tune an FM channel"); the picture area stays black: no false picture from noise or voice; FM audio flatter / brighter while the screen is open (the pager's flat path), as before after Back |
| SV2 | An SSTV signal at close range into a dummy load (a phone app such as Robot36 / an SDR or another radio sending PD120 on a simplex FM channel; licence permitting), Tools > SSTV open before it starts | the title shows "PD120" after the VIS (about 1 s of leader tones), the picture builds from the top, "Line n/496" counts, ends "Received" and stays; colours right (not swapped or tinted: compare with the source picture) |
| SV3 | Same with Robot 36, Martin M1 and Scottie S1 (and any other mode the sender has) | each named in the title and drawn; Robot 36 builds a row about every 0.2 s |
| SV4 | Picture quality and slant: a test pattern with vertical lines (colour bars) | lines straight (not leaning; "Clock" under the line count reads a few hundred ppm or less: the sender's and our sample clocks); edges not smeared more than a pixel or two; no band of missing rows. Note the clock reading |
| SV5 | Open the screen in the middle of a transmission (VIS missed) | a picture starts on the line syncs within about 4 lines (the mode in the title), from the top of the screen |
| SV6 | ISS SSTV event (ARISS, PD120 on 145.800 MHz FM, announced on ariss.org): VFO on 145.800 (Satellites can follow Doppler: U6), Tools > SSTV during a pass | pictures received; a Doppler step of the VFO does not restart the picture (a retune up to 10 kHz keeps it); note how far into the pass they decode and the noise speckle |
| SV7 | Back during a picture, reopen; a call banner while a picture comes; then `tools/plus_txdiag.py --health` | Back leaves at once, reopening starts with an empty picture (one still on the air is picked up on its syncs, SV5); a banner covers only the text under the picture; heap_min_free > 1 KB (the screen takes ~1.5 KB of heap on top of FM caller ID's 2 KB ring) |
| ST1 | SSTV send build: Tools > SSTV > OK on an FM simplex channel in the amateur band (e.g. 144.500 or 433.400 MHz, low power, a dummy load or a short range; licence permitting), the radio name a callsign (TA2DOU) | "SSTV send" page: the card at half size with the mark, "DDM32" + "Plus" and **your callsign**, "< Robot 36 >  37 s", "TX 144.500.00  TA2DOU", "OK send  <> mode"; Left / Right steps Martin M1 1:56, PD120 2:08 |
| ST2 | OK with Robot 36; receive it with a phone app (Robot36 on Android, or MMSSTV / QSSTV on a PC) held near another radio's speaker, or with another DM-32 on Tools > SSTV (SV2) | red LED, the progress bar and "Sending n/240"; the receiver shows "Robot 36" from the VIS and draws the card: callsign readable, logo and colour bars right (not tinted or swapped), lines straight (no slant), no tearing; after about 37 s "Picture sent", back to receive, the LED off |
| ST3 | Same with Martin M1 and PD120 | each named by the receiver from its VIS and drawn whole; colours right; the radio unkeys at the end by itself |
| ST4 | Back part way; send again and press PTT part way; send again and turn the channel knob part way | each time the carrier drops at once (the receiver's picture stops), "Sending stopped"; the PTT press keys no voice (the next one does); the knob's channel change happens after the stop |
| ST5 | The checks: a repeater channel (RX and TX differ), a DMR channel, a receive-only channel, the radio name not a callsign, Settings TX time-out 30 s with Robot 36 (145.600 / 145.800 / 144.800 removed 2026-11-22: SSTV send has no band plan, owner decision) | each refused with its toast ("Repeater channel (RX and TX differ)", "SSTV: tune an FM channel", "This channel is RX only", "Set your callsign...", "TX time-out shorter than the picture"); nothing keys |
| ST6 | Deviation and audio: listen on another radio (or an SDR) | clean tones, no clicks at frequency changes, no microphone audio mixed in, level like an APRS packet; note whether the receiver decodes on a 12.5 kHz (narrow) channel as well as on 25 kHz |
| ST7 | Turkish menus: the same page | "SSTV gönder", "OK gönder  <> mod", "Gönderiliyor n/240", "Resim gönderildi"; nothing cut off |
| ST8 | After a few pictures: `tools/plus_txdiag.py --health` | heap_min_free as before (48 B taken while sending, given back); no crash report |
| ID1 | ID database build (built with research-data/user.csv: the build output says "ID database: 8694 IDs ...", not "SAMPLE"): Settings > Advanced > About, scroll to the end; then a DMR call to us / our talkgroup from a Turkish DMR ID that is **not** in the codeplug contacts (the other radio with its own registered ID, or a station on a repeater) | About ends "ID database: N, date, RadioID.net"; the call screen's big line reads "CALLSIGN Name" (e.g. "TA1ABC Ahmet": the ID's RadioID.net callsign and first name, Turkish letters as plain c g i o s u), the line under it the ID and TS; the Home banner names the caller the same way |
| ID2 | The same ID saved as a codeplug contact (CPS, name e.g. "Ahmet Ev"), call again | the contact name shows, not the database's (the contact wins); the talker alias (if the caller sends one and Show talker alias is on) still shows on its own line in quotes |
| ID3 | After ID1: Recent (call history), and Home's CONTEXT hint / Last caller | the entry shows "CALLSIGN Name" with its age; CONTEXT / call back offers it by that name; a private call back dials the right ID |
| ID4 | A call from an ID not in the database (a non-Turkish ID, or a talkgroup only) | unchanged from before: the bare ID (or the CPS-uploaded database's text, if one was ever uploaded); talkgroups keep their contact names; no delay or audio gap at call start in ID1-ID4 |
| RR1 | RAM reclaim build (docs/plus.md, third round): boot, then `tools/plus_txdiag.py --health` | splash and Home as before; no crash record (the start-up task's heap check passed: a failure would restart with a crash naming core/app/app.c); `tasks.free.host` > 300 B; `heap_min_free` about as R12c |
| RR2 | Channel switching with calls: P25 (call) > DMR (call) > YSF > NXDN > FM (call) and back the other way, a few times, a call wherever a source exists | each mode's audio as before; no silence, stuck audio or garbled first call after a switch; no reset |
| RR3 | Dual watch P25 + DMR, P25 + FM, DMR + FM; calls on each pane | each call heard and its pane named as before (N3) |
| RR4 | Zone scan over a zone mixing FM, DMR, P25 (and NXDN, YSF) channels; calls on two of them | the scan stops on each call and resumes after, as before |
| RR5 | Host link at its deepest: CPS read of the codeplug, a CPS write of one channel, the debug link task view; then `--health` | all as before; `tasks.free.host` > 300 B; no crash record |
| RR6 | An hour of use (R12c pattern: P25 calls, a DMR call, GPS on, RR2 switches) then `--health` | no reset, no crash record; `heap_min_free` about as R12c (the reclaim did not touch the heap's free room) |
| SM1 | FM caller ID off: Settings > Talk & listen > FM caller ID = Off; an FM channel idle 2 min, then a voice call; the other radio sends DTMF "123#" (F2) and an MDC / FleetSync ID if a source exists (F3, F5) | the row reads Off; FM audio on the direct path again (level and sound as before the FM ID build, unlike F1's HR_C7000 playback); no MDC / FleetSync / DTMF line on Home, nothing new in Recent; On again: F2 works at once |
| SM2 | FM caller ID still Off: Tools > Pager with a POCSAG source (M1 / M2), then Tools > SSTV with an SSTV source (SV1 / SV2) on the FM channel; DTMF from the other radio meanwhile | both screens listen and decode as with it On ("Listening on this frequency", pages, pictures); no DTMF / ID line on Home while they run; after Back the FM audio is on the direct path again |
| SM3 | FM caller ID Off, power off and on; then `tools/plus_txdiag.py --health` | the row still reads Off after the power cycle and no ID is decoded (SM1); other settings and "Use anyway" (if it was chosen) unchanged; no crash record |
| SM4 | ISS APRS gates: Tools > Satellites, ISS in focus; # with the ISS below the horizon; then with the callsign cleared (radio name empty); then without a GPS fix and no fixed position | hint row "OK track  # APRS via ISS" ("OK alarm  # APRS via ISS" with no pass within 10 min) on the ISS only; toasts "ISS below the horizon: wait for its pass", "Set your callsign (the radio name) in qdmr.", "No position: GPS or a fixed one"; nothing keyed |
| SM5 | During an ISS pass with the digipeater active (ariss.org / aprs.fi), licence permitting, elevation above 10 degrees: # once, # again within 30 s | toast "Sent via ISS (ARISS)", one short burst on 145.825 MHz corrected for Doppler (145.822-145.829; check with another receiver or SDR if at hand), then the radio back on its channel; the second press "ISS: 30 s between sends", nothing keyed; heard if digipeated: aprs.fi raw packets for our call with path ARISS* (or via RS0ISS), or ariss.net. To hear it on the radio: region custom 145.825 and a channel there with APRS receive |
| TK1 | P25 trunk build: a P25 channel on a lawfully receivable P25 Phase 1 trunked system's control channel (VHF/UHF; NAC Any or the system's, talkgroup Any), Tools > P25 trunk, OK (where such a system may be heard is the owner's call) | within a second or two "Control locked"; "TSBK n/s" steady (a busy control channel: 20-40/s), "bad" grows slowly or not at all; no audio from the control channel |
| TK2 | Same, after a few seconds | "WACN xxxxx SYS xxx NAC xxx" and "Site r-s" match what the system is known to send (RadioReference or another decoder); grants appear in the list with frequencies (not "?") once the channel plan (IDEN_UP) has come |
| TK3 | A group call on a followed talkgroup (the channel's TG, or every group via Left / Right) | "Voice f" with the grant's frequency, the call heard clearly from its start, Home caller line / call log / Tools > Last call have it; after the call "Control locked" again within about a second, and the next call is followed too |
| TK4 | A call on another group (TG not followed) while idle, and while a followed call plays | the list shows it with "-" (or "busy" during a call); the receiver stays; the playing call is not interrupted |
| TK5 | An encrypted call (if the system carries any) | its grant shows "ENC" and is not followed; if one is followed anyway (no encryption bit in the grant) it stays muted with the existing encrypted notice |
| TK6 | PTT while following (on the control channel, and during a followed call) | TX denied (RX only) banner, red LED off, nothing on air (another receiver hears nothing); following goes on; release: as before |
| TK7 | Timing: from the start of a followed transmission (watch the other decoder / scanner) to its audio here | note the delay (expected about 0.2-0.4 s plus the system's own call setup); note any first syllables lost |
| TK8 | Control channel lost: detune (step the antenna away / shield) or a frequency with no system, then back; also Back and leaving the screen | "Searching" after about 3 s without TSBKs, "Control locked" again when it returns; Back stops at once, leaving the screen stops within about 5 s, and the selected channel is received as before (P25 conventional receive on it again) |
| DT1 | Plus build with DMR trunk: a DMR channel on a lawfully receivable ETSI Tier III site's control channel (TSCC; its color code or Any, RX group list Any or the groups wanted), Tools > DMR trunk, OK (where such a site may be heard is the owner's call) | within a second "Control locked"; "CSBK n/s" steady (a TSCC sends on both slots: up to about 33/s), "bad" grows slowly or not at all; no audio from the TSCC; the radio does not show RX all the time |
| DT2 | Same, after a few seconds | "Small/Large/... Net n Site s" matches what the site is known to send (another decoder, e.g. DSD-FME / SDRTrunk); "LPCN n" counts the channels whose frequency the site announced (Chan_Freq); grants appear in the list with frequencies (not "ch n") |
| DT3 | A group call on a followed talkgroup (the channel's TG / RX group list, or every group via Left / Right) | "Voice f TSn" with the grant's frequency and slot, the call heard clearly, Home caller line (with the ID database name) / call log / Tools > Last call have it; after the call "Control locked" again within about 2 s, and the next call is followed too |
| DT4 | A call on another group while idle, and while a followed call plays | the list shows it with "-" (or "busy" during a call); the receiver stays; the playing call is not interrupted |
| DT5 | An encrypted call (if the site carries any) and a private call | encrypted: listed "ENC", muted, the receiver back on the TSCC at once; private: "priv" unless every group is chosen (then followed) |
| DT6 | PTT while following (on the TSCC, and during a followed call) | TX denied (RX only), red LED off, nothing on air (another receiver hears nothing); following goes on; release: as before |
| DT7 | Timing: from the start of a followed transmission (watch the other decoder) to its audio here | note the delay (expected about 0.1-0.2 s plus the site's call setup; 0.4-0.7 s when joining a call already running); note any first syllables lost |
| DT8 | A site whose grants all show "ch n" (no Chan_Freq heard), and one of Capacity Plus / Connect Plus / XPT if at hand; TSCC lost (detune or shield, then back); Back and leaving the screen | "ch n" grants are listed, not followed (expected: such a site needs a channel plan typed in, not built); vendor trunking stays "Searching" with no grants (not Tier III); "Searching" after about 3 s without CSBKs, "Control locked" when it returns; Back stops at once, leaving the screen within about 5 s, and the channel is received as before |
| DP1 | dPMR build: a channel on the dPMR446 radio's frequency (e.g. 446.10312 for its channel 1: the record keeps 10 Hz steps), Mode dPMR, CC Any; the dPMR446 radio (licence free, its own antenna) at close range calls | the green LED while it sends; Home's bottom line "dPMR <its ID> > <called> CC n" (or "CID n > CID n", "> ALL") from the first second (the header), not only after ~0.3 s (late entry); voice clear, no analog noise or 4FSK hiss from the speaker; note the CC number shown |
| DP2 | A long call (30 s+), voice quality and level against the same audio on another dPMR radio | no dropouts, no warble; level like a DMR call; the line stays until the end and goes after PTT release (the end frame) |
| DP3 | The channel's CC set to the number DP1 showed, then to another one; calls each time | the right code plays as DP1; the wrong code stays silent (no line on Home, no audio) |
| DP4 | Turn the receiver on (switch to the channel) in the middle of a call | the caller appears within about half a second (late entry) and the voice follows |
| DP5 | Recent (call history) after DP1-DP4; PTT on the dPMR channel | entries "dPMR <ID>" with "> <called> CC n" and their age, no call back offered; PTT: TX denied banner, nothing keyed |
| DP6 | Zone scan over a zone with the dPMR channel (and NXDN, FM ones), a dPMR call; then switch dPMR -> NXDN -> FM -> dPMR channels a few times; `tools/plus_txdiag.py --health` | the scan stops on the dPMR channel's carrier like an NXDN channel; each channel receives after the switches (no stuck flat path or silence); no crash record, heap_min_free as before |
| RP1 | Repeater list build (with research-data/rptrs.json: the build output says "Repeaters: 100 (DMR 100) ...", not "SAMPLE"); a GPS fix (or a fixed position), Tools > Repeaters (with the province list, RP7: OK on "Hepsi") | "Nearest first"; the top row is the nearest repeater to where you are (e.g. in Istanbul a YM1 / YM2 Istanbul one), each row "callsign ... 12.3 km NE" and "439.1625 DMR CC1 TS1+2 City"; Up / Down walk the list in growing distance, round from the end to the start; no lag on a key press |
| RP2 | GPS off and no fixed position (Settings > Location), the screen again | "No position: sorted by frequency" (warning colour), 145.xxx first, rows without a distance; with the fix back the order turns nearest first within a few seconds |
| RP3 | OK on a DMR repeater, then OK again (tune) | details: name and DMR, city and locator, "RX 439.1625  TX 431.5625", "Offset -7.600 MHz", "CC1  TS1+2", distance and degrees, "OK: tune the VFO"; tune: Home in the VFO on 439.1625, DMR, CC1, TS1 (TS2 for a TS2-only repeater), TX 431.5625 (check in the VFO's settings); key up on the repeater's talk group (licence, own frequency rules) and the repeater answers |
| RP4 | An FM entry (own research-data/repeaters.csv with an FM repeater and its tone, rebuilt), OK, OK | VFO FM on its output, TX = output + offset, TX CTCSS = its tone, no RX tone, 25 kHz (12.5 kHz when bw 12.5); key up: the repeater opens; a previous VFO split does not stay |
| RP5 | # on the list; a C4FM / D-STAR entry (sample or own list) OK, OK; tune while transmitting is not possible by hand, skip | # opens the nearest one's details; C4FM: "Info only: not a mode of this radio", nothing tuned |
| RP6 | Back from the details, Back from the list; Turkish UI | the list, then Tools (with the province list: the list, the province list, then Tools; RP10); no crash record, heap_min_free as before (`tools/plus_txdiag.py --health`) |
| RP7 | Province list build (the build output's Repeaters line ends "60 provinces, unknown 0"), Turkish UI, Tools > Röleler | first a list: "Hepsi (252)" on top, then "01 Adana (13)", "03 Afyonkarahisar (1)", "05 Amasya (4)", "06 Ankara (8)" ... in plate order, only provinces with repeaters (no "02 Adıyaman", no "(0)"), "34 İstanbul (15)", "35 İzmir (19)", "46 Kahramanmaraş (4)", "63 Şanlıurfa (3)" with the Turkish letters as on the map; every name whole, the count visible on every row; no "Bilinmeyen" row (unknown 0); hint "^v seç  OK liste ..."; Up / Down / the knob go round; no lag scrolling the 61 rows |
| RP8 | OK on "06 Ankara (8)", then on your own province and a far one (e.g. "65 Van") | title "Röleler Ankara"; only Ankara's repeaters, nearest first with distance and direction (with a fix; "Konum yok: frekansa göre sıralı" without), Up / Down round within the 8; the far province's list nearest first too (hundreds of km); # opens that province's nearest, not the overall nearest |
| RP9 | In a province's list OK on an FM repeater, OK again (tune; no key up needed) | details as RP3 / RP4; tune: Home, VFO on its output with its tone and offset, toast "VFO röleye ayarlandı"; nothing transmitted |
| RP10 | Back from the details, Back, Back; then # on the province list, Back, Back | details -> the province's list -> the province list with the cursor still on that province -> Tools; #: the overall nearest repeater's details, Back: Hepsi's list, Back: the province list |
| RP11 | Tools > Harita, # (pick a repeater), OK; Up / Down; Back; auto repeater (MN5 / MN6) once | the details at once (no province list), Up / Down walk the whole list, Back returns to the map with the pick; the auto repeater's offer and # tune work as before; `tools/plus_txdiag.py --health`: no crash record, `tasks.free.ui` about 368 B as before |
| MD1 | Tools > Harita (Map), whole Turkey (zoom 0), then the knob / 3 one step at a time to the closest (zoom 8), around Istanbul and at home | zoom 0-2 as before (outlines, province names from zoom 2), no district dots; zoom 3: small dots (district centres) and "GeoNames" in the bottom right corner; zoom 4+: district names next to the dots in the tiny font, province names brighter; no two names overlap (some are left out where it is busy: zoom in and they appear); 9 zoom steps in all, the scale goes down to about 10 km (zoom 8: ~40 m a pixel); APRS stations green squares, repeaters blue triangles, both clearly apart from the small grey district dots; note the draw time at zoom 3-4 over Istanbul (key press to new picture) |
| MD2 | District names: pan around Hakkari, Samsun, Antalya, Istanbul (Anatolian side), Gümüşhane | Turkish letters correct (Kadıköy, Şemdinli, Ünye, 19 Mayıs, Kağıthane: no â), Hakkari has 5 districts (Derecik among them), province-centre districts carry the province name (e.g. "Gümüşhane") |
| MD3 | Outdoors with a GPS fix, the map following | under the map "34 İstanbul / <district>" (the nearest district centre in our province; near a district border it may name the neighbour: note it); without a fix "No GPS fix" as before |
| MD4 | Repeater list build (RP1), the map at zoom 0 and zoom 4 near Istanbul / Ankara | small triangles (channel-number colour) at the repeaters' places (city / town centres from places.csv), different from APRS dots and waypoint boxes; the hint ends "# repeater" |
| MD5 | Pan away from the GPS position (the dim cross marks the centre), # | the repeater nearest the centre is drawn bigger with its callsign; the line under the map "TA1ABC 439.1625 12.3 km NE" (way from the centre); hint "OK details  # next  Back clear"; # again: the next nearest, after the farthest the nearest again |
| MD6 | With a pick: OK; in the details Up / Down, then Back | Tools > Repeaters opens on that entry's details (RX / TX, CC / slot, distance from us); Up / Down step through the list order; Back returns to the map (not the repeater list), the pick still shown |
| MD7 | From MD6's details OK (tune), then back to Tools > Map; Back with a pick, Back again | the VFO tuned as RP3 and Home shown; on the map Back drops the pick (line back to the province), the next Back leaves the map |
| MD8 | `tools/plus_txdiag.py --health` after MD1-MD7; Tools > Repeaters opened from Tools (not the map) | no crash record, `tasks.free.ui` about as before (368 B in the stack check), heap_min_free as before; from Tools the list opens on the nearest as before and Back from details returns to the list |
| NC1 | Factory radio NAC typed "293" (decimal there); this radio: P25 search, then the channel editor's NAC row: OK, # # 2 9 3 OK, save | P25 search lists "$125/293"; the row reads "$125 = 293" after the decimal entry; the two radios talk; Home's caller line shows "NAC $125/293" |
| NC2 | NAC in both forms (the other radio on NAC $659 = 1625, TG 101): a call on a P25 channel; Home in Large text too; dual watch with a P25 channel; Recent; Tools > Heard list (row, OK on it, OK tune); Tools > All digital search; P25 trunk on a control channel if one is around; VFO settings TX NAC 1625 with RX NAC 659, then Home | every NAC reads hex with "$" and the decimal: caller line "NAC $659/1625" (tight "NAC$659/1625"), the pane "NAC $659/1625", Recent "TG 101 $659/1625", Heard row "TG101 $659/1625 x.. .." (where it is too narrow "TG101 x.. .."), its page and the tune toast "$659/1625", All digital search "P25 NAC $659/1625", P25 trunk "SYS ... NAC$xxx/ddd"; Home's VFO row "NAC$293/659>$659/1625 TG101" (Large text: "NAC659>1625 TG101"); nothing cut or overlapping |
| HS1 | Hotspot build (docs/plus.md, "MMDVM hotspot"), licence permitting, low power: MMDVMHost / Pi-Star / WPSD on the cable's port at 115200 (`Duplex=0`, RX = TX on a hotspot frequency the band plan allows, DMR CC1, network slot 2 only, RFLevel 10-25), the radio on a DMR channel; then the same started from a P25 channel; Tools > Hotspot meanwhile; a call from the other DMR radio (TS2, the talkgroup routed by the host) and one from the network; OK to stop; then MMDVMHost stopped without OK; `tools/plus_txdiag.py --health` after (MMDVMHost stopped) | the host log shows the modem ("RadioFoundry DM-32UV ...", protocol version) and no "modem not responding"; the radio switches to the hotspot screen by itself, Tools > Hotspot reads On; the RF call reaches the network (host log / dashboard), the network call is heard on the other radio with no gaps; PTT does nothing; OK: back to the channel (on the P25 channel the P25 receiver hears calls again); stopping the host without OK: back to the channel about 20 s later; no crash record, `tasks.free.host` > 300 B, heap_min_free as before |
| RS1 | Mode switching soak around the RAM changes: P25 (call) > DMR (call) > YSF > NXDN > dPMR > FM (call) and back, a few times; APRS receive on an APRS frequency (a packet heard) and an FM caller ID / pager decode between them; start and stop the hotspot (HS1) from a DMR, a P25 and an FM channel in between; then `tools/plus_txdiag.py --health` | each mode's audio as before (no silence, stuck or garbled first call after a switch or after hotspot mode); APRS packets still decode (its receive ring is the stream's, unchanged); no reset or crash record; heap_min_free as before |
| LW1 | Lone worker (no transmission): Tools > Lone worker, Up / Down | the "On alarm" row steps Sound only / DMR emergency / APRS SOS; the last two in the warning color with "then the alarm and it TRANSMITS"; after power off and on it is Sound only again and the time Off |
| LW2 | Sound only, 5 min, no keys (as R11), another receiver on the channel | warning at 5:00, alarm at 5:30, nothing transmitted (the other receiver hears nothing, no TX indicator); any key ends it |
| LW3 | DMR emergency chosen, an **FM** channel selected (no transmission expected; still watch the other receiver), 5 min, no keys | the alarm sounds; from the second beat the toast and the Lone worker screen say "Not sent: needs a DMR channel"; nothing transmitted; a key ends the alarm with no "Emergency canceled" toast |
| LW4 | **TRANSMITS: needs the owner's approval for this step and a DMR channel on a frequency / talkgroup where an emergency alarm is authorised (a simplex test channel with the owner's other DMR radio, not a repeater or network talkgroup).** DMR emergency chosen, that channel selected, 5 min, no keys | at 5:30 the alarm; the radio sends the emergency alarm (3 bursts) and then keys by itself for about 10 s (hot mic; the other radio shows the emergency / hears the room); "DMR emergency sent" on the toast and the screen; exactly one such sequence while the alarm goes on for a minute |
| LW5 | **TRANSMITS (as LW4).** In LW4's alarm, during the 10 s hot mic: press a key (not PTT) | the transmitter drops at once, toast "Emergency canceled", the alarm tone stops, the time starts again; a normal PTT call afterwards is an ordinary call (no emergency on the other radio) |
| LW6 | **TRANSMITS (as LW4).** Again to the alarm; press PTT instead of a key, talk, release; then any other key | PTT stops the tone, the call still shows as an emergency call on the other radio; the next other key: "Emergency canceled" |
| LW7 | APRS SOS chosen, indoors without a GPS fix and no fixed position (no transmission expected), 5 min, no keys | the alarm sounds; "SOS not sent: no position"; nothing transmitted; with the APRS callsign empty instead: "SOS not sent: APRS not ready" |
| PX1 | **TX (the other radio): owner approval and an authorised frequency.** P25 extras build: the owner's other P25 radio sends an emergency call (its emergency key / alarm, if it has one) on the 433.500 channel's talkgroup | the call plays; Home's activity row says ACİL DURUM (EMERGENCY) in the warning colour, the bottom line "ACİL DURUM TG 101 ID <id> NAC $659/1625" (or a shorter form); Menu > Recent: the entry "P25 <id>" "ACİL DURUM ..." in the warning colour; the next ordinary call shows "P25 TG 101 ..." again |
| PX2 | **TX (the other radio): as PX1.** The channel's P25 Radio ID set (editor) to e.g. 1234; the other radio makes a private (unit-to-unit) call to 1234 | the call plays on the talkgroup channel; Home "P25 Özel <id> > 1234 NAC ..." (Private); Recent "Özel > 1234" (narrow: "> 1234") |
| PX3 | **TX (the other radio): as PX1.** A private call to another ID (e.g. 5555), same channel | shown on Home ("Özel <id> > 5555"), not played; Recent lists it |
| PX4 | **TX (the other radio): as PX1.** PX3 again on a channel with talkgroup Any (RX only), and in P25 search | played |
| PX5 | Dual watch (and multi-watch) with the P25 channel; PX1's emergency call | the pane / row names it "ACİL DURUM TG101 ID<id>" |
| PX6 | **TX (the other radio): as PX1.** If the other radio is a Motorola (or any radio sending vendor link control words, e.g. talker alias): a 20 s call, compared with the build before | no 0.36 s dropouts every few superframes (an LDU1 with such an LC used to mute its superframe); the alias itself is not shown (not built) |
| PX7 | `tools/plus_txdiag.py --health` after PX1-PX6 | no crash record; `tasks.free.ui` about as before (368 B in the stack check), P25 worker stack as before |
| CM1 | P25 CH_MODE 2 build: channel list / zones with the channels 18-21 ("159.64375" ...) | they appear as P25: editor NAC "$293 = 659", talkgroup 1 (19, 20: 612), P25 Radio ID 1; on them the P25 receiver runs (P25 traffic with NAC $293 on those frequencies is heard; the owner's other radio is heard only if set to NAC $293, i.e. "659" on a decimal-entry radio); PTT there refused (outside Plus's P25 TX bands) |
| CM2 | Channel 18: editor, change nothing or e.g. the name, Save; reopen | still P25, NAC $293, TG 1, ID 1 (now stored in Plus's own P25 record, 0xA5); channels 19-21 untouched |
| CM3 | Optional, read only (its own approval for the serial read): a two-pass backup, then the PC app's codeplug view with `pcapp-codeplug.diff` applied | channel 18 shows CH_MODE 0xA5, 19-21 CH_MODE 0x02 "(CH_MODE 2 biçimi)", all four P25 NAC $293 = 659 |
| LW8 | **TRANSMITS on the APRS frequency (144.800 MHz): needs the owner's approval for this step; an emergency position is relayed by digipeaters and shown as "Emergency" on aprs.fi, so test only where that is acceptable (e.g. path None into a dummy load with a second receiver, or agreed with the local APRS operators).** APRS SOS chosen, callsign and a position set, 5 min, no keys | at 5:30 the alarm; one Mic-E emergency packet (the second receiver decodes it), then "SOS position sent"; Tools > APRS shows SOS "On, next m:ss"; a key (not PTT): toast "SOS off", Tools > APRS shows SOS Off, no further packets |
| LW9 | `tools/plus_txdiag.py --health` after LW1-LW8 | no crash record; `tasks.free.ui` about as before (368 B in the stack check); heap_min_free as before |
| MW1 | Multi-watch build: Settings > Talk & listen > Watch channels = 2, then `* 7` | upstream's dual watch exactly (two panes A / B, the A/B key, # 1-5, talk back); Back Back stops it |
| MW2 | Watch channels = 4 on a zone with FM and DMR channels (and the owner's P25 channel), `* 7`; watch the green LED / Home for a minute with no traffic | four rows A-D (A the channel you were on, B-D the next ones of its zone), A selected (▶, PTT); "Ring 0.6 s" (", P25 +3.5 s" and the toast "P25 slows the ring" with the P25 channel in it); note the real round time: key the other radio on D several times and time key-up to "Receiving on D" (at most one round plus the FM / DMR switching the host model leaves out) |
| MW3 | The other radio calls on C (FM), then on D (DMR), then the P25 radio (433.500, NAC 659, TG 101) on the P25 row | each call is heard from (nearly) its start: the row turns green, its caller on the row ("K1ABC Joe TG 91", "P25 ..."), "Receiving on C" with the meter; after the call "Heard 5s ago" on that row (amber), the ring goes on after the Dual watch delay; note syllables lost at the start of each call |
| MW4 | During MW3's wait after a call on C, the other radio keys on A | A's row says Busy (green outline), A is not played during the wait; when the wait ends the ring holds on A and plays it |
| MW5 | A/B key twice (C selected: "Talking on C"), PTT | TX on C's frequency and mode (another receiver hears it on C); after release the ring goes on (rows cycle again); # 1: "Talking on D"; # 7: D's picker, another channel: D's row shows it |
| MW6 | Watch channels = 6, then 3, then 2 while the watch runs; power off and on with C selected | 6 rows, "Ring 0.9 s"; 3 rows; back to dual watch's two panes (the selected channel stays selected as B); after power off the ring comes back with C selected on its channel (D-F found again as the next channels) |
| MW7 | P25 row: a P25 signal without voice for us (another NAC, or the P25 radio on another talkgroup with NAC Any off) | the ring holds the P25 row about 2.5 s plus the wait, then goes on; no crash, the P25 radio's own calls on the row still heard afterwards (MW3) |
| MW8 | `tools/plus_txdiag.py --health` after MW1-MW7 | no crash record; `tasks.free.ui` and `tasks.free.radio` about as before (368 / 824 B in the stack check); heap_min_free as before |
| MW9 | **TX: owner approval and an authorised frequency for each PTT; TX on each letter goes out on that letter's frequency only.** The owner's setup (`mwfix` build): Watch channels 3, A DMR 436.275 CC1 TS1 TG1, B P25 145.500 TG1 RX NAC Any TX NAC $293, C FM 433.500 no tone; the DMR radio on A's settings, the P25 radio on B's, a third receiver (or the scanner) on 433.500 FM. Select C (A/B key until C has ▶ / PTT), wait for a quiet ring, PTT 3 s ten times at random moments | every time only the FM receiver on 433.500 hears it (no P25 on 145.500, no DMR on 436.275); Home "TALKING on C" while held, then the ring line again within a second of release |
| MW10 | MW9, C selected: the P25 radio talks on B (Home "Receiving on B"), and within a second of the end of its over, PTT 3 s (the report-4 case) | the FM receiver on 433.500 hears it, the P25 radio hears nothing; "TALKING on C"; after release the ring goes on; the P25 radio's next over on B is heard |
| MW11 | MW9 with B selected: PTT 3 s with the ring quiet (ten times), then as the reply right after the P25 radio's over (talk 3 s, longer than 1.5 s) | every time the P25 radio hears us (TG1, TX NAC $293 / 659 decimal on a decimal radio), never DMR / FM; "TALKING on B" while held; after release TALKING goes away within a second (the report-3 case), the P25 radio's reply within 2.5 s is heard on B, then the ring goes on |
| MW12 | MW9 with A selected: PTT 3 s with the ring quiet, then right after a P25 over on B (held there) | the DMR radio hears us on TG1 (CC1 TS1), nothing on 145.500 / 433.500; after release the ring goes on |
| MW13 | After MW9-MW12: the P25 radio calls on B five times, 20 s apart, with the other two radios calling in between (the report-1 case); then 5 minutes without a power cycle, again | every P25 call on B is heard (the row turns green, "Receiving on B" with its caller); no stuck TALKING / TX denied on Home at any time; never needed a power cycle |
| MW14 | Watch channels 2 (dual watch, B the P25 channel), then 4, 5, 6 with a P25 and a DMR letter among them: select each letter in turn, PTT 2 s (the ring quiet) | each PTT on the selected letter's frequency and mode only (the matching radio hears it), the PTT pill / TX on that row; dual watch: with talk back on, after a call on the other pane the toast "Talk back on B" and the PTT pill moves there: PTT then goes out on B (that is the rule), else always on the pane with PTT |
| MW15 | An RX-only letter (a channel set RX only, or an NXDN / dPMR channel) selected in the ring, PTT | "TX denied" (RX only / mode) while held, nothing transmitted on any letter; released: the hint goes back, the ring goes on |
| MW16 | `tools/plus_txdiag.py --health` after MW9-MW15 | no crash record; `tasks.free.radio` / `tasks.free.ui` about as before (728 / 184 B in the stack check) |
| HD1 | Home's details, single channel: the owner's P25 channel B (RX NAC Any, TX NAC $293, TG 1), then a P25 channel with NAC $293 and TG 42, a YSF / NXDN / dPMR channel if there is one; normal and Large text | the chips row says `NAC Any>$293/659 TG 1` (or the tight `NACAny>$293/659 TG1`), `NAC $293/659 TG 42`, `DG-ID ..`, `RAN ..`, `CC ..` instead of "No tone 12.5k", with the power where it fits; nothing runs off the screen; FM and DMR channels as before |
| HD2 | Dual watch (Watch channels 2) with B the P25 channel, then B a P25 VFO (VFO key, `* 4 4`) | pane B row 2 `TG 1` (what PTT talks to, as a DMR pane's talk group), row 3 `145.5000 MHz  NAC Any>$293/659`; the VFO pane no longer says "VFO FM" |
| HD3 | MW9's three letters (A DMR, B P25, C FM), then 6 letters; Large text on and off | line 2 of each row: A `DMR CC1 TS1 TG1` (selected) or `436.2750 CC1 TS1 TG1`, B `P25 145.5000 NAC Any>$293/659 TG 1` or a shorter form, C `FM 433.5000 No tone`; Large text one line per letter as before; nothing cut in the middle of a number |
| HD4 | A letter made a VFO in P25 (`# 6`..`# 9` picker or the VFO key, `* 4 4`, `#` TG 99 NAC 1110) | its row follows at once: `P25 145.5750 NAC $456/1110 TG 99` (or shorter) |
| MW17 | **No TX expected; a receiver on each letter's frequency.** MW9's setup with B (P25) selected, the ring on A or C: Tools / the key for the 1750 Hz burst (if assigned) and, separately, the emergency key (hot mic) with the time-out set to Off | nothing transmitted on 145.500 (no P25), "TX denied" while it lasts, then idle; with C (FM) selected the burst goes out on 433.500 FM only; with A (DMR) selected the burst is refused (no 1750 on DMR) |
| MW18 | While transmitting on B (PTT held), press the A/B key | toast "Not while transmitting", the PTT pill stays on B; after release the A/B key moves it |
| MW19 | Listening only, MW9's setup, the ring running: the P25 radio starts a call on TG 1 while the receiver is on A or C, ten times; once with the other radio on another talk group for 5 s, then TG 1 | every TG 1 call heard within about a second (late entry); after the other talk group, the TG 1 call heard within a few seconds (no 8 s gap) |
| MW20 | Watch channels 2, 3, 4, 5, 6 (EN and TR, normal and Large text) | the status bar says DUAL / TRIPLE / QUAD / 5X / 6X (ÇİFT / ÜÇLÜ / DÖRTLÜ / BEŞLİ / ALTILI) where it fits, tile 7 "Quad watch" / "Quad", Tools' row and the # screen title the same, the start toast "Quad watch on" / "Dörtlü izleme açık"; nothing cut or overlapping |
| MW21 | Dual watch with talk back on: a call on the other pane ends | the toast "Talk back on B" with a short beep, the PTT pill moves to B; PTT then goes out on B (the rule); after 10 s it goes back |
| MW22 | **Emergency: owner approval; it transmits an alarm / emergency call on the DMR letter (TG1).** Triple watch with A (DMR) selected, the ring running: the emergency key; then with C (FM) selected; then the lone worker's alarm (or SOS) with C selected | each time "Watch off: on A" and the alarm / hot mic on 436.275 DMR only (never P25 or FM); a ring without a DMR letter: the watch goes on, the emergency reports it can't start |
| MW24 | **Emergency: owner approval (transmits on A, DMR TG1).** Triple watch with C selected, the P25 radio talking on B (Home "Receiving on B"): the emergency key; then a single P25 channel (watch off) with the P25 radio talking: SOS (or the lone worker's alarm) | the first: within about a second "Watch off: on A" and the alarm / hot mic on 436.275 DMR while the P25 radio still talks (not after its call); the watch stays off afterwards (start it again by hand). The second: the emergency reports it can't start within about a second and SOS / the lone worker fall back (sound, APRS) |
| MW25 | Triple watch, C (FM) selected: hold PTT and press the emergency key | nothing changes while PTT is held (C stays on the air); on release the emergency goes out on A (DMR), never on C |
| MW26 | **Emergency: owner approval.** P25 trunk following running (Tools > P25 trunk), then the P25 search playing a find: SOS (or the emergency key) | within about a second the following / search ends and the emergency reports it can't start on a P25 / FM channel; SOS / the lone worker fall back (sound, APRS); on a DMR channel it would go out there |
| MW23 | While the P25 radio talks on B (Home "Receiving on B"), select C with the A/B key | the P25 call on B is not cut (it plays on), the PTT pill on C |
| MW27 | **Listening only.** Triple watch with A DMR, B P25 (RX NAC Any), C FM (the owner's setup). A DMR call comes in on A (any talk group, also one other than A's channel setting), Home | A's row turns green; line 2 names the caller and the HEARD group (`K1ABC Joe TG 91`, or `TG 91` when narrow), filling in a moment after the call starts (before: stayed "DMR call"). After the call: "Heard Ns ago" and the channel's details again |
| MW28 | **Listening only.** Same setup, the P25 radio calls on TG 101 (not B's own TG 1) | B's row (and a pane, if B is in one): `TG 101 ID <source>` and the NAC in the `$hex/dec` form; after the call B's `NAC Any>$293/659 TG 1` details come back. The first moments may say `P25 call` or the group of the call before until the link control arrives (as on the single channel) |
| MW29 | Quad, 5x and 6x watch, a DMR call on a letter, normal and Large text; then Dual watch (2 letters) and a private call to the radio | one-line rows (6 letters, Large text): `TG 91` in green left of the RX / PTT pill, the name shortened; private: `Pvt <caller ID>` (two lines: `... to you`); the dual panes show the group on row 2; nothing runs off the screen; Large text dual watch shows the letters only, as before |
| VX1 | Every mode in the VFO: VFO, type 433.500, `* 4` until "VFO: P25", then `#` (VFO settings) | Mode P25, Talkgroup Any, NAC Any (F7E), P25 Radio ID = the radio's DMR ID; Home "VFO P25 NAC Any TG Any"; the owner's P25 radio (NAC 659, TG 101) is heard with its caller line, as on a P25 channel |
| VX2 | VX1, NAC `#` 1 6 2 5 OK (decimal 1625 = $659), Talkgroup 1 0 1 OK; Home | "$659 = 1625"; Home "VFO P25 NAC $659 TG 101"; the other radio's calls on TG 101 heard, a call on another TG not |
| VX3 | **TX: owner approval and an authorised frequency.** VX2, PTT and talk | the P25 radio hears the call (TG 101, our unit ID); with NAC or Talkgroup set back to Any: PTT refused (TX denied, RX only) |
| VX4 | Power off and on in VX2 | the VFO comes back in P25 with NAC $659 TG 101 and the same unit ID; its FM tones (if it had any) are gone, its DMR talk group is not |
| VX5 | `* 4` through YSF, NXDN, dPMR (with a transmitter of that mode if one is available; else only the rows) | YSF: DG-ID row, Home "VFO YSF DG-ID Any"; NXDN: RAN; dPMR: CC; PTT refused in all three; a signal of that mode, if available, plays as on such a channel |
| VX6 | DMR in the VFO: `#`, Color code down to Any, Timeslot to Any, RX group list to "Any (all groups)" on a DMR repeater's output | calls on both slots and every talkgroup heard ("CC*", "TS*"); PTT refused; power off and on: still Any |
| VX7 | VFO settings > Save as new channel in VX2, hold OK | the channel editor on a new channel named after the frequency, Mode P25, NAC $659, TG 101; hold OK: "Saved as channel n"; that channel works as VX2 |
| VX8 | Copy to VFO (Quick 4) on the owner's P25 channel | the VFO opens in P25 with the channel's NAC, talkgroup and ID; heard as on the channel |
| VX9 | AM off the air band: VFO on 433.500 (or 145.500), `* 4` to "VFO: AM", an AM-modulated carrier from a signal generator if one is available | Home "VFO AM", chip "AM"; whether the FD6818 AM demodulator works outside 108-137 MHz is what this row decides (audio, squelch on the strength); PTT refused (TX denied AM) |
| VX10 | Multi-watch, Watch channels 4, `* 7`: select C (A/B key twice), VFO key; on C: 145.575 typed, `* 4 4` P25, `#` TG / NAC of the P25 radio; select D, VFO key, `* 4 4 4` YSF; Home | C's and D's rows say VFO with their frequencies and modes; A and B as they were (their VFOs untouched); the ring visits C and D; a P25 call on C holds the ring and plays (as MW3 on a P25 channel). **TX: owner approval and an authorised frequency** for PTT on C: it goes out on C only |
| VX11 | Power off and on in VX10 with C selected | the ring comes back with B in pane 1 (C..F's VFOs are RAM only); VFO A / B as saved |
| VX12 | Settings > Keys > SK1 hold: "Dual/multi-watch" (Watch channels 4); hold SK1 on Home, again; then `tools/plus_txdiag.py --health` after VX1-VX12 | the ring of 4 starts, then stops ("Watch off: on ..."); no crash record, task free bytes about as before (UI 368, radio 824) |
| TF1 | Tone finder build: Tools > Tone finder (the second last tool before Battery) on an FM channel with no signal (squelch closed, then opened with monitor), 2 min; then on a DMR channel and on the air band | "Listening on this frequency" within a second and "Searching...", after 3 s "No tone"; nothing found from noise; the speaker sounds flatter / brighter while the screen is open (the flat path), as before after Back; on DMR / AM: "Tone finder: tune an FM channel" |
| TF2 | **TX: owner approval and an authorised frequency** (the other radio transmits, ours only receives). The other radio on a simplex FM channel with CTCSS 88.5 Hz, then 67.0, 69.3, 151.4, 203.5, 250.3 Hz; Tools > Tone finder open on the same frequency (its RX tone none) | each named ("CTCSS 88.5 Hz", ...) within about 1-2 s of key-up, the bar mostly full; never a neighbour (67.0 / 69.3, 196.6 / 199.5); after key-up ends "Last: CTCSS ..." stays; note the time to the name |
| TF3 | **TX as TF2.** The other radio with DCS 023N, then 023I, 754N, 411I | 023N named "DCS 023N" (with "= DCS 047I" below); if it reads "DCS 023I" for 023N the receive audio is inverted: build with `-DTONEF_DCS_INVERT=1` (note it); 023I reads as an equivalent (e.g. "DCS 047N = DCS 023I"); no CTCSS shown during DCS or at its turn-off |
| TF4 | **TX as TF2.** CTCSS 88.5 found: OK > RX and TX on a saved FM channel; then on the VFO OK > TX only; then a DMR channel | "Saved channel N", the channel editor shows RX and TX tone 88.5 and the rest of the channel as before, the squelch now opens only with 88.5 (the other radio without tone stays muted); VFO: its TX tone 88.5, RX unchanged, nothing written to the codeplug; DMR: "FM only: this is not an FM channel" |
| TF5 | During TF2 and TF3, read the "FD6818:" lines (experimental: the chip's 0x68-0x6A) | note what they show for each tone / code and with no signal. If they track the tone ("FD6818: CTCSS 88.5 Hz") or the code, the chip measures them itself (a faster, hardware tone finder is possible); "-" or fixed words: not on the FD6818 |
| TF6 | **TX as TF2.** Weak signal: the other radio at low power / farther, 88.5 Hz; also with voice | the tone found while the voice is readable; with voice the name stays (no flicker to another tone) |
| TF7 | `tools/plus_txdiag.py --health` after TF1-TF6 | no crash record; heap_min_free > 1 KB (the screen takes ~0.7 KB of heap beside FM caller ID's 2 KB ring); UI / radio stack free as before (368 / 824 B) |
| SC1 | DTMF / SelCall build: Tools > DTMF / SelCall on an FM channel with no signal and with voice traffic, 5 min | "Listening on this frequency - Auto", "Nothing heard yet": nothing listed from noise or voice; the speaker sounds as usual (no flat path) |
| SC2 | **TX: owner approval and an authorised frequency** (the other radio sends). DTMF from the other radio: "1234#", then "*0#" 5 s later | "DTMF 1234#  <frequency>" about 1.5 s after the last digit, then "DTMF *0#" above it; FM caller ID's Home line also shows them (as before) |
| SC3 | **TX as SC2.** 5-tone calls if the other radio (or a PC / phone tone app into its microphone) can send them: ZVEI1 "12345", CCIR "12345", EIA "12345", a call with a repeated digit ("11223") | "ZVEI 12345" (ZVEI1 / ZVEI2 can't be told apart by digits only), "CCIR 12345", "EIA 12345", "ZVEI1 11223" (the repeat tone names it); the same call sent twice refreshes one row |
| SC4 | # on the screen: Auto > ZVEI1 > ZVEI2 > CCIR > EIA > Auto; with CCIR picked send a ZVEI call | the toast and the line above the rows follow; the ZVEI call is not listed while CCIR is picked |
| SC5 | Settings > Talk & listen > FM caller ID Off, then SC2 again; Back from the screen | still decoded on the screen; after Back FM audio is on the direct path again (as with the pager, SM2) |
| SC6 | `tools/plus_txdiag.py --health` after SC1-SC5 | no crash record; heap_min_free > 1 KB (the screen takes ~0.75 KB of heap); stacks as before |
| MN1 | APRS on (Tools > APRS, receive on the APRS frequency) until a few stations with positions are heard (Tools > APRS > Heard); Tools > Map near them, zoom 3-4 | a small dot (receive colour) at each station heard with a position, its callsign beside it from zoom 3; no two labels overlap (district names give way); stations heard without a position (status only) not drawn |
| MN2 | 0, 0 again, ... past the last | the station nearest the map's centre drawn larger with its callsign in white; under the map "TA1ABC-9 2.8 km NE 3m ago" (the way from the centre; "3m" if the line is short); 0 walks outward, after the farthest the nearest again; the scale is not shown while a pick is |
| MN3 | OK on a picked station, then OK again; in the composer Text or a canned text, Send (**TRANSMITS an APRS message on the APRS frequency: owner approval and an authorised frequency**); Back | OK: "/> 376 m comment" (symbol, altitude when the station sends one, the comment without "/A="); OK again: Tools > APRS > New message with To = that callsign; Back returns to the map with the details; the message goes as from Tools > APRS (acks, 3 tries) |
| MN4 | With a station picked: #, then 0; Back, Back, Back | # picks the repeater nearest the centre (station pick gone), 0 the station again; Back: details off, then the pick off ("06 Ankara / ..." again), then Tools |
| MN5 | Repeater list build (RP1), GPS fix outdoors on an FM channel; Tools > Auto repeater, OK | the row says On; within a second a banner "Nearest: <callsign> <MHz> <km> <direction>" / "# tunes the VFO" / "Back hide" with its seconds counting down (an FM repeater in the radio's bands); on a DMR channel the nearest DMR one instead |
| MN6 | # while the banner is up (Home or any screen) | VFO tuned as RP3 / RP4 (RX, mode, tone or CC / slot, TX = RX + offset), toast "VFO tuned to the repeater", Home; the radio never keys up by itself |
| MN7 | Auto repeater on again, Back on the banner, then stay at the same place 5 minutes | the banner goes at once, nothing tuned; no new banner while standing (no repeated offers); the banner never hides a message banner (send a DMR text to the radio while waiting: it shows, the offer only after it) |
| MN8 | Drive (the auto repeater uses the GPS fix only, not a fixed position) from one repeater's area clearly into another's | "Nearer: <the other one> ..." within about 30 s of it being 20 % and 2 km nearer; none for one only barely nearer; # tunes it; no flip-flop when driving back and forth near the halfway point |
| MN9 | Tools > Auto repeater OK (Off); power off and on | "Auto repeater off", no more banners; after power on the row says Off |
| MN10 | GPS fix outdoors, walk or drive 10 minutes; Tools > Map at zoom 5-6 | a dotted line (TX colour) behind the GPS cross along the way taken, a point about every 30 s (none while standing still); after 48 points the oldest part disappears |
| MN11 | On the map: 9 | the waypoint START (the track's first point) saved and Tools > Go-To opened on it with the arrow, distance and ETA back to the start; Back returns to the map; 9 again opens Go-To without a second START; START drawn as a waypoint box on the map |
| MN12 | 7, then hold 7; `tools/plus_txdiag.py --health` after MN1-MN11 | 7: "Hold 7 to clear the track" (nothing cleared); hold 7: "Track cleared", the line gone (a new track starts at the next fix); no crash record, `tasks.free.ui` about as before (368 B in the stack check), heap_min_free as before |
| YT1 | **TX: owner approval and an authorised frequency; a YSF receiver needed.** YSF transmit build only (`--feature YSF_TX=y`, docs/ysf-nxdn-tx.md; the Plus preset has it off). Settings > My radio: a name that is not a callsign (e.g. "Ahmet"); a YSF channel on an authorised 70 cm simplex frequency, editor RX only = No; PTT | refused at once: "Radio name must be a callsign" / "Settings > My radio", no TX LED, nothing on the YSF receiver; release, the channel receives again |
| YT2 | **TX: owner approval and an authorised frequency; a YSF receiver needed.** Radio name = the owner's callsign; the YSF receiver (a Yaesu Fusion radio in digital AMS/DN, an MMDVM hotspot in YSF mode, or an SDR with DSDcc / DSD+) on the frequency; PTT, 5 s of speech, release | the receiver plays clear speech (VW / "Voice FR"); note the quality and whether the first word is cut. If nothing decodes: an SDR's deviation reading (expected about ±1.9 kHz outer, YSF normal is ±2.7 kHz) and whether an MMDVM hotspot logs "V Mode 3" with its BER; report before trying again |
| YT3 | **TX: as YT2.** Look at the receiver during YT2; then the channel's DG-ID set to 12, the Yaesu's DG-ID (RX) 12, then 13 | our callsign shown (MMDVM hotspot: "received RF header from TA1..."); DG-ID 12 heard, 13 not (when the Yaesu filters on DG-ID) |
| YT4 | **TX: as YT2.** 30 s over, then several short overs (under 0.3 s) and a PTT tap; then `tools/plus_txdiag.py --health` and the live counters (debug link 0x20 op 13, read only) | no break in the long over; a tap sends nothing (the modem never starts without speech) or a short call; `live.faults=0`, `live.reason` 3 (TX complete), `live.encode_max_us` under 20000, `live.overflows=0`; no crash record, p25-lab stack free about as before (908 B in the stack check) |
| YT5 | **TX: as YT2 (no transmission expected).** The gates: the YSF channel's TX frequency outside 144-148 / 420-450 MHz; RX only = Yes; Settings TX inhibit on; also the channel's TX time-out at its shortest with PTT held past it | refused with the band text / "This channel is RX only" / TX inhibit, nothing keyed; the time-out ends the transmission with "TX time-out reached" and the channel receives again after release |
| YT6 | **TX (the other radio): owner approval and an authorised frequency.** The Yaesu sends to us in VW, then in DN, on the channel's DG-ID | both heard; Home's bottom line "YSF <callsign>" from the start of each call (VW now names the caller from the header too); our own transmission is not looped back |
| AF1 | Misc build: Tools > Airfields with a GPS fix (or the fixed APRS position) | the nearest airfield first ("LTAC Esenboğa" in Turkish letters, "26.0 km NE  TWR 118.100"), the next ones farther away; Up / Down round; without any position "No position: by code" and LTAB first |
| AF2 | OK on the nearest, then OK on its TWR (or ATIS) row | details: kind, distance and degrees, every frequency; then Home on the VFO at that frequency with "AM" on Home and "Air Band AM" as Tools > Air band shows it; the toast "LTxx TWR 118.100: AM, RX only"; ATC or ATIS audible when in range |
| AF3 | PTT on that VFO frequency | refused as in the air band (TX denied banner, TXDENY_AM); nothing keyed (check with a second receiver or the TX LED) |
| AF4 | LTFJ (Sabiha Gökçen) GND 121.580 (an 8.33 kHz name) | the VFO on 121.575 MHz (Home's frequency); Up / Down with the 8.33 kHz step moves to 121.5833 / 121.5667 (the same grid) |
| AF5 | An airfield without frequencies (e.g. LTAL Kastamonu), OK, OK | "No frequencies in the data", nothing tuned |
| MV1 | Tools > Marine VHF | opens on 16 ("Distress, calling", 156.800) in the warning colour; Up / Down, the knob, # back to 16; 70 and 87B / 88B marked data |
| MV2 | OK on 16, then on 22 (or the coast channel of a Turkish coast station in range) | Home on the VFO 156.800 (then 161.700), FM, 25 kHz, no tone, simplex; toast "Ch 16 156.800: RX only"; marine traffic audible near the coast |
| MV3 | PTT there; then a TX split to 145.500 (VFO quick settings) and PTT; then power off and on and PTT | refused every time with the RX only banner (TXDENY_RX_ONLY), nothing keyed (second receiver / TX LED); after the power cycle the VFO is still on 156.800 and still refused |
| MV4 | OK on 70 and on AIS 1 | toast "Ch 70 is DSC data: not tuned for voice" / "AIS is data: not tuned", the VFO unchanged; a VFO typed to 145.500 still transmits normally (**TX: owner approval and an authorised frequency**) |
| QL1 | A DMR call heard (or FM / P25 caller ID), Recent, * on its row | toast "Marked as QSO", a star before the row; * again: "QSO mark removed"; * on Home or another list still opens the quick settings |
| QL2 | **TX: owner approval and an authorised frequency.** Answer a call heard on a channel within a minute (PTT on the same channel), FM, DMR and P25 once each | the heard call's row gets the star by itself (P25: note whether it does: the rule uses the TX state the radio view shows); a call heard on another channel does not |
| QL3 | Read only, its own approval for the serial port: the debug link query `0x21 [14]` (`tools/plus_txdiag.py` or the PC app once it has the export) | status 0, version 1, the entries newest first with age, mode, flags (QSO 0x04), NAC, source, destination, frequency and name as docs/plus.md gives them; the marks of QL1 / QL2 |
| QL4 | Power off and on, Recent | empty (RAM only), as before |
| BG1 | Tools > Battery right after power on | the charge level and voltage ("84%  7.90 V"), "Time left: --" with "Collecting: needs 30 min", "First point in 2 min" in the graph box; after 2 minutes the first point |
| BG2 | Leave the radio receiving (no charger) for 45-60 minutes, then Tools > Battery | the graph falls slowly; "Time left: x h y min"; note it with the percentage, then again 30 minutes later (does the estimate hold? the radio's real run time to the low battery warning is the check) |
| BG3 | Put it on the charger for 10 minutes | "Time left: --" with "Charging"; off the charger, 30 minutes later an estimate again |
| PL1 | Read only, its own approval for the serial port: Tools > Spectrum running, the debug link query `0x21 [13]` every 200 ms from the PC; then leave the spectrum screen (Home) and keep querying | status 0 with version 1, points 32 / 64, the centre and step of the screen, dBm values matching the bars, the hit as the screen names it; after leaving the screen the sweep stops by itself within about 3 s (the radio returns to its channel's audio) although the PC still asks; then status "unavailable" |
| PL2 | Read only: GET_VIEW on a P25 channel, on the VFO in the air band (Tools > Air band), on an FM and a DMR channel | `plus_mode=p25`, `am`, `fm`, `dmr`; `view=` unchanged from the build before |
| PS1 | Priority / skip build: a zone of FM channels (a few with traffic), Settings > Talk & listen > Priority channel (the last row) on Home's channel: OK, then Back to Home | toast "Priority: 12 <name>"; the row says the number; Home's channel number reads "12P"; OK on the row again: "Priority channel off", the P gone (set it again for PS2-PS8) |
| PS2 | Go to another zone's channel, start Tools > Scan (the priority channel is not in this zone); watch Home for 20 s with no traffic | the strip says "SCAN P"; Home shows the priority channel first, then about every 2 s (one short stop, its zone's name shown while there) between the zone's channels, which keep their order; note the real interval |
| PS3 | The other radio keys the priority channel (FM) for 10 s while the scan steps | heard within about 2 s of key-up (note the syllables lost), the scan holds there as on any channel, then 2 s after the call (scan resume delay) the scan goes on in the scanned zone |
| PS4 | The other radio calls on a channel of the zone (A) for 10 s; during the call key a third radio (or the same one quickly afterwards) on the priority channel | no break in A's audio during the call (no look during a call); in the 2 s wait after A's over a short look at the priority channel (~150 ms gap): with a signal there the radio stays on it, without one it goes back to A, and an answer on A in the wait is still caught |
| PS5 | **TX: owner approval and an authorised frequency.** In PS4's wait after A's over, PTT (at any moment, also during a look) | transmits on A (the channel of the wait), never on the priority channel; the scan stops there |
| PS6 | Dual watch (`* 7`, Watch channels 2 and then 4) with the priority channel outside the ring, then D's channel made the priority one | outside: the strip "DUAL P", a short stop on it about every 2 s, the panes / rows keep their order; a call there holds the watch (heard; the panes don't name it), the ring goes on after the Dual watch delay; on D: a call on D during the wait after a call on B moves the watch to D at the first look instead of only showing Busy |
| PS7 | Zone scan stopped on a busy channel: 0 | toast "Skipped until the scan stops", the scan moves on and never stops there again; 0 while the scan is still stepping: "Nothing to skip yet"; # shows 4 "Clear skips" with the count, # 4: "Skips cleared", the channel stops the scan again |
| PS8 | Skip two channels (0), Back (scan stops), start the scan again; skip the priority channel while held on it; power off and on | after the restart nothing is skipped; the priority channel skipped: no more priority stops until # 4 or a new scan; after power on the priority setting stays (Home "12P"), no skips |
| PS9 | P25 priority channel (the owner's 433.500 MHz, NAC 659, TG 101) in a zone of 16+ channels; the P25 radio makes short (2-3 s) calls; Zones > the zone > # on that channel > 7 Set as priority | toast "Priority: ..."; more short calls caught than without a priority channel (count 10 calls each way); 0 while the P25 receiver holds the channel: given back, the scan moves on and leaves it alone; `tools/plus_txdiag.py --health` afterwards: no crash record, `tasks.free.ui` and `tasks.free.radio` about as before (368 / 832 B in the stack check) |
| NX1 | RX / TX NAC build (`CONFIG_P25_TXNAC`, docs/plus.md "Separate RX NAC and TX NAC"): the owner's P25 channel (433.500 MHz, NAC $659, TG 101) as saved before this build; channel editor, look only, Back | the rows read "RX NAC  $659 = 1625" and "TX NAC  RX ile aynı" (Same as RX); the other radio's calls are heard as before; nothing to save (no dot on any row) |
| NX2 | On NX1's channel: TX NAC row, OK, 1 2 5 OK, then OK, # # 2 9 3 OK; then # (Same as RX); Back without saving | "$125 = 293" after the hex digits, "$125 = 293" again after the decimal entry, "RX ile aynı" after #; the RX NAC row unchanged ($659); Back asks to discard |
| NX3 | **TX: owner approval and an authorised frequency.** NX1's channel, TX NAC $125 saved (hold OK); PTT and talk; then TX NAC back to Same as RX, saved, PTT again | with TX $125 the other radio (NAC $659) does not open (or shows NAC $125 / 293 if it displays foreign NACs); our receiver still hears the other radio's calls (RX $659); with Same as RX it hears us as before |
| NX4 | A copy of the channel (Save as new channel) with RX NAC # (Any): toast "Tüm NAC: yalnız RX"; then TX NAC 6 5 9 OK, hold OK | after the TX NAC: toast "TX, TX NAC ile", the RX only row reads Hayır (No); saved; a call from the other radio with another NAC (if it can change its NAC) is heard too |
| NX5 | **TX: owner approval and an authorised frequency.** NX4's channel (RX Any, TX $659): PTT and talk | the other radio (NAC $659) hears us; Home's caller line shows the other radio's NAC when it answers |
| NX6 | NX4's channel: TX NAC # (Same as RX), OK; PTT | toast "Tüm NAC: yalnız RX"; PTT refused (TX denied, RX only); nothing transmitted |
| NX7 | VFO: 433.500 typed, `* 4 4` (P25), `#`: Talkgroup 101, TX NAC 6 5 9 (RX NAC left Any); Back Back; **TX: owner approval and an authorised frequency** for PTT; then power off and on | Home's VFO row "NACTümü>659 TG101" (the tight form; a wider screen "VFO P25 NAC Tümü>$659 TG 101"); PTT: the other radio hears us; after power on the VFO is P25 NAC Tümü TG 101 with TX NAC "RX ile aynı" (RAM only) and PTT refused (RX only) |
| NX8 | `tools/plus_txdiag.py --health` after NX1-NX7; optional, read only (its own approval for the serial read): a two-pass backup, then the PC app's codeplug view with `p25nac/pcapp-codeplug.diff` applied | no crash record, `tasks.free.ui` and `tasks.free.radio` about as before (stack check); the PC app shows NX4's channel with NAC "Herhangi ($F7E)", TX NAC "$659 = 1625", the others "RX ile aynı" |
| CW1 | CW decoder build: Tools > CW decoder (the last tool, before Battery) on an FM channel with no signal, squelch closed then opened with monitor, 2 min; then a voice call on it | "Listening: MCW tone 400-1000 Hz" within a second ("Waiting for receive audio" at most a second first; on a DMR / P25 / NXDN channel "CW: tune an FM or AM channel"); "-- WPM --- Hz", the lamp dark; no text from noise; a voice call gives no text or a few stray characters at most; the speaker plays FM audio as usual (the normal receive path, not the flat one) |
| CW2 | The other radio (or a phone / SDR into a dummy load on a simplex FM channel, licence permitting) sends MCW: a CW ID or beacon tone of 600-800 Hz at 20 WPM, "CQ CQ DE TA1ABC TA1ABC K" | within the first few characters the text appears (shown when the speed is known: the first characters come out together), then character by character as sent; "20 WPM" (+-2) and the tone (+-10 Hz of the sender's) above it; the lamp follows the keying |
| CW3 | Same at 8, 12, 30 and 35 WPM, and with the tone at 450 and 950 Hz | each decoded; the WPM reading follows within a few characters after each change (a pause of more than two word spaces starts the learning again); text spaced into words |
| CW4 | Weak signal: the other radio far / low power until the FM noise is clearly audible under the tone | still readable with occasional wrong characters (host test: under 2 % at +3 dB tone-to-noise in the audio band); as it fades into noise the text stops rather than filling with junk |
| CW5 | A repeater's own CW ID (an FM repeater that IDs in MCW), Tools > CW decoder on its output | its callsign decoded (often 20-25 WPM, 1 kHz or so); note the repeater and the text |
| CW6 | AM: Tools > Air band (AM) on a frequency with an MCW beacon or ident if one is in range (or the other radio in AM is not possible: skip if no AM MCW source) | decoded the same way; the screen says "Listening" on the AM channel (the ring taken as ACARS does: M4's open question applies) |
| CW7 | OK on the decoder (clears the text); # (CW practice); Back, Back; reopen; `tools/plus_txdiag.py --health` | the text cleared; the practice page opens and the decoder stops (no new text appears there meanwhile); Back returns to the decoder, Back to Tools; the decoded text is still there on reopening; heap_min_free > 1 KB (the decoder takes ~650 B of heap while its screen is open, on top of FM caller ID's 2 KB ring) |
| CW8 | CW practice: OK; listen; type the group with the keypad letters (5 5 = K, wait or > before the next letter on the same key), OK. **Watch the red TX LED and the other radio on the channel's frequency throughout** | the group sounds in the speaker only at the speed and tone shown (20 WPM, 600 Hz; Left / Right change them, the next group plays with the new ones); "Right: KMKMM" / "Sent: ..." and the score; the TX LED never lights and the other radio hears nothing at any point (local only: the beep generator, never the transmitter) |
| CW9 | CW practice: Mode Characters (Up to Mode, Right), Koch 10 (Down, Right x 8), OK; answer a few right and a few wrong; hold OK; # | one character at a time, its dots and dashes shown after the check, the next plays by itself a second later; OK held plays the last again; # "Score reset". Key beeps (Settings) on: pressing keys while it plays does not cut the Morse |
| CW10 | CW practice OK, then PTT while it plays (any FM channel where TX is authorised: **TX: owner approval and an authorised frequency**); and OK pressed while PTT is held | PTT transmits as usual (the practice Morse stops or goes on in the speaker, never on air: the other radio hears only the microphone); OK during TX: toast "Not while transmitting", nothing played |
| BR1 | Name: power the radio on (dark theme, then light: Settings > Display) with and without a CPS boot text | the splash reads "DDM32 Plus" ("DDM32" in the text color, "Plus" teal) under the mark, the boot text lines under the rule, board and version small and dim, and under them on the very bottom line "TA2DOU" small, centred, teal; nothing cut at the edges |
| BR2 | Settings > Advanced > About (Turkish and English), scroll to the end | first line "DDM32 Plus · d0ukan · TA2DOU" (the dot drawn, not a "?"); then "Based on Open25-DM32 Classic" (no "Firmware: Open25-DM32 Classic", no upstream Issues link); "DDM32 Plus: GPL-3.0-or-later" right above "License: MIT, (c) 2026 Open25-DM32 contributors"; "Based on RadioFoundry", its MIT line and both authors as before; at the end "ID veritabanı: 8694, …" (owner build only) and "Röle verisi: RadioID.net, TRAC" / "Repeater data: …"; nothing cut off at the bottom |
| BR3 | Public build only (`dm32-p25-plus-public`, if the owner installs it): a DMR call from an ID with no codeplug contact; About | the caller shows as the bare ID (no callsign / name from the ID database); About has no "ID database" line; Tools > Repeaters lists the repeaters as in the owner build |

### Classic r13 base (docs/plus.md "On the Classic r13 base")

| # | Do | Expect |
|---|---|---|
| R13-1 | First boot of the r13-based build over the r7-based one (the codeplug as it is: its P25 channels are 0xA5 records, channels 18-21 mode 2, YSF / NXDN / dPMR ones) | no "Start new" question; every channel listed as before; a 0xA5 P25 channel shows its RX NAC, "TX NAC: Same as RX", talkgroup and ID as before; channels 18-21 are P25 |
| R13-2 | One 0xA5 P25 channel: open it in the editor, Save; reboot; `tools/plus_txdiag.py --health` | still P25 with the same NAC / talkgroup / ID; heap and stacks as before. (It is now a DMR base record with an RFKV tag: the upstream Classic r13 CPS, read only, shows it as P25) |
| R13-3 | P25 receive on 433.500 MHz (the other radio's NAC $659, TG 101) with APRS on (Settings > APRS), then an APRS beacon on an FM channel | P25 heard as before (the native lease guard now on timer 5); the APRS beacon still goes out (AFSK on timer 4); no crash, no stuck receiver |
| R13-4 | P25 trunk following (Tools > P25 trunk) on a control channel, if one is in range | grants followed as before; the trunk screen's counts move ("bad" counts dropped TSDUs now) |
| R13-5 | The editor's "TX NAC" row on a P25 channel: hex digits, # (Same as RX), # # decimal; RX NAC Any with Same as RX; PTT on a channel with TX NAC $659 into the other radio set to NAC $659 (**TX: owner approval and an authorised frequency**) | the rows work as in the host tests; RX Any keeps TX (sends $293); the other radio hears the call only on $659 |
| R13-6 | Channel editor: a new channel outside 144-148 / 420-450 MHz; then RX only Off | Classic r10's rule: it starts RX only; turning RX only off asks "Enable TX?" with Keep RX-only as the default |

### Round 13 ideas 1 (docs/plus.md "Round 13 ideas 1")

| # | Do | Expect |
|---|---|---|
| I23-1 | Tools > P25 search, range around 433.500 MHz, until the other radio's call (NAC $659, TG 101) is found; # | "Saving 1 of 1" briefly, then toast "Bulunanlar 1: 1 saved"; Zones lists "Bulunanlar 1" with "P25 433.500"; the channel editor shows P25, RX NAC $659, TX NAC Same as RX, squelch Normal, talkgroup 101, Radio ID Default, RX only No when 433.500 is in the TX band (else RX only); the codeplug's other zones and channels unchanged |
| I23-2 | On that channel (Zones > Bulunanlar 1): receive the other radio's call | plays as on a hand-made P25 channel (every talkgroup on NAC $659) |
| I23-3 | Back to P25 search: # again; then Down on the find, OK; then a second find (another frequency or NAC), Down to it, OK | "Bulunanlar 2: 1 saved"; "Already in Bulunanlar 2" (nothing written); "Saved to Bulunanlar 2" and Zones > Bulunanlar 2 holds both |
| I23-4 | Tools > DMR search near a DMR repeater or simplex user; #; then Down on a find, hold OK; OK on a find | a new "Bulunanlar N" with "DMR 438.xxx" channels: colour code and slot as found (Any where unknown), RX group list Any, RX only, no contact; hold OK: "Already in ..." or "Saved to ..."; OK still tunes the VFO there (as before) |
| I23-5 | A codeplug with few free channels (or a find zone of 80): # with several finds | the save stops cleanly: "Codeplug full: n saved" / "Zone full: n saved" (or the zone editor's own refusal toast, then "Stopped: n saved"); the radio keeps working; `tools/plus_txdiag.py --health`: no crash record |
| I23-6 | A codeplug of another firmware (the "Start new" question pending): # on a search with finds | the codeplug question appears first (as for any edit); Cancel: "Stopped: 0 saved", nothing written; Start new: the zone and channels are written |
| I28-1 | Two radios with APRS (the other one beaconing a GPS position every minute); Tools > APRS > Stations heard, OK on it | Go-To "Follow <call>", arrow, distance and bearing, "Heard 0s ago"; the hint "Back ends following" |
| I28-2 | Walk away with the other radio (or let it send a new position) | within a second of its next beacon the distance / bearing change; the age starts again |
| I28-3 | Turn the other radio's APRS off for 10 minutes | from 10 minutes "STALE: heard 10m ago" in the warning colour, the last distance kept |
| I28-4 | Back; Tools > Go-To again; Tools > Map, 0 (pick it), 5; Up | "Following ended", Go-To closes; Go-To opened from Tools is not following; from the map: Go-To following it; Up: "Following ended", the waypoint / station list again |
| I30-1 | Go-To, #, then # and type KM69kw (5 5, 6, hold 6, hold 9, 5 5, 9), OK | the page shows "KM69kw", "Centre" over "39.9375 N  32.8750 E", our distance / bearing to it (with a GPS fix or a fixed position) and "Here <our locator>" (the same as Tools > Position's grid page) |
| I30-2 | OK; Back | "Saved as a waypoint"; Go-To shows waypoint "KM69kw" with the arrow towards it |
| I30-3 | # and type KM6 (or ZZ00), OK | toast "Not a locator: like KM69 or KM69kw"; OK: "# types a locator", nothing saved |
| I33-1 | Settings > Keys: SK1 press = SOS (SK1 long press left Default); Tools > SOS key | the value list offers "SOS"; Tools > SOS key: APRS SOS, DMR emergency, CW SOS (once), all Off |
| I33-2 | All Off: press SK1; OK; hold SK1 | page "No SOS actions chosen" / "Tools > SOS key picks them"; OK: toast, nothing keyed (TX LED off, the other radio hears nothing) |
| I33-3 | Tools > SOS key: CW SOS On; Home; press SK1, Back | "Send SOS now?", "CW SOS (once)", "It TRANSMITS"; Back: Home, nothing keyed |
| I33-4 | **TX: owner approval and an authorised frequency.** A simplex FM channel; radio name = the callsign; press SK1, OK | "SOS started"; the other radio hears "SOS SOS SOS DE <call>" once; toast "CW SOS sent", the page "CW SOS (once): sent", "SOS ended" |
| I33-5 | **TX: owner approval and an authorised frequency.** APRS SOS and DMR emergency On (a DMR channel with an ID; APRS configured with callsign and an amateur APRS frequency); press SK1, hold SK1 | the DMR emergency starts (the other DMR radio alarms) and the APRS emergency position goes out (aprs.fi / the other radio: Emergency); toasts "DMR emergency sent", "SOS position sent"; the page "SOS is on" |
| I33-6 | Press SK1 again; OK | "Stop SOS?"; OK: "SOS stopped", the emergency canceled ("Emergency canceled" on the other side as upstream's cancel), no more SOS positions (watch 5 min) |
| I33-7 | **TX: owner approval and an authorised frequency.** All three On on a DMR channel; press SK1, OK | the CW message first, then the DMR emergency; the APRS SOS once the CW is on air (it goes when the channel is free); note the order on the other radios |
| I33-8 | Gates: TX inhibit on (or an RX only channel, or a P25 channel), all three On; press SK1, OK | nothing keyed; toasts "CW SOS not sent: can't transmit here", "Not sent: needs a DMR channel" (and the APRS one's reason if it has one); the page names each reason |
| I33-9 | Keys locked (long *); press SK1; OK; hold SK1 (one action On, **TX: owner approval and an authorised frequency**) | the page opens while locked; OK says the keys are locked; holding SK1 confirms |
| I33-10 | Power off and on | Tools > SOS key all Off again; SK1 still set to SOS (the key assignment is stored as before) |

### Round 13 ideas 2 (docs/plus.md "Round 13 ideas 2")

| # | Do | Expect |
|---|---|---|
| RR13-1 | Boot; `tools/plus_txdiag.py --health` (read only) | normal boot; `tasks.heap_min_free` and the task stacks' free bytes as before this build (the heap and stacks were not changed); no crash record |
| RR13-2 | Tools > Spectrum (a sweep runs); then Tools > P25 search, DMR search, P25 trunk (on a P25 channel), DMR trunk (on a DMR channel), Pager with a message open, each opened from the spectrum and Back to it | each screen shows its own content, never another's (no WACN line on the search, no find rows on the trunk screens); back on the spectrum the bars and Hit / Floor lines come back within a second; OK on the spectrum tunes the VFO to its hit |
| RR13-3 | P25 search with at least two finds: # and at once Back, then open Tools > Spectrum | the save stops: toast "Stopped" or "Stopped: n saved"; any channel written in "Bulunanlar N" is one of the finds, whole (name, NAC, talkgroup as the list showed) |
| RR13-4 | With the PC live panel (`tools/plus_live.py`) open for a few minutes | the log view still shows the radio's lines (the debug log ring is now 512 B: fewer kept between reads) |
| I24-1 | Receive the other radio's P25 calls (NAC $659, TG 101) three times, 10 s apart; a DMR call or two on a DMR channel; Tools > Heard list | rows "P25 <its ID>" "TG101 $659 x3 ..." and the DMR station's; newest on top; the age counts up |
| I24-2 | One long P25 call (30 s) | its row counts it once (x+1), not per second |
| I24-3 | OK on the P25 row; OK | the page shows TG 101, the ID, $659, 433.500, the calls; OK: Home in the VFO on 433.500 P25, NAC $659, talkgroup 101 (VFO settings show them); receives the other radio's next call |
| I24-4 | OK on a DMR row; OK | the VFO on that frequency in DMR with the colour code and slot heard; toast "Talking to TG ..."; a call from that talkgroup is heard |
| I24-5 | Power off and on; Tools > Heard list | "Nothing heard yet" |
| I26-1 | Tools > Spectrum around an active frequency (a repeater, or the other radio keyed on and off) | the bars on top, the waterfall below fills from the top a row a sweep; a carrier shows as a bright column where it is; the hint names * |
| I26-2 | *; * again; Up / Down (another center) | "Waterfall off" (bars use the whole height); "Waterfall on" (the rows start empty); a new center starts the rows over |
| I26-3 | With the waterfall open for a minute: `tools/plus_txdiag.py --health` (read only); then Back and read again | `tasks.heap_min_free` stays at 1 KB or more with the waterfall (about 1.3 KB less than without); after Back the heap's free bytes are back; note both numbers in the log |
| I26-4 | Open the spectrum on the APRS frequency in FM (APRS receive on) | as I26-1 (the APRS ring stays during the sweep): no "No room"; if "No room" shows, note heap_min_free |
| I27-1 | Map track on (it records while the GPS has a fix); walk or drive 2-3 km away from a repeater on its output (FM), the squelch open or with its beacon / traffic; Tools > Map, * | toast "Coverage: track by signal"; the track's points coloured green near, yellow, then red far; the legend "S7+ S5 <S5" under the map |
| I27-2 | * again; power off and on | "Coverage off"; after power on coverage is off (and the track empty, as before) |
| I29-1 | Tools > GPS standing still with a fix for 2 minutes | speed 0 (or 1), heading "-", altitude, "Fix 3D, n satellites"; trip stays 0 m |
| I29-2 | Drive (or walk) a known distance, about 1 km, with the screen closed part of the time | trip within a few % of the distance; top speed about the highest speed driven; heading's compass point and degrees match the way |
| I29-3 | OK; hold OK | "Hold OK to reset the trip"; "Trip reset", trip 0 m, top speed 0 |
| I34-1 | Tools > Clock without a GPS fix since power on | "No clock: needs GPS"; the title shows no time |
| I34-2 | Stopwatch: OK, # twice, OK; # | runs; Lap 1 / Lap 2 with their own times; stops; # resets to 0:00.0 |
| I34-3 | Countdown: down to 0:30 with Left; OK; go Home (or any screen) | after 30 s a tone and the banner "Countdown done" on that screen, the backlight on |
| I34-4 | With a GPS fix: the title shows local time (+03:00) equal to a phone's; Alarm two minutes ahead, OK (On); open Tools > Map | at that minute a tone and the banner "Alarm hh:mm" over the map; the alarm row shows Off afterwards |
| I34-5 | Alarm set and on; lose the fix (indoors) until it is due | it still rings on time (the clock runs on without the fix) |

### DOTA Transfer (docs/dota.md, branch `dota`; two radios with the same DOTA build)

**Every row transmits from both radios (the receiver answers): owner approval for the step, an authorised amateur
frequency, Power Low, FM with a CTCSS tone. Both radios need their callsign as the radio name; DMR rows need the
sender's DMR ID set and the receiver's ID typed on the sender.** Sender A, receiver B, 2 m apart unless the row says
otherwise. Times: FM from the host test in real time (tests/plus/test_dota_fm.c), DMR docs/dota.md 8.2's estimates.

| # | Do | Expect |
|---|---|---|
| D-F1 | B: Tools > DOTA Transfer, Role Receive, FM, Frequency Default (433.450, 88.5: nothing typed), Import New zone, Zone name "DOTA1", Ready (note B's code). A: Role Send, FM, One channel (a DMR channel with a contact), the defaults, the code, Send, OK | both play "dit dit diiit"; B's speaker plays the AFSK; A "Sent 1 channel", B "Imported 1 channel, 1 zone" and the success tones; on B the zone "DOTA1" holds the channel, identical in the editor (frequency, mode, CC, slot, contact, RX group list), receive only |
| D-F2 | As D-F1 with A sending "A zone" of 16 channels | done in about 13 s (the host test's real-time figure); the zone and its channels identical on B; the contacts and RX group lists the channels use are on B once (matched by ID where B had them) |
| D-F3 | A third radio (or the SDR) on the frequency, first without, then with A's tone; then a run with B on another tone than A | the third radio's speaker opens only with the tone set; B with the wrong tone takes nothing (A: "No answer") |
| D-F4 | FM, 80 channels (Selected channels from two zones, or a zone of 80) | about 25 s (64 channels took 20.3 s in the host test); the progress counts up; a resend is a pause, never a failure; after each minute keyed A rests 5 s |
| D-S1 | Listen to A during its transmissions in D-F1 / D-F2 | A's own AFSK at the knob's level (the amplifier on during AFSK transmit). If A is silent while keyed, note it: owner answer 6 accepts a silent FM sender only when this fails |
| D-S2 | During D-D1 / D-D2 listen to A and B | while either transmits: a click every 50-140 ms (the amplifier's switch-on pop); between transmissions: the crackle while data comes in and while A waits for the answer; note any silent gap and its length |
| D-D1 | As D-F1 over DMR: both on the defaults (433.450; CC 1 / TS 1 are fixed and not shown); A's Receiver ID = B's DMR ID (B's run page shows it) | as D-F1; A and B show "On air" in turns |
| D-D2 | DMR, 80 channels | about 18 s; note the time from A's last burst to B's answer (A shows "Waiting for answer") |
| D-P1 | During D-F2: PTT on A, then Back (No); on a new run PTT on A, then OK (Yes) | the deny tone and "Stop the transfer?"; No: the transfer goes on and completes; Yes: A stops, B shows "Nothing changed" within a few seconds and its codeplug is unchanged |
| D-R1 | B: Back during a transfer, OK (Yes); on a new run, B powered off in the middle and on again | "Nothing changed"; after power on no new channel, zone, contact or RX group list on B |
| D-R2 | Refusals: A with a wrong code; B nearly full (Import Current zone with 75 of 80 used, A sends 16) | wrong code: B stays silent, A "No answer"; no room: A "Codeplug full"; B never changes |
| D-B1 | No transmission. A: Tools > DOTA Transfer, Frequency row "MHz": type 446000 OK, 162550 OK, 145600 OK, 435500 OK, 144800 OK, then 145500 OK (Turkish: the same with the radio in Turkish) | the toasts "PMR446: no repeaters allowed", "Not an amateur band", "Repeater frequency", "Satellite segment", "APRS frequency" (Turkish "PMR446: röle yasak", "Amatör bant değil", "Röle frekansı", "Uydu bölümü", "APRS frekansı"), each time the row still shows 433.450 and the source Default; 145500 is taken: 145.500, source Manual |
| D-B2 | No transmission. Type 440000 OK, Right; Left; then type 4349875 OK, Right, a code, Send (and as receiver, Ready) | Right at 440.000: "Not an amateur band", it stays 440.000; Left: 439.9875; Right from 434.9875: 435.000 shown, Send / Ready: "Satellite segment", nothing transmits |
| D-B3 | No transmission. On a simplex channel in a repeater segment (for example 145.650 or 438.800, TX allowed, nothing is keyed) and on one of the repeater list's frequencies: Frequency source Left / Right | "Current channel" is not offered (Default, Last used, Manual only); on a simplex 433.xxx channel it is |
| D-R3 | DOTA Clone of the owner's codeplug over DMR, then over FM, into a radio with a CPS-stored codeplug and room | about 3 min 40 s DMR / 4 min 45 s FM; every zone (the sender's names, " 2" when taken) and channel equal (compare with the PC app); battery and case temperature noted |

### Round 14 APRS job (branch `r14-aprs`, items 47 and 49)

| # | Step | Expected |
|---|---|---|
| A47-1 | Menu > Settings, Up once (the last group, no digit): APRS | "Beacon format: Standard", no message row |
| A47-2 | OK on Beacon format | "Mic-E", the row "Mic-E message: En route" appears; OK on it steps In service ... Priority, Off duty, never Emergency |
| A47-3 | Tools > APRS > Send position now with Mic-E / En route; the SDR or a second radio on 144.800 decoding APRS (or aprs.fi through an iGate) | one packet, destination like `TY0S5P` (not APZRDF), `` ` `` information field; the decoder shows our position, "En route", the comment; aprs.fi shows the Mic-E status |
| A47-4 | As A47-3 while moving (GPS, SmartBeaconing on) | speed and heading in the decoded packet match the GPS page; packets shorter than with Standard (note the air time if the SDR shows it) |
| A47-5 | Beacon format back to Standard; Send position now | `APZRDF` and `!` / `=` position as before |
| A47-6 | Power off and on | Settings > APRS keeps the format and message |
| A49-1 | APRS receive on 144.800, a second station beaconing (or a local digipeater); Tools > APRS > Stations heard, OK on it | its page: callsign, symbol name ("Car /> ..."), distance and direction from us (GPS fix), degrees and locator, altitude / speed / heading if it sends them, comment, "Heard: ... direct" or "via digi", packets; the age counts up every second |
| A49-2 | On the page: OK, then Back | Go-To following that station (as I28), Back returns to the page |
| A49-3 | On the page: 0 | the map centred on the station, picked, its details under the map; Back x3: the page |
| A49-4 | On the page: #, Canned text QSL, Send; Back to the page; the other station acks | "> QSL (sending)", then "(delivered)"; a message it sends us appears as "< text" (newest first) |
| A48-S | SK1 = SOS, every action Off (Settings > Keys > SOS key); press SK1, then OK | "No SOS actions" and "Pick: Settings > Keys", nothing cut at the right edge (Turkish too); OK: toast "No SOS actions: Settings > Keys", nothing keyed |
| A49-5 | Up / Down on the page | the previous / next heard station; turn the other radio off and wait until it drops out of the list (12 stations): "Not heard any more" |

### User data packs (branch `datapack`; the data chip is written only by the firmware, in 0xE70000-0xEFFFFF)

| # | Do | Expect |
|---|---|---|
| DP1 | Settings > About on a radio that never had a pack | "RadioID pack: none", "Air band pack: none"; DMR caller names and the airfield list as before |
| DP2 | Load a RadioID pack built by `tools/datapack.py radioid` from the owner's radioid.net download, through the programmer (CPS protocol, docs/plus.md "User data packs"); close the session | About: "RadioID pack: <name>, <date>, <size> KB"; a DMR call from a station not in the codeplug shows "CALLSIGN Name" |
| DP3 | Load a second RadioID pack; power off in the middle of the transfer, power on, look at About; load it again completely | after the cut the first pack is still in force; after the full load the second |
| DP4 | Load an air band pack built from the AD 2.18 pages the owner downloaded | Tools > Airfields: the listed airfields show the pack's frequencies, the others the built-in ones; About shows the pack |
| DP5 | A read-only backup of the data chip before and after DP2-DP4 (the existing backup tool), compared outside the region | identical outside 0xE70000-0xEFFFFF |
| DP6 | Tools > Airfields with the air band pack of DP4 in force: OK on an airfield the pack names, then # | its line shows the pack's tower ("+N" when the pack has more than one); the page lists the pack's frequencies; # puts them all on the air watch ring (its ATIS the last letter), Back twice ends it as before |
| DP7 | DOTA (each step transmits: approval, an authorised frequency, low power): radio A sends its air band pack (What "Air band pack") over FM to radio B (Role Receive, Import "Current zone", Ready) | B asks "Data pack: <name>, <date>, <size> KB. Accept?" (Turkish "Veri paketi: ..., Kabul?"); OK: A "Sent data pack", B "Imported data pack", B's About shows A's pack at once, B's airfields use it |
| DP8 | As DP7 over DMR (A types B's ID); then the RadioID pack (long: ~18 min FM / ~14 min DMR for ~100 KB; once is enough) | as DP7; B names DMR callers from the new RadioID pack |
| DP9 | As DP7, B answers Back (No); again, B answers nothing | A "Not accepted" both times (the second after ~20 s); B's About unchanged; B's question gone ~10 s after A stopped asking |
| DP10 | As DP8 with the RadioID pack: stop A part way (Back, OK); again, power B off part way and on | B's About shows its old pack (or "none") both times, the DMR names as before; a full transfer afterwards works |

### Round 14 air band and spectrum (docs/plus.md "Round 14: air band and spectrum", branch `r14-air`)

| # | Do | Expect |
|---|---|---|
| AW1 | Tools > Airfields near Antalya (or the fixed APRS position there); then near Ankara | LTAI's line "TWR 125.000 +1"; LTAB's "TWR 132.000" without "+"; LTAD Etimesgut shows only TWR 122.100 (no APP 119.100) |
| AW2 | OK on LTAI; Down to the last frequency; OK | seven rows (ATIS, GND, TWR x2, APP x3), the page scrolls to APP 124.425; OK tunes 124.425 in AM, receive only |
| AW3 | Air watch: on a channel (note it), Tools > Airfields, OK on LTAC Esenboğa (or the nearest busy one), # | toast "LTAC: Air watch on"; Home shows **four** rows A..D (GND 121.900, TWR 118.100, APP 119.100, ATIS 123.600 last, as VFOs, "AM"; beta.6 showed only three), the ring's period under them; the receiver hops |
| AW4 | During AW3, a transmission on one of them (an aircraft calling the tower) | the ring stops on that letter (green), the audio in AM, clear; after it ends and the Dual watch delay passes the ring goes on; note whether a weak call opens the AM squelch per letter |
| AW5 | PTT during AW3 | refused ("Air band: receive only"), nothing transmitted |
| AW6 | Back twice (or # 5) | toast "Air watch off"; the radio back on the channel noted in AW3, VFO A / B as before (check their frequencies); repeat with your own multi-watch of 3-4 channels running first: it comes back with the same letters and selected letter |
| AW7 | Start the air watch, power off and on | the radio comes up as before the air watch (your channel, your watch on or off); no air ring |
| AW8 | During AW3 within range of Esenboğa's ATIS (123.600 on the air all the time) | the ring stops on D (the ATIS) about 5 s, then goes on and passes D over for about a minute (D never green meanwhile; A..C heard as usual, a tower call on A held to its end); after the minute D holds 5 s again |
| AW10 | AM check (beta.7 candidate report "received in FM"): if a priority channel is set in Settings, keep it; AW3 on LTAC Esenboğa; look at Home's top bar and rows; listen near the airport for 2 min; then LTAB Güvercinlik (two frequencies: the pair) | top bar **"AM DUAL"** (Turkish "AM ÇİFT"; it said "FM DUAL"), every row "AM 121.900" etc., no "FM" anywhere; traffic sounds AM (clear voice, no FM hiss when a carrier is on); the ring **never** stops on your priority channel (no FM channel name or frequency appears); the pair's panes read "VFO AM"; PTT refused as AW5; Back twice: your channel in FM as before, the priority channel still works in your own scan / watch |
| AW9 | Air watch on LTAI Antalya (seven frequencies: ATIS, GND, TWR x2, APP x3) | toast "LTAI: 6/7 frequencies watched" (Turkish "LTAI: 6/7 frekans izleniyor"); six rows A..F: GND, the two towers, the three approaches; the ATIS 118.275 not among them |
| WF3-1 | Tools > Spectrum around an active frequency (a repeater, or the other radio keyed on and off); * | toast "3D waterfall" ("3B şelale"); the rows as ridges, the newest in front at the bottom, older ones smaller towards the back; a carrier is a ridge line running back; it flows a row a sweep |
| WF3-2 | * again; * again; Back and open the spectrum again | "Waterfall off" (bars alone); "Waterfall on" (flat); after * to 3D, Back and back in: the 3D view again |
| WF3-3 | 3D view open for a minute: `tools/plus_txdiag.py --health` (read only) | `tasks.heap_min_free` as with the flat waterfall (I26-3: the same 1,280 B block, nothing more); the UI task's stack free not lower; no crash record |

## 7. Not testable here

YSF (no YSF transmitter), NXDN (not in Plus; see docs/plus.md), M17 voice (Codec2 too heavy for the CPU).

## Way back

| To | Command |
|---|---|
| Classic as it was | `python -u tools/install_plus.py --port COM15 --backup backups/dm32-047-classic-20261117 --restore --out backups/restore-classic-<time> --wait 600` |
| Stock .047 | `python -u tools/install_firstboot.py --port COM15 --backup backups/dm32-047-20261113 --restore-stock --use-saved-backup --wait 600 --out backups/restore-047-<time>` (proven twice) |

The data chip is never written by these tools: after Classic's "Start new" the stock codeplug lives only in the
Firmware Utility backup and in `backups/dm32-047-20261113/q128-pass1.bin`.

## Round 14 xband (branch `r14-xband`)

Last calls (Tools > Last call, docs/plus.md "Last calls"). Receive only: nothing here transmits.

| # | Do | Expect |
|---|---|---|
| LC-1 | Hear a DMR call, a P25 call, a YSF call and an FM call with an MDC1200 or DTMF ID (the other radio, the SDR); open Tools > Last call | one row each, newest first: "DMR <name or ID>  TG..", "P25 <ID>  TG..", "YSF <callsign>", "FM MDC ...." / "FM DTMF ...", each with its age counting up |
| LC-2 | Watch channels 4 (multi-watch on), a call on C, then on A, then on D | the rows start with C, A, D (newest first), each with the caller and the talkgroup |
| LC-3 | After LC-1, on the P25 channel: OK on the P25 row (marked ">"), then OK again | the call plays again as Tools > Last call did before ("Playing the last call"), the second OK stops it ("Replay stopped"); OK on the DMR row: "No audio kept for this call" |
| LC-4 | The same station calls twice with 10 s between; then an 8-call burst from several stations | two rows for the first station; the list never holds more than 8, the oldest goes |

Cross-band store and forward (Tools > Cross-band, docs/plus.md "Cross-band store and forward"). **Transmits**: attended
only, amateur frequencies only, the owner's callsign as the radio name (the CW ID). Needs a DMR radio on A (2 m) and
one on B (70 cm), or the SDR; an FM radio or the SDR on B to hear the CW ID.

| # | Do | Expect |
|---|---|---|
| XB-1 | Tools > Cross-band: A 145500, B 433500 (typed), DMR, the side rows as they come (RX Any, TX CC 1 TS 1, TX TG As heard), Low, Start | the question "Attended only: stay at the radio while it relays. Start?"; OK: Home with CROSS in the status strip, the bottom line "Listen A  Back", then "Listen B" and back every second |
| XB-2 | While it runs: Up, Down, OK, a digit, #, the side keys, PTT; the volume knob | each key: "Cross-band on: Back stops", nothing changes; PTT: the deny tone, nothing keyed; the volume follows the knob |
| XB-3 | Radio 1 on 145.500 (CC 1, TS 1) talks 10 s to TG 9 | the DM-32 plays it; "Record A 0:10"; at its end "Send B 0:10": radio 2 on 433.500 hears the over from the DM-32's DMR ID to TG 9, the voice as radio 1 said it; then the CW ID (the callsign, 20 wpm) on 433.500 in FM; then "Listen B" |
| XB-4 | Radio 2 answers on 433.500; within 10 minutes a third over; after 10 minutes another one | the answer sent on 145.500; no ID for the second and third overs; after the 10 minutes the CW ID on each frequency it transmitted on since the last ID |
| XB-5 | Radio 1 talks 45 s | the alert tone and "!" on Home at about 38 s (the memory full); 38 s sent on B, the rest not |
| XB-6 | Refusals: B 446.00625, 439.000, 145.600, 435.500, 144.800, 145.500 (= A), 162.550; the picker's repeater channel; modes FM / FM, P25 / DMR, DMR / P25, NXDN / NXDN; a radio name that is not a callsign | the toasts "PMR446: no repeaters allowed", "Repeater frequency" (x2), "Satellite segment", "APRS frequency", "A and B are the same", "Not an amateur band", "Repeater channel (RX and TX differ)", "Mode pair not supported" (x4), "Set your callsign ..."; nothing transmits |
| XB-7 | Back while it sends (XB-3) | off the air at once, "Cross-band off"; the user's channel back (frequency, mode, power) |
| XB-8 | Start from a P25 channel while it receives; stop after one relay | it starts within 2 s; after the stop the P25 channel receives again; Tools > Last call's P25 replay: nothing kept (the memory came back cleared) |

A mode per side (docs/plus.md "A mode per side"). Same rules as above (transmits, attended, amateur frequencies). Do
XB-1..XB-3 first: these build on the DMR relay.

| # | Do | Expect |
|---|---|---|
| XB-9 | A 145500 **DMR**, B 433500 **FM**, Start; radio 1 (DMR, CC 1, TS 1) talks 10 s on A; an FM radio or the SDR on 433.500 | Home stays on "Listen A" (no "Listen B": FM is never heard); "Record A 0:10", then "Send B": an FM carrier on 433.500 without a sub-tone carrying radio 1's voice (stock codec decoded; note level and clarity), unkeyed about 0.3 s after the voice ends; the CW ID on 433.500; then "Listen A" |
| XB-10 | XB-9 with radio 1 talking 40 s | cut at about 32 s with the alert tone and "!" |
| XB-11 | A 145500 **NXDN** (needs an NXDN48 source on A), B 433500 **DMR**; the source calls group 100 | "Record A", then a DMR over on 433.500 from the DM-32's ID to TG 100 with the NXDN voice; with B **FM** instead: the voice in FM. An encrypted NXDN call: not relayed |
| XB-12 | A 145500 **P25**, B 433500 **P25**; the factory P25 radio on A (NAC 659, TG 101) talks 10 s | nothing heard on the speaker while recording ("Record A 0:10"); then on 433.500 a P25 call with B's TX NAC (set B TX NAC 659 first, or pick B from a P25 channel with NAC 659; default 293), TG 101 (B TX TG As heard) from the DM-32's DMR ID with the voice as heard (the factory radio on B decodes it); the CW ID on 433.500 (FM); then "Listen B"; the factory radio answers on B: sent on A the same way |
| XB-13 | During XB-12's session: PTT; Back while it sends | PTT: the deny tone, no P25 key-up; Back: off the air, "Cross-band off", the user's channel back (a P25 channel receives again) |
| XB-14 | Segments free (#, docs/plus.md "Segments free"). Transmits only after the question. Tools > Cross-band: the hint; B 435500, Start; then **#**; Start again (Back at the question); B 446100, 440500, 145500 (= A), Start each; **#** again, B 435500, Start; then power off and on, open the screen | the hint ends "# segs: off" ("# bölüm: kapalı"); 435.500: "Satellite segment"; after #: "Segments free" under the title, hint "# segs: on", Start asks the question (also with "Segments free" shown); 446.100 "PMR446: no repeaters allowed", 440.500 "Not an amateur band", 145.500 "A and B are the same"; after # again "Satellite segment"; after the power cycle the hint says off again. During a session # on Home: "Cross-band on: Back stops", nothing changes |
| XB-15 | Side rows (docs/plus.md "A side's filters and TX values"). Transmits. A 145500 DMR, B 433500 DMR; A RX CC 1, A RX TG 9; B TX CC 3, B TX TS 2, B TX TG 91; Start. Radio 1 on A talks on CC 2 to TG 9, then on CC 1 to TG 8, then on CC 1 to TG 9; radio 2 on 433.500 CC 3 TS 2 | the form shows the rows under B mode ("A RX CC 1", "A RX TG 9", "B TX TG 91", ...); CC 2 and TG 8: nothing recorded (Home stays "Listen"); CC 1 TG 9: recorded, sent on B: radio 2 (CC 3, slot 2) hears it to TG 91 from the DM-32's ID. Then A and B P25: A RX NAC 659, B TX NAC 1C8 (only 0-9 can be typed: type 190, then Right 56 times or hold Right), B TX TG 101; the factory radio on A with NAC 293: not recorded; with NAC 659 TG 101: sent on B with NAC 1C8 to TG 101 |

DMR receive audio (Tools > Diagnostics, docs/plus.md "DMR receive audio diagnostics"). Receive only. Needs a steady
DMR source on a channel: the other radio a few metres away on Low (its frequency, colour code, slot and talkgroup on
the DM-32's channel), the same 20-30 s of speech each time; the volume as usual. Where possible also listen to the
same over on a third, stock radio: it decodes the same transmission.

| # | Do | Expect |
|---|---|---|
| DA-1 | On the DMR channel: Tools > Diagnostics, press 9; receive one 30 s over (m0, as shipped); photograph the five `dv` / `sp` / `au` lines and note how it sounded | the counters move (`in` and `pl` about 1,500 for 30 s); note `cc`, `un`, `bd`, `ls`, `gp` and its longest, `bl` min-max, `ov`, `pk` |
| DA-2 | Press 0 once (`dv m1`), 9, the same over again; photograph | compare the sound with DA-1; `gp` stays 0 (or near it) if candidate 1 is the cause |
| DA-3 | Press 0 (`m2`), 9, the same over; then 0 (`m3`), 9, the same over; photograph each | the sound, `gp` and `fu` for paced feeding alone and for both |
| DA-4 | In m1: one over from the DM-32 to the other radio and its answer | our transmit unchanged (the modes touch only the speaker) |
| DA-5 | In m1: a key beep, the talk permit tone, the start and end of a DMR call | beeps whole (20 ms later), no click at a call's start; the last syllable of an over not cut |
| DA-6 | Power off and on; open Diagnostics | `dv m0`: the default came back |

Reading the result: a mode that sounds clean with `gp0` where m0 had `gp` above 0 -> that mode becomes the default
(candidates 1 / 2). `un` / `cc` growing in every mode -> the AMBE queue starves (DMR task or air timing). `bd` / `be`
high -> the signal, not the audio path (move the radios, check the frequency). `pk` near 32767 on normal speech ->
clipping. Every mode as rough as m0 with clean counters while the stock radio sounds clean on the same over -> the
decoder call itself (candidate 4).

Round 2 (after DA-1..3 on beta.4: `pk` about 1,000, about 2 bit errors fixed per frame at -100 dBm). Same source and
over as DA-1. Stay in m0 (the shipped feed) unless a row says otherwise.

| # | Do | Expect |
|---|---|---|
| DA-7 | Strong signal: the other radio 1-2 m away on High (or the DM-32 next to it), press 9, one over; photograph the page and note the `rx` line | `be` near 0 and `cc` / `ls` 0 at -60 dBm or stronger. If it now sounds clean, the cause was the signal (DA-10) |
| DA-8 | Press 8 (`x2`), 9, the same over; then 8 (`x4`), 9; then 8 (`x8`), 9; photograph each | louder each step (turn the knob down to keep it comfortable); note whether the crackle / robotic sound stays the same relative to the voice, and `cl` / `po` (`po` about 2x, 4x, 8x `pk`; `cl` 0 until it clips) |
| DA-9 | At the knob position you use, compare a key beep with the DMR voice at x1 | how much louder the beep is (the beep is about -12 dBFS, the voice about -30) |
| DA-10 | Only if DA-7 still shows `be` well above 0 at a strong signal: Settings > Advanced > Freq trim, try -1.0 and +1.0 ppm (then back to 0), one over each after 9 | the trim with the lower `be`; if one is much lower the receiver is off frequency (measure it properly before keeping a trim) |
| DA-11 | Power off and on | `dv m0 x1` (beta.5); `dv m3 x2` since beta.6 |

Reading round 2: `be` drops to about 0 at a strong signal and it sounds clean -> the signal / receiver (DA-10 decides
which). Clean only at x4 / x8 -> the level: x2 or x4 becomes a fixed gain (or a setting). Rough at every level with
`be` about 0 -> the decoder call itself (candidate 4), compared against a stock radio on the same over.
## Round 15 channels (branch `r15-channels`)

Zones, DMR channel rows, digital monitor, Delete channel, P25 NAC in decimal (docs/plus.md "Round 15"). Receive only
unless a row says it transmits.

| # | Do | Expect |
|---|---|---|
| Z-1 | Menu > Zones > the 27-channel zone: look at the Add channel row and the list; then on Home step Up past 26 | "Add channel 27/80"; every position listed (one that can't be read shows "27 ? Ch nnn", "Can't be read"); Home's top right counts "27/27" on the 27th. If Home still stops at 26: note whether Home shows "x/27" or "x/26", the Settings > Keys > Up/Down value and whether dual / multi-watch is on |
| Z-1b | (zone27; docs/plus.md "Zone size: the real cause") Zones > All channels > Zone options > Add unzoned channels (TR "Bölgesiz kanalları ekle"); then Home on All channels: Up / Down and the top knob through the zone, single channel mode, then dual watch and multi-watch with a letter on All channels | "Added to the zone: 1" (TR "Bölgeye eklendi: 1"; channel 27, Simplex 145.500, the one made after the zones were stored), then the same row says "Every channel is in a zone"; Home reaches 27 ("27/27") and wraps to 1 both ways, the same for the letter |
| Z-1c | Make a zone "B" with two channels; Home on B: Menu > Channels > New from current, hold OK. Home on Bulunanlar 1: the same. Home on the VFO: the editor's Save as new channel. Dual watch, B selected on zone B: New from current. Copy here on a channel of B | Each time "Saved as channel N" (TR "Kaydedildi, yeni kanal N") and the channel is the last of All channels; also the last of B when made on B or for the letter on B, at Copy here's position for Copy here; Bulunanlar 1 and the VFO leave B unchanged. Home steps onto it |
| Z-1d | Rename All channels to another name, then New from current with Home on the VFO. (Optional, on a test codeplug: All channels at 80, New from current on the VFO) | "In no zone: channel N" (TR "Bölgesiz: kanal N"); Add unzoned channels on a zone takes it. All channels full: "All channels full, in no zone: ch N" (TR "Tüm kanallar dolu, bölgesiz: kanal N"); never a channel in no zone without a toast |
| Z-2 | Add channels to a zone until it holds 80, then Add channel once more; Copy here on one of its channels; the channel editor's Zones row | "80/80" dimmed; "Zone full: max 80 channels" each time, nothing written; in the editor's Zones list "80/80" dimmed beside that zone |
| Z-3 | Menu > Zones, Create New Zone row | "n/68" with n the zones stored |
| D-1 | A DMR repeater channel, Edit > RX list: OK | the picker lists None, "Any: all TGs", then the lists; on Any: "Any: every TG on this slot", the row "All TGs TS1"; hold OK saves |
| D-2 | On that channel (DMon off) another station calls a talkgroup not in the channel's lists on TS1; then on TS2; then a private call between two other IDs | the TS1 group call plays; the TS2 call and the private call do not |
| D-3 | Edit an FM channel: Mode to DMR, look at TX contact; RX only Yes, TX contact None, hold OK | TX contact is the codeplug's first talkgroup contact; the RX only channel saves ("Saved channel n") |
| M-1 | On the D-1 channel (RX list Any): Home; press the monitor key; again | Home's DMR row shows "AllTG" (no DMon); first press: "DMon" (amber) and "Digital monitor on: every call"; second: DMon gone, "AllTG" back, "Monitor off: channel hears all TGs" |
| M-2 | On a DMR channel with an ordinary RX list: monitor key on, off; the quick tile Digital monitor on, off | DMon appears and goes each time with the toggle ("Digital monitor off"); no AllTG |
| X-1 | Menu > Zones > a zone > # on a channel that is not the one in use > Delete channel | "Delete channel?" with its name and "Leaves zones: n", "No, keep it" focused; OK: nothing changes |
| X-2 | The same, Yes | "Deleted <name>"; the channel is gone from every zone that held it and from Channels; Channels > New from current takes its number next |
| X-3 | Delete the channel in use (zone menu or Channels > OK > Delete channel) | "In use: switch to another channel first", nothing deleted |
| X-4 | Channels > OK on another channel > the last row Delete channel > Yes | the editor closes, "Deleted <name>"; after a power cycle it is still gone; a P25 channel deleted this way and its number reused for an FM channel: the new one is FM (no P25 left over) |
| N-1 | A P25 channel (NAC $293), Edit > RX NAC | the row reads "659 ($293)"; OK, 2 9 3, OK: "293 ($125)"; hold OK saves; the channel then hears the other radio programmed with NAC 293 in its factory CPS (decimal) |
| N-2 | RX NAC: OK, 4 0 9 6 | "NAC goes 0 to 4095" at the 6, the row keeps "[409] ($199)"; Back twice: "[4] ($004)" |
| N-3 | RX NAC: OK, #, OK; TX NAC: OK, 3 9 6 6, OK | RX reads "Any 3966 ($F7E)"; TX: "TX NAC can't be 3966 or 3967", the row stays open; Back x4, 1 6 2 5, OK: "1625 ($659)" |
| N-4 | RX NAC: OK, # #, then the knob / digits | "NAC: type it in hex", the hex digits with the cursor ("$2[9]3 (659)") as before; # again: decimal |
| N-5 | VFO in P25 (Quick > Mode), VFO settings: RX NAC 1110, TX NAC 1625, back to Home | Home's VFO row "P25 NAC 1110>1625 TG .." (or the shorter forms) |

## Round 15 scan (branch `r15-scan`)

Search ranges (Tools > P25 search and DMR search, docs/plus.md "Search ranges"). Receive only.

| # | Do | Expect |
|---|---|---|
| SR-1 | Tools > P25 search; Left / Right through the presets | "Range +-1 MHz  12.5k", then +-2.5 MHz, 430-440, 144-146, +-500 kHz; no "Manual" yet |
| SR-2 | 4 3 3 (the editor opens), 5, OK, 4 3 4, OK | the editor "Search range, MHz": Lower 433.5, then "Range 433.5-434  12.5k" on the search screen; OK starts the sweep there ("Sweep 433.5000" counting up to 434.0000) |
| SR-3 | * , Down Down (Step), Left Left (6.25 kHz), OK, OK | "433.5-434  6.25k"; the sweep shows 433.50625 steps (the screen shows 4 decimals: 433.5062, 433.5125); the other radio's P25 on 433.5125 or 433.50625 is found |
| SR-4 | * , Down x3 (Move), Right, then hold Right (DM-32: long Right) | Lower / Upper +6.25 kHz, then +1 MHz per repeat; hold Left: -1 MHz |
| SR-5 | * , type Lower 173, Upper 401 | toast "Range must lie in one band", the editor stays; Upper below Lower: "Upper must be above Lower" |
| SR-6 | Leave the search (Back), open it again; then power off and on | the manual range and step are kept on return; after the power cycle the default "+-1 MHz  12.5k" |
| SR-7 | DMR search: a 1 MHz range around the other radio's DMR frequency, step 25 kHz, then 12.5 kHz | the find on 25 kHz steps only when the frequency is on the 25 kHz grid; on 12.5 kHz always |
| SR-8 | On the search screen with the editor closed, press * | the editor opens (not quick settings); on any other screen * opens quick settings as before |

Faster searches (docs/plus.md "Faster searches"). Receive only.

| # | Do | Expect |
|---|---|---|
| SF-1 | P25 search, 430-440 MHz, 12.5 kHz, quiet band; time two passes with a watch ("Pass N") | about 20 s a pass (before: about 40 s); the floor near -120..-125 |
| SF-2 | DMR search, 430-440 MHz, quiet band | about 60 s a pass (before: about 80 s) |
| SF-3 | The other radio sends P25 (NAC 0x659, TG 101) on 433.500 MHz, 5 s overs every 20 s; P25 search +-1 MHz around it | found on 433.5000 (not 433.4875 or 433.5125) within one pass; no probes ("Probe") on its neighbours |
| SF-4 | The other radio in DMR direct mode (simplex) on 433.450, 5 s overs; DMR search +-1 MHz | found on 433.4500 with its CC and slot within one pass; repeat 5 times |
| SF-5 | An FM signal (the other radio in FM, a long over) inside the P25 search range | probed once ("Probe" 433.xxxx), then passed: the next probe of it no sooner than 10 s, then 30 s while it stays |
| SF-6 | A weak P25 signal (other radio at low power, far away, about -110 dBm on Home) | still found; if it is missed while the strong one of SF-3 is found, note its level (the quick reading may be slower than 25 ms) |

The multi-watch ring (docs/plus.md "The multi-watch ring"). Receive only except MR-5 (PTT on the selected letter, the
owner's channel and licence).

| # | Do | Expect |
|---|---|---|
| MR-1 | Watch channels 6, all FM, quiet | Home "Ring 0.3 s" (before: 0.9 s); the rows' receiver marker (or the audible clicks, if any) goes round visibly faster |
| MR-2 | Same ring with 4 DMR + 2 FM channels | "Ring 0.5 s"; watch whether the FM <-> DMR switch slows it (time 10 rounds with the debug link or by ear) |
| MR-3 | The other radio calls on F (FM), 3 s overs, 10 times at random moments | every over caught, the first syllable at most cut (before: up to 0.9 s lost) |
| MR-4 | The same on a DMR letter (repeater or direct) | every over played (late entry as before) |
| MR-5 | A P25 letter (C); the other radio sends **FM** on C's frequency for 20 s, then calls on A (FM) | the ring does not stop on C for long (about 1.2 s, then C is passed over while the FM lasts); the call on A is heard (before: the ring froze on C until the FM ended) |
| MR-6 | The other radio sends P25 on C with the right NAC and talkgroup (TG 101, NAC 659) | held, the native receiver plays it, given back 1.5 s after the last voice, the ring goes on after the wait |
| MR-7 | Watch channels 2 (dual watch) | as before: 150 ms a stop, no "Ring" line |
| MR-8 | A weak FM signal near the squelch on one letter | it opens there as before (a single reading at 50 ms decides a quiet stop, as the squelch itself does); note if it is missed more than with the build before |

## Round 16 airprov (branch `r16-airprov`)

Airfields by province (Tools > Airfields, docs/plus.md "Airfields by province"). Receive only.

| # | Do | Expect |
|---|---|---|
| AP-1 | Tools > Airfields | the first level: "All airfields (74)" (Turkish "Hepsi (74)"), "01 Adana (2)", "02 Adıyaman (1)", ... in plate order, only provinces with airfields, the last "76 Iğdır (1)"; Up from All goes round to it |
| AP-2 | Down to "06 Ankara (5)", OK | the title "Airfields Ankara", Güvercinlik, Etimesgut, ... only Ankara's five, nearest first with distance and direction; Up / Down go round within the five |
| AP-3 | In Ankara's list, # | the nearest Ankara airfield's details; Back: Ankara's list; Back: the first level with Ankara still picked; Back: Tools |
| AP-4 | "34 İstanbul (5)" | Atatürk, Hezarfen, Samandıra, Sabiha Gökçen and İstanbul; "41 Kocaeli (1)" holds Cengiz Topel only; "33 Mersin (1)" Çukurova |
| AP-5 | "07 Antalya (2)", OK on Antalya, Down to a frequency, OK | the list line "TWR 125.000 +1"; the details list all seven; OK tunes the VFO in AM (receive only) |
| AP-6 | In Antalya's details, # | air watch on its six frequencies as before (Home's ring A..F); Back Back stops it |
| AP-7 | "All airfields", OK | the whole list nearest first as before; # on the first level opens the nearest airfield's details |
| AP-8 | With an air band pack installed (Settings > About shows it) | a province's list shows the pack's frequencies for the airfields it names, as "All" does |

Spectrum in colour (Tools > Spectrum, docs/plus.md "The spectrum in colour"). Receive only.

| # | Do | Expect |
|---|---|---|
| SC-1 | Tools > Spectrum on a busy band (or the other radio sending FM nearby), waterfall view | quiet points dark blue, weak ones blue / cyan, stronger green, yellow, orange, the strongest red; the bars and texts as before |
| SC-2 | * (3D waterfall) | the ridges in the same colours by height: the strongest peaks red |
| SC-3 | Settings > Display > Theme: Light, back to Tools > Spectrum | a pale blue for the weakest, darker blue, teal, green, dark yellow, orange, red on white; every colour distinct |
| SC-4 | Look closely at the texts on the spectrum screen (title, frequency, Hit / Floor, the key hint) in both themes | readable; their edges may look a little harder than on other screens (the palette's entries go to the colours first) |
| SC-5 | * again (bars alone), then *: waterfall | bars alone without colours; the waterfall back in colour |

## Status strip signal bars (tools/plus_sigbar.py)

| Row | Do | Expect |
|---|---|---|
| SB-1 | An FM channel with nothing on it, squelch at its usual level; watch the top right for a minute | the signal bars stay empty (they climbed and fell before) |
| SB-2 | The other radio sends FM on that channel, near and then far | the bars fill while the squelch is open (more when near), empty again after the over |
| SB-3 | A DMR channel: idle, then a call from the other radio | empty while idle; filled during the call; empty after it |
| SB-4 | A P25 channel: idle, then a call | the same as SB-3 |
| SB-5 | Monitor key held on an empty FM channel | the bars show the noise level while the monitor is open (the receiver is open: expected) |

## All digital search (tools/plus_alldigital.py)

Tools > All digital search (Turkish "Tüm dijital arama", docs/plus.md "All digital search"). Receive only: nothing is
keyed in any row. The other radio sends on 433.500 (or a free authorised frequency) as each row says.

| Row | Do | Expect |
|---|---|---|
| AD-1 | Tools: the list's last entries | "All digital search" ("Tüm dijital arama") after CW decoder, before Battery; the other tools keep their numbers |
| AD-2 | Open it, range +-1 MHz around 433.500, OK; nothing on air | "Sweep 432.5000 ..." steps on; "No digital found yet" ("Henüz dijital bulunmadı"); about 16 s a pass |
| AD-3 | The other radio calls P25 (NAC 659, TG 101) | "Probe P25 433.5000", then "P25 433.5000" in green and the call plays; the find "433.5000 P25 NAC 659 ..." with "TG 101 from <ID>"; 2 s after the over the sweep goes on |
| AD-4 | The other radio calls DMR (direct mode, CC1, TS1) | "Probe P25 ..." about 0.7 s, then "Probe DMR ...", then the call plays; the find "DMR CC1 TS1 DIR" with its talkgroup and source |
| AD-5 | The other radio calls YSF (DN, DG-ID 0) | P25 and DMR run out (about 1.4 s), then "YSF 433.5000" plays; the find "YSF DG 0" with "from <its callsign>" |
| AD-6 | The other radio calls NXDN48 (if it has NXDN; RAN 1) | after P25, DMR and YSF (about 2.1 s) "NXDN ..." plays; the find "NXDN RAN 1" with group and source. Note it if no NXDN radio is at hand |
| AD-7 | The other radio sends FM (no digital) for 30 s | the four probes (about 3 s), then the sweep goes on and 433.500 is not probed again for a while; no found entry |
| AD-8 | After AD-4: the DMR radio calls again a minute later | caught at the next revisit (a few seconds), probed as DMR first (no P25 wait) |
| AD-9 | PTT during "Probe P25", during "Probe DMR", while sweeping | nothing transmitted (red LED off, the other radio hears nothing); the search stops; the channel is back |
| AD-10 | Leave the screen with Back; and again leave it with the side key / Home without stopping | Back stops at once; otherwise the search stops about 5 s later, the channel back |
| AD-11 | Up / Down to the P25 find, OK; then the YSF and DMR finds | the VFO on 433.500 in P25 (NAC Any) hears the next P25 call; YSF in YSF (DG-ID Any); DMR with CC and slot as DMR search does |
| AD-12 | Tools > DMR search after AD-3..AD-5; then back to All digital search, OK | DMR search starts with an empty list; the all digital search's list was cleared by it (the two share one list) |
| AD-13 | The PC programmer's search log during AD-3..AD-6 | a row per call with the mode P25 / DMR / YSF / NXDN, its code ($659, CC1, DG0, RAN1), the YSF callsign in the caller column |
| AD-14 | Turkish: Settings > Language Türkçe, open the screen | title "Tüm dijital arama", "Durdu: OK başlatır" / "Tarama ..." / "Deneme P25 ..." texts fit the screen |
| SL-1 | PC search log during a P25 search, a call whose talkgroup comes late (start the other radio on TG 101 right after a call on TG 102 at the same frequency) | the PC total equals the calls made; TG 101 and TG 102 lines each with their own calls; no line with group "-" left over |
| SL-2 | PC search log running; pull the cable 30 s, plug it back (same COM port); then leave the PC idle for an hour (longer than its sleep time) | "waiting for the radio" while out, the table kept, logging goes on by itself; Windows did not sleep |
| SL-3 | Open the session CSV in Excel while logging; close it after a few calls | Excel shows columns (Turkish letters right); a "-open.csv" message while open; after closing, the session file has every call and the -open file is gone |
| SL-4 | Restart the radio while the PC logs; restart the PC program and start again | counts go on without a jump; the new program run writes a new session file, the old one stays |
| SL-5 | All digital search: a YSF call on one frequency, then a YSF carrier with no callsign yet on another | the second find has no callsign until its own call is decoded (not the first call's) |

### Found list: one entry per frequency + code + group (branch `finds`)

| # | Steps | Expected |
|---|---|---|
| FL-1 | P25 search around the source (e.g. 159.125 NAC $178 TG 1, or 433.500 NAC $659 TG 101); five calls a minute apart, short and long, the screen and the PC search log open | ONE entry for the frequency (never twice, also right after a call ends); the PC log's line for it counts 5 calls; the entry's TG and ID are the call's |
| FL-2 | The same frequency and NAC, the other radio on another talkgroup (e.g. TG 102), then back on TG 101 | two entries for the frequency (TG 101, TG 102), each with its own calls on the PC; the one heard last on top |
| FL-3 | The same frequency, another NAC (e.g. $293) | its own entry; FL-1's entries unchanged |
| FL-4 | DMR search: calls on TG 91 and TG 9 (TS1 / TS2 or one after the other) on one frequency; then All digital search with FL-1..FL-3 | DMR: one entry per talkgroup with its calls; all digital: the same per mode, a mixed P25 / DMR frequency one entry per mode and group |
| FL-5 | More than 8 keys found | the oldest (least recently heard) entry drops, the new one on top |

## Search reliability (branch `searchfix`, docs/plus.md "Search reliability")

The owner's beta.7 report: the DMR search did not catch the other radio sending DMR direct mode in the same room. Receive
only in every row: our radio keys nothing (PTT stops the search). The other radio transmits on the owner's authorised
frequencies (the owner's licence). Rows SX (SR-1..8 are the search range rows above). Write down the search screen's
"Sweep 436.xxxx  NN/s" (steps a second), "Pass N  hits H  floor F" before and after each row; those three figures
settle what the host model could only assume.

| # | Do | Expect |
|---|---|---|
| SX-1 | Nothing on air. DMR search, the range 436.000-436.500 typed in the range editor (`*`), step 12.5k; let it run 30 s | "Sweep ... 13/s" (a quiet DMR step 75 ms); Pass goes up about every 3 s; **hits stays 0**; floor steady. Write down the floor. Hits climbing on a quiet band = the floor problem this branch fixes; report the floor |
| SX-2 | Same, P25 search | "40/s"; hits 0; floor the same as SX-1 within a few dB |
| SX-3 | **The owner's case.** The other radio: DMR direct mode (simplex), 436.275, CC1, TS1, TG 1, LOW power, same room, a few metres away; overs of 5-10 s with 5 s pauses. DMR search 436.000-436.500 already running | found as "436.2750 CC1 TS1 DIR" with "TG 1 from <its ID>" and the call plays, normally within the first over (host model: median 2.5 s, 9 of 10 within 20 s, 195 of 200 within a minute). Never a find on 436.2625 / 436.2875. Before this branch: 21.5 s median, 15 % not within a minute (the model) |
| SX-4 | SX-3 with the range typed 436.010-436.500 (off the channel grid) | the screen's first step is 436.0125 (the steps stay on the 12.5 kHz grid); found on 436.2750 as in SX-3 |
| SX-5 | SX-3 on 433.200 with the range 436.000-436.500 | not found (433.200 is outside the range: correct). With the range 432.700-433.700 (or +-500 kHz around 433.200): found on 433.2000 as in SX-3 |
| SX-6 | SX-3 with the other radio on HIGH power, 1 m away (the front end overloaded) | found on 436.2750 (the middle of the strong steps); "hits" grows by 1-3 per over at most (the steps next to it are not probed one by one) |
| SX-7 | All digital search, SX-3's settings | "Probe DMR 436.2750" directly (no "Probe P25" first: the readings rose and fell as a direct mode carrier's), then the call plays; normally within the first over |
| SX-8 | P25 search: the other radio P25 (NAC $659, TG 101) on 433.500, LOW power, same room, 5 s overs; range +-1 MHz | found on 433.5000 within 1-2 overs; hits small (no probes of 433.4875 / 433.5125) |
| SX-9 | DMR search on 6.25 kHz raster: the other radio on 436.28125 (if it can), range 436.000-436.500, 12.5k | found on 436.2812 (the half step between 436.2750 and 436.2875); with 6.25k steps found on 436.2812 too |
| SX-10 | The other radio sends FM (a long over) on 436.300 inside SX-3's range | "Probe 436.3000" once, then the sweep passes it (10 s, then 30 s while it stays); a DMR over on 436.275 meanwhile is still found |
| SX-11 | During SX-3, key the other radio for 1-2 s only, several times | each short burst may be missed (the probe needs a DMR sync: once per 360 ms in a call), but the frequency is **never skipped** for the next over: the next 5 s over is found |
| SX-12 | Leave the search screen during a probe; Back; PTT during "Probe" | as before: stops (5 s after leaving), Back stops at once, PTT transmits nothing |

What to send back if SX-3 still misses: the three screen figures (steps/s, hits, floor) before and during the other
radio's over, whether "Probe ..." showed and on which frequency, and how long the over was. A floor that jumps up by
20-30 dB during the over says the receiver's level reading (FD6818 register 0x67) is desensed across the range by the
strong signal; "Probe 436.2750" shown but no find says the DMR modem's lock in the 1.1 s window is the limit.

## Faster searches (branch `fastsrch`, docs/plus.md "Faster searches (beta.9 candidate)")

Receive only in every row: our radio keys nothing (PTT stops the search). The other radio transmits on the owner's
authorised frequencies. The PC's Search log page runs in every row (it shows the stats line once a second and the CSV
keeps it); held 0 on the search screen switches fast / classic ("Search: fast" / "Search: classic (beta.8)"). Nothing
here was measured yet: the expectations are the host model's.

| # | Do | Expect |
|---|---|---|
| FS-1 | Nothing on air. DMR search 430-440 MHz (range editor `*`), 1 minute classic, then 1 minute fast | "pass" about 60 s classic, about 40 s fast (801 steps); probes 0 in both; the screen "Sweep ... 13/s" classic, "20/s" fast. Probes or hits on fast only = the 10 ms settle is too short on this radio: send the stats lines |
| FS-2 | The other radio DMR direct mode 436.275, CC1, TS1, low power, 5-10 s overs, 5 s pauses. DMR search 436.000-436.500: classic 2 minutes, then fast 2 minutes (start each anew so FIRST_MS counts from the start) | both find "436.2750 CC1 TS1 DIR"; fast's "first find" and "seen->found" about half classic's (model: median 2.6 -> 1.3 s); calls a minute about equal; **"early" stays 0** in both |
| FS-3 | FS-2 with All digital search | found as DMR in both; fast first find shorter (model 2.9 -> 1.6 s); **"early" stays 0** |
| FS-4 | The other radio P25 (NAC $659, TG 101) on 433.500; P25 search +-1 MHz, classic then fast | about the same in both (the P25 sweep didn't change); write down "sync" and "max": the native receiver's lock time |
| FS-5 | The other radio's over cut short while the screen shows "Probe 436.2750" (unkey at once), then keyed again within 10 s; DMR search 430-440 MHz | fast: "gone" +1, the next over found within a few seconds ("watch" +1); classic: found only when the pass comes back (up to a minute) |
| FS-6 | Two radios if available: FM on 436.300 (long over) and DMR on 436.275; All digital search 436.000-436.500, fast | the FM carrier probed twice, about 10 s apart ("no sync" +2), then left alone for 2 minutes; the DMR found. Held 0 twice (back to fast): the skips are cleared, the FM carrier is probed again |
| FS-7 | A strong FM carrier (the other radio FM, same room) on 436.300; DMR search 436.000-436.500, fast, 1 minute | "probes" grows by about 2 then stops (the carrier, not its neighbours: no probe on 436.2750 / 436.2875 unless something is there) |
| FS-8 | Safety as before, fast: PTT during "Probe"; leave the screen; a watch running then OK on the search | PTT stops the search and keys nothing; it stops 5 s after leaving; "Stop the scan or watch first" |

## Faster channel scan (branch `fastscan`, docs/plus.md "Faster channel scan", v1.0.1 candidate)

Receive only in every row: our radio keys nothing. The other radio transmits on the owner's authorised frequencies.
Held 0 on Home while a zone or VFO scan runs switches fast / classic ("Scan: fast" / "Scan: classic (v1.0.0)", Turkish
"Tarama: hızlı" / "Tarama: klasik (v1.0.0)"). The counters are the debug link section `scanst` (GET_VIEW 31; not on the
PC programmers' pages yet): send the `scanst.*` lines of each run. Nothing here was measured yet: the expectations are
the host model's.

| # | Do | Expect |
|---|---|---|
| SF-1 | Nothing on air. A zone of 16 FM channels (or the owner's own zone), scan 30 s on fast, read `scanst`; held 0, 30 s on classic, read it again | fast: PASS_MS about 0.9 s for 16 FM channels (FM_MS / FM_N about 52-56 ms), FALLBACK near 0, NOTICED 0; classic: about 2.4 s (150 ms a stop). FALLBACK high on fast = the readings 30-50 ms after the retune are not quiet on this radio (the noise reading's dip lasts longer, or a strong signal nearby): send the lines |
| SF-2 | The same zone with some DMR channels | DMR_MS / DMR_N about 100 ms on fast, 150 on classic |
| SF-3 | The other radio FM on one of the zone's channels, low power, a few metres away, overs of 3-5 s with 5 s pauses; 2 minutes fast, 2 minutes classic | both stop on it and play it; fast stops sooner after the over starts (model median 0.6 s against 0.7 s in 16 channels, more overs caught in bigger zones) and misses no over that classic catches |
| SF-4 | SF-3 with the other radio far away or behind walls so the signal only just opens the squelch (Home's meter near the squelch level) | fast catches at least as many overs as classic. **Fewer on fast = the noise reading lags a new carrier by about 30 ms or more (unmeasured; the one limit the model shows)**: report it and hold 0 for classic (v1.0.0's scan) until it is fixed |
| SF-5 | The other radio DMR direct mode (CC1, TS1, a channel of the zone) in the same room, overs of 5-10 s; fast, then classic | both stop on it (probe, then the call plays if the talkgroup matches); fast sooner; the counters' NOTICED grow |
| SF-6 | The other radio P25 (NAC $659, TG 101) on a P25 channel of the zone, overs of 5 s | both stop and the native receiver plays it; fast sooner |
| SF-7 | Fast: monitor held while scanning (FM channel); PTT while stepping and while stopped on a signal; # 1 skip, # 5 priority; a VFO band scan | as in v1.0.0: monitor pauses (on a DMR channel the key toggles DMR monitor, as before), PTT goes back to the start channel / talks back on the signal, skip and priority as before; the VFO scan steps about 2.8x faster on FM |
| SF-8 | Dual watch and a triple watch running | unchanged (their own timing); held 0 there does nothing new |

## A zone scans slowly (branch `zonescan`, docs/plus.md "A zone scans slowly", v1.0.2 candidate)

Receive only: our radio keys nothing. Reading the counters opens the serial port: each run of
`pc/radiotools/plus_live.py` needs the owner's approval in chat first (read only: GET_VIEW queries only).
**The cause of the owner's slow zone is not proven** (host-tested mechanism only): ZS-1 and ZS-2 are the evidence.

| # | Do | Expect |
|---|---|---|
| ZS-1 | **Before installing 1.0.2, on 1.0.1 (the evidence).** Nothing on air. Write down the zone you made, channel by channel in its order: the frequency (so the band: 2 m, 70 cm, air band 118-137), the mode (FM / DMR / P25 / NXDN / dPMR), AM or not, an RX tone or not; how the zone was made (Zones > New + Add channels, Find & save, DOTA, a CPS); the channel Home is on when you start; your scan dwell, resume and priority channel settings. The same list for what All channels holds around it is not needed (send its channel count). Start the scan on Home in that zone, 30 s, then read `python pc/radiotools/plus_live.py --port COM14 --count 1 --scan-stats` (1.0.1 shows `scanst.st` and `scanst.quiet` only); stop; the same in All channels from the same kind of channel | 1.0.1 slow zone: NOTICED / HELD high with nothing on air, or FALLBACK about equal to STOPS (fast) = quiet channels held or handed to classic. Send both runs' lines and the notes |
| ZS-2 | 1.0.2: ZS-1 again in both zones, fast and classic (held 0 switches), each 30 s, `--scan-stats` after each | in both zones PASS_MS / PASS_STOPS about 55 ms (fast, FM) or 150 ms (classic); NOTICED 0, FALLBACK near 0; `scanst.cost` STEP_MAX and GAP_MAX a few ms (a GAP_MAX over 25 ms = a step still reads slowly: send it); `scanst.slow` names the longest stop: with nothing on air under 200 ms. Still slow: `scanst.slow` CAUSE and `scanst.fb` REASON name it (noise: a channel whose noise reading never reads quiet; am: AM channels; dmrcfg: steps into DMR; `cost` BANDS high: VHF / UHF jumps) — send the lines and the ZS-1 list |
| ZS-3 | ZS-2 started from a P25 channel of the zone (and from an FM one) | the same speed from both start channels (1.0.1 read the start channel at every stop) |
| ZS-4 | A priority channel set from All channels (Zones > the channel > # > Set as priority), not in the zone you made; scan the made zone 30 s | one short stop on it about every 2 s (`scanst.cost` OTHER_ZONE about 15 in 30 s), the zone's own channels as fast as in ZS-2 |
| ZS-5 | The other radio FM on a channel of the made zone, overs of 3-5 s; then DMR direct mode on a DMR channel of it | both stop and play as in SF-3 / SF-5; `scanst.slow` names that channel with WHY 3 (heard, held) or 7 (fast handed it to classic first); `scanst.fb` its channel and a noise under the line |

## Scan held on the false carrier after a retune (branch `scanslow`, docs/scan-slow-2026-11-25.md, v1.0.3 candidate)

Receive only; nothing keyed. Host-tested only until these rows run.

| Row | Do | Expect |
|---|---|---|
| SS-1 | **On 1.0.2 first (the evidence).** The scan that is slow, nothing on air, 30 s fast then 30 s classic (held 0); `python pc/radiotools/plus_live.py --port COM14 --count 1 --scan-stats` after each | the mechanism if NOTICED / HELD is close to STOPS and PASS_MS / PASS_STOPS is near 2000 ms; `scanst.slow` CAUSE held / open / noise. Send the lines |
| SS-2 | The `scanslow` build: SS-1 again | NOTICED / HELD about 0; PASS_MS / PASS_STOPS about 55-95 ms fast, 150 ms classic; `scanst.cost` DIPS (7th field): close to STOPS = the radio's dip is longer than 30 ms, near 0 = it is not (then send everything); DIP_HOLDS (8th) small |
| SS-3 | The other radio weak FM (just over the squelch) on one channel of the zone, overs of 3-5 s, both algorithms | stops and plays as before (SF-4); a strong station too |
| SS-4 | The 5-channel P25 zone, nothing on the air, 60 s fast then 60 s classic; `--scan-stats` and the native counters (0x21 [12]) after each | after the first pass (about 10 s) the stops are quick: PASS_MS / PASS_STOPS about 60 ms fast, 170 ms classic (1.0.2: 1,928 ms); `scanst.cost` P25_LEARNED (9th field) about 5, P25_MASKED (10th) climbing; native `takes` stops growing after the first pass (about 5 more every 2 minutes) |
| SS-5 | The same P25 zone, the other radio P25 on one of its channels, overs of 3-5 s, both algorithms | stops and plays (1.0.2 did on ch 2); a take there hears frames and learns nothing (P25_LEARNED does not grow on it) |
| SS-6 | The 3-channel zone (ch 1 DMR, ch 2 P25, ch 3 FM), the other radio DMR on ch 1, overs of 3-5 s, fast then classic | **stops and plays** (1.0.2: never, either algorithm); `scanst.step` names the longest step and its modes (send it: what the ~168 ms switch costs is not known); `scanst.fb` on ch 1 shows the DMR reading |
| SS-7 | SS-6 with nothing on the air | DMR stops now listen a full dwell after the switch (a DMR stop about 170 ms + its step); the pass is slower than on 1.0.2 by about the dwell per DMR stop, as it should be |

## Upstream RadioFoundry fixes (branch `rfports`, docs/plus.md "Upstream RadioFoundry fixes")

Each step that opens the serial port, installs, restarts or transmits needs the owner's approval in chat first.
TX only on frequencies the licence allows, the other radio listening.

| # | Do | Expect |
|---|---|---|
| RF-1 | **625b921.** An FM channel with an RX CTCSS tone (e.g. 88.5 Hz), receive on it a few seconds; then change to a DMR channel (simplex, CC1, TS1, TG 1) and key a 5 s over; the other radio on that DMR channel | the other radio hears the over (caller ID and audio), every time in 5 tries. Upstream before the fix: the overs went out but none decoded |
| RF-2 | RF-1 with an FM channel with DCS (e.g. D023N), then D023I | heard each time |
| RF-3 | Dual watch / multi-watch ring with a toned FM channel and the DMR channel; let it rotate, then PTT on the DMR channel; then a P25 over after the toned FM channel | DMR heard; the P25 over decoded by the other radio too |
| RF-4 | **947c139.** DMR channel with polite admit (own CC); the other radio sends an over, wait 2 s after it ends, PTT | keys first time (never refused "busy" after the channel went quiet); during the other radio's over: refused, as before |
| RF-5 | **916ac26.** A long DMR session (an hour, overs every few minutes, zone changes in between); then the DMR diagnostics line 6 | every over keys first time; note the line's " k" count (0 is normal; more means stuck requests were dropped and recovered). Report any "TX for a split second then idle" (Open25 #28) |
| RF-6 | **577c242 (evaluation).** SDR: our P25 TX on 433.500 (NAC $659, TG 101), measure the outer deviation (C4FM, nominal 1800 Hz) | about 1.8 kHz; write the figure down. Well below (about 1.69 kHz) means the native path needs upstream's correction |
| RF-7 | **f5d2fd1 (second round).** DMR simplex channel with polite admit (own CC); the other radio sends a short over; press PTT at once when its over ends (within a quarter second), 5 times | keys every time (before: often "busy"/refused in the first 0.36 s); a PTT pressed during the other radio's over is still refused. Through a repeater (RMO channel): unchanged |
| RF-8 | **18fdd29.** FM 12.5 kHz channel, the other radio a few metres away (strong signal); speak S/SH/T-heavy words ("seksen sekiz, şişe, tıss") into it, squelch at the default level | no audio drop-outs on the fricatives. When the other radio unkeys, the squelch closes at once as before (no 0.4 s noise burst). A weak signal near the noise floor: behaves as before |
| RF-9 | RF-8 on a 25 kHz FM channel, then on a 12.5 kHz channel with an RX CTCSS tone (the other radio sends it) | 25 kHz: as before this build. With the tone: the same as RF-8; the other radio without the tone: stays shut |
| RF-10 | **3d5940b (idea), P25.** Leave a P25 channel receiving for 30-60 minutes with an over from the other radio (433.500, NAC $659, TG 101) every 4-5 minutes | every over heard to the end. Report the first one missed and the time since the channel was selected (Open25 #40 pattern: deaf after 10-20 minutes) |
| RF-11 | **3d5940b (idea), NXDN / APRS receive.** The same soak on an NXDN channel (if a sender is available) or with APRS receive on, with a packet every few minutes | every call / packet decoded; a miss after 15-20 minutes with the radio otherwise working is the RF #923 pattern |
| RF-12 | **f4b19e9.** TX inhibit on (Settings), then: APRS beacon due (or "send now" while the channel is busy, inhibit set during the wait), the CW beacon, an SSTV send, a DOTA transfer; and the MMDVM hotspot armed (if used) | **nothing transmitted** (red LED off, nothing on a receiver); the hotspot stops keying at once when inhibit is set and keys again when it is cleared |

## Transmit safety (branch `txsafe`, docs/plus.md "Pre-release hole hunt: transmit safety")

Each step that opens the serial port, installs, restarts or transmits needs the owner's approval in chat first.
TS-1..TS-4, TS-6 and TS-7 expect **nothing transmitted**: watch the red LED and a receiver (or SDR) on the frequencies named.

| # | Do | Expect |
|---|---|---|
| TS-1 | Triple watch, A selected on an FM channel outside the band plan (e.g. a PMR446 or business channel, or 146.520), B / C amateur simplex; Tools > SSTV > OK while the ring runs (several times, the ring on each letter) | the page says "Stop the scan or watch first" ("Önce taramayı veya izlemeyi durdurun"); nothing keyed on any letter; the watch goes on |
| TS-2 | A zone scan running, then Tools > SSTV > OK | the same refusal; the scan goes on |
| TS-3 | Spectrum, then Back and at once Tools > SSTV > OK (within the 5 s the sweep stays alive); the same after DMR / P25 / All digital search | refused ("Stop the scan or watch first") or, after 5 s, sends normally on an amateur simplex channel (ST1 / ST2) |
| TS-4 | **Removed 2026-11-22** (owner decision: band plan only for DOTA, docs/plus.md). Was: SSTV send on 146.520 (outside Region 1's 2 m band) and on 145.000 (a repeater input) as the channel on view | refused with the band plan's reason ("Not an amateur band" / "Repeater frequency"); nothing keyed |
| TS-5 | **Emergency.** Triple watch, A a DMR channel set RX only, B a DMR channel that may transmit, C FM selected: the emergency key; then with B also RX only | first: "Watch off: on B", the alarm / hot mic on B's frequency only (owner approval: it transmits on B, TG1); second: the watch goes on, the emergency says it can't start, nothing keyed |
| TS-6 | Dual watch (or a multi-watch, or the air watch from Tools > Airfields) running: Tools > All digital search, OK; then DMR search, P25 search | each says "Stop the scan or watch first" and does not start; the watch goes on; after stopping the watch the search starts |
| TS-7 | **Removed 2026-11-22** (owner decision: band plan only for DOTA, docs/plus.md). Was: All digital search finds a P25 / DMR system outside the amateur bands (e.g. 159.125 or a repeater output 438.900): OK on the find, then PTT | toast "This channel is RX only"; Home on the VFO there, PTT "TX denied" (RX only), nothing keyed; step the VFO to 433.500: PTT works again |
| TS-8 | All digital search find on 433.500 (amateur simplex), OK, PTT (licence permitting) | no toast; the VFO transmits as before |
| TS-9 | Radio name "DM32UV" (the factory name), then "TA2DOU/P": Tools > SSTV > OK, Tools > Cross-band, the CW beacon | DM32UV: "Set your callsign (the radio name) in qdmr." everywhere, nothing keyed; TA2DOU/P: TA2DOU used as the ID |
| TS-10 | Second round. A zone scan (or a watch) running: Tools > CW beacon > OK (the 146.520 half of this row removed 2026-11-22: the beacon has no band plan) | "Stop the scan or watch first", nothing keyed |
| TS-11 | **Removed 2026-11-22** (owner decision: band plan only for DOTA, docs/plus.md). Was: VFO on 159.125 (typed, or Heard -> VFO of a station there), FM then DMR, PTT; power off and on, PTT again | "TX denied" with "VFO: outside the amateur bands" / "Tune into a band, then PTT", nothing keyed, also after the restart; a codeplug channel outside the bands still transmits (the user's CPS) |
| TS-12 | **Removed 2026-11-22** (owner decision: band plan only for DOTA, docs/plus.md). Was: VFO 438.650 simplex, PTT; then set TX 431.050 (the repeater offset), PTT (licence permitting, the local repeater) | first refused ("VFO: outside the amateur bands": simplex on a repeater output); with the offset it transmits on 431.050 |
| TS-13 | DMR (or All digital) search find on a DMR repeater's output (e.g. 438.900, repeater sync), OK | toast "Repeater: set the TX offset" (the DMR repeater-sync hint, older than the hole hunt). Withdrawn 2026-11-22: the PTT refusal until the offset is set (no VFO band plan); a P25 / YSF / NXDN find on a repeater segment no longer toasts |
| TS-14 | Radio name "TC100A" (or another special-event call), Tools > SSTV / CW beacon | accepted as the ID |
| TS-15 | SFR repeater channel with its CW ID on and the radio name "DM32UV"; its role page | the repeater doesn't start, the page says "Set your callsign (the radio name) in qdmr."; with TA2DOU it starts |
| TS-16 | **Removed 2026-11-22** (owner decision: band plan only for DOTA, docs/plus.md). Was: Third round. VFO 431.050 simplex, PTT; VFO RX 431.050 with TX 438.650 (reverse split), PTT; VFO RX 438.650 TX 431.050, PTT (licence permitting) | the first two refused ("VFO: outside the amateur bands"), the third transmits on 431.050 |
| TS-17 | Radio name "IC7300", then "TC2023X": Tools > SSTV > OK | IC7300: "Set your callsign (the radio name) in qdmr."; TC2023X accepted as the ID |
| RA-1 | Home on a zone with room (not the VFO); Tools > Repeaters > All repeaters, pick an FM repeater with an access tone, press `*` (also try it in its details) | toast "Added: ch N NAME in zone ZONE" (TR "Eklendi: kanal N NAME bölge ZONE"); Home's zone and "All channels" each hold it at the end; Menu > Channels shows it with RX, TX = RX + offset, the tone as the TX tone, 12.5 / 25 kHz; Home steps onto it; with PTT it transmits on the TX frequency (licensed use only) |
| RA-2 | Press `*` again on the same repeater; then on a DMR repeater; then on a C4FM entry | "Already in ZONE: ch N", no second channel; the DMR one is added receive only with its colour code and slot (RX list All TGs); the C4FM one: "Info only: not a mode of this radio" |
| RA-3 | With Home's zone at 80 channels, `*` on a repeater; with a DMR data send running, `*` on a repeater | "Zone full: max 80 channels", nothing written; "Can't add while transmitting" |
| RA-4 | In the VFO (Home on the VFO), `*` on a repeater; on the first (province) level of Tools > Repeaters press `*`, then `#` on a list | the channel goes only into "All channels"; on the province level `*` opens quick settings as before; `#` still opens the nearest repeater's details |

## VFO step key (branch `vfostep`, docs/plus.md "VFO step key", v1.0.2 candidate)

Receive only: nothing is transmitted. English menus. SK2 is the lower of the two side keys below PTT (the A/B key).

| # | Do | Expect |
|---|---|---|
| VS-1 | Home on the VFO (Zones > VFO, or the VFO key), look at the bottom row and the top-right of the top row | the bottom row ends "SK2 step" (e.g. "0-9 freq  * Quick  SK2 step"); the top row says "Step 12.5k" or "Step 25k" on the right |
| VS-2 | Press SK2 briefly, one press at a time, 10 times (on an air band or AM VFO, e.g. 121.575, 11 times: "8.33" comes after "6.25") | each press a toast "Step 2.5 kHz", "5", "6.25", "10", "12.5" (no 8.33 on a 2 m FM VFO), "20", "25", "50", "100 kHz", then "Step Auto"; the top row's Step follows; the frequency never moves by itself |
| VS-3 | Set a step with SK2 (say 5 kHz), then press Up three times; then Settings > Talk > VFO step; then switch the radio off and on, back to the VFO | Up moves the frequency 5 kHz a press; the menu row shows 5 kHz; after the power cycle the step is still 5 kHz |
| VS-4 | On a channel (not the VFO): look at the bottom row; press SK2 briefly; then press SK2 held | no "SK2 step"; short SK2 starts dual watch as before (Back ends it); held SK2 as before |
| VS-5 | VFO on Home: hold SK2 (dual watch starts, the VFO is a letter); press SK2 briefly twice; Back; in the VFO type "4 3 3" and press SK2; start a scan (SK2 is not the scan key: use the scan key) and press SK2 | in dual watch short SK2 selects the other letter, the step does not change; typing and scanning: SK2 does not step |
| IK-1 | On a channel (not the VFO), Home idle: look at the bottom row; hold 4 about a second with no ISS pass within 10 min (needs GPS time and TLEs) | the row reads "OK Menu  * Quick  4 ISS" (Turkish "OK Menü  * Hızlı  4 ISS"; with a caller heard "* Quick  # name  4 ISS", the name cut with an ellipsis; in dual watch "# more  4 ISS  Back stop"); the hold opens Tools > ISS with the next pass time |
| IK-2 | Within 10 min of an ISS AOS (or during a pass), Home on a channel: hold 4; then any key | the radio goes to 145.800 receive-only, banner "ISS tracking, max NN" once the radio holds (or a toast "Can't track: ..."); any key ends it, the radio back as before; a short 4 still opens Find; in the VFO a held 4 types the digit |

## RAM saving: the safe-area frame buffer (branch `ramsave`, docs/plus.md "RAM budget", v1.0.2 candidate)

The frame buffer now holds only the 220 x 310 area inside the bezel; the radio paints the 10 px left / right and
5 px top / bottom edges in the background color itself (`fill_insets`). The host tests show the panel gets the same
pixels on every simulator screen, but only the real panel proves the LCD side (window addressing, the edge fill, wake
from sleep). Installing needs the owner's approval in chat; nothing here transmits. Look closely at all four edges
and the corners in every step: any stripe, garbage, a 1 px line of another color, or a picture shifted by 10 / 5 px
is a failure (photo it).

| # | Do | Expect |
|---|---|---|
| FB-1 | Power on; boot splash, then Home on an FM channel, a DMR channel and the VFO | as v1.0.1: the edges in the background color, nothing shifted, text not cut at the left / top |
| FB-2 | Menu, Channels (a long list: scroll to the end and back), Zones, Contacts, Settings pages, Tools and three tool screens (Spectrum with the waterfall, the map, SSTV) | every screen whole; the focus bar and the scroll bar reach the same place as before; no garbage left at the edges after scrolling |
| FB-3 | Settings > Display: dark, light, high-contrast theme; the filled status bar off and on; Large text on Home | the edges change to each theme's background at once (no dark frame around a light screen, no light frame after going back to dark) |
| FB-4 | Receive a call (FM and DMR) and transmit (licence permitting) | the RX / TX banner and the status bar (bars, battery, icons) draw as before |
| FB-5 | Let the backlight time out / the screen sleep, wait a minute, press a key; also the power-save sleep if used | the screen comes back whole: no stripes or old content at the edges, the edges in the current theme's color |
| FB-6 | Pop-ups: open and close quick settings, the lock (long Quick 5), a message banner | banners and pop-ups draw whole |
| FB-7 | Only if the owner uses the debug link screenshot (`rfoundry_debug.py screenshot`): take one of Home | a whole 240 x 320 picture (the packed form now answers "unavailable", so the tool reads the slower RGB565 form; the picture is the same) |
