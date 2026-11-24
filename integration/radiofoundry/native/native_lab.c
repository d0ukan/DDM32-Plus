/* SPDX-License-Identifier: MIT */
#include "native_lab.h"
#include "board.h"
#include "hal_time.h"
#include "p25/p25_lab.h"
#if CONFIG_FIRSTBOOT
#include "firstboot/firstboot.h"
#endif
#include "radio/beep.h"
#if CONFIG_DMR_EMERGENCY
#include "dmr/dmr_emergency.h"
#endif
#if CONFIG_SPECTRUM
#include "radio/spectrum.h"
#endif
#if CONFIG_P25_SEARCH
#include "radio/p25search.h"
#endif
#if CONFIG_DMR_SEARCH
#include "radio/dmrsearch.h"
#endif
#if CONFIG_P25_TRUNK
#include "radio/p25trunk.h"
#endif
#if CONFIG_DMR_TRUNK
#include "radio/dmrtrunk.h"
#endif
#if CONFIG_YSF_TX
#include "radio/radio_cwid.h"
#endif
#if CONFIG_QSO_LOG
#include "ui/qso_log.h"
#endif
#include "debuglink/debuglink_proto.h"
#include "FreeRTOS.h"
#include "task.h"
#if CONFIG_YSF_TX
/* DM32 Plus YSF transmit: our callsign from the radio name (Settings > My radio), as the CW beacon takes it, for the
 * worker's next transmission; radio_sm_tx_check already refused the PTT without one (TXDENY_NO_CALL). Not inlined:
 * its buffer stays off the radio task's deepest path through native_lab_request */
static __attribute__((noinline)) void ysf_tx_callsign(const radio_sm_t *sm)
{
    char call[11];
    if(!radio_cwid_callsign(sm->radio_name,call,sizeof call))call[0]='\0';
    p25_lab_set_ysf_tx(call);
}
#endif
static nt_test test;
static uint32_t pending_start, battery_at;
static uint32_t voice_owner;
#if CONFIG_DEV_TX
static bool live_enabled, live_rx_pending, live_tx_pending, live_restore_pending;
#if CONFIG_P25_CHANNELS
static int live_owner(void);
static bool channel_selected, channel_handoff;
/* DDM32 Plus: PTT pressed for the native transmitter (its own PTT branch below), not any sm->ptt: the radio core also
 * sets sm->ptt for its own keying (a tone burst, the emergency hot mic, VOX, cross-band) and refuses it on a P25
 * letter; that must never key P25 here, and a native transmission ends as soon as this or sm->ptt goes */
static bool native_ptt;
static uint8_t channel_denial;
static uint32_t channel_tx_ms;
static void channel_view(radio_sm_t *sm)
{
#if CONFIG_YSF
    static uint32_t caller_seen;
    uint32_t caller=p25_lab_ysf_caller(NULL,NULL);
    if(caller!=caller_seen) { caller_seen=caller;sm->serial++; }   /* Home redraws the caller */
#endif
#if CONFIG_P25_CALLER
    static uint32_t p25_seen;
    uint32_t p25=p25_lab_p25_caller(NULL,NULL);
    if(p25!=p25_seen) { p25_seen=p25;sm->serial++; }
#endif
    if(sm->ptt && !native_ptt && !live_owner()) return;  /* the radio core's own keying (or its refusal) shows */
    radio_state_t state=channel_denial && sm->ptt ? RS_TX_DENIED :
        live_owner() && (test.state==NT_ARMED || test.state==NT_TX || test.state==NT_DRAIN) ? RS_TX :
        p25_lab_channel_receiving() ? RS_RX : RS_IDLE;
    if(sm->view.state!=state || sm->view.tx_deny!=channel_denial){
        sm->view.state=state;sm->view.tx_deny=channel_denial;sm->serial++;
    }
}
static void restore_channel(radio_sm_t *sm);
static inline void __attribute__((unused)) give_back(radio_sm_t *sm, bool on, uint8_t stop_type);
static int channel_let_go(radio_sm_t *sm);
#if CONFIG_P25_SCAN
/* P25 scan (docs/plus.md): the scan finds a P25 channel's carrier on the FM path and holds there; the native
 * receiver then takes the channel. It gives the channel back when the call has ended (or no P25 voice came),
 * and the scan's wait/resume goes on as for FM. A channel that gave no voice is not taken again for a while,
 * so a foreign carrier (another NAC, analog traffic) can't start the receiver over and over. */
enum { SCAN_P25_END_MS=1500, SCAN_P25_NO_VOICE_MS=2500, SCAN_P25_COOLDOWN_MS=8000, SCAN_P25_SETTLE_MS=700 };
/* Zone scan (docs/plus.md "P25 in a zone scan"): frames of the channel's kind tell a call from a foreign carrier
 * long before voice does. No frame PROBE_MS after the take: not ours, given back; the channel is left alone for
 * RETRY_MS, or the whole cooldown when the take before it (less than AGAIN_MS ago) heard none either. Frames, then
 * none for GONE_MS and no voice: the call ended under us (a take on its tail), given back with no cooldown so the
 * reply is taken. Frames that go on without voice for us (another talkgroup, encrypted): the old limit and cooldown */
enum { SCAN_P25_PROBE_MS=1200, SCAN_P25_GONE_MS=600, SCAN_P25_RETRY_MS=2000, SCAN_P25_AGAIN_MS=10000 };
#if CONFIG_MULTI_WATCH
/* DM32 Plus multi-watch: dual watch's ring holds a P25 channel the same way; Home's ring estimate uses the limit */
#include "radio/radio_mwatch.h"
_Static_assert(RADIO_MW_P25_HOLD_MS==SCAN_P25_NO_VOICE_MS, "radio_mwatch.h: the P25 hold is native_lab.c's");
#endif
#if CONFIG_P25_RXGL
#include "p25/p25_rxgl.h"	/* DDM32 Plus: a P25 channel's RX group list */
#endif
static bool scan_owned, scan_release, scan_quiet, scan_carrier;
/* DDM32 Plus TX target fix: the take transmitted (PTT released since): no voice after it is the reply's wait, not a
 * foreign carrier, so it is given back after SCAN_P25_NO_VOICE_MS without a cooldown (before: 8 s deaf after our over) */
static bool scan_after_tx;
static uint8_t scan_stop_seen;
static uint32_t scan_taken_ms, scan_skip_until, scan_settle_until;
/* The take's last voice and last frame (0: none yet). Kept here: the worker starts over 0.5 s after the last valid
 * frame and then reports none, which made every call's give back a "no voice" one with the cooldown after it */
static uint32_t scan_voice_ms, scan_sync_ms;
static uint16_t scan_skip_pos;
/* The cooldown's key: the zone position, or for a VFO (DM32 Plus: a P25 VFO in dual watch or a multi-watch ring,
 * CONFIG_VFO_MODES) 0xFFF0 + its index, so a VFO and the channel at the zone position it left never share one */
static uint16_t scan_key(const radio_sm_t *sm)
{
    return CONFIG_VFO_MODES && sm->view.vfo ? (uint16_t)(0xFFF0u | sm->view.vfo_index) : sm->view.zone_pos;
}
uint16_t native_lab_scan_key(const radio_sm_t *sm)
{
    return scan_key(sm);
}
/* DDM32 Plus faster ring (CONFIG_MWATCH_FAST, docs/plus.md "The multi-watch ring"): a ring of 3..6 channels gives a P25
 * channel back by the zone scan's rules (frames tell a call from a foreign carrier; a channel in its cooldown is
 * passed over); dual watch's pair keeps its own */
static bool scan_by_frames(const radio_sm_t *sm)
{
#if CONFIG_MWATCH_FAST && CONFIG_MULTI_WATCH
    if(sm->scan.kind==SCAN_DUAL && sm->dual_n>2) return true;
#endif
    return sm->scan.kind==SCAN_ZONE;
}
/* Counters for the debug link (0x21 [12], read only; they wrap at 65536): zone scan stops on a P25 channel, those
 * with a carrier, those passed over in a cooldown; takes, takes that heard a frame, voice; give backs by cause (the
 * call ended, a tail, no frame, frames without voice for us); the last take's time to its first frame and voice */
