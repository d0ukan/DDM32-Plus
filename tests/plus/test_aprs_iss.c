// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus ISS APRS digipeater from Satellites (aprs.h aprs_iss_send, tools/plus_features.py prepare_iss_aprs): the
// upstream radio-core APRS suite (tests/host/aprsRadioTest.c: the engine, radio_sm.c's APRS block, a fake AFSK
// transmitter whose tones are decoded back to frames) run again with it built in, then:
//   - the one-shot frame: keyed on the frequency asked for (145.825 MHz plus the Doppler correction), path ARISS, our
//     callsign, the position; the radio back on its channel after it; alone in its transmission;
//   - the gates: no callsign, no position, a frequency outside the ham bands (nothing keyed); at least 30 s between
//     sends; one at a time;
//   - independent of the APRS frequency: it goes while the region frequency is not one we may send on, and an
//     ordinary beacon still goes on the APRS frequency;
//   - a satellite's held track: only the ISS frame keys, the beacons wait for its end.
// Built like aprsRadioTest (tools/test_classic.py) with -DCONFIG_SATELLITES=1.
#define main upstream_aprs_radio_main
#include "aprsRadioTest.c"
#undef main
#include "aprs/aprs_fmt.h"
#include "radio/sat.h"

// The hold's owner (sat_svc.c, not built here): a held track runs (radio_sathold.inc's watchdog leaves the hold)
bool sat_auto_on(void) { return true; }

#define ISS_HZ		145825000u
#define ISS_DOPPLER	145828000u	// a rising pass: +3 kHz on the uplink

static void test_iss_frame(void)
{
	reset();
	aprs_set_test_call("N0TST");
	configure(0, false);	// a fixed position, no automatic beacons
	run(1000);
	CHECK(aprs_iss_send(ISS_DOPPLER) == APRS_OK);
	CHECK(wait_start(1, 6000));
	CHECK(hw.tx_calls == 1 && hw.tx_hz == ISS_DOPPLER);
	CHECK(nframes == 1);
	CHECK(strcmp(frames[0], "N0TST-7>APZRDF,ARISS:!4903.50N/07201.75W[RadioFoundry test") == 0);
	if (nframes > 0)
		printf("# via ISS: %s on %u Hz\n", frames[0], (unsigned)hw.tx_hz);
	ax25_frame_t f;
	aprs_parsed_t q;
	CHECK(nframes > 0 && ax25_from_tnc2(frames[0], strlen(frames[0]), &f) && f.ndigi == 1 &&
	      strcmp(f.digi[0].call, "ARISS") == 0 && strcmp(f.src.call, "N0TST") == 0 &&
	      aprs_parse(&f.dst, f.info, f.info_len, &q) && q.has_pos);
	wait_idle();
	CHECK(!hw.keyed && hw.rx_hz == CH_HZ && sm.view.state == RS_IDLE);
	aprs_view_t v;
	aprs_get_view(&v);
	CHECK(v.rf_sent == 1);
	// Not again within 30 s of the last one; then again
	run(10u * 1000u);
	CHECK(aprs_iss_send(ISS_HZ) == APRS_ISS_WAIT);
	run(15u * 1000u);
	CHECK(aprs_iss_send(ISS_HZ) == APRS_ISS_WAIT);
	run(6u * 1000u);
	CHECK(hw.starts == 1);
	CHECK(aprs_iss_send(ISS_HZ) == APRS_OK);
	// One at a time: a second press before it went is refused (and still counts as too soon)
	CHECK(aprs_iss_send(ISS_HZ) != APRS_OK);
	nframes = 0;
	CHECK(wait_start(2, 6000));
	CHECK(hw.tx_hz == ISS_HZ && nframes == 1 && strstr(frames[0], ",ARISS:!4903.50N/07201.75W") != NULL);
	wait_idle();
	// Nothing by itself afterwards
	run(5u * 60u * 1000u);
	CHECK(hw.starts == 2);
}

static void test_iss_gates(void)
{
	// No callsign: refused, nothing keyed
	reset();
	configure(0, false);
	run(1000);
	CHECK(aprs_iss_send(ISS_HZ) == APRS_NO_CALL);
	// A callsign without a position: refused
	reset();
	aprs_set_test_call("N0TST");
	run(1000);
	CHECK(aprs_iss_send(ISS_HZ) == APRS_NO_POSITION);
	// Not a ham band (or one the radio doesn't send on)
	reset();
	aprs_set_test_call("N0TST");
	configure(0, false);
	run(1000);
	CHECK(aprs_iss_send(150000000u) == APRS_NOT_AMATEUR);
	CHECK(aprs_iss_send(1296000000u) == APRS_NOT_AMATEUR);
	run(60u * 1000u);
	CHECK(hw.starts == 0 && hw.tx_calls == 0);
	// A refused press doesn't start the 30 s wait
	CHECK(aprs_iss_send(ISS_HZ) == APRS_OK);
	CHECK(wait_start(1, 6000));
	CHECK(hw.tx_hz == ISS_HZ);
	wait_idle();
}

