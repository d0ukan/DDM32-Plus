/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus P25 search engine (p25search.c): stepping and wrap, settle, the floor and hits, the probe outcomes
 * (P25 found -> found list with NAC/TG/source, no P25 -> skipped for a while), the found list order and limits. */
#include <stdio.h>
#include "radio/p25search.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t now = 1000;

/* Sweep until a hit or `max` steps; `level(hz)` is the reading at each step. Returns the hit frequency or 0 */
static uint32_t run(int16_t (*level)(uint32_t), unsigned max)
{
	for (unsigned i = 0; i < max * 4u; i++) {
		bool hit;
		p25s_view_t look;
		p25s_snapshot(&look);	/* the screen looks */
		now += 10;
		p25s_sweep(level(p25s_at_hz()), now, &hit);
		if (hit)
			return p25s_at_hz();
	}
	return 0;
}

static int16_t quiet(uint32_t hz) { (void)hz; return -124; }
static int16_t one(uint32_t hz) { return hz == 433500000u ? -80 : -124; }
static int16_t two(uint32_t hz) { return hz == 433500000u || hz == 433025000u ? -90 : -123; }
static int16_t weak(uint32_t hz) { return hz == 433500000u ? -118 : -124; }

/* Sweep (quiet) to `hz` and make it a hit there: the probe holds it */
static void probe_at(uint32_t hz)
{
	p25s_view_t v;
	bool hit = false;
	for (unsigned i = 0; i < 20000u && p25s_at_hz() != hz; i++) {
		p25s_snapshot(&v);
		p25s_sweep(-124, now += 40, &hit);
	}
	p25s_sweep(-70, now += 40, &hit);
	CHECK(hit && p25s_at_hz() == hz && p25s_phase() == P25S_PROBE);
}

/* The found list's entries for one frequency */
static unsigned entries_at(const p25s_view_t *v, uint32_t hz)
{
	unsigned n = 0;
	for (unsigned i = 0; i < v->found_n; i++)
		n += v->found[i].hz == hz;
	return n;
}

static const p25s_found_t *entry(const p25s_view_t *v, uint32_t hz, uint16_t nac, uint16_t tg)
{
	for (unsigned i = 0; i < v->found_n; i++)
		if (v->found[i].hz == hz && v->found[i].nac == nac && v->found[i].tg == tg)
			return &v->found[i];
	return NULL;
}

/* The PC's search log: the calls counted since power on (p25s_peek's seq) */
static uint16_t calls_seq(void)
{
	p25s_peek_t pk;
	(void)p25s_peek(0u, &pk);
	return pk.seq;
}

/* The found list keyed by frequency + NAC + talkgroup (owner 2026-11-21, beta.7 candidate: 159.125 NAC $178 TG 1 was
 * listed twice, one entry with 1 call and one with 2) */