static struct { uint16_t stops,carriers,cooled,takes,synced,voiced,ended,tails,quiet,foreign,sync_ms,voice_ms; } scan_n;
static bool scan_skipping(const radio_sm_t *sm)
{
    return scan_key(sm)==scan_skip_pos && (int32_t)(scan_skip_until-hal_millis())>0;
}
/* While a scan runs, the native receiver may take a P25 channel only where the scan holds on a signal */
static bool scan_allows(radio_sm_t *sm)
{
#if CONFIG_DUAL_WATCH
    /* DDM32 Plus: PTT on the selected letter of dual watch / the ring: the native transmitter takes it whatever the
     * ring does there (a quiet stop, the settle after a give back, a cooldown), as on a single channel */
    if(native_ptt && sm->scan.kind==SCAN_DUAL && !radio_sm_dual_away(sm,false,0)) return true;
#endif
    /* Just given back: the radio retunes the FM path and the scan reads the channel again before a retake,
     * so a carrier that is still there (the call goes on) is taken again and one that is gone lets it move on */
    if(scan_on(&sm->scan) && (int32_t)(scan_settle_until-hal_millis())>0) return false;
    /* A zone scan or dual watch holding on a signal (dual watch: on the channel it is on now) */
    return !scan_on(&sm->scan) ||
        ((sm->scan.kind==SCAN_ZONE || sm->scan.kind==SCAN_DUAL) && sm->scan.phase==SCAN_PH_HOLD && !scan_skipping(sm));
}
bool native_lab_scan_stop(uint16_t pos, uint8_t stop, bool carrier)
{
    bool cooling=pos==scan_skip_pos && (int32_t)(scan_skip_until-hal_millis())>0;
    if(stop!=scan_stop_seen) { scan_stop_seen=stop;scan_carrier=false;scan_n.stops++; }
    if(carrier && !scan_carrier) { scan_carrier=true;scan_n.carriers++;scan_n.cooled+=cooling; }
    return cooling;
}
static void scan_give_back(radio_sm_t *sm)
{
    uint32_t now=hal_millis();
    if(!scan_release) {
        uint32_t heard=p25_lab_channel_last_voice_ms();
        if(heard && (int32_t)(heard-scan_taken_ms)>=0) {
            if(!scan_voice_ms) { scan_n.voiced++;scan_n.voice_ms=(uint16_t)(heard-scan_taken_ms); }
            scan_voice_ms=heard;
        }
        heard=p25_lab_channel_last_sync_ms();
        if(heard && (int32_t)(heard-scan_taken_ms)>=0) {
            if(!scan_sync_ms) { scan_n.synced++;scan_n.sync_ms=(uint16_t)(heard-scan_taken_ms);scan_quiet=false; }
            scan_sync_ms=heard;
        }
        bool zone=scan_by_frames(sm);
        uint32_t cool=0;
        uint16_t *cause=&scan_n.foreign;
        if(scan_voice_ms) {
            if(now-scan_voice_ms<SCAN_P25_END_MS) return;
            cause=&scan_n.ended;
        } else if(scan_after_tx) {
            if(now-scan_taken_ms<SCAN_P25_NO_VOICE_MS) return;
            cause=&scan_n.ended;    /* no reply to our over: back to the ring, no cooldown */
        } else if(zone && scan_sync_ms && now-scan_sync_ms>=SCAN_P25_GONE_MS) cause=&scan_n.tails;
        else if(zone && !scan_sync_ms && now-scan_taken_ms>=SCAN_P25_PROBE_MS) {
            bool again=scan_quiet && scan_key(sm)==scan_skip_pos &&
                (int32_t)(now-scan_skip_until)<(int32_t)SCAN_P25_AGAIN_MS;
            cool=again?SCAN_P25_COOLDOWN_MS:SCAN_P25_RETRY_MS;
            cause=&scan_n.quiet;
        } else if(now-scan_taken_ms<SCAN_P25_NO_VOICE_MS) return;
        else if(scan_sync_ms && !(scan_key(sm)==scan_skip_pos &&
                (int32_t)(now-scan_skip_until)<(int32_t)SCAN_P25_AGAIN_MS)) {
            /* DDM32 Plus: P25 frames but no voice for us yet (a late entry still waiting for its link control, or
             * another talk group): taken again after the short pause, so a call of ours isn't skipped for 8 s; the
             * whole cooldown when it happens again */
            cool=SCAN_P25_RETRY_MS;
        } else cool=SCAN_P25_COOLDOWN_MS;
        if(sm->ptt || live_owner()) return;
        (*cause)++;
        if(cause==&scan_n.quiet) scan_quiet=true;
        if(cool) { scan_skip_pos=scan_key(sm); scan_skip_until=now+cool; }
        scan_release=true;channel_handoff=true;
        live_enabled=live_rx_pending=live_tx_pending=false;
        p25_lab_channel_stop();
        if(voice_owner){pending_start=0;nt_stop(&test,test.session,NT_USER_STOP);}
    }
    if(voice_owner || test.owned || p25_lab_busy()) return;   /* the worker is still letting go */
    scan_release=scan_owned=scan_after_tx=false;
    scan_settle_until=hal_millis()+SCAN_P25_SETTLE_MS;
    channel_selected=channel_handoff=false;
    live_restore_pending=false;channel_denial=0;
    restore_channel(sm);    /* back on the FM path: the scan sees the carrier gone and moves on */
}
#endif
#if CONFIG_P25_SEARCH
/* P25 search (radio/p25search.h): the sweep stops on a carrier; the native receiver takes that frequency with any
 * NAC and any talkgroup. P25 frames within P25S_PROBE_MS: the call plays and is noted, and the frequency is given
 * back P25S_END_MS after the last frame; none: given back at once (the search skips it for a while). Receive only:
 * PTT is consumed (it stops the search), never keyed. */
#if CONFIG_SEARCH_SURE
/* DDM32 Plus surer search: the zone scan's probe time for this receiver (SCAN_P25_PROBE_MS: its start and a frame
 * with a NID; 0.7 s left a slow start's P25 unheard and skipped); a carrier that went off the air meanwhile
 * (p25s_carrier_gone) is given back at once and not skipped */
enum { P25S_PROBE_MS=1200, P25S_END_MS=1500 };
#else
enum { P25S_PROBE_MS=700, P25S_END_MS=1500 };
#endif
static bool search_owned, search_release, search_was_p25;
static uint32_t search_taken_ms, search_hz, search_frames0;
#if CONFIG_ALL_SEARCH
/* DDM32 Plus all digital search (radio/dmrsearch.h): the same take and give back for its P25 and YSF probes. The
 * engine decides (dmrs_probe, as for its DMR and NXDN probes in the radio core): this reports what the receiver heard
 * since it took the frequency and hands it back when the engine moves on (the next mode, the sweep, a stop). The
 * caller's sequence at the take (a call heard before is not this frequency's) */
static bool search_all;
#endif
#if CONFIG_ALL_SEARCH || CONFIG_P25_CALLER
static uint32_t search_seq0;
#endif
static void search_take(radio_sm_t *sm)
{
    (void)sm;
    uint32_t hz=p25s_at_hz();
    bool ysf=false;
    (void)ysf;
#if CONFIG_ALL_SEARCH
    if(dmrs_on())hz=dmrs_native_hz(&ysf);
#endif
#if CONFIG_YSF
    if(!p25_lab_set_ysf(ysf,0))return;  /* the all digital search's YSF probe: any DG-ID */
#endif
    if(!p25_lab_set_channel(0xF7Eu,0xF7Eu,0,0,0))return;    /* any NAC, any talkgroup, receive only */
    search_hz=hz;
    search_frames0=p25_lab_channel_frames(NULL,NULL);
    search_taken_ms=hal_millis();
#if CONFIG_ALL_SEARCH
    search_all=dmrs_on();
    if(search_all) {
#if CONFIG_YSF
        search_seq0=ysf?p25_lab_ysf_caller(NULL,NULL):p25_lab_p25_caller(NULL,NULL);
#else
        search_seq0=p25_lab_p25_caller(NULL,NULL);
#endif
        dmrs_retuned(search_taken_ms);    /* the mode's window starts now */
    }
#endif
#if CONFIG_P25_CALLER
#if CONFIG_ALL_SEARCH
    if(!search_all)
#endif
    search_seq0=p25_lab_p25_caller(NULL,NULL);  /* the P25 search: a caller heard before is not this frequency's */
#endif
    search_owned=true;search_release=search_was_p25=false;
    channel_selected=live_enabled=live_rx_pending=true;
    live_tx_pending=false;channel_denial=0;
}
/* Start handing the frequency back (the worker lets go first) */
static void search_let_go(void)
{
    search_release=true;channel_handoff=true;
    live_enabled=live_rx_pending=live_tx_pending=false;
    p25_lab_channel_stop();
    if(voice_owner){pending_start=0;nt_stop(&test,test.session,NT_USER_STOP);}
}
#if CONFIG_ALL_SEARCH
/* What the native receiver heard since the take, for the engine: P25 frames (any NAC) or YSF frames whose FICH held,
 * the NAC / DG-ID, the call's talkgroup and source (P25) or source callsign (YSF), voice playing. Not inlined: its
 * locals stay off the radio task's deepest path */
