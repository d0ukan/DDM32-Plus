#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Classic saved-channel and real radio-state-machine regression tests."""
from pathlib import Path
import subprocess
import shlex
import sys
import os
import tempfile
import zipfile

ROOT=Path(__file__).resolve().parents[1]
APP=ROOT/'build/radiofoundry-p25'
OUT=ROOT/'build/classic-host'
# Classic r13's channel store: the codeplug and its RFKV mode records (tools/classic_standard.py)
CODEPLUG=[APP/'core/codeplug/codeplug.c',APP/'core/codeplug/codeplug_kv.c',APP/'core/codeplug/codeplug_tag.c',
          APP/'core/codeplug/codeplug_records.c',APP/'core/p25/p25_codeplug.c',APP/'core/codeplug/codeplug_talk.c']
# The conventional P25 binding and the pinned CAI library under it (Classic r8+); DDM32 Plus's LC extras
CAI=sorted((ROOT/'third_party/p25cai/src').glob('*.c'))
P25SRC=[ROOT/'src/p25/p25.c',ROOT/'src/p25/p25_lcx.c',*CAI]
def obj_name(source):
    """An object's name in a shared directory: the CAI library's voice.c is not src/p25/voice.c"""
    return ('cai-' if source.parent.parent.name=='p25cai' else '')+source.stem+'.o'
sys.path.insert(0,str(ROOT/'tools'))
import test_p25nac as p25nac	# DM32 Plus separate RX / TX NAC

def run(args, **kw):
    return subprocess.run([str(a) for a in args],check=True,**kw)

