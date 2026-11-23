/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DOTA Transfer: the receiver's import (core/radio/dota_import.c, codeplug_dota.inc; docs/dota.md 10) on
 * the real codeplug over a simulated NOR flash with power cuts. A transfer is encoded with the real record codec, cut
 * into frames, taken in a shuffled order (within the sections' barrier) and committed:
 *   - nothing shows before the commit: every channel and zone reads as before (contacts and RX group lists the
 *     transfer adds are the documented exception, and the undo takes them away);
 *   - a power cut at every flash write while staging leaves the radio's channels and zones exactly as they were;
 *   - the commit makes it all visible: new zone (name made unique), the end of the current zone, DOTA Clone's zones
 *     under the sender's names, contacts and lists matched by ID / name and members or added, receive only unless
 *     allowed, out-of-band channels left out (and their zone holes closed), P25 channels with their tags;
 *   - the undo leaves the codeplug as it was; the refusals at HELLO (room, a codeplug not stored). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/zone_edit.h"
#include "settings/settings.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"
#include "radio/dota_import.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

#define FLASH_SIZE	0x200000u
static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};
static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };
const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 3, .flash = &sim_flash };

static dota_imp_t im;

static channel_t make(const char *name, uint32_t hz, uint8_t mode)
{
	channel_t c;
	memset(&c, 0, sizeof c);
	strncpy(c.name, name, CODEPLUG_NAME_LEN);
	c.rx_hz = c.tx_hz = hz;
	c.mode = mode;
	c.bw = RADIO_BW_12K5;
	c.power = CODEPLUG_POWER_DEFAULT;
	c.color_code = 1;
	c.timeslot = 1;
	return c;
}

/* The radio as it starts each case: 5 channels, zones "Home" (1-3) and "Far" (4-5), 2 contacts, 1 list */
static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	settings_init();
	codeplug_init();
	const char *names[] = { "S20", "R0", "R1", "PMR1", "DMR rpt" };
	const uint32_t hz[] = { 145500000, 145600000, 145625000, 446006250, 438500000 };
	for (int i = 0; i < 5; i++) {
		channel_t c = make(names[i], hz[i], i == 4 ? RADIO_MODE_DMR : RADIO_MODE_FM);
		CHECK(codeplug_set_channel((uint16_t)(i + 1), &c));
	}
	zone_t z = { "Home", 3, { 1, 2, 3 } };
	CHECK(zone_edit_create(&z, NULL) == ZONE_EDIT_OK);
	zone_t f = { "Far", 2, { 4, 5 } };
	CHECK(zone_edit_create(&f, NULL) == ZONE_EDIT_OK);
	contact_t a = { "My TG 286", 286, CONTACT_GROUP, 0 }, b = { "Ali", 2860001, CONTACT_PRIVATE, 0 };
	CHECK(codeplug_set_contact(1, &a) && codeplug_set_contact(2, &b));
	listen_list_t l = { "Mine", 1, { 1 } };
	CHECK(codeplug_set_listen_list(1, &l));
}

static void boot_again(void)
{
	sim_flash_power_on();
	CHECK(storage_init());
	settings_init();
	codeplug_init();
	zone_edit_settle();
}

/* What the radio shows: every channel and zone (FNV-1a of their fields) */
static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
	const uint8_t *b = p;
	for (size_t i = 0; i < n; i++)
		h = (h ^ b[i]) * 16777619u;
	return h;
}
static uint32_t shown(bool with_contacts)
{
	uint32_t h = 2166136261u;
	channel_t c;
	zone_t z;
	for (uint16_t n = 1; n <= CODEPLUG_CHANNELS_MAX; n++) {
		if (!codeplug_channel(n, &c))
			continue;
		h = fnv(h, &n, sizeof n);
		h = fnv(h, c.name, strlen(c.name));
		h = fnv(h, &c.rx_hz, 4);
		h = fnv(h, &c.tx_hz, 4);
		h = fnv(h, &c.mode, 1);
		h = fnv(h, &c.rx_only, 1);
	}
	for (uint16_t i = 0; i < codeplug_zone_count(); i++) {
		CHECK(codeplug_zone(i, &z));
		h = fnv(h, z.name, strlen(z.name));
		h = fnv(h, z.channels, z.count * 2u);
	}
	if (with_contacts) {
		contact_t ct;
		listen_list_t l;
		for (uint16_t i = 1; i <= CODEPLUG_CONTACTS_MAX; i++)
			if (codeplug_contact(i, &ct)) {
				h = fnv(h, &i, 2);
				h = fnv(h, &ct.id, 4);
			}
		for (uint16_t i = 1; i <= CODEPLUG_LISTS_MAX; i++)
			if (codeplug_listen_list(i, &l)) {
				h = fnv(h, &i, 2);
				h = fnv(h, l.contacts, l.count * 2u);
			}
	}
	return h;
}

