/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus (CONFIG_CHAN_EXTRAS, tools/plus_chux.py prepare_new_joins, docs/plus.md "Zone size: the real cause"):
 * a channel made on the radio goes into "All channels" and the zone it was made for, and Add unzoned channels.
 * Included into core/codeplug/zone_edit.c (work, probe, load, write_back, the storage lock).
 *
 * "All channels" is zone 0 while it keeps the name make_room stored it with (the virtual zone's, codeplug.c
 * all_channels; "Tüm kanallar" typed on the radio counts too). Renamed, it is an ordinary zone: a new channel then goes
 * only into the zone it was made for. */

/* What the last ZONE_OP_ADD_JOIN or ZONE_OP_UNZONED did, for the UI (zone_edit_note): written by the storage job, read
 * by the UI task; the number first, the kind last */
static volatile uint16_t note_n;
static volatile uint8_t note_k;

static void note(uint8_t kind, uint16_t n)
{
	note_n = n;
	note_k = kind;
}

uint8_t zone_edit_note(uint16_t *n)
{
	uint8_t k = note_k;
	if (k != ZONE_NOTE_IDLE) {
		*n = note_n;
		note_k = ZONE_NOTE_IDLE;
	}
	return k;
}

/* work is zone 0 as make_room stored it */
static bool all_zone(void)
{
	return strcmp(work.name, "All channels") == 0 || strcmp(work.name, "T\374m kanallar") == 0;
}

/* number at pos of zone (past the end: at the end) */
static zone_edit_result_t join(uint16_t zone, uint16_t pos, uint16_t number)
{
	zone_edit_result_t r = load(zone);
	if (r == ZONE_EDIT_OK)
		r = zone_list_insert(&work, pos < work.count ? pos : work.count, number, codeplug_zone_capacity());
	return r == ZONE_EDIT_OK ? write_back(zone) : r;
}

zone_edit_result_t zone_edit_add_join(uint16_t zone, uint16_t pos, uint16_t number, const channel_t *ch)
{
	storage_lock();
	zone_edit_result_t r = number == 0 || codeplug_channel(number, &probe) ? ZONE_EDIT_NO_FREE :
			       codeplug_set_channel(number, ch) ? ZONE_EDIT_OK : ZONE_EDIT_FAILED;
	if (r != ZONE_EDIT_OK || !codeplug_zones_stored()) {
		/* The virtual zone holds every channel, its first 80 */
		if (r == ZONE_EDIT_OK && load(0) == ZONE_EDIT_OK && zone_list_find(&work, number) < 0)
			note(ZONE_NOTE_NO_ZONE, number);
		storage_unlock();
		return r;
	}
	bool all = load(0) == ZONE_EDIT_OK && all_zone();
	zone_edit_result_t ra = all ? join(0, ZONE_POS_JOIN, number) : ZONE_EDIT_NO_ZONE;
	zone_edit_result_t rh = ZONE_EDIT_NO_ZONE;
	if (ra != ZONE_EDIT_FAILED && zone != ZONE_NONE && !(all && zone == 0)) {
		rh = load(zone);
		if (rh == ZONE_EDIT_OK && pos == ZONE_POS_JOIN && strncmp(work.name, "Bulunanlar ", 11) == 0)
			rh = ZONE_EDIT_NO_ZONE;		/* Home on a Find & save zone: it holds the finds only */
		else if (rh == ZONE_EDIT_OK)
			rh = join(zone, pos, number);
	}
	if (ra == ZONE_EDIT_FAILED || (rh == ZONE_EDIT_FAILED && ra != ZONE_EDIT_OK)) {
		codeplug_delete_channel(number);	/* in no zone because a write failed: not saved at all */
		storage_unlock();
		return ZONE_EDIT_FAILED;
	}
	uint8_t k = ZONE_NOTE_IDLE;
	if (ra != ZONE_EDIT_OK && rh != ZONE_EDIT_OK)
		k = ra == ZONE_EDIT_FULL ? ZONE_NOTE_ALL_FULL : ZONE_NOTE_NO_ZONE;
	else if (ra == ZONE_EDIT_FULL)
		k = ZONE_NOTE_NOT_ALL;
	else if (ra == ZONE_EDIT_OK && rh != ZONE_EDIT_OK && rh != ZONE_EDIT_NO_ZONE)
		k = ZONE_NOTE_ONLY_ALL;			/* its zone full (or its write failed) */
	if (k != ZONE_NOTE_IDLE)
		note(k, number);
	storage_unlock();
	return ZONE_EDIT_OK;
}

/* Every channel in no zone, at the end of zone in number order while it has room: one write for each 256 channel
 * numbers that have such channels (one for most radios) */
zone_edit_result_t zone_edit_add_unzoned(uint16_t zone)
{
	storage_lock();
	zone_edit_result_t r = codeplug_zones_stored() ? load(zone) : ZONE_EDIT_OK;	/* virtual: every one is in it */
	uint16_t added = 0, top = codeplug_zones_stored() ? codeplug_channel_count() : 0;
	bool full = false;
	for (uint16_t base = 1; r == ZONE_EDIT_OK && !full && base <= top; base = (uint16_t)(base + 256u)) {
		uint8_t seen[32];
		memset(seen, 0, sizeof seen);
		for (uint16_t zi = 0; r == ZONE_EDIT_OK && zi < codeplug_zone_count(); zi++) {
			r = load(zi);
			for (uint16_t i = 0; r == ZONE_EDIT_OK && i < work.count; i++) {
				uint16_t c = (uint16_t)(work.channels[i] - base);
				if (work.channels[i] >= base && c < 256u)
					seen[c >> 3] |= (uint8_t)(1u << (c & 7u));
			}
		}
		if (r == ZONE_EDIT_OK)
			r = load(zone);
		uint16_t had = work.count;
		for (uint16_t c = 0; r == ZONE_EDIT_OK && c < 256u && base + c <= top; c++) {
			if (seen[c >> 3] & (1u << (c & 7u)) || !codeplug_channel((uint16_t)(base + c), &probe))
				continue;
			if (work.count >= codeplug_zone_capacity() || work.count >= CODEPLUG_ZONE_MAX) {
				full = true;
				break;
			}
			work.channels[work.count++] = (uint16_t)(base + c);
		}
		if (r == ZONE_EDIT_OK && work.count != had) {
			r = write_back(zone);
			if (r == ZONE_EDIT_OK)
				added = (uint16_t)(added + work.count - had);
		}
	}
	if (r == ZONE_EDIT_OK)
		note(full ? ZONE_NOTE_ADDED_FULL : added ? ZONE_NOTE_ADDED : ZONE_NOTE_NO_UNZONED, added);
	storage_unlock();
	return r;
}
