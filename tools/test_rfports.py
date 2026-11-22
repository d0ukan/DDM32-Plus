#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""DDM32 Plus upstream RadioFoundry fixes (tools/plus_rfports.py): the pinned FD6818 and HR_C7000 DMR host suites, with
the ported upstream cases, built with CONFIG_RF_FIXES=1 under ASan and UBSan (test_classic.py runs them with it off)."""
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / 'build/radiofoundry-p25'
HOST = APP / 'tests/host'
OUT = ROOT / 'build/rfports-host'


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def build(target, extra=()):
    """The host Makefile's own compile line for target, with the switch on and the output in OUT"""
    command = run(['make', '-Bn', target, 'SANITIZE=1', *extra], cwd=HOST, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if (' -o %s ' % target) in line + ' '))
    argv[argv.index('-o') + 1] = str(OUT / target)
    argv.insert(1, '-DCONFIG_RF_FIXES=1')
    run(argv, cwd=HOST)
    run([OUT / target], cwd=HOST)


def main():
    run(['python3', ROOT / 'tools/build_radiofoundry.py', '--prepare-only'], cwd=ROOT)
    OUT.mkdir(parents=True, exist_ok=True)
    build('fd6818Test', ['RFBUS=' + str(APP / 'drivers/rf/rfbus/rfbus.c') + ' ' +
                         str(APP / 'drivers/rf/fd6818/fd6818_setup_b.c')])
    build('hrc7000DmrTest')
    print('rfports host tests: passed')


if __name__ == '__main__':
    main()
