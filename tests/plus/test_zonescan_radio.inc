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
static bool zn_split;	/* channels 9-16 on 2 m: All channels jumps band twice a pass, Mine at 8 of its 12 stops */
static uint16_t zn_dwell;	/* not 0: the scan dwell setting for zn_run */

/* fx_codeplug's 16 channels (zone 0, renamed "All channels"), then zone 1 "Mine" */
static void zn_codeplug(void)
{
	fx_codeplug();
	for (uint16_t n = 2; zn_vhf && n <= 16; n += 2)
		chans[n].rx_hz = chans[n].tx_hz = 145000000u + 12500u * n;
	for (uint16_t n = 9; zn_split && n <= 16; n++)
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
	if (zn_dwell != 0)
		sm.set.scan_dwell_ms = zn_dwell;
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

/* v1.0.3 (docs/scan-slow-2026-11-25.md): the receiver's false carrier right after a retune (the noise reading's dip)
 * longer than the model's 15 ms; nobody has measured it on the radio. Built twice by tools/test_fastscan.py zones:
 * FS_DIP_GUARD 0 is v1.0.2's scan (the "before" rows, and their check that a dip of 25 ms or more holds quiet
 * channels), 1 the fix (the "after" rows: quick, nothing held). Each row prints ms a stop, holds and scanst's DIPS */
static zn_res_t zn_dip_run(bool fast, uint16_t zone, uint32_t dip, uint32_t band_dip, uint32_t cost_us, uint16_t dwell)
{
	fx_dip_ms = dip;
	fx_dip_band_ms = band_dip;
	zn_dwell = dwell;
	fx_setup(16, true, FX_NONE, 0, 0, 0, 0);
	zn_res_t r = zn_run(fast, zone, 0, cost_us, 5, 20000);
	fx_dip_ms = FX_DIP_MS;
	fx_dip_band_ms = 0;
	zn_dwell = 0;
	return r;
}

static void zn_dip_print(const char *what, bool fast, const zn_res_t *r)
{
	printf("Dip %s, %s (FS_DIP_GUARD %d): %.1f ms a stop, %u stops, held %u, fall-back %u, dips %u\n", what,
	       fast ? "fast" : "classic", FS_DIP_GUARD, r->ms_per_stop, r->stops, (unsigned)r->st.held,
	       (unsigned)r->st.fallback, (unsigned)r->st.dips);
}

static void test_zonescan_dip(void)
{
	static const uint32_t dips[] = { 15, 25, 35, 45 };
	char what[64];
	/* A quiet zone (Mine, 12 FM channels in one band), the dip the same after every retune, a read 0 or 3 ms */
	for (unsigned d = 0; d < 4; d++)
		for (unsigned c = 0; c < 2; c++)
			for (unsigned f = 0; f < 2; f++) {
				zn_res_t r = zn_dip_run(f != 0, 1, dips[d], 0, c * 3000u, 0);
				snprintf(what, sizeof what, "%u ms, channel read %u ms", (unsigned)dips[d], c * 3u);
				zn_dip_print(what, f != 0, &r);
				CHECK(r.stops > 5);
				if (FS_DIP_GUARD) {
					CHECK(r.st.held == 0 && r.st.noticed == 0);
					/* classic: its dwell; fast: three readings from 30 ms (no dip seen), or from FS_DIP_MS after
					 * the retune (a slow read shifts that onto the next 10 ms tick) */
					CHECK(f ? r.ms_per_stop <= (dips[d] < 30 ? 56.0 + c * 3.0 : 86.0 + c * 10.0) :
						  r.ms_per_stop <= 156.0);
					if (dips[d] >= 35)
						CHECK(!f || r.st.dips + 2u >= r.stops);
				} else if (dips[d] >= 25) {
					CHECK(r.st.held > 0 && r.ms_per_stop > 500.0);	/* v1.0.2: quiet channels held */
				}
			}
	/* A short dwell (75 ms: the guard window is 25 ms) and a 35 ms dip: the squelch the dip opens holds the stop until
	 * it closes, then the scan moves on without the resume delay */
	{
		zn_res_t r = zn_dip_run(false, 1, 35, 0, 0, 75);
		zn_dip_print("35 ms, scan dwell 75 ms", false, &r);
		printf("  ... holds ended without the resume delay: %u\n", (unsigned)r.st.dip_holds);
		if (FS_DIP_GUARD)
			CHECK(r.ms_per_stop <= 135.0 && r.st.dip_holds + 2u >= r.st.held);
		else
			CHECK(r.ms_per_stop > 1000.0);
	}
	/* The owner's v1.0.1 report on the model: a dip of 15 ms in one band and 40 ms on a VHF / UHF jump; All
	 * channels (1-8 UHF, 9-16 VHF) jumps twice a pass, Mine at 8 of its 12 stops */
	zn_split = true;
	for (unsigned f = 0; f < 2; f++) {
		zn_res_t all = zn_dip_run(f != 0, 0, 15, 40, 0, 0);
		zn_res_t mine = zn_dip_run(f != 0, 1, 15, 40, 0, 0);
		zn_dip_print("15 ms, 40 ms on a band jump, All channels", f != 0, &all);
		zn_dip_print("15 ms, 40 ms on a band jump, Mine", f != 0, &mine);
		if (FS_DIP_GUARD) {
			CHECK(all.st.held == 0 && mine.st.held == 0);
			CHECK(mine.ms_per_stop <= (f ? 75.0 : 156.0) && all.ms_per_stop <= (f ? 62.0 : 156.0));
		} else {
			CHECK(mine.ms_per_stop > 3.0 * all.ms_per_stop);	/* v1.0.2: the made zone far slower */
		}
	}
	zn_split = false;
	/* An FM station 3 dB over the squelch line in overs, the dip 35 ms: found as with the 15 ms dip, and the fast rule
	 * ends no stop on it while it is on the air */
	if (FS_DIP_GUARD) {
		for (unsigned f = 0; f < 2; f++) {
			unsigned overs = 0, caught = 0, missed = 0, quiet = 0;
			for (uint32_t seed = 1; seed <= 4; seed++) {
				fx_setup(16, true, FX_FM, 8, -105, 0, 0);
				fx_dip_ms = 35;
				fx_res_t r = fx_run(f != 0, seed, 40000);
				fx_dip_ms = FX_DIP_MS;
				overs += r.overs;
				caught += r.caught;
				missed += r.missed;
				quiet += r.holds_quiet;
			}
			printf("Dip 35 ms, FM -105 dBm in overs, %s: %u of %u overs caught, false quiet %u\n",
			       f ? "fast" : "classic", caught, overs, missed);
			CHECK(missed == 0 && caught * 10u >= overs * 9u);
		}
	}
	/* Verifier: a slow step (a channel read of 55 ms: a zone with many scan-skipped or P25 channels read in a step) and
	 * a scan dwell of 100 ms. The guard counts from the end of the retune, the dwell from the step's start: the
	 * longer guard must still end before the last poll of the dwell, or an FM station without a tone (no probe) that
	 * opens the squelch at once is stepped past. Both builds catch every over */
	for (unsigned f = 0; f < 2; f++) {
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
			fs_set_fast(f != 0);
			req(RREQ_ZONE, 1);
			sm.set.scan_dwell_ms = 100;
			fx.overs = 0;
			for (uint32_t t = 1500u; fx.overs < 4; t += 6000u) {
				fx.t0[fx.overs] = now + t + fx_rand(1000);
				fx.len[fx.overs++] = 2000u;
			}
			bool got[4] = { false };
			fx_begin_scan();
			zn_cost_us = 55000u;
			CHECK(sm.scan.cfg.dwell_ms == 100);
			while ((int32_t)(now - fx.t0[3] - 2500u) < 0) {
				fx_ms();
				int k = fx_over_now();
				if (k >= 0 && hw.rx_hz == fx_hz(8) &&
				    (sm.scan.phase == SCAN_PH_HOLD || sm.scan.phase == SCAN_PH_PROBE))
					got[k] = true;
			}
			zn_cost_us = 0;
			for (int k = 0; k < 4; k++) {
				overs++;
				caught += got[k];
			}
			req(RREQ_SCAN, RSCAN_STOP);
			fs_set_fast(true);
		}
		printf("Slow step 55 ms, scan dwell 100 ms, %s (FS_DIP_GUARD %d): %u of %u overs caught\n",
		       f ? "fast" : "classic", FS_DIP_GUARD, caught, overs);
		CHECK(caught == overs);
	}
}

