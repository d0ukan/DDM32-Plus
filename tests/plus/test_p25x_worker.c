/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus P25 extras end to end through the actual P25 channel worker (built with CONFIG_P25_EXTRAS=1,
 * CONFIG_P25_CALLER=1, CONFIG_CALL_LOG=1, CONFIG_NAC_DEC=1).
 *
 * As tests/plus/test_trunk_worker.c does, the real p25_lab.c is #included and its worker task body (receive_run,
 * request_mode 5) runs a channel receive. Only the edges are mocked: FreeRTOS and the clock, the native modem session
 * (one 36-byte RAM record every 30 ms) and the board's PCM audio. The PHY record mapping (src/p25/phy_stream.c), the
 * framer, FEC and voice receive (src/p25/p25.c, voice.c) and the host IMBE codec are the real ones.
 *
 * Each case is one call as the air carries it: HDU, LDU1, LDU2, LDU1, LDU2, TDU. The LDU1s come from the real
 * encoder, their link control rewritten by tests/plus/p25_lc_build.h (RS and Hamming coded as MMDVMHost does) for the
 * formats the encoder can't make. Checked per case: which voice frames the channel plays or mutes (its talkgroup
 * filter and unit ID), the caller as Home, a dual watch pane / multi-watch row show it, the kind query Home's
 * activity line uses, and the call history's entry. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include "p25_lab.c"
#include "p25_lc_build.h"

static unsigned failures;
#define CHECK(c) do { if(!(c)) { failures++; printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); } } while(0)

#if CONFIG_LANG_ANY
/* The UI's catalogs aren't linked here: English (the words are the catalogs' msgids, tr.po has them in Turkish) */
int tr_turkish;	/* tests/plus/test_p25rxnac.c: the two words the caller line translates, as tr.po has them */
const char *tr(const char *en)
{
    if(tr_turkish && !strcmp(en,"EMERGENCY"))return "AC\xC4\xB0L DURUM";
    if(tr_turkish && !strcmp(en,"Private"))return "\xC3\x96zel";
    return en;
}
#endif

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

static audio_block_cb_t callback;
static void *callback_ctx;
static uint32_t audio_at;
static bool audio_start(audio_block_cb_t cb,void *ctx) { callback=cb;callback_ctx=ctx;audio_at=clock_ms;return true; }
static void audio_stop(void) { callback=NULL; }
static void route(bool on) { (void)on; }
static const hal_audio_t audio={.start=audio_start,.stop=audio_stop,.speaker_pcm=route};
const board_t board={.audio=&audio};

void vTaskDelay(TickType_t ms)
{
    assert(!depth);
    clock_ms+=ms;assert(clock_ms<=maximum_ms);
    while(callback && clock_ms-audio_at>=20u) {
        int16_t mic[160]={0},spk[160];
        audio_at+=20;
        callback(mic,spk,callback_ctx);
    }
}
bool input_ptt_down(void) { return false; }
int native_lab_reserved(void) { return 0; }
int native_lab_live_poll(uint32_t sid,uint8_t *s,uint8_t *r,uint8_t *q)
{ (void)sid;(void)s;(void)r;(void)q;assert(0);return 0; }
int native_lab_live_block(uint32_t sid,const uint8_t block[36],int last)
{ (void)sid;(void)block;(void)last;assert(0);return 0; }
int native_lab_live_go(uint32_t sid) { (void)sid;assert(0);return 0; }
void native_lab_live_summary(uint32_t out[5]) { memset(out,0,5*sizeof *out); }

/* ---- Native modem session: one record every 30 ms, then empty (all-zero) records ---- */

