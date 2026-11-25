/* SPDX-License-Identifier: MIT */
/* Conventional P25 worker and device/audio integration. */
#include <string.h>
#include "p25_lab.h"
#include "selftest.h"
#include "phy_stream.h"
#include "audio_fifo.h"
#include "mic_condition.h"
#include "cpu_profile.h"
#include "config.h"
#include "board.h"
#include "hal_time.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dl_fmt.h"
#include "FreeRTOS.h"
#include "task.h"
#if CONFIG_BEEP
#include "radio/beep.h"
#endif
#if CONFIG_SIGNAL_QUALITY
#include "radio/sigq.h"
#endif
#if CONFIG_P25_EXTRAS
#include "ui/i18n.h"
#endif
#if CONFIG_DEV_TX
#include "input/input.h"
#endif
#if CONFIG_P25_NATIVE
#include "native_modem/native_lab.h"
#if CONFIG_YSF
#include "ysf/ysf.h"
#include "ysf/ysf_vw.h"
#include "p25.h"
#include "dmr/ambe/ambeVoice.h"
#include "dmr/ambe/ambeFec.h"
#if CONFIG_YSF_TX
#include "ysf/ysf_tx.h"
#endif
#endif
#endif

static p25_voice voice;
#if CONFIG_XBAND
/* DDM32 Plus cross-band (p25_lab_lend): 1 voice, fifo and rp_buf are lent; 2 a P25 session's, fifo past the frame ring
 * and rp_buf (the worker receives and transmits for it, never with audio) */
static volatile uint8_t xb_lent;
#define XB_LENT (xb_lent==1)
#define XB_P25 (xb_lent==2)
#define XB_ANY (xb_lent!=0)
#else
#define XB_LENT 0
#define XB_P25 0
#define XB_ANY 0
#endif
#if CONFIG_P25_BENCH || CONFIG_DEV_TX
static p25_selftest_result result;
#endif
#if CONFIG_P25_BENCH
static uint8_t sample_checks=1;
#else
#define sample_checks 0
#endif
static TaskHandle_t worker;
static volatile uint8_t state; /* 0 idle, 1 pending/running, 2 passed/finished, 3 failed, 4 no worker */
static uint8_t request_mode, play_requested, delay_seconds;
static volatile uint8_t stop_requested;
static uint32_t stack_free;
#if CONFIG_P25_BENCH
/* Cable bench replay records; a product build keeps one (docs/plus.md, RAM) */
#ifndef P25_REPLAY_RECORDS
#define P25_REPLAY_RECORDS 64
#endif
static struct { uint8_t kind,raw[36]; } replay[P25_REPLAY_RECORDS];
#endif
static uint16_t expected,loaded,nac,group;
static uint32_t frequency,duration_ms;
static p25_audio_fifo fifo;
static uint32_t records,pcm_blocks,pcm_crc,nonzero,record_max_us,decode_us;
static uint32_t callbacks,played,played_nonzero,session,gaps,unsupported;
static uint8_t native_state,native_reason,audio_on;
static uint8_t pcm_paced=1;
static p25_phy_rx phy;
#if CONFIG_DEV_TX
static uint32_t live_sid;
#if CONFIG_P25_CHANNELS
static p25_call channel_call;
static uint16_t channel_rx_nac;
static uint8_t channel_selective;
#if CONFIG_P25_RXGL
/* DDM32 Plus P25 RX group list of the channel (p25_lab_set_groups): with Selective its talkgroups play too */
static uint16_t channel_groups[P25_LAB_GROUPS];
static uint8_t channel_group_count;
#endif
static uint32_t channel_voice_ms;
static bool channel_voice_seen;
#if CONFIG_P25_SCAN
/* The last valid frame of the channel's kind, 0 = none yet (not cleared when the receive starts over: the scan
 * compares it with its take, native_lab.c) */
static volatile uint32_t channel_sync_ms;
static void channel_synced(void) { uint32_t ms=hal_millis();channel_sync_ms=ms?ms:1u; }
#endif
#if CONFIG_P25_TRUNK
/* DM32 Plus P25 trunk following: the channel receive also frames TSDUs (the pinned CAI library's control channel
 * receiver, voice.c); each valid TSBK waits here for the radio task (native_lab.c hands it to radio/p25trunk.c), the
 * oldest dropped when nobody took them in time */
enum { TSBK_RING=8 };
static bool trunk_rx;
static uint8_t tsbk_ring[TSBK_RING][10],tsbk_head,tsbk_count;
static p25_lab_trunk_t trunk_counts;
#if CONFIG_YSF
/* RAM (Classic r13 base): a trunk following receive is never a YSF one (p25_lab_set_trunk and p25_lab_set_ysf turn
 * the other off), so the TSDU receiver shares the YSF framer's storage */
#define P25_LAB_RX_SHARED 1
static union { ysf_framer_t ysf; p25cai_control_receiver tsdu; } rx_shared;
#define tsdu_rx rx_shared.tsdu
#define ysf_fr rx_shared.ysf
static bool ysf_rx;
#else
static p25cai_control_receiver tsdu_rx;
#endif
int p25_lab_set_trunk(int on)
{
    taskENTER_CRITICAL();
    int ok=state!=1;
    if(ok) { trunk_rx=on!=0;tsbk_head=tsbk_count=0; }
#if CONFIG_YSF
    if(ok && on)ysf_rx=false;
#endif
    taskEXIT_CRITICAL();
    return ok;
}
int p25_lab_tsbk_pop(uint8_t out[10])
{
    taskENTER_CRITICAL();
    int ok=tsbk_count!=0;
    if(ok) {
        memcpy(out,tsbk_ring[tsbk_head],10);
        tsbk_head=(uint8_t)((tsbk_head+1u)%TSBK_RING);tsbk_count--;
    }
    taskEXIT_CRITICAL();
    return ok;
}
void p25_lab_trunk_counts(p25_lab_trunk_t *out)
{
    taskENTER_CRITICAL();*out=trunk_counts;taskEXIT_CRITICAL();
}
/* voice.c: a TSDU the control receiver completed (its blocks' CRCs held), or (unit NULL) one it dropped on a block
 * that failed */
static void trunk_tsdu(void *ctx,const p25cai_control *unit)
{
    (void)ctx;
    taskENTER_CRITICAL();
    if(!unit)trunk_counts.bad++;
    else trunk_counts.nac=unit->nac;
    for(unsigned i=0;unit && i<unit->count;i++) {
        if(tsbk_count==TSBK_RING) {
            tsbk_head=(uint8_t)((tsbk_head+1u)%TSBK_RING);tsbk_count--;trunk_counts.dropped++;
        }
        memcpy(tsbk_ring[(tsbk_head+tsbk_count)%TSBK_RING],unit->block[i],10);tsbk_count++;
    }
    taskEXIT_CRITICAL();
}
#endif
int p25_lab_set_channel(uint16_t receive_nac,uint16_t transmit_nac,uint16_t talkgroup,uint32_t radio_id,int selective)
{
    taskENTER_CRITICAL();
    int ok=state!=1;
    if(ok){
        channel_rx_nac=receive_nac;channel_selective=selective!=0;
        channel_call=(p25_call){.nac=transmit_nac,.talkgroup=talkgroup,.source_id=radio_id};
    }
#if CONFIG_P25_TRUNK
    if(ok)trunk_rx=false;   /* a channel of its own: no TSDUs (p25_lab_set_trunk after this for trunk following) */
#endif
#if CONFIG_P25_RXGL
    if(ok)channel_group_count=0;   /* no RX group list (p25_lab_set_groups after this) */
#endif
    taskEXIT_CRITICAL();
    return ok;
}
#if CONFIG_P25_RXGL
int p25_lab_set_groups(const uint16_t *talkgroups,uint8_t count)
{
    if(count>P25_LAB_GROUPS)count=P25_LAB_GROUPS;
    taskENTER_CRITICAL();
    int ok=state!=1;
    if(ok){
        for(unsigned i=0;i<count;i++)channel_groups[i]=talkgroups[i];
        channel_group_count=count;
    }
    taskEXIT_CRITICAL();
    return ok;
}
#endif
void p25_lab_channel_stop(void) { stop_requested=1; }
#if CONFIG_P25_SEARCH
/* DM32 Plus P25 search: valid P25 frames heard on the channel receive (encrypted or not), when the last one came,
 * and its NAC; the search's probe decides "P25 here" from them */
static volatile uint32_t channel_frames, channel_frame_ms;
uint32_t p25_lab_channel_frames(uint32_t *last_ms,uint16_t *heard_nac)
{
    taskENTER_CRITICAL();
    uint32_t n=channel_frames;
    if(last_ms)*last_ms=channel_frame_ms;
    if(heard_nac)*heard_nac=voice.active_nac;
    taskEXIT_CRITICAL();
    return n;
}
#endif
#if CONFIG_P25_CALLER
/* DM32 Plus: the talkgroup and source ID of the P25 call being heard (from its link control), for Home */
static uint16_t p25_tg, p25_nac=P25_NAC_ANY;
static uint32_t p25_src;
static volatile uint32_t p25_caller_seq;
uint32_t p25_lab_p25_caller(uint16_t *tg,uint32_t *src)
{
    taskENTER_CRITICAL();
    if(tg)*tg=p25_tg;
    if(src)*src=p25_src;
    uint32_t seq=p25_caller_seq;
    taskEXIT_CRITICAL();
    return seq;
}
#if CONFIG_P25_EXTRAS
/* DM32 Plus P25 extras: that call's kind (P25_LAB_EMERGENCY, P25_LAB_PRIVATE) and the unit a unit-to-unit call is for */
static uint32_t p25_target;
static uint8_t p25_kind;
static unsigned voice_kind(void)
{ return (voice.emergency?(unsigned)P25_LAB_EMERGENCY:0u)|(voice.private_call?(unsigned)P25_LAB_PRIVATE:0u); }
unsigned p25_lab_p25_kind(uint32_t *target)
{
    taskENTER_CRITICAL();
    unsigned kind=p25_kind;
    if(target)*target=p25_target;
    taskEXIT_CRITICAL();
    return kind;
}
#endif
#if CONFIG_SIGNAL_QUALITY
/* DM32 Plus signal quality: the channel receive's IMBE FEC corrections over the last second (radio/sigq.h) */
static sigq_t p25_q;
static uint32_t q_frames,q_bits,q_lost;
static void quality_track(void)
{
    if(voice.rx_fec_frames<q_frames || voice.rx_fec_lost<q_lost)q_frames=q_bits=q_lost=0;   /* a new receive */
    uint32_t frames=voice.rx_fec_frames-q_frames,bits=voice.rx_fec_bits-q_bits,lost=voice.rx_fec_lost-q_lost;
    if(!frames && !lost)return;
    q_frames=voice.rx_fec_frames;q_bits=voice.rx_fec_bits;q_lost=voice.rx_fec_lost;
    uint32_t now=hal_millis();
    taskENTER_CRITICAL();
    sigq_add(&p25_q,bits+lost*SIGQ_P25_LOST,(frames+lost)*SIGQ_P25_BITS,now);
    taskEXIT_CRITICAL();
}
int p25_lab_ber_permille(void)
{
    uint32_t now=hal_millis();
    taskENTER_CRITICAL();
    int ber=sigq_ber_permille(&p25_q,now);
    taskEXIT_CRITICAL();
    return ber;
}
#endif
#if CONFIG_CALL_LOG
/* DM32 Plus call history: P25 calls as calls (a new one after a 1.5 s gap or another talkgroup/NAC), each with
 * whether a clear-voice superframe (LDU2 ALGID 0x80) was seen; none in its first 0.5 s = encrypted */
