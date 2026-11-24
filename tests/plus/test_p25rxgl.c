/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus P25 RX group list (CONFIG_P25_RXGL, tools/plus_p25rxgl.py) in the real codeplug encoder, flash journal
 * and reboot path (built with CONFIG_P25_RXGL=1, CONFIG_DMR_ANY=1): a P25 channel keeps its RX group list (None, a
 * list, Any) and its explicit talkgroup over a save and a reboot; the filter rule; the talkgroups the radio takes from
 * a list of the codeplug (group contacts 1-65535 only); what reaches the P25 worker (a fake p25_lab_set_groups). */
#define main upstream_codeplug_main
#include "codeplugEditTest.c"
#undef main
#include "p25/p25_rxgl.h"

/* The worker's side, faked: what p25_rxgl_apply hands over */
static uint16_t lab_tg[64];
static unsigned lab_n, lab_calls;
static int lab_ok = 1;
int p25_lab_set_groups(const uint16_t *talkgroups, uint8_t count)
{
	lab_calls++;
	if (!lab_ok)
		return 0;
	lab_n = count;
	for (unsigned i = 0; i < count && i < 64u; i++)
		lab_tg[i] = talkgroups[i];
	return 1;
}
#include "p25/p25_rxgl.c"

static contact_t group(const char *name, uint32_t id, uint8_t type)
{
	contact_t c;
	memset(&c, 0, sizeof c);
	strncpy(c.name, name, CODEPLUG_NAME_LEN);
	c.id = id;
	c.type = type;
	return c;
}

static channel_t p25(const char *name, uint16_t tg, uint16_t list)
{
	channel_t p = make(name, 438500000, 430900000);
	p.mode = RADIO_MODE_P25;
	p.p25_nac = 0x659;
	p.p25_tg = tg;
	p.p25_id = 1234;
	p.dmr_id = 1234;
	p.p25_squelch = 1;
	p.listen_list = list;
	return p;
}

