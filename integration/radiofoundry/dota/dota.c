/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DOTA Transfer: the session (dota.h, docs/dota.md 3, 4, 5, 11, 13; a data pack: 20). Installed as
 * core/radio/dota.c. */
#include "config.h"
#if CONFIG_DOTA
#include <string.h>
#include "radio/dota.h"
#include "radio/radio.h"
#include "radio/beep.h"
#include "aprs/ax25.h"
#include "aprs/hdlc.h"
#include "aprs/afsk.h"
#include "dmr/dmrData.h"
#include "dmr/dmrDataService.h"
#include "board.h"
#if CONFIG_DATAPACK
#include "storage/datapack.h"
#include "storage/ref_pack.h"
#endif

#define PREAMBLE_FLAGS	60u	/* 400 ms: the receiver's CTCSS detector opens first */
#define TAIL_FLAGS	3u
#define KEYUP_MS	25u
#define KEY_MAX_MS	10000u	/* a transmission's hard limit */
#define TONE_HOLD_MS	1500u	/* FM: a frame counts when our tone was heard this recently */
#define BUSY_GIVE_UP_MS	20000u	/* sending: the channel never cleared */
#define START_TONE_MS	900u	/* the start tones play before the first key-up */
#define CRACKLE_MS	140u
#define AFSK_LEVEL	50u
#define HALF		256u	/* FM: one half of the tone buffer holds one frame (128 B payload, stuffed) */
#define FM_FRAME_MAX	(DOTA_HDR + 128u + 2u)
#define AX_HDR		16u	/* AX.25 UI: two addresses, control, PID */
#define DMR_SLOTS	3u
#define UDP_HDR		8u
#define PACK_CHUNK	120u	/* a data pack's bytes in one DATA frame (one "record" of the channels section) */
#define PACK_ASK_MS	10000u	/* the receiver's question goes when no HELLO came for this long */

/* --- Links (docs/dota.md 4.3, 5.3, 7.2) --- */
static const dota_link_t fm_link = {
	.window = 7, .frame_extra = AX_HDR + 3u, .payload_max = 128, .window_bytes = 1100, .turn_ms = 150,
	.frame_ms = 1100, .ack_ms = 2500, .end_gap_ms = 4000, .idle_ms = 30000, .rest_after_ms = 60000, .rest_ms = 5000,
	.polls = 4, .tries = 8, .end_tries = 5, .call = false,
};
static const dota_link_t dmr_link = {
	.window = 6, .frame_extra = 13, .payload_max = DOTA_PAYLOAD_MAX, .turn_ms = 0, .frame_ms = 1250, .ack_ms = 3500,
	.end_gap_ms = 4000, .idle_ms = 30000, .polls = 4, .tries = 8, .end_tries = 5, .call = true,
};

/* --- The session's memory: one role at a time --- */
typedef struct {
	dota_tx_t p;
	uint8_t sel[CODEPLUG_CHANNELS_MAX / 8u];	/* channels sent (number - 1) */
	uint8_t cts[CODEPLUG_CONTACTS_MAX / 8u];	/* contacts sent (slot - 1) */
	uint8_t lists[(CODEPLUG_LISTS_MAX + 7u) / 8u];
	uint8_t parts[CODEPLUG_ZONES_MAX];		/* DOTA Clone: zone parts per zone */
	zone_t z;					/* kind ZONE / CHANNEL: the channels in order; else a zone read */
	uint8_t stage;					/* the preparation's step */
	uint16_t k;
#if CONFIG_DATAPACK
	dpk_hdr_t ph;					/* kind PACK: the pack's header, HELLO's attachment */
#endif
	union {
		struct {
			uint8_t half[2][HALF];
			uint8_t frame[AX_HDR + DOTA_HDR + DOTA_CALL + 128u + 2u];
		} fm;
		uint8_t pkt[DMR_SLOTS][UDP_HDR + DOTA_FRAME_MAX];
	} io;
} txs_t;

typedef struct {
	uint8_t sec, count;
	uint16_t first;
	uint8_t len;
	uint8_t p[DOTA_PAYLOAD_MAX];
} qframe_t;

typedef struct {
	/* First: below the sender's transport area, which the UI borrows while no session runs (dota_scratch) */
	bool pack;					/* this session takes a data pack (u.pk, not u.im) */
	volatile bool begin;				/* a data pack: the storage job starts its slot */
	volatile uint8_t ask;				/* a data pack's question: PK_* */
	uint32_t t_ask;					/* the last HELLO of the pack asked about */
	dota_rx_t p;
	union {
		dota_imp_t im;				/* channels, zones, contacts, RX group lists */
#if CONFIG_DATAPACK
		struct {
			dpk_hdr_t h;			/* the pack offered (asked about), then the one written */
			dpk_writer_t w;
		} pk;					/* a data pack (docs/dota.md 20) */
#endif
	} u;
	qframe_t q[2];
	volatile uint8_t qh, qt;			/* the radio task puts, the storage job takes */
	union {
		struct {
			uint8_t half[2][96];
			uint8_t frame[AX_HDR + DOTA_HDR + DOTA_CALL + 40u + DOTA_FW_MAX];
		} fm;
		uint8_t pkt[UDP_HDR + DOTA_HDR + DOTA_CALL + 40u + DOTA_FW_MAX];
	} io;
} rxs_t;

static union {
	txs_t tx;
	rxs_t rx;
	uint32_t align;
} b;

/* The receiver's data pack question (rxs_t.ask) */
enum { PK_NONE, PK_ASK, PK_YES, PK_NO };

/* The UI's scratch while no session runs (the pickers' zone and channel copies): the sender's transport area */
void *dota_scratch(void)
{
	return dota_on() ? NULL : (void *)&b.tx.io;
}

/* The sending selection lives in the buffer: the UI fills it before the start */
uint8_t *dota_selection(void)
{
	return b.tx.sel;
}

/* --- Shared state --- */
static dota_req_t req;			/* the UI writes it before want_start, the radio task reads it after */
static volatile bool want_start, want_stop, active;
static dota_link_t lk;
static dota_view_t view;
static uint8_t phase;
static uint32_t peer_dmr;		/* DMR: the other side's ID */
static uint32_t t_phase, t_heard, t_tone, t_crackle, t_click, t_end;
static uint32_t answer_to;		/* DMR: the source of the last frame heard (a refusal answers it) */
static bool ending;			/* the result is out; the radio goes back at t_end */
static uint32_t last_blocks;
static uint32_t now_ms;
static ax25_addr_t me;
static volatile bool want_commit, want_undo, job_failed;
static volatile uint8_t commit_result;	/* 0 none, 1 ok, 2 failed */
static bool undo_needed;
static uint16_t session_id;

