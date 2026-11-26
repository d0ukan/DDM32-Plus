<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# DOTA Transfer: channels and zones from radio to radio (design, phase 1)

Status: **phase 2 built** (2026-11-20, branch `dota`): steps 0-7 of section 18 are in, host-tested, not yet tried on
a radio (step 8, the rows D-* of `docs/plus-radio-test.md`). Sections 1-16 are the phase 1 design; the owner's
answers are section 17 and **what was built differs from the design where section 19 says so** (section 19 wins).
Section 20: the content type "data pack" (round 14, branch `datapack`).

DOTA Transfer sends channels, zones and the DMR contacts they use from one DM-32UV running DDM32 Plus to another one
running the same firmware, over the air, on FM (AFSK 1200, the APRS modem) or DMR (the DMR data service). One
transport-independent protocol layer (session, frames, CRC, poll-response ARQ) runs over both.

Clean room: designed from this tree (Plus sources, the pinned upstream RadioFoundry tree and its `docs/spec/`),
AX.25 2.2, ETSI TS 102 361-1/-2/-3 and the RadioFoundry zone sharing spec (`docs/spec/zone-share.md`, MIT, read for
ideas only: the DOTA record format is ours). No other radio firmware's code was read.

**Line references** (`file:line`) are to the patched tree the Plus build compiles, snapshot of
`~/ddm25-r13/build/radiofoundry-p25/` on 2026-11-20 (FLASH 864,036 B, RAM 209,672 B). Lines move as Plus changes;
the function names are the stable reference. Spec names without a path (`aprs.md`, `dmr-data.md`, `gotchas.md`,
`hardware-dm32.md`, `modem-hrc7000.md`, `zone-share.md`) are upstream's `docs/spec/` in the pinned tree.

## 1. Goals and limits

| | v1 |
|---|---|
| What | one channel, selected channels (any in the codeplug), one zone, all zones with their channels ("DOTA Clone"); the DMR contacts those channels talk to |
| Transports | FM: AFSK 1200 (Bell 202) in AX.25 UI frames, CTCSS required. DMR: unconfirmed UDP/IP over DMR data, rate 1/2, private to the receiver's DMR ID, on a chosen colour code and slot (direct mode) |
| Roles | TX (sender) and RX (receiver), picked on the screen |
| Receiver import | add to the current zone, or into a new zone (name required); "imported channels may transmit" (default off: receive only) |
| Atomic | nothing visible changes unless every frame arrived and verified; a failure or Back leaves the receiver's codeplug as it was (section 10) |
| Never sent | calibration, the radio's DMR ID, per-channel DMR ID overrides, P25 unit IDs, settings, privacy keys, APRS configuration, location fields |
| No encryption | amateur rules: every frame in clear, the sender's callsign in every frame |
| Not in v1 | overwrite everything on the receiver; RX group lists (section 9.4); scan lists; rate 3/4 DMR |
| Capacity | 1024 channels, 80 per zone, 68 zones, 1024 contacts (`core/codeplug/codeplug.h:19`, `:193-194`, `:265`) |

## 2. Feasibility verdict

| Transport | Verdict |
|---|---|
| **FM** | **Feasible with small patches.** Everything exists: AFSK modulator and HDLC/AX.25 framing (`core/aprs/afsk.c`, `hdlc.c`, `ax25.c`), the DM-32's AFSK transmit path (`platform/hrc7000/hrc_afsk.c`) and receive path (the HR_C7000 FM RX RAM, `fm_rx_audio.c`), CTCSS on transmit and the FD6818's tone detect on receive (`boards/dm32/radio_dm32.c:337`, `:408`, `:490`). Needed patches: (1) an append call in `hrc_afsk.c` so one keyed transmission can carry ~8 s of frames from a 512 B buffer (the APRS path takes one buffer of at most 4096 symbols, `aprs.c:31`); (2) route the APRS demodulator's frames to DOTA while a session listens (`aprs.c:1104`, `:1136`); (3) a DOTA block in the radio core like APRS's (`radio_sm.c:3741-3930`) that keys **with** a CTCSS tone (APRS keys with none, `radio_sm.c:3879`); (4) optionally the sender's speaker monitor during AFSK transmit (board patch, `radio_dm32.c:597`). No blocker. Untested on this unit: APRS TX/RX (upstream measured both on a DM-32 on its SDR bench, `hardware-dm32.md`), and CTCSS together with AFSK (measured nowhere) |
| **DMR** | **Feasible with small patches.** The DMR data service is in the build (`CONFIG_DMR_DATA_CORE`, DMR_SMS, DMR_TMS on): unconfirmed IP packets with compressed UDP headers, and up to 8 packets chained in one transmission of up to 64 bursts (`dmrDataServiceSendIPChain`, `core/dmr/dmrDataService.c:715`; written, not linked today). Needed: (1) a UDP receive hook for the DOTA port next to zone sharing's (`dmrDataService.c:2202-2208`, compiled out today); (2) the radio core's DOTA block tuning a temporary DMR channel (frequency, CC, slot, DMO) so the existing data path runs on it (`radio_sm.c:2179` `data_tick`). ZONE_SHARE (52 KB / 12.5 KB RAM) is **not** needed. One limit: **no sound on the DMR sender while it transmits** (the speaker PCM shares the prompt FIFO with the modulator and is gated off during DMR TX, `modem-hrc7000.md` 8.4, `radio_dm32.c:555-565`). Untested on this unit: DMR data (SMS) TX and RX |
| **Budget** | **Fits, with one RAM reclaim first.** Estimated +16..24 KB flash of the ~37 KB free. Static RAM: the 2,048 B APRS ring in `core/usbaudio/audio_mux.c:42` is dead on the DM-32 (the board has its own AFSK receive path, so `aprs.c` never calls `audio_mux_aprs_rx`); compiled out, it pays for DOTA's 1,280 B session buffer and ~190 B of state, net about **-580 B** (section 14) |

## 3. Architecture

```
  UI task                         radio task (radio_sm_poll, ~25 ms)          housekeeping task
  core/ui/ui_dota.c  --req-->  core/radio/radio_dota.inc (radio core block) 
  (screens, forms,             core/dota/dota_proto.c  (session, ARQ, frames) 
   progress, texts)  <-view--  core/dota/dota_rec.c    (record codec v1)       core/dota/dota_import.c
                                 |               |                             (staging, commit, undo:
                       FM: dota_fm.c         DMR: dota_dmr.c                     storage jobs)
                       afsk/hdlc/ax25,       dmrDataServiceSendIPChain,
                       hrc_afsk (append),    UDP hook in dmrDataService.c
                       aprs.c demod route
```

- `core/dota/` is radio-agnostic (no board header), host-testable with a fake transport and the simulated flash.
- Our code lives in `integration/radiofoundry/dota/`, installed into `core/dota/`, `core/ui/` and `core/radio/` by a new
  `tools/plus_dota.py` (called last from `plus_features.prepare()`, as `plus_toolbox.py` is), switch `CONFIG_DOTA`
  (needs `APRS=y APRS_RX=y DMR_DATA_CORE=y ZONE_EDIT=y`). Upstream files change only through transforms there.
- Threads: the engine runs in the radio task; codeplug **reads** for the sender run there too (the radio task already
  reads channels, `radio_sm.c:661` `set_channel`). Codeplug **writes** never run in the radio task: the receiver hands
  frames to a storage job (`storage_job_post`, `core/storage/storage.h:108`, posted from the UI task as the zone
  editor does: "one job at a time, posted from one task").
- The radio core owns the radio for the session (section 11.3): no scan, dual watch, APRS beacons, FM caller ID,
  pager, NXDN listener, roaming, CW beacon, lone worker alarms or codeplug reload until the session ends; then the
  user's channel or VFO is restored exactly as it was (`reload()`, `radio_sm.c:5201`).

## 4. FM transport

### 4.1 What is reused

| Piece | Where | Use |
|---|---|---|
| AX.25 UI frame encode / decode | `core/aprs/ax25.c` (`ax25_encode`, `ax25_decode`), `ax25.h` | destination `DOTA`, source = our callsign-SSID, control 0x03, PID 0xF0, information field = the DOTA frame |
| HDLC encoder (flags, stuffing, FCS, NRZI) | `core/aprs/hdlc.c:7-80` (`hdlc_enc_init`, `hdlc_enc_flags`, `hdlc_enc_frame`) | one AX.25 frame per 256 B half-buffer of tone bits; the NRZI level is carried from half to half (`hdlc_enc_t.level`) |
| CRC-16 (X.25) | `ax25_fcs` (`ax25.h`) | the HDLC FCS, and the DOTA frame's own CRC-16 (section 6) |
| AFSK modulator, 8 kHz, phase continuous | `core/aprs/afsk.c:38` `afsk_mod`, driven by `platform/hrc7000/hrc_afsk.c:42` `fill()` from timer 4 every 2 ms | unchanged; the tones go through the prompt FIFO to the modulator with the microphone muted (`radio_dm32.c:597` `afsk_tx_route`) |
| Keying | `board.radio->tx(hz, bw, power, &tone)` (`radio_dm32.c:374`; sets the C7000 sub-audio before FM_PTT, `:408`) | as APRS does (`radio_sm.c:3879`) but **with the session's CTCSS tone** instead of `none` |
| AFSK demodulator (2 paths, integer, ~6 % CPU) | `core/aprs/afsk.c` via APRS's state `a.demod` (`aprs.c:135`) and receive loop `rx_service` (`aprs.c:1136`) | reused as is: while DOTA listens, `rx_service` runs on the DOTA frequency and `demod_frame` (`aprs.c:1104`) hands frames to DOTA instead of `aprs_rx_frame` |
| FM receive audio (HR_C7000 RX RAM, 8 kHz, not squelch gated; 2 KB ring from the heap while listening) | `platform/hrc7000/fm_rx_audio.c:45`, `hrc_afsk.c` `rx_start` / `rx_read` | unchanged; FM caller ID yields the ring as it does for APRS (`radio_fmid_yield`) |
| CTCSS detect | FD6818, `status()` `st->tone_ok` (`radio_dm32.c:490`); the core's tone squelch (`radio_sm.c` `tone_needed`) | the receiver tunes with the session tone: the speaker opens only on it, and a frame counts only when `tone_ok` was seen within the last 1.5 s |
| Band and amateur checks | `board_in_band(hz, true)` (`hal/board.h:122`), the cross-band's band plan (`xband_check_hz`, `ui_xband_refusal`; was `aprs_amateur_hz`), `aprs_callsign_ok` (`aprs.c:181`) | refuse a frequency the radio may not transmit on; refuse outside 144-146 / 430-440 MHz and the band plan's segments (12.6, owner 2026-11-21; was a warning outside 2 m / 1.25 m / 70 cm); the callsign rule is APRS's (the radio name, as "Set your callsign (the radio name) in qdmr.") |

### 4.2 Patches

