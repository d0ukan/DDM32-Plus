/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus faster searches: the counters and the candidate memory the search engines share (searchhy.h) */
#include "config.h"
#if CONFIG_SEARCH_HYBRID && (CONFIG_P25_SEARCH || CONFIG_DMR_SEARCH)
#include <string.h>
#include "radio/searchhy.h"

enum { C_USED = 1u, C_WATCH = 2u, C_VISITED = 4u };

typedef struct {
	uint32_t hz;
	uint16_t first_q;	/* first read strong, ms / 16 (held at most FIRST_MAX_Q back: no wrap) */
	uint16_t last_q;	/* last read strong, ms / 256 (wraps after 4.6 h) */
	int8_t lvl;		/* the strongest reading, dBm */
	uint8_t st;		/* C_* */
} cand_t;

static sh_stats_t st;
static cand_t cand[SH_CANDS];
static bool want_fast = true;
static uint32_t probe_t0;

void sh_set_fast(bool fast) { want_fast = fast; }
bool sh_fast_wanted(void) { return want_fast; }
bool sh_fast(void) { return st.fast; }

static uint32_t age_ms(const cand_t *c, uint32_t now_ms)
{
	return (uint32_t)(uint16_t)((uint16_t)(now_ms >> 8) - c->last_q) << 8;
}

#define FIRST_MAX_Q 0xF000u	/* 16 min in ms / 16 */

static uint32_t first_age_ms(const cand_t *c, uint32_t now_ms)
{
	return (uint32_t)(uint16_t)((uint16_t)(now_ms >> 4) - c->first_q) << 4;
}

