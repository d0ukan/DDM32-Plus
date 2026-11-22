#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Portions Copyright (c) 2026 The RadioFoundry Authors; see LICENSES/RadioFoundry-MIT.txt.
"""DDM32 Plus: upstream RadioFoundry fixes on our pinned tree (CONFIG_RF_FIXES, docs/plus.md "Upstream RadioFoundry
fixes"). Our pin is dfbaf20c; these come from later RadioFoundry commits (MIT, The RadioFoundry Authors), adapted to
the pin and to Classic r13's own FD6818 change. Called last from plus_features.prepare(); every hunk sits under
CONFIG_RF_FIXES, so with the switch off the tree is Classic's.

- 625b921 (#825, Spencer Fowler): the FD6818's sub-audio off on entering DMR. Classic r13 already clears 0x51 from
  the default and DMR-6943 tables (build_radiofoundry.py, fd6818_setup_b.c); this makes it the driver's rule for
  every set-up (fd6818_set_mode), and set-up B's two receive sub-audio words go back to the no-tone pair (board).
- 916ac26 (#389, Spencer Fowler): HR_C7000 lost modem requests. Every receiver restart drops pending TS/RF TX and
  system requests (the PIC acts on a line rising; one left high never interrupts again); the poll drops a system
  request pending 50 ms with no handler run (st.sys_stuck); after a stall or a stuck request the glue unmasks the
  modem sources again. Not ported: CHIP_REG, the IRQ stack canary, the Find speed-ups, the debug link health view.
- 947c139 (#541, Joshua Fuller, Spencer Fowler): polite access and receive recovery aged by wall time, not timeslot
  ticks, and the recovery also runs from the task poll: when the slot interrupts stopped, a station heard just
  before stayed "busy" and every polite PTT was refused. A poll restart mid-call reports the lost voice first.

The task-side additions in hrc7000_dmr.c and dmr_glue.c go to flash (.flashfunc): the linker script puts those two
files in RAM whole, and RAM is the Plus image's scarce resource.

Second round (prepare_rfports2, branch rfports2): f5d2fd1, 18fdd29, 3d5940b (the idea), f4b19e9 (the keying gate),
22b27e8; see its docstring."""

ON = '#if CONFIG_RF_FIXES\n'


