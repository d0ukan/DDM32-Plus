/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DOTA Transfer: the receiver's import (dota_import.h, docs/dota.md 10). Installed as
 * core/radio/dota_import.c. */
#include "config.h"
#if CONFIG_DOTA
#include <string.h>
#include "radio/dota_import.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/zone_edit.h"
#include "board.h"

uint16_t codeplug_contact_index(uint32_t id, uint8_t type);	/* codeplug_dota.inc */

static bool bit(const uint8_t *b, unsigned i)
{
	return (b[i >> 3] >> (i & 7u)) & 1u;
}

static void set_bit(uint8_t *b, unsigned i)
{
	b[i >> 3] |= (uint8_t)(1u << (i & 7u));
}

static uint8_t ct_type_of(uint8_t dota)
{
	return dota == DOTA_CT_PRIVATE ? CONTACT_PRIVATE : dota == DOTA_CT_ALL ? CONTACT_ALL_CALL : CONTACT_GROUP;
}

/* --- HELLO --- */

uint8_t dota_imp_begin(dota_imp_t *im, const dota_imp_cfg_t *cfg, const dota_manifest_t *m, uint8_t power_count,
		       uint16_t *free_ch, uint8_t *free_zones, uint8_t *room)
{
	memset(im, 0, sizeof *im);
	im->cfg = *cfg;
	im->kind = m->kind;
	im->zones = m->zones;
	im->channels = m->recs[DOTA_SEC_CHANNELS];
	im->power_count = power_count;
	*free_ch = codeplug_free_channels();
	*free_zones = codeplug_free_zone_slots();
	*room = 0;
	if (codeplug_source() != CODEPLUG_OPENFW || !codeplug_zones_stored() || zone_edit_busy())
		return DOTA_R_NOT_READY;
	if (m->kind > DOTA_KIND_CLONE || (m->kind == DOTA_KIND_CLONE) != (m->recs[DOTA_SEC_ZONES] != 0) ||
	    im->channels == 0 || im->channels > CODEPLUG_CHANNELS_MAX)
		return DOTA_R_VERSION;
	if (*free_ch < im->channels)
		return DOTA_R_CHANNELS;
	if (m->kind == DOTA_KIND_CLONE) {
		if (*free_zones < im->zones)
			return DOTA_R_ZONES;
	} else if (cfg->mode == DOTA_IMP_ADD) {
		if (!codeplug_zone(cfg->zone, &im->z))
			return DOTA_R_NOT_READY;
		*room = (uint8_t)(CODEPLUG_ZONE_MAX - im->z.count);
		if (im->channels > *room)
			return DOTA_R_ZONE_FULL;
	} else {
		*room = CODEPLUG_ZONE_MAX;
		if (*free_zones == 0)
			return DOTA_R_ZONES;
		if (im->channels > CODEPLUG_ZONE_MAX || cfg->name[0] == '\0')
			return DOTA_R_ZONE_FULL;
	}
	/* Contacts and lists: room for every one as new (none may match), and what is free now (the undo's snapshot) */
	uint16_t ct_free = 0;
	for (uint16_t i = 1; i <= CODEPLUG_CONTACTS_MAX; i++) {
		if (!codeplug_contact(i, &im->rec.ct)) {
			set_bit(im->ct_free, i - 1u);
			ct_free++;
		}
	}
	if (ct_free < m->recs[DOTA_SEC_CONTACTS])
		return DOTA_R_CONTACTS;
	uint8_t lists_free = 0;
	for (uint16_t i = 1; i <= CODEPLUG_LISTS_MAX; i++) {
		if (!codeplug_listen_list(i, &im->rec.ll)) {
			set_bit(im->list_free, i - 1u);
			lists_free++;
		}
	}
	if (lists_free < m->recs[DOTA_SEC_LISTS] || m->recs[DOTA_SEC_LISTS] > CODEPLUG_LISTS_MAX)
		return DOTA_R_LISTS;
	return DOTA_R_OK;
}

/* --- Records --- */

