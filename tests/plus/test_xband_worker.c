/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus cross-band P25 -> P25 through the actual P25 channel worker (radio/xband.h, p25_lab.h: a P25 session's
 * memory and frame ring). Host only: every RF edge is a fake, nothing transmits.
 *
 * The real p25_lab.c is #included (as tests/plus/test_trunk_worker.c does) and built with CONFIG_XBAND=1; its worker
 * task body run() is driven with a transmission (request_mode 4) and a channel receive (request_mode 5). Mocked:
 * FreeRTOS and the clock, the board's PCM audio (never started here), PTT, the native modem's live session (one 36-byte
 * block taken every 30 ms from the worker's go, an underrun faults) and its receive session (one record every 30 ms).
 * The radio task's side of the ring (p25_lab_xb_put / _take, every 25 ms) runs in vTaskDelay. The P25 framing, FEC and
 * the host IMBE codec are the real ones.
 *
 * Checked: the lend (two pieces past the ring, refused while the worker runs or lent already); the microphone never
 * opens and a transmission without the ring armed is refused; 120 IMBE frames handed to the ring go out in order, bit
 * for bit, in LDUs with the NAC, talkgroup and unit ID given (our own receiver decodes the blocks: HDU, LDUs, TDU); the
 * same blocks received again: every frame back through the ring in order, with the call's NAC, talkgroup and source,
 * no audio started; neither run writes a byte of the lent memory; the give back. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include "p25_lab.c"

static unsigned failures;
#define CHECK(c) do { if(!(c)) { failures++; printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); } } while(0)

/* ---- FreeRTOS, clock, board ---- */

static jmp_buf finished;
static void (*worker_fn)(void *);
static uint32_t clock_ms,maximum_ms;
static unsigned depth,turn;

void audio_test_enter(void) { depth++; }
void audio_test_exit(void) { assert(depth);depth--; }
uint32_t hal_millis(void) { return clock_ms; }
uint32_t hal_micros(void) { return clock_ms*1000u; }
BaseType_t xTaskCreate(void (*fn)(void *),const char *name,unsigned words,void *ctx,unsigned priority,TaskHandle_t *h)
{ (void)name;(void)words;(void)ctx;(void)priority;worker_fn=fn;*h=(void *)1;return pdPASS; }
void xTaskNotifyGive(TaskHandle_t h) { assert(h && !depth); }
uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t wait)
{ assert(clear && wait==portMAX_DELAY);if(turn++)longjmp(finished,1);return 1; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t h) { (void)h;return 512; }

static unsigned audio_starts,route_calls;
static bool audio_start(audio_block_cb_t cb,void *ctx) { (void)cb;(void)ctx;audio_starts++;return true; }
static void audio_stop(void) { }
static void spk(bool on) { (void)on; }
static void mic_route(bool on) { (void)on;route_calls++; }
static const hal_audio_t audio={.start=audio_start,.stop=audio_stop,.speaker_pcm=spk};
const board_t board={.audio=&audio,.native_mic_route=mic_route};
bool input_ptt_down(void) { return false; }	/* never held: a cross-band over is not a PTT */

/* ---- The frames: valid IMBE parameters (the host encoder's, of varied tones) ---- */
#define NFR 120u
static uint16_t params[NFR][8];
static void make_params(void)
{
    static dm32_imbe_context enc;
    dm32_imbe_init(&enc);
    for(unsigned k=0;k<NFR;k++) {
        int16_t pcm[160];
        unsigned period=16u+(k*7u)%50u;
        for(unsigned i=0;i<160u;i++)pcm[i]=(int16_t)((int)((i+k*3u)%period)*600-(int)period*300);
        CHECK(dm32_imbe_encode(&enc,pcm,params[k]));
    }
}

/* ---- The radio task's side of the ring, every 25 ms ---- */
static bool radio_sending,radio_receiving;
static unsigned put_n,got_n;
static uint16_t got[NFR+20][8];
static uint32_t radio_at;
static void radio_side(void)
{
    if(clock_ms-radio_at<25u)return;
    radio_at=clock_ms;
    if(radio_sending) {
        uint8_t f[P25_LAB_XB_BYTES];
        while(put_n<NFR) {
            rp_pack(params[put_n],f);
            if(!p25_lab_xb_put(f))break;
            put_n++;
        }
        if(put_n==NFR)p25_lab_xb_send(0);	/* no more frames */
    }
    if(radio_receiving) {
        uint8_t f[P25_LAB_XB_BYTES];
        while(got_n<NFR+20u && p25_lab_xb_take(f))rp_unpack(f,got[got_n++]);
    }
}

