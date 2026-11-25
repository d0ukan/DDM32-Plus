/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus cross-band store and forward (xband.h): the rules, the state machine, the frame store. Installed as
 * core/radio/xband.c. The UI writes the request and reads the view (as DOTA's dota.c does: plain copies, the radio
 * task owns everything else); the radio task runs the rest. */
#include "config.h"
#if CONFIG_XBAND || CONFIG_DOTA	/* the band plan: DOTA checks with it too */
#include <string.h>
#include "radio/xband.h"

/* --- The rules --- */

#define MHZ(m, k)	((uint32_t)(m) * 1000000u + (uint32_t)(k) * 1000u)
#define HALF_CH		6250u		/* half a 12.5 kHz channel: a signal there overlaps the segment */

typedef struct {
	uint32_t lo, hi;
	uint8_t why;
} seg_t;

/* IARU Region 1 band plan (Turkey's): checked in this order, the first that holds names the refusal */
static const seg_t refused[] = {
	{ MHZ(446, 0), MHZ(446, 200), XB_R_PMR446 },
	{ MHZ(144, 800) - HALF_CH, MHZ(144, 800) + HALF_CH, XB_R_APRS },
	{ MHZ(145, 0) - HALF_CH, MHZ(145, 187) + 500u + HALF_CH, XB_R_REPEATER },	/* RV48-RV63 inputs */
	{ MHZ(145, 600) - HALF_CH, MHZ(145, 787) + 500u + HALF_CH, XB_R_REPEATER },	/* RV48-RV63 outputs */
	{ MHZ(145, 800), MHZ(146, 0), XB_R_SATELLITE },
	{ MHZ(431, 50) - HALF_CH, MHZ(431, 825) + HALF_CH, XB_R_REPEATER },		/* RU inputs (-7.6 MHz) */
	{ MHZ(435, 0), MHZ(438, 0), XB_R_SATELLITE },
	{ MHZ(438, 650) - HALF_CH, MHZ(439, 425) + HALF_CH, XB_R_REPEATER },		/* RU outputs */
};
static const seg_t amateur[] = { { MHZ(144, 0), MHZ(146, 0), 0 }, { MHZ(430, 0), MHZ(440, 0), 0 } };

uint8_t xband_check_hz_free(uint32_t hz, bool free)
{
	for (unsigned i = 0; i < sizeof refused / sizeof refused[0]; i++)
		if (hz >= refused[i].lo && hz <= refused[i].hi && (!free || refused[i].why == XB_R_PMR446))
			return refused[i].why;
	for (unsigned i = 0; i < sizeof amateur / sizeof amateur[0]; i++)
		if (hz >= amateur[i].lo && hz <= amateur[i].hi)
			return XB_R_OK;
	return XB_R_BAND;
}

uint8_t xband_check_hz(uint32_t hz)
{
	return xband_check_hz_free(hz, false);
}
#endif

#if CONFIG_XBAND
/* The pairs built (xband.h): frames as heard, the stock codec's decode into FM, or (FM -> DMR) the stock codec's
 * encode of FM receive audio */
bool xband_pair_ok(uint8_t from, uint8_t to)
{
	switch (from) {
	case XB_MODE_DMR:
	case XB_MODE_NXDN: return to == XB_MODE_DMR || to == XB_MODE_FM;
	case XB_MODE_P25: return to == XB_MODE_P25;
	case XB_MODE_FM: return to == XB_MODE_DMR;	/* FM receive -> AMBE+2 encode -> DMR (radio_xband.inc) */
	default: return false;
	}
}

/* A side is heard when its over can go to the other side. An FM side is heard only when the DMR side it relays to
 * has a TX talkgroup: FM carries none, so with "As heard" there is nothing to key to; listening there would only
 * drop every FM over with an error beep and halve the time the DMR side is heard (DMR -> FM stays one-way then) */
static bool heard_on(const xband_req_t *r, uint8_t s)
{
	return xband_pair_ok(r->mode[s], r->mode[1u - s]) && (r->mode[s] != XB_MODE_FM || r->side[1u - s].tx_tg != 0);
}

uint8_t xband_listens(const xband_req_t *r)
{
	return (uint8_t)((heard_on(r, 0) ? 1u : 0u) | (heard_on(r, 1) ? 2u : 0u));
}

uint8_t xband_frame_bytes(const xband_req_t *r)
{
	return r->mode[0] == XB_MODE_P25 ? XB_P25_FRAME_BYTES : XB_FRAME_BYTES;	/* P25 pairs only with P25 */
}

/* Tools > Cross-band's # (owner, 2026-11-25): RAM only, off at every power-on, never saved; it cannot change while a
 * session runs, so every check of the session (the start, xband_begin) reads the value it started with */
