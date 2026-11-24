<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# DDM32 Plus art: splash, wordmark and icons

Our own look for DDM32 Plus (working name DM32 Plus until 2026-11-19): a boot mark, a "DDM32 Plus" wordmark, the
author's callsign TA2DOU small at the splash's bottom centre, and 32 list icons. Nothing here is taken from
RadioFoundry (its radio-in-a-hexagon mark and two-color "RadioFoundry" wordmark), from the upstream Open25-DM32
Classic/Next releases, OnurP25 or the DM32-nxdn-p25 avatar. The name on screen is "DDM32 Plus" (`art.h`
`ART_PRODUCT`, `ART_CALLSIGN`; docs/plus.md "Name and public build"). `art/preview.png` and `art/mock-splash.png`
still show the old wordmark until `--preview` is run again.

Status: wired into Plus builds since 4a64cc1e (CONFIG_PLUS_ART, `prepare_art` in tools/plus_features.py); host
test done; not yet seen on the real screen (the radio still runs 7c90427b, which shows RadioFoundry's splash).

## Files

| Path | What |
|---|---|
| `art/icons.txt` | 32 icons, 16x16, `#` ink / `.` none; order = `ART_ICON_*` enum order |
| `art/mark.txt` | splash mark, 38x56, `.` none, `a` `b` `c` = its three colors (RGB + role in the header: ink, paper or fg) |
| `art/mark_gen.py` | traces `mark.txt` from the owner's drawing `art/mark-src.png` (needs Pillow; `mark.txt` may be edited by hand instead) |
| `art/mark-src.png` | the owner's logo drawing, the source of the mark |
| `art/wordmark.txt` | "DDM32+ Plus", 87x12, 1 bit; columns from `split` on (the plus and "Plus") are drawn in the accent color |
| `art/preview.png` | contact sheet: mark 2x/1x, wordmark 2x/1x, every icon at 3x and 1x on dark, light, focus blue |
| `art/mock-splash.png` | 240x320 boot screen, dark and light theme, shown at 2x |
| `art/mock-tools.png` | 240x320 Tools list with the icon column, dark (focus on 2) and light, shown at 2x |
| `tools/art_build.py` | text sources to `integration/radiofoundry/art/art_data.[ch]`; `--check`; `--preview FONT_DIR` |
| `integration/radiofoundry/art/art.[ch]` | `art_icon`, `art_mark`, `art_wordmark`, `art_splash` (only `gfx_fill`) |
| `integration/radiofoundry/art/art_data.[ch]` | generated const tables |
| `integration/radiofoundry/art/splash_plus.c` | `ui_draw_splash_plus`: the whole boot screen |
| `tests/plus/test_art.c` | host test: tables = sources, drawn pixels = sources, colors, bounds, centering; the whole boot screen on stand-ins (`tests/plus/art_host_ui.h`): the callsign on the bottom line, centred, inside the panel |

The panel: DM-32UV is a **240x320** ST7789 TFT driven at **4 bpp** (a 16-entry palette gfx builds per frame,
`boards/dm32/board.mk` `GFX_INDEXED=1`), not 160x128. With `CONFIG_FONT_BIG` the list font is 9x18 and a list
row is 24 px (`ui_layout_init`), so 16x16 icons fit with 4 px above and below.

## Style rules

- 16x16 grid, 1 bit a pixel. Main strokes 2 px, details 1 px; silhouettes rather than outlines where a shape
  is small (fox, plane, pin). Keep a pixel of air between separate parts.
- Icons are a mask tinted with **one color**: the row's text color (label color, focus text, dim gray). So an
  icon never adds a palette entry on the 4 bpp panel and follows every theme and accent automatically.
- The mark (owner's design, 2026-11-20): a handheld radio drawn as a "D" - a thick stem is the antenna, with two
  signal arcs at its tip, and the shared left side of an outer D frame and an inner D; an orange key sits on the
  stem. Traced from the owner's drawing `art/mark-src.png` by `art/mark_gen.py` (Pillow; 38x56, square pixels, each
  pixel from its source box's coverage), not redrawn by hand. The white parts use the role `fg` (the theme's text
  color: white on dark, black on light), the key the accent orange `(240,120,0)`, which reads on black and on
  white. Drawn at 2x (76x112) on 240 px panels. One fixed color + the theme's bg/fg/dim stay well under 16.
- Color roles in `mark.txt`: `ink` / `paper` (a fixed RGB; on a 1 bpp panel fg / bg) or `fg` (the theme's text
  color; RGB is only the preview's stand-in). Color `a` is the accent (`art_accent`: "Plus", the rule, the callsign)
  and must be a fixed color that reads on both themes.
- The wordmark: "DDM32" in the theme's text color, then a superscript plus and "Plus" in the accent (owner
  2026-11-20: "DDM32+ Plus"; white/black on mono). 87 px wide: 174 px at 2x.
- Mono (1 bpp) fallback: mark colors marked `ink` or `fg` in fg, `paper` in bg; wordmark in fg.
- Source order is the ABI: append new icons at the end of `icons.txt`, never reorder (the enum follows it).

## Cost

| Part | Flash (csky-elf-gcc 14.3 -Os, ck803s) | RAM |
|---|---|---|
| `art_data.o` (32 icons 1024 B, mark 784 B, colors 9 B, wordmark 120 B) | 1937 B | 0 |
| `art.o` (drawing) | 736 B | 0 |
| `splash_plus.o` | 626 B | 0 |
| **Total** | **≈ 3.1 KiB** (budget 24 KiB) | 0 static; stack < 100 B |

Each icon is 32 B; 20 more icons would add 640 B. The stock mark code in `ui_screens.c` (`mark_color`,
`draw_mark`, `draw_wordmark`, ~1 KiB) stays linked because `ui_draw_splash` keeps its fallback path; it can be
put under `#if !CONFIG_PLUS_ART` later if the flash is wanted (the fallback then never draws RadioFoundry's mark).
`ui_item_t` grows by one byte (`icon`); every `ui_item_t` is on the stack, none static except one in `ui_app.c`.

## Regenerating and testing

```sh
python3 art/mark_gen.py                     # only after changing art/mark-src.png
python3 tools/art_build.py                  # writes integration/radiofoundry/art/art_data.[ch]
python3 tools/art_build.py --check          # CI: fails if art_data.[ch] are stale
python3 tools/art_build.py --preview <dir>  # PNGs; <dir> holds jbmono_8x16.inc, jbmono_9x18.inc, jbmono_12x24.inc
                                            # (copies of the patched tree's core/ui/fonts/)
cc -std=gnu11 -Wall -Wextra -Wshadow -Wconversion -Werror -fsanitize=address,undefined -DART_HOST_TEST \
   -Iintegration/radiofoundry/art -Itests/plus tests/plus/test_art.c integration/radiofoundry/art/art.c \
   integration/radiofoundry/art/art_data.c integration/radiofoundry/art/splash_plus.c -o test_art && ./test_art art
```

Verified 2026-11-17: gcc 15 and clang (WSL), ASan+UBSan, `-Wconversion -Wsign-conversion -Werror`: zero warnings,
`art: 32 icons, 1725 bytes of tables: ok`. A pixel flipped in `icons.txt` fails the test. `art.c`,
`splash_plus.c` and `art_data.c` also pass `-fsyntax-only -Wall -Wextra -Wshadow -Wdouble-promotion -Werror`
against the patched tree's headers (`build/dm32-p25-plus/gen`) with `CONFIG_PLUS_ART=1`, and build with the csky
toolchain. Not done: a full firmware build, the simulator, the radio.

## Integration (for the main session)

All in a new `prepare_art(transform, install)` in `tools/plus_features.py`, called **after** every other Plus
feature (it names their `T_*` tools). Anchors below were checked unique in the patched tree of 2026-11-17.

1. **Install** (core/ui is globbed by the Makefile):
   ```python
   for f in ('art.h', 'art.c', 'art_data.h', 'art_data.c', 'splash_plus.c'):
       install('core/ui/' + f, 'integration/radiofoundry/art/' + f)
   ```
2. **Switch** `CONFIG_PLUS_ART` (all three art sources compile to nothing when it is 0):
   - `config/features.mk`: after `'CONFIG_NAV ?= n\nFEATURES += NAV\n'` add
     `'CONFIG_PLUS_ART ?= n\nFEATURES += PLUS_ART\n'`.
   - `core/config.h`: `('#ifndef CONFIG_NAV\n', '#ifndef CONFIG_PLUS_ART\n#define CONFIG_PLUS_ART 0\n#endif\n#ifndef CONFIG_NAV\n')`.
   - `tools/build_radiofoundry.py`: `"CONFIG_PLUS_ART="+("y" if args.plus else "n"),` beside `CONFIG_NAV`, and
     `PLUS_ART` in `PLUS_FEATURES` if that list is meant to name it.
3. **Splash** — `core/ui/ui_screens.c`:
   ```python
   ('#include "ui_svc.h"\n', '#include "ui_svc.h"\n#if CONFIG_PLUS_ART\n#include "art.h"\n#endif\n'),
   ('void ui_draw_splash(const char *board_name, const char *version, const char *line1, const char *line2)\n{\n',
    'void ui_draw_splash(const char *board_name, const char *version, const char *line1, const char *line2)\n{\n'
    '#if CONFIG_PLUS_ART\n\tif (ui_draw_splash_plus(board_name, version, line1, line2))\n\t\treturn;\n#endif\n'),
   ```
   This also covers `core/stubentry/stub_entry.c`, which calls `ui_draw_splash`.
4. **Icon column in lists** — `core/ui/ui_screens.h` (the field) and `core/ui/ui_screens.c` (`ui_draw_list`):
   ```python
   # ui_screens.h
   ("\tchar num;\t\t// key shown before the label ('1'..'9', '0'), 0 = none\n",
    "\tchar num;\t\t// key shown before the label ('1'..'9', '0'), 0 = none\n#if CONFIG_PLUS_ART\n"
    "\tuint8_t icon;\t\t// DM32 Plus: ART_ICON_* + 1 drawn before the label, 0 = none\n#endif\n"),
   # ui_screens.c, ui_draw_list
   ('\t\tit.num = 0;\n', '\t\tit.num = 0;\n#if CONFIG_PLUS_ART\n\t\tit.icon = 0;\n#endif\n'),
   ('\t\t\tx = gfx_text(f, x, ry + pad, n, focus ? fg : (gfx_bpp() == 1 ? fg : ui_col.dim));\n\t\t}\n',
    '\t\t\tx = gfx_text(f, x, ry + pad, n, focus ? fg : (gfx_bpp() == 1 ? fg : ui_col.dim));\n\t\t}\n'
    '#if CONFIG_PLUS_ART\n\t\tif (it.icon && L->row_h >= ART_ICON_H + 2) {\n'
    '\t\t\tart_icon(it.icon - 1u, x, ry + (L->row_h - ART_ICON_H) / 2, fg);\n'
    '\t\t\tx += ART_ICON_W + 5;\n\t\t}\n#endif\n'),
   ```
   `fg` there is already the row's color (dim rows gray, focus row `focus_fg`, ink/paper on mono). Rows shorter
   than 18 px (small panels) skip the icon, so other boards look as before.
5. **Tools icons** — `core/ui/ui_menus.c` (`#include "art.h"` under `#if CONFIG_PLUS_ART` after
   `#include "fmt.h"\n`), then in `tools_item`:
   ```python
   ('\titem_num(it, (uint16_t)(i + 1u));\t// the tool\'s own number, not its row (see the enum)\n',
    '\titem_num(it, (uint16_t)(i + 1u));\t// the tool\'s own number, not its row (see the enum)\n'
    '#if CONFIG_PLUS_ART\n\tstatic const uint8_t tool_icon[T_COUNT] = {\n'
    '\t\t[T_SCAN] = ART_ICON_SCAN + 1, [T_POSITION] = ART_ICON_POSITION + 1, [T_HOTSPOT] = ART_ICON_HOTSPOT + 1,\n'
    '\t\t[T_USB_NET] = ART_ICON_USB_NET + 1, [T_SFR] = ART_ICON_SFR + 1, [T_ROAM] = ART_ICON_ROAM + 1,\n'
    '\t\t[T_DIAG] = ART_ICON_DIAG + 1, [T_APRS] = ART_ICON_APRS + 1, [T_SPECTRUM] = ART_ICON_SPECTRUM + 1,\n'
    '\t\t[T_FOX] = ART_ICON_FOX + 1, [T_BEACON] = ART_ICON_BEACON + 1, [T_LONE] = ART_ICON_LONE + 1,\n'
    '\t\t[T_ALMANAC] = ART_ICON_ALMANAC + 1, [T_GOTO] = ART_ICON_GOTO + 1, [T_P25SEARCH] = ART_ICON_P25SEARCH + 1,\n'
    '\t\t[T_AIRBAND] = ART_ICON_AIRBAND + 1, [T_BATTERY] = ART_ICON_BATTERY + 1,\n'
    '\t\t[T_ZONE_OFFER] = ART_ICON_ZONE_OFFER + 1,\n\t};\n\tit->icon = tool_icon[i];\n#endif\n'),
   ```
   The anchor (with its comment) occurs once; the bare `item_num(it, (uint16_t)(i + 1u));` occurs 10 times.
6. **Hub icons (optional, same pattern)** — in `hub_item` after `\titem_num(it, e);\n`:
   `static const uint8_t hub_icon[] = { ART_ICON_LAST_CALLER + 1, ART_ICON_CONTACTS + 1, ART_ICON_MESSAGES + 1,
   ART_ICON_RECENT + 1, ART_ICON_ZONES + 1, ART_ICON_CHANNELS + 1, ART_ICON_SCAN + 1 /* Tools: or SETTINGS */,
   ART_ICON_SETTINGS + 1 };	it->icon = hub_icon[e];` (HUB_* order). `MAP`, `DUAL`, `LANGUAGE` are ready for the
   settings/dual-watch screens.
7. **Tests**: in `tools/test_classic.py` beside the other Plus tests:
   ```python
   run(['python3', ROOT/'tools/art_build.py', '--check'], cwd=ROOT)
   run(['clang',*flags,'-DART_HOST_TEST','-I'+str(ROOT/'integration/radiofoundry/art'),ROOT/'tests/plus/test_art.c',
        ROOT/'integration/radiofoundry/art/art.c',ROOT/'integration/radiofoundry/art/art_data.c','-o',OUT/'test_art'],cwd=ROOT)
   run([OUT/'test_art', ROOT/'art'], cwd=ROOT)
   ```
8. `docs/plus.md`: list `CONFIG_PLUS_ART` with its flash (≈ 3.1 KiB) and 0 RAM.
9. `firmware/.gitignore` is an allowlist and currently ignores every new art file. Add (style of the file):
   `!/art/`, `!/art/*.txt`, `!/art/*.png`, `!/art/mark_gen.py`, `!/integration/radiofoundry/art/`,
   `!/integration/radiofoundry/art/*`, `!/tools/art_build.py`, `!/tests/plus/test_art.c`, `!/docs/art.md`
   (with whatever parent `!` lines the file's existing entries need).

## Needs a look on the real screen

- Icon legibility at 16 px on the 2.4" panel (about 167 dpi): `scan` (two curved arrows), `sfr`, `roam`,
  `almanac` and `language` are the busiest; `fox`, `airband`, `battery`, `position`, `spectrum` read best.
- Whether the icon column crowds long labels: with it a row has 21 px (about 2 characters) less room, so
  "Single-freq repeater" and longer Turkish labels get shortened sooner when a value is shown.
- Splash colors through the 4 bpp palette and the panel's gamma: the teal `(0,150,136)` and "Plus" in teal on
  black (contrast about 5:1), the yellow plus.
- Boot timing: the splash art is about 320 runs (`gfx_fill`, 224 of them the mark) at any scale, comparable to the stock procedural mark which
  calls `gfx_pixel` for every pixel of a 2r x 2r box.
- The mono fallback (1 bpp panels) is only covered by the host test; no Plus target has such a panel.