/* v1.0.3, the owner's radio on 1.0.2 (scanst read on the radio): a zone of five P25 channels, nothing on the air, the
 * strength read -72 dBm at every stop (noise 94, the line 60), every stop a carrier by strength, the native receiver
 * took each one, heard no frame, gave it back: 1.9 s a stop, cooled 0. The model: five P25 channels, the strength
 * reading 53 dB high (the noise reading not). A no-frame take learns the channel's strength (fsc_floor): after the first
 * pass the stops are quick. Then a real P25 signal (-95 dBm, read as -42) in overs on one of them, its frames reaching
 * the native receiver: every over still noticed, and its take learns nothing */
static void test_zonescan_p25_strength(void)
{
	fx_all_p25 = true;
	fx_rssi_bias = 53;
	for (unsigned f = 0; f < 2; f++) {
		fx_setup(5, false, FX_NONE, 0, 0, 0, 0);
		fx_res_t r = fx_run(f != 0, 3, 60000);
		fs_stats_t st;
		fs_view(now, &st);
		printf("P25 zone, 5 channels, strength read 53 dB high, %s: %.1f ms a stop over 60 s (%u stops), learned %u, "
		       "masked %u, held %u\n", f ? "fast" : "classic", r.ms_per_stop, r.stops, (unsigned)st.p25_learned,
		       (unsigned)st.p25_masked, (unsigned)st.held);
		if (FS_P25_FLOOR) {
			CHECK(st.p25_learned >= 5u && st.p25_learned <= 7u);
			CHECK(st.p25_masked > 50u);
			/* the first two passes 5 x about 1.9 s (noted, then confirmed), then quick stops */
			CHECK(r.ms_per_stop < (f ? 120.0 : 260.0));
		} else {
			CHECK(r.ms_per_stop > 1000.0);	/* v1.0.2: every stop taken, as on the radio */
		}
	}
	fx_frames = true;
	for (unsigned f = 0; f < 2; f++) {
		unsigned overs = 0, caught = 0;
		uint16_t learned = 0;
		for (uint32_t seed = 1; seed <= 3; seed++) {
			fx_setup(5, false, FX_P25, 2, -95, 0, 0);
			fx_res_t r = fx_run(f != 0, seed, 60000);
			fs_stats_t st;
			fs_view(now, &st);
			overs += r.overs;
			caught += r.caught;
			learned = (uint16_t)(learned + st.p25_learned);
		}
		printf("P25 zone, strength 53 dB high, P25 -95 dBm in overs with frames, %s (FS_P25_FLOOR %d): %u of %u overs "
		       "noticed, learned %u\n", f ? "fast" : "classic", FS_P25_FLOOR, caught, overs, (unsigned)learned);
		/* v1.0.2: 10 of 18 (every stop held 1.9 s on the false strength); with the learned level 13 of 18: the first two
		 * passes (noted, then confirmed: about 19 s of 1.9 s stops) still miss the overs of that time */
		CHECK(overs > 6u && (!FS_P25_FLOOR || caught * 10u >= overs * 7u));
	}
	fx_frames = false;
	fx_all_p25 = false;
	fx_rssi_bias = 0;
}

