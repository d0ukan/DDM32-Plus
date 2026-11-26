# Changelog

DDM32 Plus is a fork of Open25-DM32 Classic (r13) for the Baofeng DM-32UV. See the disclaimer in
[README.md](README.md) before installing anything.

## Unreleased

Firmware (to be 1.1.0 with programmer 3.2.0, owner decision 2026-11-26). Host-tested only; not yet run on the radio.

- **The map pack.** The Turkey map (provinces, districts), the repeater list and the airfields left program flash for
  a new data pack on the data chip, the map pack (data pack type 3, `ddm32-ref-<version>.dpk`, a separate signed
  release asset beside the firmware zip; docs/plus.md "The map pack and layout 2", docs/mappack-design.md). Without it
  the map says "No map data. Load the map pack (PC programmer).", Repeaters and Airfields say "load the map pack", and
  Settings > About has a "Map pack:" line ("none", "damaged, not used", "for another version"). Programmer 3.2 writes
  it (13 sectors, ~20-30 s estimated); DOTA can send it to another radio ("Map pack", FM ~9 min, DMR ~7 min).
- **Layout 2 of the data pack region**: the RadioID slots are 192 KB each (same starts as 1.0.x); the map pack takes
  the former tails. A 1.0.x RadioID pack of up to 192 KB (the owner's 98 KB one) stays in force: no reload. A larger
  one reads "too big for this firmware, rebuild it" and is not used. The firmware never writes the region by itself.
  Use programmer 3.2 or later: an older programmer writing a RadioID pack over 192 KB can damage a map pack slot.
- **Repeater refusals unchanged without a pack**: cross-band and DOTA refuse every listed repeater's output and input
  from a frequency set kept in program flash (frequencies only), with or without the map pack; a user's own map pack
  can only add refusals. Owner decision 2026-11-26: RadioID.net and the owner's TRAC list both ship, in the map pack's
  repeater list and in that set, as in 1.0.x (TRAC states no licence; the owner takes the responsibility).
- The map is drawn again from the chip only when something on it changes, not on every RSSI or caller change.
- Program flash: about 40 KB freed (measured, see docs/plus.md); static RAM about +0.5 KB.

## v1.0.6 (2026-11-26, with programmer 3.1.0)

Firmware. Host-tested only; not yet run on the radio. The programmer 3.1.0 is unchanged.

- Search found lists (Tools > P25 search, DMR search, All digital search) keep 16 finds instead of 8, newest first;
  the 17th new find drops the oldest. The list now scrolls: Up / Down keep the picked find whole on screen (both its
  lines) and new finds arriving on top no longer push it off the screen; with nothing picked the list shows from the
  newest, as before. A small "^" / "v" at the right of the Pass line says finds are above / below the part shown.
  Owner report 2026-11-25: the list stopped at 8 and Down moved the highlight off the screen.
- The PC search log is unchanged: the debug link sections still carry the newest 8 finds (a reply has no room for 16
  all digital lines); an entry that hears a call moves to the top, so the log misses nothing it caught before.
- The revisits are unchanged (one find in the range read again every 16 sweep steps, round robin): with more than 8
  finds in the range, each of them is read again less often (16 finds: every 256 steps instead of every 128).
- Cost: 504 B RAM (the two found lists 352 B, the screens' shared copy 144 B, the scroll state 2 B) and 652 B program
  flash. `p25s_start` no longer copies the found list on the radio task's stack.
- Tools > Cross-band gains **FM -> DMR** and **FM CTCSS / DCS sub-tones** (docs/plus.md "Cross-band store and forward",
  "FM -> DMR and FM sub-tones"). FM is now a receive source paired with DMR: the FM receive audio is encoded by the
  stock AMBE+2 codec and sent on the DMR side to that side's TX talkgroup. FM carries no talkgroup, so the FM side is
  heard only when the DMR side has a TX talkgroup set; with "As heard" a DMR / FM pair stays one-way DMR -> FM as
  before (no dwell to the FM side); Start still starts such a session, with the toast "FM->DMR off: set a DMR TX TG"
  in place of "Cross-band on: Back stops". An FM side of the form has an **RX Tone** and a **TX Tone** row:
  the RX tone gates an FM -> DMR receive side (only a carrier with it is relayed; none: carrier squelch), and the TX
  tone goes on a DMR / NXDN -> FM carrier so tone-gated radios and repeaters open. The sub-tones reuse the radio's own
  path (`ch->rx_tone` / `ch->tx_tone`, `board.radio->tx()`), so no new transmit code and no new TX or band limit; the
  FM receive side only receives. **FM -> DMR is HARDWARE-UNVERIFIED** - the stock encoder and the FM receive audio
  level are radio-only and have never run on the owner's unit (the engine relay and the capture / tone plumbing are
  host-tested with a fake codec). With a DCS TX tone the microphone stays muted through the turn-off tail (the tone
  path's route is held until the carrier is off); FM caller ID, the pager and APRS receive keep off the FM receive ring
  while a session runs; battery saving never sleeps an FM receive side. Known limit: squelch 0 with no RX tone keeps an
  FM receive side recording noise (docs/plus.md). Cost: FLASH +1,204 B, then +312 B for the review fixes and the toast (5,052 B free), RAM +352 B; zero warnings, forced rebuild
  identical. Radio test rows XB-16..XB-18 in docs/plus-radio-test.md.

## v1.0.5 (2026-11-25, with programmer 3.1.0)

Firmware. Host-tested only; not yet run on the radio. Only removals, to free program flash; nothing new. The programmer
3.1.0 is unchanged.

- The Tools > Battery graph and time left are left out of the Plus build to free program flash (battgraph.c and
  ui_battgraph.c: 1,154 B + 1,296 B flash, 112 B RAM in 1.0.4). The owner never used it. Tools > Battery stays and opens
  upstream's battery page; the status bar icon and percentage are untouched; no tool numbers move, so stored hold-key
  codes are unaffected. The source and its guarded hooks stay (`CONFIG_BATT_GRAPH`, now `n` in
  `tools/build_radiofoundry.py`), so it can come back.
- Tools > Clock (stopwatch, countdown, alarm, UTC offset) is left out of the Plus build to free program flash (the
  image was 1,964 B under the limit). The owner never used it. The source and its guarded hooks stay
  (`CONFIG_CLOCK_TOOLS`, now `n` in `tools/build_radiofoundry.py`), so it can come back. The Tools list loses the row; the other
  tools keep their numbers, so stored hold-key codes still mean the same tools. A hold key that was set to Clock on 1.0.4
  now does nothing (the "is empty: Settings > Keys" toast), its Settings > Keys row reads "None", and the picker steps over the Clock code (one "None", not two); the other hold keys of that radio are untouched.

## v1.0.4 (2026-11-25, with programmer 3.1.0)

Firmware. Host-tested only; not yet run on the radio. Published on the owner's word before the radio test. The release
carries the Rust programmer 3.1.0 only (the C# programmer 2.0.0 is retired; it can still install this firmware).

- Channel scan, the owner's 1.0.3 radio run: a P25 zone "starts slow, then speeds up" (docs/scan-slow-2026-11-25.md
  section 6). 1.0.3 needed two no-frame takes (about 1.9 s each) per P25 channel before its false strength was masked,
  forgot it after two minutes and at every scan start. Now one take is enough when the strength is still there, as
  high, right after the P25 receiver gives the channel back and the noise reading says quiet (a call's tail has gone by
  then; otherwise two takes as before); a steady reading (within 6 dB of the level) keeps the level alive; the levels
  are kept when the same zone is scanned again (not after a codeplug change). A reading more than 10 dB under a learned
  level drops it (a level learned on a foreign carrier or a call's tail must not outlive it; the release hole hunt
  moved this from 6 to 10 dB: the level is a stop's strongest reading, and with 2-3 dB of reading jitter the 6 dB rule
  dropped it at most visits, slower than 1.0.3 in the model). A signal 10 dB stronger, the noise reading, a
  reading with no strength and a take with frames work as before. Model, 5 P25 channels: slow start 19 s -> 9 s, first
  10 s 2000 -> 455 ms a stop (fast), no re-learning every 2 minutes (20 slow stops in 4 minutes -> 0), a restart has no
  slow stops; real P25 overs noticed 13 -> 16 of 18; calls after a learned foreign carrier 5 -> 16 of 16; DMR and FM
  unchanged. FLASH +224 B, RAM +8.
- Hold keys on Home: every digit 0-9 held on a channel (in the VFO a held digit still types it) opens a tool you pick in
  Settings > Keys, rows "0 hold" .. "9 hold" (None or any tool of the Tools list). Default as before: 4 = ISS tracking,
  the others None (the old toast, now "Speed dial N is empty: Settings > Keys" instead of "set it in qdmr"). A held 0
  during a scan stays the fast / classic switch. The list offers every Tools name, also SOS key (never a Tools row) and
  Battery on a radio without a voltmeter: those give the toast. A downgrade to 1.0.3 or older keeps every other setting
  and forgets the hold keys (back to 4 = ISS on the next 1.0.4). Home's hint row names the first digit set ("4 ISS", "7
  Satellites", a "+" when more are set) and still never cuts the keys. Stored in one appended settings field
  (`hold_tool[12]`, the record 392 -> 404 B, no field moves; an older record reads the defaults). Cost: FLASH +1,376 B,
  RAM +336 B. docs/plus.md "Hold keys: any digit, any tool"; radio rows IK-3 .. IK-8.
- P25 call NAC (owner's radio result on 1.0.3: Home showed the received NAC only as "$193"): now "$hex/dec" like
  everywhere else ("$1C8/456"), in every form, within the 220 px safe area (English and Turkish, normal and Large
  text, worst case TG 65535, ID 16777214, NAC $FFF). When the line is too wide it gives up, in this order: the "P25 "
  lead and spaces, the "NAC" word, BER, the talk group (last form "ID16777214 $FFF/4095"); the NAC "$hex/dec" is kept.
  An emergency: "EMERGENCY TG ID $x/d" | "EMG TG ID $x/d" | "EMG ID $x/d". In practice, on a channel with NAC Any
  Home's caller line usually shows **no BER** with a NAC (a 5-digit ID: "TG101 ID77285 $293/659 BER1.2%" is 30
  characters, the row holds about 24; the "$hex/dec" is kept, BER goes first); a channel with its own NAC is unchanged.
- Dual and multi watch panes: the heard NAC ("$x/d") and the caller (TG, ID) are on a pane's second line while a P25
  call comes in; a new narrow form "ID16777214 $FFF/4095" keeps the NAC in a pane when the wide form does not fit.
- Cost of the whole candidate (`--plus --public`, against 1.0.3 `9763158`): FLASH 897,336 -> 899,156 B of 901,120
  (+1,820; 1,964 B left), static RAM 205,216 -> 205,560 B (+344). Stack free: start 348 -> 312 B, host 576 -> 564 B
  (the settings record is 12 B bigger), ui 184 and radio 316 B unchanged.

## v1.0.3 (test build, published inside v1.0.4)

Firmware. Host-tested; on the owner's radio the ISS hold key, the search screen fix and Sun & Moon were seen working.

- Channel scan, from the owner's `scanst` readings on 1.0.2 (docs/scan-slow-2026-11-25.md section 5):
  - A zone of P25 channels took 1.9 s a channel with nothing on the air: the radio reads -62 / -72 dBm there (no board
    correction for the strength on the DM-32), every P25 stop counted it as a carrier and the P25 receiver took it.
    Now a P25 channel whose take heard no frame twice at about the same strength learns it; for two minutes a strength
    under it + 10 dB is no carrier there (the noise reading and a stronger signal still are). A reading with no carrier
    by strength or a take that hears frames forgets it, so a real P25 carrier is not hidden. Model (60 s, learning
    included): 1,900 ms a stop -> 73 ms fast, 204 ms classic; a real P25 signal in overs noticed 13 of 18 (before 10).
  - A DMR call was never caught in a zone that mixes DMR with FM / P25: a step into or out of DMR takes about 168 ms on
    the radio, and the stop's dwell counted from the step's start, so it had no listening time left. A step of 20 ms
    or more now starts the dwell when the retune is done. Model with a 168 ms switch: fast 0 -> 18 of 18 overs,
    classic 18 of 18 (on the radio classic missed too: unproven there until row SS-6).
  - `scanst.cost` gains P25_LEARNED and P25_MASKED; a new `scanst.step` line names the longest step and the modes it
    switched between (what the 168 ms is spent on is not known yet).
- Channel scan "extremely slow", the first theory (the radio's readings did not show it; kept as a guard): not a
  regression from 1.0.1 in the code. On the host model
  the scan holds every quiet channel for the 2 s resume delay when the receiver's false carrier right after a retune
  lasts 25 ms or more (unmeasured on the radio); longer after a VHF / UHF jump, a zone that mixes bands gets much slower
  than All channels. Zone and VFO scans, fast and classic, now cope with up to 50 ms of it: the squelch waits up to
  50 ms after a retune, the fast rule tolerates early readings, and a hold on such an early squelch that heard no
  carrier moves on without the resume delay. Model: 152 ms a stop classic, 53-94 ms fast with a 25-45 ms dip (before
  600-2000 ms); unchanged with a short dip. `scanst.cost` gains DIPS and DIP_HOLDS. See docs/scan-slow-2026-11-25.md.
  A side effect to know: while a P25 channel's false strength is learned (two minutes at a time), a real P25 signal
  less than 10 dB over that reading is noticed only by the noise reading, not by its strength (rows SS-4 / SS-5).
- Tools > Cross-band, a mode per side (docs/plus.md "A mode per side"; docs/xband-multiproto-study.md): DMR -> DMR,
  NXDN -> DMR, DMR / NXDN -> FM (decoded by the stock codec) and P25 -> P25 (the IMBE frames as heard, the P25
  transmitter's encoder skipped). Every other pair is refused; a one-way pair listens on its source side only. Every
  earlier refusal, the question, the lock and the CW ID stay; a user's PTT during a session, and until a P25 session's
  channel is let go at its end, gets the deny tone.
- Cross-band, a side's filters and TX values (docs/plus.md "A side's filters and TX values"): DMR RX CC / TS / TG and
  TX CC / TS / TG; P25 RX NAC, TX NAC, TX TG. RAM only. **P25 sends with the side's TX NAC, 293 unless set or brought
  by a picked P25 channel; it no longer sends the NAC it heard.** A NAC is typed with 0-9 only (Left / Right step to
  A-F, one value a press).
- Cross-band: # on the form lifts the satellite, repeater and APRS segment refusals (docs/plus.md "Segments free"; RAM
  only, off at every power-on, never while a session runs). Outside the amateur bands and PMR446 stay refused; DOTA's
  band plan is unchanged.
- Cross-band: a typed frequency is MHz from its first three digits (433, 4335, 433500); fewer than three digits are told
  ("Type the MHz: 433500") instead of being dropped silently (the likely cause of the 1.0.2 "Satellite segment" report).
- P25 RX group list (docs/plus.md "P25 RX group list", docs/rx-group-list-budget.md): a P25 channel picks one of the
  codeplug's RX group lists in the channel editor ("RX list"); with a list the channel's talkgroup and the list's P25
  talkgroups play, Any every group, None the channel's squelch as before. Stored in the record's existing byte by the
  existing channel save; the lists and contacts are only read (edited in the CPS or a DOTA pack).
- P25 search: an optional soft P25 probe on the FM discriminator (docs/p25-soft-probe-design.md), chosen with held 0
  on the P25 search screen (fast -> fast + soft probe -> classic). **Off by default**; off it changes nothing.
  Unproven on the radio (rows SP-1..SP-4).
- P25 on a channel with NAC Any: Home's caller line keeps the NAC the call came on (decoded from the NID) in its short
  forms, " $1C8" after TG and ID ("TG101 ID77285 $1C8 BER1.2%"); the last form is "TG65535 ID16777215 $1C8" (no BER) so
  the worst case still fits the 220 px row. A channel with its own NAC is unchanged (it already says it). An
  emergency call on such a channel says it too: "EMERGENCY TG101 ID77285 $1C8" | "EMG TG101 ID77285 $1C8" | "EMG
  ID16777214 $1C8" (the form that fits the row's font is taken; no BER). Checked in the simulator with the real fonts.
- Home on a channel: hold 4 starts ISS tracking when a pass is under way or due within 10 minutes, else opens Tools >
  ISS; the hint row shows "4 ISS" (docs/plus.md "ISS key on Home"). In the VFO a held digit still types it (no key, no
  hint there); on a dual / multi-watch VFO letter no hint either.
- Screens: Sun & Moon labels no longer overlap the time columns (Twil. / Alaca for the dawn and dusk row); P25, DMR and
  All digital search lines fit the 220 px safe area (shorter forms, never a clipped edge; Turkish probe word "Dene");
  Tools menu icons of their own for DOTA Transfer, Cross-band, CW decoder and SOS key.
- Size: FLASH 897,336 B of 901,120 B (3,784 B free, 99.58 %), RAM 205,216 B (6,240 B free); stacks fit (radio task
  316 B free, p25-lab 324 B).

## v1.0.2 (2026-11-25)

Firmware. Host-tested only (simulator flows in English and Turkish, normal and Large text); not yet run on the radio.

- VFO step key: on Home with the VFO selected, a short SK2 (the A/B key) takes the tuning step to the next value of
  Settings > Talk > VFO step (Auto, 2.5, 5, 6.25, 8.33, 10, 12.5, 20, 25, 50, 100 kHz, then Auto again; 8.33 only on the air band and AM, where it belongs: it snaps the frequency to its grid) and shows it as a
  toast ("Step 12.5 kHz"); Home's hint row names the key ("SK2 step", Turkish "SK2 adım"). It is the menu row's own setting
  (same storage, nothing new to save). Nothing else changes: on a channel short SK2 still starts dual watch, in dual and
  multi-watch it still selects the other letter, held SK2 is untouched, and while a scan runs or a frequency is typed it
  is not the step key. See docs/plus.md "VFO step key".

Firmware (host-tested only; not yet run on a radio).

- More free RAM for the coming software P25 receive probe: 4,792 B more static RAM free (1,968 -> 6,760 B; the
  1,500 B floor kept), no feature removed and no task stack touched (branch `ramsave`, `tools/plus_ramsave.py`; two of
  the three savings of RadioFoundry 454a5b76, MIT; the third, the start-up stack in the RTOS heap, was already in):
  - The screen's frame buffer holds only the area inside the bezel (220 x 310 of the 240 x 320 panel): 34,100 B instead
    of 38,400. Nothing was ever drawn in the 10 / 5 px edges; the radio now paints them in the background color
    itself. The screen shows the same pixels (every simulator screen compared both ways, frame by frame).
  - Settings and storage each use one 256-byte slot buffer instead of two (they were never used at the same time):
    -512 B.
  - The debug link's packed (LZ4) screenshot now answers "unavailable", and the debug tool reads the RGB565 form
    instead, which gives the same whole-screen picture (slower over the cable). Our PC programs use neither.

- Zone scan slow in a zone you made, classic and fast (owner's report 2026-11-24, v1.0.1; host-tested only, rows
  ZS-1..ZS-5; docs/plus.md "A zone scans slowly"). **The cause is not proven** until rows ZS-1 / ZS-2 are run on the
  radio; what follows is a mechanism found in the code and removed. The scan has no zone-specific path: a stored zone, "All channels"
  included, is read once when chosen. What cost time is reading channels on the radio task: a stop read three (the
  scan's start channel at every stop, needed only for the priority revisits, and the channel picked twice), and with
  any P25 channel in the codeplug each read walks the codeplug settings area on the data chip (several walks for a P25
  channel). The stop's timing started before those reads, so after a slow step the first poll met the receiver's
  noise-reading dip right after the retune: the squelch opened and the scan held a quiet channel for its resume delay
  (classic), or the fast rule's first readings fell in the dip and handed the stop to classic (fast). Host model at
  3 ms a read: classic 290 ms a stop instead of 152. Fixed: a stop reads one channel; the squelch guard and the fast
  rule's first reading count from the end of the retune (a quick step reads as before); the same model now gives
  152 ms classic and 53 ms fast at 0 and 3 ms a read, in a made zone and in All channels alike. Which zone it hit
  depends on its channels and on the channel the scan starts from: row ZS-1 (1.0.1's counters for both zones) is the
  proof on the radio. New `scanst` lines say where a scan's time goes: `slow` (the longest stop, its channel and why),
  `fb` (the reading that handed the last stop to classic), `cost` (the radio task's longest tick gap and step,
  stops in another zone, FM / DMR switches, band jumps VHF / UHF / air band, AM stops), with the fall-back's reason
  (noise, am, open, strength, dmr) and the slow stop's cause class (held, heard, the reason, dmrcfg, step, quiet);
  `pc/radiotools/plus_live.py --scan-stats` reads them (read only). The new 20 ms squelch guard also holds back a
  first poll that comes early after a step a request made (the scan's start, a nuisance skip); it is off with a scan
  dwell under 75 ms, so short dwells keep v1.0.1's squelch. No new limits. FLASH +1,500 B (884,576 B, 98.16 %), RAM +64 B (1,880 B free), stacks fit (--stack-check passed).

## v1.1.0 (candidate, not released yet)

Programmer (Rust, `rust/`; the version numbers are not bumped yet). The Rust program becomes the only programmer for
Windows, Linux and macOS; host-tested only (fakes, no serial port, no network), not yet run against a radio.

- What the radio runs: after every verified install (new backup, last backup / quick update, without a backup) and
  restore (earlier backup, factory firmware) the program records it in `settings.json` (the C# program's
  `installed_sha256` / `installed_firmware`, plus `installed_record` with its time and backup); an install that stopped
  after its writes started records "unknown". The record is used unless a verified backup is newer than it. So after
  installing 1.0.1 the window no longer says 1.0.0 (the backup taken before the install) and does not offer 1.0.1
  again.
- One version line in the title bar and on Home, with its source: "Program 3.0.0 · Radio firmware 1.0.1 (from your
  last install)" / "(from your last backup; may be older)" ("Telsiz firmware" in Turkish; "not read yet" / "unknown"
  when the program does not know); About lists both separately.
- Start-up update question: after the start-up check one dialog asks when something is newer, "Program 3.0.0 → 3.1.0"
  and / or "Radio firmware 1.0.1 → 1.0.2", each with Update now and Skip this version, and Later (the default button).
  Update now runs the existing signed self-update on Windows (the release page opens on Linux / macOS); for the
  firmware it only opens the update wizard with that version selected, never a radio step by itself. The firmware item
  comes only from the install record or a backup newer than it. Skip is stored per item in `settings.json`
  (`skip_program_version`, `skip_firmware_version`); Later asks again at the next start. The question waits while
  anything is under way (a radio job, a download, the live view or the search log, the wizard past its first step, a
  ready restore, another dialog). Re-checked every 24 h while the window stays open.
- The program update refuses to start, and to restart after its download, while anything of that is under way.
- A failed update check is no longer swallowed: `logs/update-check.log` and a dismissable line on Home. The Check
  button no longer stays at "Checking…" after a failure.
- Home's firmware line compares the newest version with what the radio runs: "up to date" instead of "a new version is
  ready" when they are equal; hedged when it comes from a backup.
- The program update gets the firmware list's beta fallback: on the default channel, while no stable program build
  exists, the betas are offered.
- Cable picker in the menu bar ("Cable: COM14 (auto) [Change]"): Auto (CH340, as before), each CH340 cable, and under
  Advanced every other serial port the OS lists (virtual pairs marked, with a "not a CH340 cable" warning). Kept in
  `settings.json` (`port`, `port_auto`); a chosen port that is gone falls back to Auto and says so in the header and on
  Home until dismissed. Locked while the port is held and from the wizard's backup choice to its install. Ports are
  only listed (on a worker thread), never opened to probe. The port in use is shown on Health, Live, Search log, Data
  packs, Backups and the wizard's radio steps. Hidden in a files-only build.
- Dialogs are modal for the keyboard too: the controls under them take no focus and read as disabled.
- Disclaimer (was missing in the Rust window): the C# program's text in Turkish and English, after the language at
  the first start, again when the text changes, read only from Help > Disclaimer. Every radio step (update wizard,
  restore, data pack write, health, live view, search log) stays off until it is accepted. Saved as the C# program
  saves it (`settings.json` `disclaimer`, same version hash), so a C# user who accepted is not asked again.
- The window's close button and Alt+F4 are guarded like the C# window: refused while a backup, install, restore, pack
  write, health read or download runs; the live view and the search log are stopped first, then the window closes.
- A test / alpha version asks Yes / No (No by default) before it is downloaded; it used to warn and download anyway.
- Beta visibility carries over from the C# program: its `include_beta` (default on) sets the channel when this
  program has none of its own; choosing a channel writes both keys. Fresh installs stay on Latest.
- Hand-over from C# 2.0.x (see `rust/docs/handover.md`): the Windows program needs no Visual C++ Redistributable any
  more (static C runtime), and the self-update start marker is written once the window's event loop runs, so a window
  that cannot come up is rolled back to the old program. The update helper no longer flashes console windows.
- Tests read their fixtures from `rust/tests/fixtures` (copied from `win/`); `NET_VERSION` is the C# program's 2.0.0.
- The release channel is decided once and saved at the first start: a fresh install no longer turns to Beta after its
  first install (the install record writes `installed_*`, which had counted as a C# key). A C# restore after a Rust
  install now supersedes the Rust install record ("Factory firmware" / unknown instead of the older install).
- A panic in a worker thread can no longer leave the window busy: every worker reports back and the step shows an
  error line. Pressing the close button twice within 5 s while a step that does not write to the radio runs asks
  "Close anyway?" (No by default); never during an install, restore or pack write.
- `DDM32Plus.exe` carries the program icon; the Windows setup installs `ddm32.exe` too and needs nothing from `win/`.
- Backups are ordered by the time in their manifest, not the folder's (copying a backup no longer makes it "newest").
- A radio step's start no longer lists the serial ports on the window's thread (its gates are checked again when the
  listing returns); a C# `port` setting is taken over.
- The wizard's firmware download reports back like every other worker; "Close anyway?" closes when a radio write
  starts and a refused "Yes" says why; start-up never rewrites an unreadable settings.json, and `--demo` saves no
  channel.
- The window gets what C# 2.0.0 users had (host-tested only, no radio run yet; `rust/README.md` "The C# 2.0.0
  pages"):
  - Versions page: every signed version with its label and state (on the radio, newer / older, downloaded, old names,
    test), notes in both languages, Check for updates with the last check's time and error, the releases that were
    not listed and why, the signing key, the program update; "Switch to this version" opens the wizard at it.
  - Backups and going back: "Back to the previous version", "Back to stock firmware" (the C# `restore_previous_backup`
    / `stock_backups`, now also written by this program), "Choose another backup folder...", and a Yes / No question
    before every restore (it used to start at once).
  - Data packs: read the radio's packs (read only) or a backup's, RadioID download (after a Yes), countries (no longer
    fixed to Turkey) and pack names (the C# `pack_*` keys), several AD 2 PDFs with Clear, a saved .dpk, separate
    writes for the RadioID and the air band pack, each asking first.
  - My channels: zone and mode filters, all columns, sort by any column, zone double click, another backup, CSV of
    the rows shown.
  - Health: Check the cable and Read every 30 s (off by default).
  - The window keeps its size and place (`settings.json` `window`, the C# format); offline Help; menu entries for
    Versions, Help and Language; a developer build's title mark, local release folder and `--release-folder`;
    `--show PAGE[:demo|:FOLDER]`.
  - The search log keeps a Linux / macOS PC awake too (`systemd-inhibit` / `caffeinate`, when present).
  - Verifier round: tests never read the user's real backups (an injectable environment); fictional demo data; only
    the wizard's own stock backups are recorded; a damaged settings.json is never written for a C# key and the channel
    cannot flip to Beta; the window is restored only with its title bar on a monitor (one DPI basis); a late Yes or a
    tool start never runs beside another radio job; the versions page keeps its selected version after a check; the
    restore question names the backup once; "the folder you opened" on My channels; a RadioID download holds the close
    button. Second round: the wizard's backup and install refuse before anything moves while another radio job holds
    the port; an empty, whitespace-only or BOM-prefixed settings.json is read as the C# program reads it (only real
    garbage refuses a C# key, and the refusal is logged in `logs/settings.log`).

## v1.0.1 (2026-11-24)

- Home stopped at channel 26 of 33 (owner's report 2026-11-23, v1.0.0). Cause, besides channels made before beta.7 that
  were never repaired: two channel-making paths still wrote their channels into their own zone only, never into "All
  channels" (zone 0, a stored copy once the radio has a zone). Find & save (`#` in a search, hold OK on a find) put
  its channels in "Bulunanlar N" only, and a DOTA import (a new zone, the end of a zone, DOTA Clone's zones) into that
  zone only. So Home on All channels, Up / Down and the knob, did not reach them while Channels listed them. Fixed:
  Find & save now saves each find with the same single write as New from current (`ZONE_OP_ADD_JOIN`: "Bulunanlar N"
  at its end and All channels), and a DOTA import adds its new channels to All channels after the commit
  (`zone_edit_join_all`, one write; those already in it are skipped; All channels full: the first that did not fit is
  said, "All channels full, not in it: ch N"; the import itself is never undone by it). All channels renamed: nothing
  joins it, as before. Channels already stranded by v1.0.0 are repaired once by hand: Zones > All channels >
  Zone options > "Add unzoned channels". Host-tested only (tests/plus/test_zone_cap.c `test_find_and_import_join`,
  tests/plus/test_dota_import.c with `CONFIG_CHAN_EXTRAS`). FLASH +220 B (880,600 B, 97.72 %), RAM +0 B (2,072 B free).

- Faster channel scan (owner's request 2026-11-23; host-tested only, rows SF-1..SF-8; docs/plus.md "Faster channel
  scan"): the zone scan and the VFO scan leave a quiet channel as soon as the receiver's readings say it is quiet
  instead of listening the whole 150 ms scan dwell. The radio task's 10 ms tick reads the receiver from 30 ms after the
  retune; three quiet readings (30-50 ms) end an FM, P25 / YSF, NXDN or dPMR stop, a whole 60 ms TDMA frame of them
  (30-90 ms) a DMR stop. Anything not clearly quiet (a margin of about 2 dB over the scan's own carrier rule) keeps
  v1.0.0's dwell, probe and hold. Host model: a quiet 16-channel FM zone goes round in about 0.9 s instead of 2.4 s
  (2.8x), a mixed one (every 4th DMR, every 8th P25) 2.3x faster; every over v1.0.0 catches is caught too, mostly
  sooner, including a DMR radio in the same room with or without the TDMA gap leak, P25 with a noise reading deaf to it
  and FM 1-3 dB over the squelch line, with a reading that lags up to 20 ms. The limit (unmeasured): if the receiver's
  noise reading lags a new carrier by about 30 ms, an FM signal only 1 dB over the squelch line can be passed over;
  held 0 on Home while scanning switches to classic ("Scan: classic (v1.0.0)", back with "Scan: fast"), kept while the
  radio is on; radio row SF-4 checks it. The monitor key on a DMR channel still doesn't pause the scan (upstream's rule).
  New debug link section `scanst` (counters for both: rounds, quiet dwell by mode, stops noticed and held, fast stops
  and fall-backs). Dual watch and the multi-watch ring are unchanged. FLASH +1,832 B (882,432 B, 97.93 %), RAM +104 B
  (1,968 B free), stacks fit (--stack-check passed).

## v1.0.0 (2026-11-23, first stable release)

- Tools > ISS: a row "Show orbit" (Turkish "Yörüngeyi göster") under "Auto track" opens the sky plot with OK, exactly
  as key `2` does (key 2 stays as a shortcut). Owner's report on the radio (beta.9): the "2 sky" hint could not be
  seen. Cause: the hints alternated every 3 s by the satellite service's prediction clock, which does not run without
  GPS time (indoors), and the "2 sky" label on the "145.825 APRS" line was only drawn when there was GPS time, so the
  key was undiscoverable indoors. The alternating hints are gone on both pages. The row works without GPS time too (the
  plot then says "Waiting for the GPS time"), and under a held auto track OK on it opens the plot without ending the
  track (Back returns to the page). Tools > Satellites has no room for a row, so its fixed bottom hint now names key 2
  in every state ("OK track  2 sky  <> UTC", "OK alarm  2 sky  <> UTC", ...; "^v pick" was dropped to make room, with
  the ISS in focus "OK track  # APRS  2 sky"). Also: the Satellites page's "Needs a GPS fix or a typed position" line,
  cut at the screen edge without GPS time, now wraps. Host-tested only (rows SK-8..SK-11; render images/ddm32/iss-show-orbit.png); FLASH +80 B (880,380 B, 97.70 %), RAM +0 B (2,072 B free), stacks unchanged (--stack-check passed).

## v1.0.0-beta.9 (2026-11-23, beta: the new parts not yet confirmed on the radio)

- Sky plot (owner's request 2026-11-22; host-tested, not yet checked on the radio, rows SK-1..SK-6): key `2` on Tools >
  ISS and on Tools > Satellites (the satellite in focus) opens a polar plot of its next pass (or the one under way)
  from your location: north up, the centre is the zenith, the edge the horizon, rings at 30 and 60 degrees, N / E / S /
  W marks (Turkish K / D / G / B). The track is a line from the rise (filled square) to the set (open square) worked
  out from the same prediction as the pass list (no new orbit model: the points are `sat_eval` at about 20 s steps,
  64 at most, kept as 128 B so a redraw costs no SGP4); the satellite now is a filled dot in the warning color that
  moves with the live look (once a second). Under the plot: AOS and LOS times, the top elevation and the time to the
  next event ("Max 47°  LOS in 7:00"), the live azimuth and elevation. Back returns to the page. Nothing here tunes or
  transmits: OK does nothing on the plot, and an auto track or the AOS pop-up carry on as before; under a held auto
  track the plot may be shown (on Tools > Satellites key `2` and the plot's Back are not taken as the "any key ends the
  track" key). Messages instead of a plot: "Waiting for the GPS time", "Waiting for a GPS position", "no pass soon",
  "Looking for the pass", "bad elements". The key is named on the pages: the hint rows alternate every 3 s between
  the usual hint and one with "2 sky" (Turkish "2 gök"), and "2 sky" sits at the right of the "145.825 APRS" line on
  Tools > ISS. The plot is map-style (north up, east right, like Gpredict). FLASH +3,672 B (876,652 B, 97.28 %), RAM +200 B (2,272 B free), stacks
  unchanged: the points are worked out by the radio task (the UI task has no stack for SGP4).
- Satellites and the sky plot now also use the position the APRS settings hold (a typed position, Settings > Location,
  as the beacons use it; with position ambiguity set the same blur applies, and the altitude is taken as 0) but only
  until the first GPS fix since power on: that fix replaces it for good and is kept when lost (never back to the typed
  one: away from home it would be the wrong place). The source shows in the titles ("Pos: GPS" / "Pos: typed"). One
  line turns the fallback off: `SAT_TYPED_POS` in `sat.h` (the owner has not confirmed it yet). The time still comes from the GPS alone: the radio has no clock of its own, so with no
  GPS time nothing can be predicted. The Satellites page's hint now reads "Needs a GPS fix or a typed position".
- Faster searches (owner's request 2026-11-23; host-modelled, not yet measured on the radio, rows FS-1..FS-8;
  docs/plus.md "Faster searches (beta.9 candidate)"). First the measurement: P25, DMR and All digital search now count
  what they do (passes and their time, probes and how each ended, time probing against sweeping, the time from a
  carrier first seen to its find, a found probe's wait for its first frame) and the PC programmers' Search log page
  shows it as a stats line (the CSV ends with a `stats` row). Held 0 on a search screen switches between the new
  "fast" algorithm (default) and beta.8's ("classic"), so the same signal can be compared both ways. Fast: the DMR and
  All digital sweeps read a quiet step in 50 ms instead of 75 (samples on the 10 ms tick, their peak); carriers left
  undecided (the over ended during the probe or before it) are read again between steps for 30 s instead of a whole
  pass later; a steady carrier found not digital again on a later pass is skipped 2 minutes instead of 30 s. The probe
  windows are beta.8's. Host model (four random sequences, with and without a strong transmitter's TDMA gaps reading
  high): the owner's room radio found in a median 1.3 s instead of 2.6, a 10 MHz DMR pass 40 s instead of 60, never
  fewer finds or calls than beta.8. FLASH +3,648 B
  (880,300 B, 97.69 %), RAM +200 B (2,072 B free).

## v1.0.0-beta.8 (2026-11-22, beta: not yet confirmed on the radio)

- Upstream RadioFoundry fixes, second round (docs/plus.md "Upstream RadioFoundry fixes"; host-tested, not yet
  checked on the radio, rows RF-7..RF-12):
  - DMR: a reply keyed right after the other station's over ends (within 0.36 s) is no longer refused as "busy" on a
    polite channel in direct mode (RadioFoundry f5d2fd1). Through a repeater nothing changes.
  - FM: on 12.5 kHz channels the squelch no longer chops on S/SH/T sounds of a strong station; it still closes at once
    when the station unkeys or fades (RadioFoundry 18fdd29).
  - P25, NXDN, dPMR and APRS receive: a lost modem interrupt request no longer leaves receive deaf until the channel
    is changed; it is cleared within about 0.1-0.25 s (after the idea of RadioFoundry 3d5940b; against "P25 stops
    receiving after 10-20 minutes", Open25 #40, not reproduced here).
  - Transmit: TX inhibit and a critical battery now stop every transmission at the keying itself, also the MMDVM
    hotspot (and its CW ID), an APRS beacon that waited for a clear channel, and DOTA (after RadioFoundry f4b19e9; no
    band rule added).
  - DMR text: received control characters (0x7F-0x9F) are dropped instead of being drawn as icons (RadioFoundry 22b27e8).
- Tools > Repeaters (owner's request): `*` on the picked repeater (in the list or in its details) adds it as a NEW channel in the zone Home is on (`#` keeps its job, the nearest repeater; `*` was free here: it only opened quick settings, which it still does on the province level). The channel carries what the list has: RX, TX = RX + the offset, FM with the access tone as its TX tone and 12.5 / 25 kHz, or DMR with its colour code and slot (both or not known: Any), the repeater's name, the radio's power. A DMR channel is receive only with RX list Any (the list has no talkgroup: pick a TX contact in the channel editor to transmit); a repeater whose TX is outside the radio's TX bands is added receive only. It goes through the channel editor's own write (All channels and the zone in one write, the beta.7 zone fix; on the VFO All channels only). Toast "Added: ch N NAME in zone ZONE" (TR "Eklendi: kanal N NAME bölge ZONE"). A repeater already in the zone (same RX, TX, mode, tone or colour code and slot) says "Already in ZONE: ch N" and adds nothing. Refused cleanly: C4FM / D-STAR / P25 / ... entries (no channel for them: "Info only: not a mode of this radio"), while transmitting, a write in progress ("Saving: wait"), a full zone (80), no free channel, a frequency outside the radio's bands. No band-plan check (the band plan is DOTA's only). Host-tested (`tests/plus/test_ui_rptadd.c`); radio rows RA-1 to RA-4.
- DMR search and the all digital search's DMR probe: the modem now hears every color code at once. Before, "any color
  code" made the DMR modem step through the codes one at a time, and once a strong signal near the frequency moved it
  off the right code it heard nothing at the probe, which is how a DMR radio in the same room was never found
  (docs/plus.md "DMR search: the color code scan"). Ported from upstream RadioFoundry (d1a7ae4, MIT); "Any" color
  code channels use the same path. Host-tested only; the owner's radio test decides.
- From upstream RadioFoundry (2093f64, MIT): a simplex hotspot that sends base station sync is no longer dropped as
  the other timeslot.
- Searches: a strong signal that raises several channels around it is probed on a real channel, never 6.25 kHz off,
  and a signal standing out of a raised band is probed instead of being taken as the noise floor.
- Dual / triple / quad ... watch: Home now shows the talk group of the call being heard on a watch letter (owner report on
  beta.7: it showed nothing, the single channel did). The text that fills in after a call starts was never refreshed on
  the letters in the two panes; DMR callers keep the group in their short form ("TG 91", private "Pvt 3101234");
  one-line rows (6 letters, Large text) draw it left of the RX pill; NXDN and dPMR letters name their caller instead of
  "FM signal". The group shown is the heard call's, not the channel's setting (RX group list Any, NAC Any). Docs:
  docs/plus.md "Multi-watch: the heard talk group"; radio test rows MW27-MW29.
  A new P25 receive starts without the last call's talk group (the old one showed on another letter before the first
  link control); "TG 0" / "Pvt 0" are never drawn; the new texts are translated.

## v1.0.0-beta.7 (not released on its own; included in beta.8)

- PC programmers (Windows C# and Rust): a classic menu bar (File, Radio, Tools, Help) on top of the window, reaching the existing pages, the backups and logs folders, the GitHub page and About; the radio entries are off while a radio job holds the port.
- Transmit safety (pre-release hole hunt; docs/plus.md "Pre-release hole hunt: transmit safety"): SSTV send is
  refused while a scan, a dual / multi-watch, the air watch, a search, the spectrum or trunk following runs ("Stop the
  scan or watch first"; before, a watch letter the page never checked could be keyed for up to 2 minutes). DOTA
  checks the amateur band plan in the radio core too, not only on its page: a session frequency outside it never keys.
  The multi-watch emergency takes the selected or first DMR letter that may transmit (an RX-only letter is passed
  over). All digital, DMR and P25 search are refused over a dual / multi-watch or the air watch. The radio name must
  look like a callsign for SSTV, the cross-band and CW IDs ("DM32UV", the factory name, is refused; TA2DOU/P works).
- Band plan only for DOTA (owner decision 2026-11-22): the VFO, a search find or Tools > Heard -> VFO, SSTV send and
  the CW beacon have no amateur band plan; they transmit wherever PTT may (the radio's own TX checks: TX inhibit, RX
  only, the radio's band, the mode's own limits). A P25 / YSF / NXDN VFO that listens to "Any" stays receive only, as
  before. Only DOTA refuses a frequency outside the amateur bands.
- Transmit safety, second round: the CW beacon is refused during a scan or watch. Special-event callsigns (YM100TC,
  TC18MRT, TC2023X) are accepted; an SFR repeater with its CW ID on and no callsign does not start.
- An emergency (the key, the lone worker, SOS) also ends P25 and DMR trunk following, the P25 search and the DMR /
  all digital search instead of waiting for them: within about 0.3 s the channel is tuned again and the emergency goes
  out there (before this fix it usually failed while a search ran and the search went on). One pressed while
  transmitting in a watch waits 10 s at most. Host-tested; radio row IS-16.
- Satellite tracks (ISS auto track and the held track from Tools > ISS / the pop-up): the emergency key, the lone
  worker and SOS now end the track at once and go on from the user's channel (a DMR channel transmits the emergency;
  before, the key only ended the track and SOS / the lone worker failed on the receive-only satellite frequency). A
  track whose end got lost can no longer leave the radio receive only and not saving until a power cycle: it ends by
  itself within 2 s. A Doppler step no longer aborts the "# APRS via ISS" frame while it waits for a clear channel.
  No track takes the radio while an emergency runs (and no AOS pop-up then); an Emergency or SOS key (also as SK1's
  hold function) always reaches the emergency during a track. A rise during the radio's own APRS frame no longer
  skips the auto track for the pass. The AOS pop-up stays 30 s instead of 60 s. Host-tested; radio rows IS-12 to IS-15.
- Transmit safety, third round: radio model names (IC7300, FT991A, DM1701 ...) are refused as callsigns, special-event calls
  keep working with a round 3-digit or a 4-digit number (YM100TC, TC2023X; TC100A and TC1453 are no longer taken; a few model names shaped like callsigns, e.g. UV5R or FT8900R, still pass);
  switching the SFR CW ID off lets a repeater role without a callsign start.
- Fixed: Home stopped at channel 26 of a 27-channel radio (owner's beta.7 report). The zone really held 26: when the
  radio stores its first zone (Create New Zone, Find & save), "All channels" is stored as a copy of the channels of
  that moment, and a channel made later (Channels > New from current, Save as new channel, the VFO's copy) was saved
  in no zone. Now a channel made on the radio (New from current, Save as new channel, the VFO's copy, Copy here) goes
  into "All channels" and into the zone it was made for (Copy here's zone, else the zone Home or the selected watch
  letter is on; not the VFO, not a Find & save zone) in one write. It is never left in no zone silently: a toast says
  so ("All channels full, in no zone: ch N"). A renamed "All channels" counts as an ordinary zone. New Zone options
  row "Add unzoned channels" (TR "Bölgesiz kanalları ekle") puts every channel in no zone into that zone and says how
  many: use it once for a channel made before this fix.
- Dual / multi-watch emergency: the key, the lone worker and SOS use the selected letter when it is DMR, else the
  first DMR letter of the ring (the watch ends there and says which; it isn't restored after); refused only with no
  DMR letter. An emergency never waits for a P25 call being received (on a single P25 channel it now fails at once,
  so SOS and the lone worker fall back), and one pressed while transmitting waits for PTT to end.
- Dual / multi-watch: the PTT pill follows the radio when it keeps another letter, without cutting a P25 call; a TX indicator
  left with nothing keyed is cleared in the ring.
- Upstream RadioFoundry fixes (MIT, The RadioFoundry Authors; docs/plus.md "Upstream RadioFoundry fixes"):
  DMR after an FM channel with an RX tone: the FD6818's sub-audio is off on entering DMR for every RF set-up
  (RadioFoundry 625b921; Classic's tables already covered the DM-32UV's two set-ups, set-up B now also gets its
  no-tone words back). HR_C7000 interrupt recovery (916ac26): every receiver restart drops pending modem requests, a
  system request stuck for 50 ms is dropped, the modem interrupts are unmasked again after a stalled transmission;
  this is the likely cause of "the radio shows TX for a split second, then idle, until a reboot" (Open25 #28). Polite
  admit no longer refuses every PTT once the slot interrupts stop (947c139): its busy window and the receiver's
  recovery run on wall time. The DMR glue's task-side code moved to flash: 1,352 B more free RAM.
- Tools > ISS (owner's request): the ISS on one page: the next pass (AOS / LOS, top elevation; "Above now" with
  azimuth / elevation), the downlinks 145.800 FM voice / SSTV and 145.825 APRS with the Doppler of the one in use, the
  notification and auto track settings; OK tracks now (within 10 min of AOS or during the pass: receive only, the
  radio back to the previous channel 30 s after LOS), # APRS via ISS, 1 switches the downlink (also during a track).
  The new tool comes after All digital search; only Battery's number moves. With notifications on and auto track Off
  the AOS banner on Home is now a pop-up for 30 s, "ISS overhead, max 47°, OK: track": OK starts that track (the
  radio says "ISS tracking" once it holds, or why it can't), Back hides it, none while transmitting. During such a
  track only the "# APRS via ISS" frame is sent; beacons wait for its end. Fixed: Tools > Satellites without a GPS fix showed only "Back" in the hint row
  although OK opens the pass alarm page ("OK alarm  Back").
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
- The watch is named by its letters: Dual, Triple, Quad, 5x, 6x watch (Çift, Üçlü, Dörtlü, Beşli, Altılı izleme) on
  the status bar, Quick settings, Tools, the # screen and its toast. In dual / multi-watch the 1750 Hz burst, the
  emergency hot mic, VOX and SSTV send act on the letter with PTT and never key P25 by themselves; a new letter can't
  be selected while transmitting; a P25 call of ours is no longer skipped for 8 s when its voice comes late.
- Satellites: ISS notifications and auto track. The pass alarm page gains "Every pass" (the ISS or any satellite:
  every qualifying pass announced with the beeps and banner before and at AOS, armed again by itself and kept across
  power cycles) and "Auto track" (Off / On / APRS): at AOS, when the radio is idle on Home or the Satellites screen, it
  follows the pass's downlink with Doppler (ISS 145.800 voice / SSTV or 145.825 APRS), receive only, shows "ISS
  tracking, max 47°", and 30 s after LOS (or on any key or PTT) returns exactly to the previous channel or VFO. No
  banner while transmitting (it comes after). Stored in spare settings bits; nothing new in RAM but 24 bytes.
- SSTV send (owner's request): Tools > SSTV > OK sends a built-in test picture: the DDM32 Plus logo, your callsign (the
  radio name) and colour bars, in Robot 36 (37 s, default), Martin M1 or PD120, on the FM simplex channel in use after
  an explicit OK, with every TX check PTT has. Back or PTT stops it at once. Receive it with a phone app such as Robot36 or another DM-32 with SSTV.
- P25, DMR and All digital search: faster stepping and a better probe of strong carriers (the owner's beta.7 report:
  a DMR radio in the same room on 436.275 was not found and the stepping was slow). Measured only on the host model;
  DMR search on a strong radio nearby is still under investigation and not yet confirmed on the radio. A strong carrier is now probed on its strongest step (or the
  middle of an overloaded plateau, or the half step of a 6.25 kHz channel) instead of one neighbour after the other;
  the noise floor is learned from the first step (a receiver reading high made every step a probe); the steps stay on
  the channel grid whatever start was typed; a direct mode DMR carrier gets 1.1 s, P25 1.2 s; a carrier that went off
  the air during its probe, or brought a burst, is never skipped; the All digital search probes a direct mode carrier
  as DMR first. The search screen shows the steps a second. Host model of the owner's case (not a radio
  measurement): found in a median 2.5 s instead of 21.5 s.
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
- Search log on the PC, exact counts and all night: the radio now gives each found entry a serial and counts its calls
  (`X.seq`), and the programmers count each call once by them. Before, a call the radio moved to its right talkgroup
  was counted again (5 then 4 showed 9), an entry whose talkgroup or colour code came later left a "-" / "CC?" line
  beside a second one, and P25 search and All digital search finds of one talkgroup were not added up. A radio restart
  loses nothing and counts nothing twice; calls the PC could not see are shown as "may be missed". One CSV per logging
  session (`logs\search-YYYYMMDD-HHMMSS.csv`, never overwritten after a program restart, a run past midnight stays in
  its file), with a BOM and Windows' list separator so Excel opens it in columns with the Turkish letters right; a
  callsign starting with "-" or "=" is not run as a formula; while Excel holds the file the log goes to a
  `...-open.csv` beside it. Windows does not sleep while it logs; a pulled cable, a PC sleep or a radio restart is
  waited for with the table kept ("waiting for the radio"); a radio without the search log says it needs
  v1.0.0-beta.7; a search that stopped on the radio shows when. Programmer 3.0.0: Health, Live and the radio steps
  wait while the search log has the port (as on Windows).
- Fixed: All digital search could store the callsign of an earlier YSF call on another frequency in a YSF find (and
  in the PC's CSV); only a callsign heard in the probe is the find's now.
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
