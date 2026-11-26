/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus: the codeplug settings check cache (tools/plus_chlag.py, docs/plus.md "Channels list speed"). A lookup
 * checks the whole settings chain (CRC and entry walk of every page) only once per change of
 * codeplug_kv_generation(); this counts the data-chip reads of the Channels screen's lookups, and proves that every
 * path that can change the area underneath (a key write, a CPS / TFTP sector commit, a codeplug record write, a failed
 * or cut write, a reload, a power-on) makes the next lookup check it whole again: a corrupt area is refused and a
 * changed value is seen exactly as without the cache. With -DKV_BASELINE only the read counts (the code before). */
#define main upstream_codeplug_main
#include "codeplugEditTest.c"
#undef main
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_tag.h"

static uint32_t reads, read_bytes;
static bool (*real_read)(uint32_t addr, void *buf, uint32_t len);

static bool counting_read(uint32_t addr, void *buf, uint32_t len)
{
	reads++;
	read_bytes += len;
	return real_read(addr, buf, len);
}

static void count_reads(void)
{
	if (sim_flash.read != counting_read) {
		real_read = sim_flash.read;
		sim_flash.read = counting_read;
	}
}

/* n channels, every second one a P25 channel (a tag each) until p25 of them; one zone holds them (its cap) */
static void kv_populate(uint16_t n, uint16_t p25)
{
	fresh();
	count_reads();
	CHECK(codeplug_set_dmr_id(12345));
	zone_t z;
	memset(&z, 0, sizeof z);
	strcpy(z.name, "All");
	uint16_t tagged = 0;
	for (uint16_t i = 1; i <= n; i++) {
		char name[CODEPLUG_NAME_LEN + 1];
		snprintf(name, sizeof name, "CH %u", i);
		channel_t c = make(name, 438000000u + i * 12500u, 438000000u + i * 12500u);
		if (i % 2 == 0 && tagged < p25) {
			c.mode = RADIO_MODE_P25;
			c.kind = CODEPLUG_KIND_P25;
			c.p25_nac = 0x293;
			c.p25_tg = (uint16_t)(100u + i);
			c.p25_id = 101;
			c.dmr_id = 0;	/* the radio-wide P25 unit ID: codeplug_p25_unit_id's lookup */
			tagged++;
		}
		CHECK(codeplug_set_channel(i, &c));
		if (z.count < CODEPLUG_ZONE_MAX)
			z.channels[z.count++] = i;
	}
	CHECK(codeplug_set_zone(0, &z));
}

/* The Channels screen's reads (ui_menus.c chlist_*, ui_app.c): entering it loads every channel (ui_filter_rebuild)
 * and draws 7 rows; a move loads the focused row (list_move) and draws 7 rows again. Before the fix each row also read
 * the zone (chlist_current); after it, each count() of the list does (list_key, list_move, list_draw: 3 a move) */
static void measure(uint16_t n, uint16_t p25)
{
	kv_populate(n, p25);
	channel_t ch;
	zone_t z;
	reads = read_bytes = 0;
	(void)codeplug_channel(2, &ch);
	uint32_t first = reads;
	reads = read_bytes = 0;
	for (uint16_t i = 1; i <= n; i++)
		(void)codeplug_channel(i, &ch);
	uint32_t rebuild = reads, rebuild_bytes = read_bytes;
	reads = read_bytes = 0;
	for (uint16_t i = 1; i <= 8; i++)
		(void)codeplug_channel(i, &ch);
	uint32_t rows8 = reads, rows8_bytes = read_bytes;
	reads = read_bytes = 0;
	(void)codeplug_zone(0, &z);
	uint32_t zone = reads, zone_bytes = read_bytes;
	uint32_t row7 = rows8 * 7u / 8u;
#ifdef KV_BASELINE
	uint32_t enter = rebuild + row7 + 8u * zone, move = rows8 + 8u * zone;
#else
	uint32_t enter = rebuild + row7 + 2u * zone, move = rows8 + 3u * zone;
#endif
	printf("kv %s: %u channels (%u P25): first lookup after a change %u reads; all channels %u reads (%u B); "
	       "8 rows %u reads (%u B); zone %u reads (%u B); enter ~%u reads, move ~%u reads\n",
#ifdef KV_BASELINE
	       "before",
#else
	       "cached",
#endif
	       n, p25, first, rebuild, rebuild_bytes, rows8, rows8_bytes, zone, zone_bytes, enter, move);
#ifndef KV_BASELINE
	/* Measured before the fix (v1.2.0, this fixture, -DKV_BASELINE): loading all 27 channels 4,415 reads, 8 rows 1,296;
	 * 60 channels 22,200 and 2,856. Cached (now): 510 / 116 and 2,160 / 184. The bounds keep it about a tenth */
	CHECK(rebuild < (n <= 30 ? 700u : 2600u));
	CHECK(rows8 < 260u);
#endif
}

