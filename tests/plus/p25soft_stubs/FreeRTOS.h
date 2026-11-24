/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus P25 soft probe radio test: the RTOS heap as the test's (it counts and can fail allocations) */
#ifndef P25SOFT_STUB_FREERTOS_H
#define P25SOFT_STUB_FREERTOS_H
#include <stddef.h>
void *pvPortMalloc(size_t n);
void vPortFree(void *p);
#endif
