/* SPDX-License-Identifier: MIT */
/* DDM32 Plus: the native session's lost-request watchdog (hn_watch, after RadioFoundry 3d5940b), built with
 * CONFIG_RF_FIXES=1 by tools/test_native_modem.py. A receive session's owned modem system request left pending with
 * no hn_sys run for 50 ms is dropped and the source unmasked again; a busy channel (requests pending at the poll,
 * the handler running between polls), a request the session doesn't own or hasn't enabled, a session that is not
 * receiving and no session at all are left alone. */
#include "hrc_native_chip.h"
#include <assert.h>
#include <stdio.h>
#include "../../integration/radiofoundry/native/hrc_native_chip.c"

static unsigned kicks;
static int prepare(void *p, const nt_config *c) { (void)p; (void)c; return 1; }
static void noop(void *p) { (void)p; }
static int irqs(void *p, nt_test *t) { (void)p; (void)t; return 1; }
static int restore_hook(void *p) { (void)p; return 1; }
static void kick(void *p) { (void)p; kicks++; }

#define REG(r) regs[(r) / 4]

int main(void)
{
    static uint32_t regs[0x600 / 4];
    static uint8_t buf[0x1000];
    hrc7000_t modem = { .regs = regs, .buf = buf };
    hn_chip chip; nt_test test; nt_ops ops;
    hn_hooks hooks = { NULL, prepare, noop, noop, noop, irqs, noop, noop, restore_hook, NULL, NULL, kick };
    hn_init(&chip, &modem, &hooks, &ops); nt_init(&test, &ops);
    assert(ops.watch != NULL);
    uint32_t t = 1000;

    /* No session: nothing, whatever is pending */
    REG(C7_SYS_INTERP_LIST) = RX_DONE; REG(C7_SYS_INTERP_MASK) = OWNED_SYS;
    for (unsigned i = 0; i < 8; i++) { ops.watch(ops.ctx, t); t += 25000; }
    assert(!kicks && !chip.sys_stuck);

    assert(nt_arm_voice(&test, 145500000, 300000) && nt_begin(&test, test.session, t));
    assert(test.state == NT_RX && REG(C7_SYS_INTERP_MASK) == OWNED_SYS);

    /* A busy channel: a request pending at every poll, the handler running in between: never dropped */
    for (unsigned i = 0; i < 12; i++) {
        assert(nt_ping(&test, test.session, t + 1000));
        REG(C7_SYS_INTERP_LIST) = PHY_READY;
        hn_sys(&chip, t + 5000);
        ops.watch(ops.ctx, t + 25000);
        t += 25000;
    }
    assert(!kicks && !chip.sys_stuck && test.state == NT_RX);

    /* Lost: pending, no hn_sys. First seen, then 25 and 50 ms later still waiting, dropped past 50 ms */
    REG(C7_SYS_INTERP_LIST) = RX_DONE;
    REG(C7_SYS_INTERP_CLEAR) = 0;
    ops.watch(ops.ctx, t);			/* first seen pending with no handler since the last poll */
    ops.watch(ops.ctx, t + 25000);
    ops.watch(ops.ctx, t + 50000);
    assert(!kicks && !chip.sys_stuck);
    ops.watch(ops.ctx, t + 50001);
    assert(kicks == 1 && chip.sys_stuck == 1 && REG(C7_SYS_INTERP_CLEAR) == RX_DONE);
    t += 100000;

    /* The handler runs again: the check starts over (no second drop for the same wait) */
    hn_sys(&chip, t);
    REG(C7_SYS_INTERP_LIST) = 0;
    for (unsigned i = 0; i < 8; i++) { ops.watch(ops.ctx, t); t += 25000; }
    assert(kicks == 1 && chip.sys_stuck == 1);

    /* A request the session doesn't own, or one it hasn't enabled: left alone */
    REG(C7_SYS_INTERP_LIST) = 1u << 14;
    for (unsigned i = 0; i < 8; i++) { ops.watch(ops.ctx, t); t += 25000; }
    REG(C7_SYS_INTERP_LIST) = RX_DONE; REG(C7_SYS_INTERP_MASK) = PHY_READY;
    for (unsigned i = 0; i < 8; i++) { ops.watch(ops.ctx, t); t += 25000; }
    assert(kicks == 1 && chip.sys_stuck == 1);
    REG(C7_SYS_INTERP_MASK) = OWNED_SYS;

    /* Stopped: no session receiving any more, nothing */
    nt_record rec;
    while (nt_pop(&test, test.session, &rec)) {}
    assert(nt_stop(&test, test.session, NT_USER_STOP));
    for (unsigned i = 0; i < 8; i++) { ops.watch(ops.ctx, t); t += 25000; }
    assert(kicks == 1 && chip.sys_stuck == 1);
    assert(nt_cleanup(&test));
    for (unsigned i = 0; i < 8; i++) { ops.watch(ops.ctx, t); t += 25000; }
    assert(kicks == 1 && chip.sys_stuck == 1);

    /* A new session after it: a lost request is caught again */
    assert(nt_arm_voice(&test, 145500000, 300000) && nt_begin(&test, test.session, t));
    REG(C7_SYS_INTERP_LIST) = ABNORMAL | RX_DONE;
    for (unsigned i = 0; i < 5; i++) { ops.watch(ops.ctx, t); t += 25000; }
    assert(kicks == 2 && chip.sys_stuck == 2 && REG(C7_SYS_INTERP_CLEAR) == (ABNORMAL | RX_DONE));
    assert(nt_stop(&test, test.session, NT_USER_STOP) && nt_cleanup(&test));

    puts("PASS native lost-request watchdog: dropped after 50 ms with no handler, busy channel and foreign/masked/stopped left alone");
    return 0;
}
