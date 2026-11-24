# SPDX-License-Identifier: GPL-3.0-or-later
"""Host test of the DDM32 Plus P25 RX group list (tools/plus_p25rxgl.py, docs/rx-group-list-budget.md), run by
tools/test_classic.py after its own (the build tree prepared, ASan / UBSan flags given): the real codeplug over a
simulated flash (no radio), the loader and the filter rule, the worker faked. The admission itself is in
tools/test_voice.py (tests/test_voice.c built with CONFIG_P25_RXGL=1)."""
import subprocess


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def main(ROOT, APP, OUT, flags, host):
    CODEPLUG = [APP / 'core/codeplug' / f for f in ('codeplug.c', 'codeplug_kv.c', 'codeplug_tag.c', 'codeplug_records.c',
                                                    'codeplug_talk.c')] + [APP / 'core/p25/p25_codeplug.c']
    run(['clang', *flags, '-DCONFIG_P25_RXGL=1', '-DCONFIG_DMR_ANY=1', '-DCONFIG_DEV_TX=1',
         ROOT / 'tests/plus/test_p25rxgl.c', *CODEPLUG, APP / 'core/storage/storage.c', host / 'sim/sim_flash.c',
         '-o', OUT / 'test_p25rxgl'], cwd=ROOT)
    run([OUT / 'test_p25rxgl'], cwd=ROOT)
