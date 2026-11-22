/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus r15 "channels" (docs/plus.md "Channels, zones and DMR rows"): the zone and codeplug limits on the real
 * codeplug, its RFKV mode records and zone_edit.c over a simulated NOR flash (upstream's zoneEditTest fixture, its main
 * renamed). Checks:
 *   - a zone grows one channel at a time from 26 to 80 (FM, DMR and P25 channels mixed, so the 27th on is every kind),
 *     each read back whole and readable, the 81st refused as full; the same after a power cycle;
 *   - the limits the UI reports (codeplug_zone_capacity 80, CODEPLUG_ZONES_MAX 68, CODEPLUG_CHANNELS_MAX 1024);
 *   - zone_edit_drop_channel (Delete channel): the channel leaves every zone that holds it (the other
 *     channels in their order), its slot is free (codeplug_free_channel) and a new channel written there is in no
 *     zone; a P25 channel's RFKV record goes with it; the virtual "All channels" zone; an unknown number refused;
 *   - the owner's "only up to 26" (a zone shaped like his): a new channel written alone stays out of the stored "All
 *     channels" copy; ZONE_POS_JOIN puts it at the zone's end (alone when full, the zone gone or virtual). */
#define main upstream_zone_main
#include "zoneEditTest.c"
#undef main
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_tag.h"

static channel_t kind_of(uint16_t n)
{
	char name[CODEPLUG_NAME_LEN + 1];
	snprintf(name, sizeof name, "Ch %u", (unsigned)n);
	channel_t c = make(name, 430000000u + 25000u * n);
	if (n % 3u == 1u) {
		c.mode = RADIO_MODE_DMR;
		c.contact = 1;
	} else if (n % 3u == 2u) {
		c.mode = RADIO_MODE_P25;
		c.p25_nac = 0x293;
		c.p25_tg = 1;
		c.p25_id = 1;
		c.dmr_id = 1;
		c.p25_squelch = 1;
	}
	return c;
}

static void test_zone_grows(void)
{
	fresh();
	for (uint16_t n = 1; n <= 100; n++) {
		channel_t c = kind_of(n);
		CHECK(codeplug_set_channel(n, &c));
	}
	uint16_t first[26];
	for (uint16_t i = 0; i < 26; i++)
		first[i] = (uint16_t)(i + 1u);
	zone_t z = zone_of("Big", first, 26);
	uint16_t at;
	/* The first zone made keeps "All channels" (stored first, its first 80 channels) as zone 0: this one is 1 */
	CHECK(zone_edit_create(&z, &at) == ZONE_EDIT_OK && at == 1 && codeplug_zone_count() == 2);
	CHECK(codeplug_zone_capacity() == CODEPLUG_ZONE_MAX && CODEPLUG_ZONE_MAX == 80);
	CHECK(CODEPLUG_ZONES_MAX == 68 && CODEPLUG_CHANNELS_MAX == 1024);
	for (uint16_t k = 27; k <= 80; k++) {
		CHECK(zone_edit_insert(at, (uint16_t)(k - 1u), k) == ZONE_EDIT_OK);
		zone_t r;
		CHECK(codeplug_zone(at, &r) && r.count == k && r.channels[k - 1u] == k);
	}
	CHECK(zone_edit_insert(at, 80, 81) == ZONE_EDIT_FULL);
	boot_again();
	zone_t r;
	CHECK(codeplug_zone(at, &r) && r.count == 80);
	for (uint16_t i = 0; i < r.count; i++) {
		channel_t c;
		CHECK(r.channels[i] == i + 1u && codeplug_channel(r.channels[i], &c));
		CHECK(c.mode == kind_of(r.channels[i]).mode);
	}
}

