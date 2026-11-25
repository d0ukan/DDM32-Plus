/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus search log (tools/plus_searchlog.py, docs/plus.md "Search log on the PC"): the debug link sections
 * "p25s" and "dmrs" carry the P25 and DMR searches' state and found lists, so the PC programmer can keep every call a
 * search finds all night; "alls" the all digital search's. Read only and not counted as a look
 * (p25s_peek, dmrs_peek, dmrs_peek_all): a search
 * still stops 5 s after its screen was left. Keys (new keys may be added; the meaning of these stays):
 *   X.on=0|1 X.phase=N X.at=HZ X.lo=HZ X.hi=HZ X.n=N X.seq=N
 *   p25s.fI=HZ,NAC,TG,SRC,CALLS,AGE_S,ID          (I = 0 newest .. min(n, SEARCHLOG_ROWS) - 1)
 *   dmrs.fI=HZ,CC,TSMASK,SYNCMASK,GROUP,ENC,DST,SRC,CALLS,AGE_S,ID   (CC 255 = not named yet)
 *   alls.mode=P25|DMR|YSF|NXDN   (the mode probed or played now)
 *   alls.fI=HZ,MODE,CODE,TSMASK,SYNCMASK,GROUP,ENC,DST,SRC,CALLS,AGE_S,ID,CALL
 *     MODE P25 / DMR / YSF / NXDN; CODE the P25 NAC, DMR color code, YSF DG-ID or NXDN RAN (65535 = not known);
 *     TSMASK / SYNCMASK as dmrs (DMR only); DST / SRC the talkgroup (or called ID) and source (YSF: 0); CALL the YSF
 *     source callsign (empty otherwise; the line's last field, it has no comma)
 * AGE_S: seconds since the entry was last heard. The all digital search ("alls", DDM32 Plus) shares the DMR search's
 * engine: whichever of the two ran last has the list, the other reads as off with nothing found.
 * The PC's call count (A2 of the beta.7 hole hunt): X.seq is the engine's calls counted since power on (16 bits,
 * wraps; dmrs and alls show the same engine's), ID an entry's serial given when it was made (p25s 16 bits, dmrs /
 * alls 8 bits) that stays while its code / group are filled in. The PC follows each entry by engine + ID + frequency
 * + mode and adds the change of its CALLS (a call the radio moves to the right talkgroup goes down on one entry and up
 * on the other); seq going back is a restart, seq growing more than the entries' calls is calls lost between polls.
 * DDM32 Plus faster searches (CONFIG_SEARCH_HYBRID, radio/searchhy.h): the section of the engine that ran last ends
 * with its counters (the line is left out when the section is full; the other sections have none):
 *   X.st=ALGO,UP_MS,PASSES,PASS_MS,PASS_STEPS,PROBES,FOUND,NOSYNC,PARTIAL,GONE,SKIPPED,EARLY,WATCH,PROBE_MS,SWEEP_MS,
 *        FIRST_MS,SEEN_MS,SYNC_LAST,SYNC_MAX
 *     ALGO fast | classic; UP_MS since the search started (stopped: to its last probe / sweep change); PASS_MS /
 *     PASS_STEPS the last complete pass (0 = none yet); the probes' outcomes; SKIPPED targets in the skip list; EARLY
 *     probes ended early; WATCH probes of watched carriers; PROBE_MS / SWEEP_MS the time in each; FIRST_MS start to
 *     the first find, SEEN_MS the last find's carrier first read strong to found (0 = none); SYNC_LAST / SYNC_MAX a
 *     found probe's wait for its mode's first frame. */
#include "config.h"
#include "hal_time.h"
#include "debuglink/debuglink.h"
#include "radio/searchlog_view.h"
#if CONFIG_P25_SEARCH
#include "radio/p25search.h"
#endif
#if CONFIG_DMR_SEARCH
#include "radio/dmrsearch.h"
#endif
#if CONFIG_SEARCH_HYBRID
#include "radio/searchhy.h"
#endif

/* DDM32 Plus searchlist: the radio keeps 16 finds a search (P25S_FOUND_MAX, DMRS_FOUND_MAX) but a section still
 * carries the newest 8, as before. A reply holds 1002 bytes of text (dl_frame.h DL_REPLY_MAX less GET_VIEW's header
 * and truncation mark): 16 "alls" lines would not fit with the header and the counters' line. The PC programmers
 * read f0..f7 and miss nothing: an entry that hears a call moves to the top of the list (X.n stays the radio's own
 * count, up to 16) */
#define SEARCHLOG_ROWS		8u

static unsigned age_s(uint32_t now, uint32_t heard)
{
	return (unsigned)((now - heard) / 1000u);
}

#if CONFIG_SEARCH_HYBRID
/* The counters' line, when this section's engine owns them (radio/searchhy.h) */
static void __attribute__((noinline)) st_line(dl_text_t *t, const char *x, uint8_t owner, bool running, uint32_t now)
{
	sh_stats_t v;
	if (!sh_view(owner, running, now, &v))
		return;
	dl_text(t, "%s.st=%s,%lu,%u,%lu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%lu,%lu,%lu,%lu,%u,%u\n", x, v.fast ? "fast" : "classic",
		(unsigned long)(v.mark_ms - v.start_ms), (unsigned)v.passes, (unsigned long)v.pass_ms,
		(unsigned)v.pass_steps, (unsigned)v.probes, (unsigned)v.found, (unsigned)v.nosync, (unsigned)v.partial,
		(unsigned)v.gone, (unsigned)v.skipped, (unsigned)v.early, (unsigned)v.watch, (unsigned long)v.probe_ms,
		(unsigned long)v.sweep_ms, (unsigned long)v.first_ms, (unsigned long)v.seen_ms, (unsigned)v.sync_last,
		(unsigned)v.sync_max);
}
#endif

void searchlog_view_p25(dl_text_t *t)
{
#if CONFIG_P25_SEARCH
	p25s_peek_t p;
	uint32_t now = hal_millis();

	(void)p25s_peek(0u, &p);
	dl_text(t, "p25s.on=%u\np25s.phase=%u\np25s.at=%lu\np25s.lo=%lu\np25s.hi=%lu\np25s.n=%u\np25s.seq=%u\n",
		p.phase != P25S_OFF ? 1u : 0u, (unsigned)p.phase, (unsigned long)p.at_hz, (unsigned long)p.lo_hz,
		(unsigned long)p.hi_hz, (unsigned)p.found_n, (unsigned)p.seq);
	for (unsigned i = 0; i < SEARCHLOG_ROWS && i < P25S_FOUND_MAX; i++) {
		if (!p25s_peek(i, &p))
			break;
		dl_text(t, "p25s.f%u=%lu,%u,%u,%lu,%u,%u,%u\n", i, (unsigned long)p.f.hz, (unsigned)p.f.nac,
			(unsigned)p.f.tg, (unsigned long)p.f.src, (unsigned)p.f.calls, age_s(now, p.f.heard_ms),
			(unsigned)p.f.id);
	}
#if CONFIG_SEARCH_HYBRID
	st_line(t, "p25s", SH_P25, p25s_on(), now);
#endif
#else
	dl_text(t, "p25s.built=0\n");
#endif
}

void searchlog_view_dmr(dl_text_t *t)
{
#if CONFIG_DMR_SEARCH
	dmrs_peek_t p;
	uint32_t now = hal_millis();

	(void)dmrs_peek(0u, &p);
	dl_text(t, "dmrs.on=%u\ndmrs.phase=%u\ndmrs.at=%lu\ndmrs.lo=%lu\ndmrs.hi=%lu\ndmrs.n=%u\ndmrs.seq=%u\n",
		p.phase != DMRS_OFF ? 1u : 0u, (unsigned)p.phase, (unsigned long)p.at_hz, (unsigned long)p.lo_hz,
		(unsigned long)p.hi_hz, (unsigned)p.found_n, (unsigned)p.seq);
	for (unsigned i = 0; i < SEARCHLOG_ROWS && i < DMRS_FOUND_MAX; i++) {
		if (!dmrs_peek(i, &p))
			break;
		dl_text(t, "dmrs.f%u=%lu,%u,%u,%u,%u,%u,%lu,%lu,%u,%u,%u\n", i, (unsigned long)p.f.hz,
			p.f.cc == DMRS_CC_NONE ? 255u : (unsigned)p.f.cc, (unsigned)p.f.ts_mask, (unsigned)p.f.sync_mask,
			p.f.group ? 1u : 0u, p.f.enc ? 1u : 0u, (unsigned long)p.f.dst, (unsigned long)p.f.src,
			(unsigned)p.f.calls, age_s(now, p.f.heard_ms), (unsigned)p.f.id);
	}
#if CONFIG_SEARCH_HYBRID
	st_line(t, "dmrs", SH_DMR, dmrs_on(), now);
#endif
#else
	dl_text(t, "dmrs.built=0\n");
#endif
}

void searchlog_view_all(dl_text_t *t)
{
#if CONFIG_ALL_SEARCH
	dmrs_peek_t p;
	uint32_t now = hal_millis();

	(void)dmrs_peek_all(0u, &p);
	dl_text(t, "alls.on=%u\nalls.phase=%u\nalls.mode=%s\nalls.at=%lu\nalls.lo=%lu\nalls.hi=%lu\nalls.n=%u\nalls.seq=%u\n",
		p.phase != DMRS_OFF ? 1u : 0u, (unsigned)p.phase, dmrs_mode_name(p.mode), (unsigned long)p.at_hz,
		(unsigned long)p.lo_hz, (unsigned long)p.hi_hz, (unsigned)p.found_n, (unsigned)p.seq);
	for (unsigned i = 0; i < SEARCHLOG_ROWS && i < DMRS_FOUND_MAX; i++) {
		if (!dmrs_peek_all(i, &p))
			break;
		char call[11] = "";
		bool ysf = p.f.mode == DMRS_MODE_YSF;
		if (ysf)
			dmrs_call_unpack(p.f.dst, p.f.src, call);
		dl_text(t, "alls.f%u=%lu,%s,%u,%u,%u,%u,%u,%lu,%lu,%u,%u,%u,%s\n", i, (unsigned long)p.f.hz,
			dmrs_mode_name(p.f.mode), (unsigned)p.f.cc, (unsigned)p.f.ts_mask, (unsigned)p.f.sync_mask,
			p.f.group ? 1u : 0u, p.f.enc ? 1u : 0u, ysf ? 0ul : (unsigned long)p.f.dst,
			ysf ? 0ul : (unsigned long)p.f.src, (unsigned)p.f.calls, age_s(now, p.f.heard_ms), (unsigned)p.f.id,
			call);
	}
#if CONFIG_SEARCH_HYBRID
	st_line(t, "alls", SH_ALL, dmrs_on(), now);
#endif
#else
	dl_text(t, "alls.built=0\n");
#endif
}