/* The transport's state: one transmission (a window, or an answer) */
enum { TS_IDLE, TS_KEYUP, TS_SEND };
static struct {
	uint8_t st;
	uint8_t n, i;		/* frames, the next to put out */
	uint8_t q;		/* FM: the half queued last */
	uint8_t level;		/* FM: the NRZI level after the last half */
	uint32_t t0;
	bool answer;		/* the receiver's answer (built already) */
	uint8_t alen;
} ts;

static bool sending(void)
{
	return req.role == DOTA_SEND;
}

static bool fm(void)
{
	return req.method == DOTA_FM;
}

static void touch(void)
{
	view.serial++;
}

/* --- Bitmaps --- */
static bool bit(const uint8_t *m, unsigned i)
{
	return (m[i >> 3] >> (i & 7u)) & 1u;
}
static void set_bit(uint8_t *m, unsigned i)
{
	m[i >> 3] |= (uint8_t)(1u << (i & 7u));
}
static int nth_set(const uint8_t *m, unsigned bits, unsigned k)
{
	for (unsigned i = 0; i < bits; i++)
		if (bit(m, i) && k-- == 0)
			return (int)i;
	return -1;
}
static unsigned rank(const uint8_t *m, unsigned i)
{
	unsigned r = 0;
	for (unsigned j = 0; j < i; j++)
		r += bit(m, j);
	return r;
}


/* --- Any task --- */
bool dota_on(void)
{
	return active || want_start;
}

bool dota_start(const dota_req_t *r)
{
	if (dota_on())
		return false;
	req = *r;
	want_stop = false;
	want_start = true;
	radio_req_dota(true);
	return true;
}

void dota_stop(void)
{
	if (dota_on()) {
		want_stop = true;
		radio_req_dota(false);
	}
}

void dota_view(dota_view_t *v)
{
	*v = view;
}

void dota_ptt_prompt_clear(void)
{
	view.ptt_prompt = false;
	touch();
}

bool dota_job_wanted(void)
{
	return !sending() && (b.rx.qh != b.rx.qt || want_commit || want_undo || (b.rx.begin && active));
}

#if CONFIG_DATAPACK
/* The receiver: the data pack its user is asked about (true: *h filled) */
bool dota_pack_offer(dpk_hdr_t *h)
{
	if (sending() || b.rx.ask != PK_ASK)
		return false;
	*h = b.rx.u.pk.h;
	return b.rx.ask == PK_ASK;	/* still the question (the radio task may have moved on) */
}

/* The user's answer: the next HELLO of that pack is accepted (or refused) */
void dota_pack_answer(bool yes)
{
	if (!sending() && b.rx.ask == PK_ASK) {
		b.rx.ask = yes ? PK_YES : PK_NO;
		view.asking = false;
		touch();
	}
}

/* A frame of the pack: the chunk at its record's place, whole (the last one as long as the pack's end leaves it) */
static bool pack_frame_ok(const dpk_hdr_t *h, uint8_t sec, uint16_t first, uint8_t count, uint8_t len)
{
	uint32_t off = (uint32_t)first * PACK_CHUNK;
	return sec == DOTA_SEC_CHANNELS && count == 1u && off < h->len &&
	       len == (h->len - off < PACK_CHUNK ? h->len - off : PACK_CHUNK);
}
#endif

/* The start-up task's storage job: the queued frames, then the commit or the undo */
void dota_job(void *ctx)
{
	(void)ctx;
	rxs_t *r = &b.rx;
#if CONFIG_DATAPACK
	/* A data pack: the slot not in force erased first (the pack in force stays until the commit) */
	if (r->begin && active && r->pack) {
		if (!job_failed && !dpk_begin(&r->u.pk.w, &r->u.pk.h))
			job_failed = true;
		r->begin = false;
	}
#endif
	while (r->qt != r->qh) {
		qframe_t *f = &r->q[r->qt & 1u];
#if CONFIG_DATAPACK
		bool ok = r->pack ? dpk_put(&r->u.pk.w, (uint32_t)f->first * PACK_CHUNK, f->p, f->len) :
				    dota_imp_frame(&r->u.im, f->sec, f->first, f->count, f->p, f->len);
#else
		bool ok = dota_imp_frame(&r->u.im, f->sec, f->first, f->count, f->p, f->len);
#endif
		if (!job_failed && !ok)
			job_failed = true;
		__atomic_signal_fence(__ATOMIC_RELEASE);
		r->qt++;
	}
	if (want_commit) {
#if CONFIG_DATAPACK
		/* the payload read back against its CRC, then the header: the new pack is in force */
		bool ok = !job_failed && (r->pack ? dpk_commit(&r->u.pk.w) : dota_imp_commit(&r->u.im));
		if (!ok && r->pack && r->u.pk.h.type == DPK_REF) {
			view.pack_bad = true;	/* its CRC or its structure did not check: never committed */
			touch();		/* the screen says "Bad pack" */
		}
#else
		bool ok = !job_failed && dota_imp_commit(&r->u.im);
#endif
		want_commit = false;
		commit_result = ok ? 1u : 2u;
	}
	if (want_undo) {
		if (!r->pack)
			dota_imp_undo(&r->u.im);	/* a data pack: nothing to take back (its header was never written) */
		want_undo = false;
	}
}

/* --- The sender's records (the fill callback) --- */

static uint16_t channel_number(uint16_t k)
{
	if (req.kind == DOTA_KIND_CHANNEL || req.kind == DOTA_KIND_ZONE)
		return k < b.tx.z.count ? b.tx.z.channels[k] : 0;
	int i = nth_set(b.tx.sel, CODEPLUG_CHANNELS_MAX, k);
	return i < 0 ? 0 : (uint16_t)(i + 1);
}

static bool contact_of(uint16_t slot, uint8_t *type, uint32_t *id)
{
	contact_t c;
	if (slot == 0 || !codeplug_contact(slot, &c))
		return false;
	*type = c.type == CONTACT_PRIVATE ? DOTA_CT_PRIVATE : c.type == CONTACT_ALL_CALL ? DOTA_CT_ALL : DOTA_CT_GROUP;
	*id = c.id;
	return true;
}

