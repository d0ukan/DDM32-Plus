# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the VFO step key (CONFIG_VFO_STEPKEY, tools/plus_vfostep.py, docs/plus.md "VFO step key"), run by
tools/test_classic.py (the build tree prepared, ASan / UBSan flags given): the real UI in upstream's simulator,
tests/plus/test_ui_vfostep.c, with ui_vfostep.c built in, English and Turkish, normal and Large text."""
import shlex
import subprocess
from pathlib import Path


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def ui_tests(ROOT, APP, OUT, host):
    tp = ROOT / 'tests/plus'
    run(['make', 'out/gen/lang_tables.h'], cwd=host, stdout=subprocess.DEVNULL)
    command = run(['make', '-Bn', 'uiFlowTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')] = str(tp / 'test_ui_vfostep.c')
    argv[argv.index('-o') + 1] = str(OUT / 'test_ui_vfostep')
    run([*argv, '-I' + str(host), '-DCONFIG_P25_CHANNELS=1', '-DCONFIG_VFO_STEPKEY=1', '-DCONFIG_LANG_TR=1',
         str(APP / 'core/ui/ui_vfostep.c')], cwd=host)
    run([OUT / 'test_ui_vfostep'], cwd=host)


def main(ROOT, APP, OUT, flags, host):
    ui_tests(Path(ROOT), Path(APP), Path(OUT), Path(host))


if __name__ == '__main__':
    ROOT = Path(__file__).resolve().parents[1]
    APP = ROOT / 'build/radiofoundry-p25'
    OUT = ROOT / 'build/classic-host'
    OUT.mkdir(parents=True, exist_ok=True)
    ui_tests(ROOT, APP, OUT, APP / 'tests/host')
