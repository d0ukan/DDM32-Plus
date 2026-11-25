<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Cross-band multi-protocol: feasibility study (research only, not for this release)

Status 2026-11-24, branch `xbstudy` from `ddm25` aec22d0. Read-only study: no code changed, nothing run on a radio.
Owner request: Tools > Cross-band with side A and side B each on its own frequency **and its own mode** (for example
A 144.500 MHz P25, B 433.500 MHz DMR), any mix of FM, DMR, P25, NXDN and YSF. Today it is store-and-forward DMR to DMR
only (docs/plus.md "Cross-band store and forward", ROADMAP item 46, `integration/radiofoundry/xband/`).

Sources read: `xband/xband.h`, `xband/radio_xband.inc`, `p25_lab.c` (`p25_lab_lend`, `rp_buf`, the YSF consumers, the
P25 transmit loop), docs/plus.md (RAM budget and map, cross-band, YSF, data packs, DMR audio path, SSTV TX),
docs/nxdn.md, docs/ysf-nxdn-tx.md, docs/p25-soft-probe-design.md, the lab's ROADMAP.md and PROGRESS.md (codec timings),
upstream RadioFoundry `platform/hrc7000/vocoder.c`, `core/dmr/ambe/ambeVoice.h`, `docs/spec/hardware-dm1801pro.md`.

## 1. Hard facts that decide everything