#define MAX_RECORDS 80
static uint8_t rec_raw[MAX_RECORDS][36];
static unsigned rec_count,rec_next,starts,owned;
static uint32_t next_raw_at;
uint32_t native_lab_voice_start(uint32_t hz,uint32_t ms)
{ assert(hz==433500000u && !ms && !owned);owned=1;starts++;next_raw_at=0;rec_next=0;return starts; }
int native_lab_voice_poll(uint32_t sid,uint8_t *s,uint8_t *why)
{ assert(sid==starts && owned);*s=NT_RX;*why=NT_OK;return 1; }
int native_lab_voice_read(uint32_t sid,nt_record *r)
{
    assert(sid==starts && owned);
    if(clock_ms<next_raw_at)return 0;
    memset(r,0,sizeof *r);r->sequence=rec_next;r->time_us=next_raw_at*1000u;
    if(rec_next<rec_count)memcpy(r->raw,rec_raw[rec_next],36);
    rec_next++;next_raw_at+=30;return 1;
}
void native_lab_voice_stop(uint32_t sid) { assert(sid==starts && owned); }
int native_lab_voice_release(uint32_t sid) { assert(sid==starts && owned);owned=0;return 1; }

/* ---- Air stream ---- */

static uint32_t rng=0x0BADC0DEu;
static uint32_t rnd(void) { rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng; }
#define MAX_STREAM_BITS (MAX_RECORDS*288u)
static uint8_t stream[MAX_STREAM_BITS/8];
static unsigned stream_bits;
static void add_bit(unsigned b) { assert(stream_bits<MAX_STREAM_BITS);lcb_put(stream,stream_bits++,b); }
static void add_noise(unsigned dibits) { while(dibits--) { add_bit(rnd()&1u);add_bit(rnd()&1u); } }
static void add_bits(const uint8_t *p,unsigned bits) { for(unsigned i=0;i<bits;i++)add_bit(lcb_get(p,i)); }

static const uint16_t layout01[][2]={{0,24},{72,108},{24,48},{180,108}};
static void air_to_ram(const uint8_t air[36],uint8_t ram[36])
{
    unsigned a=0;
    memset(ram,0,36);
    for(unsigned s=0;s<4u;s++)for(unsigned i=0;i<layout01[s][1];i++,a++)lcb_put(ram,layout01[s][0]+i,lcb_get(air,a));
}
static void make_records(void)
{
    rec_count=(stream_bits+287u)/288u;assert(rec_count<=MAX_RECORDS);
    for(unsigned r=0;r<rec_count;r++) {
        uint8_t air[36]={0},back[36];
        for(unsigned i=0;i<288u;i++)if(r*288u+i<stream_bits)lcb_put(air,i,lcb_get(stream,r*288u+i));
        air_to_ram(air,rec_raw[r]);
        CHECK(!p25_phy_rx_air(rec_raw[r],0,back) && !memcmp(back,air,36));
    }
}

/* One call: HDU (talkgroup hdu_tg), LDU1 with lc1, LDU2, LDU1 with lc2, LDU2, TDU */
static void build_call(uint16_t nac,uint16_t hdu_tg,const uint8_t lc1[9],const uint8_t lc2[9])
{
    const p25_call call={.nac=nac,.talkgroup=hdu_tg,.source_id=1};
    uint8_t frame[P25_LDU_BYTES];
    uint16_t imbe[P25_IMBE_FRAMES][8];
    const uint8_t lsd[2]={0,0};
    memset(stream,0,sizeof stream);stream_bits=0;
    for(unsigned i=0;i<P25_IMBE_FRAMES;i++)for(unsigned j=0;j<8u;j++)
        imbe[i][j]=(uint16_t)(rnd()&((1u<<(j<4?12:j<7?11:7))-1u));
    add_noise(77);
    CHECK(!p25_encode_hdu(&call,frame));add_bits(frame,8u*P25_HDU_BYTES);
    for(unsigned s=0;s<2u;s++) {
        CHECK(!p25_encode_ldu(&call,P25_DUID_LDU1,imbe,lsd,frame));
        ldu1_set_lc(frame,s?lc2:lc1);
        add_bits(frame,8u*P25_LDU_BYTES);
        CHECK(!p25_encode_ldu(&call,P25_DUID_LDU2,imbe,lsd,frame));add_bits(frame,8u*P25_LDU_BYTES);
    }
    CHECK(!p25_encode_tdu(&call,frame));add_bits(frame,8u*P25_TDU_BYTES);
    add_noise(40);
    make_records();
}

