/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus faster searches (CONFIG_SEARCH_HYBRID, radio/searchhy.h, docs/plus.md "Faster searches (beta.9
 * candidate)"): the engines step by step on the fast algorithm against classic (beta.8's) in the same build: the DMR
 * quiet step in two polls from the 10 ms tick's samples (classic three), a direct mode carrier still found whatever its
 * phase, a steady carrier's DMR probe ended at DMRS_EARLY_MS (a TDMA one keeps the long window), a steady carrier not
 * digital twice skipped SH_REJECT_MS, a carrier gone during its probe read again between steps (watch), the algorithm
 * latched at the start, and the counters the PC reads (searchlog_view.c X.st). Built by tools/test_searchfix.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio/p25search.h"
#include "radio/dmrsearch.h"
#include "radio/searchhy.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define POLL 25u
#define F 433500000u
static uint32_t now = 100000u, phase;

typedef int16_t (*band_t)(uint32_t hz);
static uint32_t off(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }
static int16_t quiet(uint32_t hz) { (void)hz; return -124; }
static int16_t strong(uint32_t hz)
{
	uint32_t o = off(hz, F);
	return o == 0 ? -60 : o == 12500u ? -90 : o == 25000u ? -105 : -124;
}
/* A direct mode carrier: on air 30 ms in every 60 from `phase` */
static int16_t tdma(uint32_t hz) { return ((now + phase) / 30u) % 2u == 0 ? strong(hz) : -124; }
/* A carrier on F alone (no skirt on its neighbours), steady or TDMA */
static int16_t lone(uint32_t hz) { return hz == F ? -60 : -124; }
static int16_t lone_tdma(uint32_t hz) { return ((now + phase) / 30u) % 2u == 0 ? lone(hz) : -124; }
/* A weak signal on F (-105 dBm) just below a strong FM carrier on F + 25 kHz (its skirt 30 / 45 dB down) */
static int16_t fmnext(uint32_t hz)
{
	uint32_t o = off(hz, F + 25000u);
	int16_t c = o == 0 ? -60 : o == 12500u ? -90 : o == 25000u ? -105 : -124;
	return hz == F && c < -105 ? -105 : c;
}

/* One poll of the DMR engine with the 10 ms tick's samples before it (the radio task: tick at 3, 13, 23 ms ...) */
static uint32_t dmr_poll(band_t b, bool *hit)
{
	uint32_t until = now + POLL;
	for (uint32_t t = now + 1u; t < until; t++)
		if (t % 10u == 3u) {
			uint32_t keep = now;
			now = t;
			if (dmrs_sampling(now))
				dmrs_sample(b(dmrs_at_hz()), now);
			now = keep;
		}
	now = until;
	dmrs_view_t v;
	dmrs_snapshot(&v);	/* the screen looks */
	return dmrs_sweep(b(dmrs_at_hz()), now, hit);
}

static uint32_t dmr_sweep(band_t b, unsigned max)
{
	for (unsigned i = 0; i < max; i++) {
		bool hit;
		dmr_poll(b, &hit);
		if (hit)
			return dmrs_at_hz();
	}
	return 0;
}

static uint32_t dmr_hold(band_t b, uint16_t bursts, uint32_t ms)
{
	uint32_t t0 = now;
	while (now - t0 < ms) {
		dmrs_view_t v;
		dmrs_snapshot(&v);
		now += POLL;
		dmrs_heard_t h;
		memset(&h, 0, sizeof h);
		h.cc = DMRS_CC_NONE;
		h.bursts = bursts;
		h.call = bursts >= 2u;
		h.rssi_dbm = b(dmrs_at_hz());
		if (dmrs_probe(&h, now) != 0)
			return now - t0;
	}
	return 0;
}

static sh_stats_t stats(uint8_t owner)
{
	sh_stats_t v;
	memset(&v, 0, sizeof v);
	CHECK(sh_view(owner, true, now, &v));
	return v;
}