| Fact | Consequence | Source |
|---|---|---|
| One FD6818 transceiver, one synthesizer; the LNAs are off while it transmits | **Never simultaneous.** Every pair is store-and-forward: record a whole over on one side, then send it on the other. A real-time repeater is impossible in any mode | docs/plus.md cross-band, xband.h header |
| HR_C7000 does DMR framing in hardware (voice reaches the MCU as 72-bit AMBE+2 air frames, 9 B / 20 ms) | DMR RX and TX are the cheapest legs | `radio_xband.inc` voice sink / source |
| HR_C7000 "native" raw 4FSK stream, fixed 4800 sym/s, the CPU builds every bit | P25 phase 1 and YSF (both C4FM 4800 sym/s) ride on it, RX and TX. NXDN48 (2400 sym/s) cannot | ysf-nxdn-tx.md 1 |
| NXDN48 receive = software demodulator on the 8 kHz FM RX RAM copy of the discriminator; radio stays in FM mode | NXDN is **receive only**; NXDN TX is not feasible on the native modem, the software FM-TX route is unmeasured | nxdn.md, ysf-nxdn-tx.md table |
| Vocoders are software on the MCU: the stock AMBE+2 binary from the codec slot (owner's bytes, `vocoder.c`, `enc_state` 7,168 B + `dec_state` 6,144 B static) and our IMBE (`voice` context in the P25 worker) | Every transcode costs MCU time; the stock codec cannot run in host tests | plus.md RAM map |
| P25 phase 2 (TDMA, H-CPM / H-DQPSK, AMBE+2 half rate) | **Not possible**: no demodulator, the native stream is C4FM only | - |
| The soft P25 probe (`CONFIG_SEARCH_SOFTP25`) | A sync detector for search only, off by default, host-tested only. No voice role here | p25-soft-probe-design.md |

## 2. Codec and CPU numbers (per 20 ms frame)

| Operation | Time | Measured where |
|---|---|---|
| IMBE encode (ours, O3 speed codec) | 13.0-13.3 ms average, **18.6 ms worst** | on the radio, live P25 TX (PROGRESS.md; ysf-nxdn-tx.md 1) |
| IMBE decode (ours) | **15.0-16.2 ms worst** (average not recorded) | on the radio, P25 RX (PROGRESS.md 2026-11-06) |
| AMBE+2 encode (stock codec slot) | ~12 ms | upstream, on a DM-1801 Pro (same HR_C7000 family, same `vocoder.c` host); **not measured on the DM-32** |
| AMBE+2 decode (stock) | ~3 ms | same, not measured on the DM-32 |
| DMR / NXDN / YSF-DN frame re-FEC (`ambeFecEncode` / decode) | well under 1 ms | host code, bit ops |

So: **any decode + encode chain in real time is at or over the 20 ms budget in the worst case**:
AMBE decode + IMBE encode = 16-22 ms; IMBE decode + AMBE encode = up to ~28 ms; IMBE decode + IMBE encode = up to
~35 ms. Only a single codec step per frame fits live (as P25 TX and DMR TX already do). Because the relay is
store-and-forward anyway, a transcode can instead run **at whatever speed the CPU allows between the end of the
recording and the key-up** (an "offline pass"), at the price of extra delay (roughly 1x the over's length) and a busy
CPU (UI and the other tasks starve unless the pass runs at the worker's low priority, 3).

## 3. Memory per second of audio

| Representation | Bytes / 20 ms | Bytes / s | In 4,400 B (`rp_buf`) | In 10,188 B (`fifo` + `rp_buf`) | In ~16.5 KB (today's loan: `voice` + `fifo` + `rp_buf`) | In 32 KB (pack-region tail, see 4) |
|---|---:|---:|---:|---:|---:|---:|
| DMR / NXDN / YSF-DN air frame (72 bit) | 9 | 450 | 9.8 s | 22.6 s | 38 s (1,899 frames, as today) | 72 s |
| AMBE+2 parameters (49 bit, packed 7 B) | 7 | 350 | 12.6 s | 29 s | 48 s | 93 s |
| IMBE parameters (88 bit, 11 B, as the call replay stores) | 11 | 550 | 8.0 s | 18.5 s | 31 s | 59 s |
| P25 / YSF-VW channel codeword (144 bit) | 18 | 900 | 4.9 s | 11.3 s | 18.8 s | 36 s |
| PCM 8 kHz 16 bit | 320 | 16,000 | 0.27 s | 0.64 s | 1.0 s | 2.0 s |
| PCM 8 kHz u-law 8 bit | 160 | 8,000 | 0.55 s | 1.3 s | 2.1 s | 4.1 s |
| ADPCM 4 bit | 80 | 4,000 | 1.1 s | 2.5 s | 4.1 s | 8.2 s |

Static RAM free: **6,488 B** (aec22d0 `--plus`, the RAM rule keeps >= 1,500 B). Conclusions:

- **PCM storage is out** in every form. FM must be stored through a vocoder (codec parameters or air frames).
- The loan only works in full while the P25 worker is idle. **Any P25 or YSF leg needs `voice`** (IMBE context, LDU
  state), so it cannot be lent; a leg that decodes or encodes PCM through IMBE also needs `fifo` (the 18-block PCM
  FIFO). What is left:
  - P25 <-> P25 frames as heard (no PCM: RX only FEC-decodes to parameters, TX sends stored parameters without the
    encoder): `fifo` + `rp_buf` = 10,188 B = **18.5 s** an over.
  - Any leg that runs IMBE to or from PCM: only `rp_buf` = **8-10 s** an over. Too short to be useful.
- Other big buffers (APRS state 7,328 B, `test` 2,096 B, DOTA `b` 1,640 B, hotspot 1,728 B, `txBursts` 1,728 B) are
  mode-exclusive in principle, but plus.md's 12.7 KB AMBE/IMBE overlay analysis shows what a safe overlay costs (both
  tasks quiesced, re-init, radio-only checks). Not recommended for this feature.

## 4. The data-chip pack region as an audio spool

Region 0xE70000-0xEFFFFF (576 KB) is already allocated: RadioID 2 x 256 KB (0xE70000, 0xEB0000), air band 2 x 16 KB
(0xEF0000, 0xEF4000). **Unallocated: 0xEF8000-0xEFFFFF = 32 KB** (8 sectors of 4 KB). Using a pack slot not in force
as spool would destroy the user's previous pack copy and race a DOTA / PC pack write: no.

- Capacity of the 32 KB tail: 72 s of DMR frames, 59 s of IMBE parameters (table above). Doubles the over length at
  best; for P25 / transcoded legs it is the only way past 8-18 s.
- Wear: W25Q128-class data flash, ~100,000 erase cycles a sector. A 30 s DMR over writes 13.5 KB = 3.3 sectors; as a
  ring over 8 sectors that is ~240,000 overs before wear-out. Acceptable for an attended relay, but it is real wear on
  the chip that also holds calibration and codeplug (different sectors, same part).
- Timing: a 4 KB erase is ~45 ms typical, up to ~400 ms worst, needed every ~9 s at 450 B/s; page programs every
  ~0.6 s. Must go through the storage mutex on the data chip's SPI bus (shared with codeplug, settings, packs, ID
  lookups): RAM staging of at least one sector-erase worth (~400 ms = 180-220 B) plus a 256 B page buffer.
- **Rule 2 needs a new owner decision**: today the pack region is for user data packs written with CRC and an atomic
  header commit; a raw audio spool is a different use. Ask before any design.
- Cost: ~1.5-2 KB flash, ~0.5 KB RAM. Host-testable with a fake flash (as `tools/test_datapack.py`).

## 5. Pair by pair

Legend: **Fwd** = forward frames as heard (no vocoder; at most a FEC re-encode), **TC** = transcode (decode to PCM and
encode in the other codec). "Live" = the codec step runs as the frames arrive or leave; "offline" = in a pass between
recording and key-up.

| Pair (RX -> TX) | Possible? | How | Codec work | Over length (RAM only) | Status of the pieces |
|---|---|---|---|---|---|
| DMR -> DMR | **yes, built** | Fwd 9 B air frames | none | 38 s | built, host-tested; XB-1..XB-8 have no recorded radio result |
| P25 -> P25 | **yes** | Fwd: LDU voice FEC-decoded to IMBE parameters (11 B), sent again through the P25 transmitter with the encoder skipped | none (FEC only) | 18.5 s (`fifo` + `rp_buf`); 59 s more with the spool | P25 RX and P25 TX both proven on the radio (owner talked with a factory P25 radio, 2026-11-18); a "frame source" in the transmit loop (`p25_lab.c` ~849, `dm32_imbe_encode` replaced by stored parameters) is new. New LC: our own P25 unit ID to the heard TG |
| FM -> FM | possible, **poor value** | TC live at RX: FM receive audio -> IMBE encode (ours, 13-18.6 ms) -> 11 B; TX: IMBE decode (<= 16 ms) -> PCM -> `tx_pcm` (the SSTV / AFSK prompt-FIFO path into the FM modulator) | IMBE both ways, one step per frame | 8-10 s (needs `voice` + `fifo`) | FM RX audio to the MCU: the 8 kHz FM RX RAM copy (NXDN demod) or the flat receive path (`radio_msg_audio`, SSTV / pager); unfiltered discriminator audio: needs de-emphasis and a sub-tone high-pass; the vocoded result sounds digital, not like FM. AMBE instead of IMBE would allow 38 s but is not host-testable (owner's earlier condition) |
| DMR -> FM | **yes, cheapest mixed pair** | stored 9 B frames (as today); TX: stock AMBE decode (~3 ms) -> PCM -> `tx_pcm` into the FM modulator | AMBE decode, live at TX | 38 s | stock decoder in FM board mode is unproven on the radio (NXDN / dPMR use it there, never heard on the radio: no NXDN source); `tx_pcm` path exists (SSTV TX, merged 2026-11-22). AMBE runs only on the radio |
| FM -> DMR | **built 2026-11-25 (branch `xbandfm`), HARDWARE-UNVERIFIED** | TC live at RX: FM audio -> stock AMBE encode (~12 ms) -> `ambeFecEncode` -> 9 B frames; TX as today. **Correction to this study's assumption:** the DM-32 modem does *not* route FM receive audio onto the 8 kHz PCM stream (`hal_audio.h`; the board has no `audio_tap`, unlike the uv380 family), so the audio task's `ambeVoiceAudioBlock` mic is the *microphone*, not the FM audio. Instead the radio task reads the FM receive audio through `board.afsk` (the APRS / NXDN path, 8 kHz) into a ring and feeds it to the encoder through a guarded mic-source hook (`ambeVoiceSetMicSource`); the DMR side's TX talkgroup is used (FM carries none) | AMBE encode, live at RX | 38 s | **built**: engine relay host-tested (`test_xband.c`), capture / encode / relay plumbing and RX/TX tone gating host-tested with a fake encoder and faked `board.afsk` (`test_xband_radio.inc`); the FM receive audio level, de-emphasis and the stock encoder on the air are **radio only, never run** |
| P25 -> DMR | possible, offline | Fwd-store IMBE parameters at RX (11 B), then offline: IMBE decode + AMBE encode per frame (up to ~28 ms each), in place into 9 B frames (smaller), then send as today | both codecs | 8 s (`rp_buf`; `voice` busy) or ~59 s with the spool | the offline pass and the AMBE encode-from-buffer path are new; the delay before key-up grows by about the over's length |
| DMR -> P25 | possible, offline or tight live | store 9 B frames; TX live: AMBE decode (~3 ms, audio task) -> PCM into the P25 worker's `fifo` in place of the mic -> IMBE encode (13-18.6 ms): 16-22 ms per frame, over budget in the worst case; or an offline pass to IMBE parameters (needs room for 11 B per 9 B frame: not in place) | both codecs | 8-10 s (RAM); more only with the spool | the P25 TX loop tolerates 18.6 ms with an 18-block FIFO backlog; 3 ms more in another task of the same or higher priority is unmeasured |
| NXDN -> DMR | **yes, cheap** | Fwd: the 72-bit NXDN voice field **is** the DMR air frame bit for bit (nxdn.md) | none | 38 s | NXDN RX host-tested (MMDVMHost cross-checks), never heard on the radio (no NXDN source); the NXDN receiver runs in FM mode, needs the demod state (`nxdn_receiver_t` 576 B) and the AMBE path only for monitoring |
| NXDN -> FM | yes, radio-only | Fwd-store 9 B, TX as DMR -> FM | AMBE decode | 38 s | as DMR -> FM |
| NXDN -> P25 | as DMR -> P25 | TC | both | 8-10 s | as DMR -> P25 |
| any -> NXDN | **no** | NXDN48 TX not feasible on the native stream (2400 sym/s, deviation ~1.85x) | - | - | ysf-nxdn-tx.md |
| NXDN -> NXDN | **no** (no TX) | - | - | - | - |
| YSF DN -> DMR | yes on paper | Fwd: the DN 49-bit parameters re-FEC'd to the 72-bit DMR frame (`ambeFecEncode`, cross-checked with YSF2DMR) | none | 38 s | YSF RX built, host-tested only; symbol polarity and deviation never checked on the radio (no YSF source) |
| YSF VW -> P25 | yes on paper | Fwd: each VW 144-bit section **is** a P25 IMBE codeword (YSF2P25 style) | none | 18.5 s | same as above; VW RX decodes IMBE in the P25 worker |
| P25 -> YSF VW | yes on paper, gated | Fwd parameters into the YSF VW transmitter | none | 18.5 s | `CONFIG_YSF_TX` built but **off** pending owner approval; deviation ~+-1.94 kHz vs YSF's +-2.7 (72 %); never on the air |
| DMR -> YSF DN | not now | needs a DN transmitter (not built) | none (re-FEC) | 38 s | DN TX not built (ysf-nxdn-tx.md) |
| YSF VD1 / VW data / VD2 DN -> anything | VD1: **no** (not decoded); DN: yes; VW: yes | - | - | - | V/D mode 1 is on plus.md's "still to do" list |
| FM <-> YSF / P25 / NXDN | as the FM <-> P25 / DMR rows with the codec of the digital side | | | | |

### YSF in short

- **The radio can demodulate C4FM / YSF**: YSF is 4800 sym/s 4FSK like P25, so the HR_C7000's native raw stream
  carries it; `p25_lab.c` feeds the air bits through `p25_phy_rx_air` to the YSF framer (`ysf_consume`), FICH decode,
  DG-ID filter. No special HR_C7000 YSF mode; it is our software framing.
- **YSF voice decode**: V/D mode 2 (DN, AMBE+2 half rate) goes to `ambeVoiceSink` (stock decoder); Voice FR (VW, IMBE)
  is decoded by our IMBE in the worker; **V/D mode 1 is not decoded**. End-to-end worker test exists
  (`tests/plus/test_ysf_worker.c`).
- **Not proven on the radio**: the owner has no YSF transmitter, so polarity, deviation and DN audio in native mode
  are unverified (plus.md "YSF progress").

## 6. Mode switching on one receiver

The relay's listen phase tunes A and B alternately (1 s dwell, held while bursts are heard). With different modes
per side the dwell also switches the board mode: DMR (HR_C7000 DMR), P25 / YSF (native stream owned by the P25
worker; `p25_lab_set_channel`, the worker stops and starts), FM / NXDN (FM board mode, FM RX RAM). Multi-watch and
All digital search already switch between these, so the mechanism exists, but the switch time is unmeasured (the
soft-probe study lists the native hand-over as "not measured on the radio"). Expect more of the over's start lost on
the P25 side than DMR's ~1 s (the native receiver needs a sync, then an LDU with clean voice; late entry). An FM side
needs carrier squelch (and CTCSS if set) as its "heard" signal.

Each over still runs one mode at a time: record in mode X, retune, switch to mode Y, key. The worker's `voice` must
stay out of the loan whenever either side is P25 / YSF.

## 7. Flash cost (FLASH 890,276 of 901,120 B: ~10.8 KB left)

Rough estimates from what the DMR-only relay cost (+7.6 KB including strings and Turkish) and the size of the pieces:

| Step | Flash | RAM |
|---|---:|---:|
| Per-side mode in the engine, form and checks (mode per side, refusals per mode, view strings EN + TR) | 1.0-1.5 KB | ~16 B |
| P25 <-> P25 (RX parameter store, P25 TX frame source, LC with own ID, loan without `voice`) | 1.5-2.5 KB | ~16 B |
| DMR / NXDN -> FM (stored frames -> AMBE decode -> `tx_pcm`) | 1.0-1.5 KB | ~0 |
| NXDN -> DMR (NXDN receiver as a source) | 0.5-1.0 KB | ~0 |
| FM -> DMR (FM audio into the AMBE encoder, squelch as "heard") | 1.5-2.0 KB | ~0-200 B |
| P25 <-> DMR transcoding (offline pass, both directions) | 2.0-3.0 KB | ~350 B (PCM frame + state) |
| YSF DN -> DMR, YSF VW <-> P25 (sources; YSF TX itself is behind `CONFIG_YSF_TX`, extra) | 0.5-1.0 KB | ~0 |
| FM -> FM via IMBE (de-emphasis, sub-tone high-pass, both legs) | 1.5-2.0 KB | ~100 B |
| Spool in the pack-region tail (needs owner rule-2 decision) | 1.5-2.0 KB | ~0.5 KB |
| **Everything** | **~11-17 KB** | ~1-1.3 KB |

Everything does not fit in 10.8 KB. The first three to four steps (about 4-6.5 KB) do. A reclaim (strings, an
unused feature off in the preset) would be needed for the rest.

## 8. Transmit legality and guards

- Amateur bands only, as today: `xband_check_hz` (144-146 / 430-440 MHz, no PMR446, no repeater segments, no
  satellite segments, no APRS frequency, no offset channel), CW ID every 10 minutes on each frequency used, 3 minutes
  an over, attended only, screen locked, keying through `ptt_down` so every TX gate (inhibit, battery, band,
  time-out) holds. A multi-protocol version must keep all of these per side; nothing is relaxed.
- P25 TX: works in our fork (ROADMAP item 4: "Works in DM32 Plus", owner QSO with a factory P25 radio 2026-11-18,
  plus-radio-test T3). Its deviation is the DMR calibration (Harris decodes it). A relay P25 TX carries **our** unit ID,
  never the caller's.
- DMR TX: proven (normal DMR TX). FM TX: proven (normal FM). The SSTV `tx_pcm` path: merged, radio result not
  recorded in ROADMAP.
- YSF TX: `CONFIG_YSF_TX` off; needs owner approval and an on-air check first. NXDN TX: not feasible.
- No new TX or band limits beyond today's without asking the owner (owner rule, memory).

## 9. What can be host-tested, what only the radio can show

| Host (fakes, ASan / UBSan) | Radio only |
|---|---|
| Engine: per-side mode, rules, state machine, frame store, spool ring with a fake flash | Any stock AMBE+2 encode / decode (owner's codec bytes, never in host tests) |
| P25 <-> P25 parameter store and the P25 TX frame source (real worker with a fake modem, as `test_ysf_tx_worker.c`) | Mode-switch times between DMR, native and FM modes; start-of-over loss |
| NXDN / YSF frames to DMR frames (already cross-checked with MMDVMHost / YSF2DMR) | FM receive audio level, de-emphasis, sub-tone; `tx_pcm` audio on the air |
| IMBE legs (our codec): FM -> IMBE -> FM, P25 -> PCM | CPU load of the offline pass and of live DMR -> P25 (deadline misses, UI starvation) |
| Offline transcode pass with a stub AMBE (timing model only) | Spool erase / program latency on the real data chip; wear in practice |

## 10. Ranked plan and verdict

Ordered by value per cost and risk. "Radio" = needs the owner's on-air test (each serial / flash / RF step with his
approval, rule 1).

1. **Prove the existing DMR relay on the radio first** (XB-1..XB-8). No flash. Everything below builds on it.
2. **Per-side mode + P25 <-> P25** (frames as heard, 18.5 s an over). ~2.5-4 KB flash. P25 RX and TX are both
   proven; the new code is host-testable with the real worker and a fake modem. Radio: one two-radio P25 test.
3. **NXDN -> DMR and DMR / NXDN -> FM** (frames as heard into DMR; stock AMBE decode into `tx_pcm` for FM). ~1.5-2.5
   KB. No transcoding CPU risk (one ~3 ms decode a frame). Radio-only proof of the AMBE decoder in FM board mode
   (this also settles the open NXDN / dPMR audio question).
4. **FM -> DMR** (stock AMBE encode of FM audio while recording). ~1.5-2 KB. **Built 2026-11-25 (branch `xbandfm`,
   +1,204 B flash with the FM sub-tones), host-tested, HARDWARE-UNVERIFIED**; still wants a read-only measurement of
   FM audio levels into the encoder before it is trusted on the air. **Order on the radio:** that read-only check
   first (the FM receive audio level through `board.afsk` into the encoder, nothing transmitted), and only once it
   looks sane the on-air FM -> DMR rows (docs/plus-radio-test.md XB-16, XB-17). FM CTCSS / DCS sub-tones were added with it (RX
   gate, TX tone), reusing the radio's own `ch->rx_tone` / `ch->tx_tone` path (no new TX code).
5. **YSF legs** (DN -> DMR, VW <-> P25). ~0.5-1 KB, but nothing YSF is proven on the radio and YSF TX needs the owner's
   approval; wait until a YSF source exists.
6. **P25 <-> DMR transcoding** (offline pass). ~2-3 KB and a reclaim; 8 s overs without the spool, so it really
   needs step 7. Doubles the relay delay. Worth it only if the owner accepts both.
7. **Spool in 0xEF8000-0xEFFFFF** (32 KB). ~1.5-2 KB; **needs a new owner rule-2 decision** (audio is not a data
   pack; flash wear on the chip that holds calibration and codeplug).
8. **FM -> FM** via IMBE: low value (8-10 s, vocoded sound), last or never.

Never: anything -> NXDN (no NXDN TX), P25 phase 2, YSF V/D mode 1 (until decoded), simultaneous RX and TX.

**Go / no-go.** Go for a limited "multi-protocol" version: steps 1-3 (and 4 after a measurement) fit the remaining
flash and RAM and use only frame forwarding or one codec step a frame. **No-go for "any mix"**: P25 <-> DMR needs
transcoding that is over the real-time CPU budget, gets only ~8 s an over in RAM, and fits flash only with a reclaim;
NXDN cannot be transmitted at all; YSF is unproven on the radio. Nothing here is for the current release.