/* ---- Native modem live session (fake), as tests/plus/test_ysf_tx_worker.c ---- */
#define MAX_BLOCKS 200
static uint8_t blocks[MAX_BLOCKS][36];
static unsigned nloaded,go_calls,stops,releases;
static uint32_t session_sid,go_ms;
static bool kicked,faulted,sealed;
static unsigned taken_by(uint32_t t) { return kicked && t>=go_ms ? (t-go_ms)/30u+1u : 0u; }
static void modem_update(void)
{
    if(!kicked || faulted)return;
    unsigned taken=taken_by(clock_ms);
    if(sealed && taken>=nloaded)return;
    if(taken>nloaded)faulted=true;
}
int native_lab_live_poll(uint32_t sid,uint8_t *s,uint8_t *r,uint8_t *q)
{
    if(sid!=session_sid || stops)return 0;
    modem_update();
    unsigned taken=taken_by(clock_ms);
    if(faulted) { *s=NT_FAULT;*r=NT_UNDERRUN; }
    else if(kicked && sealed && taken>=nloaded) { *s=NT_STOPPED;*r=NT_TX_COMPLETE; }
    else *s=kicked?NT_TX:NT_ARMED,*r=NT_OK;
    unsigned in=nloaded>taken?nloaded-taken:0u;
    *q=(uint8_t)(in>255u?255u:in);
    return 1;
}
int native_lab_live_block(uint32_t sid,const uint8_t block[36],int last)
{
    if(sid!=session_sid || sealed || stops)return 0;
    modem_update();
    assert(nloaded<MAX_BLOCKS);
    memcpy(blocks[nloaded++],block,36);
    if(last)sealed=true;
    return 1;
}
int native_lab_live_go(uint32_t sid)
{
    if(sid!=session_sid || kicked)return 0;
    go_calls++;kicked=true;go_ms=clock_ms;
    return 1;
}
void native_lab_live_summary(uint32_t out[5]) { memset(out,0,5*sizeof *out); }
int native_lab_reserved(void) { return 0; }

/* ---- Native modem receive session (fake): one record every 30 ms, the blocks sent above, then empty ones ---- */
static uint8_t rec_raw[MAX_BLOCKS][36];
static unsigned rec_count,rec_next,rx_starts,rx_owned;
static uint32_t next_raw_at;
uint32_t native_lab_voice_start(uint32_t hz,uint32_t ms)
{ assert(hz==145500000u && !ms && !rx_owned);rx_owned=1;rx_starts++;next_raw_at=clock_ms;rec_next=0;return 100u+rx_starts; }
int native_lab_voice_poll(uint32_t sid,uint8_t *s,uint8_t *why)
{ assert(sid==100u+rx_starts && rx_owned);*s=NT_RX;*why=NT_OK;return 1; }
int native_lab_voice_read(uint32_t sid,nt_record *r)
{
    assert(sid==100u+rx_starts && rx_owned);
    if(clock_ms<next_raw_at || rec_next>=rec_count+40u)return 0;
    memset(r,0,sizeof *r);r->sequence=rec_next;r->time_us=next_raw_at*1000u;
    if(rec_next<rec_count)memcpy(r->raw,rec_raw[rec_next],36);
    rec_next++;next_raw_at+=30;return 1;
}
void native_lab_voice_stop(uint32_t sid)
{
    if(sid>=100u) { assert(sid==100u+rx_starts && rx_owned);return; }
    CHECK(sid==session_sid);stops++;
}
int native_lab_voice_release(uint32_t sid)
{
    if(sid>=100u) { assert(sid==100u+rx_starts && rx_owned);rx_owned=0;return 1; }
    CHECK(sid==session_sid && stops);releases++;return 1;
}

void vTaskDelay(TickType_t ms)
{
    assert(!depth);
    clock_ms+=ms;assert(clock_ms<=maximum_ms);
    radio_side();
    modem_update();
}