/* --- A transfer: its records and frames --- */
#define T_CH	40
static dota_ch_t tch[T_CH];
static contact_t tct[4];
static dota_list_t tl[2];
static dota_zone_t tz[6];
static int nch, nct, nl, nz, nzones;

typedef struct {
	uint8_t sec, count;
	uint16_t first;
	uint8_t len;
	uint8_t p[DOTA_PAYLOAD_MAX];
} frame_t;
static frame_t fr[64];
static int nfr;

static void pack(uint8_t sec, int total)
{
	int k = 0;
	while (k < total) {
		frame_t *f = &fr[nfr++];
		memset(f, 0, sizeof *f);
		f->sec = sec;
		f->first = (uint16_t)k;
		for (; k < total; k++) {
			uint8_t b[DOTA_CH_MAX + DOTA_ZONE_MAX];
			size_t n = sec == DOTA_SEC_CONTACTS ? dota_rec_put_contact(&tct[k], b, sizeof b) :
				   sec == DOTA_SEC_LISTS ? dota_rec_put_list(&tl[k], b, sizeof b) :
				   sec == DOTA_SEC_CHANNELS ? dota_rec_put_channel(&tch[k], 3, b, sizeof b) :
				   dota_rec_put_zone(&tz[k], b, sizeof b);
			CHECK(n > 0);
			if (f->len + n > 128u)
				break;
			memcpy(f->p + f->len, b, n);
			f->len = (uint8_t)(f->len + n);
			f->count++;
		}
	}
}

static void manifest(dota_manifest_t *m, uint8_t kind)
{
	memset(m, 0, sizeof *m);
	m->kind = kind;
	m->recs[DOTA_SEC_CONTACTS] = (uint16_t)nct;
	m->recs[DOTA_SEC_LISTS] = (uint16_t)nl;
	m->recs[DOTA_SEC_CHANNELS] = (uint16_t)nch;
	m->recs[DOTA_SEC_ZONES] = (uint16_t)nz;
	m->zones = (uint8_t)nzones;
}

/* The sender's set: 3 contacts (one the receiver has), a list, nch channels of every mode, 3 zones in 4 parts (DOTA
 * Clone only) */