static bool segs_free;

bool xband_segs_free(void)
{
	return segs_free;
}

bool xband_set_segs_free(bool on)
{
	if (xband_on())
		return false;
	segs_free = on;
	return true;
}

uint8_t xband_check(const xband_req_t *r)
{
	if (r->mode[0] >= XB_MODES || r->mode[1] >= XB_MODES || xband_listens(r) == 0)
		return XB_R_MODE;
	for (unsigned s = 0; s < 2; s++) {
		uint8_t why = xband_check_hz_free(r->hz[s], segs_free);
		if (why != XB_R_OK)
			return why;
	}
	if (r->hz[0] == r->hz[1])
		return XB_R_SAME;
	size_t n = strnlen(r->call, sizeof r->call);
	if (n < 4u || n > XB_CALL_MAX)
		return XB_R_CALL;
	return XB_R_OK;
}

uint8_t xband_power_level(uint8_t cls, uint8_t power_count)
{
	if (power_count == 0)
		return 0;
	return cls <= 1u ? 0 : cls >= 3u ? (uint8_t)(power_count - 1u) : (uint8_t)(power_count / 2u);
}

static uint8_t fb = XB_FRAME_BYTES;	/* the session's frame size */

uint16_t xband_frames_for(size_t bytes)
{
	size_t n = bytes / fb;
	return (uint16_t)(n > 0xFFFFu ? 0xFFFFu : n);
}

/* --- The UI's side --- */

#define XB_SIDE_DEFAULT { .rx_nac = XB_NAC_ANY, .tx_nac = XB_NAC_DEFAULT, .rx_cc = XB_CC_ANY, .rx_ts = XB_TS_ANY, 			 .tx_cc = 1, .tx_ts = 1 }
static xband_req_t req = { .mode = { XB_MODE_DMR, XB_MODE_DMR }, .side = { XB_SIDE_DEFAULT, XB_SIDE_DEFAULT },
			   .power = 1 };
static volatile bool want_start, want_stop;
bool xband_native_key, xband_held;
static bool active;			/* the radio core runs the session */
static xband_view_t view;

static void touch(void)
{
	view.serial++;
}

bool xband_on(void)
{
	return active || want_start;
}

xband_req_t *xband_req(void)
{
	return &req;
}

bool xband_start(void)
{
	if (xband_on() || xband_check(&req) != XB_R_OK)
		return false;
	want_stop = false;
	want_start = true;
	radio_req_xband(true);
	return true;
}

void xband_stop(void)
{
	if (xband_on()) {
		want_stop = true;
		radio_req_xband(false);
	}
}

void xband_view(xband_view_t *v)
{
	*v = view;
}

/* --- The frame store: the lent segments, frame i at its place across them --- */

static uint8_t *seg_at[XB_SEGMENTS];
static uint16_t seg_frames[XB_SEGMENTS];
static uint8_t segs;
static uint16_t capacity;

static uint8_t *frame_at(uint16_t i)
{
	for (uint8_t k = 0; k < segs; k++) {
		if (i < seg_frames[k])
			return seg_at[k] + (size_t)i * fb;
		i = (uint16_t)(i - seg_frames[k]);
	}
	return NULL;
}

/* --- The session (radio task) --- */

static struct {
	uint32_t since;			/* the phase (LISTEN: this frequency) began */
	uint32_t now;			/* the last poll */
	uint32_t last_heard;		/* recording: a frame or the call, last */
	uint32_t dst : 24;		/* the over's talkgroup or unit */
	uint32_t phase : 3, side : 1, group : 1, id_pending : 2, id_side : 2, listens : 2;
	uint32_t id_last;		/* the last CW ID */
	uint16_t rec;			/* frames recorded */
	uint16_t rec_frames;		/* recording: frames heard in the over (cut ones too; stops counting there) */
	uint16_t pos;			/* sending: frames out */
	bool rx_open : 1, rx_ended : 1, cut : 1, cut_told : 1;	/* recording */
	bool keyed : 1, drained : 1, unkeyed : 1, on_air : 1;	/* sending */
	bool id_ever : 1, id_keyed : 1, id_aired : 1;		/* the CW ID */
} s;

bool xband_begin(uint32_t now)
{
	if (!want_start || active)
		return false;
	want_start = false;
	if (xband_check(&req) != XB_R_OK) {
		view.ended = XB_E_REFUSED;
		touch();
		return false;
	}
	memset(&s, 0, sizeof s);
	s.now = now;
	s.listens = xband_listens(&req) & 3u;
	fb = xband_frame_bytes(&req);
	active = true;
	want_stop = false;
	view.phase = XB_P_LISTEN;
	view.side = s.listens & 1u ? 0 : 1;
	view.ended = XB_E_NONE;
	view.cut = false;
	view.frames = 0;
	view.capacity = 0;
	view.relayed = view.ids = 0;
	touch();
	return true;
}