/* A zone's members as runs of transfer indices (DOTA Clone), the part asked for; false when the zone is gone */
static bool zone_part(uint16_t zi, uint8_t part, dota_zone_t *p, uint8_t *runs_total)
{
	txs_t *t = &b.tx;
	if (!codeplug_zone(zi, &t->z))
		return false;
	memset(p, 0, sizeof *p);
	p->part = part;
	memcpy(p->name, t->z.name, sizeof p->name);
	uint8_t runs = 0, members = 0;
	int last = -2;
	for (uint16_t i = 0; i < t->z.count; i++) {
		uint16_t n = t->z.channels[i];
		if (n == 0 || n > CODEPLUG_CHANNELS_MAX || !bit(t->sel, n - 1u))
			continue;
		int r = (int)rank(t->sel, n - 1u);
		bool goes_on = runs > 0 && r == last + 1;
		if (!goes_on)
			runs++;
		unsigned ri = runs - 1u;	/* this member's run */
		if (ri / DOTA_ZONE_RUNS == part) {
			unsigned slot = ri % DOTA_ZONE_RUNS;
			if (!goes_on) {
				p->start[slot] = (uint16_t)r;
				p->len[slot] = 0;
				p->runs = (uint8_t)(slot + 1u);
			}
			p->len[slot]++;
		} else if (ri / DOTA_ZONE_RUNS < part) {
			p->offset++;	/* a member of an earlier part */
		}
		members++;
		last = r;
	}
	p->total = members;
	if (runs_total)
		*runs_total = runs;
	return true;
}

/* Record k of the zones section: which zone and part */
static bool zone_record(uint16_t k, uint16_t *zi, uint8_t *part)
{
	for (uint16_t i = 0; i < CODEPLUG_ZONES_MAX; i++) {
		if (k < b.tx.parts[i]) {
			*zi = i;
			*part = (uint8_t)k;
			return true;
		}
		k = (uint16_t)(k - b.tx.parts[i]);
	}
	return false;
}

static size_t put_record(uint8_t sec, uint16_t k, uint8_t *out, size_t max)
{
	txs_t *t = &b.tx;
	switch (sec) {
	case DOTA_SEC_CONTACTS: {
		int slot = nth_set(t->cts, CODEPLUG_CONTACTS_MAX, k);
		contact_t c;
		return slot >= 0 && codeplug_contact((uint16_t)(slot + 1), &c) ? dota_rec_put_contact(&c, out, max) : 0;
	}
	case DOTA_SEC_LISTS: {
		int slot = nth_set(t->lists, CODEPLUG_LISTS_MAX, k);
		listen_list_t l;
		dota_list_t d;
		if (slot < 0 || !codeplug_listen_list((uint16_t)(slot + 1), &l))
			return 0;
		memset(&d, 0, sizeof d);
		memcpy(d.name, l.name, sizeof d.name);
		for (uint8_t i = 0; i < l.count && i < CODEPLUG_LIST_LEN; i++) {
			uint8_t type;
			uint32_t id;
			if (!contact_of(l.contacts[i], &type, &id))
				continue;
			if (type == DOTA_CT_PRIVATE)
				d.private_bits |= 1u << d.count;
			d.id[d.count++] = id;
		}
		return dota_rec_put_list(&d, out, max);
	}
	case DOTA_SEC_CHANNELS: {
		dota_ch_t c;
		memset(&c, 0, sizeof c);
		uint16_t n = channel_number(k);
		if (n == 0 || !codeplug_channel(n, &c.ch))
			return 0;
		if (c.ch.mode == RADIO_MODE_DMR) {
			if (!contact_of(c.ch.contact, &c.ct_type, &c.ct_id))
				c.ct_type = DOTA_CT_NONE;
#if CONFIG_DMR_ANY
			if (c.ch.listen_list == CODEPLUG_LIST_ANY)
				c.list = DOTA_LIST_ANY;
			else
#endif
			if (c.ch.listen_list >= 1u && c.ch.listen_list <= CODEPLUG_LISTS_MAX &&
			    bit(t->lists, c.ch.listen_list - 1u))
				c.list = (uint8_t)(rank(t->lists, c.ch.listen_list - 1u) + 1u);
		}
		return dota_rec_put_channel(&c, board.power_count, out, max);
	}
	case DOTA_SEC_ZONES: {
		uint16_t zi;
		uint8_t part;
		dota_zone_t p;
		if (!zone_record(k, &zi, &part) || !zone_part(zi, part, &p, NULL))
			return 0;
		p.zone = 0;
		for (uint16_t i = 0; i < zi; i++)
			p.zone = (uint8_t)(p.zone + (t->parts[i] != 0));
		return dota_rec_put_zone(&p, out, max);
	}
	default:
		return 0;
	}
}

static size_t fill(void *ctx, uint8_t sec, uint16_t first, uint8_t *count, uint8_t *out, size_t max)
{
	(void)ctx;
#if CONFIG_DATAPACK
	if (req.kind == DOTA_KIND_PACK) {
		/* a data pack: one chunk a frame, read from the pack in force */
		const dpk_hdr_t *h = &b.tx.ph;
		uint32_t off = (uint32_t)first * PACK_CHUNK, n = off < h->len ? h->len - off : 0u;
		if (n > PACK_CHUNK)
			n = PACK_CHUNK;
		if (sec != DOTA_SEC_CHANNELS || n == 0u || n > max || *count > 1u || !dpk_read(req.pack, off, out, n))
			return 0;
		*count = 1;
		return n;
	}
#endif
	size_t n = 0;
	uint8_t c = 0;
	uint8_t rec[DOTA_ZONE_MAX > DOTA_LIST_MAX ? DOTA_ZONE_MAX : DOTA_LIST_MAX];
	uint16_t total = b.tx.p.m.recs[sec];
	for (uint16_t k = first; k < total && (*count == 0 || c < *count); k++) {
		size_t l = put_record(sec, k, rec, sizeof rec);
		if (l == 0)
			return 0;	/* the codeplug changed underneath */
		if (n + l > max)
			break;
		memcpy(out + n, rec, l);
		n += l;
		c++;
	}
	if (*count != 0 && c != *count)
		return 0;
	*count = c;
	return n;
}

