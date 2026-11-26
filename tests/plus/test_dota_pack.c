/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DOTA Transfer of a user data pack (docs/dota.md 20): two whole sessions (core/radio/dota.c twice, built
 * with CONFIG_DATAPACK, as a_* the sender and b_* the receiver; tools/test_dota.py renames their entry points) over
 * the fake FM and DMR transports of test_dota_session.c, on the real protocol and the real pack store
 * (core/storage/datapack.c) over a simulated 16 MiB data chip. Both radios share the chip here: the sender reads the
 * pack in force, the receiver writes the other slot and, at the commit, makes it the one in force (a self transfer:
 * the same payload, the next sequence). Cases:
 *   - a whole small pack (an air band pack and a RadioID pack) over FM and over DMR, with loss; the receiver's user
 *     asked "Data pack: ...? " first and answering Yes after a few seconds; a pack of more frames than the first
 *     builds' 512 over DMR;
 *   - the user's No: the sender is refused, nothing is erased or written; no answer at all: the sender gives up after
 *     its HELLOs (~20 s), the question goes;
 *   - cuts part way: the sender stopping, the link going silent, a power cut on the receiver's flash: the pack in force
 *     stays, byte for byte, and the slot written holds no header;
 *   - a receiver built without data packs (c_*: dota.c without CONFIG_DATAPACK) answers "Other version" and writes
 *     nothing. (Builds from before this type drop the 85-byte HELLO, test_dota_proto.c: the sender's "No answer".) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage/storage.h"
#include "storage/datapack.h"
#include "storage/ref_pack.h"
#include "sim/sim_flash.h"
#include "aprs/hdlc.h"
#include "radio/dota.h"
#include "radio/beep.h"
#include "radio/radio.h"
#include "dmr/dmrDataService.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* The copies of dota.c (c_: without CONFIG_DATAPACK) */
#define SIDE(p)                                                                                                       \
	bool p##dota_start(const dota_req_t *r);                                                                     \
	void p##dota_stop(void);                                                                                      \
	void p##dota_view(dota_view_t *v);                                                                            \
	bool p##dota_job_wanted(void);                                                                                \
	void p##dota_job(void *ctx);                                                                                  \
	bool p##dota_begin(dota_req_t *r, uint32_t now);                                                              \
	void p##dota_poll(const dota_io_t *io, uint32_t now);                                                         \
	bool p##dota_owns_radio(void);                                                                                \
	void p##dota_fm_frame(const uint8_t *frame, size_t n);                                                       \
	void p##dota_dmr_rx(uint32_t src, const uint8_t *payload, int length);
SIDE(a_)
SIDE(b_)
SIDE(c_)
bool b_dota_pack_offer(dpk_hdr_t *h);
void b_dota_pack_answer(bool yes);

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}
void radio_req_dota(bool on) { (void)on; }
const char *tr(const char *en) { return en; }

/* --- Fake transports (as test_dota_session.c) --- */
static int side;			/* 0: the sender polls, 1: the receiver */
static bool old_rx;			/* the receiver is c_ (no data packs) */
static bool keyed[2];
static uint32_t loss, frames_out, frames_lost;
static uint32_t rng = 1;
static bool lose(void)
{
	rng = rng * 1103515245u + 12345u;
	return (rng >> 8) % 100u < loss;
}
static hdlc_dec_t dec[2];
static void deliver_fm(const uint8_t *f, size_t n)
{
	frames_out++;
	if (lose()) {
		frames_lost++;
		return;
	}
	if (side == 0)
		old_rx ? c_dota_fm_frame(f, n) : b_dota_fm_frame(f, n);
	else
		a_dota_fm_frame(f, n);
}
static void play(const uint8_t *t, uint16_t n)
{
	for (uint16_t i = 0; i < n; i++) {
		size_t len = hdlc_dec_bit(&dec[side], hdlc_tone_bit(t, i));
		if (len)
			deliver_fm(dec[side].buf, len);
	}
}
static bool f_start(const uint8_t *t, uint16_t n, uint16_t baud, uint16_t m, uint16_t s, uint8_t l)
{
	(void)baud, (void)m, (void)s, (void)l;
	CHECK(keyed[side]);
	play(t, n);
	return true;
}
static bool f_more(const uint8_t *t, uint16_t n) { play(t, n); return true; }
static bool f_no(void) { return false; }
static void f_stop(void) {}
static void f_monitor(bool on) { (void)on; }
static void f_click(void) {}
static const hal_afsk_t fake_afsk = { .tx_start = f_start, .tx_busy = f_no, .tx_stop = f_stop, .tx_more = f_more,
				      .tx_queued = f_no, .monitor = f_monitor, .click = f_click };