static void test_found_key(void)
{
	const uint32_t F = 159125000u;
	p25s_view_t v;
	const p25s_found_t *e;
	p25s_start(159000000u, 160000000u, 12500u, now);
	/* The duplicate: a call, then the receiver's NAC reads "any" (0xFFFF) after the terminator or a damaged frame.
	 * That report used to start a second entry for the same frequency */
	probe_at(F);
	p25s_probe_found(0x178, 1, 1, now);
	p25s_probe_found(0xFFFFu, 1, 1, now += 180);	/* the terminator */
	p25s_probe_found(0xFFFFu, 0, 0, now += 180);
	CHECK(p25s_snapshot(&v) && entries_at(&v, F) == 1 && v.found[0].nac == 0x178 && v.found[0].tg == 1 &&
	      v.found[0].calls == 1);
	p25s_probe_done(true, now += 1500);
	/* The next call (a revisit or the next pass): the same entry counts it, whatever comes first in the reports */
	now += 10000u;
	probe_at(F);
	p25s_probe_found(0xFFFFu, 0, 0, now);		/* no NAC yet: the entry that fits, no new one */
	p25s_probe_found(0x178, 0, 0, now += 180);	/* NAC, no link control yet */
	p25s_probe_found(0x178, 1, 1, now += 180);
	CHECK(p25s_snapshot(&v) && entries_at(&v, F) == 1 && (e = entry(&v, F, 0x178, 1)) && e->calls == 2 &&
	      e == &v.found[0]);
	p25s_probe_done(true, now += 1500);
	/* Another talkgroup on the same frequency and NAC: its own entry, on top */
	now += 10000u;
	probe_at(F);
	p25s_probe_found(0x178, 2, 5, now);
	CHECK(p25s_snapshot(&v) && entries_at(&v, F) == 2 && v.found[0].tg == 2 && v.found[0].calls == 1 &&
	      v.found[0].src == 5 && (e = entry(&v, F, 0x178, 1)) && e->calls == 2);
	p25s_probe_done(true, now += 1500);
	/* The link control comes late: the call is first counted on the newest entry that fits (TG 2), then moves to
	 * TG 1's when its talkgroup is heard */
	now += 10000u;
	probe_at(F);
	uint16_t seq0 = calls_seq();
	p25s_probe_found(0x178, 0, 0, now);
	CHECK(p25s_snapshot(&v) && v.found[0].tg == 2 && v.found[0].calls == 2);
	CHECK(calls_seq() == (uint16_t)(seq0 + 1u));
	p25s_probe_found(0x178, 1, 9, now += 360);
	CHECK(p25s_snapshot(&v) && entries_at(&v, F) == 2 && v.found[0].tg == 1 && v.found[0].calls == 3 &&
	      v.found[0].src == 9 && (e = entry(&v, F, 0x178, 2)) && e->calls == 1);
	/* the PC's search log: the moved call is still ONE call (seq), the two entries keep their own serials */
	CHECK(calls_seq() == (uint16_t)(seq0 + 1u) && v.found[0].id != e->id && v.found[0].id != 0 && e->id != 0);
	/* ... and a talkgroup change later in the same hold is a call of that talkgroup (not moved) */
	p25s_probe_found(0x178, 2, 5, now += 4000);
	CHECK(p25s_snapshot(&v) && entries_at(&v, F) == 2 && v.found[0].tg == 2 && v.found[0].calls == 2 &&
	      (e = entry(&v, F, 0x178, 1)) && e->calls == 3);
	p25s_probe_done(true, now += 1500);
	/* Another NAC on the same frequency: its own entry */
	now += 10000u;
	probe_at(F);
	p25s_probe_found(0x293, 1, 1, now);
	CHECK(p25s_snapshot(&v) && entries_at(&v, F) == 3 && v.found[0].nac == 0x293 && v.found[0].calls == 1);
	p25s_probe_done(true, now += 1500);
	/* A frequency new to the list: no entry before a NAC is known */
	probe_at(159500000u);
	p25s_probe_found(0xFFFFu, 0, 0, now);
	CHECK(p25s_snapshot(&v) && entries_at(&v, 159500000u) == 0 && v.playing);
	p25s_probe_found(0x111, 0, 0, now += 180);
	CHECK(p25s_snapshot(&v) && entries_at(&v, 159500000u) == 1 && v.found[0].nac == 0x111 && v.found[0].tg == 0 &&
	      v.found[0].calls == 1);
	uint16_t id0 = v.found[0].id;
	p25s_probe_found(0x111, 77, 3, now += 180);	/* its talkgroup: the same entry */
	CHECK(p25s_snapshot(&v) && entries_at(&v, 159500000u) == 1 && v.found[0].tg == 77 && v.found[0].calls == 1);
	CHECK(v.found[0].id == id0);	/* the same serial: the PC moves its row to the talkgroup, no second row */
	p25s_probe_done(true, now += 1500);
	/* Full: a new key on top, the oldest drops; never two entries of one key */
	for (unsigned k = 0; k < P25S_FOUND_MAX; k++) {
		probe_at(159600000u + 12500u * k);
		p25s_probe_found((uint16_t)(0x200 + k), 1, 1, now);
		p25s_probe_done(true, now += 1500);
	}
	CHECK(p25s_snapshot(&v) && v.found_n == P25S_FOUND_MAX && v.found[0].nac == 0x200 + P25S_FOUND_MAX - 1u &&
	      v.found[P25S_FOUND_MAX - 1u].nac == 0x200 && entries_at(&v, F) == 0);
	for (unsigned i = 0; i < v.found_n; i++)
		for (unsigned j = i + 1u; j < v.found_n; j++)
			CHECK(!(v.found[i].hz == v.found[j].hz && v.found[i].nac == v.found[j].nac &&
				v.found[i].tg == v.found[j].tg));
}