static void transfer(int channels, bool zones)
{
	nch = channels;
	nct = 3;
	nl = 1;
	tct[0] = (contact_t){ "TG 286", 286, CONTACT_GROUP, 0 };	/* the receiver's "My TG 286": matched */
	tct[1] = (contact_t){ "TG 2862", 2862, CONTACT_GROUP, 0 };
	tct[2] = (contact_t){ "Dogan", 2862001, CONTACT_PRIVATE, 2 };
	memset(tl, 0, sizeof tl);
	strcpy(tl[0].name, "Ankara TGs");
	tl[0].count = 2;
	tl[0].id[0] = 286;
	tl[0].id[1] = 2862;
	for (int i = 0; i < nch; i++) {
		char name[17];
		snprintf(name, sizeof name, "T%02d", i);
		memset(&tch[i], 0, sizeof tch[i]);
		uint8_t mode = i % 4 == 1 ? RADIO_MODE_DMR : i % 4 == 2 ? RADIO_MODE_P25 : RADIO_MODE_FM;
		tch[i].ch = make(name, 145000000u + (uint32_t)i * 12500u, mode);
		if (i == 7)
			tch[i].ch.rx_hz = tch[i].ch.tx_hz = 50000000u;	/* 6 m: not in this radio's bands */
		if (mode == RADIO_MODE_DMR) {
			tch[i].ct_type = i % 8 == 1 ? DOTA_CT_GROUP : DOTA_CT_PRIVATE;
			tch[i].ct_id = i % 8 == 1 ? 2862 : 2862001;
			tch[i].list = 1;
		}
		if (mode == RADIO_MODE_P25) {
			tch[i].ch.p25_nac = 0x659;
			tch[i].ch.p25_tg = 101;
			tch[i].ch.p25_squelch = 1;
			tch[i].ch.rx_only = true;
		}
	}
	nz = nzones = 0;
	if (zones) {
		/* "Home" (taken here: becomes "Home 2"), a zone of 40 scattered members (two parts), a zone of the 6 m
		 * channel alone (left out: no zone) */
		memset(tz, 0, sizeof tz);
		tz[0] = (dota_zone_t){ .zone = 0, .part = 0, .total = 3, .offset = 0, .name = "Home", .runs = 1,
				       .start = { 0 }, .len = { 3 } };
		tz[1] = (dota_zone_t){ .zone = 1, .part = 0, .total = 38, .offset = 0, .name = "Odd ones", .runs = 32 };
		for (int r = 0; r < 32; r++) {
			tz[1].start[r] = (uint16_t)(r < 19 ? 2 * r + 1 : 0);
			tz[1].len[r] = 1;
		}
		tz[1].runs = 19;
		tz[1].total = 38;
		tz[2] = (dota_zone_t){ .zone = 1, .part = 1, .total = 38, .offset = 19, .runs = 19 };
		for (int r = 0; r < 19; r++) {
			tz[2].start[r] = (uint16_t)(2 * r);
			tz[2].len[r] = 1;
		}
		tz[3] = (dota_zone_t){ .zone = 2, .part = 0, .total = 1, .offset = 0, .name = "Six", .runs = 1,
				       .start = { 7 }, .len = { 1 } };
		nz = 4;
		nzones = 3;
	}
	nfr = 0;
	pack(DOTA_SEC_CONTACTS, nct);
	pack(DOTA_SEC_LISTS, nl);
	pack(DOTA_SEC_CHANNELS, nch);
	if (zones)
		pack(DOTA_SEC_ZONES, nz);
}

/* Each section's frames in a random order, the sections in theirs (the protocol's barrier) */
static uint32_t rng = 7;
static bool expect_frames = true;	/* false while the flash is cut: writes fail */
static void stage_all(void)
{
	int order[64];
	for (int i = 0; i < nfr; i++)
		order[i] = i;
	for (int i = nfr - 1; i > 0; i--) {
		rng = rng * 1103515245u + 12345u;
		int j = (int)((rng >> 8) % (uint32_t)(i + 1));
		if (fr[order[i]].sec == fr[order[j]].sec) {
			int t = order[i];
			order[i] = order[j];
			order[j] = t;
		}
	}
	for (int i = 0; i < nfr; i++) {
		frame_t *f = &fr[order[i]];
		bool ok = dota_imp_frame(&im, f->sec, f->first, f->count, f->p, f->len);
		CHECK(ok || !expect_frames);
	}
}

static int find_zone(const char *name, zone_t *out)
{
	for (uint16_t i = 0; i < codeplug_zone_count(); i++)
		if (codeplug_zone(i, out) && strcmp(out->name, name) == 0)
			return i;
	return -1;
}