static uint32_t __attribute__((noinline)) all_heard(uint32_t now)
{
    dmrs_heard_t h={.cc=DMRS_CC_NONE};
    bool ysf;
    (void)dmrs_native_hz(&ysf);
#if CONFIG_YSF
    if(ysf) {
        uint32_t s=p25_lab_channel_last_sync_ms();
        if(s && (int32_t)(s-search_taken_ms)>=0) {
            char src[11];
            h.bursts=DMRS_MIN_BURSTS;
            h.cc=p25_lab_ysf_dgid();
            /* a callsign heard before the take (another frequency's call) is not this one's, as P25's below */
            if(p25_lab_ysf_caller(src,NULL)!=search_seq0)dmrs_call_pack(src,&h.dst,&h.src);
        }
    } else
#endif
    {
        uint16_t nac=0;
        uint32_t n=p25_lab_channel_frames(NULL,&nac)-search_frames0;
        if(n) {
            uint16_t tg=0;
            uint32_t src=0;
            h.bursts=(uint16_t)(n<0xFFFFu?n:0xFFFFu);
            h.cc=nac;
            if(p25_lab_p25_caller(&tg,&src)!=search_seq0) { h.dst=tg;h.src=src;h.group=tg!=0; }
        }
    }
    uint32_t v=p25_lab_channel_last_voice_ms();
    h.call=v && (int32_t)(v-search_taken_ms)>=0 && now-v<DMRS_VOICE_MS;
    return dmrs_probe(&h,now);
}
#endif
static void search_service(radio_sm_t *sm)
{
    uint32_t now=hal_millis(),last_ms=0;
    uint16_t nac=0;
#if CONFIG_ALL_SEARCH
    if(search_all) {
        /* The all digital search: held while the engine stays on this mode here, given back when it moves on */
        if(!search_release && dmrs_on() && all_heard(now)==0 && dmrs_on())return;
        if(!search_release)search_let_go();
        if(voice_owner || test.owned || p25_lab_busy())return;    /* the worker is still letting go */
        search_owned=search_release=search_all=false;
        channel_selected=channel_handoff=false;
        live_restore_pending=false;channel_denial=0;
        sm->audio_out=sm->green_out=sm->red_out=-1;
#if CONFIG_DMR_EMERGENCY
        if(dmrs_on() && dmr_emergency_start_pending())give_back(sm,true,RREQ_DMR_SEARCH);  /* an emergency: it ends */
        else
#endif
        if(dmrs_on())dmrs_regained();     /* the radio core sets the receiver up for what comes next */
        else restore_channel(sm);         /* stopped meanwhile: the channel again */
        return;
    }
#endif
    if(!search_release) {
        bool p25=p25_lab_channel_frames(&last_ms,&nac)!=search_frames0;
        if(p25) {
            uint16_t tg=0;
            uint32_t src=0;
#if CONFIG_P25_CALLER
            if(p25_lab_p25_caller(&tg,&src)==search_seq0) { tg=0;src=0; }   /* no call heard here yet: not the last one's */
#endif
            p25s_probe_found(nac,tg,src,now);
            search_was_p25=true;
        }
#if CONFIG_SEARCH_SURE
        bool gone=!p25 && p25s_carrier_gone(now);    /* off the air: given back at once (and not skipped) */
#else
        const bool gone=false;
#endif
        if(!gone && (p25 ? now-last_ms<P25S_END_MS : now-search_taken_ms<P25S_PROBE_MS))return;
        search_let_go();
    }
    if(voice_owner || test.owned || p25_lab_busy())return;    /* the worker is still letting go */
    search_owned=search_release=false;
    channel_selected=channel_handoff=false;
    live_restore_pending=false;channel_denial=0;
    p25s_probe_done(search_was_p25,hal_millis());
    give_back(sm,p25s_on(),RREQ_P25_SEARCH);    /* the radio task tunes the sweep's next step (p25s_epoch changed) */
}
#endif
#if CONFIG_P25_TRUNK
/* P25 trunk following (radio/p25trunk.h): the native receiver takes the control channel with the selected channel's
 * NAC and any talkgroup, receive only, and decodes its TSBKs; the follower says where the receiver must be. A move
 * (control channel -> voice channel -> back) only stops the worker: the channel receive starts again by itself on
 * the new frequency (live_next, channel_rx_hz), and the counts the follower reads start there. PTT is denied as on an
 * RX-only channel, never keyed. */
static bool trunk_owned, trunk_release, trunk_moving;
static uint32_t trunk_hz, trunk_shown_ms, trunk_serial_seen;
static p25_lab_trunk_t trunk_at;	/* the receive's counts when it started on trunk_hz */
bool p25tk_band_ok(uint32_t hz) { return board_in_band(hz,false); }
static void trunk_take(void)
{
#if CONFIG_YSF
    if(!p25_lab_set_ysf(0,0))return;
#endif
    /* the channel's NAC (F7E: any), any talkgroup, receive only; then TSDUs too */
    if(!p25_lab_set_channel(p25tk_nac(),p25tk_nac(),0,0,0) || !p25_lab_set_trunk(1))return;
    trunk_hz=p25tk_hz();
    trunk_owned=trunk_moving=true;trunk_release=false;
    channel_selected=live_enabled=live_rx_pending=true;
    live_tx_pending=false;channel_denial=0;
}
/* live_next started the channel receive on trunk_hz: what it hears from here on is the follower's */
static void trunk_tuned(void)
{
    if(trunk_moving) { p25_lab_trunk_counts(&trunk_at);trunk_moving=false; }
}
static void trunk_stop_worker(void)
{
    p25_lab_channel_stop();
    if(voice_owner){pending_start=0;nt_stop(&test,test.session,NT_USER_STOP);}
}
/* Not inlined, and the channel is retuned by the caller (true: the receiver was handed back): its locals stay off the
 * radio task's deepest path (channel_service > restore_channel > radio_sm_request) */
static bool __attribute__((noinline)) trunk_service(radio_sm_t *sm)
{
    uint32_t now=hal_millis();
    if(!trunk_owned) {
        if(p25tk_on() && !channel_selected && !channel_handoff && !voice_owner && !test.owned && !p25_lab_busy() &&
           !sm->power_off_req && !sm->view.power_off)
            trunk_take();
        return false;
    }
    if(!trunk_release) {
        uint8_t b[10];
        while(p25_lab_tsbk_pop(b))p25tk_tsbk(b,now);
        p25_lab_trunk_t c;
        p25_lab_trunk_counts(&c);
        p25tk_rx_t in={.on_hz=trunk_moving?0u:trunk_hz,.frames=c.frames-trunk_at.frames,.terms=c.terms-trunk_at.terms,
                       .last_ms=c.last_ms,.bad=c.bad,.nac=c.nac};
#if CONFIG_P25_CALLER
        if(!trunk_moving)p25_lab_p25_caller(&in.tg,&in.src);   /* the voice's link control: the source on late entry */
#endif
        uint32_t want=p25tk_poll(&in,now);
        if(p25tk_serial()!=trunk_serial_seen && now-trunk_shown_ms>=250u) {
            trunk_serial_seen=p25tk_serial();trunk_shown_ms=now;sm->serial++;   /* the screen redraws */
        }
        if(p25tk_on()) {
            if(want!=trunk_hz) { trunk_hz=want;trunk_moving=true;trunk_stop_worker(); }
            channel_view(sm);
            return false;
        }
        /* Stopped (the screen was left): hand the receiver back, the worker lets go first */
        trunk_release=channel_handoff=true;
        live_enabled=live_rx_pending=live_tx_pending=false;
        trunk_stop_worker();
    }
    if(voice_owner || test.owned || p25_lab_busy())return false;    /* the worker is still letting go */
    trunk_owned=trunk_release=trunk_moving=false;
    channel_selected=channel_handoff=false;
    live_restore_pending=false;channel_denial=0;sm->ptt=native_ptt=false;
    p25_lab_set_trunk(0);
    return true;    /* the channel again */
}
#endif
/* The frequency the channel receive runs on: the search's hit, the trunk follower's, else the channel's */
static uint32_t channel_rx_hz(const radio_sm_t *sm)
{
#if CONFIG_P25_SEARCH
    if(search_owned)return search_hz;
#endif
#if CONFIG_P25_TRUNK
    if(trunk_owned)return trunk_hz;
#endif
    return sm->view.ch.rx_hz;
}
#if CONFIG_P25_ANY
/* DM32 Plus: the channel the native receiver was started with. A saved channel edit reaches the radio core through
 * codeplug_serial() (radio_sm_poll reloads the view), not through a request, so without this the receiver kept the
 * old NAC/talkgroup until the channel was changed (an edit to NAC Any still filtered on the old NAC). */
