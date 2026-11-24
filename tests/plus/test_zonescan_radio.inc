/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus v1.0.2 candidate: the owner's report "a zone I made scans very slowly, classic and fast; All channels
 * scans fast" (2026-11-24, v1.0.1). The real radio state machine's zone scan on the fake board (the model of
 * tests/plus/test_fastscan_radio.inc, included before this file: the receiver's readings, the radio task's tick and
 * poll with their lateness) on two stored zones of the same channels: zone 0 "All channels" (1..16, the stored copy a
 * radio with zones has) and zone 1 "Mine" (12 of them, in another order, as a zone made on the radio). The fixture
 * counts the codeplug reads (codeplug_channel, codeplug_zone) and can charge each channel read a time on the radio task
 * (zs_cost_us: the real codeplug walks the data chip's settings area for every channel, tests/plus/test_zone_cap.c
 * shows how many reads). Checks:
 *   - the same ms a stop, the same codeplug reads a stop, in both zones, fast and classic, with and without a read cost
 *     (the scan reads the zone once when it is chosen; a stop reads its channel the same way in any zone);
 *   - a priority channel set from All channels that the zone made doesn't hold: a stop there every
 *     SCANPRIO_EVERY_MS (counted as OTHER_ZONE in scanst.cost), the zone's own stops as fast as without it;
 *   - the new scanst figures: the longest stop (scanst.slow), the reading that handed a stop to the classic rules
 *     (scanst.fb: on a channel with a carrier), the radio task's tick gap and step time (scanst.cost, the read cost
 *     shows there), the FM / DMR switches. */

static const uint16_t zn_mine[] = { 12, 3, 9, 1, 16, 5, 7, 2, 14, 10, 6, 11 };
#define ZN_MINE (sizeof zn_mine / sizeof zn_mine[0])

typedef struct {
	double ms_per_stop, reads_per_stop;
	fs_stats_t st;
	unsigned stops;
} zn_res_t;

static bool zn_vhf;	/* every other channel on 2 m (the band counter's case) */

/* fx_codeplug's 16 channels (zone 0, renamed "All channels"), then zone 1 "Mine" */
static void zn_codeplug(void)
{
	fx_codeplug();
	for (uint16_t n = 2; zn_vhf && n <= 16; n += 2)
		chans[n].rx_hz = chans[n].tx_hz = 145000000u + 12500u * n;
	strcpy(zones[0].name, "All channels");
	memset(&zones[1], 0, sizeof zones[1]);
	strcpy(zones[1].name, "Mine");
	for (unsigned i = 0; i < ZN_MINE; i++)
		zones[1].channels[i] = zn_mine[i];
	zones[1].count = (uint16_t)ZN_MINE;
	nzones = 2;
}

static zn_res_t zn_run(bool fast, uint16_t zone, uint32_t prio, uint32_t cost_us, uint32_t seed, uint32_t run_ms)
{
	zn_res_t r;
	memset(&r, 0, sizeof r);
	fx.rand = seed * 2654435761u + 777u;
	zn_codeplug();
	start();
	native_lab_init();
	fake_power.switch_on = classic_on;
	fake_power.battery_mv = classic_battery;
	classic_busy = classic_stop = classic_receiving = false;
	classic_starts = classic_restores = 0;
	scan_voice_ms = zs_sync_ms = 0;
	fs_set_fast(fast);
	if (zone != 0)
		req(RREQ_ZONE, zone);
	CHECK(sm.view.zone_index == zone);
	radio_scan_prio_set(prio);
	zn_cost_us = cost_us;
	fx_begin_scan();
	uint32_t steps0 = 0, t0 = 0, reads0 = 0;
	for (uint32_t t = 0; t < run_ms; t++) {
		if (t == 2000u) {
			steps0 = sm.scan.steps;
			t0 = now;
			reads0 = zn_reads;
		}
		fx_ms();
	}
	CHECK(sm.scan.kind == SCAN_ZONE);
	r.stops = sm.scan.steps - steps0;
	r.ms_per_stop = r.stops ? (double)(now - t0) / r.stops : 0;
	r.reads_per_stop = r.stops ? (double)(zn_reads - reads0) / r.stops : 0;
	fs_view(now, &r.st);
	req(RREQ_SCAN, RSCAN_STOP);
	radio_scan_prio_set(0);
	zn_cost_us = 0;
	fs_set_fast(true);
	return r;
}