int main(void)
{
	p25s_view_t v;
	CHECK(!p25s_on());
	/* 433.000 .. 434.000 in 12.5 kHz: 81 steps */
	CHECK(p25s_start(433000000u, 434000000u, 12500u, now) == 433000000u && p25s_on() && p25s_phase() == P25S_SWEEP);
	bool hit;
	CHECK(p25s_sweep(-124, now + P25S_SETTLE_MS - 1u, &hit) == 0 && !hit);	/* settling */
	CHECK(run(quiet, 200) == 0);
	CHECK(p25s_snapshot(&v) && v.sweeps >= 2 && v.hits == 0 && v.floor_dbm >= -126 && v.floor_dbm <= -122);

	/* A carrier at 433.500: a hit, then the probe finds P25 */
	CHECK(run(one, 200) == 433500000u && p25s_phase() == P25S_PROBE);
	CHECK(p25s_sweep(-80, now + 1000u, &hit) == 0 && !hit);	/* probing: the sweep waits */
	p25s_probe_found(0x1C8, 655, 1234567, now);
	CHECK(p25s_snapshot(&v) && v.playing && v.found_n == 1 && v.found[0].hz == 433500000u && v.found[0].nac == 0x1C8 &&
	      v.found[0].tg == 655 && v.found[0].src == 1234567 && v.found[0].calls == 1 && v.probes_p25 == 1);
	now += 500;
	p25s_probe_found(0x1C8, 0, 0, now);	/* later reports keep what was known */
	CHECK(p25s_snapshot(&v) && v.found[0].tg == 655 && v.found[0].calls == 1);
	CHECK(p25s_probe_done(true, now) == 433512500u && p25s_phase() == P25S_SWEEP);
	/* P25 frequencies are probed again on the next pass (calls come and go) */
	CHECK(run(one, 200) == 433500000u);
	now += 5000;
	p25s_probe_found(0x1C8, 655, 7654321, now);
	CHECK(p25s_snapshot(&v) && v.found_n == 1 && v.found[0].calls == 2 && v.found[0].src == 7654321);
	p25s_probe_done(true, now);

	/* A carrier that isn't P25 (or another NAC can't be heard): skipped for P25S_SKIP_MS */
	uint32_t first = run(two, 200);		/* 433.5 may come first: it was found, so it is revisited */
	CHECK(first == 433025000u || first == 433500000u);
	p25s_probe_done(false, now);
	uint32_t second = run(two, 200);
	CHECK((second == 433025000u || second == 433500000u) && second != first);	/* the other one, not again */
	p25s_probe_done(false, now);
	CHECK(run(two, 100) == 0);		/* both skipped now: a whole sweep (81 steps) in about 4 s, no hit */
	now += P25S_SKIP_MS;
	CHECK(run(two, 200) != 0);		/* after the skip time: probed again */
	p25s_probe_done(false, now);

	/* Weak bumps under P25S_HIT_MIN_DBM are no hit */
	p25s_start(433000000u, 434000000u, 12500u, now);
	CHECK(run(weak, 300) == 0);

	/* The found list: newest first, at most P25S_FOUND_MAX, survives a restart */
	p25s_start(430000000u, 440000000u, 12500u, now);
	CHECK(p25s_snapshot(&v) && v.found_n == 1);
	for (unsigned k = 0; k < 10u; k++) {
		uint32_t hz = 430000000u + 25000u * k;
		while (p25s_at_hz() != hz) {
			p25s_snapshot(&v);
			now += 40;
			p25s_sweep(-124, now, &hit);
		}
		now += 40;
		p25s_sweep(-70, now, &hit);
		CHECK(hit);
		p25s_probe_found((uint16_t)(0x100 + k), (uint16_t)k, k, now);
		p25s_probe_done(true, now);
	}
	CHECK(p25s_snapshot(&v) && v.found_n == P25S_FOUND_MAX && v.found[0].nac == 0x109 && v.found[7].nac == 0x102);
	/* Revisits: a found frequency in the range is read again every P25S_REVISIT_STEPS steps, the sweep goes on
	 * where it was, and a new call there is caught within about that many steps (not a whole sweep) */
	p25s_start(432500000u, 434500000u, 12500u, now);	/* 161 steps; 430.xxx found entries are out of range */
	/* note 433.5 as found in this range */
	while (p25s_at_hz() != 433500000u) {
		p25s_snapshot(&v);
		p25s_sweep(-124, now += 40, &hit);
	}
	p25s_sweep(-70, now += 40, &hit);
	CHECK(hit);
	p25s_probe_found(0x659, 101, 77285, now);
	CHECK(p25s_probe_done(true, now) == 433512500u);	/* the sweep goes on from the next step */
	unsigned visits = 0, steps = 0, gap = 0, max_gap = 0;
	uint32_t sweeps0 = (p25s_snapshot(&v), v.sweeps), last = 0;
	bool order_ok = true;
	while (steps < 400u) {
		p25s_snapshot(&v);
		now += 40;
		uint32_t at = p25s_at_hz();
		uint32_t next = p25s_sweep(-124, now, &hit);
		if (next == 0)
			continue;	/* settling */
		CHECK(!hit);
		steps++;
		if (at == 433500000u) {
			visits++;
			if (gap > max_gap)
				max_gap = gap;
			gap = 0;
			if (last == 433487500u)
				last = 433500000u;	/* the sweep's own step there, not a revisit */
		} else {
			gap++;
			if (last != 0 && at != 432500000u && at != last + 12500u)
				order_ok = false;	/* sweep steps stay in order around the revisits */
			last = at;
		}
	}
	CHECK(visits >= 400u / (P25S_REVISIT_STEPS + 1u) && max_gap <= P25S_REVISIT_STEPS + 1u && order_ok);
	CHECK(p25s_snapshot(&v) && v.sweeps > sweeps0);	/* the sweep still wraps */
	/* A call starts on 433.5: caught within a revisit period */
	unsigned waited = 0;
	for (hit = false; !hit && waited < 200u; ) {
		p25s_snapshot(&v);
		now += 40;
		if (p25s_sweep(p25s_at_hz() == 433500000u ? -70 : -124, now, &hit) != 0 || hit)
			waited++;
	}
	CHECK(hit && p25s_at_hz() == 433500000u && waited <= P25S_REVISIT_STEPS + 2u);
	uint32_t back = p25s_probe_done(true, now);
	CHECK(back != 0 && back != 433500000u && p25s_phase() == P25S_SWEEP);	/* the sweep resumes where it was */
	/* The PC's search log (p25s_peek): the frequency just found, one entry at a time */
	{
		p25s_peek_t pk;
		bool seen = false;
		for (unsigned i = 0; p25s_peek(i, &pk); i++)
			seen |= pk.f.hz == 433500000u;
		CHECK(seen && pk.found_n > 0 && !p25s_peek(P25S_FOUND_MAX, &pk));
	}
	test_found_key();
	/* Nobody looks (the screen was left): the search stops by itself, also while the PC reads it */
	p25s_start(433000000u, 434000000u, 12500u, now);
	uint32_t t0 = now, e0 = p25s_epoch();
	p25s_peek_t pk;
	while (p25s_on() && now - t0 < 20000u) {
		(void)p25s_peek(0u, &pk);	/* the PC's search log reads it: not a look */
		p25s_sweep(-124, now += 10, &hit);
	}
	CHECK(!p25s_on() && now - t0 >= P25S_IDLE_MS && now - t0 < P25S_IDLE_MS + 50u && p25s_epoch() == e0);
	/* reversed range, stop */
	CHECK(p25s_start(434000000u, 433000000u, 0, now) == 433000000u);
	p25s_stop();
	CHECK(!p25s_on() && p25s_probe_done(true, now) == 0);
	printf("Plus P25 search: sweep/wrap, floor, hits, probe found/not, skips, found list: %d failures\n", failures);
	return failures ? 1 : 0;
}
