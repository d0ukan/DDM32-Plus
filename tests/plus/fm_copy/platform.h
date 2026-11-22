/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host double of platform/hrc7000/platform.h for tests/plus/test_fm_copy.c (tools/test_rfports.py) */
#ifndef FM_COPY_PLATFORM_H
#define FM_COPY_PLATFORM_H
#include <stdint.h>
#define HRC_RAMFUNC
uint32_t hrc_micros(void);
static inline void hrc_barrier(void) {}
#endif
