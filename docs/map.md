<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# DM32 Plus offline Turkey map (Tools > Map)

Status (2026-11-17): data generator, renderer core and host tests done; the screen (`ui_map.c`) is a **sketch, not
compiled yet** and nothing is wired into the build. Nothing has run on the radio.

Update (2026-11-19): wired into the Plus build (`CONFIG_MAP`, `prepare_map`), and extended with the **973 districts**
(ilçe) as points from GeoNames (section "Districts" below), two closer zoom levels (7 in all), non-overlapping
labels, and the built-in **repeaters** (`CONFIG_REPEATERS`) as triangles with a pick of the nearest. Host-tested;
not yet on the radio (checks MD1..MD8 in `plus-radio-test.md`).

Update (2026-11-19, round 3): the **APRS stations** heard (0 picks, details, a message to one), the **auto
repeater** (Tools > Auto repeater: a banner offers a clearly nearer repeater, # tunes) and our **track** (dotted,
"Back to start") (section "APRS stations, auto repeater, track" below). Host-tested; radio checks MN1..MN12.

## Data source and licence

| Item | Value |
|---|---|
| Dataset | Natural Earth 1:10m Cultural Vectors, *Admin 1 – States, Provinces* |
| URL | <https://naciscdn.org/naturalearth/10m/cultural/ne_10m_admin_1_states_provinces.zip> |
| Version | 5.1.1 (`ne_10m_admin_1_states_provinces.VERSION.txt` in the archive) |
| SHA-256 of the zip | `efc59726337323058f9446210adc96673179cd344e053666ee3d28cb58ba2b05` (14,909,524 bytes) |
| Licence | Public domain ("All versions of Natural Earth raster + vector map data found on this website are in the public domain", naturalearthdata.com/about/terms-of-use). No attribution required; we credit it anyway in the generated files. Compatible with embedding in GPL-3.0 firmware. |
| Selection | records with `adm0_a3 == "TUR"`, `iso_3166_2` `TR-01` .. `TR-81` (the plate number): exactly 81 |

Deliberately **not** used: GADM (non-commercial licence, no redistribution), OpenStreetMap-derived boundaries
(ODbL share-alike obligations on the database; not GPL-compatible for embedding without legal review), amateur
repeater lists (licence unclear). Repeaters, if wanted, should come from the user's own codeplug or waypoints, which
the screen already draws on top of the outlines.

The province names come from a table in `tools/turkey_map.py` (official names, facts, by plate number); the
generator cross-checks them against Natural Earth's `name_tr` and notes the two historic spellings it overrides
(`Hakkâri` -> `Hakkari`, `Elâzığ` -> `Elazığ`).

The archive itself is **not** committed. Keep it outside the repo; the generator refuses any other file (hash pin).

### Districts (ilçe): GeoNames