#ifndef KV_BASELINE
/* What a lookup gives with the cache dropped: the whole chain checked as before (the reference) */
static int uncached_get(uint16_t key, uint8_t *buf, uint16_t cap)
{
	storage_untouched = false;
	return codeplug_kv_get(key, buf, cap);
}

#define KEY_A	0x3001u
#define KEY_B	0x3002u
#define ROOT	(EE + CODEPLUG_KV_ADDR)

static int get(uint16_t key, uint8_t *buf, uint16_t cap)
{
	int n = codeplug_kv_get(key, buf, cap);
	uint8_t ref[64];
	int m = uncached_get(key, ref, sizeof ref);
	CHECK(n == m && (n < 0 || memcmp(buf, ref, (unsigned)(n < cap ? n : cap)) == 0));
	(void)codeplug_kv_get(key, buf, cap);	/* the cache warm again for the next event */
	return n;
}

static uint8_t sector[STORAGE_SECTOR];

static void warm(void)
{
	uint8_t v[8];
	(void)codeplug_kv_get(KEY_A, v, sizeof v);
	uint32_t before = reads;
	(void)codeplug_kv_get(KEY_A, v, sizeof v);
	CHECK(reads - before <= 4u);	/* a hit: the root header, an entry or two, the value */
}

static void base_area(void)
{
	fresh();
	count_reads();
	uint8_t a[4] = { 1, 2, 3, 4 }, b[4] = { 5, 6, 7, 8 };
	CHECK(codeplug_kv_set(KEY_A, a, sizeof a));
	CHECK(codeplug_kv_set(KEY_B, b, sizeof b));
	warm();
}

