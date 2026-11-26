# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the user data packs (tools/plus_datapack.py, docs/r14.md 7), run by tools/test_classic.py after its
own (the build tree prepared, ASan / UBSan flags given): the store on the simulated data chip (the region guard, the
slots, power cuts), the RadioID pack's lookup and the About line,
the air band pack over the shipped airfield table."""
import subprocess


def run(args, **kw):
    import refpack_host	# the map pack's reader, its fixture and the tables (tools/refpack_host.py)
    return subprocess.run([str(a) for a in refpack_host.patch([str(a) for a in args])], check=True, **kw)


def main(ROOT, APP, OUT, flags, host):
    tp = ROOT / 'tests/plus'
    core = APP / 'core'
    pack = ['-DCONFIG_DATAPACK=1', '-DCONFIG_AIRFIELDS=1', '-DCONFIG_APRS=1']
    # The store: guard, slots, sequence, broken slots, a power cut at every flash operation
    run(['clang', *flags, *pack, '-I' + str(host), tp / 'test_datapack.c', core / 'storage/datapack.c',
         core / 'storage/storage.c', core / 'ui/fmt.c', host / 'sim/sim_flash.c',
         *(core / 'ui' / f for f in ('air_pack.c', 'airfield.c', 'airfield_data.c')), core / 'aprs/aprs_fmt.c', '-lm',
         '-o', OUT / 'test_datapack'], cwd=ROOT)
    images = builder_tests(ROOT, OUT)
    run([OUT / 'test_datapack', *images], cwd=ROOT)
    refpack_tests(ROOT, APP, OUT, flags, host)
    ui_tests(ROOT, APP, OUT, host)


def refpack_tests(ROOT, APP, OUT, flags, host):
    """The map pack (docs/mappack-design.md G.1): T3 tools/refpack.py's payload equals the C serialisation of the
    compiled tables, section by section, and is deterministic; T4, T5, T5b, T5c, T6-T8, T10 on the simulated chip
    (tests/plus/test_refpack.c); T5d / T14 tools/datapack.py's slot choice and flags carry"""
    import struct
    import sys
    sys.path.insert(0, str(ROOT))
    from tools import datapack as dp, refpack
    tp = ROOT / 'tests/plus'
    core = APP / 'core'
    cfg = ['-DCONFIG_DATAPACK=1', '-DCONFIG_MAP=1', '-DCONFIG_REPEATERS=1', '-DCONFIG_AIRFIELDS=1', '-DCONFIG_APRS=1']
    common = [core / 'ui/map.c', core / 'ui/rpt.c', core / 'ui/airfield.c', core / 'aprs/aprs_fmt.c',
              ROOT / 'integration/radiofoundry/map/turkey_map_data.c',
              ROOT / 'integration/radiofoundry/map/turkey_district_data.c',
              ROOT / 'integration/radiofoundry/air/airfield_data.c', ROOT / 'build/rpt/rpt_data.c']
    run(['clang', *flags, *cfg, '-I' + str(core / 'ui'), tp / 'test_refpack.c', *common, '-lm', '-o',
         OUT / 'test_refpack'], cwd=ROOT)
    run([OUT / 'test_refpack'], cwd=ROOT)
    # T3: the same bytes from Python and from C
    run(['clang', *flags, *cfg, '-I' + str(core / 'ui'), tp / 'refpack_dump.c', *common, '-lm', '-o',
         OUT / 'refpack_dump'], cwd=ROOT)
    c_bytes = OUT / 'refpack_c.bin'
    run([OUT / 'refpack_dump', c_bytes], cwd=ROOT)
    rpt = refpack.load_rpt_c(ROOT / 'build/rpt/rpt_data.c')
    m, a = refpack.load_map(), refpack.load_air()
    secs = refpack.sections(m, a, rpt)
    py = refpack.payload(m['k'], secs)
    cb = c_bytes.read_bytes()
    for i, rec, off, count in refpack.directory(py):
        c_ent = [e for e in refpack.directory(cb) if e[0] == i]
        assert c_ent and c_ent[0][1:] == (rec, off, count), (i, c_ent)
        assert cb[off:off + rec * count] == py[off:off + rec * count], f'section {i} ({refpack.NAMES[i]}) differs'
    assert py == cb and refpack.payload(m['k'], refpack.sections(refpack.load_map(), refpack.load_air(), rpt)) == py
    assert refpack.validate(py)[0] == 'ok'
    img = refpack.build('9.9.9', rpt=rpt)
    assert img == refpack.build('9.9.9', rpt=rpt) and img[64:] == py and dp.parse(img)[0]['flags'] == refpack.hint(m['k'])
    print(f'  refpack.py: {len(py)}-byte payload equal to the C tables section by section, deterministic')
    # the Python rules agree with the firmware's on a few broken packs (the C test has them all)
    bad = bytearray(py)
    ent = [e for e in refpack.directory(py) if e[0] == refpack.STEP][0]
    struct.pack_into('<I', bad, refpack.HEAD + refpack.ENTRY * refpack.directory(py).index(ent) + 8, 40000)
    assert refpack.validate(bytes(bad))[0] == 'bad' and refpack.validate(py, refpack.hint(m['k']) ^ 0x100)[0] == 'other'
    bad = bytearray(py)
    bad[4] = 2
    assert refpack.validate(bytes(bad))[0] == 'other' and refpack.validate(py, 0)[0] == 'ok'
    # T5d / T14 (Python side): the slot a writer takes, the sequence, the flags carried into the header it rebuilds
    good = img
    empty = b'\xff' * 64
    other = bytearray(img[64:])
    other[4] = 2
    other_img = dp.header(dp.REF, bytes(other), '20261119', 'x', seq=9, flags=refpack.hint(m['k'])) + bytes(other)
    old = dp.reseq(good, 3)
    assert dp.inactive_slot(dp.REF, [empty, empty]) == 0
    assert dp.inactive_slot(dp.REF, [old, other_img]) == 1		# the newer OTHER is overwritten, not the valid one
    assert dp.next_seq([old, other_img]) == 10			# above both
    assert dp.ref_write_slot([old, None]) is None			# a slot not read in full: nothing written
    # across versions: the running firmware's slot (here by CRC only) is kept even when the target rejects both
    other_old = dp.header(dp.REF, bytes(other), '20261119', 'x', seq=2, flags=refpack.hint(m['k'])) + bytes(other)
    assert dp.ref_write_slot([other_old, other_img]) == 0 and dp.ref_write_slot([old, empty]) == 1
    # flags: a hint survives the writer's new sequence byte for byte; 0 (an older writer) is valid; a wrong one is not
    re = dp.reseq(good, 7)
    assert re[6:8] == good[6:8] and dp.parse(re)[0]['seq'] == 7 and dp.slot_valid(dp.REF, re)
    zero = dp.header(dp.REF, py, '20261119', 'x', seq=1) + py
    assert dp.parse(zero)[0]['flags'] == 0 and dp.slot_valid(dp.REF, zero)
    wrong = dp.header(dp.REF, py, '20261119', 'x', seq=1, flags=refpack.hint(m['k']) ^ 0x100) + py
    assert not dp.slot_valid(dp.REF, wrong)
    assert dp.SLOT[dp.RADIOID] == 0x30000 and dp.SLOT_ADDR[dp.REF] == (0xEA0000, 0xEE0000)
    plan = dp.cps_plan(0xEA0000, img)
    assert len(plan) == (len(img) + 4095) // 4096 and plan[-1][0] == 0xEA0000 and plan[-1][1][:4] == b'DPK1'
    print(f'  datapack.py: map pack slot choice, sequence, flags carried; {len(plan)} sectors, header last')


