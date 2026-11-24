# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the v1.0.2 RAM saving (tools/plus_ramsave.py), run by tools/test_classic.py after its own:

  - tests/plus/test_fbsafe_gfx.c: the same drawing with the whole panel's frame buffer and with the safe area's gives
    the same panel and whole-panel captures; then twice more with PLUS_FB_SAFE_BYTES (the radio's
    220 x 310 and an odd 221 px wide one) under AddressSanitizer: the last pixel stays inside the array;
  - tests/plus/test_fbsafe_st7789.c: upstream's st7789Test, then the window write;
  - tests/plus/test_fbsafe_gallery.c: every gallery, audit and worst-case screen of the UI simulator both ways, the
    emulated panel compared after every tick;
  - upstream's settingsTest and codeplugTest (storage.c) with PLUS_SHARED_SLOT_BUF: their whole output (cut points,
    old / new sets, sectors) must be the same as without it;
  - tools/rfoundry_debug.py --self-test: a refused packed screenshot falls back to the RGB565 form."""
import shlex
import subprocess
import sys


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def make_argv(host, target):
    command = run(['make', '-Bn', target, 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    return shlex.split(next(line for line in command.splitlines() if f' -o {target} ' in line))


def main(ROOT, APP, OUT, flags, host):
    sys.path.insert(0, str(ROOT / 'tools'))
    import plus_features
    trim = plus_features.PLUS_RAM_TRIM
    cc = ['clang', '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wshadow', '-g',
          '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer',
          '-I' + str(APP / 'core'), '-I' + str(APP / 'drivers'), '-I' + str(APP / 'hal'), '-D__attribute__(x)=']
    gfx = [APP / 'core/ui/gfx.c', APP / 'core/ui/fonts.c']
    # 1. gfx both ways
    run([*cc, '-DGFX_INDEXED=1', ROOT / 'tests/plus/test_fbsafe_gfx.c', *gfx, '-o', OUT / 'test_fbsafe_gfx'], cwd=ROOT)
    run([OUT / 'test_fbsafe_gfx'], cwd=ROOT)
    # 2. the array sized by PLUS_FB_SAFE_BYTES: the radio's safe area (the build's own number), and an odd width
    w, h, l, r, t, b = 240, 320, 10, 10, 5, 5
    if trim['PLUS_FB_SAFE_BYTES'] != (w - l - r + 1) // 2 * (h - t - b):
        raise SystemExit('PLUS_FB_SAFE_BYTES is not the DM-32UV safe area\'s frame buffer')
    for name, size, geo in (('radio', trim['PLUS_FB_SAFE_BYTES'], (w, h, l, t, w - l - r, h - t - b)),
                            ('odd', 111 * 310, (230, 320, 0, 0, 221, 310))):
        run([*cc, '-DGFX_INDEXED=1', '-DGFX_MAX_ROWS=320u', '-DFBSAFE_EDGE', f'-DPLUS_FB_SAFE_BYTES={size}u',
             *(f'-D{k}={v}' for k, v in zip(('EDGE_W', 'EDGE_H', 'EDGE_L', 'EDGE_T', 'EDGE_SW', 'EDGE_SH'), geo)),
             ROOT / 'tests/plus/test_fbsafe_gfx.c', *gfx, '-o', OUT / f'test_fbsafe_edge_{name}'], cwd=ROOT)
        run([OUT / f'test_fbsafe_edge_{name}'], cwd=ROOT)
    # 3. the panel driver's window write
    run([*cc, '-I' + str(host), ROOT / 'tests/plus/test_fbsafe_st7789.c', APP / 'drivers/display/st7789/st7789.c',
         '-o', OUT / 'test_fbsafe_st7789'], cwd=ROOT)
    run([OUT / 'test_fbsafe_st7789'], cwd=ROOT)
    # 4. every simulator screen both ways
    argv = make_argv(host, 'uiSim')
    argv[argv.index('uiSim.c')] = str(ROOT / 'tests/plus/test_fbsafe_gallery.c')
    argv[argv.index('-o') + 1] = str(OUT / 'test_fbsafe_gallery')
    run([*argv, '-I' + str(host), '-DCONFIG_P25_CHANNELS=1'], cwd=host)
    run([OUT / 'test_fbsafe_gallery'], cwd=host)
    # 5. settings and storage with one slot buffer each: the same run, line for line
    for target in ('settingsTest', 'codeplugTest'):
        outputs = []
        for shared in (False, True):
            argv = make_argv(host, target)
            exe = OUT / f'{target}_{"shared" if shared else "upstream"}'
            argv[argv.index('-o') + 1] = str(exe)
            run([*argv, *(['-DPLUS_SHARED_SLOT_BUF=1'] if shared else [])], cwd=host)
            outputs.append(run([exe], cwd=host, capture_output=True, text=True).stdout)
        if outputs[0] != outputs[1] or not outputs[1].strip():
            raise SystemExit(f'{target}: PLUS_SHARED_SLOT_BUF changed the run')
        print(f'{target} with PLUS_SHARED_SLOT_BUF: the same {len(outputs[1].splitlines())} lines as without; '
              + outputs[1].strip().splitlines()[-1])
    # 6. the host's debug tool: a packed screenshot refused as unavailable (debuglink.c, a safe-area frame buffer)
    # falls back to the RGB565 form (its self-test case "frame buffer too large to pack")
    selftest = run(['python3', APP / 'tools/rfoundry_debug.py', '--self-test'], cwd=APP, capture_output=True,
                   text=True).stdout
    if 'ok   frame buffer too large to pack: RGB565 form' not in selftest or 'self-test: ok' not in selftest:
        raise SystemExit('rfoundry_debug.py: the RGB565 fallback of a refused packed screenshot failed')
    print('rfoundry_debug.py: a refused packed screenshot falls back to the RGB565 form')
