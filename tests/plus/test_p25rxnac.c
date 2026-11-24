/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus: the NAC a P25 call was received on stays on Home's caller line while the call lasts (CONFIG_P25_CALLER,
 * CONFIG_NAC_DEC, CONFIG_SIGNAL_QUALITY, CONFIG_LANG_TR). On a channel with NAC Any the line's short forms carry it
 * as " $1C8" (the channel does not say it), after TG and ID and before BER; one more last form (TG, ID and NAC without
 * BER) is there for the worst case. A channel with its own NAC keeps the old forms (no " $293": the channel says it).
 *
 * Built on tests/plus/test_p25x_worker.c (the real p25_lab.c worker over a synthetic air stream, its mocks and call
 * builder); its own cases are not run here. Checked: the NAC from the NID of a call on an Any channel for $1C8, $659
 * and $F7E, with TG 65535 and a 8 digit ID as well as short ones; the form Home's hint row takes in each of the two
 * small fonts of the 220 px safe width (8 px: 27 characters, 9 px: 24; core/ui/gfx.c's width is characters times the
 * font's width) is at most 220 px and names the NAC; English and Turkish; the dual watch pane form is unchanged; a
 * fixed-NAC channel is unchanged; nothing is left after a new receive. */
#pragma clang diagnostic ignored "-Wunused-function"
#define main p25x_main_not_run
#include "test_p25x_worker.c"
#undef main

#define SAFE_W 220

/* The form ui_draw_hint takes: the first alternate that fits SAFE_W at cw px a character, else the last */
static void pick(const char *line,int cw,char *out)
{
    const char *p=line;
    CHECK(*p=='\x1d');
    p++;
    for(;;) {
        size_t n=0;
        while(*p && *p!='\x1f' && *p!='\x1e')out[n++]=*p++;
        out[n]=0;
        bool last=*p!='\x1f';
        if((int)n*cw<=SAFE_W || last)return;
        p++;
    }
}

/* A group call (service options svc) from src on nac over a channel on NAC Any, talkgroup Any; its BER ~1.2 % */
static void any_call(uint16_t nac,uint16_t tg,uint32_t src,uint8_t svc)
{
    uint8_t lc[9];
    lc_group(lc,svc,tg,src);
    build_call(nac,tg,lc,lc);
    clock_ms=depth=turn=0;callback=NULL;
    maximum_ms=rec_count*30u+3000u;
    CHECK(p25_lab_set_channel(0xF7E,0x293,0,0,1));
    CHECK(p25_lab_channel_rx(433500000u));
    if(!setjmp(finished))worker_fn(NULL);
    CHECK(state==2 && voice.rx_frames==6u);
    memset(&p25_q,0,sizeof p25_q);
    sigq_add(&p25_q,16,1370,hal_millis());
}

static void show(const char *what,const char *line)
{
    printf("  %s: \"",what);
    for(const char *p=line;*p;p++)printf((unsigned char)*p<0x20?"|":"%c",*p);
    puts("\"");
}

static void any_case(uint16_t nac,uint16_t tg,uint32_t src)
{
    unsigned before=failures;
    any_call(nac,tg,src,0);
    CHECK(p25_lab_p25_caller_line(home,sizeof home,false)!=0);
    CHECK(p25_lab_p25_caller_line(pane,sizeof pane,true)!=0);
    int ber=p25_lab_ber_permille();
    CHECK(ber>=0);
    char a1[64],a2[64],a3[64],tail[200];
    snprintf(a1,sizeof a1,"TG%u ID%u NAC$%03X/%u BER%d.%d%%",tg,(unsigned)src,nac,nac,ber/10,ber%10);
    snprintf(a2,sizeof a2,"TG%u ID%u $%03X BER%d.%d%%",tg,(unsigned)src,nac,ber/10,ber%10);
    snprintf(a3,sizeof a3,"TG%u ID%u $%03X",tg,(unsigned)src,nac);
    snprintf(tail,sizeof tail,"%s\x1f%s\x1f%s\x1e",a1,a2,a3);
    /* the widest form (with "P25 ") may be left out when the 112 bytes do not hold the whole; the rest are there */
    size_t tl=strlen(tail);
    CHECK(strlen(home)>tl && !strcmp(home+strlen(home)-tl,tail) && (home[strlen(home)-tl-1]=='\x1d' || home[strlen(home)-tl-1]=='\x1f'));
    if(failures!=before)show("got",home),show("want-tail",tail);
    CHECK(strlen(home)<sizeof home-1u);
    /* the pane form (no BER, NAC on the channel's own word only): as before */
    char want_pane[64],no_short[64];
    snprintf(want_pane,sizeof want_pane,"TG%u ID%u" "\x1e",tg,(unsigned)src);
    snprintf(no_short,sizeof no_short,"ID%u $",(unsigned)src);
    CHECK(strlen(pane)>strlen(want_pane) && !strcmp(pane+strlen(pane)-strlen(want_pane),want_pane) && !strstr(pane,no_short));
    char shown[96],hex[8];
    snprintf(hex,sizeof hex,"$%03X",nac);
    static const int cells[2]={8,9};
    for(unsigned i=0;i<2u;i++) {
        for(unsigned tr_on=0;tr_on<2u;tr_on++) {
            tr_turkish=(int)tr_on;
            CHECK(p25_lab_p25_caller_line(home,sizeof home,false)!=0);
            pick(home,cells[i],shown);
            if((int)strlen(shown)*cells[i]>SAFE_W || !strstr(shown,hex)) {
                failures++;printf("FAIL %s px: \"%s\" is %d px or has no NAC\n",cells[i]==8?"8":"9",shown,(int)strlen(shown)*cells[i]);
            }
        }
    }
    tr_turkish=0;
    CHECK(p25_lab_p25_caller_line(home,sizeof home,false)!=0);
    pick(home,9,shown);
    printf("%s TG %u ID %u on Any, call NAC $%03X: 9 px picks \"%s\"",failures==before?"ok  ":"FAIL",tg,(unsigned)src,nac,shown);
    pick(home,8,shown);
    printf(", 8 px \"%s\"\n",shown);
}

int main(void)
{
    p25_lab_start();beep_init(&audio);
    static const uint16_t nacs[]={0x1C8,0x659,0xF7E,0x000,0xFFF};
    for(unsigned i=0;i<sizeof nacs/sizeof nacs[0];i++) {
        any_case(nacs[i],65535,16777214u);	/* the worst case */
        any_case(nacs[i],101,77285);
    }
    any_case(0x1C8,1,1);
    any_case(0x1C8,65535,1234567u);

    /* A fixed-NAC channel: the channel says it, the short forms keep their room for BER (as before) */
    unsigned before=failures;
    uint8_t lc[9];
    lc_group(lc,0,101,77285);
    build_call(0x293,101,lc,lc);
    clock_ms=depth=turn=0;callback=NULL;maximum_ms=rec_count*30u+3000u;
    CHECK(p25_lab_set_channel(0x293,0x293,101,1234,1));
    CHECK(p25_lab_channel_rx(433500000u));
    if(!setjmp(finished))worker_fn(NULL);
    memset(&p25_q,0,sizeof p25_q);sigq_add(&p25_q,16,1370,hal_millis());
    CHECK(p25_lab_p25_caller_line(home,sizeof home,false)!=0);
    show("fixed channel",home);
    CHECK(!strstr(home,"ID77285 $293") && strstr(home,"NAC$293/659 BER") && strstr(home,"\x1f" "TG101 ID77285 BER"));
    printf("%s fixed-NAC channel: no short NAC form\n",failures==before?"ok  ":"FAIL");

    /* A new receive (another channel, here over noise): the last call's NAC is gone with the rest of the line */
    before=failures;
    memset(stream,0,sizeof stream);stream_bits=0;add_noise(600);make_records();
    clock_ms=depth=turn=0;callback=NULL;maximum_ms=rec_count*30u+3000u;
    CHECK(p25_lab_set_channel(0xF7E,0x293,0,0,1));
    CHECK(p25_lab_channel_rx(433500000u));
    if(!setjmp(finished))worker_fn(NULL);
    CHECK(p25_lab_p25_caller_line(home,sizeof home,false)==0 && p25_lab_p25_caller_line(pane,sizeof pane,true)==0);
    printf("%s a new receive: no caller line, no old NAC\n",failures==before?"ok  ":"FAIL");

    /* Emergency and unit-to-unit calls on Any, informational: widths of the forms Home takes */
    for(unsigned tr_on=0;tr_on<2u;tr_on++) {
        tr_turkish=(int)tr_on;
        any_call(0x1C8,65535,16777214u,0x80);
        CHECK(p25_lab_p25_caller_line(home,sizeof home,false)!=0);
        CHECK(strlen(home)<sizeof home-1u && home[strlen(home)-1]=='\x1e' && strstr(home,"$1C8"));
        char shown[96];
        pick(home,9,shown);
        printf("info %s emergency worst case at 9 px: \"%s\" (%d px)\n",tr_on?"TR":"EN",shown,(int)strlen(shown)*9);
    }
    tr_turkish=0;
    printf("P25 RX NAC: %u failures\n",failures);
    return failures?1:0;
}