/* The preparation, a little each poll (the radio task): what is sent, then how it packs */
static bool prep_step(void)
{
	txs_t *t = &b.tx;
	dota_manifest_t *m = &t->p.m;
	switch (t->stage) {
	case 0:
#if CONFIG_DATAPACK
		if (req.kind == DOTA_KIND_PACK) {
			/* a data pack: its chunks are the channels section's records, one a frame; no pack: nothing */
			memset(m, 0, sizeof *m);
			if (dpk_header(req.pack, &t->ph) && t->ph.len != 0u) {
				m->recs[DOTA_SEC_CHANNELS] = m->frames[DOTA_SEC_CHANNELS] =
					(uint16_t)((t->ph.len + PACK_CHUNK - 1u) / PACK_CHUNK);
				m->pack = (const uint8_t *)&t->ph;
			}
			return true;
		}
#endif
		memset(t->cts, 0, sizeof t->cts);
		memset(t->lists, 0, sizeof t->lists);
		memset(t->parts, 0, sizeof t->parts);
		memset(m, 0, sizeof *m);
		if (req.kind == DOTA_KIND_CHANNEL) {
			memset(&t->z, 0, sizeof t->z);
			t->z.count = 1;
			t->z.channels[0] = req.channel;
		} else if (req.kind == DOTA_KIND_ZONE) {
			if (!codeplug_zone(req.zone, &t->z))
				t->z.count = 0;
		} else if (req.kind == DOTA_KIND_CLONE) {
			memset(t->sel, 0, sizeof t->sel);
			for (uint16_t i = 0; i < codeplug_zone_count() && i < CODEPLUG_ZONES_MAX; i++)
				if (codeplug_zone(i, &t->z))
					for (uint16_t j = 0; j < t->z.count; j++)
						if (t->z.channels[j] >= 1u && t->z.channels[j] <= CODEPLUG_CHANNELS_MAX)
							set_bit(t->sel, t->z.channels[j] - 1u);
		}
		m->recs[DOTA_SEC_CHANNELS] = (uint16_t)(req.kind == DOTA_KIND_CHANNEL || req.kind == DOTA_KIND_ZONE ?
							 t->z.count : rank(t->sel, CODEPLUG_CHANNELS_MAX));
		t->k = 0;
		t->stage = 1;
		return false;
	case 1:	/* the DMR channels' contacts and lists */
		for (uint8_t i = 0; i < 16u && t->k < m->recs[DOTA_SEC_CHANNELS]; i++, t->k++) {
			channel_t ch;
			uint16_t n = channel_number(t->k);
			if (n == 0 || !codeplug_channel(n, &ch) || ch.mode != RADIO_MODE_DMR)
				continue;
			if (ch.contact >= 1u && ch.contact <= CODEPLUG_CONTACTS_MAX)
				set_bit(t->cts, ch.contact - 1u);
			if (ch.listen_list >= 1u && ch.listen_list <= CODEPLUG_LISTS_MAX)
				set_bit(t->lists, ch.listen_list - 1u);
		}
		if (t->k >= m->recs[DOTA_SEC_CHANNELS]) {
			t->stage = 2;
			t->k = 0;
		}
		return false;
	case 2:	/* the lists' members are contacts too */
		for (uint16_t i = 1; i <= CODEPLUG_LISTS_MAX; i++) {
			listen_list_t l;
			if (bit(t->lists, i - 1u) && codeplug_listen_list(i, &l))
				for (uint8_t j = 0; j < l.count && j < CODEPLUG_LIST_LEN; j++)
					if (l.contacts[j] >= 1u && l.contacts[j] <= CODEPLUG_CONTACTS_MAX)
						set_bit(t->cts, l.contacts[j] - 1u);
		}
		m->recs[DOTA_SEC_CONTACTS] = (uint16_t)rank(t->cts, CODEPLUG_CONTACTS_MAX);
		m->recs[DOTA_SEC_LISTS] = (uint16_t)rank(t->lists, CODEPLUG_LISTS_MAX);
		t->stage = req.kind == DOTA_KIND_CLONE ? 3 : 4;
		return false;
	case 3:	/* DOTA Clone: each zone's parts */
		for (uint8_t i = 0; i < 4u && t->k < codeplug_zone_count() && t->k < CODEPLUG_ZONES_MAX; i++, t->k++) {
			dota_zone_t p;
			uint8_t runs = 0;
			if (zone_part(t->k, 0, &p, &runs) && p.total > 0) {
				t->parts[t->k] = (uint8_t)((runs + DOTA_ZONE_RUNS - 1u) / DOTA_ZONE_RUNS);
				m->recs[DOTA_SEC_ZONES] = (uint16_t)(m->recs[DOTA_SEC_ZONES] + t->parts[t->k]);
				m->zones++;
			}
		}
		if (t->k >= codeplug_zone_count() || t->k >= CODEPLUG_ZONES_MAX) {
			t->stage = 4;
			t->k = 0;
		}
		return false;
	default: {	/* how the sections pack, a frame or two a poll */
		uint8_t sec = (uint8_t)(t->stage - 3u);	/* stage 4 = contacts ... 7 = zones */
		for (uint8_t i = 0; i < 2u && sec < DOTA_SEC_COUNT; i++) {
			if (t->k >= m->recs[sec]) {
				t->stage++;
				sec++;
				t->k = 0;
				continue;
			}
			uint8_t count = 0;
			size_t n = fill(NULL, sec, t->k, &count, fm() ? t->io.fm.frame : t->io.pkt[0], lk.payload_max);
			if (n == 0 || count == 0) {
				m->frames[DOTA_SEC_CHANNELS] = 0;
				return true;	/* a record gone or too long: the start fails */
			}
			m->frames[sec]++;
			t->k = (uint16_t)(t->k + count);
		}
		return sec >= DOTA_SEC_COUNT;
	}
	}
}

/* --- Transports --- */

/* The answer / ABORT being sent sits where its transport puts it */
static uint8_t *own_frame(void)
{
	if (fm())
		return (sending() ? b.tx.io.fm.frame : b.rx.io.fm.frame) + AX_HDR;
	return (sending() ? b.tx.io.pkt[0] : b.rx.io.pkt) + UDP_HDR;
}

static size_t own_room(void)
{
	if (fm())
		return (sending() ? sizeof b.tx.io.fm.frame : sizeof b.rx.io.fm.frame) - AX_HDR;
	return (sending() ? sizeof b.tx.io.pkt[0] : sizeof b.rx.io.pkt) - UDP_HDR;
}