#define CALL_GAP_MS 1500u
#define CALL_ENC_MS 500u
static p25_lab_call_t call_now;
static void call_track(uint32_t now)
{
    p25_lab_call_t *c=&call_now;
    bool fresh=c->n==0 || now-c->last_ms>CALL_GAP_MS || voice.active_group!=c->tg || voice.active_nac!=c->nac;
#if CONFIG_P25_EXTRAS
    /* another kind of call, or a unit-to-unit call to another unit: a new call */
    fresh=fresh || (voice.private_call!=0)!=c->priv || voice.active_target!=c->target;
#endif
    taskENTER_CRITICAL();
    if(fresh) {
        c->n++;c->start_ms=now;c->clear=false;c->src=0;
        c->tg=voice.active_group;c->nac=voice.active_nac;
#if CONFIG_P25_EXTRAS
        c->priv=voice.private_call!=0;c->target=voice.active_target;c->emergency=false;
#endif
    }
#if CONFIG_P25_EXTRAS
    if(voice.emergency)c->emergency=true;   /* any LDU1 of the call */
#endif
    if(voice.active_source)c->src=voice.active_source;   /* late entry: the source comes with LDU1 */
    if(voice.crypto_clear)c->clear=true;
    c->last_ms=now;
    c->encrypted=!c->clear && now-c->start_ms>=CALL_ENC_MS;
    taskEXIT_CRITICAL();
}
uint32_t p25_lab_call(p25_lab_call_t *out)
{
    taskENTER_CRITICAL();
    *out=call_now;
    taskEXIT_CRITICAL();
    return out->n;
}
#endif
/* The caller line as text alternates (core/ui/font.h: open 0x1D, next 0x1F, close 0x1E), longest first */
typedef struct { char *p; size_t left; bool cut; } line_t;   /* cut: something did not fit */
static void line_str(line_t *l,const char *s)
{ while(*s && l->left>1){*l->p++=*s++;l->left--;} if(*s)l->cut=true; *l->p='\0'; }
static void line_u32(line_t *l,uint32_t v)
{
    char d[10];unsigned n=0;
    do{d[n++]=(char)('0'+v%10u);v/=10u;}while(v && n<sizeof d);
    while(n && l->left>1){*l->p++=d[--n];l->left--;}
    if(n)l->cut=true;
    *l->p='\0';
}
static void line_hex3(line_t *l,uint16_t v)
{
    for(int shift=8;shift>=0;shift-=4) {
        if(l->left<=1){l->cut=true;break;}
        *l->p++="0123456789ABCDEF"[(v>>shift)&15u];l->left--;
    }
    *l->p='\0';
}
/* The NAC as the firmware writes it elsewhere: "$1C8/456" (CONFIG_NAC_DEC: hex, then decimal), else "1C8" */
static void line_nac(line_t *l,uint16_t v)
{
#if CONFIG_NAC_DEC
    line_str(l,"$");
    line_hex3(l,v);
    line_str(l,"/");line_u32(l,v);
#else
    line_hex3(l,v);
#endif
}
#if CONFIG_P25_EXTRAS
/* DM32 Plus: an emergency call on a channel that does not say its NAC (Any, or another NAC): Home's forms keep the NAC.
 * "EMERGENCY TG65535 ID16777214 $1C8" (wide) | "EMG TG65535 ID16777214 $1C8" (27 characters: the 8 px font) |
 * "EMG ID16777214 $1C8" (the caller and the NAC: 19 characters, any font). No BER here, the room is the NAC's. A
 * unit-to-unit emergency says "Private 77285>1234" in the first two and "P:77285" in the last */
static size_t emergency_nac_line(char *out,size_t cap,uint16_t tg,uint32_t src,uint32_t target,bool unit,uint16_t heard)
{
    line_t l={out,cap,false};
    for(unsigned form=0;form<3u;form++) {
        line_str(&l,form?"\x1f":"\x1d");
        line_str(&l,form?"EMG":TR("EMERGENCY"));
        line_str(&l," ");
        if(form==2u && src) {
            line_str(&l,unit?"P:":"ID");line_u32(&l,src);	/* "P:" marks a unit-to-unit call, as on Home's other forms */
        } else if(unit) {
            line_str(&l,TR("Private"));line_str(&l," ");
            line_u32(&l,src);line_str(&l,">");line_u32(&l,target);
        } else {
            line_str(&l,"TG");line_u32(&l,tg);
            if(src){line_str(&l," ID");line_u32(&l,src);}
        }
        line_str(&l," ");
        line_nac(&l,heard);
    }
    line_str(&l,"\x1e");
    return (size_t)(l.p-out);
}
#endif
size_t p25_lab_p25_caller_line(char *out,size_t cap,bool pane)
{
    if(!out || !cap)return 0;
    out[0]='\0';
    taskENTER_CRITICAL();
    uint16_t tg=p25_tg,heard_nac=p25_nac;uint32_t src=p25_src;
#if CONFIG_DEV_TX && CONFIG_P25_CHANNELS
    uint16_t channel_nac=channel_rx_nac;
#endif
#if CONFIG_P25_EXTRAS
    uint32_t target=p25_target;unsigned kind=p25_kind;
#endif
    taskEXIT_CRITICAL();
#if CONFIG_P25_EXTRAS
    bool unit=(kind&P25_LAB_PRIVATE)!=0;
    if(!tg && !unit)return 0;
#else
    if(!tg)return 0;
#endif
#if CONFIG_SIGNAL_QUALITY
    int ber=pane?-1:p25_lab_ber_permille();
#endif
    /* DM32 Plus: the NAC the call came on stays in Home's short forms (" $1C8") when the channel does not already say
     * it: a channel on NAC Any, or a NAC other than the channel's. A channel with its own NAC keeps the room for BER */
    bool short_nac=false;
    if(!pane && heard_nac!=P25_NAC_ANY) {
#if CONFIG_DEV_TX && CONFIG_P25_CHANNELS
        short_nac=channel_nac==0xF7Eu || channel_nac==P25_NAC_ANY || channel_nac!=heard_nac;
#else
        short_nac=true;
#endif
    }
#if CONFIG_P25_EXTRAS
    if(short_nac && (kind&P25_LAB_EMERGENCY))return emergency_nac_line(out,cap,tg,src,target,unit,heard_nac);
#endif
    /* The forms to offer, widest first: 0 "P25 " wide, 1 tight with the NAC word, 2 TG/ID (the short NAC form: with
     * the NAC and BER), 3 TG/ID/NAC without BER, 4 the caller and the NAC ("ID16777214 $FFF/4095": 20 characters) */
    uint8_t ids[5];
    unsigned nforms=0;
    if(!pane)ids[nforms++]=0;
    ids[nforms++]=1;
    if(short_nac) {
        ids[nforms++]=2;
#if CONFIG_SIGNAL_QUALITY
        ids[nforms++]=3;
#endif
    }
    if(src && heard_nac!=P25_NAC_ANY && (short_nac || pane))ids[nforms++]=4;
    if(!short_nac)ids[nforms++]=2;
    line_t l={out,cap,false};
    /* Home: "P25 TG 101 ID 77285 NAC 659" | "TG101 ID77285 NAC659" | "TG101 ID77285";
     * a dual watch pane: "TG 101 ID 77285 NAC 659" | "TG101 ID77285".
     * DM32 Plus (CONFIG_NAC_DEC): the NAC as "$293/659" in each form that has one. On a channel that does not say
     * its NAC (short_nac) Home's forms, widest first: "P25 TG 101 ID 77285 NAC $293/659 BER 1.2%" | "TG101 ID77285
     * NAC$293/659 BER1.2%" | "TG101 ID77285 $293/659 BER1.2%" | "TG101 ID77285 $293/659" | "ID77285 $293/659".
     * The row takes the first that fits; the NAC "$hex/dec" is never cut: BER goes first, then the TG, then the
     * words "NAC"/"P25"/spaces. An emergency: "EMERGENCY TG ID $x/d" | "EMG TG ID $x/d" | "EMG ID $x/d"
     * DM32 Plus P25 extras: "EMERGENCY " leads every form (instead of "P25 "); a unit-to-unit call shows
     * "Private 77285 > 1234" | "Private 77285>1234" instead of the talkgroup and ID. When the whole does not fit cap
     * the longest forms are left out (they are the ones too wide for the screen anyway) */
    /* Home leaves out the widest forms one by one; a pane (48 bytes) first its "ID.. $NAC" form, then the widest,
     * then everything but the plain TG/ID form */
    for(unsigned att=0;att<(pane?4u:3u);att++) {
    unsigned first=pane?(att<2u?0u:att==2u?1u:nforms-1u):att;
    bool skip4=pane && att==1u;
    l=(line_t){out,cap,false};
    line_str(&l,"\x1d");
    unsigned shown=0;
    for(unsigned fi=first;fi<nforms;fi++) {
        unsigned alt=ids[fi];
        if(skip4 && alt==4u)continue;
        bool tight=alt>=(pane?2u:1u);
        if(shown++)line_str(&l,"\x1f");
#if CONFIG_P25_EXTRAS
        if(kind&P25_LAB_EMERGENCY){line_str(&l,TR("EMERGENCY"));line_str(&l," ");}
        else
#endif
        if(alt==0)line_str(&l,"P25 ");
        if(alt==4u) {
            /* the caller and the NAC; a unit-to-unit call keeps its mark: "P:77285 $293/659" (same width as "ID") */
#if CONFIG_P25_EXTRAS
            line_str(&l,unit?"P:":"ID");
#else
            line_str(&l,"ID");
#endif
            line_u32(&l,src);
        } else
#if CONFIG_P25_EXTRAS
        if(unit) {
            line_str(&l,TR("Private"));line_str(&l," ");
            line_u32(&l,src);line_str(&l,tight?">":" > ");line_u32(&l,target);
        } else
#endif
        {
            line_str(&l,tight?"TG":"TG ");line_u32(&l,tg);
            if(src){line_str(&l,tight?" ID":" ID ");line_u32(&l,src);}
        }
        if(alt==4u || (short_nac && alt>=2u)) {
            line_str(&l," ");
            line_nac(&l,heard_nac);
        } else if(heard_nac!=P25_NAC_ANY && alt<2u){
            line_str(&l,tight?" NAC":" NAC ");
            /* DM32 Plus: "$125" marks hex, then the decimal some factory radios use, in every form ("$125/293") */
            line_nac(&l,heard_nac);
        }
#if CONFIG_SIGNAL_QUALITY
        /* DM32 Plus signal quality on Home, every form: "... BER 1.2%" | "... BER1.2%" */
        if(ber>=0 && !(short_nac && alt>=3u)) {
            line_str(&l,tight?" BER":" BER ");line_u32(&l,(uint32_t)ber/10u);
            line_str(&l,".");line_u32(&l,(uint32_t)ber%10u);line_str(&l,"%");
        }
#endif
    }
    line_str(&l,"\x1e");
    if(!l.cut)break;
    }
    return (size_t)(l.p-out);
}
#endif
#if CONFIG_YSF
/* DM32 Plus YSF receive (docs/plus.md): the channel worker's air bits go to the YSF framer instead of P25's;
 * V/D mode 2 voice becomes DMR AMBE frames for the DMR vocoder (it streams and switches the speaker itself). */