/* ---- Bits ---- */
static unsigned getb(const uint8_t *p,unsigned i) { return (p[i>>3]>>(7u-(i&7u)))&1u; }
static void putb(uint8_t *p,unsigned i,unsigned b)
{ uint8_t m=(uint8_t)(0x80u>>(i&7u));p[i>>3]=b?(uint8_t)(p[i>>3]|m):(uint8_t)(p[i>>3]&~m); }
/* The record's RAM layout of 288 air bits (tests/plus/test_trunk_worker.c) */
static const uint16_t layout01[][2]={{0,24},{72,108},{24,48},{180,108}};
static void air_to_ram(const uint8_t air[36],uint8_t ram[36])
{
    unsigned a=0;
    memset(ram,0,36);
    for(unsigned s=0;s<4u;s++)for(unsigned i=0;i<layout01[s][1];i++,a++)putb(ram,layout01[s][0]+i,getb(air,a));
}

/* ---- Our own receiver for the blocks sent ---- */
static uint16_t heard[NFR+20][8];
static unsigned heard_n;
static void ref_tap(void *ctx,const uint16_t u[8]) { (void)ctx;if(heard_n<NFR+20u)memcpy(heard[heard_n++],u,16); }
static void ref_sink(void *ctx,const int16_t pcm[160]) { (void)ctx;(void)pcm; }

static uint8_t lent_pattern(const uint8_t *p,size_t n)
{
    for(size_t i=0;i<n;i++)if(p[i]!=(uint8_t)(0xA5u^i))return 0;
    return 1;
}
static void fill_pattern(uint8_t *p,size_t n) { for(size_t i=0;i<n;i++)p[i]=(uint8_t)(0xA5u^i); }