def prepare_rfports(transform, install):
    del install
    # --- The switch ---
    transform('config/features.mk', [
        ('CONFIG_SSTV_TX ?= n\nFEATURES += SSTV_TX\n',
         'CONFIG_RF_FIXES ?= n\nFEATURES += RF_FIXES\nCONFIG_SSTV_TX ?= n\nFEATURES += SSTV_TX\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_SSTV_TX\n',
         '#ifndef CONFIG_RF_FIXES\n#define CONFIG_RF_FIXES 0\t/* DDM32 Plus upstream RadioFoundry fixes (tools/plus_rfports.py) */\n'
         '#endif\n#ifndef CONFIG_SSTV_TX\n'),
    ])

    # --- 625b921: sub-audio off on entering DMR, every set-up ---
    transform('drivers/rf/fd6818/fd6818.c', [
        ('\tif (mode == FD6818_MODE_DMR) {\n\t\tif (v->dmr != NULL) {\n\t\t\twrite_table(dev, v->dmr, v->dmr_n);\n\t\t}\n',
         '\tif (mode == FD6818_MODE_DMR) {\n\t\tif (v->dmr != NULL) {\n\t\t\twrite_table(dev, v->dmr, v->dmr_n);\n\t\t}\n'
         + ON +
         '\t\t// RadioFoundry 625b921: an FM channel\'s receive tone would stay in 0x51, and its bit 15 also keys the\n'
         '\t\t// chip\'s sub-audio transmitter: the tone on the 4FSK left every DMR burst undecodable. The driver\'s rule,\n'
         '\t\t// whatever set-up (Classic\'s tables clear it for the two the DM-32UV uses)\n'
         '\t\tfd6818_write(dev, FD6818_REG_SUBAUDIO, 0x0000);\n'
         '#endif\n'),
    ])
    transform('boards/dm32/radio_dm32.c', [
        ('\tif (m == RADIO_MODE_DMR && CONFIG_DMR) {\n\t\tfd6818_set_mode(&fd, FD6818_MODE_DMR);\n',
         '\tif (m == RADIO_MODE_DMR && CONFIG_DMR) {\n\t\tfd6818_set_mode(&fd, FD6818_MODE_DMR);\t// sub-audio off (0x51)\n'
         + ON +
         '\t\tif (rf_b) {\t// RadioFoundry 625b921: set-up B\'s receive sub-audio words back to the no-tone pair\n'
         '\t\t\tfd6818_write(&fd, FD_REG_CSS_A, 0x9608u);\n'
         '\t\t\tfd6818_write(&fd, FD_REG_CSS_B, 0x9890u);\n'
         '\t\t}\n'
         '#endif\n'),
    ])
    transform('tests/host/fd6818Test.c', [
        ('#include "rf/fd6818/fd6818.h"\n', '#include "config.h"\n#include "rf/fd6818/fd6818.h"\n'),
        ('\tCHECK(chip.log[chip.n - 1].reg == 0x47 && chip.log[chip.n - 1].val == 0x0140);\n',
         ON +
         '\ti = find_last(0x47);\n'
         '\tCHECK(i >= 0 && chip.log[i].val == 0x0140);\n'
         '\tCHECK(chip.log[chip.n - 1].reg == 0x51 && chip.log[chip.n - 1].val == 0);\t// sub-audio off (test_dmr_tone_off)\n'
         '#else\n'
         '\tCHECK(chip.log[chip.n - 1].reg == 0x47 && chip.log[chip.n - 1].val == 0x0140);\n'
         '#endif\n'),
        ('\tCHECK(chip.n == 6 && chip.log[0].reg == 0x47 && chip.log[0].val == 0x0140 && chip.log[1].reg == 0x48 &&',
         '\tCHECK(chip.n == 6 + CONFIG_RF_FIXES && chip.log[0].reg == 0x47 && chip.log[0].val == 0x0140 && chip.log[1].reg == 0x48 &&'),
        ('\tCHECK(chip.log[5].reg == 0x3F && chip.log[5].val == 0x0000);\n',
         '\tCHECK(chip.log[5].reg == 0x3F && chip.log[5].val == 0x0000);\n'
         + ON + '\tCHECK(chip.log[6].reg == 0x51 && chip.log[6].val == 0);\t// then the sub-audio off\n#endif\n'),
        ('static void test_rx_tones(void)',
         ON +
         '// RadioFoundry 625b921 (issue 685): an FM channel\'s receive tone left 0x51 set after the change to DMR, and its\n'
         '// bit 15 keys the chip\'s sub-audio transmitter. Entering DMR turns the sub-audio off on every set-up (set-up B\'s\n'
         '// first DMR table has no 0x51 word of its own), whatever tone the FM channel had\n'
         'static void test_dmr_tone_off(void)\n'
         '{\n'
         '\tstatic const fd6818_variant_t *const set_ups[] = { &fd6818_variant_default, &fd6818_variant_b,\n'
         '\t\t\t\t\t\t\t    &fd6818_variant_b_dmr6943 };\n'
         '\tstatic const fd6818_tone_t types[] = { FD6818_TONE_CTCSS, FD6818_TONE_DCS, FD6818_TONE_DCS_INV };\n'
         '\tstatic const uint16_t values[] = { 2336, 023, 0754 };\n'
         '\tfor (size_t v = 0; v < sizeof set_ups / sizeof set_ups[0]; v++) {\n'
         '\t\tfor (size_t t = 0; t < sizeof types / sizeof types[0]; t++) {\n'
         '\t\t\tfd6818_t dev;\n'
         '\t\t\treset_chip();\n'
         '\t\t\tfd6818_attach_variant(&dev, bus(), set_ups[v]);\n'
         '\t\t\tfd6818_init(&dev);\n'
         '\t\t\tfd6818_set_mode(&dev, FD6818_MODE_FM);\n'
         '\t\t\tfd6818_set_rx_tone(&dev, types[t], values[t]);\n'
         '\t\t\tCHECK((chip.regs[FD6818_REG_SUBAUDIO] & 0x8000u) != 0);\n'
         '\t\t\tfd6818_set_mode(&dev, FD6818_MODE_DMR);\n'
         '\t\t\tCHECK(chip.regs[FD6818_REG_SUBAUDIO] == 0);\n'
         '\t\t\tCHECK(chip.protocol_errors == 0);\n'
         '\t\t}\n'
         '\t}\n'
         '}\n'
         '#endif\n\n'
         'static void test_rx_tones(void)'),
        ('\ttest_rx_tones();\n', '\ttest_rx_tones();\n' + ON + '\ttest_dmr_tone_off();\n#endif\n'),
    ])

    # --- 916ac26 / 947c139: HR_C7000 lost requests and wall-time recovery ---
    transform('drivers/modem/hrc7000/hrc7000_dmr.h', [
        ('// SPDX-License-Identifier: MIT\n// Copyright (c) 2026 The RadioFoundry Authors\n',
         '// SPDX-License-Identifier: MIT\n// Copyright (c) 2026 The RadioFoundry Authors\n'
         '#ifndef CONFIG_RF_FIXES\n#define CONFIG_RF_FIXES 0\t// DDM32 Plus (tools/plus_rfports.py); the build sets it for every file\n#endif\n'),
        ('\tuint32_t stalls;\n',
         '\tuint32_t stalls;\n' + ON +
         '\tuint32_t sys_stuck;\t// system requests pending with no handler run, dropped (hrc7000_dmr_poll)\n#endif\n'),
        ('\tuint32_t t_rx_tick;\t// time of the last receive-vector tick\n',
         '\tuint32_t t_rx_tick;\t// time of the last receive-vector tick\n' + ON +
         '\tuint32_t sys_seen, t_sys_seen;\t// the stuck request check: sys_ints, and since when a request has waited\n'
         '\tbool sys_waiting;\t// a request seen pending with sys_ints at sys_seen (since t_sys_seen)\n#endif\n'),
        ('\tuint32_t resync_tick;\t// tick of the last receiver restart for lost or stale timing\n',
         ON +
         '\tuint32_t t_rx;\t\t// wall time of the last burst taken (polite access and receive recovery)\n'
         '\tuint32_t t_resync;\t// wall time of the last receiver restart, or of the first modem activity after it\n'
         '\tbool resync_pending;\t// modem activity since the restart: quiet-time recovery armed\n'
         '#else\n'
         '\tuint32_t resync_tick;\t// tick of the last receiver restart for lost or stale timing\n'
         '#endif\n'),
    ])
    transform('drivers/modem/hrc7000/hrc7000_dmr.c', [
        ('#define SYS_LOOPS\t\t4u\n',
         '#define SYS_LOOPS\t\t4u\n' + ON +
         '// DDM32 Plus, from RadioFoundry 916ac26 / 947c139 (MIT): wall-time windows; task-side code in flash (this file\n'
         '// is linked into RAM whole)\n'
         '#define BUSY_US\t\t\t(BUSY_TICKS * SLOT_US)\t// polite access: activity in the last 360 ms, by wall time\n'
         '#define SYS_STUCK_US\t\t50000u\t// a system request pending this long with no handler run: its interrupt was lost\n'
         '#ifdef __csky__\n#define DMR_FLASHFUNC\t\t__attribute__((section(".flashfunc"), noinline))\n'
         '#else\n#define DMR_FLASHFUNC\n#endif\n'
         '#endif\n'),
        ('static void chip_rx_restart(hrc7000_dmr_t *e)\n{\n',
         ON +
         '// RadioFoundry 916ac26: every modem interrupt request still pending, dropped. If the interrupt controller misses a\n'
         '// request, the request keeps its line high, and a controller that acts on the line rising never sees that source\n'
         '// again (no slot interrupt: a transmission that stalls at every key-up). Not RF RX (a board\'s own handler may have\n'
         '// it armed) nor the PCM pair (the audio driver\'s)\n'
         'static void clear_requests(hrc7000_dmr_t *e)\n'
         '{\n'
         '\tw(e, C7_INTERP_CLEAR, C7_INT_TS_TX | C7_INT_TS_RX | C7_INT_RF_TX);\n'
         '\tw(e, C7_SYS_INTERP_CLEAR, r(e, C7_SYS_INTERP_LIST));\n'
         '}\n'
         '#endif\n\n'
         'static void chip_rx_restart(hrc7000_dmr_t *e)\n{\n'
         + ON + '\tclear_requests(e);\t// the restart discards what they would report; a stale one keeps its source dead\n#endif\n'),
        ('\te->rx_any = false;\n\te->resync_tick = e->tick;\n\te->st.rx_restarts++;\n',
         '\te->rx_any = false;\n' + ON +
         '\te->t_resync = now(e);\n\te->resync_pending = false;\n#else\n\te->resync_tick = e->tick;\n#endif\n'
         '\te->st.rx_restarts++;\n'),
        ('static void rx_resync(hrc7000_dmr_t *e)\n'
         '{\n'
         '\tif (e->cc_filter < 0) {\n'
         '\t\treturn;\n'
         '\t}\n'
         '\tuint32_t quiet = e->tick - (e->rx_any ? e->rx_tick : e->resync_tick);\n'
         '\tif (quiet > (e->rx_any ? RX_LOST_TICKS : RX_IDLE_TICKS)) {\n'
         '\t\tif (e->rx_any) {\n'
         '\t\t\te->st.rx_lost++;\n'
         '\t\t} else {\n'
         '\t\t\te->st.rx_stale++;\n'
         '\t\t}\n'
         '\t\tchip_rx_restart(e);\n'
         '\t}\n'
         '}\n',
         ON +
         '// RadioFoundry 947c139: aged by wall time, from the slot interrupt and the task poll, so a receiver whose slot\n'
         '// interrupts stopped is still recovered. After a restart only new modem activity arms it again, and the stale\n'
         '// count starts with the first of it (counting the slot it reports), not at the restart: after a long silence a\n'
         '// count from the restart had run out, and the next call\'s first interrupt restarted the receiver under its header\n'
         'static void resync_arm(hrc7000_dmr_t *e)\n'
         '{\n'
         '\tif (!e->resync_pending) {\n'
         '\t\te->t_resync = now(e) - SLOT_US;\n'
         '\t\te->resync_pending = true;\n'
         '\t}\n'
         '}\n'
         '\n'
         'static void rx_resync(hrc7000_dmr_t *e)\n'
         '{\n'
         '\tif (e->cc_filter < 0 || !e->resync_pending) {\n'
         '\t\treturn;\n'
         '\t}\n'
         '\tuint32_t quiet = now(e) - (e->rx_any ? e->t_rx : e->t_resync);\n'
         '\tif (quiet > (e->rx_any ? RX_LOST_TICKS : RX_IDLE_TICKS) * SLOT_US) {\n'
         '\t\tif (e->rx_any) {\n'
         '\t\t\te->st.rx_lost++;\n'
         '\t\t} else {\n'
         '\t\t\te->st.rx_stale++;\n'
         '\t\t}\n'
         '\t\tif (e->voice_chip_slot >= 0) {\t// the restart drops the call: reported as rx_tick would have\n'
         '\t\t\tev_simple(e, DMR_EV_VOICE_LOST, e->tc_slot[e->voice_chip_slot], tick_arg(e));\n'
         '\t\t}\n'
         '\t\tchip_rx_restart(e);\n'
         '\t}\n'
         '}\n'
         '#else\n'
         'static void rx_resync(hrc7000_dmr_t *e)\n'
         '{\n'
         '\tif (e->cc_filter < 0) {\n'
         '\t\treturn;\n'
         '\t}\n'
         '\tuint32_t quiet = e->tick - (e->rx_any ? e->rx_tick : e->resync_tick);\n'
         '\tif (quiet > (e->rx_any ? RX_LOST_TICKS : RX_IDLE_TICKS)) {\n'
         '\t\tif (e->rx_any) {\n'
         '\t\t\te->st.rx_lost++;\n'
         '\t\t} else {\n'
         '\t\t\te->st.rx_stale++;\n'
         '\t\t}\n'
         '\t\tchip_rx_restart(e);\n'
         '\t}\n'
         '}\n'
         '#endif\n'),
        ('\tint8_t ts = slot_ts(e, &s);\n\te->rx_tick = e->tick;\n\te->rx_any = true;\n',
         '\tint8_t ts = slot_ts(e, &s);\n\te->rx_tick = e->tick;\n' + ON + '\te->t_rx = now(e);\n#endif\n'
         '\te->rx_any = true;\n'),
        ('\t\tif (list & SYS_LATE_LC) {\n',
         ON + '\t\tif (list & (SYS_DEFRAME | SYS_ABNORMAL)) {\n\t\t\tresync_arm(e);\n\t\t}\n#endif\n'
         '\t\tif (list & SYS_LATE_LC) {\n'),
        ('\t\tif (!tx_ticking(e)) {\n\t\t\trx_tick(e);',
         '\t\tif (!tx_ticking(e)) {\n' + ON + '\t\t\tresync_arm(e);\n#endif\n\t\t\trx_tick(e);'),
        ('\tchip_rx_restart(e);\n\te->running = true;\n',
         '\tchip_rx_restart(e);\n' + ON +
         '\te->sys_seen = e->st.sys_ints;\t// the stuck request check starts over\n\te->sys_waiting = false;\n#endif\n'
         '\te->running = true;\n'),
        ('void hrc7000_dmr_poll(hrc7000_dmr_t *e)\n{\n',
         ON +
         '// RadioFoundry 947c139 / 916ac26, task side (flash), under the engine\'s lock\n'
         'DMR_FLASHFUNC static void poll_recover(hrc7000_dmr_t *e)\n'
         '{\n'
         '\tif (e->running && e->tx_state == TXS_IDLE) {\n'
         '\t\trx_resync(e);\t// receive slot interrupts can stop while system interrupts continue\n'
         '\t}\n'
         '\t// A system request pending since an earlier poll with no handler run in between: its interrupt was lost, and\n'
         '\t// with its line kept high no later one would come (received bursts gone for good). Dropped, so the next rises.\n'
         '\t// The 50 ms count from the first poll that saw it pending, not from the poll before: after a late poll a\n'
         '\t// request that came just now (its handler held off by this poll\'s lock) isn\'t a lost one\n'
         '\tuint32_t pend = e->running ? r(e, C7_SYS_INTERP_LIST) & r(e, C7_SYS_INTERP_MASK) : 0u;\n'
         '\tif (pend == 0u || e->st.sys_ints != e->sys_seen) {\n'
         '\t\te->sys_seen = e->st.sys_ints;\n'
         '\t\te->sys_waiting = false;\n'
         '\t} else if (!e->sys_waiting) {\n'
         '\t\te->sys_waiting = true;\n'
         '\t\te->t_sys_seen = now(e);\n'
         '\t} else if ((uint32_t)(now(e) - e->t_sys_seen) > SYS_STUCK_US) {\n'
         '\t\te->st.sys_stuck++;\n'
         '\t\tw(e, C7_SYS_INTERP_CLEAR, r(e, C7_SYS_INTERP_LIST));\n'
         '\t\te->sys_waiting = false;\n'
         '\t}\n'
         '}\n'
         '#endif\n\n'
         'void hrc7000_dmr_poll(hrc7000_dmr_t *e)\n{\n'),
        ('\t\ttx_finish(e);\n\t\tev_simple(e, DMR_EV_SYNC_LOST, 0, tick_arg(e));\n\t}\n\tunlock(e);\n}\n',
         '\t\ttx_finish(e);\t// its receiver restart drops every pending request (RF fixes): a lost one is why it stalled\n'
         '\t\tev_simple(e, DMR_EV_SYNC_LOST, 0, tick_arg(e));\n\t}\n'
         + ON + '\tpoll_recover(e);\n#endif\n\tunlock(e);\n}\n'),
        ('e->rx_any && (uint32_t)(e->tick - e->rx_tick) < BUSY_TICKS &&',
         'e->rx_any &&\n#if CONFIG_RF_FIXES\n\t    (uint32_t)(now(e) - e->t_rx) < BUSY_US &&\t// wall time: the slot ticks may have stopped\n'
         '#else\n\t    (uint32_t)(e->tick - e->rx_tick) < BUSY_TICKS &&\n#endif\n\t   '),
        ('\t\tuint32_t age = e->tick - e->rx_tick;\n',
         ON + '\t\tuint32_t age = (now(e) - e->t_rx) / SLOT_US;\t// slots, by wall time: the ticks may have stopped\n'
         '#else\n\t\tuint32_t age = e->tick - e->rx_tick;\n#endif\n'),
    ])
    # After a stalled transmission or a stuck request: the modem sources unmasked again (916ac26 lost_check, without
    # the debug link's record of the PIC words)
    transform('platform/hrc7000/platform.h', [
        ('#define HRC_RAMFUNC\t\t__attribute__((section(".ramfunc"), noinline))\n',
         '#define HRC_RAMFUNC\t\t__attribute__((section(".ramfunc"), noinline))\n'
         '// DDM32 Plus (RadioFoundry 916ac26): the opposite, in a file the linker script puts in SRAM whole (the DMR glue\n'
         '// and slot engine): task-side code that needn\'t take RAM\n'
         '#define HRC_FLASHFUNC\t\t__attribute__((section(".flashfunc"), noinline))\n'),
    ])
    transform('platform/hrc7000/common.ld', [
        ('native_test.o *hrc_native_chip.o *hrc_native_port.o) .text.*)\n\t\t. = ALIGN(4);\n\t} >FLASH\n',
         'native_test.o *hrc_native_chip.o *hrc_native_port.o) .text.*)\n'
         '\t\t*(.flashfunc .flashfunc.*)\t/* HRC_FLASHFUNC: task-side code in the files placed in RAM below (DDM32 Plus) */\n'
         '\t\t. = ALIGN(4);\n\t} >FLASH\n'),
    ])
    transform('platform/hrc7000/dmr_glue.c', [
        ('// --- Deferred event task ---\n',
         ON +
         '// RadioFoundry 916ac26: the poll found a lost interrupt (a stalled transmission or a request pending with no\n'
         '// handler run): the modem sources unmasked again, unless a task is inside the engine\n'
         'static uint32_t lost_seen;\n'
         '\n'
         'HRC_FLASHFUNC static void lost_check(void)\n'
         '{\n'
         '\tuint32_t n = eng.st.stalls + eng.st.sys_stuck;\n'
         '\tif (n == lost_seen) {\n'
         '\t\treturn;\n'
         '\t}\n'
         '\tlost_seen = n;\n'
         '\tuint32_t s = hrc_irq_save();\n'
         '\tif (active && lock_depth == 0) {\n'
         '\t\tirqs_enable(true);\n'
         '\t}\n'
         '\thrc_irq_restore(s);\n'
         '}\n'
         '#endif\n\n'
         '// --- Deferred event task ---\n'),
        ('\t\t\thrc7000_dmr_poll(&eng);\n',
         '\t\t\thrc7000_dmr_poll(&eng);\n' + ON + '\t\t\tlost_check();\n#endif\n'),
        # 916ac26 also moved the glue's task-side code to flash (the file is linked into RAM whole): the event task,
        # the set-up, and the diagnostics page's lines (about 1.3 KB of RAM back)
        ('#include "task.h"\n',
         '#include "task.h"\n\n' + ON + '#define GLUE_FLASH\tHRC_FLASHFUNC\t// DDM32 Plus (RadioFoundry 916ac26): task side, in flash\n'
         '#else\n#define GLUE_FLASH\n#endif\n'),
        ('\nstatic void dmr_task(void *arg)\n', '\nGLUE_FLASH static void dmr_task(void *arg)\n'),
        ('\nvoid hrc_dmr_board_init(hrc7000_t *modem, const hrc_dmr_board_t *hooks)\n',
         '\nGLUE_FLASH void hrc_dmr_board_init(hrc7000_t *modem, const hrc_dmr_board_t *hooks)\n'),
        ('\nvoid hrc_dmr_stats_reset(void)\n', '\nGLUE_FLASH void hrc_dmr_stats_reset(void)\n'),
        # as upstream 916ac26: the counters restart, so does lost_check's memory of them (else one needless unmask)
        ('\tmemset(&eng.st, 0, offsetof(hrc7000_dmr_stats_t, last_rx_type_info));\n\tunlock(NULL);\n',
         '\tmemset(&eng.st, 0, offsetof(hrc7000_dmr_stats_t, last_rx_type_info));\n' + ON + '\tlost_seen = 0;\n#endif\n'
         '\tunlock(NULL);\n'),
        ('\nstatic void put(out_t *o, const char *s)\n', '\nGLUE_FLASH static void put(out_t *o, const char *s)\n'),
        ('\nstatic void put_u(out_t *o, uint32_t v)\n', '\nGLUE_FLASH static void put_u(out_t *o, uint32_t v)\n'),
        ('\nstatic void put_x(out_t *o, uint32_t v, int digits)\n',
         '\nGLUE_FLASH static void put_x(out_t *o, uint32_t v, int digits)\n'),
        ('\nstatic void put_kv(out_t *o, const char *k, uint32_t v)\n',
         '\nGLUE_FLASH static void put_kv(out_t *o, const char *k, uint32_t v)\n'),
        ('\nbool hrc_dmr_stats_line(uint8_t i, char *buf, uint8_t len)\n',
         '\nGLUE_FLASH bool hrc_dmr_stats_line(uint8_t i, char *buf, uint8_t len)\n'),
        ('\t\tput_x(&o, m.last_l2_status, 8);\n',
         '\t\tput_x(&o, m.last_l2_status, 8);\n' + ON + '\t\tput_kv(&o, " k", m.sys_stuck);\t// stuck requests dropped\n#endif\n'),
    ])
    prepare_rfports_tests(transform)
    prepare_rfports2(transform)