static struct {
    uint32_t hz,id; uint16_t nac,tg,tx_nac; uint8_t mode,squelch; bool rx_only;
} selected_rx;
static bool reselect;
static void selected_rx_note(const channel_t *ch)
{
    selected_rx.hz=ch->rx_hz;selected_rx.id=ch->p25_id;selected_rx.nac=ch->p25_nac;selected_rx.tg=ch->p25_tg;
    selected_rx.mode=(uint8_t)ch->mode;selected_rx.rx_only=ch->rx_only;
    selected_rx.tx_nac=codeplug_p25_tx_nac(ch);selected_rx.squelch=ch->p25_squelch;
}
static bool selected_rx_stale(const channel_t *ch)
{
    return selected_rx.hz!=ch->rx_hz || selected_rx.id!=ch->p25_id || selected_rx.nac!=ch->p25_nac ||
           selected_rx.tg!=ch->p25_tg || selected_rx.mode!=(uint8_t)ch->mode || selected_rx.rx_only!=ch->rx_only ||
           selected_rx.tx_nac!=codeplug_p25_tx_nac(ch) || selected_rx.squelch!=ch->p25_squelch;
}
#endif
static void channel_service(radio_sm_t *sm)
{
#if CONFIG_DMR_EMERGENCY
    /* DDM32 Plus: an emergency asked for (the key, the lone worker, SOS) never waits for the receiver: the DMR / all
     * digital search, the P25 search, P25 and DMR trunk following end at once (as PTT ends them) and their receiver is
     * let go below; a P25 channel or letter it holds is let go further down. A native transmission ends first (PTT).
     * The radio tick holds the start back meanwhile (emergency_tick, 200 ms at most): it usually sees it first */
    bool emergency=dmr_emergency_start_pending();
    if(emergency && !native_ptt && !live_owner()) {
        /* A search the radio core runs (the sweep's frequency, the DMR stack hearing every color code and slot) ends
         * through its own stop request, which tunes the channel at once (RREQ_RETUNE would wait for the trim to settle,
         * after the radio tick took the start). One whose receiver the native side holds (a probe, the following) stays
         * on while its receiver is let go, so the radio tick keeps holding the start back; its give back then ends it
         * through the same stop request (give_back) */
        bool owned=false,stop=false;
        radio_req_t r={.type=RREQ_PTT};     /* (its type set below when used; arg 0: the stop) */
#if CONFIG_P25_SEARCH
        owned=search_owned;
#endif
#if CONFIG_DMR_SEARCH
        if(dmrs_on() && !owned){stop=true;r.type=RREQ_DMR_SEARCH;}
#endif
#if CONFIG_P25_SEARCH
        if(p25s_on() && !owned){stop=true;r.type=RREQ_P25_SEARCH;}
        if(search_owned && !search_release)search_let_go();
#endif
#if CONFIG_P25_TRUNK
        if(p25tk_on()) {
            if(!trunk_owned){stop=true;r.type=RREQ_P25_TRUNK;}
            else if(!trunk_release) {
                trunk_release=channel_handoff=true;     /* as trunk_service's own stop, the following still on */
                live_enabled=live_rx_pending=live_tx_pending=false;
                trunk_stop_worker();
            }
        }
#endif
#if CONFIG_DMR_TRUNK
        if(dmrtk_on()){stop=true;r.type=RREQ_DMR_TRUNK;}  /* (receive only: it refused the emergency's TX) */
#endif
        (void)owned;
        if(stop)radio_sm_request(sm,&r,hal_millis());
    }
#else
    bool emergency=false;
#endif
#if CONFIG_DMR_SEARCH
#if CONFIG_ALL_SEARCH
    /* The all digital search: only its P25 / YSF probe may take the receiver (the rest is the radio core's) */
    if(dmrs_on() && search_owned) { search_service(sm);return; }
    bool all_ysf;
    if(dmrs_on() && dmrs_native_hz(&all_ysf) && !channel_selected && !channel_handoff && !voice_owner && !test.owned &&
       !p25_lab_busy() && !sm->power_off_req && !sm->view.power_off)
        search_take(sm);
#endif
    if(dmrs_on())return;    /* the DMR search has the receiver (radio_sm.c): the P25 channel waits for its end */
#endif
#if CONFIG_P25_SEARCH
    if(search_owned) { search_service(sm);return; }
    if(p25s_on()) {
        /* While searching, only the search's probe may take the receiver */
        if(p25s_phase()==P25S_PROBE && !channel_selected && !channel_handoff && !voice_owner && !test.owned &&
#if CONFIG_SEARCH_SOFTP25
           !p25s_soft_pending() &&  /* DDM32 Plus: the soft probe reads the hit first (radio/radio_p25soft.h) */
#endif
           !p25_lab_busy() && !sm->power_off_req && !sm->view.power_off)
            search_take(sm);
        return;
    }
#endif
#if CONFIG_P25_TRUNK
    if(trunk_owned || p25tk_on()) {
        if(trunk_service(sm))give_back(sm,p25tk_on(),RREQ_P25_TRUNK);
        return;
    }
#endif
#if CONFIG_DMR_EMERGENCY
    /* ... and never for a P25 call: the native receiver lets the channel go at once, as for a request (the radio task's
     * tick runs only without it), and takes none while the start is pending. The radio core then puts it on a DMR
     * letter of the watch, or fails it at once on a P25 channel (SOS and the lone worker fall back) */
    if(emergency && (channel_selected || channel_handoff) && !native_ptt && !live_owner()) {
        if(channel_let_go(sm)==0) restore_channel(sm);
        return;
    }
#endif
#if CONFIG_P25_SCAN
    if(scan_owned && (channel_selected || channel_handoff) && scan_on(&sm->scan)) { scan_give_back(sm); return; }
    if(scan_owned && !scan_on(&sm->scan)) scan_owned=scan_release=false;  /* the scan was stopped: keep the channel */
#endif
#if CONFIG_P25_ANY
    if(reselect) {
        if(voice_owner || test.owned || p25_lab_busy())return;    /* the worker is still letting go */
        reselect=channel_selected=channel_handoff=false;
        live_restore_pending=false;channel_denial=0;
        if(!codeplug_mode_p25(sm->view.ch.mode)) { restore_channel(sm);return; }   /* edited to FM/DMR: retune */
    } else if(channel_selected && !channel_handoff && !sm->ptt && !live_owner() && selected_rx_stale(&sm->view.ch)) {
        reselect=channel_handoff=true;
        live_enabled=live_rx_pending=live_tx_pending=false;
        p25_lab_channel_stop();
        if(voice_owner){pending_start=0;nt_stop(&test,test.session,NT_USER_STOP);}
        return;
    }
#endif
    if(channel_handoff)return;
    /* DM32 Plus (CONFIG_VFO_MODES): a P25 or YSF VFO is taken like a channel of that mode */
    bool selected=codeplug_mode_p25(sm->view.ch.mode) && (CONFIG_VFO_MODES || !sm->view.vfo) &&
                  !sm->power_off_req && !sm->view.power_off && !emergency;
#if CONFIG_P25_SCAN
    selected=selected && scan_allows(sm);
#endif
#if CONFIG_SPECTRUM
    selected=selected && !spectrum_on();   /* the spectrum sweep has the receiver */
#endif
#if CONFIG_BCAST_FM
    /* Broadcast FM owns the speaker while it plays: the P25 receiver waits until it is switched off */
    selected=selected && !sm->view.bcast;
#endif
#if CONFIG_HOTSPOT
    /* DM32 Plus: the MMDVM hotspot has the modem (DMR). RREQ_HS_ENTER handed the channel back like any request
     * (native_lab_request); it is not taken again until hotspot mode ends (RREQ_HS_LEAVE retunes the channel) */
    selected=selected && !sm->hotspot;
#endif
    if(selected && !channel_selected && !voice_owner && !test.owned && !p25_lab_busy()) {
        const channel_t *ch=&sm->view.ch;
        if(!codeplug_p25_valid(ch) || !board_in_band(ch->rx_hz,false))return;
#if CONFIG_YSF
        if(!p25_lab_set_ysf(codeplug_mode_ysf(ch->mode),(uint8_t)ch->p25_tg))return;
#endif
#if CONFIG_P25_RXGL
        /* DDM32 Plus: a list filters (its talkgroups play too), Any hears every group */
        if(!p25_lab_set_channel(ch->p25_nac,codeplug_p25_tx_nac(ch),ch->p25_tg,ch->p25_id,p25_rxgl_selective(ch)) ||
           !p25_rxgl_apply(ch))return;
#else
        if(!p25_lab_set_channel(ch->p25_nac,codeplug_p25_tx_nac(ch),ch->p25_tg,ch->p25_id,ch->p25_squelch==1))return;
#endif
        channel_selected=live_enabled=live_rx_pending=true;
        /* DDM32 Plus: PTT already held (pressed before the take, e.g. on a letter of the ring): it keys now */
        live_tx_pending=native_ptt && !channel_denial;
        if(!native_ptt)channel_denial=0;
#if CONFIG_P25_ANY
        selected_rx_note(ch);
#endif
#if CONFIG_P25_SCAN
        scan_owned=scan_on(&sm->scan);scan_release=scan_after_tx=false;scan_taken_ms=hal_millis();
        scan_voice_ms=scan_sync_ms=0;scan_n.takes+=scan_owned;
#endif
    }
    if(native_ptt && !sm->ptt) native_ptt=false;   /* the radio core let go of PTT (power off, a request) */
    if(!native_ptt) live_tx_pending=false;
    if(channel_selected){
        if(live_owner() && !native_ptt) p25_lab_live_up();    /* no PTT for it: the transmission ends */
        if(live_owner() && test.owned && sm->view.tot_s &&
           (uint32_t)(hal_millis()-channel_tx_ms)>=sm->view.tot_s*1000u) {
            channel_denial=TXDENY_TIMEOUT;
            p25_lab_channel_stop();nt_stop(&test,test.session,NT_DEADLINE);
        }
        channel_view(sm);
    }
}
#endif
static uint32_t live_summary[5]; /* count, stop reason, blocks sent, PA keys, faults */
static int live_owner(void) { return voice_owner && test.config.mode==NT_TRANSMIT; }
#endif
static uint32_t last_req_type,last_req_arg,last_req_ms,ptt_consumed,cancellations,cancel_type,cancel_arg;
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0]|p[1]<<8); }
static void put(uint8_t *p, uint32_t x) { for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(x>>(i*8)); }
static void deny(uint32_t sid)
{
    taskENTER_CRITICAL();
    if (test.session==sid) nt_deny(&test);
    taskEXIT_CRITICAL();
}
void native_lab_init(void) {
    pending_start=battery_at=voice_owner=0;
#if CONFIG_DEV_TX
    live_enabled=live_rx_pending=live_tx_pending=live_restore_pending=false;
#if CONFIG_P25_CHANNELS
    channel_selected=channel_handoff=native_ptt=false;channel_denial=0;channel_tx_ms=0;
#if CONFIG_P25_SCAN
    scan_owned=scan_release=scan_quiet=scan_carrier=scan_after_tx=false;scan_stop_seen=0;
    scan_taken_ms=scan_skip_until=scan_settle_until=scan_voice_ms=scan_sync_ms=0;scan_skip_pos=0;
    scan_n=(__typeof__(scan_n)){0};
#endif
#if CONFIG_P25_ANY
    reselect=false;
#endif
#if CONFIG_P25_TRUNK
    trunk_owned=trunk_release=trunk_moving=false;trunk_hz=0;
#endif
#endif
    for (unsigned i=0;i<5;i++) live_summary[i]=0;
#endif
    last_req_type=last_req_arg=last_req_ms=ptt_consumed=cancellations=cancel_type=cancel_arg=0;
    nt_init(&test,board.native_modem);
}
static int updater(void) {
#if CONFIG_FIRSTBOOT
    return firstboot_update_busy();
#else
    return 0;
#endif
}
int native_lab_reserved(void) {
    taskENTER_CRITICAL();
    int r=updater() || voice_owner || test.owned || test.state==NT_ARMED || pending_start;
#if CONFIG_DEV_TX
    r=r || live_enabled;
#endif
    taskEXIT_CRITICAL();return r;
}
uint32_t native_lab_wait_ms(void)
{
    taskENTER_CRITICAL();
    uint32_t ms=voice_owner?5u:(test.owned || test.state==NT_ARMED || pending_start)?2u:0u;
#if CONFIG_DEV_TX
    if (live_enabled && !ms) ms=5;
#endif
    taskEXIT_CRITICAL();return ms;
}
int native_lab_owns(void) {
    taskENTER_CRITICAL();
    int r=updater() || test.owned;
#if CONFIG_DEV_TX
    r=r || live_enabled || (live_owner() && test.state==NT_ARMED);
#if CONFIG_P25_CHANNELS
    r=r || channel_handoff;
#endif
#endif
    taskEXIT_CRITICAL();
    return r;
}
static int battery_ok(void)
{
    if (!board.power || !board.power->switch_on || !board.power->switch_on()) return 0;
    if (test.config.mode!=NT_TRANSMIT) return 1;
    if (!board.power->battery_mv) return 0;
    uint16_t mv=board.power->battery_mv();
    return mv && (!board.battery_critical_mv || mv>board.battery_critical_mv);
}
static int eligible(const radio_sm_t *sm)
{
    const radio_view_t *v=&sm->view;
#if CONFIG_P25_CHANNELS
    if(channel_selected) {
        if(!board.native_modem || updater() || sm->hotspot || v->power_off || !battery_ok())return 0;
        if(test.config.mode==NT_TRANSMIT)
            return native_ptt && sm->ptt && radio_sm_tx_check(sm)==TXDENY_NONE && v->ch.tx_hz==test.config.frequency_hz;
        return channel_rx_hz(sm)==test.config.frequency_hz;
    }
#endif
    if (!board.native_modem || (p25_lab_busy() && !voice_owner) || sm->ptt || sm->hotspot ||
        (v->state!=RS_IDLE && !(voice_owner && v->state==RS_RX)) ||
        v->ch.mode!=RADIO_MODE_FM || v->am || v->bcast || v->power_off || beep_busy()) return 0;
#if CONFIG_SCAN
    if (scan_on(&sm->scan)) return 0;
#endif
    if (board.audio && board.audio->spk_pending && board.audio->spk_pending()) return 0;
    if (!battery_ok()) return 0;
    if (test.config.mode==NT_TRANSMIT && (v->ch.rx_only || sm->set.tx_inhibit ||
        v->ch.tx_hz!=test.config.frequency_hz)) return 0;
    return test.config.mode==NT_TRANSMIT || v->ch.rx_hz==test.config.frequency_hz;
}
#if CONFIG_DEV_TX
static void live_next(radio_sm_t *sm)
{
    taskENTER_CRITICAL();
    if (!live_enabled || voice_owner || pending_start || test.owned || p25_lab_busy()) {
        taskEXIT_CRITICAL();return;
    }
    bool tx=live_tx_pending, rx=live_rx_pending;
    if (tx && board.audio && board.audio->spk_pending && board.audio->spk_pending()) {
        taskEXIT_CRITICAL();return;
    }
    live_tx_pending=false;
    if (!tx) live_rx_pending=false;
    if ((tx || rx) && test.state==NT_FAULT) nt_reset(&test);
    uint32_t sid=0;
    if (tx && nt_arm_live(&test,sm->view.ch.tx_hz)) {
        test.config.power=sm->view.power;voice_owner=sid=test.session;
    }
    taskEXIT_CRITICAL();
    if (sid && (!eligible(sm) || !p25_lab_live_begin(sid))) {
        deny(sid);
        native_lab_voice_release(sid);
#if CONFIG_P25_CHANNELS
        /* DDM32 Plus: a P25 transmission refused while PTT is held shows as refused (until release), not as nothing */
        if(channel_selected && native_ptt && !channel_denial) channel_denial=TXDENY_BUSY;
#endif
    }
    if (!tx && rx &&
#if !CONFIG_P25_CHANNELS
        sm->view.ch.rx_hz==145500000u &&
#endif
#if CONFIG_P25_CHANNELS
        !p25_lab_channel_rx(channel_rx_hz(sm))) live_rx_pending=true;
#else
        !p25_lab_channel_rx(sm->view.ch.rx_hz)) live_rx_pending=true;