#define ID_A 2862001u
#define ID_B 2862002u
int dmrDataServiceSendIPChain(bool group, uint32_t dst, uint32_t srcIp, uint32_t dstIp, uint8_t ttl,
			      const dmrDataChainPacket_t *p, int count)
{
	(void)srcIp, (void)dstIp, (void)ttl;
	CHECK(!group && dst == (side == 0 ? ID_B : ID_A));
		for (int i = 0; i < count; i++) {
		frames_out++;
		if (lose()) {
			frames_lost++;
			continue;
		}
		if (side == 0)
			old_rx ? c_dota_dmr_rx(ID_A, p[i].l4 + 8, p[i].length - 8) : b_dota_dmr_rx(ID_A, p[i].l4 + 8, p[i].length - 8);
		else
			a_dota_dmr_rx(ID_B, p[i].l4 + 8, p[i].length - 8);
	}
	return count;
}
bool dmrDataServiceIpBusy(void) { return false; }
static uint32_t blocks;
uint32_t dmrDataServiceRxBlockCount(void) { return blocks++; }

/* The data chip: 16 MiB, the codeplug's areas as the DM-32's (the session never touches them for a pack) */
#define FLASH_SIZE	0x1000000u
static const hal_flash_map_t map = {
	.settings_addr = 0x711000u, .settings_size = 0x8000u, .journal_addr = 0x719000u,
	.openfw_eeprom = 0x700000u, .openfw_flash = 0x720000u, .openfw_end = 0xF00000u,
	.calibration_addr = 0x710000u, .calibration_size = 0x200u,
};
static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };
const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 3, .flash = &sim_flash,
			.afsk = &fake_afsk };

static bool key_a(bool on) { keyed[0] = on; return true; }
static bool key_b(bool on) { keyed[1] = on; return true; }
static void beep_none(uint8_t b) { (void)b; }

/* --- Packs --- */
static uint8_t payload[0x10000 + 4096];
static uint32_t payload_len;

static void make_payload(uint32_t len, uint8_t seed)
{
	payload_len = len;
	for (uint32_t i = 0; i < len; i++)
		payload[i] = (uint8_t)(i * 31u + (i >> 8) * 7u + seed);
}

/* The pack in force before the transfer: written as the radio writes one */
static void install(uint8_t type, const char *name)
{
	dpk_hdr_t h;
	dpk_writer_t w;
	memset(&h, 0, sizeof h);
	h.type = type;
	h.fmt = 1;
	h.len = payload_len;
	h.crc = storage_crc32(0, payload, payload_len);
	memcpy(h.date, "20261117", 8);
	strncpy(h.name, name, sizeof h.name);
	strncpy(h.tag, "test", sizeof h.tag);
	CHECK(dpk_begin(&w, &h));
	for (uint32_t off = 0; off < payload_len; off += 1000u)
		CHECK(dpk_put(&w, off, payload + off, payload_len - off < 1000u ? payload_len - off : 1000u));
	CHECK(dpk_commit(&w));
}

static void fresh(uint8_t type, uint32_t len, const char *name)
{
	sim_flash_create(FLASH_SIZE, &map);
	dpk_scan();
	make_payload(len, (uint8_t)type);
	install(type, name);
	CHECK(dpk_active(type) != NULL && dpk_active(type)->seq == 1u);
}