/* AX.25 UI header: to DOTA, from our callsign */
static void ax25_header(uint8_t *h)
{
	static const char dst[AX25_CALL_LEN] = { 'D', 'O', 'T', 'A', ' ', ' ' };
	size_t n = strnlen(me.call, AX25_CALL_LEN);
	for (unsigned i = 0; i < AX25_CALL_LEN; i++) {
		h[i] = (uint8_t)(dst[i] << 1);
		h[7 + i] = (uint8_t)((i < n ? me.call[i] : ' ') << 1);
	}
	h[6] = 0xE0u;					/* destination SSID 0 with the C bit */
	h[13] = (uint8_t)(0x61u | (me.ssid << 1));	/* source SSID, the last address */
	h[14] = AX25_CONTROL_UI;
	h[15] = AX25_PID_NONE;
}

/* FM: the preamble (frame NULL) or one frame into a half of tone bits; the last one gets the tail flags. The NRZI level
 * goes on from half to half */
static uint16_t fm_half(uint8_t *half, size_t size, const uint8_t *frame, size_t n, bool last)
{
	hdlc_enc_t e;
	hdlc_enc_init(&e, half, size);
	e.level = ts.level;
	if (frame == NULL ? !hdlc_enc_flags(&e, PREAMBLE_FLAGS) :
	    !hdlc_enc_frame(&e, frame, n) || !hdlc_enc_flags(&e, last ? TAIL_FLAGS : 1u))
		return 0;
	ts.level = e.level;
	return (uint16_t)e.nbits;
}

static uint8_t *fm_halfbuf(uint8_t q, size_t *size)
{
	*size = sending() ? sizeof b.tx.io.fm.half[0] : sizeof b.rx.io.fm.half[0];
	return sending() ? b.tx.io.fm.half[q] : b.rx.io.fm.half[q];
}

/* Frame i of this transmission as DOTA bytes at out */
static size_t frame_bytes(uint8_t i, uint8_t *out, size_t max)
{
	if (ts.answer)
		return ts.alen;		/* built in place already */
	return dota_tx_frame(&b.tx.p, i, out, max);
}

static void finish_send(const dota_io_t *io)
{
	uint32_t keyed = now_ms - ts.t0;
	bool window = ts.st != TS_IDLE && !ts.answer && sending();
	if (fm() && ts.st != TS_IDLE) {
		if (board.afsk->monitor)
			board.afsk->monitor(false);
		board.afsk->tx_stop();
		io->key(false);
	}
	ts.st = TS_IDLE;
	view.keyed = false;
	if (window)
		dota_tx_sent(&b.tx.p, keyed, now_ms);
	if (!sending() && b.rx.p.state == DOTA_RX_ARMED)
		peer_dmr = 0;	/* a refusal went (DMR: to whoever asked): back to waiting for anyone's HELLO */
	t_heard = now_ms;
	touch();
}

static bool fm_more(void)
{
	uint8_t *frame = own_frame() - AX_HDR;
	size_t n = frame_bytes(ts.i, frame + AX_HDR, own_room());
	if (n == 0)
		return false;
	ax25_header(frame);
	uint8_t q = (uint8_t)!ts.q;
	size_t hs;
	uint8_t *half = fm_halfbuf(q, &hs);
	uint16_t bits = fm_half(half, hs, frame, AX_HDR + n, ts.i + 1u == ts.n);
	if (bits == 0 || !board.afsk->tx_more(half, bits))
		return false;
	ts.q = q;
	ts.i++;
	return true;
}

/* DMR: frames i.. as a chain of UDP datagrams to the other side */
static bool dmr_chain(void)
{
	dmrDataChainPacket_t pk[DMR_SLOTS];
	uint8_t n = 0;
	for (uint8_t i = ts.i; i < ts.n && n < (ts.answer ? 1u : DMR_SLOTS); i++, n++) {
		uint8_t *p = ts.answer ? own_frame() - UDP_HDR : b.tx.io.pkt[n];
		size_t len = frame_bytes(i, p + UDP_HDR, ts.answer ? own_room() : sizeof b.tx.io.pkt[0] - UDP_HDR);
		if (len == 0)
			return false;
		uint16_t ul = (uint16_t)(UDP_HDR + len);
		p[0] = p[2] = (uint8_t)(DOTA_PORT >> 8);
		p[1] = p[3] = (uint8_t)DOTA_PORT;
		p[4] = (uint8_t)(ul >> 8);
		p[5] = (uint8_t)ul;
		p[6] = p[7] = 0;
		pk[n] = (dmrDataChainPacket_t){ DMR_IP_PROTO_UDP, DMR_IPS_UNCONFIRMED, p, ul };
	}
	int taken = dmrDataServiceSendIPChain(false, peer_dmr, DMR_DATA_IP_OURS, DMR_DATA_IP_OURS, DMR_IP_TTL_DEFAULT,
					      pk, n);
	if (taken <= 0)
		return false;
	ts.i = (uint8_t)(ts.i + taken);
	return true;
}

/* Start a transmission of n frames (a window, or one answer / ABORT built at own_frame()) */
static bool send_start(const dota_io_t *io, uint8_t n, bool answer, uint8_t alen)
{
	memset(&ts, 0, sizeof ts);
	ts.n = n;
	ts.answer = answer;
	ts.alen = alen;
	ts.t0 = now_ms;
	ts.level = 1;
	if (fm()) {
		if (board.afsk == NULL || board.afsk->tx_more == NULL || board.afsk->tx_queued == NULL || !io->key(true))
			return false;
		ts.st = TS_KEYUP;
	} else {
		ts.st = TS_SEND;
	}
	view.keyed = true;
	touch();
	return true;
}

static void send_service(const dota_io_t *io)
{
	if (ts.st == TS_IDLE)
		return;
	if (now_ms - ts.t0 > KEY_MAX_MS) {
		finish_send(io);	/* cut: what is missing goes again */
		return;
	}
	if (fm()) {
		if (ts.st == TS_KEYUP) {
			if (now_ms - ts.t0 < KEYUP_MS)
				return;
			size_t hs;
			uint8_t *half = fm_halfbuf(0, &hs);
			uint16_t bits = fm_half(half, hs, NULL, 0, false);
			if (bits == 0 ||
			    !board.afsk->tx_start(half, bits, AFSK_BAUD, AFSK_MARK_HZ, AFSK_SPACE_HZ, AFSK_LEVEL)) {
				finish_send(io);
				return;
			}
			if (board.afsk->monitor)
				board.afsk->monitor(true);	/* the speaker plays our own tones (docs/dota.md 13) */
			ts.q = 0;
			ts.st = TS_SEND;
		}
		while (ts.i < ts.n && !board.afsk->tx_queued())
			if (!fm_more()) {
				ts.i = ts.n;	/* nothing more goes in this one: the rest is sent again later */
				break;
			}
		if (ts.i >= ts.n && !board.afsk->tx_busy())
			finish_send(io);
		return;
	}
	if (dmrDataServiceIpBusy())
		return;
	if (ts.i >= ts.n) {
		finish_send(io);
		return;
	}
	if (io->dmr_ready && !dmr_chain() && now_ms - ts.t0 > 5000u)
		finish_send(io);	/* the service won't take it: the window counts as sent, its frames go again */
}

