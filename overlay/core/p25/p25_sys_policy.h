// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 Open25-DM32 Authors
#ifndef P25_SYS_POLICY_H
#define P25_SYS_POLICY_H
#include <stdbool.h>
#include <stdint.h>

// A system whose WACN is in BEE00..BEEFF, or whose system ID is 00A, gets no transmission from the radio, of any
// kind, however it is heard or programmed
#define P25_WACN_NO_TX		0xBEE00u
#define P25_WACN_NO_TX_MASK	0xFFF00u
#define P25_SYSID_NO_TX		0x00Au

static inline bool p25_wacn_no_tx(uint32_t wacn) { return (wacn & P25_WACN_NO_TX_MASK) == P25_WACN_NO_TX; }
static inline bool p25_sysid_no_tx(uint16_t sysid) { return sysid == P25_SYSID_NO_TX; }

// Validated standard, unprotected LC only. Neighbor messages never identify this carrier.
static inline bool p25_lc_sys_no_tx(const uint8_t raw[9])
{
	const uint8_t *w;
	if ((raw[0] & 0xC0) != 0x40) return false;
	switch (raw[0] & 0x3F) {
	case 35: return p25_sysid_no_tx((uint16_t)((raw[2] & 0x0F) << 8 | raw[3]));
	case 36: w = &raw[2]; break;
	case 41: w = &raw[1]; break;
	default: return false;
	}
	return p25_wacn_no_tx((uint32_t)w[0] << 12 | (uint32_t)w[1] << 4 | w[2] >> 4) ||
	       p25_sysid_no_tx((uint16_t)((w[2] & 0x0F) << 8 | w[3]));
}
#endif