/* The pack in force: the payload made, at sequence seq; the other slot without a header (nothing half-written
 * counts) when it was not written whole */
static bool in_force(uint8_t type, uint32_t seq)
{
	const dpk_info_t *p = dpk_active(type);
	static uint8_t back[sizeof payload];
	return p != NULL && p->seq == seq && p->len == payload_len && dpk_read(type, 0, back, payload_len) &&
	       memcmp(back, payload, payload_len) == 0;
}
static bool other_slot_empty(uint8_t type)
{
	const uint8_t *h = sim_flash_data + dpk_slot_addr(type, 1);	/* slot 0 holds the pack installed first */
	for (unsigned i = 0; i < DPK_HDR; i++)
		if (h[i] != 0xFFu)
			return false;
	return true;
}

/* --- A session --- */
typedef struct {
	uint8_t method, type;
	uint32_t loss;
	int answer;			/* the receiver's user: 1 Yes, 0 No, -1 never */
	uint32_t answer_after_ms;	/* the question is up this long first */
	uint32_t stop_at_ms;		/* the sender's user stops then (0: never) */
	uint32_t silent_at_ms;		/* the link loses everything from then on (0: never) */
	uint32_t cut_after;		/* a power cut on the chip after this many flash units of the receiver's (0: none) */
	bool old;			/* the receiver is c_ */
	uint32_t limit_ms;
} case_t;

static dota_view_t va, vb;
static uint32_t now, asked_ms, answered_at, sent_ms;
static bool asked, offer_ok;
static char offer_text[48];

static void run(const case_t *c)
{
	memset(keyed, 0, sizeof keyed);
	memset(dec, 0, sizeof dec);
	hdlc_dec_init(&dec[0]);
	hdlc_dec_init(&dec[1]);
	loss = c->loss;
	old_rx = c->old;
	frames_out = frames_lost = 0;
	asked = offer_ok = false;
	asked_ms = answered_at = sent_ms = 0;
	offer_text[0] = '\0';
	dota_req_t ra = { .role = DOTA_SEND, .method = c->method, .hz = 433450000, .ctcss = 885, .cc = 1, .ts = 1,
			  .peer_id = ID_B, .power = 0, .code = 4271, .call = "TA1AAA", .kind = DOTA_KIND_PACK,
			  .pack = c->type };
	dota_req_t rb = { .role = DOTA_RECEIVE, .method = c->method, .hz = 433450000, .ctcss = 885, .cc = 1, .ts = 1,
			  .code = 4271, .call = "TA1BBB" };
	rb.imp.mode = DOTA_IMP_ADD;	/* a pack goes nowhere in the codeplug: the receiver's import choice is moot */
	dota_req_t got;
	if (c->old)
		CHECK(c_dota_start(&rb) && c_dota_begin(&got, now));
	else
		CHECK(b_dota_start(&rb) && b_dota_begin(&got, now));
	CHECK(a_dota_start(&ra) && a_dota_begin(&got, now));
	bool a_on = true, b_on = true, cut = false;
	uint32_t t0 = now, units0 = 0;
	for (uint32_t t = 0; t < c->limit_ms && (a_on || b_on); t += 25, now += 25) {
		if (c->stop_at_ms && t == c->stop_at_ms)
			a_dota_stop();
		if (c->silent_at_ms && t == c->silent_at_ms)
			loss = 100;
		dota_io_t ia = { .key = key_a, .beep = beep_none, .clear = !keyed[1], .tone_ok = true, .dmr_ready = true };
		dota_io_t ib = { .key = key_b, .beep = beep_none, .clear = !keyed[0], .tone_ok = true, .dmr_ready = true };
		side = 0;
		if (a_on)
			a_dota_poll(&ia, now);
		side = 1;
		if (c->old) {
			if (b_on)
				c_dota_poll(&ib, now);
			while (c_dota_job_wanted())
				c_dota_job(NULL);
			b_on = c_dota_owns_radio();
			c_dota_view(&vb);
		} else {
			if (b_on)
				b_dota_poll(&ib, now);
			if (b_dota_job_wanted()) {
				if (c->cut_after && !cut && vb.phase == DOTA_P_RUN) {
					units0 = sim_flash_units();
					sim_flash_cut_after(c->cut_after);	/* the receiver's writes from here on */
					cut = true;
				}
				b_dota_job(NULL);	/* the UI posts the storage job */
			}
			b_on = b_dota_owns_radio();
			b_dota_view(&vb);
			/* the receiver's user: the question, then the answer */
			if (vb.asking && !asked) {
				dpk_hdr_t h;
				asked = true;
				asked_ms = now - t0;
				offer_ok = b_dota_pack_offer(&h);
				dpk_describe(&h, offer_text, sizeof offer_text);
			}
			if (asked && answered_at == 0 && c->answer >= 0 && now - t0 >= asked_ms + c->answer_after_ms) {
				b_dota_pack_answer(c->answer == 1);
				answered_at = now - t0;
			}
		}
		a_on = a_dota_owns_radio();
		a_dota_view(&va);
		if (!a_on && sent_ms == 0)
			sent_ms = now - t0;
		if (!a_on && t > 40000u && b_on && vb.phase == DOTA_P_WAIT)
			break;	/* the receiver stays armed until its user leaves */
	}
	if (cut) {
		printf("    power cut after %u flash units of the receiver's\n", sim_flash_units() - units0);
		sim_flash_power_on();
	}
	if (c->old) {
		if (b_on)
			c_dota_stop();
	} else if (b_on) {
		b_dota_stop();
	}
	for (int k = 0; k < 400; k++, now += 25) {
		dota_io_t ib = { .key = key_b, .beep = beep_none, .clear = true, .tone_ok = false, .dmr_ready = true };
		side = 1;
		if (c->old ? !c_dota_owns_radio() : !b_dota_owns_radio())
			break;
		c->old ? c_dota_poll(&ib, now) : b_dota_poll(&ib, now);
	}
	if (!c->old)
		while (b_dota_job_wanted())
			b_dota_job(NULL);
	a_dota_view(&va);
	c->old ? c_dota_view(&vb) : b_dota_view(&vb);
	dpk_scan();	/* as the next power on does */
}