static void test_drop_channel(void)
{
	fresh();
	for (uint16_t n = 1; n <= 12; n++) {
		channel_t c = kind_of(n);
		CHECK(codeplug_set_channel(n, &c));
	}
	/* Without stored zones: "All channels" holds it until it is gone */
	CHECK(!codeplug_zones_stored());
	CHECK(zone_edit_drop_channel(12) == ZONE_EDIT_OK);
	channel_t c;
	CHECK(!codeplug_channel(12, &c) && codeplug_free_channel() == 12);
	zone_t a = zone_of("A", (const uint16_t[]){ 1, 5, 2, 7 }, 4);
	zone_t b = zone_of("B", (const uint16_t[]){ 5, 3 }, 2);
	zone_t d = zone_of("C", (const uint16_t[]){ 4 }, 1);
	uint16_t at;
	CHECK(zone_edit_create(&a, &at) == ZONE_EDIT_OK && zone_edit_create(&b, &at) == ZONE_EDIT_OK);
	CHECK(zone_edit_create(&d, &at) == ZONE_EDIT_OK);
	/* A P25 channel (5) in two zones: out of both, its RFKV record gone, the others in order */
	CHECK(codeplug_channel(5, &c) && c.mode == RADIO_MODE_P25);
	uint32_t serial = codeplug_serial();
	CHECK(zone_edit_drop_channel(5) == ZONE_EDIT_OK && codeplug_serial() != serial);
	/* Zone 0 is "All channels", stored when A was made (1-11) */
	CHECK(holds(0, (const uint16_t[]){ 1, 2, 3, 4, 6, 7, 8, 9, 10, 11 }, 10) && codeplug_zone_count() == 4);
	CHECK(holds(1, (const uint16_t[]){ 1, 2, 7 }, 3) && holds(2, (const uint16_t[]){ 3 }, 1));
	CHECK(holds(3, (const uint16_t[]){ 4 }, 1));
	CHECK(!codeplug_channel(5, &c) && codeplug_free_channel() == 5);
	uint8_t v[64];
	CHECK(codeplug_kv_get((uint16_t)(CODEPLUG_KEY_CH_TAG + 5u - 1u), v, sizeof v) < 0);
	/* A new channel in the freed slot is in no zone */
	channel_t fm = make("New", 145500000);
	CHECK(codeplug_set_channel(5, &fm));
	CHECK(holds(1, (const uint16_t[]){ 1, 2, 7 }, 3) && holds(2, (const uint16_t[]){ 3 }, 1));
	CHECK(holds(0, (const uint16_t[]){ 1, 2, 3, 4, 6, 7, 8, 9, 10, 11 }, 10));
	/* The only channel of a zone: the zone stays, empty */
	CHECK(zone_edit_drop_channel(4) == ZONE_EDIT_OK && holds(3, NULL, 0) && named(3, "C"));
	/* In no zone; an empty slot; out of range */
	CHECK(zone_edit_drop_channel(9) == ZONE_EDIT_OK && !codeplug_channel(9, &c));
	CHECK(zone_edit_drop_channel(9) == ZONE_EDIT_NO_CHANNEL && zone_edit_drop_channel(0) == ZONE_EDIT_NO_CHANNEL);
	CHECK(zone_edit_drop_channel(CODEPLUG_CHANNELS_MAX + 1u) == ZONE_EDIT_NO_CHANNEL);
	boot_again();
	CHECK(holds(1, (const uint16_t[]){ 1, 2, 7 }, 3) && !codeplug_channel(4, &c) && !codeplug_channel(9, &c));
	CHECK(holds(0, (const uint16_t[]){ 1, 2, 3, 6, 7, 8, 10, 11 }, 8) && codeplug_zone_count() == 4);
}

static uint8_t note_seen;	/* the UI's last sequence (ui_menus_zone_note keeps its own) */

static uint8_t last_note(uint16_t *n)
{
	*n = 0;
	return zone_edit_note(&note_seen, n);
}

/* n is used and in no zone */
static bool unzoned(uint16_t n)
{
	channel_t c;
	if (!codeplug_channel(n, &c))
		return false;
	for (uint16_t zi = 0; zi < codeplug_zone_count(); zi++) {
		zone_t z;
		if (codeplug_zone(zi, &z) && zone_list_find(&z, n) >= 0)
			return false;
	}
	return true;
}