1. **`hrc_afsk.c`: append.** `hal_afsk_t` gains `bool (*tx_more)(const uint8_t *tones, uint16_t nbits)`: queue the next
   buffer while one plays. In `fill()` (`hrc_afsk.c:42`), when `afsk_mod` returns 0 and a next buffer is queued, switch
   to it (keep `mod.phase`, reset `mod.sample` and `mod.bit`), else end as today. The radio task refills the half the
   interrupt has left (a half holds one frame: 1.1 s at most, so a 25 ms poll has ample time). The switch can shift
   the bit clock by up to one sample (125 us, 15 % of a bit) at the boundary, which falls inside the flag between
   frames. About 80 B. APRS keeps calling `tx_start` alone and behaves as today.
   Without this patch the fallback is one `tx_start` per frame with the routing left on between them: a gap of
   ~30 ms with an empty prompt FIFO between frames, whose on-air content is unknown (`modem-hrc7000.md` 8.4 flushes
   stale audio with 0x7F bytes). The append is preferred.
2. **`aprs.c`: the receive route.** `rx_service`'s `want` (`aprs.c:1140`) becomes `want || dota_fm_listening(r->rx_hz)`,
   and `demod_frame` calls `dota_fm_frame()` while DOTA listens. `aprs_can_start` (`radio_sm.c:3741`) is false while a
   session runs, so no APRS transmission or queue is touched. About 100 B.
