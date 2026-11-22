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

/* The owner's codeplug (backup of 2026-11-21, its structure only): 26 channels (FM, DMR, P25, some receive only),
 * then a find zone made ("Bulunanlar 1", empty), which stores "All channels" as zone 0 with channels 1-26; channel 27
 * made after it. Before the fix the new channel was written alone: in no zone, so Home stepped 1-26. ZONE_POS_JOIN
 * puts a new channel at the end of the zone it names (Home's), alone when the zone is full or the zones are virtual */
static void test_new_joins(void)
{
	fresh();
	for (uint16_t n = 1; n <= 26; n++) {
		channel_t c = kind_of(n);
		c.rx_only = n % 5u == 0u;
		CHECK(codeplug_set_channel(n, &c));
	}
	/* Virtual "All channels": a new channel is in it without a zone write, nothing gets stored */
	channel_t c = kind_of(27);
	CHECK(!codeplug_zones_stored());
	CHECK(zone_edit_add_new(0, ZONE_POS_JOIN, 27, &c) == ZONE_EDIT_OK && !codeplug_zones_stored());
	zone_t z;
	CHECK(codeplug_zone(0, &z) && z.count == 27 && z.channels[26] == 27);
	CHECK(zone_edit_drop_channel(27) == ZONE_EDIT_OK);
	uint16_t at;
	CHECK(zone_edit_new("Bulunanlar 1", &at) == ZONE_EDIT_OK && at == 1 && codeplug_zone_count() == 2);
	CHECK(codeplug_zone(0, &z) && z.count == 26 && strcmp(z.name, "All channels") == 0);
	CHECK(holds(1, NULL, 0));
	/* The old path (the channel written alone): in no zone, Home's zone still ends at 26 */
	CHECK(codeplug_set_channel(27, &c));
	CHECK(codeplug_zone(0, &z) && z.count == 26 && zone_list_find(&z, 27) < 0);
	/* The new one: channel 28 joins zone 0 at its end, every one of the 27 entries readable */
	channel_t p = kind_of(29);	/* P25 */
	p.rx_only = true;
	CHECK(zone_edit_add_new(0, ZONE_POS_JOIN, 28, &p) == ZONE_EDIT_OK);
	CHECK(codeplug_zone(0, &z) && z.count == 27 && z.channels[26] == 28);
	for (uint16_t i = 0; i < z.count; i++) {
		channel_t r;
		CHECK(codeplug_channel(z.channels[i], &r));
	}
	CHECK(holds(1, NULL, 0));
	/* Into the empty find zone too; a taken number refused, nothing changed */
	CHECK(zone_edit_add_new(1, ZONE_POS_JOIN, 30, &c) == ZONE_EDIT_OK && holds(1, (const uint16_t[]){ 30 }, 1));
	CHECK(zone_edit_add_new(0, ZONE_POS_JOIN, 28, &c) == ZONE_EDIT_NO_FREE && codeplug_zone(0, &z) && z.count == 27);
	/* A full zone: the channel is written alone (it is not lost), the zone unchanged */
	for (uint16_t n = 31; n <= 83; n++) {
		channel_t f = kind_of(n);
		CHECK(codeplug_set_channel(n, &f) && zone_edit_insert(0, z.count, n) == ZONE_EDIT_OK);
		CHECK(codeplug_zone(0, &z));
	}
	CHECK(z.count == 80);
	CHECK(zone_edit_add_new(0, ZONE_POS_JOIN, 84, &c) == ZONE_EDIT_OK);
	channel_t r;
	CHECK(codeplug_channel(84, &r) && codeplug_zone(0, &z) && z.count == 80 && zone_list_find(&z, 84) < 0);
	/* A zone that is gone: written alone */
	CHECK(zone_edit_add_new(9, ZONE_POS_JOIN, 85, &c) == ZONE_EDIT_OK && codeplug_channel(85, &r));
	boot_again();
	CHECK(codeplug_zone(0, &z) && z.count == 80 && z.channels[26] == 28 && holds(1, (const uint16_t[]){ 30 }, 1));
}

int main(void)
{
	if (upstream_zone_main() != 0)
		return 1;
	failures = 0;
	test_zone_grows();
	test_drop_channel();
	test_new_joins();
	printf("Plus zone capacity and Delete channel: %s\n", failures ? "FAILED" : "all passed");
	return failures != 0;
}