int main(void)
{
    p25_lab_start();beep_init(&audio);
    make_params();
    unsigned before=failures;

    /* The lend: refused while the worker runs; two pieces past the ring; once only */
    uint8_t *seg[3];
    size_t len[3];
    state=1;
    CHECK(p25_lab_lend(seg,len,true)==0 && !xb_lent);
    state=2;
    CHECK(p25_lab_lend(seg,len,true)==2 && xb_lent==2);
    CHECK(seg[0]==(uint8_t *)&fifo+P25_LAB_XB_RING_BYTES && len[0]==sizeof fifo-P25_LAB_XB_RING_BYTES);
    CHECK(seg[1]==&rp_buf[0][0] && len[1]==sizeof rp_buf);
    uint8_t *s2[3];size_t l2[3];
    CHECK(p25_lab_lend(s2,l2,false)==0);
    fill_pattern(seg[0],len[0]);fill_pattern(seg[1],len[1]);
    /* the microphone never: a transmission without the ring armed is refused */
    session_sid=7;
    CHECK(!p25_lab_live_begin(session_sid) && state!=1);
    printf("%s lend: two pieces past the ring, refused while running or lent; no transmission without the ring\n",
           failures==before?"ok  ":"FAIL");

    /* --- The transmission: the ring's frames in LDUs, the link control given --- */
    before=failures;
    CHECK(p25_lab_set_channel(0xF7E,0x293,1,3101234,0));
    p25_lab_xb_call(0x1C8,655,3101234);	/* (the heard NAC and talkgroup, our unit ID) */
    p25_lab_xb_send(1);
    radio_sending=true;put_n=0;
    clock_ms=depth=turn=0;maximum_ms=20000;
    radio_at=(uint32_t)-25;
    radio_side();
    CHECK(put_n==P25_LAB_XB_FRAMES);	/* the ring holds 16 */
    CHECK(p25_lab_live_begin(session_sid));
    if(!setjmp(finished))worker_fn(NULL);
    radio_sending=false;
    if(state!=2 || faulted || stops!=1 || releases!=1)
        printf("  (state %u kicked %d sealed %d faulted %d go %u stops %u releases %u at %u ms, %u blocks, stop %u overflows %u reason %u)\n",
               (unsigned)state,kicked,sealed,faulted,go_calls,stops,releases,(unsigned)clock_ms,nloaded,(unsigned)stop_requested,(unsigned)live_overflows,(unsigned)native_reason);
    CHECK(state==2 && kicked && sealed && !faulted && go_calls==1 && stops==1 && releases==1);
    CHECK(audio_starts==0 && route_calls==0);	/* the microphone never opened */
    CHECK(put_n==NFR && p25_lab_xb_sent() && !XBR->send && XBR->head==XBR->tail);
    CHECK(lent_pattern(seg[0],len[0]) && lent_pattern(seg[1],len[1]));
    for(unsigned b=0;b<3u;b++)for(unsigned i=0;i<36u;i++)CHECK(blocks[b][i]==0x77);
    /* our own receiver: the HDU's link control, then every frame in order, bit for bit */
    static p25_voice ref;
    p25_voice_init(&ref,0x1C8,0,ref_sink,NULL);
    ref.tap=ref_tap;
    uint16_t ref_tg=0;uint32_t ref_src=0;
    for(unsigned b=0;b<nloaded;b++)for(unsigned i=0;i<144u;i++) {
        p25_voice_rx_dibit(&ref,(uint8_t)(getb(blocks[b],2u*i)<<1|getb(blocks[b],2u*i+1u)));
        if(ref.have_group) { ref_tg=ref.active_group;ref_src=ref.active_source; }
    }
    CHECK(ref_tg==655 && ref_src==3101234 && ref.rx_frames>=4u);
    CHECK(heard_n>=NFR && heard_n<=NFR+18u);	/* then the encoder's tail and the last LDU's padding */
    unsigned same=0;
    for(unsigned k=0;k<NFR && k<heard_n;k++)same+=!memcmp(heard[k],params[k],16);
    CHECK(same==NFR);
    printf("%s transmission: %u frames from the ring out in %u blocks, %u of them bit for bit, TG %u ID %u NAC 1C8, "
           "no microphone, the lent memory untouched\n",failures==before?"ok  ":"FAIL",put_n,nloaded,same,
           (unsigned)ref_tg,(unsigned)ref_src);

    /* --- The receive: the same blocks heard again, every frame back through the ring, nothing played --- */
    before=failures;
    for(unsigned b=0;b<nloaded;b++)air_to_ram(blocks[b],rec_raw[b]);
    rec_count=nloaded;
    radio_receiving=true;radio_at=0;got_n=0;
    clock_ms=depth=turn=0;maximum_ms=20000;
    CHECK(p25_lab_set_channel(0xF7E,0x293,1,3101234,0));
    CHECK(p25_lab_channel_rx(145500000u) && play_requested==0);
    if(!setjmp(finished))worker_fn(NULL);
    radio_side();clock_ms+=25;radio_side();
    radio_receiving=false;
    CHECK(rx_starts==1 && !rx_owned && !audio_on && audio_starts==0);
    CHECK(got_n>=NFR && got_n<=NFR+18u);
    same=0;
    for(unsigned k=0;k<NFR && k<got_n;k++)same+=!memcmp(got[k],params[k],16);
    CHECK(same==NFR);
    uint16_t hn,ht;uint32_t hs;
    p25_lab_xb_heard(&hn,&ht,&hs);
    CHECK(hn==0x1C8 && ht==655 && hs==3101234);
    CHECK(lent_pattern(seg[0],len[0]) && lent_pattern(seg[1],len[1]) && rp_count==0);
    printf("%s receive: %u frames back through the ring, %u of them as sent, NAC %03X TG %u ID %u, nothing played, "
           "the lent memory untouched\n",failures==before?"ok  ":"FAIL",got_n,same,(unsigned)hn,(unsigned)ht,
           (unsigned)hs);

    /* --- The give back: cleared, the worker's own again --- */
    before=failures;
    p25_lab_give_back();
    CHECK(!xb_lent && fifo.count==0 && rp_count==0);
    uint8_t f[P25_LAB_XB_BYTES]={0};
    CHECK(!p25_lab_xb_take(f) && !p25_lab_xb_put(f));
    CHECK(p25_lab_lend(seg,len,false)==3 && xb_lent==1);	/* a DMR session's lend: all three */
    p25_lab_give_back();
    printf("%s give back: cleared, the ring gone\n",failures==before?"ok  ":"FAIL");
    printf("cross-band P25 worker: %u failures\n",failures);
    return failures?1:0;
}