static uint16_t contact_slot(dota_imp_t *im, uint32_t id, uint8_t type)
{
	if (id != im->cache_id || type != im->cache_type || im->cache_slot == 0) {
		im->cache_id = id;
		im->cache_type = type;
		im->cache_slot = codeplug_contact_index(id, type);
	}
	return im->cache_slot;
}

/* "1234567" into a name (a contact needs one) */
static void id_name(char *out, uint32_t id)
{
	char t[12];
	int n = 0;
	do {
		t[n++] = (char)('0' + id % 10u);
		id /= 10u;
	} while (id);
	for (int i = 0; i < n; i++)
		out[i] = t[n - 1 - i];
	out[n] = '\0';
}

static bool take_contact(dota_imp_t *im)
{
	contact_t *c = &im->rec.ct;
	if (contact_slot(im, c->id, c->type) != 0) {
		im->ct_matched++;	/* the user's own: name kept */
		return true;
	}
	if (c->name[0] == '\0')
		id_name(c->name, c->id);
	for (uint16_t i = 1; i <= CODEPLUG_CONTACTS_MAX; i++) {
		contact_t probe;
		if (codeplug_contact(i, &probe))
			continue;
		if (!codeplug_set_contact(i, c))
			return false;
		im->ct_added++;
		im->cache_slot = 0;
		return true;
	}
	return false;	/* checked at HELLO: only when the user added contacts meanwhile */
}

static bool take_list(dota_imp_t *im, uint16_t index)
{
	dota_list_t *d = &im->rec.list;
	listen_list_t want;
	memset(&want, 0, sizeof want);
	memcpy(want.name, d->name, sizeof want.name);
	for (uint8_t i = 0; i < d->count; i++) {
		uint8_t type = (d->private_bits >> i) & 1u ? CONTACT_PRIVATE : d->id[i] == 0xFFFFFFu ? CONTACT_ALL_CALL :
			       CONTACT_GROUP;
		uint16_t slot = contact_slot(im, d->id[i], type);
		if (slot != 0)
			want.contacts[want.count++] = slot;
	}
	if (want.name[0] == '\0')
		strcpy(want.name, "List");
	/* The same list here already (name and members): that one */
	for (uint16_t i = 1; i <= CODEPLUG_LISTS_MAX; i++) {
		if (codeplug_listen_list(i, &im->have) && strcmp(im->have.name, want.name) == 0 &&
		    im->have.count == want.count && memcmp(im->have.contacts, want.contacts, want.count * 2u) == 0) {
			im->list_map[index] = (uint8_t)i;
			im->lists_matched++;
			return true;
		}
	}
	for (uint16_t i = 1; i <= CODEPLUG_LISTS_MAX; i++) {
		if (codeplug_listen_list(i, &im->have))
			continue;
		if (!codeplug_set_listen_list(i, &want))
			return false;
		im->list_map[index] = (uint8_t)i;
		im->lists_added++;
		return true;
	}
	return false;
}

static bool take_channel(dota_imp_t *im, uint16_t rank)
{
	dota_ch_t *d = &im->rec.ch;
	channel_t *ch = &d->ch;
	if (rank >= im->channels || !board_in_band(ch->rx_hz, false)) {
		im->skipped++;
		return true;
	}
	if (!im->cfg.tx_ok)
		ch->rx_only = true;
#if CONFIG_DMR
	if (ch->mode == RADIO_MODE_DMR) {
		ch->contact = d->ct_type == DOTA_CT_NONE ? 0 : contact_slot(im, d->ct_id, ct_type_of(d->ct_type));
#if CONFIG_DMR_ANY
		if (d->list == DOTA_LIST_ANY)
			ch->listen_list = CODEPLUG_LIST_ANY;
		else
#endif
		ch->listen_list = d->list != DOTA_LIST_NONE && d->list != DOTA_LIST_ANY && d->list <= CODEPLUG_LISTS_MAX ?
				  im->list_map[d->list - 1u] : 0u;
	}
#endif
	uint16_t number = codeplug_free_nth(rank);
	if (number == 0 || !codeplug_stage_channel(number, ch)) {
		im->skipped++;	/* refused by the codeplug's own checks (a P25 channel it can't store) */
		return true;
	}
	set_bit(im->staged, rank);
	im->added++;
	return true;
}

