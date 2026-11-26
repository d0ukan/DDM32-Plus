/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus map navigation UI flows: upstream's UI simulator (tests/host/sim/ui_sim.c) as it is, built in place of
 * it, with a few of its services taken over for the test (ui_sim_mapnav.h): the GPS position and the APRS stations
 * the test sets, and the radio requests that tuning makes recorded in order (then handed to the simulator's own, so
 * its view follows). Also the stand-ins for the APRS engine the map, Tools > Repeaters and the APRS composer call
 * (aprs_position_now, the message engine of CONFIG_APRS_MSG). */
#define ui_svc_position sim_base_position
#define ui_svc_aprs_station sim_base_aprs_station
#define ui_svc_aprs_station_count sim_base_aprs_station_count
#define radio_req_vfo_set sim_base_vfo_set
#define radio_req_vfo_tx sim_base_vfo_tx
#define radio_req_bandwidth sim_base_bandwidth
#define radio_req_tone sim_base_tone
#define sim_reset sim_base_reset	/* refpack_sim.inc: every reset gets the map pack */
#include "sim/ui_sim.c"
#undef sim_reset
#include "refpack_sim.inc"
#undef ui_svc_position
#undef ui_svc_aprs_station
#undef ui_svc_aprs_station_count
#undef radio_req_vfo_set
#undef radio_req_vfo_tx
#undef radio_req_bandwidth
#undef radio_req_tone
#include "aprs/aprs_plus.h"
#include "ui_sim_mapnav.h"

mapnav_sim_t mapnav_sim;

void mapnav_sim_clear(void)
{
	memset(&mapnav_sim, 0, sizeof mapnav_sim);
}

void mapnav_sim_mode(uint8_t mode)
{
	view.ch.mode = mode;
	changed();
}

void mapnav_sim_radio_update(int rssi_dbm)
{
	view.rssi_dbm = (int16_t)rssi_dbm;
	changed();
}

static hal_flash_t counting_flash;
static uint32_t chip_reads;
static bool counted_read(uint32_t addr, void *buf, uint32_t len)
{
	chip_reads++;
	return sim_flash.read(addr, buf, len);
}

void mapnav_sim_count_reads(void)
{
	counting_flash = sim_flash;
	counting_flash.read = counted_read;
	board.flash = &counting_flash;
	chip_reads = 0;
}

uint32_t mapnav_sim_reads(void)
{
	return chip_reads;
}

uint32_t mapnav_sim_rows_hash(int y, int h)
{
	uint32_t hash = 2166136261u;
	for (int r = disp.inset_top + y; r < disp.inset_top + y + h; r++)
		for (int x = disp.inset_left; x < disp.width - disp.inset_right; x++)
			hash = (hash ^ panel_px[r * disp.width + x]) * 16777619u;
	return hash;
}

bool ui_svc_position(ui_position_t *out)
{
	if (!mapnav_sim.gps)
		return sim_base_position(out);
	memset(out, 0, sizeof *out);
	out->fix = mapnav_sim.fix;
	out->sats = mapnav_sim.fix ? 8 : 0;
	out->lat_e7 = mapnav_sim.lat_e7;
	out->lon_e7 = mapnav_sim.lon_e7;
	out->speed_kmh = out->course = 0xFFFFu;
	out->utc_hms = 0xFFFFFFFFu;
	return true;
}

uint8_t ui_svc_aprs_station_count(void)
{
	return mapnav_sim.stations ? mapnav_sim.nst : sim_base_aprs_station_count();
}

bool ui_svc_aprs_station(uint8_t i, aprs_station_t *out)
{
	if (!mapnav_sim.stations)
		return sim_base_aprs_station(i, out);
	if (i >= mapnav_sim.nst)
		return false;
	*out = mapnav_sim.st[i];
	return true;
}

/* The position APRS uses (Tools > Repeaters' order): the GPS one here */
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

static void req(const char *what, long v)
{
	size_t n = strlen(mapnav_sim.req);
	snprintf(mapnav_sim.req + n, sizeof mapnav_sim.req - n, "%s%s:%ld", n ? " " : "", what, v);
}

void radio_req_vfo_set(uint32_t hz)
{
	req("vfo_set", (long)hz);
	sim_base_vfo_set(hz);
}

void radio_req_vfo_tx(uint32_t hz)
{
	req("vfo_tx", (long)hz);
	sim_base_vfo_tx(hz);
}

void radio_req_bandwidth(radio_bw_t bw)
{
	req("bw", bw == RADIO_BW_12K5 ? 12 : 25);
	sim_base_bandwidth(bw);
}

void radio_req_tone(uint8_t which, subtone_t tone)
{
	req(which == RADIO_TONE_TX ? "tone_tx" : "tone_rx", tone.type == TONE_NONE ? 0 : (long)tone.value);
	sim_base_tone(which, tone);
}

/* Not in the simulator (its radio has no mode request): the view follows */
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

/* The APRS message engine (CONFIG_APRS_MSG): nothing waiting, nothing heard; a message is recorded */
void aprs_plus_state(aprs_plus_t *s)
{
	memset(s, 0, sizeof *s);
	s->block = APRS_OK;
	s->pos = true;
}

uint8_t aprs_sos_set(bool on)
{
	(void)on;
	return APRS_OK;
}

void aprs_sos_heard(char *call)
{
	call[0] = '\0';
}

uint8_t aprs_msg_count(void) { return 0; }
bool aprs_msg(uint8_t i, aprs_msg_t *out) { (void)i; (void)out; return false; }
bool aprs_msg_peek(uint8_t i, aprs_msg_t *out) { (void)i; (void)out; return false; }
bool aprs_rx_listening(void) { return true; }

bool aprs_send_message(const char *to, const char *text)
{
	snprintf(mapnav_sim.msg_to, sizeof mapnav_sim.msg_to, "%s", to);
	snprintf(mapnav_sim.msg_text, sizeof mapnav_sim.msg_text, "%s", text);
	mapnav_sim.messages++;
	return true;
}