void xband_memory(uint8_t *const seg[], const size_t len[], uint8_t n, uint32_t now)
{
	segs = 0;
	capacity = 0;
	for (uint8_t k = 0; k < n && k < XB_SEGMENTS; k++) {
		uint16_t f = xband_frames_for(len[k]);
		if (seg[k] == NULL || f == 0)
			continue;
		seg_at[segs] = seg[k];
		seg_frames[segs] = f;
		capacity = (uint16_t)(capacity + f);
		segs++;
	}
	view.capacity = capacity;
	if (capacity < XB_MIN_FRAMES) {
		view.ended = XB_E_MEMORY;	/* the poll ends it */
		want_stop = true;
	}
	s.since = now;
	touch();
}

void xband_release(void)
{
	segs = 0;
	capacity = 0;
	memset(seg_at, 0, sizeof seg_at);
	fb = XB_FRAME_BYTES;
}

bool xband_owns_radio(void)
{
	return active;
}

void xband_ptt(void)
{
	/* nothing to ask: the stop key ends it (the radio core plays the deny tone) */
}

static void phase(uint8_t p, uint8_t side)
{
	s.phase = p;
	s.side = side;
	s.since = s.now;
	view.phase = p;
	view.side = side;
	touch();
}

static void listen(const xband_io_t *io, uint8_t side)
{
	if (!(s.listens & (1u << side)))
		side = (uint8_t)(1u - side);	/* a pair built one way: only its source side is heard */
	s.rx_open = s.rx_ended = s.cut = s.cut_told = false;
	s.rec = 0;
	s.rec_frames = 0;
	view.cut = false;
	view.frames = 0;
	phase(XB_P_LISTEN, side);
	io->tune(side);
}

/* The recording's frames, as the call stack hands them over (RECORD only) */
void xband_rx_begin(uint32_t src, uint32_t dst, bool group, bool encrypted)
{
	(void)src;
	uint32_t want = req.side[s.side].rx_tg;
	if (!active || s.phase != XB_P_LISTEN || encrypted || capacity == 0 ||
	    (req.mode[s.side] == XB_MODE_DMR && want != 0 && dst != want))
		return;		/* sending, identifying, a call we can't relay (encrypted: no clear voice), or filtered out */
	s.rx_open = true;
	s.rx_ended = false;
	s.dst = dst & 0xFFFFFFu;
	s.group = group;
	s.rec = 0;
	s.rec_frames = 0;
	s.last_heard = s.now;
	s.phase = XB_P_RECORD;
	s.since = s.now;
	view.phase = XB_P_RECORD;
	view.frames = 0;
	touch();
}

bool xband_nac_ok(uint16_t nac)
{
	uint16_t want = req.side[s.side].rx_nac;
	return want == XB_NAC_ANY || nac == want;
}

void xband_rx_frame(const uint8_t *frame)
{
	if (!active || s.phase != XB_P_RECORD || !s.rx_open)
		return;
	s.last_heard = s.now;
	if (s.cut)
		return;
	s.rec_frames++;
	if (s.rec >= capacity || s.rec_frames > XB_OVER_MAX_MS / XB_FRAME_MS) {
		s.cut = true;		/* the poll beeps; the rest of the over is heard, not kept */
		return;
	}
	memcpy(frame_at(s.rec), frame, fb);
	s.rec++;
	view.frames = s.rec;
}

void xband_rx_end(void)
{
	if (active && s.phase == XB_P_RECORD && s.rx_open)
		s.rx_ended = true;
}

bool xband_tx_next(uint8_t *frame)
{
	if (!active || s.phase != XB_P_SEND || !s.keyed || s.drained)
		return false;
	if (s.pos >= s.rec) {
		s.drained = true;	/* the poll lets go */
		return false;
	}
	memcpy(frame, frame_at(s.pos), fb);
	s.pos++;
	view.frames = (uint16_t)(s.rec - s.pos);
	return true;
}

static bool id_due(void)
{
	return s.id_pending != 0 && (!s.id_ever || s.now - s.id_last >= XB_ID_EVERY_MS);
}

/* The CW ID on the next side waiting for one; false when none is left */
static bool id_next(const xband_io_t *io)
{
	while (s.id_side < 2u) {
		uint8_t k = s.id_side++;
		if (!(s.id_pending & (1u << k)))
			continue;
		phase(XB_P_ID, k);
		s.id_aired = false;
		s.id_keyed = io->cw(k);
		if (s.id_keyed)
			return true;
		io->beep(XB_BEEP_ERROR);	/* refused there: the other side's still goes */
	}
	return false;
}