/* --- Frames heard --- */

static void heard(const uint8_t *in, size_t n, const char *peer)
{
	if (sending())
		dota_tx_heard(&b.tx.p, in, n, peer, now_ms);
	else
		dota_rx_heard(&b.rx.p, in, n, peer, now_ms);
	t_heard = now_ms;
	touch();
}

bool dota_fm_listening(uint32_t rx_hz)
{
	return active && fm() && rx_hz == req.hz && ts.st == TS_IDLE && phase != DOTA_P_PREP;
}

void dota_fm_frame(const uint8_t *frame, size_t n)
{
	ax25_frame_t f;
	if (!dota_fm_listening(req.hz) || now_ms - t_tone > TONE_HOLD_MS || !ax25_decode(frame, n, &f) ||
	    f.ndigi != 0 || f.control != AX25_CONTROL_UI || f.pid != AX25_PID_NONE || strcmp(f.dst.call, "DOTA") != 0)
		return;		/* not ours, or heard without our tone */
	heard(f.info, f.info_len, f.src.call);
}

void dota_dmr_rx(uint32_t src, const uint8_t *payload, int length)
{
	if (!active || fm() || length <= 0 || length > (int)DOTA_FRAME_MAX)
		return;
	if (peer_dmr != 0 && src != peer_dmr)
		return;		/* another station */
	uint8_t before = sending() ? 0 : b.rx.p.state;
	if (!sending())
		answer_to = src;
	heard(payload, (size_t)length, NULL);
	if (!sending() && before == DOTA_RX_ARMED && b.rx.p.state == DOTA_RX_RECEIVING)
		peer_dmr = src;	/* the sender this session answers from now on */
}

/* --- The receiver's callbacks --- */

static uint8_t our_modes(void)
{
	return (uint8_t)(0x03u | (CONFIG_P25_CHANNELS ? 0x04u : 0u) | (CONFIG_YSF ? 0x08u : 0u) |
			 (CONFIG_NXDN ? 0x10u : 0u) | (CONFIG_DPMR ? 0x20u : 0u));
}

#if CONFIG_DATAPACK
/* A data pack's HELLO: checked, then its user asked (the HELLO answered "wait" until the answer); the next HELLO of
 * the same pack after Yes is accepted, after No refused */
static uint8_t pack_hello(rxs_t *r, const dota_manifest_t *m)
{
	dpk_hdr_t h;
	memcpy(&h, m->pack, sizeof h);
	uint16_t chunks = (uint16_t)((h.len + PACK_CHUNK - 1u) / PACK_CHUNK);
	/* A map pack whose header says it was built for another firmware (another format, or a version hint that
	 * disagrees; hint 0 = none, then the commit's structure check decides): refused before the ~9 minute transfer,
	 * nothing erased, and the receiver says why */
	if (h.type == DPK_REF && memcmp(h.magic, "DPK1", 4) == 0 && (h.fmt != 1u || (h.flags != 0u && h.flags != ref_hint()))) {
		if (!view.pack_other) {
			view.pack_other = true;
			touch();
		}
		return DOTA_R_VERSION;
	}
	if (h.type < 1u || h.type > DPK_TYPES || !dpk_hdr_ok(&h, h.type) || h.len == 0u || dota_manifest_total(m) != chunks ||
	    m->recs[DOTA_SEC_CHANNELS] != chunks || m->frames[DOTA_SEC_CHANNELS] != chunks)
		return DOTA_R_VERSION;
	bool same = r->ask != PK_NONE && memcmp(&r->u.pk.h, &h, sizeof h) == 0;
	r->t_ask = now_ms;
	if (same && r->ask == PK_NO)
		return DOTA_R_DECLINED;
	if (same && r->ask == PK_YES) {
		r->ask = PK_NONE;
		r->pack = true;
		r->begin = true;	/* the storage job erases the slot not in force */
		return DOTA_R_OK;
	}
	if (!same) {
		r->u.pk.h = h;
		r->ask = PK_ASK;
		view.asking = true;	/* the screen asks "Data pack: RadioID TR, 2026-11-17, 98 KB. Accept?" */
		touch();
	}
	return DOTA_R_WAIT;
}
#endif

static uint8_t rx_hello(void *ctx, const dota_manifest_t *m, const char *peer)
{
	(void)ctx;
	(void)peer;
	rxs_t *r = &b.rx;
	if (m->rec_ver != DOTA_REC_VERSION || m->layout != DOTA_LAYOUT)
		return DOTA_R_VERSION;	/* owner 2026-11-20: only the protocol / record versions must match */
	if ((m->modes & ~our_modes()) != 0)
		return DOTA_R_MODE;
	r->pack = false;
	if (m->kind == DOTA_KIND_PACK || m->pack != NULL) {
#if CONFIG_DATAPACK
		uint8_t st = m->kind == DOTA_KIND_PACK && m->pack != NULL ? pack_hello(r, m) : DOTA_R_VERSION;
		if (st == DOTA_R_OK) {
			r->qh = r->qt = 0;
			job_failed = false;
			view.channels = m->recs[DOTA_SEC_CHANNELS];
			view.pack = true;
		}
		return st;
#else
		return DOTA_R_VERSION;	/* a build without data packs: "Other version", nothing written */
#endif
	}
#if CONFIG_DATAPACK
	if (r->ask != PK_NONE) {
		r->ask = PK_NONE;	/* another sender's channels: the question goes (u.pk is overwritten now) */
		view.asking = false;
		touch();
	}
#endif
	uint8_t st = dota_imp_begin(&r->u.im, &req.imp, m, board.power_count, &r->p.free_ch, &r->p.free_zones,
				    &r->p.zone_room);
	if (st == DOTA_R_OK) {
		r->qh = r->qt = 0;
		job_failed = false;
		undo_needed = true;
		view.channels = m->recs[DOTA_SEC_CHANNELS];
	}
	view.why = st;
	touch();
	return st;
}