static void test_new_zone(void)
{
	fresh();
	transfer(T_CH, false);
	dota_manifest_t m;
	manifest(&m, DOTA_KIND_SELECTED);
	dota_imp_cfg_t cfg = { .mode = DOTA_IMP_NEW, .tx_ok = false, .name = "Far" };	/* taken: "Far 2" */
	uint16_t fc;
	uint8_t fz, room;
	/* more than a zone holds: refused */
	CHECK(dota_imp_begin(&im, &cfg, &m, 3, &fc, &fz, &room) == DOTA_R_ZONE_FULL || nch <= 80);
	uint32_t before = shown(false), before_all = shown(true);
	CHECK(dota_imp_begin(&im, &cfg, &m, 3, &fc, &fz, &room) == DOTA_R_OK);
	CHECK(fc == codeplug_free_channels() && fc >= CODEPLUG_CHANNELS_MAX - 6 && room == CODEPLUG_ZONE_MAX);
	stage_all();
	CHECK(shown(false) == before);	/* staged: nothing shows */
	CHECK(im.added == T_CH - 1 && im.skipped == 1 && im.ct_added == 2 && im.ct_matched == 1 && im.lists_added == 1);
	/* the undo: as before, contacts and lists too */
	dota_imp_undo(&im);
	CHECK(shown(true) == before_all);
	/* again, committed */
#if CONFIG_CHAN_EXTRAS
	zone_t all;
	uint16_t all0 = codeplug_zone(0, &all) ? all.count : 0;
#endif
	CHECK(dota_imp_begin(&im, &cfg, &m, 3, &fc, &fz, &room) == DOTA_R_OK);
	stage_all();
	CHECK(dota_imp_commit(&im));
	CHECK(im.zones_added == 1);
	zone_t z;
	int zi = find_zone("Far 2", &z);
	CHECK(zi >= 0 && z.count == T_CH - 1);
#if CONFIG_CHAN_EXTRAS
	/* the new zone's channels are in All channels (zone 0) too: Home on it steps through them */
	CHECK(all0 > 0 && codeplug_zone(0, &all) && strcmp(all.name, "All channels") == 0 && all.count == all0 + T_CH - 1);
	for (int i = 0; i < z.count && zi >= 0; i++)
		CHECK(zone_list_find(&all, z.channels[i]) >= 0);
#endif
	/* the channels in order, receive only, the DMR ones with their contact and list, P25 with its tag */
	contact_t ct;
	int k = 0;
	for (int i = 0; i < T_CH && zi >= 0; i++) {
		if (i == 7)
			continue;
		channel_t c;
		CHECK(codeplug_channel(z.channels[k], &c));
		CHECK(strcmp(c.name, tch[i].ch.name) == 0 && c.rx_hz == tch[i].ch.rx_hz && c.mode == tch[i].ch.mode);
		CHECK(c.rx_only);
		if (c.mode == RADIO_MODE_DMR) {
			CHECK(c.contact != 0 && codeplug_contact(c.contact, &ct) && ct.id == tch[i].ct_id);
			listen_list_t l;
			CHECK(c.listen_list != 0 && codeplug_listen_list(c.listen_list, &l) && strcmp(l.name, "Ankara TGs") == 0 &&
			      l.count == 2);
		}
		if (c.mode == RADIO_MODE_P25)
			CHECK(c.p25_nac == 0x659 && c.p25_tg == 101);
		k++;
	}
	/* the matched contact kept its name; the receiver's own zones untouched */
	CHECK(codeplug_contact(1, &ct) && strcmp(ct.name, "My TG 286") == 0);
	CHECK(find_zone("Home", &z) >= 0 && z.count == 3 && find_zone("Far", &z) >= 0 && z.count == 2);
	CHECK(sim_flash_violations() == 0);
	/* Transmit allowed: the sender's receive-only bit kept */
	fresh();
	cfg.tx_ok = true;
	strcpy(cfg.name, "Tx ok");
	CHECK(dota_imp_begin(&im, &cfg, &m, 3, &fc, &fz, &room) == DOTA_R_OK);
	stage_all();
	CHECK(dota_imp_commit(&im) && find_zone("Tx ok", &z) >= 0);
	channel_t c;
	CHECK(codeplug_channel(z.channels[0], &c) && !c.rx_only);
	CHECK(codeplug_channel(z.channels[2], &c) && c.mode == RADIO_MODE_P25 && c.rx_only);
}