/* v1.0.3, the owner's radio on 1.0.2: a zone of three, ch 1 DMR, ch 2 P25, ch 3 FM; every step that switches the
 * receiver between FM and DMR took 167-169 ms (scanst.cost); a DMR over from his other radio on ch 1 was never caught,
 * fast or classic. The model: the same zone, a mode switch charged 168 ms on the radio task, the P25 strength 53 dB high,
 * DMR direct mode -60 dBm on ch 1 in overs (no burst reaches the call stack: only the strength shows it). Before, the stop's
 * dwell counted from the step's start and was over when the step returned; now from the end of the retune */
static void test_zonescan_dmr_switch(void)
{
	static const uint8_t modes[3] = { RADIO_MODE_DMR, RADIO_MODE_P25, RADIO_MODE_FM };
	fx_mode_list = modes;
	fx_rssi_bias = 53;
	zn_mode_ms = 168;
	for (unsigned f = 0; f < 2; f++) {
		unsigned overs = 0, caught = 0;
		for (uint32_t seed = 1; seed <= 3; seed++) {
			fx_setup(3, false, FX_DMR, 0, -60, 0, 0);
			fx_res_t r = fx_run(f != 0, seed, 60000);
			overs += r.overs;
			caught += r.caught;
		}
		printf("Zone DMR / P25 / FM, mode switch 168 ms, DMR -60 dBm in overs on ch 1, %s (FS_DIP_GUARD %d): %u of %u "
		       "overs noticed\n", f ? "fast" : "classic", FS_DIP_GUARD, caught, overs);
		CHECK(overs > 6u && (!FS_DIP_GUARD || caught == overs));
	}
	zn_mode_ms = 0;
	fx_rssi_bias = 0;
	fx_mode_list = NULL;
}

