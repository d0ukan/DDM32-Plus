# P25 soft probe (branch `swprobe`, `CONFIG_SEARCH_SOFTP25`)

Status 2026-11-24: implemented behind a switch that is **off by default**; **host-tested only** (synthesized signals, a
fake ring). Nothing of it has run on the radio. The firmware build was not run on this branch yet (the WSL disk failed
during the work, see "Budget"). OpenDMR (va3bpf) gave the idea only; its licence forbids commercial use and none of its
code was read for this or used. The design below is ours.

## 1. How P25 search spends its time today

Read from `integration/radiofoundry/spectrum/p25search.c`, `searchsure.h`, `searchhy.c`, `native/native_lab.c`
(`search_take`, `search_service`) and `docs/plus.md` ("P25 search", "Search reliability", "Faster searches"):

| Stage | Cost | Bound by |
|---|---|---|
| Quiet step | one radio poll, 25 ms (fast and classic since SEARCH_FAST) | the poll period and the FD6818 reading |
| Strong step | a settled re-read (50 ms), the run followed step by step, its middle read again (ST_AIM) | the run rules |
| Probe hand-over | the native HR_C7000 P25 receiver takes the frequency (`p25_lab_set_channel` NAC F7E): the worker stops / starts, the chip is set to P25 | not measured on the radio |
| Probe window | up to **1.2 s** (`P25S_PROBE_MS`, SURE) for a frame with a NID; the chip needs a sync and loses 112 symbols after it (docs/p25.md 1.1) | the native receiver's lock time, unmeasured; SYNC_MAX in the PC stats will tell |
| Give back | the worker lets go, the radio core tunes the next step | |
| Not P25 | skipped 10 s (30 s, fast: 2 min for a steady carrier on a later pass) | |

So a quiet band is swept at 40 steps a second, and **every carrier that is not P25** (FM repeaters, simplex FM, DMR,
beacons) costs about 1.2 s of native window plus two hand-overs before the sweep moves on. In a busy band that is where
the time goes: a pass with 10 FM carriers spends more than 12 s probing them. A real P25 carrier costs the native lock
time, which the soft probe cannot shorten (the native receiver still plays the call).

## 2. What the soft probe does

While a hit waits for its probe, the board stays in FM on the hit's frequency. The receive audio is switched to the
flat discriminator path (`board_nxdn_flat`, the NXDN receiver's register recipe: FD6818 AF filters off, HR_C7000 FM
band-pass off) and the HR_C7000's 8 kHz FM RX RAM ring is started (`board.afsk` rx_start / rx_read, the path APRS, FM
caller ID and NXDN use). The first 30 ms of samples are dropped; the rest goes to `p25soft.c` for up to 450 ms.

`p25soft.c` (no RTOS, no hardware, integer only):

1. **Frame sync.** The 24 sync symbols (0x5575F5FF77FF, only +3 / -3) span 40 samples at 8 kHz (5/3 samples a symbol).
   Every sample, the last 40 samples are correlated with a template: the sync as a C4FM discriminator should show it,
   the 24 symbols through the C4FM frequency pulse (raised cosine 0.2 times x / sin x) sampled at 8 kHz, computed off
   line ("Template" below). The normalised correlation coefficient r (offset and level removed: any frequency error,
   any gain) is the measure; its sign is the polarity. A clean sync reads r^2 about 0.88; a sync needs r^2 >= 0.70.
2. **Timing.** The correlation peak is followed sample by sample and refined between samples by a parabola through the
   three r^2 around it: the sync's start to a fraction of a sample.
3. **Levels.** The 24 sync symbols are read at their middles (a 4-point cubic interpolation) and fitted, least squares,
   to +-1: the outer level (with the polarity) and the offset.
4. **NID.** The 33 symbols after the sync (32 NID dibits and the status symbol at 35, dropped) are read the same way,
   sliced at 0 and +-2/3 of the outer level, and decoded by `p25cai_nid_decode` (BCH(63,16) and the parity bit, already
   in flash for the native framer). **At most 6 bits corrected = P25**, with its NAC and DUID. 7 to 11 corrected = a
   hint. A random 64-bit word lands within 6 bits of a codeword about 3 times in 10^7, and it must follow a sync.

The radio core (`radio_p25soft.c`) then decides:

| Seen in the window | Verdict | What follows |
|---|---|---|
| a NID with <= 6 bits corrected | **P25** | the found list gets the frequency and NAC at once (`p25s_probe_found(nac, 0, 0)`); the ring and flat path are given back; the native receiver takes the frequency exactly as before, plays the call and adds the talkgroup and source to the same entry. The probe is never skipped afterwards, even if the native receiver then heard nothing |
| a NID with 7-11 bits corrected | unsure | the native receiver probes as before |
| nothing, carrier >= 25 dB over the floor, >= 400 ms of samples read | **not P25** | given back as a probe that heard nothing (`p25s_probe_done(false)`): skipped 10 s / 30 s / 2 min as classic and fast already do; a carrier that went off the air meanwhile is not skipped (SURE's rule, unchanged) |
| a carrier < 25 dB over the floor, no heap, the flat path or ring would not start, too few samples | unsure | the native receiver probes as before |

The native receiver never takes the frequency while the soft probe runs (`native_lab.c`: `!p25s_soft_pending()`).

## 3. The switch

On Tools > P25 search, **held 0** now cycles **fast -> fast + soft probe -> classic -> fast** (toast "Search: fast",
"Search: fast + soft probe", "Search: classic (beta.8)"; Turkish "Arama: hızlı", "Arama: hızlı + yazılım sonda",
"Arama: klasik (beta.8)"). The DMR and all digital search screens keep fast <-> classic. The soft probe is **off at
power on** (in RAM, not stored); it applies only with the fast algorithm and is latched when a search starts, like the
algorithm. Off, nothing of it runs: `p25s_soft_pending()` is never true, `radio_p25soft_poll` and the release in
`tune()` return at once, `p25s_probe_done` sees `soft_p25` false. The engine's classic and fast paths are the code they
were, statement for statement.

Not done here: the all digital search's P25 probe (`dmrsearch.c` order P25 > DMR > YSF > NXDN) still uses the native
receiver. It can take the same probe later; the radio core's NXDN probe already uses the same ring and flat path.

## 4. Budget

| | |
|---|---|
| FLASH | `p25soft.c` (correlator, interpolation, fit, slicer; the 40-byte template) and `radio_p25soft.c` and the engine's hooks: estimated 2-3 KB. The NID decoder is already linked. Baseline `--plus` on this branch's base (fa379fa): 874,664 B of 901,120 (26,456 free) |
| RAM, static | about 16 B (the probe's pointer and start time, the engine's four flags and a level) |
| RAM, heap | 456 B (`p25soft_t` 320 B: the 128-sample ring 256 B; and a 64-sample read buffer) from the RTOS heap for the window only, freed before the verdict; the NXDN receiver takes about 830 B the same way and never runs at the same time (NXDN listens only on an NXDN channel, `nxdn && !p25s_on()`). No heap: the native receiver probes as before |
| RAM floor | baseline 204,784 B of 211,456 (6,672 B free); the floor of 1,500 B free is far away |
| CPU | per sample: one 40-tap correlation (40 multiply-adds), the running sums, and a 64-bit division only where r^2 > 1/8; per sync 57 cubic interpolations and one BCH decode. About 0.6 M cycles a second, under 0.5 % of the 168 MHz CK803S (estimate, not measured), for 450 ms per probe |
| Stack | the radio task: a few locals (the buffers are on the heap) |

The firmware build (`--plus --stack-check --verify-rebuild`) and its FLASH / RAM figures are still to be run: the WSL
virtual disk went read-only and then failed to start when drive C: ran out of space during this job. The baseline
above was built before that.

## 5. Host tests

`tests/plus/test_p25soft.c` (the reader) and `tests/plus/test_p25soft_radio.c` (radio side with the real engine, a
fake ring, flat path and heap), both in `tools/test_classic.py`; the signal model is `tests/plus/p25soft_model.inc`:
symbols through the C4FM pulse as FM at 48 kHz complex baseband, a frequency error, white noise at a CNR in 12.5 kHz,
a +-6 kHz channel filter, a phase-step discriminator, a 3.6 kHz anti-alias filter and every sixth sample to 8 kHz int16,
at random gains and either polarity. Results (MinGW gcc, Windows host; the same sources as the clang/ASan run):

| Case | Result |
|---|---|
| P25 (HDU, LDU1/2, TDU) NACs 293, 659, 1C8, F7E, 001, ABC; entry at a random point; +-500 Hz error; gain 0.3-10 int16 per Hz; both polarities; 384 probes at 30 dB | 384 found, NAC always right; mean 104 ms, worst 201 ms of samples |
| CNR 25 / 15 / 10 / 8 / 6 / 5 / 4 / 3 dB, 96 probes each | found 96 / 96 / 96 / 96 / 95 / 74 / 43 / 19; neither found nor hinted 0 / 0 / 0 / 0 / 0 / 2 / 20 / 41 (the 25 dB trust rule sends weak carriers to the native receiver) |
| 30 s each of: unmodulated carrier, noise only, near-silent noise, FM speech with CTCSS, NXDN48, DMR-like 4800 4FSK with DMR sync, C4FM random dibits; 66 windows of 450 ms each | **P25 0 in every case**; hinted (sent to the native receiver) 0 in this run (4 % for the DMR-like and random C4FM cases with another random sequence) |
| Radio side: off / classic | the native receiver takes at once; no ring, no heap, no flat path |
| Radio side: P25 on a strong carrier, 20 entries | handed to the native receiver after 157 ms on average, 225 ms at worst; NAC listed first, talkgroup and source added to the same entry; ring stopped, flat path off, heap freed before the take; not skipped when the native receiver then hears nothing |
| Radio side: FM speech on a strong carrier | not P25 after 475 ms (450 ms window, a poll), counted NOSYNC, skipped on the next pass; native receiver never took it |
| Radio side: weak carrier (15 dB), no heap, flat path fails, a ring that brings nothing | the native receiver probes |
| Radio side: search stopped or channel tuned midway | ring, flat path and heap given back |

What the model does not know (so the figures are optimistic): the HR_C7000's own filtering and level in the FM RX RAM,
its sample timing, FD6818 AGC and click noise at low levels, real transmitters' deviation and filter, multipath.

## 6. What the owner gains, if the radio confirms it

- A carrier that is not P25 costs about 0.5 s instead of 1.2 s plus two hand-overs (the native receiver never starts).
  On a pass with many FM carriers that is most of the probing time.
- A P25 carrier shows its frequency and NAC in the found list about 0.1-0.2 s after the probe begins, before the native
  receiver locks; the call then plays as before (the native receiver's lock time is unchanged, about 0.2 s later than
  classic because the soft probe runs first).
- A P25 carrier whose NID the soft probe read is never skipped because the native receiver missed its lock.

## 7. Risks

| Risk | Effect | Guard / what to measure |
|---|---|---|
| The FM RX RAM is not the discriminator as modelled (filtering, de-emphasis left on, AGC, a sample rate other than 8 kHz, clipping) | the sync correlation stays under 0.70: no P25 found by soft; on a strong carrier "not P25" and a skip of a real P25 frequency | the trust rule (25 dB) and the minimum sample count; **radio test SP-1/SP-2 first**; NXDN's own "what the radio must confirm" list covers the same path |
| A real P25 carrier judged "not P25" | skipped 10 s (30 s / 2 min on repeats), as a failed native probe is today | only on carriers 25 dB over the floor with nothing P25-like in 400 ms of samples (two LDU syncs); the model finds 96/96 down to 8 dB CNR |
| False P25 | a non-P25 carrier listed with a made-up NAC and probed by the native receiver | NID within 6 bits after a sync r >= 0.84; 0 in 3 minutes of six kinds of non-P25 signal in the model |
| The ring is shared (APRS, FM caller ID, Pager / SSTV, NXDN) | a conflict if another owner runs during the search | all of them are off while `p25s_on()` (`plus_features.py` gates `fmid`, `msg`, `nxdn`, `dpmr` on `!p25s_on()`; the search does not start while APRS transmits). APRS receive on an APRS-frequency FM channel during a search is not checked on the radio |
| The flat path left on | FM audio without de-emphasis / filters after the search | the path is restored before every verdict, on every poll with no soft probe pending, and at the start of `tune()` (before the board leaves FM: `board_nxdn_flat(false)` writes the FM filter word) |
| The heap | 456 B for 0.45 s; a failure falls back to the native receiver | none needed |
| Hidden cost on a P25 find | the native receiver starts 0.1-0.45 s later than in fast mode | measured by SYNC_LAST / SYNC_MAX in the PC stats (from the hit) |
| The PC stats | a soft "not P25" counts as NOSYNC, a soft P25 as FOUND (the X.st line keeps its form; C# / Rust unchanged) | none |

## Template

The template `tmpl[]` in `p25soft.c` (x 64): sample i of the 40-sample window is at 3i/5 symbols; its value is
sum over the 24 sync symbols p_k (+-1) of g(3i/5 - (k + 1/2)), where g is the C4FM frequency pulse (the inverse
transform of the raised cosine 0.2 times x / sin x, x = pi f / 4800, up to 2880 Hz), normalised so held symbols reach
+-1. The symbols around the sync are unknown and left out. A few lines of Python (any numeric integration) reproduce it;
the sums are -178 and 196,804 (squares).

## Radio test (to be run only with the owner's approval for each step)

- SP-1: a P25 radio (NAC 659, TG 101) keyed near the DM-32; Tools > P25 search over +-500 kHz, held 0 twice ("Search:
  fast + soft probe"). Expect the entry "NAC 659" within a second of the hit, the call playing as before. Read the PC
  search log `p25s.st` (PROBE_MS, SYNC_LAST).
- SP-2: the same with an FM radio (simplex, speech) in the range: the FM carrier is passed in about 0.5 s (PROBES climb
  with NOSYNC, PROBE_MS per probe about 0.5 s instead of 1.2 s); no false P25 entry.
- SP-3: the P25 radio at low power / farther away (a level 10-25 dB over the floor): found as in fast (the native
  receiver decides).
- SP-4: leave the screen during a probe, PTT during a probe: FM audio normal afterwards on an FM channel (flat path off).