def prepare_rfports_tests(transform):
    """The upstream host tests of 916ac26 and 947c139, adapted to the pin (no post-TX hold, a0664450f), in the pinned
    hrc7000DmrTest under CONFIG_RF_FIXES: tools/test_rfports.py builds it with the switch on"""
    transform('tests/host/hrc7000DmrTest.c', [
        # test_rx_resync, 947c139: a long silence after a restart, then a call: no restart under its first bursts
        ('\thrc7000_dmr_rx_restart(&eng);\n\ttick(false);\n\tCHECK(eng.st.rx_stale == 2);\n\tcount_kind(0xFF, NULL);\n}\n',
         '\thrc7000_dmr_rx_restart(&eng);\n\ttick(false);\n\tCHECK(eng.st.rx_stale == 2);\n\tcount_kind(0xFF, NULL);\n'
         + ON +
         '\thrc7000_dmr_rx_restart(&eng);\n'
         '\trs = eng.st.rx_restarts;\n'
         '\tnow += 2000000u;\n'
         '\thrc7000_dmr_poll(&eng);\n'
         '\ttick(false);\n'
         '\tCHECK(eng.st.rx_restarts == rs);\n'
         '\tdeframe(rti(1, 0, 2, 0, 0, 0));\n'
         '\ttick(false);\n'
         '\tCHECK(eng.st.rx_restarts == rs && eng.st.rx_stale == 2);\n'
         '\tcount_kind(0xFF, NULL);\n'
         '#endif\n'
         '}\n'),
        ('static void test_polite_after_restart(void)\n', RF_TESTS + 'static void test_polite_after_restart(void)\n'),
        ('\ttest_polite_after_restart();\n',
         '\ttest_polite_after_restart();\n' + ON +
         '\ttest_polite_without_ticks();\n\ttest_resync_without_ticks();\n\ttest_resync_voice_lost();\n'
         '\ttest_lost_requests();\n#endif\n'),
    ])


