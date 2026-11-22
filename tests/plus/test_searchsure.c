/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus searchfix (CONFIG_SEARCH_SURE, radio/searchsure.h, docs/plus.md "Search reliability"): the P25 and DMR
 * (and all digital) search engines step by step: the floor learned from the first step (a receiver reading -112 dBm
 * quiet makes no hits), the run followed to its middle (the peak of a strong carrier with its neighbours raised, a
 * plateau, the half step between two equal steps), the skirt not probed after the probe, a carrier gone off the air
 * not skipped, a direct mode carrier held DMRS_PROBE_LONG_MS, something heard never skipped, skips within half a step,
 * the steps on the channel grid, a wide flat run taken as the floor, the steps a second, the all digital search's
 * DMR first for a TDMA carrier. Built with SEARCH_FAST and SEARCH_SURE by tools/test_searchfix.py. */
#include <stdio.h>
#include <string.h>
#include "radio/p25search.h"
#include "radio/dmrsearch.h"
#if CONFIG_SEARCH_HYBRID
#include "radio/searchhy.h"	/* fastsrch: run on classic in the hybrid build (SEARCHHY_CLASSIC, tools/test_searchfix.py) */
#endif

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define POLL 25u
#define F 433500000u
static uint32_t now = 100000u;

/* The band: a level for a frequency at now */
typedef int16_t (*band_t)(uint32_t hz);
static int16_t quiet(uint32_t hz) { (void)hz; return -124; }
static int16_t high(uint32_t hz) { (void)hz; return -112; }
static uint32_t off(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }
/* A strong carrier at F with its neighbours raised (12.5 kHz 30 dB, 25 kHz 45 dB, 37.5 kHz 55 dB down) */
static int16_t strong(uint32_t hz)
{
	uint32_t o = off(hz, F);
	return o == 0 ? -50 : o == 12500u ? -80 : o == 25000u ? -95 : o == 37500u ? -105 : -124;
}
/* A direct mode carrier (30 ms in 60) with the same neighbours */
static int16_t strong_tdma(uint32_t hz) { return (now / 30u) % 2u == 0 ? strong(hz) : -124; }
/* A signal between two steps (H + 6.25 kHz; not F: a find there is read again on its own): both steps read 6 dB down */
#define H 433700000u
static int16_t half(uint32_t hz) { return hz == H + 6250u ? -64 : hz == H || hz == H + 12500u ? -70 : -124; }
/* An overloaded front end: the ceiling on five steps around F */
static int16_t plateau(uint32_t hz) { return off(hz, F) <= 25000u ? -45 : off(hz, F) <= 50000u ? -80 : -124; }
/* The band's level raised far and wide (a radio close by desensing it): 20 steps flat at -100 */
static int16_t wide(uint32_t hz) { return hz >= 433200000u && hz < 433450000u ? -100 : -124; }
/* dmrs3: ... with a carrier 3 dB over it inside the first 16 steps (433.3875 MHz) */
#define W 433387500u
static int16_t wide_peak(uint32_t hz) { return hz == W ? -97 : wide(hz); }
/* An even plateau at the ceiling (F - 12.5 .. F + 25 kHz): its middle is a half step, 6.25 kHz off any channel */
static int16_t plateau4(uint32_t hz) { return hz >= F - 12500u && hz <= F + 25000u ? -45 : -124; }
/* Five strong steps (F - 37.5 .. F + 12.5 kHz), F 4 dB over the others: F, not the run's middle (F - 12.5 kHz) */
static int16_t peak_run(uint32_t hz) { return hz == F ? -46 : hz >= F - 37500u && hz <= F + 12500u ? -50 : -124; }

/* --- DMR --- */
static unsigned dmr_hits(void)
{
	dmrs_view_t v;
	dmrs_snapshot(&v);
	return v.hits;
}

/* Sweep until a hit (or max polls); returns the frequency probed, 0 none */
static uint32_t dmr_sweep(band_t b, unsigned max)
{
	for (unsigned i = 0; i < max; i++) {
		dmrs_view_t v;
		dmrs_snapshot(&v);
		bool hit;
		now += POLL;
		dmrs_sweep(b(dmrs_at_hz()), now, &hit);
		if (hit)
			return dmrs_at_hz();
	}
	return 0;
}