static void test_add_and_refusals(void)
{
	fresh();
	transfer(10, false);
	dota_manifest_t m;
	manifest(&m, DOTA_KIND_ZONE);
	zone_t z;
	dota_imp_cfg_t cfg = { .mode = DOTA_IMP_ADD, .zone = (uint16_t)find_zone("Home", &z) };
	uint16_t fc;
	uint8_t fz, room;
	CHECK(dota_imp_begin(&im, &cfg, &m, 3, &fc, &fz, &room) == DOTA_R_OK && room == CODEPLUG_ZONE_MAX - 3);
	stage_all();
	CHECK(dota_imp_commit(&im));
	CHECK(find_zone("Home", &z) == (int)cfg.zone && z.count == 3 + 9);
	CHECK(z.channels[0] == 1 && z.channels[2] == 3);
	/* the zone's room: 12 + 70 > 80 */
	transfer(T_CH, false);
	manifest(&m, DOTA_KIND_SELECTED);
	m.recs[DOTA_SEC_CHANNELS] = 70;
	CHECK(dota_imp_begin(&im, &cfg, &m, 3, &fc, &fz, &room) == DOTA_R_ZONE_FULL && room == 80 - 12);
	/* no room for the channels, a new zone without a name, clone without free zones */
	m.recs[DOTA_SEC_CHANNELS] = (uint16_t)(fc + 1u);
	cfg.mode = DOTA_IMP_NEW;
	strcpy(cfg.name, "X");
	CHECK(dota_imp_begin(&im, &cfg, &m, 3, &fc, &fz, &room) == DOTA_R_CHANNELS);
	m.recs[DOTA_SEC_CHANNELS] = 5;
	cfg.name[0] = '\0';
	CHECK(dota_imp_begin(&im, &cfg, &m, 3, &fc, &fz, &room) == DOTA_R_ZONE_FULL);
	manifest(&m, DOTA_KIND_CLONE);
	m.recs[DOTA_SEC_ZONES] = 1;
	m.zones = (uint8_t)(fz + 1u);
	CHECK(dota_imp_begin(&im, &cfg, &m, 3, &fc, &fz, &room) == DOTA_R_ZONES);
	m.zones = 1;
	m.recs[DOTA_SEC_CONTACTS] = 2000;
	CHECK(dota_imp_begin(&im, &cfg, &m, 3, &fc, &fz, &room) == DOTA_R_CONTACTS);
	m.recs[DOTA_SEC_CONTACTS] = 0;
	m.recs[DOTA_SEC_LISTS] = 80;
	CHECK(dota_imp_begin(&im, &cfg, &m, 3, &fc, &fz, &room) == DOTA_R_LISTS);
	/* a radio without a stored codeplug: not ready */
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	settings_init();
	codeplug_init();
	manifest(&m, DOTA_KIND_SELECTED);
	strcpy(cfg.name, "X");
	CHECK(dota_imp_begin(&im, &cfg, &m, 3, &fc, &fz, &room) == DOTA_R_NOT_READY);
}

static void test_clone(void)
{
	fresh();
	transfer(T_CH, true);
	dota_manifest_t m;
	manifest(&m, DOTA_KIND_CLONE);
	dota_imp_cfg_t cfg = { 0 };
	uint16_t fc;
	uint8_t fz, room;
	uint32_t before = shown(false);
	CHECK(dota_imp_begin(&im, &cfg, &m, 3, &fc, &fz, &room) == DOTA_R_OK);
	stage_all();
	uint16_t zones0 = codeplug_zone_count();
	CHECK(shown(false) == before && find_zone("Home 2", &im.z) < 0);
	CHECK(dota_imp_commit(&im));
	CHECK(im.zones_added == 2 && codeplug_zone_count() == zones0 + 2);	/* "Six" had only the 6 m channel: none */
	zone_t z;
	CHECK(find_zone("Home 2", &z) >= 0 && z.count == 3);
	CHECK(find_zone("Odd ones", &z) >= 0 && z.count == 37);	/* 38 members, the 6 m one's hole closed */
	CHECK(find_zone("Six", &z) < 0 && find_zone("Home", &z) >= 0 && z.count == 3);
	/* a power cut, then on again: as committed */
	boot_again();
	CHECK(codeplug_zone_count() == zones0 + 2 && find_zone("Odd ones", &z) >= 0 && z.count == 37);
	CHECK(sim_flash_violations() == 0);
}