static void test_iss_own_frequency(void)
{
	// The APRS frequency is not one we may send on: ordinary beacons are blocked, the ISS frame still goes
	reset();
	aprs_set_test_call("N0TST");
	configure(0, false);
	aprs_cfg_t c;
	aprs_get_cfg(&c);
	c.region = APRS_REGION_CUSTOM;
	c.freq_hz = 150000000u;
	aprs_set_cfg(&c, now);
	run(1000);
	aprs_view_t v;
	aprs_get_view(&v);
	CHECK(v.block == APRS_NOT_AMATEUR);
	CHECK(aprs_iss_send(ISS_HZ) == APRS_OK);
	CHECK(wait_start(1, 6000));
	CHECK(hw.tx_hz == ISS_HZ && nframes == 1 && strstr(frames[0], ",ARISS:") != NULL);
	wait_idle();
	CHECK(hw.rx_hz == CH_HZ);
	// Back on the APRS frequency: an ordinary beacon goes there, with its own path
	reset();
	aprs_set_test_call("N0TST");
	configure(0, false);
	run(1000);
	CHECK(aprs_iss_send(ISS_HZ) == APRS_OK);
	run(100);		// queued at the radio task's next tick
	CHECK(aprs_send_now());
	CHECK(wait_start(1, 6000));
	CHECK(hw.tx_hz == ISS_HZ && nframes == 1 && strstr(frames[0], ",ARISS:") != NULL);	// alone
	wait_idle();
	nframes = 0;
	CHECK(wait_start(2, 30000));
	CHECK(hw.tx_hz == APRS_HZ && nframes == 1 && strstr(frames[0], ",WIDE1-1,WIDE2-1:") != NULL);
	wait_idle();
}

// DDM32 Plus: a satellite's held track (RREQ_SAT_HOLD, Tools > ISS / the AOS pop-up) holds the radio: an ordinary
// beacon waits; the user's # APRS via ISS frame goes past it on 145.825 MHz + Doppler and the hold stays; let go, the
// beacon goes on the APRS frequency
static void test_iss_held(void)
{
	reset();
	aprs_set_test_call("N0TST");
	configure(0, false);
	run(1000);
	req(RREQ_SAT_HOLD, 1);
	run(100);
	CHECK(radio_sat_held());
	CHECK(aprs_send_now());
	CHECK(!wait_start(1, 8000));
	CHECK(hw.tx_calls == 0);
	CHECK(aprs_iss_send(ISS_DOPPLER) == APRS_OK);
	// Holehunt B8: while the frame waits for a clear channel (not keyed yet) the satellites' service is told the
	// radio's own TX (radio_sat_txk): no Doppler step meanwhile, which would abort the frame
	bool waited = false, own = true;
	for (uint32_t t = 0; t < 6000 && hw.starts < 1; t += RADIO_POLL_MS) {
		run(RADIO_POLL_MS);
		if (sm.aprs_phase != 0 && !hw.keyed) {
			waited = true;
			own = own && radio_sat_txk(&sm) == SAT_TX_OWN;
		}
	}
	CHECK(waited && own);
	CHECK(wait_start(1, 6000));
	CHECK(hw.tx_hz == ISS_DOPPLER && nframes == 1 && strstr(frames[0], ",ARISS:") != NULL);
	wait_idle();
	CHECK(radio_sat_held() && hw.tx_calls == 1);
	nframes = 0;
	CHECK(!wait_start(2, 8000));
	req(RREQ_SAT_HOLD, 0);
	CHECK(!radio_sat_held());
	CHECK(wait_start(2, 30000));
	CHECK(hw.tx_hz == APRS_HZ && nframes == 1 && strstr(frames[0], ",WIDE1-1,WIDE2-1:") != NULL);
	wait_idle();
}

int main(void)
{
	int up = upstream_aprs_radio_main();
	CHECK(up == 0);
	test_iss_frame();
	test_iss_gates();
	test_iss_own_frequency();
	test_iss_held();
	printf("# test_aprs_iss: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
