/* SPDX-License-Identifier: MIT */
#include "radio/beep.h"
/* Real Classic admission/state machine/native controller; worker and RF are fixtures. */
static nt_test *classic_live;
static bool classic_busy, classic_stop, classic_receiving;
static uint32_t classic_sid;
static unsigned classic_starts, classic_restores;
void mock_enter(void) {}
void mock_exit(void) {}
uint32_t hal_micros(void) { return now*1000u; }
static bool classic_on(void) { return true; }
static uint16_t classic_battery(void) { return 8000; }
int p25_lab_busy(void) { return classic_busy; }
static uint16_t classic_set_nac,classic_set_tx_nac,classic_set_tg;
static unsigned classic_sets;
int p25_lab_set_channel(uint16_t n,uint16_t tx,uint16_t t,uint32_t i,int selective)
{
    (void)selective;
    /* Classic: the channel, RX and TX NAC the same. DDM32 Plus: the channel edited to NAC/talkgroup Any (TX NAC
     * inherited from Any: $293, Classic r13), the P25 search's any/any, trunk following, a TX NAC of its own */
    CHECK((n==0x293 && tx==0x293 && t==42 && i==3101234) ||
          (n==0xF7E && (tx==0x293 || tx==0xF7E) && t==0 && (i==0 || i==3101234)) ||
          (n==0xF7E && tx==0x293 && t==42 && i==3101234) ||
          (CONFIG_P25_TRUNK && n==0x293 && tx==0x293 && t==0 && i==0) ||  /* trunk following: the channel NAC, any group */
          (CONFIG_P25_TXNAC && t==42 && i==3101234) ||  /* RX $293 or Any + a TX NAC of its own */
          CONFIG_VFO_MODES ||  /* the VFO's own NACs and talkgroups (tests/plus/test_vfox_radio.inc) */
          CONFIG_XBAND);       /* DDM32 Plus cross-band: its P25 session channel (tests/plus/test_xband_radio.inc) */
    if(!classic_busy){classic_set_nac=n;classic_set_tx_nac=tx;classic_set_tg=t;classic_sets++;}
    return !classic_busy;
}
void p25_lab_channel_stop(void) { classic_stop=true; }
int p25_lab_channel_receiving(void) { return classic_receiving; }
int p25_lab_channel_rx(uint32_t hz)
{ classic_sid=native_lab_voice_start(hz,0);classic_busy=classic_sid!=0;classic_stop=false;return classic_busy; }
int p25_lab_live_begin(uint32_t sid) { classic_sid=sid;classic_busy=true;classic_stop=false;return true; }
void p25_lab_live_up(void) { if(classic_live && classic_live->config.mode==NT_TRANSMIT)classic_stop=true; }
static int classic_start(void *ctx,nt_test *t)
{ (void)ctx;classic_live=t;classic_starts++;return true; }
static int classic_write(void *ctx,const uint8_t b[36]) { (void)ctx;(void)b;return true; }
static void classic_noop(void *ctx) { (void)ctx; }
static int classic_restore(void *ctx) { (void)ctx;classic_restores++;return true; }
const nt_ops classic_ops={.start=classic_start,.write=classic_write,.cancel_send=classic_noop,
 .request_key=classic_noop,.pa_on=classic_noop,.quiet=classic_noop,.restore=classic_restore};