/* The channel receive (nac, talkgroup filter, our unit ID) over the stream; frames played / muted */
static unsigned played_frames,muted_frames;
static void execute(uint16_t nac,uint16_t tg,uint32_t id)
{
    clock_ms=depth=turn=0;callback=NULL;
    maximum_ms=rec_count*30u+3000u;
    CHECK(p25_lab_set_channel(nac,nac,tg,id,1));	/* Selective: the talkgroup filters */
    CHECK(p25_lab_channel_rx(433500000u));
    if(!setjmp(finished))worker_fn(NULL);
    CHECK(state==2 && starts && !owned && !depth);
    CHECK(voice.rx_frames==6u && voice.own_unit==id);
    played_frames=voice.rx_voice+voice.rx_erasures;muted_frames=voice.rx_muted;
}

static char home[112],pane[48];
static void lines(void)
{
    CHECK(p25_lab_p25_caller_line(home,sizeof home,false)!=0);
    CHECK(p25_lab_p25_caller_line(pane,sizeof pane,true)!=0);
}
static bool same(const char *got,const char *want)
{
    if(strcmp(got,want)==0)return true;
    printf("  got  \"");for(const char *p=got;*p;p++)printf((unsigned char)*p<0x20?"\\x%02x":"%c",*p);puts("\"");
    printf("  want \"");for(const char *p=want;*p;p++)printf((unsigned char)*p<0x20?"\\x%02x":"%c",*p);puts("\"");
    return false;
}

static void report(const char *what,unsigned before)
{
    printf("%s %s (played %u, muted %u)\n",failures==before?"ok  ":"FAIL",what,played_frames,muted_frames);
}

