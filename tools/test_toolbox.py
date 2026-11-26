# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the round 13 "ideas 2" job (tools/plus_toolbox.py, docs/plus.md: the shared tool screen buffer, the
heard list, the spectrum waterfall, map coverage, the GPS dashboard, the clock tools), run by tools/test_classic.py
after its own (the build tree prepared, ASan / UBSan flags given):

- the portable parts (tests/plus/test_toolbox.c: heard.c, wfall.c, tripm.c, clockx.c, mapnav.c's coverage);
- the flows through upstream's UI simulator (tests/plus/test_ui_toolbox.c, tests/plus/ui_sim_toolbox.c): the six tool
  screens sharing their buffer (round trips between them, a find save stopped by another screen), and each new
  screen, with the waterfall's heap counted (UI_HOST_HEAP) and its allocation failing on request."""
import shlex
import subprocess


def run(args, **kw):
    import refpack_host	# the map pack's reader, its fixture and the tables (tools/refpack_host.py)
    return subprocess.run([str(a) for a in refpack_host.patch([str(a) for a in args])], check=True, **kw)


def main(ROOT, APP, OUT, flags, host):
    tp = ROOT / 'tests/plus'
    ui = APP / 'core/ui'
    exe = OUT / 'test_toolbox'
    run(['clang', *flags, '-DCONFIG_HEARD=1', '-DCONFIG_SPECTRUM_WF=1', '-DCONFIG_MAP=1', '-DCONFIG_MAP_COVERAGE=1',
         '-DCONFIG_GPS_DASH=1', '-DCONFIG_CLOCK_TOOLS=1', tp / 'test_toolbox.c',
         *(ui / f for f in ('heard.c', 'wfall.c', 'tripm.c', 'clockx.c', 'mapnav.c', 'map.c', 'turkey_map_data.c',
                            'turkey_district_data.c')), '-lm', '-o', exe], cwd=ROOT)
    run([exe], cwd=ROOT)
    run(['make', 'out/gen/lang_tables.h'], cwd=host, stdout=subprocess.DEVNULL)
    command = run(['make', '-Bn', 'uiFlowTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')] = str(tp / 'test_ui_toolbox.c')
    argv[argv.index('sim/ui_sim.c')] = str(tp / 'ui_sim_toolbox.c')
    argv[argv.index('-o') + 1] = str(OUT / 'test_ui_toolbox')
    # (-Wno-unused-function: ui_app.c's call_log_add, whose callers are the P25 / YSF / NXDN / dPMR / FM ID feeds not
    # built here)
    run([*argv, '-I' + str(host), '-I' + str(tp), '-Wno-unused-function', '-DUI_HOST_HEAP','-DCONFIG_P25_CHANNELS=1', '-DCONFIG_MAP=1',
         '-DCONFIG_NAV=1', '-DCONFIG_REPEATERS=1', '-DCONFIG_MAP_TRACK=1', '-DCONFIG_MAP_COVERAGE=1',
         '-DCONFIG_SPECTRUM=1', '-DCONFIG_SPECTRUM_WF=1', '-DCONFIG_P25_SEARCH=1', '-DCONFIG_DMR_SEARCH=1',
         '-DCONFIG_DMR_ANY=1', '-DCONFIG_FIND_SAVE=1', '-DCONFIG_P25_TRUNK=1', '-DCONFIG_DMR_TRUNK=1',
         '-DCONFIG_MSG_RX=1', '-DCONFIG_CALL_LOG=1', '-DCONFIG_HEARD=1', '-DCONFIG_VFO_MODES=1', '-DCONFIG_GPS_DASH=1',
         '-DCONFIG_CLOCK_TOOLS=1', '-DCONFIG_NAC_DEC=1', '-DCONFIG_ALMANAC=1', '-DCONFIG_LANG_TR=1',
         *(str(ui / f) for f in ('ui_map.c', 'map.c', 'turkey_map_data.c', 'turkey_district_data.c', 'mapnav.c',
                                 'ui_mapnav.c', 'nav.c', 'ui_goto.c', 'rpt.c', 'ui_rpt.c', 'ui_spectrum.c',
                                 'ui_p25search.c', 'ui_dmrsearch.c', 'ui_p25trunk.c', 'ui_dmrtrunk.c', 'ui_msg.c',
                                 'ui_scratch.c', 'findsave.c', 'ui_findsave.c', 'heard.c', 'ui_heard.c', 'wfall.c',
                                 'tripm.c', 'ui_gpsdash.c', 'clockx.c', 'ui_clock.c', 'almanac.c', 'ui_almanac.c')),
         str(APP / 'core/radio/spectrum.c'), str(APP / 'core/radio/vfox.c'),
         *(str(APP / 'core/aprs' / f) for f in ('aprs_fmt.c',)), '-lm'], cwd=host)
    run([OUT / 'test_ui_toolbox'], cwd=host)