/* Hold the probe: what the stack hears (bursts, call) and the level; returns ms until it moved on (0 = still held) */
static uint32_t dmr_hold(band_t b, uint16_t bursts, bool call, uint32_t ms)
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
		h.call = call;
		h.rssi_dbm = b(dmrs_at_hz());
		if (dmrs_probe(&h, now) != 0)
			return now - t0;
	}
	return 0;
}

static void test_dmr(void)
{
	dmrs_view_t v;
	/* A receiver whose quiet reading is -112 dBm: no hit on a whole pass (the old floor started at -125 and only
	 * quiet steps moved it: every step was a hit), the floor learned */
	dmrs_start(433000000u, 434000000u, 12500u, now);
	CHECK(dmr_sweep(high, 81u * 3u * 2u) == 0 && dmrs_snapshot(&v) && v.hits == 0 && v.floor_dbm >= -113 &&
	      v.floor_dbm <= -111);
	CHECK(v.step_ms >= 74u && v.step_ms <= 77u);	/* a quiet DMR step: three polls */
	/* ... started on a strong step: the floor comes down within a few steps and the signal is found next pass */
	dmrs_start(F, F + 500000u, 12500u, now);
	for (unsigned i = 0; i < 40u; i++)
		dmr_sweep(strong, 3u);
	CHECK(dmrs_snapshot(&v) && v.floor_dbm <= -118);

	/* A strong carrier with its neighbours raised 20-40 dB over the floor: the probe goes to its peak, never to a
	 * neighbour; after the call the neighbours above it are its skirt (not probed) */
	dmrs_start(433000000u, 434000000u, 12500u, now);
	CHECK(dmr_sweep(strong, 4000u) == F && dmr_hits() == 1);
	CHECK(dmr_hold(strong, 20, true, 3000u) == 0);	/* DMR heard, the call plays */
	CHECK(dmr_hold(quiet, 20, false, 5000u) != 0 && dmrs_phase() == DMRS_SWEEP && dmrs_at_hz() == F + 12500u);
	CHECK(dmr_sweep(strong, 12u) == 0 && dmr_hits() == 1 && dmrs_at_hz() >= F + 50000u);	/* (later a revisit) */
	/* the same with a direct mode carrier (its readings rise and fall) */
	dmrs_start(433000000u, 434000000u, 12500u, now);
	CHECK(dmr_sweep(strong_tdma, 4000u) == F);
	CHECK(dmr_hold(strong_tdma, 20, true, 1000u) == 0);

	/* Between two steps: the probe on the half step (H + 6.25 kHz) */
	dmrs_start(433000000u, 434000000u, 12500u, now);
	{ uint32_t got = dmr_sweep(half, 4000u); if (got != H + 6250u) printf("half got %u\n", got); CHECK(got == H + 6250u); }
	/* not DMR (a carrier that stays, no burst): left after DMRS_PROBE_MS, skipped, and its two steps with it */
	uint32_t ms = dmr_hold(half, 0, false, 5000u);
	CHECK(ms >= DMRS_PROBE_MS && ms <= DMRS_PROBE_MS + POLL);
	CHECK(dmr_sweep(half, 81u * 4u) == 0);	/* a whole pass: the half step is skipped (within half a step) */

	/* An overloaded front end, five steps at the ceiling: the middle one */
	dmrs_start(433000000u, 434000000u, 12500u, now);
	CHECK(dmr_sweep(plateau, 4000u) == F);
	CHECK(dmr_hold(plateau, 20, true, 500u) == 0);

	/* The over ends during the probe (the level falls to the floor): on at once, nothing skipped, the next pass
	 * probes it again */
	dmrs_start(433000000u, 434000000u, 12500u, now);
	CHECK(dmr_sweep(strong, 4000u) == F);
	ms = dmr_hold(quiet, 0, false, 5000u);
	CHECK(ms >= 200u && ms < DMRS_PROBE_MS);
	CHECK(dmr_sweep(strong, 4000u) == F);
	/* A direct mode carrier without a burst yet: held DMRS_PROBE_LONG_MS (a voice call's sync once a superframe) */
	ms = dmr_hold(strong_tdma, 0, false, 5000u);
	CHECK(ms >= DMRS_PROBE_LONG_MS && ms <= DMRS_PROBE_LONG_MS + POLL);
	/* One burst (DMR, not named yet): the long window, never skipped */
	CHECK(dmr_sweep(strong, 4000u) == F || dmr_sweep(strong, 4000u) == F);
	dmrs_start(433000000u, 434000000u, 12500u, now);
	CHECK(dmr_sweep(strong, 4000u) == F);
	ms = dmr_hold(strong, 1, false, 5000u);
	CHECK(ms >= DMRS_PROBE_LONG_MS && ms <= DMRS_PROBE_LONG_MS + POLL);
	CHECK(dmr_sweep(strong, 81u * 4u * 3u) == F);	/* probed again on the next pass */

	/* dmrs3: an even plateau is probed on a channel step (F), never on its middle's half step */
	dmrs_start(433000000u, 434000000u, 12500u, now);
	{ uint32_t got = dmr_sweep(plateau4, 4000u); if (got != F) printf("plateau4 got %u\n", got); CHECK(got == F); }
	CHECK(dmr_hold(plateau4, 20, true, 500u) == 0);
	/* ... and a run's clearly strongest step is probed, not its middle */
	dmrs_start(433000000u, 434000000u, 12500u, now);
	{ uint32_t got = dmr_sweep(peak_run, 4000u); if (got != F) printf("peak_run got %u\n", got); CHECK(got == F); }
	CHECK(dmr_hold(peak_run, 20, true, 500u) == 0);

	/* A wide flat run (the band raised, no carrier): no probe, the floor takes its level */
	dmrs_start(433000000u, 434000000u, 12500u, now);
	unsigned h0 = dmr_hits();
	CHECK(dmr_sweep(wide, 81u * 3u * 2u) == 0 && dmr_hits() == h0);
	/* dmrs3 (hole hunt A3): a carrier 3 dB over such a band: the plateau's strongest step is probed, not taken as the
	 * floor (the beta.7 rule made the whole run the floor: the carrier was never probed); not DMR: skipped, and from then
	 * the floor takes the run's level */
	dmrs_start(433000000u, 434000000u, 12500u, now);
	{ uint32_t got = dmr_sweep(wide_peak, 81u * 3u * 2u); if (got != W) printf("wide_peak got %u\n", got); CHECK(got == W); }
	ms = dmr_hold(wide_peak, 0, false, 5000u);
	CHECK(ms >= DMRS_PROBE_MS && ms <= DMRS_PROBE_MS + POLL && dmrs_snapshot(&v) && v.floor_dbm >= -101);
	h0 = dmr_hits();
	CHECK(dmr_sweep(wide_peak, 81u * 3u) == 0 && dmr_hits() == h0);	/* the next pass: within its skip */

	/* The steps on the channel grid: a typed 436.010 starts at 436.0125 and reads 436.2750 */
	CHECK(dmrs_start(436010000u, 436500000u, 12500u, now) == 436012500u);
	bool met = false;
	for (unsigned i = 0; i < 200u; i++) {
		dmrs_snapshot(&v);
		bool hit;
		dmrs_sweep(-124, now += POLL, &hit);
		met |= dmrs_at_hz() == 436275000u;
	}
	CHECK(met);
	dmrs_stop();
}