/* A zone part into its staged record (DOTA Clone) */
static bool take_zone_part(dota_imp_t *im)
{
	dota_zone_t *p = &im->rec.zp;
	if (im->kind != DOTA_KIND_CLONE || p->zone >= im->zones)
		return true;
	zone_t *z = &im->z;
	if (!bit(im->zstaged, p->zone)) {
		memset(z, 0, sizeof *z);
		memset(z->channels, 0xFF, sizeof z->channels);
		z->count = CODEPLUG_ZONE_MAX;
	} else if (!codeplug_staged_zone(p->zone, z)) {
		return false;
	}
	if (p->part == 0)
		memcpy(z->name, p->name, sizeof z->name);
	uint8_t at = p->offset;
	for (uint8_t r = 0; r < p->runs; r++) {
		for (uint8_t j = 0; j < p->len[r] && at < CODEPLUG_ZONE_MAX; j++, at++) {
			uint16_t k = (uint16_t)(p->start[r] + j);
			/* a member left out (not in this radio's bands) leaves a hole the commit closes */
			z->channels[at] = k < im->channels && bit(im->staged, k) ? codeplug_free_nth(k) : 0xFFFFu;
		}
	}
	if (!codeplug_stage_zone(p->zone, z))
		return false;
	set_bit(im->zstaged, p->zone);
	return true;
}

bool dota_imp_frame(dota_imp_t *im, uint8_t sec, uint16_t first, uint8_t count, const uint8_t *p, uint8_t len)
{
	size_t at = 0;
	for (uint8_t i = 0; i < count && !im->failed; i++) {
		size_t n = 0;
		bool ok = true;
		switch (sec) {
		case DOTA_SEC_CONTACTS:
			n = dota_rec_get_contact(p + at, len - at, &im->rec.ct);
			ok = n == 0 || take_contact(im);
			break;
		case DOTA_SEC_LISTS:
			n = dota_rec_get_list(p + at, len - at, &im->rec.list);
			ok = n == 0 || (uint16_t)(first + i) >= CODEPLUG_LISTS_MAX || take_list(im, (uint16_t)(first + i));
			break;
		case DOTA_SEC_CHANNELS:
			n = dota_rec_get_channel(p + at, len - at, im->power_count, &im->rec.ch);
			ok = n == 0 || take_channel(im, (uint16_t)(first + i));
			break;
		case DOTA_SEC_ZONES:
			n = dota_rec_get_zone(p + at, len - at, &im->rec.zp);
			ok = n == 0 || take_zone_part(im);
			break;
		default:
			break;
		}
		if (!ok)
			im->failed = true;
		if (n == 0) {
			/* a record that doesn't decode: the frame passed its CRC, so the sender's build differs. What it
			 * holds after this record is lost: the channels count as left out */
			if (sec == DOTA_SEC_CHANNELS)
				im->skipped = (uint16_t)(im->skipped + count - i);
			break;
		}
		at += n;
	}
	return !im->failed;
}

/* --- The end --- */

static bool zone_name_taken(dota_imp_t *im, const char *name)
{
	for (uint16_t i = 0; i < codeplug_zone_count(); i++)
		if (codeplug_zone(i, &im->z) && strcmp(im->z.name, name) == 0)
			return true;
	return false;
}

/* A zone name not in use here: name, else "name 2", cut to fit (the zone editor's copy rule). Uses im->z */
static void unique_name(dota_imp_t *im, char *name)
{
	char base[CODEPLUG_NAME_LEN + 1];
	memcpy(base, name, sizeof base);
	if (zone_name_taken(im, name))
		zone_copy_name(name, base);
}

/* The channels just made, im->rec.numbers (count of them), into "All channels" as well: a zone made by an import is not
 * the zone Home steps through when that is All channels (zone_join.inc; a failure does not undo the import) */
static void join_all(const dota_imp_t *im, uint16_t count)
{
#if CONFIG_CHAN_EXTRAS
	(void)zone_edit_join_all(im->rec.numbers, count);
#else
	(void)im;
	(void)count;
#endif
}

