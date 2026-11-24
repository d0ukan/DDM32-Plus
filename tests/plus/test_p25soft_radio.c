/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus P25 soft probe, the radio task side and the engine together (radio/radio_p25soft.c, radio/p25search.c
 * with SEARCH_FAST, SURE, HYBRID and SOFTP25; docs/p25-soft-probe-design.md), with a fake board: the HR_C7000 FM RX
 * ring delivers the model's 8 kHz discriminator samples (p25soft_model.inc) as the clock runs, the flat path and the
 * RTOS heap are counted. The radio task's order is kept: every 25 ms poll runs radio_p25soft_poll, then the search's
 * block (the sweep, or the level while probing), then the native receiver's take when the probe is its.
 *
 * Checks: off (the default) and classic with the soft probe wanted leave the probe to the native receiver at once,
 * nothing allocated or started (classic's behaviour); P25 is found with its NAC long before the native window and
 * the native receiver then gets a clean board (ring stopped, flat path off, heap freed) and adds the talkgroup to the
 * same entry, never skipped even when the native receiver hears nothing; FM speech on a strong carrier is "not P25"
 * after the window and skipped like a probe that heard nothing, without the native receiver; a weak carrier, no heap,
 * a flat path that fails, a ring that brings nothing: the native receiver as before; the search stopped or the channel
 * tuned midway gives everything back. Host-modelled only. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio/p25search.h"
#include "radio/searchhy.h"
#include "radio/radio_p25soft.h"
#include "radio/radio_nxdn.h"
#include "board.h"
#include "FreeRTOS.h"
#include "p25cai.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#include "p25soft_model.inc"

#define POLL 25u
#define F 433500000u
static uint32_t now = 100000u;

/* --- the fake board --- */
static const int16_t *sig;	/* the air on F (8 kHz), looped */
static size_t sig_n, sig_at;	/* where the ring starts reading */
static bool ring_on, ring_dead, flat_on, flat_fail, heap_fail;
static uint32_t ring_t0, ring_out, ring_starts, live_allocs;

static bool fake_rx_start(void)
{
	ring_on = true;
	ring_t0 = now;
	ring_out = 0;
	ring_starts++;
	return true;
}
static void fake_rx_stop(void) { ring_on = false; }
static uint16_t fake_rx_read(int16_t *buf, uint16_t max)
{
	if (!ring_on || ring_dead || sig == NULL)
		return 0;
	uint32_t made = (now - ring_t0) * 8u;
	uint16_t n = 0;
	while (n < max && ring_out < made) {
		buf[n++] = sig[(sig_at + ring_out) % sig_n];
		ring_out++;
	}
	return n;
}
static const hal_afsk_t fake_afsk = { 8000u, fake_rx_start, fake_rx_stop, fake_rx_read };
board_t board = { &fake_afsk };

static bool aprs_on;	/* APRS receive holds the ring (an FM channel on the APRS frequency, DOTA over FM) */
bool aprs_rx_listening(void) { return aprs_on; }

bool board_nxdn_flat(bool on)
{
	if (on && flat_fail)
		return false;
	flat_on = on;
	return true;
}
void *pvPortMalloc(size_t n)
{
	if (heap_fail)
		return NULL;
	live_allocs++;
	return malloc(n);
}
void vPortFree(void *p)
{
	if (p != NULL)
		live_allocs--;
	free(p);
}

/* --- the radio task --- */
static int16_t level_on_f = -84;	/* the carrier on F (the floor learns -124) */
static int16_t band(uint32_t hz) { return hz == F ? level_on_f : -124; }
static uint32_t native_took;		/* when the native receiver took the probe, 0 = not yet */
static uint32_t probe_t0;		/* when the probe began */

static void poll(void)
{
	now += POLL;
	p25s_view_t v;
	p25s_snapshot(&v);	/* the screen looks */
	radio_p25soft_poll(now);
	if (!p25s_on())
		return;
	if (p25s_phase() == P25S_SWEEP) {
		bool hit;
		p25s_sweep(band(p25s_at_hz()), now, &hit);
		if (hit)
			probe_t0 = now;
	} else if (p25s_phase() == P25S_PROBE) {
		p25s_level(band(p25s_at_hz()), now);
		if (!p25s_soft_pending() && native_took == 0) {
			/* the native receiver takes it: the board must be as after a sweep step */
			CHECK(!ring_on);
			CHECK(!flat_on);
			CHECK(live_allocs == 0);
			native_took = now;
		}
	}
}

/* Start a search around F and run until a probe of F begins */
static void to_probe(void)
{
	native_took = probe_t0 = 0;
	p25s_start(F - 100000u, F + 100000u, 12500u, now);
	for (unsigned i = 0; i < 2000u && !(p25s_phase() == P25S_PROBE && p25s_at_hz() == F); i++)
		poll();
	CHECK(p25s_phase() == P25S_PROBE && p25s_at_hz() == F);
}

/* Polls until the soft probe decided (or `ms` passed) */
static void until_decided(unsigned ms)
{
	for (unsigned t = 0; t < ms && p25s_soft_pending(); t += POLL)
		poll();
}

static int16_t *speech(size_t *n)
{
	size_t m = (size_t)(4 * FSH);
	double *dev = calloc(m, sizeof *dev), f1 = 500, f2 = 1200, a1 = 1500, a2 = 800, p1 = 0, p2 = 0, pc = 0;
	size_t next = 0;
	for (size_t i = 0; i < m; i++) {
		if (i == next) {
			f1 = 300 + rnd() % 1200u;
			f2 = 900 + rnd() % 2100u;
			a1 = rnd() % 1800u;
			a2 = rnd() % 900u;
			next = i + (size_t)(FSH * (0.04 + (rnd() % 160u) / 1000.0));
		}
		p1 += 2 * M_PI * f1 / FSH;
		p2 += 2 * M_PI * f2 / FSH;
		pc += 2 * M_PI * 88.5 / FSH;
		dev[i] = a1 * sin(p1) + a2 * sin(p2) + 400 * sin(pc);
	}
	chan_t c = { 30.0, 150.0, 6.0, 0, 1 };
	int16_t *s = radio(dev, m, &c, n);
	free(dev);
	return s;
}

/* P25 whose syncs are clean but whose NIDs are random dibits (a receive path that blurs the symbols after the sync
 * more than the model does): syncs found, no NID decodes */
static int16_t *p25_bad_nid(uint16_t nac, unsigned ldus, const chan_t *c, size_t *n)
{
	static const uint8_t sync[24] = { 1, 1, 1, 1, 1, 3, 1, 1, 3, 3, 1, 1, 3, 3, 3, 3, 1, 3, 1, 3, 3, 3, 3, 3 };
	uint8_t *dib = malloc(200000);
	size_t nd = p25_call(nac, ldus, dib), nm;
	for (size_t i = 0; i + 57u <= nd; i++)
		if (memcmp(dib + i, sync, sizeof sync) == 0)
			for (size_t k = 24u; k < 57u; k++)
				if (k != 35u)
					dib[i + k] = (uint8_t)(rnd() & 3u);
	double *dev = c4fm(dib, nd, &nm);
	int16_t *s = radio(dev, nm, c, n);
	free(dib);
	free(dev);
	return s;
}

static sh_stats_t stats(void)
{
	sh_stats_t v;
	memset(&v, 0, sizeof v);
	sh_view(SH_P25, false, now, &v);
	return v;
}

static void reset(void)
{
	p25s_stop();
	poll();
	ring_dead = flat_fail = heap_fail = aprs_on = false;
	level_on_f = -84;
	sh_set_fast(true);
}

int main(void)
{
	design();
	size_t p25n, fmn;
	chan_t c = { 30.0, -250.0, 5.0, 1, 1 };
	int16_t *p25 = p25_signal(0x659, 40, &c, &p25n), *fm = speech(&fmn);

	/* Off (the default): the native receiver takes the probe at once, nothing of the soft probe runs */
	sig = p25;
	sig_n = p25n;
	sh_set_fast(true);
	CHECK(!p25s_soft_wanted());
	to_probe();
	CHECK(!p25s_soft_pending());
	poll();
	CHECK(native_took != 0 && native_took - probe_t0 <= POLL);
	CHECK(ring_starts == 0 && live_allocs == 0 && !flat_on);
	p25s_probe_done(true, now);
	reset();

	/* Classic with the soft probe wanted: classic, unchanged */
	p25s_set_soft(true);
	sh_set_fast(false);
	to_probe();
	CHECK(!p25s_soft_pending());
	poll();
	CHECK(native_took != 0 && ring_starts == 0);
	p25s_probe_done(false, now);
	reset();

	/* P25 on a strong carrier: found with its NAC, the native receiver gets a clean board and adds the talkgroup */
	unsigned worst = 0, total = 0;
	for (unsigned trial = 0; trial < 20u; trial++) {
		sig_at = rnd() % p25n;
		to_probe();
		CHECK(p25s_soft_pending());
		until_decided(1000);
		poll();
		CHECK(native_took != 0);
		unsigned ms = native_took - probe_t0;
		worst = ms > worst ? ms : worst;
		total += ms;
		p25s_view_t v;
		p25s_snapshot(&v);
		CHECK(v.found_n >= 1 && v.found[0].hz == F && v.found[0].nac == 0x659);
		uint8_t n0 = v.found_n;
		p25s_probe_found(0x659, 101, 1234567u, now);	/* the native receiver's call */
		p25s_snapshot(&v);
		CHECK(v.found_n == n0 && v.found[0].tg == 101 && v.found[0].src == 1234567u);
		/* the native receiver then heard nothing more: still P25, never skipped */
		p25s_probe_done(false, now);
		for (unsigned i = 0; i < 400u && !(p25s_phase() == P25S_PROBE && p25s_at_hz() == F); i++)
			poll();
		CHECK(p25s_phase() == P25S_PROBE && p25s_at_hz() == F);	/* probed again on the next pass */
		reset();
	}
	printf("p25soft radio: P25 handed to the native receiver after %u ms on average, %u ms at worst (classic: at once, "
	       "then the native receiver's own lock; its window 1200 ms)\n", total / 20u, worst);
	CHECK(worst <= P25SOFT_SKIP_MS + 250u + POLL);

	/* FM speech on a strong carrier: not P25 after the window, skipped as a probe that heard nothing */
	sig = fm;
	sig_n = fmn;
	sig_at = 0;
	to_probe();
	sh_stats_t s0 = stats();
	until_decided(1000);
	CHECK(!p25s_soft_pending() && native_took == 0);
	CHECK(p25s_phase() == P25S_SWEEP);
	unsigned waited = now - probe_t0;
	printf("p25soft radio: FM speech judged not P25 after %u ms (classic: the native receiver's 1200 ms window and "
	       "the hand-overs)\n", waited);
	CHECK(waited >= P25SOFT_WINDOW_MS && waited <= P25SOFT_WINDOW_MS + 2u * POLL);
	CHECK(stats().nosync == s0.nosync + 1u);
	CHECK(!ring_on && !flat_on && live_allocs == 0);
	/* the next pass passes it over (skipped 10 s) */
	unsigned probes = 0;
	for (unsigned i = 0; i < 300u; i++) {	/* 7.5 s: more than a pass of 17 steps */
		poll();
		if (p25s_phase() == P25S_PROBE) {
			probes++;
			break;
		}
	}
	CHECK(probes == 0);
	reset();

	/* A weak carrier (15 dB over the floor): the native receiver at once, nothing started */
	level_on_f = -109;
	uint32_t starts = ring_starts;
	to_probe();
	poll();
	CHECK(native_took != 0 && native_took - probe_t0 <= 2u * POLL && ring_starts == starts);
	p25s_probe_done(false, now);
	reset();

	/* No heap, a flat path that fails: the native receiver as before */
	heap_fail = true;
	to_probe();
	poll();
	CHECK(native_took != 0 && ring_starts == starts && live_allocs == 0);
	p25s_probe_done(false, now);
	reset();
	flat_fail = true;
	to_probe();
	poll();
	CHECK(native_took != 0 && live_allocs == 0 && !ring_on);
	p25s_probe_done(false, now);
	reset();

	/* APRS receive holds the ring: the soft probe leaves it alone (no start, no stop), the native receiver probes */
	aprs_on = true;
	starts = ring_starts;
	to_probe();
	poll();
	CHECK(native_took != 0 && native_took - probe_t0 <= 2u * POLL && ring_starts == starts && live_allocs == 0);
	p25s_probe_done(false, now);
	reset();

	/* Syncs seen but no NID decodes: not "not P25" (never skipped by the soft probe), the native receiver decides */
	{
		size_t bn;
		int16_t *bad = p25_bad_nid(0x659, 40, &c, &bn);
		sig = bad;
		sig_n = bn;
		sig_at = 0;
		to_probe();
		s0 = stats();
		until_decided(1000);
		poll();
		CHECK(native_took != 0);
		CHECK(stats().nosync == s0.nosync);
		p25s_probe_done(false, now);
		reset();
		sig = p25;
		sig_n = p25n;
		free(bad);
	}

	/* A ring that brings nothing: not enough samples for "not P25", so the native receiver decides */
	ring_dead = true;
	to_probe();
	until_decided(1000);
	poll();
	CHECK(native_took != 0);
	p25s_probe_done(false, now);
	reset();

	/* The search stopped midway, or the channel tuned: everything given back */
	to_probe();
	poll();
	poll();
	CHECK(ring_on && flat_on && live_allocs == 1);
	p25s_stop();
	poll();
	CHECK(!ring_on && !flat_on && live_allocs == 0);
	reset();
	to_probe();
	poll();
	CHECK(ring_on);
	radio_p25soft_release();
	CHECK(!ring_on && !flat_on && live_allocs == 0);
	reset();

	free(p25);
	free(fm);
	if (failures) {
		printf("test_p25soft_radio: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_p25soft_radio: OK\n");
	return 0;
}
