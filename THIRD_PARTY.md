# Third-party source notices

Preserve every original per-file notice and the supplied full license texts.
Build dependency source remains available at its pinned upstream location.

Project code, the P25 CAI and the selected full-rate IMBE codec use MIT notices.
The former GPL IMBE implementation, wrapper and DSP patch recipe are replaced.
Dependencies retain the terms listed below. Independent GPL/LGPL test references
are not linked into the application firmware.

| Component | Source / pin | Terms and use |
| --- | --- | --- |
| RadioFoundry | https://github.com/RadioFoundry/RadioFoundry — `dfbaf20c400c73e2ccf6edb80af7d1d255bee418` | MIT originals; full notice in `LICENSES/RadioFoundry-MIT.txt` covers source patch context, with upstream notices also retained in the generated derivative. |
| FreeRTOS-Kernel | RadioFoundry submodule — `3a22924e0a9ddbbc8b0758881c33b3422a5cc20d` | MIT, original notices retained. |
| Full-rate IMBE | Shared Open25 fixed-point C core in `third_party/imbe`; exact revision/files in `sources.lock.json` | MIT for the independently expressed code. Numeric standard datasets and generated constants are identified in its source provenance. |
| mbelib | `sources.lock.json` | ISC; notice in `LICENSES/mbelib-ISC.txt`. Independent host reference, not the embedded codec. |
| P25 CAI | `third_party/p25cai`, revision in `sources.lock.json` | MIT; full notice in `third_party/p25cai/LICENSE`. Conventional caller bindings in `src/p25/p25.c` and `p25.h` use the same pinned source. |
| OP25 | `sources.lock.json` | GPL/LGPL host regression references; original notices retained with the downloaded dependency. |
| newlib | `tools/bootstrap_newlib.sh` | Pinned source hash; original per-file terms and upstream COPYING.NEWLIB apply. |
| GNU binutils, GCC and libgcc | `sources.lock.json`, `tools/bootstrap_toolchain.sh` | Original GPL/LGPL terms and applicable GCC Runtime Library Exception apply. |

Proprietary AMBE material is supplied by the user locally. No stock firmware,
bootloader/helper image, radio dump, calibration or key material is distributed
in this source tree.

The DM32 codec-table SRAM limit is backported from RadioFoundry commit
`88283f6ce3aab4c5ff5c9995600de2acfa2c6f7b` (MIT), extending the exclusive
end from `0x18003E28` to `0x18003E38` for NRFB .050. The generated board
registry and package metadata derive this value from the linker symbol.

The DM32 RF setup B tables, lock-based setup selection and associated board
settings are selectively ported from RadioFoundry `9e1f7209` (MIT). The pinned
base remains unchanged. The port preserves the existing FM-to-digital tone
reset and adds regression tests for both RF setups and failed lock detection.

DDM32 Plus ports three later RadioFoundry fixes (MIT, The RadioFoundry Authors)
as transforms in `tools/plus_rfports.py` (`CONFIG_RF_FIXES`): `625b921` (FD6818
sub-audio off on entering DMR, Spencer Fowler), `916ac26` (HR_C7000 lost modem
requests, task-side glue in flash, Spencer Fowler) and `947c139` (polite access
and receive recovery by wall time, Joshua Fuller and Spencer Fowler), with their
host tests adapted to the pin.

The channel-mode registry, RFKV store, tag/record readers and P25 parameter
codec are selected from RadioFoundry `9e1f7209` in `third_party/rf_codeplug`.
Its `UPSTREAM.json` records the original files. Local extensions implement the
specified continuation chain and preserve explicit TX NAC presence; MIT
copyright notices remain with the imported source.

## DDM32 Plus additions

DDM32 Plus code is GPL-3.0-or-later (`LICENSES/GPL-3.0.txt`, `LICENSING.md`);
a Plus firmware image is distributed under GPL-3.0-or-later. The sources and
data below are used by its own files and generators only, never by `--classic`.

| Component | Source / pin | Terms and use |
| --- | --- | --- |
| DM32-nxdn-p25 | `src/nxdn` file headers (ported files named there) | GPL-3.0-or-later; NXDN48 receive (demodulator, call layer, FEC) ported into `src/nxdn`. |
| MMDVMHost (G4KLX) | named in `src/nxdn`, `src/ysf` headers; host cross-checks `tools/test_*_ref.sh` | GPL-2.0-or-later, used under GPL-3.0-or-later: the NXDN coding the DM32-nxdn-p25 port follows; YSF / P25 LC reference implementations for host cross-checks only (compiled from where they are, never copied into this tree or the firmware). |
| dsdcc DSDdPMR (F4EXB) | named in `src/dpmr/dpmr.c` | GPL-3.0: the dPMR scrambler's register order; the framing follows ETSI TS 102 490 / TS 102 658. |
| Natural Earth 1:10m Admin 1 (5.1.1) | `tools/turkey_map.py` (pinned SHA-256) | Public domain; province outlines of the Tools > Map screen. |
| GeoNames `TR.zip` (2026-11-19) | `tools/turkey_districts.py` (pinned SHA-256) | CC BY 4.0, "District data: GeoNames"; 973 district points of the map. |
| OurAirports `airports.csv`, `airport-frequencies.csv` (2026-11-19) | `tools/airfields.py` (pinned SHA-256) | Unlicense (public domain dedication), "Airfield data: OurAirports"; the Turkish airfields and frequencies of Tools > Airfields. |
| RadioID.net user list and repeater list (`user.csv`, `rptrs.json`) | `tools/iddb.py`, `tools/rpt_list.py`; downloads kept in the git-ignored `research-data/` | Data © RadioID.net, credited on the radio ("Repeater data: RadioID.net"); the DMR ID database and the DMR repeaters of Tools > Repeaters. Never committed. |
| TRAC (Telsiz ve Radyo Amatörleri Cemiyeti) repeater list | `tools/rpt_list.py` (`research-data/repeaters.csv`, the owner's copy of TRAC's published list) | Credited on the radio ("Repeater data: TRAC"); TRAC states no licence: the list is never committed, and an image that carries it is for the owner's radio until TRAC's permission is clear. |
| CelesTrak amateur satellite elements | `tools/sat_tle.py`, `integration/radiofoundry/sat/tle.txt` | Public orbital element sets (NORAD / CelesTrak), Tools > Satellites. |

## The Windows program (`win/`)

The Windows program (C#, .NET 10, WPF) is GPL-3.0-or-later. Its published single-file `DDM32Plus.exe` carries the
.NET runtime (MIT, Microsoft) and these NuGet packages:

| Component | Version | Terms and use |
| --- | --- | --- |
| System.IO.Ports, System.Management | 10.0.x (Microsoft) | MIT; the serial port and Windows' device list (CH340 cable check). |
| PdfPig (UglyToad) | 0.1.16 | Apache-2.0 (https://github.com/UglyToad/PdfPig); reads the text of the AIP pages the user chooses for an air band data pack. |

Data packs are built on the user's PC from files the user downloads (RadioID.net's `user.csv`, the State's AIP
pages); nothing of them is in this repository, a release or the program.