def ui_tests(ROOT, APP, OUT, host):
    """Settings > About and Tools > Airfields (with the air watch) with packs in force, through upstream's UI simulator
    (tests/plus/ui_sim_dpk.c over the air items' ui_sim_air.c), English and Turkish"""
    import shlex
    tp = ROOT / 'tests/plus'
    core = APP / 'core'
    run(['make', 'out/gen/lang_tables.h'], cwd=host, stdout=subprocess.DEVNULL)
    command = run(['make', '-Bn', 'uiFlowTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')] = str(tp / 'test_ui_datapack.c')
    argv[argv.index('sim/ui_sim.c')] = str(tp / 'ui_sim_dpk.c')
    argv[argv.index('-o') + 1] = str(OUT / 'test_ui_datapack')
    run([*argv, '-I' + str(host), '-I' + str(tp), '-DCONFIG_P25_CHANNELS=1', '-DCONFIG_AM_RX=1', '-DCONFIG_APRS=1',
         '-DCONFIG_VFO=1', '-DCONFIG_AIRFIELDS=1', '-DCONFIG_LANG_TR=1', '-DCONFIG_LARGE_SCALE=1',
         '-DCONFIG_HOME_FREQ=1', '-DCONFIG_MULTI_WATCH=1', '-DCONFIG_VFO_MODES=1', '-DCONFIG_AIR_WATCH=1',
         '-DCONFIG_DATAPACK=1',
         *(str(core / 'ui' / f) for f in ('airfield.c', 'airfield_data.c', 'ui_airfield.c', 'air_pack.c')),
         str(core / 'storage/datapack.c'), str(core / 'storage/storage.c'), str(host / 'sim/sim_flash.c'),
         str(core / 'aprs/aprs_fmt.c'), str(core / 'radio/vfox.c'), '-lm'], cwd=host)
    run([OUT / 'test_ui_datapack'], cwd=host)


CSV = '''RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,CITY,STATE,COUNTRY
2860105,TA2XYZ,Ayşe,Kaya,Izmir,,Turkey
2860001,ta1abc,Ahmet,Yilmaz,Ankara,,Turkey
3101234,TA3AA,Mehmet Ali,Demir,Bursa,,Turkey
2860001,TA9DUP,Dup,Dup,X,,Turkey
3109999,W1AW,Hiram,Maxim,Newington,CT,United States
'''

