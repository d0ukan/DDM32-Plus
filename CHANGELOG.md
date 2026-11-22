# Changelog

DDM32 Plus is a fork of Open25-DM32 Classic (r13) for the Baofeng DM-32UV. See the disclaimer in
[README.md](README.md) before installing anything.

## Unreleased

- Dual watch / multi-watch transmit fix (owner's beta.7 report: with the FM letter selected PTT went out as P25 on the
  P25 letter; PTT on a P25 letter did nothing; TALKING stayed on after release; the P25 letter went deaf until a power
  cycle). PTT now always transmits on the letter Home shows with PTT (the selected one), with its own frequency, mode,
  power, tones, CC / TS / talk group or NAC / talk group, also while the receiver is on another letter or a P25 call is
  held there (that letter is let go and the selected one tuned first); a P25 letter transmits as a single P25 channel
  does; an FM carrier is never keyed on a P25 letter; the TX indicator clears on release, end or refusal; after our
  over on a P25 letter its reply is waited for 2.5 s without the cooldown.
- Home shows each channel's own mode settings: a P25 channel "NAC $293/659 TG 42" (RX Any with its TX NAC: "NAC
  Any>$293/659 TG 1"), YSF DG-ID, NXDN RAN, dPMR colour code on the single channel's chips row; dual watch panes of
  those modes name the talk group and NAC as DMR panes do (a P25 VFO pane no longer says "VFO FM"); the multi-watch
  rows add the NAC and TG, DMR colour code / timeslot / talk group, or the FM tones, in the form that fits.
- Satellites: ISS notifications and auto track. The pass alarm page gains "Every pass" (the ISS or any satellite:
  every qualifying pass announced with the beeps and banner before and at AOS, armed again by itself and kept across
  power cycles) and "Auto track" (Off / On / APRS): at AOS, when the radio is idle on Home or the Satellites screen, it
  follows the pass's downlink with Doppler (ISS 145.800 voice / SSTV or 145.825 APRS), receive only, shows "ISS
  tracking, max 47°", and 30 s after LOS (or on any key or PTT) returns exactly to the previous channel or VFO. No
  banner while transmitting (it comes after). Stored in spare settings bits; nothing new in RAM but 24 bytes.
- SSTV send (owner's request): Tools > SSTV > OK sends a built-in test picture: the DDM32 Plus logo, your callsign (the
  radio name) and colour bars, in Robot 36 (37 s, default), Martin M1 or PD120, on the FM simplex channel in use after
  an explicit OK, only on amateur frequencies (no repeater, satellite, APRS or PMR446 frequency) and with every TX
  check PTT has. Back or PTT stops it at once. Receive it with a phone app such as Robot36 or another DM-32 with SSTV.
- P25, DMR and All digital search catch a real signal surely (the owner's beta.7 report: a DMR radio in the same room
  on 436.275 was not found and the stepping was slow). A strong carrier is now probed on its strongest step (or the
  middle of an overloaded plateau, or the half step of a 6.25 kHz channel) instead of one neighbour after the other;
  the noise floor is learned from the first step (a receiver reading high made every step a probe); the steps stay on
  the channel grid whatever start was typed; a direct mode DMR carrier gets 1.1 s, P25 1.2 s; a carrier that went off
  the air during its probe, or brought a burst, is never skipped; the All digital search probes a direct mode carrier
  as DMR first. The search screen shows the steps a second. Host model of the owner's case: found in a median 2.5 s
  instead of 21.5 s.
- Search found lists (P25 search, DMR search, All digital search): one entry per frequency + mode + NAC / colour code
  / DG-ID / RAN + talkgroup, with a call counter, the most recently heard on top; another talkgroup or NAC on the same
  frequency is an entry of its own. The beta.7 candidate listed 159.125 twice (calls 1 and 2): the native receiver's
  NAC reads "any" (0xFFFF) after a terminator or a damaged frame and the P25 search took that as another NAC; a found
  entry also no longer takes the talkgroup and source of a call heard before the probe (another frequency's).
- Air watch in AM: Home's top bar said "FM DUAL" (and a two-frequency airfield's panes "VFO FM") although every air
  letter is received in AM; it now says "AM DUAL" / "VFO AM". A priority channel set in Settings is no longer a stop
  of the air watch's ring (it put the user's FM channel in the middle of it); your own scans and watches keep it.