#endif
#if CONFIG_P25_CHANNELS && CONFIG_P25_TRUNK
    if(!tx && rx && !live_rx_pending && trunk_owned)trunk_tuned();
#endif
}
#endif
/* DDM32 Plus: the native receiver hands the channel back: the TX / TX denied / RX it showed goes with it. Left in the
 * view (a give back of the ring or a zone scan right after our over, the drain's TX the last shown) the radio core
 * took it for its own transmission: Home showed TALKING after release, the ring stood still (scan_talked) and the
 * retune waited for the end of a TX that never came, until the time-out or a power cycle */
static void native_view_clear(radio_sm_t *sm)
{
    if(!sm->ptt && (sm->view.state==RS_TX || sm->view.state==RS_TX_DENIED || sm->view.state==RS_RX)) {
        sm->view.state=RS_IDLE;sm->view.tx_deny=0;sm->view.tot_warning=false;sm->serial++;
    }
}
static void restore_channel(radio_sm_t *sm)
{
    native_view_clear(sm);
    sm->audio_out=sm->green_out=sm->red_out=-1;
    radio_req_t r={.type=RREQ_RETUNE};
    radio_sm_request(sm,&r,hal_millis());
}
/* DDM32 Plus: a search's or trunk following's receiver handed back. With an emergency asked for and the search /
 * following still on (on), it ends through its own stop request (stop_type, arg 0), which tunes the channel at once,
 * before the radio tick takes the start (it holds it back while they run: emergency_tick); otherwise as before */