#if CONFIG_ALL_SEARCH
static void test_all(void)
{
	/* A carrier whose readings rise and fall (TDMA): DMR probed first, not P25 */
	dmrs_start_all(433000000u, 434000000u, 12500u, now);
	bool hit = false;
	for (unsigned i = 0; i < 4000u && !hit; i++) {
		dmrs_view_t v;
		dmrs_snapshot_all(&v);
		now += POLL;
		dmrs_sweep(strong_tdma(dmrs_at_hz()), now, &hit);
	}
	CHECK(hit && dmrs_at_hz() == F && dmrs_mode() == DMRS_MODE_DMR);
	dmrs_stop();
	/* A steady carrier: P25 first (the owner's order) */
	dmrs_start_all(433000000u, 434000000u, 12500u, now);
	hit = false;
	for (unsigned i = 0; i < 4000u && !hit; i++) {
		dmrs_view_t v;
		dmrs_snapshot_all(&v);
		now += POLL;
		dmrs_sweep(strong(dmrs_at_hz()), now, &hit);
	}
	CHECK(hit && dmrs_at_hz() == F && dmrs_mode() == DMRS_MODE_P25);
	dmrs_stop();
}
#endif

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

/* The native receiver's probe of `ms` with the radio core reading the level; then given back (P25 or not) */
static void p25_probe(band_t b, bool p25, uint32_t ms)
{
	uint32_t t0 = now;
	while (now - t0 < ms && !(!p25 && p25s_carrier_gone(now))) {
		p25s_view_t v;
		p25s_snapshot(&v);
		now += POLL;
		p25s_level(b(p25s_at_hz()), now);
		if (p25)
			p25s_probe_found(0x293, 655, 1234, now);
	}
	p25s_probe_done(p25, now);
	p25s_retuned(now);
}

