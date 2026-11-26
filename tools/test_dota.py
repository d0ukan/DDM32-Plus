# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of DOTA Transfer (tools/plus_dota.py, docs/dota.md 16.1), run by tools/test_classic.py after its own (the
build tree prepared, ASan / UBSan flags given): the protocol, the records, the import, two whole sessions, and the
screens in the UI simulator."""
import shlex
import subprocess


def run(args, **kw):
    import refpack_host	# the map pack's reader, its fixture and the tables (tools/refpack_host.py)
    return subprocess.run([str(a) for a in refpack_host.patch([str(a) for a in args])], check=True, **kw)


def main(ROOT, APP, OUT, flags, host):
    tp = ROOT / 'tests/plus'
    core = APP / 'core'
    codeplug = [core / 'codeplug' / f for f in ('codeplug.c', 'codeplug_kv.c', 'codeplug_tag.c', 'codeplug_records.c',
                                                'codeplug_talk.c')] + [core / 'p25/p25_codeplug.c']
    dota = ['-DCONFIG_DOTA=1', '-DCONFIG_DMR_ANY=1', '-DCONFIG_YSF=1', '-DCONFIG_NXDN=1', '-DCONFIG_DPMR=1']
    # The protocol: frames, the manifest, both state machines over a lossy, duplicating, reordering fake link
    run(['clang', *flags, *dota, '-DCONFIG_APRS=1', '-I' + str(host), tp / 'test_dota_proto.c',
         core / 'radio/dota_proto.c', core / 'aprs/ax25.c', core / 'storage/storage.c',
         host / 'sim/sim_flash.c', '-o', OUT / 'test_dota_proto'], cwd=ROOT)
    run([OUT / 'test_dota_proto'], cwd=ROOT)
    # The records: every mode and field through encoder and decoder, refusals, fuzzing
    run(['clang', *flags, *dota, tp / 'test_dota_rec.c', core / 'radio/dota_rec.c', '-o', OUT / 'test_dota_rec'],
        cwd=ROOT)
    run([OUT / 'test_dota_rec'], cwd=ROOT)
    # The import on the real codeplug over a simulated flash: staging unseen, power cuts, the commit, the undo
    run(['clang', *flags, *dota, '-I' + str(host), tp / 'test_dota_import.c', core / 'radio/dota_import.c',
         core / 'radio/dota_rec.c', *codeplug, core / 'codeplug/zone_edit.c', core / 'codeplug/zone_shift.c',
         core / 'codeplug/zone_settle.c', core / 'settings/settings.c', core / 'storage/storage.c',
         host / 'sim/sim_flash.c', '-o', OUT / 'test_dota_import'], cwd=ROOT)
    run([OUT / 'test_dota_import'], cwd=ROOT)
    # The same with the zone editor's All channels join (CONFIG_CHAN_EXTRAS: zone_join.inc): an import's channels are in
    # All channels too
    run(['clang', *flags, *dota, '-DCONFIG_CHAN_EXTRAS=1', '-I' + str(host), tp / 'test_dota_import.c',
         core / 'radio/dota_import.c', core / 'radio/dota_rec.c', *codeplug, core / 'codeplug/zone_edit.c',
         core / 'codeplug/zone_shift.c', core / 'codeplug/zone_settle.c', core / 'settings/settings.c',
         core / 'storage/storage.c', host / 'sim/sim_flash.c', '-o', OUT / 'test_dota_import_all'], cwd=ROOT)
    run([OUT / 'test_dota_import_all'], cwd=ROOT)
    # Two whole sessions talking (dota.c twice: a_* sends, b_* receives) over fake FM and DMR transports
    api = ('dota_selection', 'dota_on', 'dota_start', 'dota_stop', 'dota_view', 'dota_ptt_prompt_clear',
           'dota_job_wanted', 'dota_job', 'dota_begin', 'dota_owns_radio',
           'dota_ptt', 'dota_fm_listening', 'dota_fm_frame', 'dota_dmr_rx', 'dota_poll', 'dota_scratch')
    for side in ('a', 'b'):
        run(['clang', *flags, *dota, '-DCONFIG_APRS=1', '-DCONFIG_DMR_DATA_CORE=1', '-I' + str(host),
             *(f'-D{f}={side}_{f}' for f in api), '-c', core / 'radio/dota.c', '-o', OUT / f'dota_{side}.o'], cwd=ROOT)
    run(['clang', *flags, *dota, '-DCONFIG_APRS=1', '-DCONFIG_DMR_DATA_CORE=1', '-I' + str(host),
         tp / 'test_dota_session.c', OUT / 'dota_a.o', OUT / 'dota_b.o', core / 'radio/dota_proto.c',
         core / 'radio/dota_rec.c', core / 'radio/dota_import.c', *codeplug, core / 'codeplug/zone_edit.c',
         core / 'codeplug/zone_shift.c', core / 'codeplug/zone_settle.c', core / 'settings/settings.c',
         core / 'storage/storage.c', host / 'sim/sim_flash.c', core / 'aprs/ax25.c', core / 'aprs/hdlc.c',
         '-o', OUT / 'test_dota_session'], cwd=ROOT)
    run([OUT / 'test_dota_session'], cwd=ROOT)
    # FM end to end through audio: the AFSK modulator and demodulator, CTCSS on the air, noise, sample slips
    run(['clang', *flags, *dota, '-DCONFIG_APRS=1', '-DCONFIG_DMR_DATA_CORE=1', '-I' + str(host),
         tp / 'test_dota_fm.c', OUT / 'dota_a.o', OUT / 'dota_b.o', core / 'radio/dota_proto.c',
         core / 'radio/dota_rec.c', core / 'radio/dota_import.c', *codeplug, core / 'codeplug/zone_edit.c',
         core / 'codeplug/zone_shift.c', core / 'codeplug/zone_settle.c', core / 'settings/settings.c',
         core / 'storage/storage.c', host / 'sim/sim_flash.c', core / 'aprs/ax25.c', core / 'aprs/hdlc.c',
         core / 'aprs/afsk.c', '-lm', '-o', OUT / 'test_dota_fm'], cwd=ROOT)
    run([OUT / 'test_dota_fm'], cwd=ROOT)
    # A user data pack (docs/dota.md 20): dota.c built with CONFIG_DATAPACK twice (a_ sends, b_ receives), the real
    # pack store over a simulated 16 MiB data chip; c_: a receiver built without data packs
    pack = ['-DCONFIG_DATAPACK=1', '-DCONFIG_APRS=1', '-DCONFIG_DMR_DATA_CORE=1', '-I' + str(host)]
    for side, on in (('a', True), ('b', True), ('c', False)):
        run(['clang', *flags, *dota, *(pack if on else pack[1:]),
             *(f'-D{f}={side}_{f}' for f in (*api, 'dota_pack_offer', 'dota_pack_answer')), '-c',
             core / 'radio/dota.c', '-o', OUT / f'dota_pack_{side}.o'], cwd=ROOT)
    run(['clang', *flags, *dota, *pack, tp / 'test_dota_pack.c', *(OUT / f'dota_pack_{s}.o' for s in 'abc'),
         core / 'radio/dota_proto.c', core / 'radio/dota_rec.c', core / 'radio/dota_import.c', *codeplug,
         core / 'codeplug/zone_edit.c', core / 'codeplug/zone_shift.c', core / 'codeplug/zone_settle.c',
         core / 'settings/settings.c', core / 'storage/storage.c', core / 'storage/datapack.c', core / 'ui/fmt.c',
         host / 'sim/sim_flash.c', core / 'aprs/ax25.c', core / 'aprs/hdlc.c', '-o', OUT / 'test_dota_pack'], cwd=ROOT)
    run([OUT / 'test_dota_pack'], cwd=ROOT)
    # The screens through upstream's UI simulator (the engine faked), English and Turkish (the tables made from tr.po)
    run(['make', 'out/gen/lang_tables.h'], cwd=host, stdout=subprocess.DEVNULL)
    command = run(['make', '-Bn', 'uiFlowTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')] = str(tp / 'test_ui_dota.c')
    argv[argv.index('-o') + 1] = str(OUT / 'test_ui_dota')
    # (the frequency's checks are the cross-band's band plan: radio/xband.c and ui_xband.c built without CONFIG_XBAND
    # hold just those)
    run([*argv, '-I' + str(host), '-I' + str(core), '-Wno-unused-function', '-DCONFIG_DOTA=1', '-DCONFIG_APRS_MSG=1',
         '-DCONFIG_APRS=1', '-DCONFIG_DATAPACK=1',
         '-DCONFIG_P25_CHANNELS=1', '-DCONFIG_LANG_TR=1', str(core / 'ui/ui_dota.c'), str(core / 'ui/ui_aprsmsg.c'),
         str(core / 'ui/ui_xband.c'), str(core / 'radio/xband.c'),
         *(str(core / 'aprs' / f) for f in ('aprs_plus.c', 'aprs_fmt.c', 'ax25.c')), '-lm'], cwd=host)
    run([OUT / 'test_ui_dota'], cwd=host)
