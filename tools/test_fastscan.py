#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the faster channel scan (CONFIG_SCAN_FAST, tools/plus_fastscan.py, docs/plus.md "Faster channel
scan"), run by tools/test_classic.py after its own (the build tree prepared, ASan / UBSan flags given), or alone after
a build (python3 tools/test_fastscan.py [engine|suites|upstream|model]):

- engine: the engine alone (tests/plus/test_fastscan.c);
- suites: upstream's radio suite, the Classic / Plus P25 suites, the multi-watch ring and the priority channel /
  temporary skip suite (tests/plus/test_scanx_radio.inc) built with the faster scan and run on classic: the switch
  compiled in changes nothing; then the same with fast on: every check holds (the ring is not the fast scan's);
- upstream: upstream's scan suite (tests/host/radioFeaturesTest.c: zone, VFO and mixed scans, PTT, tones, DMR calls
  in progress, a receiver deaf to DMR, dual watch) with fast on: only its four checks that assume a 150 ms stop may
  fail (the monitor pressed while the faster scan is on a DMR channel, where the monitor doesn't pause it in either
  algorithm; and two "moved on from the DMR channel" checks the faster scan has already come round again for);
- model: the radio core's zone scan on a model of the receiver, classic -> fast (tests/plus/test_fastscan_radio.inc),
  the table printed for docs/plus.md and its checks."""
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def engine(ROOT, APP, OUT, flags):
    exe = OUT / 'test_fastscan'
    run(['clang', *flags, '-DCONFIG_SCAN_FAST=1', ROOT / 'tests/plus/test_fastscan.c', APP / 'core/radio/scanfast.c',
         '-o', exe], cwd=ROOT)
    run([exe], cwd=ROOT)


def radio_argv(ROOT, APP, OUT, host, wrapper, out_name):
    command = run(['make', '-Bn', 'radioCoreTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o radioCoreTest ' in line))
    argv[argv.index('radioCoreTest.c')] = str(wrapper)
    argv[argv.index('-o') + 1] = str(OUT / out_name)
    radio = APP / 'core/radio'
    return argv + ['-I' + str(OUT), '-I' + str(host), '-I' + str(ROOT / 'tests/native/stubs'), '-I' + str(APP / 'core'),
                   '-I' + str(ROOT / 'src/modem'), '-I' + str(ROOT / 'integration/radiofoundry'),
                   '-I' + str(ROOT / 'tests/classic'), '-I' + str(ROOT / 'tests/plus'),
                   '-DCONFIG_P25_CHANNELS=1', '-DCONFIG_P25_NATIVE=1', '-DCONFIG_P25=1', '-DCONFIG_DEV_TX=1',
                   '-DCONFIG_P25_SCAN=1', '-DCONFIG_SPECTRUM=1', '-DCONFIG_FOX=1', '-DCONFIG_CW_BEACON=1',
                   '-DCONFIG_P25_SEARCH=1', '-DCONFIG_P25_CALLER=1', '-DCONFIG_P25_ANY=1', '-DCONFIG_SCAN_FAST=1',
                   str(ROOT / 'src/modem/native_test.c'), str(ROOT / 'integration/radiofoundry/native/native_lab.c'),
                   *(str(radio / f) for f in ('spectrum.c', 'fox.c', 'cw_beacon.c', 'p25search.c', 'scanfast.c'))]


def fixture(host):
    text = (host / 'radioCoreTest.c').read_text().replace('int main(void)', 'int upstream_radio_main(void)')
    return text.replace('const board_t board = {', 'const board_t board = {\n\t.native_modem=&classic_ops,')


HEAD = '#include "native_modem/native_lab.h"\n#include "radio/scanfast.h"\nextern const nt_ops classic_ops;\n'


def suites(ROOT, APP, OUT, host):
    # The suites as tools/test_classic.py's test_radio_scanx builds them (upstream's radio suite, Classic / Plus P25,
    # the multi-watch ring, the priority channel), with the faster scan built in: classic, then fast
    wrapper = OUT / 'radioCoreFastscanSuites.c'
    wrapper.write_text(HEAD + fixture(host) +
                       '\n#define main classic_radio_main\n#include "' + str(ROOT / 'tests/classic/test_radio.inc') +
                       '"\n#undef main\n#define main mwatch_radio_main\n#include "' +
                       str(ROOT / 'tests/plus/test_mwatch_radio.inc') + '"\n#undef main\n#define main scanx_radio_main\n'
                       '#include "' + str(ROOT / 'tests/plus/test_scanx_radio.inc') + '"\n#undef main\n'
                       # ... and whether the fast rule ran in them (the zone scans of the priority channel's tests)
                       'extern uint32_t fs_total_quiet;\n'
                       'int main(void)\n{\n\tint r = scanx_radio_main();\n'
                       '\tprintf("Faster scan in these suites: %u stops ended by the fast rule\\n", (unsigned)fs_total_quiet);\n'
                       '\treturn r;\n}\n')
    # as built there (poll only in the ring's and the priority channel's helpers), then with the 10 ms tick between
    # their polls (MW_POLL_TICKS, tests/plus/test_mwatch_radio.inc mw_poll: what the faster scan reads on), classic and
    # fast
    for fast, ticks in ((0, 0), (0, 1), (1, 1)):
        name = f'test_radio_fastscan_suites_{fast}{ticks}'
        argv = radio_argv(ROOT, APP, OUT, host, wrapper, name)
        run([*argv, '-DCONFIG_MULTI_WATCH=1', '-DCONFIG_SCAN_PRIO=1', f'-DFS_DEFAULT_FAST={fast}', '-DFS_TEST_TOTALS=1',
             *(['-DMW_POLL_TICKS=1'] if ticks else [])], cwd=host)
        out = run([OUT / name], cwd=host, capture_output=True, text=True).stdout
        print(f'Radio suites with the faster scan built in ({"fast" if fast else "classic"}, '
              f'{"tick and poll" if ticks else "poll only"} in the ring / priority helpers): ' +
              ' | '.join(line for line in out.splitlines() if 'priority channel' in line or 'radioCoreTest:' in line or
                        'Faster scan in' in line))


# upstream's scan suite: the checks that assume a classic stop's 150 ms (a scan a few times faster is somewhere else
# by then), each with why (tests/host/radioFeaturesTest.c)
UPSTREAM_TIMING = {
    # the monitor pressed 400 ms into the scan: classic is on an FM channel, fast already on the DMR one, where a press
    # toggles DMR monitor on release and doesn't pause the scan (upstream's own rule, both algorithms)
    'sm.view.zone_pos == at && sm.view.scan_phase == SCAN_PH_PAUSED',
    'sm.view.zone_pos != at',
    # the mixed scan: 1 s after the DMR channel's probe began / 2.5 s after its call, fast has gone round the zone and
    # is on the DMR channel again (classic, at 150 ms a stop, is still elsewhere)
    'sm.view.zone_pos != 5 && !sim.started',
    'sm.view.zone_pos != 5',
}


def upstream(ROOT, APP, OUT, host):
    command = run(['make', '-Bn', 'radioFeaturesTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    command = re.sub(r'\\[ \t]*\n', ' ', command)
    argv = shlex.split(next(line for line in command.splitlines() if ' -o radioFeaturesTest ' in line))
    for fast in (0, 1):
        exe = OUT / f'radioFeaturesTest_fastscan_{fast}'
        a = list(argv)
        a[a.index('-o') + 1] = str(exe)
        run([*a, '-DCONFIG_SCAN_FAST=1', f'-DFS_DEFAULT_FAST={fast}', str(APP / 'core/radio/scanfast.c')], cwd=host)
        out = subprocess.run([str(exe)], cwd=host, capture_output=True, text=True).stdout
        fails = [line for line in out.splitlines() if line.startswith('FAIL')]
        if fast == 0:
            if fails or 'PASSED' not in out:
                raise SystemExit('radioFeaturesTest, faster scan built in, classic: ' + '\n'.join(fails or [out[-500:]]))
            print('Upstream scan suite with the faster scan built in, classic: ' + out.strip().splitlines()[-1])
            continue
        unexpected = [line for line in fails if line.split(': ', 1)[-1] not in UPSTREAM_TIMING]
        if unexpected:
            raise SystemExit('radioFeaturesTest with the faster scan:\n' + '\n'.join(unexpected))
        print(f'Upstream scan suite with the faster scan on: {len(fails)} of its checks assume a 150 ms stop '
              f'(listed in tools/test_fastscan.py), every other check holds')


def model(ROOT, APP, OUT, host):
    wrapper = OUT / 'radioCoreFastscanModel.c'
    fix = fixture(host)
    assert fix.count('#define NCH 8\n') == 1
    fix = fix.replace('#define NCH 8\n', '#define NCH 96\n')	# zones of up to 80 channels
    wrapper.write_text('#include <stdlib.h>\n' + HEAD + fix +
                       '\n#define main classic_radio_main\n#include "' + str(ROOT / 'tests/classic/test_radio.inc') +
                       '"\n#undef main\n#include "' + str(ROOT / 'tests/plus/test_fastscan_radio.inc') + '"\n')
    argv = radio_argv(ROOT, APP, OUT, host, wrapper, 'test_radio_fastscan_model')
    run([*argv, '-DCONFIG_SCAN_PRIO=1', '-lm'], cwd=host)
    run([OUT / 'test_radio_fastscan_model'], cwd=host)


def main(ROOT, APP, OUT, flags, host, only=None):
    if not only or 'engine' in only:
        engine(ROOT, APP, OUT, flags)
    if not only or 'suites' in only:
        suites(ROOT, APP, OUT, host)
    if not only or 'upstream' in only:
        upstream(ROOT, APP, OUT, host)
    if not only or 'model' in only:
        model(ROOT, APP, OUT, host)


if __name__ == '__main__':
    ROOT = Path(__file__).resolve().parents[1]
    APP = ROOT / 'build/radiofoundry-p25'
    OUT = ROOT / 'build/classic-host'
    OUT.mkdir(parents=True, exist_ok=True)
    host = APP / 'tests/host'
    if not (OUT / 'classic_dispatch.inc').is_file():
        # as tools/test_classic.py writes it: the generated radio task's service / retained-request dispatch
        task = (APP / 'core/radio/radio_task.c').read_text()
        first = task.index('\tfor (;;) {') + len('\tfor (;;) {')
        service = task[first:task.index('\n\t\tTickType_t due', first)]
        first = task.index('        if (retained_request ||')
        dispatch = task[first:task.index('\n\t\tTickType_t now', first)]
        (OUT / 'classic_dispatch.inc').write_text(service + '\n' + dispatch + '\n')
    flags = ['-std=gnu99', '-Wall', '-Wextra', '-Wno-unused-parameter', '-g',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer',
             '-I' + str(APP / 'core'), '-I' + str(APP / 'drivers'), '-I' + str(APP / 'hal'), '-I' + str(host),
             '-I' + str(ROOT / 'third_party/p25cai/include'),
             '-D__attribute__(x)=', '-DCONFIG_P25_CHANNELS=1', '-DCONFIG_P25_NATIVE=1', '-DCONFIG_P25=1']
    main(ROOT, APP, OUT, flags, host, sys.argv[1:] or None)