static inline void __attribute__((unused)) give_back(radio_sm_t *sm, bool on, uint8_t stop_type)
{
#if CONFIG_DMR_EMERGENCY
    if(on && dmr_emergency_start_pending()) {
        native_view_clear(sm);
        sm->audio_out=sm->green_out=sm->red_out=-1;
        radio_req_t r={.type=stop_type};
        radio_sm_request(sm,&r,hal_millis());
        return;
    }
#else
    (void)on;(void)stop_type;
#endif
    restore_channel(sm);
}
#if CONFIG_DEV_TX && CONFIG_P25_CHANNELS
/* A request (or PTT on another letter of the ring) takes the channel from the native receiver: 0 once it is let go,
 * NATIVE_REQUEST_WAIT while the worker still lets go (the radio task keeps the request) */
static int channel_let_go(radio_sm_t *sm)
{
#if CONFIG_P25_SCAN
    scan_owned=scan_release=false;   /* the user's request takes over from the scan's hold */
#endif
#if CONFIG_P25_SEARCH
    search_owned=search_release=false;  /* the radio task ends the search for the request */
#endif
#if CONFIG_P25_TRUNK
    trunk_owned=trunk_release=trunk_moving=false;   /* the radio task ends the following or it takes over again */
#endif
#if CONFIG_P25_ANY
    reselect=false;     /* the request's own handoff selects the channel again */
#endif
    if(voice_owner || test.owned || p25_lab_busy()) {
        channel_handoff=true;live_enabled=live_rx_pending=live_tx_pending=false;
        p25_lab_channel_stop();
        if(voice_owner){pending_start=0;nt_stop(&test,test.session,NT_INTERRUPTED);}
        return NATIVE_REQUEST_WAIT;
    }
    channel_selected=channel_handoff=false;
    live_enabled=live_restore_pending=false;
    native_view_clear(sm);
    return 0;
}
#endif
void native_lab_service(radio_sm_t *sm, uint32_t now)
{
    if(updater())return;
#if CONFIG_P25_CHANNELS
    channel_service(sm);
#endif
    now=hal_micros();
    taskENTER_CRITICAL();
    uint32_t sid=pending_start; pending_start=0;
    taskEXIT_CRITICAL();
    if (sid && test.state==NT_ARMED && sid==test.session) {
        if (eligible(sm)) {
            if (!nt_begin(&test,sid,now)) deny(sid);
#if CONFIG_P25_CHANNELS
            else if(channel_selected && test.config.mode==NT_TRANSMIT) {
                channel_tx_ms=sm->tx_start_ms=hal_millis();sm->tot_warned=false;
            }
#endif
            battery_at=now;
        }
        else deny(sid);
    }
    if (test.owned) {
        now=hal_micros(); /* setup may have taken time */
        if ((uint32_t)(now-battery_at)>=25000u) {
            battery_at=now;
            if (!battery_ok()) { taskENTER_CRITICAL(); nt_fault(&test,NT_DENIED); taskEXIT_CRITICAL(); }
        }
        /* Sample the clock with interrupts excluded: a newer IRQ timestamp
         * must not be compared against a stale task timestamp across setup. */
        taskENTER_CRITICAL(); nt_tick(&test,hal_micros()); taskEXIT_CRITICAL();
#if defined(CONFIG_RF_FIXES) && CONFIG_RF_FIXES
        /* A receive session's lost modem request (hn_watch, after RadioFoundry 3d5940b) */
        if (test.ops && test.ops->watch) test.ops->watch(test.ops->ctx,hal_micros());
#endif
        if (nt_cleanup(&test)) {
#if CONFIG_DEV_TX
            if (live_enabled) return; /* keep the analog speaker muted between directions */
#if CONFIG_P25_CHANNELS
            if(channel_handoff)return;
#endif
            live_restore_pending=false;
#endif
            /* Board is back in FM; force a full retune and fresh speaker/LED decisions. */
            restore_channel(sm);
        }
    }
#if CONFIG_DEV_TX
    if (live_restore_pending && !test.owned && !voice_owner) {
        live_restore_pending=false;
        restore_channel(sm);
    }
    live_next(sm);
#if CONFIG_P25_CHANNELS
    if(channel_selected)channel_view(sm);
#endif
#endif
}
int native_lab_request(radio_sm_t *sm, const radio_req_t *r)
{
    if(updater()) {
#if CONFIG_FIRSTBOOT
        if(r->type==RREQ_POWER_OFF) { firstboot_update_cancel();return 0; }
#endif
        return 1;
    }
#if CONFIG_P25_CHANNELS
    if(r->type==RREQ_PTT && (r->arg==NATIVE_PHYSICAL_PTT_DOWN || r->arg==NATIVE_PHYSICAL_PTT_UP))return 1;
#if CONFIG_HOTSPOT
    /* DM32 Plus: in hotspot mode (the channel already handed back) every request is the radio core's: PTT is denied
     * there (TXDENY_HOTSPOT: the host link transmits), never a P25 transmission on the channel underneath */
    if(sm->hotspot && !channel_selected && !channel_handoff && !voice_owner && !test.owned)return 0;
#endif
#if CONFIG_P25_TRUNK
    /* PTT never keys while following a trunked system (receive only): denied as on an RX-only channel, the
     * following goes on */
    if(r->type==RREQ_PTT && (p25tk_on() || trunk_owned)) {
        sm->ptt=native_ptt=r->arg!=0;
        channel_denial=sm->ptt?TXDENY_RX_ONLY:0;
        if(channel_selected)channel_view(sm);
        return 1;
    }
#endif
#if CONFIG_ALL_SEARCH
    /* PTT while the all digital search's P25 / YSF probe has the receiver: consumed, it stops the search and the
     * receiver is handed back (then the channel again), never keyed */
    if(r->type==RREQ_PTT && search_owned && search_all) {
        if(r->arg) {
            dmrs_stop();
            if(!search_release) search_let_go();
        }
        return 1;
    }
#endif
#if CONFIG_P25_SEARCH
    /* PTT never keys while searching or playing a find: it stops the search (receive only) */
    if(r->type==RREQ_PTT && (p25s_on() || search_owned)) {
        if(r->arg) {
            p25s_stop();
            if(search_owned && !search_release) search_let_go();
            else if(!search_owned) restore_channel(sm);
        }
        return 1;
    }
#endif
#if CONFIG_DMR_SEARCH
    /* PTT during the DMR search is the radio core's: it stops the search, nothing is keyed (receive only) */
    if(r->type==RREQ_PTT && dmrs_on())return 0;
#endif
#if CONFIG_DUAL_WATCH
    /* DDM32 Plus (docs/plus.md "Multi-watch", the TX target): PTT in dual watch or the ring talks on the selected letter,
     * never on the one the receiver happens to be on (before: C selected, the ring on or held by a call on B, keyed P25
     * on B). The native receiver holding another letter lets it go first (the request waits), then the radio core tunes
     * the selected one: a P25 / YSF letter is keyed below, an FM / DMR one by the radio core (returned to it) */
    if(r->type==RREQ_PTT && r->arg && !sm->ptt && !live_owner() && radio_sm_dual_away(sm,false,0)) {
        if(channel_selected || channel_handoff) {
            int w=channel_let_go(sm);
            if(w)return w;
            sm->audio_out=sm->green_out=sm->red_out=-1;  /* the board is back in FM: fresh speaker / LED decisions */
        }
        if(radio_sm_dual_away(sm,true,hal_millis())==2) { channel_denial=TXDENY_MODE;return 1; }
    }
#endif
    if(r->type==RREQ_PTT && (codeplug_mode_p25(sm->view.ch.mode) || channel_selected || live_owner())) {
        /* DDM32 Plus: PTT noted before the channel is looked at, so a letter of the ring is taken for it (scan_allows)
         * and a take that comes later keys at once */
        bool down=r->arg && !sm->ptt;
        if(down) { channel_denial=radio_sm_tx_check(sm);sm->ptt=native_ptt=true; }
        if(!channel_selected && codeplug_mode_p25(sm->view.ch.mode)) channel_service(sm);
        if(!r->arg) {
            sm->ptt=native_ptt=false;live_tx_pending=false;channel_denial=0;
            p25_lab_live_up();
            if(live_owner() && test.state==NT_ARMED)nt_stop(&test,test.session,NT_USER_STOP);
#if CONFIG_P25_SCAN
            /* a take of the ring that transmitted: the reply's wait from now (scan_give_back) */
            if(scan_owned) { scan_after_tx=true;scan_taken_ms=hal_millis();scan_voice_ms=scan_sync_ms=0; }
#endif
        } else if(down) {
#if CONFIG_YSF_TX
            if(!channel_denial && codeplug_mode_ysf(sm->view.ch.mode))ysf_tx_callsign(sm);
#endif
            if(!channel_denial) {
                /* ARMED is visible while mic prefill runs, before nt_begin. */
                channel_tx_ms=sm->tx_start_ms=hal_millis();
                sm->view.tx_elapsed_ms=0;sm->view.tot_warning=false;sm->tot_warned=false;
                live_tx_pending=live_rx_pending=true;
                p25_lab_channel_stop();
                if(voice_owner){pending_start=0;nt_stop(&test,test.session,NT_USER_STOP);}
            }
        }
        channel_view(sm);return 1;
    }
    if(channel_selected || channel_handoff) {
        if(r->type==RREQ_POWER_OFF) {
            sm->ptt=native_ptt=false;channel_denial=0;p25_lab_live_up();
        } else if(native_ptt || live_owner())return 1;
#if CONFIG_MULTI_WATCH
        /* DDM32 Plus: a new selection (or ring) that keeps this letter: no retune, the P25 call it holds goes on */
        if((r->type==RREQ_MWATCH || r->type==RREQ_DUAL) && channel_selected && !channel_handoff &&
           radio_sm_dual_keeps(sm,r)) return 0;
#endif
        return channel_let_go(sm);
    }
#endif
#if CONFIG_DEV_TX
    if (live_enabled && r->type==RREQ_PTT) {
        taskENTER_CRITICAL();
        if (r->arg==NATIVE_PHYSICAL_PTT_UP) {
            live_tx_pending=false;
            if (live_owner()) p25_lab_live_up();
        } else if (r->arg==NATIVE_PHYSICAL_PTT_DOWN && !live_owner()) {
            live_tx_pending=live_rx_pending=true;
            if (voice_owner) { pending_start=0;nt_stop(&test,test.session,NT_USER_STOP); }
        }
        taskEXIT_CRITICAL();
        return 1;
    }
    if (live_enabled && r->type!=RREQ_POWER_OFF) return 1;
    if (r->type==RREQ_POWER_OFF) live_enabled=live_rx_pending=live_tx_pending=false;
    if (r->type==RREQ_PTT && (r->arg==NATIVE_PHYSICAL_PTT_DOWN || r->arg==NATIVE_PHYSICAL_PTT_UP)) return 1;
#endif
    if (!native_lab_reserved()) return 0;
    taskENTER_CRITICAL();
    last_req_type=r->type;last_req_arg=(uint32_t)r->arg;last_req_ms=hal_millis();
    /* The programming plug shares PTT. RX audio never admits TX, so PTT
     * press/release are consumed rather than returning the receiver to FM. */
    if (voice_owner && r->type==RREQ_PTT) {
        if(ptt_consumed!=UINT32_MAX)ptt_consumed++;
        taskEXIT_CRITICAL();return 1;
    }
    if(cancellations!=UINT32_MAX)cancellations++;
    cancel_type=r->type;cancel_arg=(uint32_t)r->arg;
    taskEXIT_CRITICAL();
    taskENTER_CRITICAL(); pending_start=0; nt_stop(&test,test.session,NT_INTERRUPTED); taskEXIT_CRITICAL();
    native_lab_service(sm,hal_micros());
    return r->type==RREQ_PTT && r->arg!=0;
}
uint32_t native_lab_voice_start(uint32_t hz,uint32_t ms)
{
    taskENTER_CRITICAL();
    uint32_t sid=0;
    if (!updater() && board.native_modem && !voice_owner && !pending_start) {
        int armed=ms && nt_arm_voice(&test,hz,ms);
#if CONFIG_DEV_TX
        if (!ms && live_enabled && !live_tx_pending) armed=nt_arm_live_rx(&test,hz);
#endif
        if (armed) pending_start=voice_owner=sid=test.session;
    }
    taskEXIT_CRITICAL();
    return sid;
}
int native_lab_voice_poll(uint32_t sid,uint8_t *state,uint8_t *reason)
{
    taskENTER_CRITICAL();
    int ok=sid && sid==voice_owner && sid==test.session;
    if (ok) {
        if (test.state==NT_RX) nt_ping(&test,sid,hal_micros());
        *state=test.state;*reason=test.reason;
    }
    taskEXIT_CRITICAL();return ok;
}
int native_lab_voice_read(uint32_t sid,nt_record *r)
{
    taskENTER_CRITICAL();int ok=sid && sid==voice_owner && nt_pop(&test,sid,r);taskEXIT_CRITICAL();return ok;
}
void native_lab_voice_stop(uint32_t sid)
{
    taskENTER_CRITICAL();
    if (sid && sid==voice_owner) { pending_start=0;nt_stop(&test,sid,NT_USER_STOP); }
    taskEXIT_CRITICAL();
}
int native_lab_voice_release(uint32_t sid)
{
    int released=0;
    taskENTER_CRITICAL();
    if (sid && sid==voice_owner && !test.owned && test.state!=NT_ARMED) {
#if CONFIG_DEV_TX
        if (live_owner()) {
            live_summary[0]++;
            live_summary[1]=test.reason;
            live_summary[2]=test.stats.tx_sent;
            live_summary[3]=test.stats.pa_keys;
            live_summary[4]=test.stats.faults;
            if(live_enabled)live_rx_pending=true;
        }
#endif
        nt_record discard;
        while (nt_pop(&test,sid,&discard)) {}
        voice_owner=0;released=1;
#if CONFIG_P25_CHANNELS
        if(live_enabled && !channel_handoff) live_rx_pending=true;
#endif
    }
    taskEXIT_CRITICAL();return released;
}
#if CONFIG_DEV_TX
void native_lab_live_summary(uint32_t out[5])
{
    taskENTER_CRITICAL();
    for (unsigned i=0;i<5;i++) out[i]=live_summary[i];
    taskEXIT_CRITICAL();
}
int native_lab_live_poll(uint32_t sid,uint8_t *state,uint8_t *reason,uint8_t *queued)
{
    taskENTER_CRITICAL();
    int ok=live_owner() && sid==voice_owner && sid==test.session;
    if (ok) {
        if (test.state==NT_TX || test.state==NT_DRAIN) nt_ping(&test,sid,hal_micros());
        *state=test.state;
        *reason=test.reason;
        *queued=test.tx_count;
    }
    taskEXIT_CRITICAL();
    return ok;
}
int native_lab_live_block(uint32_t sid,const uint8_t raw[36],int last)
{
    taskENTER_CRITICAL();
    int ok=live_owner() && sid==voice_owner && nt_load(&test,sid,test.stats.tx_loaded,raw);
    if (ok && last) ok=nt_live_seal(&test,sid);
    taskEXIT_CRITICAL();
    return ok;
}
int native_lab_live_go(uint32_t sid)
{
    taskENTER_CRITICAL();
    int ok=live_owner() && sid==voice_owner && sid==test.session &&
        test.state==NT_ARMED && test.tx_count>=nt_tx_required(&test.config) && !pending_start;
    if (ok) pending_start=sid;
    taskEXIT_CRITICAL();
    return ok;
}
#endif
/* Private 0x21 protocol v1. Fixed sizes; no raw register write command.
 * ARM is pure software, START executes only on the radio task. TX requires
 * the explicit 'NP25' guard; it is an opt-in marker, not authentication. */