#ifndef P25_LAB_RX_SHARED
static bool ysf_rx;
static ysf_framer_t ysf_fr;
#endif
static bool ysf_call, ysf_dn_open, ysf_vw_open;
static void sink(void *ctx,const int16_t pcm[160]);
static int start_audio(void);
static uint8_t ysf_dgid;
#if CONFIG_ALL_SEARCH
/* DDM32 Plus all digital search: the DG-ID of the last frame whose FICH held (native_lab.c reads it once YSF is
 * confirmed there) */
static uint8_t ysf_heard_dgid;
uint8_t p25_lab_ysf_dgid(void) { return ysf_heard_dgid; }
#endif
static uint32_t ysf_frames, ysf_voice, ysf_bad, ysf_other, ysf_unsupported, ysf_vw_erasures;
/* The call's callsigns from the V/D mode 2 data channel (FN 1 source, FN 0 destination), "" until heard; the
 * sequence counts changes so the radio task can redraw Home (p25_lab_ysf_caller) */
static char ysf_src[11], ysf_dst[11];
static volatile uint32_t ysf_caller_seq;
static void ysf_take_call(char dst[11], const uint8_t csd[10])
{
    char s[11];
    unsigned n=10;
    for(unsigned i=0;i<10u;i++)s[i]=(csd[i]>=0x20u && csd[i]<0x7Fu)?(char)csd[i]:' ';
    while(n && s[n-1]==' ')n--;
    s[n]='\0';
    if(strcmp(s,dst)!=0) {
        taskENTER_CRITICAL();memcpy(dst,s,sizeof s);ysf_caller_seq++;taskEXIT_CRITICAL();
    }
}
uint32_t p25_lab_ysf_caller(char src[11],char dst[11])
{
    taskENTER_CRITICAL();
    uint32_t seq=ysf_caller_seq;
    if(src)memcpy(src,ysf_call?ysf_src:"",ysf_call?sizeof ysf_src:1u);
    if(dst)memcpy(dst,ysf_call?ysf_dst:"",ysf_call?sizeof ysf_dst:1u);
    taskEXIT_CRITICAL();
    return seq;
}
int p25_lab_set_ysf(int on,uint8_t dgid)
{
    taskENTER_CRITICAL();
    int ok=state!=1;
    if(ok) { ysf_rx=on!=0;ysf_dgid=dgid; }
#ifdef P25_LAB_RX_SHARED
    if(ok && on)trunk_rx=false;   /* the YSF framer and the TSDU receiver share their storage */
#endif
    taskEXIT_CRITICAL();
    return ok;
}
#if CONFIG_YSF_TX
static char ysf_tx_call[11];    /* the callsign the next YSF transmission sends, "" = none (no transmission) */
int p25_lab_set_ysf_tx(const char *call)
{
    /* Read only when a transmission starts (ysf_live_run, after the PTT request that set it) */
    unsigned n=0;
    taskENTER_CRITICAL();
    while(call && n<10u && call[n]!='\0') { ysf_tx_call[n]=call[n];n++; }
    ysf_tx_call[n]='\0';
    taskEXIT_CRITICAL();
    return 1;
}
#endif
static void ysf_end_call(void)
{
    if(!ysf_call)return;
    if(ysf_dn_open) {
        const dmr_voice_sink_t *s=ambeVoiceSink();
        if(s->end)s->end(s->ctx);
        ysf_dn_open=false;
    }
    if(ysf_vw_open) {
        /* Let the P25 PCM path play out what it holds; the worker stops it when the channel is given back */
        taskENTER_CRITICAL();fifo.draining=1;taskEXIT_CRITICAL();
        ysf_vw_open=false;
    }
    taskENTER_CRITICAL();ysf_call=false;ysf_src[0]=ysf_dst[0]='\0';ysf_caller_seq++;taskEXIT_CRITICAL();
}
/* A lost or unusable record: the frame in hand has a hole, so drop it and hunt for the next sync (a voice channel
 * has no CRC: playing a frame with bits from later in the stream would sound as noise) */
static void ysf_resync(void)
{
    ysf_framer_init(&ysf_fr);
}
static void ysf_call_open(void)
{
    if(ysf_call)return;
    taskENTER_CRITICAL();ysf_call=true;ysf_src[0]=ysf_dst[0]='\0';ysf_caller_seq++;taskEXIT_CRITICAL();
}
/* V/D mode 2 ("DN"): five AMBE+2 frames for the DMR vocoder */
static void ysf_play_dn(void)
{
    const dmr_voice_sink_t *s=ambeVoiceSink();
    if(!ysf_dn_open) {
        if(s->begin)s->begin(NULL,s->ctx);
        ysf_dn_open=true;
    }
    for(unsigned j=0;j<YSF_VCH_PER_FRAME;j++) {
        uint8_t params[YSF_AMBE_BITS],a72[AMBE_FRAME_BYTES];
        ysf_vd2_voice(ysf_fr.frame,j,params);
        ambeFecEncode(params,a72);
        s->frame(a72,s->ctx);
    }
}
/* Voice FR ("VW"): P25 IMBE frames, decoded and gated as P25 receive does (voice.c), played on the P25 PCM path */
static void ysf_play_vw(const ysf_fich_t *f)
{
    unsigned n=ysf_vw_voice_count(f);
    if(!n)return;
    if(!ysf_vw_open) {
        dm32_imbe_init(&voice.codec);
        taskENTER_CRITICAL();fifo.head=fifo.tail=fifo.count=fifo.playing=fifo.draining=0;taskEXIT_CRITICAL();
        if(!audio_on && !start_audio())return;
        ysf_vw_open=true;
    }
    for(unsigned k=0;k<n;k++) {
        uint8_t cw[YSF_VW_IMBE_BYTES],u0e=0;
        uint16_t u[8];
        int16_t pcm[160]={0};
        int ok=ysf_vw_voice(ysf_fr.frame,f,k,cw);
        int corrected=ok?p25_imbe_decode(cw,u,&u0e):-1;
        ok=corrected>=0 && u0e<=3u && corrected<=10 && dm32_imbe_decode(&voice.codec,u,pcm);
        if(!ok) {
            memset(pcm,0,sizeof pcm);
            ysf_vw_erasures++;
            dm32_imbe_init(&voice.codec);
        }
        sink(NULL,pcm);
    }
}
static void ysf_frame(void)
{
    ysf_fich_t f;
    ysf_frames++;
    if(!ysf_fich_decode(ysf_fr.frame,&f)) { ysf_bad++;return; }
#if CONFIG_ALL_SEARCH
    ysf_heard_dgid=f.dgid;
#endif
#if CONFIG_P25_SCAN
    channel_synced();
#endif
    /* A voice call's header names its source and destination (CSD1): DN and VW alike, before any voice */
    if(f.fi==YSF_FI_HEADER && (f.dt==YSF_DT_VD2 || f.dt==YSF_DT_VOICE_FR) && (!ysf_dgid || f.dgid==ysf_dgid)) {
        uint8_t csd[YSF_CSD_BYTES];
        if(ysf_csd_get(ysf_fr.frame,YSF_CSD1,csd)) {
            ysf_call_open();
            ysf_take_call(ysf_src,csd+10);
            if(csd[0]!='*')ysf_take_call(ysf_dst,csd);  /* "**********": any radio, not a name */
        }
        ysf_other++;return;
    }
    if(f.fi==YSF_FI_TERMINATOR) { ysf_end_call();return; }
    /* Voice of the channel's DG-ID (0: any): V/D mode 2 (DN) and Voice FR (VW). V/D mode 1 and data: not yet */
    bool dn=f.dt==YSF_DT_VD2, vw=f.dt==YSF_DT_VOICE_FR;
    if(f.fi!=YSF_FI_COMMS || !(dn || vw) || (ysf_dgid && f.dgid!=ysf_dgid)) { ysf_other++;return; }
    ysf_call_open();
    if(dn && f.fn<=1u) {
        uint8_t csd[10];
        if(ysf_vd2_data(ysf_fr.frame,csd))ysf_take_call(f.fn==1u?ysf_src:ysf_dst,csd);
    }
    if(dn)ysf_play_dn(); else ysf_play_vw(&f);
    ysf_voice++;
    channel_voice_ms=hal_millis();channel_voice_seen=true;
}
static void ysf_consume(uint8_t kind,const uint8_t raw[36])
{
    uint8_t air[36];
    if(p25_phy_rx_air(raw,kind,air)) { ysf_unsupported++;ysf_resync();return; }
    for(unsigned i=0;i<288u;i++)
        if(ysf_framer_bit(&ysf_fr,(uint8_t)((air[i>>3]>>(7u-(i&7u)))&1u)))ysf_frame();
}
#endif
#if CONFIG_CALL_REPLAY
/* DM32 Plus call replay (docs/plus.md): the last P25 call's IMBE frames (the last REPLAY_FRAMES of it, 20 ms each,
 * packed 88 bits in 11 bytes) kept in RAM as they decode; Tools > Last call plays them again through the channel
 * receiver's own audio path while no live call is heard (a live call ends the replay at once). Receive only. */