static void test_dmr_step(void)
{
	dmrs_view_t v;
	/* a quiet step: two polls fast, three classic */
	sh_set_fast(true);
	dmrs_start(433000000u, 434000000u, 12500u, now);
	CHECK(dmr_sweep(quiet, 400u) == 0 && dmrs_snapshot(&v));
	CHECK(v.step_ms >= 49u && v.step_ms <= 51u);
	sh_stats_t s = stats(SH_DMR);
	CHECK(s.fast && s.passes >= 2u && s.pass_steps == 81u && s.pass_ms >= 81u * 49u && s.pass_ms <= 81u * 51u);
	CHECK(s.probes == 0 && s.sweep_ms >= 9900u && s.probe_ms == 0);
	sh_set_fast(false);
	CHECK(sh_fast());	/* latched: the running search keeps its algorithm */
	dmrs_start(433000000u, 434000000u, 12500u, now);
	CHECK(!sh_fast());
	CHECK(dmr_sweep(quiet, 400u) == 0 && dmrs_snapshot(&v));
	CHECK(v.step_ms >= 74u && v.step_ms <= 76u);
	s = stats(SH_DMR);
	CHECK(!s.fast && s.pass_steps == 81u);
	sh_stats_t p;
	CHECK(!sh_view(SH_P25, true, now, &p) && !sh_view(SH_ALL, true, now, &p));	/* the DMR search owns them */
	sh_set_fast(true);

	/* a direct mode carrier is found on its channel whatever its TDMA phase (fast's samples meet a burst) */
	unsigned found = 0;
	for (phase = 0; phase < 60u; phase += 3u) {
		dmrs_start(F - 200000u, F + 200000u, 12500u, now);
		uint32_t got = dmr_sweep(tdma, 2000u);
		found += got == F;
		if (got != F)
			printf("tdma phase %u got %u\n", phase, got);
		dmrs_stop();
	}
	CHECK(found == 20u);
	phase = 0;
}

static void test_dmr_probe(void)
{
	/* a steady carrier, no burst: fast leaves at DMRS_EARLY_MS (counted early and not synced), classic at
	 * DMRS_PROBE_MS */
	sh_set_fast(true);
	dmrs_start(433000000u, 434000000u, 12500u, now);
	CHECK(dmr_sweep(strong, 4000u) == F);
	uint32_t ms = dmr_hold(strong, 0, 5000u);
	CHECK(ms >= DMRS_EARLY_MS && ms <= DMRS_EARLY_MS + POLL);
	sh_stats_t s = stats(SH_DMR);
	CHECK(s.probes == 1u && s.nosync == 1u && s.early == 1u && s.found == 0 && s.probe_ms >= DMRS_EARLY_MS);
	/* a TDMA carrier, no burst: the long window, not early */
	dmrs_start(433000000u, 434000000u, 12500u, now);
	CHECK(dmr_sweep(tdma, 4000u) == F);
	ms = dmr_hold(tdma, 0, 5000u);
	CHECK(ms >= DMRS_PROBE_LONG_MS && ms <= DMRS_PROBE_LONG_MS + POLL);
	CHECK(stats(SH_DMR).early == 0);
	sh_set_fast(false);
	dmrs_start(433000000u, 434000000u, 12500u, now);
	CHECK(dmr_sweep(strong, 4000u) == F);
	ms = dmr_hold(strong, 0, 5000u);
	CHECK(ms >= DMRS_PROBE_MS && ms <= DMRS_PROBE_MS + POLL);
	CHECK(stats(SH_DMR).early == 0);

	/* found: the counters (first find, seen to found, sync) */
	sh_set_fast(true);
	uint32_t t0 = now;
	dmrs_start(433000000u, 434000000u, 12500u, now);
	CHECK(dmr_sweep(strong, 4000u) == F);
	CHECK(dmr_hold(strong, 1, 100u) == 0);	/* one burst: synced */
	CHECK(dmr_hold(strong, 3, 100u) == 0);	/* DMR */
	s = stats(SH_DMR);
	CHECK(s.found == 1u && s.first_ms > 0 && s.first_ms <= now - t0 && s.seen_ms > 0 && s.seen_ms < 1000u);
	CHECK(s.sync_last >= POLL && s.sync_last <= 2u * POLL && s.sync_max == s.sync_last);
	CHECK(dmr_hold(quiet, 0, 5000u) != 0);	/* the call ended, the hang ran out */
	s = stats(SH_DMR);
	CHECK(s.nosync == 0 && s.gone == 0 && s.partial == 0 && !s.probing);
	dmrs_stop();
}