3. **Radio core block** (`radio_dota.inc`, included in `radio_sm.c` next to the APRS block; section 11.3). The FM
   phases mirror APRS's `aprs_rf_service` (`radio_sm.c:3849`): KEYUP 25 ms, SEND until the last half drained, back to
   receive on the same frequency (no retune to the user's channel between windows). Early return from
   `radio_sm_poll` while keyed, as `radio_sm.c:6858` does for APRS. Receive phases run the normal FM path so the
   squelch, the tone squelch and the speaker work as on any channel.
4. **Sender monitor (optional, sound plan).** The prompt FIFO also plays to the codec DAC (`modem-hrc7000.md` 8.4:
   "documented only as playback to the codec DAC"); during an FM transmission the speaker is off because the core
   sets `RS_TX`. `afsk_tx_route(on)` with a monitor flag also turns `amp(AMP_PCM)` on and keeps the speaker source on
   the codec (`radio_dm32.c:501`, `:535`). The volume knob works on the amplifier (`hardware-dm32.md`: "The volume knob
   works on the amplifier"), so the monitor follows the knob. ~40 B. **Unverified**: whether the DAC output reaches
   the speaker while the transmitter is keyed.

### 4.3 FM timing parameters

| Item | Value | Why |
|---|---|---|
| Bit rate | 1200 bit/s; HDLC stuffing adds ~1.6 % on our data | Bell 202 (`afsk.h`) |
| Preamble (flags) | 400 ms (60 flags) | the FD6818 must detect the CTCSS tone before the first frame (detect time unmeasured; APRS uses 300 ms, `APRS_TXDELAY_DEFAULT`) |
| Key-up before the tones | 25 ms | APRS's `APRS_KEYUP_MS` |
| Tail | 3 flags (20 ms) | APRS's `TAIL_FLAGS` |
| Frame | AX.25 16 B + DOTA header 12 B + payload <= 128 B + DOTA CRC 2 B + FCS 2 B + 1 flag | 161 B max = 1.09 s |
| Window | <= 7 frames in one transmission, <= 8.0 s keyed | owner's "~8-10 s of data" |
| Turnaround | the answering side keys 150 ms after the other side's window or answer ended | the other side's `tx_off` and receiver settling |
| Max keyed | min(9 s, the radio's TX time-out - 1 s) per transmission | section 7.5 |
| Bandwidth, deviation | 12.5 kHz, AFSK level 50 (3.1 kHz peak measured upstream at 25 kHz) | narrow keeps adjacent users happy; the level is APRS's default. Open: 25 kHz option |
| Frequency rules | must be TX-allowed (`board_in_band`); warning outside amateur bands; **refused on an APRS frequency** (the region's, the custom one, 145.825) | an APRS iGate would gate our UI frames to APRS-IS |

## 5. DMR transport

### 5.1 What is reused

| Piece | Where | Use |
|---|---|---|
| UDP/IPv4 over DMR, compressed header (SAP 3, 9 B with ports outside 4000-4099) | `dmrDataEncodeIP`, `ipPacketBursts` (`dmrDataService.c:700-713`), `docs/spec/dmr-data.md` 11 | the DOTA frame is the UDP payload; source and destination port **4101** (zone sharing has 4100) |
| Chained packets in one transmission | `dmrDataServiceSendIPChain` (`dmrDataService.c:715`): 2 preamble CSBKs once, then each packet's header and blocks, up to 8 packets and the 64-burst list (`DMR_DATA_MAX_BURSTS`, `dmrData.h:38`) | 3 DOTA packets of <= 192 B per transmission (62 bursts, 3.7 s) |
| Unconfirmed delivery | `DMR_IPS_UNCONFIRMED`; `CONFIG_DMR_CONFIRMED=0` in Plus | DOTA's ARQ does the retries |
| The guard between our transmissions | 250 ms (`CONFIG_DMR_DATA_TX_GUARD_MS`, `dmr-data.md` 12.6.3) | between the two transmissions of a window |
| Busy / result | `dmrDataServiceIsBusy` (`:252`), `dmrDataServiceIpBusy`, `dmrDataServiceIpResult` | the packets' buffers stay ours until IpBusy is false |
| The data path on the current channel | `data_tick` (`radio_sm.c:2179`) builds `dmrDataChannel_t` from `sm->view` each tick; `data_can_send` (`:1963`) needs a DMR channel, TX allowed, our DMR ID set; `data_tx_start` (`:1973`) keys at `sm->view.power` | the radio core puts the session's temporary DMR channel in `sm->view.ch` (rx = tx = the frequency, CC, slot, DMO, low power) and tunes; the existing path then runs on it |
| Receive | the service reassembles packets (`dmrDataRxPacket`, 612 B) and dispatches UDP by port (`dmrDataService.c:2160-2210`); `forUs` = private to our ID | new hook: `udp.dstPort == DOTA_PORT && !packet->group` -> `dota_dmr_rx(packet->src, payload, len)` next to zone sharing's |
| TX deadline | 10 s + 60 ms a burst (`gotchas.md` 3.9) | covers a 62-burst transmission |

### 5.2 Patches

1. UDP hook in `dmrDataService.c` (one `if`, ~60 B) and a getter for the received data burst count (`stats.rxBlocks`,
   for the crackle; `dmrDataServiceGetStats` copies the whole struct, too big for the radio task's stack) ~20 B.
2. `dmrDataServiceSendIPChain` and its helpers become linked (they are gc'ed today): ~600 B of existing code.
3. Radio core block: the temporary DMR channel (section 11.3). DMR keying, the time-out and the burst timing stay
   the call stack's (`radio_sm.c` returns at `if (sm->dmr_running)` after the stack runs).

### 5.3 DMR timing parameters

| Item | Value |
|---|---|
| Burst | one 30 ms burst per 60 ms frame on the chosen slot (direct mode) |
| Data packet | 1 header + ceil((9 + 20 + P + 4) / 12) rate 1/2 blocks: 9 B compressed UDP/IP header, 20 B DOTA header with callsign and CRC, P payload (<= 192), 4 B CRC-32. P = 192: 20 bursts (1.2 s) |
| Transmission | 2 preambles + 3 data packets <= 62 bursts = 3.7 s |
| Window | 2 transmissions (6 packets) with the 250 ms guard between them: <= 7.7 s |
| Answer | 2 preambles + header + 4 blocks = 7 bursts (0.42 s) |
| Turnaround | ~0.4 s before each side's answer keys: the receiving modem's end-of-call state (200-400 ms, `gotchas.md` 3.6) plus the stack's channel access (estimate; measure on the radio) |
| Addressing | private call to the receiver's DMR ID, which its Ready screen shows; answers go to the sender's ID (`packet->src`) |

## 6. DOTA frames (protocol version 1)

Every frame, on either transport, is: header (12 B), the sender's callsign (6 B, only when flag CALL is set), the
payload, CRC-16 (2 B, X.25 as `ax25_fcs`, low byte first) over all that precedes it. Multi-byte fields are big-endian.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | bits 7-4 protocol version (1), bits 3-0 type |
| 1 | 1 | flags: bit 0 POLL (answer after this frame), bit 1 CALL (callsign follows the header), bits 3-2 section (0 none, 1 contacts, 2 channels, 3 zones), bits 7-4 frames left in this window after this one (0-15) |
| 2 | 2 | pairing code, 0-9999 (the receiver's, typed into the sender) |
| 4 | 2 | session ID (the sender's random choice, not 0, new for every transfer) |
| 6 | 2 | DATA: frame number (1..N); ACK: the bitmap's base; others 0 |
| 8 | 2 | DATA: index of the first record of the section in this frame |
| 10 | 1 | DATA: records in this frame |
| 11 | 1 | payload length (checked against what the transport delivered) |

- **Callsign**: on FM the AX.25 source address carries it (CALL clear); on DMR the 6 B field (ASCII, space padded) is
  in every frame (CALL set). The receiver's answers carry the receiver's callsign the same way: it transmits too.
- A frame whose version, length, CRC, pairing code or session doesn't match is dropped without an answer (a stranger's
  radio never gets an answer from us; a wrong code shows as "No answer" on the sender).

| Type | Name | From | Payload |
|---|---|---|---|
| 1 | HELLO | TX | the manifest (6.1) |
| 2 | HELLO_ACK | RX | status (1; 0 accepted, else a refusal code 6.3), free channels (2), free zone slots (1), room in the current zone (1), the receiver's firmware string (len 1 + up to 24) |
| 3 | DATA | TX | records of one section (section 9) |
| 4 | ACK | RX | bitmap (4: bit i = frame base + i received and queued), frames received in all (2), flags (1: bit 0 queue full, slow down) |
| 5 | END | TX | data frames N (2), channels (2), contacts (2), zones (1), transfer CRC-32 (4) |
| 6 | END_ACK | RX | result (1; 0 done, 1 frames missing, 2 CRC mismatch, 3 flash write failed, 4 writing, ask again), channels added (2), zones added (1), contacts added (2), contacts matched (2) |
| 7 | ABORT | either | reason (1): 1 user, 2 battery, 3 codeplug changed underneath, 4 timeout |
| 8 | POLL | TX | none: "answer now" (the window's POLL frame was lost, or the answer was) |

**Transfer CRC-32**: the IEEE CRC-32 (`storage_crc32`) of each DATA frame's bytes 6..end of payload (frame number,
first index, count, length, payload), **summed** mod 2^32 over all frames. The sum does not depend on the order the
receiver gets frames in, so it needs no buffer. END carries the sender's sum.

### 6.1 HELLO manifest

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | model, `DM32` (from `RFOUNDRY_BOARD_ID`) |
| 4 | 1 | record format version (1) |
| 5 | 1 | codeplug layout (1: open-firmware image, 80-channel zones) |
| 6 | 1 | modes the build has: bit 0 FM, 1 DMR, 2 P25, 3 YSF, 4 NXDN, 5 dPMR |
| 7 | 1 | kind: 0 one channel, 1 selected channels, 2 a zone, 3 all zones (DOTA Clone) |
| 8 | 2 | channels |
| 10 | 2 | contacts |
| 12 | 1 | zones |
| 13 | 2 | data frames N |
| 15 | 4 | record bytes in all |
| 19 | 1 + n | firmware string (`RFOUNDRY_VERSION`, e.g. `dm32-plus-0.1.0-dev`, at most 24) |

### 6.2 Compatibility rule

Refused (HELLO_ACK status, nothing written) when the model, protocol version, record version or codeplug layout
differ, when the HELLO names a mode the receiver lacks, or (owner's rule, open question 1) when the firmware strings
differ. The refusal carries the receiver's string so both screens can name it.

### 6.3 Refusal codes (HELLO_ACK status)

1 other model or protocol, 2 other firmware, 3 not enough free channels, 4 no free zone slot, 5 not enough free
contacts, 6 the current zone has no room (80), 7 not ready (codeplug not stored, another firmware's codeplug, a
storage job busy), 8 a mode this build lacks.

## 7. ARQ: poll and response

### 7.1 Flow

1. The sender's first transmission carries HELLO and, after it, as many DATA frames as the window holds; when every
   DATA frame fits, END follows in the same window. The receiver checks HELLO first; frames after a refused HELLO are
   dropped. Small transfers (one channel, a small zone) are therefore one transmission and one answer.
2. The last frame of each window has POLL set; each DATA frame says how many frames follow it in the window (flags
   bits 7-4). The receiver answers after the POLL frame, or, when the POLL frame was lost, at the time the window
   should have ended (the frames left times the longest frame time, plus 300 ms) once the carrier is gone.
3. The answer is HELLO_ACK (to a window that began with HELLO), then ACK, or END_ACK (to END).
4. The next window holds, lowest frame number first, the frames the ACK bitmap names missing, then new frames, up to the
   window size. Frames in flight span at most 32 frame numbers from the base (the bitmap's width); when the span is
   full, a window carries only resends.
5. When every DATA frame is acknowledged, END (POLL). The receiver checks that it has all N frames and the transfer
   CRC-32, commits (section 10) and answers END_ACK. While the commit runs it answers a repeated END with result 4
   (writing); afterwards a repeated END gets the same final END_ACK (idempotent).

### 7.2 Timers and retries

| | FM | DMR |
|---|---|---|
| Window | <= 7 frames, one transmission | <= 6 packets, two transmissions + 250 ms guard |
| Payload per frame | <= 128 B | <= 192 B |
| Answer timeout at the sender (from the end of its window) | 2.5 s | 3.0 s |
| On timeout | POLL alone, at most 4 times; then "No answer from the receiver" | the same |
| A frame resent | at most 8 times; then the transfer fails | the same |
| END_ACK wait | END repeated every 4 s, 5 times (covers a commit of a full codeplug) | the same |
| Receiver idle | 30 s without a valid frame after HELLO: "Incomplete: nothing changed" | the same |
| Receiver Ready | waits until Back (no time-out) | the same |

### 7.3 Pairing and session

- The receiver draws a 4-digit pairing code when the user presses Ready (an LCG seeded from the millisecond clock,
  the noise reading and the battery millivolts) and shows it in large digits. The sender types it.
- The receiver binds to the first HELLO with its code and accepts frames only with that code and session ID (and on
  DMR from that source ID) until the session ends.
- The pairing code is not a secret (it goes in clear); it prevents accidents: two transfers on one frequency, a
  stray radio.

### 7.4 Who transmits when

The sender transmits windows; the receiver transmits only HELLO_ACK, ACK, END_ACK and, when the user stops it, one
ABORT. The receiver never transmits to a frame that failed any check (section 6).

### 7.5 Time-out (TOT) and duty

DOTA's transmissions are not PTT: the channel's TOT doesn't run on them, as for APRS. DOTA keeps its own limits: a
transmission is at most min(9 s, the radio's TX time-out - 1 s) (`radio_settings_t.tot_s`); the window shrinks to
fit. On FM a long transfer keys about 87 % of the time (7.2 s on, 1.1 s off) for minutes; at the default low power
this is what APRS's own long frames do, but over several minutes. Open question 7 proposes a rest of 5 s after each
minute of key time.

### 7.6 Ending early

| Event | Sender | Receiver |
|---|---|---|
| Back on the progress screen | "Stop the transfer?"; OK sends ABORT once and ends: "Stopped: the receiver changed nothing" | the same question; OK sends ABORT once, undoes (section 10), "Incomplete: nothing changed" |
| ABORT heard | "Stopped by the receiver" | "Incomplete: nothing changed" |
| Battery critical (`BATT_CRITICAL`) | ABORT, ends | ABORT, undo |
| PTT | refused with the deny tone while a session runs (open question 4) | the same |
| Codeplug changed from outside (CPS over the cable: `codeplug_serial` moves without DOTA writing) | ABORT, ends | ABORT, undo |

## 8. Throughput and time

### 8.1 Sizes used

Record sizes (section 9): channel ~22 B on average (14 B FM simplex with an 8-character name, 21 B FM repeater with
tones, 22 B DMR with a contact, 20 B P25), contact ~14 B, zone ~16 B (name and one run of members; 22 B with three
runs). FM frame: 5 channel records (110 B) per frame; DMR packet: 8 channel records (176 B).

FM, at 1200 bit/s and 1.6 % stuffing, a byte takes 8 x 1.016 / 1200 = 6.77 ms:

| Piece | Bytes | Time |
|---|---|---|
| DATA frame, 5 channels | 16 + 12 + 110 + 2 + 2 + 1 = 143 | 0.97 s |
| DATA frame, 128 B payload | 161 | 1.09 s |
| HELLO | 16 + 12 + 48 + 2 + 2 + 1 = 81 | 0.55 s |
| END / ACK | 45 / 44 | 0.30 s |
| Window start | key-up 0.025 + preamble 0.40 | 0.43 s |
| Answer cycle | 0.15 wait + 0.025 + 0.40 + 0.30 ACK + 0.02 tail + 0.15 before the next window | 1.05 s |
| Full window | 0.43 + 7 x 0.97 + 0.02 = 7.24 s, + 1.05 answer | **8.29 s per 7 frames = 35 channels**: 4.2 channels/s, 93 B/s of records |

DMR, 60 ms a burst:

| Piece | Bursts | Time |
|---|---|---|
| DATA packet, 8 channels (176 B) | 1 + ceil((9 + 20 + 176 + 4) / 12) = 1 + 18 = 19 | 1.14 s |
| Transmission: 2 preambles + 3 such packets | 59 | 3.54 s |
| HELLO / END | 8 / 5 | 0.48 / 0.30 s |
| Answer cycle | ~0.4 + 7 bursts 0.42 + ~0.4 | ~1.2 s |
| Full window | 2 x 3.54 + 0.25 guard + 1.2 answer | **8.53 s per 6 packets = 48 channels**: 5.6 channels/s, 124 B/s of records |

### 8.2 Scenarios (no loss; "10 % loss" resends one frame in ten once, plus the extra windows)

| Transfer | FM | DMR |
|---|---|---|
| **1 channel** | one window: 0.43 + HELLO 0.55 + DATA (55 B) 0.37 + END 0.30 + 0.02 = 1.67 s; commit ~0.3 s; END_ACK 0.9 s: **~3 s** (+0.6 s start tones) | one transmission: 2 + 8 + 6 + 5 = 21 bursts = 1.26 s; commit 0.3 s; answer 1.2 s: **~3 s** |
| **16-channel zone** | FM channels, no contacts: HELLO + 4 DATA (5, 5, 5, 1 channels) + zone + END = 7 frames, one window: 0.43 + 0.55 + 3 x 0.97 + 0.37 + 0.33 + 0.30 = 4.9 s; commit ~0.7 s; answer 0.9 s: **~6.5 s**. With 4 DMR contacts (one more frame, END in a second window): **~9.5 s** | 4 contacts, 2 channel packets: TX 1 (HELLO, contacts, 2 x channels) 57 bursts = 3.42 s, guard 0.25, TX 2 (zone, END) 13 bursts = 0.78 s; commit 0.7 s; answer 1.2 s: **~6.5 s** |
| **80 channels (one full zone)** | 16 DATA + zone + HELLO + END = 19 frames in 3 windows: 6.8 + 7.2 + 4.0 s on air, 3 answers 3.2 s, commit 1.2 s: **~22 s**; 10 % loss ~26 s | 10 contacts (1 packet, 16 bursts), 10 channel packets, zone, HELLO, END = 233 bursts in 4 transmissions (64, 59, 59, 51) = 14.0 s; 2 windows: 2 guards 0.5 s, 2 answers 2.4 s, commit 1.2 s: **~18 s**; 10 % loss ~22 s |
| **Full codeplug** (1024 channels, 150 contacts, 68 zones of ~3 runs) | channels 205 frames, contacts 17 (9 a frame), zones 14 (5 a frame) + HELLO + END = 238 frames = 34 windows x 8.29 s = 282 s, commit ~3 s: **~4 min 45 s**; 10 % loss ~5 min 15 s | channels 128 packets, contacts 11, zones 8, HELLO, END = 149 packets = 25 windows x 8.53 s = 213 s, commit ~3 s: **~3 min 40 s**; 10 % loss ~4 min |

The sender's screen shows its estimate from these formulas with the real record sizes (counted in the preparation
scan, section 11.1), rounded up to 5 s.

Receiver flash: each staged channel is one journaled sector rewrite (~100 ms, `codeplug.h:8`), a P25 channel one more
for its tag. A full codeplug is ~1024 rewrites (~100 s of the housekeeping task) spread over >= 213 s of transfer: the
storage task stays ahead. A batched stage (a frame's channels in one rewrite when their slots are contiguous, which
they are when the free slots are at the end, the usual case) cuts it to ~150 rewrites; phase 2 step 3 measures it on
the host and step 8 on the radio. When the storage task falls behind, the receive queue (3 frames) fills, frames are
not acknowledged and are resent: slower, never wrong.

Rate 3/4 DMR (18 B a block, the trellis done by the HR_C7000, `modem-hrc7000.md` 7) would carry ~45 % more per burst;
not used in v1 (no rate 3/4 data was sent from this chip yet).

## 9. Records (format version 1)

A DATA frame holds whole records of one section, numbered from `first`; a record never spans frames, so any frame can
be decoded alone, in any order. Sections go in this order: contacts, channels, zones (section 10.2 says why).

### 9.1 Channel

| Size | Field |
|---|---|
| 1 | flags A: bits 2-0 mode (0 FM, 1 DMR, 2 P25, 3 YSF, 4 NXDN, 5 dPMR), bit 3 25 kHz, bits 5-4 TX (0 simplex, 1 offset follows, 2 absolute TX frequency follows), bit 6 RX tone follows, bit 7 TX tone follows |
| 1 | flags B: bits 1-0 power class (0 the default, 1 low, 2 mid, 3 high), bit 2 receive only on the sender, bit 3 scan skip, bit 4 no beep, bit 5 no battery saving, bit 6 VOX, bit 7 extras follow |
| 4 | RX frequency, Hz |
| 0/3/4 | TX: offset in 25 Hz units, signed 24 bit (+-209 MHz), or the absolute TX frequency in Hz |
| 0/2 | RX tone; 0/2 TX tone: bits 15-14 type (1 CTCSS, 2 DCS, 3 DCS inverted), bits 13-0 value (CTCSS Hz x 10; DCS the octal code as a number), as zone sharing encodes them |
| 0/2 | extras (when flags B bit 7): bit 0 direct mode (DMO), bit 1 TOT follows, bit 2 SFR follows, bits 6-3 talker alias TX bits, bit 7 reserved; then TOT (1, 15 s units), SFR (3: flag 1 bit 1 and bytes A, B as stored) |
| mode block | DMR: 1 (bits 4-0 colour code 0-15 or 16 = any, bits 6-5 slot 1, 2 or 3 = either, bit 7 listen list "any"), contact 4 (bits 1-0 type 0 none, 1 group, 2 private, 3 all call; ID 24 bit; the contact itself is matched by ID, 10.2). P25: RX NAC 12 bit + squelch 2 bit (2), TX NAC (2: 0 = same as RX, else NAC + 1), talkgroup (2). YSF: DG-ID (1). NXDN: RAN (1). dPMR: colour code (1). FM: none |
| 1 + n | name, 0-16 printable ASCII (others sent as `?`) |

Left out on purpose: the per-channel DMR ID (`channel_t.dmr_id`), the P25 unit ID (`p25_id`): identities; the APRS
configuration index and the fixed location: the sender's own; the RX group list index (9.4); `roam` (read only);
`am` (VFO only).

### 9.2 Contact

ID (3) and type (bits 1-0) with the slot override (bits 3-2: 0 none, 1 TS1, 2 TS2) (1), name (1 + n).

### 9.3 Zone

Zone number in the transfer (1), part (1: the member offset / 80 of this part; a zone whose member list doesn't fit a
frame goes in parts), name (1 + n, in part 0 only), runs (1), then each run: first channel (2, the channel's index in
the transfer) and count (1). A zone of consecutive channels is one run (3 B); 80 scattered channels are 80 runs (240 B)
in two parts.

### 9.4 Not in v1

RX group lists: a DMR channel arrives with no list, or with "any" (hear every talkgroup) when the sender's had it
(`CODEPLUG_LIST_ANY`). Open question 3. Scan lists, privacy, APRS configurations, DTMF contacts: never.

### 9.5 Decoding

The decoder checks every length, count, reference and value (frequency in this radio's bands for receive, tone
ranges, NAC <= 0xFFF and not 0xF7F, colour code <= 16, names printable) and refuses the whole frame on any failure;
such a frame is answered as missing and resent, and after 8 tries the transfer fails. The channel decoded must pass
`codeplug_p25_valid` (`codeplug.h:124`) for the native modes.

## 10. Import and atomicity

### 10.1 Staging, then one commit

The codeplug marks a channel "in use" with a bit in its bank's bitmap, written after the record
(`codeplug_set_channel`, `codeplug.c:1049`: record `:1078`, bit `:1083-1089`), and a zone likewise with the zone
bitmap after the record (`codeplug_set_zone`, `:1134`, `:1176`). A record in a slot whose bit is clear is not a
channel (or zone) at all, and the next write to that slot blanks it first (`codeplug.c:1063-1065`). DOTA uses that:

| Step | When | What is written | Visible? |
|---|---|---|---|
| Stage a channel | each DATA frame of the channel section, in a storage job | the record (and for P25 its RFKV tag, `classic_standard_store`, `classic_standard.inc:75`) into the **k-th free channel slot** for the transfer's channel k; no bitmap bit; the codeplug serial is not moved | no |
| Stage a zone | each zone frame | the zone record into a free zone slot; no zone bitmap bit | no |
| Add a contact | each contact frame | matched by ID and type against the user's contacts (a match keeps the user's name); a missing one written to the lowest free contact slot | yes (unreferenced until commit) |
| **Commit** | after END, all N frames, CRC-32 equal | the channel bank bitmaps (<= 8 rewrites, set together for every staged slot), then the zone bitmap (1 rewrite) for new zones, or the current zone's record with the new channels appended (1 rewrite) | yes, all at once |
| Undo | any failure before the commit, Back, ABORT, timeout | delete the contacts this session added; remove staged P25 tags; staged channel and zone records stay as dead bytes in free slots | nothing left |

"k-th free slot" is computed from the codeplug's own in-use bitmap (`used[]` in `codeplug.c`, `is_used` `:291`),
which staging never changes, so the receiver needs no table of its own and frames can be staged in any order.
Contacts change the contact area as they are added, so the receiver keeps a 128 B snapshot of which contact slots
were free at the session's start: undo deletes exactly the ones that were free then and are used now.

Power cut: before the commit nothing shows (contacts added so far stay as unreferenced contacts); during the commit
(~1-2 s of bitmap rewrites, each journaled by `core/storage`) some banks' channels can show without their zone. Both
are named in the user documentation. The codeplug patch (phase 2 step 3): `codeplug_stage_channel(number, ch)` (the
body of `codeplug_set_channel` without `set_used`, `write_bitmap` and `serial++`), `codeplug_stage_zone(slot, z)`, and
`codeplug_commit(n_channels, zone_mode)` that sets the bits bank by bank, updates `find_max()` and the zone cache, logs
the zone index shift the way `zone_edit` does (the radio core follows moved zone indexes, `zone_edit.h`), and bumps the
serial once. `codeplug_free_channel` (`:1039`) and `zone_edit_new` already pick "the lowest free" the same way.

### 10.2 Rules

| Rule | Behavior |
|---|---|
| Capacity, before anything | from HELLO: free channel slots >= channels; new zone mode: a free zone slot (all zones: as many as zones), and the zone's channels <= 80; add to current zone: its count + channels <= 80; free contacts >= contacts (worst case: none matches). Otherwise refused (6.3), nothing written |
| Receive only | every imported channel is receive only, unless the receiver chose "imported channels may transmit"; then a channel keeps the sender's receive-only bit |
| Band | a channel whose RX frequency this radio can't receive is left out and counted ("2 not in band"); both radios are DM-32s, so this is a guard, not a case |
| Contacts | contacts come first in the stream; a DMR channel record names its contact by ID and type, and staging looks it up (`codeplug_contact_find`, `codeplug.c:1593`, one pass over 24 KB; the last few lookups are cached). A channel frame that arrives before every contact frame is in is not staged and not acknowledged (it is resent later; with no loss this never happens) |
| Power | the class maps to this radio's levels: low the lowest, mid the middle, high the highest, default the codeplug's default (zone sharing's rule) |
| Names | a new zone takes the name the receiver typed; with "all zones", each zone gets the sender's name, " 2" ... " 99" when taken (`zone_copy_name`, `zone_edit.h`) (open question 2) |
| No stored zones | the radio's "All channels" zone is stored as zone 0 first (zone sharing's rule), before staging |
| Duplicates | v1 adds what arrives; a channel identical to one in the zone is added again |

## 11. State machines

### 11.1 Sender

| State | Does | Next |
|---|---|---|
| FORM | the TX screen (12.2) | OK -> checks (callsign / DMR ID, band, APRS frequency, battery, "receiver ready?") -> PREP |
| PREP | radio task: one pass over what is sent: counts, record sizes, frame packing, N, the estimate; reads the codeplug only | done -> TUNE; empty selection -> FORM "Nothing to send" |
| TUNE | radio core takes the radio: the session channel (FM: freq, 12.5 kHz, CTCSS; DMR: freq, CC, slot, DMO), low power; start tones (FM: before key-up) | -> SEND |
| SEND | builds the window's frames (resends first), keys, sends; FM: one transmission, the halves refilled; DMR: 1 or 2 chained transmissions | window sent -> WAIT |
| WAIT | listens for the answer | ACK -> SEND (or END when all acknowledged); HELLO_ACK refused -> FAIL; timeout -> POLL (4x) -> FAIL "No answer from the receiver" |
| END | sends END (POLL) | END_ACK done -> DONE; result 4 -> wait and ask again (5x); other -> FAIL |
| DONE / FAIL | end tones; radio back to the user's channel; result text | Back -> Tools |

### 11.2 Receiver

| State | Does | Next |
|---|---|---|
| FORM | the RX screen (12.4) | Ready -> checks (callsign / DMR ID, zone name, battery, codeplug stored, no storage job busy) -> ARMED |
| ARMED | radio core takes the radio on the session channel; code drawn and shown; listens | HELLO with our code -> CHECK |
| CHECK | compatibility and capacity (6.2, 10.2); contact snapshot | accept -> HELLO_ACK, RECEIVING; refuse -> HELLO_ACK with the code, back to ARMED (shows why) |
| RECEIVING | each valid DATA frame: queued (3 deep), acknowledged; the UI task posts a storage job that stages the queue; answers each POLL / window end with ACK | END -> VERIFY; idle 30 s / ABORT / Back -> UNDO |
| VERIFY | all N frames staged, counts and CRC-32 equal | yes -> COMMIT; no -> END_ACK failure, UNDO |
| COMMIT | storage job: bitmaps; answers repeated END with "writing" | done -> END_ACK result -> DONE; flash failure -> UNDO -> END_ACK failure |
| UNDO | storage job: added contacts deleted, staged tags removed | -> INCOMPLETE |
| DONE / INCOMPLETE | end tones, radio back, "Done: 23 channels, 1 zone added" / "Incomplete: nothing changed" | Back -> Tools |

### 11.3 Radio core block (`radio_dota.inc`)

- Request `RREQ_DOTA` (arg 1 start / 0 stop) with the session's channel parameters left in a small struct, taken like
  `radio_vfo_apply_take` (`RREQ_VFO_APPLY`, `radio_sm.h:165`).
- Start: the user's channel stays where it is in the zone (`zone_pos`, `channel_number`, VFO flag); `sm->view.ch`'s
  frequency, mode, bandwidth, tones, colour code, slot, `rx_only = false`, `dmo` and power are replaced for the
  session; `tune(sm)` (`radio_sm.c:574`). Stop: `reload(sm)` (`:5201`) puts the stored channel or VFO back, as after a
  codeplug change. No copy of `channel_t` is kept.
- While a session runs: the reload on a codeplug serial change waits (`radio_sm.c:6604`: `&& !dota_on()`), and
  `aprs_can_start`, FM caller ID (`:6658`), the pager / ACARS and NXDN listeners, the scan and dual watch, roaming
  probes, the CW beacon and the SOS key's actions are off, as they are during a spectrum sweep (`spectrum_on()`).
- FM keyed phases return early from `radio_sm_poll` like `radio_sm.c:6858`; receive phases run the normal FM path.
- DMR: the call stack and `data_tick` run as on any DMR channel; the block only feeds the engine.

## 12. Screens and texts

English is the source (`TR()` / `TRN()`); Turkish goes into `core/ui/lang/tr.po` (`integration/radiofoundry/lang/tr.po`).
`%` in Turkish comes before the number. Keys follow the editors: Up / Down rows, Left / Right or the knob change a
value, digits type numbers, OK acts, Back goes back.

### 12.1 Tools > DOTA Transfer (`SCR_DOTA`)

| English | Türkçe |
|---|---|
| DOTA Transfer (Tools entry and title) | DOTA Aktarım |
| Role: Send / Receive | Rol: Gönder / Al |
| Method: FM / DMR | Yöntem: FM / DMR |
| Next | İleri |

### 12.2 Send (`SCR_DOTA_TX`)

| English | Türkçe |
|---|---|
| Send over FM / Send over DMR (title) | FM ile gönder / DMR ile gönder |
| What: One channel / Selected channels / A zone / All zones (DOTA Clone) | Ne: Tek kanal / Seçili kanallar / Bir bölge / Tüm bölgeler (DOTA Klon) |
| Channel: <name> | Kanal: <ad> |
| Channels: 12 selected | Kanallar: 12 seçili |
| Zone: <name> | Bölge: <ad> |
| Frequency: 433.500 | Frekans: 433.500 |
| Tone: 88.5 Hz (FM; CTCSS only, never "None") | Ton: 88.5 Hz |
| Colour code: 1 / Slot: 1 / To ID: 3101234 (DMR) | Renk kodu: 1 / Slot: 1 / Alıcı ID: 3101234 |
| Power: Low / Mid / High | Güç: Düşük / Orta / Yüksek |
| Code: ____ | Kod: ____ |
| About 1 min 20 s, 80 channels | Yaklaşık 1 dk 20 sn, 80 kanal |
| Send | Gönder |
| Nothing to send | Gönderilecek bir şey yok |
| Type the receiver's code | Alıcının kodunu girin |
| Can't transmit on this frequency | Bu frekansta gönderim yapılamaz |
| That is an APRS frequency | Bu bir APRS frekansı |
| Not an amateur band. Send anyway? | Amatör bant dışında. Yine de gönderilsin mi? |
| Battery 8 %: the transfer may be cut off. Sure? | Pil %8: aktarım yarıda kesilebilir. Emin misiniz? |
| Is the receiver ready? Sure? | Alıcı hazır mı? Emin misiniz? |
| Set your callsign (the radio name) in qdmr. (existing APRS string) | (existing translation) |
| Set your DMR ID first | Önce DMR ID'nizi girin |

The channel picker (`SCR_DOTA_PICK`): every codeplug channel with a tick; OK ticks / unticks; title "Selected: 12"
("Seçili: 12"). One channel and a zone use the same list without ticks.

### 12.3 Sender's progress and result (`SCR_DOTA_RUN`)

| English | Türkçe |
|---|---|
| Sending to 4271 (title; the code) | Gönderiliyor: 4271 |
| 433.500 FM 88.5 Hz / 433.500 CC1 TS1 ID 3101234 | (same) |
| Sending... / Waiting for answer... / Asking again (2)... | Gönderiliyor... / Yanıt bekleniyor... / Yeniden soruluyor (2)... |
| 35 of 80 channels, resent 2 | 80 kanaldan 35, yeniden 2 |
| Writing on the receiver... | Alıcıda yazılıyor... |
| Stop the transfer? | Aktarım durdurulsun mu? |
| Sent: 80 channels, 1 zone | Gönderildi: 80 kanal, 1 bölge |
| No answer from the receiver | Alıcıdan yanıt yok |
| Refused: other firmware (<string>) | Reddedildi: farklı yazılım (<dizi>) |
| Refused: no room (80 channels, 41 free) | Reddedildi: yer yok (80 kanal, 41 boş) |
| Refused: no free zone | Reddedildi: boş bölge yok |
| Refused: zone full | Reddedildi: bölge dolu |
| Refused: receiver not ready | Reddedildi: alıcı hazır değil |
| Stopped by the receiver | Alıcı durdurdu |
| Stopped: the receiver changed nothing | Durduruldu: alıcıda değişiklik yok |
| Failed: the receiver changed nothing | Başarısız: alıcıda değişiklik yok |

### 12.4 Receive (`SCR_DOTA_RX`)

| English | Türkçe |
|---|---|
| Receive over FM / Receive over DMR (title) | FM ile al / DMR ile al |
| Frequency / Tone / Colour code / Slot | Frekans / Ton / Renk kodu / Slot |
| Import: Add to current zone (<name>) / New zone | İçe aktar: Geçerli bölgeye ekle (<ad>) / Yeni bölge |
| Zone name: <name> (OK opens the text entry, `ui_text_edit`, `ui_nav.h:242`) | Bölge adı: <ad> |
| Imported channels may transmit: Off / On | Aktarılan kanallar gönderebilir: Kapalı / Açık |
| Ready | Hazır |
| Zone name needed | Bölge adı gerekli |
| Battery 8 %: the transfer may be cut off. Sure? | Pil %8: aktarım yarıda kesilebilir. Emin misiniz? |
| Store a codeplug first (no stored codeplug, or another firmware's) | Önce bir kod planı kaydedin |

### 12.5 Receiver's waiting, progress and result (`SCR_DOTA_RUN`)

| English | Türkçe |
|---|---|
| Code 4271 (large) | Kod 4271 |
| Waiting for the sender... | Gönderici bekleniyor... |
| 433.500 FM 88.5 Hz / 433.500 CC1 TS1, my ID 3101234 | 433.500 FM 88.5 Hz / 433.500 CC1 TS1, ID'm 3101234 |
| Refused TA1ABC: not enough room (80 channels, 41 free) | TA1ABC reddedildi: yer yok (80 kanal, 41 boş) |
| Refused TA1ABC: other firmware (<string>) | TA1ABC reddedildi: farklı yazılım (<dizi>) |
| From TA1ABC (title while receiving) | Gönderen: TA1ABC |
| 37 %, 30 of 80 channels | %37, 80 kanaldan 30 |
| Writing... | Yazılıyor... |
| Stop receiving? Nothing will change. | Alma durdurulsun mu? Hiçbir şey değişmeyecek. |
| Done: 23 channels, 1 zone added | Tamam: 23 kanal, 1 bölge eklendi |
| Done: 23 channels added to <zone> | Tamam: 23 kanal <bölge> bölgesine eklendi |
| 4 contacts added, 6 known | 4 kişi eklendi, 6 kişi zaten vardı |
| Incomplete: nothing changed | Tamamlanmadı: hiçbir şey değişmedi |

Form values are RAM only (the last ones used come back until power off); defaults: the current channel's receive
frequency, tone 88.5 Hz, colour code 1, slot 1, power low, import "New zone" with an empty name.

### 12.6 Pairing without typing (owner, 2026-11-20, after the first look on the radio)

"Frequency selection is hard": both roles now start from the same values, so two radios pair with no typing at all.

| Row | Choices | Default |
|---|---|---|
| Frequency (the source) | Default / Current channel / Last used / Manual (Turkish: Varsayılan / Mevcut kanal / Son kullanılan / Elle gir); Left / Right step through the ones that have a frequency now | Default |
| MHz | the source's frequency; digits type one (433500 or 4335125), Left / Right step 12.5 kHz: either makes the source Manual | 433.450 |
| Tone (FM) | a short list: 88.5, 100.0, 123.0, 131.8, 162.2 Hz (no typing) | 88.5 |
| Colour code / slot (DMR) | fixed CC 1, TS 1, on no screen | |
| Receiver ID (DMR sender) | typed; the receiver's waiting page shows its own ID ("ID 3101276") to read out | |

- **Default 433.450 MHz** (`DOTA_DEFAULT_HZ`, `dota.h`): no Turkish band plan is in this repository, so the IARU Region 1
  70 cm plan decides: 433.450 is one of its FM simplex channels (433.400-433.575, 12.5 kHz), not the calling channel
  433.500, no APRS frequency (144.800, 432.500 and the 433.800 some countries use), outside the repeater inputs and
  outputs, below the satellite segment (435-438 MHz). Like every 70 cm frequency below 434.790 it also carries LPD433
  (licence-free) devices: the CTCSS tone keeps them out of the transfer, not off the air.
- **Current channel** is offered only when the channel (or the VFO) is simplex, may transmit and passes the band check
  below.
- **Amateur bands only** (owner, 2026-11-21): every frequency DOTA transmits on goes through the cross-band's band plan
  (`xband_check_hz` in `radio/xband.c`, its toasts in `ui_xband.c`, shared as `ui_xband_refusal`): 144-146 MHz and
  430-440 MHz only, and inside them not the satellite segments (145.800-146.000, 435-438 MHz), the IARU Region 1
  repeater inputs and outputs (145.000-145.1875 and 145.600-145.7875, 431.050-431.825 and 438.650-439.425 MHz, each
  edge widened by half a 12.5 kHz channel), APRS 144.800 MHz, nor a frequency of the repeater list (`CONFIG_REPEATERS`).
  A typed frequency the plan refuses is not taken: the toast names why ("Not an amateur band", "PMR446: no repeaters
  allowed", "Repeater frequency", "Satellite segment", "APRS frequency") and the frequency and its source stay as they
  were. Left / Right do not step out of the amateur bands (the same toast); a step into a refused segment inside them is
  taken and Send / Ready refuses it with its toast, whatever the source. APRS's own frequency (Settings > APRS) stays
  refused too. The old "Not amateur band: send?" question is gone: outside the bands nothing is sent.
- **Last used** is the last session's frequency, until power off (RAM only: a stored value would need a settings
  field; proposed for later if the owner wants it across power off).
- The Receiver ID is typed. Proposal for later (not built): pick it from the last DMR stations heard (Tools > Heard
  list), costing ~0.4 KB.

## 13. Sound

| When | FM | DMR |
|---|---|---|
| Start (sender before its first key-up; receiver at Ready and on an accepted HELLO) | "dit dit diiit": new `BEEP_DOTA_MARK` {1200 Hz 70 ms, gap 60, 1200 Hz 70, gap 60, 1600 Hz 300} | the same |
| Data flowing, receiver | the real AFSK audio: the receiver is tuned with the tone squelch, so the speaker opens on the sender's carrier and tone and plays the tones at the knob's level (the normal FM path; with the APRS listen route the C7000 plays it, `hardware-dm32.md`) | synthetic crackle: new `BEEP_DOTA_DATA` {3000 Hz 4 ms, gap 30, 2200 Hz 3, gap 45, 3500 Hz 5, gap 25}, played again whenever the data burst count moved in the last 150 ms (5.2) |
| Data flowing, sender | the real AFSK audio through the monitor route (4.2 patch 4; **unverified**: if the DAC doesn't reach the speaker while keyed, the sender is silent while keyed) | **silent while keyed**: speaker PCM is gated off during DMR TX (the prompt FIFO feeds the modulator, `modem-hrc7000.md` 8.4); the crackle plays while it waits for the answer |
| Receiver's own answers | its AFSK through the monitor route, as the sender | silent while keyed |
| End | success: `BEEP_DOTA_MARK`; failure: the existing `BEEP_TX_DENY` (300 Hz 400 ms) | the same |

Beeps follow Settings > Sound's beep level and the knob (`beep_play`, `core/radio/beep.h`); during an FM transmission
the prompt FIFO is the AFSK's (`hrc_audio_pcm_hold`), so tones are played before key-up and after unkey only. The two
new patterns are table entries in `core/radio/beep.c:24-60` (BEEP_SEGS_MAX 6 holds both) and enum values before
`BEEP_COUNT` (`beep.h:20-37`); ~120 B.

### 13.1 Sound on the keyed DMR sender: other paths (owner answer 5, phase 2)

Every way this radio makes sound, checked against "audible locally, never on the air, while the DMR engine
transmits". References are to the pinned upstream specs (`docs/spec/`) and the patched Plus tree.

| Path | Evidence | Usable while DMR keyed? |
|---|---|---|
| Prompt / beep FIFO (0x16000980, AUDIO_CONTROL b7): every beep and all speaker PCM today | `modem-hrc7000.md` 8.4: "The prompt path also feeds the TX modulator"; a beep during DMR TX rode on the 4FSK and broke the voice LC headers (measured upstream); `platform/hrc7000/audio_pcm.c:14-17`, `:61` (`tx_gated`) keeps b7 clear and drops queued blocks while `hrc_dmr_tx_active()` | **no**: it would go on air and corrupt our own bursts |
| CPU speaker buffer 0x180000A0 (AUDIO_CONTROL b5), the DMR receive PCM path | `hardware-dm32.md:112`, `:902`; `modem-hrc7000.md` 8.4: "gave rough, choppy beeps on one board with every sample layout tried"; whether it also reaches the modulator while keyed is not documented anywhere | **unknown**, never measured during TX; a candidate for a later bench test with the SDR watching the 4FSK (not built: a wrong guess puts audio on the air) |
| C7000 DTMF / single tone generator (FM_TONE1-4, SUBVOICE_COEF) | `modem-hrc7000.md` 8.3: it is a transmit modulation source (deviation measured on air) | **no**: modulates the carrier |
| FD6818 tone generator 1 (register 0x71, 1750 Hz burst) | `hardware-dm32.md:755` (FD6818 table, section 8.4 of that file): the RF chip's own tone for repeater access | **no**: modulates the carrier |
| Codec digital mixer (CR_MIX / DR_MIX, MIX_2 b0 mix_rec) | `modem-hrc7000.md` 8.4 (last paragraph): it carries the DAC into the ADC record path, "which is the FM transmit input" | **no**: the opposite direction (DAC to transmit) |
| A beeper, buzzer or vibration motor | `hardware-dm32.md:599-605` (6.6): vibration motor unverified, no drive pin known; the pin map (`hardware-dm32.md:440-500`) has no beeper; PWM0 is the LCD backlight (`:241-246`, `:460`) | **none found** |
| The audio amplifier's enable (its switch-on pop) | `boards/dm32/radio_dm32.c` `amp()` (patched tree `:540`): the speaker amplifier is a GPIO; switching it on makes a click that never touches the modulator | **yes**: the only local-only sound found; built as `speaker_click` (`radio_dm32.c:626`, `AMP_DOTA`), a click every 50-140 ms while our DMR transmitter is on (`sounds()`, `dota.c`). Loudness and whether the pop is audible at all: unverified (row D-S2) |

So, plainly: **no clean tone path exists on the DMR sender while it transmits.** What is built is the closest to
continuous sound the hardware allows: the amplifier clicks while keyed (if they turn out inaudible on the radio, the
keyed parts are silent, about 1 s per transmission, estimated), and `BEEP_DOTA_DATA`'s crackle in every gap (while data
comes in, and while the sender waits for the answer), plus the start and end tones. The FM sender keeps its real AFSK
through the amplifier (row D-S1).

## 14. RAM and flash

### 14.1 Where the buffers come from

| Need | Size | From |
|---|---|---|
| AFSK demodulator, receive ring | ~2.4 KB state + 2 KB ring | APRS's own (`a.demod`, `aprs.c:135`) and the heap ring APRS receive takes anyway (`fm_rx_audio.c:45`); FM caller ID gives its ring up as for APRS |
| DMR burst list and packet buffers | 1,728 B + 3 x 612 B | the data service's own (`txBursts`, `txPacket`, `ipPacket`, `dmrDataRxPacket`) |
| DOTA session buffer `dota_buf` | **1,280 B static** | new, paid by step 0 |
| UI copies while a DOTA screen draws | < 200 B | the shared tool screen buffer `ui_scratch` (280 B, `core/ui/ui_scratch.h`): one more union member and owner id, as the other six tool screens |

`dota_buf` layout (one role at a time):

| Sender | Bytes | Receiver | Bytes |
|---|---|---|---|
| channels selected, a bit per codeplug channel | 128 | contact slots free at the start (undo) | 128 |
| contacts used, a bit per contact slot | 128 | frames received, a bit per frame (N <= 512) | 64 |
| frames in flight: number and first record (32 x 4) | 128 | receive queue, 3 frames x 216 | 648 |
| FM: two halves of tone bits; DMR: three UDP packets (3 x 224) | 512 / 672 | answer being built, zone staging scratch | 200 |
| frame being built | 216 | | |
| **largest** | **1,272** | | **1,040** |

The selection is filled by the UI before the session and read by the radio task after the start request (the
request's mailbox orders them).

### 14.2 Budget

Base: FLASH 864,036 B of 901,120 (37,084 B free), static RAM 209,672 B of 211,456 (1,784 B free). Estimates are from
the sizes of comparable Plus objects in the current map (a tool screen with its strings 1.6-2.2 KB, `aprs.o` 10.8 KB,
`zone_edit.o` 1.6 KB, `hrc_afsk.o` 0.7 KB) and the ELF symbol table; nothing was built.

| Part | Flash B | Static RAM B |
|---|---:|---:|
| Step 0: the dead APRS ring of `audio_mux.c` compiled out on boards with their own AFSK receive path (`aprs_ring`, 2,048 B in the ELF) | -150 | -2,048 |
| `dota_buf` | 0 | +1,280 |
| Protocol, session, ARQ (both roles), CRCs | 3,500 | +96 |
| Record codec v1 | 2,000 | 0 |
| Import: staging, commit, undo, capacity; codeplug patch | 2,300 | +16 |
| Radio core block, FM transport, `hrc_afsk` append, `aprs.c` route, monitor | 3,000 | +40 |
| DMR transport, UDP hook, `SendIPChain` linked | 1,500 | +8 |
| Screens (5) and English strings | 6,000 | +56 |
| Turkish strings | 1,500 | 0 |
| Sounds | 120 | 0 |
| **Total** | **~19,800** (range 16-24 K) | **~-550** |
| **After** | ~884 KB, ~17 KB free | ~209,120 B, ~2,330 B free |

Without step 0 the session buffer and state would take ~1,470 of the 1,784 B free: not acceptable. Heap: nothing new
(the FM ring is APRS's own). Stacks: the UI task has 268 B free (the deepest path is the editor's RFKV read), so the
DOTA screens read nothing from the codeplug themselves: the preparation scan and frame building run in the radio task
(760 B free; `codeplug_contact_find` keeps its buffer static for that reason, `codeplug.c:1598`), staging in the
housekeeping task with its state in `dota_buf`. `--stack-check` decides in phase 2.

## 15. Risks: what is untested on this hardware

| # | Risk | Status | Covered by |
|---|---|---|---|
| R1 | APRS AFSK transmit on the owner's unit | measured upstream on a DM-32 on the SDR bench (valid FCS at every level, `hardware-dm32.md`); rows AP* open on this unit | radio rows D-F1, D-F2 |
| R2 | AFSK receive decode on this unit | upstream: "Receive decodes off air"; two-path demodulator 12/12 on a strong flat signal | D-F2 |
| R3 | CTCSS on transmit together with AFSK through ring play | **measured nowhere**: APRS always keys without a tone | host: none possible; D-F3 checks the tone on a second radio / SDR |
| R4 | FD6818 CTCSS detect time, `tone_ok` during full-deviation AFSK (APRS saw the noise reading close the squelch on strong tones, `aprs.md` 5.3) | unknown | 400 ms preamble, `tone_ok` held 1.5 s; D-F3 counts frames refused for no tone |
| R5 | Sender's speaker monitor during AFSK transmit | unverified | D-S1; fallback: silent while keyed |
| R6 | The prompt FIFO append: no underrun between halves | new interrupt code | host test of the fill logic; D-F4 (SDR or the receiver's FCS error counter) |
| R7 | DMR data TX / RX on this unit (SMS untested here); 3 chained packets of 225 B | upstream bench: chained IP decoded (`test_dmr_tx_spacing.py`) | D-D1, D-D2 |
| R8 | DMR turnaround (end-of-call state 200-400 ms) | estimated | D-D2 measures the answer time; the timeout is 3 s |
| R9 | Staging throughput (journaled rewrites) while receiving; journal sector wear (~1,000 rewrites for a full codeplug) | estimated | host count of rewrites; D-R3 full transfer timed |
| R10 | Thermal: ~87 % key time for minutes on FM | unknown | low power default; open question 7 |
| R11 | UI and radio task stacks | 268 / 760 B free today | `--stack-check` every phase 2 step |
| R12 | The owner's codeplug is the open-firmware layout written by the CPS (needed for staging); the built-in set or another firmware's codeplug refuses ("Store a codeplug first") | by design | host test |

## 16. Test plan

### 16.1 Host tests (phase 2; `tests/plus/`, run by `tools/test_dota.py` from `tools/test_classic.py`, ASan/UBSan as the others)

| Test | What |
|---|---|
| `test_dota_proto.c` | frame header codec, CRC-16 and the order-free CRC-32 sum; every type's payload; refusals (version, length, CRC, code, session); the window builder (resends first, 32-frame span); both state machines against each other through a fake transport with **simulated loss** (0, 1, 10, 30 % frames; lost POLL; lost answer; duplicated frames; reordering between windows): every run ends Done with identical data or Failed with nothing committed; counts of windows and resends against section 8's formulas |
| `test_dota_rec.c` | encode / decode round trip of every mode (FM simplex, offset, absolute TX, CTCSS / DCS / DCS inverted, DMR with every contact type, CC any, slot either, P25 with TX NAC same / own, YSF, NXDN, dPMR), names with non-ASCII, the extras; every field left out of section 9.1 is absent; fuzzed frames never crash and are refused whole |
| `test_dota_import.c` | on the simulated DM-32 flash with the real `codeplug.c`: stage in random order, commit, read back equal; undo at every step (no channel, zone or contact left, the user's data byte-identical); a power cut at each flash write (the journal finishes it; nothing visible before the commit); capacity refusals; contact match by ID; P25 tags live after commit and dead after undo; "add to current zone" and "new zone" and "all zones" names |
| `test_dota_fm.c` | AX.25 wrap; the half-buffer encoder against `afsk_mod` + `afsk_demod` end to end (the APRS host modem), with noise at 10-20 dB SNR, **with a CTCSS tone added** at -10 / -20 dB, and with one sample of bit clock slip at each half boundary (R6) |
| `test_dota_dmr.c` | the chained packets' burst counts against `dmr-data.md` 11.3; the UDP hook with the data service's host test harness; answers addressed to `packet->src` |
| `test_dota_radio.inc` in `test_radio` | the real radio state machine on the fake board: the session channel tuned and the user's channel restored exactly (zone, position, VFO); APRS, FM caller ID, scan, reload held off during a session; keyed time per transmission <= the TOT rule; PTT refused; battery critical aborts; **nothing keys before the user's confirmation** |
| `test_ui_dota.c` (upstream's UI simulator, as `tests/plus/test_ui_misc.c`) | every screen opens (`screen=` names), the forms' checks and texts (callsign, band, APRS frequency, battery, zone name, ready), estimate shown, both roles' progress and every result text, Back / stop questions, English and Turkish fit their rows |

### 16.2 Two radios (owner, last; rows for `docs/plus-radio-test.md`)

**Each row transmits: owner approval for the step and an authorised frequency, low power, CTCSS (FM).**

| Row | Test | Pass |
|---|---|---|
| D-F1 | FM, one channel, radios 2 m apart | "Sent: 1 channel" / "Done: 1 channel, 1 zone added"; the channel identical in the editor, receive only |
| D-F2 | FM, a 16-channel zone | done in about 7 s (section 8.2); the zone and channels identical |
| D-F3 | FM with a third radio (or SDR) on the frequency without / with the tone | the third radio's speaker opens only with the tone set; the receiver's frames are refused while the sender's tone is wrong |
| D-F4 | FM, 80 channels | about 22 s; resends shown; no FCS errors at the halves (debug link counter) |
| D-S1 | FM sender's speaker during its windows | the AFSK heard at the knob's level (or note: silent) |
| D-D1 | DMR, one channel to the receiver's ID | as D-F1 |
| D-D2 | DMR, 80 channels | about 18 s; the answer time noted |
| D-R1 | Receiver: Back in the middle, and power off in the middle | "Incomplete: nothing changed"; after power on no new channel, zone or contact |
| D-R2 | Receiver refusals: wrong code (no answer), full codeplug (no room), other firmware | the texts of section 12 |
| D-R3 | DOTA Clone of the owner's full codeplug over DMR, then over FM | about 3 min 40 s / 4 min 45 s; every zone and channel equal (compare in the PC app); battery and case temperature noted |

## 17. The owner's answers (2026-11-20) and where they are built

| # | Question (phase 1) | Answer | Built |
|---|---|---|---|
| 1 | Refuse on different firmware strings, or only on different DOTA versions? | only when the DOTA protocol / record versions differ | no firmware string is exchanged; HELLO is refused with "Other version" when the frame version, the record version or the record layout differ (`rx_hello`, `dota.c`; `DOTA_R_VERSION`) |
| 2 | DOTA Clone: every zone under the sender's name, " 2" when taken? | yes; the RX import choice applies to one channel, selected channels and one zone | `dota_import.c` (`unique_name` with `zone_copy_name`); the receiver's Import / Zone name rows apply to the other kinds |
| 3 | Send DMR RX group lists? | yes, matched like contacts | section `DOTA_SEC_LISTS` before the channels; a list equal by name and members is reused, else added; channels point at the receiver's index (`dota_rec_put_list`, `take_list`) |
| 4 | PTT during a transfer? | deny tone **and** the prompt "Stop the transfer?" (Yes / No); the transfer keeps running; Yes cancels cleanly | the radio core refuses PTT with `BEEP_TX_DENY` and calls `dota_ptt()` (`radio_dota.inc`); the run page asks; No (`dota_ptt_prompt_clear`) goes on, Yes (`dota_stop`) sends one ABORT and ends with nothing changed on the receiver |
| 5 | Silent DMR sender acceptable? | no: look for other sound paths with evidence; if none, crackle / tones in every gap, near-continuous, and say so | section 13.1: no other path found that is safe; built: the amplifier's click while keyed (unverified, D-S2) and the crackle in every gap |
| 6 | Silent FM sender acceptable if the speaker can't play while keyed? | only if the radio test shows it can't: try first | built: the amplifier stays on during AFSK transmit (`afsk_monitor`, `radio_dm32.c`); D-S1 decides |
| 7 | 5 s rest after each minute of FM key time? | yes | `fm_link.rest_after_ms = 60000`, `.rest_ms = 5000` (`dota.c`) |
| 8 | Power choice? | Low / Mid / High, default Low | the Power row (sender); the radio core maps it to the board's lowest / middle / highest level |
| 9 | FM bandwidth? | 12.5 kHz only | the session channel is 12.5 kHz (`radio_dota.inc`) |
| 10 | Receiver's answers carry its callsign? | yes | every frame from either side carries the AX.25 source (FM) / the callsign field (DMR); Ready needs a valid callsign |
| 11 | Wrong pairing code: no answer? | yes | `dota_rx_heard` drops a HELLO with another code without answering |
| 12 | Channel extras sent / never sent | agreed | `dota_rec_put_channel` (section 9.1 flags A / B, extras byte); per-channel DMR ID, P25 unit ID, APRS index and location are not in the record |

## 18. Phase 2: order of work

Each step builds with zero warnings, `forced_rebuild_identical`, `--stack-check`, `make test` clean, and is committed
on its own. Flash / RAM are the estimates of section 14.2.

| Step | Work | Flash | RAM | Tests |
|---|---|---:|---:|---|
| 0 | RAM reclaim: compile out `audio_mux.c`'s APRS ring where the board has its own AFSK receive path (`PLUS_AUDIO_MUX_NO_APRS`, a build error if a board without one sets it); measured | -150 | -2,048 | APRS host tests unchanged |
| 1 | `CONFIG_DOTA`, `tools/plus_dota.py`, `core/dota/dota_proto.c`: frames, CRCs, session, ARQ both roles, fake transport | +3,500 | +1,376 (`dota_buf` + state) | `test_dota_proto.c` with loss |
| 2 | Record codec v1 | +2,000 | 0 | `test_dota_rec.c` |
| 3 | Import: codeplug stage / commit patch, contacts, undo, capacity; batched staging | +2,300 | +16 | `test_dota_import.c` (power cuts) |
| 4 | Radio core block + FM transport: session channel, keying with CTCSS, `hrc_afsk` append, `aprs.c` route, gates, monitor | +3,000 | +40 | `test_dota_fm.c`, `test_dota_radio.inc` |
| 5 | DMR transport: UDP hook, chained sends, answers, burst counter | +1,500 | +8 | `test_dota_dmr.c` |
| 6 | Screens, texts, Turkish, sounds | +7,600 | +56 | `test_ui_dota.c` |
| 7 | `docs/plus.md` section, radio rows D-* in `docs/plus-radio-test.md`, the build measured against this plan | 0 | 0 | |
| 8 | Two-radio tests with the owner (each transmitting step approved separately) | | | rows D-* |

## 19. As built (phase 2, 2026-11-20)

Where the code differs from sections 1-16. Code: `integration/radiofoundry/dota/`, installed by `tools/plus_dota.py`
into `core/radio/` (`dota_proto.c`, `dota_rec.c`, `dota_import.c`, `dota.c`, `radio_dota.inc`), `core/codeplug/`
(`codeplug_dota.inc`) and `core/ui/` (`ui_dota.c`). `CONFIG_DOTA` is on in the Plus build and needs `APRS`, `APRS_RX`,
`APRS_MSG` (the text entry), `DMR` and `ZONE_EDIT` (else forced off for that board).

| Topic | Design | Built |
|---|---|---|
| Layout | `core/dota/`, `dota_fm.c`, `dota_dmr.c` | `core/radio/`; both transports in `dota.c` (one session, one role at a time) |
| What is sent | channels, zones, contacts | four sections in order: contacts, RX group lists (owner answer 3), channels, zones; a barrier between sections |
| Compatibility | firmware strings | protocol version in every frame header; HELLO's manifest (21 B, fixed): record version, record layout, the modes used, the kind, the zone count, records and frames per section. Refusal "Other version" only when versions differ (answer 1). No firmware string on the air |
| HELLO_ACK | free channels, zones, room, firmware string | free channels, free zone slots, room in the current zone |
| Windows | section 7 | FM: 7 frames of up to 128 B payload, at most 1,100 B a window, AX.25 UI to "DOTA", 60-flag preamble (400 ms), each frame in its own half of the tone buffer, appended with `tx_more` (`hrc_afsk.c`); DMR: 6 frames of up to 192 B, up to 3 chained UDP datagrams (port 4101) a transmission, private to the peer's ID. 5 s rest after each keyed minute on FM (answer 7) |
| Endings | Done / Failed | also "No final answer" (sender: every frame acknowledged, the END_ACK never came; the receiver may have written it all) and "No answer" when nothing was ever heard back; the receiver stays 8 s after Done to answer a repeated END |
| Session memory | `dota_buf` 1,280 B + `ui_scratch` | one static union `b` in `dota.c`, 1,676 B (sender: selection, contact / list maps, zone parts, the transport buffers; receiver: protocol state, import state, a 2-frame queue for the storage job, answer buffers). While no session runs, the UI borrows the sender's transport area for its zone and channel copies (`dota_scratch()`) |
| Import | section 10 | channels staged into free slots without their in-use bit (`codeplug_stage_channel`, a twin of `codeplug_set_channel`), zones staged raw into free zone slots; one commit sets the bank bitmaps, then the zone bitmap or the current zone's record; contacts and RX group lists are added visibly and taken away again by the undo (a snapshot of the free slots). Writes run as storage jobs on the start-up task, posted from the UI tick |
| Screens | five screens (`SCR_DOTA`, `_TX`, `_RX`, `_RUN`, `_PICK`) | two: `SCR_DOTA` (the form with Role and Method on top and the role's rows below, its questions, the run page) and `SCR_DOTA_PICK` (`dota-pick`: a channel of a zone, channels ticked in any zone with Left / Right for the zone, a zone). Every page is a `ui_draw_list` list (scrolls, wraps the long texts) |
| Texts | section 12 | shorter, to fit the flash budget: "Sent 12 channels, 1 zone" / "Imported 1 channel" (singular and plural;
the counts sit where the language puts them: "Sent ^" / "^ gönderildi", "Imported ^" / "^ içe aktarıldı"; no zone
count when no zone went), "No answer", "Other version", "Not ready", "Codeplug full", "Receiver stopped", "Nothing changed", "No final answer", "Channel busy", "Battery low. Sure?", "Receiver ready?", "Not amateur band: send?"; reused where the catalog had them ("Color code", "Can't transmit there", " channel(s)", " zone(s)", "Codeplug full", "Back"). 45 new Turkish entries in `integration/radiofoundry/lang/tr.po` |
| Sound | section 13 | as section 13 with 13.1: the DMR sender's amplifier click while keyed; FM sender's monitor through the amplifier (unverified) |
| PTT | refused | refused with the deny tone and the prompt "Stop the transfer?" (answer 4) |
| Power | Low / Mid / High | stored as a class 0-2, mapped by `dota_power_level` to the board's levels |

### 19.1 Measured (`--plus --stack-check --verify-rebuild`)

| | FLASH B | RAM B (free) | Stacks free: UI / radio / start |
|---|---:|---:|---|
| Base (`ddm25` 1c391c7) | 864,036 | 209,672 (1,784) | 268 / 760 / 348 |
| DOTA, steps 0-6 | 888,540 (+24,504) | 209,648 (1,808) | 216 / 760 / 348 |
| DOTA, after the review (plural texts, stack edges) | 888,652 (+24,616) | 209,648 (1,808) | 196 / 760 / 348 |
| Base `ddm25` 3032f91 (with CONFIG_HOME_FREQ, +344) | 864,380 | 209,672 (1,784) | 268 / 760 / 348 |
| DOTA rebased onto 3032f91 | **888,996** (+24,616) | **209,656 (1,800)** | 196 / 760 / 348 |

The limits of the job were FLASH headroom >= 12 KB (<= 888,832 B) and static RAM free >= 1,500 B: both held, with
180 B of flash to spare. On the `ddm25` base with the frequency on Home (+344 B) the owner relaxed the flash rule to
>= 11,500 B free: 901,120 - 888,996 = **12,124 B free**. The estimate of section 14.2 was ~19.8 K (16-24 K); the result is at its top end, and step 6
needed trimming (shorter texts, table-driven results, list pages) to stay inside.

**Stacks** (the check's figures include the HR_C7000 port's 68 B and the 128 B margin; "free" is beyond both):

| Chain | Task (stack) | Deepest | Free |
|---|---|---:|---:|
| The receiver's storage job: `startup_task` > `storage_job_poll` > (pointer) `dota_job` > `dota_imp_commit` > `make_zone` > `zone_edit_create` > `codeplug_set_zone` > `materialize` > `set_channel_as` > ... `codeplug_kv_write` > `write_chain` > `storage_sector_write` > `flash_crc` > ... > the tick interrupt's crash capture | start (2,560 B) | 16 + 1,972 = 1,988 B (`dota_job` alone 1,880 B) | 376 B |
| The start task's deepest chain (boot: `radio_sm_init` > `aprs_init` > ...), unchanged by DOTA | start | 2,016 B | 348 B |
| `dota_poll` (radio task) > `prep_step` > `fill` > `codeplug_channel` > ... | radio (3,072 B) | 1,424 B below `radio_task`'s own frames; the task's deepest chain is not DOTA's (2,116 B) | 760 B |
| `dota_draw` > `ui_draw_list` > a list item (the UI task's deepest chain) | ui (2,016 B) | 1,624 B | 196 B |

The check follows the storage job's pointer from `storage_job_post`'s callers, and `tools/plus_dota.py`'s
`STACK_EDGES` (added to the Plus build's table by `build_radiofoundry.py`) names it as well (`call storage_job_poll
dota_job`), together with the session's transport calls (`dota_io_t.key` / `.beep` are set in `radio_dota.inc`, which
the source scanner does not read: before, the check listed `io->key` as unresolved and skipped `io->beep`). The check
now reports no unresolved DOTA call.

### 19.2 Host tests (`tools/test_dota.py`, from `tools/test_classic.py`)

| Test | What |
|---|---|
| `test_dota_proto.c` | frames, CRCs, manifest; both state machines against each other at 0 / 1 / 10 % loss with duplicates and swaps, and 30 %, FM and DMR links; every run ends done with identical data or failed with nothing committed |
| `test_dota_rec.c` | every mode and field through encoder and decoder, refusals, 200,000 fuzzed records |
| `test_dota_import.c` | the real `codeplug.c` on the simulated flash: staging unseen, power cuts at every flash write during staging and commit, the undo, names, contacts and lists matched |
| `test_dota_session.c` | two whole sessions (`dota.c` twice): FM tone buffers decoded bit by bit by the HDLC decoder, CTCSS required; DMR chained datagrams; zones, DOTA Clone, loss, a wrong code, stops, the PTT prompt |
| `test_dota_fm.c` | FM end to end through audio: each side's tone buffers through `afsk.c`'s modulator at 8 kHz, a sample dropped or repeated at every half boundary, the transmitter's CTCSS at -16 dB and noise on the air in real time (25 ms ticks), heard through `afsk.c`'s demodulator and a CTCSS detector standing in for the FD6818's (a Goertzel at the receiver's tone over 200 ms). A zone of 16 clean, at 20 dB and 12 dB SNR, and a zone of 64 at 12 dB: done, identical, no FCS error. The sender without a tone, and on 100.0 Hz against 88.5: the receiver demodulates the frames and takes none, the sender ends with "No answer", nothing written |
| `test_dota_radio.inc` (`test_radio_dota`, from `tools/test_classic.py`) | the real radio core (`radio_sm.c` with `radio_dota.inc`) and the real session on radioCoreTest's fake board, after upstream's and Classic's radio suites pass with DOTA built in: the session channel (433.500, 12.5 kHz, CTCSS both ways, lowest power; DMR: CC, slot, direct mode); PTT refused with the deny tone and the prompt, never keyed; channel / VFO / zone requests dropped during the session and working after it; the user's channel, and the VFO, back exactly (frequency, tones, mode, power) after "No answer", after the user's stop, after a DMR receiver's wait; every keyed span at most 10 s |
| `test_ui_dota.c` | the screens in upstream's UI simulator (engine faked), English and Turkish: rows per role and method, scrolling, digits, the steps, the checks and questions in order, the request, the picker, progress, Back and the PTT prompt (No goes on, Yes stops), writing can't be stopped, every result text in singular and plural, every page inside the screen |

In `test_dota_fm.c` a zone of 16 takes 12.9 s in real time, not section 8.2's 7 s: the key-ups, the 400 ms
preamble, the turnarounds and the answers cost more than the estimate counted (64 channels: 20.3 s). The radio rows
take the measured figures.

## 20. Content type "data pack" (round 14, branch `datapack`, `CONFIG_DATAPACK`)

A radio sends the RadioID pack or the air band pack in force (`docs/plus.md` "User data packs", `docs/r14.md` 7.6) to
another radio. The session, pairing, code, ARQ, frame CRC-16, the order-free CRC-32 and the receiver's atomic end are
the channels' (sections 6-10); only what travels, the receiver's question and where it is written differ.

| Topic | Built |
|---|---|
| Kind | `DOTA_KIND_PACK` (4) in the manifest; `dota_req_t.pack` names the type (RadioID / air band). The manifest's modes are 0 (a pack uses none) |
| HELLO | the 21-byte manifest **and the pack's 64-byte header** (`dpk_hdr_t`: type, format, sequence, length, payload CRC-32, date, name, tag, header CRC): 85 bytes, inside FM's 128 and DMR's 192. `dota_manifest_t.pack` points at it; `dota_manifest_put` / `_get` take 21 or 85 bytes. The sender's first window holds the HELLO alone (the receiver's user answers first) |
| Older builds | **fail safe, nothing committed**: builds from before this type take a manifest of exactly 21 bytes (`dota_manifest_get`), so they drop the 85-byte HELLO without an answer and the sender ends with "No answer"; a build without `CONFIG_DATAPACK` answers "Other version" (`rx_hello`) |
| Data | the payload in **120-byte chunks**, each a record of the channels section, one a DATA frame (record index = offset / 120, the last one shorter); records = frames = ceil(length / 120). The sender reads them from the pack in force (`dpk_read`) as it fills each frame; nothing is copied to RAM but the header (`txs_t.ph`, 64 B inside the session buffer) |
| Frames | up to `DOTA_FRAMES_MAX` = 2,240 (a 256 KB RadioID slot is 2,185 frames). The receiver no longer keeps a bit per frame (64 B for 512): it keeps a **32-frame window** from its base (4 B, `dota_rx_t.got`), which is all the ACK carries anyway; the sender never has new frames beyond `DOTA_SPAN` of the lowest one not acknowledged |
| The receiver's question | on a pack's HELLO the receiver checks the header (magic, type, format, length within the type's slot, header CRC) and the counts, then answers **"wait"** (`DOTA_T_REFUSE` with status `DOTA_R_WAIT` = 9: not a refusal) and its screen asks **"Data pack: RadioID TR, 2026-11-17, 98 KB. Accept?"** (Turkish "Veri paketi: RadioID TR, 2026-11-17, 98 KB. Kabul?"), OK yes / Back no. The sender takes "wait" as an answer that came: it sends the HELLO again after its answer wait (2.5 s FM, 3.5 s DMR), as when no answer came, at most `polls` (4) times, so the user has **~20 s** (host model with air time: FM 20.0 s, DMR 22.2 s). The next HELLO of the same pack (header equal byte for byte) after **Yes** is accepted (ACK: the transfer runs); after **No** it is refused (status `DOTA_R_DECLINED` = 10). No answer in time: the sender ends with "Not accepted" (status `DOTA_R_WAIT` kept), and the receiver's question goes 10 s after the last HELLO of that pack |
| Writing | as the channels' staging, in storage jobs on the start-up task: **`dpk_begin`** on the first job after Yes (the slot not in force erased, up to 64 sectors of 4 KB for RadioID: frames wait in the 2-frame queue meanwhile and are resent), **`dpk_put`** per frame at its offset (a frame that is not exactly its chunk is never taken), **`dpk_commit`** at END once every frame is in and the transfer CRC-32 matches: the payload read back against the header's CRC-32, then the header programmed with the receiver's next sequence; the new pack is in force at once (no restart). Undo: nothing to take back |
| Failures | any failure, a stop (either side), the link going silent (the receiver's 30 s idle), a power cut: **the old pack stays in force** (the header of the slot being written is never programmed; the next boot's scan ignores it) |
| Sender's screen | What: "RadioID pack" / "Air band pack" after the channel kinds; the row under it names the pack in force ("RadioID TR", "2026-11-17, 98 KB", the date or the size alone where the row is short), greyed "none" without one (Send: "Nothing to send"); the estimate from its length at the steady rates of section 8 |
| Progress, results | "49/98 KB"; "Sent data pack" / "veri paketi gönderildi", "Imported data pack" / "veri paketi içe aktarıldı", "Not accepted" / "Kabul edilmedi" (No, or no answer); the other results as for channels |
| Time | the steady rates of section 8 (FM ~93 B/s, DMR ~124 B/s): an air band pack of a few KB well under a minute; a RadioID pack of ~100 KB ~15-20 min on FM, ~11-14 min on DMR (`docs/r14.md` 7.6; the PC path is the better one for it) |

**Fixed on the way** (all kinds): a DMR receiver's refusal went to DMR ID 0. The receiver forgot the asking station
(`peer_dmr`) as soon as it built the refusal, before the data service sent it; it now forgets it once the answer went
(`finish_send`).

**Cost** (public build, `--stack-check --verify-rebuild`): FLASH +2,296 B (estimate 2.5-3.5 KB in `docs/r14.md` 7.7),
static RAM -24 B (the receiver's bitmap went; the pack's state sits in the session buffer, which shrank from 1,676 to
1,640 B); the UI task's stack 184 B free (was 200: the run page's text buffer holds the question).

**Tests** (`tools/test_dota.py`): `test_dota_proto.c` (the 85-byte manifest and its refusals, the 21-byte rule older
builds apply, 629 frames over FM with 10 % loss through the receiver's window, a pack's HELLO alone, "wait" answered
twice then Yes, never answered: given up after ~20 s, No: refused); `test_dota_pack.c` (two whole sessions with
`CONFIG_DATAPACK` on the real pack store over a simulated 16 MiB data chip: an air band and a RadioID pack over FM and
DMR, with loss, a 64 KB pack of 547 frames over DMR; No; no answer; the sender's stop, a silent link, power cuts at
four points of the receiver's writing: the old pack in force byte for byte each time; a receiver built without data
packs: "Other version"); `test_ui_dota.c` (the What choice and the pack row, greyed "none", "Nothing to send", the
request, "49/98 KB", the question with Yes and No, the results, English and Turkish, every page inside the screen).
Radio rows DP6-DP10 in `docs/plus-radio-test.md`.

**The map pack (firmware 1.1, `docs/mappack-design.md` E.4).** The same content type carries type 3 unchanged: What
gains "Map pack" ("Harita paketi") after the two others (`WHATS` = 4 + `DPK_TYPES`, row `3 + rq.pack`), greyed "none"
without one. ~50.7 KB in 120-byte chunks: ~423 frames, FM ~9 min, DMR ~7 min, plus 13 sector erases (the PC path is
the better one). A receiver checks the HELLO's header first: a map pack whose `fmt` is not 1 or whose non-zero
`flags` version hint is not this firmware's is refused at once ("Other version" to the sender) and the receiver shows
"Map pack for another version" ("Başka sürümün harita paketi"), nothing erased; `flags` 0 (an older writer) is no
hint. At END `dpk_commit` checks the pack's structure after the CRC and before the header (`ref_validate`): one that
does not check is never committed and the receiver ends with "Bad pack" ("Bozuk paket"), the old pack in force. A
1.0.x receiver refuses type 3 (`h.type > DPK_TYPES`) without writing; a 1.1 receiver refuses a 1.0.x RadioID pack over
196,544 B (`dpk_hdr_ok`'s length) without writing. Radio row MP11 in `docs/plus-radio-test.md`.
