/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus P25 RX group list: see p25_rxgl.h */
#include "p25_rxgl.h"
#include "p25/p25_lab.h"

static bool has_list(const channel_t *ch)
{
	return ch->listen_list >= 1u && ch->listen_list <= CODEPLUG_LISTS_MAX;
}

bool p25_rxgl_selective(const channel_t *ch)
{
	if (ch->mode == RADIO_MODE_P25) {
		if (ch->listen_list == CODEPLUG_LIST_ANY)
			return false;
		if (has_list(ch))
			return true;
	}
	return ch->p25_squelch == 1u;
}

uint8_t p25_rxgl_load(const channel_t *ch, uint16_t out[CODEPLUG_LIST_LEN])
{
	static listen_list_t list;	// only the radio task loads a channel
	if (ch->mode != RADIO_MODE_P25 || !has_list(ch) || !codeplug_listen_list(ch->listen_list, &list))
		return 0;
	uint8_t n = 0;
	for (unsigned i = 0; i < list.count && i < CODEPLUG_LIST_LEN; i++) {
		contact_t c;
		if (codeplug_contact(list.contacts[i], &c) && c.type == CONTACT_GROUP && c.id >= 1u && c.id <= 65535u)
			out[n++] = (uint16_t)c.id;
	}
	return n;
}

bool p25_rxgl_apply(const channel_t *ch)
{
	uint16_t tg[CODEPLUG_LIST_LEN];
	return p25_lab_set_groups(tg, p25_rxgl_load(ch, tg)) != 0;
}
