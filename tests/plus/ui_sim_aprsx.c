/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 14 APRS job UI flows: upstream's UI simulator (tests/host/sim/ui_sim.c) as it is, built in place of
 * it, with the services the APRS screens read taken over for the test (ui_sim_aprsx.h): the GPS position, the APRS
 * stations heard and the inbox; the SOS actions and the messages sent recorded. */
#define ui_svc_position sim_base_position
#define ui_svc_aprs_station sim_base_aprs_station
#define ui_svc_aprs_station_count sim_base_aprs_station_count
#define ui_svc_emergency sim_base_emergency
#define sim_reset sim_base_reset	/* refpack_sim.inc: every reset gets the map pack */
#include "sim/ui_sim.c"
#undef sim_reset
#include "refpack_sim.inc"
#undef ui_svc_position
#undef ui_svc_aprs_station
#undef ui_svc_aprs_station_count
#undef ui_svc_emergency
#include "dmr/dmr_emergency.h"
#include "ui_sim_aprsx.h"

ax_sim_t ax_sim;

void ax_sim_clear(void)
{
	memset(&ax_sim, 0, sizeof ax_sim);
}

bool ui_svc_position(ui_position_t *out)
{
	memset(out, 0, sizeof *out);
	out->fix = ax_sim.fix;
	out->sats = ax_sim.fix ? 8 : 0;
	out->lat_e7 = ax_sim.lat_e7;
	out->lon_e7 = ax_sim.lon_e7;
	out->speed_kmh = out->course = 0xFFFFu;
	out->utc_hms = 0xFFFFFFFFu;
	return true;
}

uint8_t ui_svc_aprs_station_count(void)
{
	return ax_sim.nst;
}

bool ui_svc_aprs_station(uint8_t i, aprs_station_t *out)
{
	if (i >= ax_sim.nst)
		return false;
	*out = ax_sim.st[i];
	return true;
}

bool aprs_position_now(int32_t *lat, int32_t *lon, uint32_t *err_m, bool *dmr_gps)
{
	*err_m = 0;
	*dmr_gps = false;
	if (!ax_sim.fix)
		return false;
	*lat = ax_sim.lat_e7;
	*lon = ax_sim.lon_e7;
	return true;
}

/* --- The SOS actions (the SOS key's screens): recorded, the simulator's own emergency after --- */
bool ui_svc_emergency(bool on)
{
	if (on)
		ax_sim.emerg_on++;
	else
		ax_sim.emerg_off++;
	return sim_base_emergency(on);
}

dmr_emergency_state_t dmr_emergency_state(void)
{
	return DMR_EMERGENCY_IDLE;
}

void aprs_plus_state(aprs_plus_t *s)
{
	*s = ax_sim.aprs;
	s->block = APRS_OK;
	s->pos = true;
	s->messages = ax_sim.nmsg;
}

uint8_t aprs_sos_set(bool on)
{
	if (on)
		ax_sim.aprs_on++;
	else
		ax_sim.aprs_off++;
	ax_sim.aprs.sos = on;
	return APRS_OK;
}

void aprs_sos_heard(char *call)
{
	call[0] = '\0';
}

/* --- The inbox and our message --- */
uint8_t aprs_msg_count(void)
{
	return ax_sim.nmsg;
}

bool aprs_msg_peek(uint8_t i, aprs_msg_t *out)
{
	if (i >= ax_sim.nmsg)
		return false;
	*out = ax_sim.msg[i];
	return true;
}

bool aprs_msg(uint8_t i, aprs_msg_t *out)
{
	if (i >= ax_sim.nmsg)
		return false;
	ax_sim.msg[i].read = true;
	*out = ax_sim.msg[i];
	return true;
}

bool aprs_rx_listening(void)
{
	return true;
}

bool aprs_send_message(const char *to, const char *text)
{
	snprintf(ax_sim.sent_to, sizeof ax_sim.sent_to, "%s", to);
	snprintf(ax_sim.sent_text, sizeof ax_sim.sent_text, "%s", text);
	ax_sim.aprs.out = APRS_OUT_SENDING;
	return true;
}

/* Every text on the screen, for a failed expect (AX_DUMP=1) */
void ax_sim_dump(void)
{
	for (int i = 0; i < live_n; i++)
		printf("    [%s]\n", live[i].s);
}
