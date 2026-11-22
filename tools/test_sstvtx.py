# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of DDM32 Plus SSTV send (tools/plus_sstvtx.py, docs/plus.md "SSTV send"), run by tools/test_classic.py
after its own (the build tree prepared, ASan / UBSan flags given):

- the test picture's audio (tests/plus/test_sstvtx.c: sstv_tx.c): the card, the timing, the phase, the VIS, and a
  round trip through the SSTV receiver (sstv.c) for Robot 36, Martin M1 and PD120;
- the page through upstream's UI simulator (tests/plus/test_ui_sstvtx.c), English and Turkish;
- the radio core's side (tests/plus/test_sstvtx_radio.inc, radio()): called by tools/test_classic.py with the other
  radio core suites."""
import shlex
import subprocess


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def main(ROOT, APP, OUT, flags, host):
    tp = ROOT / 'tests/plus'
    core = APP / 'core'
    art = ['-DCONFIG_PLUS_ART=1', core / 'ui/art_data.c']
    # The picture's audio, decoded again by the SSTV receiver
    run(['clang', *flags, '-O1', '-DCONFIG_SSTV=1', '-DCONFIG_SSTV_TX=1', '-I' + str(core / 'radio'),
         tp / 'test_sstvtx.c', core / 'radio/sstv_tx.c', core / 'radio/sstv.c', *art, '-lm', '-o', OUT / 'test_sstvtx'],
        cwd=ROOT)
    run([OUT / 'test_sstvtx'], cwd=ROOT)
    # The page through the real UI, English and Turkish (the tables made from tr.po); the band plan is the cross-band's
    # (radio/xband.c and ui_xband.c built for SSTV send alone hold just that)
    run(['make', 'out/gen/lang_tables.h'], cwd=host, stdout=subprocess.DEVNULL)
    command = run(['make', '-Bn', 'uiFlowTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')] = str(tp / 'test_ui_sstvtx.c')
    argv[argv.index('-o') + 1] = str(OUT / 'test_ui_sstvtx')
    run([*argv, '-I' + str(host), '-I' + str(core), '-DCONFIG_P25_CHANNELS=1', '-DCONFIG_DEV_TX=1',
         '-DCONFIG_MSG_RX=1', '-DCONFIG_SSTV=1', '-DCONFIG_SSTV_TX=1', '-DCONFIG_LANG_TR=1',
         *(str(core / 'ui' / f) for f in ('ui_sstvtx.c', 'ui_sstv.c', 'ui_msg.c', 'ui_scratch.c', 'ui_xband.c')),
         *(str(core / 'radio' / f) for f in ('xband.c', 'radio_cwid.c', 'sstv.c', 'sstv_tx.c'))], cwd=host)
    run([OUT / 'test_ui_sstvtx'], cwd=host)


def radio(argv_cw, cwrad, fixture, ROOT, APP, OUT, host):
    """The radio core with SSTV send built in: upstream's and Classic's radio suites again, then the picture keyed,
    played and stopped every way (tests/plus/test_sstvtx_radio.inc). argv_cw / cwrad: tools/test_classic.py's CW
    practice radio suite command and its wrapper, swapped for this one's."""
    wrap = OUT / 'radioCoreSstvtx.c'
    wrap.write_text('#include "native_modem/native_lab.h"\nextern const nt_ops classic_ops;\n' +
                    fixture.replace('const board_t board = {\n\t.native_modem=&classic_ops,',
                                    'extern const hal_afsk_t sstvtx_afsk;\nconst board_t board = {\n'
                                    '\t.native_modem=&classic_ops,\n\t.afsk=&sstvtx_afsk,') +
                    '\n#define main classic_radio_main\n#include "' + str(ROOT / 'tests/classic/test_radio.inc') +
                    '"\n#undef main\n#include "' + str(ROOT / 'tests/plus/test_sstvtx_radio.inc') + '"\n')
    argv = list(argv_cw)
    argv[argv.index(str(cwrad))] = str(wrap)
    argv[argv.index('-o') + 1] = str(OUT / 'test_radio_sstvtx')
    core = APP / 'core'
    run([*argv, '-DCONFIG_SSTV_TX=1', '-DCONFIG_PLUS_ART=1', '-DCONFIG_CW_BEACON=1', str(core / 'radio/cw_beacon.c'), '-I' + str(ROOT / 'tests/classic'),
         '-DpvPortMalloc=__builtin_malloc', '-DvPortFree=__builtin_free',
         str(core / 'radio/radio_sstvtx.c'), str(core / 'radio/sstv_tx.c'), str(core / 'radio/xband.c'),
         str(core / 'ui/art_data.c')], cwd=host)
    run([OUT / 'test_radio_sstvtx'], cwd=host)
