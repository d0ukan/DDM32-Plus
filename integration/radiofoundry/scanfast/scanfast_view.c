/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus faster channel scan (radio/scanfast.h): the debug link section "scanst" (GET_VIEW 31), the zone / VFO
 * scan's counters for the PC. Read only. Keys (new keys may be added; the meaning of these stays):
 *   scanst.built=0|1
 *   scanst.on=0|1          a zone or VFO scan runs
 *   scanst.st=ALGO,UP_MS,STOPS,PASSES,PASS_MS,PASS_STOPS,NOTICED,HELD,FAST_QUIET,FALLBACK,LATE
 *     ALGO fast | classic; UP_MS since the scan (or the counters, after a switch) started, a stopped scan to its end;
 *     STOPS stops made; PASSES complete passes of the zone / band (the first wrap starts the count), PASS_MS and
 *     PASS_STOPS the last one's time and stops; NOTICED stops that probed or held (something heard), HELD those that
 *     held (a signal the scan stops for); FAST_QUIET stops ended quiet by the fast rule; FALLBACK stops the fast rule
 *     left to the classic rules (a reading not clearly quiet); LATE spans started again after a late reading
 *   scanst.quiet=FM_N,FM_MS,DMR_N,DMR_MS,P25_N,P25_MS,OTHER_N,OTHER_MS
 *     quiet stops (nothing heard) by mode and their total time, tune to the next tune (P25 counts YSF; OTHER NXDN and
 *     dPMR); FM_MS / FM_N is an FM channel's quiet dwell
 *   scanst.rate=CPS_X10    quiet channels a second, times ten (all modes: quiet stops / their time)
 *   scanst.slow=MS,WHERE,CH,MODE,WHY
 *     the longest stop since the counters started: its time (tune to the next tune), the sweep position (VFO: Hz), the
 *     channel number (0: VFO), FM | DMR | P25 | OTHER, WHY bits 1 something heard (probe or hold), 2 held, 4 handed
 *     to the classic rules by the fast rule, 8 in another zone (a priority channel there)
 *   scanst.fb=CH,MODE,NOISE,LINE,RSSI
 *     the last stop the fast rule handed to the classic rules: its channel, mode, and the reading that did it: the
 *     noise reading (0: the squelch was already open), the noise it had to reach (the squelch's opening line + the
 *     margin) and the strength in dBm (DMR and the native modes must be FS_RSSI_MARGIN under DMR's strength line)
 *   scanst.cost=GAP_MAX,STEP_MAX,OTHER_ZONE,SWITCHES
 *     the radio task: the longest gap between two of its 10 ms ticks while a stop listened, the longest step the tick
 *     made (reading the next channel and retuning), both ms; stops in another zone than the one scanned; stops whose
 *     tune switched the receiver between FM and DMR
 * Counts are modulo 65536. */
#include "config.h"
#include "debuglink/debuglink.h"
#if CONFIG_SCAN_FAST
#include "hal_time.h"
#include "radio/scanfast.h"
#endif

void scanfast_view(dl_text_t *t)
{
#if CONFIG_SCAN_FAST
	fs_stats_t v;
	uint32_t n = 0, ms = 0;

	fs_view(hal_millis(), &v);
	for (unsigned m = 0; m < FS_MODES; m++) {
		n += v.q_n[m];
		ms += v.q_ms[m];
	}
	dl_text(t, "scanst.built=1\nscanst.on=%u\n", v.on ? 1u : 0u);
	dl_text(t, "scanst.st=%s,%lu,%u,%u,%lu,%u,%u,%u,%u,%u,%u\n", v.fast ? "fast" : "classic",
		(unsigned long)(v.mark_ms - v.start_ms), (unsigned)v.stops, (unsigned)v.passes, (unsigned long)v.pass_ms,
		(unsigned)v.pass_stops, (unsigned)v.noticed, (unsigned)v.held, (unsigned)v.fast_quiet, (unsigned)v.fallback,
		(unsigned)v.late);
	dl_text(t, "scanst.quiet=%u,%lu,%u,%lu,%u,%lu,%u,%lu\n", (unsigned)v.q_n[FS_M_FM], (unsigned long)v.q_ms[FS_M_FM],
		(unsigned)v.q_n[FS_M_DMR], (unsigned long)v.q_ms[FS_M_DMR], (unsigned)v.q_n[FS_M_P25],
		(unsigned long)v.q_ms[FS_M_P25], (unsigned)v.q_n[FS_M_OTHER], (unsigned long)v.q_ms[FS_M_OTHER]);
	dl_text(t, "scanst.rate=%lu\n", ms != 0 ? (unsigned long)((n * 10000u + ms / 2u) / ms) : 0ul);
	static const char *const mode[FS_MODES + 1] = { "FM", "DMR", "P25", "OTHER", "-" };
	dl_text(t, "scanst.slow=%lu,%lu,%u,%s,%u\n", (unsigned long)v.slow_ms, (unsigned long)v.slow_where,
		(unsigned)v.slow_ch, mode[v.slow_mode < FS_MODES ? v.slow_mode : FS_MODES], (unsigned)v.slow_why);
	dl_text(t, "scanst.fb=%u,%s,%u,%u,%d\n", (unsigned)v.fb_ch, mode[v.fb_mode < FS_MODES ? v.fb_mode : FS_MODES],
		(unsigned)v.fb_noise, (unsigned)v.fb_line, (int)v.fb_rssi);
	dl_text(t, "scanst.cost=%u,%u,%u,%u\n", (unsigned)v.gap_max, (unsigned)v.step_max, (unsigned)v.other_zone,
		(unsigned)v.switches);
#else
	dl_text(t, "scanst.built=0\n");
#endif
}