- P25 NAC in both forms everywhere: hex marked with "$" and the decimal, e.g. "NAC $659/1625", on Home's caller line
  (the tight form too), dual / multi-watch panes, Home's VFO row (with the TX NAC), Recent, Heard list, All digital
  search and P25 trunk (the "$" was missing there); P25 search and the NAC editor rows already showed both.
- Search log on the PC: the Windows programmer 2.0.0-beta.4 and the programmer 3.0.0-alpha.3 have a Search log page.
  With P25 search, DMR search or All digital search running on the radio and the cable in, it reads the search's
  finds every second (read only: two / three debug link views, not counted as a look, so the search still stops once
  its screen is left) and keeps every call, also all night: time, mode, frequency, NAC / colour code / DG-ID / RAN,
  slot, group, source or YSF callsign, in a table and in `logs\search-YYYYMMDD.csv`. No audio is recorded.
- Programmer 3.0.0-alpha.3: the backup and install progress bar fills over the whole run and the percent line stays
  (other tool lines go under it); a plain desktop look in both themes; "DDM32 Plus - TA2DOU" in the title (both
  programmers).
- Tools > All digital search (Turkish "Tüm dijital arama"): sweeps a range like P25 / DMR search and probes each
  carrier in the order P25 > DMR > YSF > NXDN (each with its own receiver; the first confirmed wins); found list with
  the mode and its fields (NAC / colour code / DG-ID / RAN, talkgroup, source, YSF callsign), the call played, receive
  only. The PC search log reads it as a third section `alls` (the CSV gains a `caller` column).
