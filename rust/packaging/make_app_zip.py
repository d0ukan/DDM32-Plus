#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes this program's update zip for one platform: the `app_rs` asset a signed release lists
(pc/release/make_release.py --app-rs-zip PLATFORM=ZIP).

Layout (what ddm32-core's self_update::safe_extract accepts): every member under DDM32Plus/, the window as
DDM32Plus/DDM32Plus.exe (Windows) or DDM32Plus/DDM32Plus (Linux, macOS), the command line next to it, no user-data
name (backups, releases, updates, packs, settings.json) at the top. Deterministic: fixed time stamps and order,
so the same binaries give the same SHA-256.

    python rust/packaging/make_app_zip.py windows-x86_64 rust/target/release rust/dist/DDM32Plus-3.1.0-windows-x86_64.zip

Only reads the two binaries and writes the zip; publishes nothing.
"""
import argparse
import re
import sys
import zipfile
from pathlib import Path

PLATFORM = re.compile(r'^(windows|linux|macos)-[0-9A-Za-z_]{1,32}$')
STAMP = (2020, 1, 1, 0, 0, 0)


def members(platform, bin_dir):
    windows = platform.startswith('windows-')
    ext = '.exe' if windows else ''
    gui = bin_dir / f'ddm32-gui{ext}'
    cli = bin_dir / f'ddm32{ext}'
    for p in (gui, cli):
        if not p.is_file():
            raise SystemExit(f'missing {p} (build first: cargo build --release -p ddm32-cli -p ddm32-gui --features '
                             'ddm32-cli/radio,ddm32-gui/radio)')
    return [(f'DDM32Plus/DDM32Plus{ext}', gui), (f'DDM32Plus/ddm32{ext}', cli)]


def write_zip(out, items):
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + '.partial')
    with zipfile.ZipFile(tmp, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, path in items:
            info = zipfile.ZipInfo(name, STAMP)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o100755 << 16)  # a regular, executable file on Linux and macOS
            info.create_system = 3
            z.writestr(info, path.read_bytes())
    tmp.replace(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('platform', help='windows-x86_64, linux-x86_64, macos-aarch64, ...')
    ap.add_argument('bin_dir', type=Path, help='the folder with ddm32-gui and ddm32 (target/release)')
    ap.add_argument('out', type=Path, help='the zip to write')
    a = ap.parse_args(argv)
    if not PLATFORM.match(a.platform):
        ap.error(f'platform {a.platform!r} is not windows-ARCH, linux-ARCH or macos-ARCH')
    write_zip(a.out, members(a.platform, a.bin_dir))
    print(a.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