static bool rx_data(void *ctx, uint8_t sec, uint16_t first, uint8_t count, const uint8_t *p, uint8_t len)
{
	(void)ctx;
	rxs_t *r = &b.rx;
#if CONFIG_DATAPACK
	if (r->pack && !pack_frame_ok(&r->u.pk.h, sec, first, count, len))
		return false;	/* not a chunk of the pack: never taken (the sender's tries run out) */
#endif
	if ((uint8_t)(r->qh - r->qt) >= 2u)
		return false;	/* both slots wait for the storage job */
	qframe_t *f = &r->q[r->qh & 1u];
	f->sec = sec;
	f->first = first;
	f->count = count;
	f->len = len;
	memcpy(f->p, p, len);
	__atomic_signal_fence(__ATOMIC_RELEASE);
	r->qh++;
	return true;
}

static void rx_commit(void *ctx)
{
	(void)ctx;
	commit_result = 0;
	want_commit = true;
	touch();
}

/* --- The radio core --- */

bool dota_begin(dota_req_t *r, uint32_t now)
{
	if (!want_start || active)
		return false;
	want_start = false;
	now_ms = now;
	memset(&view, 0, sizeof view);
	memset(&ts, 0, sizeof ts);
	view.role = req.role;
	view.method = req.method;
	view.code = req.code;
	view.pack = req.kind == DOTA_KIND_PACK && sending();
	lk = fm() ? fm_link : dmr_link;
	memset(&me, 0, sizeof me);
	(void)ax25_addr_parse(req.call, strlen(req.call), &me);
	memcpy(lk.me, me.call, sizeof lk.me);
	peer_dmr = sending() ? req.peer_id : 0;
	answer_to = 0;
	want_commit = want_undo = job_failed = false;
	commit_result = 0;
	undo_needed = false;
	ending = false;
	t_phase = t_heard = now;
	t_tone = now - TONE_HOLD_MS - 1u;
	last_blocks = dmrDataServiceRxBlockCount();
	session_id = (uint16_t)(((now * 2654435761u) >> 16) | 1u);
	if (sending()) {
		b.tx.stage = 0;
		b.tx.k = 0;
		phase = DOTA_P_PREP;
	} else {
		dota_rx_t *p = &b.rx.p;
		b.rx.pack = b.rx.begin = false;
		b.rx.ask = PK_NONE;
		dota_rx_arm(p, &lk, req.code, rx_hello, rx_data, rx_commit, NULL, now);
		phase = DOTA_P_WAIT;
	}
	view.phase = phase;
	active = true;
	*r = req;
	touch();
	return true;
}

bool dota_owns_radio(void)
{
	return active;
}

void dota_ptt(void)
{
	if (active && !ending) {
		view.ptt_prompt = true;
		touch();
	}
}

/* The end: the result's tones; the radio is given back now, or after a while for the receiver that answers a
 * repeated END (its END_ACK lost) */
static void conclude(const dota_io_t *io, uint8_t result, uint8_t why, uint32_t linger_ms)
{
	if (ending)
		return;
	view.result = result;
	view.why = why;
	view.ptt_prompt = false;
	bool ok = result == DOTA_RES_SENT || result == DOTA_RES_DONE;
	phase = ok ? DOTA_P_DONE : DOTA_P_FAILED;
	io->beep(ok ? BEEP_DOTA_MARK : BEEP_TX_DENY);
	if (!sending() && !ok && undo_needed) {
		want_undo = true;	/* the storage job takes away what this session added */
		undo_needed = false;
	}
	ending = true;
	t_end = now_ms + linger_ms;
	touch();
}

static void tx_result(const dota_io_t *io)
{
	dota_tx_t *p = &b.tx.p;
	if (p->state == DOTA_TX_DONE) {
		view.ch_added = p->ch_added;
		view.zones_added = p->zones_added;
		conclude(io, DOTA_RES_SENT, 0, 0);
		return;
	}
	/* By DOTA_TF_* */
	static const uint8_t res[] = { DOTA_RES_FAILED, DOTA_RES_NO_ANSWER, DOTA_RES_REFUSED, DOTA_RES_STOPPED,
				       DOTA_RES_PEER_STOPPED, DOTA_RES_FAILED, DOTA_RES_FAILED, DOTA_RES_FAILED,
				       DOTA_RES_UNSURE };
	if (p->state != DOTA_TX_FAILED)
		return;
	uint8_t r = p->fail < sizeof res ? res[p->fail] : DOTA_RES_FAILED;
	if (r == DOTA_RES_UNSURE && !p->hello_ok)
		r = DOTA_RES_NO_ANSWER;	/* nothing ever heard back (it doesn't claim nothing changed either) */
	conclude(io, r, p->fail == DOTA_TF_REFUSED ? p->status : p->fail, 0);
}

static void rx_result(const dota_io_t *io)
{
	rxs_t *r = &b.rx;
	dota_rx_t *p = &r->p;
	if (p->state == DOTA_RX_DONE) {
		view.ch_added = r->pack ? 0u : r->u.im.added;
		view.zones_added = r->pack ? 0u : r->u.im.zones_added;
		conclude(io, DOTA_RES_DONE, 0, 8000u);
	} else if (p->state == DOTA_RX_FAILED) {
		conclude(io, p->fail == DOTA_RF_FLASH ? DOTA_RES_FLASH : p->fail == DOTA_RF_TX_STOPPED ?
			 DOTA_RES_PEER_STOPPED : p->fail == DOTA_RF_STOPPED ? DOTA_RES_STOPPED : DOTA_RES_INCOMPLETE,
			 p->fail, p->fail == DOTA_RF_CRC || p->fail == DOTA_RF_FLASH ? 4000u : 0u);
	}
}

static void sounds(const dota_io_t *io)
{
	if (fm() || ending)
		return;	/* FM: the real tones are heard (the receiver's speaker, the sender's monitor) */
	/* DMR: a crackle while data comes in and while the sender waits for the answer (docs/dota.md 13). While our own
	 * transmitter is keyed the speaker's PCM is held off the air path: the amplifier's clicks stand in */
	uint32_t blocks = dmrDataServiceRxBlockCount();
	bool flowing = blocks != last_blocks;
	last_blocks = blocks;
	if (ts.st != TS_IDLE) {
		if (board.afsk != NULL && board.afsk->click != NULL && now_ms - t_click >= 50u + (now_ms * 7u) % 90u) {
			t_click = now_ms;
			board.afsk->click();
		}
		return;
	}
	bool waiting = sending() && b.tx.p.state == DOTA_TX_WAIT;
	if ((flowing || waiting) && now_ms - t_crackle >= CRACKLE_MS) {
		t_crackle = now_ms;
		io->beep(BEEP_DOTA_DATA);
	}
}