/* The owner's codeplug (backup of 2026-11-21, its structure only): 26 channels (FM, DMR, P25, some receive only),
 * then a find zone made ("Bulunanlar 1", empty), which stores "All channels" as zone 0 with channels 1-26; channel 27
 * made after it was in no zone, so Home stepped 1-26. zone_edit_add_join: All channels and the zone it was made for */
static void test_new_joins(void)
{
	fresh();
	for (uint16_t n = 1; n <= 26; n++) {
		channel_t c = kind_of(n);
		c.rx_only = n % 5u == 0u;
		CHECK(codeplug_set_channel(n, &c));
	}
	uint16_t n;
	zone_t z;
	channel_t c = kind_of(27), r;
	/* Virtual "All channels": it shows the new one, nothing stored, nothing to say */
	CHECK(zone_edit_add_join(ZONE_NONE, ZONE_POS_JOIN, 27, &c) == ZONE_EDIT_OK && !codeplug_zones_stored());
	CHECK(codeplug_zone(0, &z) && z.count == 27 && last_note(&n) == ZONE_NOTE_IDLE);
	CHECK(zone_edit_drop_channel(27) == ZONE_EDIT_OK);
	uint16_t at;
	CHECK(zone_edit_new("Bulunanlar 1", &at) == ZONE_EDIT_OK && at == 1 && codeplug_zone_count() == 2);
	CHECK(codeplug_zone(0, &z) && z.count == 26 && strcmp(z.name, "All channels") == 0 && holds(1, NULL, 0));
	/* The old path (written alone): the owner's 27th, in no zone */
	CHECK(codeplug_set_channel(27, &c) && unzoned(27));
	/* Home on the find zone: All channels only (entry 27, readable), the find zone keeps its finds only */
	channel_t p = kind_of(29);	/* P25, receive only */
	p.rx_only = true;
	CHECK(zone_edit_add_join(1, ZONE_POS_JOIN, 28, &p) == ZONE_EDIT_OK && last_note(&n) == ZONE_NOTE_IDLE);
	CHECK(codeplug_zone(0, &z) && z.count == 27 && z.channels[26] == 28 && holds(1, NULL, 0));
	for (uint16_t i = 0; i < z.count; i++)
		CHECK(codeplug_channel(z.channels[i], &r));
	/* Home on another zone: both, at the ends */
	zone_t b = zone_of("B", (const uint16_t[]){ 1, 2 }, 2);
	CHECK(zone_edit_create(&b, &at) == ZONE_EDIT_OK && at == 2);
	CHECK(zone_edit_add_join(2, ZONE_POS_JOIN, 29, &c) == ZONE_EDIT_OK && last_note(&n) == ZONE_NOTE_IDLE);
	CHECK(holds(2, (const uint16_t[]){ 1, 2, 29 }, 3) && codeplug_zone(0, &z) && z.channels[z.count - 1u] == 29);
	/* Home on All channels: once */
	CHECK(zone_edit_add_join(0, ZONE_POS_JOIN, 30, &c) == ZONE_EDIT_OK && codeplug_zone(0, &z) && z.count == 29);
	/* The VFO (no zone): All channels */
	CHECK(zone_edit_add_join(ZONE_NONE, ZONE_POS_JOIN, 31, &c) == ZONE_EDIT_OK && codeplug_zone(0, &z) &&
	      z.channels[z.count - 1u] == 31 && holds(2, (const uint16_t[]){ 1, 2, 29 }, 3));
	/* Copy here (Save as new from its editor too): its zone at its position, and All channels */
	CHECK(zone_edit_add_join(2, 1, 32, &c) == ZONE_EDIT_OK && holds(2, (const uint16_t[]){ 1, 32, 2, 29 }, 4));
	CHECK(codeplug_zone(0, &z) && z.count == 31 && z.channels[30] == 32);
	/* A taken number: refused, nothing written */
	CHECK(zone_edit_add_join(2, ZONE_POS_JOIN, 32, &c) == ZONE_EDIT_NO_FREE && holds(2, (const uint16_t[]){ 1, 32, 2, 29 }, 4));
	/* Repair: the owner's stranded 27 into All channels, the count said; then nothing left */
	CHECK(zone_edit_add_unzoned(0) == ZONE_EDIT_OK && last_note(&n) == ZONE_NOTE_ADDED && n == 1);
	CHECK(codeplug_zone(0, &z) && z.count == 32 && z.channels[31] == 27 && !unzoned(27));
	CHECK(zone_edit_add_unzoned(0) == ZONE_EDIT_OK && last_note(&n) == ZONE_NOTE_NO_UNZONED && n == 0);
	/* Home's zone full: in All channels only, said */
	for (uint16_t k = 33; k <= 108; k++) {
		channel_t f = kind_of(k);
		CHECK(codeplug_set_channel(k, &f) && zone_edit_insert(2, 4u + (k - 33u), k) == ZONE_EDIT_OK);
	}
	CHECK(codeplug_zone(2, &z) && z.count == 80);
	CHECK(zone_edit_add_join(2, ZONE_POS_JOIN, 109, &c) == ZONE_EDIT_OK && last_note(&n) == ZONE_NOTE_ONLY_ALL && n == 109);
	CHECK(codeplug_zone(0, &z) && z.channels[z.count - 1u] == 109);
	/* All channels full: Home's zone takes it (said); with no other zone it stays in no zone (said), never silently */
	for (uint16_t k = 115; k <= 162; k++) {
		channel_t f = kind_of(k);
		CHECK(codeplug_set_channel(k, &f));	/* 48 channels in no zone (written alone) */
	}
	CHECK(zone_edit_add_unzoned(0) == ZONE_EDIT_OK && last_note(&n) == ZONE_NOTE_ADDED_FULL && n == 47);
	CHECK(codeplug_zone(0, &z) && z.count == 80 && z.channels[79] == 161 && unzoned(162));
	zone_t d = zone_of("D", (const uint16_t[]){ 3 }, 1);
	CHECK(zone_edit_create(&d, &at) == ZONE_EDIT_OK && at == 3);
	CHECK(zone_edit_add_join(3, ZONE_POS_JOIN, 110, &c) == ZONE_EDIT_OK && last_note(&n) == ZONE_NOTE_NOT_ALL && n == 110);
	CHECK(holds(3, (const uint16_t[]){ 3, 110 }, 2));
	CHECK(zone_edit_add_join(ZONE_NONE, ZONE_POS_JOIN, 111, &c) == ZONE_EDIT_OK && last_note(&n) == ZONE_NOTE_ALL_FULL);
	CHECK(n == 111 && unzoned(111));
	/* Repair into D: every channel in no zone (111, 162) in number order */
	CHECK(zone_edit_add_unzoned(3) == ZONE_EDIT_OK && last_note(&n) == ZONE_NOTE_ADDED && n == 2);
	CHECK(holds(3, (const uint16_t[]){ 3, 110, 111, 162 }, 4));
	/* All channels renamed: an ordinary zone; Home's zone only, or in no zone (said) */
	CHECK(zone_edit_rename(0, "Mine") == ZONE_EDIT_OK);
	CHECK(zone_edit_add_join(3, ZONE_POS_JOIN, 112, &c) == ZONE_EDIT_OK && last_note(&n) == ZONE_NOTE_IDLE);
	CHECK(holds(3, (const uint16_t[]){ 3, 110, 111, 162, 112 }, 5));
	CHECK(zone_edit_add_join(1, ZONE_POS_JOIN, 113, &c) == ZONE_EDIT_OK && last_note(&n) == ZONE_NOTE_NO_ZONE);
	CHECK(n == 113 && unzoned(113) && holds(1, NULL, 0));
	/* Rollback: the zone write fails with the channel in no zone: the channel is deleted (the UI says not saved) */
	CHECK(zone_edit_rename(0, "All channels") == ZONE_EDIT_OK && zone_edit_drop_channel(113) == ZONE_EDIT_OK);
	CHECK(zone_edit_remove(0, 79) == ZONE_EDIT_OK);
	sim_flash_bad_range(EE + 0x8000u, 0x3000u);
	CHECK(zone_edit_add_join(ZONE_NONE, ZONE_POS_JOIN, 114, &c) == ZONE_EDIT_FAILED && !codeplug_channel(114, &r));
	sim_flash_bad_range(0, 0);
	boot_again();
	/* Still gone after a power cycle (the worn zone sector itself is the flash's loss, not this path's) */
	CHECK(!codeplug_channel(114, &r));
}