int main(void)
{
	int prior = upstream_codeplug_main();
	fresh();
	/* The codeplug's contacts and lists (CPS, DOTA import) */
	contact_t c = group("TG 101", 101, CONTACT_GROUP);
	CHECK(codeplug_set_contact(1, &c));
	c = group("Big DMR", 70000, CONTACT_GROUP);		/* not a P25 talkgroup */
	CHECK(codeplug_set_contact(2, &c));
	c = group("Joe", 1234, CONTACT_PRIVATE);
	CHECK(codeplug_set_contact(3, &c));
	c = group("All", 0xFFFFFF, CONTACT_ALL_CALL);
	CHECK(codeplug_set_contact(4, &c));
	c = group("TG 655", 655, CONTACT_GROUP);
	CHECK(codeplug_set_contact(5, &c));
	c = group("TG 65535", 65535, CONTACT_GROUP);
	CHECK(codeplug_set_contact(6, &c));
	listen_list_t l;
	memset(&l, 0, sizeof l);
	strcpy(l.name, "Local P25");
	l.count = 8;
	const uint16_t members[8] = {1, 2, 3, 4, 5, 9 /* empty */, 6, 1 /* twice */};
	memcpy(l.contacts, members, sizeof members);
	CHECK(codeplug_set_listen_list(3, &l));
	listen_list_t full;
	memset(&full, 0, sizeof full);
	strcpy(full.name, "Full");
	full.count = CODEPLUG_LIST_LEN;
	for (unsigned i = 0; i < CODEPLUG_LIST_LEN; i++)
		full.contacts[i] = 5;
	CHECK(codeplug_set_listen_list(76, &full));

	/* A P25 channel with a list: saved and read back (and after a reboot) with the list and the talkgroup */
	channel_t ch = p25("P25 list", 101, 3), back;
	CHECK(codeplug_set_channel(21, &ch));
	CHECK(codeplug_channel(21, &back) && back.mode == RADIO_MODE_P25 && back.listen_list == 3 && back.p25_tg == 101 &&
	      back.p25_nac == 0x659 && back.contact == 0);
	CHECK(codeplug_init() == CODEPLUG_OPENFW);
	CHECK(codeplug_channel(21, &back) && back.listen_list == 3 && back.p25_tg == 101 && !back.rx_only);
	/* Any (every group) and None */
	ch = p25("P25 any", 655, CODEPLUG_LIST_ANY);
	CHECK(codeplug_set_channel(22, &ch) && codeplug_init() == CODEPLUG_OPENFW);
	CHECK(codeplug_channel(22, &back) && back.listen_list == CODEPLUG_LIST_ANY && back.p25_tg == 655);
	ch = p25("P25 none", 655, 0);
	CHECK(codeplug_set_channel(23, &ch) && codeplug_channel(23, &back) && back.listen_list == 0 && back.p25_tg == 655);
	/* The record: the list in the RX group byte, Any in the colour code byte's 0x20 bit (as a DMR channel) */
	uint8_t r[CODEPLUG_OPENFW_CHANNEL_SIZE];
	memset(r, 0xFF, sizeof r);
	ch = p25("P25 list", 101, 3);
	codeplug_openfw_encode(&ch, r);
	CHECK(r[0x2B] == 3);

	/* The filter: a list filters, Any does not, None keeps the squelch; other modes keep theirs */
	ch = p25("x", 101, 3);
	ch.p25_squelch = 0;
	CHECK(p25_rxgl_selective(&ch));
	ch.listen_list = CODEPLUG_LIST_ANY;
	ch.p25_squelch = 1;
	CHECK(!p25_rxgl_selective(&ch));
	ch.listen_list = 0;
	CHECK(p25_rxgl_selective(&ch));
	ch.p25_squelch = 0;
	CHECK(!p25_rxgl_selective(&ch));
	ch.listen_list = 77;	/* out of range: as None */
	CHECK(!p25_rxgl_selective(&ch));
	channel_t y = ch;
	y.mode = RADIO_MODE_YSF;
	y.listen_list = 3;
	y.p25_squelch = 0;
	CHECK(!p25_rxgl_selective(&y));

	/* The talkgroups taken: group contacts 1-65535 in list order; private, All Call, >65535 and empty slots left out */
	uint16_t tg[CODEPLUG_LIST_LEN];
	ch = p25("x", 101, 3);
	CHECK(p25_rxgl_load(&ch, tg) == 4 && tg[0] == 101 && tg[1] == 655 && tg[2] == 65535 && tg[3] == 101);
	ch.listen_list = 76;
	CHECK(p25_rxgl_load(&ch, tg) == CODEPLUG_LIST_LEN && tg[CODEPLUG_LIST_LEN - 1] == 655);
	ch.listen_list = 4;	/* no such list */
	CHECK(p25_rxgl_load(&ch, tg) == 0);
	ch.listen_list = CODEPLUG_LIST_ANY;
	CHECK(p25_rxgl_load(&ch, tg) == 0);
	ch.listen_list = 0;
	CHECK(p25_rxgl_load(&ch, tg) == 0);
	y.listen_list = 3;
	CHECK(p25_rxgl_load(&y, tg) == 0);	/* not P25 */

	/* To the worker: the list, or an empty one; a refusal is reported */
	ch = p25("x", 101, 3);
	CHECK(p25_rxgl_apply(&ch) && lab_n == 4 && lab_tg[1] == 655);
	ch.listen_list = 0;
	CHECK(p25_rxgl_apply(&ch) && lab_n == 0);
	lab_ok = 0;
	CHECK(!p25_rxgl_apply(&ch) && lab_calls == 3);
	CHECK(sim_flash_violations() == 0);
	puts("Plus P25 RX group list: channel list / Any / None and talkgroup persist, filter rule, list talkgroups, worker hand-over");
	return prior || failures ? 1 : 0;
}
