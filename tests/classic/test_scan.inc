/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus P25 scan (docs/plus.md): the native receiver takes a P25 channel only where a zone scan holds on a
 * signal, gives it back when the call ends or no voice came, and does not retake a silent channel at once.
 * Included by test_radio.inc when built with CONFIG_P25_SCAN=1; the worker and RF are its fixtures. */
static uint32_t scan_voice_ms;
uint32_t p25_lab_channel_last_voice_ms(void) { return scan_voice_ms; }
#if CONFIG_SPECTRUM
static void test_plus_spectrum_sm(void);
#endif
#if CONFIG_P25_SEARCH
#include "radio/p25search.h"
static uint32_t ps_frames, ps_frame_ms;
static uint16_t ps_nac, ps_tg;
static uint32_t ps_src;
uint32_t p25_lab_channel_frames(uint32_t *last_ms, uint16_t *heard_nac)
{ if(last_ms)*last_ms=ps_frame_ms; if(heard_nac)*heard_nac=ps_nac; return ps_frames; }
/* The caller's sequence follows the frames, unless frozen (no new caller heard: tg / src are the last one's) */
static bool ps_caller_frozen;
static uint32_t ps_caller_seq;
uint32_t p25_lab_p25_caller(uint16_t *tg, uint32_t *src)
{ if(tg)*tg=ps_tg; if(src)*src=ps_src; return ps_caller_frozen?ps_caller_seq:ps_frames; }
static void test_plus_p25search_sm(void);
#endif

#include "../plus/test_zscan.inc"