/* v1.0.4 (docs/scan-slow-2026-11-25.md section 6), the owner's radio on 1.0.3: the P25 zone "starts slow, then speeds
 * up". The learned P25 strength cost two 1.9 s stops a P25 channel at each scan start (noted, then confirmed) and again
 * every two minutes (the level expired 120 s after its last take). Measured here from the scan's start: ms a stop in the
 * first 10 s, the slow stops (over 1 s: a take) and when the last one of the start ended, ms a stop from 30 s on, the
 * slow stops after the first 60 s (re-learning) and after a restart of the scan in the same zone. Built as v1.0.3
 * (FS_P25_SEED 0: before) and as this version (after) */
typedef struct {
	double ms10, steady, ms10_restart;
	uint32_t slow_end;		/* ms from the start: the end of the start's last slow stop */
	unsigned slow60, slow_late, slow_restart;
} zn_seed_t;

static void zn_seed_reset(bool fast)
{
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
}

/* A quiet zone (the P25 strength high) for run_ms; restart (not 0): the scan stopped and started again then */
static zn_seed_t zn_seed_run(bool fast, uint16_t n, uint32_t run_ms, uint32_t restart)
{
	zn_seed_t r;
	memset(&r, 0, sizeof r);
	fx_setup(n, false, FX_NONE, 0, 0, 0, 0);
	fx.rand = 4242u + n;
	zn_seed_reset(fast);
	uint32_t t0 = now, s0 = sm.scan.steps, last = now, s_last = s0, t30 = 0, s30 = 0, rt = 0, rs = 0;
	bool done10 = false, done_r10 = false;
	while (now - t0 < run_ms) {
		uint32_t e = now - t0;
		if (restart != 0 && rt == 0 && e >= restart) {
			r.steady = (double)(now - t30) / (sm.scan.steps - s30);
			req(RREQ_SCAN, RSCAN_STOP);
			fx_ms();
			fx_begin_scan();
			rt = now;
			rs = s_last = sm.scan.steps;
			last = now;
		}
		if (t30 == 0 && e >= 30000u) {
			t30 = now;
			s30 = sm.scan.steps;
		}
		fx_ms();
		if (sm.scan.steps != s_last) {
			if (now - last > 1000u) {
				if (rt != 0)
					r.slow_restart++;
				else if (now - t0 <= 60000u) {
					r.slow60++;
					r.slow_end = now - t0;
				} else
					r.slow_late++;
			}
			s_last = sm.scan.steps;
			last = now;
		}
		if (!done10 && now - t0 >= 10000u) {
			done10 = true;
			r.ms10 = (double)(now - t0) / (sm.scan.steps - s0);
		}
		if (rt != 0 && !done_r10 && now - rt >= 10000u) {
			done_r10 = true;
			r.ms10_restart = (double)(now - rt) / (sm.scan.steps - rs);
		}
	}
	if (restart == 0)
		r.steady = (double)(now - t30) / (sm.scan.steps - s30);
	CHECK(sm.scan.kind == SCAN_ZONE);
	req(RREQ_SCAN, RSCAN_STOP);
	fs_set_fast(true);
	return r;
}