enum { REPLAY_FRAMES=400, REPLAY_BYTES=11, REPLAY_GAP_MS=1500, REPLAY_TAKE_MS=2000 };
static uint8_t rp_buf[REPLAY_FRAMES][REPLAY_BYTES];
static uint16_t rp_head, rp_count, rp_pos, rp_tg, rp_nac;
static uint32_t rp_src, rp_last_ms, rp_request_ms, rp_voice_seen, rp_end_ms;
static volatile uint8_t rp_request, rp_playing;
static const uint8_t rp_width[8]={12,12,12,12,11,11,11,7};
static void sink(void *ctx,const int16_t pcm[160]);
static void rp_pack(const uint16_t u[8],uint8_t out[REPLAY_BYTES])
{
    uint32_t acc=0;unsigned bits=0,n=0;
    memset(out,0,REPLAY_BYTES);
    for(unsigned i=0;i<8u;i++) {
        acc=(acc<<rp_width[i])|(u[i]&((1u<<rp_width[i])-1u));bits+=rp_width[i];
        while(bits>=8u){bits-=8u;out[n++]=(uint8_t)(acc>>bits);}
    }
}
static void rp_unpack(const uint8_t in[REPLAY_BYTES],uint16_t u[8])
{
    uint32_t acc=0;unsigned bits=0,n=0;
    for(unsigned i=0;i<8u;i++) {
        while(bits<rp_width[i]){acc=(acc<<8)|in[n++];bits+=8u;}
        bits-=rp_width[i];u[i]=(uint16_t)((acc>>bits)&((1u<<rp_width[i])-1u));
    }
}
static void replay_tap(void *ctx,const uint16_t u[8])
{
    (void)ctx;
    if(rp_playing)return;
    uint32_t now=hal_millis();
    taskENTER_CRITICAL();
    if(!rp_count || now-rp_last_ms>REPLAY_GAP_MS ||
       (voice.have_group && (voice.active_group!=rp_tg || voice.active_source!=rp_src))) {
        rp_head=rp_count=0;     /* a new call: the old one is dropped */
        rp_tg=voice.active_group;rp_src=voice.active_source;rp_nac=voice.active_nac;
    }
    rp_pack(u,rp_buf[rp_head]);
    rp_head=(uint16_t)((rp_head+1u)%REPLAY_FRAMES);
    if(rp_count<REPLAY_FRAMES)rp_count++;
    rp_last_ms=now;
    taskEXIT_CRITICAL();
}
uint16_t p25_lab_replay_info(uint16_t *tg,uint32_t *src,uint16_t *heard_nac)
{
    taskENTER_CRITICAL();
    if(tg)*tg=rp_tg;
    if(src)*src=rp_src;
    if(heard_nac)*heard_nac=rp_nac;
    uint16_t n=rp_count;
    taskEXIT_CRITICAL();
    return n;
}
int p25_lab_replay(int start)
{
    if(!start) { rp_request=0;rp_playing=0;return 1; }
    if(!rp_count || rp_playing)return 0;
    rp_request_ms=hal_millis();rp_request=1;
    return 1;
}
int p25_lab_replaying(void) { return rp_playing || rp_request; }
/* The receive loop's turn: start a pending replay, or put the next frame into the audio FIFO while it has room */
static void replay_service(void)
{
    if(rp_request) {
        rp_request=0;
        if(!audio_on || (uint32_t)(hal_millis()-rp_request_ms)>REPLAY_TAKE_MS)return;
        rp_pos=0;rp_voice_seen=voice.rx_voice;
        dm32_imbe_init(&voice.codec);
        taskENTER_CRITICAL();fifo.draining=0;taskEXIT_CRITICAL();
        rp_playing=1;
    }
    if(!rp_playing)return;
    if(voice.rx_voice!=rp_voice_seen || rp_pos>=rp_count) {
        rp_playing=0;    /* a live call (it wins) or the end of the recording */
        rp_end_ms=hal_millis();
        dm32_imbe_init(&voice.codec);
        taskENTER_CRITICAL();fifo.draining=1;taskEXIT_CRITICAL();
        return;
    }
    if(fifo.count>=P25_AUDIO_BLOCKS-2u)return;
    uint16_t u[8];
    int16_t pcm[160];
    taskENTER_CRITICAL();
    rp_unpack(rp_buf[(rp_head+REPLAY_FRAMES-rp_count+rp_pos)%REPLAY_FRAMES],u);
    taskEXIT_CRITICAL();
    rp_pos++;
    if(!dm32_imbe_decode(&voice.codec,u,pcm)) { memset(pcm,0,sizeof pcm);dm32_imbe_init(&voice.codec); }
    sink(NULL,pcm);
}
#endif
#if CONFIG_XBAND
/* DDM32 Plus cross-band, a P25 session (p25_lab.h): the frames between the worker and the radio task wait in the lent
 * FIFO's first bytes, a ring of P25_LAB_XB_FRAMES; both sides touch it only inside a critical section, and only while
 * it is lent (xb_lent 2) */
typedef struct {
    uint8_t head,tail,send,eof;     /* send: the ring feeds the next transmission; eof: no more frames come */
    uint8_t frame[P25_LAB_XB_FRAMES][P25_LAB_XB_BYTES];
} xb_ring_t;
_Static_assert(sizeof(xb_ring_t)==P25_LAB_XB_RING_BYTES && sizeof(xb_ring_t)<sizeof(p25_audio_fifo) &&
               (int)P25_LAB_XB_BYTES==(int)REPLAY_BYTES,"the frame ring in the FIFO");
#define XBR ((xb_ring_t *)(void *)&fifo)
static volatile uint8_t xb_sent;
static uint16_t xb_nac,xb_tg;
static uint32_t xb_src;
static int xb_push(const uint8_t f[P25_LAB_XB_BYTES])
{
    xb_ring_t *r=XBR;
    if((uint8_t)(r->head-r->tail)>=P25_LAB_XB_FRAMES)return 0;
    memcpy(r->frame[r->head%P25_LAB_XB_FRAMES],f,P25_LAB_XB_BYTES);r->head++;
    return 1;
}
static int xb_pop(uint8_t f[P25_LAB_XB_BYTES])
{
    xb_ring_t *r=XBR;
    if(r->head==r->tail)return 0;
    memcpy(f,r->frame[r->tail%P25_LAB_XB_FRAMES],P25_LAB_XB_BYTES);r->tail++;
    return 1;
}
/* The worker's receive: every IMBE frame that would play (admitted, decoded) to the ring, the call it is from noted */
static void xb_tap(void *ctx,const uint16_t u[8])
{
    (void)ctx;
    uint8_t f[P25_LAB_XB_BYTES];
    rp_pack(u,f);
    taskENTER_CRITICAL();
    if(XB_P25 && !XBR->send && xb_push(f)) { xb_nac=voice.active_nac;xb_tg=voice.active_group;xb_src=voice.active_source; }
    taskEXIT_CRITICAL();
}
/* The worker's transmission: the next frame to send (1), none yet (0), or none will come (-1: the end, or given up) */
static int xb_tx_pop(uint16_t u[8])
{
    uint8_t f[P25_LAB_XB_BYTES];
    taskENTER_CRITICAL();
    int got=XB_P25 && XBR->send ? xb_pop(f) : 0;
    int end=!got && (!XB_P25 || !XBR->send || XBR->eof);
    taskEXIT_CRITICAL();
    if(got)rp_unpack(f,u);
    return got?1:end?-1:0;
}
int p25_lab_xb_take(uint8_t frame[P25_LAB_XB_BYTES])
{
    taskENTER_CRITICAL();
    int ok=XB_P25 && !XBR->send && xb_pop(frame);
    taskEXIT_CRITICAL();
    return ok;
}
int p25_lab_xb_put(const uint8_t frame[P25_LAB_XB_BYTES])
{
    taskENTER_CRITICAL();
    int ok=XB_P25 && XBR->send && !XBR->eof && xb_push(frame);
    taskEXIT_CRITICAL();
    return ok;
}
void p25_lab_xb_heard(uint16_t *heard_nac,uint16_t *tg,uint32_t *src)
{
    taskENTER_CRITICAL();*heard_nac=xb_nac;*tg=xb_tg;*src=xb_src;taskEXIT_CRITICAL();
}
void p25_lab_xb_send(int on)
{
    taskENTER_CRITICAL();
    if(XB_P25) {
        xb_ring_t *r=XBR;
        if(on>0) { r->head=r->tail=0;r->send=1;r->eof=0;xb_sent=0; }
        else if(on==0) r->eof=1;
        else { r->head=r->tail=0;r->send=r->eof=0; }   /* (-1) sent, or given up: the ring receives again */
    }
    taskEXIT_CRITICAL();
}
int p25_lab_xb_sent(void) { return xb_sent; }
void p25_lab_xb_call(uint16_t tx_nac,uint16_t tg,uint32_t id)
{
    taskENTER_CRITICAL();
    channel_call=(p25_call){.nac=tx_nac,.talkgroup=tg,.source_id=id};
    taskEXIT_CRITICAL();
}
#endif
int p25_lab_channel_receiving(void)
{
#if CONFIG_CALL_REPLAY
    if(state==1 && request_mode==5 && (rp_playing || (rp_end_ms && (uint32_t)(hal_millis()-rp_end_ms)<700u)))
        return 1;  /* the speaker opens for the replay and its last blocks */
#endif
    return state==1 && request_mode==5 && channel_voice_seen && (uint32_t)(hal_millis()-channel_voice_ms)<650u;
}
#if CONFIG_P25_SCAN
uint32_t p25_lab_channel_last_voice_ms(void)
{
    taskENTER_CRITICAL();
    uint32_t ms=channel_voice_seen?channel_voice_ms:0u;
    taskEXIT_CRITICAL();
    return ms;
}
uint32_t p25_lab_channel_last_sync_ms(void) { return channel_sync_ms; }
#endif
#endif
static volatile uint8_t live_released,live_capturing;
static volatile uint32_t live_key_frame;
static uint32_t live_mic_frames,live_peak,live_encode_max,live_encode_us,live_slow;
static uint32_t live_first_us,live_last_us,live_encoded;
static uint16_t live_fifo_high;
static uint32_t live_overflows;