static void scan_hold(uint8_t phase, uint16_t pos)
{
    sm.scan.kind=SCAN_ZONE;sm.scan.phase=phase;sm.view.zone_pos=pos;
}
static void test_plus_scan(void)
{
    classic_select(false);scan_voice_ms=zs_sync_ms=0;
    /* Listening (dwell) on a P25 channel: the scan keeps the FM path, nothing native starts */
    scan_hold(SCAN_PH_LISTEN,3);classic_pump();classic_pump();
    CHECK(classic_starts==0 && !native_lab_owns() && !classic_busy);

    /* The scan holds on a carrier: the native receiver takes the channel */
    scan_hold(SCAN_PH_HOLD,3);classic_pump();classic_pump();
    CHECK(classic_live && classic_live->state==NT_RX && native_lab_owns() && classic_starts==1);

    /* No P25 frame within the probe time (a zone scan): it gives the channel back and the radio retunes the FM path */
    now+=1000;classic_pump();CHECK(native_lab_owns());
#ifndef ZSCAN_BEFORE
    now+=300;classic_pump();classic_pump();classic_pump();
    CHECK(!native_lab_owns() && !classic_busy && classic_live->state!=NT_RX);
    now+=1300;
#else
    now+=1600;classic_pump();classic_pump();classic_pump();
    CHECK(!native_lab_owns() && !classic_busy && classic_live->state!=NT_RX);
#endif

    /* A silent channel is not taken again during its cooldown, even while the scan still holds there */
    classic_pump();classic_pump();CHECK(!native_lab_owns() && classic_starts==1);

    /* Another stop with P25 voice: kept while the call lasts, given back after the end delay */
    now+=800;   /* past the settle after the last give back (the scan's resume wait is longer) */
    scan_hold(SCAN_PH_HOLD,4);classic_pump();classic_pump();
    CHECK(native_lab_owns() && classic_starts==2);
    now+=300;scan_voice_ms=now;
    now+=1200;classic_pump();CHECK(native_lab_owns());
    scan_voice_ms=now;now+=1000;classic_pump();CHECK(native_lab_owns());
    now+=600;classic_pump();classic_pump();classic_pump();
    CHECK(!native_lab_owns() && !classic_busy);
    /* The carrier is still there after the settle (the call goes on): taken again */
    now+=800;classic_pump();classic_pump();CHECK(native_lab_owns() && classic_starts==3);
    scan_voice_ms=now;now+=1600;classic_pump();classic_pump();classic_pump();CHECK(!native_lab_owns());
    /* The scan moved on (resume wait): nothing is taken outside a hold */
    now+=800;sm.scan.phase=SCAN_PH_RESUME;classic_pump();classic_pump();CHECK(!native_lab_owns());

    /* After the cooldown the silent channel may be taken again */
    scan_voice_ms=0;now+=8000;scan_hold(SCAN_PH_HOLD,3);classic_pump();classic_pump();
    CHECK(native_lab_owns() && classic_starts==4);

    /* The user stops the scan while the receiver holds: the channel stays with the receiver */
    sm.scan.kind=SCAN_OFF;now+=3000;classic_pump();classic_pump();
    CHECK(native_lab_owns() && !classic_stop);   /* past the no-voice limit, still not given back */

    /* PTT during a scan hold is the native owner's, and the hold is not given back while talking */
    classic_select(false);scan_voice_ms=0;
    scan_hold(SCAN_PH_HOLD,5);classic_pump();classic_pump();CHECK(native_lab_owns());
    sm.ptt=true;now+=3000;classic_pump();CHECK(native_lab_owns());
    sm.ptt=false;classic_pump();classic_pump();classic_pump();CHECK(!native_lab_owns());

    /* Dual watch: listening (alternating) leaves the FM path; a hold on the P25 channel hands it over */
    classic_select(false);scan_voice_ms=0;
    sm.scan.kind=SCAN_DUAL;sm.scan.phase=SCAN_PH_LISTEN;sm.view.zone_pos=6;classic_pump();classic_pump();
    CHECK(!native_lab_owns() && classic_starts==0);
    sm.scan.phase=SCAN_PH_HOLD;classic_pump();classic_pump();CHECK(native_lab_owns() && classic_starts==1);
    scan_voice_ms=now+100;now+=1700;classic_pump();classic_pump();classic_pump();CHECK(!native_lab_owns());
    sm.scan.kind=SCAN_OFF;
#ifndef ZSCAN_BEFORE
    /* Zone scan, frames of ours but never voice (another talkgroup, encrypted): the old 2.5 s limit, then the cooldown */
    classic_select(false);scan_voice_ms=zs_sync_ms=0;
    scan_hold(SCAN_PH_HOLD,3);classic_pump();classic_pump();CHECK(native_lab_owns());
    for(unsigned i=0;i<12;i++) { now+=200;zs_sync_ms=now;classic_pump();CHECK(native_lab_owns()); }
    now+=200;zs_sync_ms=now;classic_pump();classic_pump();classic_pump();CHECK(!native_lab_owns());
    /* DDM32 Plus (mwfix): frames without voice are a late entry or another talk group: taken again after 2 s (a call
     * of ours isn't skipped for 8 s); the same again within 10 s: the whole cooldown */
    now+=1700;classic_pump();classic_pump();CHECK(!native_lab_owns());
    now+=400;zs_sync_ms=now;classic_pump();classic_pump();CHECK(native_lab_owns());
    for(unsigned i=0;i<12;i++) { now+=200;zs_sync_ms=now;classic_pump(); }
    now+=200;zs_sync_ms=now;classic_pump();classic_pump();classic_pump();CHECK(!native_lab_owns());
    now+=7000;classic_pump();classic_pump();CHECK(!native_lab_owns());
    now+=1100;classic_pump();classic_pump();CHECK(native_lab_owns());
    /* Dual watch keeps its limit without frames (multi-watch's ring estimate, radio_mwatch.h) */
    classic_select(false);scan_voice_ms=zs_sync_ms=0;
    sm.scan.kind=SCAN_DUAL;sm.scan.phase=SCAN_PH_HOLD;sm.view.zone_pos=6;classic_pump();classic_pump();
    now+=2400;classic_pump();CHECK(native_lab_owns());
    now+=200;classic_pump();classic_pump();classic_pump();CHECK(!native_lab_owns());
    sm.scan.kind=SCAN_OFF;
#endif
    test_zscan();
#if CONFIG_SPECTRUM
    test_plus_spectrum_sm();
#if CONFIG_P25_SEARCH
    test_plus_p25search_sm();
#endif
#endif
    printf("Plus P25 scan: hold-only take, no-voice and call-end give back, cooldown, user stop, PTT\n");
}

#if CONFIG_SPECTRUM
#include "radio/spectrum.h"
#if CONFIG_FOX
#include "radio/fox.h"
#endif
#if CONFIG_CW_BEACON
#include "radio/cw_beacon.h"
#endif
/* The real radio state machine with the sweep: it takes the receiver (stepping, speaker muted), any other request
 * ends it, the stop request and the idle stop tune the channel again */