int main(void)
{
    p25_lab_start();beep_init(&audio);
    uint8_t lc1[9];
    p25_lab_call_t c;
    uint32_t target;
    unsigned before,n0;

    /* An emergency group call on the channel's talkgroup: played, EMERGENCY everywhere */
    before=failures;
    lc_group(lc1,0x80,101,77285);
    build_call(0x293,101,lc1,lc1);
    execute(0x293,101,1234);
    CHECK(played_frames==36u && muted_frames==0);
    CHECK(p25_lab_p25_kind(&target)==P25_LAB_EMERGENCY && target==0);
    lines();
    CHECK(same(home,"\x1d" "EMERGENCY TG 101 ID 77285 NAC $293/659" "\x1f" "EMERGENCY TG101 ID77285 NAC$293/659"
                    "\x1f" "EMERGENCY TG101 ID77285" "\x1e"));
    /* both pane forms would take 64 of the pane's 48 bytes: the short one alone */
    CHECK(same(pane,"\x1d" "EMERGENCY TG101 ID77285" "\x1e"));
    n0=p25_lab_call(&c);
    CHECK(n0!=0 && c.emergency && !c.priv && c.tg==101 && c.src==77285 && c.nac==0x293 && c.target==0 && c.clear);
    report("emergency group call: played, \"EMERGENCY TG 101 ID 77285\", the call history marks it",before);

    /* The same talkgroup without the emergency bit: plain again (nothing left over) */
    before=failures;
    lc_group(lc1,0x00,101,77285);
    build_call(0x293,101,lc1,lc1);
    execute(0x293,101,1234);
    CHECK(played_frames==36u && muted_frames==0 && p25_lab_p25_kind(NULL)==0);
    lines();
    CHECK(same(home,"\x1d" "P25 TG 101 ID 77285 NAC $293/659" "\x1f" "TG101 ID77285 NAC$293/659" "\x1f" "TG101 ID77285" "\x1e"));
    CHECK(p25_lab_call(&c)==n0+1u && !c.emergency && !c.priv && c.tg==101);
    report("group call after it: played, caller line as before the extras",before);

    /* Unit to unit, to our unit ID on a talkgroup channel: played; "Private 77285 > 1234" */
    before=failures;
    lc_unit(lc1,0x00,1234,77285);
    build_call(0x293,0,lc1,lc1);
    execute(0x293,101,1234);
    CHECK(played_frames==36u && muted_frames==0);
    CHECK(p25_lab_p25_kind(&target)==P25_LAB_PRIVATE && target==1234);
    lines();
    CHECK(same(home,"\x1d" "P25 Private 77285 > 1234 NAC $293/659" "\x1f" "Private 77285>1234 NAC$293/659"
                    "\x1f" "Private 77285>1234" "\x1e"));
    /* 54 bytes with the wide form: the caller and the NAC, then the short one */
    CHECK(same(pane,"\x1d" "P:77285 $293/659" "\x1f" "Private 77285>1234" "\x1e"));
    char small[40];	/* the length returned is the text's */
    CHECK(p25_lab_p25_caller_line(small,sizeof small,true)==37u && same(small,"\x1d" "P:77285 $293/659" "\x1f" "Private 77285>1234" "\x1e"));
    char tiny[24];	/* only the plain form */
    CHECK(p25_lab_p25_caller_line(tiny,sizeof tiny,true)==20u && same(tiny,"\x1d" "Private 77285>1234" "\x1e"));
    CHECK(p25_lab_call(&c)==n0+2u && c.priv && c.target==1234 && c.tg==0 && c.src==77285 && !c.emergency);
    report("unit-to-unit call to our ID on a talkgroup channel: played, \"Private 77285 > 1234\"",before);

    /* ... to another unit on a talkgroup channel: shown, muted */
    before=failures;
    lc_unit(lc1,0x00,5555,77285);
    build_call(0x293,0,lc1,lc1);
    execute(0x293,101,1234);
    CHECK(played_frames==0 && muted_frames==36u);
    CHECK(p25_lab_p25_kind(&target)==P25_LAB_PRIVATE && target==5555);
    lines();
    CHECK(same(pane,"\x1d" "P:77285 $293/659" "\x1f" "Private 77285>5555" "\x1e"));
    /* a buffer the wide pane form fits: both forms */
    char wide[64];
    CHECK(p25_lab_p25_caller_line(wide,sizeof wide,true)!=0 &&
          same(wide,"\x1d" "Private 77285 > 5555 NAC $293/659" "\x1f" "Private 77285>5555" "\x1e"));
    CHECK(p25_lab_call(&c)==n0+3u && c.priv && c.target==5555);
    report("unit-to-unit call to another unit on a talkgroup channel: shown, muted",before);

    /* ... on a channel with no unit ID: muted too */
    before=failures;
    execute(0x293,101,0);
    CHECK(played_frames==0 && muted_frames==36u);
    report("unit-to-unit call on a talkgroup channel without a unit ID: muted",before);

    /* ... on an Any channel (talkgroup 0): every unit-to-unit call plays */
    before=failures;
    execute(0x293,0,0);
    CHECK(played_frames==36u && muted_frames==0 && p25_lab_p25_kind(NULL)==P25_LAB_PRIVATE);
    report("unit-to-unit call to another unit on an Any channel: played",before);

    /* An emergency unit-to-unit call to us; the dual watch pane's 48 bytes keep the shortest form, whole */
    before=failures;
    lc_unit(lc1,0x80,1234,77285);
    build_call(0x293,0,lc1,lc1);
    execute(0x293,101,1234);
    CHECK(played_frames==36u && p25_lab_p25_kind(&target)==(P25_LAB_EMERGENCY|P25_LAB_PRIVATE) && target==1234);
    lines();
    /* both NAC forms in each (DM32 Plus): Home's 112 bytes leave out the widest form (43 characters, wider than the
     * screen in every font anyway) */
    CHECK(same(home,"\x1d" "EMERGENCY Private 77285>1234 NAC$293/659" "\x1f" "EMERGENCY Private 77285>1234" "\x1e"));
    CHECK(same(pane,"\x1d" "EMERGENCY Private 77285>1234" "\x1e"));
    CHECK(p25_lab_call(&c)==n0+6u && c.priv && c.emergency && c.target==1234);	/* each receive above: one call */
    report("emergency unit-to-unit call: both marks; the pane keeps the form that fits",before);

    /* Longest IDs: the whole line always fits its buffer, the close mark included */
    before=failures;
    lc_unit(lc1,0x80,0xFFFFFF,9999999);
    build_call(0x293,0,lc1,lc1);
    execute(0x293,0,0);
    lines();
    CHECK(strlen(home)<sizeof home-1u && home[strlen(home)-1]=='\x1e' && strlen(pane)<sizeof pane && pane[strlen(pane)-1]=='\x1e');
    CHECK(same(pane,"\x1d" "EMERGENCY Private 9999999>16777215" "\x1e"));
    report("longest IDs: Home and pane lines whole",before);

    /* A group call on another talkgroup: shown, muted (unchanged) */
    before=failures;
    lc_group(lc1,0x00,202,77285);
    build_call(0x293,202,lc1,lc1);
    execute(0x293,101,1234);
    CHECK(played_frames==0 && muted_frames==36u && p25_lab_p25_kind(NULL)==0);
    lines();
    CHECK(same(home,"\x1d" "P25 TG 202 ID 77285 NAC $293/659" "\x1f" "TG202 ID77285 NAC$293/659" "\x1f" "TG202 ID77285" "\x1e"));
    report("group call on another talkgroup: shown, muted (as before)",before);

    /* A Motorola-style talker alias LC (LCF 0x15, MFID 0x90) in the second LDU1: that superframe is no longer muted;
     * the caller stays the group call's */
    before=failures;
    lc_group(lc1,0x00,101,77285);
    static const uint8_t alias[9]={0x15,0x90,0x00,0x01,0x02,0x03,0x10,0x41,0x42};
    build_call(0x293,101,lc1,alias);
    execute(0x293,101,1234);
    CHECK(played_frames==36u && muted_frames==0 && p25_lab_p25_kind(NULL)==0);
    lines();
    CHECK(same(home,"\x1d" "P25 TG 101 ID 77285 NAC $293/659" "\x1f" "TG101 ID77285 NAC$293/659" "\x1f" "TG101 ID77285" "\x1e"));
    report("vendor LC (talker alias header) in an LDU1: not muted, the caller kept",before);

    /* ... but a protected LC (P bit) is still treated as before: that superframe muted */
    before=failures;
    uint8_t prot[9];
    lc_group(prot,0x00,101,77285);prot[0]=0x80;
    build_call(0x293,101,lc1,prot);
    execute(0x293,101,1234);
    CHECK(played_frames==18u && muted_frames==18u);
    report("protected LC in an LDU1: that superframe muted (as before)",before);

    /* A receive starts without the last call's talkgroup (multi-watch: the old one showed on another letter before the
     * new call's first link control): over noise only, nothing is known, the kind is gone too */
    before=failures;
    lc_group(lc1,0x00,202,77285);
    build_call(0x293,202,lc1,lc1);
    execute(0x293,0,0);
    lines();
    CHECK(p25_lab_p25_caller(NULL,NULL)!=0);
    memset(stream,0,sizeof stream);stream_bits=0;add_noise(600);make_records();
    clock_ms=depth=turn=0;callback=NULL;maximum_ms=rec_count*30u+3000u;
    CHECK(p25_lab_set_channel(0x293,0x293,0,0,1));
    CHECK(p25_lab_channel_rx(433500000u));
    if(!setjmp(finished))worker_fn(NULL);
    {
        uint16_t tg=1;uint32_t src=1;
        p25_lab_p25_caller(&tg,&src);
        CHECK(tg==0 && src==0 && p25_lab_p25_kind(&target)==0 && target==0);
        CHECK(p25_lab_p25_caller_line(home,sizeof home,false)==0 && p25_lab_p25_caller_line(pane,sizeof pane,true)==0);
    }
    report("a new receive over noise: the last call's talkgroup, source and kind are gone",before);

    printf("P25 extras worker: %u failures\n",failures);
    return failures?1:0;
}