/* Copy here into All channels itself: at Copy here's position. Two notes before the UI reads: the later one, whole */
static void test_copy_into_all(void)
{
	fresh();
	for (uint16_t n = 1; n <= 8; n++) {	/* past the 6 built-in channels a fresh codeplug starts from */
		channel_t c = kind_of(n);
		CHECK(codeplug_set_channel(n, &c));
	}
	uint16_t at, n;
	CHECK(zone_edit_new("B", &at) == ZONE_EDIT_OK && at == 1 && holds(0, (const uint16_t[]){ 1, 2, 3, 4, 5, 6, 7, 8 }, 8));
	channel_t c = kind_of(9);
	CHECK(zone_edit_add_join(0, 2, 9, &c) == ZONE_EDIT_OK);
	CHECK(holds(0, (const uint16_t[]){ 1, 2, 9, 3, 4, 5, 6, 7, 8 }, 9));
	CHECK(holds(1, NULL, 0) && last_note(&n) == ZONE_NOTE_IDLE);
	/* Home on All channels: at the end, once */
	CHECK(zone_edit_add_join(0, ZONE_POS_JOIN, 10, &c) == ZONE_EDIT_OK);
	CHECK(holds(0, (const uint16_t[]){ 1, 2, 9, 3, 4, 5, 6, 7, 8, 10 }, 10));
	CHECK(zone_edit_rename(0, "X") == ZONE_EDIT_OK);
	CHECK(zone_edit_add_join(ZONE_NONE, ZONE_POS_JOIN, 11, &c) == ZONE_EDIT_OK);
	CHECK(zone_edit_add_unzoned(1) == ZONE_EDIT_OK);	/* "Added to the zone: 1" replaces "In no zone: channel 11" */
	CHECK(last_note(&n) == ZONE_NOTE_ADDED && n == 1 && last_note(&n) == ZONE_NOTE_IDLE);
	CHECK(holds(1, (const uint16_t[]){ 11 }, 1));
}