/* A power cut at every flash write while staging: on again, the radio's channels and zones are as before */
static void test_power_cuts(void)
{
	fresh();
	transfer(12, true);
	dota_manifest_t m;
	manifest(&m, DOTA_KIND_CLONE);
	dota_imp_cfg_t cfg = { 0 };
	uint16_t fc;
	uint8_t fz, room;
	uint32_t before = shown(false);
	static uint8_t snapshot[FLASH_SIZE];
	memcpy(snapshot, sim_flash_data, FLASH_SIZE);
	CHECK(dota_imp_begin(&im, &cfg, &m, 3, &fc, &fz, &room) == DOTA_R_OK);
	uint32_t start = sim_flash_units();
	stage_all();
	uint32_t units = sim_flash_units() - start;
	int cuts = 0;
	for (uint32_t cut = 0; cut < units; cut += 1 + units / 60u) {
		memcpy(sim_flash_data, snapshot, FLASH_SIZE);
		boot_again();
		CHECK(dota_imp_begin(&im, &cfg, &m, 3, &fc, &fz, &room) == DOTA_R_OK);
		sim_flash_cut_after(cut);
		expect_frames = false;
		stage_all();
		expect_frames = true;
		boot_again();
		CHECK(shown(false) == before);
		cuts++;
	}
	/* ... and after a cut the import can run again from the start and commit */
	memcpy(sim_flash_data, snapshot, FLASH_SIZE);
	boot_again();
	CHECK(dota_imp_begin(&im, &cfg, &m, 3, &fc, &fz, &room) == DOTA_R_OK);
	stage_all();
	printf("  power cuts while staging: %d points of %u flash units, channels and zones unchanged\n", cuts, units);
	static uint8_t staged[FLASH_SIZE];
	memcpy(staged, sim_flash_data, FLASH_SIZE);
	dota_imp_t keep = im;
	boot_again();
	uint16_t zones0 = codeplug_zone_count();
	start = sim_flash_units();
	CHECK(dota_imp_commit(&im) && codeplug_zone_count() == zones0 + 2);
	uint32_t after = shown(false);
	CHECK(sim_flash_violations() == 0);
	/* A cut during the commit (the bitmaps, then the zones): on again, the radio's own channels and zones whole, and
	 * the import either not seen, all seen, or part way (the documented window: some channels, not all zones) */
	units = sim_flash_units() - start;
	int commit_cuts = 0, none = 0, all = 0;
	for (uint32_t cut = 1; cut <= units; cut += cut < 24u ? 1u : 1u + units / 40u) {	/* (0 is no cut) */
		memcpy(sim_flash_data, staged, FLASH_SIZE);
		boot_again();
		im = keep;
		sim_flash_cut_after(cut);
		(void)dota_imp_commit(&im);
		boot_again();
		uint32_t h = shown(false);
		none += h == before;
		all += h == after;
		zone_t z;
		for (uint16_t i = 0; i < codeplug_zone_count(); i++)
			CHECK(codeplug_zone(i, &z) && z.count <= CODEPLUG_ZONE_MAX);
		CHECK(find_zone("Home", &z) >= 0 && z.count == 3 && find_zone("Far", &z) >= 0 && z.count == 2);
		commit_cuts++;
	}
	CHECK(none > 0 && sim_flash_violations() == 0);
	printf("  power cuts while committing: %d points (%d before it showed, %d all of it, the rest part way)\n",
	       commit_cuts, none, all);
}

int main(void)
{
	test_new_zone();
	test_add_and_refusals();
	test_clone();
	test_power_cuts();
	printf("dota import: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