static const char *const methods[] = { "FM", "DMR" };

/* A whole small pack, either type, either method: asked, Yes, the next sequence in force with the same bytes */
static void test_whole(uint8_t method, uint8_t type, uint32_t len, uint32_t lossy)
{
	fresh(type, len, type == DPK_AIR ? "TR aerodromes" : "RadioID TR");
	case_t c = { .method = method, .type = type, .loss = lossy, .answer = 1, .answer_after_ms = 3000,
		     .limit_ms = 900000 };
	run(&c);
	CHECK(asked && offer_ok);
	const char *says = type == DPK_AIR ? "TR aerodromes, 2026-11-17, " : "RadioID TR, 2026-11-17, ";
	CHECK(strncmp(offer_text, says, strlen(says)) == 0);
	CHECK(va.result == DOTA_RES_SENT && va.pack && vb.result == DOTA_RES_DONE && vb.pack);
	CHECK(va.ch_added == 0 && vb.ch_added == 0 && vb.zones_added == 0);
	CHECK(in_force(type, 2));	/* the copy, in the other slot, one sequence on */
	printf("  %s %s pack of %u B, %u %% loss: asked at %u.%u s (\"%s\"), done in %u s, %u frames, %u lost\n",
	       methods[method], type == DPK_AIR ? "air band" : "RadioID", len, lossy, asked_ms / 1000u,
	       asked_ms % 1000u / 100u, offer_text, sent_ms / 1000u, frames_out, frames_lost);
}

/* The receiver's user says No: refused, nothing erased; never answers: the sender gives up (~20 s), the question
 * goes */