/* A codeplug with the old 16-channel zone records (layout byte past 4): All channels holds 16, then it is full */
static void test_legacy16(void)
{
	fresh();
	for (uint16_t n = 1; n <= 16; n++) {
		channel_t c = kind_of(n);
		CHECK(codeplug_set_channel(n, &c));
	}
	uint8_t *ee = sim_flash_data + EE;
	ee[0x8010] = 0x01;			/* zone slot 0 in use */
	memset(ee + 0x8030, 0xFF, 0x30 * 2);
	memcpy(ee + 0x8030, "All channels", 12);
	for (uint16_t i = 0; i < 16; i++) {
		ee[0x8040 + 2 * i] = (uint8_t)(i + 1u);
		ee[0x8041 + 2 * i] = 0;
	}
	boot_again();				/* 0x806F, record 1's last name byte: 0xFF, the old layout */
	CHECK(codeplug_zone_capacity() == 16 && codeplug_zones_stored() && codeplug_zone_count() == 1);
	CHECK(holds(0, (const uint16_t[]){ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, 16));
	channel_t c = kind_of(17);
	uint16_t n;
	CHECK(zone_edit_add_join(0, ZONE_POS_JOIN, 17, &c) == ZONE_EDIT_OK && last_note(&n) == ZONE_NOTE_ALL_FULL && n == 17);
	CHECK(unzoned(17));
	CHECK(zone_edit_add_unzoned(0) == ZONE_EDIT_OK && last_note(&n) == ZONE_NOTE_ADDED_FULL && n == 0);
}

int main(void)
{
	if (upstream_zone_main() != 0)
		return 1;
	failures = 0;
	test_zone_grows();
	test_drop_channel();
	test_new_joins();
	test_legacy16();
	test_copy_into_all();
	printf("Plus zone capacity and Delete channel: %s\n", failures ? "FAILED" : "all passed");
	return failures != 0;
}
