/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus P25 soft probe radio test (tests/plus/test_p25soft_radio.c): the board as radio_p25soft.c sees it, the
 * APRS receive ring (hal_afsk.h's receive half) only. The test supplies `board` and its fake ring. */
#ifndef P25SOFT_STUB_BOARD_H
#define P25SOFT_STUB_BOARD_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
	uint32_t rx_rate_hz;
	bool (*rx_start)(void);
	void (*rx_stop)(void);
	uint16_t (*rx_read)(int16_t *buf, uint16_t max);
} hal_afsk_t;
typedef struct {
	const hal_afsk_t *afsk;
} board_t;
extern board_t board;
#endif