| Item | Value |
|---|---|
| Dataset | GeoNames country dump for Turkey, `TR.txt` in `TR.zip`; records of feature class `A`, code `ADM2` |
| URL | <https://download.geonames.org/export/dump/TR.zip> (archive readme: <https://download.geonames.org/export/dump/readme.txt>) |
| Version | the dump of 2026-11-19 (Last-Modified 2026-11-19 02:51 GMT; GeoNames rebuilds it daily, so the pin is that day's file) |
| SHA-256 of the zip | `2df01e05aa64b678be9ae0733efe35cca36a407b249dd4432f0915085a442848` (3,700,109 bytes) |
| Licence | **CC BY 4.0** ("This work is licensed under a Creative Commons Attribution 4.0 License", readme.txt). Attribution: **"District data: GeoNames (CC BY 4.0)"**: in the generated files (`TURKEY_DISTRICT_ATTRIBUTION`), here, and on the radio ("GeoNames" in the map's corner whenever district points are drawn). CC BY 4.0 allows redistribution and adaptation (also commercial) with attribution; compatible with embedding in GPL-3.0 firmware (the data stays CC BY; credit it when publishing a build). |
| Kept at | `research-data/geonames/TR.zip` (git-ignored; `research-data/` is in `.gitignore`) |
| Selection | 974 ADM2 records; one dropped (`8694443` "Doski Aşireti Bölgesi", Hakkari: a tribal area, not a district): **973** |

District boundaries are **not** included: the only candidate, geoBoundaries `TUR` `ADM2` (gbOpen, 2023 build), is
"OpenStreetMap, OSM Boundaries" under ODbL 1.0 (its metadata, `boundaryLicense`), the same reason OSM is out above;
GADM is non-commercial. So districts are points (the GeoNames coordinate: the district seat or the middle of the
district) with their names.

`tools/turkey_districts.py` (`python -I tools/turkey_districts.py --zip research-data/geonames/TR.zip [--check]
[--list] [--charset iso-8859-9|utf-8|ascii]`, default ISO-8859-9 like the committed province table):

- GeoNames' admin1 codes are **not** plate numbers (Ankara is `68`, Adana `81`, Gaziantep `83`); they are mapped to
  the plates by the ADM1 record's name against the 81 official names in `turkey_map.py` (all 81 must match once).
- Names: the suffixes " İlçesi" / " District" / " Merkez" are removed; a central district named after its province
  takes the province's official spelling ("Kilis Merkez", "Bingol", "Elazig" -> Kilis, Bingöl, Elazığ: 27 such);
  corrections to the official names: Ulukışla, Yeşilhisar, İncesu, Beypazarı, Yahyalı, Selçuklu (ASCII-only in
  GeoNames), Muradiye ("Muradiye / Berkri"), Şemdinli ("Şemdinni"), Şebinkarahisar ("Şebin Karahisar"), 19 Mayıs
  ("Ondokuzmayıs"); â î û folded to a i u (Kâhta, Kâğıthane, Lâlapaşa, Lâpseki, Şarkîkaraağaç -> Kahta, Kağıthane,
  Lalapaşa, Lapseki, Şarkikaraağaç: the fonts have no glyph for them; the same rule as Hakkari / Elazığ). Every name
  must match `[0-9A-Za-zÇĞİÖŞÜçğıöşü' .-]`, at most 20 characters, unique in its province.
- Counts: every province must have exactly its official number of districts (a table in the generator, 973 in
  all; Hakkari 5 since Derecik in 2018). The dump matched all 81 after dropping the tribal area, so a future dump
  that adds, splits or drops a district fails loudly instead of drifting.
- Positions within Turkey's box, quantised to the province table's units (0.001°, same origin).
- Size caps: the district table alone ≤ 20 KB; provinces (read from `turkey_map_data.h`) plus districts
  ≤ **60 KB** (hard cap; the build data is 32.3 KB).
- `--list` prints plate, name, latitude, longitude and the GeoNames id of every district for review.

Format (`turkey_district_data.h`): `turkey_district_pos[973]` (`int16` x, y units), `turkey_district_first[82]`
(the districts of province p are `first[p] .. first[p+1]-1`, sorted by name), `turkey_district_name_first[81]`
(the offset of the province's first name) and the NUL-separated names. No per-district name offset: a name is found
by walking at most 38 names from its province's first; the renderer walks them in order anyway. 973 × 4 + 82 × 2 +
81 × 2 + 7,896 = **12,114 B**.

Checked in the host test: 15 of the 973 points fall outside their own province on Natural Earth's simplified
outlines (with 3 km slack): border towns (Hamamözü, Süloğlu, Alacakaya, Han, İnönü, Eynesil, Tuzla, Derinkuyu,
Kaynarca, Gemerek, Bahçesaray, Sarıyahşi, Altınova) and two islands Natural Earth drops (Adalar, Bozcaada); every
one is within 12 km of its province. The test fails if any is farther or if more than 20 are off.

## Files

| File | What |
|---|---|
| `tools/turkey_map.py` | generator: zip -> `turkey_map_data.c/.h`; parses zip/dBASE/shapefile itself (no GDAL), pinned SHA-256, deterministic, `--check` mode |
| `integration/radiofoundry/map/turkey_map_data.c/.h` | generated table (do not edit) |
| `tools/turkey_districts.py` | generator: GeoNames `TR.zip` -> `turkey_district_data.c/.h` (districts), pinned SHA-256, official counts, `--check`, `--list` |
| `integration/radiofoundry/map/turkey_district_data.c/.h` | generated district table (do not edit) |
| `integration/radiofoundry/map/map.c/.h` | portable renderer core: projection, zoom/pan, clipping, line callback, province lookup |
| `integration/radiofoundry/map/ui_map.c` | Tools > Map screen (built in the Plus build) |
| `tests/plus/test_map.c` | host tests |

Regenerate (Windows or WSL, from `firmware/`):

```sh
python -I tools/turkey_map.py --zip /path/to/ne_10m_admin_1_states_provinces.zip            # writes the table
python -I tools/turkey_map.py --zip /path/to/ne_10m_admin_1_states_provinces.zip --check    # verifies it
python -I tools/turkey_districts.py --zip research-data/geonames/TR.zip                      # districts
python -I tools/turkey_districts.py --zip research-data/geonames/TR.zip --check              # verifies them
```

Options: `--tolerance` (Douglas-Peucker, degrees, default 0.002), `--min-island` (drop islands smaller than this,
deg², default 0.01; the largest part of each province is always kept), `--charset utf-8|iso-8859-9|ascii`.

### Format

- Units: 0.001° east/north of the origin 25.0° E, 35.5° N (`int16`).
- Simplification is topology-aware: every vertex records which provinces use it; the points where that set changes
  (junctions of three provinces, where a border meets the coast) are fixed, and each chain between two junctions is
  simplified once (canonical direction, cached), so neighbours get **identical** borders with no slivers or gaps.
  Islands are anchored at two far-apart vertices.
- Each ring: an `int16` start point + `int8 (dx, dy)` steps; edges longer than 127 units are split into several steps.
  The last step returns to the start (closed). Rings are stored counter-clockwise.
- Per province: first ring, ring count, plate, name offset, label point (inside the outline: centroid, or the middle
  of the widest inside run on the centroid's row), bounding box.
- Names: NUL-separated in one `const char[]`. Default UTF-8 as requested; **note**: the radio's fonts draw one byte a
  character and put the six non-Latin-1 Turkish letters in ISO-8859-9 slots only in a `CONFIG_LANG_TR` build
  (`tools/turkish_ui.py`). So for the radio, regenerate with `--charset iso-8859-9` (with `CONFIG_LANG_TR=y`) or
  `--charset ascii` (without). `TURKEY_MAP_CHARSET` in the header says which one is built in; the tests adapt.

## Sizes

Measured with `csky-elf-gcc 14.3.0 -mcpu=ck803s -Os` (objects only, not linked):

| Part | Flash | RAM |
|---|---|---|
| `turkey_map_data.o` (81 provinces, 85 rings, 8,715 steps, 659 B of names) | 20,227 B | 0 |
| `map.o` | 2,981 B | 0 |
| `ui_map.c` (estimate, not compiled) | ~2.5 KB | ~32 B static (`map_view_t` 24 B + 4 flags) |

Plus libgcc's 64-bit multiply/divide helpers if not already linked (used only by `map_province_near`). Total well
under the 40 KB target; the generator fails above 40 KB. Tolerance 0.006° gives 15.9 KB, 0.01° gives 14.0 KB; the
source is already coarse (6,577 distinct vertices for Turkey), so 0.002° keeps nearly all of it.

With the districts (2026-11-19, linked Plus build): FLASH 755,052 -> 769,440 B (+14,388: district table 12,114,
renderer and screen ~2.2 KB), static RAM 208,024 -> 208,024 B (the 2-byte repeater pick fits in existing padding);
the UI task's deepest stack path is unchanged (368 B free; the label grid `map_occ_t`, 56 B, is on the stack of
`draw_names`, which is not on that path). Map data in all: 20,185 + 12,114 = 32,299 B of the 60 KB cap.

Stack: `map_draw` and the lookup use a few dozen bytes; no recursion, no heap, no float (CK803S has no FPU).

## Renderer core (`map.h`)

- Equirectangular projection with cos(latitude) of the view centre (Q15 table 30–50°, interpolated), Q16 fixed point.
- Zoom 0..8: 0.1, 0.05, 0.025, 0.0125, 0.00625, 0.003125, 0.0015625, 0.00075, 0.000375 °/pixel (latitude). On a
  160 px wide screen zoom 0 shows all of Turkey (~1,780 km across), zoom 4 ~110 km (province level), zoom 6 ~28 km
  (a few districts), zoom 8 ~7 km (a town, ~40 m a pixel); twice that on the DM-32UV's 320 px. From zoom 6 on,
  `pixels x k` exceeds int32, so the projection multiplies in 64 bits. Zoom 7 and 8 (owner's request 2026-11-20)
  are finer than the data's 0.001° unit: borders and district dots stay on that grid, but `map_project` /
  `map_unproject` work in 1e-7° so stations, repeaters and our position don't snap to it.
- `map_view_pan` (pixels), `map_view_zoom` (keeps the centre), the centre is clamped to the data bounds ± 2°;
  far positions are clamped before multiplying, so nothing overflows.
- `map_draw` culls provinces by bounding box, merges consecutive points on the same pixel (zoom 0: ~4,000 segments
  instead of 8,715), clips each segment (Cohen–Sutherland) and calls `fn(ctx, x0, y0, x1, y1, province)`; the
  caller draws (the UI uses Bresenham + `gfx_pixel`, the test a framebuffer).
- `map_unproject` uses ceil division so `project(unproject(pixel)) == pixel` exactly.
- `map_province_at`: even-odd point in polygon in 0.0001° integers (int32 safe because steps are ≤ 127 units).
  `map_province_near(.., max_m)`: else the nearest border within `max_m` (≤ 50 km), for fixes on a simplified shore.
- Districts: `map_districts_draw(v, fn, ctx)` hands out each district centre inside the view with its name
  (provinces whose box is off the view are skipped whole, names are walked in order); `map_district_name`,
  `map_district_province` (binary search over `first`), `map_district_pos`, `map_district_near(lat, lon, province,
  max_m)` (the nearest centre, equirectangular, optionally only in one province; the screen names it under the map
  as "34 İstanbul / Kadıköy": the nearest district **centre** in the province we are in, within 30 km, not a
  boundary test: near a district border it can name the neighbour).
- Labels: `map_occ_t` splits the view into 16 x 24 cells (a bit each, 48 B); `map_occ_take` takes the cells a label's
  box touches or refuses it when one is taken, so labels never overlap; the first drawn wins.

## Screen (`ui_map.c`)

- Borders as before; province names from zoom 2 (in the foreground colour once district names show), district
  dots from zoom 3, district names (tiny font, right of the dot) from zoom 4; provinces' labels are placed first.
  "GeoNames" in the bottom right corner whenever district dots are drawn (CC BY 4.0 attribution).
- Repeaters (`CONFIG_REPEATERS`, the existing `rpt_table`: no copy of the data): a filled triangle in the theme's
  `priv` blue at each entry with a position, at every zoom; APRS stations are 5 px squares in `rx` green. Both sit on
  a one-pixel ring of the background so they stand out from the 2 px dim district dots and the border lines
  (owner's request 2026-11-20). # picks the repeater nearest the map's centre (`rpt_here_set` with the
  centre, `rpt_nearest`), # again the next nearest (`rpt_next`, round to the nearest after the last): drawn larger in
  the foreground colour with its callsign; the line under the map reads "TA1ABC 439.1625 12.3 km NE" (the way from
  the centre), the hint "OK details  # next  Back clear". OK opens Tools > Repeaters on that entry's details
  (`ui_rpt_open`: `ui_push(SCR_REPEATERS, i + 1)`, read back in `rpt_enter` from the frame's arg, so no RAM), where
  OK tunes the VFO as from the list and Back returns to the map; Back on the map drops the pick.
- The map's centre (what # and 0 measure from) is marked with a small dim cross whenever the view does not follow the
  GPS.
- APRS stations, the track and the auto repeater: next section.
- Static RAM: `map_view_t`, five flags, the 2-byte repeater pick and the 2-byte station pick.

## APRS stations, auto repeater, track (2026-11-19; owner's round 3 items 7-9)

`prepare_mapnav` (`tools/plus_features.py`), switches `CONFIG_MAP_TRACK` (needs MAP) and `CONFIG_AUTO_RPT` (needs
REPEATERS and GPS); the station part rides on `CONFIG_MAP`. Code: `integration/radiofoundry/map/mapnav.{c,h}`
(portable, integer only), `ui_mapnav.c` (the UI task's tick and the Tools entry), `ui_map.c`.

### APRS stations on the map

- **No copy**: upstream's APRS engine already keeps the stations heard (`aprs_station_t`, `APRS_STATIONS` = 12, most
  recent first: callsign, position, symbol, course / speed, a 24-character comment, heard time). The map reads it
  through `ui_svc_aprs_station`, as Go-To does. The only change to the engine: the entry keeps the **altitude** the
  parser already decodes (`/A=` in the comment, compressed, Mic-E) as an `int16_t alt_m` in the struct's tail padding
  (`sizeof` stays 64: no RAM; `INT16_MIN` none; a DMR caller's position none). Transform hunks in `aprs.h`
  (`station_update`, `aprs_dmr_heard`).
- Drawn: a 3 x 3 dot (receive colour) per station with a position, the callsign beside it from zoom 3 (the label grid:
  provinces first, then stations, then districts).
- **0** picks the station nearest the map's centre, 0 again the next nearest, after the farthest the nearest again
  (`mapnav_station_next`: the order is squared equirectangular distance, then the callsign hash, then the list index:
  walked, never stored). The pick is a 16-bit hash of the callsign (the list reorders as stations are heard): two
  callsigns with the same hash (about 1 in 600 for 12 stations) would be one stop of the walk. A station that leaves
  the list drops the pick. The picked one is a 5 x 5 square in the foreground colour with its callsign.
- Under the map (the whole line, the scale is left out while a pick is shown): "TA1ABC-9 2.8 km NE 3m ago" (the way
  from the centre; "3m" where "ago" does not fit). **OK**: the details "/> 376 m Hello mobile" (symbol table and code,
  altitude when known, the comment without its `/A=` and doubled spaces; cut at the end). **OK again**: the APRS
  message composer (`CONFIG_APRS_MSG`) with To filled in, on top of the map (`ui_aprsmsg_compose_to`; Back returns
  to the map). Without APRS_MSG, OK again closes the details. **Back**: details off, then the pick off, then the map.
- **#** switches to the repeater pick (as before) and **0** back to stations: one pick at a time.
- Hints: "# repeater  0 APRS  ^v<> pan  knob zoom  OK follow" (groups that do not fit are dropped from the right);
  "OK details  0 next  # repeater  Back clear"; "OK message  0 next  Back".

### Auto repeater

- Tools > **Auto repeater** (tool 27, after Repeaters; Battery, last, is tool 41 since Tools > ISS): On / Off, OK toggles
  ("Auto repeater on", "... on: waiting for GPS" without a fix, "Auto repeater off"). RAM only: off at every power on.
- With a GPS fix (`ui_svc_position`), every UI tick at most once a second the tick runs; the check itself at most every
  30 s (`ARPT_EVERY_MS`): the nearest **suitable** repeater of the built-in table (`rpt_nearest_if`, one pass): FM on an
  FM (or P25 / other non-DMR) channel, DMR on a DMR channel; with a position; RX and TX in the radio's bands
  (`board_in_band`); C4FM, D-STAR, P25, NXDN and M17 entries never.
- **Hysteresis**: the first suitable one after turning it on is offered at once ("Nearest: ..."). After that another is
  offered only when it is at least 20 % nearer (`ARPT_GAIN_PCT`) **and** more than 2 km nearer (`ARPT_GAIN_M`) than
  the one offered before ("Nearer: ..."); when the channel's mode changes, the old one no longer counts and the new
  mode's nearest is offered. Never the same one twice in a row; one the radio is already on (its RX frequency) becomes
  the current one without an offer. Standing still or driving between two repeaters gives no repeated offers (host
  test: 2 minutes at one place, the halfway point, back and forth).
- **The offer** is a banner over any screen (a new banner kind, `UI_BANNER_RPT`, upstream's banner box: no focus
  change, 8 s, its seconds counting down): "Nearer: SIVRITEPE 145.700 12.3 km NE" / "# tunes the VFO" / "Back hide".
  **#** (the banner key, as for messages) tunes it through Tools > Repeaters' own tune (`ui_rpt_tune` -> `tune`: the
  same refusals and toast; requests `radio_req_vfo_set`, `radio_req_mode`, then FM bandwidth and tones or DMR colour
  code and slot, then `radio_req_vfo_tx`) and goes Home. Back hides it. No offer while another banner is up or while
  transmitting: it waits for the next tick. Turning it off removes an offer still up.
- It **never tunes without the key and never transmits**.
- RAM: 8 B state + 2 B (the banner's repeater) + 4 B (the tick's last poll).

### Track

- While GPS has a fix the tick keeps the last **48** points (`TRACK_POINTS`) in a ring: the first fix at once, then a
  point when 30 s have passed since the last one **and** we are 50 m from where it was taken (exact positions, so GPS
  jitter while standing adds nothing) **and** in another 0.001 degree cell. Points are `int16` offsets of 0.001 degree
  from the map data's origin (the map format, about 110 m x 85 m); outside the int16 range (about +-32 degrees around
  25 E 35.5 N) nothing is kept. 48 points: at least 24 minutes, at 50 km/h about 20 km.
- The first point since the last clear is also kept exactly (`start_lat/lon`): it stays when the ring drops it.
- Drawn as a dotted line (every second pixel, the TX colour) from the oldest to the newest, clipped to the view
  (`track_draw` hands out clipped segments like `map_draw`), under the GPS cross.
- **9**: "Back to start": the first point saved as the waypoint **START** (once; `nav_rename`) and Tools > Go-To opened
  on it (arrow, distance, ETA; Back returns to the map). **7** says "Hold 7 to clear the track"; **7 held** clears it
  (the next fix starts a new track). RAM only.
- RAM: the ring 192 B + 23 B.

### Sizes and tests

FLASH 777,924 -> 783,100 B (+5,176), static RAM 208,072 -> 208,304 B (+232: track 216, auto repeater 14, map 2).
UI task stack unchanged (368 B free; the tick's frames are noinline and shallow, the map's station loops moved out of
`map_draw_screen`). Host tests: `tests/plus/test_mapnav.c` (integer cos / distance against double precision; station
order, walk, wrap, twins, a station gone; the comment; the track's 30 s / 50 m / cell rules, jitter on a cell edge, the
wrap and its order, the start kept, out of range, ms wrap; the track drawn into a framebuffer through the map renderer:
inside the viewport, clipped, dotted; the auto repeater over a synthetic table: suitability, first offer, pacing, no
repeat, 20 % without 2 km, 2 km without 20 %, no flip-flop back, mode change, already tuned, no fix, no bands, off, ms
wrap; mutation-checked: removing either hysteresis term or the 50 m rule fails it), `tests/plus/test_aprs_map.c`
(upstream's APRS radio suite again with CONFIG_MAP, then the altitude through the engine's receive path and the pick
over the engine's own list), `tests/plus/test_ui_mapnav.c` (16 UI flows in the simulator on the DM-32UV's display:
the Tools row, on / off, the offer and # with the exact request sequence for DMR and FM, Back hides, no spam over 5
minutes, moving, waiting for another banner and for TX; 0 / OK / OK / Back / # on the map, a canned message sent to the
station; the track, START once, 7 / 7 held). Radio checks MN1-MN12 in `plus-radio-test.md`.

## Host tests

```sh
# in WSL, from firmware/ (config.h stub: a directory with a config.h that defines nothing but CONFIG_MAP 0)
gcc -std=gnu99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -fno-omit-frame-pointer -DCONFIG_MAP=1 -I<stub> -Iintegration/radiofoundry/map \
    tests/plus/test_map.c integration/radiofoundry/map/map.c integration/radiofoundry/map/turkey_map_data.c \
    integration/radiofoundry/map/turkey_district_data.c \
    -o /tmp/test_map && /tmp/test_map [out.pgm]
```

Passes with gcc and clang, ASan + UBSan, zero warnings, for all three `--charset` variants. Covers: 81 provinces in
plate order, closed CCW rings inside each bounding box and inside Turkey, contiguous indices, UTF-8 names, every
label point inside its own province, ≤ 40 KB; Ankara, İstanbul (both sides), İzmir, Van, Trabzon, Antalya, Erzurum,
Gaziantep, Konya, Edirne in the right province; Athens / Black Sea / the far side of the world outside; the
near-shore fallback (Samsun); exact pixel round trips at every zoom; zoom/pan/clamping; eight clipping cases;
rendering into a 160×128 framebuffer (whole Turkey: every province drawn, nothing outside the viewport; Istanbul
zoom 3; open sea draws nothing; an offset viewport). With an argument it writes the zoom 0 render as a PGM.

## Wiring it in (for the main session; nothing below is applied)

1. `firmware/.gitignore` (allowlist) — add:
   ```
   !/docs/map.md
   !/tools/turkey_map.py
   !/integration/radiofoundry/map/map.c
   !/integration/radiofoundry/map/map.h
   !/integration/radiofoundry/map/turkey_map_data.c
   !/integration/radiofoundry/map/turkey_map_data.h
   !/integration/radiofoundry/map/ui_map.c
   !/tests/plus/test_map.c
   ```
2. Regenerate the names for the radio: `--charset iso-8859-9` when the build has `CONFIG_LANG_TR=y`, else `ascii`
   (or teach `ui_map.c` to transcode UTF-8 at draw time).
3. `tools/plus_features.py`: a new function, called after `prepare_p25_search(...)` in the list of `prepare_*` calls
   (the Tools list is a chain of anchors; this one hangs off P25 search and Air band/Last call as they are today —
   re-check the anchors against the current file):
   ```python
   def prepare_map(transform, install):
       """Offline Turkey map (CONFIG_MAP, needs NAV: GPS, APRS stations and waypoints): Tools > Map (docs/map.md)."""
       install('core/ui/map.h', 'integration/radiofoundry/map/map.h')
       install('core/ui/map.c', 'integration/radiofoundry/map/map.c')
       install('core/ui/turkey_map_data.h', 'integration/radiofoundry/map/turkey_map_data.h')
       install('core/ui/turkey_map_data.c', 'integration/radiofoundry/map/turkey_map_data.c')
       install('core/ui/ui_map.c', 'integration/radiofoundry/map/ui_map.c')
       transform('config/features.mk', [
           ('CONFIG_NAV ?= n\nFEATURES += NAV\n', 'CONFIG_NAV ?= n\nFEATURES += NAV\nCONFIG_MAP ?= n\nFEATURES += MAP\n'
            'ifeq ($(CONFIG_MAP),y)\nifneq ($(CONFIG_NAV),y)\n$(error MAP needs NAV=y)\nendif\nendif\n'),
       ])
       transform('core/config.h', [
           ('#ifndef CONFIG_NAV\n', '#ifndef CONFIG_MAP\n#define CONFIG_MAP 0\n#endif\n#ifndef CONFIG_NAV\n'),
       ])
       transform('core/ui/ui_nav.h', [
           ('(CONFIG_P25_SEARCH)\n\tSCR_COUNT',
            '(CONFIG_P25_SEARCH)\n\tSCR_MAP,\t\t\t// DM32 Plus: Tools > Map (CONFIG_MAP)\n\tSCR_COUNT'),
       ])
       transform('core/ui/ui_menus.c', [
           ('// ui_goto.c (DM32 Plus)', '// ui_goto.c (DM32 Plus)\nextern const ui_scr_def_t ui_map_screen;\t// ui_map.c (DM32 Plus)'),
           ('\t[SCR_GOTO] = &ui_goto_screen,\n#endif\n',
            '\t[SCR_GOTO] = &ui_goto_screen,\n#endif\n#if CONFIG_MAP\n\t[SCR_MAP] = &ui_map_screen,\n#endif\n'),
           ('T_ALMANAC, T_GOTO, T_P25SEARCH,', 'T_ALMANAC, T_GOTO, T_MAP, T_P25SEARCH,'),
           ('(i != T_GOTO || CONFIG_NAV) &&', '(i != T_GOTO || CONFIG_NAV) && (i != T_MAP || CONFIG_MAP) &&'),
           ('TRN("Go-To"), TRN("P25 search"),', 'TRN("Go-To"), TRN("Map"), TRN("P25 search"),'),
           ('\tcase T_GOTO: ui_push(SCR_GOTO, 0); break;',
            '\tcase T_GOTO: ui_push(SCR_GOTO, 0); break;\n\tcase T_MAP: ui_push(SCR_MAP, 0); break;'),
       ])
   ```
   Note `map.c` includes `"turkey_map_data.h"` (same directory) and `ui_map.c` includes `"ui/map.h"`, `"ui/nav.h"`,
   matching the `core/ui/` install paths above. If `core/ui/*.c` is not picked up by a wildcard, add the three
   `.c` files where `nav.c`/`ui_goto.c` are listed.
4. `tools/build_radiofoundry.py`: next to `"CONFIG_NAV="+("y" if args.plus else "n"),` add
   `"CONFIG_MAP="+("y" if args.plus else "n"),`.
5. `tools/test_classic.py`, after the `test_nav` lines:
   ```python
   run(['clang',*flags,'-DCONFIG_MAP=1','-I'+str(APP/'core/ui'),ROOT/'tests/plus/test_map.c',APP/'core/ui/map.c',
        APP/'core/ui/turkey_map_data.c','-o',OUT/'test_map'],cwd=ROOT)
   run([OUT/'test_map'],cwd=ROOT)
   ```
   (`flags` already has `-I core` for `config.h` and sanitizers; `test_map.c` includes `"map.h"` and
   `"turkey_map_data.h"`, hence the extra `-I core/ui`.)
6. New UI strings for `lang/tr.po`: `Map`, `No GPS fix` (exists), `Outside Turkey`, `Following GPS`,
   `^v<> pan  knob zoom  OK follow`.
7. Optionally add `python -I tools/turkey_map.py --zip … --check` to CI where the archive is cached.

## Not verified / open risks

- `ui_map.c` has never been compiled: names of `gfx_*`, `ui_layout.tiny`, `aprs_station_t` fields, `ui_toast` etc.
  were taken from `ui_goto.c` / headers but not built. Key mapping assumes the DM-32UV delivers `ACT_NAV_LEFT/RIGHT`
  or digits 2/4/6/8 on this screen; check the key map.
- Draw time on the CK803S at 168 MHz is unmeasured (zoom 0: ~4,000 short Bresenham lines plus 81 label checks per
  redraw). If slow, cache the province of the fix and redraw only on change/keys.
- Labels never overlap now (`map_occ_t`), but the grid is coarse (20 x 10 px cells on 320 x 240): a free spot
  next to a taken one can be refused, so some district names are left out where the map is busy (Istanbul at zoom 4;
  zoom in for them). Province labels win over district labels.
- District points are GeoNames' coordinates (seat or middle of the district), not boundaries; "/ district" under
  the map is the nearest centre (see above).
- Repeater positions are only as good as `rpt/places.csv` (city / town centres) or the owner's CSV. The district
  points are not (yet) used as an extra gazetteer for `rpt_list.py`: today's RadioID list places all 100 entries
  already, and short district names (Of, Sur, Han, Kale, Merkez...) would need whole-word rules to avoid false hits.
- Natural Earth 1:10m is coarse (≈ 1 km): positions within a few hundred metres of a provincial border can be named
  wrongly, and shore fixes rely on `map_province_near` (3 km in the UI). Small islands (< 0.01 deg², e.g. Bozcaada)
  are dropped; larger ones (Gökçeada) are kept. The output has 85 rings for 81 provinces.
- The licence statement is from Natural Earth's terms-of-use page as known to us; re-read it when updating the
  version pin.
- `TURKEY_MAP_CHARSET` must match the font build (see Format).
- Not tested on hardware at all.
