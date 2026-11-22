/* SPDX-License-Identifier: MIT */
#ifndef HRC_NATIVE_CHIP_H
#define HRC_NATIVE_CHIP_H
#include "native_test.h"
#include "modem/hrc7000/hrc7000.h"
typedef struct {
    void *ctx;
    int (*prepare)(void *, const nt_config *);
    void (*fast_off)(void *);
    void (*key_request)(void *);
    void (*pa_on)(void *);
    int (*irq_on)(void *, nt_test *);
    void (*irq_quiet)(void *);
    void (*irq_restore)(void *);
    int (*restore)(void *);
    uint32_t (*lock)(void *);
    void (*unlock)(void *, uint32_t);
#if defined(CONFIG_RF_FIXES) && CONFIG_RF_FIXES
    void (*irq_kick)(void *); /* DDM32 Plus: the modem system source unmasked again (hn_watch); optional */
#endif
} hn_hooks;
enum { HN_REGS=15, HN_DIAG_REGS=24 };
typedef struct {
    hrc7000_t *modem;
    const hn_hooks *hooks;
    nt_test *test;
    uint32_t saved[HN_REGS];
    uint32_t saved_rf_mode;
    uint32_t last_event,last_us,last_sys,last_type,last_sys_us;
    uint32_t event_regs[HN_DIAG_REGS];
#if CONFIG_DEV_TX
    uint32_t staged_index;
#endif
#if defined(CONFIG_RF_FIXES) && CONFIG_RF_FIXES
    /* DDM32 Plus lost-request watchdog (hn_watch): the last hn_sys time it saw with a request
     * pending, since when that request waits, and how many were dropped. */
    uint32_t stuck_sys_us, stuck_since, sys_stuck;
    uint8_t stuck_waiting;
#endif
    uint8_t prepared, saved_valid;
} hn_chip;
void hn_init(hn_chip *, hrc7000_t *, const hn_hooks *, nt_ops *);
void hn_sys(hn_chip *, uint32_t now_us);
void hn_slot(hn_chip *, uint8_t source, uint32_t now_us);
void hn_rftx(hn_chip *, uint32_t now_us);
#endif
