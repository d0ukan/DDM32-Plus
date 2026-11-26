/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 13 ideas 2 UI flows: upstream's UI simulator (tests/host/sim/ui_sim.c) as it is, built in place of
 * it, with the services the tool screens use taken over for the test (ui_sim_toolbox.h): the GPS (position, speed,
 * course, altitude, a running UTC time), the P25 / DMR search lists, the trunk followers' views, the pager's
 * messages, the beeps, and the heap the waterfall takes (counted, can be made to fail). The spectrum sweep is the
 * real engine (radio/spectrum.c): radio_req_spectrum starts and stops it, the test feeds its readings. */
#define ui_svc_position sim_base_position
#define sim_reset sim_base_reset	/* refpack_sim.inc: every reset gets the map pack */
#include "sim/ui_sim.c"
#undef sim_reset
#include "refpack_sim.inc"
#undef ui_svc_position
#include <stdlib.h>
#include "ui_sim_toolbox.h"

tb_sim_t tb_sim;

void tb_sim_clear(void)
{
	memset(&tb_sim, 0, sizeof tb_sim);
}

void tb_sim_rssi(int16_t dbm)
{
	view.rssi_dbm = dbm;
	changed();
}

void tb_sim_changed(void)
{
	changed();	/* the radio's view changed: the screen on top draws again (as the sweep's own updates do) */
}

uint32_t tb_sim_freq(void)
{
	return view.ch.rx_hz;
}

static void req(const char *what, long v)
{
	size_t n = strlen(tb_sim.req);
	snprintf(tb_sim.req + n, sizeof tb_sim.req - n, "%s%s:%ld", n ? " " : "", what, v);
}

bool ui_svc_position(ui_position_t *out)
{
	if (!tb_sim.gps)
		return sim_base_position(out);
	memset(out, 0, sizeof *out);
	out->fix = tb_sim.fix;
	out->sats = tb_sim.fix ? 8 : 0;
	out->mode = tb_sim.fix ? 3 : 1;
	out->lat_e7 = tb_sim.lat_e7;
	out->lon_e7 = tb_sim.lon_e7;
	out->alt_ok = tb_sim.fix;
	out->alt_m = tb_sim.alt_m;
	out->hdop10 = 10;
	out->speed_kmh = tb_sim.fix ? tb_sim.speed_kmh : 0xFFFFu;
	out->course = tb_sim.fix ? tb_sim.course : 0xFFFFu;
	out->utc_hms = 0xFFFFFFFFu;
	if (tb_sim.time) {
		uint32_t s0 = tb_sim.utc_hms / 10000u * 3600u + tb_sim.utc_hms / 100u % 100u * 60u + tb_sim.utc_hms % 100u;
		uint32_t s = (s0 + (now_ms - tb_sim.utc_at) / 1000u) % 86400u;
		out->utc_hms = s / 3600u * 10000u + s / 60u % 60u * 100u + s % 60u;
		out->date_dmy = 201126;
	}
	return true;
}

bool aprs_position_now(int32_t *lat, int32_t *lon, uint32_t *err_m, bool *dmr_gps)
{
	ui_position_t p;
	*err_m = 0;
	*dmr_gps = false;
	if (!ui_svc_position(&p) || !p.fix)
		return false;
	*lat = p.lat_e7;
	*lon = p.lon_e7;
	return true;
}

/* --- The spectrum sweep: the real engine, started and stopped here --- */
void radio_req_spectrum(uint32_t center_hz, uint32_t span_hz)
{
	req("spectrum", (long)(center_hz / 1000u));
	if (center_hz != 0)
		(void)spectrum_start(center_hz, span_hz, now_ms);
	else
		spectrum_stop();
}

/* --- The searches: the lists the test sets; a start / stop recorded --- */
bool p25s_snapshot(p25s_view_t *out)
{
	*out = tb_sim.p25;
	return true;
}

void radio_req_p25_search(uint32_t lo_hz, uint32_t hi_hz)
{
	req("p25s", (long)(lo_hz / 1000u));
	tb_sim.p25.phase = lo_hz != 0 ? P25S_SWEEP : P25S_OFF;
	(void)hi_hz;
}

bool dmrs_snapshot(dmrs_view_t *out)
{
	*out = tb_sim.dmr;
	return true;
}

void radio_req_dmr_search(uint32_t lo_hz, uint32_t hi_hz)
{
	req("dmrs", (long)(lo_hz / 1000u));
	tb_sim.dmr.phase = lo_hz != 0 ? DMRS_SWEEP : DMRS_OFF;
	(void)hi_hz;
}

void radio_req_mode(uint8_t mode)
{
	req(mode == RADIO_MODE_DMR ? "mode_dmr" : "mode_fm", 1);
	view.ch.mode = mode;
	changed();
}

void radio_req_dmr_color_code(uint8_t cc)
{
	req("cc", cc);
	view.dmr.cc = cc;
	changed();
}

void radio_req_dmr_timeslot(uint8_t ts)
{
	req("ts", ts);
	view.dmr.ts = ts;
	changed();
}

/* --- The trunk followers: the views the test sets --- */
bool p25tk_snapshot(p25tk_view_t *out)
{
	*out = tb_sim.p25tk;
	return tb_sim.p25tk.state != P25TK_OFF;
}

void radio_req_p25_trunk(bool on, bool all)
{
	req("p25tk", on);
	(void)all;
}

bool dmrtk_snapshot(dmrtk_view_t *out)
{
	*out = tb_sim.dmrtk;
	return tb_sim.dmrtk.state != DMRTK_OFF;
}

void radio_req_dmr_trunk(bool on, bool all)
{
	req("dmrtk", on);
	(void)all;
}

/* --- The pager's messages --- */
static uint8_t msg_wanted;
void radio_msg_want(uint8_t kind) { msg_wanted = kind; }
uint8_t radio_msg_status(void) { return msg_wanted ? RADIO_MSG_ST_ON : RADIO_MSG_ST_OFF; }
uint32_t radio_msg_seq(void) { return tb_sim.msg_seq; }
uint8_t radio_msg_count(uint8_t kind) { return kind == RADIO_MSG_PAGER ? tb_sim.pager_n : 0; }
const radio_pager_msg_t *radio_msg_pager(uint8_t i) { return i < tb_sim.pager_n ? &tb_sim.pager[i] : NULL; }
const radio_acars_msg_t *radio_msg_acars(uint8_t i) { (void)i; return NULL; }

/* --- Beeps the UI asks the radio task for (the clock's alarm) --- */
void radio_req_beep(uint8_t beep)
{
	tb_sim.beeps++;
	tb_sim.last_beep = beep;
}

/* --- The heap (UI_HOST_HEAP): the waterfall's buffer, the only block taken --- */
#include "ui/wfall.h"
#define WF_HOST_BLOCK	WF_BYTES
void *ui_host_malloc(size_t n)
{
	if (n != WF_HOST_BLOCK)
		abort();
	if (tb_sim.heap_fail) {
		tb_sim.heap_refused++;
		return NULL;
	}
	void *p = malloc(n);
	if (p != NULL) {
		tb_sim.heap_allocs++;
		tb_sim.heap_held += n;
	}
	return p;
}

void ui_host_free(void *p)
{
	if (p == NULL)
		return;
	tb_sim.heap_frees++;
	tb_sim.heap_held -= WF_HOST_BLOCK;	/* the one block there is: the waterfall's */
	free(p);
}

/* Every text on the screen, for a failed expect (TB_DUMP=1) */
void tb_sim_dump(void)
{
	for (int i = 0; i < live_n; i++)
		printf("    [%s]\n", live[i].s);
}