static void test_refused(uint8_t method)
{
	fresh(DPK_AIR, 3000, "TR aerodromes");
	uint32_t erases = sim_flash_erases(dpk_slot_addr(DPK_AIR, 1));
	case_t c = { .method = method, .type = DPK_AIR, .answer = 0, .answer_after_ms = 2000, .limit_ms = 120000 };
	run(&c);
	CHECK(asked && va.result == DOTA_RES_REFUSED && va.why == DOTA_R_DECLINED && vb.result != DOTA_RES_DONE);
	CHECK(in_force(DPK_AIR, 1) && other_slot_empty(DPK_AIR) && sim_flash_erases(dpk_slot_addr(DPK_AIR, 1)) == erases);
	c.answer = -1;
	run(&c);
	CHECK(asked && va.result == DOTA_RES_REFUSED && va.why == DOTA_R_WAIT && !vb.asking);
	CHECK(in_force(DPK_AIR, 1) && sim_flash_erases(dpk_slot_addr(DPK_AIR, 1)) == erases);
	CHECK(sent_ms >= 12000u && sent_ms <= 30000u);	/* no air time here: ~20-25 s on the air (test_dota_proto.c) */
	printf("  %s: No refused at once; no answer: the sender gave up after %u s, nothing erased\n", methods[method],
	       sent_ms / 1000u);
}

/* Cut part way: the pack in force stays, byte for byte; the slot written has no header */
static void test_cuts(void)
{
	/* the sender stops */
	fresh(DPK_RADIOID, 60000, "RadioID TR");
	case_t c = { .method = DOTA_DMR, .type = DPK_RADIOID, .answer = 1, .answer_after_ms = 1000, .stop_at_ms = 12000,
		     .limit_ms = 300000 };
	run(&c);
	CHECK(va.result == DOTA_RES_STOPPED && vb.result != DOTA_RES_DONE);
	CHECK(in_force(DPK_RADIOID, 1) && other_slot_empty(DPK_RADIOID));
	/* the link goes silent: the receiver gives up after its idle time */
	fresh(DPK_RADIOID, 60000, "RadioID TR");
	case_t s = { .method = DOTA_FM, .type = DPK_RADIOID, .answer = 1, .answer_after_ms = 1000, .silent_at_ms = 15000,
		     .limit_ms = 300000 };
	run(&s);
	CHECK(va.result != DOTA_RES_SENT && vb.result == DOTA_RES_INCOMPLETE);
	CHECK(in_force(DPK_RADIOID, 1) && other_slot_empty(DPK_RADIOID));
	/* a power cut while the receiver writes (the chip is shared here: the sender's reads fail too) */
	static const uint32_t cuts[] = { 1, 5000, 9000, 15000 };
	for (unsigned i = 0; i < sizeof cuts / sizeof cuts[0]; i++) {
		fresh(DPK_RADIOID, 20000, "RadioID TR");
		case_t p = { .method = DOTA_DMR, .type = DPK_RADIOID, .answer = 1, .answer_after_ms = 1000,
			     .cut_after = cuts[i], .limit_ms = 300000 };
		run(&p);
		CHECK(vb.result != DOTA_RES_DONE && va.result != DOTA_RES_SENT);
		CHECK(in_force(DPK_RADIOID, 1));
	}
	printf("  cut part way (sender's stop, silent link, power cuts): the pack in force stayed\n");
}

/* A receiver built without data packs: "Other version", nothing written */
static void test_old_receiver(void)
{
	fresh(DPK_AIR, 2000, "TR aerodromes");
	case_t c = { .method = DOTA_FM, .type = DPK_AIR, .answer = 1, .old = true, .limit_ms = 60000 };
	run(&c);
	CHECK(va.result == DOTA_RES_REFUSED && va.why == DOTA_R_VERSION);
	CHECK(in_force(DPK_AIR, 1) && other_slot_empty(DPK_AIR));
}

/* --- The map pack (type 3, firmware 1.1; docs/mappack-design.md E.4, T5b, T5e, T11) --- */

/* A map pack payload of 16 bytes: a directory without sections (valid: every feature off), or one that says it has a
 * section with no room for its entry (structurally bad, CRC-valid) */