RF_TESTS = ON + r'''// RadioFoundry 947c139: wall time ages the busy window while the receive timeslot vector is silent, across clock wrap
static void test_polite_without_ticks(void)
{
	for (int wrap = 0; wrap < 2; wrap++) {
		for (int policy = DMR_ACCESS_POLITE_CC; policy <= DMR_ACCESS_POLITE_ALL; policy++) {
			setup();
			if (wrap) now = UINT32_MAX - 200000u;
			const dmr_config_t cfg = { .cc = 1 };
			hrc7000_dmr_configure(&eng, &cfg);
			hrc7000_dmr_run(&eng);
			for (int i = 0; i < 3; i++) {
				deframe(rti(1, 0, 1, 0, 0, 0));
				now += 300000u;
				hrc7000_dmr_poll(&eng);
				CHECK(!hrc7000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 1, (dmr_access_t)policy));
				count_kind(0xFF, NULL);
			}
			now += 59999u;
			CHECK(!hrc7000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 1, (dmr_access_t)policy));
			now++;
			CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 1, (dmr_access_t)policy));
			hrc7000_dmr_tx_end(&eng);
			for (int i = 0; i < 3; i++) {
				now += 33000000u;
				hrc7000_dmr_poll(&eng);
				CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 1, (dmr_access_t)policy));
				hrc7000_dmr_tx_end(&eng);
				count_kind(0xFF, NULL);
			}
			CHECK(eng.tick == 0 && !rf_state);
		}
	}
}

// 947c139: a call stopped with the slot interrupts: the poll's restart still reports the lost voice
static void test_resync_voice_lost(void)
{
	setup();
	const dmr_config_t cfg = { .cc = 1 };
	hrc7000_dmr_configure(&eng, &cfg);
	hrc7000_dmr_set_cc_filter(&eng, cfg.cc);
	hrc7000_dmr_run(&eng);
	deframe(rti(1, 0, 1, 3, 0, 0));	// voice A, TS2 sync, chip slot 0
	count_kind(0xFF, NULL);
	CHECK(eng.voice_chip_slot == 0);
	now += 391000u;
	hrc7000_dmr_poll(&eng);
	CHECK(eng.st.rx_lost == 1 && eng.voice_chip_slot < 0);
	CHECK(count_kind(DMR_EV_VOICE_LOST, NULL) == 1);
	CHECK(eng.tick == 0);
}

// 947c139 / 916ac26: the poll restarts a receiver whose slot interrupts stopped, and the restart drops the requests
static void test_resync_without_ticks(void)
{
	setup();
	const dmr_config_t cfg = { .cc = 1 };
	hrc7000_dmr_configure(&eng, &cfg);
	hrc7000_dmr_set_cc_filter(&eng, cfg.cc);
	hrc7000_dmr_run(&eng);
	deframe(rti(1, 0, 1, 0, 0, 0));
	uint32_t rs = eng.st.rx_restarts;
	now += 390000u;
	hrc7000_dmr_poll(&eng);
	CHECK(eng.st.rx_restarts == rs);
	now++;
	REG(C7_INTERP_CLEAR) = 0;
	hrc7000_dmr_poll(&eng);
	CHECK(eng.st.rx_restarts == rs + 1 && !eng.rx_any && eng.st.rx_lost == 1);
	CHECK(REG(C7_INTERP_CLEAR) == (C7_INT_TS_TX | C7_INT_TS_RX | C7_INT_RF_TX));
	// A silent receiver needs no repeated restart
	now += 1000000u;
	hrc7000_dmr_poll(&eng);
	CHECK(eng.st.rx_restarts == rs + 1);
	// Unusable bursts only: stale timing is restarted, the count starting with the first of them
	deframe((rti(0, 0, 1, 0, 0, 0) & ~3u) | 1u);
	hrc7000_dmr_poll(&eng);
	CHECK(eng.st.rx_restarts == rs + 1);
	for (int i = 0; i < 20; i++) {
		now += 30000u;
		deframe((rti(0, 0, 1, 0, 0, 0) & ~3u) | 1u);
		hrc7000_dmr_poll(&eng);
	}
	CHECK(eng.st.rx_restarts == rs + 2 && eng.st.rx_stale == 1);
	CHECK(eng.tick == 0);
}

// 916ac26 (gotchas 2.9): a receiver restart drops every pending modem request but the board's and the audio's, and
// the poll drops a system request left pending with no handler run
static void test_lost_requests(void)
{
	setup();
	REG(C7_SYS_INTERP_MASK) = 0x1187Fu;
	REG(C7_SYS_INTERP_LIST) = 1u << 3;
	hrc7000_dmr_run(&eng);
	CHECK(REG(C7_INTERP_CLEAR) == (C7_INT_TS_TX | C7_INT_TS_RX | C7_INT_RF_TX));
	CHECK(REG(C7_SYS_INTERP_CLEAR) == (1u << 3));

	// Still pending (the array keeps it), no handler run: dropped once it has waited 50 ms, counted once
	REG(C7_SYS_INTERP_CLEAR) = 0;
	hrc7000_dmr_poll(&eng);
	CHECK(eng.st.sys_stuck == 0 && REG(C7_SYS_INTERP_CLEAR) == 0);
	now += 30000;
	hrc7000_dmr_poll(&eng);
	CHECK(eng.st.sys_stuck == 0 && REG(C7_SYS_INTERP_CLEAR) == 0);
	now += 30000;
	hrc7000_dmr_poll(&eng);
	CHECK(eng.st.sys_stuck == 1 && REG(C7_SYS_INTERP_CLEAR) == (1u << 3));
	now += 30000;
	hrc7000_dmr_poll(&eng);
	CHECK(eng.st.sys_stuck == 1);	// the next 50 ms start from the drop

	// A handler ran between the polls: the request is a new one each time
	REG(C7_SYS_INTERP_CLEAR) = 0;
	for (int i = 0; i < 6; i++) {
		now += 30000;
		eng.st.sys_ints++;
		hrc7000_dmr_poll(&eng);
	}
	CHECK(eng.st.sys_stuck == 1 && REG(C7_SYS_INTERP_CLEAR) == 0);

	// A late poll (the one before, 80 ms ago, saw nothing pending): a request pending now is a new one, not lost
	REG(C7_SYS_INTERP_LIST) = 0;
	hrc7000_dmr_poll(&eng);
	now += 80000;
	REG(C7_SYS_INTERP_LIST) = 1u << 3;
	hrc7000_dmr_poll(&eng);
	CHECK(eng.st.sys_stuck == 1 && REG(C7_SYS_INTERP_CLEAR) == 0);

	// A cause the chip doesn't interrupt for (its mask bit off) is never a lost interrupt
	REG(C7_SYS_INTERP_LIST) = C7_SYS_FM_RX;
	for (int i = 0; i < 6; i++) {
		now += 30000;
		hrc7000_dmr_poll(&eng);
	}
	CHECK(eng.st.sys_stuck == 1);

	// Halted (FM receive owns the system interrupt): not checked
	REG(C7_SYS_INTERP_LIST) = 1u << 3;
	hrc7000_dmr_halt(&eng);
	for (int i = 0; i < 6; i++) {
		now += 30000;
		hrc7000_dmr_poll(&eng);
	}
	CHECK(eng.st.sys_stuck == 1);

	// A run long after: the check starts over rather than counting the time halted
	now += 10000000;
	REG(C7_SYS_INTERP_LIST) = 1u << 3;
	hrc7000_dmr_run(&eng);
	REG(C7_SYS_INTERP_LIST) = 1u << 3;	// came again just after the restart
	hrc7000_dmr_poll(&eng);
	CHECK(eng.st.sys_stuck == 1);

	// A stalled transmission drops the pending requests too (the restart after it)
	CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	REG(C7_INTERP_CLEAR) = 0;
	REG(C7_SYS_INTERP_LIST) = 0;
	now += 250000;
	hrc7000_dmr_poll(&eng);
	CHECK(eng.st.stalls == 1 && REG(C7_INTERP_CLEAR) == (C7_INT_TS_TX | C7_INT_TS_RX | C7_INT_RF_TX));
}
#endif

'''


# --- Second round (branch rfports2, 2026-11-22): f5d2fd1, 18fdd29, 3d5940b (the idea), f4b19e9 (the gate), 22b27e8 ---