static bool zn_close(double a, double b, double tol)
{
	return a > b * (1.0 - tol) && a < b * (1.0 + tol);
}

/* The two zones side by side: quiet FM channels, fast and classic, a channel read costing 0, 3 and 12 ms */
static void test_zonescan_same(void)
{
	static const uint32_t costs[] = { 0, 3000, 12000 };
	for (unsigned f = 0; f < 2; f++) {
		double base = 0;
		for (unsigned c = 0; c < 3; c++) {
			fx_setup(16, true, FX_NONE, 0, 0, 0, 0);
			zn_res_t all = zn_run(f != 0, 0, 0, costs[c], 1, 20000);
			fx_setup(16, true, FX_NONE, 0, 0, 0, 0);
			zn_res_t mine = zn_run(f != 0, 1, 0, costs[c], 1, 20000);
			printf("Zone scan, %s, channel read %u ms: All channels %.1f ms a stop, %.2f reads (pass %lu ms / %u stops); "
			       "Mine %.1f ms, %.2f reads (pass %lu ms / %u stops); tick gap %u / %u ms, step %u / %u ms\n",
			       f ? "fast" : "classic", (unsigned)(costs[c] / 1000u), all.ms_per_stop, all.reads_per_stop,
			       (unsigned long)all.st.pass_ms, (unsigned)all.st.pass_stops, mine.ms_per_stop, mine.reads_per_stop,
			       (unsigned long)mine.st.pass_ms, (unsigned)mine.st.pass_stops, (unsigned)all.st.gap_max,
			       (unsigned)mine.st.gap_max, (unsigned)all.st.step_max, (unsigned)mine.st.step_max);
			CHECK(all.stops > 50 && mine.stops > 50);
			CHECK(zn_close(mine.ms_per_stop, all.ms_per_stop, 0.03));
			CHECK(zn_close(mine.reads_per_stop, all.reads_per_stop, 0.03));
			/* nothing heard (no false hold after a slow step), no zone change, no FM / DMR switch, no fall-back */
			CHECK(mine.st.noticed == 0 && all.st.noticed == 0 && mine.st.other_zone == 0 && mine.st.switches == 0);
			CHECK(mine.st.fallback == 0 && all.st.fallback == 0);
			CHECK(mine.st.slow_why == 0 && mine.st.slow_mode == FS_M_FM && mine.st.slow_ch != 0);
			/* one channel read a stop: the one picked, read once (v1.0.1: three, the start channel's at every stop
			 * and the picked one twice) */
			CHECK(zn_close(mine.reads_per_stop, 1.0, 0.05));
			/* a slow read makes a stop no longer than that read (fast: its first reading waits for the retune, on
			 * the 10 ms tick) and classic's dwell is timed as before (v1.0.1 classic at 3 ms a read, three reads a
			 * stop: 290 ms a stop instead of 152, quiet channels held for the resume delay) */
			if (c == 0)
				base = mine.ms_per_stop;
			CHECK(mine.ms_per_stop <= base + (f ? costs[c] / 1000.0 + 10.0 : 2.0));
			/* the read cost shows in the step the tick made (fast) or in the tick gap (classic's poll stepped) */
			if (costs[c] != 0)
				CHECK((f ? mine.st.step_max : mine.st.gap_max) + 1u >= costs[c] / 1000u + (f ? 0u : 10u));
			else
				CHECK(mine.st.step_max <= 1u && mine.st.gap_max <= 14u);
		}
	}
}

