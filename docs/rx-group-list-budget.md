# P25 RX group list: survey, budget and verdict (branch `rxgl`, 2026-11-24)

Request: a P25 "RX group list" (the set of talkgroups whose audio the radio plays on a channel), editable on the radio
or through the programmer / codeplug as far as the code allows.

## 1. What exists today (survey)

| Area | Where | What it does |
|---|---|---|
| P25 talkgroup filter | `src/p25/voice.c` (admission in `p25_voice_rx_dibit`) | one talkgroup: `group_filter` 0 = every group, else that TG or All Call (0xFFFF); unit-to-unit only for `own_unit` (P25 extras) |
| Channel to worker | `native/native_lab.c` channel take -> `p25_lab_set_channel(rx, tx, tg, id, selective)` -> `p25_lab_channel_rx` (`group = selective ? tg : 0`) | Selective squelch = the channel's TG, Normal = every group |
| P25 channel storage | Classic r13 RFKV tag (`codeplug/classic_standard.inc`) on a DMR base record (mode 1) | squelch, RX/TX NAC, explicit TG (flag bit 0); without the flag the TG comes from the contact or the first TG of the RX group list (upstream CPS); an explicit TG cleared `listen_list` |
| DMR RX group lists | upstream `codeplug.h`: 76 lists x 32 contact indices (`listen_list_t`), 1024 contacts (`contact_t`, 24-bit IDs, group / private / all call); `channel_t.listen_list` in the record's byte 0x2B, Plus "Any" in the colour code byte's bit 0x20 (`CONFIG_DMR_ANY`) | edited by the CPS and by the DOTA import on the radio (`dota_import.c` calls `codeplug_set_listen_list` / `codeplug_set_contact`); upstream has no on-radio list or contact editor |
| Channel editor | `ui_menus.c` rows; `CE_LIST` ("RX list") is a DMR-only row; the picker `SCR_CHPICK` arg 1 shows None, Any, the 76 lists by name | P25 rows: TG, RX NAC, TX NAC, ID. There is no squelch row (Normal / Selective comes from the CPS; a channel made P25 on the radio is Selective) |
| PC programmers | `win/`, `rust/`, `pc/` | read-only codeplug viewers (they show the RX list column); the codeplug is written by the upstream web CPS |

Conclusion of the survey: the DMR RX group lists and their contacts can be reused as they are. A P25 talkgroup is a
group contact with an ID of 1-65535 (the r13 decode already rejects group contacts above 65535 as a P25 TG). The P25
channel record already has the list byte; only Classic's "explicit TG clears the list" rule stood in the way.

## 2. Design (implemented, part A)

- **Persisted**: one value per channel, `channel_t.listen_list`, in the channel record's own RX group byte (0x2B), and
  "Any" in the colour code byte's 0x20 bit, exactly as a DMR channel stores it. Written only by the channel editor's
  existing save of the channel (the same write every P25 channel edit makes today). No new storage area, no new
  writer, nothing in the data pack region, no calibration or settings byte. The lists and contacts are only read.
- **Codec** (`classic_standard.inc`, `#if CONFIG_P25_RXGL`): the talkgroup stays explicit (RFKV flag bit 0) whenever
  there is no TX contact, and the list survives decoding. A Classic / upstream reader of a Plus record sees the
  explicit TG and drops the list (its old rule): no change for it.
- **Filter rule** (`p25_rxgl_selective`): P25 channel with a list = filter on (TG + list), with Any = every group,
  with None = the channel's squelch as before. A channel TG of "Any" (0) still hears every group.