def prepare_rfports2(transform):
    """RadioFoundry (MIT, The RadioFoundry Authors) fixes of 2026-11-20..22, adapted to the pin, every hunk under
    CONFIG_RF_FIXES (docs/plus.md "Upstream RadioFoundry fixes"):

    - f5d2fd1 (#943, Spencer Fowler): in direct mode a terminator with a good CRC and no call left on either slot ends
      the polite busy window, so a reply keyed within DMR_CALL_ALIGN_MS (300 ms) of a call's end is not refused.
    - 18fdd29 (#918, Spencer Fowler): the FM squelch holds through voice peaks on 12.5 kHz channels (the noise reading
      over the close line with the strength steady: closes after 16 polls, not 2).
    - 3d5940b (#929, Spencer Fowler), the idea, not the code (our pin has no hrc7000_air.c): the FM receive copy that
      NXDN, dPMR and APRS receive read is restarted when no half came for 200 ms (fm_rx_audio.c, dmr_glue.c). The
      native P25 session's own watchdog is in integration/radiofoundry/native (hn_watch).
    - f4b19e9 (#827, Spencer Fowler), the keying gate: every board.radio->tx() in the radio core (PTT, DMR data, a
      roaming probe, the SFR repeater, the hotspot, CW IDs and the beacon, APRS, DOTA, SSTV) refuses on TX inhibit
      or a critical battery; the hotspot drops its armed keying when TX inhibit is set. No band rule (owner decision
      2026-11-22).
    - 22b27e8 (#790, Spencer Fowler): received DMR text drops the C1 controls and DEL (0x7F-0x9F): the UI font draws
      its own icons at 0x80-0x86."""
    prepare_polite_terminator(transform)
    prepare_squelch_peaks(transform)
    prepare_copy_watchdog(transform)
    prepare_key_gate(transform)
    prepare_text_c1(transform)


def prepare_polite_terminator(transform):
    transform('drivers/modem/hrc7000/hrc7000_dmr.h', [
        ('\tbool rx_any;\n#if CONFIG_RF_FIXES\n',
         '\tbool rx_any;\n#if CONFIG_RF_FIXES\n'
         '\tbool rx_call_end;\t// RadioFoundry f5d2fd1: the last burst taken ended a call (direct): polite access sees a free channel\n'),
    ])
    transform('drivers/modem/hrc7000/hrc7000_dmr.c', [
        ('\te->t_rx = now(e);\n#endif\n\te->rx_any = true;\n',
         '\te->t_rx = now(e);\n\te->rx_call_end = false;\t// any burst opens the busy window again (f5d2fd1)\n#endif\n'
         '\te->rx_any = true;\n'),
        ('\t\t\te->listen_voice = false;\n\t\t\te->st.rx_terminator++;\n',
         '\t\t\te->listen_voice = false;\n' + ON +
         '\t\t\t// RadioFoundry f5d2fd1: direct mode, the call is over and the channel free, as the call stack\'s own check has\n'
         '\t\t\t// it. A reply keyed now is aligned to that call (DMR_CALL_ALIGN_MS, 300 ms) and was refused for the rest of\n'
         '\t\t\t// the busy window. Through a repeater a terminator is its hang. Only a good CRC and no call left on the other\n'
         '\t\t\t// slot frees the channel\n'
         '\t\t\te->rx_call_end = !e->cfg.repeater && e->voice_slots == 0 && s.crc_ok;\n'
         '#endif\n'
         '\t\t\te->st.rx_terminator++;\n'),
        ('e->rx_any &&\n#if CONFIG_RF_FIXES\n\t    (uint32_t)(now(e) - e->t_rx) < BUSY_US &&',
         'e->rx_any &&\n#if CONFIG_RF_FIXES\n\t    !e->rx_call_end && (uint32_t)(now(e) - e->t_rx) < BUSY_US &&'),
    ])
    transform('tests/host/hrc7000DmrTest.c', [
        ('static void test_polite_after_restart(void)\n', RF2_POLITE_TESTS + 'static void test_polite_after_restart(void)\n'),
        ('\ttest_lost_requests();\n#endif\n',
         '\ttest_lost_requests();\n\ttest_polite_after_terminator();\n\ttest_polite_other_slot_terminator();\n#endif\n'),
    ])


RF2_POLITE_TESTS = ON + r'''// RadioFoundry f5d2fd1 (#943): a call that ended with its terminator leaves the channel free: a reply keyed within the
// busy window, aligned to that call (the call stack does so within DMR_CALL_ALIGN_MS, 300 ms), was refused as busy until
// 360 ms after the terminator
static void polite_try(bool admit)
{
	count_kind(0xFF, NULL);
	CHECK(hrc7000_dmr_tx_begin(&eng, DMR_TIMING_ALIGNED, 1, DMR_ACCESS_POLITE_CC) == admit);
	CHECK(count_kind(DMR_EV_TX_REJECTED, NULL) == (admit ? 0 : 1));
	if (admit) {
		hrc7000_dmr_tx_abort(&eng);
		tick(true);
		CHECK(!hrc7000_dmr_is_transmitting(&eng));
	}
	count_kind(0xFF, NULL);
}

static void test_polite_after_terminator(void)
{
	for (int repeater = 0; repeater < 2; repeater++) {
		setup();
		hrc7000_dmr_run(&eng);
		const dmr_config_t cfg = { .own_id = 1234, .cc = 1, .access = DMR_ACCESS_POLITE_CC, .repeater = repeater };
		hrc7000_dmr_configure(&eng, &cfg);
		hrc7000_dmr_set_cc_filter(&eng, cfg.cc);
		tick(false);
		deframe(rti(1, 0, 1, 2, 0, 0));	// voice, own color code
		count_kind(0xFF, NULL);
		polite_try(false);		// the call is on
		tick(false);
		deframe(rti(0, DMR_BURST_TERMINATOR_LC, 1, 2, 0, 0) | (1u << 25));	// a bad CRC ends nothing
		polite_try(false);
		tick(false);
		deframe(rti(0, DMR_BURST_TERMINATOR_LC, 1, 2, 0, 0));
		now += 100000u;
		// Direct mode: the call is over, the reply admitted. A repeater's terminator is its hang: still busy
		polite_try(!repeater);
		if (repeater) {
			continue;
		}
		tick(false);
		deframe(rti(0, DMR_BURST_TERMINATOR_LC, 1, 2, 0, 0));
		tick(false);
		deframe(rti(1, 0, 1, 2, 0, 0));	// a new call after the terminator: busy again
		polite_try(false);
		CHECK(eng.st.tx_rejected == 3);
	}
}

// A terminator on one chip slot while the other slot's call runs leaves the channel busy
static void test_polite_other_slot_terminator(void)
{
	setup();
	hrc7000_dmr_run(&eng);
	const dmr_config_t cfg = { .own_id = 1234, .cc = 1, .access = DMR_ACCESS_POLITE_CC };
	hrc7000_dmr_configure(&eng, &cfg);
	hrc7000_dmr_set_cc_filter(&eng, cfg.cc);
	tick(false);
	deframe(rti(1, 0, 1, 2, 0, 0));	// call A: voice on chip slot 0
	tick(false);
	deframe(rti(0, DMR_BURST_TERMINATOR_LC, 1, 2, 1, 0));	// call B ends on chip slot 1
	now += 10000u;
	count_kind(0xFF, NULL);
	polite_try(false);		// call A is still on
	tick(false);
	deframe(rti(0, DMR_BURST_TERMINATOR_LC, 1, 2, 0, 0));	// then A ends too
	now += 10000u;
	polite_try(true);
}
#endif

'''


