# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the Tools menu icons (tools/art_build.py, art/icons.txt, the tool_icon table the plus_*.py transforms
build in core/ui/ui_menus.c; docs/art.md), run by tools/test_classic.py (the build tree prepared, ASan / UBSan flags
given); by hand after a build, from the firmware root: python3 tools/test_toolicons.py [SHOT_DIR]

- the table in the built ui_menus.c: every Tools entry has an icon; DOTA Transfer, Cross-band, CW decoder, SOS key and
  CW beacon each have an icon of their own; every other icon two entries share is a deliberate family (below);
- the real UI in upstream's simulator (tests/plus/test_ui_toolicons.c), dark and light theme: the entries the build
  config shows, each focused in turn, its icon recorded; inside the frame buffer, no layout issue. With SHOT_DIR (or
  TOOLICON_SHOTS) the Tools menu is saved there as PNG files."""
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

# Icons the list repeats on purpose: a family of tools that are one idea (the same list as the C test's)
FAMILY = {'ALMANAC', 'GOTO', 'P25SEARCH', 'AIRBAND', 'RECENT', 'MAP', 'POSITION', 'SPECTRUM'}
OWN = {'T_DOTA': 'DOTA', 'T_XBAND': 'XBAND', 'T_CW': 'CW', 'T_SOS': 'SOS', 'T_BEACON': 'BEACON'}


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def table(ui_menus):
    """(entry -> icon name) from the tool_icon table, and the entries of the T_ enum"""
    text = Path(ui_menus).read_text(encoding='utf-8')
    enum = re.search(r'enum \{ (T_SCAN,[^}]*) \};', text).group(1)
    entries = [e.strip() for e in enum.split(',') if e.strip() not in ('')]
    entries = [e for e in entries if e != 'T_COUNT']
    body = re.search(r'static const uint8_t tool_icon\[T_COUNT\] = \{(.*?)\n\t\};', text, re.S).group(1)
    icons = {m.group(1): m.group(2) for m in re.finditer(r'\[(T_[A-Z0-9_]+)\] = ART_ICON_([A-Z0-9_]+) \+ 1', body)}
    return entries, icons


def check_table(ui_menus):
    entries, icons = table(ui_menus)
    errors = []
    for e in entries:
        if e == 'T_ZONE_OFFER':
            continue
        if e not in icons:
            errors.append(f'{e} has no icon')
    for e, name in OWN.items():
        if icons.get(e) != name:
            errors.append(f'{e} has icon {icons.get(e)}, want {name}')
        for o, n in icons.items():
            if o != e and n == name:
                errors.append(f'{e} shares icon {name} with {o}')
    byicon = {}
    for e, n in icons.items():
        byicon.setdefault(n, []).append(e)
    for n, es in sorted(byicon.items()):
        if len(es) > 1 and n not in FAMILY and not set(es) <= {'T_ZONE_OFFER'}:
            errors.append(f'icon {n} is shared by {", ".join(es)}')
    if errors:
        raise SystemExit('Tools icons: ' + '; '.join(errors))
    print(f'tool icons table: {len(icons)} entries, {len(byicon)} icons, the five own icons unique: ok')
    for n, es in sorted(byicon.items()):
        print(f'  {n:12s} {", ".join(es)}')


def ui_argv(host, test, out):
    command = run(['make', '-Bn', 'uiFlowTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')] = str(test)
    argv[argv.index('-o') + 1] = str(out)
    return argv


def main(ROOT, APP, OUT, flags, host, shots=None):
    tp = ROOT / 'tests/plus'
    core = APP / 'core'
    check_table(core / 'ui/ui_menus.c')
    run(['make', 'out/gen/lang_tables.h'], cwd=host, stdout=subprocess.DEVNULL)
    exe = OUT / 'test_ui_toolicons'
    argv = ui_argv(host, tp / 'test_ui_toolicons.c', exe)
    # (the screens behind the entries and the radio core's side are stubbed in the test; the screens' symbols the other
    # entries would need are left unresolved: nothing here opens a tool)
    run([*argv, '-I' + str(host), '-I' + str(core), '-Wno-unused-function', '-DCONFIG_P25_CHANNELS=1',
         '-DCONFIG_PLUS_ART=1', '-DCONFIG_DOTA=1', '-DCONFIG_XBAND=1', '-DCONFIG_CW_RX=1', '-DCONFIG_MSG_RX=1',
         '-DCONFIG_SOS_KEY=1', '-DCONFIG_CW_BEACON=1', '-DCONFIG_DEV_TX=1', '-DCONFIG_LANG_TR=1',
         str(core / 'ui/art.c'), str(core / 'ui/art_data.c'), str(core / 'ui/splash_plus.c'),
         '-Wl,--wrap=art_icon', '-Wl,--unresolved-symbols=ignore-all', '-lm'], cwd=host)
    env = dict(os.environ)
    if shots:
        Path(shots).mkdir(parents=True, exist_ok=True)
        env['TOOLICON_SHOTS'] = str(shots)
    run([exe], cwd=host, env=env)


if __name__ == '__main__':
    root = Path(__file__).resolve().parents[1]
    app = root / 'build/radiofoundry-p25'
    out = root / 'build/classic-host'
    out.mkdir(parents=True, exist_ok=True)
    main(root, app, out, [], app / 'tests/host', sys.argv[1] if len(sys.argv) > 1 else os.environ.get('TOOLICON_SHOTS'))