/* After an over or the IDs: the CW ID when due, else listening (on the side last transmitted on: the reply) */
static void after_tx(const xband_io_t *io, uint8_t side)
{
	if (s.phase != XB_P_ID && id_due()) {
		s.id_side = 0;
		if (id_next(io))
			return;
	}
	if (s.phase == XB_P_ID) {
		s.id_ever = true;
		s.id_last = s.now;
		s.id_pending = 0;
	}
	listen(io, side);
}

static void end(uint8_t why)
{
	active = false;
	want_stop = false;
	view.phase = XB_P_OFF;
	if (view.ended == XB_E_NONE)
		view.ended = why;
	touch();
}

void xband_poll(const xband_io_t *io, const xband_in_t *in, uint32_t now)
{
	s.now = now;
	if (!active)
		return;
	if (want_stop || in->battery_critical) {
		if (s.phase == XB_P_SEND && s.keyed && !s.unkeyed)
			io->unkey();	/* on the air (or about to be): let go at once */
		end(in->battery_critical ? XB_E_BATTERY : XB_E_STOPPED);
		return;
	}
	if (s.phase == XB_P_OFF) {		/* the first poll: on A (on B when only B is heard) */
		listen(io, 0);
		return;
	}
	switch (s.phase) {
	case XB_P_LISTEN:
		if (id_due() && !in->rx && !in->activity && !in->tx) {
			after_tx(io, s.side);	/* an ID still owed (one was refused, or the 10 minutes came round) */
			break;
		}
		if (in->rx || in->activity)
			s.since = now;		/* hold this frequency while something is heard */
		else if (now - s.since >= XB_DWELL_MS && s.listens == 3u)
			listen(io, (uint8_t)(1u - s.side));	/* (one side heard: it stays there) */
		break;
	case XB_P_RECORD:
		if (s.cut && !s.cut_told) {
			s.cut_told = true;
			view.cut = true;
			io->beep(XB_BEEP_CUT);
			touch();
		}
		if (in->rx)
			s.last_heard = now;
		if (!s.rx_ended && now - s.last_heard < XB_REC_QUIET_MS)
			break;
		if (s.rec < XB_MIN_FRAMES) {
			listen(io, s.side);	/* a key-up: nothing to relay */
			break;
		}
		/* the over is whole: to the other frequency */
		s.pos = 0;
		s.keyed = s.drained = s.unkeyed = s.on_air = false;
		s.rx_open = false;
		phase(XB_P_SEND, (uint8_t)(1u - s.side));
		view.frames = s.rec;
		io->tune(s.side);
		{
			uint32_t tg = req.side[s.side].tx_tg;	/* the talkgroup set for this side, else the one heard */
			s.keyed = io->key(s.side, tg != 0 ? tg : s.dst, tg != 0 || s.group);
		}
		if (!s.keyed) {
			io->beep(XB_BEEP_ERROR);	/* the radio would not transmit there: the over is dropped */
			listen(io, (uint8_t)(1u - s.side));
		}
		break;
	case XB_P_SEND: {
		uint32_t length = (uint32_t)s.rec * XB_FRAME_MS + XB_SEND_SLACK_MS;
		if (in->tx)
			s.on_air = true;
		if (s.drained && !s.unkeyed) {
			s.unkeyed = true;
			io->unkey();
		}
		if (!in->tx && now - s.since >= 100u) {
			/* off the air: the over is out (or the radio cut it: its time-out, a refusal) */
			bool whole = s.drained;
			if (!s.unkeyed) {
				s.unkeyed = true;
				io->unkey();
			}
			if (whole)
				view.relayed++;
			else
				io->beep(XB_BEEP_ERROR);
			if (s.on_air)
				s.id_pending |= (uint8_t)(1u << s.side);	/* this frequency owes the CW ID */
			after_tx(io, s.side);
		} else if (now - s.since >= length && !s.unkeyed) {
			s.unkeyed = true;	/* the frames should be out: stop waiting for the call stack */
			io->unkey();
		}
		break;
	}
	case XB_P_ID:
		if (s.id_keyed && (in->tx || in->id_wait)) {
			s.id_aired = s.id_aired || in->tx;
			break;
		}
		if (s.id_keyed) {
			s.id_keyed = false;
			if (s.id_aired) {
				view.ids++;
				touch();
			} else {
				io->beep(XB_BEEP_ERROR);	/* it never went on the air (refused after a wait) */
			}
		}
		if (!id_next(io))
			after_tx(io, s.side);
		break;
	default:
		break;
	}
}
#endif
