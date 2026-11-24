#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes the window's logo (rust/crates/ddm32-gui/ui/logo-light.png, logo-dark.png) from the owner's drawing
art/mark-src.png (the same art as the C# program's icon): the radio "D" with its antenna arcs and the orange key, and
"DDM32+" under it, on a transparent background; and the Linux / macOS icon rust/packaging/art/ddm32plus.png.

dark: the white strokes stay white (for the dark theme); light: they take the light theme's text colour #15181d.
The orange stays orange in both (a little darker on light, so it still reads on white). Needs Pillow; deterministic:

    python rust/packaging/art/make_logo.py
"""
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parents[3]
SRC = REPO / 'art' / 'mark-src.png'
OUT = REPO / 'rust' / 'crates' / 'ddm32-gui' / 'ui'
INK = {'dark': (255, 255, 255), 'light': (0x15, 0x18, 0x1d)}
ORANGE = {'dark': (240, 120, 0), 'light': (214, 100, 0)}


def make(theme):
    im = Image.open(SRC).convert('RGB')
    w, h = im.size
    out = Image.new('RGBA', (w, h))
    src = im.load()
    dst = out.load()
    for y in range(h):
        for x in range(w):
            r, g, b = src[x, y]
            a = max(r, g, b)
            if a < 24:
                dst[x, y] = (0, 0, 0, 0)
                continue
            orange = r - b > 80 and r > g
            c = ORANGE[theme] if orange else INK[theme]
            dst[x, y] = c + (min(255, int(a * 1.05)),)
    bbox = out.getbbox()
    out = out.crop(bbox)
    out.save(OUT / f'logo-{theme}.png', optimize=True)
    return out.size


def make_icon():
    """The Linux / macOS application icon (rust/packaging/art/ddm32plus.png, 256x256): the C# program's icon, its
    largest frame, so all three systems show the same picture (ddm32plus.ico here is a copy of it; build.rs puts it
    into DDM32Plus.exe)."""
    ico = Image.open(Path(__file__).resolve().parent / 'ddm32plus.ico')
    ico.size = (256, 256)
    icon = ico.convert('RGBA')
    icon.save(Path(__file__).resolve().parent / 'ddm32plus.png', optimize=True)
    return icon.size


if __name__ == '__main__':
    for t in ('light', 'dark'):
        print(t, make(t))
    print('icon', make_icon())