static void test_plus_spectrum_sm(void)
{
    spectrum_view_t v;
    setup_codeplug();start();now+=1000;radio_sm_poll(&sm,now);
    uint32_t ch_hz=sm.view.ch.rx_hz;
    radio_req_t r={.type=RREQ_SPECTRUM,.arg=433500000,.arg2=500000};
    radio_sm_request(&sm,&r,now);
    CHECK(spectrum_on() && hw.rx_hz==433500000u-16u*15625u && hw.rx_tone.type==TONE_NONE);
    hw.st.rssi_dbm=-80;hw.st.noise=0;   /* a strong carrier everywhere: still no speaker */
    uint32_t seen=hw.rx_hz;int moves=0;
    for(int i=0;i<100;i++){ now+=20;spectrum_snapshot(&v);radio_sm_poll(&sm,now);if(hw.rx_hz!=seen){moves++;seen=hw.rx_hz;}CHECK(!hw.audio); }
    CHECK(moves>=32 && spectrum_snapshot(&v) && v.sweeps>=1);
    /* Another request ends the sweep: the radio is back on a channel */
    req(RREQ_CH_STEP,1);CHECK(!spectrum_on() && hw.rx_hz==sm.view.ch.rx_hz);
    /* The stop request tunes the channel again */
    radio_sm_request(&sm,&r,now);CHECK(spectrum_on());
    radio_req_t s={.type=RREQ_SPECTRUM};radio_sm_request(&sm,&s,now);
    CHECK(!spectrum_on() && hw.rx_hz==sm.view.ch.rx_hz);
    /* Nobody looks: the sweep stops by itself and the channel is tuned again */
    radio_sm_request(&sm,&r,now);
    for(int i=0;i<200 && spectrum_on();i++){ now+=20;radio_sm_poll(&sm,now); }
    CHECK(!spectrum_on() && hw.rx_hz==sm.view.ch.rx_hz);
    hw.st.rssi_dbm=0;(void)ch_hz;
#if CONFIG_FOX
    /* Fox hunt: the vario tone plays through the beep while watched, and not when off */
    {
        beep_stats_t b0,b1;setup_codeplug();start();
        radio_req_t f={.type=RREQ_FOX,.arg=1,.arg2=0};radio_sm_request(&sm,&f,now);
        hw.st.rssi_dbm=-60;beep_get_stats(&b0);
        for(int i=0;i<40;i++){ now+=20;fox_seen();radio_sm_poll(&sm,now); }
        beep_get_stats(&b1);CHECK(fox_on() && b1.played+b1.no_stream>b0.played+b0.no_stream);
        f.arg=0;radio_sm_request(&sm,&f,now);beep_get_stats(&b0);
        for(int i=0;i<40;i++){ now+=20;radio_sm_poll(&sm,now); }
        beep_get_stats(&b1);CHECK(!fox_on() && b1.played+b1.no_stream==b0.played+b0.no_stream);
        hw.st.rssi_dbm=0;
    }
#endif
#if CONFIG_CW_BEACON
    /* CW beacon: keys the message on the channel's TX frequency and comes back to the channel; once or periodic;
     * PTT cuts it at once; refused without a callsign or while the channel may not transmit */
    {
        cw_beacon_view_t bv;
        setup_codeplug();start();now+=1000;radio_sm_poll(&sm,now);
        strcpy(sm.radio_name,"TA1ABC");
        uint32_t rx_hz=sm.view.ch.rx_hz;int tx0=hw.tx_calls,off0=hw.tx_off_calls;
        radio_req_t rb={.type=RREQ_CW_BEACON,.arg=CW_BEACON_SOS,.arg2=0};radio_sm_request(&sm,&rb,now);
        now+=10;radio_sm_tick(&sm,now);cw_beacon_view(&bv);
        CHECK(bv.on && bv.keying && hw.tx_calls==tx0+1 && hw.tx_hz==sm.view.ch.tx_hz && hw.led[LED_RED]);
        for(int i=0;i<4000 && bv.keying;i++){ now+=10;radio_sm_tick(&sm,now);cw_beacon_view(&bv); }
        CHECK(!bv.keying && !bv.on && bv.sent==1 && hw.tx_off_calls>off0 && hw.rx_hz==rx_hz && !hw.led[LED_RED]);
        /* Periodic: the next one 30 s after the end of the last */
        rb.arg=CW_BEACON_DE;rb.arg2=1;radio_sm_request(&sm,&rb,now);
        for(int i=0;i<4000;i++){ now+=10;radio_sm_tick(&sm,now);cw_beacon_view(&bv);if(i>5 && !bv.keying)break; }
        int tx1=hw.tx_calls;CHECK(bv.on && !bv.keying && bv.sent==2);
        for(int i=0;i<2900;i++){ now+=10;radio_sm_tick(&sm,now); }
        cw_beacon_view(&bv);CHECK(!bv.keying && hw.tx_calls==tx1);
        for(int i=0;i<200;i++){ now+=10;radio_sm_tick(&sm,now); }
        cw_beacon_view(&bv);CHECK(bv.keying && hw.tx_calls==tx1+1);
        /* PTT cuts the message on air and disarms the beacon (and then transmits as usual) */
        int off1=hw.tx_off_calls;req(RREQ_PTT,1);cw_beacon_view(&bv);
        CHECK(!bv.on && !bv.keying && hw.tx_off_calls>off1);
        req(RREQ_PTT,0);
        /* No callsign: refused, nothing keyed */
        sm.radio_name[0]='\0';int tx2=hw.tx_calls;rb.arg=CW_BEACON_CQ;rb.arg2=0;radio_sm_request(&sm,&rb,now);
        now+=10;radio_sm_tick(&sm,now);cw_beacon_view(&bv);
        CHECK(!bv.on && !bv.keying && bv.refused!=0 && hw.tx_calls==tx2);
        /* TX inhibit: refused with its reason */
        strcpy(sm.radio_name,"TA1ABC");sm.set.tx_inhibit=true;radio_sm_request(&sm,&rb,now);
        now+=10;radio_sm_tick(&sm,now);cw_beacon_view(&bv);
        CHECK(!bv.on && bv.refused!=0 && bv.refused!=0xFFu && hw.tx_calls==tx2);
        sm.set.tx_inhibit=false;
        /* Stop request while armed */
        rb.arg2=3;radio_sm_request(&sm,&rb,now);rb.arg=CW_BEACON_KINDS;radio_sm_request(&sm,&rb,now);
        now+=10;radio_sm_tick(&sm,now);cw_beacon_view(&bv);CHECK(!bv.on && !bv.keying);
        /* DDM32 Plus (hole hunt): a zone scan runs: refused (it would key whatever channel the scan is on), the scan
         * goes on, nothing keyed */
        {
            radio_req_t zsr={.type=RREQ_SCAN,.arg=RSCAN_ZONE};radio_sm_request(&sm,&zsr,now);
            for(int i=0;i<30;i++){ now+=10;radio_sm_tick(&sm,now);radio_sm_poll(&sm,now); }
            CHECK(scan_on(&sm.scan));
            int tx3=hw.tx_calls;bool busy_seen=false;
            for(int k=0;k<8;k++){   /* wherever the scan is (a receive-only channel says so with its own reason) */
                rb.arg=CW_BEACON_CQ;rb.arg2=0;radio_sm_request(&sm,&rb,now);
                for(int i=0;i<5;i++){ now+=10;radio_sm_tick(&sm,now); }
                cw_beacon_view(&bv);
                busy_seen=busy_seen || bv.refused==0xFFu;
                CHECK(!bv.on && !bv.keying && bv.refused!=0 && hw.tx_calls==tx3 && scan_on(&sm.scan));
                for(int i=0;i<17;i++){ now+=10;radio_sm_tick(&sm,now);radio_sm_poll(&sm,now); }
            }
            CHECK(busy_seen);   /* refused on a channel that may transmit too */
            zsr.arg=RSCAN_STOP;radio_sm_request(&sm,&zsr,now);
            for(int i=0;i<30;i++){ now+=10;radio_sm_tick(&sm,now);radio_sm_poll(&sm,now); }
        }
        printf("Plus CW beacon: once, periodic, PTT cut, no callsign, TX inhibit, stop, refused during a scan\n");
    }
#endif
    printf("Plus spectrum in the radio state machine: takes the receiver muted, request/stop/idle give it back\n");
}
#endif