static void test_zonescan_p25_seed(void)
{
	static const uint8_t mixed[3] = { RADIO_MODE_DMR, RADIO_MODE_P25, RADIO_MODE_FM };
	fx_rssi_bias = 53;
	for (unsigned z = 0; z < 2; z++) {
		/* the owner's two zones: five P25 channels; DMR / P25 / FM with the 168 ms mode switch */
		uint16_t n = z == 0 ? 5 : 3;
		unsigned p25 = z == 0 ? 5u : 1u;
		fx_all_p25 = z == 0;
		fx_mode_list = z == 0 ? NULL : mixed;
		zn_mode_ms = z == 0 ? 0 : 168;
		for (unsigned f = 0; f < 2; f++) {
			zn_seed_t a = zn_seed_run(f != 0, n, 300000, 0);
			zn_seed_t b = zn_seed_run(f != 0, n, 90000, 60000);
			printf("P25 seed (FS_P25_SEED %d) %s %s: first 10 s %.0f ms a stop, %u slow stops at the start (the last "
			       "ended %.1f s in), from 30 s %.1f ms a stop, slow stops 60-300 s %u; restart at 60 s: first 10 s %.0f "
			       "ms a stop, %u slow stops\n", FS_P25_SEED, z == 0 ? "5 x P25" : "DMR/P25/FM", f ? "fast" : "classic",
			       a.ms10, a.slow60, a.slow_end / 1000.0, a.steady, a.slow_late, b.ms10_restart, b.slow_restart);
			if (FS_P25_SEED) {
				CHECK(a.slow60 <= p25 + 1u && a.slow_late == 0 && b.slow_restart == 0);
				CHECK(b.ms10_restart < (f ? 200.0 : 400.0));
			}
		}
	}
	/* Real signals: overs caught, 3 seeds x 60 s each, from the scan's start (the overs begin 2-5 s in) */
	static const struct { uint8_t zone, kind, pos; int16_t dbm; const char *name; } sig[] = {
		{ 0, FX_P25, 2, -95, "5 x P25, P25 -95 dBm with frames on ch 3" },
		{ 1, FX_P25, 1, -95, "DMR/P25/FM, P25 -95 dBm with frames on ch 2" },
		{ 1, FX_DMR, 0, -60, "DMR/P25/FM, DMR -60 dBm on ch 1" },
		{ 1, FX_FM, 2, -105, "DMR/P25/FM, FM -105 dBm on ch 3" },
	};
	for (unsigned i = 0; i < sizeof sig / sizeof sig[0]; i++) {
		fx_all_p25 = sig[i].zone == 0;
		fx_mode_list = sig[i].zone == 0 ? NULL : mixed;
		zn_mode_ms = sig[i].zone == 0 ? 0 : 168;
		fx_frames = sig[i].kind == FX_P25;
		for (unsigned f = 0; f < 2; f++) {
			unsigned overs = 0, caught = 0, early = 0, early_caught = 0;
			for (uint32_t seed = 1; seed <= 3; seed++) {
				fx_setup(sig[i].zone == 0 ? 5 : 3, false, sig[i].kind, sig[i].pos, sig[i].dbm, 0, 0);
				fx_res_t r = fx_run(f != 0, seed, 60000);
				overs += r.overs;
				caught += r.caught;
				for (int k = 0; k < fx.overs; k++) {
					if (fx.t0[k] - 1000u < 20000u) {	/* the first 20 s (start() sets the time to 1000) */
						early++;
						early_caught += fx.caught[k];
					}
				}
			}
			printf("P25 seed (FS_P25_SEED %d) %s, %s: %u of %u overs noticed (in the first 20 s %u of %u)\n",
			       FS_P25_SEED, sig[i].name, f ? "fast" : "classic", caught, overs, early_caught, early);
			if (FS_P25_SEED)
				CHECK(overs > 6u && caught * 10u >= overs * (sig[i].kind == FX_P25 ? 8u : 10u));
		}
	}
	fx_frames = false;
	fx_all_p25 = false;
	fx_mode_list = NULL;
	zn_mode_ms = 0;
	fx_rssi_bias = 0;
}

int main(void)
{
	int before = failures;
	if (FS_DIP_GUARD && !FS_P25_SEED) {	/* the v1.0.3 build (before): the seed rows only */
		test_zonescan_p25_seed();
		printf("Zone scan P25 seed rows, v1.0.3 (FS_P25_SEED 0): %d checks, %d failures\n", checks, failures - before);
		return failures != before;
	}
	test_zonescan_dip();
	test_zonescan_p25_strength();
	test_zonescan_dmr_switch();
	if (!FS_DIP_GUARD) {	/* the "before" build: v1.0.2's rows only */
		printf("Zone scan dip rows, v1.0.2 (FS_DIP_GUARD 0): %d checks, %d failures\n", checks, failures - before);
		return failures != before;
	}
	test_zonescan_p25_seed();
	test_zonescan_same();
	test_zonescan_priority();
	test_zonescan_figures();
	test_zonescan_short_dwell();
	printf("Zone scan, a zone made vs All channels: %d checks, %d failures\n", checks, failures - before);
	return failures != before;
}