int p25_lab_live_begin(uint32_t sid)
{
    taskENTER_CRITICAL();
    int ok=worker && state!=1 && !XB_LENT && board.native_mic_route && board.audio &&
           board.audio->start && board.audio->stop;
#if CONFIG_XBAND
    ok=ok && (!XB_P25 || XBR->send);  /* a P25 cross-band session transmits its ring only, never the microphone */
#endif
#if CONFIG_YSF_TX
    /* DM32 Plus: a YSF channel transmits YSF Voice FR, and only with a callsign (docs/ysf-nxdn-tx.md) */
    ok=ok && (!ysf_rx || ysf_tx_call[0]!='\0');
#elif CONFIG_YSF
    ok=ok && !ysf_rx;   /* never P25 on a YSF channel: this build has no YSF transmitter */
#endif
    if (ok) {
        live_sid=sid;
        live_released=live_capturing=stop_requested=0;
        request_mode=4;
        state=1;
    }
    taskEXIT_CRITICAL();
    if (ok)xTaskNotifyGive(worker);
    return ok;
}
void p25_lab_live_up(void) { live_released=1; }
void p25_lab_live_key(void)
{
    /* Native RF callback runs with interrupts excluded; tag the first PCM
     * block containing the switch, regardless of worker/FIFO scheduling. */
    if(live_capturing)live_key_frame=live_mic_frames;
}

int p25_lab_channel_rx(uint32_t hz)
{
    taskENTER_CRITICAL();
    int ok=worker && state!=1 && !XB_LENT;
    if (ok) {
        frequency=hz;
#if CONFIG_P25_CHANNELS
        nac=channel_rx_nac==0xF7Eu?P25_NAC_ANY:channel_rx_nac;
        group=channel_selective?channel_call.talkgroup:0;channel_voice_seen=false;
#else
        nac=0x293;group=1;
#endif
        duration_ms=0;
        request_mode=5;play_requested=!XB_P25;delay_seconds=0;stop_requested=0;   /* (a P25 cross-band session: silent) */
        state=1;
    }
    taskEXIT_CRITICAL();
    if (ok)xTaskNotifyGive(worker);
    return ok;
}

static void mic_block(const int16_t *mic,int16_t *spk,void *ctx)
{
    (void)ctx;
    memset(spk,0,320);
    taskENTER_CRITICAL();
    if (live_capturing && !live_released) {
        if (!p25_audio_fifo_push(&fifo,mic)) stop_requested=1;
        else {
            live_last_us=hal_micros();
            if (!live_mic_frames++) live_first_us=live_last_us;
        }
    }
    taskEXIT_CRITICAL();
}

static unsigned queued_symbols(void)
{
    unsigned symbols=0;
    for (unsigned i=0;i<voice.count;i++)
        symbols+=voice.queue[(voice.tail+i)%P25_VOICE_QUEUE].bytes*4u;
    return symbols-voice.packet_symbol;
}

#if CONFIG_YSF_TX
/* DM32 Plus YSF Voice FR transmission (docs/ysf-nxdn-tx.md), the P25 live path's twin: the same microphone FIFO,
 * IMBE encoder (the P25 codec context, idle while we send) and native modem blocks, gates and drain; only the
 * framing differs (ysf_tx.h). The frame being sent lives in the YSF receive framer's buffer (no receive runs now).
 * Air: 3 blocks of the 0x77 preamble (as P25), the header, the sub-header, voice frames, the terminator; the block
 * the terminator ends in is filled up with the preamble pattern, then one more preamble block keeps the PA on through
 * the hardware pipeline (sealed, as P25's last block). */