def prepare_squelch_peaks(transform):
    transform('core/radio/radio_sm.h', [
        ('#define RADIO_SQL_CLOSE_POLLS\t2\n',
         '#define RADIO_SQL_CLOSE_POLLS\t2\n' + ON +
         '// RadioFoundry 18fdd29 (#918, MIT): a voice peak. On a 12.5 kHz channel, speech above about 2 kHz (fricatives)\n'
         '// raises the noise reading over the close line for 100-300 ms while the carrier stays as strong as it was. Noise\n'
         '// over the line with the signal strength within RADIO_SQL_PEAK_RSSI_DB of the last poll the squelch held closes\n'
         '// only after RADIO_SQL_PEAK_POLLS polls (400 ms), and only for a carrier RADIO_SQL_PEAK_FLOOR_DB or more over the\n'
         '// receiver\'s floor (sql_floor, learned on idle polls, rising a dB every RADIO_SQL_FLOOR_RISE_POLLS polls): when\n'
         '// that carrier goes or fades, its strength falls by more than RADIO_SQL_PEAK_RSSI_DB and the squelch closes as before\n'
         '#define RADIO_SQL_PEAK_RSSI_DB\t3\n'
         '#define RADIO_SQL_PEAK_POLLS\t16\n'
         '#define RADIO_SQL_PEAK_FLOOR_DB\t10\n'
         '#define RADIO_SQL_FLOOR_RISE_POLLS\t4\n'
         '#endif\n'),
        ('\tint16_t rssi_published;\n',
         '\tint16_t rssi_published;\n' + ON +
         '\tint16_t sql_rssi;\t\t// the signal strength on the last poll the squelch held (close_need, RadioFoundry 18fdd29)\n'
         '\tint16_t sql_floor;\t\t// the receiver\'s floor, from idle polls (track_floor); INT16_MAX not known\n'
         '\tuint8_t sql_close;\t\t// polls over the close line the last close took (1-2 as a carrier goes, 16 after a hold)\n'
         '#endif\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('\tsm->base_squelch = RADIO_SQUELCH_DEFAULT;\n',
         '\tsm->base_squelch = RADIO_SQUELCH_DEFAULT;\n' + ON +
         '\tsm->sql_floor = INT16_MAX;\t// not known yet: no voice peak hold before an idle poll measured it (18fdd29)\n'
         '#endif\n'),
        ('\treturn RADIO_SQL_CLOSE_POLLS;\n}\n',
         '\treturn RADIO_SQL_CLOSE_POLLS;\n}\n' + ON + r'''
// RadioFoundry 18fdd29 (#918, MIT). sql_rssi on a poll the squelch holds: the strength when the noise held it, none
// (INT16_MAX) when level 0 or a broadcast listen did, so a squelch raised over a noise-only channel closes as before
static int16_t held_rssi(const radio_status_t *st, uint8_t sq, uint8_t open_below)
{
	return sq != 0 && st->noise < open_below + RADIO_SQL_HYST_DB ? st->rssi_dbm : INT16_MAX;
}

// The receiver's floor (sql_floor), from idle polls whose noise says no carrier at any level: down at once, up 1 dB
// every RADIO_SQL_FLOOR_RISE_POLLS polls, so a carrier coming up in the poll before the squelch opens moves it by a dB
// at most
static void track_floor(radio_sm_t *sm, const radio_status_t *st, uint32_t now_ms)
{
	if (st->noise < RADIO_SQL_OPEN_L1 + RADIO_SQL_HYST_DB) {
		return;
	}
	if (st->rssi_dbm < sm->sql_floor) {
		sm->sql_floor = st->rssi_dbm;
	} else if (st->rssi_dbm > sm->sql_floor && (now_ms / RADIO_POLL_MS) % RADIO_SQL_FLOOR_RISE_POLLS == 0) {
		sm->sql_floor++;
	}
}

// Polls of noise above the line before the squelch closes on this reading: close_polls(), or RADIO_SQL_PEAK_POLLS for
// a voice peak: a 12.5 kHz channel, the RX tone still there, the strength where it was on the last poll the squelch
// held and that strength RADIO_SQL_PEAK_FLOOR_DB over the floor. An RX tone that goes and a carrier that fades or goes
// close as before; so does a carrier near the floor, whose strength may not fall far enough when it goes
static uint8_t close_need(const radio_sm_t *sm, const radio_status_t *st, uint8_t sq)
{
	uint8_t polls = close_polls();
	if (polls == RADIO_SQL_CLOSE_POLLS && sq != 0 && sm->view.tone_ok && sm->view.ch.bw == RADIO_BW_12K5 &&
	    st->rssi_dbm + RADIO_SQL_PEAK_RSSI_DB >= sm->sql_rssi &&
	    (int32_t)sm->sql_rssi >= (int32_t)sm->sql_floor + RADIO_SQL_PEAK_FLOOR_DB) {
		return RADIO_SQL_PEAK_POLLS;
	}
	return polls;
}
#endif
'''),
        ('\tbool holds = listening || (sm->view.tone_ok && (sq == 0 || st.noise < open_below + RADIO_SQL_HYST_DB));\n'
         '\n'
         '\tif (sm->view.state == RS_IDLE) {\n'
         '\t\tif (opens) {\n'
         '\t\t\tsm->view.state = RS_RX;\n'
         '\t\t\tsm->close_count = 0;\n'
         '\t\t\tchanged(sm);\n'
         '\t\t}\n'
         '\t} else if (holds) {\n'
         '\t\tactivity(sm, now_ms);\n'
         '\t\tsm->close_count = 0;\n'
         '\t} else if (++sm->close_count >= close_polls()) {\n',
         '\tbool holds = listening || (sm->view.tone_ok && (sq == 0 || st.noise < open_below + RADIO_SQL_HYST_DB));\n'
         + ON +
         '\tuint8_t need = close_need(sm, &st, sq);\t\t// for the close below (RadioFoundry 18fdd29)\n'
         '\n'
         '\tif (sm->view.state == RS_IDLE) {\n'
         '\t\tif (opens) {\n'
         '\t\t\tsm->view.state = RS_RX;\n'
         '\t\t\tsm->close_count = 0;\n'
         '\t\t\tsm->sql_rssi = held_rssi(&st, sq, open_below);\n'
         '\t\t\tchanged(sm);\n'
         '\t\t} else {\n'
         '\t\t\ttrack_floor(sm, &st, now_ms);\n'
         '\t\t}\n'
         '\t} else if (holds) {\n'
         '\t\tactivity(sm, now_ms);\n'
         '\t\tsm->close_count = 0;\n'
         '\t\tsm->sql_rssi = held_rssi(&st, sq, open_below);\n'
         '\t} else if (++sm->close_count >= need) {\n'
         '\t\tsm->sql_close = sm->close_count;\n'
         '#else\n'
         '\n'
         '\tif (sm->view.state == RS_IDLE) {\n'
         '\t\tif (opens) {\n'
         '\t\t\tsm->view.state = RS_RX;\n'
         '\t\t\tsm->close_count = 0;\n'
         '\t\t\tchanged(sm);\n'
         '\t\t}\n'
         '\t} else if (holds) {\n'
         '\t\tactivity(sm, now_ms);\n'
         '\t\tsm->close_count = 0;\n'
         '\t} else if (++sm->close_count >= close_polls()) {\n'
         '#endif\n'),
    ])
    transform('tests/host/radioCoreTest.c', [
        ('static void test_tx(void)\n', RF2_SQUELCH_TESTS + 'static void test_tx(void)\n'),
        ('\ttest_squelch();\n', '\ttest_squelch();\n' + ON + '\ttest_squelch_voice_peak();\n#endif\n'),
    ])


RF2_SQUELCH_TESTS = ON + r'''// Receive a carrier at rssi with the noise well under the line, after idle polls that teach the floor (-125)
static void peak_open(uint8_t thr, int16_t rssi)
{
	hw.st.noise = 120;
	hw.st.tone_ok = true;
	hw.st.rssi_dbm = -125;
	poll_for(10 * RADIO_POLL_MS);
	CHECK(sm.view.state == RS_IDLE);
	hw.st.noise = (uint8_t)(thr - 10);
	hw.st.rssi_dbm = rssi;
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);
}

// RadioFoundry 18fdd29 (#906): speech above about 2 kHz on a 12.5 kHz channel raises the noise reading over the close
// line for a few polls while the carrier stays as strong. The squelch holds through it, and still closes at once when
// the carrier goes (also when the strength reading lags the noise by a poll), after two polls when the strength falls,
// after RADIO_SQL_PEAK_POLLS when the noise stays up, and as before on a carrier near the floor or a 25 kHz channel.
// Our pin has no fast close (RADIO_SQL_FAST_CLOSE_DB): a carrier that goes closes in two polls, or one when its
// strength drops with the noise (no hold)
static void test_squelch_voice_peak(void)
{
	setup_codeplug();
	start();
	uint8_t thr = radio_sm_squelch_open_below(RADIO_SQUELCH_DEFAULT);
	uint8_t peak = (uint8_t)(thr + RADIO_SQL_HYST_DB + 11);
	CHECK(sm.view.ch.bw == RADIO_BW_12K5);

	peak_open(thr, -70);
	// A 120 ms fricative, the strength a dB lower: open the whole time, and again after a louder one
	for (int n = 0; n < 2; n++) {
		hw.st.noise = (uint8_t)(peak + n * 10);
		hw.st.rssi_dbm = -71;
		for (int i = 0; i < 5; i++) {
			poll_for(RADIO_POLL_MS);
			CHECK(sm.view.state == RS_RX && hw.audio);
		}
		hw.st.noise = (uint8_t)(thr - 10);
		hw.st.rssi_dbm = -70;
		poll_for(2 * RADIO_POLL_MS);
		CHECK(sm.view.state == RS_RX);
	}
	// The noise stays up with the strength: closes after RADIO_SQL_PEAK_POLLS
	hw.st.noise = peak;
	poll_for((RADIO_SQL_PEAK_POLLS - 1) * RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_IDLE && !hw.audio && sm.sql_close == RADIO_SQL_PEAK_POLLS);
	// The carrier goes: the noise up and the strength at the floor: two polls (the close line's count, no hold)
	peak_open(thr, -70);
	hw.st.noise = (uint8_t)(peak + 1);
	hw.st.rssi_dbm = -125;
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_IDLE && sm.sql_close == 2);
	// ... with the strength a poll behind the noise: the poll after
	peak_open(thr, -70);
	hw.st.noise = (uint8_t)(peak + 30);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);
	hw.st.rssi_dbm = -125;
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_IDLE && sm.sql_close == 2);
	// A fade: the noise over the line and the strength RADIO_SQL_PEAK_RSSI_DB + 1 lower closes after two polls
	peak_open(thr, -70);
	hw.st.noise = (uint8_t)(thr + RADIO_SQL_HYST_DB);
	hw.st.rssi_dbm = -70 - RADIO_SQL_PEAK_RSSI_DB - 1;
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_IDLE && sm.sql_close == 2);
	// The strength falls in the middle of a peak: the polls already counted close it on the next one
	peak_open(thr, -70);
	hw.st.noise = peak;
	poll_for(3 * RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);
	hw.st.rssi_dbm = -80;
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_IDLE);
	// A carrier just under RADIO_SQL_PEAK_FLOOR_DB over the floor: no hold, a peak closes it in two polls
	peak_open(thr, -125 + RADIO_SQL_PEAK_FLOOR_DB - 1);
	hw.st.noise = peak;
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_IDLE && sm.sql_close == 2);
	// ... and at RADIO_SQL_PEAK_FLOOR_DB the hold is back
	peak_open(thr, -125 + RADIO_SQL_PEAK_FLOOR_DB);
	hw.st.noise = peak;
	poll_for(5 * RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);
	// The floor rises slowly: a carrier in the idle polls before it opens doesn't take the hold away
	hw.st.noise = 120;
	hw.st.rssi_dbm = -125;
	poll_for(2 * RADIO_POLL_MS);
	CHECK(sm.view.state == RS_IDLE);
	hw.st.rssi_dbm = -70;	// the carrier's strength comes up a poll before its noise reading falls
	poll_for(RADIO_POLL_MS);
	hw.st.noise = (uint8_t)(thr - 10);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);
	hw.st.noise = peak;
	poll_for(5 * RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);
	hw.st.noise = 120;
	hw.st.rssi_dbm = -125;
	poll_for(2 * RADIO_POLL_MS);
	CHECK(sm.view.state == RS_IDLE);
	// A 25 kHz channel: no hold, the closes as before
	sm.view.ch.bw = RADIO_BW_25K;
	peak_open(thr, -70);
	hw.st.noise = peak;
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_IDLE && sm.sql_close == 2);
	sm.view.ch.bw = RADIO_BW_12K5;
	req(RREQ_CLEAR_TEMP, 0);
}
#endif

'''