static void test_dmr_reject(void)
{
	/* a steady carrier not DMR twice in a row: fast skips it SH_REJECT_MS, classic DMRS_SKIP_LONG_MS */
	for (int fast = 1; fast >= 0; fast--) {
		sh_set_fast(fast);
		dmrs_start(F - 100000u, F + 100000u, 12500u, now);
		for (int k = 0; k < 2; k++) {
			CHECK(dmr_sweep(lone, 40000u) == F);
			CHECK(dmr_hold(lone, 0, 5000u) != 0);
		}
		/* sweep 60 s: classic probes it again after its 30 s, fast not */
		uint32_t t0 = now;
		bool again = false;
		while (now - t0 < 60000u && !again)
			again = dmr_sweep(lone, 1u) == F;
		CHECK(again == !fast);
		if (again)
			CHECK(now - t0 >= DMRS_SKIP_LONG_MS - 1000u);
		CHECK(stats(SH_DMR).skipped > 0);
		dmrs_stop();
	}
	/* ... a TDMA carrier with no burst keeps classic's 30 s (it may be DMR the modem missed) */
	sh_set_fast(true);
	dmrs_start(F - 100000u, F + 100000u, 12500u, now);
	for (int k = 0; k < 2; k++) {
		CHECK(dmr_sweep(lone_tdma, 40000u) == F);
		CHECK(dmr_hold(lone_tdma, 0, 5000u) != 0);
	}
	uint32_t t0 = now;
	bool again = false;
	while (now - t0 < 60000u && !again)
		again = dmr_sweep(lone_tdma, 1u) == F;
	CHECK(again);
	dmrs_stop();

	/* a weak signal just below a strong FM carrier: the FM carrier's run takes it in and its peak is probed (not DMR,
	 * skipped); fast watches the superseded run and probes F on its own */
	for (int fast = 1; fast >= 0; fast--) {
		sh_set_fast(fast);
		dmrs_start(F - 200000u, F + 200000u, 12500u, now);
		bool met = false;
		uint32_t t1 = now;
		while (now - t1 < 60000u && !met) {
			uint32_t got = dmr_sweep(fmnext, 1u);
			if (got != 0 && getenv("HY_DEBUG"))
				printf("fmnext fast %d probe %u at %u ms\n", fast, got, now - t1);
			if (got == F)
				met = true;
			else if (got != 0)
				CHECK(dmr_hold(fmnext, 0, 5000u) != 0);	/* the FM carrier (or a skirt step): not DMR */
		}
		if (fast)
			CHECK(met);	/* (classic may meet it too, on a later pass: the model compares how often) */
		dmrs_stop();
	}
}

/* A carrier keyed in overs on G (not in the found list of the tests before: its revisits would read it) */
#define G 437300000u
static bool on_air;
static int16_t over(uint32_t hz) { return on_air && hz == G ? -60 : -124; }	/* no skirt: the watch alone */