/* A priority channel set from All channels (channel 15: not in Mine) while Mine is scanned */
static void test_zonescan_priority(void)
{
	fx_setup(16, true, FX_NONE, 0, 0, 0, 0);
	zn_res_t plain = zn_run(true, 1, 0, 0, 2, 20000);
	fx_setup(16, true, FX_NONE, 0, 0, 0, 0);
	zn_res_t prio = zn_run(true, 1, scanprio_pack(15, 0), 0, 2, 20000);
	unsigned expect = (unsigned)((prio.st.mark_ms - prio.st.start_ms) / SCANPRIO_EVERY_MS);
	printf("Zone scan of Mine with a priority channel from All channels: %u stops there in %lu ms (one each %u ms), "
	       "%.1f ms a stop (%.1f without), pass %lu ms (%lu without)\n", (unsigned)prio.st.other_zone,
	       (unsigned long)(prio.st.mark_ms - prio.st.start_ms), (unsigned)SCANPRIO_EVERY_MS, prio.ms_per_stop,
	       plain.ms_per_stop, (unsigned long)prio.st.pass_ms, (unsigned long)plain.st.pass_ms);
	CHECK(plain.st.other_zone == 0);
	CHECK(prio.st.other_zone + 2u >= expect && prio.st.other_zone <= expect + 2u);
	/* one extra quiet stop every 2 s: the pass a stop or so longer, never seconds */
	CHECK(prio.st.pass_ms <= plain.st.pass_ms + 2u * (uint32_t)plain.ms_per_stop + 30u);
	CHECK(zn_close(prio.ms_per_stop, plain.ms_per_stop, 0.05));
}

/* An FM carrier on one channel of Mine: the stop that heard it is the longest (held), the fast rule's hand-over
 * recorded with the reading under the line; a mixed zone counts its FM / DMR switches */
static void test_zonescan_figures(void)
{
	fx_setup(16, true, FX_FM, 8, -105, 0, 0);	/* position 8 of zone 0 = channel 9, Mine holds it */
	fx.overs = 1;
	zn_res_t r;
	{
		/* one over of 4 s from 3 s after the start */
		memset(&r, 0, sizeof r);
		fx.rand = 99;
		zn_codeplug();
		start();
		native_lab_init();
		fake_power.switch_on = classic_on;
		fake_power.battery_mv = classic_battery;
		classic_busy = classic_stop = classic_receiving = false;
		fs_set_fast(true);
		req(RREQ_ZONE, 1);
		fx.t0[0] = now + 3000u;
		fx.len[0] = 4000u;
		fx_begin_scan();
		for (uint32_t t = 0; t < 12000u; t++)
			fx_ms();
		fs_view(now, &r.st);
		req(RREQ_SCAN, RSCAN_STOP);
	}
	printf("scanst.slow=%lu,%lu,%u,%u,%u scanst.fb=%u,%u,%u,%u,%d\n", (unsigned long)r.st.slow_ms,
	       (unsigned long)r.st.slow_where, (unsigned)r.st.slow_ch, (unsigned)r.st.slow_mode, (unsigned)r.st.slow_why,
	       (unsigned)r.st.fb_ch, (unsigned)r.st.fb_mode, (unsigned)r.st.fb_noise, (unsigned)r.st.fb_line,
	       (int)r.st.fb_rssi);
	CHECK(r.st.held >= 1);
	CHECK(r.st.slow_ch == 9 && (r.st.slow_why & (FS_WHY_HELD | FS_WHY_FALLBACK)) == (FS_WHY_HELD | FS_WHY_FALLBACK));
	CHECK(r.st.slow_ms >= 3000u);
	CHECK(r.st.fb_ch == 9 && r.st.fb_mode == FS_M_FM && (r.st.fb_noise == 0 || r.st.fb_noise < r.st.fb_line));
	CHECK(r.st.fb_why == FS_FB_NOISE || r.st.fb_why == FS_FB_OPEN);
	CHECK(((r.st.slow_why >> FS_WHY_FB_SHIFT) & 7u) == r.st.fb_why);

	/* The band counter: Mine with every even channel on 2 m (12, 16, 2, 14, 10, 6 VHF; 3, 9, 1, 5, 7, 11 UHF) */
	zn_vhf = true;
	fx_setup(16, true, FX_NONE, 0, 0, 0, 0);
	zn_res_t band_run = zn_run(true, 1, 0, 0, 4, 10000);
	zn_vhf = false;
	printf("Mine across VHF / UHF: %u band changes in %u stops, %u AM stops\n", (unsigned)band_run.st.bands,
	       (unsigned)band_run.st.stops, (unsigned)band_run.st.am_stops);
	CHECK(band_run.st.bands > band_run.st.stops / 3u && band_run.st.bands <= band_run.st.stops && band_run.st.am_stops == 0);
	CHECK(band_run.st.noticed == 0 && band_run.st.fallback == 0);
	fx_setup(16, true, FX_NONE, 0, 0, 0, 0);
	zn_res_t one = zn_run(true, 1, 0, 0, 4, 10000);
	CHECK(one.st.bands == 0);

	/* fx's mixed zone (channels 4, 8, 12, 16 DMR, 6, 14 P25): Mine holds DMR 12 and 16 among FM and P25 ones */
	fx_setup(16, false, FX_NONE, 0, 0, 0, 0);
	zn_res_t mixed = zn_run(true, 1, 0, 0, 3, 20000);
	printf("Mixed zone Mine: %u FM / DMR switches in %u stops\n", (unsigned)mixed.st.switches, (unsigned)mixed.st.stops);
	CHECK(mixed.st.switches > 0 && mixed.st.switches <= mixed.st.stops);
}

