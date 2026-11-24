/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus P25 soft probe radio test: APRS receive as radio_p25soft.c sees it (the test says whether it listens) */
#ifndef P25SOFT_STUB_APRS_H
#define P25SOFT_STUB_APRS_H
#include <stdbool.h>
bool aprs_rx_listening(void);
#endif