/* The user stops (or the battery goes): one ABORT, best effort, then the end */
static void stop_now(const dota_io_t *io, uint8_t reason)
{
	if (ts.st != TS_IDLE)
		finish_send(io);
	if (!sending() && b.rx.p.state == DOTA_RX_COMMIT)
		return;		/* writing: it runs to its end */
	size_t n = 0;
	bool started = !sending() || phase != DOTA_P_PREP;
	if (started)
		n = sending() ? dota_tx_stop(&b.tx.p, reason, own_frame(), own_room()) :
			      dota_rx_stop(&b.rx.p, reason, own_frame(), own_room());
	if (n != 0 && (fm() ? io->clear : io->dmr_ready))
		(void)send_start(io, 1, true, (uint8_t)n);
	conclude(io, reason == DOTA_A_USER ? DOTA_RES_STOPPED : DOTA_RES_FAILED, reason, 0);
}

static void tx_poll(const dota_io_t *io)
{
	dota_tx_t *p = &b.tx.p;
	if (ts.st == TS_IDLE && !ending) {
		dota_tx_tick(p, now_ms);
		bool clear = fm() ? io->clear : io->dmr_ready;
		if (!clear && p->state == DOTA_TX_READY && now_ms - t_heard > BUSY_GIVE_UP_MS) {
			conclude(io, DOTA_RES_BUSY, 0, 0);
			return;
		}
		if (clear && now_ms - t_phase >= START_TONE_MS && now_ms - t_heard >= lk.turn_ms) {
			uint8_t n = dota_tx_window(p, now_ms, fm() ? b.tx.io.fm.frame : b.tx.io.pkt[0]);
			if (n != 0 && !send_start(io, n, false, 0))
				conclude(io, DOTA_RES_FAILED, 0, 0);
		}
	}
	view.done = p->acked;
	view.total = p->total;
	view.asking = p->state == DOTA_TX_WAIT;
	view.pct = p->total ? (uint8_t)((uint32_t)p->acked * 100u / p->total) : 0;
	memcpy(view.peer, p->peer, sizeof view.peer);
	if (ts.st == TS_IDLE)
		tx_result(io);
}

static void rx_poll(const dota_io_t *io)
{
	rxs_t *r = &b.rx;
	dota_rx_t *p = &r->p;
	dota_rx_tick(p, now_ms);
	if (r->ask != PK_NONE && (p->state != DOTA_RX_ARMED || now_ms - r->t_ask >= PACK_ASK_MS)) {
		r->ask = PK_NONE;	/* no HELLO of the pack asked about for a while: the sender gave up */
		view.asking = false;
		touch();
	}
	if (job_failed && p->state == DOTA_RX_RECEIVING) {
		uint8_t tmp[24];
		(void)dota_rx_stop(p, DOTA_A_USER, tmp, sizeof tmp);
		p->fail = DOTA_RF_FLASH;
	}
	if (commit_result != 0 && p->state == DOTA_RX_COMMIT) {
		if (!r->pack) {		/* a data pack: END_ACK's counts stay 0 */
			p->ch_added = r->u.im.added;
			p->zones_added = r->u.im.zones_added;
			p->ct_added = r->u.im.ct_added;
			p->ct_matched = r->u.im.ct_matched;
			p->lists_added = r->u.im.lists_added;
		}
		undo_needed = commit_result != 1u && !r->pack;
		dota_rx_commit_done(p, commit_result == 1u);
		commit_result = 0;
	}
	if (ts.st == TS_IDLE && p->answer != 0) {
		if (!fm() && peer_dmr == 0)
			peer_dmr = answer_to;	/* a refusal goes back to whoever asked (forgotten once sent: finish_send) */
		size_t n = dota_rx_answer(p, now_ms, fm() ? io->clear : io->dmr_ready, own_frame(), own_room());
		if (n != 0)
			(void)send_start(io, 1, true, (uint8_t)n);
	}
	if (phase == DOTA_P_WAIT && p->state == DOTA_RX_RECEIVING) {
		phase = DOTA_P_RUN;
		io->beep(BEEP_DOTA_MARK);
	}
	if (p->state == DOTA_RX_COMMIT)
		phase = DOTA_P_WRITING;
	memcpy(view.peer, p->peer, sizeof view.peer);
	view.done = p->got_n;
	view.total = p->total;
	view.pct = p->state == DOTA_RX_DONE ? 100u : p->total ? (uint8_t)((uint32_t)p->got_n * 100u / p->total) : 0u;
	if (ts.st == TS_IDLE && p->answer == 0)
		rx_result(io);
}

void dota_poll(const dota_io_t *io, uint32_t now)
{
	now_ms = now;
	if (!active)
		return;
	if (io->tone_ok)
		t_tone = now;
	if (!ending && (want_stop || io->battery == RADIO_BATT_CRITICAL)) {
		uint8_t reason = want_stop ? DOTA_A_USER : DOTA_A_BATTERY;
		want_stop = false;
		stop_now(io, reason);
	}
	send_service(io);
	if (phase == DOTA_P_PREP && !ending) {
		if (prep_step()) {
			dota_manifest_t *m = &b.tx.p.m;
			view.channels = m->recs[DOTA_SEC_CHANNELS];
			if (dota_manifest_total(m) == 0 || m->frames[DOTA_SEC_CHANNELS] == 0) {
				conclude(io, DOTA_RES_NOTHING, 0, 0);
			} else {
				m->rec_ver = DOTA_REC_VERSION;
				m->layout = DOTA_LAYOUT;
				m->modes = req.kind == DOTA_KIND_PACK ? 0u : our_modes();	/* a pack uses no mode */
				m->kind = req.kind;
				dota_manifest_t keep = *m;
				dota_tx_start(&b.tx.p, &lk, &keep, req.code, session_id, fill, NULL);
				view.total = b.tx.p.total;
				io->beep(BEEP_DOTA_MARK);
				t_phase = now;
				phase = DOTA_P_RUN;
			}
		}
	} else if (sending()) {
		tx_poll(io);
	} else {
		rx_poll(io);
	}
	sounds(io);
	if (ending && ts.st == TS_IDLE && (int32_t)(now - t_end) >= 0)
		active = false;		/* the radio core tunes back */
	view.phase = phase;
}
#endif