static void classic_pump(void)
{
    native_lab_service(&sm,now);
    if(classic_stop && classic_busy) {
        native_lab_voice_stop(classic_sid);native_lab_service(&sm,now);
        CHECK(native_lab_voice_release(classic_sid));classic_busy=classic_stop=false;
    }
}
static void classic_select(bool receive)
{
    setup_codeplug();start();native_lab_init();
    fake_power.switch_on=classic_on;fake_power.battery_mv=classic_battery;
    classic_busy=classic_stop=classic_receiving=false;classic_starts=classic_restores=0;
    sm.view.ch.mode=RADIO_MODE_P25;sm.view.ch.bw=RADIO_BW_12K5;
    sm.view.ch.p25_nac=0x293;sm.view.ch.p25_tg=42;sm.view.ch.p25_id=3101234;
    sm.view.power=2;
    req(RREQ_PTT,1);CHECK(hw.tx_calls==0 && !sm.ptt);
    if(receive) {
        classic_pump();classic_pump();
        CHECK(classic_live && classic_live->state==NT_RX && native_lab_owns());
    }
}
static void classic_press(void)
{
    sm.tx_start_ms=now-197000u;sm.view.tx_elapsed_ms=197000;
    sm.view.tot_s=15;sm.set.tot_warn_s=10;
    sm.tot_warned=true;sm.view.tot_warning=true;
    beep_stats_t prior_beeps,after_beeps;beep_get_stats(&prior_beeps);
    int prior_starts=aud.starts;
    radio_req_t p={.type=RREQ_PTT,.arg=1};CHECK(native_lab_request(&sm,&p)==1);
    CHECK(sm.tx_start_ms==now&&!sm.view.tx_elapsed_ms&&!sm.tot_warned&&!sm.view.tot_warning);
    classic_pump();classic_pump();
    radio_sm_native_maintenance(&sm,now+100);
    beep_get_stats(&after_beeps);
    CHECK(!sm.view.tot_warning&&after_beeps.played==prior_beeps.played&&aud.starts==prior_starts);
    uint8_t raw[36]={0};
    CHECK(native_lab_live_block(classic_sid,raw,0));CHECK(native_lab_live_block(classic_sid,raw,0));
    CHECK(native_lab_live_go(classic_sid));classic_pump();
    CHECK(classic_live->state==NT_TX && classic_live->config.power==2);
}
static void test_classic_ownership(void)
{
    classic_select(true);
    for(unsigned gate=0;gate<6;gate++) {
        channel_t saved=sm.view.ch;
        sm.set.tx_inhibit=gate==0;sm.view.ch.rx_only=gate==1;
        if(gate==2)sm.view.ch.tx_hz=100000000;
        if(gate==3)sm.batt.level=BATT_CRITICAL;
        if(gate==4)sm.view.ch.p25_id=0;
        if(gate==5){sm.set.bcl=RADIO_BCL_CARRIER;sm.view.state=RS_RX;}
        radio_req_t p={.type=RREQ_PTT,.arg=1};
        CHECK(native_lab_request(&sm,&p)==1 && sm.view.state==RS_TX_DENIED);
        CHECK(classic_live->state==NT_RX && classic_starts==1);
        p.arg=0;CHECK(native_lab_request(&sm,&p)==1 && !sm.ptt);
        sm.view.ch=saved;sm.set.tx_inhibit=false;sm.batt.level=BATT_OK;sm.set.bcl=RADIO_BCL_OFF;
    }
    classic_press();
    beep_stats_t prior_beeps,after_beeps;beep_get_stats(&prior_beeps);
    int prior_starts=aud.starts;
    now+=5000;radio_sm_native_maintenance(&sm,now);beep_get_stats(&after_beeps);
    CHECK(sm.view.tot_warning&&sm.tot_warned&&after_beeps.played==prior_beeps.played+1);
    CHECK(aud.starts==prior_starts&&beep_busy()); /* warning cannot replace P25 PCM */
    int16_t local_beep[160]={0};
    for(unsigned i=0;i<30;i++){memset(local_beep,0,sizeof local_beep);beep_mix(local_beep);}
    CHECK(!beep_busy());
    sm.view.tot_s=1;now+=1000;classic_pump();
    CHECK(!classic_live->owned && !classic_live->keyed && sm.view.tx_deny==TXDENY_TIMEOUT);
    radio_req_t up={.type=RREQ_PTT,.arg=0};CHECK(native_lab_request(&sm,&up)==1);
    classic_pump();classic_pump();
    CHECK(classic_live->state==NT_RX);
    radio_req_t change={.type=RREQ_ZONE,.arg=1};
    CHECK(native_lab_request(&sm,&change)==NATIVE_REQUEST_WAIT);
    CHECK(native_lab_owns()); /* normal core stays excluded until the worker releases */
    classic_pump();CHECK(native_lab_request(&sm,&change)==0);
    radio_sm_request(&sm,&change,now);CHECK(!native_lab_owns());
    CHECK(sm.view.ch.mode==RADIO_MODE_FM);
    req(RREQ_PTT,1);CHECK(sm.view.state==RS_TX && hw.tx_calls==1);
    req(RREQ_PTT,0);CHECK(!sm.ptt && sm.view.state==RS_IDLE);
    /* PTT arriving in the same queue batch as selection must never key FM. */
    classic_select(false);classic_press();CHECK(hw.tx_calls==0);
    radio_req_t off={.type=RREQ_POWER_OFF};
    CHECK(native_lab_request(&sm,&off)==NATIVE_REQUEST_WAIT);classic_pump();
    CHECK(native_lab_request(&sm,&off)==0 && !native_lab_owns() && !sm.ptt);
    uint8_t raw_arm[17]={1},reply[512];
    CHECK(native_lab_command(raw_arm,sizeof raw_arm,reply,sizeof reply)==1 && reply[0]!=0);
}
#if CONFIG_P25_ANY
/* A saved edit reaches the core through codeplug_serial(), not a request: the receiver restarts with the new NAC */
static void test_plus_edit_reselect(void)
{
    classic_select(true);
    CHECK(classic_set_nac==0x293 && classic_set_tg==42);
    unsigned sets=classic_sets;uint32_t old=classic_sid;
    classic_pump();classic_pump();CHECK(classic_sets==sets && classic_sid==old);  /* unchanged: no restart */
    sm.view.ch.p25_nac=0xF7E;sm.view.ch.p25_tg=0;sm.view.ch.rx_only=true;
    for(unsigned i=0;i<4;i++)classic_pump();
    CHECK(classic_sets==sets+1 && classic_set_nac==0xF7E && classic_set_tg==0);
    CHECK(classic_sid!=old && classic_live->state==NT_RX && native_lab_owns());
    /* And back to a fixed NAC/talkgroup */
    sm.view.ch.p25_nac=0x293;sm.view.ch.p25_tg=42;sm.view.ch.rx_only=false;
    for(unsigned i=0;i<4;i++)classic_pump();
    CHECK(classic_set_nac==0x293);
    /* Edited to FM: the native receiver lets go and the FM path is tuned again */
    sm.view.ch.mode=RADIO_MODE_FM;
    for(unsigned i=0;i<4;i++)classic_pump();
    CHECK(!native_lab_owns());
}
#endif
static void test_classic_idle_rearm(void)
{
    classic_select(true);
    uint32_t old=classic_sid;unsigned starts_before=classic_starts;
    /* The RX worker's normal idle exit uses this existing stop/release path. */
    native_lab_voice_stop(old);classic_pump();
    CHECK(!classic_live->owned && classic_live->state==NT_STOPPED && !classic_live->stats.faults);
    CHECK(native_lab_voice_release(old));classic_busy=false;
    classic_pump();classic_pump();
    CHECK(classic_live->state==NT_RX && classic_sid!=old && classic_starts==starts_before+1);
    uint8_t s,r;CHECK(!native_lab_voice_poll(old,&s,&r) && !native_lab_voice_release(old));
    CHECK(classic_live->state==NT_RX && !classic_live->keyed && !hw.tx_calls);

    classic_select(true);old=classic_sid;
    native_lab_voice_stop(old);
    radio_req_t down={.type=RREQ_PTT,.arg=1};
    CHECK(native_lab_request(&sm,&down)==1);
    classic_pump();classic_pump();
    CHECK(classic_sid!=old && classic_live->state==NT_ARMED && classic_live->config.mode==NT_TRANSMIT);
    CHECK(!native_lab_voice_release(old)); /* late RX cleanup cannot release the pending TX */
    uint8_t block[36]={0};
    CHECK(native_lab_live_block(classic_sid,block,0) && native_lab_live_block(classic_sid,block,0));
    CHECK(native_lab_live_go(classic_sid));classic_pump();
    CHECK(classic_live->state==NT_TX && !classic_live->keyed && !hw.tx_calls);
    down.arg=0;CHECK(native_lab_request(&sm,&down)==1);classic_pump();classic_pump();classic_pump();
    CHECK(classic_live->state==NT_RX && !sm.ptt && !classic_live->keyed && !hw.tx_calls);
}
/* The queue is empty except for the actual task's retained request. */
static radio_req_t classic_request;
static bool classic_retained;
static radio_settings_t pending_settings;
static unsigned settings_sent, settings_taken, req_handled;
#define pdTRUE 1
#define portMAX_DELAY 0
#define xSemaphoreTake(...) ((void)0)
#define xSemaphoreGive(...) ((void)0)
#define vTaskDelay(...) ((void)0)
#define xQueueReceive(...) 0
static void classic_dispatch(void)
{
    radio_req_t req=classic_request;
    bool retained_request=classic_retained;
    int32_t left=0;(void)left;
    now+=25;
    /* The real voice worker renews its RF lease while processing audio. */
    if(classic_busy){
        uint8_t state,reason,queued;
        if(classic_live->config.mode==NT_TRANSMIT)
            (void)native_lab_live_poll(classic_sid,&state,&reason,&queued);
        else (void)native_lab_voice_poll(classic_sid,&state,&reason);
    }
#include "classic_dispatch.inc"
    classic_request=req;classic_retained=retained_request;
    classic_pump();
    if(native_lab_owns())radio_sm_native_maintenance(&sm,now);
    else radio_sm_poll(&sm,now);
}
static unsigned classic_saved_edit(uint32_t hz)
{
    unsigned number=sm.view.channel_number;
    chans[number]=sm.view.ch;
    chans[number].rx_hz=chans[number].tx_hz=hz;
    cp_serial++;
    return number;
}
static void test_classic_saved_refresh(void)
{
    classic_retained=false;classic_select(true);
    unsigned number=classic_saved_edit(146550000);
    for(unsigned i=0;i<8;i++)classic_dispatch();
    CHECK(sm.view.channel_number==number && sm.view.ch.rx_hz==146550000);
    CHECK(sm.codeplug_serial==codeplug_serial());
    CHECK(classic_live->state==NT_RX && classic_live->config.frequency_hz==146550000);

    classic_select(true);classic_press();
    uint32_t old_rx=sm.view.ch.rx_hz,old_tx=classic_live->config.frequency_hz;
    classic_saved_edit(146575000);
    for(unsigned i=0;i<2;i++)classic_dispatch();
    CHECK(sm.ptt && classic_live->state==NT_TX && classic_live->config.frequency_hz==old_tx);
    CHECK(sm.view.ch.rx_hz==old_rx && sm.codeplug_serial!=codeplug_serial());
    classic_request=(radio_req_t){.type=RREQ_PTT,.arg=0};classic_retained=true;
    for(unsigned i=0;i<8;i++)classic_dispatch();
    CHECK(!sm.ptt && sm.view.ch.rx_hz==146575000 && sm.view.ch.tx_hz==146575000);
    CHECK(classic_live->state==NT_RX && classic_live->config.frequency_hz==146575000);

    /* An existing channel-change handoff must never be overwritten by reload. */
    classic_select(true);classic_saved_edit(146550000);
    classic_request=(radio_req_t){.type=RREQ_ZONE,.arg=1};classic_retained=true;
    for(unsigned i=0;i<8;i++)classic_dispatch();
    CHECK(!classic_retained && sm.view.zone_index==1 && sm.view.ch.mode==RADIO_MODE_FM);
    CHECK(sm.view.ch.rx_hz==145500000 && hw.rx_hz==145500000);
}
static void test_issue28_idle_dmr(void)
{
    const uint32_t idle_ms[]={1000u,60000u,600000u,1800000u,2147480u,2147490u,4294960u,4294970u,7200000u,2147480000u,4294950000u};
    for(unsigned age=0;age<sizeof idle_ms/sizeof *idle_ms;age++) {
        dmr_start(true);aud.feed=true;
        now=idle_ms[age];radio_sm_poll(&sm,now);radio_sm_tick(&sm,now);
        for(unsigned call=0;call<3;call++) {
            unsigned began=sim.tx_begins;
            req(RREQ_PTT,1);run(600);
            CHECK(sm.ptt && sm.view.state==RS_TX && sim.tx_begins>began && sim.air_count>0);
            req(RREQ_PTT,0);run(1000);
            CHECK(!sm.ptt && sm.view.state==RS_IDLE && !sim.tx_active);
        }
        aud.feed=false;
    }
}
#if CONFIG_HOTSPOT
/* DM32 Plus MMDVM hotspot from a P25 channel (docs/plus.md, "Hotspot"): RREQ_HS_ENTER takes the channel from the
 * native receiver like any request, the receiver stays off while hotspot mode has the modem (no restart, no session),
 * and takes the channel again once hotspot mode ends. Several rounds: nothing left over from the last one. */