- DOTA Transfer: a typed frequency must be in the amateur bands (144-146, 430-440 MHz) and outside what the
  cross-band refuses there (repeater inputs and outputs, the satellite segments 145.800-146.000 and 435-438 MHz, APRS
  144.800, PMR446, the repeater list's frequencies); otherwise a toast says why ("Not an amateur band", "Repeater
  frequency", "Satellite segment", ...) and the frequency stays. Left / Right no longer step out of the bands, Send /
  Ready checks every source the same way, and the "Not amateur band: send?" question is gone (nothing is sent
  outside the bands).
- Air watch (Tools > Airfields, #) now watches every frequency of the airfield: beta.6 left the ATIS out, so
  Esenboğa's four became three letters. The ATIS goes last and holds the ring 5 s at most, then is passed over for
  60 s (its recording never stops). An airfield with more than six: the first six, and the toast says
  "LTAI: 6/7 frequencies watched" ("LTAI: 6/7 frekans izleniyor").

## v1.0.0-beta.6 (2026-11-21)

With the Windows programmer 2.0.0-beta.3 and the cross-platform programmer 3.0.0-alpha.2 (Windows; alpha.1 was replaced the same day: it listed no version when only betas were published, and had no icon).

- DMR receive audio fixed: the speaker is fed with one block of margin and at the P25 path's steady pace, and the
  decoded voice gets x2 gain (`dv m3 x2` on Tools > Diagnostics is now the power-on default; NXDN, dPMR and YSF
  share it, and P25 voice gets the same x2). On the author's radio the old feeding sounded crackly at any volume;
  this setting sounds clean. FM is unchanged: its audio does not pass a digital stage (volume knob only).
- Windows programmer 2.0.0-beta.2: new pages Radio health, Live panel and My channels (codeplug viewer with search
  and CSV export); one radio step at a time across all pages; read-only guard on the health and live readers.
- Programmers (next C# version, and the Rust one): the program folder's subfolders have English names: `backups`,
  `releases`, `updates`, `packs` (were `yedekler`, `surumler`, `guncelleme`, `paketler`). At start the program
  renames the old ones in place (nothing copied or deleted); an old backups folder that cannot be renamed is still
  searched, and a backup path saved by the old version still finds the moved backup.
- Airfields by province: Tools > Airfields opens on "All airfields" and the provinces that have airfields, in plate
  order with their counts ("06 Ankara (5)"), as Tools > Repeaters does; OK lists that province's airfields nearest
  first, with the details, listening and air watch as before. Each airfield's province comes from its position (the
  map's provinces; at a border OurAirports' own province decides: Sabiha Gökçen is in İstanbul, Çukurova in Mersin).
- Spectrum in colour: the waterfall and the 3D waterfall draw signal strength as a colour ramp (dark blue, blue,
  cyan, green, yellow, orange, red; the light theme its own shades) instead of a few grey and theme colours.
- The status strip's signal bars light only while something is heard (squelch open, the monitor key, or a DMR call at
  its own level). Before, they followed the receiver's noise floor and climbed and fell on any channel as if a call
  or data came in.
- DMR receive audio diagnostics, round 2: on Tools > Diagnostics key 8 steps a digital gain on the decoded voice
  (x1, x2, x4, x8, saturating; beeps keep their level) and a sixth line shows the samples it clipped and the peak after
  it; the dropped-blocks counter counts only blocks lost while the speaker plays (the idle stream between calls filled
  it before). Power-on is x1: nothing changes until the owner presses 8. For the radio rows DA-7..DA-11.
- Turkish: the texts the catalogue missed are translated: the multi-watch ring line ("Döngü 0.6 s"), its # menu
  ("Konuş: B", "Değiş: C", "Durdur, kal: B") and toasts, the cross-band form's Slot row, the caller line before the
  first P25 / YSF / NXDN link control, the battery's time left ("4 sa 20 dk"), upstream's zone toasts and "In Zones"
  list, About's "Based on" lines. A host test now fails when any text the radio can show has no Turkish.
- tools/datapack.py finds the aerodrome code in AIP file names such as LT_AD_2_LTAC_en.pdf (an underscore before
  the code hid it), else in the page text before AD 2.18, as the Windows programmer does.

## v1.0.0-beta.4 (2026-11-21)

Installed and checked on the author's radio with the new Windows programmer, published with it (2.0.0-beta.1: `DDM32Plus-Setup-2.0.0-beta.1.exe`).

- DOTA Transfer pairs without typing: both roles start on the same frequency (Default 433.450 MHz, a 70 cm FM
  simplex channel) and tone (88.5 Hz); the frequency can also come from the current channel (when it is a simplex
  amateur one), the last session or be typed; the tone from a short list. DMR uses colour code 1 and slot 1, no longer
  shown or asked.
- APRS Mic-E beacons: Settings > APRS > Beacon format (Standard / Mic-E) and Mic-E message (Off duty, En route,
  In service, Returning, Committed, Special, Priority; En route first). Manual, automatic and SmartBeaconing beacons
  (and the ISS one) go out as Mic-E positions with speed, heading, altitude and the comment: shorter packets. The
  emergency stays the SOS's own. Standard by default.
- APRS station page: OK on a station in Tools > APRS > Stations heard opens its page: the callsign and its symbol by
  name (car, house, ...), distance and direction from us, position (degrees and locator), altitude, speed and
  heading, comment, when it was heard (direct or through a digipeater) and how many packets, and the last messages
  from it and the one we sent it. OK: Go-To follows it; 0: the map centred on it with its details; #: a message to
  it; Up / Down: the previous / next station.
- The SOS key's page with no action chosen fits the screen ("No SOS actions") and points to Settings > Keys, where the
  actions are picked now (it still said Tools > SOS key).
- Airfields: the frequencies that are not in an aerodrome's official publication are removed (61, among them
  another unit's approach frequencies and the unofficial ones of Bolu); nothing is added. An airfield keeps all of
  its frequencies (up to 64), and the list line shows "+1" when the service shown has more ("TWR 125.000 +1").
- Air watch: on an airfield's page, # puts up to six of its frequencies (not the ATIS) on the multi-watch ring, each
  letter its own VFO, received in AM; the radio hops between them and stops on activity. Stopping the watch brings
  everything back as it was (your channel or VFO, your own dual / multi-watch); nothing of it is saved.
- Spectrum: a third view, a 3D waterfall (the last sweeps as ridges in perspective, the newest in front); * cycles
  waterfall, 3D, bars alone. Multi-watch rows in the air band say "AM".
- User data packs: a RadioID list and an air band frequency list the user builds on their own PC from sources they
  download themselves (radioid.net; the aerodromes' AIP pages) can live on the radio's data chip, in a region of its
  own (two slots each, switched only when a new pack is complete and checked). DMR callers get their names from the
  RadioID pack, the airfield screens their frequencies from the air band pack; Settings > About shows which pack is in
  force. Loaded through the programming cable with the CPS protocol (tools/datapack.py builds the pack). Without a
  pack nothing changes.
- Data packs from radio to radio: Tools > DOTA Transfer can send the RadioID pack or the air band pack in force
  (What: "RadioID pack" / "Air band pack") to another radio over FM or DMR. The receiver is asked "Data pack: <name>,
  <date>, <size>. Accept?" and keeps its old pack until the new one has arrived whole and checked; a stop, a lost link
  or a power cut leaves the old one. Radios with older firmware simply don't answer (nothing changes on them).
- DOTA over DMR: a receiver's refusal ("Codeplug full", "Other version") now reaches the sender (it was sent to no
  one, so the sender only saw "No answer").
- Last calls (Tools > Last call): the last 8 calls heard from every mode (DMR, P25, YSF, NXDN, dPMR, FM caller IDs)
  and every watched channel, newest first: the watch letter (A-F), the mode, the caller, the talkgroup, how long ago.
  The newest P25 call still plays again with OK; the other modes keep no audio. **Not tried on the radio yet**
  (docs/plus-radio-test.md, rows LC-*).
- Cross-band store and forward (Tools > Cross-band), DMR only: an over heard on frequency A is recorded and sent on
  frequency B (and B to A); both in the amateur bands, never PMR446, a repeater, satellite or APRS frequency. Attended
  operation only; while it runs the keys are locked (Back stops it) and Home shows CROSS. Up to 38 s an over (the
  memory; 3 minutes at most), the callsign in CW on each frequency used, then at least every 10 minutes. FM and P25
  are not possible this way on this radio (docs/plus.md). **Not tried on the radio yet** (rows XB-*).
- DMR receive audio diagnostics (owner's report: received DMR audio crackly and artificial): Tools > Diagnostics shows
  five new lines first (the voice queue, the speaker FIFO, the audio task's timing), and on that page 0 steps the
  speaker FIFO feed between m0 (as before), m1 (one block of margin before the FIFO starts), m2 (paced, as P25) and
  m3 (both); 9 zeroes the counters. Power-on is always m0: nothing changes until the owner presses 0. **For the radio
  test rows DA-*;** the default changes once they show which feed sounds right.
- Zones show their size where channels are added: the zone list's Add channel row "26/80", the channel editor's
  Zones "26/80", Create New Zone "5/68"; a full zone says "Zone full: max 80 channels"; a channel the radio can't
  read shows in its zone as "27 ? Ch 123" instead of vanishing (Home skips it).
- DMR channel editor: RX group list Any now means every talkgroup on the channel's slot (no longer digital
  monitor: no private calls of others, Home no longer says DMon for it); "RX list" shows "All TGs TS1"; the picker offers
  Any right after None and says in one line what Any or None hears; a channel made DMR gets the first group contact; an
  RX only DMR channel saves without a TX contact; Direct mode explains itself.
- Digital monitor: DMon on Home follows the toggle again; a channel whose RX list is Any shows "AllTG" instead, and the
  toggle says what it does ("Digital monitor on: every call", "Monitor off: channel hears all TGs").
- Delete channel: the channel editor's last row and the zone channel menu's last entry; asks first (No focused), takes
  the channel out of every zone and frees its number; refused for the channel in use and while cross-band runs.
- P25 NAC in decimal first: the editor and VFO settings show "659 ($293)" and take the decimal value (0-4095, the
  knob steps by one); # is Any / Same as RX as before, # # the hex digits; 3967 on RX and 3966 / 3967 on TX are refused
  with a reason; Home's VFO row shows the NACs in decimal.
- P25 search and DMR search: any range of the radio's 2 m / 70 cm bands. * (or a digit) opens the range editor: Lower
  and Upper typed or moved by a step (held Left / Right: 1 MHz), a step of 2.5, 5, 6.25, 10, 12.5, 20, 25, 50,
  100 kHz or 1 MHz, and Move to slide the whole range; OK applies it. Each search remembers its range and step while
  the radio is on; Left / Right still pick the presets. **Not tried on the radio yet** (rows SR-*).
- Faster P25 and DMR searches: a quiet step takes one radio poll (25 ms) instead of two in P25 search (430-440 MHz:
  about 20 s a pass instead of 40) and three instead of four in DMR search (60 s instead of 80); a strong reading is
  checked again before the frequency is probed, and a busy FM channel in the range is probed less often (30 s apart
  once it has been checked twice). **Not tried on the radio yet** (rows SF-*).
- Multi-watch with 3 to 6 channels goes round up to three times faster: a quiet channel is left after 50 ms (DMR
  100 ms) instead of the whole 150 ms dwell, so 6 FM channels take 0.3 s a round (was 0.9 s). A P25 channel with a
  signal that isn't ours (FM on its frequency, another NAC) no longer freezes the ring: it is given back after 1.2 s
  and passed over while the signal lasts. Dual watch (2 channels) is unchanged. **Not tried on the radio yet** (rows
  MR-*).

## v1.0.0-beta.3 (2026-11-20)

Installed and checked on the author's radio with the new Windows programmer (C#), the first install it did.

- Frequency on Home: a channel's receive frequency on a row of its own ("RX 438.5000  TX 430.9000", or one
  frequency when both are the same); Large text shows it as a third line (TX in red while transmitting). Not in the
  VFO, whose large line is the frequency.
- DOTA Transfer (Tools > DOTA Transfer): send one channel, selected channels, a zone, or every zone ("DOTA Clone") with
  the DMR contacts and RX group lists they use to another DM-32UV running DDM32 Plus, over FM (AFSK 1200 with a CTCSS
  tone) or DMR (data to the receiver's ID). The receiver shows a four-digit code the sender types; nothing changes on
  the receiver unless every frame arrived (a stop, a power cut or a lost link leaves its codeplug as it was).
  Imported channels are receive only unless allowed; PTT during a transfer asks "Stop the transfer?". Low / Mid /
  High power, Low by default. **Not tried between two radios yet** (docs/plus-radio-test.md, rows D-*). Design and
  limits: docs/dota.md.

## v1.0.0-beta.2 (2026-11-20)

Second pre-release, the first as DDM32 Plus. Installed and checked on the author's radio (the fixes below and
the new boot screen).

- New boot screen: the author's radio-"D" mark and "DDM32+ Plus", readable on the dark and the light theme.
- Renamed from DDM25 Plus to DDM32 Plus (repository d0ukan/DDM32-Plus). v1.0.0-beta.1 was published as DDM25 Plus;
  its program cannot update itself to a later release (new file names): download the next program zip by hand once.
  Radios running a DDM25 Plus build are still recognized, and an existing `%LOCALAPPDATA%\DDM25Plus` data folder
  (backups, downloads, settings) keeps being used.
- Tools > Marine: duplex channels (18 on) no longer overlap the channel number.
- Boot screen in the saved theme (dark stayed light before).
- Map: APRS stations and repeaters larger, in their own colours; two closer zoom steps (~80 m and ~40 m a pixel).
- Large text: Home's two lines drawn up to twice as large on the DM-32UV's screen.

## v1.0.0-beta.1 (2026-11-20, published as DDM25 Plus)

First public pre-release. **Beta:** everything below is built and covered by host tests, but most of it has not been
field-tested on a radio yet.

### Base
- Classic r13 (Open25-DM32), split RX / TX NAC (upstream RFKV format), Turkish-first UI, DDM32 Plus name and boot
  screen, public build profile without the RadioID ID database.

### Modes
- P25: NAC in hex and decimal, Any NAC / talkgroup, split RX / TX NAC, emergency and unit-to-unit calls, P25 in zone
  scan (short calls caught), P25 search and trunk following, the CH_MODE 2 channels of the DM32-nxdn-p25 format read
  as P25.
- DMR: Any colour code / slot / talkgroup, Tier III trunk following, DMR search.
- Receive: YSF (DN and VW), NXDN48, dPMR, pager (POCSAG), ACARS, SSTV, airband AM, broadcast FM.
- YSF VW transmit (switched off until on-air tests).

### Listening
- Multi-watch 2-6 channels, each letter a channel or its own VFO; one-key multi-watch.
- Every mode in the VFO with all parameters (FM, DMR, P25, YSF, NXDN, dPMR, AM).
- Priority channel, temporary skip, heard list (tune the VFO from a heard call).

### Maps and position
- Offline Turkey map: 81 provinces, 973 districts, repeaters, APRS stations, track and coverage colours.
- 252 repeaters grouped by province, nearest first, auto repeater; airfields; marine VHF table (receive only).
- APRS messages and SOS; Go-To, find my friend (follow an APRS station), locator calculator, GPS dashboard.
- Satellites: passes, pass alarm, Doppler tracking.

### Tools
- Tone finder (CTCSS / DCS), DTMF / SelCall, CW decoder and practice, spectrum with waterfall, fox hunt, CW beacon,
  lone worker with an alarm action, one-key SOS (off by default), save search finds as channels, QSO marks, battery
  graph with time left, clock (stopwatch, countdown, alarm), MMDVM hotspot over the cable.

### PC programmer (Windows, no Python needed)
- Disclaimer to accept, port auto-detect, backup, plan, install (also from stock firmware with the radio's own codec),
  back to the previous version or stock from your own backup, signed updates, version list with downgrade warnings,
  self-update, live panel, channel viewer.
