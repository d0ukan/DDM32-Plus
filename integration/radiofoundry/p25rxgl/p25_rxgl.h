/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus P25 RX group list (CONFIG_P25_RXGL, tools/plus_p25rxgl.py, docs/rx-group-list-budget.md).
 * A P25 channel names one of the codeplug's RX group lists (channel_t.listen_list, the record's own byte, as a DMR
 * channel does); its group contacts with a talkgroup of 1-65535 play on the channel besides the channel's talkgroup.
 * Nothing new is stored: the lists and contacts are the codeplug's (CPS, DOTA import), read here only. */
#ifndef P25_RXGL_H
#define P25_RXGL_H
#include <stdbool.h>
#include <stdint.h>
#include "codeplug/codeplug.h"

/* The talkgroup filter for the channel: a P25 channel with a list filters (Selective), with Any (all groups) not,
 * else its squelch decides as before (Selective = 1). Other modes: their squelch as before */
bool p25_rxgl_selective(const channel_t *ch);
/* The P25 talkgroups of the channel's list into out, in list order, at most CODEPLUG_LIST_LEN; 0 when the channel
 * is not P25, has no list (None / Any) or the list is unreadable. Private contacts, All Call (always heard) and DMR
 * talkgroups above 65535 are left out */
uint8_t p25_rxgl_load(const channel_t *ch, uint16_t out[CODEPLUG_LIST_LEN]);
/* After p25_lab_set_channel for the channel: hand its list to the P25 worker. false if the worker refused it */
bool p25_rxgl_apply(const channel_t *ch);
#endif