/* Every path that writes the area: the next lookup sees the change, and an area made corrupt by it is refused */
static void test_invalidation(void)
{
	uint8_t v[8];

	/* 1. A key write (codeplug_kv_write): the new value */
	base_area();
	uint8_t a2[4] = { 9, 9, 9, 9 };
	CHECK(codeplug_kv_set(KEY_A, a2, sizeof a2));
	CHECK(get(KEY_A, v, sizeof v) == 4 && v[0] == 9);
	CHECK(codeplug_kv_remove(KEY_B) && get(KEY_B, v, sizeof v) < 0);

	/* 2. A sector commit (CPS shim, fast CPS, TFTP: codeplug_kv_commit) with a changed value */
	base_area();
	memcpy(sector, sim_flash_data + ROOT, STORAGE_SECTOR);
	uint16_t hdr = sector[5];
	CHECK(sector[hdr] == (KEY_A & 0xFF) && sector[hdr + 4] == 1);
	sector[hdr + 4] = 0x77;
	uint32_t crc = storage_crc32(0, sector + hdr, (uint16_t)(sector[6] | sector[7] << 8));
	sector[8] = (uint8_t)crc; sector[9] = (uint8_t)(crc >> 8); sector[10] = (uint8_t)(crc >> 16); sector[11] = (uint8_t)(crc >> 24);
	CHECK(codeplug_kv_commit(ROOT, sector));
	CHECK(get(KEY_A, v, sizeof v) == 4 && v[0] == 0x77);

	/* 3. A journaled sector write that leaves the area corrupt (a CPS image with a bad area: storage_sector_write) */
	base_area();
	memcpy(sector, sim_flash_data + ROOT, STORAGE_SECTOR);
	sector[sector[5] + 4] ^= 0x10;	/* the data no longer matches its CRC */
	CHECK(storage_sector_write(ROOT, sector));
	CHECK(get(KEY_A, v, sizeof v) < 0 && get(KEY_B, v, sizeof v) < 0);
	uint16_t len, entries;
	CHECK(codeplug_kv_info(&len, &entries) == CODEPLUG_KV_BAD);

	/* 4. A small record write into the area (storage_write: the codeplug's record path, its patch journal) */
	base_area();
	uint8_t x = (uint8_t)(sim_flash_data[ROOT + sim_flash_data[ROOT + 5] + 5] ^ 0x01);
	CHECK(storage_write(ROOT + sim_flash_data[ROOT + 5] + 5, &x, 1));
	CHECK(get(KEY_A, v, sizeof v) < 0);

	/* 5. A failed write (worn flash: nothing programmed): reads as the flash is */
	base_area();
	sim_flash_bad_range(ROOT, STORAGE_SECTOR);
	CHECK(!codeplug_kv_set(KEY_A, a2, sizeof a2));
	sim_flash_bad_range(0, 0);
	int n = get(KEY_A, v, sizeof v);
	CHECK(n < 0 || (n == 4 && (v[0] == 1 || v[0] == 9)));
	CHECK(get(KEY_B, v, sizeof v) == 4 || n < 0);

	/* 6. A write cut by a power loss at every point, then power on (storage_init replays the journal) */
	for (uint32_t cut = 1; cut < 9000u; cut += cut < 64u ? 1u : 61u) {
		base_area();
		sim_flash_cut_after(cut);
		(void)codeplug_kv_set(KEY_A, a2, sizeof a2);
		bool was_cut = sim_flash_is_cut();
		sim_flash_cut_after(0);
		sim_flash_power_on();
		CHECK(storage_init());
		n = get(KEY_A, v, sizeof v);
		CHECK(n == 4 && (v[0] == 1 || v[0] == 9));
		CHECK(get(KEY_B, v, sizeof v) == 4 && v[0] == 5);
		if (!was_cut)
			break;
	}

	/* 7. The area changed under the firmware without any of its writes (a CPS raw image, the stub): a reload
	 * (codeplug_reload / codeplug_written, codeplug_serial()) checks it whole */
	base_area();
	sim_flash_data[ROOT + sim_flash_data[ROOT + 5] + 5] ^= 0x02;	/* bits cleared or set: a raw change */
	(void)codeplug_reload();
	CHECK(get(KEY_A, v, sizeof v) < 0);

	/* 8. Power on (storage_init: nothing in RAM is trusted) after a raw change */
	base_area();
	sim_flash_data[ROOT + sim_flash_data[ROOT + 5] + 5] ^= 0x02;
	sim_flash_power_on();
	CHECK(storage_init());
	CHECK(get(KEY_A, v, sizeof v) < 0);

	/* 9. A raw change of the root header (its CRC or length) is seen even with no event: the header is read at every
	 * lookup and compared with the one checked */
	base_area();
	sim_flash_data[ROOT + 8] ^= 0x01;
	CHECK(codeplug_kv_get(KEY_A, v, sizeof v) < 0);
	CHECK(get(KEY_A, v, sizeof v) < 0);

	/* 11. The trust of a lookup's walk ends with it: the debug link's check and every write still check the chain
	 * whole (a raw change after a lookup, no event: codeplug_kv_info finds it) */
	base_area();
	sim_flash_data[ROOT + sim_flash_data[ROOT + 5] + 5] ^= 0x02;
	CHECK(codeplug_kv_info(&len, &entries) == CODEPLUG_KV_BAD);

	/* 10. Continuation pages (the area over one sector): a page rewritten corrupt, and a page's value changed */
	base_area();
	uint8_t big[200];
	for (unsigned i = 0; i < 40; i++) {
		memset(big, (int)i, sizeof big);
		CHECK(codeplug_kv_set((uint16_t)(0x3100u + i), big, sizeof big));
	}
	CHECK(sim_flash_data[ROOT + 12] != 0 || sim_flash_data[ROOT + 13] != 0 || sim_flash_data[ROOT + 14] != 0);
	CHECK(codeplug_kv_get(0x3127u, big, sizeof big) == 200 && big[0] == 39);
	CHECK(codeplug_kv_info(&len, &entries) == CODEPLUG_KV_VALID && entries >= 42);
	/* a value in a continuation page, changed by a key write */
	memset(big, 0xA5, sizeof big);
	CHECK(codeplug_kv_set(0x3127u, big, sizeof big));
	CHECK(codeplug_kv_get(0x3127u, big, sizeof big) == 200 && big[0] == 0xA5);
	CHECK(uncached_get(0x3127u, big, sizeof big) == 200 && big[0] == 0xA5);
	/* the page found again (the write may have moved the chain to the other bank), then corrupted by a journaled
	 * sector write: the whole area is refused, as before */
	uint32_t root_next = (uint32_t)sim_flash_data[ROOT + 12] | sim_flash_data[ROOT + 13] << 8 |
			     (uint32_t)sim_flash_data[ROOT + 14] << 16 | (uint32_t)sim_flash_data[ROOT + 15] << 24;
	CHECK(root_next >= 0xAF000u && root_next < 0xCF000u);
	uint32_t page = FL + root_next - 0x20000u;
	(void)codeplug_kv_get(KEY_A, v, sizeof v);
	memcpy(sector, sim_flash_data + page, STORAGE_SECTOR);
	sector[sector[5] + 6] ^= 0x40;
	CHECK(storage_sector_write(page, sector));
	CHECK(codeplug_kv_get(KEY_A, v, sizeof v) < 0 && get(0x3127u, big, sizeof big) < 0);
	CHECK(codeplug_kv_info(&len, &entries) == CODEPLUG_KV_BAD);
}