def prepare_copy_watchdog(transform):
    transform('platform/hrc7000/fm_rx_audio.c', [
        ('void hrc_fm_rx_attach(hrc7000_t *m)\n{',
         ON + r'''// DDM32 Plus, the idea of RadioFoundry 3d5940b (#929, MIT; not the code: our pin has no hrc7000_air.c): a copy request
// lost for good (a request line still high after the interrupt took it never interrupts again, modem-hrc7000.md 3.3) or
// the modem's system source left masked leaves the FM receive audio deaf (NXDN, dPMR, APRS receive) until the next
// start; the halted DMR engine's own stuck-request check (poll_recover) does not run outside DMR. A half comes every
// 32 ms: none taken or dropped for 200 ms, and the glue drops what is pending, sets the copy's mask bit again and
// unmasks the system source (hrc_dmr_fm_rx_kick). From the reader, at task level, so nothing new runs in the ISR
#define COPY_STALL_US	200000u
static uint32_t copy_seen, copy_t, copy_restarts;

static void copy_watch(void)
{
	uint32_t n = halves + dropped, t = hrc_micros();
	if (n != copy_seen) {
		copy_seen = n;
		copy_t = t;
		return;
	}
	if ((uint32_t)(t - copy_t) <= COPY_STALL_US) {
		return;
	}
	copy_t = t;		// one try every 200 ms while none comes
	if (hrc_dmr_fm_rx_kick()) {
		copy_restarts++;
	}
}

uint32_t hrc_fm_rx_restarts(void)
{
	return copy_restarts;
}
#endif

void hrc_fm_rx_attach(hrc7000_t *m)
{'''),
        ('\thead = tail = 0;\n',
         '\thead = tail = 0;\n' + ON + '\tcopy_seen = halves + dropped;\t// the watchdog starts over (copy_watch)\n\tcopy_t = hrc_micros();\n#endif\n'),
        ('uint16_t hrc_fm_rx_read(int16_t *buf, uint16_t max)\n{\n\tif (ring == NULL) {\n\t\treturn 0;\n\t}\n',
         'uint16_t hrc_fm_rx_read(int16_t *buf, uint16_t max)\n{\n\tif (ring == NULL) {\n\t\treturn 0;\n\t}\n'
         + ON + '\tcopy_watch();\n#endif\n'),
    ])
    transform('platform/hrc7000/fm_rx_audio.h', [
        ('void hrc_fm_rx_stats(uint32_t *halves, uint32_t *dropped);\n',
         'void hrc_fm_rx_stats(uint32_t *halves, uint32_t *dropped);\n' + ON +
         '// Copy restarts after no half for 200 ms (DDM32 Plus, after RadioFoundry 3d5940b)\nuint32_t hrc_fm_rx_restarts(void);\n#endif\n'),
    ])
    transform('platform/hrc7000/dmr_glue.h', [
        ('bool hrc_dmr_fm_rx_hook(void (*fn)(void));\n',
         'bool hrc_dmr_fm_rx_hook(void (*fn)(void));\n' + ON +
         '// DDM32 Plus (fm_rx_audio.c copy_watch): outside DMR, with this glue on the modem\'s system interrupt, the FM\n'
         '// copy\'s pending request dropped, its mask bit set and the system source unmasked. False: not this glue\'s now\n'
         'bool hrc_dmr_fm_rx_kick(void);\n#endif\n'),
        ('static inline bool hrc_dmr_fm_rx_hook(void (*fn)(void)) { (void)fn; return false; }\n',
         'static inline bool hrc_dmr_fm_rx_hook(void (*fn)(void)) { (void)fn; return false; }\n'
         'static inline bool hrc_dmr_fm_rx_kick(void) { return false; }\n'),
    ])
    transform('platform/hrc7000/dmr_glue.c', [
        ('\thrc_irq_enable(IRQ_MODEM_SYS, active || fn != NULL);\n\treturn true;\n}\n',
         '\thrc_irq_enable(IRQ_MODEM_SYS, active || fn != NULL);\n\treturn true;\n}\n' + ON + r'''
// DDM32 Plus, after RadioFoundry 3d5940b (#929): the FM copy gave no half for 200 ms (fm_rx_audio.c copy_watch).
// Outside DMR (in DMR the engine's own check covers it) and only with this glue's handler on the system interrupt (a
// native P25 session installs its own and owns the requests then): the pending requests dropped (a line kept high
// never interrupts again), the copy's mask bit set again and the system source unmasked at the controller
GLUE_FLASH bool hrc_dmr_fm_rx_kick(void)
{
	uint32_t s = hrc_irq_save();
	bool ok = !active && eng.fm_rx != NULL && lock_depth == 0 && hrc_irq_table[IRQ_MODEM_SYS] == isr_sys;
	if (ok) {
		hrc7000_wr(eng.dev, C7_SYS_INTERP_CLEAR, hrc7000_rd(eng.dev, C7_SYS_INTERP_LIST));
		hrc7000_fm_rx_audio_irq(eng.dev, true);
		hrc_irq_enable(IRQ_MODEM_SYS, true);
	}
	hrc_irq_restore(s);
	return ok;
}
#endif
'''),
    ])


