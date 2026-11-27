<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Licensing of DDM32 Plus

DDM32 Plus is built on the upstream Open25-DM32 Classic source (r13), which is
itself built on RadioFoundry. Three kinds of files live in this tree:

| Files | Licence | Notice |
|---|---|---|
| Upstream Open25-DM32 files (Classic r13: `tools/build_radiofoundry.py`, `tools/classic_*.py`, `src/p25/*` from upstream, `integration/radiofoundry/*` from upstream, upstream tests, ...) | MIT | `LICENSES/Open25-MIT.txt` (upstream's notice, unchanged); the SPDX header in each file |
| Vendored libraries (`third_party/p25cai`, `third_party/imbe`, `third_party/rf_codeplug`) and RadioFoundry (patched at build time) | MIT | their own `LICENSE` files, `LICENSES/RadioFoundry-MIT.txt` |
| DDM32 Plus additions: the files we created (`tools/plus_*.py`, `tools/turkish_ui.py`, the other generators and checkers we added, `integration/radiofoundry/<our folders>/*`, `src/ysf`, `src/nxdn`, `src/dpmr`, `src/p25/p25_lcx.*`, `tests/plus/*`, our `docs/*`, `art/*`) | GPL-3.0-or-later | `LICENSES/GPL-3.0.txt`; the SPDX header in each file |

The root `LICENSE` holds the full GPL-3.0 text, the licence of the DDM32 Plus
additions and of the firmware image. Upstream's MIT notice is kept unchanged in
`LICENSES/Open25-MIT.txt`.

Rules we follow:

- New DDM32 Plus code goes into our own GPL-3.0-or-later files, not into
  upstream MIT files.

- An upstream MIT file keeps its MIT header and stays MIT, also where we
  changed it. Most of our changes to upstream code are build-time transforms
  in `tools/plus_features.py` (GPL-3.0-or-later): the transform code is ours,
  the patched RadioFoundry/Open25 file in the build tree keeps its MIT notice.
- A few upstream MIT files carry DDM32 Plus code directly, inside
  `#if CONFIG_...` blocks: `integration/radiofoundry/p25_lab.c`/`.h`,
  `integration/radiofoundry/native/native_lab.c`/`.h`,
  `integration/radiofoundry/native/hrc_native_port.c`/`.h`,
  `src/p25/voice.c`/`.h`, `src/modem/native_test.h`,
  `tests/classic/test_radio.inc`, `tools/test_classic.py` and
  `tools/build_radiofoundry.py`. Those files keep
  one licence, MIT, so these in-file additions are offered under the MIT terms
  of the file they sit in. New code that needs to stay GPL goes into a file of
  its own.
- Each of our own files carries `SPDX-License-Identifier: GPL-3.0-or-later`.

## The firmware binary

The DDM32 Plus firmware image combines MIT code with GPL-3.0-or-later code, so
the combined binary is distributed under **GPL-3.0-or-later**. Whoever passes
the binary on must offer the complete corresponding source of this tree
(including the RadioFoundry pin and the vendored libraries named in
`sources.lock.json`) under the GPL. The MIT notices of the upstream parts
(`LICENSES/Open25-MIT.txt`, `LICENSES/RadioFoundry-MIT.txt`, the vendored `LICENSE` files) are
kept with the source and must stay with any copy.

The upstream Classic build (`--classic`) contains no DDM32 Plus code and keeps
upstream's MIT terms.

Data and third-party material used by the Plus generators (Natural Earth,
GeoNames, OurAirports, RadioID.net, TRAC) are listed with their terms in
`THIRD_PARTY.md`.
