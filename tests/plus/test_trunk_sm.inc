/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus P25 trunk following in the real radio state machine and native admission (radio_sm.c, native_lab.c):
 * Tools > P25 trunk's request starts it from the selected P25 channel; the native receiver takes the control channel
 * (the channel's NAC, any talkgroup, TSDUs on); a grant for the followed group moves it to the voice channel and the
 * terminator back; PTT is denied as on an RX-only channel and never keys; another request ends it; leaving the screen
 * stops it and the channel is received as before; a channel that is not P25 can't start it. Included by
 * tests/classic/test_radio.inc when built with CONFIG_P25_TRUNK=1; the worker (its TSBK queue and counts) and RF
 * are fixtures. */
#include <assert.h>
#include "p25_lab.h"
#include "radio/p25trunk.h"
static p25_lab_trunk_t tk_counts;
static uint8_t tk_queue[16][10];
static unsigned tk_queued, tk_taken, tk_trunk_sets;
static int tk_trunk_on;
int p25_lab_set_trunk(int on)
{
    if(classic_busy)return 0;
    tk_trunk_on=on;tk_trunk_sets++;
    return 1;
}
int p25_lab_tsbk_pop(uint8_t out[10])
{
    if(tk_taken==tk_queued)return 0;
    memcpy(out,tk_queue[tk_taken++],10);
    return 1;
}
void p25_lab_trunk_counts(p25_lab_trunk_t *out) { *out=tk_counts; }
static void tk_push(const uint8_t b[10]) { assert(tk_queued<16u);memcpy(tk_queue[tk_queued++],b,10); }
/* The radio task's turn 10 ms on; the screen looks */
static void tk_step(void)
{
    p25tk_view_t v;
    now+=10;p25tk_snapshot(&v);classic_pump();
}
/* A request as the radio task makes it: native admission first (it may wait for the worker), then the core */
static void tk_task_req(uint8_t type,int32_t arg,uint32_t arg2)
{
    radio_req_t r={.type=type,.arg=arg,.arg2=arg2};
    int h=native_lab_request(&sm,&r);
    for(int i=0;i<50 && h==NATIVE_REQUEST_WAIT;i++){classic_pump();h=native_lab_request(&sm,&r);}
    if(!h)radio_sm_request(&sm,&r,now);
}
static uint32_t tk_rx_hz(void) { return classic_live && classic_live->state==NT_RX ? classic_live->config.frequency_hz : 0; }
static void test_plus_trunk_sm(void)
{
    classic_select(false);
    sm.view.ch.rx_hz=433500000u;    /* the control channel; NAC 293, talkgroup 42 */
    tk_queued=tk_taken=tk_trunk_sets=0;tk_trunk_on=0;memset(&tk_counts,0,sizeof tk_counts);
    int tx0=hw.tx_calls;
    tk_task_req(RREQ_P25_TRUNK,1,0);
    CHECK(p25tk_on());
    for(int i=0;i<10;i++)tk_step();
    CHECK(native_lab_owns() && tk_trunk_on && classic_set_nac==0x293 && classic_set_tg==0 && tk_rx_hz()==433500000u);
    /* The control channel: the plan (id 1: 433.000 MHz + n x 12.5 kHz), then a grant for TG 42 on 434.000 */
    static const uint8_t iden[10]={0x3D,0x00,0x13,0x20,0x00,100,0x05,0x29,0x69,0x40};
    static const uint8_t grant[10]={0x00,0x00,0x00,0x10,80,0x00,42,0x12,0xD6,0x87};
    tk_push(iden);tk_counts.frames+=1;tk_counts.last_ms=now;tk_counts.nac=0x293;
    for(int i=0;i<5;i++)tk_step();
    CHECK(tk_rx_hz()==433500000u);
    uint32_t t0=now;
    tk_push(grant);tk_counts.frames+=1;tk_counts.last_ms=now;
    for(int i=0;i<20 && tk_rx_hz()!=434000000u;i++)tk_step();
    CHECK(tk_rx_hz()==434000000u && native_lab_owns());
    /* radio task turns (10 ms each here) from the grant to the receiver running on the voice channel */
    unsigned turns=(unsigned)(now-t0)/10u;
    CHECK(turns<=4u);
    /* The call: voice frames, then the terminator: back on the control channel */
    for(int i=0;i<30;i++){tk_counts.frames++;tk_counts.last_ms=now;tk_step();}
    CHECK(tk_rx_hz()==434000000u);
    tk_counts.terms++;
    for(int i=0;i<20 && tk_rx_hz()!=433500000u;i++)tk_step();
    CHECK(tk_rx_hz()==433500000u && p25tk_on());
    /* PTT: denied as on an RX-only channel, nothing keyed, the following goes on */
    radio_req_t ptt={.type=RREQ_PTT,.arg=1};
    CHECK(native_lab_request(&sm,&ptt)==1 && sm.ptt && sm.view.state==RS_TX_DENIED && sm.view.tx_deny==TXDENY_RX_ONLY);
    for(int i=0;i<5;i++)tk_step();
    CHECK(hw.tx_calls==tx0 && classic_live->config.mode!=NT_TRANSMIT && !classic_live->keyed && p25tk_on() &&
          tk_rx_hz()==433500000u);
    ptt.arg=0;
    CHECK(native_lab_request(&sm,&ptt)==1 && !sm.ptt && sm.view.state!=RS_TX_DENIED);
    /* Another request ends it: the channel is tuned again */
    tk_task_req(RREQ_CH_STEP,1,0);
    CHECK(!p25tk_on() && hw.tx_calls==tx0);
    /* The screen left: the follower stops by itself and the selected P25 channel is received as before */
    classic_select(false);
    sm.view.ch.rx_hz=433500000u;
    tk_task_req(RREQ_P25_TRUNK,1,1);
    for(int i=0;i<10;i++)tk_step();
    CHECK(p25tk_on() && native_lab_owns() && tk_rx_hz()==433500000u && classic_set_tg==0);
    for(int i=0;i<700 && p25tk_on();i++){now+=10;classic_pump();}
    for(int i=0;i<10;i++){now+=10;classic_pump();}
    CHECK(!p25tk_on() && !tk_trunk_on && classic_set_nac==0x293 && classic_set_tg==42 && tk_rx_hz()==433500000u);
#if CONFIG_DMR_EMERGENCY
    /* DDM32 Plus (mwfix4): an emergency asked for while following (the key, the lone worker, SOS) never waits for it:
     * the following ends, the receiver is let go and the radio tick takes the start: on this P25 channel it fails at
     * once (SOS / the lone worker fall back), within a second; the channel is received again after */
    classic_select(false);
    sm.view.ch.rx_hz=433500000u;
    tk_task_req(RREQ_P25_TRUNK,1,0);
    for(int i=0;i<10;i++)tk_step();
    CHECK(p25tk_on() && native_lab_owns());
    dmr_emergency_settings_t tk_es;
    dmr_emergency_settings_default(&tk_es);
    tk_es.mode=DMR_EMERGENCY_MODE_CALL;
    dmr_emergency_set_settings(&tk_es);
    radio_sm_tick(&sm,now);
    CHECK(dmr_emergency_start());
    uint32_t tk_t0=now,tk_failed=0;
    for(int i=0;i<300 && !tk_failed;i++) {
        tk_counts.frames++;tk_counts.last_ms=now;	/* the control channel goes on */
        tk_step();
        if(!native_lab_owns())radio_sm_tick(&sm,now);	/* as radio_task.c: the tick only without the native receiver */
        if(dmr_emergency_state()==DMR_EMERGENCY_FAILED)tk_failed=now-tk_t0;
    }
    CHECK(tk_failed!=0 && tk_failed<=1000u && !p25tk_on() && hw.tx_calls==tx0);
    printf("Plus P25 trunk: an emergency while following: following ended, the emergency failed (P25 channel) after %u ms\n",
           (unsigned)tk_failed);
    dmr_emergency_cancel();
    for(int i=0;i<20;i++){now+=10;classic_pump();if(!native_lab_owns())radio_sm_tick(&sm,now);}
#endif
    /* Not a P25 channel: no start */
    classic_select(false);
    sm.view.ch.mode=RADIO_MODE_FM;
    tk_task_req(RREQ_P25_TRUNK,1,0);
    CHECK(!p25tk_on());
    printf("Plus P25 trunk in the state machine: control channel taken, grant -> voice channel (%u radio task turns) "
           "-> terminator -> back, PTT denied (RX only), other request ends it, idle stop gives the channel back, FM "
           "channel refused\n",turns);
}