def iddb_fixture(csv_path, expect_path):
    """A synthetic RadioID.net user.csv and what the radio must show for it (tests/plus/test_iddb.c): dense IDs over
    several blocks, gaps of 1-3 LEB128 bytes, the 24-bit ends, duplicates, Turkish / accented / long / empty names,
    callsigns with digits, rows the country and prefix filters keep or drop, broken rows"""
    rows=[('RADIO_ID','CALLSIGN','FIRST_NAME','LAST_NAME','CITY','STATE','COUNTRY')]
    want={}
    missing=[]
    def add(rid,call,first,country='Turkiye',last='',expect=None,shown=None):
        rows.append((str(rid),call,first,last,'City','',country))
        if expect is not None and rid not in want:
            want[rid]=(shown or call.upper(),expect)
    letters='ABCDEFGHIJKLMNOPQRSTUVWXYZ'
    for k in range(200):                     # 2860000.. step 3: four blocks, 1-byte gaps
        call='TA%d%s%s%s'%(k%10,letters[k%26],letters[k//26%26],letters[k*7%26])
        add(2860000+k*3,call,'Op%d'%k,expect='Op%d'%k)
    add(1,'TA1A','Ali',expect='Ali')                         # the smallest ID
    add(0xFFFFFF,'TA9ZZZ','zeynep',expect='Zeynep')         # the largest
    add(3000000,'TA2GAP','Gap',expect='Gap')
    add(3000200,'TA2GAQ','Gap2',expect='Gap2')               # 2-byte gap
    add(3100000,'TA2GAR','Gap3',expect='Gap3')               # 3-byte gap
    add(9000000,'TA2GAS','Gap4',expect='Gap4')
    add(2861000,'TA1ABC','Abdurrahmanxyz',expect='Abdurrahma')   # cut to 10
    add(2861001,'TA1SKR','ŞÜKRÜ',expect='Sukru')
    add(2861002,'TA1ISM','İsmail Hakkı',expect='Ismail')
    add(2861003,'TA1CAG','çağlar',expect='Caglar')
    add(2861004,'TA1OMR','Ömer Faruk',expect='Omer')
    add(2861005,'TA1IGD','ığdırlı',expect='Igdirli')
    add(2861006,'TA1YLM','',last='Yılmaz',expect='Yilmaz')    # the last name when there is no first name
    add(2861007,'TA1NON','',expect='')                        # no name: the callsign alone
    add(2861008,'TA1JLC','Jean-Luc',expect='Jean-Luc')
    add(2861009,'TA1OBR',"O'Brien",expect="O'Brien")
    add(2861010,'TA1MEH','MEHMET',expect='Mehmet')
    add(2861011,'TA1MCD','McDonald',expect='McDonald')
    add(2861012,'ta2low','0zero',expect='0zero',shown='TA2LOW')  # lower-case callsign; a name starting with a digit
    add(2861013,'TA1DOT','José',expect='Jose')
    add(2860003,'TB1DUP','Dup')                               # a duplicate of 2860003: the first row wins
    add(2861014,'5B4AAA','Andreas',country='Cyprus',expect='Andreas')
    add(2861015,'TA3GER','Gerd',country='Germany',expect='Gerd')   # kept by --prefix TA
    add(2861016,'TA3TUR','Turgut',country='Türkiye',expect='Turgut')
    add(2861017,'TA3TRK','Tarik',country='TURKEY',expect='Tarik')
    for rid,call,country in ((2861100,'DL1ABC','Germany'),(2861101,'TA1ABCD','Turkiye'),(2861102,'TA1-AB','Turkiye'),
                             (2861103,'','Turkiye')):
        add(rid,call,'Drop',country=country)                  # another country, too long, not A-Z 0-9, empty
        missing.append(rid)
    rows.append(('0','TA0ZER','Zero','','','','Turkiye'))
    rows.append(('16777216','TA0BIG','Big','','','','Turkiye'))
    rows.append(('abc','TA0BAD','Bad','','','','Turkiye'))
    missing+= [16777214,2,2861099,9000001,3000001]
    import csv
    with open(csv_path,'w',encoding='utf-8',newline='') as f:
        csv.writer(f).writerows(rows)
    with open(expect_path,'w',encoding='ascii') as f:
        for rid in sorted(want):
            f.write('%d\t%s\t%s\n'%(rid,*want[rid]))
        for rid in missing:
            f.write('-\t%d\n'%rid)

def iddb_tests(flags):
    # DM32 Plus DMR ID database (docs/plus.md): the generator's packing and folding, its table from a synthetic CSV
    # through the C lookup (tests/plus/test_iddb.c), then the table this prepare made (download or sample)
    sys.path.insert(0,str(ROOT))
    from tools import iddb
    for call in ('TA1A','TA9ZZZ','5B4AAA','A','999999','TB0ZZ'):
        if iddb.unpack_call(iddb.pack_call(call))!=call or not 0<=iddb.pack_call(call)<2**32:
            raise SystemExit('iddb: callsign %s does not pack back'%call)
    if any(iddb.pack_call(c) is not None for c in ('TA1ABCD','TA-1','','TA 1')):
        raise SystemExit('iddb: packed a callsign it cannot hold')
    for raw,folded in (('ŞÜKRÜ','Sukru'),('İSMAİL','Ismail'),('ığdır','Igdir'),('Çağrı Ömer','Cagri'),('Ünal','Unal')):
        if iddb.short_name(raw,'')!=folded:
            raise SystemExit('iddb: %s folded to %s'%(raw,iddb.short_name(raw,'')))
    if iddb.country_key('Türkiye')!=iddb.country_key('Turkey'):
        raise SystemExit('iddb: Turkey and Türkiye differ')
    with tempfile.TemporaryDirectory() as tmp:
        csv_path,expect,gen=Path(tmp)/'user.csv',Path(tmp)/'expect.txt',Path(tmp)/'iddb_data.c'
        iddb_fixture(csv_path,expect)
        run(['python3',ROOT/'tools/iddb.py',csv_path,'--country','Turkey','--country','Cyprus','--prefix','TA',
             '--date','2026-01-02','-o',gen],cwd=ROOT)
        run(['clang',*flags,'-DCONFIG_ID_DB=1',ROOT/'tests/plus/test_iddb.c',APP/'core/radio/iddb.c',gen,
             '-o',OUT/'test_iddb'],cwd=ROOT)
        run([OUT/'test_iddb',expect],cwd=ROOT)
        r=subprocess.run(['python3',ROOT/'tools/iddb.py',csv_path,'--max-bytes','100'],capture_output=True,text=True)
        if r.returncode==0 or 'more than 100' not in r.stderr:
            raise SystemExit('iddb.py made a table over its byte limit')
    run(['clang',*flags,'-DCONFIG_ID_DB=1',ROOT/'tests/plus/test_iddb.c',APP/'core/radio/iddb.c',
         APP/'core/radio/iddb_data.c','-o',OUT/'test_iddb_shipped'],cwd=ROOT)
    run([OUT/'test_iddb_shipped'],cwd=ROOT)

def rpt_fixture(csv_path, json_path, expect_path):
    """A synthetic owner CSV and RadioID.net rptrs.json and the table the radio must get (tests/plus/test_rpt.c):
    positions from lat/lon, a locator, the gazetteer (a town before its province, a word's start), none; Turkish
    folding, lower-case callsigns, offsets both ways and as a TX frequency, tones with a comma, 12.5 kHz, DMR colour
    codes and slots, RadioID's country spellings and status, duplicates, and rows that must be refused (counted)"""
    import csv, json
    head=['callsign','city','lat','lon','locator','rx_mhz','tx_mhz','offset_mhz','ctcss','mode','cc','ts','bw']
    rows=[['TA1AAA','Ankara','39.93340','32.85970','','145.6000','','-0.600','88.5','FM','','','25'],
          ['TA1BBB','İstanbul Kadıköy','','','KN41ka','439.1625','431.5625','','','DMR','2','2',''],
          ['ta1ccc','ŞANLIURFA','','','','439.2000','','-7.6','','DMR','15','1+2',''],
          ['TA1DDD','Ordualtinordu','','','','430.0125','','','','C4FM','','',''],
          ['TA1EEE','Nowhere','','','','145.5000','','','','NFM','','',''],
          ['TA1FFF','Mersin Silifke','','','','145.7250','','+0.600','123,0','FM','','','12.5'],
          ['TA1AAA','Ankara','39.9','32.8','','145.6000','','','','FM','','',''],          # duplicate
          ['TA1ABCDEFGH','Ankara','','','','145.6000','','','','FM','','',''],              # callsign too long
          ['','Ankara','','','','145.6000','','','','FM','','',''],                         # no callsign
          ['TA1BAD','Ankara','','','','abc','','','','FM','','',''],                        # frequency
          ['TA1BAD','Ankara','','','','145.6001','','','','FM','','',''],                   # not on 1.25 kHz
          ['TA1BAD','Ankara','','','','145.6000','','','300','FM','','',''],                # tone
          ['TA1BAD','Ankara','','','','145.6000','','','','AM','','',''],                   # mode
          ['TA1BAD','Ankara','','','','439.1000','','','','DMR','16','',''],                # colour code
          ['TA1BAD','Ankara','','','','439.1000','','','','DMR','1','3',''],                # slot
          ['TA1BAD','Ankara','95','30','','145.6000','','','','FM','','',''],               # latitude
          ['TA1BAD','Ankara','','','ZZ99','145.6000','','','','FM','','',''],               # locator
          ['TA1BAD','Ankara','','','','145.6000','','','','FM','','','20']]                 # bandwidth
    with open(csv_path,'w',encoding='utf-8',newline='') as f:
        f.write('# the list the owner keeps\n')
        csv.writer(f).writerows([head,*rows])
    def rr(call,city,mhz,off,cc,ts,country='Turkiye',status='ACTIVE'):
        return {'identity_type':'DMR','callsign':call,'city':city,'state':'','country':country,'frequency':mhz,
                'color_code':cc,'offset':off,'ts_linked':ts,'status':status}
    data={'rptrs':[rr('YM0AAA','Bursa Nilufer','439.40000','-7.600',1,'TS1 TS2'),
                   rr('YM0BBB','Van','145.68750','-0.600',3,'Mixed Mode',country='Türkiye'),
                   rr('YM0CCC','Izmir','439.30000','-7.600',1,'TS1',status='INACTIVE'),
                   rr('DB0AAA','Berlin','439.30000','-7.600',1,'TS1',country='Germany'),
                   rr('YM0AAA','Bursa','439.40000','-7.600',1,'TS1 TS2')]}           # duplicate
    with open(json_path,'w',encoding='utf-8') as f:
        json.dump(data,f)
    want=[(-2147483648,-2147483648,145500000,0,0,0x80,0x00,'TA1EEE','Nowhere'),
          (3993340,3285970,145600000,-480,885,0,0x00,'TA1AAA','Ankara'),
          (3848910,4340890,145687500,-480,0,1,0x03,'YM0BBB','Van'),
          (3637780,3393440,145725000,480,1230,0x80,0x00,'TA1FFF','Mersin Silif'),
          (4098390,3787640,430012500,0,0,2,0x00,'TA1DDD','Ordualtinord'),
          (4102083,2887500,439162500,-6080,0,1,0x22,'TA1BBB','Istanbul Kad'),
          (3715910,3879690,439200000,-6080,0,1,0x3F,'TA1CCC','Sanliurfa'),
          (4021300,2898700,439400000,-6080,0,1,0x31,'YM0AAA','Bursa Nilufe')]
    with open(expect_path,'w',encoding='ascii') as f:
        for w in want:
            f.write('%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s\n'%w)
    return {'callsign':2,'frequency':2,'tone':1,'mode':1,'dmr':2,'position':2,'bandwidth':1,'duplicate':2,'inactive':1}

def rpt_tests(flags):
    # DM32 Plus repeater list (docs/plus.md): the generator's parsing, positions and folding from a synthetic CSV and
    # RadioID.net JSON through the C table and math (tests/plus/test_rpt.c), then the table this prepare made
    sys.path.insert(0,str(ROOT))
    from tools import rpt_list
    if max(abs(a-b) for a,b in zip(rpt_list.locator_pos('KM69kw'),(39.9375,32.875)))>1e-9 or rpt_list.fold_text('İĞDIR ılıca',12)!='Igdir Ilica':
        raise SystemExit('rpt_list: locator or folding')
    if rpt_list.mhz_hz('439.16250')!=439162500 or rpt_list.mhz_hz('-7.6')!=-7600000:
        raise SystemExit('rpt_list: MHz parsing')
    with tempfile.TemporaryDirectory() as tmp:
        csv_path,json_path,expect,gen=(Path(tmp)/n for n in ('rpt.csv','rptrs.json','expect.txt','rpt_data.c'))
        skipped=rpt_fixture(csv_path,json_path,expect)
        _,stats=rpt_list.generate(str(csv_path),str(json_path),date='2026-01-02')
        if stats['skipped']!=skipped or stats['count']!=8 or stats['nopos']!=1:
            raise SystemExit('rpt_list: skipped %s, count %d'%(stats['skipped'],stats['count']))
        run(['python3',ROOT/'tools/rpt_list.py','--csv',csv_path,'--radioid',json_path,'--date','2026-01-02',
             '-o',gen],cwd=ROOT)
        run(['clang',*flags,'-DCONFIG_REPEATERS=1','-DCONFIG_APRS=1',ROOT/'tests/plus/test_rpt.c',APP/'core/ui/rpt.c',
             gen,APP/'core/aprs/aprs_fmt.c','-lm','-o',OUT/'test_rpt'],cwd=ROOT)
        run([OUT/'test_rpt',expect],cwd=ROOT)
        r=subprocess.run(['python3',ROOT/'tools/rpt_list.py','--csv',csv_path,'--max-bytes','100'],capture_output=True,
                         text=True)
        if r.returncode==0 or 'more than 100' not in r.stderr:
            raise SystemExit('rpt_list.py made a table over its byte limit')
    run(['clang',*flags,'-DCONFIG_REPEATERS=1','-DCONFIG_APRS=1',ROOT/'tests/plus/test_rpt.c',APP/'core/ui/rpt.c',
         APP/'core/ui/rpt_data.c',APP/'core/aprs/aprs_fmt.c','-lm','-o',OUT/'test_rpt_shipped'],cwd=ROOT)
    run([OUT/'test_rpt_shipped'],cwd=ROOT)

def p25_extras_tests(flags, worker_flags, codec):
    """DM32 Plus P25 extras (CONFIG_P25_EXTRAS) and CH_MODE 2 P25 channels (CONFIG_P25_MODE2), docs/plus.md. The
    MMDVMHost cross-check of the link control is tools/test_p25lc_ref.sh (needs the reference tree)."""
    host=APP/'tests/host'
    # The link control layer with the switch and without it (the old decode): emergency bit, unit to unit, FEC,
    # vendor formats (LDU1s from the real encoder, LCs rewritten by tests/plus/p25_lc_build.h)
    for v in ('1','0'):
        run(['clang',*flags,'-Werror',f'-DCONFIG_P25_EXTRAS={v}','-I'+str(ROOT/'src/p25'),'-I'+str(ROOT/'tests/plus'),
             ROOT/'tests/plus/test_p25x.c',*P25SRC,'-o',OUT/f'test_p25x_{v}'],cwd=ROOT)
        run([OUT/f'test_p25x_{v}'],cwd=ROOT)
    # End to end through the actual channel worker: what plays, the caller lines, the call history entry
    p25x_flags=[f for f in worker_flags if f not in ('-DCONFIG_YSF=1','-DCONFIG_DMR=1')]
    p25x_flags+=['-DCONFIG_P25_EXTRAS=1','-DCONFIG_NAC_DEC=1','-I'+str(ROOT/'tests/plus')]
    p25x_out=OUT/'p25x-worker'
    p25x_out.mkdir(exist_ok=True)
    objects=[]
    for source in [*(ROOT/'src/p25'/f'{n}.c' for n in ('p25','p25_lcx','voice','phy_stream','selftest','audio_fifo')),*CAI,
                   ROOT/'integration/radiofoundry/cpu_profile.c',APP/'core/debuglink/dl_fmt.c',APP/'core/radio/beep.c',
                   ROOT/'tests/plus/test_p25x_worker.c']:
        obj=p25x_out/obj_name(source)
        run(['clang',*p25x_flags,'-c',source,'-o',obj],cwd=ROOT)
        objects.append(obj)
    run(['clang++','-fsanitize=address,undefined',*objects,codec,'-o',OUT/'test_p25x_worker'],cwd=ROOT)
    run([OUT/'test_p25x_worker'],cwd=ROOT)
    # The call history's P25 rows in the real UI (upstream's simulator; the worker's queries stubbed), on the DM-32's
    # narrow rows and on a full 240x320 screen
    command=run(['make','-Bn','uiFlowTest','SANITIZE=1'],cwd=host,capture_output=True,text=True).stdout
    argv=shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')]=str(ROOT/'tests/plus/test_ui_p25x.c')
    argv[argv.index('-o')+1]=str(OUT/'test_ui_p25x')
    run([*argv,'-I'+str(host),'-DCONFIG_P25_CHANNELS=1','-DCONFIG_DEV_TX=1','-DCONFIG_CALL_LOG=1','-DCONFIG_P25_CALLER=1',
         '-DCONFIG_P25_EXTRAS=1','-DCONFIG_NAC_DEC=1'],cwd=host)	# NAC_DEC as Plus: the NAC's two forms, " $293/659"
    run([OUT/'test_ui_p25x'],cwd=host)
    # CH_MODE 2 records (the owner's channels 18-21): with the switch and without; every record that is not mode 2
    # must decode the same in both builds
    decoded={}
    for v in ('1','0'):
        run(['clang',*flags,f'-DCONFIG_P25_MODE2={v}','-DCONFIG_YSF=1','-DCONFIG_NXDN=1','-DCONFIG_DPMR=1',
             '-DCONFIG_DMR_ANY=1',ROOT/'tests/plus/test_p25_mode2.c',*CODEPLUG,
             APP/'core/storage/storage.c',host/'sim/sim_flash.c','-o',OUT/f'test_p25_mode2_{v}'],cwd=ROOT)
        r=subprocess.run([OUT/f'test_p25_mode2_{v}'],cwd=ROOT,capture_output=True,text=True)
        print('\n'.join(l for l in r.stdout.splitlines() if not l.startswith('rec ')))
        if r.returncode!=0:
            raise SystemExit(f'test_p25_mode2 (CONFIG_P25_MODE2={v}) failed')
        # (0xA5: the r7 P25 records the switch reads since the Classic r13 base)
        decoded[v]=[l for l in r.stdout.splitlines() if l.startswith('rec ') and ' mode 0x02:' not in l and
                    ' mode 0xA5:' not in l]
    if decoded['1']!=decoded['0'] or len(decoded['1'])<3000:
        raise SystemExit('CH_MODE 2 support changed how a record of another mode decodes')
    print(f'CH_MODE 2: the other {len(decoded["1"])} records decode the same with and without it')

def fmdec_tests(flags, host):
    """DM32 Plus tone finder and DTMF / SelCall (integration/radiofoundry/fmdec, docs/plus.md): the decoders, their
    radio task glue through the pager's radio_msg.c and FM caller ID's radio_fmid.c, and their screens in the UI
    simulator"""
    radio=APP/'core/radio'
    run(['clang',*flags,'-O1','-DCONFIG_TONE_FIND=1','-I'+str(radio),ROOT/'tests/plus/test_tonefind.c',radio/'tonefind.c',
         radio/'tones.c','-lm','-o',OUT/'test_tonefind'],cwd=ROOT)
    run([OUT/'test_tonefind'],cwd=ROOT)
    run(['clang',*flags,'-O1','-DCONFIG_SELCALL=1','-DCONFIG_FM_ID=1','-I'+str(radio),ROOT/'tests/plus/test_selcall.c',
         radio/'selcall.c',radio/'fmid.c','-lm','-o',OUT/'test_selcall'],cwd=ROOT)
    run([OUT/'test_selcall'],cwd=ROOT)
    run(['clang',*flags,'-DCONFIG_FM_ID=1','-DCONFIG_APRS_RX=1','-DCONFIG_MSG_RX=1','-DCONFIG_NXDN=1',
         '-DCONFIG_TONE_FIND=1','-DCONFIG_SELCALL=1','-I'+str(radio),'-I'+str(ROOT/'tests/audio/stubs'),
         '-include','stdlib.h','-DpvPortMalloc=malloc','-DvPortFree=free',ROOT/'tests/plus/test_fmdec_glue.c',
         *(radio/f for f in ('radio_fmid.c','fmid.c','radio_msg.c','pocsag.c','acars.c','radio_fmdec.c','tonefind.c',
                             'selcall.c','tones.c')),'-lm','-o',OUT/'test_fmdec_glue'],cwd=ROOT)
    run([OUT/'test_fmdec_glue'],cwd=ROOT)
    command=run(['make','-Bn','uiFlowTest','SANITIZE=1'],cwd=host,capture_output=True,text=True).stdout
    argv=shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')]=str(ROOT/'tests/plus/test_ui_fmdec.c')
    argv[argv.index('-o')+1]=str(OUT/'test_ui_fmdec')
    run([*argv,'-I'+str(host),'-DCONFIG_P25_CHANNELS=1','-DCONFIG_MSG_RX=1','-DCONFIG_TONE_FIND=1','-DCONFIG_SELCALL=1',
         str(APP/'core/ui/ui_msg.c'),str(APP/'core/ui/ui_scratch.c'),str(APP/'core/ui/ui_fmdec.c'),str(radio/'tonefind.c')],
        cwd=host)
    run([OUT/'test_ui_fmdec'],cwd=host)

def main():
    run(['python3',ROOT/'tools/sources.py','verify-vendored'],cwd=ROOT)
    run(['python3',ROOT/'tools/build_radiofoundry.py','--prepare-only'],cwd=ROOT)
    OUT.mkdir(parents=True,exist_ok=True)
    host=APP/'tests/host'
    # VERSION must advertise the saved-channel capability, not the lab engine.
    # Link the real handler/formatter/build_info; unused radio commands are discarded.
    for p25, channels in [(0, 0), (1, 0), (1, 1)]:
        version=OUT/f'test_version_{p25}_{channels}'
        run(['clang','-std=gnu99','-Wall','-Wextra','-Wno-unused-parameter','-g',
             '-fsanitize=address,undefined','-fno-sanitize-recover=undefined',
             '-ffunction-sections','-fdata-sections','-D__attribute__(x)=',
             f'-DCONFIG_P25={p25}',f'-DCONFIG_P25_CHANNELS={channels}',
             '-I'+str(APP/'core'),'-I'+str(APP/'drivers'),'-I'+str(APP/'hal'),
             ROOT/'tests/classic/test_version.c',APP/'core/hostlink/build_info.c',
             APP/'core/debuglink/dl_fmt.c',
             # Linux (DDM32 Plus host): the GNU linker keeps every ASan-instrumented global's section, so the unused
             # radio commands stay and their callees are unresolved; LLD with dead-strippable globals discards them
             *(['-Wl,-dead_strip'] if sys.platform=='darwin' else
               ['-fuse-ld=lld','-fsanitize-address-globals-dead-stripping','-Wl,--gc-sections']),
             '-o',version],cwd=ROOT)
        run([version],cwd=ROOT)
    flags=['-std=gnu99','-Wall','-Wextra','-Wno-unused-parameter','-g',
           '-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer',
           '-I'+str(APP/'core'),'-I'+str(APP/'drivers'),'-I'+str(APP/'hal'),'-I'+str(host),
           '-I'+str(ROOT/'third_party/p25cai/include'),
           '-D__attribute__(x)=','-DCONFIG_P25_CHANNELS=1','-DCONFIG_P25_NATIVE=1','-DCONFIG_P25=1']
    run(['clang',*flags,'-ffunction-sections','-fdata-sections','-DCONFIG_DEV_TX=1',
         '-DCONFIG_P25_BENCH=0','-I'+str(ROOT/'tests/audio/stubs'),
         '-I'+str(APP/'core/p25'),ROOT/'tests/classic/test_p25_audio.c',
         APP/'core/p25/audio_fifo.c',APP/'core/radio/beep.c',
         '-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections',
         '-o',OUT/'test_p25_audio'],cwd=ROOT)
    run([OUT/'test_p25_audio'],cwd=ROOT)
    run(['clang',*flags,ROOT/'tests/classic/test_channels.c',*CODEPLUG,
         APP/'core/storage/storage.c',host/'sim/sim_flash.c','-o',OUT/'test_channels'],cwd=ROOT)
    run([OUT/'test_channels'],cwd=ROOT)
    run(['clang',*flags,'-DCONFIG_YSF=1',ROOT/'tests/plus/test_ysf_channel.c',*CODEPLUG,
         APP/'core/storage/storage.c',host/'sim/sim_flash.c','-o',OUT/'test_ysf_channel'],cwd=ROOT)
    run([OUT/'test_ysf_channel'],cwd=ROOT)
    # DM32 Plus YSF transmit (docs/ysf-nxdn-tx.md): a YSF channel that allows TX, kept as it is
    run(['clang',*flags,'-DCONFIG_YSF=1','-DCONFIG_YSF_TX=1',ROOT/'tests/plus/test_ysf_channel.c',
         *CODEPLUG,APP/'core/storage/storage.c',host/'sim/sim_flash.c',
         '-o',OUT/'test_ysf_channel_tx'],cwd=ROOT)
    run([OUT/'test_ysf_channel_tx'],cwd=ROOT)
    # DM32 Plus dPMR channels: the record (CH_MODE 0xA8, the colour code filter), RX only, NXDN unchanged
    run(['clang',*flags,'-DCONFIG_NXDN=1','-DCONFIG_DPMR=1',ROOT/'tests/plus/test_dpmr_channel.c',
         *CODEPLUG,APP/'core/storage/storage.c',host/'sim/sim_flash.c','-o',OUT/'test_dpmr_channel'],
        cwd=ROOT)
    run([OUT/'test_dpmr_channel'],cwd=ROOT)
    # DM32 Plus DMR any color code / timeslot / talkgroup: the channel record (codeplug) and the call layer
    run(['clang',*flags,'-DCONFIG_DMR_ANY=1',ROOT/'tests/plus/test_dmr_any.c',*CODEPLUG,
         APP/'core/storage/storage.c',host/'sim/sim_flash.c','-o',OUT/'test_dmr_any'],cwd=ROOT)
    run([OUT/'test_dmr_any'],cwd=ROOT)
    command=run(['make','-Bn','dmrCallTest','SANITIZE=1'],cwd=host,capture_output=True,text=True).stdout
    argv=shlex.split(next(line for line in command.splitlines() if ' -o dmrCallTest ' in line))
    argv[argv.index('dmrCallTest.c')]=str(ROOT/'tests/plus/test_dmr_any_call.c')
    argv[argv.index('-o')+1]=str(OUT/'test_dmr_any_call')
    run([*argv,'-I'+str(host),'-DCONFIG_DMR_ANY=1'],cwd=host)
    run([OUT/'test_dmr_any_call'],cwd=host)
    # DM32 Plus SFR role set on the radio: the channel record (codeplug) through sfr.c and radio_sfr_channel()
    run(['clang',*flags,'-ffunction-sections','-fdata-sections','-DCONFIG_SFR_EDIT=1','-DCONFIG_SFR=1',
         ROOT/'tests/plus/test_sfr_edit.c',*CODEPLUG,APP/'core/storage/storage.c',
         host/'sim/sim_flash.c',APP/'core/dmr/sfr.c',APP/'core/radio/radio_sfr.c',APP/'core/dmr/sfrRunner.c',
         APP/'core/dmr/dmrData.c',APP/'core/hotspot/dmrDataFrame.c',APP/'core/dmr/fec/dmrFec.c',
         '-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections','-o',OUT/'test_sfr_edit'],cwd=ROOT)
    run([OUT/'test_sfr_edit'],cwd=ROOT)
    # DM32 Plus spectrum engine (docs/plus.md)
    run(['clang',*flags,'-DCONFIG_SPECTRUM=1','-I'+str(ROOT/'tests/plus'),ROOT/'tests/plus/test_spectrum.c',
         APP/'core/radio/spectrum.c','-o',OUT/'test_spectrum'],cwd=ROOT)
    run([OUT/'test_spectrum'],cwd=ROOT)
    run(['clang',*flags,'-DCONFIG_FOX=1',ROOT/'tests/plus/test_fox.c',APP/'core/radio/fox.c','-o',OUT/'test_fox'],cwd=ROOT)
    run([OUT/'test_fox'],cwd=ROOT)
    # DM32 Plus signal quality: the bit error rate window (radio/sigq.c)
    run(['clang',*flags,'-DCONFIG_SIGNAL_QUALITY=1',ROOT/'tests/plus/test_sigq.c',APP/'core/radio/sigq.c','-o',OUT/'test_sigq'],cwd=ROOT)
    run([OUT/'test_sigq'],cwd=ROOT)
    run(['clang',*flags,'-DCONFIG_P25_SEARCH=1',ROOT/'tests/plus/test_p25search.c',APP/'core/radio/p25search.c','-o',OUT/'test_p25search'],cwd=ROOT)
    run([OUT/'test_p25search'],cwd=ROOT)
    # DM32 Plus P25 trunk following: TSBK decoder, framer TSDU mode, channel plan, grants, the follower
    run(['clang',*flags,'-DCONFIG_P25_TRUNK=1','-I'+str(APP/'core/p25'),'-I'+str(ROOT/'src/p25'),'-I'+str(ROOT/'tests/plus'),
         ROOT/'tests/plus/test_p25trunk.c',APP/'core/p25/p25_tsbk.c',APP/'core/radio/p25trunk.c',*P25SRC,
         '-o',OUT/'test_p25trunk'],cwd=ROOT)
    run([OUT/'test_p25trunk'],cwd=ROOT)
    run(['clang',*flags,'-DCONFIG_DMR_SEARCH=1',ROOT/'tests/plus/test_dmrsearch.c',APP/'core/radio/dmrsearch.c','-o',OUT/'test_dmrsearch'],cwd=ROOT)
    run([OUT/'test_dmrsearch'],cwd=ROOT)
    # DDM32 Plus all digital search: the DMR search engine's second kind (with and without the faster sweep)
    for fast in (0,1):
        run(['clang',*flags,'-DCONFIG_DMR_SEARCH=1','-DCONFIG_ALL_SEARCH=1','-DCONFIG_YSF=1','-DCONFIG_NXDN=1',
             f'-DCONFIG_SEARCH_FAST={fast}',ROOT/'tests/plus/test_allsearch.c',APP/'core/radio/dmrsearch.c',
             '-o',OUT/f'test_allsearch_{fast}'],cwd=ROOT)
        run([OUT/f'test_allsearch_{fast}'],cwd=ROOT)
    # DM32 Plus DMR Tier III trunk following: CSBK CRC, BPTC (upstream's FEC), fields, channel plan, the follower
    run(['clang',*flags,'-DCONFIG_DMR_TRUNK=1','-I'+str(ROOT/'tests/plus'),ROOT/'tests/plus/test_dmrtrunk.c',
         APP/'core/radio/dmr_csbk.c',APP/'core/radio/dmrtrunk.c',APP/'core/dmr/fec/dmrFec.c','-o',OUT/'test_dmrtrunk'],cwd=ROOT)
    run([OUT/'test_dmrtrunk'],cwd=ROOT)
    run(['clang',*flags,'-DCONFIG_LONE_WORKER=1','-DCONFIG_DMR_EMERGENCY=0',ROOT/'tests/plus/test_lone.c',APP/'core/ui/lone.c','-o',OUT/'test_lone'],cwd=ROOT)
    run([OUT/'test_lone'],cwd=ROOT)
    # ... and its alarm action on upstream's own DMR emergency (ui_svc_emergency.c, dmr_emergency.c) and a fake APRS SOS
    run(['clang',*flags,'-DCONFIG_LONE_WORKER=1','-DCONFIG_DMR_EMERGENCY=1','-DCONFIG_APRS=1','-DCONFIG_APRS_MSG=1',
         '-DLONE_TEST_TX','-I'+str(APP/'core/ui'),ROOT/'tests/plus/test_lone.c',APP/'core/ui/lone.c',APP/'core/ui/ui_svc_emergency.c',
         APP/'core/dmr/dmr_emergency.c','-o',OUT/'test_lone_tx'],cwd=ROOT)
    run([OUT/'test_lone_tx'],cwd=ROOT)
    run(['clang',*flags,'-DCONFIG_ALMANAC=1',ROOT/'tests/plus/test_almanac.c',APP/'core/ui/almanac.c','-lm','-o',OUT/'test_almanac'],cwd=ROOT)
    run([OUT/'test_almanac'],cwd=ROOT)
    run(['clang',*flags,'-DCONFIG_NAV=1','-DCONFIG_APRS=1',ROOT/'tests/plus/test_nav.c',APP/'core/ui/nav.c',
         APP/'core/aprs/aprs_fmt.c','-lm','-o',OUT/'test_nav'],cwd=ROOT)
    run([OUT/'test_nav'],cwd=ROOT)
    # DM32 Plus satellites: SGP4 against Vallado's verification output, the pass search, Doppler, the shipped table
    # (radio/sat_data.c, generated by tools/sat_tle.py in the prepare above); a TLE with a bad checksum is refused
    run(['clang',*flags,'-DCONFIG_SATELLITES=1',ROOT/'tests/plus/test_sat.c',APP/'core/radio/sgp4.c',
         APP/'core/radio/sat.c',APP/'core/radio/sat_data.c','-lm','-o',OUT/'test_sat'],cwd=ROOT)
    run([OUT/'test_sat'],cwd=ROOT)
    with tempfile.TemporaryDirectory() as tmp:
        bad=Path(tmp)/'tle.txt'
        lines=(ROOT/'integration/radiofoundry/sat/tle.txt').read_text().splitlines()
        i=next(k for k,l in enumerate(lines) if l.startswith('2 '))
        lines[i]=lines[i][:68]+str((int(lines[i][68])+1)%10)
        bad.write_text('\n'.join(lines)+'\n')
        r=subprocess.run(['python3',ROOT/'tools/sat_tle.py','--tle',bad],capture_output=True,text=True)
        if r.returncode==0 or 'checksum' not in r.stderr:
            raise SystemExit('sat_tle.py took a TLE line with a bad checksum')
    # DM32 Plus satellites' pass alarm and Doppler tracking: the service (radio/sat_svc.c) with a fake GPS and radio
    # core (the alarm against a one-second scan, the tracking's requests, step hysteresis, PTT, the set); the radio's
    # Doppler against tests/plus/sat_doppler_ref.py (its own Python SGP4 and frames on the same tle.txt); the screen
    # in the simulator (tracking, the alarm page, its banner and tone on Home)
    run(['clang',*flags,'-DCONFIG_SATELLITES=1','-DSAT_SVC_TEST=1','-I'+str(ROOT/'tests/native/stubs'),ROOT/'tests/plus/test_sat_svc.c',
         APP/'core/radio/sgp4.c',APP/'core/radio/sat.c',APP/'core/radio/sat_svc.c',APP/'core/radio/sat_data.c','-lm',
         '-o',OUT/'test_sat_svc'],cwd=ROOT)
    run([OUT/'test_sat_svc'],cwd=ROOT)
    run(['clang',*flags,'-DCONFIG_SATELLITES=1',ROOT/'tests/plus/test_sat_doppler.c',APP/'core/radio/sgp4.c',
         APP/'core/radio/sat.c',APP/'core/radio/sat_data.c','-lm','-o',OUT/'test_sat_doppler'],cwd=ROOT)
    run(['python3',ROOT/'tests/plus/sat_doppler_ref.py',OUT/'test_sat_doppler'],cwd=ROOT)
    # (Tools > ISS and the AOS pop-up in English and Turkish: the tables made from tr.po)
    run(['make','out/gen/lang_tables.h'],cwd=host,stdout=subprocess.DEVNULL)
    command=run(['make','-Bn','uiFlowTest','SANITIZE=1'],cwd=host,capture_output=True,text=True).stdout
    satui=shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    satui[satui.index('uiFlowTest.c')]=str(ROOT/'tests/plus/test_ui_sat.c')
    satui[satui.index('-o')+1]=str(OUT/'test_ui_sat')
    run([*satui,'-I'+str(host),'-DCONFIG_P25_CHANNELS=1','-DCONFIG_SATELLITES=1','-DCONFIG_LANG_TR=1',str(APP/'core/radio/sat.c'),
         str(APP/'core/radio/sgp4.c'),str(APP/'core/radio/sat_data.c'),'-lm'],cwd=host)
    run([OUT/'test_ui_sat'],cwd=host)
    # DM32 Plus NXDN48 receive core (docs/nxdn.md; the MMDVMHost cross-check is tools/test_nxdn_ref.sh)
    run(['clang',*flags,'-I'+str(ROOT/'src'),ROOT/'tests/plus/test_nxdn.c',ROOT/'src/nxdn/nxdn.c',
         ROOT/'src/nxdn/nxdn_demod.c',ROOT/'src/nxdn/nxdn_rx.c','-lm','-o',OUT/'test_nxdn'],cwd=ROOT)
    run([OUT/'test_nxdn'],cwd=ROOT)
    # DM32 Plus dPMR receive core (docs/dpmr.md): the NXDN suite runs again first in it (the demodulator both use)
    run(['clang',*flags,'-I'+str(ROOT/'src'),ROOT/'tests/plus/test_dpmr.c',ROOT/'src/dpmr/dpmr.c',
         ROOT/'src/dpmr/dpmr_rx.c',ROOT/'src/nxdn/nxdn.c',ROOT/'src/nxdn/nxdn_demod.c',ROOT/'src/nxdn/nxdn_rx.c',
         '-lm','-o',OUT/'test_dpmr'],cwd=ROOT)
    run([OUT/'test_dpmr'],cwd=ROOT)
    # DM32 Plus FM caller ID decoders: MDC1200, FleetSync I, DTMF (integration/radiofoundry/fmid/fmid.h)
    run(['clang',*flags,'-DCONFIG_FM_ID=1','-I'+str(APP/'core/radio'),ROOT/'tests/plus/test_fmid.c',
         APP/'core/radio/fmid.c','-lm','-o',OUT/'test_fmid'],cwd=ROOT)
    run([OUT/'test_fmid'],cwd=ROOT)
    # DM32 Plus pager and ACARS decoders: POCSAG, ACARS (integration/radiofoundry/msg/pocsag.h, acars.h)
    for name in ('pocsag','acars'):
        run(['clang',*flags,'-DCONFIG_MSG_RX=1','-I'+str(APP/'core/radio'),ROOT/f'tests/plus/test_{name}.c',
             APP/f'core/radio/{name}.c','-lm','-o',OUT/f'test_{name}'],cwd=ROOT)
        run([OUT/f'test_{name}'],cwd=ROOT)
    # DM32 Plus APRS messages and SOS (integration/radiofoundry/aprsmsg): the upstream radio-core APRS suite again with
    # the feature built in, then the Mic-E encoder, SOS, the composer and the inbox through the same fake radio
    command=run(['make','-Bn','aprsRadioTest','SANITIZE=1'],cwd=host,capture_output=True,text=True).stdout
    argv=shlex.split(next(line for line in command.splitlines() if ' -o aprsRadioTest ' in line))
    argv[argv.index('aprsRadioTest.c')]=str(ROOT/'tests/plus/test_aprs_msg.c')
    argv[argv.index('-o')+1]=str(OUT/'test_aprs_msg')
    run([*argv,'-I'+str(host),'-DCONFIG_APRS_MSG=1',str(APP/'core/aprs/aprs_plus.c')],cwd=host)
    run([OUT/'test_aprs_msg'],cwd=host)
    # DM32 Plus ISS APRS digipeater from Satellites (prepare_iss_aprs): the same suite with it built in, then the
    # one-shot frame (145.825 MHz + Doppler, path ARISS, alone), its gates and the 30 s limit
    argv[argv.index(str(ROOT/'tests/plus/test_aprs_msg.c'))]=str(ROOT/'tests/plus/test_aprs_iss.c')
    argv[argv.index('-o')+1]=str(OUT/'test_aprs_iss')
    run([*argv,'-I'+str(host),'-DCONFIG_SATELLITES=1'],cwd=host)
    run([OUT/'test_aprs_iss'],cwd=host)
    # DM32 Plus APRS stations on the map (prepare_mapnav): the same suite with CONFIG_MAP (the station entry's
    # altitude), then that hook through the engine's receive path and the map's pick over the engine's own list
    mapnav_aprs=[str(ROOT/'tests/plus/test_aprs_map.c') if a==str(ROOT/'tests/plus/test_aprs_iss.c') else a for a in argv]
    mapnav_aprs[mapnav_aprs.index('-o')+1]=str(OUT/'test_aprs_map')
    run([*mapnav_aprs,'-I'+str(host),'-DCONFIG_MAP=1',*(str(APP/'core/ui'/f) for f in
         ('mapnav.c','map.c','turkey_map_data.c','turkey_district_data.c'))],cwd=host)
    run([OUT/'test_aprs_map'],cwd=host)
    # DM32 Plus FM caller ID on / off: the gate and the ring with a fake receive path (radio_fmid.c), and the setting
    # through the actual UI storage service and settings store across power cycles
    run(['clang',*flags,'-DCONFIG_FM_ID=1','-DCONFIG_APRS_RX=1','-DCONFIG_MSG_RX=1','-I'+str(APP/'core/radio'),
         '-I'+str(ROOT/'tests/audio/stubs'),ROOT/'tests/plus/test_fmid_gate.c',APP/'core/radio/radio_fmid.c',
         APP/'core/radio/fmid.c','-lm','-o',OUT/'test_fmid_gate'],cwd=ROOT)
    run([OUT/'test_fmid_gate'],cwd=ROOT)
    run(['clang',*flags,'-ffunction-sections','-fdata-sections','-DCONFIG_FM_ID=1',
         '-DCONFIG_ZONE_EDIT=0','-DCONFIG_CONTACT_EDIT=0','-DCONFIG_KEY_ASSIGN=0',
         ROOT/'tests/plus/test_fmid_set.c',APP/'core/ui/ui_svc_storage.c',
         *CODEPLUG,APP/'core/settings/settings.c',
         APP/'core/storage/storage.c',host/'sim/sim_flash.c',
         '-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections',
         '-o',OUT/'test_fmid_set'],cwd=ROOT)
    run([OUT/'test_fmid_set'],cwd=ROOT)
    # DDM32 Plus satellites' Every pass and Auto track in cp_flags / rs_flags spare bits, through the UI storage service
    run(['clang',*flags,'-ffunction-sections','-fdata-sections','-DCONFIG_FM_ID=1','-DCONFIG_SATELLITES=1',
         '-DCONFIG_ZONE_EDIT=0','-DCONFIG_CONTACT_EDIT=0','-DCONFIG_KEY_ASSIGN=0',
         ROOT/'tests/plus/test_sat_set.c',APP/'core/ui/ui_svc_storage.c',
         *CODEPLUG,APP/'core/settings/settings.c',
         APP/'core/storage/storage.c',host/'sim/sim_flash.c',
         '-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections',
         '-o',OUT/'test_sat_set'],cwd=ROOT)
    run([OUT/'test_sat_set'],cwd=ROOT)
    # DM32 Plus multi-watch: Watch channels and the ring's pane 1 letter in settings_t.dual_state's spare bits
    run(['clang',*flags,'-ffunction-sections','-fdata-sections','-DCONFIG_MULTI_WATCH=1',
         '-DCONFIG_ZONE_EDIT=0','-DCONFIG_CONTACT_EDIT=0','-DCONFIG_KEY_ASSIGN=0',
         ROOT/'tests/plus/test_mwatch_set.c',APP/'core/ui/ui_svc_storage.c',
         *CODEPLUG,APP/'core/settings/settings.c',
         APP/'core/storage/storage.c',host/'sim/sim_flash.c',
         '-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections',
         '-o',OUT/'test_mwatch_set'],cwd=ROOT)
    run([OUT/'test_mwatch_set'],cwd=ROOT)
    # DM32 Plus every mode in the VFO: the VFOs' modes and parameters in settings_t's spare bits (vfox.c) on the actual
    # settings store, an older record unchanged, what upstream firmware reads back; the ring letters' compact VFOs
    run(['clang',*flags,'-DCONFIG_VFO_MODES=1','-DCONFIG_YSF=1','-DCONFIG_NXDN=1','-DCONFIG_DPMR=1','-DCONFIG_DMR_ANY=1',
         ROOT/'tests/plus/test_vfox_set.c',APP/'core/radio/vfox.c',APP/'core/radio/radio_am.c',APP/'core/radio/tones.c',
         APP/'core/settings/settings.c',APP/'core/storage/storage.c',host/'sim/sim_flash.c','-o',OUT/'test_vfox_set'],cwd=ROOT)
    run([OUT/'test_vfox_set'],cwd=ROOT)
    # DM32 Plus SSTV decoder: every mode, VIS, noise, clock and frequency offsets, no VIS (integration/radiofoundry/sstv)
    # and its radio task glue (rows to the display) with the audio tests' RTOS stubs
    run(['clang',*flags,'-O1','-DCONFIG_SSTV=1','-DCONFIG_MSG_RX=1','-I'+str(APP/'core/radio'),
         '-I'+str(ROOT/'tests/audio/stubs'),'-DpvPortMalloc=__builtin_malloc','-DvPortFree=__builtin_free',
         ROOT/'tests/plus/test_sstv.c',APP/'core/radio/sstv.c',APP/'core/radio/radio_sstv.c','-lm',
         '-o',OUT/'test_sstv'],cwd=ROOT)
    run([OUT/'test_sstv'],cwd=ROOT)
    # DM32 Plus CW decoder (integration/radiofoundry/cw): synthetic MCW at 5-35 WPM with jitter, weight, noise, fading,
    # tone drift; prosigns; speed changes; no text from noise; the radio task glue (the audio tests' RTOS stubs)
    run(['clang',*flags,'-O1','-DCONFIG_CW_RX=1','-DCONFIG_MSG_RX=1','-I'+str(ROOT/'tests/audio/stubs'),
         '-DpvPortMalloc=__builtin_malloc','-DvPortFree=__builtin_free',ROOT/'tests/plus/test_cwdec.c',
         APP/'core/radio/cwdec.c',APP/'core/radio/radio_cw.c','-lm','-o',OUT/'test_cwdec'],cwd=ROOT)
    run([OUT/'test_cwdec'],cwd=ROOT)
    # DM32 Plus CW practice: Koch draw, score, timing, the hand-over, the beep generator's Morse (beep.c BEEP_MORSE)
    run(['clang',*flags,'-O1','-DCONFIG_CW_RX=1','-DCONFIG_BEEP=1',ROOT/'tests/plus/test_cw_train.c',
         APP/'core/radio/cw_train.c',APP/'core/radio/morse.c',APP/'core/radio/beep.c',APP/'core/radio/cwdec.c',
         '-o',OUT/'test_cw_train'],cwd=ROOT)
    run([OUT/'test_cw_train'],cwd=ROOT)
    fmdec_tests(flags,host)
    # DM32 Plus Turkey map (docs/map.md) and art (docs/art.md)
    run(['clang',*flags,'-DCONFIG_MAP=1','-I'+str(APP/'core/ui'),ROOT/'tests/plus/test_map.c',APP/'core/ui/map.c',
         APP/'core/ui/turkey_map_data.c',
         APP/'core/ui/turkey_district_data.c','-o',OUT/'test_map'],cwd=ROOT)
    run([OUT/'test_map'],cwd=ROOT)
    # DM32 Plus map navigation core (prepare_mapnav): station pick, track ring and drawing, auto repeater hysteresis
    run(['clang',*flags,'-DCONFIG_MAP=1','-DCONFIG_AUTO_RPT=1','-DCONFIG_REPEATERS=1','-DCONFIG_APRS=1',
         '-I'+str(APP/'core/ui'),ROOT/'tests/plus/test_mapnav.c',APP/'core/ui/mapnav.c',APP/'core/ui/map.c',
         APP/'core/ui/turkey_map_data.c',APP/'core/ui/turkey_district_data.c',APP/'core/ui/rpt.c',
         APP/'core/aprs/aprs_fmt.c','-lm','-o',OUT/'test_mapnav'],cwd=ROOT)
    run([OUT/'test_mapnav'],cwd=ROOT)
    run(['python3',ROOT/'tools/art_build.py','--check'],cwd=ROOT)
    run(['clang',*flags,'-DART_HOST_TEST','-I'+str(ROOT/'integration/radiofoundry/art'),'-I'+str(ROOT/'tests/plus'),
         ROOT/'tests/plus/test_art.c',ROOT/'integration/radiofoundry/art/art.c',ROOT/'integration/radiofoundry/art/art_data.c',
         ROOT/'integration/radiofoundry/art/splash_plus.c','-o',OUT/'test_art'],cwd=ROOT)
    # DDM32 Plus public build profile (tools/plus_brand.py): no ID database, the check that refuses one
    run(['python3',ROOT/'tools/plus_brand.py','--self-test'],cwd=ROOT)
    run([OUT/'test_art',ROOT/'art'],cwd=ROOT)
    iddb_tests(flags)
    rpt_tests(flags)
    # DM32 Plus RAM reclaim: the start-up stack at the top of the RTOS heap, against the kernel's own heap_4.c
    sys.path.insert(0,str(ROOT/'tools'))
    import plus_features
    trim=plus_features.PLUS_RAM_TRIM
    kernel=APP/'third_party/FreeRTOS-Kernel'
    run(['clang','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
         '-fno-sanitize-recover=undefined','-fno-omit-frame-pointer','-I'+str(ROOT/'tests/plus/startup_heap'),
         '-I'+str(kernel/'include'),'-I'+str(kernel/'portable/ThirdParty/GCC/Posix'),
         f'-DconfigTOTAL_HEAP_SIZE={trim["PLUS_HEAP_BYTES"]}',
         f'-DPLUS_STARTUP_STACK_IN_HEAP={trim["PLUS_STARTUP_STACK_IN_HEAP"]}',
         ROOT/'tests/plus/test_startup_heap.c',kernel/'portable/MemMang/heap_4.c','-o',OUT/'test_startup_heap'],cwd=ROOT)
    run([OUT/'test_startup_heap'],cwd=ROOT)
    # DM32 Plus hotspot RAM: the shared PCM stream without its host audio side (Plus) and as upstream has it; the
    # macro refused where USRP needs that side (tools/plus_features.py prepare_audio_mux)
    mux=[ROOT/'tests/plus/test_audio_mux.c',APP/'core/usbaudio/audio_mux.c']
    mux_flags=[*flags,'-DCONFIG_APRS=1','-DCONFIG_APRS_RX=1']
    run(['clang',*mux_flags,'-DCONFIG_DMR_IP_GATEWAY=0',f'-DPLUS_AUDIO_MUX_NO_HOST={trim["PLUS_AUDIO_MUX_NO_HOST"]}',
         *mux,'-o',OUT/'test_audio_mux'],cwd=ROOT)
    run([OUT/'test_audio_mux'],cwd=ROOT)
    run(['clang',*mux_flags,*mux,APP/'core/usbaudio/pcm_slip.c','-o',OUT/'test_audio_mux_upstream'],cwd=ROOT)
    run([OUT/'test_audio_mux_upstream'],cwd=ROOT)
    # DDM32 Plus (docs/dota.md step 0): and without its APRS ring (the DM-32 has its own AFSK receive path)
    run(['clang',*mux_flags,'-DCONFIG_DMR_IP_GATEWAY=0',f'-DPLUS_AUDIO_MUX_NO_HOST={trim["PLUS_AUDIO_MUX_NO_HOST"]}',
         f'-DPLUS_AUDIO_MUX_NO_APRS={trim["PLUS_AUDIO_MUX_NO_APRS"]}',ROOT/'tests/plus/test_audio_mux_noaprs.c',
         APP/'core/usbaudio/audio_mux.c','-o',OUT/'test_audio_mux_noaprs'],cwd=ROOT)
    run([OUT/'test_audio_mux_noaprs'],cwd=ROOT)
    usrp=subprocess.run(['clang',*mux_flags,'-DCONFIG_DMR_IP_GATEWAY=1','-DPLUS_AUDIO_MUX_NO_HOST=1','-c',
                         APP/'core/usbaudio/audio_mux.c','-o',OUT/'audio_mux_usrp.o'],cwd=ROOT,capture_output=True,text=True)
    if usrp.returncode==0 or 'USRP needs the host audio side' not in usrp.stderr:
        raise SystemExit('PLUS_AUDIO_MUX_NO_HOST was not refused with USRP')
    # DM32 Plus hotspot (CONFIG_HOTSPOT, no split duplex): upstream's MMDVM / hotspot suites on the transformed core
    for name in ('mmdvmTest','hotspotSfrTest','hotspotDataTest','hotspotUplinkTest'):
        run(['make','-B',name,'SANITIZE=1'],cwd=host)
        run([host/name],cwd=host)
    # DM32 Plus YSF layer (the reference cross-check is tools/test_ysf_ref.sh)
    run(['clang',*flags,'-I'+str(ROOT/'src'),ROOT/'tests/plus/test_ysf.c',ROOT/'src/ysf/ysf.c','-o',OUT/'test_ysf'],cwd=ROOT)
    run([OUT/'test_ysf'],cwd=ROOT)
    # DM32 Plus YSF Voice FR transmit framing against our own receiver (the MMDVMHost cross-check is
    # tools/test_ysf_tx_ref.sh)
    run(['clang',*flags,'-I'+str(ROOT/'src'),'-I'+str(ROOT/'src/p25'),ROOT/'tests/plus/test_ysf_tx.c',
         ROOT/'src/ysf/ysf_tx.c',ROOT/'src/ysf/ysf_vw.c',ROOT/'src/ysf/ysf.c',*P25SRC,
         '-o',OUT/'test_ysf_tx'],cwd=ROOT)
    run([OUT/'test_ysf_tx'],cwd=ROOT)
    # DM32 Plus YSF receive end to end through the actual channel worker (DN, VW, DG-ID, callsigns, lost records;
    # standalone: tools/test_ysf_worker.sh). Links the host IMBE codec that tools/test_receive_audio.py builds
    # (make test runs receive-audio-test first).
    codec=ROOT/'build/codec-speed-host/libdm32_imbe.a'
    if not codec.exists():
        raise SystemExit(f'{codec} missing: run python3 tools/test_receive_audio.py first')
    ysf_flags=['-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
               '-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer',
               '-I'+str(ROOT/'src/p25'),'-I'+str(ROOT/'src/codec'),'-I'+str(ROOT/'src'),
               '-I'+str(ROOT/'third_party/p25cai/include'),
               '-I'+str(ROOT/'tests/audio/stubs'),'-I'+str(ROOT/'integration/radiofoundry'),
               '-I'+str(APP/'core'),'-I'+str(APP/'hal'),
               '-DCONFIG_P25_NATIVE=1','-DCONFIG_P25_BENCH=0','-DCONFIG_DEV_TX=1','-DCONFIG_P25_CHANNELS=1',
               '-DCONFIG_DMR=1','-DCONFIG_YSF=1','-DCONFIG_P25_CALLER=1','-DCONFIG_CALL_LOG=1']
    ysf_out=OUT/'ysf-worker'
    ysf_out.mkdir(exist_ok=True)
    ysf_objects=[]
    for source in [*(ROOT/'src/p25'/f'{n}.c' for n in ('p25','p25_lcx','voice','phy_stream','selftest','audio_fifo')),*CAI,
                   ROOT/'src/ysf/ysf.c',ROOT/'src/ysf/ysf_vw.c',ROOT/'integration/radiofoundry/cpu_profile.c',
                   APP/'core/debuglink/dl_fmt.c',APP/'core/radio/beep.c',APP/'core/dmr/ambe/ambeFec.c',
                   ROOT/'tests/plus/test_ysf_worker.c']:
        obj=ysf_out/obj_name(source)
        run(['clang',*ysf_flags,'-c',source,'-o',obj],cwd=ROOT)
        ysf_objects.append(obj)
    run(['clang++','-fsanitize=address,undefined',*ysf_objects,codec,'-o',OUT/'test_ysf_worker'],cwd=ROOT)
    run([OUT/'test_ysf_worker'],cwd=ROOT)
    # DM32 Plus YSF transmit through the actual channel worker (fake modem and microphone, nothing transmits), with
    # the switch and without (then the worker refuses a YSF transmission)
    for ysf_tx in (1,0):
        tx_out=OUT/f'ysf-tx-worker-{ysf_tx}'
        tx_out.mkdir(exist_ok=True)
        tx_objects=[]
        for source in [*(ROOT/'src/p25'/f'{n}.c' for n in ('p25','p25_lcx','voice','phy_stream','selftest','audio_fifo')),*CAI,
                       ROOT/'src/ysf/ysf.c',ROOT/'src/ysf/ysf_vw.c',ROOT/'src/ysf/ysf_tx.c',
                       ROOT/'integration/radiofoundry/cpu_profile.c',APP/'core/debuglink/dl_fmt.c',
                       APP/'core/radio/beep.c',APP/'core/dmr/ambe/ambeFec.c',ROOT/'tests/plus/test_ysf_tx_worker.c']:
            obj=tx_out/obj_name(source)
            run(['clang',*ysf_flags,f'-DCONFIG_YSF_TX={ysf_tx}','-c',source,'-o',obj],cwd=ROOT)
            tx_objects.append(obj)
        run(['clang++','-fsanitize=address,undefined',*tx_objects,codec,'-o',OUT/f'test_ysf_tx_worker_{ysf_tx}'],cwd=ROOT)
        run([OUT/f'test_ysf_tx_worker_{ysf_tx}'],cwd=ROOT)
    # DM32 Plus P25 trunk following end to end through the actual channel worker: TSDUs in, TSBKs and counts out
    trunk_flags=[f for f in ysf_flags if f not in ('-DCONFIG_YSF=1','-DCONFIG_DMR=1','-DCONFIG_P25_CALLER=1','-DCONFIG_CALL_LOG=1')]
    trunk_flags+=['-DCONFIG_P25_TRUNK=1','-I'+str(ROOT/'integration/radiofoundry/trunk'),'-I'+str(ROOT/'tests/plus')]
    trunk_out=OUT/'trunk-worker'
    trunk_out.mkdir(exist_ok=True)
    trunk_objects=[]
    for source in [*(ROOT/'src/p25'/f'{n}.c' for n in ('p25','p25_lcx','voice','phy_stream','selftest','audio_fifo')),*CAI,
                   ROOT/'integration/radiofoundry/cpu_profile.c',APP/'core/debuglink/dl_fmt.c',APP/'core/radio/beep.c',
                   ROOT/'integration/radiofoundry/trunk/p25_tsbk.c',ROOT/'tests/plus/test_trunk_worker.c']:
        obj=trunk_out/obj_name(source)
        run(['clang',*trunk_flags,'-c',source,'-o',obj],cwd=ROOT)
        trunk_objects.append(obj)
    run(['clang++','-fsanitize=address,undefined',*trunk_objects,codec,'-o',OUT/'test_trunk_worker'],cwd=ROOT)
    run([OUT/'test_trunk_worker'],cwd=ROOT)
    p25_extras_tests(flags,ysf_flags,codec)
    p25nac.codeplug(flags)	# DM32 Plus separate RX / TX NAC: the record, CH_MODE 2, the transmitted NID
    p25nac.worker(ysf_flags,codec)
    # Link the actual UI storage job to the actual settings/codeplug/journal owners.
    run(['clang',*flags,'-ffunction-sections','-fdata-sections',
         '-DCONFIG_ZONE_EDIT=0','-DCONFIG_CONTACT_EDIT=0','-DCONFIG_KEY_ASSIGN=0',
         ROOT/'tests/classic/test_start_new.c',APP/'core/ui/ui_svc_storage.c',
         *CODEPLUG,APP/'core/settings/settings.c',
         APP/'core/storage/storage.c',host/'sim/sim_flash.c',
         '-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections',
         '-o',OUT/'test_start_new'],cwd=ROOT)
    run([OUT/'test_start_new'],cwd=ROOT)
    if os.environ.get('DM32_CLASSIC_BACKUP'):
        # Optional private regression input; never embedded in source or test fixtures.
        with tempfile.TemporaryDirectory(dir=OUT) as scratch:
            data=Path(scratch)/'data.bin'
            with zipfile.ZipFile(os.environ['DM32_CLASSIC_BACKUP']) as archive:
                data.write_bytes(archive.read('data.bin'))
            run([OUT/'test_start_new',data],cwd=ROOT)
    # Run the existing comprehensive FM/DMR suite against the transformed core.
    run(['make','-B','fd6818Test','SANITIZE=1',
         'RFBUS='+str(APP/'drivers/rf/rfbus/rfbus.c')+' '+str(APP/'drivers/rf/fd6818/fd6818_setup_b.c')],cwd=host)
    run([host/'fd6818Test'],cwd=host)
    run(['make','-B','hrc7000DmrTest','SANITIZE=1'],cwd=host)
    run([host/'hrc7000DmrTest'],cwd=host)
    # DDM32 Plus upstream RadioFoundry fixes: the same two suites with CONFIG_RF_FIXES=1 (tools/plus_rfports.py)
    run(['python3',ROOT/'tools/test_rfports.py'],cwd=ROOT)
    run(['make','-B','radioCoreTest','SANITIZE=1'],cwd=host)
    run([host/'radioCoreTest'],cwd=host)
    fixture=(host/'radioCoreTest.c').read_text().replace('int main(void)','int upstream_radio_main(void)')
    fixture=fixture.replace('const board_t board = {','const board_t board = {\n\t.native_modem=&classic_ops,')
    # Compile the generated radio task's real service/retained-request dispatch.
    task=(APP/'core/radio/radio_task.c').read_text()
    first=task.index('\tfor (;;) {')+len('\tfor (;;) {')
    service=task[first:task.index('\n\t\tTickType_t due',first)]
    first=task.index('        if (retained_request ||')
    dispatch=task[first:task.index('\n\t\tTickType_t now',first)]
    (OUT/'classic_dispatch.inc').write_text(service+'\n'+dispatch+'\n')
    wrapper=OUT/'radioCoreClassic.c'
    wrapper.write_text('#include "native_modem/native_lab.h"\nextern const nt_ops classic_ops;\n'+fixture+
                       '\n#include "'+str(ROOT/'tests/classic/test_radio.inc')+'"\n')
    command=run(['make','-Bn','radioCoreTest','SANITIZE=1'],cwd=host,capture_output=True,text=True).stdout
    argv=shlex.split(next(line for line in command.splitlines() if ' -o radioCoreTest ' in line))
    argv[argv.index('radioCoreTest.c')]=str(wrapper)
    argv[argv.index('-o')+1]=str(OUT/'test_radio')
    argv+=['-I'+str(OUT),'-I'+str(host),'-I'+str(ROOT/'tests/native/stubs'),'-I'+str(APP/'core'),
           '-I'+str(ROOT/'src/modem'),'-I'+str(ROOT/'integration/radiofoundry'),
           '-DCONFIG_P25_CHANNELS=1','-DCONFIG_P25_NATIVE=1','-DCONFIG_P25=1','-DCONFIG_DEV_TX=1',
           str(ROOT/'src/modem/native_test.c'),str(ROOT/'integration/radiofoundry/native/native_lab.c')]
    run(argv,cwd=host)
    run([OUT/'test_radio'],cwd=host)
    # DM32 Plus: the same suite with P25 scan compiled in (docs/plus.md)
    argv[argv.index('-o')+1]=str(OUT/'test_radio_scan')
    run([*argv,'-DCONFIG_P25_SCAN=1','-DCONFIG_SPECTRUM=1','-DCONFIG_FOX=1','-DCONFIG_CW_BEACON=1','-DCONFIG_P25_SEARCH=1','-DCONFIG_P25_CALLER=1','-DCONFIG_P25_ANY=1','-I'+str(ROOT/'tests/classic'),
         str(APP/'core/radio/spectrum.c'),str(APP/'core/radio/fox.c'),str(APP/'core/radio/cw_beacon.c'),str(APP/'core/radio/p25search.c')],cwd=host)
    run([OUT/'test_radio_scan'],cwd=host)
    # DM32 Plus: the DMR search in the same suite, built without the P25 search (either builds without the other)
    argv[argv.index('-o')+1]=str(OUT/'test_radio_dmrsearch')
    run([*argv,'-DCONFIG_DMR_ANY=1','-DCONFIG_DMR_SEARCH=1','-I'+str(ROOT/'tests/plus'),str(APP/'core/radio/dmrsearch.c')],cwd=host)
    run([OUT/'test_radio_dmrsearch'],cwd=host)
    # DDM32 Plus: the all digital search in the same suite (the P25 scan / search and DMR search built in: it is both)
    argv[argv.index('-o')+1]=str(OUT/'test_radio_allsearch')
    run([*argv,'-DCONFIG_P25_SCAN=1','-DCONFIG_SPECTRUM=1','-DCONFIG_FOX=1','-DCONFIG_CW_BEACON=1','-DCONFIG_P25_SEARCH=1',
         '-DCONFIG_P25_CALLER=1','-DCONFIG_P25_ANY=1','-DCONFIG_DMR_ANY=1','-DCONFIG_DMR_SEARCH=1','-DCONFIG_ALL_SEARCH=1',
         '-I'+str(ROOT/'tests/classic'),'-I'+str(ROOT/'tests/plus'),str(APP/'core/radio/spectrum.c'),
         str(APP/'core/radio/fox.c'),str(APP/'core/radio/cw_beacon.c'),str(APP/'core/radio/p25search.c'),
         str(APP/'core/radio/dmrsearch.c')],cwd=host)
    run([OUT/'test_radio_allsearch'],cwd=host)
    # DDM32 Plus: the all digital search with YSF built in: a YSF find's callsign is the probe's call's only
    # (tests/plus/test_allsearch_ysf_radio.inc; test_radio.inc's main is not run in this build)
    allysf=OUT/'radioCoreAllYsf.c'
    allysf.write_text('#include "native_modem/native_lab.h"\nextern const nt_ops classic_ops;\n'+fixture+
                      '\n#define main classic_radio_main\n#include "'+str(ROOT/'tests/classic/test_radio.inc')+'"\n'
                      '#undef main\n#include "'+str(ROOT/'tests/plus/test_allsearch_ysf_radio.inc')+'"\n')
    argv_ay=[str(allysf) if a==str(wrapper) else a for a in argv]
    argv_ay[argv_ay.index('-o')+1]=str(OUT/'test_radio_allsearch_ysf')
    run([*argv_ay,'-DCONFIG_P25_SCAN=1','-DCONFIG_SPECTRUM=1','-DCONFIG_FOX=1','-DCONFIG_CW_BEACON=1',
         '-DCONFIG_P25_SEARCH=1','-DCONFIG_P25_CALLER=1','-DCONFIG_P25_ANY=1','-DCONFIG_DMR_ANY=1','-DCONFIG_DMR_SEARCH=1',
         '-DCONFIG_ALL_SEARCH=1','-DCONFIG_YSF=1','-I'+str(ROOT/'tests/classic'),'-I'+str(ROOT/'tests/plus'),
         str(APP/'core/radio/spectrum.c'),str(APP/'core/radio/fox.c'),str(APP/'core/radio/cw_beacon.c'),
         str(APP/'core/radio/p25search.c'),str(APP/'core/radio/dmrsearch.c')],cwd=host)
    run([OUT/'test_radio_allsearch_ysf'],cwd=host)
    # DM32 Plus: P25 trunk following in the same suite (its own build; the worker's TSBK queue is a fixture)
    argv[argv.index('-o')+1]=str(OUT/'test_radio_trunk')
    run([*argv,'-DCONFIG_P25_TRUNK=1','-I'+str(ROOT/'tests/plus'),str(APP/'core/radio/p25trunk.c')],cwd=host)
    run([OUT/'test_radio_trunk'],cwd=host)
    # DM32 Plus: DMR Tier III trunk following in the same suite (CSBKs through the real call stack)
    argv[argv.index('-o')+1]=str(OUT/'test_radio_dmrtrunk')
    run([*argv,'-DCONFIG_DMR_ANY=1','-DCONFIG_DMR_TRUNK=1','-I'+str(ROOT/'tests/plus'),str(APP/'core/radio/dmrtrunk.c'),
         str(APP/'core/radio/dmr_csbk.c')],cwd=host)
    run([OUT/'test_radio_dmrtrunk'],cwd=host)
    # DM32 Plus multi-watch: the ring of 3..6 channels in the radio core, after the whole suite above (upstream's dual
    # watch included) runs again with it built in; P25 scan on for the ring's P25 channel
    mwatch=OUT/'radioCoreMwatch.c'
    mwatch.write_text('#include "native_modem/native_lab.h"\nextern const nt_ops classic_ops;\n'+fixture+
                      '\n#define main classic_radio_main\n#include "'+str(ROOT/'tests/classic/test_radio.inc')+'"\n#undef main\n'
                      '#include "'+str(ROOT/'tests/plus/test_mwatch_radio.inc')+'"\n')
    argv[argv.index(str(wrapper))]=str(mwatch)
    argv[argv.index('-o')+1]=str(OUT/'test_radio_mwatch')
    run([*argv,'-DCONFIG_MULTI_WATCH=1','-DCONFIG_P25_SCAN=1','-DCONFIG_SPECTRUM=1','-DCONFIG_FOX=1','-DCONFIG_CW_BEACON=1',
         '-DCONFIG_P25_SEARCH=1','-DCONFIG_P25_CALLER=1','-DCONFIG_P25_ANY=1','-I'+str(ROOT/'tests/classic'),
         str(APP/'core/radio/spectrum.c'),str(APP/'core/radio/fox.c'),str(APP/'core/radio/cw_beacon.c'),
         str(APP/'core/radio/p25search.c')],cwd=host)
    run([OUT/'test_radio_mwatch'],cwd=host)
    # DM32 Plus every mode in the VFO (tests/plus/test_vfox_radio.inc): the VFO in each mode, its TX gates, the modes
    # kept across a restart, C..F's own VFOs in the ring; after upstream's radio suite (its VFO persistence), the Classic /
    # Plus P25 suites and the multi-watch ring run again with it built in
    vfox=OUT/'radioCoreVfox.c'
    vfox.write_text('#include "native_modem/native_lab.h"\nextern const nt_ops classic_ops;\n'+
                    fixture.replace('\t.native_modem=&classic_ops,','\t.native_modem=&classic_ops,\n\t.caps=CAP_AM_RX,')+
                    '\n#define main classic_radio_main\n#include "'+str(ROOT/'tests/classic/test_radio.inc')+'"\n#undef main\n'
                    '#define main mwatch_radio_main\n#include "'+str(ROOT/'tests/plus/test_mwatch_radio.inc')+'"\n#undef main\n'
                    '#include "'+str(ROOT/'tests/plus/test_vfox_radio.inc')+'"\n')
    argv[argv.index(str(mwatch))]=str(vfox)
    argv[argv.index('-o')+1]=str(OUT/'test_radio_vfox')
    run([*argv,'-DCONFIG_MULTI_WATCH=1','-DCONFIG_P25_SCAN=1','-DCONFIG_SPECTRUM=1','-DCONFIG_FOX=1','-DCONFIG_CW_BEACON=1',
         '-DCONFIG_P25_SEARCH=1','-DCONFIG_P25_CALLER=1','-DCONFIG_P25_ANY=1','-DCONFIG_VFO_MODES=1','-DCONFIG_YSF=1',
         '-DCONFIG_NXDN=1','-DCONFIG_DPMR=1','-DCONFIG_DMR_ANY=1','-I'+str(ROOT/'tests/classic'),
         str(APP/'core/radio/spectrum.c'),str(APP/'core/radio/fox.c'),str(APP/'core/radio/cw_beacon.c'),
         str(APP/'core/radio/p25search.c'),str(APP/'core/radio/vfox.c')],cwd=host)
    run([OUT/'test_radio_vfox'],cwd=host)
    # DDM32 Plus satellites' auto track, the radio core's hold (tests/plus/test_sathold_radio.inc): upstream's and
    # Classic's radio suites again with CONFIG_SATELLITES built in, then the hold from a channel and the VFO, refused
    # while busy, TX refused while held, and the radio core's settings save keeping the auto track bits
    sathold=OUT/'radioCoreSathold.c'
    sathold.write_text('#include "native_modem/native_lab.h"\nextern const nt_ops classic_ops;\n#include "radio/sat.h"\n'+
                       fixture+'\n#define main classic_radio_main\n#include "'+str(ROOT/'tests/classic/test_radio.inc')+
                       '"\n#undef main\n#include "'+str(ROOT/'tests/plus/test_sathold_radio.inc')+'"\n')
    argv_sh=[str(sathold) if a==str(vfox) else a for a in argv]
    argv_sh[argv_sh.index('-o')+1]=str(OUT/'test_radio_sathold')
    run([*argv_sh,'-DCONFIG_SATELLITES=1'],cwd=host)
    run([OUT/'test_radio_sathold'],cwd=host)
    # DM32 Plus priority channel and temporary skip (prepare_scanx): the radio core's priority stops and looks, nuisance
    # delete and Clear skips, after every suite above (multi-watch's included) runs again with it built in
    scanx=OUT/'radioCoreScanx.c'
    scanx.write_text('#include "native_modem/native_lab.h"\nextern const nt_ops classic_ops;\n'+fixture+
                     '\n#define main classic_radio_main\n#include "'+str(ROOT/'tests/classic/test_radio.inc')+'"\n#undef main\n'
                     '#define main mwatch_radio_main\n#include "'+str(ROOT/'tests/plus/test_mwatch_radio.inc')+'"\n#undef main\n'
                     '#include "'+str(ROOT/'tests/plus/test_scanx_radio.inc')+'"\n')
    argv[argv.index(str(vfox))]=str(scanx)
    argv[argv.index('-o')+1]=str(OUT/'test_radio_scanx')
    run([*argv,'-DCONFIG_MULTI_WATCH=1','-DCONFIG_P25_SCAN=1','-DCONFIG_SPECTRUM=1','-DCONFIG_FOX=1','-DCONFIG_CW_BEACON=1',
         '-DCONFIG_P25_SEARCH=1','-DCONFIG_P25_CALLER=1','-DCONFIG_P25_ANY=1','-DCONFIG_SCAN_PRIO=1','-I'+str(ROOT/'tests/classic'),
         str(APP/'core/radio/spectrum.c'),str(APP/'core/radio/fox.c'),str(APP/'core/radio/cw_beacon.c'),
         str(APP/'core/radio/p25search.c')],cwd=host)
    run([OUT/'test_radio_scanx'],cwd=host)
    # DM32 Plus YSF transmit gates (docs/ysf-nxdn-tx.md) after the whole suite, with the switch and without
    ysftx=OUT/'radioCoreYsfTx.c'
    ysftx.write_text('#include "native_modem/native_lab.h"\nextern const nt_ops classic_ops;\n'+fixture+
                     '\n#define main classic_radio_main\n#include "'+str(ROOT/'tests/classic/test_radio.inc')+'"\n'
                     '#undef main\n#include "'+str(ROOT/'tests/plus/test_ysftx_radio.inc')+'"\n')
    argv[argv.index(str(scanx))]=str(ysftx)
    for ysf_tx in (0,1):
        argv_tx=[a for a in argv if a!='-DCONFIG_MULTI_WATCH=1']
        argv_tx[argv_tx.index('-o')+1]=str(OUT/f'test_radio_ysftx_{ysf_tx}')
        run([*argv_tx,'-DCONFIG_YSF=1',f'-DCONFIG_YSF_TX={ysf_tx}'],cwd=host)
        run([OUT/f'test_radio_ysftx_{ysf_tx}'],cwd=host)
    # DM32 Plus separate RX / TX NAC in the radio core (tests/plus/test_p25nac_radio.inc), the VFO's modes built in
    p25nac.radio(argv,ysftx,fixture,['-DCONFIG_MULTI_WATCH=1','-DCONFIG_P25_SCAN=1','-DCONFIG_SPECTRUM=1',
        '-DCONFIG_FOX=1','-DCONFIG_CW_BEACON=1','-DCONFIG_P25_SEARCH=1','-DCONFIG_P25_CALLER=1','-DCONFIG_P25_ANY=1',
        '-DCONFIG_VFO_MODES=1','-DCONFIG_YSF=1','-DCONFIG_NXDN=1','-DCONFIG_DPMR=1','-DCONFIG_DMR_ANY=1',
        '-I'+str(ROOT/'tests/classic'),str(APP/'core/radio/spectrum.c'),str(APP/'core/radio/fox.c'),
        str(APP/'core/radio/cw_beacon.c'),str(APP/'core/radio/p25search.c'),str(APP/'core/radio/vfox.c')])
    # DM32 Plus CW practice in the radio core: upstream's and Classic's radio suites again with it built in, then the
    # practice's Morse through the real beep stream, decoded and timed, and no TX request of any kind
    # (tests/plus/test_cw_radio.inc)
    cwrad=OUT/'radioCoreCw.c'
    cwrad.write_text('#include "native_modem/native_lab.h"\nextern const nt_ops classic_ops;\n'+fixture+
                     '\n#define main classic_radio_main\n#include "'+str(ROOT/'tests/classic/test_radio.inc')+'"\n#undef main\n'
                     '#include "'+str(ROOT/'tests/plus/test_cw_radio.inc')+'"\n')
    argv[argv.index(str(ysftx))]=str(cwrad)
    argv_cw=[a for a in argv if a!='-DCONFIG_MULTI_WATCH=1']
    argv_cw[argv_cw.index('-o')+1]=str(OUT/'test_radio_cw')
    run([*argv_cw,'-DCONFIG_CW_RX=1','-I'+str(ROOT/'tests/classic'),str(APP/'core/radio/cw_train.c'),
         str(APP/'core/radio/cwdec.c')],cwd=host)
    run([OUT/'test_radio_cw'],cwd=host)
    # DDM32 Plus DOTA Transfer in the radio core: the same suites with it built in (the fake board gains a fake AFSK
    # transmitter), then the session channel, PTT refused, requests dropped and released, the channel / VFO restored,
    # keyed spans (tests/plus/test_dota_radio.inc)
    dotarad=OUT/'radioCoreDota.c'
    dotarad.write_text('#include "native_modem/native_lab.h"\nextern const nt_ops classic_ops;\n'+
                       fixture.replace('const board_t board = {\n\t.native_modem=&classic_ops,',
                                       'extern const hal_afsk_t dota_afsk;\nconst board_t board = {\n'
                                       '\t.native_modem=&classic_ops,\n\t.afsk=&dota_afsk,')+
                       '\n#define main classic_radio_main\n#include "'+str(ROOT/'tests/classic/test_radio.inc')+'"\n#undef main\n'
                       '#include "'+str(ROOT/'tests/plus/test_dota_radio.inc')+'"\n')
    argv_dota=list(argv_cw)
    argv_dota[argv_dota.index(str(cwrad))]=str(dotarad)
    argv_dota[argv_dota.index('-o')+1]=str(OUT/'test_radio_dota')
    run([*argv_dota,'-DCONFIG_DOTA=1','-I'+str(ROOT/'tests/classic'),'-Wl,--wrap=beep_play',
         *(str(APP/'core/radio'/f) for f in ('dota.c','dota_proto.c','dota_rec.c','dota_import.c','xband.c')),
         str(APP/'core/aprs/ax25.c'),str(APP/'core/aprs/hdlc.c')],cwd=host)
    run([OUT/'test_radio_dota'],cwd=host)
    # DDM32 Plus cross-band store and forward in the radio core: the same suites with it built in, then a DMR call on A
    # recorded and sent on B through the real call stack, the CW ID, the lock on requests and PTT, the channel back,
    # a refused transmission, the P25 worker's memory not lent (tests/plus/test_xband_radio.inc)
    xbrad=OUT/'radioCoreXband.c'
    xbrad.write_text('#include "native_modem/native_lab.h"\nextern const nt_ops classic_ops;\n'+fixture+
                     '\n#define main classic_radio_main\n#include "'+str(ROOT/'tests/classic/test_radio.inc')+'"\n#undef main\n'
                     '#include "'+str(ROOT/'tests/plus/test_xband_radio.inc')+'"\n')
    argv_xb=list(argv_cw)
    argv_xb[argv_xb.index(str(cwrad))]=str(xbrad)
    argv_xb[argv_xb.index('-o')+1]=str(OUT/'test_radio_xband')
    run([*argv_xb,'-DCONFIG_XBAND=1','-I'+str(ROOT/'tests/classic'),'-Wl,--wrap=beep_play',
         str(APP/'core/radio/xband.c')],cwd=host)
    run([OUT/'test_radio_xband'],cwd=host)
    # DDM32 Plus SSTV send in the radio core: the same suites with it built in, then the test picture keyed,
    # played whole and stopped every way (tools/test_sstvtx.py, tests/plus/test_sstvtx_radio.inc)
    import test_sstvtx
    test_sstvtx.radio(argv_cw,cwrad,fixture,ROOT,APP,OUT,host)
    command=run(['make','-Bn','uiFlowTest','SANITIZE=1'],cwd=host,capture_output=True,text=True).stdout
    argv=shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')]=str(ROOT/'tests/classic/test_ui.c')
    argv[argv.index('-o')+1]=str(OUT/'test_ui')
    argv+=['-I'+str(host),'-DCONFIG_P25_CHANNELS=1']
    run(argv,cwd=host)
    run([OUT/'test_ui'],cwd=host)
    # DM32 Plus: the editor's Any NAC / Any talkgroup (CONFIG_P25_ANY)
    argv[argv.index(str(ROOT/'tests/classic/test_ui.c'))]=str(ROOT/'tests/plus/test_ui_any.c')
    argv[argv.index('-o')+1]=str(OUT/'test_ui_any')
    run([*argv,'-DCONFIG_P25_ANY=1','-DCONFIG_NAC_DEC=1'],cwd=host)
    run([OUT/'test_ui_any'],cwd=host)
    # DM32 Plus: the editor's SFR role rows (CONFIG_SFR_EDIT; sfr.c holds the role encoding)
    argv[argv.index(str(ROOT/'tests/plus/test_ui_any.c'))]=str(ROOT/'tests/plus/test_ui_sfr.c')
    argv[argv.index('-o')+1]=str(OUT/'test_ui_sfr')
    run([*argv,'-DCONFIG_SFR_EDIT=1','-DCONFIG_SFR=1','-ffunction-sections','-fdata-sections',str(APP/'core/dmr/sfr.c'),
         '-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections'],cwd=host)
    run([OUT/'test_ui_sfr'],cwd=host)
    # DM32 Plus: the editor's dPMR mode and colour code row (CONFIG_DPMR; the UI's reads of the receivers stubbed)
    argv[argv.index(str(ROOT/'tests/plus/test_ui_sfr.c'))]=str(ROOT/'tests/plus/test_ui_dpmr.c')
    argv[argv.index('-o')+1]=str(OUT/'test_ui_dpmr')
    run([*argv,'-DCONFIG_P25_ANY=1','-DCONFIG_DEV_TX=1','-DCONFIG_YSF=1','-DCONFIG_NXDN=1','-DCONFIG_DPMR=1',str(APP/'core/dpmr/dpmr.c')],
        cwd=host)
    run([OUT/'test_ui_dpmr'],cwd=host)
    # DM32 Plus multi-watch: Watch channels 3..6 on Home, the keys, the pickers (tests/plus/test_ui_mwatch.c). Upstream's
    # own flows run first in the same binary with it built in (Watch channels 2, dual watch's flows): they must fail
    # exactly where they fail without it (the Classic / Plus tree changes a few of upstream's expectations)
    def fails(out):
        return sorted(l for l in out.decode(errors='replace').splitlines() if l.startswith('FAIL ') and not l.startswith('FAIL MW '))
    argv[argv.index(str(ROOT/'tests/plus/test_ui_dpmr.c'))]=str(host/'uiFlowTest.c')
    argv[argv.index('-o')+1]=str(OUT/'ui_flow_base')
    run(argv,cwd=host)
    base=subprocess.run([OUT/'ui_flow_base'],cwd=host,capture_output=True).stdout
    argv[argv.index(str(host/'uiFlowTest.c'))]=str(ROOT/'tests/plus/test_ui_mwatch.c')
    argv[argv.index('-o')+1]=str(OUT/'test_ui_mwatch')
    run([*argv,'-DCONFIG_MULTI_WATCH=1'],cwd=host)
    mw=subprocess.run([OUT/'test_ui_mwatch'],cwd=host,capture_output=True)
    print('\n'.join(l for l in mw.stdout.decode(errors='replace').splitlines() if 'MW ' in l or 'multi-watch' in l))
    if mw.returncode!=0:
        raise SystemExit('multi-watch UI flows failed')
    if fails(base)!=fails(mw.stdout):
        raise SystemExit('upstream UI flows fail differently with multi-watch built in:\n'+
                         '\n'.join(sorted(set(fails(base))^set(fails(mw.stdout)))))
    print(f'upstream UI flows with multi-watch built in: the same {len(fails(base))} failure lines as without')
    # DM32 Plus every mode in the VFO and the multi-watch key (tests/plus/test_ui_vfox.c): the Mode tile's cycle, VFO
    # settings (TG, NAC hex / decimal / Any, DG-ID, RAN, dPMR CC, DMR Any), Save as new channel, C..F's own VFOs
    argv[argv.index(str(ROOT/'tests/plus/test_ui_mwatch.c'))]=str(ROOT/'tests/plus/test_ui_vfox.c')
    argv[argv.index('-o')+1]=str(OUT/'test_ui_vfox')
    run([*argv,'-DCONFIG_P25_ANY=1','-DCONFIG_DEV_TX=1','-DCONFIG_YSF=1','-DCONFIG_NXDN=1','-DCONFIG_DPMR=1',
         '-DCONFIG_NAC_DEC=1','-DCONFIG_DMR_ANY=1','-DCONFIG_MULTI_WATCH=1','-DCONFIG_VFO_MODES=1','-DCONFIG_P25_NATIVE=1',
         str(APP/'core/dpmr/dpmr.c'),str(APP/'core/radio/vfox.c')],cwd=host)
    run([OUT/'test_ui_vfox'],cwd=host)
    # DDM32 Plus Home details (tests/plus/test_ui_mwdetail.c, tools/plus_mwfix.py): the formatter for every mode, the
    # multi-watch rows and dual watch's panes in English and Turkish, normal and Large text, with the layout checks
    run(['make','out/gen/lang_tables.h'],cwd=host,stdout=subprocess.DEVNULL)
    argv_md=[str(ROOT/'tests/plus/test_ui_mwdetail.c') if a==str(ROOT/'tests/plus/test_ui_vfox.c') else a for a in argv]
    argv_md[argv_md.index('-o')+1]=str(OUT/'test_ui_mwdetail')
    run([*argv_md,'-DCONFIG_P25_ANY=1','-DCONFIG_DEV_TX=1','-DCONFIG_YSF=1','-DCONFIG_NXDN=1','-DCONFIG_DPMR=1',
         '-DCONFIG_NAC_DEC=1','-DCONFIG_DMR_ANY=1','-DCONFIG_MULTI_WATCH=1','-DCONFIG_VFO_MODES=1','-DCONFIG_P25_NATIVE=1',
         '-DCONFIG_P25_TXNAC=1','-DCONFIG_LANG_TR=1','-I'+str(host),
         str(APP/'core/dpmr/dpmr.c'),str(APP/'core/radio/vfox.c')],cwd=host)
    run([OUT/'test_ui_mwdetail'],cwd=host)
    # DDM32 Plus the heard call's talk group on the multi-watch rows and dual watch's panes (tests/plus/test_ui_mwheard.c,
    # tools/plus_mwfix.py seventh round): a DMR call on a letter in a pane and outside, private, 4 and 6 letters, a P25 VFO
    # letter (the P25 worker's caller stubbed), normal and Large text, with the layout checks
    argv_mh=[str(ROOT/'tests/plus/test_ui_mwheard.c') if a==str(ROOT/'tests/plus/test_ui_vfox.c') else a for a in argv]
    argv_mh[argv_mh.index('-o')+1]=str(OUT/'test_ui_mwheard')
    run([*argv_mh,'-DCONFIG_P25_ANY=1','-DCONFIG_DEV_TX=1','-DCONFIG_YSF=1','-DCONFIG_NXDN=1','-DCONFIG_DPMR=1',
         '-DCONFIG_NAC_DEC=1','-DCONFIG_DMR_ANY=1','-DCONFIG_MULTI_WATCH=1','-DCONFIG_VFO_MODES=1','-DCONFIG_P25_NATIVE=1',
         '-DCONFIG_P25_TXNAC=1','-DCONFIG_CALL_LOG=1','-DCONFIG_P25_CALLER=1','-DCONFIG_P25_EXTRAS=1','-DCONFIG_LANG_TR=1','-I'+str(host),
         str(APP/'core/dpmr/dpmr.c'),str(APP/'core/radio/vfox.c')],cwd=host)
    run([OUT/'test_ui_mwheard'],cwd=host)
    # DM32 Plus separate RX / TX NAC: the editor's and VFO settings' rows (tests/plus/test_ui_p25nac.c)
    p25nac.ui(argv,ROOT/'tests/plus/test_ui_vfox.c',['-DCONFIG_P25_ANY=1','-DCONFIG_DEV_TX=1','-DCONFIG_YSF=1',
        '-DCONFIG_NXDN=1','-DCONFIG_DPMR=1','-DCONFIG_NAC_DEC=1','-DCONFIG_DMR_ANY=1','-DCONFIG_MULTI_WATCH=1',
        '-DCONFIG_VFO_MODES=1','-DCONFIG_P25_NATIVE=1',str(APP/'core/dpmr/dpmr.c'),str(APP/'core/radio/vfox.c')])
    # DM32 Plus map navigation UI (prepare_mapnav): Tools > Auto repeater and its offers, APRS stations on the map, the
    # track, through the real UI; the simulator wrapped by tests/plus/ui_sim_mapnav.c (the test's GPS and stations,
    # the tune path's requests recorded)
    command=run(['make','-Bn','uiFlowTest','SANITIZE=1'],cwd=host,capture_output=True,text=True).stdout
    argv=shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')]=str(ROOT/'tests/plus/test_ui_mapnav.c')
    argv[argv.index('sim/ui_sim.c')]=str(ROOT/'tests/plus/ui_sim_mapnav.c')
    argv[argv.index('-o')+1]=str(OUT/'test_ui_mapnav')
    run([*argv,'-I'+str(host),'-I'+str(ROOT/'tests/plus'),'-DCONFIG_P25_CHANNELS=1','-DCONFIG_MAP=1','-DCONFIG_NAV=1',
         '-DCONFIG_REPEATERS=1','-DCONFIG_AUTO_RPT=1','-DCONFIG_MAP_TRACK=1','-DCONFIG_APRS_MSG=1',
         *(str(APP/'core/ui'/f) for f in ('ui_map.c','map.c','turkey_map_data.c','turkey_district_data.c','mapnav.c',
                                         'ui_mapnav.c','nav.c','ui_goto.c','rpt.c','ui_rpt.c','ui_aprsmsg.c')),
         *(str(APP/'core/aprs'/f) for f in ('aprs_plus.c','aprs_fmt.c','ax25.c')),'-lm'],cwd=host)
    run([OUT/'test_ui_mapnav'],cwd=host)
    # DM32 Plus priority channel and temporary skip UI (prepare_scanx): 0 skips, # 4 / # 5, the marks, the setting, the
    # zone editor's action (tests/plus/test_ui_scanx.c, the simulator's scan wrapped by tests/plus/ui_sim_scanx.c). Upstream's
    # flows run first in the same binary and must fail exactly where they fail without it
    command=run(['make','-Bn','uiFlowTest','SANITIZE=1'],cwd=host,capture_output=True,text=True).stdout
    argv=shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')]=str(ROOT/'tests/plus/test_ui_scanx.c')
    argv[argv.index('sim/ui_sim.c')]=str(ROOT/'tests/plus/ui_sim_scanx.c')
    argv[argv.index('-o')+1]=str(OUT/'test_ui_scanx')
    run([*argv,'-I'+str(host),'-DCONFIG_P25_CHANNELS=1','-DCONFIG_SCAN_PRIO=1',str(APP/'core/ui/ui_scanx.c')],cwd=host)
    sx=subprocess.run([OUT/'test_ui_scanx'],cwd=host,capture_output=True)
    print('\n'.join(l for l in sx.stdout.decode(errors='replace').splitlines() if 'SX ' in l or 'temporary skip' in l))
    if sx.returncode!=0:
        raise SystemExit('priority channel and temporary skip UI flows failed')
    if fails(base)!=fails(sx.stdout):
        raise SystemExit('upstream UI flows fail differently with the priority channel built in:\n'+
                         '\n'.join(sorted(set(fails(base))^set(fails(sx.stdout)))))
    print(f'upstream UI flows with the priority channel built in: the same {len(fails(base))} failure lines as without')
    # DM32 Plus Tools > CW decoder and CW practice through the real UI (tests/plus/test_ui_cw.c: the radio task's side
    # faked; cw_train.c real; no TX request)
    command=run(['make','-Bn','uiFlowTest','SANITIZE=1'],cwd=host,capture_output=True,text=True).stdout
    argv=shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')]=str(ROOT/'tests/plus/test_ui_cw.c')
    argv[argv.index('-o')+1]=str(OUT/'test_ui_cw')
    run([*argv,'-I'+str(host),'-DCONFIG_P25_CHANNELS=1','-DCONFIG_MSG_RX=1','-DCONFIG_CW_RX=1',
         str(APP/'core/radio/cw_train.c'),str(APP/'core/radio/morse.c')],cwd=host)
    run([OUT/'test_ui_cw'],cwd=host)
    # DM32 Plus round 3 misc job: airfields, marine VHF, QSO log, battery graph, PC live panel (tools/test_misc.py)
    import test_misc
    test_misc.main(ROOT,APP,OUT,flags,host)
    # DM32 Plus Tools > Repeaters by province (tools/test_rptprov.py): rpt_list.py provinces, the screen's flows
    import test_rptprov
    test_rptprov.main(ROOT,APP,OUT,flags,host)
    # DDM32 Plus round 13 ideas 1: find saves, find my friend, locator, one-key SOS (tools/test_fieldx.py)
    import test_fieldx
    test_fieldx.main(ROOT,APP,OUT,flags,host)
    # DDM32 Plus round 13 ideas 2: shared screen buffer, heard list, waterfall, coverage, GPS, clock (tools/test_toolbox.py)
    import test_toolbox
    test_toolbox.main(ROOT,APP,OUT,flags,host)
    # DDM32 Plus DOTA Transfer (tools/test_dota.py, docs/dota.md 16.1)
    import test_dota
    test_dota.main(ROOT,APP,OUT,flags,host)
    # DDM32 Plus round 14 APRS job: SOS key actions, Mic-E beacons, the station page (tools/test_aprsx.py)
    import test_aprsx
    test_aprsx.main(ROOT,APP,OUT,flags,host)
    # DDM32 Plus round 14 air items: the airfield table, air watch, the 3D waterfall (tools/test_air.py)
    import test_air
    test_air.main(ROOT,APP,OUT,flags,host)
    # DDM32 Plus user data packs (tools/test_datapack.py, docs/r14.md 7)
    import test_datapack
    test_datapack.main(ROOT,APP,OUT,flags,host)
    # DDM32 Plus round 14 xband: last calls of every mode and letter (tools/test_xband.py)
    import test_xband
    test_xband.main(ROOT,APP,OUT,flags,host)
    # DDM32 Plus SSTV send: the picture's audio decoded again, the page (tools/test_sstvtx.py)
    test_sstvtx.main(ROOT,APP,OUT,flags,host)
    # DDM32 Plus DMR receive audio diagnostics: the PCM driver's counters and prefill, the page (tools/test_dmraudio.py)
    import test_dmraudio
    test_dmraudio.main(ROOT,APP,OUT,flags,host)
    # DDM32 Plus r15 channels: zone size, DMR rows, digital monitor, Delete channel, NAC in decimal (tools/test_chux.py)
    import test_chux
    test_chux.main(ROOT,APP,OUT,flags,host)
    # DDM32 Plus round 15 scan: search ranges and speed, the multi-watch ring (tools/test_scan.py)
    import test_scan
    test_scan.main(ROOT,APP,OUT,flags,host)
    # DDM32 Plus searchfix: the surer P25 / DMR / all digital searches (tools/test_searchfix.py)
    import test_searchfix
    test_searchfix.main(ROOT,APP,OUT,flags,host)
    # DDM32 Plus status strip signal bars: filled only while something is heard (tools/test_sigbar.py)
    import test_sigbar
    test_sigbar.main(ROOT,APP,OUT,flags,host)
    # DDM32 Plus round 16 airprov: airfields by province, the spectrum in colour (tools/test_airprov.py)
    import test_airprov
    test_airprov.main(ROOT,APP,OUT,flags,host)
    # DDM32 Plus round 16 polish: the Turkish catalogue complete (tools/test_polish.py)
    import test_polish
    test_polish.main(ROOT,APP,OUT,flags,host)

if __name__=='__main__':main()