static uint32_t dist(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

/* The candidate within `within` Hz of hz (the nearest), NULL = none; forgotten ones are dropped on the way */
static cand_t *near_in(uint32_t hz, uint32_t within, uint32_t now_ms)
{
	cand_t *best = NULL;
	for (unsigned i = 0; i < SH_CANDS; i++) {
		cand_t *c = &cand[i];
		if ((c->st & C_USED) && age_ms(c, now_ms) >= SH_FORGET_MS)
			c->st = 0;
		if ((c->st & C_USED) && dist(c->hz, hz) <= within && (best == NULL || dist(c->hz, hz) < dist(best->hz, hz)))
			best = c;
	}
	return best;
}

static cand_t *near(uint32_t hz, uint32_t now_ms) { return near_in(hz, SH_NEAR_HZ, now_ms); }

/* Time so far into the probing or sweeping total */
static void mark(uint32_t now_ms, bool probing)
{
	uint32_t dt = now_ms - st.mark_ms;
	if (st.probing)
		st.probe_ms += dt;
	else
		st.sweep_ms += dt;
	st.mark_ms = now_ms;
	st.probing = probing;
}

void sh_start(uint8_t owner, uint32_t now_ms)
{
	memset(&st, 0, sizeof st);
	memset(cand, 0, sizeof cand);
	st.owner = owner;
	st.fast = want_fast;
	st.start_ms = st.mark_ms = st.pass_t0 = now_ms;
	st.cur_steps = 1;	/* the first step, read before any advance */
}

void sh_step(uint32_t now_ms)
{
	(void)now_ms;
	if (st.cur_steps < UINT16_MAX)
		st.cur_steps++;
}

void sh_pass(uint32_t now_ms)
{
	st.passes++;
	st.pass_ms = now_ms - st.pass_t0;
	st.pass_t0 = now_ms;
	st.pass_steps = st.cur_steps;
	st.cur_steps = 0;
}

void sh_seen(uint32_t hz, int16_t lvl, uint32_t now_ms)
{
	cand_t *c = near(hz, now_ms);
	int8_t l = (int8_t)(lvl < -128 ? -128 : lvl > 0 ? 0 : lvl);
	if (c == NULL) {
		c = &cand[0];	/* a free entry, else the one seen longest ago */
		for (unsigned i = 0; i < SH_CANDS; i++) {
			if (!(cand[i].st & C_USED)) {
				c = &cand[i];
				break;
			}
			if (age_ms(&cand[i], now_ms) > age_ms(c, now_ms))
				c = &cand[i];
		}
		c->hz = hz;
		c->first_q = (uint16_t)(now_ms >> 4);
		c->lvl = l;
		c->st = C_USED;
	} else {
		if (l > c->lvl)
			c->lvl = l;
		if ((uint16_t)((uint16_t)(now_ms >> 4) - c->first_q) > FIRST_MAX_Q)
			c->first_q = (uint16_t)((now_ms >> 4) - FIRST_MAX_Q);
	}
	c->last_q = (uint16_t)(now_ms >> 8);
}

/* hz watched: its own entry (a neighbour's within SH_NEAR_HZ stays as it is: two runs side by side are two) */
static void watch(uint32_t hz, uint32_t now_ms)
{
	cand_t *c = near_in(hz, SH_NEAR_HZ / 4u, now_ms);
	if (c == NULL) {
		cand_t *n = near(hz, now_ms);	/* first seen with the carrier it was read beside */
		uint16_t first = n != NULL ? n->first_q : (uint16_t)(now_ms >> 4);
		int8_t lvl = n != NULL ? n->lvl : -128;
		c = &cand[0];	/* a free entry, else the one seen longest ago */
		for (unsigned i = 0; i < SH_CANDS; i++) {
			if (!(cand[i].st & C_USED)) {
				c = &cand[i];
				break;
			}
			if (age_ms(&cand[i], now_ms) > age_ms(c, now_ms))
				c = &cand[i];
		}
		c->first_q = first;
		c->lvl = lvl;
		c->last_q = (uint16_t)(now_ms >> 8);
	}
	c->hz = hz;	/* the frequency probed (on the channel grid) is the one read again */
	c->st = (uint8_t)((c->st | C_USED | C_WATCH) & ~C_VISITED);
}

void sh_unsettled(uint32_t hz, uint32_t now_ms) { watch(hz, now_ms); }

void sh_skipped(void) { st.skipped++; }

void sh_probe(bool watch_hit, uint32_t now_ms)
{
	mark(now_ms, true);
	probe_t0 = now_ms;
	st.synced = false;
	st.probes++;
	if (watch_hit)
		st.watch++;
}

void sh_sync(uint32_t waited_ms)
{
	if (st.synced)
		return;
	st.synced = true;
	st.sync_last = (uint16_t)(waited_ms < UINT16_MAX ? waited_ms : UINT16_MAX);
}

void sh_found(uint32_t hz, uint32_t now_ms)
{
	st.found++;
	if (st.first_ms == 0)
		st.first_ms = now_ms - st.start_ms ? now_ms - st.start_ms : 1u;
	cand_t *c = near(hz, now_ms);
	st.seen_ms = c != NULL ? first_age_ms(c, now_ms) : now_ms - probe_t0;
	if (st.seen_ms == 0)
		st.seen_ms = 1;
	if (c != NULL)
		c->st = 0;	/* the found list reads it again from now (revisits) */
	if (!st.synced)
		sh_sync(now_ms - probe_t0);
	if (st.sync_last > st.sync_max)
		st.sync_max = st.sync_last;
}

void sh_end(uint8_t how, bool early, uint32_t hz, uint32_t now_ms)
{
	mark(now_ms, false);
	if (early)
		st.early++;
	cand_t *c;
	switch (how) {
	case SH_END_NOSYNC:
		st.nosync++;
		c = near_in(hz, SH_NEAR_HZ / 4u, now_ms);	/* this frequency's only (a weaker one watched below it stays) */
		if (c != NULL)
			c->st = 0;	/* the skip list has it */
		break;
	case SH_END_PARTIAL:
		st.partial++;
		watch(hz, now_ms);
		break;
	case SH_END_GONE:
		st.gone++;
		watch(hz, now_ms);
		break;
	default:
		break;
	}
}

uint32_t sh_watch_pick(uint32_t lo, uint32_t hi, uint32_t now_ms, bool (*skip)(uint32_t hz, uint32_t now_ms))
{
	for (unsigned round = 0; round < 2u; round++) {
		cand_t *best = NULL;
		bool any = false;
		for (unsigned i = 0; i < SH_CANDS; i++) {
			cand_t *c = &cand[i];
			if (!(c->st & C_WATCH))
				continue;
			if (age_ms(c, now_ms) >= SH_WATCH_MS || c->hz < lo || c->hz > hi) {
				c->st &= (uint8_t)~C_WATCH;
				continue;
			}
			if (skip(c->hz, now_ms))
				continue;
			any = true;
			if (!(c->st & C_VISITED) && (best == NULL || c->lvl > best->lvl))
				best = c;
		}
		if (best != NULL) {
			best->st |= C_VISITED;
			return best->hz;
		}
		if (!any)
			return 0;
		for (unsigned i = 0; i < SH_CANDS; i++)
			cand[i].st &= (uint8_t)~C_VISITED;	/* every watched one read again: a new round */
	}
	return 0;
}

bool sh_view(uint8_t owner, bool running, uint32_t now_ms, sh_stats_t *out)
{
	if (st.owner != owner || owner == SH_NONE)
		return false;
	*out = st;
	if (running) {
		uint32_t dt = now_ms - out->mark_ms;	/* the phase running now, so far */
		if (out->probing)
			out->probe_ms += dt;
		else
			out->sweep_ms += dt;
		out->mark_ms = now_ms;
	}
	return true;
}
#endif