/* The radio-wide P25 unit ID (codeplug_p25_unit_id, cached per codeplug_kv_generation()) follows every change */
static void test_p25_unit_id(void)
{
	base_area();
	CHECK(codeplug_set_dmr_id(12345));
	CHECK(codeplug_p25_unit_id(0) == 12345 && codeplug_p25_unit_id(77) == 77);
	uint8_t own[] = { 0x32, 0x09, 1, 0 };
	CHECK(codeplug_kv_set(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_P25, own, sizeof own));
	CHECK(codeplug_p25_unit_id(0) == 0x010932u);
	uint32_t before = reads;
	CHECK(codeplug_p25_unit_id(0) == 0x010932u);
	CHECK(reads == before);		/* cached: no read at all */
	/* changed by a sector commit */
	memcpy(sector, sim_flash_data + ROOT, STORAGE_SECTOR);
	uint16_t hdr = sector[5], dlen = (uint16_t)(sector[6] | sector[7] << 8), at = 0;
	while (at < dlen && (sector[hdr + at] | sector[hdr + at + 1] << 8) != CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_P25)
		at = (uint16_t)(at + 4u + (sector[hdr + at + 2] | sector[hdr + at + 3] << 8));
	CHECK(at < dlen);
	sector[hdr + at + 4] = 0x33;
	uint32_t crc = storage_crc32(0, sector + hdr, dlen);
	sector[8] = (uint8_t)crc; sector[9] = (uint8_t)(crc >> 8); sector[10] = (uint8_t)(crc >> 16); sector[11] = (uint8_t)(crc >> 24);
	CHECK(codeplug_kv_commit(ROOT, sector));
	CHECK(codeplug_p25_unit_id(0) == 0x010933u);
	/* the DMR ID (the settings, not the area) when the key goes */
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_P25));
	CHECK(codeplug_p25_unit_id(0) == 12345);
	CHECK(codeplug_set_dmr_id(23456) && codeplug_p25_unit_id(0) == 23456);
	/* a corrupt area: no radio-wide P25 ID, the DMR ID */
	CHECK(codeplug_kv_set(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_P25, own, sizeof own));
	CHECK(codeplug_p25_unit_id(0) == 0x010932u);
	sim_flash_data[ROOT + sim_flash_data[ROOT + 5] + 5] ^= 0x02;
	sim_flash_power_on();
	CHECK(storage_init());
	CHECK(codeplug_p25_unit_id(0) == 23456);
}

/* The Channels list reads the same channels with the cache warm as with every lookup checking the whole area */
static void test_same_channels(uint16_t n, uint16_t p25)
{
	kv_populate(n, p25);
	for (uint16_t i = 1; i <= n; i++) {
		channel_t a, b;
		memset(&a, 0, sizeof a);
		memset(&b, 0, sizeof b);
		bool ok_a = codeplug_channel(i, &a);
		storage_untouched = false;
		bool ok_b = codeplug_channel(i, &b);
		CHECK(ok_a == ok_b && memcmp(&a, &b, sizeof a) == 0);
		CHECK(ok_a && (i % 2 != 0 || i / 2 > p25 || (a.mode == RADIO_MODE_P25 && a.p25_tg == 100u + i &&
							       a.p25_id == 12345)));
	}
}
#endif

int main(void)
{
	measure(27, 13);
	measure(60, 30);
#ifndef KV_BASELINE
	test_invalidation();
	test_p25_unit_id();
	test_same_channels(27, 13);
	test_same_channels(60, 30);
#endif
	printf("%s: %d failure(s)\n", __FILE__, failures);
	return failures != 0;
}