static void test_dmr_watch(void)
{
	/* the over ends during the probe (gone): fast reads it again between steps, well before the pass comes back
	 * (10 MHz: 801 steps, 40 s); classic waits for the pass */
	for (int fast = 1; fast >= 0; fast--) {
		sh_set_fast(fast);
		dmrs_start(430000000u, 440000000u, 12500u, now);
		on_air = true;
		while (dmr_sweep(over, 1u) != G)
			;
		on_air = false;
		CHECK(dmr_hold(over, 0, 5000u) != 0);
		sh_stats_t s = stats(SH_DMR);
		CHECK(s.gone == 1u && s.nosync == 0);
		on_air = true;	/* the next over */
		uint32_t t0 = now;
		bool met = false;
		while (now - t0 < 30000u && !met)
			met = dmr_sweep(over, 1u) == G;
		if (getenv("HY_DEBUG"))
			printf("watch fast %d met %d after %u ms, watch %u probes %u\n", fast, met, now - t0,
			       stats(SH_DMR).watch, stats(SH_DMR).probes);
		if (fast) {
			CHECK(met && now - t0 < 3000u);	/* a revisit within 32 steps */
			CHECK(stats(SH_DMR).watch == 1u);
		} else {
			CHECK(!met);	/* the pass is 60 s */
			CHECK(stats(SH_DMR).watch == 0);
		}
		dmrs_stop();
	}
	/* watched carriers are read strongest first, each once a round, and dropped after SH_WATCH_MS */
	sh_start(SH_DMR, now);
	sh_unsettled(431000000u, now);
	sh_seen(432000000u, -60, now);
	sh_unsettled(432000000u, now);
	sh_seen(433000000u, -90, now);
	sh_end(SH_END_GONE, false, 433000000u, now);
	extern bool fake_skip(uint32_t, uint32_t);
	CHECK(sh_watch_pick(430000000u, 440000000u, now, fake_skip) == 432000000u);
	CHECK(sh_watch_pick(430000000u, 440000000u, now, fake_skip) == 433000000u);
	CHECK(sh_watch_pick(430000000u, 440000000u, now, fake_skip) == 431000000u);
	CHECK(sh_watch_pick(430000000u, 440000000u, now, fake_skip) == 432000000u);	/* a new round */
	CHECK(sh_watch_pick(431500000u, 440000000u, now, fake_skip) != 431000000u);	/* out of the range */
	CHECK(sh_watch_pick(430000000u, 440000000u, now + SH_WATCH_MS + 300u, fake_skip) == 0);
}

bool fake_skip(uint32_t hz, uint32_t t) { (void)t; return hz == 0; }

/* --- P25 --- */
static uint32_t p25_sweep(band_t b, unsigned max)
{
	for (unsigned i = 0; i < max; i++) {
		p25s_view_t v;
		p25s_snapshot(&v);
		bool hit;
		now += POLL;
		p25s_sweep(b(p25s_at_hz()), now, &hit);
		if (hit)
			return p25s_at_hz();
	}
	return 0;
}

static void test_p25(void)
{
	/* not P25 twice: fast skips it for minutes; found: the counters */
	for (int fast = 1; fast >= 0; fast--) {
		sh_set_fast(fast);
		p25s_start(F - 100000u, F + 100000u, 12500u, now);
		for (int k = 0; k < 2; k++) {
			CHECK(p25_sweep(lone, 40000u) == F);
			for (int i = 0; i < 48; i++)
				p25s_level(lone(F), now += POLL);
			p25s_probe_done(false, now);
		}
		uint32_t t0 = now;
		bool again = false;
		while (now - t0 < 60000u && !again)
			again = p25_sweep(lone, 1u) == F;
		CHECK(again == !fast);
		sh_stats_t s = stats(SH_P25);
		CHECK(s.nosync == 2u + (again ? 0u : 0u) && s.probes >= 2u && s.fast == (fast != 0));
		p25s_stop();
	}
	sh_set_fast(true);
	p25s_start(F - 100000u, F + 100000u, 12500u, now);
	CHECK(p25_sweep(strong, 4000u) == F);
	now += 400u;
	p25s_probe_found(0x293u, 101u, 1234u, now);
	sh_stats_t s = stats(SH_P25);
	CHECK(s.found == 1u && s.sync_last >= 400u && s.first_ms > 0);
	p25s_probe_done(true, now += 2000u);
	CHECK(stats(SH_P25).probing == false);
	sh_stats_t d;
	CHECK(!sh_view(SH_DMR, true, now, &d));
	p25s_stop();
}

int main(void)
{
	test_dmr_step();
	test_dmr_probe();
	test_dmr_reject();
	test_dmr_watch();
	test_p25();
	printf("Plus faster searches (fast vs classic: DMR quiet step, direct mode found at every phase, early end, long "
	       "reject, watch, latch, counters): %d failures\n", failures);
	return failures != 0;
}