size_t native_lab_command(const uint8_t *p, uint16_t n, uint8_t *out, size_t cap)
{
    if (!cap) return 0;
    out[0]=DL_ST_BAD_ARG;
    if (!n || !p) return 1;
    int ok=0;
    if (p[0]==0 && n==1) {
        if (cap<1+4*NT_STATUS_WORDS) return 1;
        uint32_t words[NT_STATUS_WORDS];
        taskENTER_CRITICAL(); nt_status(&test,words); taskEXIT_CRITICAL();
        for (unsigned i=0;i<NT_STATUS_WORDS;i++) put(out+1+4*i,words[i]);
        out[0]=DL_ST_OK; return 1+4*NT_STATUS_WORDS;
    }
    if(p[0]==9 && n==1) {
        if(cap<33)return 1;
        taskENTER_CRITICAL();
        uint32_t words[]={1,last_req_type,last_req_arg,last_req_ms,ptt_consumed,cancellations,cancel_type,cancel_arg};
        for(unsigned i=0;i<8;i++)put(out+1+4*i,words[i]);
        taskEXIT_CRITICAL();out[0]=DL_ST_OK;return 33;
    }
#if CONFIG_DEV_TX && CONFIG_P25_CHANNELS && CONFIG_P25_SCAN
    /* DM32 Plus: the zone scan's P25 counters (docs/plus.md "P25 in a zone scan"), read only */
    if(p[0]==12 && n==1) {
        enum { WORDS=1+sizeof scan_n/sizeof(uint16_t) };
        if(cap<1+4*WORDS)return 1;
        taskENTER_CRITICAL();
        __typeof__(scan_n) c=scan_n;
        taskEXIT_CRITICAL();
        put(out+1,1);
        for(unsigned i=1;i<WORDS;i++)put(out+1+4*i,((const uint16_t *)&c)[i-1]);
        out[0]=DL_ST_OK;return 1+4*WORDS;
    }
#endif
#if CONFIG_PC_LIVE || CONFIG_QSO_LOG
    /* DM32 Plus, read only (docs/plus.md "PC live panel", "QSO log"): [13] the spectrum sweep while one runs (never
     * counted as a look at it, never starts or retunes), [14] the call history with its QSO marks. Each copies under
     * its owner's sequence word and is tried again a tick later while the owner was writing */
    if((p[0]==13 || p[0]==14) && n==1) {
        size_t len=0;
        for(unsigned tries=0;tries<4 && !len;tries++) {
            if(tries) vTaskDelay(1);
#if CONFIG_PC_LIVE && CONFIG_SPECTRUM
            if(p[0]==13) len=spectrum_export(out+1,cap-1);
#endif
#if CONFIG_QSO_LOG
            if(p[0]==14) len=qso_log_export(out+1,cap-1,hal_millis());
#endif
        }
        out[0]=len?DL_ST_OK:DL_ST_UNAVAILABLE;
        return 1+len;
    }
#endif
#if CONFIG_P25_CHANNELS
    if(p[0]!=11){out[0]=DL_ST_UNAVAILABLE;return 1;}
#endif
    if (!board.native_modem) { out[0]=DL_ST_UNAVAILABLE; return 1; }
    if (p[0]==8 && n==1) {
        if (!board.native_modem->snapshot) { out[0]=DL_ST_UNAVAILABLE; return 1; }
        if (cap<1+4*NT_SNAPSHOT_WORDS)return 1;
        uint32_t words[NT_SNAPSHOT_WORDS];
        taskENTER_CRITICAL();
        size_t count=board.native_modem->snapshot(board.native_modem->ctx,words,NT_SNAPSHOT_WORDS);
        taskEXIT_CRITICAL();
        if (count!=NT_SNAPSHOT_WORDS) { out[0]=DL_ST_UNAVAILABLE; return 1; }
        for (unsigned i=0;i<count;i++)put(out+1+4*i,words[i]);
        out[0]=DL_ST_OK;return 1+4*count;
    }
#if CONFIG_DEV_TX
    if (p[0]==11 && n==1 && cap>=1+4*NT_TRACE_WORDS) {
        uint32_t words[NT_TRACE_WORDS];
        taskENTER_CRITICAL();nt_trace_summary(words);taskEXIT_CRITICAL();
        for (unsigned i=0;i<NT_TRACE_WORDS;i++) put(out+1+4*i,words[i]);
        out[0]=DL_ST_OK;return 1+4*NT_TRACE_WORDS;
    }
    if (p[0]==11 && n==7) {
        taskENTER_CRITICAL();
        size_t count=nt_trace_read(u32(p+1),u16(p+5),out+1,cap-1);
        taskEXIT_CRITICAL();
        out[0]=count?DL_ST_OK:DL_ST_REFUSED;
        return 1+count;
    }
    if (p[0]==10 && n==2 && p[1]<=1) {
        taskENTER_CRITICAL();
        if (live_enabled==p[1]) out[0]=DL_ST_OK;
        else if (!p[1] || (!native_lab_reserved() && !p25_lab_busy())) {
            live_enabled=live_rx_pending=p[1]!=0;
            live_tx_pending=false;
            live_restore_pending=!live_enabled;
            if (!live_enabled && voice_owner) {
                pending_start=0;nt_stop(&test,test.session,NT_USER_STOP);
            }
            out[0]=DL_ST_OK;
        } else out[0]=DL_ST_BUSY;
        taskEXIT_CRITICAL();
        return 1;
    }
    if (live_enabled) { out[0]=DL_ST_BUSY;return 1; }
#endif
    if (voice_owner || updater()) { out[0]=DL_ST_BUSY;return 1; }
    if (p[0]==1 && n==17 && cap>=5) {
        nt_config c={.mode=p[1],.p25_sync=p[2],.blocks=u16(p+3),.duration_ms=u16(p+5),
                     .lease_ms=u16(p+7),.frequency_hz=u32(p+9)};
        taskENTER_CRITICAL();
        ok=!p25_lab_busy() && !pending_start && nt_arm(&test,&c,u32(p+13)==0x3532504eu);
        if (ok) put(out+1,test.session);
        taskEXIT_CRITICAL();
        out[0]=ok?DL_ST_OK:DL_ST_REFUSED; return ok?5:1;
    }
    if (n<5) return 1;
    uint32_t sid=u32(p+1);
    taskENTER_CRITICAL();
    if (p[0]==2 && n==43) ok=nt_load(&test,sid,u16(p+5),p+7);
    else if (p[0]==3 && n==5) {
        ok=sid==test.session && test.state==NT_ARMED && !pending_start &&
            (test.config.mode==NT_RECEIVE || test.tx_count>=nt_tx_required(&test.config));
        if (ok) pending_start=sid;
    } else if (p[0]==4 && n==5) ok=nt_ping(&test,sid,hal_micros());
    else if (p[0]==5 && n==5) { ok=nt_stop(&test,sid,NT_USER_STOP); if (ok) pending_start=0; }
    else if (p[0]==6 && n==5 && cap>=53) {
        nt_record r;
        ok=nt_pop(&test,sid,&r);
        if (ok) {
            put(out+1,r.sequence); put(out+5,r.time_us); put(out+9,r.sys_flags); put(out+13,r.rx_info);
            for (unsigned i=0;i<36;i++) out[17+i]=r.raw[i];
            taskEXIT_CRITICAL(); out[0]=DL_ST_OK; return 53;
        }
    } else if (p[0]==7 && n==5 && sid==test.session) ok=nt_reset(&test);
    else { taskEXIT_CRITICAL(); return 1; }
    taskEXIT_CRITICAL();
    out[0]=ok?DL_ST_OK:DL_ST_REFUSED; return 1;
}
