# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the round 13 "ideas 1" job (tools/plus_fieldx.py, docs/plus.md: search finds saved as channels, find
my friend, the locator calculator, the one-key SOS), run by tools/test_classic.py after its own (the build tree
prepared, ASan / UBSan flags given):

- the pure parts (tests/plus/test_findsave.c, test_follow.c, test_locator.c), each built with and without the
  options they depend on;
- the SOS state machine against recording fakes (tests/plus/test_sos.c: nothing asked before the confirmation or
  with every action off; the order; Stop), with and without the CW beacon;
- the SOS key function in the side key assignment (tests/plus/test_sos_key.c, core/input/keyassign.c);
- the four items' UI flows in upstream's simulator (tests/plus/test_ui_fieldx.c, tests/plus/ui_sim_fieldx.c)."""
import shlex
import subprocess


def run(args, **kw):
    import refpack_host	# the map pack's reader, its fixture and the tables (tools/refpack_host.py)
    return subprocess.run([str(a) for a in refpack_host.patch([str(a) for a in args])], check=True, **kw)


def main(ROOT, APP, OUT, flags, host):
    tp = ROOT / 'tests/plus'
    ui = APP / 'core/ui'
    units = [
        ('test_findsave', ['-DCONFIG_FIND_SAVE=1', '-DCONFIG_DMR_ANY=1'], [ui / 'findsave.c']),
        ('test_findsave', ['-DCONFIG_FIND_SAVE=1', '-DCONFIG_DMR_ANY=0'], [ui / 'findsave.c']),
        ('test_follow', ['-DCONFIG_NAV_FOLLOW=1'], [ui / 'follow.c']),
        ('test_locator', ['-DCONFIG_LOCATOR=1', '-DCONFIG_NAV=1', '-DCONFIG_APRS=1'],
         [ui / 'locator.c', ui / 'nav.c', APP / 'core/aprs/aprs_fmt.c', '-lm']),
        ('test_sos', ['-DCONFIG_SOS_KEY=1', '-DCONFIG_DMR_EMERGENCY=1', '-DCONFIG_APRS_MSG=1', '-DCONFIG_CW_BEACON=1'],
         [ui / 'sos.c']),
        ('test_sos', ['-DCONFIG_SOS_KEY=1', '-DCONFIG_DMR_EMERGENCY=1', '-DCONFIG_APRS_MSG=1', '-DCONFIG_CW_BEACON=0'],
         [ui / 'sos.c']),
        ('test_sos_key', ['-DCONFIG_KEY_ASSIGN=1', '-DCONFIG_SOS_KEY=1'], [APP / 'core/input/keyassign.c']),
        ('test_sos_key', ['-DCONFIG_KEY_ASSIGN=1', '-DCONFIG_SOS_KEY=0'], [APP / 'core/input/keyassign.c']),
    ]
    for i, (name, defs, sources) in enumerate(units):
        exe = OUT / f'{name}_{i}'
        run(['clang', *flags, *defs, tp / (name + '.c'), *sources, '-o', exe], cwd=ROOT)
        run([exe], cwd=ROOT)
    command = run(['make', '-Bn', 'uiFlowTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')] = str(tp / 'test_ui_fieldx.c')
    argv[argv.index('sim/ui_sim.c')] = str(tp / 'ui_sim_fieldx.c')
    argv[argv.index('-o') + 1] = str(OUT / 'test_ui_fieldx')
    run([*argv, '-I' + str(host), '-I' + str(tp), '-DCONFIG_P25_CHANNELS=1', '-DCONFIG_MAP=1', '-DCONFIG_NAV=1',
         '-DCONFIG_REPEATERS=1', '-DCONFIG_APRS_MSG=1', '-DCONFIG_P25_SEARCH=1', '-DCONFIG_DMR_SEARCH=1',
         '-DCONFIG_DMR_ANY=1', '-DCONFIG_FIND_SAVE=1', '-DCONFIG_NAV_FOLLOW=1', '-DCONFIG_LOCATOR=1',
         '-DCONFIG_SOS_KEY=1',
         *(str(ui / f) for f in ('ui_map.c', 'map.c', 'turkey_map_data.c', 'turkey_district_data.c', 'mapnav.c',
                                 'nav.c', 'ui_goto.c', 'rpt.c', 'ui_rpt.c', 'ui_aprsmsg.c', 'ui_p25search.c',
                                 'ui_dmrsearch.c', 'findsave.c', 'ui_findsave.c', 'follow.c', 'locator.c',
                                 'ui_locator.c', 'sos.c', 'ui_sos.c', 'ui_scratch.c')),
         *(str(APP / 'core/aprs' / f) for f in ('aprs_plus.c', 'aprs_fmt.c', 'ax25.c')), '-lm'], cwd=host)
    run([OUT / 'test_ui_fieldx'], cwd=host)
