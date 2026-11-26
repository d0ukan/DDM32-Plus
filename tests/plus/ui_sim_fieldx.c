/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 13 ideas 1 UI flows: upstream's UI simulator (tests/host/sim/ui_sim.c) as it is, built in place of
 * it, with a few of its services taken over for the test (ui_sim_fieldx.h): the GPS position and the APRS stations,
 * the P25 / DMR searches' lists, a zone's capacity, and the SOS actions recorded (the emergency key's call, then the
 * simulator's own; the APRS engine's SOS). Also the stand-ins for the APRS engine that the map's composer, Tools >
 * Repeaters and the locator page call (aprs_position_now: the GPS here). */
#define ui_svc_position sim_base_position
#define ui_svc_aprs_station sim_base_aprs_station
#define ui_svc_aprs_station_count sim_base_aprs_station_count
#define codeplug_zone_capacity sim_base_zone_capacity
#define ui_svc_emergency sim_base_emergency
#define radio_req_vfo_set sim_base_vfo_set
#define sim_reset sim_base_reset	/* refpack_sim.inc: every reset gets the map pack */
#include "sim/ui_sim.c"
#undef sim_reset
#include "refpack_sim.inc"
#undef ui_svc_position
#undef ui_svc_aprs_station
#undef ui_svc_aprs_station_count
#undef codeplug_zone_capacity
#undef ui_svc_emergency
#undef radio_req_vfo_set
#include "dmr/dmr_emergency.h"
#include "ui_sim_fieldx.h"

fx_sim_t fx_sim;

void fx_sim_clear(void)
{
	memset(&fx_sim, 0, sizeof fx_sim);
	fx_sim.aprs_why = APRS_OK;
}

bool ui_svc_position(ui_position_t *out)
{
	if (!fx_sim.gps)
		return sim_base_position(out);
	memset(out, 0, sizeof *out);
	out->fix = fx_sim.fix;
	out->sats = fx_sim.fix ? 8 : 0;
	out->lat_e7 = fx_sim.lat_e7;
	out->lon_e7 = fx_sim.lon_e7;
	out->speed_kmh = out->course = 0xFFFFu;
	out->utc_hms = 0xFFFFFFFFu;
	return true;
}

uint8_t ui_svc_aprs_station_count(void)
{
	return fx_sim.stations ? fx_sim.nst : sim_base_aprs_station_count();
}

bool ui_svc_aprs_station(uint8_t i, aprs_station_t *out)
{
	if (!fx_sim.stations)
		return sim_base_aprs_station(i, out);
	if (i >= fx_sim.nst)
		return false;
	*out = fx_sim.st[i];
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

uint16_t codeplug_zone_capacity(void)
{
	return fx_sim.zone_cap ? fx_sim.zone_cap : sim_base_zone_capacity();
}

static void req(const char *what, long v)
{
	size_t n = strlen(fx_sim.req);
	snprintf(fx_sim.req + n, sizeof fx_sim.req - n, "%s%s:%ld", n ? " " : "", what, v);
}

/* --- The searches: the lists the test sets; a start / stop recorded --- */
bool p25s_snapshot(p25s_view_t *out)
{
	*out = fx_sim.p25;
	return true;
}

void radio_req_p25_search(uint32_t lo_hz, uint32_t hi_hz)
{
	req("p25s", (long)(lo_hz / 1000u));
	fx_sim.p25.phase = lo_hz != 0 ? P25S_SWEEP : P25S_OFF;
	(void)hi_hz;
}

bool dmrs_snapshot(dmrs_view_t *out)
{
	*out = fx_sim.dmr;
	return true;
}

void radio_req_dmr_search(uint32_t lo_hz, uint32_t hi_hz)
{
	req("dmrs", (long)(lo_hz / 1000u));
	fx_sim.dmr.phase = lo_hz != 0 ? DMRS_SWEEP : DMRS_OFF;
	(void)hi_hz;
}

void radio_req_vfo_set(uint32_t hz)
{
	req("vfo_set", (long)hz);
	sim_base_vfo_set(hz);
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

/* --- The SOS actions: the emergency key's call (then the simulator's own), the APRS engine's SOS --- */
bool ui_svc_emergency(bool on)
{
	if (on)
		fx_sim.emerg_on++;
	else
		fx_sim.emerg_off++;
	return sim_base_emergency(on);
}

dmr_emergency_state_t dmr_emergency_state(void)
{
	return DMR_EMERGENCY_IDLE;
}

void aprs_plus_state(aprs_plus_t *s)
{
	*s = fx_sim.aprs;
	s->block = APRS_OK;
	s->pos = true;
}

uint8_t aprs_sos_set(bool on)
{
	if (!on) {
		fx_sim.aprs_off++;
		fx_sim.aprs.sos = false;
		return APRS_OK;
	}
	fx_sim.aprs_on++;
	if (fx_sim.aprs_why == APRS_OK)
		fx_sim.aprs.sos = true;
	return fx_sim.aprs_why;
}

void aprs_sos_heard(char *call)
{
	call[0] = '\0';
}

/* The APRS message engine (the map's composer): nothing waiting, nothing heard */
uint8_t aprs_msg_count(void) { return 0; }
bool aprs_msg(uint8_t i, aprs_msg_t *out) { (void)i; (void)out; return false; }
bool aprs_msg_peek(uint8_t i, aprs_msg_t *out) { (void)i; (void)out; return false; }
bool aprs_rx_listening(void) { return true; }
bool aprs_send_message(const char *to, const char *text) { (void)to; (void)text; return true; }

/* Every text on the screen, for a failed expect (FX_DUMP=1) */
void fx_sim_dump(void)
{
	for (int i = 0; i < live_n; i++)
		printf("    [%s]\n", live[i].s);
}