static void ysf_fill_preamble(uint8_t raw[36],unsigned from)
{
    for(unsigned i=from;i<288u;i++) {
        uint8_t m=(uint8_t)(0x80u>>(i&7u));
        raw[i>>3]=(0x77u&m)?(uint8_t)(raw[i>>3]|m):(uint8_t)(raw[i>>3]&~m);
    }
}
static int ysf_live_run(void)
{
    int16_t pcm[160];
    uint16_t u[8];
    uint8_t raw[36],queued=0;
    unsigned lead=3,fill=0;
    bool kicked=false,sealed=false;
    uint32_t sid=live_sid;
    ysf_tx_t tx;
    p25_mic_condition conditioner;  /* the microphone conditioning P25's live_run applies (Classic r8) */
    p25_mic_condition_init(&conditioner);
    live_mic_frames=live_peak=live_encode_max=live_encode_us=live_slow=live_encoded=0;
    live_first_us=live_last_us=live_fifo_high=live_overflows=0;
    live_key_frame=UINT32_MAX;
    native_state=NT_ARMED;
    native_reason=NT_OK;
    p25_audio_fifo_reset(&fifo);
    dm32_imbe_init(&voice.codec);
    ysf_tx_begin(&tx,ysf_fr.frame,ysf_tx_call,ysf_dgid);
    bool ok=ysf_tx_call[0]!='\0';
    if (ok) {
        board.native_mic_route(true);
        live_capturing=1;
        ok=board.audio->start(mic_block,NULL);
    }
    uint32_t last_input=hal_millis();
    while (ok && !stop_requested) {
        if (!native_lab_live_poll(sid,&native_state,&native_reason,&queued) || native_state==NT_FAULT) { ok=false;break; }
        if (native_state==NT_STOPPED) { ok=sealed && native_reason==NT_TX_COMPLETE;break; }
        bool closing=live_released || !input_ptt_down();
        if (closing && live_capturing) {
            live_capturing=0;
            board.audio->stop();
        }
        while (!sealed && queued<NT_TX_CAP) {
            if (lead) {
                memset(raw,0x77,sizeof raw);
                lead--;
            } else if (ysf_tx_done(&tx)) {
                /* The rest of the terminator's block, then one whole preamble block to seal the call */
                if (!fill) sealed=true;
                ysf_fill_preamble(raw,fill);
                fill=0;
            } else {
                fill+=ysf_tx_take(&tx,raw,fill,288u-fill);
                if (fill<288u && !ysf_tx_done(&tx)) break;  /* the next frame waits for voice */
                if (fill<288u) continue;                   /* the terminator ended inside this block */
                fill=0;
            }
            if (!native_lab_live_block(sid,raw,sealed)) { ok=false;break; }
            queued++;
        }
        if (!ok) break;
        if (!kicked && (queued>=7 || sealed)) {
            kicked=native_lab_live_go(sid)!=0;
            if (!kicked) { ok=false;break; }
        }
        if (ysf_tx_need_voice(&tx)) {
            taskENTER_CRITICAL();
            int have=p25_audio_fifo_take(&fifo,pcm);
            taskEXIT_CRITICAL();
            if (have) {
                last_input=hal_millis();
                for (unsigned i=0;i<160;i++) {
                    int sample=pcm[i];
                    unsigned magnitude=(unsigned)(sample<0?-sample:sample);
                    if (magnitude>live_peak) live_peak=magnitude;
                }
                p25_mic_condition_block(&conditioner,pcm,live_encoded==live_key_frame);
            } else if (closing && !tx.imbe) {
                break;                      /* an empty PTT: nothing was keyed (the preamble alone never starts) */
            } else if (closing && !tx.closing) {
                ysf_tx_close(&tx);          /* a voice frame not begun becomes the terminator */
                continue;
            } else if (closing) {
                memset(pcm,0,sizeof pcm);   /* pad the frame begun with encoded silence */
                have=1;
            }
            if (have) {
                uint32_t at=hal_micros();
                ok=dm32_imbe_encode(&voice.codec,pcm,u)!=0;
                if (ok) ysf_tx_voice(&tx,u);
                uint32_t elapsed=hal_micros()-at;
                live_encode_us+=elapsed;
                live_encoded++;
                if (elapsed>live_encode_max) live_encode_max=elapsed;
                if (elapsed>20000u) live_slow++;
                /* Give the idle watchdog time to run even while catching up. */
                vTaskDelay(1);
                continue;
            }
        }
        if (!closing && hal_millis()-last_input>300u) { ok=false;break; }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    live_capturing=0;
    native_lab_voice_stop(sid);
    board.audio->stop();
    board.native_mic_route(false);
    /* As P25's live_run (Classic r13): the radio task owns cleanup; do not abandon its reservation on a delay */
    while (!native_lab_voice_release(sid))
        vTaskDelay(pdMS_TO_TICKS(2));
    live_fifo_high=fifo.high_water;live_overflows=fifo.overflows;
    ysf_framer_init(&ysf_fr);   /* the transmit frame was in the receive framer's buffer */
    return ok && !stop_requested && !live_overflows;
}
#endif

static int live_run(void)
{
#if CONFIG_YSF_TX
    if (ysf_rx) return ysf_live_run();
#endif
#if CONFIG_P25_CHANNELS
    const p25_call call=channel_call;
#else
    const p25_call call={0x293,1,1,0,0,0};
#endif
    int16_t pcm[160];
    p25_mic_condition conditioner;
    p25_mic_condition_init(&conditioner);
    uint8_t raw[36],queued=0;
    unsigned lead=3;
    bool kicked=false,sealed=false,closed=false;
    bool ok=p25_voice_tx_begin(&voice,&call)!=0;
    uint32_t sid=live_sid;
#if CONFIG_XBAND
    /* DDM32 Plus cross-band: the frames of an over heard, from the ring, in place of the microphone (p25_lab_xb_send) */
    const bool xb=XB_P25;
    uint16_t xu[8];
#else
    const bool xb=false;
#endif
    live_mic_frames=live_peak=live_encode_max=live_encode_us=live_slow=live_encoded=0;
    live_first_us=live_last_us=live_fifo_high=live_overflows=0;
    live_key_frame=UINT32_MAX;
    native_state=NT_ARMED;
    native_reason=NT_OK;
    if (!xb) p25_audio_fifo_reset(&fifo);
    if (ok && !xb) {
        board.native_mic_route(true);
        live_capturing=1;
        ok=board.audio->start(mic_block,NULL);
    }
    uint32_t last_input=hal_millis();
    while (ok && !stop_requested) {
        if (!native_lab_live_poll(sid,&native_state,&native_reason,&queued) || native_state==NT_FAULT) { ok=false;break; }
        if (native_state==NT_STOPPED) { ok=sealed && native_reason==NT_TX_COMPLETE;break; }
        bool closing=live_released || (!xb && !input_ptt_down());
        if (closing && live_capturing) {
            live_capturing=0;
            board.audio->stop();
        }
        while (!sealed && queued<NT_TX_CAP && (voice.started || closed)) {
            if (lead) {
                memset(raw,0x77,sizeof raw);
                lead--;
            } else if (closed && p25_voice_tx_done(&voice)) {
                /* Keep the PA through the final CAI unit and hardware pipeline. */
                memset(raw,0x77,sizeof raw);
                sealed=true;
            } else if (closed || queued_symbols()>=P25_PHY_SYMBOLS) {
                if (!p25_phy_tx_block(&voice,raw)) {
                    if (voice.fault) ok=false;
                    break;
                }
            } else break;
            if (!native_lab_live_block(sid,raw,sealed)) { ok=false;break; }
            queued++;
        }
        if (!ok) break;
        if (!kicked && (queued>=7 || sealed)) {
            kicked=native_lab_live_go(sid)!=0;
            if (!kicked) { ok=false;break; }
        }
        if (!closed && (voice.count<P25_VOICE_QUEUE || voice.voice_count<8)) {
            int have;
#if CONFIG_XBAND
            if (xb) {
                have=closing?-1:xb_tx_pop(xu);
                if (have<0) { closing=true;have=0; }    /* the over is out (or the session gave it up) */
                if (have) last_input=hal_millis();
            } else
#endif
            {
            taskENTER_CRITICAL();
            have=p25_audio_fifo_take(&fifo,pcm);
            taskEXIT_CRITICAL();
            }
            if (have && xb) {
                /* (stored parameters: no microphone level, no conditioning) */
            } else if (have) {
                last_input=hal_millis();
                for (unsigned i=0;i<160;i++) {
                    int sample=pcm[i];
                    unsigned magnitude=(unsigned)(sample<0?-sample:sample);
                    if (magnitude>live_peak) live_peak=magnitude;
                }
                p25_mic_condition_block(&conditioner,pcm,live_encoded==live_key_frame);
            } else if (closing) {
                /* Voice owns codec tail and LDU padding after the final PCM. */
                ok=p25_voice_tx_end(&voice)!=0;
                if (p25_voice_tx_done(&voice)) break; /* an empty PTT */
                closed=true;
                continue;
            }
            if (have) {
                uint32_t at=hal_micros();
#if CONFIG_XBAND
                if (xb) ok=p25_voice_tx_imbe(&voice,xu)!=0; else
#endif
                ok=p25_voice_tx_pcm(&voice,pcm)!=0;
                uint32_t elapsed=hal_micros()-at;
                live_encode_us+=elapsed;
                live_encoded++;
                if (elapsed>live_encode_max) live_encode_max=elapsed;
                if (elapsed>20000u) live_slow++;
                /* Give the idle watchdog time to run even while catching up. */
                vTaskDelay(1);
                continue;
            }
        }
        /* (cross-band: the radio task tops the ring up every poll; a second without a frame is a stall) */
        if (!closing && !closed && hal_millis()-last_input>(xb?1000u:300u)) { ok=false;break; }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    live_capturing=0;
    native_lab_voice_stop(sid);
    if (!xb) {
        board.audio->stop();
        board.native_mic_route(false);
    }
    /* The radio task owns cleanup; do not abandon its reservation on a delay. */
    while (!native_lab_voice_release(sid))
        vTaskDelay(pdMS_TO_TICKS(2));
    if (!xb) { live_fifo_high=fifo.high_water;live_overflows=fifo.overflows; }
    p25_voice_tx_abort(&voice);
#if CONFIG_XBAND
    if (xb) { p25_lab_xb_send(-1);xb_sent=1; }    /* the ring receives again; the radio task lets go of PTT */
#endif
    return ok && !stop_requested && !live_overflows;
}
#endif
int p25_lab_busy(void) { return state==1; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0]|p[1]<<8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
/* Empty RX polling may wait 5 ms: records arrive every 30 ms and the
 * independent 500 ms lease/fault timer still runs. Bench timing stays at 1 ms. */
static void relax(void) { vTaskDelay(pdMS_TO_TICKS(CONFIG_P25_BENCH?1:5)); }

/* Only the worker decodes; only the audio task consumes the fixed PCM FIFO. */
#if CONFIG_P25_NATIVE
static void audio_block(const int16_t *mic,int16_t *spk,void *ctx)
{
    (void)mic;(void)ctx;
    taskENTER_CRITICAL();
    callbacks++;
    int ok=p25_audio_fifo_pop(&fifo,spk);
    if(ok) {
        played++;
        if(sample_checks)for(unsigned i=0;i<160;i++)played_nonzero+=spk[i]!=0;
    }
    taskEXIT_CRITICAL();
#if CONFIG_BEEP
    /* Local feedback shares playback; never mix it into TX microphone PCM. */
    beep_mix(spk);
#endif
    /* Keep the digital route through silence and buffering. Toggling it inside
     * this callback empties the driver's queue and discards the current block. */
}
#endif
static void sink(void *ctx,const int16_t pcm[160])
{
    (void)ctx;
    pcm_blocks++;
    if(sample_checks)for(unsigned i=0;i<160;i++) {
        nonzero+=pcm[i]!=0;
        for(unsigned j=0;j<2;j++) {
            pcm_crc^=((uint16_t)pcm[i]>>(8*j))&255u;
            for(unsigned b=0;b<8;b++)pcm_crc=(pcm_crc>>1)^(0xedb88320u&(0u-(pcm_crc&1u)));
        }
    }
    if(audio_on) {
        taskENTER_CRITICAL();
        if(!p25_audio_fifo_push(&fifo,pcm))stop_requested=1;
        taskEXIT_CRITICAL();
    }
}
#if CONFIG_P25_NATIVE
static void reset_stream(void)
{
#if CONFIG_YSF
    if(ysf_rx)ysf_resync();   /* a gap in the records: the YSF frame in hand is broken too (only YSF owns it) */
#endif
    p25_voice_rx_reset(&voice);
    taskENTER_CRITICAL();if(!XB_P25)fifo.head=fifo.tail=fifo.count=fifo.playing=0;taskEXIT_CRITICAL();
    memset(&phy,0,sizeof phy);
}
#endif
static void consume(uint32_t seq,uint8_t kind,const uint8_t raw[36])
{
    uint32_t at=hal_micros();
    uint32_t prior_gaps=phy.gaps;
    uint32_t prior_frames=voice.rx_frames;
#if CONFIG_P25_CHANNELS
    uint32_t prior_voice=voice.rx_voice;
#endif
#if CONFIG_P25_TRUNK
    uint32_t prior_framed=voice.framer.complete_frames;    /* frames of the voice framer (TSDUs: trunk_tsdu) */
#endif
    if(p25_phy_rx_record(&phy,&voice,seq,kind,raw)) {
        gaps+=phy.gaps-prior_gaps;unsupported=phy.unsupported;
        if(unsupported)stop_requested=1;
        taskENTER_CRITICAL();if(!XB_P25)fifo.head=fifo.tail=fifo.count=fifo.playing=0;taskEXIT_CRITICAL();
    }
#if CONFIG_P25_CHANNELS
    if(voice.rx_voice!=prior_voice) { channel_voice_ms=hal_millis();channel_voice_seen=true; }
#if CONFIG_P25_TRUNK
    if(trunk_rx && voice.rx_frames!=prior_frames) {
        uint8_t duid=voice.framer.nid.duid;
        uint32_t now=hal_millis();
        taskENTER_CRITICAL();
        trunk_counts.frames+=voice.rx_frames-prior_frames;trunk_counts.last_ms=now;
        if(voice.framer.complete_frames!=prior_framed) {
            trunk_counts.nac=voice.framer.nid.nac;
            if(duid==P25_DUID_TDU || duid==P25_DUID_TDULC)trunk_counts.terms++;
        }
        taskEXIT_CRITICAL();
    }
#endif
#if CONFIG_P25_CALLER
#if CONFIG_P25_EXTRAS
    if(voice.have_group && (voice.active_group!=p25_tg || voice.active_source!=p25_src || voice.active_nac!=p25_nac ||
                            voice.active_target!=p25_target || voice_kind()!=p25_kind)) {
        taskENTER_CRITICAL();
        p25_target=voice.active_target;p25_kind=(uint8_t)voice_kind();
#else
    if(voice.have_group && (voice.active_group!=p25_tg || voice.active_source!=p25_src || voice.active_nac!=p25_nac)) {
        taskENTER_CRITICAL();
#endif
        p25_tg=voice.active_group;p25_src=voice.active_source;p25_nac=voice.active_nac;p25_caller_seq++;
        taskEXIT_CRITICAL();
    }
#if CONFIG_CALL_LOG
    if(voice.rx_frames!=prior_frames && voice.have_group)call_track(hal_millis());
#endif
#if CONFIG_SIGNAL_QUALITY
    quality_track();
#endif
#endif
#endif
    if(voice.rx_frames!=prior_frames) {
#if CONFIG_DEV_TX && CONFIG_P25_CHANNELS && CONFIG_P25_SCAN
        channel_synced();
#endif
#if CONFIG_P25_SEARCH
        taskENTER_CRITICAL();channel_frames++;channel_frame_ms=hal_millis();taskEXIT_CRITICAL();
#endif
        taskENTER_CRITICAL();
        if(!XB_P25)fifo.draining=voice.framer.nid.duid==P25_DUID_TDU || voice.framer.nid.duid==P25_DUID_TDULC;
        taskEXIT_CRITICAL();
    }
    uint32_t dt=hal_micros()-at;
    decode_us+=dt;if(dt>record_max_us)record_max_us=dt;
    records++;
}
#if CONFIG_P25_NATIVE
static int start_audio(void)
{
    if(!board.audio || !board.audio->start || !board.audio->stop || !board.audio->speaker_pcm)return 0;
    audio_on=1;
    if(board.audio->playback_pace)board.audio->playback_pace(pcm_paced!=0);
    board.audio->speaker_pcm(true);
    if(board.audio->start(audio_block,NULL))return 1;
    audio_on=0;board.audio->stop();board.audio->speaker_pcm(false);return 0;
}
#endif
static void stop_audio(void)
{
    if(!audio_on)return;
    board.audio->stop();board.audio->speaker_pcm(false);audio_on=0;
}
#if CONFIG_P25_NATIVE
static int wait_native(void)
{
    uint32_t end=hal_millis()+1000;
    do {
        if(!native_lab_voice_poll(session,&native_state,&native_reason))return 0;
        if(native_state==NT_RX)return 1;
        if(native_state==NT_STOPPED || native_state==NT_FAULT)return 0;
        relax();
    } while((int32_t)(hal_millis()-end)<0 && !stop_requested);
    return 0;
}
#endif
static int receive_run(void)
{
    records=pcm_blocks=nonzero=record_max_us=decode_us=callbacks=played=played_nonzero=0;
    session=gaps=unsupported=0;native_state=native_reason=0;pcm_crc=sample_checks?0xffffffffu:0;
    memset(&phy,0,sizeof phy);
    if(!XB_P25)p25_audio_fifo_reset(&fifo);   /* (a P25 cross-band session: the FIFO is lent; nothing plays) */
    p25_voice_init(&voice,nac,group,sink,NULL);voice.micros=sample_checks?hal_micros:NULL;
#if CONFIG_P25_CALLER
    /* DDM32 Plus multi-watch: a receive starts without the last call's talkgroup, source and kind (they were never
     * cleared: before its first link control a new call on another letter showed the old one's talkgroup) */
    taskENTER_CRITICAL();
    p25_tg=0;p25_src=0;p25_nac=P25_NAC_ANY;
#if CONFIG_P25_EXTRAS
    p25_target=0;p25_kind=0;
#endif
    taskEXIT_CRITICAL();
#endif
#if CONFIG_DEV_TX && CONFIG_P25_CHANNELS && CONFIG_P25_TRUNK
    /* The control receiver takes the receive's NAC filter (P25_NAC_ANY is the library's any NAC); no trellis
     * budget of its own: each block's CRC decides */
    if(trunk_rx && request_mode==5 && p25cai_control_receiver_init(&tsdu_rx,nac,196u)==0)
        p25_voice_set_tsdu(&voice,&tsdu_rx,trunk_tsdu,NULL);
#endif
#if CONFIG_DEV_TX && CONFIG_P25_CHANNELS && CONFIG_P25_EXTRAS
    /* DM32 Plus P25 extras: our unit ID on this channel; a unit-to-unit call for it plays on a talkgroup channel too */
    if(request_mode==5)voice.own_unit=channel_call.source_id;
#endif
#if CONFIG_DEV_TX && CONFIG_P25_CHANNELS && CONFIG_P25_RXGL
    /* DDM32 Plus: the channel's RX group list widens its talkgroup filter (none without one: Normal, search, trunk) */
    if(request_mode==5 && group){voice.rx_groups=channel_groups;voice.rx_group_count=channel_group_count;}
#endif
#if CONFIG_CALL_REPLAY
    voice.tap=request_mode==5 ? replay_tap : NULL;  /* P25 channel receive only (a YSF one never decodes IMBE here) */
#if CONFIG_XBAND
    if(request_mode==5 && XB_P25)voice.tap=xb_tap;  /* DDM32 Plus cross-band: the frames to the ring, not the replay */
#endif
#endif
    int ok=1;
#if CONFIG_P25_NATIVE
    /* Playback also borrows native RX ownership to exclude normal beeps/PTT and
     * radio retunes. Replayed bytes remain in RAM and never enter a TX path. */
    if(request_mode==3 || request_mode==5 || play_requested) {
        session=native_lab_voice_start(frequency,duration_ms);
        if(!session || !wait_native())ok=0;
#if CONFIG_YSF
        if(ysf_rx && request_mode==5) { ysf_framer_init(&ysf_fr);ysf_call=ysf_dn_open=ysf_vw_open=false; }
        else
#endif
        if(ok && play_requested && !start_audio())ok=0;
    }
#else
    if(request_mode==3 || play_requested)ok=0;
#endif
    uint32_t due=hal_millis()+delay_seconds*1000u;
#if CONFIG_P25_BENCH
    uint16_t index=0;
#endif
#if CONFIG_P25_NATIVE
    uint32_t last_tick=0,last_record_at=hal_millis(),last_valid_at=0;
    int silent_reset=0;
#endif
    while(ok && !stop_requested) {
#if CONFIG_P25_NATIVE
        if(session) {
            if(!native_lab_voice_poll(session,&native_state,&native_reason)) { ok=0;break; }
            if(native_state!=NT_RX) {
                ok=native_state==NT_STOPPED && (native_reason==NT_RX_DURATION || native_reason==NT_RX_LIMIT ||
                    (request_mode==5 && native_reason==NT_USER_STOP));
                break;
            }
            if(request_mode==2) { nt_record discard;while(native_lab_voice_read(session,&discard)) {} }
        }
#endif
#if CONFIG_CALL_REPLAY
        if(request_mode==5)replay_service();
#endif
        if((int32_t)(hal_millis()-due)<0) { relax();continue; }
        if(request_mode==2) {
#if CONFIG_P25_BENCH
            if(index==loaded)break;
            consume(index,replay[index].kind,replay[index].raw);index++;
            if(play_requested)due+=30; /* a hardware block's real arrival cadence */
#else
            ok=0;break;
#endif
        } else {
#if CONFIG_P25_NATIVE
            nt_record r;
            if(native_lab_voice_read(session,&r)) {
                uint32_t delta=r.time_us-last_tick;
                if(last_tick && (delta>45000u || delta<15000u)) { gaps++;reset_stream(); }
                last_tick=r.time_us;last_record_at=hal_millis();silent_reset=0;
                if(!records)last_valid_at=hal_millis();
                uint32_t prior_frames=voice.rx_frames;
#if CONFIG_YSF
                if(ysf_rx && request_mode==5) {
                    uint32_t prior_ysf=ysf_frames-ysf_bad;
                    ysf_consume((uint8_t)(r.rx_info&3u),r.raw);records++;
                    if(ysf_frames-ysf_bad!=prior_ysf)last_valid_at=hal_millis();
                } else
#endif
                consume(r.sequence,(uint8_t)(r.rx_info&3u),r.raw);
                if(voice.rx_frames!=prior_frames)last_valid_at=hal_millis();
                if (request_mode==5) vTaskDelay(1);
            } else {
                if(!silent_reset && hal_millis()-last_record_at>500) {
                    reset_stream();silent_reset=1;
                }
                relax();
            }
            /* Noise records cannot keep old PHY synchronization alive. Let
             * the normal channel owner rearm between complete frames. */
            if(request_mode==5 && records && !voice.framer.symbols &&
               (uint32_t)(hal_millis()-last_valid_at)>=500u
#if CONFIG_CALL_REPLAY
               && !rp_playing && !rp_request && (uint32_t)(hal_millis()-rp_end_ms)>=700u  /* a replay keeps the receiver until its audio has played */
#endif
               )break;
#else
            ok=0;
#endif
        }
    }
    if(request_mode==2 && audio_on && ok && !stop_requested) {
        taskENTER_CRITICAL();fifo.draining=1;taskEXIT_CRITICAL();
        uint32_t end=hal_millis()+2000;
        while((fifo.count || (board.audio->spk_pending && board.audio->spk_pending())) &&
              (int32_t)(hal_millis()-end)<0 && !stop_requested) {
#if CONFIG_P25_NATIVE
            if(session && (!native_lab_voice_poll(session,&native_state,&native_reason) || native_state!=NT_RX)) { ok=0;break; }
#endif
            relax();
        }
        if(fifo.count || (board.audio->spk_pending && board.audio->spk_pending()))ok=0;
    }
    stop_audio();
#if CONFIG_YSF
    if(ysf_rx && request_mode==5) { ysf_end_call();ambeVoiceStop(); }
#endif
#if CONFIG_P25_NATIVE
    if(session) {
        native_lab_voice_stop(session);
        /* Keep ownership until the radio task confirms hardware cleanup. */
        while(!native_lab_voice_release(session))relax();
    }
#endif
    if(sample_checks)pcm_crc^=0xffffffffu;
    return ok && (XB_P25 || !fifo.overflows) && !unsupported;
}
static void run(void *ctx)
{
    (void)ctx;
    for(;;) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        int ok;
#if CONFIG_P25_BENCH || CONFIG_DEV_TX
        if(request_mode==1) {
            p25_selftest_result r;
            p25_selftest_run(&voice,&r,hal_micros,relax);
            taskENTER_CRITICAL();result=r;taskEXIT_CRITICAL();ok=r.ok;
        } else
#endif
#if CONFIG_DEV_TX
        if(request_mode==4) ok=live_run(); else
#endif
        ok=receive_run();
        uint32_t free_bytes=(uint32_t)uxTaskGetStackHighWaterMark(NULL)*sizeof(StackType_t);
        taskENTER_CRITICAL();stack_free=free_bytes;state=ok?2:3;taskEXIT_CRITICAL();
    }
}
#ifndef P25_WORKER_WORDS /* DM32 Plus sets its own from the stack check (docs/plus.md, RAM) */
#if CONFIG_P25_BENCH || CONFIG_DEV_TX
#define P25_WORKER_WORDS 2048 /* synthetic encoder is the deepest path */
#else
#define P25_WORKER_WORDS 1024 /* RX: static bound 2368 B; >1.5 KiB margin */
#endif
#endif
#if CONFIG_XBAND
/* DDM32 Plus cross-band (radio/xband.h, docs/plus.md "Cross-band store and forward"): the worker's own memory lent as
 * plain bytes while it is idle; every start refuses meanwhile (XB_LENT), and nothing else reads it then (the caller
 * fields Home shows are copies outside voice). Given back zeroed: the state the worker finds at power on, which its
 * next receive or transmission initializes anyway */
uint8_t p25_lab_lend(uint8_t *seg[],size_t len[],bool p25)
{
    taskENTER_CRITICAL();
    int ok=!xb_lent && state!=1;
    if(ok) {
        if(p25)memset(&fifo,0,P25_LAB_XB_RING_BYTES);   /* an empty ring, receiving */
        xb_lent=p25?2:1;
        rp_request=rp_playing=0;rp_count=rp_head=0;     /* the last call's frames go */
    }
    taskEXIT_CRITICAL();
    if(!ok)return 0;
    uint8_t n=0;
    if(p25) {
        /* P25: the voice context stays the worker's, the ring stays at the FIFO's start */
        seg[n]=(uint8_t *)&fifo+P25_LAB_XB_RING_BYTES;len[n++]=sizeof fifo-P25_LAB_XB_RING_BYTES;
    } else {
        seg[n]=(uint8_t *)&voice;len[n++]=sizeof voice;
        seg[n]=(uint8_t *)&fifo;len[n++]=sizeof fifo;
    }
    seg[n]=&rp_buf[0][0];len[n++]=sizeof rp_buf;
    return n;
}
/* Given back cleared. A P25 session's may come back while the worker still runs its receive: it reads xb_lent under
 * the same lock before every touch of the FIFO or the ring, and finds an empty FIFO after */
void p25_lab_give_back(void)
{
    if(!XB_P25)memset(&voice,0,sizeof voice);
    taskENTER_CRITICAL();
    memset(&fifo,0,sizeof fifo);
    memset(rp_buf,0,sizeof rp_buf);rp_count=rp_head=rp_pos=0;
    xb_lent=0;
    taskEXIT_CRITICAL();
}
#endif
void p25_lab_start(void)
{
    if(xTaskCreate(run,"p25-lab",P25_WORKER_WORDS,NULL,3,&worker)!=pdPASS) { worker=NULL;state=4; }
}
/* Private 0x20: diagnostics, bounded RAM capture replay and RX audio.
 * Live TX can only start from the local PTT path. */
size_t p25_lab_command(const uint8_t *req,uint16_t len,uint8_t *out,size_t cap)
{
    if(!cap)return 0;
    out[0]=DL_ST_BAD_ARG;
    if(!req || !len)return 1;
#if CONFIG_P25_CHANNELS
    /* Saved channels and normal PTT own operation; diagnostics are read-only. */
    if(req[0]!=0 && req[0]!=7 && req[0]!=10 && req[0]!=11 && req[0]!=13) {
        out[0]=DL_ST_UNAVAILABLE;return 1;
    }
#endif
#if CONFIG_DEV_TX
    if (req[0]==13 && len==1) {
        out[0]=DL_ST_OK;
        dl_text_t t;dl_text_init(&t,(char *)out+1,cap-1);
        uint32_t tx[5];native_lab_live_summary(tx);
        taskENTER_CRITICAL();
        dl_text(&t,"live.calls=%u\nlive.reason=%u\nlive.tx_blocks=%u\nlive.pa_keys=%u\nlive.faults=%u\n",
            (unsigned)tx[0],(unsigned)tx[1],(unsigned)tx[2],(unsigned)tx[3],(unsigned)tx[4]);
        dl_text(&t,"live.mic_frames=%u\nlive.mic_span_us=%u\nlive.peak=%u\nlive.encode_max_us=%u\n"
            "live.encoded=%u\nlive.encode_us=%u\nlive.encode_slow=%u\nlive.fifo_high=%u\nlive.overflows=%u\n",
            (unsigned)live_mic_frames,(unsigned)(live_last_us-live_first_us),(unsigned)live_peak,
            (unsigned)live_encode_max,(unsigned)live_encoded,(unsigned)live_encode_us,(unsigned)live_slow,
            (unsigned)live_fifo_high,(unsigned)live_overflows);
        taskEXIT_CRITICAL();
        return t.len+1;
    }
#endif
    if(req[0]==11 && len==1) {
        out[0]=DL_ST_OK;dl_text_t t;dl_text_init(&t,(char *)out+1,cap-1);
        dl_text(&t,"resource.version=1\np25.bench=%u\np25.sample_checks=%u\np25.context_bytes=%u\np25.context_used_bytes=%u\n",
            (unsigned)CONFIG_P25_BENCH,(unsigned)sample_checks,(unsigned)sizeof voice.codec,(unsigned)dm32_imbe_state_bytes());
        dl_text(&t,"p25.empty_poll_ms=%u\n",CONFIG_P25_BENCH?1u:5u);
        p25_cpu_report(&t);return t.len+1;
    }
    if(req[0]==10 && len==1) {
        if(cap<65)return 1;
        if(!board.audio || !board.audio->playback_stats) { out[0]=DL_ST_UNAVAILABLE;return 1; }
        uint32_t w[16];board.audio->playback_stats(w);
        for(unsigned i=0;i<16;i++)for(unsigned j=0;j<4;j++)out[1+4*i+j]=(uint8_t)(w[i]>>(8*j));
        out[0]=DL_ST_OK;return 65;
    }
    if(req[0]==7 && len==1) {
        if(cap<129)return 1;
        uint32_t w[32];
        taskENTER_CRITICAL();
        uint32_t values[32]={1,request_mode,state,loaded,expected,play_requested,
            records,voice.rx_frames,voice.rx_muted,voice.rx_erasures,pcm_blocks,pcm_crc,nonzero,
            voice.fec_max_us,voice.imbe_max_us,record_max_us,decode_us,callbacks,played,played_nonzero,
            fifo.count,fifo.underruns,fifo.overflows,fifo.high_water,session,native_state,native_reason,
            gaps,unsupported,stack_free,voice.framer.bad_nids,voice.framer.truncated_frames};
        memcpy(w,values,sizeof w);taskEXIT_CRITICAL();
        for(unsigned i=0;i<32;i++)for(unsigned j=0;j<4;j++)out[1+4*i+j]=(uint8_t)(w[i]>>(8*j));
        out[0]=DL_ST_OK;return 129;
    }
    if(req[0]==6 && len==1) { stop_requested=1;out[0]=DL_ST_OK;return 1; }
    if(req[0]==0 && len==1) {
        uint8_t s;uint32_t free;
        taskENTER_CRITICAL();s=state;free=stack_free;taskEXIT_CRITICAL();
        out[0]=DL_ST_OK;dl_text_t t;dl_text_init(&t,(char *)out+1,cap-1);
        dl_text(&t,"p25.lab=%u\np25.state=%u\np25.hardware_verified=0\np25.rf_mode_available=0\n",(unsigned)CONFIG_P25_BENCH,s);
#if CONFIG_P25_BENCH || CONFIG_DEV_TX
        p25_selftest_result r;
        taskENTER_CRITICAL();r=result;taskEXIT_CRITICAL();
        dl_text(&t,"p25.ok=%u\np25.air_bytes=%u\np25.pcm_blocks=%u\np25.pcm_crc=%08x\n",
            (unsigned)r.ok,(unsigned)r.air_bytes,(unsigned)r.pcm_blocks,(unsigned)r.pcm_crc);
        dl_text(&t,"p25.nonzero=%u\np25.tx_block_max_us=%u\np25.rx_unit_max_us=%u\n",
            (unsigned)r.nonzero_samples,(unsigned)r.tx_block_max_us,(unsigned)r.rx_unit_max_us);
        dl_text(&t,"p25.tx_first_us=%u\np25.tx_total_us=%u\np25.tx_over_20ms=%u\n",
            (unsigned)r.tx_first_us,(unsigned)r.tx_total_us,(unsigned)r.tx_over_20ms);
#endif
        dl_text(&t,"p25.session_bytes=%u\np25.stack_free=%u\n",(unsigned)sizeof voice,(unsigned)free);
        return t.len+1;
    }
#if !CONFIG_P25_BENCH
    if((req[0]>=(CONFIG_DEV_TX?2:1) && req[0]<=4) || req[0]==9 || req[0]==12) { out[0]=DL_ST_UNAVAILABLE;return 1; }
#endif
    taskENTER_CRITICAL();
    int busy=state==1 || XB_ANY;
#if CONFIG_P25_NATIVE
    busy=busy || native_lab_reserved();
#endif
    if(busy || !worker) {
        out[0]=worker?DL_ST_BUSY:DL_ST_UNAVAILABLE;taskEXIT_CRITICAL();return 1;
    }
    int start=0;
    if(0) { }
#if CONFIG_P25_BENCH || CONFIG_DEV_TX
    else if(req[0]==1 && len==1) { request_mode=1;start=1; }
#endif
#if CONFIG_P25_BENCH
    else if(req[0]==12 && len==2 && req[1]<=1) { sample_checks=req[1];out[0]=DL_ST_OK; }
    else if(req[0]==9 && len==2 && req[1]<=1) { pcm_paced=req[1];out[0]=DL_ST_OK; }
    else if(req[0]==2 && len==3 && u16(req+1)>=1 && u16(req+1)<=P25_REPLAY_RECORDS) {
        expected=u16(req+1);loaded=0;out[0]=DL_ST_OK;
    } else if(req[0]==3 && len==40 && loaded<expected && u16(req+1)==loaded && req[3]<=2) {
        replay[loaded].kind=req[3];memcpy(replay[loaded].raw,req+4,36);loaded++;out[0]=DL_ST_OK;
    }
    else if(req[0]==4 && len==3 && loaded && loaded==expected && req[1]<=1 && req[2]<=15) {
        request_mode=2;play_requested=req[1];delay_seconds=req[2];frequency=145500000;
        duration_ms=60000;nac=0x293;group=0;start=1;
    }
#endif
    else if((req[0]==5 || req[0]==8) && len==11 && u32(req+1) &&
              (req[0]==5?(u16(req+5)>=1000 && u16(req+5)<=60000):(u16(req+5)>=1 && u16(req+5)<=300)) && u16(req+7)<=0xfff) {
#if CONFIG_P25_NATIVE
        request_mode=3;play_requested=1;delay_seconds=0;frequency=u32(req+1);
        duration_ms=(uint32_t)u16(req+5)*(req[0]==8?1000u:1u);nac=u16(req+7);group=u16(req+9);start=1;
#else
        out[0]=DL_ST_UNAVAILABLE;
#endif
    }
    if(start) { state=1;stop_requested=0;out[0]=DL_ST_OK; }
    taskEXIT_CRITICAL();
    if(start)xTaskNotifyGive(worker);
    return 1;
}
