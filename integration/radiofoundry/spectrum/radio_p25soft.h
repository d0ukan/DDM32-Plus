/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus P25 soft probe, radio task side (docs/p25-soft-probe-design.md). Installed as core/radio/radio_p25soft.h
 * when CONFIG_SEARCH_SOFTP25=y (needs P25_SEARCH, SEARCH_HYBRID and NXDN: the flat FM receive path and the HR_C7000's
 * 8 kHz FM RX RAM ring that NXDN reads, board_nxdn_flat and board.afsk).
 *
 * While the P25 search's probe waits for a soft verdict (p25s_soft_pending), the board stays in FM on the hit's
 * frequency: the receive audio is switched to the flat discriminator, the ring is started, P25SOFT_SKIP_MS of samples
 * are dropped (the path's start) and the rest goes to radio/p25soft.c for up to P25SOFT_WINDOW_MS. A good NID is P25
 * (the native receiver then takes the frequency as before); nothing P25-like on a carrier at least P25SOFT_TRUST_DB
 * over the floor is not P25; anything else is left to the native receiver. The ring and the flat path are given back
 * before the verdict, so the native receiver finds the board as after a sweep step.
 *
 * RAM: the probe and its read buffer (about 450 bytes) come from the RTOS heap for the window only; without heap or
 * ring the native receiver probes as before. HARDWARE-UNVERIFIED: host-tested on synthesized signals only. */
#ifndef RADIO_P25SOFT_H
#define RADIO_P25SOFT_H

#include <stdbool.h>
#include <stdint.h>

#define P25SOFT_WINDOW_MS	450u	/* two LDU frame syncs (180 ms apart) after the start */
#define P25SOFT_SKIP_MS		30u	/* samples dropped after the start (flat path, ring, the PLL after the last read) */
#define P25SOFT_TRUST_DB	25	/* "not P25" is believed only on a carrier this far over the floor */

/* Every radio poll (radio_sm.c, before the P25 search's own block): runs the soft probe while one is pending, gives
 * the ring and the flat path back when none is */
void radio_p25soft_poll(uint32_t now_ms);
/* The channel is tuned again (radio_sm.c tune()): a soft probe midway lets go first, while the board is still in FM */
void radio_p25soft_release(void);

#endif