static void ref_payload(bool bad)
{
	uint32_t coord = ref_coord_id();
	memset(payload, 0, 64);
	memcpy(payload, "REF1", 4);
	payload[4] = 1;
	payload[5] = bad ? 1 : 0;
	memcpy(payload + 8, &coord, 4);
	payload_len = 16;	/* bad: one section said, no room for its entry (the same length: the sender's view of it) */
}

/* The header of the pack in force rewritten in place (no rescan: the sender still sends it), its CRCs made good */
static void rewrite_in_force(uint16_t flags, bool new_payload)
{
	uint32_t a = dpk_active_addr(DPK_REF);
	dpk_hdr_t h;
	memcpy(&h, sim_flash_data + a, sizeof h);
	h.flags = flags;
	if (new_payload) {
		memcpy(sim_flash_data + a + DPK_HDR, payload, payload_len);
		h.len = payload_len;
		h.crc = storage_crc32(0, payload, payload_len);
	}
	dpk_seal(&h);
	memcpy(sim_flash_data + a, &h, sizeof h);
}

static void test_map_pack(void)
{
	/* T11: a map pack goes whole, the receiver's copy in force one sequence on */
	sim_flash_create(FLASH_SIZE, &map);
	dpk_scan();
	ref_payload(false);
	install(DPK_REF, "DDM32 TR test");
	CHECK(dpk_active(DPK_REF) != NULL);
	case_t c = { .method = DOTA_FM, .type = DPK_REF, .answer = 1, .answer_after_ms = 1000, .limit_ms = 120000 };
	run(&c);
	CHECK(asked && va.result == DOTA_RES_SENT && vb.result == DOTA_RES_DONE && in_force(DPK_REF, 2) && !vb.pack_bad);
	/* T5e: a hint for another version: refused at the HELLO, the receiver says why, nothing erased */
	sim_flash_create(FLASH_SIZE, &map);
	dpk_scan();
	ref_payload(false);
	install(DPK_REF, "DDM32 TR test");
	rewrite_in_force((uint16_t)(ref_hint() ^ 0x0100u), false);
	uint32_t erases = sim_flash_erases(dpk_slot_addr(DPK_REF, 1));
	run(&c);
	CHECK(!asked && va.result == DOTA_RES_REFUSED && va.why == DOTA_R_VERSION && vb.pack_other);
	CHECK(other_slot_empty(DPK_REF) && sim_flash_erases(dpk_slot_addr(DPK_REF, 1)) == erases);
	/* T5b over DOTA: CRC-valid but structurally bad: never committed (its header sector stays erased), "Bad pack" */
	sim_flash_create(FLASH_SIZE, &map);
	dpk_scan();
	ref_payload(false);
	install(DPK_REF, "DDM32 TR test");
	ref_payload(true);
	rewrite_in_force(0, true);
	run(&c);
	CHECK(asked && vb.result != DOTA_RES_DONE && vb.pack_bad && va.result != DOTA_RES_SENT);
	if (!(asked && vb.pack_bad))
		printf("    (asked %d, receiver result %u why %u, pack_bad %d, sender result %u why %u)\n", asked, vb.result,
		       vb.why, vb.pack_bad, va.result, va.why);
	CHECK(other_slot_empty(DPK_REF));
	printf("  map pack: sent whole; another version refused at the HELLO; a bad structure never committed\n");
}

int main(void)
{
	now = 1000;
	test_map_pack();
	test_whole(DOTA_FM, DPK_AIR, 2500, 0);
	test_whole(DOTA_DMR, DPK_AIR, 2500, 0);
	test_whole(DOTA_FM, DPK_RADIOID, 9008, 10);
	test_whole(DOTA_DMR, DPK_RADIOID, 9008, 10);
	test_whole(DOTA_DMR, DPK_RADIOID, 0x10000, 2);	/* 547 frames: more than the first builds' 512 */
	test_refused(DOTA_FM);
	test_refused(DOTA_DMR);
	test_cuts();
	test_old_receiver();
	printf("dota data pack: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