def prepare_key_gate(transform):
    transform('core/radio/radio_sm.c', [
        # every keying in the radio core through the one gate (the .inc files of DOTA and SSTV use KEY_TX too)
        ('board.radio->tx(', 'KEY_TX(sm, ', 8),
        ('static void deny(radio_sm_t *sm, uint8_t reason)\n{',
         ON + r'''// RadioFoundry f4b19e9 (#827, MIT): what holds for the whole radio, whatever keys it (PTT, DMR data, a roaming probe,
// the SFR repeater, the hotspot, a CW ID or the beacon, APRS after its channel wait, DOTA, SSTV), is checked at the
// keying itself and not only by each path: TX inhibit and a critical battery. No band rule here (owner decision
// 2026-11-22: the band plan is DOTA's alone)
static bool tx_held(const radio_sm_t *sm)
{
	return sm->set.tx_inhibit || sm->batt.level == BATT_CRITICAL;
}

static bool key_tx(const radio_sm_t *sm, uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tone)
{
	return !tx_held(sm) && board.radio->tx(hz, bw, power, tone);
}
#define KEY_TX(sm, hz, bw, power, tone)	key_tx(sm, hz, bw, power, tone)
#else
#define KEY_TX(sm, hz, bw, power, tone)	board.radio->tx(hz, bw, power, tone)
#endif

static void deny(radio_sm_t *sm, uint8_t reason)
{'''),
        ('\tsm->dmr_keyed = KEY_TX(sm, sm->hs_tx_hz, RADIO_BW_12K5, sm->hs_power, &none);\n',
         ON +
         '\tif (tx_held(sm)) {\t// RadioFoundry f4b19e9: refused now (TX inhibit, the battery): not left armed\n'
         '\t\tif (sm->dmr_keyed) {\n'
         '\t\t\tsm->dmr_keyed = false;\n'
         '\t\t\tboard.radio->tx_off();\n'
         '\t\t\tif (sm->hs_rx_hz != 0) {\n'
         '\t\t\t\tboard.radio->rx(sm->hs_rx_hz, RADIO_BW_12K5, &none);\n'
         '\t\t\t}\n'
         '\t\t}\n'
         '\t\treturn;\n'
         '\t}\n'
         '#endif\n'
         '\tsm->dmr_keyed = KEY_TX(sm, sm->hs_tx_hz, RADIO_BW_12K5, sm->hs_power, &none);\n'),
        ('\t\ts->talk_permit != sm->set.talk_permit;\t// the permit hold (permit_before_key)\n#endif\n',
         '\t\ts->talk_permit != sm->set.talk_permit;\t// the permit hold (permit_before_key)\n'
         + ON + '\tbool inhibit_changed = s->tx_inhibit != sm->set.tx_inhibit;\n#endif\n#endif\n'),
        ('\tif (reply_changed && sm->dmr_running) {\n',
         ON + '\tif (inhibit_changed) {\n\t\thotspot_arm(sm);\t// f4b19e9: the hotspot\'s keying stays armed between bursts: dropped or armed again\n'
         '\t}\n#endif\n'
         '\tif (reply_changed && sm->dmr_running) {\n'),
    ])
    transform('tests/host/radioCoreTest.c', [
        ('\treq(RREQ_HS_POWER, 255);\n\tCHECK(hw.tx_calls == tx0 + 2 && hw.tx_power == 2);\n',
         '\treq(RREQ_HS_POWER, 255);\n\tCHECK(hw.tx_calls == tx0 + 2 && hw.tx_power == 2);\n' + ON + r'''	// RadioFoundry f4b19e9: TX inhibit drops the armed keying at once and holds it off; cleared, the keying is armed again
	{
		int off1 = hw.tx_off_calls;
		radio_settings_t set = sm.set;
		set.tx_inhibit = true;
		radio_sm_set_settings(&sm, &set, now);
		CHECK(!sm.dmr_keyed && hw.rx_hz == 438800000 && hw.tx_calls == tx0 + 2 && hw.tx_off_calls == off1 + 1);
		req(RREQ_HS_POWER, 255);
		radio_req_t f = { RREQ_HS_FREQ, 438800000, 438800000 };
		radio_sm_request(&sm, &f, now);
		CHECK(!sm.dmr_keyed && hw.tx_calls == tx0 + 2);
		set.tx_inhibit = false;
		radio_sm_set_settings(&sm, &set, now);
		CHECK(sm.dmr_keyed && hw.tx_calls == tx0 + 3);
		// Nor armed on a critical battery
		sm.batt.level = BATT_CRITICAL;
		req(RREQ_HS_POWER, 255);
		CHECK(!sm.dmr_keyed && hw.tx_calls == tx0 + 3 && hw.rx_hz == 438800000);
		sm.batt.level = BATT_OK;
		req(RREQ_HS_POWER, 255);
		CHECK(sm.dmr_keyed && hw.tx_calls == tx0 + 4);
		tx0 += 2;
		off0 = hw.tx_off_calls;
	}
#endif
'''),
    ])
    transform('tests/host/aprsRadioTest.c', [
        ("// On a broadcast channel the beacon's carrier sense",
         ON + r'''// RadioFoundry f4b19e9: the beacon's key-up after its channel wait goes through the keying gate. TX inhibit set while it
// waits for a clear channel: nothing keyed when the channel clears, back on the channel
static void test_inhibit_during_wait(void)
{
	reset();
	aprs_set_test_call("N0TST");
	configure(60, false);
	hw.noise_on_aprs = 30;	// a carrier on 144.390: it waits
	run(61u * 1000u);
	CHECK(hw.rx_hz == APRS_HZ && hw.tx_calls == 0 && sm.aprs_phase != 0);
	sm.set.tx_inhibit = true;	// (a critical battery: the hotspot case in radioCoreTest; this fake's battery is full)
	hw.noise_on_aprs = 120;	// clear: its turn comes
	run(3000);
	CHECK(hw.tx_calls == 0);
	CHECK(hw.starts == 0 && !hw.keyed);
	CHECK(hw.rx_hz == CH_HZ && sm.aprs_phase == 0);
	sm.set.tx_inhibit = false;
}
#endif

// On a broadcast channel the beacon's carrier sense'''),
        ('\ttest_busy_channel();\n', '\ttest_busy_channel();\n' + ON + '\ttest_inhibit_during_wait();\n#endif\n'),
    ])


def prepare_text_c1(transform):
    transform('core/dmr/dmrData.c', [
        ('\tif (cp < 0x20)\n\t{\n\t\treturn 0;\n\t}\n\treturn (cp <= 0xFF) ? (char)cp : \'?\';',
         '#if CONFIG_RF_FIXES\n'
         '\t// RadioFoundry 22b27e8 (#790, MIT): the C1 controls and DEL (0x7F-0x9F) too: they are no letter, and the UI\'s\n'
         '\t// font draws its own icons at 0x80-0x86 (GLYPH_FIRST_EXTRA), so a sender\'s U+0082 would have drawn an arrow\n'
         '\tif ((cp < 0x20) || ((cp >= 0x7F) && (cp < 0xA0)))\n'
         '#else\n'
         '\tif (cp < 0x20)\n'
         '#endif\n'
         '\t{\n\t\treturn 0;\n\t}\n\treturn (cp <= 0xFF) ? (char)cp : \'?\';'),
        ('\t\ttms->text[n++] = (c < 0x100) ? c : \'?\';\n',
         ON +
         '\t\t{\t// 22b27e8: the controls dropped as the ETSI and UTF-16 paths drop them (latin1)\n'
         '\t\t\tchar ch = latin1(c);\n'
         '\t\t\tif (ch != 0)\n'
         '\t\t\t{\n'
         '\t\t\t\ttms->text[n++] = ch;\n'
         '\t\t\t}\n'
         '\t\t}\n'
         '#else\n'
         '\t\ttms->text[n++] = (c < 0x100) ? c : \'?\';\n'
         '#endif\n'),
    ])
    transform('tests/host/dmrDataServiceTest.c', [
        ('\tCHECK(!dmrDataDecodeIPText(be, 1, text, sizeof(text)));\n',
         '\tCHECK(!dmrDataDecodeIPText(be, 1, text, sizeof(text)));\n' + ON + r'''	// RadioFoundry 22b27e8: control characters are dropped, the C1 ones and DEL too (the UI font's own icons sit at
	// 0x80-0x86). Latin-1 letters stay
	static const uint8_t c1[] = { 0x00, 'a', 0x00, 0x82, 0x00, 0x85, 0x00, 0x7F, 0x00, 0x9F, 0x00, 0x0A, 0x00, 0xE9, 0x00, 0xA0 };
	CHECK(dmrDataDecodeIPText(c1, sizeof(c1), text, sizeof(text)) && (strcmp(text, "a\xE9\xA0") == 0));
	// The same in ISO 8859-1 defined short data, where the bytes are the characters
	count = dmrDataBuildEtsiText(false, OUR_ID, RADIO_A, "x\x82y\x9Fz\xF1", false, 0, bursts, DMR_DATA_MAX_BURSTS);
	r = DMR_DATA_RX_NONE;
	dmrDataRxReset();
	for (int i = 0; (i < count) && (r != DMR_DATA_RX_PACKET); i++)
	{
		r = dmrDataRxBurst(&bursts[i]);
	}
	CHECK((count > 0) && (r == DMR_DATA_RX_PACKET));
	CHECK(dmrDataDecodeEtsiText(&dmrDataRxPacket, text, sizeof(text)) && (strcmp(text, "xyz\xF1") == 0));
	dmrDataRxReset();
#if CONFIG_DMR_TMS
	// The same in a Motorola-format TMS message (UTF-16): the controls drop, the Latin-1 letters stay
	dmrDataTMS_t tms_c1;
	count = dmrDataBuildTMS(false, OUR_ID, RADIO_A, "a" "\x82" "\x85" "\x7F" "\x9F" "\xE9" "z", 1, false,
				DT_RATE_12_DATA, 0, bursts, DMR_DATA_MAX_BURSTS);
	r = DMR_DATA_RX_NONE;
	dmrDataRxReset();
	for (int i = 0; (i < count) && (r != DMR_DATA_RX_PACKET); i++)
	{
		r = dmrDataRxBurst(&bursts[i]);
	}
	CHECK((count > 0) && (r == DMR_DATA_RX_PACKET));
	CHECK(dmrDataGetUDP(&dmrDataRxPacket, &udp) && dmrDataDecodeTMS(udp.payload, udp.length, &tms_c1) &&
	      (strcmp(tms_c1.text, "a\xE9z") == 0));
	dmrDataRxReset();
#endif
#endif
'''),
    ])