static void test_p25(void)
{
	p25s_view_t v;
	p25s_start(433000000u, 434000000u, 12500u, now);
	CHECK(p25_sweep(high, 81u * 3u) == 0 && p25s_snapshot(&v) && v.hits == 0 && v.floor_dbm >= -113 &&
	      v.floor_dbm <= -111 && v.step_ms >= 24u && v.step_ms <= 27u);
	p25s_start(433000000u, 434000000u, 12500u, now);
	CHECK(p25_sweep(strong, 4000u) == F && p25s_snapshot(&v) && v.hits == 1);
	p25_probe(strong, true, 1000u);
	CHECK(p25_sweep(strong, 8u) == 0 && p25s_snapshot(&v) && v.hits == 1 && p25s_at_hz() >= F + 50000u);	/* its skirt */
	p25s_start(433000000u, 434000000u, 12500u, now);
	CHECK(p25_sweep(half, 4000u) == H + 6250u);
	p25_probe(half, false, 1200u);	/* a carrier that stays, not P25: skipped */
	CHECK(p25_sweep(half, 81u * 2u) == 0);
	p25s_start(433000000u, 434000000u, 12500u, now);
	CHECK(p25_sweep(plateau, 4000u) == F);
	p25_probe(quiet, false, 1200u);	/* gone off the air during the probe: given back at once, not skipped */
	{ uint32_t got = p25_sweep(plateau, 81u * 4u); if (got != F) printf("p25 plateau got %u\n", got); CHECK(got == F); }
	p25_probe(plateau, true, 500u);
	/* dmrs3: the even plateau on a channel step; the strongest step of a wide run probed */
	p25s_start(433000000u, 434000000u, 12500u, now);
	CHECK(p25_sweep(plateau4, 4000u) == F);
	p25_probe(plateau4, true, 500u);
	p25s_start(433000000u, 434000000u, 12500u, now);
	{ uint32_t got = p25_sweep(wide_peak, 81u * 4u); if (got != W) printf("p25 wide_peak got %u\n", got); CHECK(got == W); }
	p25_probe(wide_peak, true, 500u);
	CHECK(p25s_start(436010000u, 436500000u, 12500u, now) == 436012500u);
	p25s_stop();
}

int main(void)
{
#if CONFIG_SEARCH_HYBRID
	sh_set_fast(false);
#endif
	test_dmr();
#if CONFIG_ALL_SEARCH
	test_all();
#endif
	test_p25();
	printf("Plus surer searches (floor, run middle, half step, plateau, skirt, gone, long window, skips, grid, wide run, "
	       "steps a second, even plateau, strongest step, a wide run's peak probed%s): %d failures\n", CONFIG_ALL_SEARCH ? ", all digital DMR first" : "", failures);
	return failures != 0;
}
