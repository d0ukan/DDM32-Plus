# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the round 14 APRS job (tools/plus_aprs.py, docs/plus.md), run by tools/test_classic.py after its own
(the build tree prepared, ASan / UBSan flags given):

- item 47, Mic-E beacons: the upstream radio-core APRS suite again with CONFIG_APRS_MICE, then the encoder against the
  engine's parser over a grid of messages, positions, speeds, courses and altitudes, the beacons on the air through
  the fake radio (manual, automatic, SmartBeaconing, APRS-IS) and the setting's storage (tests/plus/test_aprs_mice.c);
- the screens through upstream's UI simulator (tests/plus/test_ui_aprsx.c, tests/plus/ui_sim_aprsx.c): Settings >
  APRS; the APRS station page with Go-To, the map and a message; the SOS key's page with nothing chosen; English and
  Turkish (the tables made from tr.po), every page's layout checked."""
import shlex
import subprocess


def run(args, **kw):
    import refpack_host	# the map pack's reader, its fixture and the tables (tools/refpack_host.py)
    return subprocess.run([str(a) for a in refpack_host.patch([str(a) for a in args])], check=True, **kw)


def main(ROOT, APP, OUT, flags, host):
    tp = ROOT / 'tests/plus'
    ui = APP / 'core/ui'
    # Mic-E beacons through the engine and the fake radio (built like aprsRadioTest)
    command = run(['make', '-Bn', 'aprsRadioTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o aprsRadioTest ' in line))
    argv[argv.index('aprsRadioTest.c')] = str(tp / 'test_aprs_mice.c')
    argv[argv.index('-o') + 1] = str(OUT / 'test_aprs_mice')
    run([*argv, '-I' + str(host), '-DCONFIG_APRS_MSG=1', '-DCONFIG_APRS_MICE=1', str(APP / 'core/aprs/aprs_plus.c')],
        cwd=host)
    run([OUT / 'test_aprs_mice'], cwd=host)
    # The screens: upstream's simulator, English and Turkish
    run(['make', 'out/gen/lang_tables.h'], cwd=host, stdout=subprocess.DEVNULL)
    command = run(['make', '-Bn', 'uiFlowTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')] = str(tp / 'test_ui_aprsx.c')
    argv[argv.index('sim/ui_sim.c')] = str(tp / 'ui_sim_aprsx.c')
    argv[argv.index('-o') + 1] = str(OUT / 'test_ui_aprsx')
    run([*argv, '-I' + str(host), '-I' + str(tp), '-DCONFIG_LANG_TR=1', '-DCONFIG_P25_CHANNELS=1', '-DCONFIG_APRS=1',
         '-DCONFIG_APRS_MSG=1', '-DCONFIG_APRS_MICE=1', '-DCONFIG_APRS_RX=1', '-DCONFIG_MAP=1', '-DCONFIG_NAV=1',
         '-DCONFIG_NAV_FOLLOW=1', '-DCONFIG_APRS_STATION=1', '-Wno-unused-function',
         '-DCONFIG_SOS_KEY=1', '-DCONFIG_DMR_EMERGENCY=1',
         *(str(ui / f) for f in ('sos.c', 'ui_sos.c', 'ui_aprsmsg.c', 'ui_aprsx.c', 'ui_scratch.c', 'ui_map.c', 'map.c', 'turkey_map_data.c',
                                 'turkey_district_data.c', 'mapnav.c', 'nav.c', 'ui_goto.c', 'follow.c')),
         *(str(APP / 'core/aprs' / f) for f in ('aprs_plus.c', 'aprs_fmt.c', 'ax25.c')), '-lm'], cwd=host)
    run([OUT / 'test_ui_aprsx'], cwd=host)