static void test_plus_hotspot_p25(void)
{
    classic_select(true);
    for(unsigned round=0;round<3;round++) {
        unsigned sets=classic_sets,starts=classic_starts;uint32_t old=classic_sid;
        CHECK(classic_live->state==NT_RX && native_lab_owns() && !sm.hotspot);
        radio_req_t enter={.type=RREQ_HS_ENTER,.arg=446000000};
        CHECK(native_lab_request(&sm,&enter)==NATIVE_REQUEST_WAIT && native_lab_owns());
        classic_pump();
        CHECK(native_lab_request(&sm,&enter)==0 && !native_lab_owns());
        radio_sm_request(&sm,&enter,now);
        CHECK(sm.hotspot && sm.board_mode==RADIO_MODE_DMR && hw.rx_hz==446000000);
        for(unsigned i=0;i<10;i++) { now+=100;classic_pump(); }
        CHECK(classic_sets==sets && classic_starts==starts && classic_sid==old && !classic_busy);
        CHECK(!native_lab_owns() && !native_lab_reserved() && classic_live->state==NT_STOPPED && !hw.tx_calls);
        /* PTT in hotspot mode: the radio core's (on a P25 channel it ignores PTT), never the P25 transmitter */
        radio_req_t ptt={.type=RREQ_PTT,.arg=1};
        CHECK(native_lab_request(&sm,&ptt)==0);
        radio_sm_request(&sm,&ptt,now);
        classic_pump();classic_pump();
        CHECK(!native_lab_owns() && classic_starts==starts && classic_sets==sets && !hw.tx_calls);
        CHECK(sm.view.state!=RS_TX && sm.hotspot);
        ptt.arg=0;CHECK(native_lab_request(&sm,&ptt)==0);
        radio_sm_request(&sm,&ptt,now);
        CHECK(!sm.ptt && sm.view.state!=RS_TX);
        radio_req_t leave={.type=RREQ_HS_LEAVE};
        CHECK(native_lab_request(&sm,&leave)==0);
        radio_sm_request(&sm,&leave,now);
        CHECK(!sm.hotspot);
        /* The channel is the P25 one again (the fixture's codeplug has no P25 channel: tune() loaded FM) */
        sm.view.ch.mode=RADIO_MODE_P25;sm.view.ch.bw=RADIO_BW_12K5;
        sm.view.ch.p25_nac=0x293;sm.view.ch.p25_tg=42;sm.view.ch.p25_id=3101234;
        classic_pump();classic_pump();
        CHECK(classic_sets==sets+1 && classic_sid!=old && classic_live->state==NT_RX && native_lab_owns());
        CHECK(!hw.tx_calls);
    }
}
#endif
#if CONFIG_P25_SCAN
#include "test_scan.inc"
#endif
#if CONFIG_DMR_SEARCH
#include "test_dmrsearch_sm.inc"
#endif
#if CONFIG_ALL_SEARCH
#include "test_allsearch_sm.inc"
#endif
#if CONFIG_P25_TRUNK
#include "test_trunk_sm.inc"
#endif
#if CONFIG_DMR_TRUNK
#include "test_dmrtrunk_sm.inc"
#endif
int main(void)
{
    int prior=upstream_radio_main();test_issue28_idle_dmr();test_classic_ownership();test_classic_idle_rearm();
    test_classic_saved_refresh();
#if CONFIG_P25_ANY
    test_plus_edit_reselect();
#endif
#if CONFIG_HOTSPOT
    test_plus_hotspot_p25();
#endif
#if CONFIG_P25_SCAN
    test_plus_scan();
#endif
#if CONFIG_DMR_SEARCH
    test_plus_dmrsearch_sm();
#endif
#if CONFIG_ALL_SEARCH
    test_plus_allsearch_sm();
    test_plus_allsearch_watch();
#endif
#if CONFIG_P25_TRUNK
    test_plus_trunk_sm();
#endif
#if CONFIG_DMR_TRUNK
    test_plus_dmrtrunk_sm();
#endif
    printf("Classic real FM/DMR/P25 admission and ownership: %d failures\n",failures);
    return prior || failures ? 1 : 0;
}
