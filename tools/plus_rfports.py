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
files in RAM whole, and RAM is the Plus image's scarce resource."""

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
