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


def fm_copy():
    """The prepared fm_rx_audio.c beside the host doubles of its platform, RTOS and glue headers (tests/plus/fm_copy),
    so its quoted includes find them first; tests/plus/test_fm_copy.c includes it"""
    work = OUT / 'fm_copy'
    work.mkdir(parents=True, exist_ok=True)
    for f in (APP / 'platform/hrc7000/fm_rx_audio.c', APP / 'platform/hrc7000/fm_rx_audio.h',
              *sorted((ROOT / 'tests/plus/fm_copy').glob('*.h'))):
        (work / f.name).write_bytes(f.read_bytes())
    run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', '-DCONFIG_APRS_RX=1', '-DCONFIG_RF_FIXES=1', '-I', work, '-I', APP / 'core',
         '-I', APP / 'drivers', ROOT / 'tests/plus/test_fm_copy.c', '-o', OUT / 'test_fm_copy'])
    run([OUT / 'test_fm_copy'])


def main():
    run(['python3', ROOT / 'tools/build_radiofoundry.py', '--prepare-only'], cwd=ROOT)
    OUT.mkdir(parents=True, exist_ok=True)
    build('fd6818Test', ['RFBUS=' + str(APP / 'drivers/rf/rfbus/rfbus.c') + ' ' +
                         str(APP / 'drivers/rf/fd6818/fd6818_setup_b.c')])
    build('hrc7000DmrTest')
    # Second round (rfports2): the squelch's voice peak hold and the keying gate (radioCoreTest, aprsRadioTest), received DMR text's
    # C1 controls (dmrDataServiceTest), the FM receive copy's watchdog (fm_rx_audio.c on host doubles)
    build('radioCoreTest')
    build('dmrDataServiceTest')
    build('aprsRadioTest')
    fm_copy()
    print('rfports host tests: passed')


if __name__ == '__main__':
    main()