/* A short scan dwell (60 ms, the setting's range starts at 50): classic, no read time, an FM signal without a tone on
 * one channel of Mine. The retune guard is off under FS_RETUNE_GUARD_DWELL_MS, so classic catches what it caught */
static void test_zonescan_short_dwell(void)
{
	unsigned overs = 0, caught = 0;
	for (uint32_t seed = 1; seed <= 4; seed++) {
		fx_setup(16, true, FX_FM, 8, -105, 0, 0);
		fx.rand = seed * 7919u;
		zn_codeplug();
		start();
		native_lab_init();
		fake_power.switch_on = classic_on;
		fake_power.battery_mv = classic_battery;
		classic_busy = classic_stop = classic_receiving = false;
		fs_set_fast(false);
		req(RREQ_ZONE, 1);
		sm.set.scan_dwell_ms = 60;
		fx.overs = 0;
		for (uint32_t t = 1500u; fx.overs < 4; t += 6000u) {
			fx.t0[fx.overs] = now + t + fx_rand(1000);
			fx.len[fx.overs++] = 2000u;
		}
		bool got[4] = { false };
		fx_begin_scan();
		CHECK(sm.scan.cfg.dwell_ms == 60);
		while ((int32_t)(now - fx.t0[3] - 2500u) < 0) {
			fx_ms();
			/* caught: held (or probed) on channel 9 while its over is on (fx's own count is by zone 0's position) */
			int k = fx_over_now();
			if (k >= 0 && hw.rx_hz == fx_hz(8) &&
			    (sm.scan.phase == SCAN_PH_HOLD || sm.scan.phase == SCAN_PH_PROBE))
				got[k] = true;
		}
		for (int k = 0; k < 4; k++) {
			overs++;
			caught += got[k];
		}
		req(RREQ_SCAN, RSCAN_STOP);
		fs_set_fast(true);
	}
	printf("Classic, scan dwell 60 ms: %u of %u overs caught\n", caught, overs);
	CHECK(caught == overs);
}

int main(void)
{
	int before = failures;
	test_zonescan_same();
	test_zonescan_priority();
	test_zonescan_figures();
	test_zonescan_short_dwell();
	printf("Zone scan, a zone made vs All channels: %d checks, %d failures\n", checks, failures - before);
	return failures != before;
}
