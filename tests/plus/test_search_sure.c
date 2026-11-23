/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus searchfix (docs/plus.md "Search reliability"): the P25, DMR and all digital search engines (p25search.c,
 * dmrsearch.c) driven as the radio task drives them (a poll every 25 ms +-1 ms) against a model of the band, the
 * receiver's strength reading and the probes' receivers. Built twice by tools/test_scan.py: CONFIG_SEARCH_SURE=0 (the
 * beta.7 engines: SEARCH_FAST on) and =1, the two compared. A scenario is run 200 times (random step, level, TDMA
 * phase, over / pause lengths, the native receiver's start); printed per engine and scenario:
 *   MODEL <engine> <scenario> <found in 60 s of 200> <median s to find> <90th pct s> <probes a minute> <steps/s>
 * The model:
 *   - the band: one transmitter of interest, keyed in overs (5-20 s) with pauses (3-10 s) from a random moment;
 *     DMR direct mode is on air 30 ms in every 60 (any phase); P25 and a DMR repeater all the time while keyed;
 *     optional FM carriers (always on, no digital mode) elsewhere;
 *   - the receiver: noise +-1 dB around the floor (-124 dBm, or -112 for a receiver whose quiet reading sits high),
 *     the level of a signal off the tuned frequency by the selectivity (6.25 kHz 6 dB, 12.5 kHz 30 dB, 25 kHz 45 dB,
 *     37.5 kHz 55 dB, 50 kHz 60 dB, further 80 dB), a reading ceiling (-45 dBm: an overloaded front end, a radio a
 *     metre away at 0 dBm reads the ceiling on every step within 50 kHz), a first-order lag tau after each retune;
 *   - the DMR modem: heard only within 1.5 kHz of a keyed DMR signal, from 60 ms after a retune (the all digital
 *     search's FM -> DMR switch: 150 ms); a direct mode over's syncs: three headers 60 ms apart, then once per 360 ms
 *     superframe, each caught with probability 0.85; from the first caught one a burst every 60 ms (the call's LC
 *     after 360 ms, at once from a header); a repeater's sync every 60 ms;
 *   - the native P25 receiver: hands over 50 ms after the hit, starts in 250-1000 ms (the zone scan's measured range
 *     is "under 700 ms"; the slow tail is the case that was skipped), a frame every 180 ms while keyed.
 * Looking into a case: SURE_DEBUG=<scenario> prints the misses, SURE_TRACE=<hz> the engine's phases for that
 * transmitter (SURE_VERBOSE=1 every reading within 100 kHz of it).
 * DDM32 Plus faster searches (CONFIG_SEARCH_HYBRID, radio/searchhy.h, branch fastsrch): built with the switch, the
 * first argument picks the algorithm ("fast" or "classic", the engine's name in the output); the radio task's 10 ms tick
 * hands the DMR engine its samples (dmrs_sampling / dmrs_sample) between the polls. After the 60 s rows above, rows
 * named "fpm-..." keep each search running 120 s and print, in the last column, the calls caught a minute (the engine's
 * seq: one per over found and counted). SURE_SEED / SURE_LEAK: the verifier's variants (gap_leak above). The rows after
 * the original ones (p25-rpt, p25-fmnext, *-wide10) are added at
 * the end of their group so the original rows' figures stay identical to the build without the switch. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio/p25search.h"
#include "radio/dmrsearch.h"
#if CONFIG_SEARCH_HYBRID
#include "radio/searchhy.h"
#endif

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

enum { M_P25, M_DMRDIR, M_DMRRPT, M_FM };
typedef struct {
	uint32_t hz;
	double dbm;
	int mode;
	uint32_t phase;		/* TDMA */
	uint32_t t0, over, pause;	/* keyed from t0 for `over`, then off `pause`, again (over 0: always on) */
} sig_t;

#define NSIG_MAX 8
static sig_t sigs[NSIG_MAX];
static int nsig;
static double tau, reading, floor_dbm = -124.0, ceiling = 0.0;
static uint32_t now, tuned_hz, filt_ms, tuned_at;
static unsigned seed = 4242u, case_seed = 991u;
static bool all_kind;
static bool keep_going;		/* fpm rows: the search runs on after a find */
/* fastsrch verifier variants: SURE_LEAK=<dB> makes a direct mode carrier's TDMA gap read that many dB under its burst
 * instead of the floor (a strong transmitter in the room, a level reading that averages over the gap); SURE_SEED=<n>
 * another random sequence (the engines' and the cases') */
static double gap_leak;
static uint16_t seq0, seq1;	/* the engine's calls counted at the run's start and end */
static const char *trace_name = "";
static uint32_t trace_hz;	/* SURE_TRACE=<hz>: the engine's phases for that transmitter */

static unsigned rnd(void)
{
	seed = seed * 1103515245u + 12345u;
	return (seed >> 16) & 0x7FFFu;
}

static unsigned crnd(void)
{
	case_seed = case_seed * 1103515245u + 12345u;
	return (case_seed >> 16) & 0x7FFFu;
}

static bool keyed(const sig_t *s, uint32_t t)
{
	if (s->over == 0)
		return true;
	if ((int32_t)(t - s->t0) < 0)
		return false;
	return (t - s->t0) % (s->over + s->pause) < s->over;
}

/* The start of the over keyed at t */
static uint32_t over_start(const sig_t *s, uint32_t t)
{
	if (s->over == 0)
		return 0;
	return t - (t - s->t0) % (s->over + s->pause);
}

static double att(uint32_t off)
{
	return off < 3125u ? 0.0 : off <= 6250u ? 6.0 : off <= 12500u ? 30.0 : off <= 25000u ? 45.0 :
	       off <= 37500u ? 55.0 : off <= 50000u ? 60.0 : 80.0;
}

static uint32_t offset(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

static double level(uint32_t t)
{
	double best = floor_dbm + (double)(rnd() % 21u) / 10.0 - 1.0;
	for (int i = 0; i < nsig; i++) {
		const sig_t *s = &sigs[i];
		if (!keyed(s, t))
			continue;
		double gapdb = 0.0;
		if (s->mode == M_DMRDIR && (t + s->phase) % 60u >= 30u) {
			if (gap_leak == 0.0)
				continue;
			gapdb = gap_leak;	/* the gap reads gap_leak dB under the burst */
		}
		double l = s->dbm - att(offset(tuned_hz, s->hz)) - gapdb;
		if (ceiling != 0.0 && l > ceiling)
			l = ceiling;
		if (l > best)
			best = l;
	}
	return best;
}

static int16_t read_now(void)
{
	double k = tau > 0.0 ? 1.0 - exp(-1.0 / tau) : 1.0;
	for (; filt_ms < now; filt_ms++)
		reading += (level(filt_ms) - reading) * k;
	return (int16_t)lround(reading);
}

static void retune(uint32_t hz)
{
	tuned_hz = hz;
	tuned_at = now;
}

/* The radio task's 10 ms tick between two polls (from the last poll to now): the DMR sweep's samples */
static uint32_t tick_from;
static void ticks(void)
{
#if CONFIG_SEARCH_HYBRID
	uint32_t until = now;
	for (uint32_t t = tick_from + 1u; t <= until; t++) {
		if (t % 10u != 7u)
			continue;
		now = t;
		if (dmrs_sampling(now))
			dmrs_sample(read_now(), now);
	}
	now = until;
#endif
	tick_from = now;
}

/* --- the DMR modem --- */
static uint32_t synced_at;	/* the first caught sync since the retune (0 none) */
static bool from_header;

static const sig_t *dmr_sig(void)
{
	for (int i = 0; i < nsig; i++)
		if ((sigs[i].mode == M_DMRDIR || sigs[i].mode == M_DMRRPT) && offset(tuned_hz, sigs[i].hz) <= 1500u)
			return &sigs[i];
	return NULL;
}

/* Walk the syncs from the last look to now; the first caught one starts the bursts */
static uint32_t dmr_looked;
static void dmr_modem(uint32_t ready_ms)
{
	const sig_t *s = dmr_sig();
	if (s == NULL || synced_at != 0) {
		dmr_looked = now;
		return;
	}
	for (uint32_t t = dmr_looked + 1u; t <= now; t++) {
		if (t < ready_ms || !keyed(s, t))
			continue;
		uint32_t o = t - over_start(s, t);
		bool sync, header = false;
		if (s->mode == M_DMRRPT)
			sync = (t + s->phase) % 60u == 0;
		else if (o < 180u)
			sync = header = o % 60u == 0;
		else
			sync = (o - 180u) % 360u == 0;
		if (sync && rnd() % 100u < 85u) {
			synced_at = t;
			from_header = header;
			break;
		}
	}
	dmr_looked = now;
}

static void dmr_heard(dmrs_heard_t *h)
{
	memset(h, 0, sizeof *h);
	h->cc = DMRS_CC_NONE;
	const sig_t *s = dmr_sig();
	if (s == NULL || synced_at == 0)
		return;
	uint32_t until = now;
	bool on = keyed(s, now);
	h->bursts = (uint16_t)(1u + (until - synced_at) / 60u);
	h->cc = 1;
	h->sync_mask = s->mode == M_DMRRPT ? DMRS_SYNC_BS : DMRS_SYNC_MS;
	if (on && s->mode == M_DMRDIR && (from_header || now - synced_at >= 360u)) {
		h->call = h->group = true;
		h->dst = 91;
		h->src = 2860001;
	}
	if (!on && s->mode == M_DMRDIR)
		synced_at = 0;	/* the over ended: the next over's sync again */
}

/* --- the native P25 receiver --- */
static uint32_t native_ready, native_frames, native_last;
static const sig_t *p25_sig(void)
{
	for (int i = 0; i < nsig; i++)
		if (sigs[i].mode == M_P25 && offset(tuned_hz, sigs[i].hz) <= 1500u)
			return &sigs[i];
	return NULL;
}

static void native_take(void)
{
	native_ready = now + 250u + (rnd() % 751u);
	native_frames = 0;
	native_last = 0;
}

static void native_run(void)
{
	const sig_t *s = p25_sig();
	if (s == NULL || now < native_ready || !keyed(s, now))
		return;
	uint32_t o = now - over_start(s, now);
	if (native_last == 0 || now - native_last >= 180u) {
		(void)o;
		native_frames++;
		native_last = now;
	}
}

/* --- results --- */
typedef struct {
	int found;
	double t[256];
	unsigned probes;
	double run_s;
	unsigned steps_q;	/* sweep steps (quiet-band pass) */
	double steps_s;
} res_t;

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

static void print_res(const char *eng, const char *name, res_t *r, int trials)
{
	qsort(r->t, (size_t)r->found, sizeof r->t[0], cmp_d);
	double med = r->found ? r->t[r->found / 2] : 99.0, p90 = r->found ? r->t[(r->found * 9) / 10 < r->found ?
										   (r->found * 9) / 10 : r->found - 1] : 99.0;
	if (r->found < trials * 9 / 10)
		p90 = 99.0;
	printf("MODEL %s %s %d %.1f %.1f %.1f %.1f %.1f\n", eng, name, r->found, med, p90,
	       r->run_s > 0 ? r->probes * 60.0 / r->run_s : 0.0, r->steps_s,
	       /* fastsrch: the median over every trial, a miss counted as 99 s (fair when the finds differ) */
	       trials / 2 < r->found ? r->t[trials / 2] : 99.0);
}

/* --- DMR / all digital search --- */
static bool dmr_found_at(uint32_t hz, uint32_t since)
{
	dmrs_view_t v;
#if CONFIG_ALL_SEARCH
	if (all_kind ? !dmrs_snapshot_all(&v) : !dmrs_snapshot(&v))
		return false;
#else
	if (!dmrs_snapshot(&v))
		return false;
#endif
	for (unsigned i = 0; i < v.found_n; i++)
		if (offset(v.found[i].hz, hz) <= 1500u && (int32_t)(v.found[i].heard_ms - since) >= 0)
			return true;
	return false;
}

static uint32_t ready;

/* The radio core set the receiver up for what the engine does now (a step, a mode's probe) */
static void dmr_setup(void)
{
	retune(dmrs_at_hz());
	synced_at = 0;
	dmr_looked = now;
	bool dmr_probe = dmrs_phase() != DMRS_SWEEP && dmrs_mode() == DMRS_MODE_DMR;
	ready = now + (all_kind && dmr_probe ? 150u : 60u);	/* the all digital search: the FM -> DMR switch */
#if CONFIG_ALL_SEARCH
	if (all_kind)
		dmrs_retuned(now);
#endif
}

#if CONFIG_ALL_SEARCH
/* One poll of the all digital search: the radio core (sweep, DMR / NXDN probe) or the native receiver (P25 / YSF) */
static bool native;
static uint32_t native_at;
static uint8_t epoch_seen;
static void all_poll(unsigned *nsteps)
{
	dmrs_view_t v;
	dmrs_snapshot_all(&v);
	if (dmrs_phase() != DMRS_SWEEP)
		tick_from = now;
	if (native) {
		if (now < native_at)
			return;
		if (native_at != 0) {
			native_at = 0;
			native_take();
			dmrs_retuned(now);	/* the take: the mode's window from now */
		}
		native_run();
		dmrs_heard_t h;
		memset(&h, 0, sizeof h);
		h.cc = DMRS_CC_NONE;
		bool ysf;
		(void)dmrs_native_hz(&ysf);
		if (!ysf && native_frames) {
			h.bursts = (uint16_t)native_frames;
			h.cc = 0x293;
			h.dst = 655;
			h.src = 1234;
			h.call = now - native_last < DMRS_VOICE_MS;
		}
		if (dmrs_probe(&h, now) != 0 || !dmrs_on() || !dmrs_native_hz(&ysf)) {
			native = false;
			now += 50u;	/* the worker lets go */
			dmrs_regained();
		}
		return;
	}
	if (dmrs_epoch() != epoch_seen) {
		epoch_seen = dmrs_epoch();
		dmr_setup();
	}
	bool ysf;
	if (dmrs_phase() == DMRS_SWEEP) {
		bool hit;
		ticks();
		uint32_t hz = dmrs_sweep(read_now(), now, &hit);
		if (hit)
			dmr_setup();
		else if (hz != 0) {
			retune(hz);
			(*nsteps)++;
		}
	} else if (dmrs_native_hz(&ysf)) {
		native = true;
		native_at = now + 50u;	/* native_lab.c takes it at its next look */
		return;
	} else {
		dmrs_heard_t h;
		if (dmrs_mode() == DMRS_MODE_DMR) {
			dmr_modem(ready);
			dmr_heard(&h);
		} else {
			memset(&h, 0, sizeof h);
			h.cc = DMRS_CC_NONE;
		}
		h.rssi_dbm = read_now();
		if (dmrs_probe(&h, now) != 0)
			dmr_setup();
	}
	if (dmrs_phase() != DMRS_SWEEP && dmrs_native_hz(&ysf)) {
		native = true;
		native_at = now + 50u;
	}
}
#endif

/* Run the DMR (or all digital) search over lo..hi for up to ms; returns ms to the first find of `want` (0 = none) */
static uint32_t dmr_run(uint32_t lo, uint32_t hi, uint32_t ms, const sig_t *want, unsigned *probes, unsigned *steps)
{
	uint32_t t_start = now, end = now + ms, found_ms = 0;
	dmrs_view_t v;
	unsigned nsteps = 0;
#if CONFIG_ALL_SEARCH
	if (all_kind) {
		tuned_hz = dmrs_start_all(lo, hi, 12500u, now);
		epoch_seen = dmrs_epoch();
		native = false;
	} else
#endif
		tuned_hz = dmrs_start(lo, hi, 12500u, now);
	tuned_at = now;
	synced_at = 0;
	dmr_looked = now;
	ready = now + 60u;
	tick_from = now;
	{
		dmrs_peek_t pk;
#if CONFIG_ALL_SEARCH
		if (all_kind)
			(void)dmrs_peek_all(0u, &pk);
		else
#endif
			(void)dmrs_peek(0u, &pk);
		seq0 = pk.seq;
	}
	while ((int32_t)(end - now) > 0) {
		now += 24u + rnd() % 3u;
#if CONFIG_ALL_SEARCH
		if (all_kind) {
			all_poll(&nsteps);
		} else
#endif
		{
			dmrs_snapshot(&v);
			uint32_t hz;
			if (dmrs_phase() == DMRS_SWEEP) {
				bool hit;
				ticks();
				hz = dmrs_sweep(read_now(), now, &hit);
				if (hz != 0)
					nsteps++;
			} else {
				tick_from = now;
				dmrs_heard_t h;
				dmr_modem(ready);
				dmr_heard(&h);
				h.rssi_dbm = read_now();
				hz = dmrs_probe(&h, now);
			}
			if (hz != 0) {
				retune(hz);
				synced_at = 0;
				dmr_looked = now;
				ready = now + 60u;
			}
		}
		if (trace_hz && want && want->hz == trace_hz) {
			static uint8_t ph;
			static uint32_t at;
			if (getenv("SURE_VERBOSE") && offset(tuned_hz, want->hz) <= 100000u)
				printf("V %6u tuned %u read %.0f floor %d ph %u\n", now - t_start, tuned_hz, reading,
				       (dmrs_snapshot(&v), v.floor_dbm), dmrs_phase());
			if (dmrs_phase() != ph || (dmrs_phase() != DMRS_SWEEP && dmrs_at_hz() != at)) {
				dmrs_snapshot(&v);
				printf("T %s %6u ph %u at %u floor %d keyed %d\n", trace_name, now - t_start, dmrs_phase(), dmrs_at_hz(), v.floor_dbm,
				       keyed(want, now));
				ph = dmrs_phase();
				at = dmrs_at_hz();
			}
		}
		if (want && !found_ms && dmr_found_at(want->hz, t_start)) {
			found_ms = now - t_start;
			if (!keep_going)
				break;
		}
	}
	{
		dmrs_peek_t pk;
#if CONFIG_ALL_SEARCH
		if (all_kind)
			(void)dmrs_peek_all(0u, &pk);
		else
#endif
			(void)dmrs_peek(0u, &pk);
		seq1 = pk.seq;
	}
#if CONFIG_ALL_SEARCH
	if (all_kind)
		dmrs_snapshot_all(&v);
	else
#endif
		dmrs_snapshot(&v);
	if (probes)
		*probes += v.hits;
	if (steps)
		*steps += nsteps;
	dmrs_stop();
	return found_ms;
}

/* --- P25 search (the radio core's sweep, native_lab.c's probe) --- */
static uint32_t p25_run(uint32_t lo, uint32_t hi, uint32_t ms, const sig_t *want, unsigned *probes, unsigned *steps)
{
	uint32_t t_start = now, end = now + ms, found_ms = 0, epoch_seen25, take_at = 0, taken_ms = 0;
	p25s_view_t v;
	retune(p25s_start(lo, hi, 12500u, now));
	epoch_seen25 = p25s_epoch();
	bool native25 = false, was_p25 = false;
	unsigned nsteps = 0;
	const uint32_t probe_ms = CONFIG_SEARCH_SURE ? 1200u : 700u;	/* native_lab.c P25S_PROBE_MS */
	p25s_peek_t pk;
	(void)p25s_peek(0u, &pk);
	seq0 = pk.seq;
	while ((int32_t)(end - now) > 0) {
		now += 24u + rnd() % 3u;
		p25s_snapshot(&v);
		if (p25s_phase() == P25S_SWEEP) {
			if (p25s_epoch() != epoch_seen25) {
				epoch_seen25 = p25s_epoch();
				retune(p25s_at_hz());
				p25s_retuned(now);
			} else {
				bool hit;
				uint32_t hz = p25s_sweep(read_now(), now, &hit);
				if (hz != 0) {
					retune(hz);
					nsteps++;
					epoch_seen25 = p25s_epoch();
				}
				if (hit) {
					native25 = true;
					take_at = now + 50u;
				}
			}
		} else if (native25) {
#if CONFIG_SEARCH_SURE
			p25s_level(read_now(), now);	/* the radio core reads the level while the native receiver probes */
#endif
			if (now >= take_at) {
				if (take_at != 0) {
					native_take();
					take_at = 0;
					taken_ms = now;
					was_p25 = false;
				}
				native_run();
				bool p25 = native_frames != 0;
				if (p25) {
					p25s_probe_found(0x293, 655, 1234, now);
					was_p25 = true;
				}
				bool gone = false;
#if CONFIG_SEARCH_SURE
				gone = !p25 && p25s_carrier_gone(now);
#endif
				if (gone || (p25 ? now - native_last >= 1500u : now - taken_ms >= probe_ms)) {
					native25 = false;
					now += 50u;
					p25s_probe_done(was_p25, now);
				}
			}
		}
		if (want && !found_ms) {
			p25s_snapshot(&v);
			for (unsigned i = 0; i < v.found_n && !found_ms; i++)
				if (offset(v.found[i].hz, want->hz) <= 1500u && (int32_t)(v.found[i].heard_ms - t_start) >= 0)
					found_ms = now - t_start;
			if (found_ms && !keep_going)
				break;
		}
	}
	(void)p25s_peek(0u, &pk);
	seq1 = pk.seq;
	p25s_snapshot(&v);
	if (probes)
		*probes += v.hits;
	if (steps)
		*steps += nsteps;
	p25s_stop();
	return found_ms;
}

/* --- scenarios --- */
typedef struct {
	const char *name;
	int mode;		/* the transmitter of interest */
	double lo_dbm, hi_dbm;	/* its level range */
	bool half;		/* on a 6.25 kHz raster between two steps */
	bool keyed_overs;	/* overs and pauses (else always on) */
	double nf;		/* receiver floor */
	double ceil;		/* reading ceiling (0 none) */
	int fm;			/* FM carriers elsewhere */
	double tau;
	uint32_t lo, hi, at;	/* the range (0: 433-435 MHz) and a fixed frequency (0: a random step) */
} scen_t;

/* fastsrch: a "-fmnext" scenario has a strong FM carrier (-60 dBm) 25 kHz above the transmitter */
static double next_dbm(const scen_t *sc) { return strstr(sc->name, "fmnext") ? -60.0 : 0.0; }

static void scenario(const char *eng, int engine, const scen_t *sc, int trials)
{
	static res_t r;
	memset(&r, 0, sizeof r);
	trace_name = sc->name;
	floor_dbm = sc->nf;
	ceiling = sc->ceil;
	tau = sc->tau;
	uint32_t lo = sc->lo ? sc->lo : 433000000u, hi = sc->hi ? sc->hi : 435000000u;
	uint32_t n = (hi - lo) / 12500u + 1u;
	unsigned probes = 0;
	double ran = 0;
	for (int k = 0; k < trials; k++) {
		nsig = 1;
		sig_t *s = &sigs[0];
		memset(s, 0, sizeof *s);
		s->hz = sc->at ? sc->at : lo + 12500u * (n / 8u + crnd() % (n - n / 4u)) + (sc->half ? 6250u : 0u);
		s->dbm = sc->lo_dbm + (double)(crnd() % 1000u) / 999.0 * (sc->hi_dbm - sc->lo_dbm);
		s->mode = sc->mode;
		s->phase = crnd() % 60u;
		if (sc->keyed_overs) {
			s->over = 5000u + crnd() % 15001u;
			s->pause = 3000u + crnd() % 7001u;
			s->t0 = now - crnd() % (s->over + s->pause);
		}
		if (next_dbm(sc) != 0.0) {
			sig_t *c = &sigs[nsig++];
			memset(c, 0, sizeof *c);
			c->hz = s->hz + 25000u;
			c->dbm = next_dbm(sc);
			c->mode = M_FM;
		}
		for (int f = 0; f < sc->fm && nsig < NSIG_MAX; f++) {
			sig_t *c = &sigs[nsig++];
			memset(c, 0, sizeof *c);
			do
				c->hz = lo + 12500u * (crnd() % n);
			while (offset(c->hz, s->hz) < 100000u);
			c->dbm = -95.0 + (double)(crnd() % 30u);
			c->mode = M_FM;
		}
		uint32_t t0 = now, got;
		if (engine == 0)
			got = p25_run(lo, hi, 60000u, s, &probes, NULL);
		else
			got = dmr_run(lo, hi, 60000u, s, &probes, NULL);
		ran += (now - t0) / 1000.0;
		if (!got && getenv("SURE_DEBUG") && strstr(sc->name, getenv("SURE_DEBUG"))) printf("MISS %s hz %u dbm %.0f over %u pause %u probes %u\n", sc->name, s->hz, s->dbm, s->over, s->pause, probes);
		if (got) {
			r.t[r.found] = got / 1000.0;
			r.found++;
		}
		now += 40000u;	/* a fresh start: the skips run out */
	}
	r.probes = probes;
	r.run_s = ran;
	/* the sweep's speed over the same range without the transmitter (the FM carriers stay) */
	sigs[0].dbm = -300.0;
	unsigned steps = 0;
	uint32_t t0 = now;
	if (engine == 0)
		p25_run(lo, hi, 30000u, NULL, NULL, &steps);
	else
		dmr_run(lo, hi, 30000u, NULL, NULL, &steps);
	r.steps_s = steps * 1000.0 / (now - t0);
	print_res(eng, sc->name, &r, trials);
	now += 40000u;
}

/* Calls caught a minute: `trials` searches of 120 s each that run on after a find (the transmitter keyed in overs);
 * printed as MODEL <engine> fpm-<scenario> <found> <median s to the first find> 0 <probes a minute> 0 <calls a minute> */
static void scenario_fpm(const char *eng, int engine, const scen_t *sc, int trials)
{
	static res_t r;
	memset(&r, 0, sizeof r);
	floor_dbm = sc->nf;
	ceiling = sc->ceil;
	tau = sc->tau;
	uint32_t lo = sc->lo ? sc->lo : 433000000u, hi = sc->hi ? sc->hi : 435000000u;
	uint32_t n = (hi - lo) / 12500u + 1u;
	unsigned probes = 0, calls = 0;
	double ran = 0;
	keep_going = true;
	for (int k = 0; k < trials; k++) {
		nsig = 1;
		sig_t *s = &sigs[0];
		memset(s, 0, sizeof *s);
		s->hz = sc->at ? sc->at : lo + 12500u * (n / 8u + crnd() % (n - n / 4u)) + (sc->half ? 6250u : 0u);
		s->dbm = sc->lo_dbm + (double)(crnd() % 1000u) / 999.0 * (sc->hi_dbm - sc->lo_dbm);
		s->mode = sc->mode;
		s->phase = crnd() % 60u;
		s->over = 5000u + crnd() % 15001u;
		s->pause = 3000u + crnd() % 7001u;
		s->t0 = now - crnd() % (s->over + s->pause);
		if (next_dbm(sc) != 0.0) {
			sig_t *c = &sigs[nsig++];
			memset(c, 0, sizeof *c);
			c->hz = s->hz + 25000u;
			c->dbm = next_dbm(sc);
			c->mode = M_FM;
		}
		for (int f = 0; f < sc->fm && nsig < NSIG_MAX; f++) {
			sig_t *c = &sigs[nsig++];
			memset(c, 0, sizeof *c);
			do
				c->hz = lo + 12500u * (crnd() % n);
			while (offset(c->hz, s->hz) < 100000u);
			c->dbm = -95.0 + (double)(crnd() % 30u);
			c->mode = M_FM;
		}
		uint32_t t0 = now, got;
		if (engine == 0)
			got = p25_run(lo, hi, 120000u, s, &probes, NULL);
		else
			got = dmr_run(lo, hi, 120000u, s, &probes, NULL);
		ran += (now - t0) / 1000.0;
		calls += (uint16_t)(seq1 - seq0);
		if (got) {
			r.t[r.found] = got / 1000.0;
			r.found++;
		}
		now += 40000u;
	}
	keep_going = false;
	qsort(r.t, (size_t)r.found, sizeof r.t[0], cmp_d);
	printf("MODEL %s fpm-%s %d %.1f 0 %.1f 0 %.2f\n", eng, sc->name, r.found, r.found ? r.t[r.found / 2] : 99.0,
	       ran > 0 ? probes * 60.0 / ran : 0.0, ran > 0 ? calls * 60.0 / ran : 0.0);
}

/* The owner's beta.7 test (2026-11-21): the other radio on DMR direct mode (TG 1, CC 1, TS1), LOW power in the same
 * room, on 436.275 (or 433.200), the search on a typed range of about 436.000-436.500 MHz: a reading at the ceiling
 * for 50 kHz around it, the skirt beyond (the strong-signal profile); 433.200 lies outside the range */
#define OWNER_LO 436000000u
#define OWNER_HI 436500000u

int main(int argc, char **argv)
{
	const char *eng = CONFIG_SEARCH_SURE ? "sure" : "before";
#if CONFIG_SEARCH_HYBRID
	bool fast = argc > 1 && strcmp(argv[1], "fast") == 0;
	sh_set_fast(fast);
	eng = fast ? "fast" : "classic";
#else
	(void)argc;
	(void)argv;
#endif
	now = 100000u;
	if (getenv("SURE_SEED")) {
		seed = (unsigned)atoi(getenv("SURE_SEED"));
		case_seed = seed * 7u + 3u;
	}
	if (getenv("SURE_LEAK"))
		gap_leak = atof(getenv("SURE_LEAK"));
	if (getenv("SURE_TRACE"))
		trace_hz = (uint32_t)strtoul(getenv("SURE_TRACE"), NULL, 10);
	static const scen_t p25s[] = {
		{ "p25-overs", M_P25, -110, -60, false, true, -124, 0, 0, 5, 0, 0, 0 },
		{ "p25-close", M_P25, -40, -20, false, true, -124, -45, 0, 5, 0, 0, 0 },
		{ "p25-highfloor", M_P25, -95, -60, false, true, -112, 0, 0, 5, 0, 0, 0 },
		{ "p25-half", M_P25, -100, -60, true, true, -124, 0, 0, 5, 0, 0, 0 },
		{ "p25-busy", M_P25, -100, -60, false, true, -124, 0, 6, 5, 0, 0, 0 },
	};
	static const scen_t dmrs[] = {
		{ "dmr-owner-436275", M_DMRDIR, -30, -15, false, true, -124, -45, 0, 5, OWNER_LO, OWNER_HI, 436275000u },
		{ "dmr-owner-offgrid", M_DMRDIR, -30, -15, false, true, -124, -45, 0, 5, OWNER_LO + 10000u, OWNER_HI, 436275000u },
		{ "dmr-owner-433200", M_DMRDIR, -30, -15, false, true, -124, -45, 0, 5, OWNER_LO, OWNER_HI, 433200000u },
		{ "dmr-direct", M_DMRDIR, -110, -60, false, true, -124, 0, 0, 5, 0, 0, 0 },
		{ "dmr-direct-lag15", M_DMRDIR, -110, -60, false, true, -124, 0, 0, 15, 0, 0, 0 },
		{ "dmr-close", M_DMRDIR, -40, -20, false, true, -124, -45, 0, 5, 0, 0, 0 },
		{ "dmr-strong", M_DMRDIR, -70, -50, false, true, -124, 0, 0, 5, 0, 0, 0 },
		{ "dmr-highfloor", M_DMRDIR, -95, -60, false, true, -112, 0, 0, 5, 0, 0, 0 },
		{ "dmr-half", M_DMRDIR, -100, -60, true, true, -124, 0, 0, 5, 0, 0, 0 },
		{ "dmr-busy", M_DMRDIR, -100, -60, false, true, -124, 0, 6, 5, 0, 0, 0 },
		{ "dmr-repeater", M_DMRRPT, -110, -60, false, true, -124, 0, 0, 5, 0, 0, 0 },
		{ "dmr-steady", M_DMRDIR, -100, -60, false, false, -124, 0, 0, 5, 0, 0, 0 },
	};
	const int trials = 200;
	for (unsigned i = 0; i < sizeof p25s / sizeof p25s[0]; i++)
		scenario(eng, 0, &p25s[i], trials);
	all_kind = false;
	for (unsigned i = 0; i < sizeof dmrs / sizeof dmrs[0]; i++)
		scenario(eng, 1, &dmrs[i], trials);
#if CONFIG_ALL_SEARCH
	all_kind = true;
	static const scen_t alls[] = {
		{ "all-owner-436275", M_DMRDIR, -30, -15, false, true, -124, -45, 0, 5, OWNER_LO, OWNER_HI, 436275000u },
		{ "all-dmr-direct", M_DMRDIR, -110, -60, false, true, -124, 0, 0, 5, 0, 0, 0 },
		{ "all-dmr-close", M_DMRDIR, -40, -20, false, true, -124, -45, 0, 5, 0, 0, 0 },
		{ "all-p25", M_P25, -110, -60, false, true, -124, 0, 0, 5, 0, 0, 0 },
		{ "all-dmr-repeater", M_DMRRPT, -110, -60, false, true, -124, 0, 0, 5, 0, 0, 0 },
		{ "all-highfloor", M_DMRDIR, -95, -60, false, true, -112, 0, 0, 5, 0, 0, 0 },
		{ "all-busy", M_DMRDIR, -100, -60, false, true, -124, 0, 6, 5, 0, 0, 0 },
	};
	for (unsigned i = 0; i < sizeof alls / sizeof alls[0]; i++)
		scenario(eng, 1, &alls[i], trials);
	all_kind = false;
#endif
	/* fastsrch rows (after the original ones, whose figures stay as without the switch): a P25 repeater (always
	 * keyed), a weak P25 / DMR with a strong FM carrier 25 kHz above it, 430-440 MHz */
	static const scen_t new_p25[] = {
		{ "p25-rpt", M_P25, -110, -60, false, false, -124, 0, 0, 5, 0, 0, 0 },
		{ "p25-fmnext", M_P25, -110, -100, false, true, -124, 0, 0, 5, 0, 0, 0 },
		{ "p25-wide10", M_P25, -110, -60, false, true, -124, 0, 0, 5, 430000000u, 440000000u, 0 },
	};
	static const scen_t new_dmr[] = {
		{ "dmr-fmnext", M_DMRDIR, -110, -100, false, true, -124, 0, 0, 5, 0, 0, 0 },
		{ "dmr-wide10", M_DMRDIR, -110, -60, false, true, -124, 0, 0, 5, 430000000u, 440000000u, 0 },
	};
	for (unsigned i = 0; i < sizeof new_p25 / sizeof new_p25[0]; i++)
		scenario(eng, 0, &new_p25[i], trials);
	for (unsigned i = 0; i < sizeof new_dmr / sizeof new_dmr[0]; i++)
		scenario(eng, 1, &new_dmr[i], trials);
#if CONFIG_ALL_SEARCH
	all_kind = true;
	static const scen_t new_all[] = {
		{ "all-p25-fmnext", M_P25, -110, -100, false, true, -124, 0, 0, 5, 0, 0, 0 },
		{ "all-wide10", M_DMRDIR, -110, -60, false, true, -124, 0, 0, 5, 430000000u, 440000000u, 0 },
	};
	for (unsigned i = 0; i < sizeof new_all / sizeof new_all[0]; i++)
		scenario(eng, 1, &new_all[i], trials);
	all_kind = false;
#endif
	/* fastsrch: calls caught a minute (searches left running 120 s) */
	static const scen_t fpm_p25[] = {
		{ "p25-overs", M_P25, -110, -60, false, true, -124, 0, 0, 5, 0, 0, 0 },
		{ "p25-busy", M_P25, -100, -60, false, true, -124, 0, 6, 5, 0, 0, 0 },
		{ "p25-fmnext", M_P25, -110, -100, false, true, -124, 0, 0, 5, 0, 0, 0 },
		{ "p25-wide10", M_P25, -110, -60, false, true, -124, 0, 0, 5, 430000000u, 440000000u, 0 },
	};
	static const scen_t fpm_dmr[] = {
		{ "dmr-strong", M_DMRDIR, -70, -50, false, true, -124, 0, 0, 5, 0, 0, 0 },
		{ "dmr-owner-436275", M_DMRDIR, -30, -15, false, true, -124, -45, 0, 5, OWNER_LO, OWNER_HI, 436275000u },
		{ "dmr-busy", M_DMRDIR, -100, -60, false, true, -124, 0, 6, 5, 0, 0, 0 },
		{ "dmr-fmnext", M_DMRDIR, -110, -100, false, true, -124, 0, 0, 5, 0, 0, 0 },
		{ "dmr-wide10", M_DMRDIR, -110, -60, false, true, -124, 0, 0, 5, 430000000u, 440000000u, 0 },
	};
	const int ftrials = 40;
	for (unsigned i = 0; i < sizeof fpm_p25 / sizeof fpm_p25[0]; i++)
		scenario_fpm(eng, 0, &fpm_p25[i], ftrials);
	for (unsigned i = 0; i < sizeof fpm_dmr / sizeof fpm_dmr[0]; i++)
		scenario_fpm(eng, 1, &fpm_dmr[i], ftrials);
#if CONFIG_ALL_SEARCH
	all_kind = true;
	static const scen_t fpm_all[] = {
		{ "all-busy", M_DMRDIR, -100, -60, false, true, -124, 0, 6, 5, 0, 0, 0 },
		{ "all-p25-fmnext", M_P25, -110, -100, false, true, -124, 0, 0, 5, 0, 0, 0 },
		{ "all-wide10", M_DMRDIR, -110, -60, false, true, -124, 0, 0, 5, 430000000u, 440000000u, 0 },
	};
	for (unsigned i = 0; i < sizeof fpm_all / sizeof fpm_all[0]; i++)
		scenario_fpm(eng, 1, &fpm_all[i], ftrials);
	all_kind = false;
#endif
	CHECK(failures == 0);
	return failures != 0;
}