AD218 = '''LTAC AD 2.17 something
LTAC AD 2.18 ATS COMMUNICATION FACILITIES
Service designation  Call sign  Frequency  Hours  Remarks
ATIS  ESENBOGA INFORMATION  123.600 MHZ  H24
TWR   ESENBOGA TOWER  118.100 MHZ  H24
      121.500 MHZ emergency
APP   ANKARA APPROACH 119.100 MHZ H24
      UHF 257.800 MHZ (military, not air band)
LTAC AD 2.19 RADIO NAVIGATION
VOR 112.500 MHZ
'''


def builder_tests(ROOT, OUT):
    """tools/datapack.py on synthetic input (never real RadioID or AIP data): the payloads and headers, the CPS write
    order, the AD 2.18 parser; returns the two images for the C test, which writes and reads them as the radio does"""
    import struct
    import sys
    sys.path.insert(0, str(ROOT))
    from tools import datapack as dp
    csv = OUT / 'dp_user.csv'
    csv.write_text(CSV, encoding='utf-8')
    payload, n, skipped = dp.radioid_from_csv(str(csv), ['Turkey'])
    assert n == 3 and skipped['duplicate'] == 1, (n, skipped)
    image = dp.header(dp.RADIOID, payload, '20261120', 'RadioID TR') + payload
    h, back = dp.parse(image)
    assert h and h['crc_ok'] and h['len'] == len(payload) == 8 + 3 * 15 and back == payload, h
    assert struct.unpack_from('<IBB', payload) == (3, 15, 16)
    assert payload[8:11] == (2860001).to_bytes(3, 'little')		# sorted by ID
    try:
        dp.radioid_payload([(1, 'A', ''), (1, 'B', '')])
        raise AssertionError('a repeated ID was taken')
    except dp.PackError:
        pass
    # the aerodrome code: in the file name (underscores around it, as the AIP names its files), else in the page text
    assert dp.code_of('LT_AD_2_LTAC_en.pdf') == 'LTAC' and dp.code_of('pdf/LTFM.pdf') == 'LTFM'
    assert dp.code_of('LTBA-AD2.pdf') == 'LTBA' and dp.code_of('LT_AD_2_LTAC_LTFJ_en.pdf') == 'LTFJ'
    assert dp.code_of('LT_AD_2_LTACX.pdf', 'LTAI AD 2.1\nLTAC AD 2.18 ATS\nLTBJ\n') == 'LTAC'
    assert dp.code_of('ad2.pdf', 'aerodrome LTAC\n') == 'LTAC'
    for name, text in (('ad2.pdf', None), ('XLTACX.pdf', 'no code here AD 2.18')):
        try:
            dp.code_of(name, text)
            raise AssertionError(f'a code in {name!r}')
        except dp.PackError:
            pass
    fields = {'LTAC': dp.parse_ad218(AD218)}
    t = dp.airfields.TYPES
    assert fields['LTAC'] == [(t.index('ATIS'), 123600), (t.index('TWR'), 118100), (t.index('TWR'), 121500),
                              (t.index('APP'), 119100)], fields		# the 2.19 VOR and the UHF one left out
    fields['LTAC'] = [f for f in fields['LTAC'] if f[1] != 121500]
    air = dp.air_payload(fields)
    air_image = dp.header(dp.AIR, air, '20261120', 'TR aerodromes') + air
    assert dp.parse(air_image)[0]['crc_ok']
    bad = bytearray(image)
    bad[70] ^= 1
    assert dp.parse(bytes(bad))[0]['crc_ok'] is False and dp.parse(b'\xff' * 64)[0] is None
    # the radio's slots: the new pack goes where the pack in force is not; its sequence one more
    empty = b'\xff' * 64
    assert dp.inactive_slot(dp.RADIOID, [empty, empty]) == 0 and dp.next_seq([empty, empty]) == 1
    s1 = dp.header(dp.RADIOID, payload, '20261120', 'x', seq=4) + payload
    assert dp.inactive_slot(dp.RADIOID, [s1, empty]) == 1 and dp.next_seq([s1, empty]) == 5
    # the CPS order: payload sectors, the header's sector last
    big = dp.header(dp.RADIOID, bytes(9000), '20261120', 'x') + bytes(9000)
    plan = dp.cps_plan(0xEB0000, big)
    assert [a for a, _ in plan] == [0xEB1000, 0xEB2000, 0xEB0000] and all(len(d) == 4096 for _, d in plan)
    assert plan[-1][1][:4] == b'DPK1'
    rid = OUT / 'dp_radioid.dpk'
    rid.write_bytes(image)
    airf = OUT / 'dp_air.dpk'
    airf.write_bytes(air_image)
    assert 'payload CRC ok' in dp.info(image) and '3 IDs' in dp.info(image) and '1 airfields' in dp.info(air_image)
    print('  datapack.py: RadioID and air band packs from synthetic input, the AD 2.18 parser, the CPS order')
    return [rid, airf]
