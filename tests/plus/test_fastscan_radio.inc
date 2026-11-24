/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus faster channel scan (radio/scanfast.h, radio_scanfast.inc; docs/plus.md "Faster channel scan"): the real
 * radio state machine's zone scan on the fake board of radioCoreTest (its codeplug widened to 96 channels by
 * tools/test_fastscan.py), the native P25 receiver behind it, and a model of the receiver's readings. Classic (v1.0.0's
 * scan) and fast are run on the same zones, the same transmissions and the same random sequences.
 *
 * The model (HOST-MODELLED, not measured):
 * - the radio task as radio_task.c runs it: the tick every 10 ms, the poll every 25 ms, one in ten 1..3 ms late;
 *   the native receiver, while it has the channel, stops both (its maintenance on the poll's period);
 * - the level reading: the signal on the tuned frequency, else the floor (-125 dBm +-2 dB at random each ms), through a
 *   first-order lag of tau ms (0, 5, 10, 20) that starts from the last frequency's reading after a retune (a stale reading);
 * - the noise reading from the level (120 at the floor, 4 a dB, the squelch's opening line 52 about -108 dBm), and for
 *   FX_DIP_MS after every retune 30 (a carrier: OpenDMR's note that the noise reads low a moment after a change);
 * - the signals, each on one channel of the zone, in overs (1.5-4 s on, 3-12 s off, from 2 s): FM -105 dBm; FM around
 *   the squelch line (the noise 44..67 at random each ms); DMR direct mode one slot on air 27.5 ms of every 60 at
 *   -60 dBm (a radio in the same room), its gaps at the floor or (leak) 40 dB under the burst; DMR -95 dBm; P25 -95 dBm
 *   with the noise reading deaf to it (only the strength rises: upstream gotchas 17.3);
 * - no DMR burst reaches the call stack and no P25 frame the native receiver: a DMR stop with a carrier is probed and
 *   left, a P25 one held, handed to the native receiver and given back. What is compared is whether the scan noticed
 *   the over (probed or held its channel while it was on the air) and how soon.
 * Zones: 16, 32 and 80 channels, every 4th DMR and every 8th (from 5) P25, the rest FM; "all FM" too. */
#include <math.h>
#include "radio/scanfast.h"

enum { FX_NONE, FX_FM, FX_FM_EDGE, FX_DMR, FX_P25 };
enum { FX_DIP_MS = 15, FX_FLOOR_DBM = -125, FX_OVERS = 96 };
/* The dip's length (tests/plus/test_zonescan_radio.inc tries others), and (not 0) its length after a retune of more than
 * 100 MHz (a VHF / UHF jump) */
static uint32_t fx_dip_ms = FX_DIP_MS, fx_dip_band_ms;
static uint32_t fx_last_hz;	/* the frequency before the last retune */
static bool fx_jumped;		/* the last retune was a VHF / UHF jump */
#define FX_BASE_HZ	446006250u
#define FX_STEP_HZ	12500u

static struct {
	uint16_t n;
	bool all_fm;
	uint8_t kind;		/* FX_*: the signal */
	uint16_t pos;		/* its channel */
	int16_t dbm;
	uint8_t leak_db;	/* DMR: its gaps read this far under the burst (0: the floor) */
	uint8_t njit;		/* the noise reading +- this at random each reading (verifier's variant) */
	double tau;
	uint32_t rand;
	uint32_t t0[FX_OVERS], len[FX_OVERS];
	int overs;
	/* the receiver */
	double reading;
	int rx_calls;
	uint32_t tuned_at;
	uint32_t next_tick, next_poll, tick_late, poll_late;
	/* results */
	bool caught[FX_OVERS];
	uint32_t delay[FX_OVERS];
	uint32_t stop_steps;	/* the stop being watched for a miss: scan_t.steps then */
	bool stop_on_sig, stop_air;
	unsigned missed;	/* fast: stops on the signal's channel, it on the air all the stop, ended by the fast rule */
	unsigned holds_quiet;	/* holds in an all-quiet run (none may happen) */
} fx;

static uint32_t fx_rand(uint32_t n)
{
	fx.rand = fx.rand * 1664525u + 1013904223u;
	return (fx.rand >> 8) % n;
}

static uint32_t fx_hz(uint16_t pos)
{
	return FX_BASE_HZ + FX_STEP_HZ * pos;
}

static uint8_t fx_mode_at(uint16_t i)
{
	if (fx.all_fm)
		return RADIO_MODE_FM;
	return i % 4 == 3 ? RADIO_MODE_DMR : i % 8 == 5 ? RADIO_MODE_P25 : RADIO_MODE_FM;
}

/* Zone 0: n channels 1..n, as above; the signal's channel gets its mode */
static void fx_codeplug(void)
{
	memset(used, 0, sizeof(used));
	memset(zones, 0, sizeof(zones));
	for (uint16_t i = 0; i < fx.n; i++) {
		uint8_t mode = fx_mode_at(i);
		if (i == fx.pos && fx.kind != FX_NONE)
			mode = fx.kind == FX_DMR ? RADIO_MODE_DMR : fx.kind == FX_P25 ? RADIO_MODE_P25 : RADIO_MODE_FM;
		char name[12];
		snprintf(name, sizeof name, "Ch %u", (unsigned)i + 1u);
		add((uint16_t)(i + 1u), name, fx_hz(i), mode, false, CODEPLUG_POWER_DEFAULT);
		channel_t *c = &chans[i + 1];
		c->bw = RADIO_BW_12K5;
		c->color_code = 1;
		c->timeslot = 1;
		if (mode == RADIO_MODE_P25) {
			c->p25_nac = 0x293;
			c->p25_tg = 42;
			c->p25_id = 3101234;
		}
		zones[0].channels[i] = (uint16_t)(i + 1u);
	}
	strcpy(zones[0].name, "Scan");
	zones[0].count = fx.n;
	nzones = 1;
}

static int fx_over_now(void)
{
	for (int k = 0; k < fx.overs; k++)
		if ((int32_t)(now - fx.t0[k]) >= 0 && now - fx.t0[k] < fx.len[k])
			return k;
	return -1;
}

/* The receiver's readings now (hw.st) */
static void fx_status(void)
{
	double r = fx.reading;
	int noise = (int)lround(120.0 - 4.0 * (r - FX_FLOOR_DBM));
	bool here = fx.kind != FX_NONE && hw.rx_hz == fx_hz(fx.pos) && fx_over_now() >= 0;
	if (here && fx.kind == FX_P25)
		noise = 120 - (int)fx_rand(6);			/* deaf: the noise stays at its idle value */
	if (here && fx.kind == FX_FM_EDGE)
		noise = 44 + (int)fx_rand(24);			/* around the squelch line */
	if (now - fx.tuned_at < (fx_jumped && fx_dip_band_ms != 0 ? fx_dip_band_ms : fx_dip_ms))
		noise = 30;
	else if (fx.njit != 0)
		noise += (int)fx_rand(2u * fx.njit + 1u) - fx.njit;
	hw.st.noise = (uint8_t)(noise < 8 ? 8 : noise > 120 ? 120 : noise);
	hw.st.rssi_dbm = (int16_t)lround(r);
	hw.st.tone_ok = true;
}

/* One millisecond: the level on the tuned frequency, the lag, the radio task's tick and poll */
static void fx_ms(void)
{
	now++;
	fs_stats_t fq;
	fs_view(now, &fq);
	if (hw.rx_calls != fx.rx_calls) {
		fx.rx_calls = hw.rx_calls;
		fx.tuned_at = now;
		fx_jumped = (hw.rx_hz > fx_last_hz ? hw.rx_hz - fx_last_hz : fx_last_hz - hw.rx_hz) > 100000000u;
		fx_last_hz = hw.rx_hz;
	}
	double target = FX_FLOOR_DBM - 2 + (double)fx_rand(5);
	int k = fx_over_now();
	if (k >= 0 && hw.rx_hz == fx_hz(fx.pos)) {
		if (fx.kind == FX_DMR) {
			bool burst = (now - fx.t0[k]) % 60u * 2u < 55u;	/* 27.5 ms of every 60 */
			if (burst)
				target = fx.dbm;
			else if (fx.leak_db != 0)
				target = fx.dbm - fx.leak_db;
		} else if (fx.kind == FX_FM_EDGE) {
			target = -110;
		} else {
			target = fx.dbm;
		}
	}
	fx.reading = fx.tau <= 0 ? target : fx.reading + (target - fx.reading) * (1.0 - exp(-1.0 / fx.tau));
	bool native = native_lab_owns();
	/* radio_task.c: the tick and the poll on their own grids (10 / 25 ms), the tick first when both are due; one in
	 * ten runs 1..3 ms late (the task busy elsewhere), the next on the grid again */
	if ((int32_t)(now - fx.next_tick - fx.tick_late) >= 0) {
		fx.next_tick += RADIO_TICK_MS;
		fx.tick_late = fx_rand(10) == 0 ? 1u + fx_rand(3) : 0u;
		if (!native) {
			fx_status();
			radio_sm_tick(&sm, now);
		}
	}
	if ((int32_t)(now - fx.next_poll - fx.poll_late) >= 0) {
		fx.next_poll += RADIO_POLL_MS;
		fx.poll_late = fx_rand(10) == 0 ? 1u + fx_rand(3) : 0u;
		fx_status();
		if (native)
			radio_sm_native_maintenance(&sm, now);
		else
			radio_sm_poll(&sm, now);
		classic_pump();
	}
	/* Noticed: the scan probes or holds the signal's channel (or the native receiver has it) while it is on the air */
	bool on_sig = sm.scan.kind == SCAN_ZONE && sm.view.zone_pos == fx.pos && hw.rx_hz == fx_hz(fx.pos);
	bool noticing = on_sig && (sm.scan.phase == SCAN_PH_PROBE || sm.scan.phase == SCAN_PH_HOLD || native_lab_owns());
	if (sm.scan.phase == SCAN_PH_HOLD && fx.kind == FX_NONE) {
		if (fx.holds_quiet++ == 0 && getenv("FASTSCAN_DEBUG"))
			printf("DBG hold in a quiet zone: pos %u mode %u noise %u rssi %d state %u sq %u since %u fast %u\n",
			       sm.view.zone_pos, sm.view.ch.mode, hw.st.noise, hw.st.rssi_dbm, sm.view.state, sm.view.squelch,
			       (unsigned)(now - sm.scan.tuned_ms), fs_fast());
	}
	if (noticing && k >= 0 && !fx.caught[k]) {
		fx.caught[k] = true;
		fx.delay[k] = now - fx.t0[k];
	}
	/* A false quiet: the fast rule ended a stop on the signal's channel with the signal on the air all the stop */
	if (sm.scan.steps != fx.stop_steps) {
		fs_stats_t fa;
		fs_view(now, &fa);
		if (fx.stop_on_sig && fx.stop_air && fa.fast_quiet != fq.fast_quiet)
			fx.missed++;
		fx.stop_steps = sm.scan.steps;
		fx.stop_on_sig = on_sig;
		fx.stop_air = k >= 0;
	}
	fx.stop_air = fx.stop_air && k >= 0;
}

typedef struct {
	unsigned overs, caught, missed, stops, holds_quiet;
	uint32_t med_delay;
	double ms_per_stop;
} fx_res_t;

static int fx_cmp(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

/* The scan starts as a poll ends (a request handled between polls may meet the next one within the noise reading's
 * dip after the retune: in classic that opens the squelch at the scan's first stop; not this model's subject) */
static void fx_begin_scan(void)
{
	fx.reading = FX_FLOOR_DBM;
	radio_sm_poll(&sm, now);
	req(RREQ_SCAN, RSCAN_ZONE);
	fx.rx_calls = hw.rx_calls;
	fx.tuned_at = now;
	fx_last_hz = hw.rx_hz;
	fx_jumped = false;
	fx.next_tick = now + 1u + fx_rand(RADIO_TICK_MS);
	fx.next_poll = now + RADIO_POLL_MS;
	fx.tick_late = fx.poll_late = 0;
	fx.stop_steps = sm.scan.steps;
	fx.stop_on_sig = fx.stop_air = false;
}

/* One run: the zone scanned for run_ms with this algorithm */
static fx_res_t fx_run(bool fast, uint32_t seed, uint32_t run_ms)
{
	fx_res_t res;
	memset(&res, 0, sizeof res);
	fx.rand = seed * 2654435761u + 12345u;
	fx.overs = 0;
	if (fx.kind != FX_NONE) {
		uint32_t t = 2000u + fx_rand(3000);
		while (fx.overs < FX_OVERS && t < run_ms) {
			fx.t0[fx.overs] = 1000u + t;	/* start() sets the time to 1000 */
			fx.len[fx.overs] = 1500u + fx_rand(2501);
			t += fx.len[fx.overs] + 3000u + fx_rand(9001);
			fx.overs++;
		}
	}
	memset(fx.caught, 0, sizeof fx.caught);
	memset(fx.delay, 0, sizeof fx.delay);
	fx.missed = fx.holds_quiet = 0;
	fx_codeplug();
	start();
	native_lab_init();
	fake_power.switch_on = classic_on;
	fake_power.battery_mv = classic_battery;
	classic_busy = classic_stop = classic_receiving = false;
	classic_starts = classic_restores = 0;
	scan_voice_ms = zs_sync_ms = 0;
	fs_set_fast(fast);
	fx_begin_scan();
	uint32_t steps0 = 0, t_count = 0;
	for (uint32_t t = 0; t < run_ms; t++) {
		if (t == 3000u) {
			steps0 = sm.scan.steps;
			t_count = now;
		}
		fx_ms();
	}
	CHECK(sm.scan.kind == SCAN_ZONE);	/* still scanning: nothing ended it */
	res.stops = sm.scan.steps - steps0;
	res.ms_per_stop = res.stops ? (double)(now - t_count) / res.stops : 0;
	uint32_t d[FX_OVERS];
	unsigned nd = 0;
	for (int k = 0; k < fx.overs; k++) {
		if (fx.t0[k] + fx.len[k] > now)
			continue;	/* still on the air at the end */
		res.overs++;
		if (fx.caught[k]) {
			res.caught++;
			d[nd++] = fx.delay[k];
		}
	}
	if (nd) {
		qsort(d, nd, sizeof d[0], fx_cmp);
		res.med_delay = d[nd / 2];
	}
	res.missed = fx.missed;
	res.holds_quiet = fx.holds_quiet;
	req(RREQ_SCAN, RSCAN_STOP);
	fs_set_fast(true);
	return res;
}

/* --- Semantics in fast mode on the model (the upstream suites cover the rest: tools/test_fastscan.py) --- */

static void fx_setup(uint16_t n, bool all_fm, uint8_t kind, uint16_t pos, int16_t dbm, uint8_t leak, double tau)
{
	memset(&fx, 0, sizeof fx);
	fx.n = n;
	fx.all_fm = all_fm;
	fx.kind = kind;
	fx.pos = pos;
	fx.dbm = dbm;
	fx.leak_db = leak;
	fx.tau = tau;
}

static void test_fastscan_semantics(void)
{
	fs_stats_t v;
	/* An all-FM zone, quiet: every stop ends on the fast rule, three readings 30-50 ms after the retune; nothing held, no squelch opened
	 * by the noise reading's dip after a retune (the poll waits while the tick's stop is young) */
	fx_setup(16, true, FX_NONE, 0, 0, 0, 0);
	fx_res_t r = fx_run(true, 1, 6000);
	fs_view(now, &v);
	CHECK(r.holds_quiet == 0 && v.noticed == 0 && v.held == 0 && v.fallback == 0 && v.fast_quiet + 1u >= v.stops);
	CHECK(v.q_n[FS_M_FM] > 100 && v.q_ms[FS_M_FM] / v.q_n[FS_M_FM] <= 56 && v.passes >= 5 && v.pass_stops == 16);
	printf("Faster scan: quiet FM zone %.1f ms a stop (%u stops), fallback %u\n", r.ms_per_stop, v.stops, v.fallback);
	fx_res_t c = fx_run(false, 1, 6000);
	fs_view(now, &v);
	CHECK(c.holds_quiet == 0 && v.fast_quiet == 0 && !v.fast && v.q_ms[FS_M_FM] / v.q_n[FS_M_FM] >= 150);
	/* Mixed: DMR stops a whole TDMA frame (30 + 60 ms), P25 / FM three readings */
	fx_setup(16, false, FX_NONE, 0, 0, 0, 0);
	r = fx_run(true, 2, 6000);
	fs_view(now, &v);
	uint32_t dmr = v.q_ms[FS_M_DMR] / (v.q_n[FS_M_DMR] ? v.q_n[FS_M_DMR] : 1u);
	uint32_t p25 = v.q_ms[FS_M_P25] / (v.q_n[FS_M_P25] ? v.q_n[FS_M_P25] : 1u);
	CHECK(r.holds_quiet == 0 && dmr >= 90 && dmr <= 102 && p25 <= 56 && v.fallback == 0);
	printf("Faster scan: mixed zone, quiet dwell FM %u ms, DMR %u ms, P25 %u ms\n",
	       (unsigned)(v.q_ms[FS_M_FM] / v.q_n[FS_M_FM]), (unsigned)dmr, (unsigned)p25);
	/* A signal: held as in classic (FM: the squelch open, the hold), and the wait after it */
	fx_setup(16, true, FX_FM, 6, -105, 0, 0);
	r = fx_run(true, 3, 30000);
	CHECK(r.caught == r.overs && r.overs >= 2 && r.missed == 0);
	/* The switch mid-scan: the next stop is classic's 150 ms dwell (and the stop the tick has just stepped to keeps the
	 * poll's wait: no squelch opened by the reading's dip, no false hold) */
	fx_setup(16, true, FX_NONE, 0, 0, 0, 0);
	fx.rand = 7;
	fx_codeplug();
	start();
	native_lab_init();
	fs_set_fast(true);
	fx_begin_scan();
	for (int i = 0; i < 500; i++)
		fx_ms();
	fs_set_fast(false);
	for (int i = 0; i < 400; i++)
		fx_ms();
	uint32_t s0 = sm.scan.steps, t0 = now;
	for (int i = 0; i < 3000; i++)
		fx_ms();
	CHECK((now - t0) / (sm.scan.steps - s0) >= 150u && (now - t0) / (sm.scan.steps - s0) <= 160u && !fs_fast());
	fs_view(now, &v);
	CHECK(!v.fast && v.fast_quiet == 0 && v.stops >= 18);
	req(RREQ_SCAN, RSCAN_STOP);
	fs_view(now, &v);
	CHECK(!v.on);
	fs_set_fast(true);
	printf("Faster scan: holds, the switch, the counters\n");
}

/* A priority look in the wait after a call, ended quiet on the tick, tunes back to the wait's channel (RESUME): a poll
 * inside the noise reading's dip right after that retune must not open the squelch (a false hold that starts the wait
 * again, a look in each). Verifier's case on 7db7c7b, where the poll's wait required the listen phase */
#include "radio/scanprio.h"
static void test_fastscan_prio_look(void)
{
	unsigned looks = 0, false_holds = 0, late = 0;
	for (uint32_t seed = 1; seed <= 12; seed++) {
		fx_setup(16, true, FX_FM, 6, -105, 0, 0);
		fx.rand = seed * 40503u;
		fx_codeplug();
		start();
		native_lab_init();
		fake_power.switch_on = classic_on;
		fake_power.battery_mv = classic_battery;
		classic_busy = classic_stop = classic_receiving = false;
		fs_set_fast(true);
		radio_scan_prio_set(scanprio_pack(11, 0));	/* channel 11: position 10 */
		fx.overs = 1;
		fx.t0[0] = now + 1500u + fx_rand(400);
		fx.len[0] = 3000u;
		fx_begin_scan();
		uint32_t end = fx.t0[0] + fx.len[0], left = 0;
		bool looking = false;
		uint8_t was = SCAN_PH_LISTEN;
		while ((int32_t)(now - end - 4000u) < 0) {
			fx_ms();
			if ((int32_t)(now - end) < 0) {
				was = sm.scan.phase;
				continue;
			}
			bool at_wait = sm.view.zone_pos == fx.pos && hw.rx_hz == fx_hz(fx.pos);
			bool at_prio = hw.rx_hz == fx_hz(10);
			if (at_prio && !looking && left == 0)
				looks++;
			looking = at_prio;
			/* the wait held again with nothing on the air: a squelch opened on the dip */
			if (at_wait && left == 0 && was == SCAN_PH_RESUME && sm.scan.phase == SCAN_PH_HOLD)
				false_holds++;
			was = sm.scan.phase;
			if (left == 0 && !at_wait && !at_prio)
				left = now;
		}
		if (left == 0 || left - end > sm.set.scan_delay_ms + 400u)
			late++;
	}
	radio_scan_prio_set(0);
	CHECK(looks >= 6 && false_holds == 0 && late == 0);
	printf("Faster scan: a priority look in the wait, ended on the tick: %u looks in 12 waits, %u false holds, "
	       "%u waits late\n", looks, false_holds, late);
}

/* --- The table: classic -> fast --- */

typedef struct {
	const char *name;
	bool all_fm;
	uint8_t kind;
	int16_t dbm;
	uint8_t leak;
	uint8_t njit;
} fx_row_t;

static const fx_row_t fx_rows[] = {
	{ "quiet-fm", true, FX_NONE, 0, 0, 0 },
	{ "quiet-mix", false, FX_NONE, 0, 0, 0 },
	{ "quiet-mix-jit6", false, FX_NONE, 0, 0, 6 },
	{ "fm-105", false, FX_FM, -105, 0, 0 },
	{ "fm-105-jit6", false, FX_FM, -105, 0, 6 },
	{ "fm-107", false, FX_FM, -107, 0, 0 },
	{ "fm-107-jit6", false, FX_FM, -107, 0, 6 },
	{ "fm-edge", false, FX_FM_EDGE, 0, 0, 0 },
	{ "dmr-room", false, FX_DMR, -60, 0, 0 },
	{ "dmr-room-jit6", false, FX_DMR, -60, 0, 6 },
	{ "dmr-room-leak", false, FX_DMR, -60, 40, 0 },
	{ "dmr-95", false, FX_DMR, -95, 0, 0 },
	{ "dmr-95-jit6", false, FX_DMR, -95, 0, 6 },
	{ "p25-95", false, FX_P25, -95, 0, 0 },
};

static int test_fastscan_table(void)
{
	static const uint16_t sizes[] = { 16, 32, 80 };
	static const double taus[] = { 0, 5, 10, 20, 30 };
	int before = failures;
	unsigned seeds = 4;
	const char *env = getenv("FASTSCAN_SEEDS");
	if (env != NULL && atoi(env) > 0)
		seeds = (unsigned)atoi(env);
	printf("Faster scan model (HOST-MODELLED; %u seeds; classic -> fast)\n", seeds);
	printf("%-26s %3s %3s | %-17s | %-15s | %-15s | %-11s\n", "row", "n", "tau", "s a round", "overs caught",
	       "median catch s", "fast false quiet");
	for (unsigned ri = 0; ri < sizeof fx_rows / sizeof fx_rows[0]; ri++) {
		const fx_row_t *row = &fx_rows[ri];
		for (unsigned si = 0; si < 3; si++) {
			for (unsigned ti = 0; ti < 5; ti++) {
				if (row->kind == FX_NONE && ti > 0)
					continue;	/* a quiet zone's time doesn't depend on the lag */
				fx_res_t sum[2];
				memset(sum, 0, sizeof sum);
				double mps[2] = { 0, 0 };
				uint32_t med[2] = { 0, 0 };
				for (unsigned a = 0; a < 2; a++) {
					for (unsigned seed = 1; seed <= seeds; seed++) {
						/* the signal's channel: an FM one (the row's kind replaces it) away from the start */
						fx_setup(sizes[si], row->all_fm, row->kind, (uint16_t)(sizes[si] / 2u + seed % 3u),
							 row->dbm, row->leak, taus[ti]);
						fx.njit = row->njit;
						fx_res_t r = fx_run(a == 1, seed * 7919u + ri * 31u + si, row->kind == FX_NONE ? 20000 : 120000);
						sum[a].overs += r.overs;
						sum[a].caught += r.caught;
						sum[a].missed += r.missed;
						sum[a].holds_quiet += r.holds_quiet;
						mps[a] += r.ms_per_stop / seeds;
						med[a] += r.med_delay / seeds;
					}
				}
				double pass0 = mps[0] * sizes[si] / 1000.0, pass1 = mps[1] * sizes[si] / 1000.0;
				printf("MODEL %-20s %3u %3.0f | %5.2f -> %5.2f %4.1fx | %3u -> %3u of %3u | %5.2f -> %5.2f | %3u\n",
				       row->name, (unsigned)sizes[si], taus[ti], pass0, pass1, pass0 / (pass1 > 0 ? pass1 : 1),
				       sum[0].caught, sum[1].caught, sum[0].overs, med[0] / 1000.0, med[1] / 1000.0,
				       sum[1].missed);
				/* The checks: never slower; in a quiet zone nothing held; every over classic catches, fast catches
				 * too (summed over the seeds), and no stop is ended by the fast rule while the signal is on the air:
				 * with a lag up to 20 ms for FM 3 dB over the squelch line (noise jitter +-6 or not), FM 1 dB over it
				 * without jitter, DMR (leak, jitter) and P25; FM 1 dB over with jitter up to 10 ms; at 30 ms DMR and
				 * P25 without jitter. The rest is printed, not checked (the limits: docs/plus.md); around the
				 * squelch line classic misses too */
				CHECK(pass1 < pass0);
				CHECK(sum[0].holds_quiet == 0 && sum[1].holds_quiet == 0);
				CHECK(sum[0].overs == sum[1].overs);
				bool weak_fm = row->kind == FX_FM && row->dbm < -105;
				bool checked = row->kind != FX_FM_EDGE &&
					       (taus[ti] <= 10 || (taus[ti] <= 20 && !(weak_fm && row->njit != 0)) ||
						((row->kind == FX_DMR || row->kind == FX_P25) && row->njit == 0));
				if (checked) {
					CHECK(sum[1].caught >= sum[0].caught);
					CHECK(sum[1].missed == 0);
				}
				if (sum[1].caught < sum[0].caught)
					printf("NOTE %s n %u tau %.0f: fast caught fewer\n", row->name, (unsigned)sizes[si], taus[ti]);
			}
		}
	}
	return failures - before;
}

int main(void)
{
	int before = failures;
	test_fastscan_semantics();
	test_fastscan_prio_look();
	int t = test_fastscan_table();
	printf("Faster scan radio core: %d checks, %d failures (table %d)\n", checks, failures - before, t);
	return failures != before;
}