#if CONFIG_P25_SEARCH
/* P25 search in the real state machine and native admission: the sweep owns the receiver (muted, stepping), a
 * carrier hands the frequency to the native receiver with any NAC/TG; P25 frames are noted and played, then the
 * frequency is given back and the sweep goes on from the next step; no P25: given back at once and skipped; PTT
 * never transmits; any other request ends the search and the channel is tuned again */
static int16_t ps_level(uint32_t hz) { return (hz==433500000u || hz==433600000u) ? -80 : -124; }
static void ps_step(void)
{
    p25s_view_t v;
    now+=10;p25s_snapshot(&v);
    hw.st.rssi_dbm=ps_level(hw.rx_hz);
    radio_sm_poll(&sm,now);classic_pump();
}
static void test_plus_p25search_sm(void)
{
    p25s_view_t v;
    classic_select(false);sm.view.ch.mode=RADIO_MODE_FM;
    ps_frames=0;ps_nac=0;ps_tg=0;ps_src=0;
    int tx0=hw.tx_calls;
    radio_req_t r={.type=RREQ_P25_SEARCH,.arg=433000000,.arg2=434000000};
    radio_sm_request(&sm,&r,now);
    CHECK(p25s_on() && hw.rx_hz==433000000u && !hw.audio);
    /* sweep until the carrier at 433.500: the native receiver takes it with any NAC/TG */
    for(int i=0;i<2000 && !native_lab_owns();i++) ps_step();
    classic_pump();classic_pump();      /* armed -> receiving */
    CHECK(native_lab_owns() && p25s_phase()==P25S_PROBE && p25s_at_hz()==433500000u && classic_live &&
          classic_live->config.frequency_hz==433500000u && classic_live->state==NT_RX);
    /* P25 frames arrive: noted with NAC, talkgroup and source, and played */
    for(int i=0;i<40;i++){ if(i%9==0){ps_frames++;ps_frame_ms=now;ps_nac=0x1C8;ps_tg=655;ps_src=1234567;} now+=20;classic_pump(); }
    CHECK(p25s_snapshot(&v) && v.playing && v.found_n==1 && v.found[0].hz==433500000u && v.found[0].nac==0x1C8 &&
          v.found[0].tg==655 && v.found[0].src==1234567 && native_lab_owns());
    /* the terminator: the receiver's NAC reads "any" (0xFFFF) after it; the same entry, no second one (beta.7) */
    ps_frames++;ps_frame_ms=now;ps_nac=0xFFFFu;
    for(int i=0;i<5;i++){ now+=20;classic_pump(); }
    CHECK(p25s_snapshot(&v) && v.found_n==1 && v.found[0].nac==0x1C8 && v.found[0].tg==655 && v.found[0].calls==1);
    /* the call ends: given back 1.5 s after the last frame, the sweep resumes at the next step */
    for(int i=0;i<200 && native_lab_owns();i++){ now+=10;classic_pump(); }
    CHECK(!native_lab_owns() && p25s_phase()==P25S_SWEEP && p25s_at_hz()==433512500u);
    ps_step();CHECK(hw.rx_hz==433512500u);
    /* a carrier without P25 (433.600): given back after the probe time and skipped */
    for(int i=0;i<2000 && !native_lab_owns();i++) ps_step();
    CHECK(native_lab_owns() && p25s_at_hz()==433600000u);
    uint32_t t0=now;
    for(int i=0;i<200 && native_lab_owns();i++){ now+=10;classic_pump(); }
    /* (the surer search, CONFIG_SEARCH_SURE: the zone scan's 1.2 s probe time) */
    CHECK(!native_lab_owns() && now-t0>=(CONFIG_SEARCH_SURE?1150u:650u) && now-t0<(CONFIG_SEARCH_SURE?1500u:1000u) &&
          p25s_snapshot(&v) && v.found_n==1);
    /* PTT during a probe: consumed, the search stops, nothing is keyed */
    for(int i=0;i<4000 && !native_lab_owns();i++) ps_step();    /* the next pass: 433.500 again */
    CHECK(native_lab_owns() && p25s_at_hz()==433500000u);
    /* P25 there, but no new caller since the take: the last caller's talkgroup (another frequency's, a call ago) is
     * not this frequency's; the frames go to its entry, no entry for a stale talkgroup */
    ps_caller_frozen=true;ps_caller_seq=ps_frames;ps_tg=999;ps_src=42;
    for(int i=0;i<20;i++){ if(i%9==0){ps_frames++;ps_frame_ms=now;ps_nac=0x1C8;} now+=20;classic_pump(); }
    CHECK(p25s_snapshot(&v) && v.found_n==1 && v.found[0].tg==655 && v.found[0].src==1234567);
    ps_caller_frozen=false;
    radio_req_t ptt={.type=RREQ_PTT,.arg=1};
    CHECK(native_lab_request(&sm,&ptt)==1);
    for(int i=0;i<50;i++){ now+=10;classic_pump(); }
    CHECK(!p25s_on() && !native_lab_owns() && hw.tx_calls==tx0 && !sm.ptt);
    ptt.arg=0;native_lab_request(&sm,&ptt);
    /* while sweeping, another request ends the search and tunes the channel */
    radio_sm_request(&sm,&r,now);ps_step();CHECK(p25s_on());
    req(RREQ_CH_STEP,1);CHECK(!p25s_on() && hw.rx_hz==sm.view.ch.rx_hz);
    /* PTT while sweeping: consumed too, the channel tuned again */
    radio_sm_request(&sm,&r,now);ps_step();CHECK(p25s_on());
    ptt.arg=1;CHECK(native_lab_request(&sm,&ptt)==1);ptt.arg=0;native_lab_request(&sm,&ptt);
    radio_sm_poll(&sm,now+=10);
    CHECK(!p25s_on() && hw.tx_calls==tx0);
#if CONFIG_DMR_EMERGENCY
    /* DDM32 Plus (mwfix4): an emergency while the search plays a find: the search ends, the receiver is let go and the
     * radio tick takes the start within a second (on this FM channel it fails: SOS / the lone worker fall back) */
    radio_sm_request(&sm,&r,now);
    for(int i=0;i<4000 && !native_lab_owns();i++) ps_step();
    CHECK(native_lab_owns() && p25s_phase()==P25S_PROBE);
    for(int i=0;i<10;i++){ ps_frames++;ps_frame_ms=now;ps_nac=0x1C8;ps_tg=655;ps_src=1234567;now+=20;classic_pump(); }
    CHECK(p25s_snapshot(&v) && v.playing);
    {
        dmr_emergency_settings_t es;
        dmr_emergency_settings_default(&es);
        es.mode=DMR_EMERGENCY_MODE_CALL;
        dmr_emergency_set_settings(&es);
        radio_sm_tick(&sm,now);
        CHECK(dmr_emergency_start());
        uint32_t et0=now,failed=0;
        for(int i=0;i<100 && !failed;i++) {
            ps_frames++;ps_frame_ms=now;	/* the find still on the air */
            now+=10;classic_pump();
            if(!native_lab_owns())radio_sm_tick(&sm,now);
            if(dmr_emergency_state()==DMR_EMERGENCY_FAILED)failed=now-et0;
        }
        CHECK(failed!=0 && failed<=1000u && !p25s_on() && hw.tx_calls==tx0);
        printf("Plus P25 search: an emergency while playing a find: search ended, the emergency failed (FM channel) after %u ms\n",
               (unsigned)failed);
        dmr_emergency_cancel();
        for(int i=0;i<20;i++){ now+=10;classic_pump();if(!native_lab_owns())radio_sm_tick(&sm,now); }
    }
#endif
#if CONFIG_DMR_EMERGENCY
    /* DDM32 Plus (satfix, the verifier's race): an emergency during the sweep (the radio core's), set while the radio
     * task waits in its queue, so the radio tick sees it before the native service: the start waits (200 ms at most)
     * for the native service to stop the search and tune the channel; on this FM channel it then fails there */
    {
        radio_sm_request(&sm,&r,now);
        ps_step();
        CHECK(p25s_on() && p25s_phase()==P25S_SWEEP && !native_lab_owns() && hw.rx_hz>=433000000u);
        uint32_t ch_hz=sm.view.ch.rx_hz;
        CHECK(dmr_emergency_start());
        radio_sm_tick(&sm,now);	/* the tick first */
        uint32_t et0=now,failed=0;
        for(int i=0;i<60 && !failed;i++) {
            now+=10;classic_pump();
            if(!native_lab_owns())radio_sm_tick(&sm,now);
            if(dmr_emergency_state()==DMR_EMERGENCY_FAILED)failed=now-et0;
        }
        CHECK(failed!=0 && failed<200u && !p25s_on() && hw.rx_hz==ch_hz && hw.tx_calls==tx0);
        dmr_emergency_cancel();
        for(int i=0;i<20;i++){ now+=10;classic_pump();if(!native_lab_owns())radio_sm_tick(&sm,now); }
        printf("Plus P25 search: an emergency during the sweep, tick first: search ended, the channel again, failed after %u ms\n",
               (unsigned)failed);
    }
#endif
    hw.st.rssi_dbm=0;
    printf("Plus P25 search in the state machine: sweep, any-NAC probe, found list, give back, skip, PTT never keys\n");
}
#endif