- **Load** (`p25_rxgl_load`, at each channel take, radio task): the list's group contacts with IDs 1-65535 in list
  order (private, All Call, DMR TGs > 65535 and empty slots skipped), at most 32, handed to the worker
  (`p25_lab_set_groups`, copied; cleared by every `p25_lab_set_channel`, so P25 search, trunk following and YSF never
  see a channel's list).
- **Admission** (`voice.c group_wanted`): the call's TG is the filter's, All Call, or one of the list's.
- **UI**: the channel editor shows the "RX list" row on P25 channels (not in the VFO, which has no lists); the picker
  is the existing one. Picking None / Any on a P25 channel says "None: the channel's squelch" / "Any: every TG";
  Any shows "All TGs" (no slot). Typing the TG keeps the list (it removes only a TX contact).

UI sketch (channel editor, P25 channel):

```
 Mode        P25
 RX freq     438.50000
 ...
 RX list     Local P25      <- OK: picker  None / Any: all TGs / 1 Local P25 / 2 ... (0-9 find)
 Talkgroup   101
 RX NAC      659 ($293)
 TX NAC      Same as RX
 Radio ID    Default
```

## 3. Budget

Base: `ddm25` fa379fa, `--plus`, real research data: FLASH 885,400 B of 901,120 B (15,720 B free), RAM 204,784 B of
211,456 B (6,672 B free; floor 1,500 B free).

| Part | Flash (estimate) | RAM (estimate) |
|---|---|---|
| admission loop (`voice.c`) | ~40 B | 8 B in `p25_voice` (pointer + count) |
| worker list + `p25_lab_set_groups` | ~80 B | 65 B (32 TGs + count) |
| loader + rule (`p25_rxgl.c`) | ~250 B | ~84 B static `listen_list_t`; 64 B + 24 B on the radio task's stack at a take |
| editor row, toasts, codec rule | ~150 B (+ 2 strings x 2 languages ~100 B) | 0 |
| **Total** | **~0.6 KB** (limit for this job 8 KB) | **~160 B static** (limit 1 KB) |

Measured: see section 6.

Run-time cost: one list read and up to 32 contact reads (24 B each) from the data chip at each P25 channel take with
a list (a few ms; multi-watch and scan take channels often, the read is short and only for channels with a list).

## 4. Part B, on-radio editing of the lists themselves: not implemented (needs the owner's decision)

Editing a list's members on the radio (add a TG by number, remove, rename, new list) means writing the codeplug's
lists and contacts (`codeplug_set_listen_list`, `codeplug_set_contact`, a free contact slot found by a scan of up to
1024 records, 24 KB). This job's brief forbids touching codeplug storage, so it stops here. Options:

1. Use the firmware's existing codeplug writers from a new "RX group lists" editor (the DOTA import already writes
   lists and contacts on the radio through them). Estimate 3-5 KB flash, ~150 B RAM. Fits the budget, but it is a
   new codeplug-writing screen: owner approval needed.
2. A P25 list pack in the data pack region 0xE70000-0xEFFFFF (CRC, atomic header, 4 KB erase per save). The owner's
   2026-11-20 decision names the RadioID database and air band data only: also an owner decision, and more code
   (~1.5-2 KB on top of 1) for a second copy of what the codeplug already holds. Not recommended.

Until then the lists are edited where they are today: the web CPS (codeplug) or a DOTA pack; the radio picks one per
P25 channel.

## 5. Verdict

Part A fits clearly (well under 8 KB flash and 1 KB RAM, no new storage) and is implemented with host tests:
`tests/test_voice.c` (admission with a list, built with `CONFIG_P25_RXGL=1` by `tools/test_voice.py`),
`tests/plus/test_p25rxgl.c` (real codeplug over a simulated flash: list / Any / None and TG persist over a save and
a reboot, the record byte, the filter rule, the talkgroups taken, the worker hand-over faked; run by
`tools/test_p25rxgl.py` from `tools/test_classic.py`). Not tested on the radio. Part B waits for the owner.
Built and host-tested in WSL (section 6); still not tested on the radio.

## 6. Measured

2026-11-24, WSL, `--plus --stack-check --verify-rebuild`, real research data:

| Tree | FLASH (of 901,120 B) | RAM (of 211,456 B) | Notes |
|---|---|---|---|
| ddm25 fa379fa (base) | 885,400 B | 204,784 B (6,672 free) | |
| rxgl on fa379fa | 885,936 B (+536) | 204,944 B (+160; 6,512 free) | 0 warnings after the verifier's brace fix, forced rebuild identical |
| rxgl merged with ddm25 5a611ed (swprobe) | 888,488 B | 204,960 B (6,496 free) | 0 warnings, forced rebuild identical |

`tools/test_voice.py` (with and without `CONFIG_P25_RXGL`) and `tools/test_classic.py` (with `tools/test_p25rxgl.py`)
pass on both trees.