/* A new zone named name with the channels in im->rec.numbers */
static bool make_zone(dota_imp_t *im, const char *name, uint16_t count)
{
	char unique[CODEPLUG_NAME_LEN + 1];
	memcpy(unique, name, sizeof unique);
	unique[CODEPLUG_NAME_LEN] = '\0';
	if (unique[0] == '\0')
		strcpy(unique, "Zone");
	unique_name(im, unique);
	zone_t *z = &im->z;
	memset(z, 0, sizeof *z);
	memcpy(z->name, unique, sizeof z->name);
	z->count = count;
	memcpy(z->channels, im->rec.numbers, count * sizeof z->channels[0]);
	if (zone_edit_create(z, NULL) != ZONE_EDIT_OK)
		return false;
	im->zones_added++;
	return true;
}

bool dota_imp_commit(dota_imp_t *im)
{
	if (im->failed)
		return false;
	uint16_t *numbers = im->rec.numbers;
	uint16_t count = 0;
	/* One channel, selected channels, one zone: the staged channels in their order, numbered before the commit (a
	 * rank names a free slot only until then) */
	if (im->kind != DOTA_KIND_CLONE)
		for (uint16_t k = 0; k < im->channels && count < CODEPLUG_ZONE_MAX; k++)
			if (bit(im->staged, k))
				numbers[count++] = codeplug_free_nth(k);
	if (!codeplug_commit_staged(im->staged, im->channels))
		return false;
	if (im->kind != DOTA_KIND_CLONE) {
		if (count == 0)
			return true;	/* nothing this radio can receive: nothing to add */
		if (im->cfg.mode == DOTA_IMP_NEW) {
			if (!make_zone(im, im->cfg.name, count))
				return false;
			join_all(im, count);
			return true;
		}
		zone_t *z = &im->z;
		if (!codeplug_zone(im->cfg.zone, z))
			return false;
		for (uint16_t i = 0; i < count && z->count < CODEPLUG_ZONE_MAX; i++)
			z->channels[z->count++] = numbers[i];
		if (!codeplug_set_zone(im->cfg.zone, z))
			return false;
		join_all(im, count);	/* the zone it was added to may be any one */
		return true;
	}
	/* DOTA Clone: each staged zone in order under the sender's name. Each takes the lowest free slot, which is where
	 * the next one was staged (the ranks shift by the zones already made); a zone left empty is passed over */
	uint8_t passed = 0;
	char name[CODEPLUG_NAME_LEN + 1];
	for (uint8_t i = 0; i < im->zones; i++) {
		if (!bit(im->zstaged, i))
			continue;	/* no part came: it was never staged, so it takes no rank */
		zone_t *z = &im->z;
		if (!codeplug_staged_zone(passed, z))
			return false;
		count = 0;
		for (uint16_t j = 0; j < CODEPLUG_ZONE_MAX; j++)
			if (z->channels[j] != 0xFFFFu && z->channels[j] != 0)
				numbers[count++] = z->channels[j];
		memcpy(name, z->name, sizeof name);
		if (count == 0) {
			passed++;	/* stays staged in its slot: the next zone is one rank on */
			continue;
		}
		if (!make_zone(im, name, count))
			return false;
		join_all(im, count);
	}
	return true;
}

void dota_imp_undo(dota_imp_t *im)
{
	for (uint16_t i = 1; i <= CODEPLUG_CONTACTS_MAX; i++)
		if (bit(im->ct_free, i - 1u) && codeplug_contact(i, &im->rec.ct))
			(void)codeplug_delete_contact(i);
	for (uint16_t i = 1; i <= CODEPLUG_LISTS_MAX; i++)
		if (bit(im->list_free, i - 1u) && codeplug_listen_list(i, &im->have))
			(void)codeplug_delete_listen_list(i);
	for (uint16_t k = 0; k < im->channels; k++)
		if (bit(im->staged, k)) {
			uint16_t number = codeplug_free_nth(k);
			if (number != 0)
				(void)codeplug_tag_remove(number);	/* a staged P25 channel's tag */
		}
	memset(im->staged, 0, sizeof im->staged);
	im->ct_added = 0;
	im->lists_added = 0;
	im->added = 0;
}
#endif
