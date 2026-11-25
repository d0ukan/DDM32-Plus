# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the round 14 "xband" job (tools/plus_xband.py, docs/plus.md "Last calls"), run by tools/test_classic.py
after its own (the build tree prepared, ASan / UBSan flags given):

- last calls, the portable part (tests/plus/test_lastcall.c: lastcall.c);
- last calls through upstream's UI simulator (tests/plus/test_ui_lastcall.c), English and Turkish: every mode, every
  watched letter, the P25 replay's row, the layout;
- cross-band store and forward: the rules and the engine on a fake radio core (tests/plus/test_xband.c), and Tools >
  Cross-band through the UI simulator with the real engine (tests/plus/test_ui_xband.c), English and Turkish. The
  radio core's side (tests/plus/test_xband_radio.inc) runs in tools/test_classic.py with the other radio core suites."""
import shlex
import subprocess


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def ui_argv(host, test, out):
    """upstream's uiFlowTest command with test in place of uiFlowTest.c"""
    command = run(['make', '-Bn', 'uiFlowTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')] = str(test)
    argv[argv.index('-o') + 1] = str(out)
    return argv


def main(ROOT, APP, OUT, flags, host):
    tp = ROOT / 'tests/plus'
    ui = APP / 'core/ui'
    # Last calls (item 43): the list's rules
    run(['clang', *flags, '-DCONFIG_LAST_CALLS=1', tp / 'test_lastcall.c', ui / 'lastcall.c', '-o',
         OUT / 'test_lastcall'], cwd=ROOT)
    run([OUT / 'test_lastcall'], cwd=ROOT)
    # ... and its screen with every feed, in English and Turkish (the tables made from tr.po)
    run(['make', 'out/gen/lang_tables.h'], cwd=host, stdout=subprocess.DEVNULL)
    argv = ui_argv(host, tp / 'test_ui_lastcall.c', OUT / 'test_ui_lastcall')
    # (-Wno-unused-function: ui_app.c's call_log_add callers for P25 / YSF / NXDN are the radio task's feeds, not built)
    run([*argv, '-I' + str(host), '-I' + str(APP / 'core'), '-Wno-unused-function', '-DCONFIG_P25_CHANNELS=1',
         '-DCONFIG_DEV_TX=1', '-DCONFIG_CALL_LOG=1', '-DCONFIG_CALL_REPLAY=1', '-DCONFIG_LAST_CALLS=1',
         '-DCONFIG_MULTI_WATCH=1', '-DCONFIG_FM_ID=1', '-DCONFIG_LANG_TR=1', str(ui / 'lastcall.c'),
         str(ui / 'ui_lastcall.c')], cwd=host)
    run([OUT / 'test_ui_lastcall'], cwd=host)
    # Cross-band store and forward (item 46): the rules and the engine on a fake radio core
    run(['clang', *flags, '-DCONFIG_XBAND=1', tp / 'test_xband.c', APP / 'core/radio/xband.c', '-o',
         OUT / 'test_xband'], cwd=ROOT)
    run([OUT / 'test_xband'], cwd=ROOT)
    # ... and Tools > Cross-band through the real UI with the real engine, English and Turkish (the radio core's side:
    # tests/plus/test_xband_radio.inc, run by tools/test_classic.py with the other radio core suites)
    argv = ui_argv(host, tp / 'test_ui_xband.c', OUT / 'test_ui_xband')
    run([*argv, '-I' + str(host), '-I' + str(APP / 'core'), '-DCONFIG_P25_CHANNELS=1', '-DCONFIG_DEV_TX=1',
         '-DCONFIG_XBAND=1', '-DCONFIG_REPEATERS=1', '-DCONFIG_SFR_CWID=1', '-DCONFIG_SUBTONE=1', '-DCONFIG_LANG_TR=1',
         str(ui / 'ui_xband.c'), str(APP / 'core/radio/radio_cwid.c'),	# (tones.c: in uiFlowTest's own sources)
         str(ui / 'ui_scratch.c'), str(ui / 'rpt.c'), str(ui / 'ui_rpt.c'), str(APP / 'core/radio/xband.c'),
         str(APP / 'core/aprs/aprs_fmt.c'), '-lm'], cwd=host)
    run([OUT / 'test_ui_xband'], cwd=host)
