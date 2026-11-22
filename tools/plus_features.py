# SPDX-License-Identifier: GPL-3.0-or-later
"""DM32 Plus additions to the pinned RadioFoundry tree, applied after the Classic patches (docs/plus.md).

Each feature sits behind its own CONFIG_ switch; with every switch off the tree is Classic's.
"""


def prepare(transform, install):
    # Spectrum / close call (CONFIG_SPECTRUM): engine in the radio core, its screen under Tools
    install('core/radio/spectrum.h', 'integration/radiofoundry/spectrum/spectrum.h')
    install('core/radio/spectrum.c', 'integration/radiofoundry/spectrum/spectrum.c')
    install('core/ui/ui_spectrum.c', 'integration/radiofoundry/spectrum/ui_spectrum.c')
    transform('core/radio/radio_sm.h', [
        ("\tRREQ_VFO_TX,\t\t// arg: the VFO's TX Hz, 0 = simplex (its RX frequency)\n} radio_req_type_t;",
         "\tRREQ_VFO_TX,\t\t// arg: the VFO's TX Hz, 0 = simplex (its RX frequency)\n"
         "\tRREQ_SPECTRUM,\t\t// DM32 Plus: arg center Hz (0 = stop), arg2 span Hz (radio/spectrum.h)\n} radio_req_type_t;"),
    ])
    transform('core/radio/radio_task.c', [
        ('#include "radio/radio_sm.h"\n', '#include "radio/radio_sm.h"\n#if CONFIG_SPECTRUM\n#include "radio/spectrum.h"\n#endif\n'),
        ('void radio_req_scan_dual(uint16_t pos) { post_pair(RREQ_SCAN, RSCAN_DUAL, pos, 0); }\n',
         'void radio_req_scan_dual(uint16_t pos) { post_pair(RREQ_SCAN, RSCAN_DUAL, pos, 0); }\n'
         '#if CONFIG_SPECTRUM\nvoid radio_req_spectrum(uint32_t center_hz, uint32_t span_hz)\n'
         '{ post_pair(RREQ_SPECTRUM, (int32_t)center_hz, span_hz, 0); }\n#endif\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#include "radio/tones.h"\n', '#include "radio/tones.h"\n#if CONFIG_SPECTRUM\n#include "radio/spectrum.h"\n#endif\n'),
        # The sweep has the receiver: read, retune, nothing else of the receive path (speaker muted in outputs)
        ('#if CONFIG_BATTERY_SAVE\n\tif (save_poll(sm, now_ms)) {', '''#if CONFIG_SPECTRUM
	if (spectrum_on()) {
		static uint32_t spectrum_shown_ms;
		board.radio->status(&st);
		uint32_t hz = spectrum_service(st.rssi_dbm, now_ms);
		if (!spectrum_on()) {
			tune(sm, now_ms);	// nobody looks at the sweep any more: the channel again
		} else if (hz != 0) {
			static const subtone_t spectrum_no_tone = { TONE_NONE, 0 };
			board.radio->rx(hz, RADIO_BW_12K5, &spectrum_no_tone);
		}
		if (now_ms - spectrum_shown_ms >= 250u) {
			spectrum_shown_ms = now_ms;
			changed(sm);	// the UI redraws the sweep about four times a second
		}
		outputs(sm);
		return;
	}
#endif
#if CONFIG_BATTERY_SAVE
	if (save_poll(sm, now_ms)) {'''),
        ('\tbool green = open;', '#if CONFIG_SPECTRUM\n\tif (spectrum_on()) open = false;\n#endif\n\tbool green = open;'),
        ('\tif (!alert) {\n\t\tactivity(sm, now_ms);\n\t}', '''\tif (!alert) {
		activity(sm, now_ms);
	}
#if CONFIG_SPECTRUM
	// Anything the user does but the sweep's own requests ends the sweep first: the channel is tuned again
	if (!alert && req->type != RREQ_SPECTRUM && req->type != RREQ_SETTINGS && req->type != RREQ_RETUNE &&
	    req->type != RREQ_DMR_FOCUS && spectrum_on()) {
		spectrum_stop();
		tune(sm, now_ms);
	}
#endif'''),
        ('\tcase RREQ_VFO_TX: {', '''#if CONFIG_SPECTRUM
	case RREQ_SPECTRUM: {
		if (req->arg == 0) {
			if (spectrum_on()) {
				spectrum_stop();
				tune(sm, now_ms);
			}
			break;
		}
		bool aprs_busy = false;
#if CONFIG_APRS
		aprs_busy = sm->aprs_phase != APRS_RF_OFF;
#endif
		if (busy || sm->ptt || aprs_busy)
			break;
		if (scan_on(&sm->scan))
			scan_stop(sm, false, now_ms);
		if (sm->board_mode == RADIO_MODE_DMR)
			dmr_end(sm);
		if (sm->board_mode != RADIO_MODE_FM) {
			sm->board_mode = RADIO_MODE_FM;
			board.radio->set_mode(RADIO_MODE_FM);
		}
		static const subtone_t sweep_no_tone = { TONE_NONE, 0 };
		board.radio->rx(spectrum_start((uint32_t)req->arg, req->arg2, now_ms), RADIO_BW_12K5, &sweep_no_tone);
		outputs(sm);
		changed(sm);
		break;
	}
#endif
	case RREQ_VFO_TX: {'''),
    ])
    transform('core/ui/ui_nav.h', [
        ('\tSCR_COUNT\n} ui_scr_t;', '\tSCR_SPECTRUM,\t\t\t// DM32 Plus: Tools > Spectrum (CONFIG_SPECTRUM)\n\tSCR_COUNT\n} ui_scr_t;'),
    ])
    transform('core/ui/ui_menus.c', [
        ('extern const ui_scr_def_t ui_gps_screen;', 'extern const ui_scr_def_t ui_gps_screen;\n'
         'extern const ui_scr_def_t ui_spectrum_screen;\t// ui_spectrum.c (DM32 Plus)'),
        ('#if CONFIG_GPS_SKY\n\t[SCR_GPS] = &ui_gps_screen,', '#if CONFIG_SPECTRUM\n\t[SCR_SPECTRUM] = &ui_spectrum_screen,\n#endif\n'
         '#if CONFIG_GPS_SKY\n\t[SCR_GPS] = &ui_gps_screen,'),
        ('T_DIAG, T_APRS, T_BATTERY, T_ZONE_OFFER, T_COUNT };', 'T_DIAG, T_APRS, T_SPECTRUM, T_BATTERY, T_ZONE_OFFER, T_COUNT };'),
        ('\treturn (i != T_DIAG || CONFIG_DIAG) &&', '\treturn (i != T_DIAG || CONFIG_DIAG) && (i != T_SPECTRUM || CONFIG_SPECTRUM) &&'),
        ('"APRS",\n\t\t\t\t\t     TRN("Battery"),', '"APRS",\n\t\t\t\t\t     TRN("Spectrum"), TRN("Battery"),'),
        ('\tcase T_APRS: ui_push(SCR_APRS, 0); break;', '\tcase T_APRS: ui_push(SCR_APRS, 0); break;\n\tcase T_SPECTRUM: ui_push(SCR_SPECTRUM, 0); break;'),
    ])
    transform('config/features.mk', [
        ('CONFIG_P25_CHANNELS ?= n\n', 'CONFIG_P25_CHANNELS ?= n\nCONFIG_SPECTRUM ?= n\nFEATURES += SPECTRUM\n'
         'CONFIG_P25_SCAN ?= n\nFEATURES += P25_SCAN\n'
         'ifeq ($(CONFIG_P25_SCAN),y)\nifneq ($(CONFIG_P25_CHANNELS)$(CONFIG_SCAN),yy)\n'
         '$(error P25_SCAN needs P25_CHANNELS=y and SCAN=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_P25_CHANNELS\n', '#ifndef CONFIG_SPECTRUM\n#define CONFIG_SPECTRUM 0\n#endif\n'
         '#ifndef CONFIG_P25_SCAN\n#define CONFIG_P25_SCAN 0\n#endif\n'
         '#ifndef CONFIG_P25_CHANNELS\n'),
    ])
    # P25 scan: a P25 channel is scanned like an FM one (the receiver's noise reading finds the carrier). When the
    # scan stops on one, the native P25 receiver takes the channel (native_lab.c channel_service) and gives it back
    # once the call has ended, so the scan's own wait and resume carry on.
    transform('core/radio/radio_sm.c', [
        ('codeplug_channel(sm->zone.channels[pos], &ch) && !ch.scan_skip && !codeplug_mode_p25(ch.mode);',
         'codeplug_channel(sm->zone.channels[pos], &ch) && !ch.scan_skip &&\n'
         '#if CONFIG_P25_SCAN\n\t       (!codeplug_mode_p25(ch.mode) || codeplug_p25_valid(&ch));\n'
         '#else\n\t       !codeplug_mode_p25(ch.mode);\n#endif'),
        ('\t/* P25 currently supports selected-channel receive, not scan dwell. */\n'
         '\tif (codeplug_mode_p25(sm->view.ch.mode)) return;',
         '\t/* Classic: selected-channel P25 receive only. Plus scans P25 channels too (CONFIG_P25_SCAN). */\n'
         '\tif (!CONFIG_P25_SCAN && codeplug_mode_p25(sm->view.ch.mode)) return;'),
        # Dual watch: a P25 channel may be the other one (its receiver takes it while the watch holds there)
        ('!codeplug_channel(sm->zone.channels[req->arg2], &other) || codeplug_mode_p25(other.mode)) break;',
         '!codeplug_channel(sm->zone.channels[req->arg2], &other) ||\n'
         '#if CONFIG_P25_SCAN\n\t\t    (codeplug_mode_p25(other.mode) && !codeplug_p25_valid(&other))) break;\n'
         '#else\n\t\t    codeplug_mode_p25(other.mode)) break;\n#endif'),
    ])
    prepare_fox(transform, install)
    prepare_cw_beacon(transform, install)
    prepare_lone_worker(transform, install)
    prepare_almanac(transform, install)
    prepare_nav(transform, install)
    prepare_p25_records(transform)
    prepare_ysf(transform, install)
    prepare_p25_search(transform, install)
    prepare_p25_any(transform)
    prepare_p25_zone_scan(transform)
    prepare_plus_ui(transform)
    prepare_turkish(transform, install)
    prepare_call_replay(transform)
    prepare_map(transform, install)
    prepare_nxdn(transform, install)
    prepare_dmr_any(transform)
    prepare_dmr_search(transform, install)
    prepare_sfr_edit(transform)
    prepare_call_log(transform)
    prepare_signal_quality(transform, install)
    prepare_satellites(transform, install)
    prepare_art(transform, install)
    prepare_ram_trim(transform)
    prepare_fm_id(transform, install)
    prepare_messages(transform, install)
    prepare_sstv(transform, install)
    prepare_aprs_msg(transform, install)
    prepare_id_db(transform, install)
    prepare_iss_aprs(transform)
    prepare_p25_trunk(transform, install)
    prepare_dmr_trunk(transform, install)
    prepare_dpmr(transform, install)
    prepare_repeaters(transform, install)
    prepare_nac_decimal(transform)
    prepare_hotspot(transform)
    prepare_multi_watch(transform, install)
    prepare_mapnav(transform, install)
    prepare_sat_alarm(transform)
    prepare_p25_extras(transform)
    prepare_p25_mode2(transform)
    __import__((__package__ + "." if __package__ else "") + "plus_vfox", fromlist=["x"]).prepare_vfo_modes(transform, install)
    prepare_fm_decoders(transform, install)
    prepare_scanx(transform, install)
    prepare_ysf_tx(transform, install)
    prepare_misc(transform, install)
    prepare_cw_rx(transform, install)
    prepare_rpt_prov(transform)
    __import__((__package__ + "." if __package__ else "") + "plus_p25nac", fromlist=["x"]).prepare_p25_txnac(transform, install)
    __import__((__package__ + "." if __package__ else "") + "plus_brand", fromlist=["x"]).prepare_brand(transform)
    __import__((__package__ + "." if __package__ else "") + "plus_fieldx", fromlist=["x"]).prepare_fieldx(transform, install)
    __import__((__package__ + "." if __package__ else "") + "plus_toolbox", fromlist=["x"]).prepare_toolbox(transform, install)
    prepare_large_scale(transform)
    prepare_home_freq(transform)
    __import__((__package__ + "." if __package__ else "") + "plus_dota", fromlist=["x"]).prepare_dota(transform, install)
    __import__((__package__ + "." if __package__ else "") + "plus_aprs", fromlist=["x"]).prepare_aprs(transform, install)
    __import__((__package__ + "." if __package__ else "") + "plus_air", fromlist=["x"]).prepare_air(transform, install)
    __import__((__package__ + "." if __package__ else "") + "plus_datapack", fromlist=["x"]).prepare_datapack(transform, install)
    __import__((__package__ + "." if __package__ else "") + "plus_xband", fromlist=["x"]).prepare_xband(transform, install)
    __import__((__package__ + "." if __package__ else "") + "plus_dmraudio", fromlist=["x"]).prepare_dmraudio(transform, install)
    __import__((__package__ + "." if __package__ else "") + "plus_chux", fromlist=["x"]).prepare_chux(transform, install)
    __import__((__package__ + "." if __package__ else "") + "plus_scan", fromlist=["x"]).prepare_scan(transform, install)
    __import__((__package__ + "." if __package__ else "") + "plus_sigbar", fromlist=["x"]).prepare_sigbar(transform, install)
    __import__((__package__ + "." if __package__ else "") + "plus_airprov", fromlist=["x"]).prepare_airprov(transform, install)
    __import__((__package__ + "." if __package__ else "") + "plus_polish", fromlist=["x"]).prepare_polish(transform, install)
    __import__((__package__ + "." if __package__ else "") + "plus_searchlog", fromlist=["x"]).prepare_searchlog(transform, install)
    __import__((__package__ + "." if __package__ else "") + "plus_alldigital", fromlist=["x"]).prepare_alldigital(transform, install)
    __import__((__package__ + "." if __package__ else "") + "plus_satauto", fromlist=["x"]).prepare_sat_auto(transform, install)
    __import__((__package__ + "." if __package__ else "") + "plus_sstvtx", fromlist=["x"]).prepare_sstvtx(transform, install)
    __import__((__package__ + "." if __package__ else "") + "plus_rfports", fromlist=["x"]).prepare_rfports(transform, install)


def prepare_large_scale(transform):
    """Large text on wide panels (CONFIG_LARGE_SCALE, owner's report 2026-11-20): on 240 px panels Home's channel
    font is already the largest font (16x32), so Settings > Display > Large text only took the other rows away and
    left the screen looking empty. There each of its two lines is drawn 2x, 1.5x or 4/3 (gfx_text_scaled: every glyph
    pixel a block, anti-aliased edges blended), else 1x, the largest that fits with the text's alternates, centred in
    its half; a line that needs shortening even at 1x is drawn as before. Runs last: it anchors on the final draw_home_large and gfx.c."""
    transform('config/features.mk', [
        ('CONFIG_PLUS_ART ?= n\nFEATURES += PLUS_ART\n',
         'CONFIG_PLUS_ART ?= n\nFEATURES += PLUS_ART\nCONFIG_LARGE_SCALE ?= n\nFEATURES += LARGE_SCALE\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_PLUS_ART\n',
         '#ifndef CONFIG_LARGE_SCALE\n#define CONFIG_LARGE_SCALE 0\n#endif\n#ifndef CONFIG_PLUS_ART\n'),
    ])
    transform('core/ui/gfx.h', [
        ('int gfx_text_bg(const font_t *f, int x, int y, const char *s, color_t fg, color_t bg);\n',
         'int gfx_text_bg(const font_t *f, int x, int y, const char *s, color_t fg, color_t bg);\n'
         '#if CONFIG_LARGE_SCALE\n'
         '// DDM32 Plus: s in f at num/den of its size (2/1, 3/2): each glyph pixel a block. Returns the x after it\n'
         'int gfx_text_scaled(const font_t *f, int x, int y, const char *s, color_t fg, int num, int den);\n'
         '#endif\n'),
    ])
    transform('core/ui/gfx.c', [
        ('\nint gfx_text(const font_t *f, int x, int y, const char *s, color_t fg)\n{\n',
         '\n#if CONFIG_LARGE_SCALE\n'
         '// DDM32 Plus: the text loop above with every glyph pixel drawn as a num/den block (anti-aliased edges blended\n'
         '// per pixel of the block). A glyph the font lacks is drawn from primitives at the font\'s own size, centred\n'
         'int gfx_text_scaled(const font_t *f, int x, int y, const char *s, color_t fg, int num, int den)\n{\n'
         '\tchar first[ALT_TEXT_MAX];\n'
         '\tif (strchr(s, TEXT_ALT_OPEN[0])) {\n\t\tbool more;\n\t\talt_expand(s, 0, first, sizeof first, &more);\n'
         '\t\ts = first;\n\t}\n'
         '#ifdef GFX_FIT_TRACE\n\tconst char *s0 = s;\n\tint x0 = x;\n#endif\n'
         '\tint cw = f->w * num / den, ch = f->h * num / den;\n'
         '\tint bytes = (f->w * f->bpp + 7) / 8;\n'
         '\tfor (; *s; s++, x += cw) {\n'
         '\t\tint px = x + ox, py = y + oy;\n'
         '\t\tif (px >= clip_x1)\n\t\t\tbreak;\n'
         '\t\tif (px + cw <= clip_x0 || py >= clip_y1 || py + ch <= clip_y0)\n\t\t\tcontinue;\n'
         '\t\tint top, rows;\n'
         '\t\tconst uint8_t *g = font_glyph(f, (uint8_t)*s, &top, &rows);\n'
         '\t\tif (g == NULL) {\n'
         '\t\t\tfallback_glyph(f, x + (cw - f->w) / 2, y + (ch - f->h) / 2, (uint8_t)*s, fg);\n'
         '\t\t\tcontinue;\n\t\t}\n'
         '\t\tfor (int r = 0; r < rows; r++, g += bytes) {\n'
         '\t\t\tint by0 = py + (top + r) * num / den, by1 = py + (top + r + 1) * num / den;\n'
         '\t\t\tfor (int col = 0; col < f->w; col++) {\n'
         '#if CONFIG_FONT_AA\n'
         '\t\t\t\tunsigned a = f->bpp == 2 ? (g[col >> 2] >> (6 - 2 * (col & 3))) & 3u :\n'
         '\t\t\t\t\t     (g[col >> 3] & (0x80u >> (col & 7))) ? 3u : 0;\n'
         '#else\n'
         '\t\t\t\tunsigned a = (g[col >> 3] & (0x80u >> (col & 7))) ? 3u : 0;\n'
         '#endif\n'
         '\t\t\t\tif (a == 0)\n\t\t\t\t\tcontinue;\n'
         '\t\t\t\tint bx0 = px + col * num / den, bx1 = px + (col + 1) * num / den;\n'
         '\t\t\t\tfor (int yy = by0; yy < by1; yy++) {\n'
         '\t\t\t\t\tif (yy < clip_y0 || yy >= clip_y1)\n\t\t\t\t\t\tcontinue;\n'
         '\t\t\t\t\tfor (int xx = bx0; xx < bx1; xx++) {\n'
         '\t\t\t\t\t\tif (xx < clip_x0 || xx >= clip_x1)\n\t\t\t\t\t\t\tcontinue;\n'
         '#if CONFIG_FONT_AA\n'
         '\t\t\t\t\t\tif (a != 3) {\n\t\t\t\t\t\t\tedge_pixel(xx, yy, fg, a);\n\t\t\t\t\t\t\tcontinue;\n'
         '\t\t\t\t\t\t}\n'
         '#endif\n'
         '\t\t\t\t\t\tspan(xx, xx + 1, yy, fg);\n'
         '\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n'
         '#ifdef GFX_FIT_TRACE\n\ttrace(GFX_TRACE_TEXT, x0, y, (int)strlen(s0) * cw, ch, s0);\n#endif\n'
         '\treturn x;\n}\n#endif\n\n'
         'int gfx_text(const font_t *f, int x, int y, const char *s, color_t fg)\n{\n'),
    ])
    transform('core/ui/ui_screens.c', [
        ('// Large-text Home (accessibility): the status strip, the channel and the talk target, each as large as fits\n',
         '#if CONFIG_LARGE_SCALE\n'
         '// DDM32 Plus: a Large text line where the largest font is already Home\'s channel font (240 px panels), so the\n'
         '// setting would make nothing larger: drawn 2x, 1.5x or 4/3, else at 1x, the largest that fits with its\n'
         '// alternates, centred in its half. False (even 1x needs shortening): drawn as before\n'
         'static bool __attribute__((noinline)) large_scaled(const char *text, int y, int room, color_t c, char *out,\n'
         '\t\t\t\t\t\t   size_t cap)\n{\n'
         '\tconst font_t *f = font_largest;\n'
         '\tif (f != ui_layout.large)\n\t\treturn false;\n'
         '\tstatic const uint8_t sc[4][2] = { { 2, 1 }, { 3, 2 }, { 4, 3 }, { 1, 1 } };\n'
         '\tfor (unsigned i = 0; i < 4u; i++) {\n'
         '\t\tint n = sc[i][0], d = sc[i][1], h = f->h * n / d;\n'
         '\t\tif (h > room)\n\t\t\tcontinue;\n'
         '\t\tbool more = true;\n'
         '\t\tfor (int level = 0; more; level++) {\n'
         '\t\t\tgfx_text_alt(text, level, out, cap, &more);\n'
         '\t\t\tif ((int)strlen(out) * (f->w * n / d) <= gfx_width()) {\n'
         '\t\t\t\tgfx_text_scaled(f, 0, y + (room - h) / 2, out, c, n, d);\n'
         '\t\t\t\treturn true;\n\t\t\t}\n\t\t}\n\t}\n'
         '\treturn false;\n}\n#endif\n\n'
         '// Large-text Home (accessibility): the status strip, the channel and the talk target, each as large as fits\n'),
        ('\tconst font_t *f = large_font(buf, room, form, sizeof form);\n'
         '\tgfx_text_fit(f, form, w, fit, sizeof fit);\n'
         '\tgfx_text(f, 0, y, fit, tx ? ui_col.tx : ui_col.fg);\n',
         '\tconst font_t *f;\n'
         '#if CONFIG_LARGE_SCALE\n'
         '\tif (!large_scaled(buf, y, room, tx ? ui_col.tx : ui_col.fg, form, sizeof form))\n'
         '#endif\n'
         '\t{\n'
         '\t\tf = large_font(buf, room, form, sizeof form);\n'
         '\t\tgfx_text_fit(f, form, w, fit, sizeof fit);\n'
         '\t\tgfx_text(f, 0, y, fit, tx ? ui_col.tx : ui_col.fg);\n'
         '\t}\n'),
        ('\tf = large_font(buf, room, form, sizeof form);\n'
         '\tgfx_text_fit(f, form, w, fit, sizeof fit);\n'
         '\tgfx_text(f, 0, y, fit, v->state == RS_TX ? ui_col.tx : v->state == RS_RX ? ui_col.rx : ui_col.fg);\n',
         '\tcolor_t c2 = v->state == RS_TX ? ui_col.tx : v->state == RS_RX ? ui_col.rx : ui_col.fg;\n'
         '#if CONFIG_LARGE_SCALE\n'
         '\tif (large_scaled(buf, y, room, c2, form, sizeof form))\n\t\treturn;\n'
         '#endif\n'
         '\tf = large_font(buf, room, form, sizeof form);\n'
         '\tgfx_text_fit(f, form, w, fit, sizeof fit);\n'
         '\tgfx_text(f, 0, y, fit, c2);\n'),
    ])


def prepare_home_freq(transform):
    """The channel's frequency on Home (CONFIG_HOME_FREQ, owner 2026-11-20): a channel's receive frequency was shown
    nowhere on Home (FM's talk row has the TX one, DMR's the talk target). On panels with room (240x320) a row under
    the chips reads "RX 438.5000  TX 430.9000", or "438.5000 MHz" when both are the same; the VFO shows its frequency
    large already. Large text gets a third line, the frequency (TX while transmitting), between the channel and the
    talk target. Runs after prepare_large_scale: it anchors on that patch."""
    transform('config/features.mk', [
        ('CONFIG_LARGE_SCALE ?= n\nFEATURES += LARGE_SCALE\n',
         'CONFIG_LARGE_SCALE ?= n\nFEATURES += LARGE_SCALE\nCONFIG_HOME_FREQ ?= n\nFEATURES += HOME_FREQ\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_LARGE_SCALE\n',
         '#ifndef CONFIG_HOME_FREQ\n#define CONFIG_HOME_FREQ 0\n#endif\n#ifndef CONFIG_LARGE_SCALE\n'),
    ])
    transform('core/ui/ui_screens.h', [
        ('\tint channel_y, talk_y, chips_y;\n',
         '\tint channel_y, talk_y, chips_y;\n'
         '#if CONFIG_HOME_FREQ\n\tint freq_y;\t\t// DDM32 Plus: the channel\'s frequencies under the chips; < 0: no room\n#endif\n'),
    ])
    transform('core/ui/ui_screens.c', [
        # The row only where Home keeps room for activity under it (five small rows), so smaller panels keep theirs
        ('\tL->chips_y = y;\n\ty += L->small->h;\n',
         '\tL->chips_y = y;\n\ty += L->small->h;\n'
         '#if CONFIG_HOME_FREQ\n'
         '\tL->freq_y = L->hint_y - y >= 7 * L->small->h ? y : -1;\n'
         '\tif (L->freq_y >= 0)\n\t\ty += L->small->h;\n'
         '#endif\n'),
        ('static void draw_chips(const radio_view_t *v, const ui_state_t *s)\n{\n',
         '#if CONFIG_HOME_FREQ\n'
         '// DDM32 Plus: "RX 438.5000  TX 430.9000" under the chips, one number when both are the same (TX off or RX only:\n'
         '// the receive one). Not in the VFO: its frequency is the large line\n'
         'static void __attribute__((noinline)) draw_home_freq(const radio_view_t *v)\n{\n'
         '\tconst ui_layout_t *L = &ui_layout;\n'
         '\tif (L->freq_y < 0 || v->vfo || v->ch.rx_hz == 0)\n\t\treturn;\n'
         '\tchar buf[40], fit[40];\n\tsbuf_t b;\n\tsb_init(&b, buf, sizeof buf);\n'
         '\tbool split = !v->ch.rx_only && v->ch.tx_hz != 0 && v->ch.tx_hz != v->ch.rx_hz;\n'
         '\tif (split)\n\t\tsb_str(&b, "RX ");\n'
         '\tsb_freq(&b, v->ch.rx_hz);\n'
         '\tif (split) {\n\t\tsb_str(&b, "  TX ");\n\t\tsb_freq(&b, v->ch.tx_hz);\n\t} else {\n\t\tsb_str(&b, " MHz");\n\t}\n'
         '\tgfx_text_fit(L->small, buf, gfx_width(), fit, sizeof fit);\n'
         '\tgfx_text(L->small, 0, L->freq_y, fit, ui_col.dim);\n}\n#endif\n\n'
         'static void draw_chips(const radio_view_t *v, const ui_state_t *s)\n{\n'),
        ('\t\tdraw_chips(v, s);\n\t\tif (ui_layout.sep_y >= 0)\n',
         '\t\tdraw_chips(v, s);\n#if CONFIG_HOME_FREQ\n\t\tdraw_home_freq(v);\n#endif\n\t\tif (ui_layout.sep_y >= 0)\n'),
        # Large text: three lines for a channel (name, frequency, talk target), two in the VFO (its line is the
        # frequency)
        ('\tint room = (L->hint_y - y) / 2;\n\tchar form[48];\n',
         '#if CONFIG_HOME_FREQ\n'
         '\tint room = (L->hint_y - y) / (v->vfo ? 2 : 3);\n'
         '#else\n'
         '\tint room = (L->hint_y - y) / 2;\n'
         '#endif\n'
         '\tchar form[48];\n'),
        ('\t\tgfx_text(f, 0, y, fit, tx ? ui_col.tx : ui_col.fg);\n\t}\n\ty += room;\n',
         '\t\tgfx_text(f, 0, y, fit, tx ? ui_col.tx : ui_col.fg);\n\t}\n\ty += room;\n'
         '#if CONFIG_HOME_FREQ\n'
         '\tif (!v->vfo) {\n'
         '\t\tsb_init(&b, buf, sizeof buf);\n'
         '\t\tsb_freq(&b, tx ? v->ch.tx_hz : v->ch.rx_hz);\n'
         '#if CONFIG_LARGE_SCALE\n'
         '\t\tif (!large_scaled(buf, y, room, tx ? ui_col.tx : ui_col.dim, form, sizeof form))\n'
         '#endif\n'
         '\t\t{\n'
         '\t\t\tf = large_font(buf, room, form, sizeof form);\n'
         '\t\t\tgfx_text_fit(f, form, w, fit, sizeof fit);\n'
         '\t\t\tgfx_text(f, 0, y, fit, tx ? ui_col.tx : ui_col.dim);\n'
         '\t\t}\n'
         '\t\ty += room;\n'
         '\t}\n'
         '#endif\n'),
    ])


def prepare_sat_alarm(transform):
    """Satellites' pass alarm (part of CONFIG_SATELLITES, docs/plus.md "Satellites: pass alarm and Doppler tracking"):
    armed on the Satellites screen (radio/sat_svc.c finds the pass and keeps the time; integration/radiofoundry/sat/),
    it sounds on any screen: ui_app_tick asks ui_sat_alarm (ui_sat.c) each tick and shows its text as an info banner
    (a toast over a banner that waits for the user) with the backlight on, as the low battery warning does."""
    transform('core/ui/ui_app.c', [
        ('#if CONFIG_HOTSPOT\n#include "hotspot/hotspot.h"\n#endif\n',
         '#if CONFIG_HOTSPOT\n#include "hotspot/hotspot.h"\n#endif\n#if CONFIG_SATELLITES\n#include "radio/sat.h"\n#endif\n'),
        ('\tif (ui_st.toast != NULL && (int32_t)(now - toast_until) >= 0) {\n',
         '#if CONFIG_SATELLITES\n'
         '\t// DM32 Plus Satellites: a pass alarm due (ui_sat.c) has sounded its tone; its text goes up on any screen\n'
         '\tconst char *sat_alarm = ui_sat_alarm();\n'
         '\tif (sat_alarm != NULL) {\n'
         '\t\tif (ui_st.banner.hold)\n'
         '\t\t\tui_toast(sat_alarm);\t// the zone offer stays up: it waits for the user\n'
         '\t\telse\n'
         '\t\t\tbanner(UI_BANNER_INFO, false, 0, sat_alarm, NULL);\n'
         '\t\twake_req = true;\n'
         '\t\tdirty = true;\n'
         '\t}\n'
         '#endif\n'
         '\tif (ui_st.toast != NULL && (int32_t)(now - toast_until) >= 0) {\n'),
    ])


def prepare_hotspot(transform):
    """DM32 Plus MMDVM hotspot (CONFIG_HOTSPOT, docs/plus.md "Hotspot"): upstream's hotspot runs as is on the host
    link, which on the DM32 is the programming cable's UART (115200 8N1, platform/hrc7000/uart.c). Tools > Hotspot's
    page said "on USB"; a radio without USB says the cable. (The P25 channel receiver stays off in hotspot mode:
    integration/radiofoundry/native/native_lab.c channel_service.)"""
    transform('core/ui/ui_menus.c', [
        ('\t\tsb_str(&b, TR("Start MMDVMHost or Pi-Star on a computer on USB: hotspot mode starts by itself."));\n',
         '#if CONFIG_USB\n'
         '\t\tsb_str(&b, TR("Start MMDVMHost or Pi-Star on a computer on USB: hotspot mode starts by itself."));\n'
         '#else\t// DM32 Plus: the host link is the programming cable\n'
         '\t\tsb_str(&b, TR("Start MMDVMHost, Pi-Star or WPSD on a computer on the programming cable (115200 baud): "\n'
         '\t\t\t\t"hotspot mode starts by itself."));\n'
         '#endif\n'),
    ])


def prepare_iss_aprs(transform):
    """ISS APRS digipeater from Satellites (CONFIG_SATELLITES, which needs APRS; docs/plus.md): on the Satellites
    screen, # on the ISS while it is above the horizon sends one position beacon through its digipeater, 145.825 MHz
    FM simplex corrected for the uplink Doppler of the press, path ARISS (integration/radiofoundry/sat/ui_sat.c).
    The engine (core/aprs/aprs.c) checks callsign, position, an amateur frequency the radio sends on, AFSK, and at
    least 30 s since the last press; the radio task builds the frame at its next tick and the RF job sends it alone, on
    its own frequency (CSMA there first), whatever the APRS region frequency is. Only on a key press, never automatic.
    Runs last: it anchors on prepare_aprs_msg's SOS call in aprs_tick (and keeps every anchor it uses)."""
    transform('core/aprs/aprs.h', [
        ('// RF job for the radio core\n', '''#if CONFIG_SATELLITES
// DM32 Plus Satellites: one position beacon through the ISS's APRS digipeater on hz (145.825 MHz with the Doppler
// correction), path APRS_ISS_PATH, on an explicit key press (any task). APRS_OK, or why not: APRS_NO_CALL,
// APRS_NOT_AMATEUR (hz not in a ham band this radio sends on), APRS_NO_AFSK, APRS_NO_POSITION, APRS_ISS_WAIT (less
// than APRS_ISS_GAP_MS since the last one), APRS_BUSY (one still waiting, or the queue full). The radio task queues it
// at its next tick; it goes alone in its transmission on hz (CSMA there), not on the APRS frequency
#define APRS_ISS_PATH		"ARISS"
#define APRS_ISS_GAP_MS		30000u
#define APRS_ISS_WAIT		0x40u
uint8_t aprs_iss_send(uint32_t hz);
#endif

// RF job for the radio core
'''),
    ])
    transform('core/aprs/aprs.c', [
        ('enum { K_BEACON, K_STATUS, K_MSG, K_ACK, K_HOST, K_GATE, K_DIGI };\n',
         'enum { K_BEACON, K_STATUS, K_MSG, K_ACK, K_HOST, K_GATE, K_DIGI };\n'
         '#if CONFIG_SATELLITES\n'
         '#define K_ISS\t(K_DIGI + 1)\t// DM32 Plus: a position for the ISS digipeater (aprs_iss_send), alone on iss.hz\n'
         'static struct {\n'
         '\tuint32_t hz;\t\t// where it goes: 145.825 MHz with the Doppler of the press\n'
         '\tuint32_t last_ms;\t// the last press taken (APRS_ISS_GAP_MS to the next)\n'
         '\tbool sent;\t\t// any since start-up\n'
         '\tbool due;\t\t// the radio task queues it at its next tick\n'
         '} iss;\n#endif\n'),
        ('\tmemset(&a, 0, sizeof(a));\n',
         '\tmemset(&a, 0, sizeof(a));\n#if CONFIG_SATELLITES\n\tmemset(&iss, 0, sizeof(iss));\n#endif\n'),
        ('\n// --- Receive ---\n', '''
#if CONFIG_SATELLITES
// DM32 Plus: an ISS frame waits in the queue (lock held)
static bool iss_queued(void)
{
	for (uint8_t i = 0; i < a.qn; i++) {
		if (a.q[i].kind == K_ISS) {
			return true;
		}
	}
	return false;
}

uint8_t aprs_iss_send(uint32_t hz)
{
	LOCK();
	uint8_t src;
	uint8_t why = !a.me_ok ? APRS_NO_CALL :
		      !aprs_amateur_hz(hz) || !band_tx(hz) ? APRS_NOT_AMATEUR :
		      board.afsk == NULL || board.afsk->tx_start == NULL ? APRS_NO_AFSK :
		      position(&src) == NULL ? APRS_NO_POSITION :
		      iss.sent && a.now - iss.last_ms < APRS_ISS_GAP_MS ? APRS_ISS_WAIT :
		      iss.due || iss_queued() || a.qn >= QUEUE_LEN ? APRS_BUSY : APRS_OK;
	if (why == APRS_OK) {
		iss.hz = hz;
		iss.last_ms = a.now;
		iss.sent = true;
		iss.due = true;
		changed();
	}
	UNLOCK();
	return why;
}

// The press's position beacon into the queue, path ARISS (lock held; radio task, like the beacons)
static void iss_service(const fix_t *f)
{
	if (!iss.due) {
		return;
	}
	iss.due = false;
	size_t n = f != NULL ? build_beacon(f) : 0;
	if (n != 0 && queue_frame(K_ISS, APRS_ISS_PATH, a.info, n)) {
		note_info(n);
		changed();
	}
}
#endif

// --- Receive ---
'''),
        ('\tsos_service(f);\n#endif\n', '\tsos_service(f);\n#endif\n#if CONFIG_SATELLITES\n\tiss_service(f);\n#endif\n'),
        # An ISS frame goes alone, on its own frequency, whatever blocks the APRS frequency
        ('\tbool p = a.qn > 0 && a.taken == 0 && a.block == APRS_OK && (int32_t)(a.now - a.rf_next_ms) >= 0;\n',
         '#if CONFIG_SATELLITES\n'
         '\tbool p = a.qn > 0 && a.taken == 0 && (a.block == APRS_OK || a.q[0].kind == K_ISS) &&\n'
         '\t\t (int32_t)(a.now - a.rf_next_ms) >= 0;\n#else\n'
         '\tbool p = a.qn > 0 && a.taken == 0 && a.block == APRS_OK && (int32_t)(a.now - a.rf_next_ms) >= 0;\n'
         '#endif\n'),
        ('\tif (a.qn == 0 || a.block != APRS_OK || a.taken != 0) {\n',
         '#if CONFIG_SATELLITES\n'
         '\tif (a.qn == 0 || (a.block != APRS_OK && a.q[0].kind != K_ISS) || a.taken != 0) {\n#else\n'
         '\tif (a.qn == 0 || a.block != APRS_OK || a.taken != 0) {\n#endif\n'),
        ('\t\twhile (ok && n < a.qn) {\n',
         '#if CONFIG_SATELLITES\n'
         '\t\twhile (ok && n < a.qn && (n == 0 || (a.q[0].kind != K_ISS && a.q[n].kind != K_ISS))) {\n#else\n'
         '\t\twhile (ok && n < a.qn) {\n#endif\n'),
        ('\tjob->hz = aprs_cfg_freq(&a.cfg);\n\tjob->narrow = a.cfg.narrow;\n',
         '\tjob->hz = aprs_cfg_freq(&a.cfg);\n\tjob->narrow = a.cfg.narrow;\n'
         '#if CONFIG_SATELLITES\n'
         '\tif (a.q[0].kind == K_ISS) {\n'
         '\t\tjob->hz = iss.hz;\t// DM32 Plus: the ISS digipeater\n'
         '\t\tjob->narrow = false;\t// 25 kHz, as the Satellites screen follows a pass (5 kHz deviation)\n'
         '\t}\n#endif\n'),
    ])


def prepare_id_db(transform, install):
    """DMR ID database (CONFIG_ID_DB, docs/plus.md): callsign and first name for a DMR caller no codeplug contact
    names ("TA1ABC Ahmet"), from a table in program flash that tools/iddb.py makes from RadioID.net's user.csv at
    every prepare (research-data/user.csv, git-ignored, or $IDDB_CSV; countries from $IDDB_COUNTRY, comma separated,
    default Turkey; without a download the committed fictional sample, said in the build output). Looked up in the
    radio task's find_name, which every caller display reads (call screen, Home banner, Recent, Last caller, dual
    watch): contact first, then this, then upstream's CPS-uploaded database in the data flash. Settings > Advanced >
    About shows the count and date. Nothing is written to any flash; no static RAM but About's longer buffer."""
    import os
    from pathlib import Path
    from tools import iddb
    root = Path(__file__).resolve().parents[1]
    csv_path = Path(os.environ.get('IDDB_CSV') or root / 'research-data' / 'user.csv')
    sample = not csv_path.is_file()
    countries = [c.strip() for c in os.environ.get('IDDB_COUNTRY', 'Turkey').split(',') if c.strip()]
    gen = root / 'build' / 'iddb' / 'iddb_data.c'
    try:
        if sample:
            text, stats = iddb.generate(iddb.SAMPLE, date='sample', sample=True)
        else:
            text, stats = iddb.generate(str(csv_path), countries)
    except (iddb.IddbError, OSError) as e:
        raise SystemExit(f'iddb: {e}')
    print(iddb.describe(stats, sample), flush=True)
    gen.parent.mkdir(parents=True, exist_ok=True)
    if not gen.exists() or gen.read_text(encoding='ascii') != text:
        gen.write_text(text, encoding='ascii', newline='\n')
    install('core/radio/iddb.h', 'integration/radiofoundry/iddb/iddb.h')
    install('core/radio/iddb.c', 'integration/radiofoundry/iddb/iddb.c')
    install('core/radio/iddb_data.c', 'build/iddb/iddb_data.c')
    transform('config/features.mk', [
        ('CONFIG_APRS_MSG ?= n\n',
         'CONFIG_ID_DB ?= n\nFEATURES += ID_DB\n'
         'ifeq ($(CONFIG_ID_DB),y)\nifneq ($(CONFIG_DMR),y)\n$(error ID_DB needs DMR=y)\nendif\nendif\n'
         'CONFIG_APRS_MSG ?= n\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_APRS_MSG\n', '#ifndef CONFIG_ID_DB\n#define CONFIG_ID_DB 0\n#endif\n#ifndef CONFIG_APRS_MSG\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#include "codeplug/dmr_iddb.h"\n', '#include "codeplug/dmr_iddb.h"\n#if CONFIG_ID_DB\n#include "radio/iddb.h"\n#endif\n'),
        # Contact, then the built-in database (no flash chip read, short "TA1ABC Ahmet"), then the CPS-uploaded one
        ('\t} else if (!group) {\n\t\tchar text[DMR_IDDB_TEXT_MAX + 1];\n',
         '#if CONFIG_ID_DB\n\t} else if (!group && iddb_text(id, n->name, sizeof(n->name))) {\n'
         '\t\t// DM32 Plus: the built-in DMR ID database (program flash)\n#endif\n'
         '\t} else if (!group) {\n\t\tchar text[DMR_IDDB_TEXT_MAX + 1];\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('#include "rfoundry_about.h"\n', '#include "rfoundry_about.h"\n#if CONFIG_ID_DB\n#include "radio/iddb.h"\n#endif\n'),
        ('static const char *about_text(void)\n{\n\tstatic char t[640];',
         'static const char *about_text(void)\n{\n\tstatic char t[640 + 48 * CONFIG_ID_DB];\t// DM32 Plus: the ID database line'),
        ('\tsb_str(&b, TR("AMBE+2 codec: DVSI (stock)"));\t// from the radio\'s own firmware, not part of RadioFoundry\n',
         '\tsb_str(&b, TR("AMBE+2 codec: DVSI (stock)"));\t// from the radio\'s own firmware, not part of RadioFoundry\n'
         '#if CONFIG_ID_DB\n'
         '\t// DM32 Plus: "ID database: 8694, 2026-11-17, RadioID.net" (or "6, sample": no download in this build)\n'
         '\tsb_chr(&b, \'\\n\');\n\tsb_str(&b, TR("ID database: "));\n\tsb_u32(&b, iddb_info.count, 1);\n'
         '\tsb_str(&b, ", ");\n\tsb_str(&b, iddb_info.date);\n\tif (!iddb_info.sample) {\n\t\tsb_str(&b, ", ");\n'
         '\t\tsb_str(&b, iddb_info.source);\n\t}\n#endif\n'),
    ])


def prepare_messages(transform, install):
    """Pager and ACARS receive (CONFIG_MSG_RX, docs/plus.md third round): Tools > Pager decodes POCSAG (512 / 1200 /
    2400 bit/s, numeric and alphanumeric) on the FM channel in use, Tools > ACARS the 2400 bit/s MSK blocks on an
    air band channel (AM); each lists the last six messages, OK shows one in full. The decoders run only while their
    screen is open (radio/radio_msg.h): the pager hears FM caller ID's audio with the board's flat receive path on
    (board_nxdn_flat), ACARS takes the receive ring itself in AM. Receive only. Runs after prepare_fm_id (the pager
    hears its audio; it anchors on its insertions) and prepare_art (the Tools icons)."""
    for f in ('pocsag.c', 'pocsag.h', 'acars.c', 'acars.h', 'radio_msg.c', 'radio_msg.h'):
        install('core/radio/' + f, 'integration/radiofoundry/msg/' + f)
    install('core/ui/ui_msg.c', 'integration/radiofoundry/msg/ui_msg.c')
    transform('config/features.mk', [
        ('$(error FM_ID needs DMR=y and APRS=y)\nendif\nendif\n',
         '$(error FM_ID needs DMR=y and APRS=y)\nendif\nendif\n'
         'CONFIG_MSG_RX ?= n\nFEATURES += MSG_RX\n'
         'ifeq ($(CONFIG_MSG_RX),y)\nifneq ($(CONFIG_FM_ID),y)\n$(error MSG_RX needs FM_ID=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_FM_ID\n', '#ifndef CONFIG_MSG_RX\n#define CONFIG_MSG_RX 0\n#endif\n#ifndef CONFIG_FM_ID\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#if CONFIG_FM_ID\n#include "radio/radio_fmid.h"\n#endif\n',
         '#if CONFIG_FM_ID\n#include "radio/radio_fmid.h"\n#endif\n#if CONFIG_MSG_RX\n#include "radio/radio_msg.h"\n#endif\n'),
        # Before NXDN: a channel change from the pager's flat path to an NXDN channel ends ours before NXDN sets its own
        ('#if CONFIG_NXDN\n\t{\n\t\t// DM32 Plus: an NXDN channel listens on the FM audio while nothing else has the receiver\n',
         '''#if CONFIG_MSG_RX
	{
		// DM32 Plus pager / ACARS (radio/radio_msg.h), only while their screen is open: the pager on an FM channel FM
		// caller ID listens on (it hears its audio), ACARS on one received in AM (its own ring: nothing else listens)
		bool msg = sm->view.ch.mode == RADIO_MODE_FM && sm->board_mode == RADIO_MODE_FM && sm->view.state != RS_TX &&
			   sm->aprs_phase == APRS_RF_OFF && !sm->hotspot && !sm->view.bcast;
#if CONFIG_SPECTRUM
		msg = msg && !spectrum_on();
#endif
#if CONFIG_P25_SEARCH
		msg = msg && !p25s_on();
#endif
		if (radio_msg_poll(!msg ? 0 : sm->view.am ? RADIO_MSG_ACARS : RADIO_MSG_PAGER, sm->view.ch.rx_hz, now_ms))
			changed(sm);
	}
#endif
#if CONFIG_NXDN
	{
		// DM32 Plus: an NXDN channel listens on the FM audio while nothing else has the receiver
'''),
    ])
    transform('core/ui/ui_nav.h', [
        ('(CONFIG_SATELLITES)\n\tSCR_COUNT',
         '(CONFIG_SATELLITES)\n'
         '\tSCR_PAGER, SCR_ACARS,\t\t// DM32 Plus: Tools > Pager, Tools > ACARS (CONFIG_MSG_RX)\n'
         '\tSCR_MSG_VIEW,\t\t\t// DM32 Plus: one pager / ACARS message (arg: its id, bit 15 = ACARS)\n'
         '\tSCR_COUNT'),
        ('// Recent (ui_app.c): newest first\n',
         '#if CONFIG_MSG_RX\n'
         '// DM32 Plus pager / ACARS (ui_msg.c): every tick with the screen on top; true: draw again\n'
         'bool ui_msg_tick(uint8_t scr, uint16_t arg);\n#endif\n\n'
         '// Recent (ui_app.c): newest first\n'),
    ])
    transform('core/ui/ui_app.c', [
        ('\tif (ui_st.toast != NULL && (int32_t)(now - toast_until) >= 0) {\n',
         '#if CONFIG_MSG_RX\n'
         '\t// DM32 Plus: the pager / ACARS decoders listen while their screen is on top; a message draws it again\n'
         '\tif (ui_msg_tick(stack[sp].scr, stack[sp].arg))\n\t\tdirty = true;\n#endif\n'
         '\tif (ui_st.toast != NULL && (int32_t)(now - toast_until) >= 0) {\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('// ui_dmrsearch.c (DM32 Plus)',
         '// ui_dmrsearch.c (DM32 Plus)\nextern const ui_scr_def_t ui_pager_screen, ui_acars_screen, ui_msgview_screen;'
         '\t// ui_msg.c (DM32 Plus)'),
        ('\t[SCR_DMRSEARCH] = &ui_dmrsearch_screen,\n#endif\n',
         '\t[SCR_DMRSEARCH] = &ui_dmrsearch_screen,\n#endif\n'
         '#if CONFIG_MSG_RX\n\t[SCR_PAGER] = &ui_pager_screen,\n\t[SCR_ACARS] = &ui_acars_screen,\n'
         '\t[SCR_MSG_VIEW] = &ui_msgview_screen,\n#endif\n'),
        ('T_REPLAY, T_BATTERY,', 'T_REPLAY, T_PAGER, T_ACARS, T_BATTERY,'),
        ('(i != T_REPLAY || CONFIG_CALL_REPLAY) &&',
         '(i != T_REPLAY || CONFIG_CALL_REPLAY) && (i != T_PAGER || CONFIG_MSG_RX) &&\n'
         '\t       (i != T_ACARS || (CONFIG_MSG_RX && radio_am_capable())) &&'),
        ('TRN("Last call"), TRN("Battery"),', 'TRN("Last call"), TRN("Pager"), "ACARS", TRN("Battery"),'),
        ('\tcase T_ZONE_OFFER: ui_push(SCR_OFFER, 0); break;\n',
         '\tcase T_PAGER: ui_push(SCR_PAGER, 0); break;\n\tcase T_ACARS: ui_push(SCR_ACARS, 0); break;\n'
         '\tcase T_ZONE_OFFER: ui_push(SCR_OFFER, 0); break;\n'),
        ('\t\t[T_REPLAY] = ART_ICON_RECENT + 1, [T_BATTERY] = ART_ICON_BATTERY + 1,\n',
         '\t\t[T_REPLAY] = ART_ICON_RECENT + 1, [T_BATTERY] = ART_ICON_BATTERY + 1,\n'
         '\t\t[T_PAGER] = ART_ICON_MESSAGES + 1, [T_ACARS] = ART_ICON_AIRBAND + 1,\t// the envelope, the aircraft\n'),
    ])


def prepare_sstv(transform, install):
    """SSTV receive (CONFIG_SSTV, docs/plus.md third round): Tools > SSTV draws slow-scan TV pictures (Robot 36 / 72,
    PD50-PD240, Martin M1 / M2, Scottie S1 / S2 / DX; the VIS picks the mode, or the line syncs when it was missed)
    from the FM channel in use, line by line straight into the frame buffer. It listens like the pager (radio_msg.c:
    RADIO_MSG_SSTV, FM caller ID's audio, the flat receive path) only while its screen is open; the decoder and a
    three-row ring come from the RTOS heap meanwhile. Receive only. Runs after prepare_messages (it anchors on its
    insertions; the Tools entries keep their anchors for other features)."""
    for f in ('sstv.c', 'sstv.h', 'radio_sstv.c', 'radio_sstv.h'):
        install('core/radio/' + f, 'integration/radiofoundry/sstv/' + f)
    install('core/ui/ui_sstv.c', 'integration/radiofoundry/sstv/ui_sstv.c')
    transform('config/features.mk', [
        ('CONFIG_MSG_RX ?= n\nFEATURES += MSG_RX\n',
         'CONFIG_MSG_RX ?= n\nFEATURES += MSG_RX\nCONFIG_SSTV ?= n\nFEATURES += SSTV\n'
         'ifeq ($(CONFIG_SSTV),y)\nifneq ($(CONFIG_MSG_RX),y)\n$(error SSTV needs MSG_RX=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_MSG_RX\n', '#ifndef CONFIG_SSTV\n#define CONFIG_SSTV 0\n#endif\n#ifndef CONFIG_MSG_RX\n'),
    ])
    transform('core/ui/ui_nav.h', [
        ('\tSCR_COUNT\n} ui_scr_t;', '\tSCR_SSTV,\t\t\t// DM32 Plus: Tools > SSTV (CONFIG_SSTV)\n\tSCR_COUNT\n} ui_scr_t;'),
        ('bool ui_msg_tick(uint8_t scr, uint16_t arg);\n#endif\n',
         'bool ui_msg_tick(uint8_t scr, uint16_t arg);\n#endif\n'
         '#if CONFIG_SSTV\n// DM32 Plus SSTV (ui_sstv.c), from ui_msg_tick while its screen is on top: true: draw again\n'
         'bool ui_sstv_tick(void);\n#endif\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('\t// ui_msg.c (DM32 Plus)', '\t// ui_msg.c (DM32 Plus)\nextern const ui_scr_def_t ui_sstv_screen;\t// ui_sstv.c (DM32 Plus)'),
        ('\t[SCR_MSG_VIEW] = &ui_msgview_screen,\n#endif\n',
         '\t[SCR_MSG_VIEW] = &ui_msgview_screen,\n#endif\n#if CONFIG_SSTV\n\t[SCR_SSTV] = &ui_sstv_screen,\n#endif\n'),
        ('T_BATTERY, T_ZONE_OFFER, T_COUNT };', 'T_SSTV, T_BATTERY, T_ZONE_OFFER, T_COUNT };'),
        ('(i != T_BATTERY || board_volts()', '(i != T_SSTV || CONFIG_SSTV) && (i != T_BATTERY || board_volts()'),
        ('TRN("Battery"), TRN("Zones/Chs Received") };', '"SSTV", TRN("Battery"), TRN("Zones/Chs Received") };'),
        ('\tcase T_ZONE_OFFER: ui_push(SCR_OFFER, 0); break;\n',
         '\tcase T_SSTV: ui_push(SCR_SSTV, 0); break;\n\tcase T_ZONE_OFFER: ui_push(SCR_OFFER, 0); break;\n'),
        ('\t\t[T_ZONE_OFFER] = ART_ICON_ZONE_OFFER + 1,\n',
         '\t\t[T_SSTV] = ART_ICON_MAP + 1,\t// a picture\n\t\t[T_ZONE_OFFER] = ART_ICON_ZONE_OFFER + 1,\n'),
    ])


def prepare_fm_id(transform, install):
    """FM caller ID (CONFIG_FM_ID, docs/plus.md third round): MDC1200, FleetSync I and DTMF decoded from an FM
    channel's 8 kHz receive audio (the APRS receive ring: our own while APRS doesn't listen, else fed by APRS, which
    takes the ring over through radio_fmid_yield()). Home's bottom line names the caller for a few seconds ("MDC 1A2B",
    "FleetSync 107-1772", "DTMF 1234#"); each ID joins Recent (CONFIG_CALL_LOG) with its own mode (0x80-0x83,
    radio_fmid.h), without PTT or actions. Receive only. Runs after prepare_nxdn and prepare_call_log (it anchors on
    their insertions)."""
    for f in ('fmid.c', 'fmid.h', 'radio_fmid.c', 'radio_fmid.h'):
        install('core/radio/' + f, 'integration/radiofoundry/fmid/' + f)
    transform('config/features.mk', [
        ('$(error NXDN needs DMR=y and P25_CHANNELS=y)\nendif\nendif\n',
         '$(error NXDN needs DMR=y and P25_CHANNELS=y)\nendif\nendif\n'
         'CONFIG_FM_ID ?= n\nFEATURES += FM_ID\n'
         # (APRS_RX, the FM audio ring, is checked in radio_fmid.c like NXDN's)
         'ifeq ($(CONFIG_FM_ID),y)\nifneq ($(CONFIG_DMR)$(CONFIG_APRS),yy)\n'
         '$(error FM_ID needs DMR=y and APRS=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_NXDN\n', '#ifndef CONFIG_FM_ID\n#define CONFIG_FM_ID 0\n#endif\n#ifndef CONFIG_NXDN\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#include "radio/tones.h"\n', '#include "radio/tones.h"\n#if CONFIG_FM_ID\n#include "radio/radio_fmid.h"\n#endif\n'),
        ('\taprs_poll(sm, now_ms);\n', '''\taprs_poll(sm, now_ms);
#if CONFIG_FM_ID
	{
		// DM32 Plus: FM caller ID listens while an FM channel receives (before NXDN: the ring is free when an NXDN
		// channel takes it)
		bool fmid = sm->view.ch.mode == RADIO_MODE_FM && sm->board_mode == RADIO_MODE_FM && sm->view.state != RS_TX &&
			    sm->aprs_phase == APRS_RF_OFF && !sm->hotspot && !sm->view.am && !sm->view.bcast;
#if CONFIG_SPECTRUM
		fmid = fmid && !spectrum_on();
#endif
#if CONFIG_P25_SEARCH
		fmid = fmid && !p25s_on();
#endif
		if (radio_fmid_poll(fmid, sm->view.ch.rx_hz, now_ms))
			changed(sm);
	}
#endif
'''),
    ])
    # APRS's receive path: the ring is handed over before APRS starts it, and what APRS reads is decoded too
    transform('core/aprs/aprs.c', [
        ('#include "aprs/afsk.h"\n', '#include "aprs/afsk.h"\n#include "config.h"\n#if CONFIG_FM_ID\n#include "radio/radio_fmid.h"\n#endif\n'),
        ('\t\tif (a.rx_running && own) {\n\t\t\ta.rx_running = h->rx_start();\n',
         '\t\tif (a.rx_running && own) {\n#if CONFIG_FM_ID\n\t\t\tradio_fmid_yield();\t// DM32 Plus: FM caller ID gives the ring back\n#endif\n'
         '\t\t\ta.rx_running = h->rx_start();\n'),
        ('\t\t\taprs_rx_audio(a.rxbuf, k, now_ms);\n',
         '\t\t\taprs_rx_audio(a.rxbuf, k, now_ms);\n#if CONFIG_FM_ID\n\t\t\tif (own)\n\t\t\t\tradio_fmid_audio(a.rxbuf, k);\n#endif\n'),
    ])
    transform('core/ui/ui_screens.c', [
        ('#include "config.h"\n', '#include "config.h"\n#if CONFIG_FM_ID\n#include "radio/radio_fmid.h"\n#endif\n'),
        ('void ui_draw_home(const radio_view_t *v, const ui_state_t *s)\n', '''#if CONFIG_FM_ID
// DM32 Plus: who called on an FM channel ("MDC 1A2B", "FleetSync 107-1772", "DTMF 1234#"), NULL for none. noinline
// and static buffers: Home's own frame is on the UI task's deepest path (dual watch panes)
static const char *__attribute__((noinline)) fmid_bottom(const radio_view_t *v)
{
	static char line[48];
	static radio_fmid_id_t fi;
	radio_fmid_shown(&fi);
	if (v->ch.mode != RADIO_MODE_FM || fi.kind == 0)
		return NULL;
	radio_fmid_text(fi.kind, fi.unit, fi.fleet, fi.dtmf, line, sizeof line);
	if (fi.kind == RADIO_FMID_MDC_EMERG) {
		sbuf_t b;
		size_t n = strlen(line);
		sb_init(&b, line + n, sizeof line - n);
		sb_chr(&b, ' ');
		sb_str(&b, TR("Emergency"));
	}
	return line;
}
#endif

void ui_draw_home(const radio_view_t *v, const ui_state_t *s)
'''),
        ('\tui_draw_bottom(s, bottom);', '''#if CONFIG_FM_ID
	{
		const char *fm = fmid_bottom(v);
		if (fm != NULL)
			bottom = fm;
	}
#endif
	ui_draw_bottom(s, bottom);'''),
    ])
    # Call history: each ID (a DTMF string once it has ended) is an entry of its own mode, no PTT or actions
    transform('core/ui/ui_app.c', [
        ('#include "config.h"\n', '#include "config.h"\n#if CONFIG_FM_ID\n#include "radio/radio_fmid.h"\n#endif\n'),
        ('#if CONFIG_NXDN\n\tstatic uint32_t nxdn_seen;\n',
         '#if CONFIG_FM_ID\n'
         '\tstatic uint32_t fmid_seen;\n\tstatic radio_fmid_id_t fl;\t// static: the UI task\'s stack is tight\n'
         '\tuint32_t fq = radio_fmid_logged(&fl);\n'
         '\tif (fq != fmid_seen) {\n\t\tfmid_seen = fq;\n'
         '\t\tif (fl.kind != 0)\n'
         '\t\t\tcall_log_add(fl.kind, fl.unit, fl.fleet, false, 0, false, 0, fl.kind == RADIO_FMID_DTMF ? fl.dtmf : NULL);\n'
         '\t}\n#endif\n'
         '#if CONFIG_NXDN\n\tstatic uint32_t nxdn_seen;\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('#include "config.h"\n', '#include "config.h"\n#if CONFIG_FM_ID\n#include "radio/radio_fmid.h"\n#endif\n'),
        ('#if CONFIG_CALL_LOG\n\tif (r->mode != 0) {\t// DM32 Plus: "P25 77285"',
         '#if CONFIG_CALL_LOG\n#if CONFIG_FM_ID\n'
         '\tif (r->mode >= RADIO_FMID_MDC) {\t// DM32 Plus FM caller ID: "MDC 1A2B"  "Emergency 5m"\n'
         '\t\tradio_fmid_text(r->mode, (uint16_t)r->src, (uint16_t)r->dst, r->name, it->label, sizeof it->label);\n'
         '\t\tsbuf_t vb;\n\t\tsb_init(&vb, it->value, sizeof it->value);\n'
         '\t\tif (r->mode == RADIO_FMID_MDC_EMERG) {\n\t\t\tsb_str(&vb, TR("Emergency"));\n\t\t\tsb_chr(&vb, \' \');\n'
         '\t\t\tit->flags |= UI_ITEM_WARN;\n\t\t}\n'
         '\t\tsb_age(&vb, ui_st.now_ms - r->ms);\n\t\treturn;\n\t}\n#endif\n'
         '\tif (r->mode != 0) {\t// DM32 Plus: "P25 77285"'),
    ])
    # Settings > Talk & listen > FM caller ID, on by default. Off: radio_fmid.c never takes the receive ring for itself
    # (FM audio keeps the FD6818's direct path), only while the Pager / SSTV screen wants the audio. Stored in a spare
    # bit of an existing settings byte, settings_t.cp_flags bit 7 (SETTINGS_CP_NO_FM_ID, set = off): the record's
    # layout and size stay upstream's, an older record and a factory reset (settings_defaults) read 0 = on, and
    # upstream's own cp_flags users (app.c) change only bit 0 with a mask. The CPS never reads the settings ring.
    transform('core/settings/settings.h', [
        ('#define SETTINGS_CP_ACCEPTED\t\t0x01u\n',
         '#define SETTINGS_CP_ACCEPTED\t\t0x01u\n'
         '#define SETTINGS_CP_NO_FM_ID\t\t0x80u\t// DM32 Plus: FM caller ID off (Settings > Talk & listen; 0 = on)\n'),
    ])
    transform('core/ui/ui_svc.h', [
        ('\tUI_SET_COUNT\n',
         '#if CONFIG_FM_ID\n'
         '\tUI_SET_FM_ID,\t\t// DM32 Plus FM caller ID on (settings_t.cp_flags SETTINGS_CP_NO_FM_ID clear)\n'
         '#endif\n'
         '\tUI_SET_COUNT\n'),
    ])
    transform('core/ui/ui_svc_stub.c', [
        ('\t[UI_SET_BCAST_STEP] = 200,\n',
         '\t[UI_SET_BCAST_STEP] = 200,\n#if CONFIG_FM_ID\n\t[UI_SET_FM_ID] = 1,\t// DM32 Plus\n#endif\n'),
    ])
    transform('core/ui/ui_svc_storage.c', [
        ('\tcase UI_SET_NOISE_REDUCTION:\n',
         '#if CONFIG_FM_ID\n\tcase UI_SET_FM_ID:\t// DM32 Plus: the bit is set for "off" (0 = on, older records too)\n'
         '\t\tsettings_read(&st);\n\t\treturn (st.cp_flags & SETTINGS_CP_NO_FM_ID) == 0;\n#endif\n'
         '\tcase UI_SET_NOISE_REDUCTION:\n'),
        ('\tcase UI_SET_NOISE_REDUCTION:\t// ui_svc_apply hands them to the vocoder\n',
         '#if CONFIG_FM_ID\n\tcase UI_SET_FM_ID:\t// DM32 Plus; ui_svc_apply hands it to radio_fmid.c\n'
         '\t\tst = settings_begin();\n'
         '\t\tst->cp_flags = (uint8_t)(value != 0 ? (st->cp_flags & ~SETTINGS_CP_NO_FM_ID) :\n'
         '\t\t\t\t\t  (st->cp_flags | SETTINGS_CP_NO_FM_ID));\n'
         '\t\tsettings_end(hal_millis());\n\t\treturn true;\n#endif\n'
         '\tcase UI_SET_NOISE_REDUCTION:\t// ui_svc_apply hands them to the vocoder\n'),
    ])
    transform('core/ui/ui_svc_radio.c', [
        ('#include "config.h"\n', '#include "config.h"\n#if CONFIG_FM_ID\n#include "radio/radio_fmid.h"\n#endif\n'),
        ("\tcase UI_SET_GPS:\t\t// the module's power and the position sources, now (not a radio core setting)\n",
         '#if CONFIG_FM_ID\n\tcase UI_SET_FM_ID:\t\t// DM32 Plus: the decoders and the receive ring (radio_fmid.c), now\n'
         '\t\tradio_fmid_enable(value != 0);\n\t\treturn;\n#endif\n'
         "\tcase UI_SET_GPS:\t\t// the module's power and the position sources, now (not a radio core setting)\n"),
    ])
    transform('core/ui/ui_menus.c', [
        ('\tTOGGLE(TRN("Show talker alias"), NULL, UI_SET_ALIAS_SHOW),\n',
         '\tTOGGLE(TRN("Show talker alias"), NULL, UI_SET_ALIAS_SHOW),\n#if CONFIG_FM_ID\n'
         '\tTOGGLE(TRN("FM caller ID"), "MDC1200 FleetSync DTMF ANI decode", UI_SET_FM_ID),\t// DM32 Plus\n#endif\n'),
    ])


def prepare_call_log(transform):
    """Call history (CONFIG_CALL_LOG): upstream's Recent list (DMR calls heard and made) grows to 20 entries and
    takes P25, YSF and NXDN calls too, each with its mode, talkgroup, NAC (P25) and an encrypted mark. P25 calls come
    from p25_lab_call() (p25_lab.c call_track: a call per 1.5 s gap or talkgroup/NAC change; encrypted when no clear
    voice superframe arrives in its first 0.5 s), YSF from p25_lab_ysf_caller(), NXDN from radio_nxdn_caller(). An
    encrypted P25 call also shows the toast DMR uses ("Encrypted: not played"). RAM only: gone at power off. The
    non-DMR entries have no PTT or actions (they aren't DMR targets)."""
    transform('config/features.mk', [
        ('CONFIG_DMR_ANY ?= n\nFEATURES += DMR_ANY\n', 'CONFIG_DMR_ANY ?= n\nFEATURES += DMR_ANY\n'
         'CONFIG_CALL_LOG ?= n\nFEATURES += CALL_LOG\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_DMR_ANY\n', '#ifndef CONFIG_CALL_LOG\n#define CONFIG_CALL_LOG 0\n#endif\n#ifndef CONFIG_DMR_ANY\n'),
    ])
    transform('core/ui/ui_nav.h', [
        ('#define UI_RECENT_MAX\t12\n',
         '#if CONFIG_CALL_LOG\n#define UI_RECENT_MAX\t20\t// DM32 Plus call history\n#else\n#define UI_RECENT_MAX\t12\n#endif\n'),
        ('\tuint32_t gps_dist_m;\n} ui_recent_t;',
         '\tuint32_t gps_dist_m;\n#if CONFIG_CALL_LOG\n'
         '\tuint8_t mode;\t\t// DM32 Plus: 0 DMR, else the call\'s RADIO_MODE_P25 / _YSF / _NXDN\n'
         '\tbool enc;\t\t// encrypted: heard, not played\n'
         '\tuint16_t nac;\t\t// P25: the NAC; NXDN: the RAN\n'
         '\tuint32_t call;\t\t// P25: the p25_lab_call() number, so the same call updates its entry\n#endif\n'
         '} ui_recent_t;'),
    ])
    transform('core/ui/ui_app.c', [
        ('#include "config.h"\n',
         '#include "config.h"\n#if CONFIG_CALL_LOG\n#include "p25/p25_lab.h"\n#if CONFIG_NXDN\n#include "radio/radio_nxdn.h"\n'
         '#endif\n#endif\n'),
        ('\t\tif (recent[i].src == r->src && recent[i].dst == r->dst && recent[i].out == r->out)\n',
         '#if CONFIG_CALL_LOG\n'
         '\t\tif (recent[i].src == r->src && recent[i].dst == r->dst && recent[i].out == r->out &&\n'
         '\t\t    recent[i].mode == r->mode && recent[i].nac == r->nac &&\n'
         '\t\t    (r->mode == 0 || strcmp(recent[i].name, r->name) == 0))\n#else\n'
         '\t\tif (recent[i].src == r->src && recent[i].dst == r->dst && recent[i].out == r->out)\n#endif\n'),
        ('\trecent[0] = *r;\n\tdirty = true;\n}\n#endif\n',
         '\trecent[0] = *r;\n\tdirty = true;\n}\n#endif\n'
         '#if CONFIG_CALL_LOG\n'
         '// DM32 Plus call history: P25, YSF and NXDN calls join Recent (newest first, the same station again moves up)\n'
         'static void call_log_add(uint8_t mode, uint32_t src, uint32_t dst, bool group, uint16_t nac, bool enc,\n'
         '\t\t\t uint32_t call, const char *name)\n{\n'
         '\tui_recent_t r;\n\tmemset(&r, 0, sizeof r);\n'
         '\tr.mode = mode;\n\tr.src = src;\n\tr.dst = dst;\n\tr.group = group;\n\tr.nac = nac;\n\tr.enc = enc;\n'
         '\tr.call = call;\n\tr.ms = now;\n'
         '\tif (name != NULL)\n\t\tstrncpy(r.name, name, sizeof r.name - 1);\n'
         '\trecent_add(&r);\n}\n\n'
         'static void call_log_update(void)\n{\n'
         '#if CONFIG_P25_CALLER\n'
         '\tstatic uint32_t p25_seen, p25_last;\n\tstatic bool enc_told;\n\tp25_lab_call_t c;\n'
         '\tif (p25_lab_call(&c) != 0) {\n'
         '\t\tif (c.n != p25_seen) {\n'
         '\t\t\tp25_seen = c.n;\n\t\t\tp25_last = c.last_ms;\n\t\t\tenc_told = false;\n'
         '\t\t\tcall_log_add(RADIO_MODE_P25, c.src, c.tg, true, c.nac, c.encrypted, c.n, NULL);\n'
         '\t\t} else if (c.last_ms != p25_last || (recent_n > 0 && recent[0].call == c.n &&\n'
         '\t\t\t   (recent[0].src != c.src || recent[0].enc != c.encrypted))) {\n'
         '\t\t\tp25_last = c.last_ms;\n'
         '\t\t\tfor (uint8_t i = 0; i < recent_n; i++) {\n'
         '\t\t\t\tif (recent[i].mode == RADIO_MODE_P25 && recent[i].call == c.n) {\n'
         '\t\t\t\t\trecent[i].src = c.src;\n\t\t\t\t\trecent[i].enc = c.encrypted;\n\t\t\t\t\trecent[i].ms = now;\n'
         '\t\t\t\t\tdirty = true;\n\t\t\t\t\tbreak;\n\t\t\t\t}\n\t\t\t}\n\t\t}\n'
         '\t\tif (c.encrypted && !enc_told) {\n\t\t\tenc_told = true;\n'
         '\t\t\tui_toast(TR("Encrypted: not played"));\n\t\t}\n\t}\n'
         '#endif\n'
         '#if CONFIG_YSF\n'
         '\tstatic uint32_t ysf_seen;\n\tchar ys[11], yd[11];\n'
         '\tuint32_t ysf = p25_lab_ysf_caller(ys, yd);\n'
         '\tif (ysf != ysf_seen) {\n\t\tysf_seen = ysf;\n'
         '\t\tif (ys[0] != \'\\0\')\n\t\t\tcall_log_add(RADIO_MODE_YSF, 0, 0, false, 0, false, 0, ys);\n\t}\n'
         '#endif\n'
         '#if CONFIG_NXDN\n'
         '\tstatic uint32_t nxdn_seen;\n\tradio_nxdn_caller_t nc;\n'
         '\tuint32_t nx = radio_nxdn_caller(&nc);\n'
         '\tif (nx != nxdn_seen) {\n\t\tnxdn_seen = nx;\n'
         '\t\tif (nc.active && nc.source != 0)\n'
         '\t\t\tcall_log_add(RADIO_MODE_NXDN, nc.source, nc.destination, nc.group, nc.ran, nc.encrypted, 0, NULL);\n'
         '\t}\n#endif\n}\n#endif\n'),
        ('#if CONFIG_DMR\n\trecent_update(v);\n',
         '#if CONFIG_DMR\n\trecent_update(v);\n#if CONFIG_CALL_LOG\n\tcall_log_update();\n#endif\n'),
        # Last caller and call back are DMR targets: they skip the other modes' entries
        ('\t\tif (!recent[i].out) {\n\t\t\tui_recent_target(&recent[i], t);\n',
         '#if CONFIG_CALL_LOG\n\t\tif (!recent[i].out && recent[i].mode == 0) {\n#else\n\t\tif (!recent[i].out) {\n#endif\n'
         '\t\t\tui_recent_target(&recent[i], t);\n'),
        ('\t\tconst ui_recent_t *r = ui_recent_get(i);\n\t\tif (r->out)\n\t\t\tcontinue;\n',
         '\t\tconst ui_recent_t *r = ui_recent_get(i);\n\t\tif (r->out)\n\t\t\tcontinue;\n'
         '#if CONFIG_CALL_LOG\n\t\tif (r->mode != 0)\n\t\t\tcontinue;\n#endif\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('\tconst ui_recent_t *r = ui_recent_get((uint8_t)i);\n\tif (r == NULL)\n\t\treturn false;\n\tui_recent_target(r, t);\n',
         '\tconst ui_recent_t *r = ui_recent_get((uint8_t)i);\n\tif (r == NULL)\n\t\treturn false;\n'
         '#if CONFIG_CALL_LOG\n\tif (r->mode != 0)\n\t\treturn false;\t// a P25 / YSF / NXDN call: not a DMR target\n#endif\n'
         '\tui_recent_target(r, t);\n'),
        ('\tconst ui_recent_t *r = ui_recent_get((uint8_t)i);\n\tif (r == NULL)\n\t\treturn;\n\tsbuf_t b;\n',
         '\tconst ui_recent_t *r = ui_recent_get((uint8_t)i);\n\tif (r == NULL)\n\t\treturn;\n'
         '#if CONFIG_CALL_LOG\n'
         '\tif (r->mode != 0) {\t// DM32 Plus: "P25 77285"  "TG 101 NAC 659 5m"\n'
         '\t\tsbuf_t lb, vb;\n\t\tsb_init(&lb, it->label, sizeof it->label);\n\t\tsb_init(&vb, it->value, sizeof it->value);\n'
         '\t\tsb_str(&lb, r->mode == RADIO_MODE_NXDN ? "NXDN " : r->mode == RADIO_MODE_YSF ? "YSF " : "P25 ");\n'
         '\t\tif (r->name[0])\n\t\t\tsb_str(&lb, r->name);\n\t\telse if (r->src != 0)\n\t\t\tsb_u32(&lb, r->src, 1);\n'
         "\t\telse\n\t\t\tsb_chr(&lb, '?');\n"
         '\t\tif (r->enc) {\n\t\t\tsb_str(&vb, TR("Encrypted"));\n\t\t\tsb_chr(&vb, \' \');\n\t\t\tit->flags |= UI_ITEM_WARN;\n\t\t}\n'
         '\t\tif (r->mode != RADIO_MODE_YSF) {\n\t\t\tsb_str(&vb, r->group ? "TG " : "ID ");\n\t\t\tsb_u32(&vb, r->dst, 1);\n\t\t}\n'
         '\t\tif (r->mode == RADIO_MODE_P25) {\n\t\t\tsb_str(&vb, " NAC ");\n'
         '\t\t\tfor (int shift = 8; shift >= 0; shift -= 4)\n'
         '\t\t\t\tsb_chr(&vb, "0123456789ABCDEF"[(r->nac >> shift) & 15u]);\n\t\t}\n'
         "\t\tsb_chr(&vb, ' ');\n\t\tsb_age(&vb, ui_st.now_ms - r->ms);\n\t\treturn;\n\t}\n#endif\n"
         '\tsbuf_t b;\n'),
        ('\tconst ui_recent_t *r = ui_recent_get((uint8_t)i);\n\tui_target_t t;\n\tif (r != NULL) {\n',
         '\tconst ui_recent_t *r = ui_recent_get((uint8_t)i);\n\tui_target_t t;\n'
         '#if CONFIG_CALL_LOG\n\tif (r != NULL && r->mode != 0)\n\t\treturn;\n#endif\n\tif (r != NULL) {\n'),
        ('\t\tui_target_t t;\n\t\tui_recent_target(ui_recent_get(0), &t);\n',
         '#if CONFIG_CALL_LOG\n\t\tconst ui_recent_t *dmr = NULL;\n'
         '\t\tfor (uint8_t k = 0; k < ui_recent_count() && dmr == NULL; k++)\n'
         '\t\t\tif (ui_recent_get(k)->mode == 0)\n\t\t\t\tdmr = ui_recent_get(k);\n'
         '\t\tif (dmr == NULL) {\n\t\t\tui_push(SCR_RECENT, 0);\t// only P25 / YSF / NXDN calls: the list\n\t\t\treturn;\n\t\t}\n'
         '\t\tui_target_t t;\n\t\tui_recent_target(dmr, &t);\n#else\n'
         '\t\tui_target_t t;\n\t\tui_recent_target(ui_recent_get(0), &t);\n#endif\n'),
        ('\t\tconst ui_recent_t *r = ui_recent_get(i);\n\t\tif (r->group) {\n',
         '\t\tconst ui_recent_t *r = ui_recent_get(i);\n'
         '#if CONFIG_CALL_LOG\n\t\tif (r->group && r->mode == 0) {\n#else\n\t\tif (r->group) {\n#endif\n'),
    ])


def prepare_signal_quality(transform, install):
    """Signal quality during a call (CONFIG_SIGNAL_QUALITY). A bit error rate estimate from the FEC corrections the
    voice decoders already count, over about the last second (radio/sigq.h): P25 from the IMBE frames of every voice
    superframe on the channel (src/p25/voice.c, p25_lab.c), shown at the end of Home's P25 caller line ("BER 1.2%",
    in each of its shorter forms too); DMR from the AMBE+2 frames the voice sink decodes (ambeVoice.c counts them),
    shown before the level on the DMR call screen ("BER 1.2%  -87 dBm"; upstream already shows the level and its
    meter there). The level during a P25 (or YSF) call: radio_task.c skips radio_sm_poll while the native receiver
    has the channel, so Home's dBm and meter kept the last FM reading; the native maintenance now reads the FD6818
    the same way while a call is heard (the native receive leaves it tuned as for DMR; its interrupts use only the
    HR_C7000's bus)."""
    install('core/radio/sigq.h', 'integration/radiofoundry/spectrum/sigq.h')
    install('core/radio/sigq.c', 'integration/radiofoundry/spectrum/sigq.c')
    transform('config/features.mk', [
        ('CONFIG_CALL_LOG ?= n\nFEATURES += CALL_LOG\n', 'CONFIG_CALL_LOG ?= n\nFEATURES += CALL_LOG\n'
         'CONFIG_SIGNAL_QUALITY ?= n\nFEATURES += SIGNAL_QUALITY\n'
         'ifeq ($(CONFIG_SIGNAL_QUALITY),y)\nifneq ($(CONFIG_P25_CALLER),y)\n'
         '$(error SIGNAL_QUALITY needs P25_CALLER=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_CALL_LOG\n', '#ifndef CONFIG_SIGNAL_QUALITY\n#define CONFIG_SIGNAL_QUALITY 0\n#endif\n'
         '#ifndef CONFIG_CALL_LOG\n'),
    ])
    transform('core/dmr/ambe/ambeVoice.h', [
        ('void ambeVoiceGetStats(ambe_voice_stats_t *out);\n',
         'void ambeVoiceGetStats(ambe_voice_stats_t *out);\n'
         '#if defined(CONFIG_SIGNAL_QUALITY) && CONFIG_SIGNAL_QUALITY\n'
         '// DM32 Plus signal quality: the voice frames handed in so far and the bit errors their FEC found (an uncorrectable\n'
         '// frame counts 7, as ambe_fec_result_t does). Any task: two counters read as they are, no copy of the stats\n'
         'void ambeVoiceRxErrors(uint32_t *frames, uint32_t *errors);\n#endif\n'),
    ])
    transform('core/dmr/ambe/ambeVoice.c', [
        ('void ambeVoiceGetStats(ambe_voice_stats_t *out)\n{\n\t*out = stats;\n}\n',
         'void ambeVoiceGetStats(ambe_voice_stats_t *out)\n{\n\t*out = stats;\n}\n'
         '#if defined(CONFIG_SIGNAL_QUALITY) && CONFIG_SIGNAL_QUALITY\n\nvoid ambeVoiceRxErrors(uint32_t *frames, uint32_t *errors)\n{\n'
         '\t*frames = stats.rx_in;\n\t*errors = stats.rx_bit_errors + 7u * stats.rx_bad;\n}\n#endif\n'),
    ])
    transform('core/radio/radio.h', [
        ('\tint16_t rssi_dbm;\t// last burst of the call, 0 = unknown\n',
         '\tint16_t rssi_dbm;\t// last burst of the call, 0 = unknown\n'
         '#if CONFIG_SIGNAL_QUALITY\n'
         '\tuint16_t ber_pm1;\t// DM32 Plus: the call\'s bit error rate (last second) in 0.1 % steps plus 1, 0 = unknown\n'
         '#endif\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#if CONFIG_P25_CHANNELS\n#include "p25/p25_lab.h"\n#endif\n',
         '#if CONFIG_P25_CHANNELS\n#include "p25/p25_lab.h"\n#endif\n#if CONFIG_SIGNAL_QUALITY\n#include "radio/sigq.h"\n#endif\n'),
        ('#if CONFIG_P25_CHANNELS\nvoid radio_sm_native_maintenance(radio_sm_t *sm, uint32_t now_ms)\n{\n', '''#if CONFIG_SIGNAL_QUALITY && CONFIG_DMR
// DM32 Plus signal quality: the bit error rate of the DMR voice in about the last second, from the AMBE+2 frames the
// voice sink decoded since the last poll (radio/sigq.h). Published twice a second while a call is heard
static void dmr_quality(radio_sm_t *sm, uint32_t now_ms)
{
	static sigq_t q;
	static uint32_t frames_seen, errors_seen, shown_ms;
	static bool primed;
	uint32_t frames, errors;
	ambeVoiceRxErrors(&frames, &errors);
	if (primed && frames >= frames_seen && errors >= errors_seen)
		sigq_add(&q, errors - errors_seen, (frames - frames_seen) * SIGQ_DMR_BITS, now_ms);
	primed = true;	// the first poll only takes the counts: what came before isn't this call
	frames_seen = frames;
	errors_seen = errors;
	bool rx = sm->view.dmr.state == RDMR_RX || sm->view.dmr.state == RDMR_RX_OTHER;
	if (rx && now_ms - shown_ms < 500u)
		return;
	shown_ms = now_ms;
	uint16_t pm1 = (uint16_t)((rx ? sigq_ber_permille(&q, now_ms) : -1) + 1);
	if (pm1 != sm->view.dmr.ber_pm1) {
		sm->view.dmr.ber_pm1 = pm1;
		changed(sm);
	}
}
#endif

#if CONFIG_SIGNAL_QUALITY && CONFIG_P25_CHANNELS
// DM32 Plus signal quality while the native receiver has the channel (radio_task.c skips radio_sm_poll then): the
// level read from the FD6818 as the poll reads it (two register frames, interrupts off about 50 us each; the native
// receive leaves the chip tuned as for DMR and its interrupts don't use this bus), and a redraw when the P25 bit error
// rate changes. Only while a call is heard, never keyed: Home shows the level (dBm and meter) and the rate then
static void native_quality(radio_sm_t *sm, uint32_t now_ms)
{
	static uint32_t ber_ms;
	static int ber_shown = -1;
	if (sm->view.state != RS_RX || sm->ptt)
		return;
	radio_status_t st;
	board.radio->status(&st);
	sm->view.rssi_dbm = rssi_held(sm, st.rssi_dbm);
	int drift = sm->view.rssi_dbm - sm->rssi_published;
	if (drift >= RADIO_RSSI_DEADBAND_DB || drift <= -RADIO_RSSI_DEADBAND_DB) {
		sm->rssi_published = sm->view.rssi_dbm;
		changed(sm);
	}
	if (now_ms - ber_ms >= 500u) {
		ber_ms = now_ms;
		int ber = p25_lab_ber_permille();
		if (ber != ber_shown) {
			ber_shown = ber;
			changed(sm);
		}
	}
}
#endif

#if CONFIG_P25_CHANNELS
void radio_sm_native_maintenance(radio_sm_t *sm, uint32_t now_ms)
{
'''),
        ('\tif (sm->view.state == RS_TX) tx_timer(sm, now_ms);\n\toutputs(sm);\n',
         '\tif (sm->view.state == RS_TX) tx_timer(sm, now_ms);\n'
         '#if CONFIG_SIGNAL_QUALITY\n\tnative_quality(sm, now_ms);\n#endif\n\toutputs(sm);\n'),
        ('\t\t\tsm->view.dmr.rssi_dbm = level;\n\t\t\tchanged(sm);\n\t\t}\n\t}\n#endif\n',
         '\t\t\tsm->view.dmr.rssi_dbm = level;\n\t\t\tchanged(sm);\n\t\t}\n\t}\n#endif\n'
         '#if CONFIG_SIGNAL_QUALITY && CONFIG_DMR\n\tdmr_quality(sm, now_ms);\n#endif\n'),
    ])
    transform('core/ui/ui_screens.c', [
        # Home's P25 caller line: each of its three forms ends with the bit error rate now
        ("\t\tstatic char p25_line[80];\t// static: the UI task's stack is tight. Three alternates, NAC included\n",
         "#if CONFIG_SIGNAL_QUALITY\n"
         "\t\tstatic char p25_line[112];\t// static: the UI task's stack is tight. Three alternates, NAC and BER included\n"
         "#else\n"
         "\t\tstatic char p25_line[80];\t// static: the UI task's stack is tight. Three alternates, NAC included\n"
         "#endif\n"),
        # The DMR call screen's signal line: "BER 1.2%  -87 dBm", the meter in the room left
        ('\t\tint16_t dbm = d->rssi_dbm != 0 ? d->rssi_dbm : v->rssi_dbm;\n\t\tsb_init(&b, buf, sizeof buf);\n',
         '\t\tint16_t dbm = d->rssi_dbm != 0 ? d->rssi_dbm : v->rssi_dbm;\n\t\tsb_init(&b, buf, sizeof buf);\n'
         '#if CONFIG_SIGNAL_QUALITY\n\t\tif (d->ber_pm1 != 0) {\t// DM32 Plus: the voice\'s bit error rate (last second)\n'
         '\t\t\tsb_str(&b, "BER ");\n\t\t\tsb_u32(&b, (d->ber_pm1 - 1u) / 10u, 1);\n\t\t\tsb_chr(&b, \'.\');\n'
         '\t\t\tsb_u32(&b, (d->ber_pm1 - 1u) % 10u, 1);\n\t\t\tsb_str(&b, "%  ");\n\t\t}\n#endif\n'),
    ])


def prepare_satellites(transform, install):
    """Satellites (CONFIG_SATELLITES, needs GPS, APRS and the FM VFO): Tools > Satellites lists the next pass of each
    amateur FM satellite over the GPS position (SGP4 from integration/radiofoundry/sat/tle.txt, the transponders from
    sat_freq.txt there, compiled in every build by tools/sat_tle.py), the live look angles, range and Doppler of the
    one in focus, and OK follows a pass in the VFO (RX downlink, TX uplink, CTCSS, Doppler corrected in steps). The
    math runs in the radio task at its poll (radio/sat_svc.c), only while the screen looks or a pass is followed."""
    from pathlib import Path
    from tools import sat_tle
    root = Path(__file__).resolve().parents[1]
    src = root / 'integration' / 'radiofoundry' / 'sat'
    gen = root / 'build' / 'sat-tle' / 'sat_data.c'
    try:
        text = sat_tle.generate(str(src / 'tle.txt'), str(src / 'sat_freq.txt'))
    except (sat_tle.TleError, ValueError) as e:
        raise SystemExit(f'sat_tle: {e}')
    gen.parent.mkdir(parents=True, exist_ok=True)
    if not gen.exists() or gen.read_text(encoding='ascii') != text:
        gen.write_text(text, encoding='ascii', newline='\n')
    for f in ('sgp4.h', 'sgp4.c', 'sat.h', 'sat.c', 'sat_svc.c'):
        install('core/radio/' + f, 'integration/radiofoundry/sat/' + f)
    install('core/radio/sat_data.c', 'build/sat-tle/sat_data.c')
    install('core/ui/ui_sat.c', 'integration/radiofoundry/sat/ui_sat.c')
    transform('config/features.mk', [
        ('$(error SIGNAL_QUALITY needs P25_CALLER=y)\nendif\nendif\n',
         '$(error SIGNAL_QUALITY needs P25_CALLER=y)\nendif\nendif\n'
         'CONFIG_SATELLITES ?= n\nFEATURES += SATELLITES\n'
         'ifeq ($(CONFIG_SATELLITES),y)\nifneq ($(CONFIG_GPS)$(CONFIG_APRS)$(CONFIG_VFO_FM),yyy)\n'
         '$(error SATELLITES needs GPS=y, APRS=y and VFO_FM=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_SIGNAL_QUALITY\n', '#ifndef CONFIG_SATELLITES\n#define CONFIG_SATELLITES 0\n#endif\n'
         '#ifndef CONFIG_SIGNAL_QUALITY\n'),
    ])
    # The service at the radio poll, and at the native receiver's maintenance poll on a P25 channel (lighter there)
    transform('core/radio/radio_task.c', [
        ('#if CONFIG_DMR_SEARCH\n#include "radio/dmrsearch.h"\n#endif\n',
         '#if CONFIG_DMR_SEARCH\n#include "radio/dmrsearch.h"\n#endif\n#if CONFIG_SATELLITES\n#include "radio/sat.h"\n#endif\n'),
        ('\t\t\tradio_hs_watch(&sm, hal_millis());\n#endif\n',
         '\t\t\tradio_hs_watch(&sm, hal_millis());\n#endif\n#if CONFIG_SATELLITES\n'
         '\t\t\tsat_service(sm.view.vfo, sm.view.ch.rx_hz, sm.view.ch.tx_hz, sm.view.state == RS_TX || sm.ptt, false,\n'
         '\t\t\t\t    hal_millis());\n#endif\n'),
        ('                radio_sm_native_maintenance(&sm,hal_millis());\n',
         '                radio_sm_native_maintenance(&sm,hal_millis());\n#if CONFIG_SATELLITES\n'
         '                sat_service(sm.view.vfo, sm.view.ch.rx_hz, sm.view.ch.tx_hz, sm.view.state == RS_TX || sm.ptt, true,\n'
         '                            hal_millis());\n#endif\n'),
    ])
    transform('core/ui/ui_nav.h', [
        ('(CONFIG_DMR_SEARCH)\n\tSCR_COUNT',
         '(CONFIG_DMR_SEARCH)\n\tSCR_SATELLITES,\t\t\t// DM32 Plus: Tools > Satellites (CONFIG_SATELLITES)\n\tSCR_COUNT'),
    ])
    transform('core/ui/ui_menus.c', [
        ('extern const ui_scr_def_t ui_dmrsearch_screen;',
         'extern const ui_scr_def_t ui_sat_screen;\t// ui_sat.c (DM32 Plus)\nextern const ui_scr_def_t ui_dmrsearch_screen;'),
        ('#if CONFIG_DMR_SEARCH\n\t[SCR_DMRSEARCH] = &ui_dmrsearch_screen,\n#endif\n',
         '#if CONFIG_DMR_SEARCH\n\t[SCR_DMRSEARCH] = &ui_dmrsearch_screen,\n#endif\n'
         '#if CONFIG_SATELLITES\n\t[SCR_SATELLITES] = &ui_sat_screen,\n#endif\n'),
        ('T_MAP, T_P25SEARCH,', 'T_MAP, T_SATELLITES, T_P25SEARCH,'),
        ('(i != T_MAP || CONFIG_MAP) &&', '(i != T_MAP || CONFIG_MAP) && (i != T_SATELLITES || CONFIG_SATELLITES) &&'),
        ('TRN("Map"), TRN("P25 search"),', 'TRN("Map"), TRN("Satellites"), TRN("P25 search"),'),
        ('\tcase T_MAP: ui_push(SCR_MAP, 0); break;\n#endif\n',
         '\tcase T_MAP: ui_push(SCR_MAP, 0); break;\n#endif\n'
         '#if CONFIG_SATELLITES\n\tcase T_SATELLITES: ui_push(SCR_SATELLITES, 0); break;\n#endif\n'),
    ])


def prepare_dmr_any(transform):
    """'Any' color code, timeslot and talkgroup on a DMR channel (CONFIG_DMR_ANY). The call layer already filters by
    talkgroup (hear_all) and color code (any_cc, a modem CC filter of -1); this adds 'any timeslot' (no slot lock,
    the first slot heard holds the call) and makes all three channel settings. Stored in the free high bits of the
    channel record's color code byte (only DMR records, mode 1): 0x80 any color code, 0x40 any timeslot, 0x20 every
    talkgroup. In channel_t: color_code 16, timeslot 3, listen_list 0xFFFF. Any color code or timeslot makes the
    channel RX only (no single one to transmit on); 'every talkgroup' is the RX group list choice 'Any' and leaves TX
    to the contact as it was."""
    transform('config/features.mk', [
        ('CONFIG_P25_ANY ?= n\nFEATURES += P25_ANY\n', 'CONFIG_P25_ANY ?= n\nFEATURES += P25_ANY\n'
         'CONFIG_DMR_ANY ?= n\nFEATURES += DMR_ANY\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_P25_ANY\n', '#ifndef CONFIG_DMR_ANY\n#define CONFIG_DMR_ANY 0\n#endif\n#ifndef CONFIG_P25_ANY\n'),
    ])
    transform('core/codeplug/codeplug.h', [
        ('enum { RADIO_MODE_P25 = 2, RADIO_MODE_YSF = 3, RADIO_MODE_NXDN = 4 };\n#endif\n',
         'enum { RADIO_MODE_P25 = 2, RADIO_MODE_YSF = 3, RADIO_MODE_NXDN = 4 };\n#endif\n'
         '// DM32 Plus (CONFIG_DMR_ANY): "any" values of a DMR channel\n'
         '#define CODEPLUG_CC_ANY\t\t16u\t// color_code: every color code\n'
         '#define CODEPLUG_TS_ANY\t\t3u\t// timeslot: either slot (the first heard holds the call)\n'
         '#define CODEPLUG_LIST_ANY\t0xFFFFu\t// listen_list: every talkgroup (hear all)\n'),
    ])
    transform('core/codeplug/codeplug.c', [
        ('\tout->timeslot = (r[CH_FLAG2] & FLAG2_TS2) ? 2 : 1;\n',
         '\tout->timeslot = (r[CH_FLAG2] & FLAG2_TS2) ? 2 : 1;\n'
         '#if CONFIG_DMR_ANY\n'
         '\tif (r[CH_MODE] == 1) {\t// DM32 Plus: the "any" bits above a DMR channel\'s color code\n'
         '\t\tif (r[CH_CC] & 0x80u)\n\t\t\tout->color_code = CODEPLUG_CC_ANY;\n'
         '\t\tif (r[CH_CC] & 0x40u)\n\t\t\tout->timeslot = CODEPLUG_TS_ANY;\n\t}\n#endif\n'),
        ('\tout->listen_list = r[CH_RX_GROUP];\n',
         '\tout->listen_list = r[CH_RX_GROUP];\n'
         '#if CONFIG_DMR_ANY\n\tif (r[CH_MODE] == 1 && (r[CH_CC] & 0x20u))\n\t\tout->listen_list = CODEPLUG_LIST_ANY;\n#endif\n'),
        ('\tr[CH_CC] = ch->color_code & 0x0Fu;\n',
         '\tr[CH_CC] = ch->color_code & 0x0Fu;\n'
         '#if CONFIG_DMR_ANY\n\tif (ch->mode == RADIO_MODE_DMR)\n'
         '\t\tr[CH_CC] |= (uint8_t)((ch->color_code == CODEPLUG_CC_ANY ? 0x80u : 0u) |\n'
         '\t\t\t\t      (ch->timeslot == CODEPLUG_TS_ANY ? 0x40u : 0u) |\n'
         '\t\t\t\t      (ch->listen_list == CODEPLUG_LIST_ANY ? 0x20u : 0u));\n#endif\n'),
    ])
    transform('core/dmr/call/dmr_call.h', [
        ('\tbool any_cc;\t\t// monitor: any color code too\n',
         '\tbool any_cc;\t\t// monitor: any color code too\n'
         '#if CONFIG_DMR_ANY\n\tbool any_ts;\t\t// DM32 Plus: either timeslot, no slot lock (the first heard holds the call)\n#endif\n'),
    ])
    transform('core/dmr/call/dmr_call.c', [
        ('\tbool other_slot = sfr_user() ? false : ts_known && b->ts != s.cfg.ts;\n',
         '#if CONFIG_DMR_ANY\n\tbool other_slot = sfr_user() || s.cfg.listen.any_ts ? false : ts_known && b->ts != s.cfg.ts;\n#else\n'
         '\tbool other_slot = sfr_user() ? false : ts_known && b->ts != s.cfg.ts;\n#endif\n'),
        ('\ts.modem->rx_lock_slot(s.cfg.rmo ? (int8_t)s.cfg.ts : 0);\n',
         '#if CONFIG_DMR_ANY\n\ts.modem->rx_lock_slot(s.cfg.rmo && !s.cfg.listen.any_ts ? (int8_t)s.cfg.ts : 0);\n#else\n'
         '\ts.modem->rx_lock_slot(s.cfg.rmo ? (int8_t)s.cfg.ts : 0);\n#endif\n', 2),
        ('\tmemcpy(s.call_lc, lc, DMR_LC_BYTES);\n\ts.call_ts = b->ts;\n',
         '\tmemcpy(s.call_lc, lc, DMR_LC_BYTES);\n\ts.call_ts = b->ts;\n'
         '#if CONFIG_DMR_ANY\n\tif (s.cfg.listen.any_ts && b->ts != 0)\n\t\ts.view.ts = (uint8_t)b->ts;\t// the slot heard\n#endif\n'),
        ('\ts.last_call_end_ms = now;\n\ts.call_end_seen = true;\n\ts.rx = RXC_NONE;\n',
         '\ts.last_call_end_ms = now;\n\ts.call_end_seen = true;\n\ts.rx = RXC_NONE;\n'
         '#if CONFIG_DMR_ANY\n\tif (s.cfg.listen.any_cc)\n\t\ts.view.cc = 16;\t// any (codeplug.h CODEPLUG_CC_ANY)\n'
         '\tif (s.cfg.listen.any_ts)\n\t\ts.view.ts = 0;\t// the UI shows the channel\'s: any\n#endif\n'),
        ('\t\t\t cfg->listen.any_cc != s.cfg.listen.any_cc || cfg->access != s.cfg.access ||\n',
         '\t\t\t cfg->listen.any_cc != s.cfg.listen.any_cc || cfg->access != s.cfg.access ||\n'
         '#if CONFIG_DMR_ANY\n\t\t\t cfg->listen.any_ts != s.cfg.listen.any_ts ||\n#endif\n'),
        ('\ts.view.ts = s.cfg.ts;\n\ts.view.rmo = s.cfg.rmo;\n',
         '\ts.view.ts = s.cfg.ts;\n\ts.view.rmo = s.cfg.rmo;\n'
         '#if CONFIG_DMR_ANY\n\tif (s.cfg.listen.any_cc)\n\t\ts.view.cc = 16;\t// any (codeplug.h CODEPLUG_CC_ANY)\n'
         '\tif (s.cfg.listen.any_ts)\n\t\ts.view.ts = 0;\n#endif\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('\tcfg.listen.hear_all = sm->view.monitor || sm->hear_all_latch;\n',
         '\tcfg.listen.hear_all = sm->view.monitor || sm->hear_all_latch;\n'
         '#if CONFIG_DMR_ANY\n\tcfg.listen.hear_all = cfg.listen.hear_all || ch->listen_list == CODEPLUG_LIST_ANY;\n'
         '\tcfg.listen.any_cc = ch->color_code == CODEPLUG_CC_ANY;\n'
         '\tcfg.listen.any_ts = ch->timeslot == CODEPLUG_TS_ANY;\n#endif\n'),
        ('\tif (codeplug_mode_nxdn(ch->mode)) return TXDENY_MODE;\t// DM32 Plus: NXDN is receive only\n',
         '\tif (codeplug_mode_nxdn(ch->mode)) return TXDENY_MODE;\t// DM32 Plus: NXDN is receive only\n'
         '#if CONFIG_DMR_ANY\n\tif (ch->mode == RADIO_MODE_DMR && (ch->color_code == CODEPLUG_CC_ANY || ch->timeslot == CODEPLUG_TS_ANY))\n'
         '\t\treturn TXDENY_MODE;\t// DM32 Plus: no single color code or slot to send on\n#endif\n'),
    ])
    # Channel editor: color code 0-15 then Any, timeslot 1 > 2 > Any, RX group list "Any (all groups)"
    transform('core/ui/ui_menus.c', [
        ('\tcase CE_CC:\n\t\tsb_u32(&b, editing ? (uint32_t)ced_val_or(ch->color_code) : ch->color_code, 1);\n\t\tbreak;\n'
         '\tcase CE_TS:\n\t\tsb_u32(&b, ch->timeslot, 1);\n\t\tbreak;\n',
         '\tcase CE_CC: {\n\t\tuint32_t cc = editing ? (uint32_t)ced_val_or(ch->color_code) : ch->color_code;\n'
         '#if CONFIG_DMR_ANY\n\t\tif (cc == CODEPLUG_CC_ANY) {\n\t\t\tsb_str(&b, TR("Any"));\n\t\t\tbreak;\n\t\t}\n#endif\n'
         '\t\tsb_u32(&b, cc, 1);\n\t\tbreak;\n\t}\n'
         '\tcase CE_TS:\n#if CONFIG_DMR_ANY\n\t\tif (ch->timeslot == CODEPLUG_TS_ANY) {\n\t\t\tsb_str(&b, TR("Any"));\n\t\t\tbreak;\n\t\t}\n#endif\n'
         '\t\tsb_u32(&b, ch->timeslot, 1);\n\t\tbreak;\n'),
        ('\tcase CE_LIST: {\n\t\tstatic listen_list_t l;\n',
         '\tcase CE_LIST: {\n#if CONFIG_DMR_ANY\n\t\tif (ch->listen_list == CODEPLUG_LIST_ANY) {\n'
         '\t\t\tsb_str(&b, TR("Any (all groups)"));\n\t\t\tbreak;\n\t\t}\n#endif\n\t\tstatic listen_list_t l;\n'),
        ('\tcase CE_CC:\n\t\tced_val = (ced_val + dir + 16) % 16;\n\t\tbreak;\n',
         '\tcase CE_CC:\n#if CONFIG_DMR_ANY\n\t\tced_val = (ced_val + dir + 17) % 17;\t// 16: any\n#else\n'
         '\t\tced_val = (ced_val + dir + 16) % 16;\n#endif\n\t\tbreak;\n'),
        ('\tcase CE_CC:\n\t\tc->color_code = (uint8_t)ced_val;\n\t\tbreak;\n',
         '\tcase CE_CC:\n\t\tc->color_code = (uint8_t)ced_val;\n'
         '#if CONFIG_DMR_ANY\n\t\tif (c->color_code == CODEPLUG_CC_ANY && !c->rx_only) {\n\t\t\tc->rx_only = true;\n'
         '\t\t\tui_toast(TR("Any color code: RX only"));\n\t\t}\n#endif\n\t\tbreak;\n'),
        ('\tcase CE_TS: c->timeslot = c->timeslot == 2 ? 1 : 2; break;\n',
         '\tcase CE_TS:\n#if CONFIG_DMR_ANY\n'
         '\t\tc->timeslot = c->timeslot == 1 ? 2 : c->timeslot == 2 ? CODEPLUG_TS_ANY : 1;\n'
         '\t\tif (c->timeslot == CODEPLUG_TS_ANY && !c->rx_only) {\n\t\t\tc->rx_only = true;\n'
         '\t\t\tui_toast(TR("Any timeslot: RX only"));\n\t\t}\n#else\n'
         '\t\tc->timeslot = c->timeslot == 2 ? 1 : 2;\n#endif\n\t\tbreak;\n'),
        ('\treturn (uint16_t)((ui_frame()->arg == 0 ? CODEPLUG_CONTACTS_MAX : CODEPLUG_LISTS_MAX) + 1u);\n',
         '#if CONFIG_DMR_ANY\n\tif (ui_frame()->arg != 0)\n\t\treturn (uint16_t)(CODEPLUG_LISTS_MAX + 2u);\t// the last: Any\n#endif\n'
         '\treturn (uint16_t)((ui_frame()->arg == 0 ? CODEPLUG_CONTACTS_MAX : CODEPLUG_LISTS_MAX) + 1u);\n'),
        ('\tconst char *name = TR("None");\n\tif (raw != 0) {\n\t\tif (ui_frame()->arg == 0 ? !codeplug_contact(raw, &chpick_c)',
         '\tconst char *name = TR("None");\n'
         '#if CONFIG_DMR_ANY\n\tif (ui_frame()->arg != 0 && raw == CODEPLUG_LISTS_MAX + 1u) {\n'
         '\t\tname = TR("Any (all groups)");\n\t\traw = 0;\n\t}\n#endif\n'
         '\tif (raw != 0) {\n\t\tif (ui_frame()->arg == 0 ? !codeplug_contact(raw, &chpick_c)'),
        ('\treturn ui_frame()->arg == 0 ? ced_ch.contact : ced_ch.listen_list;\n',
         '#if CONFIG_DMR_ANY\n\tif (ui_frame()->arg != 0 && ced_ch.listen_list == CODEPLUG_LIST_ANY)\n'
         '\t\treturn (uint16_t)(CODEPLUG_LISTS_MAX + 1u);\n#endif\n'
         '\treturn ui_frame()->arg == 0 ? ced_ch.contact : ced_ch.listen_list;\n'),
        ('\telse\n\t\tced_ch.listen_list = raw;\n',
         '#if CONFIG_DMR_ANY\n\telse if (raw == CODEPLUG_LISTS_MAX + 1u)\n\t\tced_ch.listen_list = CODEPLUG_LIST_ANY;\n#endif\n'
         '\telse\n\t\tced_ch.listen_list = raw;\n'),
    ])
    # Home, quick tiles and dual watch: an "any" color code or timeslot reads "*" until a call names it
    transform('core/ui/ui_screens.c', [
        ('\t\tsb_u32(&b, k ? (v->dmr.active ? v->dmr.cc : v->ch.color_code) : (v->dmr.ts ? v->dmr.ts : v->ch.timeslot), 1);\n',
         '#if CONFIG_DMR_ANY\n\t\tuint32_t val = k ? (v->dmr.active ? v->dmr.cc : v->ch.color_code) : (v->dmr.ts ? v->dmr.ts : v->ch.timeslot);\n'
         "\t\tif (k ? val > 15u : val > 2u)\n\t\t\tsb_chr(&b, '*');\n\t\telse\n\t\t\tsb_u32(&b, val, 1);\n#else\n"
         '\t\tsb_u32(&b, k ? (v->dmr.active ? v->dmr.cc : v->ch.color_code) : (v->dmr.ts ? v->dmr.ts : v->ch.timeslot), 1);\n#endif\n'),
        ('\t\tsb_u32(&b, v->dmr.ts ? v->dmr.ts : v->ch.timeslot, 1);\n',
         '#if CONFIG_DMR_ANY\n\t\tif ((v->dmr.ts ? v->dmr.ts : v->ch.timeslot) > 2u)\n'
         "\t\t\tsb_chr(&b, '*');\n\t\telse\n#endif\n"
         '\t\tsb_u32(&b, v->dmr.ts ? v->dmr.ts : v->ch.timeslot, 1);\n'),
        ('\t\tsb_u32(&b, editing ? s->cc_edit : v->dmr.active ? v->dmr.cc : v->ch.color_code, 1);\n',
         '#if CONFIG_DMR_ANY\n\t\tif ((editing ? s->cc_edit : v->dmr.active ? v->dmr.cc : v->ch.color_code) > 15u)\n'
         "\t\t\tsb_chr(&b, '*');\n\t\telse\n#endif\n"
         '\t\tsb_u32(&b, editing ? s->cc_edit : v->dmr.active ? v->dmr.cc : v->ch.color_code, 1);\n'),
    ])
    transform('core/ui/ui_dual.c', [
        ('\t\tsb_u32(&b, s->valid && s->ts ? s->ts : ch.timeslot ? ch.timeslot : 1u, 1);\n\t\tsb_str(&b, " CC");\n'
         '\t\tsb_u32(&b, ch.color_code, 1);\n',
         '#if CONFIG_DMR_ANY\n\t\tif ((s->valid && s->ts ? s->ts : ch.timeslot ? ch.timeslot : 1u) > 2u)\n'
         "\t\t\tsb_chr(&b, '*');\n\t\telse\n#endif\n"
         '\t\tsb_u32(&b, s->valid && s->ts ? s->ts : ch.timeslot ? ch.timeslot : 1u, 1);\n\t\tsb_str(&b, " CC");\n'
         '#if CONFIG_DMR_ANY\n\t\tif (ch.color_code > 15u)\n'
         "\t\t\tsb_chr(&b, '*');\n\t\telse\n#endif\n"
         '\t\tsb_u32(&b, ch.color_code, 1);\n'),
    ])


def prepare_dmr_search(transform, install):
    """DMR search (CONFIG_DMR_SEARCH, receive only): Tools > DMR search sweeps a range like P25 search, but the board
    stays in DMR with the call stack hearing every color code, slot and talkgroup (hear all, any_cc, any_ts of
    CONFIG_DMR_ANY). A step well over the noise floor is held while the modem reports bursts (dmr_call_heard): DMR
    there goes into the found list (frequency, color code, slots, BS / MS sync, the last call's talkgroup and source)
    and a voice call plays; the sweep goes on after it. Runs after prepare_dmr_any: it needs any_ts, and its Tools
    entry sits in the list as the later Plus features (map, air band, last call) left it."""
    install('core/radio/dmrsearch.h', 'integration/radiofoundry/spectrum/dmrsearch.h')
    install('core/radio/dmrsearch.c', 'integration/radiofoundry/spectrum/dmrsearch.c')
    install('core/ui/ui_dmrsearch.c', 'integration/radiofoundry/spectrum/ui_dmrsearch.c')
    transform('config/features.mk', [
        ('$(error P25_SEARCH needs P25_CHANNELS=y)\nendif\nendif\n',
         '$(error P25_SEARCH needs P25_CHANNELS=y)\nendif\nendif\n'
         'CONFIG_DMR_SEARCH ?= n\nFEATURES += DMR_SEARCH\n'
         'ifeq ($(CONFIG_DMR_SEARCH),y)\nifneq ($(CONFIG_DMR)$(CONFIG_DMR_ANY),yy)\n'
         '$(error DMR_SEARCH needs DMR=y and DMR_ANY=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_P25_SEARCH\n#define CONFIG_P25_SEARCH 0\n#endif\n',
         '#ifndef CONFIG_P25_SEARCH\n#define CONFIG_P25_SEARCH 0\n#endif\n'
         '#ifndef CONFIG_DMR_SEARCH\n#define CONFIG_DMR_SEARCH 0\n#endif\n'),
    ])
    # What the receiver heard since the search tuned, whatever the filters: bursts with a good check, the color code,
    # the slots with traffic (named by a repeater's CACH or TDMA direct sync), the sync families
    transform('core/dmr/call/dmr_call.h', [
        ('bool dmr_call_last_activity(uint32_t *ms);\n',
         'bool dmr_call_last_activity(uint32_t *ms);\n'
         '#if CONFIG_DMR_SEARCH\n'
         '// DM32 Plus DMR search (radio/dmrsearch.h): every burst heard with a good check since dmr_call_heard_reset(), before\n'
         '// the color code and slot filters\n'
         'typedef struct {\n'
         '\tuint16_t bursts;\n'
         '\tuint8_t cc;\t\t// of the latest burst that carries one (not voice A), 0xFF = none yet\n'
         '\tuint8_t ts_mask;\t// slots with traffic named by their sync: bit 0 TS1, bit 1 TS2\n'
         '\tuint8_t sync_mask;\t// 1 << dmr_sync_family_t of the syncs heard\n'
         '} dmr_call_heard_t;\n'
         'void dmr_call_heard(dmr_call_heard_t *out);\n'
         'void dmr_call_heard_reset(void);\n'
         '#endif\n'),
    ])
    transform('core/dmr/call/dmr_call.c', [
        ('static void rx_burst(const dmr_burst_t *b, uint32_t now, uint32_t slot)\n{\n',
         '#if CONFIG_DMR_SEARCH\nstatic dmr_call_heard_t dmrs_heard = { .cc = 0xFFu };\t// DM32 Plus DMR search\n#endif\n\n'
         'static void rx_burst(const dmr_burst_t *b, uint32_t now, uint32_t slot)\n{\n'),
        ('\ts.last_any_ms = now;\n\ts.any_seen = true;\n',
         '\ts.last_any_ms = now;\n\ts.any_seen = true;\n'
         '#if CONFIG_DMR_SEARCH\n'
         '\tif (b->crc_ok) {\t// DM32 Plus DMR search: what is on the frequency, whatever the filters below\n'
         '\t\tif (dmrs_heard.bursts != 0xFFFFu)\n\t\t\tdmrs_heard.bursts++;\n'
         '\t\tif (!(b->kind == DMR_BURST_VOICE && b->voice_index % 6 == 0))\n'
         '\t\t\tdmrs_heard.cc = (uint8_t)(b->cc & 0x0Fu);\t// voice A carries no color code\n'
         '\t\tif (b->sync != DMR_SYNC_NONE && b->sync <= DMR_SYNC_TS2)\n'
         '\t\t\tdmrs_heard.sync_mask |= (uint8_t)(1u << b->sync);\n'
         '\t\tif (b->kind != DMR_BURST_IDLE && (b->ts == 1 || b->ts == 2) &&\n'
         '\t\t    (b->sync == DMR_SYNC_BS || b->sync == DMR_SYNC_TS1 || b->sync == DMR_SYNC_TS2))\n'
         '\t\t\tdmrs_heard.ts_mask |= (uint8_t)(1u << (b->ts - 1));\n'
         '\t}\n'
         '#endif\n'),
        ('bool dmr_call_last_activity(uint32_t *ms)\n{\n',
         '#if CONFIG_DMR_SEARCH\n'
         'void dmr_call_heard(dmr_call_heard_t *out)\n{\n\t*out = dmrs_heard;\n}\n\n'
         'void dmr_call_heard_reset(void)\n{\n\tmemset(&dmrs_heard, 0, sizeof dmrs_heard);\n\tdmrs_heard.cc = 0xFFu;\n}\n\n'
         '#endif\n'
         'bool dmr_call_last_activity(uint32_t *ms)\n{\n'),
    ])
    transform('core/radio/radio_sm.h', [
        ('(radio/p25search.h)\n} radio_req_type_t;', '(radio/p25search.h)\n'
         '\tRREQ_DMR_SEARCH,\t// DM32 Plus: arg low Hz (0 = stop), arg2 high Hz (radio/dmrsearch.h)\n} radio_req_type_t;'),
    ])
    transform('core/radio/radio_task.c', [
        ('#if CONFIG_P25_SEARCH\n#include "radio/p25search.h"\n#endif\n',
         '#if CONFIG_P25_SEARCH\n#include "radio/p25search.h"\n#endif\n#if CONFIG_DMR_SEARCH\n#include "radio/dmrsearch.h"\n#endif\n'),
        ('{ post_pair(RREQ_P25_SEARCH, (int32_t)lo_hz, hi_hz, 0); }\n#endif\n',
         '{ post_pair(RREQ_P25_SEARCH, (int32_t)lo_hz, hi_hz, 0); }\n#endif\n'
         '#if CONFIG_DMR_SEARCH\nvoid radio_req_dmr_search(uint32_t lo_hz, uint32_t hi_hz)\n'
         '{ post_pair(RREQ_DMR_SEARCH, (int32_t)lo_hz, hi_hz, 0); }\n#endif\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#if CONFIG_P25_SEARCH\n#include "radio/p25search.h"\n#endif\n',
         '#if CONFIG_P25_SEARCH\n#include "radio/p25search.h"\n#endif\n#if CONFIG_DMR_SEARCH\n#include "radio/dmrsearch.h"\n#endif\n'),
        ('#if CONFIG_P25_SEARCH\n\tif (p25s_on()) open = false;\n#endif\n',
         '#if CONFIG_P25_SEARCH\n\tif (p25s_on()) open = false;\n#endif\n'
         '#if CONFIG_DMR_SEARCH\n\tif (dmrs_on()) open = false;\t// a call found plays as DMR voice (PCM)\n#endif\n'),
        # Any retune of the channel (a request, a reload, the reference trim) ends the search first
        ('static void tune(radio_sm_t *sm, uint32_t now_ms)\n{\n',
         'static void tune(radio_sm_t *sm, uint32_t now_ms)\n{\n'
         '#if CONFIG_DMR_SEARCH\n\tif (dmrs_on())\n\t\tdmrs_stop();\t// DM32 Plus: the DMR search lets the receiver go\n#endif\n'),
        # Receive only: nothing transmits while it runs (PTT, data, a repeater role, roaming probes)
        ('\tconst channel_t *ch = &sm->view.ch;\n\tif (sm->set.tx_inhibit) {\n\t\treturn TXDENY_INHIBIT;\n\t}\n',
         '\tconst channel_t *ch = &sm->view.ch;\n'
         '#if CONFIG_DMR_SEARCH\n\tif (dmrs_on())\n\t\treturn TXDENY_MODE;\t// DM32 Plus: the DMR search is receive only\n#endif\n'
         '\tif (sm->set.tx_inhibit) {\n\t\treturn TXDENY_INHIBIT;\n\t}\n'),
        ('#if CONFIG_SCAN\n\tif (scan_on(&sm->scan)) {\n\t\treturn false;\n\t}\n#endif\n'
         '#if CONFIG_FM_TX_TONES\n\tif (sm->roger_ms != 0 || sm->burst_keyed) {\n',
         '#if CONFIG_SCAN\n\tif (scan_on(&sm->scan)) {\n\t\treturn false;\n\t}\n#endif\n'
         '#if CONFIG_DMR_SEARCH\n\tif (dmrs_on()) {\n\t\treturn false;\t// no APRS job while the DMR search has the receiver\n\t}\n#endif\n'
         '#if CONFIG_FM_TX_TONES\n\tif (sm->roger_ms != 0 || sm->burst_keyed) {\n'),
        # Every (re)configuration of the call stack while searching is the search's: hear everything, no repeater
        ('\t\tcfg.permit_busy = permit_busy;\n\t}\n\tdmr_call_configure(&cfg);\n',
         '\t\tcfg.permit_busy = permit_busy;\n\t}\n'
         '#if CONFIG_DMR_SEARCH\n'
         '\tif (dmrs_on()) {\t// DM32 Plus DMR search: every color code, slot and talkgroup, receive only\n'
         '\t\tcfg.cc = 1;\n\t\tcfg.ts = 1;\n\t\tcfg.rmo = false;\n\t\tcfg.sfr_user = false;\n\t\tcfg.cc2 = -1;\n'
         '\t\tcfg.listen.hear_all = cfg.listen.any_cc = cfg.listen.any_ts = true;\n\t}\n'
         '#endif\n'
         '\tdmr_call_configure(&cfg);\n'),
        ('// --- Scan (radio/scan.c decides; this tunes) ---\n', '''#if CONFIG_DMR_SEARCH
// DM32 Plus DMR search (radio/dmrsearch.h): the board stays in DMR with the call stack hearing everything (dmr_configure
// while dmrs_on); the engine steps the receiver, this reads the level and what the modem heard. tune() ends it.
static void dmrs_tune(radio_sm_t *sm, uint32_t hz)
{
	static const subtone_t none = { TONE_NONE, 0 };
	board.radio->rx(hz, RADIO_BW_12K5, &none);
	if (sm->dmr_running) {
		const dmr_call_view_t *cv = dmr_call_view();
		if (cv->state != DMR_CALL_STATE_IDLE) {
			dmr_call_stop();	// a call heard on the last frequency ends here, not by its fade on the next one
			sm->dmr_running = dmr_call_start();
			sm->view.dmr.active = sm->dmr_running;
			sm->dmr_serial = cv->serial - 1u;
		} else {
			board.dmr->rx_restart();	// the last frequency's slot timing dropped (as dmr_begin)
		}
	}
	sm->dmr_rx_hz = hz;
	dmr_call_heard_reset();
}

// What the call stack heard since the search tuned (dmr_call_heard) and the call it plays, for the engine
static void dmrs_heard_read(dmrs_heard_t *h)
{
	dmr_call_heard_t mh;
	dmr_call_heard(&mh);
	const dmr_call_view_t *cv = dmr_call_view();
	memset(h, 0, sizeof *h);
	h->bursts = mh.bursts;
	h->cc = mh.cc != 0xFFu ? mh.cc : DMRS_CC_NONE;
	h->ts_mask = mh.ts_mask;
	if (mh.sync_mask & (1u << DMR_SYNC_BS))
		h->sync_mask |= DMRS_SYNC_BS;
	if (mh.sync_mask & ((1u << DMR_SYNC_MS) | (1u << DMR_SYNC_TS1) | (1u << DMR_SYNC_TS2)))
		h->sync_mask |= DMRS_SYNC_MS;
	h->call = cv->phase == DMR_CALL_PHASE_RX_VOICE;
	h->other = cv->phase == DMR_CALL_PHASE_RX_BUSY;
	if (h->call || h->other) {
		h->dst = cv->caller.dst;
		h->src = cv->caller.src;
		h->group = cv->caller.group;
	}
}

// Every poll while searching: the sweep's reading, or what the probe heard; retune when the engine moves on
static void __attribute__((noinline)) dmrs_service(radio_sm_t *sm, uint32_t now_ms)
{
	static uint32_t shown_ms;
	uint32_t hz;
	if (dmrs_phase() == DMRS_SWEEP) {
		radio_status_t st;
		bool hit;
		board.radio->status(&st);
		hz = dmrs_sweep(st.rssi_dbm, now_ms, &hit);
	} else {
		dmrs_heard_t h;
		dmrs_heard_read(&h);
		hz = dmrs_probe(&h, now_ms);
	}
	if (!dmrs_on()) {
		tune(sm, now_ms);	// nobody looks at the search any more: the channel again
		return;
	}
	if (hz != 0)
		dmrs_tune(sm, hz);
	if (now_ms - shown_ms >= 250u) {
		shown_ms = now_ms;
		changed(sm);	// the screen follows the search about four times a second
	}
	outputs(sm);
}
#endif

// --- Scan (radio/scan.c decides; this tunes) ---
'''),
        ("#if CONFIG_P25_SEARCH\n\t// Anything the user does but the search's own request ends the search", '''#if CONFIG_DMR_SEARCH
	// Anything the user does but the search's own request ends the DMR search: the channel is tuned again. PTT only
	// stops it (receive only: nothing is keyed until the next press)
	if (!alert && req->type != RREQ_DMR_SEARCH && req->type != RREQ_SETTINGS && req->type != RREQ_DMR_FOCUS &&
	    dmrs_on()) {
		tune(sm, now_ms);	// ends the search
		if (req->type == RREQ_PTT)
			return;
	}
#endif
#if CONFIG_P25_SEARCH
	// Anything the user does but the search's own request ends the search'''),
        ('#if CONFIG_P25_SEARCH\n\tcase RREQ_P25_SEARCH: {', '''#if CONFIG_DMR_SEARCH
	case RREQ_DMR_SEARCH: {
		if (req->arg == 0) {
			if (dmrs_on())
				tune(sm, now_ms);	// ends the search: the channel again
			break;
		}
		bool aprs_busy = false;
#if CONFIG_APRS
		aprs_busy = sm->aprs_phase != APRS_RF_OFF;
#endif
		if (busy || sm->ptt || aprs_busy || board.dmr == NULL)
			break;
		if (scan_on(&sm->scan))
			scan_stop(sm, false, now_ms);
		bool again = dmrs_on();		// a new range: the stack keeps running
		uint32_t hz = dmrs_start((uint32_t)req->arg, req->arg2, 12500u, now_ms);
		if (!again) {
			end_reception(sm, now_ms);
			if (sm->board_mode == RADIO_MODE_DMR)
				dmr_end(sm);	// the channel's call, a repeater role: the search starts the stack anew
			if (sm->board_mode != RADIO_MODE_DMR) {
				sm->board_mode = RADIO_MODE_DMR;
				board.radio->set_mode(RADIO_MODE_DMR);
			}
		}
		dmrs_tune(sm, hz);
		if (!sm->dmr_running) {
			dmr_configure(sm);	// dmrs_on: the search's (every color code, slot and talkgroup)
			sm->dmr_running = dmr_call_start();
			sm->view.dmr.active = sm->dmr_running;
			sm->dmr_serial = dmr_call_view()->serial - 1u;
		}
		if (!sm->dmr_running) {
			tune(sm, now_ms);	// no DMR modem: the search ends, the channel again
			break;
		}
		outputs(sm);
		changed(sm);
		break;
	}
#endif
#if CONFIG_P25_SEARCH
	case RREQ_P25_SEARCH: {'''),
        ('#if CONFIG_P25_SEARCH\n\tif (p25s_on()) {\n',
         '#if CONFIG_DMR_SEARCH\n\tif (dmrs_on()) {\n\t\tdmrs_service(sm, now_ms);\n\t\treturn;\n\t}\n#endif\n'
         '#if CONFIG_P25_SEARCH\n\tif (p25s_on()) {\n'),
    ])
    transform('core/ui/ui_nav.h', [
        ('(CONFIG_MAP)\n\tSCR_COUNT',
         '(CONFIG_MAP)\n\tSCR_DMRSEARCH,\t\t\t// DM32 Plus: Tools > DMR search (CONFIG_DMR_SEARCH)\n\tSCR_COUNT'),
    ])
    transform('core/ui/ui_menus.c', [
        ('// ui_p25search.c (DM32 Plus)',
         '// ui_p25search.c (DM32 Plus)\nextern const ui_scr_def_t ui_dmrsearch_screen;\t// ui_dmrsearch.c (DM32 Plus)'),
        ('\t[SCR_P25SEARCH] = &ui_p25search_screen,\n#endif\n',
         '\t[SCR_P25SEARCH] = &ui_p25search_screen,\n#endif\n'
         '#if CONFIG_DMR_SEARCH\n\t[SCR_DMRSEARCH] = &ui_dmrsearch_screen,\n#endif\n'),
        ('T_P25SEARCH, T_AIRBAND,', 'T_P25SEARCH, T_DMRSEARCH, T_AIRBAND,'),
        ('(i != T_P25SEARCH || CONFIG_P25_SEARCH) &&',
         '(i != T_P25SEARCH || CONFIG_P25_SEARCH) && (i != T_DMRSEARCH || CONFIG_DMR_SEARCH) &&'),
        ('TRN("P25 search"), TRN("Air band"),', 'TRN("P25 search"), TRN("DMR search"), TRN("Air band"),'),
        ('\tcase T_P25SEARCH: ui_push(SCR_P25SEARCH, 0); break;\n',
         '\tcase T_P25SEARCH: ui_push(SCR_P25SEARCH, 0); break;\n\tcase T_DMRSEARCH: ui_push(SCR_DMRSEARCH, 0); break;\n'),
    ])


def prepare_sfr_edit(transform):
    """A DMR channel's SFR role set on the radio (CONFIG_SFR_EDIT, docs/spec/sfr.md 4; until now only the PC Codeplug
    Editor set it). Channel editor row "SFR" cycles Off > User > ERDM user > Repeater > Repeater (ERDM style); once a
    role is set, "SFR inbound slot" (TS1 / TS2) and, for the ERDM roles, "SFR outbound CC" (the channel's or 0-15).
    Framing and hang time stay as stored (a new role: auto framing, 1 s hang, the codeplug defaults). The rows read and
    write channel_t.sfr_flag1 / sfr_a / sfr_b through sfrChannelDecode / sfrChannelEncode, and the open-firmware
    encoder now writes those bytes: a role as the channel has it, a channel without one clears a record that had one
    and keeps any other record's bytes (a CPS-made channel round-trips byte for byte). A role needs one frequency
    (radio_sfr_channel): elsewhere the row reads "Needs one frequency" and can't be set; a repeater role also needs TX
    (not RX only), and choosing one says the radio will transmit by itself."""
    transform('config/features.mk', [
        ('$(error DMR_SEARCH needs DMR=y and DMR_ANY=y)\nendif\nendif\n',
         '$(error DMR_SEARCH needs DMR=y and DMR_ANY=y)\nendif\nendif\n'
         'CONFIG_SFR_EDIT ?= n\nFEATURES += SFR_EDIT\n'
         'ifeq ($(CONFIG_SFR_EDIT),y)\nifneq ($(CONFIG_SFR)$(CONFIG_CHANNEL_EDIT),yy)\n'
         '$(error SFR_EDIT needs SFR=y and CHANNEL_EDIT=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_DMR_SEARCH\n#define CONFIG_DMR_SEARCH 0\n#endif\n',
         '#ifndef CONFIG_DMR_SEARCH\n#define CONFIG_DMR_SEARCH 0\n#endif\n'
         '#ifndef CONFIG_SFR_EDIT\n#define CONFIG_SFR_EDIT 0\n#endif\n'),
    ])
    transform('core/codeplug/codeplug.h', [
        ('void codeplug_openfw_encode(const channel_t *ch, uint8_t *rec);\n',
         'void codeplug_openfw_encode(const channel_t *ch, uint8_t *rec);\n'
         '#if CONFIG_SFR_EDIT\n'
         '// DM32 Plus: the encoder writes the channel\'s SFR bit and bytes (a channel without a role clears a record\'s).\n'
         '// This copies a record\'s into a channel built elsewhere (the CPS view of the VFO), so its bytes stay as stored\n'
         'void codeplug_openfw_sfr(const uint8_t *rec, channel_t *ch);\n'
         '#endif\n'),
    ])
    transform('core/codeplug/codeplug.c', [
        ('\t\t\t\t (ch->no_eco ? LFLAG1_NO_ECO : 0) | (ch->dmo ? LFLAG1_DMO : 0));\n',
         '\t\t\t\t (ch->no_eco ? LFLAG1_NO_ECO : 0) | (ch->dmo ? LFLAG1_DMO : 0));\n'
         '#if CONFIG_SFR_EDIT\n'
         '\t// DM32 Plus: the SFR bit and bytes as the channel has them (sfrChannelEncode in the channel editor). A channel\n'
         '\t// without a role clears a record that has one; any other record keeps its bytes (a CPS may leave 0xFF there)\n'
         '\tif (ch->sfr_flag1 & LFLAG1_SFR) {\n'
         '\t\tr[CH_LFLAG1] |= LFLAG1_SFR;\n\t\tr[CH_SFR_A] = ch->sfr_a;\n\t\tr[CH_SFR_B] = ch->sfr_b;\n'
         '\t} else if (r[CH_LFLAG1] & LFLAG1_SFR) {\n'
         '\t\tr[CH_LFLAG1] &= (uint8_t)~LFLAG1_SFR;\n\t\tr[CH_SFR_A] = 0;\n\t\tr[CH_SFR_B] = 0;\n\t}\n'
         '#endif\n'),
        ('static uint32_t ch_errors;',
         '#if CONFIG_SFR_EDIT\n'
         'void codeplug_openfw_sfr(const uint8_t *r, channel_t *ch)\n{\n'
         '\tch->sfr_flag1 = r[CH_LFLAG1] & LFLAG1_SFR;\t// as codeplug_openfw_decode reads them\n'
         '\tch->sfr_a = ch->sfr_flag1 ? r[CH_SFR_A] : 0;\n'
         '\tch->sfr_b = ch->sfr_flag1 ? r[CH_SFR_B] : 0;\n}\n\n'
         '#endif\n'
         'static uint32_t ch_errors;'),
    ])
    transform('core/cps/cps_openfw.c', [
        ('\t\treturn false;\n\t}\n\tcodeplug_openfw_encode(&ch, rec);\n',
         '\t\treturn false;\n\t}\n'
         '#if CONFIG_SFR_EDIT\n\tcodeplug_openfw_sfr(rec, &ch);\t// DM32 Plus: the VFO has no SFR role; its record keeps its bytes\n#endif\n'
         '\tcodeplug_openfw_encode(&ch, rec);\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('#endif\n       CE_ZONES, CE_GO, CE_SHARE,',
         '#endif\n#if CONFIG_SFR_EDIT\n       CE_SFR, CE_SFR_SLOT, CE_SFR_CC,\n#endif\n       CE_ZONES, CE_GO, CE_SHARE,'),
        ('[CE_P25_ID] = CE_FIELD(dmr_id),\n#endif\n};\n',
         '[CE_P25_ID] = CE_FIELD(dmr_id),\n#endif\n'
         '#if CONFIG_SFR_EDIT\n'
         '\t// DM32 Plus: the role row marks a change of any of the three SFR bytes\n'
         '\t[CE_SFR] = { offsetof(channel_t, sfr_flag1), 3, 0 }, [CE_SFR_SLOT] = { offsetof(channel_t, sfr_a), 1, 0x20 },\n'
         '\t[CE_SFR_CC] = CE_FIELD(sfr_b),\n'
         '#endif\n};\n'
         '#if CONFIG_SFR_EDIT\n'
         '_Static_assert(offsetof(channel_t, sfr_b) == offsetof(channel_t, sfr_flag1) + 2u, "the SFR bytes are together");\n'
         '#endif\n'),
        ('\nstatic void ced_build(void)\n{\n', '''
#if CONFIG_SFR_EDIT
// DM32 Plus: a DMR channel's SFR role (docs/spec/sfr.md 4), set by the rows SFR, SFR inbound slot and SFR outbound CC.
// Framing and hang time stay as stored (a new role: the codeplug's defaults, auto framing and a 1 s hang)
static sfrChannelSettings_t ced_sfr;	// static: the UI task's stack is short

// The channel's SFR settings into ced_sfr; its role (SFR_ROLE_OFF without one)
static uint8_t ced_sfr_get(const channel_t *c)
{
	(void)sfrChannelDecode(c->sfr_flag1, c->sfr_a, c->sfr_b, &ced_sfr);
	return ced_sfr.role;
}

static void ced_sfr_put(channel_t *c)
{
	sfrChannelEncode(&ced_sfr, &c->sfr_flag1, &c->sfr_a, &c->sfr_b);
}

// One frequency, as a role needs (radio_sfr_channel)
static bool ced_sfr_simplex(const channel_t *c)
{
	return c->tx_hz == c->rx_hz || c->tx_hz == 0;
}
#endif

static void ced_build(void)
{
'''),
        ('\t\tced_rows[ced_n++] = r;\n',
         '#if CONFIG_SFR_EDIT\n'
         '\t\t// DM32 Plus SFR rows: DMR only; the slot once a role is set, the outbound color code for the ERDM roles\n'
         '\t\tif (r >= CE_SFR && r <= CE_SFR_CC) {\n'
         '\t\t\tuint8_t role = dmr && ced_sfr_simplex(&ced_ch) ? ced_sfr_get(&ced_ch) : SFR_ROLE_OFF;\n'
         '\t\t\tif (!dmr || (r != CE_SFR && role == SFR_ROLE_OFF) ||\n'
         '\t\t\t    (r == CE_SFR_CC && role != SFR_ROLE_USER_ERDM && role != SFR_ROLE_RPT_MOTOROLA))\n'
         '\t\t\t\tcontinue;\n'
         '\t\t}\n'
         '#endif\n'
         '\t\tced_rows[ced_n++] = r;\n'),
        ('#endif\n\t\tTRN("Zones"), TRN("Tune to this ch"),',
         '#endif\n#if CONFIG_SFR_EDIT\n\t\t"SFR", TRN("SFR inbound slot"), TRN("SFR outbound CC"),\n#endif\n'
         '\t\tTRN("Zones"), TRN("Tune to this ch"),'),
        ('\tcase CE_ZONES:\n\t\tsb_chinfo_zones(&b, ced_num);\n', '''#if CONFIG_SFR_EDIT
	case CE_SFR: {
		static const char *const roles[SFR_ROLE_NUM_STORED] = { TRN("Off"), TRN("User"), TRN("ERDM user"),
			TRN("Repeater"), TRN("Repeater (ERDM style)") };
		if (!ced_sfr_simplex(ch))
			sb_str(&b, TR(TEXT_ALT_OPEN "Needs one frequency" TEXT_ALT_NEXT "Needs 1 freq" TEXT_ALT_CLOSE));
		else
			sb_str(&b, tr(roles[ced_sfr_get(ch)]));
		break;
	}
	case CE_SFR_SLOT:
		(void)ced_sfr_get(ch);
		sb_str(&b, ced_sfr.inboundSlot ? "TS2" : "TS1");
		break;
	case CE_SFR_CC: {
		(void)ced_sfr_get(ch);
		int32_t cc = editing ? ced_val : ced_sfr.outboundCCSet ? ced_sfr.outboundCC : 16;
		if (cc > 15)
			sb_str(&b, TR("Channel's"));	// the ERDM repeat goes out with the channel's color code
		else
			sb_u32(&b, (uint32_t)cc, 1);
		break;
	}
#endif
	case CE_ZONES:
		sb_chinfo_zones(&b, ced_num);
'''),
        ('\tcase CE_TOT:\n\t\tced_val += dir * CE_TOT_STEP;\n',
         '#if CONFIG_SFR_EDIT\n\tcase CE_SFR_CC:\n\t\tced_val = (ced_val + dir + 17) % 17;\t// 16: the channel\'s color code\n'
         '\t\tbreak;\n#endif\n'
         '\tcase CE_TOT:\n\t\tced_val += dir * CE_TOT_STEP;\n'),
        ('\tcase CE_CC:\n\tcase CE_TOT: {\n\t\tint32_t hi = ced_edit == CE_CC ? 15 : CE_TOT_MAX;\n',
         '\tcase CE_CC:\n#if CONFIG_SFR_EDIT\n\tcase CE_SFR_CC:\n\tcase CE_TOT: {\n'
         '\t\tint32_t hi = ced_edit == CE_TOT ? CE_TOT_MAX : 15;\n#else\n'
         '\tcase CE_TOT: {\n\t\tint32_t hi = ced_edit == CE_CC ? 15 : CE_TOT_MAX;\n#endif\n'),
        ('\tcase CE_TOT:\n\t\tif (ced_val % CE_TOT_STEP != 0) {\n', '''#if CONFIG_SFR_EDIT
	case CE_SFR_CC:
		(void)ced_sfr_get(c);
		ced_sfr.outboundCCSet = ced_val < 16;
		ced_sfr.outboundCC = (uint8_t)(ced_val & 15);
		ced_sfr_put(c);
		break;
#endif
	case CE_TOT:
		if (ced_val % CE_TOT_STEP != 0) {
'''),
        ('\tcase CE_NUMBER: ui_toast(TR("Save as new channel to use another number")); break;\n', '''#if CONFIG_SFR_EDIT
	case CE_SFR:
		if (!ced_sfr_simplex(c)) {
			ui_toast(TR("SFR needs one frequency: RX = TX"));
			break;
		}
#if CONFIG_DMR_ANY
		if (c->color_code == CODEPLUG_CC_ANY || c->timeslot == CODEPLUG_TS_ANY) {
			ui_toast(TR("SFR needs a fixed color code and slot"));	// "Any" has no single one to repeat on
			break;
		}
#endif
		ced_sfr.role = ced_sfr_get(c);
		ced_sfr.role = (uint8_t)((ced_sfr.role + 1u) % SFR_ROLE_NUM_STORED);
		if (sfrRoleIsRepeater(ced_sfr.role) && c->rx_only) {
			ced_sfr.role = SFR_ROLE_OFF;	// a repeater sends: not on an RX only channel
			ui_toast(TR("Repeater needs TX: RX only is on"));
		} else if (sfrRoleIsRepeater(ced_sfr.role)) {
			ui_toast(TR("Repeater role: transmits by itself"));	// licence conditions: the owner's call
		}
		ced_sfr_put(c);
		break;
	case CE_SFR_SLOT:
		(void)ced_sfr_get(c);
		ced_sfr.inboundSlot ^= 1u;
		ced_sfr_put(c);
		break;
	case CE_SFR_CC:
		(void)ced_sfr_get(c);
		ced_edit = r;
		ced_val = ced_sfr.outboundCCSet ? ced_sfr.outboundCC : 16;
		ced_typed = false;
		break;
#endif
	case CE_NUMBER: ui_toast(TR("Save as new channel to use another number")); break;
'''),
        ('\t\t\tsb_str(&b, TR("This channel has no SFR role. Set Repeater or User in the Codeplug Editor, on a channel "\n'
         '\t\t\t\t"with one frequency."));\n',
         '#if CONFIG_SFR_EDIT\n'
         '\t\t\tsb_str(&b, TR("This channel has no SFR role. Set one in the channel editor, row SFR (DMR, one "\n'
         '\t\t\t\t"frequency)."));\n'
         '#else\n'
         '\t\t\tsb_str(&b, TR("This channel has no SFR role. Set Repeater or User in the Codeplug Editor, on a channel "\n'
         '\t\t\t\t"with one frequency."));\n'
         '#endif\n'),
    ])


# DM32 Plus task stacks (words) and RTOS heap, set by the Plus build (build_radiofoundry.py OPT). Sized from the stack
# check's static worst case (logs/radiofoundry-stack-plus.log, 128 B margin and the port overhead on top) and the
# radio's measured high-water marks (R12, 2026-11-17: tasks.free.*): the worker kept 5,348 of 8,192 B free, dmr
# 1,808 of 2,048, gps 1,120 of 1,536. The 3,840 B the three stacks give back leave the heap, so its measured free
# room stays as it was and static RAM gains it.
#
# RAM reclaim (2026-11-18, docs/plus.md "RAM reclaim"): the host link task's stack the same way (static deepest
# 1,428 B in this build; upstream's 731 words cover the web view and hotspot chains, both compiled out here), Home's
# all-zero "no view yet" placeholder in flash, and the 2 KB start-up stack (main() until the scheduler runs) moved
# into the top of the RTOS heap, which nothing allocates from before the scheduler starts (prepare_startup_stack).
PLUS_HOST_TASK_WORDS = 560	# 2,240 B: static deepest 1,428 B, 616 B free above overhead and margin (was 2,924 B)
PLUS_RAM_TRIM = {
    'P25_WORKER_WORDS': 1408,	# 5,632 B: static deepest 4,528 B (the live encoder), measured 2,844 B
    'PLUS_DMR_TASK_WORDS': 320,	# 1,280 B: static deepest 284 B, measured 240 B
    'PLUS_GPS_TASK_WORDS': 256,	# 1,024 B: static deepest 356 B, measured 416 B
    # debug link log: about half a dozen lines (upstream 2,048 B: two dozen; 1,024 until the round 13 RAM reclaim)
    'PLUS_DLOG_RING_BYTES': 512,
    'PLUS_HOST_TASK_WORDS': PLUS_HOST_TASK_WORDS,
    'PLUS_VIEW_NONE_CONST': 1,	# ui_app.c view_none (720 B) in flash
    'PLUS_STARTUP_STACK_IN_HEAP': 2048,	# bytes main() may use at the top of the heap (was the 2 KB .stack section)
    'PLUS_AUDIO_MUX_NO_HOST': 1,	# audio_mux.c without its host audio side (4.4 KB; only USRP uses it: prepare_audio_mux)
    'PLUS_AUDIO_MUX_NO_APRS': 1,	# ... and without its APRS ring (2 KB; the DM-32 has its own AFSK receive path)
    # +2,048 B back (2026-11-18): the radio read heap_min_free 1,640 B with FM caller ID's 2 KB ring running, and the
    # SSTV screen takes 1,472 B more while it is open (docs/plus-radio-test.md RR1, SV7)
    'PLUS_HEAP_BYTES': 36 * 1024 + 512 - 3840 - (731 - PLUS_HOST_TASK_WORDS) * 4 + 2048,
}


def prepare_ram_trim(transform):
    """Plus RAM headroom for the next features (docs/plus.md, RAM): smaller task stacks where the stack check and the
    radio both show a large surplus, the heap shrunk by what they give back, and a shorter debug log ring. Each is
    a macro the Plus build defines; other builds keep upstream's sizes."""
    transform('platform/hrc7000/dmr_glue.c', [
        ("#define DMR_TASK_STACK\t\t512\t// words: the stack's callback runs on it\n",
         "#ifdef PLUS_DMR_TASK_WORDS\n#define DMR_TASK_STACK\t\tPLUS_DMR_TASK_WORDS\t// DM32 Plus (docs/plus.md, RAM)\n#else\n"
         "#define DMR_TASK_STACK\t\t512\t// words: the stack's callback runs on it\n#endif\n"),
    ])
    transform('platform/hrc7000/gps_hrc.c', [
        ("#define GPS_TASK_STACK\t\t384\t// words: the APRS NMEA parser runs on it\n",
         "#ifdef PLUS_GPS_TASK_WORDS\n#define GPS_TASK_STACK\t\tPLUS_GPS_TASK_WORDS\t// DM32 Plus (docs/plus.md, RAM)\n#else\n"
         "#define GPS_TASK_STACK\t\t384\t// words: the APRS NMEA parser runs on it\n#endif\n"),
    ])
    transform('core/debuglink/dlog.h', [
        ('#define DLOG_RING_BYTES\t2048\n',
         '#ifdef PLUS_DLOG_RING_BYTES\n#define DLOG_RING_BYTES\tPLUS_DLOG_RING_BYTES\t// DM32 Plus (docs/plus.md, RAM)\n#else\n'
         '#define DLOG_RING_BYTES\t2048\n#endif\n'),
    ])
    transform('boards/dm32/freertos_board.h', [
        ('#if CONFIG_P25 && (CONFIG_P25_BENCH || CONFIG_DEV_TX) && CONFIG_CALL_REPLAY\n',
         '#ifdef PLUS_HEAP_BYTES\n#define configTOTAL_HEAP_SIZE PLUS_HEAP_BYTES\t// DM32 Plus: trimmed task stacks (docs/plus.md, RAM)\n'
         '#elif CONFIG_P25 && (CONFIG_P25_BENCH || CONFIG_DEV_TX) && CONFIG_CALL_REPLAY\n'),
    ])
    transform('core/hostlink/hostlink.c', [
        ('#define HOSTLINK_TASK_STACK\t731\n',
         '#ifdef PLUS_HOST_TASK_WORDS\n#define HOSTLINK_TASK_STACK\tPLUS_HOST_TASK_WORDS\t// DM32 Plus (docs/plus.md, RAM reclaim)\n'
         '#else\n#define HOSTLINK_TASK_STACK\t731\n#endif\n'),
    ])
    # Home's view until the radio core's first tick: all zero and only ever read (view is const radio_view_t *)
    transform('core/ui/ui_app.c', [
        ('static radio_view_t view_none;\t\t// until the first tick\n',
         '#ifdef PLUS_VIEW_NONE_CONST\nstatic const radio_view_t view_none;\t// until the first tick (DM32 Plus: in flash)\n'
         '#else\nstatic radio_view_t view_none;\t\t// until the first tick\n#endif\n'),
    ])
    prepare_startup_stack(transform)
    prepare_audio_mux(transform)


def prepare_audio_mux(transform):
    """DM32 Plus (docs/plus.md, "Hotspot"): the shared PCM stream (core/usbaudio/audio_mux.c) is built here for the APRS
    receive ring only. Its host audio side (the receive ring to a host, 2 KB; the transmit jitter buffer, 2 KB; the
    host microphone block) has one user, USRP over the network (core/usbaudio/usrp_net.c, CONFIG_NET_USRP), which
    needs the IP gateway the DM32 build leaves out: nothing else calls audio_mux_host_*() or audio_mux_fm_pull(), so
    host_rx stays AUDIO_MUX_RX_OFF and host_tx_dmr false for ever and the buffers are never read or written. With
    PLUS_AUDIO_MUX_NO_HOST they go, the API stays (silence, zero counts); a build with USRP refuses the macro.
    Without the macro the file is upstream's."""
    f = 'core/usbaudio/audio_mux.c'
    transform(f, [
        ('#define RX_MASK\t\t(AUDIO_MUX_RX_CAP - 1u)\n',
         '#define RX_MASK\t\t(AUDIO_MUX_RX_CAP - 1u)\n'
         '// DM32 Plus: no host audio side (tools/plus_features.py prepare_audio_mux; only USRP uses it)\n'
         '#ifdef PLUS_AUDIO_MUX_NO_HOST\n#if CONFIG_NET_USRP\n#error "USRP needs the host audio side: no PLUS_AUDIO_MUX_NO_HOST"\n#endif\n'
         '#define MUX_HOST\t0\n#else\n#define MUX_HOST\t1\n#endif\n'),
        ('static int16_t rx_ring[AUDIO_MUX_RX_CAP];\n',
         '#if MUX_HOST\nstatic int16_t rx_ring[AUDIO_MUX_RX_CAP];\n#endif\n'),
        ('static pcm_slip_t slip;\nstatic int16_t host_mic[AUDIO_BLOCK_SAMPLES];\n',
         '#if MUX_HOST\nstatic pcm_slip_t slip;\nstatic int16_t host_mic[AUDIO_BLOCK_SAMPLES];\n#endif\n'),
        ('static void rx_put(const int16_t *pcm, uint32_t n)\n{\n',
         '#if MUX_HOST\nstatic void rx_put(const int16_t *pcm, uint32_t n)\n{\n'),
        ('\tst.rx_samples += n;\n}\n', '\tst.rx_samples += n;\n}\n#endif\n'),
        ('\tif (host_tx_dmr) {\n\t\tpcm_slip_pull(&slip, host_mic, AUDIO_BLOCK_SAMPLES);\n\t\tin = host_mic;\n'
         '\t\tst.tx_blocks++;\n\t}\n',
         '#if MUX_HOST\n\tif (host_tx_dmr) {\n\t\tpcm_slip_pull(&slip, host_mic, AUDIO_BLOCK_SAMPLES);\n\t\tin = host_mic;\n'
         '\t\tst.tx_blocks++;\n\t}\n#endif\n'),
        ('\taudio_mux_rx_t src = host_rx;\n\tif (src == AUDIO_MUX_RX_SILENT) {\n'
         '\t\tstatic const int16_t silence[AUDIO_BLOCK_SAMPLES];\n\t\trx_put(silence, AUDIO_BLOCK_SAMPLES);\n'
         '\t} else if (src != AUDIO_MUX_RX_OFF) {\n\t\trx_put(src == AUDIO_MUX_RX_OUTPUT ? spk : mic, AUDIO_BLOCK_SAMPLES);\n\t}\n',
         '#if MUX_HOST\n\taudio_mux_rx_t src = host_rx;\n\tif (src == AUDIO_MUX_RX_SILENT) {\n'
         '\t\tstatic const int16_t silence[AUDIO_BLOCK_SAMPLES];\n\t\trx_put(silence, AUDIO_BLOCK_SAMPLES);\n'
         '\t} else if (src != AUDIO_MUX_RX_OFF) {\n\t\trx_put(src == AUDIO_MUX_RX_OUTPUT ? spk : mic, AUDIO_BLOCK_SAMPLES);\n\t}\n'
         '#endif\n'),
        ('\tpcm_slip_init(&slip, AUDIO_MUX_TX_TARGET, AUDIO_BLOCK_SAMPLES);\n',
         '#if MUX_HOST\n\tpcm_slip_init(&slip, AUDIO_MUX_TX_TARGET, AUDIO_BLOCK_SAMPLES);\n#endif\n'),
        ('// --- host side ---\n', '// --- host side ---\n#if MUX_HOST\n'),
        ('uint32_t audio_mux_host_tx_fill(void)\n{\n\treturn pcm_slip_fill(&slip);\n}\n',
         'uint32_t audio_mux_host_tx_fill(void)\n{\n\treturn pcm_slip_fill(&slip);\n}\n'
         '#else\t// no host audio side: a session never starts, a host never gets or gives samples\n'
         'void audio_mux_host_rx(audio_mux_rx_t src)\n{\n\t(void)src;\n}\n\n'
         'uint32_t audio_mux_host_avail(void)\n{\n\treturn 0;\n}\n\n'
         'uint32_t audio_mux_host_read(int16_t *pcm, uint32_t n)\n{\n\t(void)pcm;\n\t(void)n;\n\treturn 0;\n}\n\n'
         'void audio_mux_host_push(const int16_t *pcm, uint32_t n)\n{\n\t(void)pcm;\n\t(void)n;\n}\n\n'
         'void audio_mux_host_tx(bool on, bool dmr)\n{\n\t(void)on;\n\t(void)dmr;\n}\n\n'
         'uint32_t audio_mux_host_pushes(void)\n{\n\treturn st.tx_pushes;\n}\n\n'
         'uint32_t audio_mux_host_tx_fill(void)\n{\n\treturn 0;\n}\n#endif\n'),
        ('\tpcm_slip_pull(&slip, pcm, n);\n\tst.fm_pulls += n;\n',
         '#if MUX_HOST\n\tpcm_slip_pull(&slip, pcm, n);\n#else\n\tmemset(pcm, 0, n * sizeof *pcm);\n#endif\n\tst.fm_pulls += n;\n'),
        ('\t*out = slip.stats;\n', '#if MUX_HOST\n\t*out = slip.stats;\n#else\n\tmemset(out, 0, sizeof *out);\n#endif\n'),
    ])
    prepare_audio_mux_no_aprs(transform)


def prepare_audio_mux_no_aprs(transform):
    """DDM32 Plus (docs/dota.md, phase 2 step 0): APRS receive reads the board's own AFSK receive path when the board
    has one (aprs.c rx_service: own = rx_by_board()); the shared stream's APRS ring (2,048 B) is read only on boards
    without one (rx_by_stream: an audio tap with AUDIO_TAP_FM_RX). The DM-32 has its own path (hrc_afsk rx_*) and no
    audio tap, so the ring is never written or read there. With PLUS_AUDIO_MUX_NO_APRS it goes: the API stays (the
    stream carries nothing for APRS), and aprs.c no longer offers the stream as a receive path, so a board without its
    own path built this way has no APRS receive instead of a silent one. Without the macro the file is as before."""
    transform('core/usbaudio/audio_mux.c', [
        ('#define MUX_HOST\t1\n#endif\n',
         '#define MUX_HOST\t1\n#endif\n'
         '// DDM32 Plus: no APRS ring where the board has its own AFSK receive path (docs/dota.md step 0)\n'
         '#if CONFIG_APRS_RX && !defined(PLUS_AUDIO_MUX_NO_APRS)\n#define MUX_APRS\t1\n#else\n#define MUX_APRS\t0\n#endif\n'),
        ('#if CONFIG_APRS_RX\n// --- APRS (radio task) ---\n', '#if MUX_APRS\n// --- APRS (radio task) ---\n'),
        ('\taprs_rd = r + n;\n\treturn n;\n}\n#endif\n',
         '\taprs_rd = r + n;\n\treturn n;\n}\n'
         '#elif CONFIG_APRS_RX\t// PLUS_AUDIO_MUX_NO_APRS: the board has its own path; the stream carries nothing for APRS\n'
         'void audio_mux_aprs_rx(bool on)\n{\n\t(void)on;\n}\n\n'
         'uint32_t audio_mux_aprs_read(int16_t *pcm, uint32_t n)\n{\n\t(void)pcm;\n\t(void)n;\n\treturn 0;\n}\n#endif\n'),
        ('#if CONFIG_APRS_RX\n', '#if MUX_APRS\n', 3),
    ])
    transform('core/aprs/aprs.c', [
        ('static bool rx_by_stream(void)\n{\n\treturn board.audio != NULL',
         'static bool rx_by_stream(void)\n{\n'
         '#ifdef PLUS_AUDIO_MUX_NO_APRS\n\treturn false;\t// DDM32 Plus: the shared stream has no APRS ring (audio_mux.c)\n#endif\n'
         '\treturn board.audio != NULL'),
    ])


def prepare_startup_stack(transform):
    """DM32 Plus: the start-up stack (crt0 to vTaskStartScheduler, 2 KB of its own after .bss) at the top of the RTOS
    heap instead. heap_4 hands out its single free block from the low end and nothing frees before the scheduler
    starts, so before then the heap's top is never written except heap_4's end marker (the last 8 bytes, below which
    the stack starts); once the first task runs, main()'s frames are dead (it never returns, interrupts have their own
    stack, platform/hrc7000/freertos/portasm.S) and the heap may use the bytes. The start-up task checks the least
    free heap so far before anything else: if the allocations before the scheduler ever reach main()'s 2 KB it stops
    with a crash record (configASSERT) instead of running on a corrupted object. Other builds: the heap is heap_4's
    own array and the linker script lays out exactly as before."""
    transform('boards/dm32/freertos_board.h', [
        ('#ifdef PLUS_HEAP_BYTES\n',
         '#ifdef PLUS_STARTUP_STACK_IN_HEAP\n#define configAPPLICATION_ALLOCATED_HEAP 1\t// boards/dm32/board.c (docs/plus.md, RAM reclaim)\n'
         '#endif\n#ifdef PLUS_HEAP_BYTES\n'),
    ])
    transform('boards/dm32/board.c', [
        ('void board_init(void)\n{\n\thrc_flash_spi0_configure(&flash_cfg);\n',
         '#ifdef PLUS_STARTUP_STACK_IN_HEAP\n'
         '// DM32 Plus: the RTOS heap in its own section, last in .bss: the start-up stack runs at its top until the\n'
         '// scheduler starts (platform/hrc7000/common.ld __stack_top; tools/plus_features.py prepare_startup_stack)\n'
         'uint8_t ucHeap[configTOTAL_HEAP_SIZE] __attribute__((section(".plus_heap"), aligned(8)));\n'
         '_Static_assert(configTOTAL_HEAP_SIZE >= 8 * PLUS_STARTUP_STACK_IN_HEAP, "the start-up stack is a small part of the heap");\n'
         '#endif\n\n'
         'void board_init(void)\n{\n\thrc_flash_spi0_configure(&flash_cfg);\n'),
    ])
    transform('core/app/app.c', [
        ('static void startup_task(void *arg)\n{\n\t(void)arg;\n',
         '#ifdef PLUS_STARTUP_STACK_IN_HEAP\n'
         '// DM32 Plus: nothing allocated before the scheduler reached the heap top main() ran on (boards/dm32/board.c\n'
         '// ucHeap). Without a free heap_4 hands out its one free block from the low end, so the least free so far bounds\n'
         '// the highest byte ever allocated (tests/plus/test_startup_heap.c). Its own frame: not under the task\'s deepest path\n'
         'static __attribute__((noinline)) void plus_startup_stack_check(void)\n{\n'
         '\tHeapStats_t h;\n\tvPortGetHeapStats(&h);\n'
         '\tconfigASSERT(h.xNumberOfSuccessfulFrees == 0 && h.xMinimumEverFreeBytesRemaining >= PLUS_STARTUP_STACK_IN_HEAP + 64u);\n'
         '}\n#endif\n\n'
         'static void startup_task(void *arg)\n{\n\t(void)arg;\n'
         '#ifdef PLUS_STARTUP_STACK_IN_HEAP\n\tplus_startup_stack_check();\n#endif\n'),
    ])
    # Without a .plus_heap input section (every other build) __plus_heap_end == __plus_heap_start: the start-up
    # stack keeps its 2 KB after .bss and __stack_top its address, so the image is byte for byte the same
    transform('platform/hrc7000/common.ld', [
        ('__stack_top = ORIGIN(RAM) + LENGTH(RAM) - 8;\t/* just below the codec window */\n',
         '__ram_top = ORIGIN(RAM) + LENGTH(RAM) - 8;\t/* just below the codec window */\n'),
        ('\t\t*(.bss .bss.* .sbss .sbss.* COMMON)\n\t\t. = ALIGN(4);\n\t\t__bss_end = .;\n',
         '\t\t*(.bss .bss.* .sbss .sbss.* COMMON)\n'
         '\t\t__plus_heap_start = .;\n'
         '\t\tKEEP(*(.plus_heap))\t/* DM32 Plus: the RTOS heap last, the start-up stack at its top (board.c) */\n'
         '\t\t__plus_heap_end = .;\n'
         '\t\t. = ALIGN(4);\n\t\t__bss_end = .;\n'),
        ('\t\t. = . + _Min_Stack_Size;\n\t} >RAM\n',
         '\t\t. = . + (__plus_heap_end > __plus_heap_start ? 0 : _Min_Stack_Size);\n\t} >RAM\n'
         '\t__stack_top = __plus_heap_end > __plus_heap_start ? ((__plus_heap_end - 16) & ~7) : __ram_top;\n'),
        ('\tASSERT(. <= __stack_top, "RAM overflows into the start-up stack and the codec window")\n',
         '\tASSERT(. <= __ram_top, "RAM overflows into the start-up stack and the codec window")\n'
         '\tASSERT(__plus_heap_end == __plus_heap_start || __plus_heap_end - __plus_heap_start >= 8 * _Min_Stack_Size,\n'
         '\t       "the start-up stack needs the top of a heap of 16 KB at least")\n'),
    ])


def ram_reclaim_audit(nm_text, size_text):
    """Check a linked Plus image (csky-elf-nm -S, csky-elf-size -A) for the RAM reclaim layout; SystemExit if not.
    The start-up stack top (crt0's __stack_top) inside ucHeap, below heap_4's end marker (the heap's last 8 bytes,
    8-byte aligned; 32-bit BlockLink_t), with PLUS_STARTUP_STACK_IN_HEAP bytes of the heap under it; no 2 KB .stack
    section any more; view_none in flash."""
    sym = {}
    for line in nm_text.splitlines():
        f = line.split()
        if len(f) == 4:
            sym[f[3]] = (int(f[0], 16), int(f[1], 16), f[2])
        elif len(f) == 3:
            sym[f[2]] = (int(f[0], 16), 0, f[1])
    sections = {f[0]: int(f[1]) for f in (l.split() for l in size_text.splitlines()) if len(f) == 3 and f[1].isdigit()}
    heap, heap_size, kind = sym.get('ucHeap', (0, 0, ''))
    top = sym.get('__stack_top', (0, 0, ''))[0]
    end_marker = (heap + heap_size - 8) & ~7
    need = PLUS_RAM_TRIM['PLUS_STARTUP_STACK_IN_HEAP']
    problems = []
    if kind != 'B' or heap_size != PLUS_RAM_TRIM['PLUS_HEAP_BYTES'] or heap % 8:
        problems.append('ucHeap is not the board\'s global, 8-byte aligned heap of PLUS_HEAP_BYTES')
    if top != (heap + heap_size - 16) & ~7 or not (heap + need <= top - need < top < end_marker):
        problems.append(f'start-up stack top {top:#x} is not just below the end of ucHeap {heap:#x}+{heap_size}')
    if sections.get('.stack', 0) > 8:
        problems.append(f'.stack still reserves {sections[".stack"]} B')
    view = sym.get('view_none', (0, 0, ''))
    if PLUS_RAM_TRIM.get('PLUS_VIEW_NONE_CONST') and not (view[2] in ('r', 'R') and view[0] >= 0x03000000):
        problems.append('view_none is not in flash')
    if PLUS_RAM_TRIM.get('PLUS_AUDIO_MUX_NO_APRS') and 'aprs_ring' in sym:
        problems.append('audio_mux.c still holds its APRS ring')	# docs/dota.md step 0
    if problems:
        raise SystemExit('Plus RAM reclaim audit: ' + '; '.join(problems))
    return {'plus_startup_stack_in_heap': True, 'plus_startup_stack_top': hex(top), 'plus_heap': [hex(heap), heap_size]}


def prepare_nxdn(transform, install):
    """NXDN48 receive channels (CONFIG_NXDN, docs/nxdn.md; ported from the GPL-3.0-or-later DM32-nxdn-p25, MMDVMHost
    GPL-2.0-or-later). Unlike YSF it is not a native-modem channel: the radio stays on the FM path and a software
    4FSK demodulator reads the 8 kHz FM RX audio (the APRS ring); voice (AMBE+2) goes to the DMR vocoder. Record:
    CH_MODE 0xA7, RAN in CH_RX_GROUP (0 = any, 1-63), always RX only. codeplug_mode_p25() stays false for it."""
    for f in ('nxdn.c', 'nxdn.h', 'nxdn_demod.c', 'nxdn_demod.h', 'nxdn_demod_tables.inc', 'nxdn_rx.c', 'nxdn_rx.h'):
        install('core/nxdn/' + f, 'src/nxdn/' + f)
    install('core/radio/radio_nxdn.c', 'integration/radiofoundry/nxdn/radio_nxdn.c')
    install('core/radio/radio_nxdn.h', 'integration/radiofoundry/nxdn/radio_nxdn.h')
    install('boards/dm32/dm32_nxdn.inc', 'integration/radiofoundry/nxdn/dm32_nxdn.inc')
    transform('config/features.mk', [
        ('CONFIG_NAV ?= n\nFEATURES += NAV\n', 'CONFIG_NAV ?= n\nFEATURES += NAV\nCONFIG_NXDN ?= n\nFEATURES += NXDN\n'
         # (APRS_RX, the FM audio ring, is checked in radio_nxdn.c: the board registry evaluates every board)
         'ifeq ($(CONFIG_NXDN),y)\nifneq ($(CONFIG_DMR)$(CONFIG_P25_CHANNELS),yy)\n'
         '$(error NXDN needs DMR=y and P25_CHANNELS=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_NAV\n', '#ifndef CONFIG_NXDN\n#define CONFIG_NXDN 0\n#endif\n#ifndef CONFIG_NAV\n'),
    ])
    transform('Makefile', [
        ('APP_DIRS := $(CORE_DIRS)', 'ifeq ($(CONFIG_NXDN),y)\nCORE_DIRS += core/nxdn\nendif\nAPP_DIRS := $(CORE_DIRS)'),
    ])
    # boards/dm32/radio_dm32.c includes dm32_nxdn.inc after dm32_native.inc (tools/build_radiofoundry.py)
    transform('core/codeplug/codeplug.h', [
        ('enum { RADIO_MODE_P25 = 2, RADIO_MODE_YSF = 3 };', 'enum { RADIO_MODE_P25 = 2, RADIO_MODE_YSF = 3, RADIO_MODE_NXDN = 4 };'),
        ('#if CONFIG_P25_CHANNELS\nstatic inline bool codeplug_p25_valid(const channel_t *ch)\n{\n',
         '// DM32 Plus: an NXDN48 channel (FM path, software demodulator; not a native-modem channel)\n'
         'static inline bool codeplug_mode_nxdn(uint8_t mode)\n{\n#if CONFIG_P25_CHANNELS && CONFIG_NXDN\n'
         '\treturn mode == RADIO_MODE_NXDN;\n#else\n\t(void)mode;\n\treturn false;\n#endif\n}\n'
         '#if CONFIG_P25_CHANNELS\nstatic inline bool codeplug_p25_valid(const channel_t *ch)\n{\n'
         '\tif (codeplug_mode_nxdn(ch->mode))\n\t\treturn ch->bw == RADIO_BW_12K5 && ch->rx_only && ch->p25_tg <= 63u;\n'),
    ])
    transform('core/codeplug/codeplug.c', [
        ('!(CONFIG_YSF && r[CH_MODE] == 0xA6u)) return false;',
         '!(CONFIG_YSF && r[CH_MODE] == 0xA6u) &&\n\t    !(CONFIG_NXDN && r[CH_MODE] == 0xA7u)) return false;'),
        ('\t\tout->rx_only = true;\n\t\treturn codeplug_p25_valid(out);\n\t}\n#endif\n#endif\n',
         '\t\tout->rx_only = true;\n\t\treturn codeplug_p25_valid(out);\n\t}\n#endif\n'
         '#if CONFIG_NXDN\n\tif (r[CH_MODE] == 0xA7u) {\n\t\tout->mode = RADIO_MODE_NXDN;\n\t\tout->p25_nac = 0;\n'
         '\t\tout->p25_tg = (uint16_t)(r[CH_RX_GROUP] & 0x3Fu);\n\t\tout->p25_id = 0;\n'
         '\t\tout->dmo = out->roam = out->vox = false;\n\t\tout->rx_only = true;\n\t\tout->bw = RADIO_BW_12K5;\n'
         '\t\treturn codeplug_p25_valid(out);\n\t}\n#endif\n#endif\n'),
        ('r[CH_MODE] = codeplug_mode_ysf(ch->mode) ? 0xA6u :', 'r[CH_MODE] = codeplug_mode_nxdn(ch->mode) ? 0xA7u : codeplug_mode_ysf(ch->mode) ? 0xA6u :'),
        ('\t\tr[CH_RX_GROUP] = (uint8_t)(codeplug_mode_ysf(ch->mode) ? ch->p25_tg : ch->p25_nac);\n',
         '\t\tr[CH_RX_GROUP] = (uint8_t)(codeplug_mode_ysf(ch->mode) ? ch->p25_tg : ch->p25_nac);\n'
         '\tif (codeplug_mode_nxdn(ch->mode))\n\t\tr[CH_RX_GROUP] = (uint8_t)(ch->p25_tg & 0x3Fu);\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#include "radio/spectrum.h"\n', '#include "radio/spectrum.h"\n#if CONFIG_NXDN\n#include "radio/radio_nxdn.h"\n#include "hal_time.h"\n#endif\n'),
        ('\taprs_poll(sm, now_ms);', '''\taprs_poll(sm, now_ms);
#if CONFIG_NXDN
	{
		// DM32 Plus: an NXDN channel listens on the FM audio while nothing else has the receiver
		bool nxdn = codeplug_mode_nxdn(sm->view.ch.mode) && sm->view.state != RS_TX &&
			    sm->board_mode == RADIO_MODE_FM && !sm->view.bcast;
#if CONFIG_SCAN
		nxdn = nxdn && !scan_on(&sm->scan);
#endif
#if CONFIG_SPECTRUM
		nxdn = nxdn && !spectrum_on();
#endif
#if CONFIG_P25_SEARCH
		nxdn = nxdn && !p25s_on();
#endif
		radio_nxdn_poll(nxdn, sm->view.ch.rx_hz, (uint8_t)sm->view.ch.p25_tg, now_ms);
	}
#endif'''),
        ('\tif (codeplug_mode_p25(sm->view.ch.mode)) { open = false; green = sm->view.state == RS_RX; }\n',
         '\tif (codeplug_mode_p25(sm->view.ch.mode)) { open = false; green = sm->view.state == RS_RX; }\n'
         '#if CONFIG_NXDN\n\tif (codeplug_mode_nxdn(sm->view.ch.mode)) { open = false; green = radio_nxdn_heard(hal_millis()); }\n#endif\n'),
        ('\tif (codeplug_mode_p25(ch->mode) && !codeplug_p25_valid(ch)) return TXDENY_MODE;\n',
         '\tif (codeplug_mode_p25(ch->mode) && !codeplug_p25_valid(ch)) return TXDENY_MODE;\n'
         '\tif (codeplug_mode_nxdn(ch->mode)) return TXDENY_MODE;\t// DM32 Plus: NXDN is receive only\n'),
    ])
    nxdn_label = 'codeplug_mode_nxdn(%s) ? "NXDN" : codeplug_mode_ysf(%s) ? "YSF"'
    transform('core/ui/ui_menus.c', [
        ('codeplug_mode_ysf(ch->mode) ? "YSF"', nxdn_label % ('ch->mode', 'ch->mode'), 2),
        ('\t\t} else if (codeplug_mode_p25(c->mode)) {\n\t\t\tc->mode = RADIO_MODE_FM;',
         '\t\t} else if (CONFIG_NXDN && c->mode == RADIO_MODE_YSF) {\n'
         '\t\t\tc->mode = RADIO_MODE_NXDN; c->p25_nac = 0; c->p25_tg = 0; c->p25_id = 0; c->rx_only = true;\n'
         '\t\t\tc->bw = RADIO_BW_12K5;\n'
         '\t\t} else if (codeplug_mode_p25(c->mode) || codeplug_mode_nxdn(c->mode)) {\n\t\t\tc->mode = RADIO_MODE_FM;'),
        ('\t\tbool p25 = codeplug_mode_p25(ced_ch.mode);\n',
         '\t\tbool p25 = codeplug_mode_p25(ced_ch.mode) || codeplug_mode_nxdn(ced_ch.mode);\t// NXDN: RAN row, no FM rows\n'),
        ('\t\tif (codeplug_mode_ysf(ced_ch.mode) && (r == CE_P25_NAC || r == CE_P25_TX_NAC || r == CE_P25_ID)) continue;\n',
         '\t\tif ((codeplug_mode_ysf(ced_ch.mode) || codeplug_mode_nxdn(ced_ch.mode)) && (r == CE_P25_NAC || r == CE_P25_TX_NAC || r == CE_P25_ID))\n'
         '\t\t\tcontinue;\n'),
        ('\tif (r == CE_P25_TG && codeplug_mode_ysf(ch->mode))\n\t\tstr_copy(it->label, sizeof it->label, "DG-ID");\n',
         '\tif (r == CE_P25_TG && codeplug_mode_ysf(ch->mode))\n\t\tstr_copy(it->label, sizeof it->label, "DG-ID");\n'
         '\tif (r == CE_P25_TG && codeplug_mode_nxdn(ch->mode))\n\t\tstr_copy(it->label, sizeof it->label, "RAN");\n'),
        ('\tif (codeplug_mode_p25(c->mode) && !codeplug_p25_valid(c))\n',
         '\tif ((codeplug_mode_p25(c->mode) || codeplug_mode_nxdn(c->mode)) && !codeplug_p25_valid(c))\n'),
    ])
    transform('core/ui/ui_screens.c', [
        ('codeplug_mode_ysf(v->ch.mode) ? "YSF"', nxdn_label % ('v->ch.mode', 'v->ch.mode')),
        ('#include "config.h"\n', '#include "config.h"\n#if CONFIG_NXDN\n#include "radio/radio_nxdn.h"\n#endif\n'),
        ('\tui_draw_bottom(s, bottom);', '''#if CONFIG_NXDN
	{
		// DM32 Plus: who is talking on an NXDN channel ("NXDN 1234 > TG 101", "NXDN ENC 1234", "NXDN call")
		static char nxdn_line[32];	// static: the UI task's stack is tight
		static radio_nxdn_caller_t nc;
		radio_nxdn_caller(&nc);
		if (codeplug_mode_nxdn(v->ch.mode) && nc.active) {
			sbuf_t nb;
			sb_init(&nb, nxdn_line, sizeof nxdn_line);
			sb_str(&nb, "NXDN ");
			if (nc.encrypted)
				sb_str(&nb, "ENC ");
			if (nc.source == 0 && !nc.encrypted) {
				sb_str(&nb, "call");
			} else {
				sb_u32(&nb, nc.source, 1);
				if (!nc.encrypted) {
					sb_str(&nb, nc.group ? " > TG " : " > ");
					sb_u32(&nb, nc.destination, 1);
				}
			}
			bottom = nxdn_line;
		}
	}
#endif
	ui_draw_bottom(s, bottom);'''),
    ])
    transform('core/debuglink/debuglink_view.c', [
        ('codeplug_mode_ysf(v->ch.mode) ? "ysf"', 'codeplug_mode_nxdn(v->ch.mode) ? "nxdn" : codeplug_mode_ysf(v->ch.mode) ? "ysf"'),
        ('codeplug_mode_ysf(c->mode) ? "ysf"', 'codeplug_mode_nxdn(c->mode) ? "nxdn" : codeplug_mode_ysf(c->mode) ? "ysf"'),
    ])


def prepare_map(transform, install):
    """Offline Turkey map (CONFIG_MAP, needs NAV: GPS, APRS stations and waypoints): Tools > Map (docs/map.md).
    Natural Earth 5.1.1 admin-1 (public domain), names in ISO-8859-9 for the Turkish fonts (tools/turkey_map.py);
    the 973 districts as points from GeoNames (CC BY 4.0, tools/turkey_districts.py); the built-in repeaters
    (CONFIG_REPEATERS) as triangles, # picks the nearest to the centre, OK opens its details."""
    install('core/ui/map.h', 'integration/radiofoundry/map/map.h')
    install('core/ui/map.c', 'integration/radiofoundry/map/map.c')
    install('core/ui/turkey_map_data.h', 'integration/radiofoundry/map/turkey_map_data.h')
    install('core/ui/turkey_map_data.c', 'integration/radiofoundry/map/turkey_map_data.c')
    install('core/ui/turkey_district_data.h', 'integration/radiofoundry/map/turkey_district_data.h')
    install('core/ui/turkey_district_data.c', 'integration/radiofoundry/map/turkey_district_data.c')
    install('core/ui/ui_map.c', 'integration/radiofoundry/map/ui_map.c')
    transform('config/features.mk', [
        ('CONFIG_NAV ?= n\nFEATURES += NAV\n', 'CONFIG_NAV ?= n\nFEATURES += NAV\nCONFIG_MAP ?= n\nFEATURES += MAP\n'
         'ifeq ($(CONFIG_MAP),y)\nifneq ($(CONFIG_NAV),y)\n$(error MAP needs NAV=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_NAV\n', '#ifndef CONFIG_MAP\n#define CONFIG_MAP 0\n#endif\n#ifndef CONFIG_NAV\n'),
    ])
    transform('core/ui/ui_nav.h', [
        ('(CONFIG_P25_SEARCH)\n\tSCR_COUNT',
         '(CONFIG_P25_SEARCH)\n\tSCR_MAP,\t\t\t// DM32 Plus: Tools > Map (CONFIG_MAP)\n\tSCR_COUNT'),
    ])
    transform('core/ui/ui_menus.c', [
        ('// ui_goto.c (DM32 Plus)', '// ui_goto.c (DM32 Plus)\nextern const ui_scr_def_t ui_map_screen;\t// ui_map.c (DM32 Plus)'),
        ('\t[SCR_GOTO] = &ui_goto_screen,\n#endif\n',
         '\t[SCR_GOTO] = &ui_goto_screen,\n#endif\n#if CONFIG_MAP\n\t[SCR_MAP] = &ui_map_screen,\n#endif\n'),
        ('T_ALMANAC, T_GOTO, T_P25SEARCH,', 'T_ALMANAC, T_GOTO, T_MAP, T_P25SEARCH,'),
        ('(i != T_GOTO || CONFIG_NAV) &&', '(i != T_GOTO || CONFIG_NAV) && (i != T_MAP || CONFIG_MAP) &&'),
        ('TRN("Go-To"), TRN("P25 search"),', 'TRN("Go-To"), TRN("Map"), TRN("P25 search"),'),
        ('\tcase T_GOTO: ui_push(SCR_GOTO, 0); break;',
         '\tcase T_GOTO: ui_push(SCR_GOTO, 0); break;\n#if CONFIG_MAP\n\tcase T_MAP: ui_push(SCR_MAP, 0); break;\n#endif'),
    ])


def prepare_art(transform, install):
    """DM32 Plus look (CONFIG_PLUS_ART, docs/art.md): our own boot splash (mark + "DM32 Plus" wordmark) and 16x16
    icons in the Tools list, 1-bpp masks tinted with the row's colour (no palette entries, every theme). Runs after
    every other Plus feature: it names their Tools entries."""
    for f in ('art.h', 'art.c', 'art_data.h', 'art_data.c', 'splash_plus.c'):
        install('core/ui/' + f, 'integration/radiofoundry/art/' + f)
    transform('config/features.mk', [
        ('CONFIG_NAV ?= n\nFEATURES += NAV\n', 'CONFIG_NAV ?= n\nFEATURES += NAV\nCONFIG_PLUS_ART ?= n\nFEATURES += PLUS_ART\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_NAV\n', '#ifndef CONFIG_PLUS_ART\n#define CONFIG_PLUS_ART 0\n#endif\n#ifndef CONFIG_NAV\n'),
    ])
    transform('core/ui/ui_screens.c', [
        ('#include "ui_svc.h"\n', '#include "ui_svc.h"\n#if CONFIG_PLUS_ART\n#include "art.h"\n#endif\n'),
        ('void ui_draw_splash(const char *board_name, const char *version, const char *line1, const char *line2)\n{\n',
         'void ui_draw_splash(const char *board_name, const char *version, const char *line1, const char *line2)\n{\n'
         '#if CONFIG_PLUS_ART\n\tif (ui_draw_splash_plus(board_name, version, line1, line2))\n\t\treturn;\n#endif\n'),
        ('\t\tit.num = 0;\n', '\t\tit.num = 0;\n#if CONFIG_PLUS_ART\n\t\tit.icon = 0;\n#endif\n'),
        ('\t\t\tx = gfx_text(f, x, ry + pad, n, focus ? fg : (gfx_bpp() == 1 ? fg : ui_col.dim));\n\t\t}\n',
         '\t\t\tx = gfx_text(f, x, ry + pad, n, focus ? fg : (gfx_bpp() == 1 ? fg : ui_col.dim));\n\t\t}\n'
         '#if CONFIG_PLUS_ART\n\t\tif (it.icon && L->row_h >= ART_ICON_H + 2) {\n'
         '\t\t\tart_icon(it.icon - 1u, x, ry + (L->row_h - ART_ICON_H) / 2, fg);\n'
         '\t\t\tx += ART_ICON_W + 5;\n\t\t}\n#endif\n'),
    ])
    # The splash is drawn before ui_app_init applies the saved settings, so it showed the compiled-in theme (Classic's
    # light one, tools/classic_channels.py) on a radio set to dark. Settings are loaded before the UI task starts
    # (app.c: settings_init, then ui_start), so the saved theme is already there to read.
    transform('core/ui/ui.c', [
        ('\t\tui_layout_init();\n\t\tchar line1[17], line2[17];\n',
         '\t\tui_layout_init();\n#if CONFIG_PLUS_ART\n'
         '\t\tui_theme_set((uint8_t)ui_svc_setting_get(UI_SET_THEME));\t// DDM32 Plus: the splash in the saved theme\n'
         '#endif\n\t\tchar line1[17], line2[17];\n'),
    ])
    transform('core/ui/ui_screens.h', [
        ("\tchar num;\t\t// key shown before the label ('1'..'9', '0'), 0 = none\n",
         "\tchar num;\t\t// key shown before the label ('1'..'9', '0'), 0 = none\n#if CONFIG_PLUS_ART\n"
         "\tuint8_t icon;\t\t// DM32 Plus: ART_ICON_* + 1 drawn before the label, 0 = none\n#endif\n"),
    ])
    transform('core/ui/ui_menus.c', [
        ('#include "fmt.h"\n', '#include "fmt.h"\n#if CONFIG_PLUS_ART\n#include "art.h"\n#endif\n'),
        ('\titem_num(it, (uint16_t)(i + 1u));\t// the tool\'s own number, not its row (see the enum)\n',
         '\titem_num(it, (uint16_t)(i + 1u));\t// the tool\'s own number, not its row (see the enum)\n'
         '#if CONFIG_PLUS_ART\n\tstatic const uint8_t tool_icon[T_COUNT] = {\n'
         '\t\t[T_SCAN] = ART_ICON_SCAN + 1, [T_POSITION] = ART_ICON_POSITION + 1, [T_HOTSPOT] = ART_ICON_HOTSPOT + 1,\n'
         '\t\t[T_USB_NET] = ART_ICON_USB_NET + 1, [T_SFR] = ART_ICON_SFR + 1, [T_ROAM] = ART_ICON_ROAM + 1,\n'
         '\t\t[T_DIAG] = ART_ICON_DIAG + 1, [T_APRS] = ART_ICON_APRS + 1, [T_SPECTRUM] = ART_ICON_SPECTRUM + 1,\n'
         '\t\t[T_FOX] = ART_ICON_FOX + 1, [T_BEACON] = ART_ICON_BEACON + 1, [T_LONE] = ART_ICON_LONE + 1,\n'
         '\t\t[T_ALMANAC] = ART_ICON_ALMANAC + 1, [T_GOTO] = ART_ICON_GOTO + 1, [T_MAP] = ART_ICON_MAP + 1,\n'
         '\t\t[T_SATELLITES] = ART_ICON_ALMANAC + 1,\t// the sky, as Sun & Moon\n'
         '\t\t[T_P25SEARCH] = ART_ICON_P25SEARCH + 1, [T_AIRBAND] = ART_ICON_AIRBAND + 1,\n'
         '\t\t[T_DMRSEARCH] = ART_ICON_P25SEARCH + 1,\t// the same magnifier\n'
         '\t\t[T_REPLAY] = ART_ICON_RECENT + 1, [T_BATTERY] = ART_ICON_BATTERY + 1,\n'
         '\t\t[T_ZONE_OFFER] = ART_ICON_ZONE_OFFER + 1,\n\t};\n\tit->icon = tool_icon[i];\n#endif\n'),
    ])


def prepare_call_replay(transform):
    """Last call replay (CONFIG_CALL_REPLAY, receive only): the P25 channel receiver keeps the last call's IMBE
    frames (8 s, 4.4 KB; p25_lab.c replay_tap) and Tools > Last call plays them again on the P25 channel in use.
    The RAM comes from the RTOS heap: 86 minutes on the radio left heap_min_free at 8,144 B (R12, 2026-11-17), so
    the heap gives 4 KB back with the replay on."""
    transform('config/features.mk', [
        ('CONFIG_PLUS_UI ?= n\nFEATURES += PLUS_UI\n', 'CONFIG_PLUS_UI ?= n\nFEATURES += PLUS_UI\n'
         'CONFIG_CALL_REPLAY ?= n\nFEATURES += CALL_REPLAY\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_PLUS_UI\n', '#ifndef CONFIG_CALL_REPLAY\n#define CONFIG_CALL_REPLAY 0\n#endif\n#ifndef CONFIG_PLUS_UI\n'),
    ])
    transform('boards/dm32/freertos_board.h', [
        ('#if CONFIG_P25 && (CONFIG_P25_BENCH || CONFIG_DEV_TX)\n#define configTOTAL_HEAP_SIZE (40 * 1024 + 512)\n',
         '#if CONFIG_P25 && (CONFIG_P25_BENCH || CONFIG_DEV_TX) && CONFIG_CALL_REPLAY\n'
         '#define configTOTAL_HEAP_SIZE (36 * 1024 + 512)\t// DM32 Plus: min free measured 8,144 B at 40.5 KB\n'
         '#elif CONFIG_P25 && (CONFIG_P25_BENCH || CONFIG_DEV_TX)\n#define configTOTAL_HEAP_SIZE (40 * 1024 + 512)\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('T_P25SEARCH, T_AIRBAND, T_BATTERY,', 'T_P25SEARCH, T_AIRBAND, T_REPLAY, T_BATTERY,'),
        ('(i != T_AIRBAND || (CONFIG_PLUS_UI && radio_am_capable())) &&',
         '(i != T_AIRBAND || (CONFIG_PLUS_UI && radio_am_capable())) && (i != T_REPLAY || CONFIG_CALL_REPLAY) &&'),
        ('TRN("Air band"), TRN("Battery"),', 'TRN("Air band"), TRN("Last call"), TRN("Battery"),'),
        ('#include "radio/radio_am.h"\n', '#include "radio/radio_am.h"\n#if CONFIG_CALL_REPLAY\n#include "p25/p25_lab.h"\n#endif\n'),
        ('\tcase T_ZONE_OFFER: ui_push(SCR_OFFER, 0); break;',
         '#if CONFIG_CALL_REPLAY\n'
         '\tcase T_REPLAY: {\n'
         '\t\t// DM32 Plus: play the last P25 call again on the P25 channel in use (receive only); again: stop\n'
         '\t\tconst radio_view_t *rv = ui_view();\n'
         '\t\tif (p25_lab_replaying()) {\n\t\t\tp25_lab_replay(0);\n\t\t\tui_toast(TR("Replay stopped"));\n\t\t\tbreak;\n\t\t}\n'
         '\t\tbool p25 = !rv->vfo && codeplug_mode_p25(rv->ch.mode);\n'
         '#if CONFIG_YSF\n\t\tp25 = p25 && !codeplug_mode_ysf(rv->ch.mode);\n#endif\n'
         '\t\tif (!p25) {\n\t\t\tui_toast(TR("Last call: go to a P25 channel"));\n\t\t\tbreak;\n\t\t}\n'
         '\t\tif (p25_lab_replay_info(NULL, NULL, NULL) == 0 || !p25_lab_replay(1)) {\n'
         '\t\t\tui_toast(TR("No call recorded yet"));\n\t\t\tbreak;\n\t\t}\n'
         '\t\tui_go_home();\n\t\tui_toast(TR("Playing the last call"));\n\t\tbreak;\n\t}\n'
         '#endif\n'
         '\tcase T_ZONE_OFFER: ui_push(SCR_OFFER, 0); break;'),
    ])


def prepare_turkish(transform, install):
    """Turkish on the radio (CONFIG_LANG_TR, CONFIG_LANG_DEFAULT_TR; docs/dev/translating.md "Adding a language").
    The radio draws one byte a character (ASCII + Latin-1); the six Turkish letters outside Latin-1 take the
    ISO-8859-9 slots of six Icelandic letters no shipped language uses (Ð->Ğ, Ý->İ, Þ->Ş, ð->ğ, ý->ı, þ->ş). The
    fonts get those glyphs only in a build with CONFIG_LANG_TR (tools/turkish_ui.py), and tools/i18n.py writes the
    Turkish letters of a catalog as those bytes. The catalog is integration/radiofoundry/lang/tr.po."""
    from pathlib import Path
    from tools import turkish_ui
    for name in turkish_ui.FONTS:
        source = turkish_ui.UP / 'core/ui/fonts' / (name + '.inc')
        if not source.exists():
            continue
        pair = turkish_ui.font_transform(name, source.read_text(encoding='utf-8'))
        if pair is not None:
            transform('core/ui/fonts/%s.inc' % name, [pair])
    latin5 = ', '.join('"\\u%04x": 0x%02X' % (ord(ch), b) for ch, b in turkish_ui.TURKISH_BYTES.items())
    transform('tools/i18n.py', [
        ('TEXT_TO_BYTE = {t: b for b, t in MACROS.values()}\n',
         'TEXT_TO_BYTE = {t: b for b, t in MACROS.values()}\n'
         '# DM32 Plus: the Turkish letters outside Latin-1 in their ISO-8859-9 slots (tools/turkish_ui.py)\n'
         'TEXT_TO_BYTE.update({%s})\n' % latin5),
    ])
    transform('config/features.mk', [
        ('CONFIG_LANG_IT ?= n\n', 'CONFIG_LANG_IT ?= n\n# DM32 Plus: Turkish (core/ui/lang/tr.po, tools/turkish_ui.py)\n'
         'CONFIG_LANG_TR ?= n\n'),
        ('CONFIG_LANG_DEFAULT_IT ?= n\n', 'CONFIG_LANG_DEFAULT_IT ?= n\nCONFIG_LANG_DEFAULT_TR ?= n\n'),
        ('LANG_ES LANG_IT LANG_DEFAULT_ES LANG_DEFAULT_IT ', 'LANG_ES LANG_IT LANG_TR LANG_DEFAULT_ES LANG_DEFAULT_IT LANG_DEFAULT_TR '),
        ('$(if $(and $(call feature_on,LANG_DEFAULT_IT),$(if $(call feature_on,LANG_IT),,x)),'
         '$(error CONFIG_LANG_DEFAULT_IT=y needs CONFIG_LANG_IT=y))\n',
         '$(if $(and $(call feature_on,LANG_DEFAULT_IT),$(if $(call feature_on,LANG_IT),,x)),'
         '$(error CONFIG_LANG_DEFAULT_IT=y needs CONFIG_LANG_IT=y))\n'
         '$(if $(and $(call feature_on,LANG_DEFAULT_TR),$(if $(call feature_on,LANG_TR),,x)),'
         '$(error CONFIG_LANG_DEFAULT_TR=y needs CONFIG_LANG_TR=y))\n'),
    ])
    transform('core/config.h', [
        ('#define CONFIG_LANG_ANY\t\t\t(CONFIG_LANG_ES || CONFIG_LANG_IT)',
         '#ifndef CONFIG_LANG_TR\n#define CONFIG_LANG_TR\t\t\t0\n#endif\n'
         '#define CONFIG_LANG_ANY\t\t\t(CONFIG_LANG_ES || CONFIG_LANG_IT || CONFIG_LANG_TR)'),
        ('#ifndef CONFIG_LANG_DEFAULT_IT\n#define CONFIG_LANG_DEFAULT_IT\t\t0\n#endif\n',
         '#ifndef CONFIG_LANG_DEFAULT_IT\n#define CONFIG_LANG_DEFAULT_IT\t\t0\n#endif\n'
         '#ifndef CONFIG_LANG_DEFAULT_TR\n#define CONFIG_LANG_DEFAULT_TR\t\t0\n#endif\n'),
    ])
    transform('docs/build-config.md', [
        ('| `CONFIG_LANG_DEFAULT_ES` |',
         '| `CONFIG_LANG_TR` | n | Turkish on the radio (`core/ui/lang/tr.po`; DM32 Plus: the six letters outside '
         'Latin-1 in their ISO-8859-9 slots, tools/turkish_ui.py), as `CONFIG_LANG_ES` |\n'
         '| `CONFIG_LANG_DEFAULT_TR` | n | The radio starts in Turkish, as `CONFIG_LANG_DEFAULT_ES`. Needs `CONFIG_LANG_TR` |\n'
         '| `CONFIG_LANG_DEFAULT_ES` |'),
    ])
    if (Path(__file__).resolve().parents[1] / 'integration/radiofoundry/lang/tr.po').exists():
        install('core/ui/lang/tr.po', 'integration/radiofoundry/lang/tr.po')


def prepare_plus_ui(transform):
    """Small UI fixes from the radio test (CONFIG_PLUS_UI, 2026-11-17): the A/B key starts dual watch when it is off
    (it only said "Turn on dual watch first", though the key list says it starts it), and Tools > Air band opens the
    VFO on the air band (AM receive was implicit: an FM channel or the VFO on 108-136 MHz, with no way to find it)."""
    transform('config/features.mk', [
        ('CONFIG_P25_ANY ?= n\nFEATURES += P25_ANY\n', 'CONFIG_P25_ANY ?= n\nFEATURES += P25_ANY\n'
         'CONFIG_PLUS_UI ?= n\nFEATURES += PLUS_UI\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_P25_ANY\n', '#ifndef CONFIG_PLUS_UI\n#define CONFIG_PLUS_UI 0\n#endif\n#ifndef CONFIG_P25_ANY\n'),
    ])
    transform('core/ui/ui_dual.c', [
        ('\tif (d->on)\n\t\tui_dual_select(!d->sel);\n\telse\n'
         '\t\tui_toast(TR(TEXT_ALT_OPEN "Turn on dual watch first" TEXT_ALT_NEXT "Dual watch is off" TEXT_ALT_CLOSE));\n',
         '\tif (d->on)\n\t\tui_dual_select(!d->sel);\n\telse\n#if CONFIG_PLUS_UI\n'
         '\t\tui_dual_key();\t// DM32 Plus: off, the A/B key starts the watch (as the key list says)\n#else\n'
         '\t\tui_toast(TR(TEXT_ALT_OPEN "Turn on dual watch first" TEXT_ALT_NEXT "Dual watch is off" TEXT_ALT_CLOSE));\n'
         '#endif\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('T_GOTO, T_P25SEARCH, T_BATTERY,', 'T_GOTO, T_P25SEARCH, T_AIRBAND, T_BATTERY,'),
        ('(i != T_P25SEARCH || CONFIG_P25_SEARCH) &&',
         '(i != T_P25SEARCH || CONFIG_P25_SEARCH) && (i != T_AIRBAND || (CONFIG_PLUS_UI && radio_am_capable())) &&'),
        ('TRN("P25 search"), TRN("Battery"),', 'TRN("P25 search"), TRN("Air band"), TRN("Battery"),'),
        ('\tcase T_P25SEARCH: ui_push(SCR_P25SEARCH, 0); break;',
         '\tcase T_P25SEARCH: ui_push(SCR_P25SEARCH, 0); break;\n#if CONFIG_PLUS_UI\n'
         '\tcase T_AIRBAND: {\n'
         '\t\t// DM32 Plus: the VFO on the air band (AM receive; 108-136 MHz), back where it was last there\n'
         '\t\tstatic uint32_t air_hz = 118000000u;\n'
         '\t\tconst radio_view_t *av = ui_view();\n'
         '\t\tif (av->vfo && radio_am_band(av->ch.rx_hz))\n\t\t\tair_hz = av->ch.rx_hz;\n'
         '\t\tradio_req_vfo(true);\n\t\tradio_req_vfo_set(air_hz);\n\t\tui_go_home();\n'
         '\t\tui_toast(TR("Air band AM: digits or Up/Down"));\n\t\tbreak;\n\t}\n#endif'),
    ])


def prepare_p25_any(transform):
    """'Any NAC' and 'any talkgroup' as plain channel editor choices (CONFIG_P25_ANY). The receive side already
    takes NAC F7E as any NAC and talkgroup 0 as any group; the editor now shows them as "Any", the context key (#) sets
    them while editing (on the TX NAC row it chooses "Same as RX"), and talkgroup Any makes the channel RX only (a
    channel that hears every group has none to transmit with), with a toast saying so. RX NAC Any keeps TX as Classic
    r13 has it: an inherited TX NAC is then $293 (codeplug_p25_tx_nac), an explicit one its own value."""
    transform('config/features.mk', [
        ('CONFIG_P25_SEARCH ?= n\nFEATURES += P25_SEARCH\n', 'CONFIG_P25_SEARCH ?= n\nFEATURES += P25_SEARCH\n'
         'CONFIG_P25_ANY ?= n\nFEATURES += P25_ANY\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_P25_SEARCH\n', '#ifndef CONFIG_P25_ANY\n#define CONFIG_P25_ANY 0\n#endif\n#ifndef CONFIG_P25_SEARCH\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('\tcase CE_P25_TG: sb_u32(&b, ch->p25_tg, 1); break;',
         '\tcase CE_P25_TG:\n#if CONFIG_P25_ANY\n\t\tif (ch->p25_tg == 0) { sb_str(&b, TR("Any")); break; }\n#endif\n'
         '\t\tsb_u32(&b, ch->p25_tg, 1); break;'),
        ("\t\tsb_chr(&b, '$');\n\t\tfor (unsigned digit = 0; digit < 3; digit++) {",
         "#if CONFIG_P25_ANY\n\t\tif (r == CE_P25_NAC && value == 0xF7Eu && cursor == 3) { sb_str(&b, TR(\"Any (F7E)\")); break; }\n#endif\n"
         "\t\tsb_chr(&b, '$');\n\t\tfor (unsigned digit = 0; digit < 3; digit++) {"),
        ('\tcase CE_P25_TG: if (ced_len) { c->p25_tg = (uint16_t)ced_val; c->contact=c->listen_list=0; } break;',
         '\tcase CE_P25_TG:\n\t\tif (ced_len) { c->p25_tg = (uint16_t)ced_val; c->contact=c->listen_list=0; }\n#if CONFIG_P25_ANY\n'
         '\t\tif (c->p25_tg == 0 && !codeplug_mode_ysf(c->mode) && !c->rx_only) {\n\t\t\tc->rx_only = true;\n'
         '\t\t\tui_toast(TR("Any talkgroup: RX only"));\n\t\t}\n#endif\n\t\tbreak;'),
        ('    if ((ced_edit == CE_P25_NAC || ced_edit == CE_P25_TX_NAC) && dir && ev->gesture != GESTURE_UP && '
         'ev->action != ACT_ENC_STEP) {',
         '#if CONFIG_P25_ANY\n'
         '    if ((ced_edit == CE_P25_NAC || ced_edit == CE_P25_TX_NAC || ced_edit == CE_P25_TG) &&\n'
         '        ev->action == ACT_CONTEXT && ev->gesture == GESTURE_PRESS) {\n'
         '        /* The context key: "any" (RX NAC F7E, talkgroup 0); on the TX NAC "Same as RX" */\n'
         '        if (ced_edit == CE_P25_NAC) { ced_val = 0xF7E; ced_len = 0; }\n'
         '        else if (ced_edit == CE_P25_TX_NAC) ced_len = 3;\n'
         '        else { ced_val = 0; ced_len = 1; ced_digits[0] = \'0\'; ced_digits[1] = 0; }\n'
         '        ui_dirty(); return true;\n    }\n#endif\n'
         '    if ((ced_edit == CE_P25_NAC || ced_edit == CE_P25_TX_NAC) && dir && ev->gesture != GESTURE_UP && '
         'ev->action != ACT_ENC_STEP) {'),
    ])


def prepare_p25_zone_scan(transform):
    """P25 in a zone scan (CONFIG_P25_SCAN, docs/plus.md "P25 in a zone scan"): the scan's stop on a P25 (or YSF)
    channel also counts the signal strength as a carrier, as a DMR stop does (upstream gotchas 17.3: this receiver's
    noise reading may not follow a 4FSK signal), two polls of it in a row hold the scan so the native receiver takes
    the channel; and a channel in its cooldown (native_lab.c: a take there heard nothing of ours) is passed over
    instead of holding the scan muted. Zone scan only: dual watch and multi-watch stop as before."""
    transform('core/radio/radio_sm.c', [
        ('#if CONFIG_P25_CHANNELS\n#include "p25/p25_lab.h"\n#endif\n',
         '#if CONFIG_P25_CHANNELS\n#include "p25/p25_lab.h"\n#endif\n'
         '#if CONFIG_P25_SCAN\n#include "native_modem/native_lab.h"\t// native_lab_scan_stop\n#endif\n'),
        ('\tsc->cfg.probe_ms = dmr ? RADIO_SCAN_DMR_PROBE_MS : tone_needed(sm) ? RADIO_SCAN_FM_PROBE_MS : 0;\n',
         '\tsc->cfg.probe_ms = dmr ? RADIO_SCAN_DMR_PROBE_MS : tone_needed(sm) ? RADIO_SCAN_FM_PROBE_MS : 0;\n'
         '#if CONFIG_P25_SCAN\n'
         '\t// DM32 Plus, P25 in a zone scan (docs/plus.md): a C4FM carrier by its strength too (the noise reading may not\n'
         '\t// follow it, gotchas 17.3); the second poll of it in a row holds the scan (the first one probes), and the\n'
         '\t// native receiver takes the channel. A channel in its cooldown is passed over\n'
         '\tif (sc->kind == SCAN_ZONE && codeplug_mode_p25(sm->view.ch.mode) && now_ms - sc->tuned_ms >= SCAN_SETTLE_MS) {\n'
         '\t\tbool strong = false;\n'
         '#if CONFIG_DMR\n'
         '\t\tstrong = st->rssi_dbm != 0 && dmr_strength_heard(sm, st->rssi_dbm);\n'
         '#endif\n'
         '\t\tif (native_lab_scan_stop(sm->view.zone_pos, (uint8_t)sc->steps, carrier || strong)) {\n'
         '\t\t\tactive = carrier = false;\n'
         '\t\t\tif (sc->phase == SCAN_PH_HOLD || sc->phase == SCAN_PH_RESUME) {\n'
         '\t\t\t\tscan_step(sm, now_ms);\n'
         '\t\t\t\treturn;\n'
         '\t\t\t}\n'
         '\t\t} else if (strong) {\n'
         '\t\t\tactive = active || sc->heard;\n'
         '\t\t\tcarrier = true;\n'
         '\t\t\tsc->cfg.probe_ms = 2 * RADIO_POLL_MS;\n'
         '\t\t}\n'
         '\t}\n'
         '#endif\n'),
    ])


def prepare_p25_search(transform, install):
    """P25 search (CONFIG_P25_SEARCH, receive only): Tools > P25 search sweeps a range with the channel receiver and
    hands each carrier to the native P25 receiver with any NAC and talkgroup (native_lab.c search_service). Like the
    spectrum sweep it owns the receiver while sweeping (muted) and any other request ends it."""
    install('core/radio/p25search.h', 'integration/radiofoundry/spectrum/p25search.h')
    install('core/radio/p25search.c', 'integration/radiofoundry/spectrum/p25search.c')
    install('core/ui/ui_p25search.c', 'integration/radiofoundry/spectrum/ui_p25search.c')
    transform('config/features.mk', [
        ('CONFIG_YSF ?= n\nFEATURES += YSF\n', 'CONFIG_YSF ?= n\nFEATURES += YSF\nCONFIG_P25_SEARCH ?= n\nFEATURES += P25_SEARCH\n'
         'ifeq ($(CONFIG_P25_SEARCH),y)\nifneq ($(CONFIG_P25_CHANNELS),y)\n$(error P25_SEARCH needs P25_CHANNELS=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_YSF\n', '#ifndef CONFIG_P25_SEARCH\n#define CONFIG_P25_SEARCH 0\n#endif\n#ifndef CONFIG_YSF\n'),
    ])
    transform('core/radio/radio_sm.h', [
        ('(radio/cw_beacon.h)\n} radio_req_type_t;', '(radio/cw_beacon.h)\n'
         '\tRREQ_P25_SEARCH,\t// DM32 Plus: arg low Hz (0 = stop), arg2 high Hz (radio/p25search.h)\n} radio_req_type_t;'),
    ])
    transform('core/radio/radio_task.c', [
        ('#if CONFIG_CW_BEACON\n#include "radio/cw_beacon.h"\n#endif\n',
         '#if CONFIG_CW_BEACON\n#include "radio/cw_beacon.h"\n#endif\n#if CONFIG_P25_SEARCH\n#include "radio/p25search.h"\n#endif\n'),
        ('{ post_pair(RREQ_CW_BEACON, kind, interval, 0); }\n#endif\n', '{ post_pair(RREQ_CW_BEACON, kind, interval, 0); }\n#endif\n'
         '#if CONFIG_P25_SEARCH\nvoid radio_req_p25_search(uint32_t lo_hz, uint32_t hi_hz)\n'
         '{ post_pair(RREQ_P25_SEARCH, (int32_t)lo_hz, hi_hz, 0); }\n#endif\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#if CONFIG_FOX\n#include "radio/fox.h"\n#endif\n',
         '#if CONFIG_FOX\n#include "radio/fox.h"\n#endif\n#if CONFIG_P25_SEARCH\n#include "radio/p25search.h"\n#endif\n'),
        # The sweep owns the receiver like the spectrum's; while a probe runs the native receiver has it
        ('#if CONFIG_SPECTRUM\n\tif (spectrum_on()) {', '''#if CONFIG_P25_SEARCH
	if (p25s_on()) {
		static uint32_t p25s_shown_ms, p25s_epoch_seen;
		static const subtone_t p25s_no_tone = { TONE_NONE, 0 };
		if (p25s_phase() == P25S_SWEEP) {
			if (p25s_epoch() != p25s_epoch_seen) {
				// back from a probe (the channel was tuned again meanwhile): the sweep's step, settled anew
				p25s_epoch_seen = p25s_epoch();
				board.radio->rx(p25s_at_hz(), RADIO_BW_12K5, &p25s_no_tone);
				p25s_retuned(now_ms);
			} else {
				bool hit;
				board.radio->status(&st);
				uint32_t hz = p25s_sweep(st.rssi_dbm, now_ms, &hit);
				if (!p25s_on()) {
					tune(sm, now_ms);	// nobody looks at the search any more: the channel again
				} else if (hz != 0) {
					board.radio->rx(hz, RADIO_BW_12K5, &p25s_no_tone);
					p25s_epoch_seen = p25s_epoch();
				}
			}
		}
		if (now_ms - p25s_shown_ms >= 250u) {
			p25s_shown_ms = now_ms;
			changed(sm);
		}
		outputs(sm);
		return;
	}
#endif
#if CONFIG_SPECTRUM
	if (spectrum_on()) {'''),
        ('#if CONFIG_SPECTRUM\n\tif (spectrum_on()) open = false;\n#endif\n',
         '#if CONFIG_SPECTRUM\n\tif (spectrum_on()) open = false;\n#endif\n#if CONFIG_P25_SEARCH\n\tif (p25s_on()) open = false;\n#endif\n'),
        ('#if CONFIG_CW_BEACON\n\t// Anything the user does (PTT first of all) stops the beacon', '''#if CONFIG_P25_SEARCH
	// Anything the user does but the search's own request ends the search: the channel is tuned again
	if (!alert && req->type != RREQ_P25_SEARCH && req->type != RREQ_SETTINGS && req->type != RREQ_RETUNE &&
	    req->type != RREQ_DMR_FOCUS && p25s_on()) {
		p25s_stop();
		tune(sm, now_ms);
	}
#endif
#if CONFIG_CW_BEACON
	// Anything the user does (PTT first of all) stops the beacon'''),
        ('#if CONFIG_CW_BEACON\n\tcase RREQ_CW_BEACON:', '''#if CONFIG_P25_SEARCH
	case RREQ_P25_SEARCH: {
		if (req->arg == 0) {
			if (p25s_on()) {
				p25s_stop();
				tune(sm, now_ms);
			}
			break;
		}
		bool aprs_busy = false;
#if CONFIG_APRS
		aprs_busy = sm->aprs_phase != APRS_RF_OFF;
#endif
		if (busy || sm->ptt || aprs_busy)
			break;
		if (scan_on(&sm->scan))
			scan_stop(sm, false, now_ms);
		if (sm->board_mode == RADIO_MODE_DMR)
			dmr_end(sm);
		if (sm->board_mode != RADIO_MODE_FM) {
			sm->board_mode = RADIO_MODE_FM;
			board.radio->set_mode(RADIO_MODE_FM);
		}
		static const subtone_t search_no_tone = { TONE_NONE, 0 };
		board.radio->rx(p25s_start((uint32_t)req->arg, req->arg2, 12500u, now_ms), RADIO_BW_12K5, &search_no_tone);
		outputs(sm);
		changed(sm);
		break;
	}
#endif
#if CONFIG_CW_BEACON
	case RREQ_CW_BEACON:'''),
    ])
    transform('core/ui/ui_nav.h', [
        ('(CONFIG_NAV)\n\tSCR_COUNT', '(CONFIG_NAV)\n\tSCR_P25SEARCH,\t\t\t// DM32 Plus: Tools > P25 search (CONFIG_P25_SEARCH)\n\tSCR_COUNT'),
    ])
    transform('core/ui/ui_menus.c', [
        ('// ui_goto.c (DM32 Plus)', '// ui_goto.c (DM32 Plus)\nextern const ui_scr_def_t ui_p25search_screen;\t// ui_p25search.c (DM32 Plus)'),
        ('\t[SCR_GOTO] = &ui_goto_screen,\n#endif\n',
         '\t[SCR_GOTO] = &ui_goto_screen,\n#endif\n#if CONFIG_P25_SEARCH\n\t[SCR_P25SEARCH] = &ui_p25search_screen,\n#endif\n'),
        ('T_ALMANAC, T_GOTO, T_BATTERY,', 'T_ALMANAC, T_GOTO, T_P25SEARCH, T_BATTERY,'),
        ('(i != T_GOTO || CONFIG_NAV) &&', '(i != T_GOTO || CONFIG_NAV) && (i != T_P25SEARCH || CONFIG_P25_SEARCH) &&'),
        ('TRN("Go-To"), TRN("Battery"),', 'TRN("Go-To"), TRN("P25 search"), TRN("Battery"),'),
        ('\tcase T_GOTO: ui_push(SCR_GOTO, 0); break;',
         '\tcase T_GOTO: ui_push(SCR_GOTO, 0); break;\n\tcase T_P25SEARCH: ui_push(SCR_P25SEARCH, 0); break;'),
    ])


def prepare_p25_records(transform):
    """DDM32 Plus on Classic r13's channel store (RFKV mode records, third_party/rf_codeplug, tools/classic_standard.py).
    Classic r13 stores a P25 channel as a DMR base record (receive only, "safe shape") plus an RFKV tag of kind P25
    with the P25 parameter block (RX NAC, TX NAC with its "Same as RX" presence, squelch, talkgroup), and refuses every
    other mode byte: a codeplug holding one needs the explicit Start new. Plus keeps that format for its P25 channels
    (upstream's CPS and this firmware read each other's channels) and adds, on top:
    - Its own modes keep their own records (YSF 0xA6, NXDN 0xA7, dPMR 0xA8; nothing upstream knows), so the tag paths
      of codeplug.c (the DMR base record, the P25 tag and parameter block) take RADIO_MODE_P25 only, not every
      native-modem channel (codeplug_mode_p25 is true for YSF too).
    - Read compatibility (CONFIG_P25_MODE2) for the P25 records Classic r4-r7 and the r7-based Plus wrote (CH_MODE
      0xA5: NAC in CH_CC's low nibble and CH_RX_GROUP, talkgroup in the contact word, unit ID in CH_DMR_ID): read as
      P25 channels with no tag, as they read before (talkgroup filter on: Selective; TX NAC Same as RX); saving one
      writes the RFKV form. Reading writes nothing.
    - The Start new gate ignores the mode bytes this build reads (0xA5 and 2 with P25_MODE2, 0xA6, 0xA7, 0xA8)."""
    transform('core/codeplug/codeplug.h', [
        # The r7 shape of the P25 rule's opening, which the per-mode rules below anchor on
        ('static inline bool codeplug_p25_valid(const channel_t *ch)\n{\n\tuint16_t tx_nac=codeplug_p25_tx_nac(ch);\n',
         '#endif\n#if CONFIG_P25_CHANNELS\nstatic inline bool codeplug_p25_valid(const channel_t *ch)\n{\n'
         '\tuint16_t tx_nac=codeplug_p25_tx_nac(ch);\n'),
    ])
    transform('core/codeplug/codeplug.c', [
        ('\tif (r[CH_MODE] != 0 && r[CH_MODE] != 1) return false;',
         '\tif (r[CH_MODE] != 0 && r[CH_MODE] != 1 && !(CONFIG_P25_MODE2 && r[CH_MODE] == 0xA5u)) return false;'),
        ('\tout->alias_tx = (uint8_t)(((r[CH_FLAG1]',
         '#if CONFIG_P25_CHANNELS\n'
         '\tif (CONFIG_P25_MODE2 && r[CH_MODE] == 0xA5u) {\n'
         '\t\t// DDM32 Plus: a P25 record of Classic r4-r7 / the r7-based Plus, read as it read there (no tag)\n'
         '\t\tout->mode = RADIO_MODE_P25;\n'
         '\t\tout->p25_nac = (uint16_t)(((r[CH_CC] & 15u) << 8) | r[CH_RX_GROUP]);\n'
         '\t\tout->p25_tg = out->contact;\t// the contact word (a union with it there)\n'
         '\t\tout->p25_id = out->dmr_id;\t// CH_DMR_ID with LFLAG1_ID, else none\n'
         '\t\tout->p25_squelch = 1;\t\t// Selective: the talkgroup filter it had\n'
         '\t\tout->contact = out->listen_list = 0;\n'
         '\t\tout->dmo = out->roam = out->vox = false;\n'
         '\t\treturn (r[CH_CC] & 0xF0u) == 0xD0u && codeplug_p25_valid(out);\n'
         '\t}\n'
         '#endif\n'
         '\tout->alias_tx = (uint8_t)(((r[CH_FLAG1]'),
        ('    if(codeplug_mode_p25(ch->mode)) {\n        base.mode=RADIO_MODE_DMR;',
         '    if(ch->mode==RADIO_MODE_P25) {\t/* DDM32 Plus: standard P25 only (not YSF) */\n        base.mode=RADIO_MODE_DMR;'),
        ('\tr[CH_RX_GROUP] = (uint8_t)(ch->listen_list <= 0xFFu ? ch->listen_list : 0);\n',
         '\tr[CH_RX_GROUP] = (uint8_t)(ch->listen_list <= 0xFFu ? ch->listen_list : 0);\n'
         '#if CONFIG_P25_CHANNELS\n\t// DDM32 Plus native modes (YSF; standard P25 is a DMR base record here)\n'
         '\tif (codeplug_mode_p25(ch->mode)) r[CH_RX_GROUP] = (uint8_t)ch->p25_nac;\n'
         '#endif\n'),
        ('        if(codeplug_mode_p25(ch->mode)) tagged.kind=CODEPLUG_KIND_P25;',
         '        if(ch->mode==RADIO_MODE_P25) tagged.kind=CODEPLUG_KIND_P25;'),
        ('if(ok) ok=codeplug_mode_p25(ch->mode)?classic_standard_store(',
         'if(ok) ok=ch->mode==RADIO_MODE_P25?classic_standard_store('),
        ('                if(r[CH_MODE]!=0u && r[CH_MODE]!=1u) incompatible_mode_records=true;',
         '                if(r[CH_MODE]!=0u && r[CH_MODE]!=1u &&\n'
         '                   !(CONFIG_P25_MODE2 && (r[CH_MODE]==0xA5u || r[CH_MODE]==2u)) && !(CONFIG_YSF && r[CH_MODE]==0xA6u) &&\n'
         '                   !(CONFIG_NXDN && r[CH_MODE]==0xA7u) && !(CONFIG_DPMR && r[CH_MODE]==0xA8u))\n'
         '                    incompatible_mode_records=true;\t/* DDM32 Plus: its own mode bytes are not foreign */'),
    ])
    # The UI simulator (host tests): the editor's P25 ID row calls classic_standard.inc's unit ID, which the UI tests
    # don't link; Classic's own test_ui.c defines it, so here a weak one (as test_ui.c's: the channel's, else the DMR ID)
    transform('tests/host/sim/ui_sim.c', [
        ('static bool run_token(char *t, char *err, size_t len)\n{\n',
         '#if CONFIG_P25_CHANNELS\n'
         '__attribute__((weak)) uint32_t codeplug_p25_unit_id(uint32_t own) { return own ? own : codeplug_dmr_id(); }\n'
         '#endif\n'
         'static bool run_token(char *t, char *err, size_t len)\n{\n'),
    ])


def prepare_ysf(transform, install):
    """YSF receive channels (CONFIG_YSF): a channel mode beside P25 on the native modem path. codeplug_mode_p25() is
    true for both ("a native-modem channel": scan, dual watch, PTT and retune rules follow it); codeplug_mode_ysf()
    tells them apart. Record: CH_MODE 0xA6, DG-ID in CH_RX_GROUP (0 = any), always RX only (no YSF TX yet). The
    worker (p25_lab.c) feeds the air bits to src/ysf and V/D mode 2 voice to the DMR vocoder (ambeVoiceSink)."""
    install('core/ysf/ysf.h', 'src/ysf/ysf.h')
    install('core/ysf/ysf.c', 'src/ysf/ysf.c')
    install('core/ysf/ysf_vw.h', 'src/ysf/ysf_vw.h')
    install('core/ysf/ysf_vw.c', 'src/ysf/ysf_vw.c')
    transform('config/features.mk', [
        ('CONFIG_NAV ?= n\nFEATURES += NAV\n', 'CONFIG_NAV ?= n\nFEATURES += NAV\nCONFIG_P25_CALLER ?= n\nFEATURES += P25_CALLER\n'
         'CONFIG_YSF ?= n\nFEATURES += YSF\n'
         'ifeq ($(CONFIG_YSF),y)\nifneq ($(CONFIG_P25_CHANNELS)$(CONFIG_DMR),yy)\n'
         '$(error YSF needs P25_CHANNELS=y and DMR=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_NAV\n', '#ifndef CONFIG_P25_CALLER\n#define CONFIG_P25_CALLER 0\n#endif\n'
         '#ifndef CONFIG_YSF\n#define CONFIG_YSF 0\n#endif\n#ifndef CONFIG_NAV\n'),
    ])
    transform('Makefile', [
        ('APP_DIRS := $(CORE_DIRS)', 'ifeq ($(CONFIG_YSF),y)\nCORE_DIRS += core/ysf\nendif\nAPP_DIRS := $(CORE_DIRS)'),
    ])
    transform('core/codeplug/codeplug.h', [
        ('enum { RADIO_MODE_P25 = 2 };', 'enum { RADIO_MODE_P25 = 2, RADIO_MODE_YSF = 3 };'),
        ('\treturn mode == RADIO_MODE_P25;', '\treturn mode == RADIO_MODE_P25 || (CONFIG_YSF && mode == RADIO_MODE_YSF);'),
        ('#if CONFIG_P25_CHANNELS\nstatic inline bool codeplug_p25_valid(const channel_t *ch)\n{\n',
         '// DM32 Plus: a YSF channel (codeplug_mode_p25 is true for it too: a native-modem channel)\n'
         'static inline bool codeplug_mode_ysf(uint8_t mode)\n{\n#if CONFIG_P25_CHANNELS && CONFIG_YSF\n'
         '\treturn mode == RADIO_MODE_YSF;\n#else\n\t(void)mode;\n\treturn false;\n#endif\n}\n'
         '#if CONFIG_P25_CHANNELS\nstatic inline bool codeplug_p25_valid(const channel_t *ch)\n{\n'
         '\tif (codeplug_mode_ysf(ch->mode))\n\t\treturn ch->bw == RADIO_BW_12K5 && ch->rx_only && ch->p25_tg <= 127u;\n'),
    ])
    transform('core/codeplug/codeplug.c', [
        ('r[CH_MODE] == 0xA5u)) return false;', 'r[CH_MODE] == 0xA5u) && !(CONFIG_YSF && r[CH_MODE] == 0xA6u)) return false;'),
        ('\t\treturn (r[CH_CC] & 0xF0u) == 0xD0u && codeplug_p25_valid(out);\n\t}\n',
         '\t\treturn (r[CH_CC] & 0xF0u) == 0xD0u && codeplug_p25_valid(out);\n\t}\n'
         '#if CONFIG_YSF\n\tif (r[CH_MODE] == 0xA6u) {\n\t\tout->mode = RADIO_MODE_YSF;\n'
         '\t\tout->p25_nac = 0xF7Eu;\n\t\tout->p25_tg = (uint16_t)(r[CH_RX_GROUP] & 0x7Fu);\n\t\tout->p25_id = 0;\n'
         '\t\tout->dmo = out->roam = out->vox = false;\n\t\tout->rx_only = true;\n\t\treturn codeplug_p25_valid(out);\n\t}\n#endif\n'),
        ('\tr[CH_MODE] = ch->mode == RADIO_MODE_DMR ? 1 : 0;',
         '\tr[CH_MODE] = codeplug_mode_ysf(ch->mode) ? 0xA6u : ch->mode == RADIO_MODE_DMR ? 1 : 0;'),
        ('\tif (codeplug_mode_p25(ch->mode)) r[CH_RX_GROUP] = (uint8_t)ch->p25_nac;',
         '\tif (codeplug_mode_p25(ch->mode))\n\t\tr[CH_RX_GROUP] = (uint8_t)(codeplug_mode_ysf(ch->mode) ? ch->p25_tg : ch->p25_nac);'),
    ])
    ysf_label = 'codeplug_mode_ysf(%s) ? "YSF" : codeplug_mode_p25(%s) ? "P25"'
    transform('core/ui/ui_menus.c', [
        ('codeplug_mode_p25(ch->mode) ? "P25"', ysf_label % ('ch->mode', 'ch->mode'), 2),
        ('\t\t} else if (codeplug_mode_p25(c->mode)) {\n\t\t\tc->mode = RADIO_MODE_FM;',
         '\t\t} else if (CONFIG_YSF && c->mode == RADIO_MODE_P25) {\n'
         '\t\t\tc->mode = RADIO_MODE_YSF; c->p25_nac = 0xF7E; c->p25_tg = 0; c->p25_id = 0; c->rx_only = true;\n'
         '\t\t\tc->kind = CODEPLUG_KIND_NONE; c->system = 0xFF; c->dmr_id = 0; c->p25_tx_nac_override = 0;\n'
         '\t\t} else if (codeplug_mode_p25(c->mode)) {\n\t\t\tc->mode = RADIO_MODE_FM;'),
        ('\t\tif ((p25_only && !p25) ||', '\t\tif (codeplug_mode_ysf(ced_ch.mode) && (r == CE_P25_NAC || r == CE_P25_TX_NAC || r == CE_P25_ID)) continue;\n'
         '\t\tif ((p25_only && !p25) ||'),
        ('\tstr_copy(it->label, sizeof it->label, tr(labels[r]));\n',
         '\tstr_copy(it->label, sizeof it->label, tr(labels[r]));\n'
         '\tif (r == CE_P25_TG && codeplug_mode_ysf(ch->mode))\n\t\tstr_copy(it->label, sizeof it->label, "DG-ID");\n'),
    ])
    transform('core/ui/ui_screens.c', [
        ('codeplug_mode_p25(v->ch.mode) ? "P25"', ysf_label % ('v->ch.mode', 'v->ch.mode')),
        ('#include "config.h"\n', '#include "config.h"\n#if CONFIG_YSF || CONFIG_P25_CALLER\n#include "p25/p25_lab.h"\n#endif\n'),
        # Home's bottom line names who is talking on a YSF channel ("YSF TA1ABC > ALL") while the call lasts
        ('\tui_draw_bottom(s, home_hint(v, s));', '''\tconst char *bottom = home_hint(v, s);
#if CONFIG_P25_CALLER
	{
		static char p25_line[80];	// static: the UI task's stack is tight. Three alternates, NAC included
		if (codeplug_mode_p25(v->ch.mode) && !codeplug_mode_ysf(v->ch.mode) && v->state == RS_RX &&
		    p25_lab_p25_caller_line(p25_line, sizeof p25_line, false) != 0)
			bottom = p25_line;
	}
#endif
#if CONFIG_YSF
	{
		static char ysf_src[11], ysf_dst[11], ysf_line[32];	// static: the UI task's stack is tight
		p25_lab_ysf_caller(ysf_src, ysf_dst);
		if (codeplug_mode_ysf(v->ch.mode) && v->state == RS_RX && ysf_src[0] != '\\0') {
			sbuf_t yb;
			sb_init(&yb, ysf_line, sizeof ysf_line);
			sb_str(&yb, "YSF ");
			sb_str(&yb, ysf_src);
			if (ysf_dst[0] != '\\0') {
				sb_str(&yb, " > ");
				sb_str(&yb, ysf_dst);
			}
			bottom = ysf_line;
		}
	}
#endif
	ui_draw_bottom(s, bottom);'''),
    ])
    # Dual watch: a pane receiving a P25 or YSF channel names its caller as Home does, not "FM signal" (R7)
    transform('core/ui/ui_dual.c', [
        ('#include "codeplug/codeplug.h"\n',
         '#include "codeplug/codeplug.h"\n#if CONFIG_YSF || CONFIG_P25_CALLER\n#include "p25/p25_lab.h"\n#endif\n'),
        ('\t\t\tcaller_text(caller_buf, sizeof caller_buf, x->dmr, x->src, x->src_name, x->dst, x->group);\n',
         '''#if CONFIG_P25_CALLER || CONFIG_YSF
			if (!native_caller(v, i))
#endif
				caller_text(caller_buf, sizeof caller_buf, x->dmr, x->src, x->src_name, x->dst, x->group);
'''),
        ('// A call that ended on pane i goes to the top of the list\n', '''#if CONFIG_P25_CALLER || CONFIG_YSF
// DM32 Plus: pane i receives a native-modem channel (P25, YSF): its caller from the P25 worker into caller_buf
static bool native_caller(const radio_view_t *v, uint8_t i)
{
	if (!codeplug_mode_p25(v->ch.mode) || tuned_pane(v) != i)
		return false;
#if CONFIG_YSF
	if (codeplug_mode_ysf(v->ch.mode)) {
		static char src[11], dst[11];	// static: the UI task's stack is tight
		p25_lab_ysf_caller(src, dst);
		if (src[0] == '\\0') {
			str_copy(caller_buf, sizeof caller_buf, "YSF call");	// no callsign yet
			return true;
		}
		sbuf_t b;
		sb_init(&b, caller_buf, sizeof caller_buf);
		sb_str(&b, TEXT_ALT_OPEN "YSF ");
		sb_str(&b, src);
		if (dst[0] != '\\0') {
			sb_str(&b, " > ");
			sb_str(&b, dst);
		}
		sb_str(&b, TEXT_ALT_NEXT);
		sb_str(&b, src);
		sb_str(&b, TEXT_ALT_CLOSE);
		return true;
	}
#endif
#if CONFIG_P25_CALLER
	if (p25_lab_p25_caller_line(caller_buf, sizeof caller_buf, true) != 0)
		return true;
#endif
	str_copy(caller_buf, sizeof caller_buf, "P25 call");	// no link control yet
	return true;
}
#endif

// A call that ended on pane i goes to the top of the list
'''),
    ])
    transform('core/debuglink/debuglink_view.c', [
        ('codeplug_mode_p25(v->ch.mode) ? "p25"', 'codeplug_mode_ysf(v->ch.mode) ? "ysf" : codeplug_mode_p25(v->ch.mode) ? "p25"'),
        ('codeplug_mode_p25(c->mode) ? "p25"', 'codeplug_mode_ysf(c->mode) ? "ysf" : codeplug_mode_p25(c->mode) ? "p25"'),
    ])


def prepare_nav(transform, install):
    """Waypoints and Go-To (CONFIG_NAV, needs APRS: its great circle and heard stations): Tools > Go-To."""
    install('core/ui/nav.h', 'integration/radiofoundry/spectrum/nav.h')
    install('core/ui/nav.c', 'integration/radiofoundry/spectrum/nav.c')
    install('core/ui/ui_goto.c', 'integration/radiofoundry/spectrum/ui_goto.c')
    transform('config/features.mk', [
        ('CONFIG_ALMANAC ?= n\nFEATURES += ALMANAC\n', 'CONFIG_ALMANAC ?= n\nFEATURES += ALMANAC\nCONFIG_NAV ?= n\nFEATURES += NAV\n'
         'ifeq ($(CONFIG_NAV),y)\nifneq ($(CONFIG_GPS)$(CONFIG_APRS),yy)\n$(error NAV needs GPS=y and APRS=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_ALMANAC\n', '#ifndef CONFIG_NAV\n#define CONFIG_NAV 0\n#endif\n#ifndef CONFIG_ALMANAC\n'),
    ])
    transform('core/ui/ui_nav.h', [
        ('(CONFIG_ALMANAC)\n\tSCR_COUNT', '(CONFIG_ALMANAC)\n\tSCR_GOTO,\t\t\t// DM32 Plus: Tools > Go-To (CONFIG_NAV)\n\tSCR_COUNT'),
    ])
    transform('core/ui/ui_menus.c', [
        ('// ui_almanac.c (DM32 Plus)', '// ui_almanac.c (DM32 Plus)\nextern const ui_scr_def_t ui_goto_screen;\t// ui_goto.c (DM32 Plus)'),
        ('\t[SCR_ALMANAC] = &ui_almanac_screen,\n#endif\n',
         '\t[SCR_ALMANAC] = &ui_almanac_screen,\n#endif\n#if CONFIG_NAV\n\t[SCR_GOTO] = &ui_goto_screen,\n#endif\n'),
        ('T_LONE, T_ALMANAC, T_BATTERY,', 'T_LONE, T_ALMANAC, T_GOTO, T_BATTERY,'),
        ('(i != T_ALMANAC || CONFIG_ALMANAC) &&', '(i != T_ALMANAC || CONFIG_ALMANAC) && (i != T_GOTO || CONFIG_NAV) &&'),
        ('TRN("Sun & Moon"), TRN("Battery"),', 'TRN("Sun & Moon"), TRN("Go-To"), TRN("Battery"),'),
        ('\tcase T_ALMANAC: ui_push(SCR_ALMANAC, 0); break;',
         '\tcase T_ALMANAC: ui_push(SCR_ALMANAC, 0); break;\n\tcase T_GOTO: ui_push(SCR_GOTO, 0); break;'),
    ])


def prepare_almanac(transform, install):
    """Sun & Moon (CONFIG_ALMANAC, needs GPS and APRS: the GPS fix comes through APRS): the RMC date is carried to the
    UI's position (ui_position_t.date_dmy) and Tools > Sun & Moon computes the almanac (almanac.h)."""
    install('core/ui/almanac.h', 'integration/radiofoundry/spectrum/almanac.h')
    install('core/ui/almanac.c', 'integration/radiofoundry/spectrum/almanac.c')
    install('core/ui/ui_almanac.c', 'integration/radiofoundry/spectrum/ui_almanac.c')
    transform('config/features.mk', [
        ('CONFIG_LONE_WORKER ?= n\nFEATURES += LONE_WORKER\n', 'CONFIG_LONE_WORKER ?= n\nFEATURES += LONE_WORKER\n'
         'CONFIG_ALMANAC ?= n\nFEATURES += ALMANAC\nifeq ($(CONFIG_ALMANAC),y)\nifneq ($(CONFIG_GPS)$(CONFIG_APRS),yy)\n'
         '$(error ALMANAC needs GPS=y and APRS=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_LONE_WORKER\n', '#ifndef CONFIG_ALMANAC\n#define CONFIG_ALMANAC 0\n#endif\n#ifndef CONFIG_LONE_WORKER\n'),
    ])
    transform('core/aprs/aprs.h', [
        ('\tuint32_t utc_hms;\t// hhmmss of the last fix, 0xFFFFFFFF\n',
         '\tuint32_t utc_hms;\t// hhmmss of the last fix, 0xFFFFFFFF\n\tuint32_t date;\t\t// ddmmyy of the last RMC, 0 unknown (DM32 Plus)\n'),
    ])
    transform('core/aprs/aprs.c', [
        ('\t\tout->utc_hms = a.gps.utc_hms;\n', '\t\tout->utc_hms = a.gps.utc_hms;\n\t\tout->date = a.gps.date;\n'),
    ])
    transform('core/ui/ui_svc.h', [
        ('\tuint32_t utc_hms;\t// hhmmss, 0xFFFFFFFF unknown\n',
         '\tuint32_t utc_hms;\t// hhmmss, 0xFFFFFFFF unknown\n\tuint32_t date_dmy;\t// ddmmyy (UTC), 0 unknown (DM32 Plus)\n'),
    ])
    transform('core/ui/ui_svc_aprs.c', [
        ('\tout->utc_hms = g.utc_hms;\n', '\tout->utc_hms = g.utc_hms;\n\tout->date_dmy = g.date;\n'),
    ])
    transform('core/ui/ui_nav.h', [
        ('(CONFIG_LONE_WORKER)\n\tSCR_COUNT', '(CONFIG_LONE_WORKER)\n\tSCR_ALMANAC,\t\t\t// DM32 Plus: Tools > Sun & Moon (CONFIG_ALMANAC)\n\tSCR_COUNT'),
    ])
    transform('core/ui/ui_menus.c', [
        ('// ui_lone.c (DM32 Plus)', '// ui_lone.c (DM32 Plus)\nextern const ui_scr_def_t ui_almanac_screen;\t// ui_almanac.c (DM32 Plus)'),
        ('\t[SCR_LONE] = &ui_lone_screen,\n#endif\n',
         '\t[SCR_LONE] = &ui_lone_screen,\n#endif\n#if CONFIG_ALMANAC\n\t[SCR_ALMANAC] = &ui_almanac_screen,\n#endif\n'),
        ('T_BEACON, T_LONE, T_BATTERY,', 'T_BEACON, T_LONE, T_ALMANAC, T_BATTERY,'),
        ('(i != T_LONE || CONFIG_LONE_WORKER) &&', '(i != T_LONE || CONFIG_LONE_WORKER) && (i != T_ALMANAC || CONFIG_ALMANAC) &&'),
        ('TRN("Lone worker"), TRN("Battery"),', 'TRN("Lone worker"), TRN("Sun & Moon"), TRN("Battery"),'),
        ('\tcase T_LONE: ui_push(SCR_LONE, 0); break;',
         '\tcase T_LONE: ui_push(SCR_LONE, 0); break;\n\tcase T_ALMANAC: ui_push(SCR_ALMANAC, 0); break;'),
    ])


def prepare_lone_worker(transform, install):
    """Lone worker (CONFIG_LONE_WORKER): the UI task counts keys and PTT, warns and then alarms (lone.h)."""
    install('core/ui/lone.h', 'integration/radiofoundry/spectrum/lone.h')
    install('core/ui/lone.c', 'integration/radiofoundry/spectrum/lone.c')
    install('core/ui/ui_lone.c', 'integration/radiofoundry/spectrum/ui_lone.c')
    transform('config/features.mk', [
        ('CONFIG_FOX ?= n\nFEATURES += FOX\n', 'CONFIG_FOX ?= n\nFEATURES += FOX\nCONFIG_LONE_WORKER ?= n\nFEATURES += LONE_WORKER\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_FOX\n', '#ifndef CONFIG_LONE_WORKER\n#define CONFIG_LONE_WORKER 0\n#endif\n#ifndef CONFIG_FOX\n'),
    ])
    transform('core/ui/ui.c', [
        ('#include "config.h"\n', '#include "config.h"\n#if CONFIG_LONE_WORKER\n#include "ui/lone.h"\n#include "radio/beep.h"\n'
         '#include "ui_nav.h"\n#include "i18n.h"\n#endif\n'),
        ('\t\tif (input_next(&ev, wait)) {\n', '\t\tif (input_next(&ev, wait)) {\n#if CONFIG_LONE_WORKER\n'
         '\t\t\t// any key or PTT: the lone worker time starts again; a key ends what its alarm started (lone.h)\n'
         '\t\t\tlone_on_t lone_end = lone_activity(hal_millis(), ev.action == ACT_PTT);\n'
         '\t\t\tif (lone_end != LONE_ON_SOUND)\n'
         '\t\t\t\tui_toast(lone_end == LONE_ON_DMR ? TR("Emergency canceled") : TR("SOS off"));\n#endif\n'),
        ('\t\tif (ui_app_tick(&view, now))\n\t\t\tdirty = true;\n', '''\t\tif (ui_app_tick(&view, now))
			dirty = true;
#if CONFIG_LONE_WORKER
		lone_act_t lone = lone_tick(now);
		if (lone != LONE_ACT_NONE) {
			radio_req_beep(lone == LONE_ACT_ALARM ? BEEP_EMERGENCY : BEEP_TOT_WARN);
			const char *sent = lone == LONE_ACT_ALARM ? ui_lone_tx_text() : NULL;	// what the alarm sent, or why not
			ui_toast(sent != NULL ? sent : lone == LONE_ACT_ALARM ? TR("Lone worker: ALARM") : TR("Lone worker: press a key"));
			radio_active_ms = now;	// the backlight comes on
			dirty = true;
		}
#endif
'''),
    ])
    transform('core/ui/ui_nav.h', [
        ('(CONFIG_CW_BEACON)\n\tSCR_COUNT', '(CONFIG_CW_BEACON)\n\tSCR_LONE,\t\t\t// DM32 Plus: Tools > Lone worker (CONFIG_LONE_WORKER)\n\tSCR_COUNT'),
    ])
    transform('core/ui/ui_menus.c', [
        ('// ui_beacon.c (DM32 Plus)', '// ui_beacon.c (DM32 Plus)\nextern const ui_scr_def_t ui_lone_screen;\t// ui_lone.c (DM32 Plus)'),
        ('\t[SCR_BEACON] = &ui_beacon_screen,\n#endif\n',
         '\t[SCR_BEACON] = &ui_beacon_screen,\n#endif\n#if CONFIG_LONE_WORKER\n\t[SCR_LONE] = &ui_lone_screen,\n#endif\n'),
        ('T_FOX, T_BEACON, T_BATTERY,', 'T_FOX, T_BEACON, T_LONE, T_BATTERY,'),
        ('(i != T_BEACON || CONFIG_CW_BEACON) &&', '(i != T_BEACON || CONFIG_CW_BEACON) && (i != T_LONE || CONFIG_LONE_WORKER) &&'),
        ('TRN("CW beacon"), TRN("Battery"),', 'TRN("CW beacon"), TRN("Lone worker"), TRN("Battery"),'),
        ('\tcase T_BEACON: ui_push(SCR_BEACON, 0); break;',
         '\tcase T_BEACON: ui_push(SCR_BEACON, 0); break;\n\tcase T_LONE: ui_push(SCR_LONE, 0); break;'),
    ])


def prepare_cw_beacon(transform, install):
    """CW beacon / SOS (CONFIG_CW_BEACON): Tools > CW beacon keys a message in Morse on the channel's TX frequency
    with the radio's CW ID keyer (RADIO_SM_CW), once or periodically. It transmits, so every channel TX check applies
    and any user request (PTT first of all) stops it. Chained on the fox patches."""
    install('core/radio/cw_beacon.h', 'integration/radiofoundry/spectrum/cw_beacon.h')
    install('core/radio/cw_beacon.c', 'integration/radiofoundry/spectrum/cw_beacon.c')
    install('core/ui/ui_beacon.c', 'integration/radiofoundry/spectrum/ui_beacon.c')
    transform('config/features.mk', [
        ('CONFIG_FOX ?= n\nFEATURES += FOX\n', 'CONFIG_FOX ?= n\nFEATURES += FOX\nCONFIG_CW_BEACON ?= n\nFEATURES += CW_BEACON\n'
         'ifeq ($(CONFIG_CW_BEACON),y)\nifneq ($(CONFIG_SFR_CWID),y)\n'
         '$(error CW_BEACON needs the CW keyer: SFR_CWID=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_FOX\n', '#ifndef CONFIG_CW_BEACON\n#define CONFIG_CW_BEACON 0\n#endif\n#ifndef CONFIG_FOX\n'),
    ])
    transform('core/radio/radio_sm.h', [
        ('(radio/fox.h)\n} radio_req_type_t;', '(radio/fox.h)\n'
         '\tRREQ_CW_BEACON,\t\t// DM32 Plus: arg kind (CW_BEACON_KINDS = stop), arg2 interval code (radio/cw_beacon.h)\n'
         '} radio_req_type_t;'),
    ])
    transform('core/radio/radio_task.c', [
        ('#if CONFIG_FOX\n#include "radio/fox.h"\n#endif\n',
         '#if CONFIG_FOX\n#include "radio/fox.h"\n#endif\n#if CONFIG_CW_BEACON\n#include "radio/cw_beacon.h"\n#endif\n'),
        ('{ post_pair(RREQ_FOX, on, att_db, 0); }\n#endif\n', '{ post_pair(RREQ_FOX, on, att_db, 0); }\n#endif\n'
         '#if CONFIG_CW_BEACON\nvoid radio_req_cw_beacon(uint8_t kind, uint8_t interval)\n'
         '{ post_pair(RREQ_CW_BEACON, kind, interval, 0); }\n#endif\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#if CONFIG_FOX\n#include "radio/fox.h"\n#endif\n',
         '#if CONFIG_FOX\n#include "radio/fox.h"\n#endif\n#if CONFIG_CW_BEACON\n#include "radio/cw_beacon.h"\n'
         '#include "radio/radio_cwid.h"\n#include "radio/morse.h"\n#endif\n'),
        ('#define CW_LEAD_MS\t\t150', '''#if CONFIG_CW_BEACON
// DM32 Plus CW beacon (radio/cw_beacon.h): armed, keying, its schedule. The keyer below does the Morse
static cw_beacon_view_t beacon;
static uint32_t beacon_now_ms;	// the last tick, for the restore (cw_restore has no time)

void cw_beacon_view(cw_beacon_view_t *out)
{
	*out = beacon;
}
#endif

#define CW_LEAD_MS\t\t150'''),
        ('static void cw_restore(radio_sm_t *sm)\n{\n\tstatic const subtone_t none = { TONE_NONE, 0 };\n'
         '\tboard.radio->tx_tone(0);\n\tboard.radio->tx_off();\n\tsm->cw_phase = CW_OFF;\n\tsm->cw_key = false;\n',
         '''static void cw_restore(radio_sm_t *sm)
{
	static const subtone_t none = { TONE_NONE, 0 };
	board.radio->tx_tone(0);
	board.radio->tx_off();
	sm->cw_phase = CW_OFF;
	sm->cw_key = false;
#if CONFIG_CW_BEACON
	if (beacon.keying) {
		// The beacon's message is over (or cut): back to the channel as it was, the next one scheduled
		beacon.keying = false;
		sm->board_mode = RADIO_MODE_FM;
		tune(sm, beacon_now_ms);
		uint32_t every = cw_beacon_interval_ms(beacon.interval);
		if (beacon.on && every != 0)
			beacon.next_ms = beacon_now_ms + every;
		else
			beacon.on = false;
		outputs(sm);
		changed(sm);
		return;
	}
#endif
'''),
        ('\t} else if (sm->cw_phase == CW_TAIL && (now_ms - sm->cw_ms) >= CW_TAIL_MS) {\n'
         '\t\tuint32_t ms = now_ms - sm->cw_start_ms;\n\t\tcw_restore(sm);\n\t\tcw_ended(sm, true, ms);\n\t\tchanged(sm);\n\t}\n}\n',
         '''\t} else if (sm->cw_phase == CW_TAIL && (now_ms - sm->cw_ms) >= CW_TAIL_MS) {
		uint32_t ms = now_ms - sm->cw_start_ms;
		cw_restore(sm);
		cw_ended(sm, true, ms);
		changed(sm);
	}
}

#if CONFIG_CW_BEACON
// Key the beacon's message now, or refuse (the channel may not transmit, no callsign): refused disarms it
static void beacon_key(radio_sm_t *sm, uint32_t now_ms)
{
	char call[RADIO_CWID_CALL_MAX + 1], text[MORSE_TEXT_MAX];
	uint8_t deny = radio_sm_tx_check(sm);
	bool busy = sm->hotspot || sm->ptt || sm->view.state == RS_TX || sm->cw_phase != CW_OFF ||
		    board.radio->tx_tone == NULL || codeplug_mode_p25(sm->view.ch.mode);
#if CONFIG_SFR
	busy = busy || sm->sfr_rpt;
#endif
	uint8_t n = 0;
	if (deny == TXDENY_NONE && !busy && radio_cwid_callsign(sm->radio_name, call, sizeof call) != 0)
		n = cw_beacon_text(beacon.kind, call, text, sizeof text);
	if (deny != TXDENY_NONE || busy || n == 0) {
		beacon.on = false;
		beacon.refused = deny != TXDENY_NONE ? deny : 0xFFu;	// 0xFF: no callsign, or the radio is busy
		beep(sm, BEEP_ERROR);
		changed(sm);
		return;
	}
	if (scan_on(&sm->scan))
		scan_stop(sm, false, now_ms);
	if (sm->board_mode == RADIO_MODE_DMR)
		dmr_end(sm);
	beacon.keying = true;
	beacon.refused = 0;
	if (cw_begin(sm, sm->view.ch.tx_hz, sm->view.power, text, n, now_ms) == 0) {
		beacon.on = false;		// the board would not key (cw_restore put the channel back)
		beacon.refused = 0xFFu;
		return;
	}
	beacon.sent++;
	changed(sm);
}

// Stop: a message on air is cut at once
static void beacon_stop(radio_sm_t *sm)
{
	beacon.on = false;
	if (beacon.keying)
		cw_restore(sm);
}
#endif
'''),
        ('#if RADIO_SM_CW\n\tcw_service(sm, now_ms);\n#endif\n', '''#if RADIO_SM_CW
	cw_service(sm, now_ms);
#endif
#if CONFIG_CW_BEACON
	beacon_now_ms = now_ms;
	if (beacon.on && !beacon.keying && (int32_t)(now_ms - beacon.next_ms) >= 0)
		beacon_key(sm, now_ms);
#endif
'''),
        ('#if CONFIG_SPECTRUM\n\t// Anything the user does but the sweep\'s own requests ends the sweep first',
         '''#if CONFIG_CW_BEACON
	// Anything the user does (PTT first of all) stops the beacon, a message on air cut at once
	if (!alert && req->type != RREQ_CW_BEACON && req->type != RREQ_SETTINGS && req->type != RREQ_RETUNE &&
	    req->type != RREQ_DMR_FOCUS && (beacon.on || beacon.keying)) {
		beacon_now_ms = now_ms;
		beacon_stop(sm);
	}
#endif
#if CONFIG_SPECTRUM
	// Anything the user does but the sweep's own requests ends the sweep first'''),
        ('#if CONFIG_FOX\n\tcase RREQ_FOX:', '''#if CONFIG_CW_BEACON
	case RREQ_CW_BEACON:
		beacon_now_ms = now_ms;
		if (req->arg < 0 || req->arg >= CW_BEACON_KINDS) {
			beacon_stop(sm);
		} else if (!beacon.keying) {
			beacon.on = true;
			beacon.kind = (uint8_t)req->arg;
			beacon.interval = req->arg2 < CW_BEACON_INTERVALS ? (uint8_t)req->arg2 : 0;
			beacon.next_ms = now_ms;	// the first one at the next tick
			beacon.refused = 0;
		}
		changed(sm);
		break;
#endif
#if CONFIG_FOX
	case RREQ_FOX:'''),
    ])
    transform('core/ui/ui_nav.h', [
        ('(CONFIG_FOX)\n\tSCR_COUNT', '(CONFIG_FOX)\n\tSCR_BEACON,\t\t\t// DM32 Plus: Tools > CW beacon (CONFIG_CW_BEACON)\n\tSCR_COUNT'),
    ])
    transform('core/ui/ui_menus.c', [
        ('// ui_fox.c (DM32 Plus)', '// ui_fox.c (DM32 Plus)\nextern const ui_scr_def_t ui_beacon_screen;\t// ui_beacon.c (DM32 Plus)'),
        ('\t[SCR_FOX] = &ui_fox_screen,\n#endif\n',
         '\t[SCR_FOX] = &ui_fox_screen,\n#endif\n#if CONFIG_CW_BEACON\n\t[SCR_BEACON] = &ui_beacon_screen,\n#endif\n'),
        ('T_SPECTRUM, T_FOX, T_BATTERY,', 'T_SPECTRUM, T_FOX, T_BEACON, T_BATTERY,'),
        ('(i != T_FOX || CONFIG_FOX) &&', '(i != T_FOX || CONFIG_FOX) && (i != T_BEACON || CONFIG_CW_BEACON) &&'),
        ('TRN("Fox hunt"), TRN("Battery"),', 'TRN("Fox hunt"), TRN("CW beacon"), TRN("Battery"),'),
        ('\tcase T_FOX: ui_push(SCR_FOX, 0); break;',
         '\tcase T_FOX: ui_push(SCR_FOX, 0); break;\n\tcase T_BEACON: ui_push(SCR_BEACON, 0); break;'),
    ])


def prepare_fox(transform, install):
    """Fox hunt (CONFIG_FOX): Tools > Fox hunt, the meter screen, and a vario tone through a beep of any pitch.
    Built on the spectrum patches above (the same files, chained)."""
    install('core/radio/fox.h', 'integration/radiofoundry/spectrum/fox.h')
    install('core/radio/fox.c', 'integration/radiofoundry/spectrum/fox.c')
    install('core/ui/ui_fox.c', 'integration/radiofoundry/spectrum/ui_fox.c')
    transform('config/features.mk', [
        ('CONFIG_SPECTRUM ?= n\nFEATURES += SPECTRUM\n', 'CONFIG_SPECTRUM ?= n\nFEATURES += SPECTRUM\nCONFIG_FOX ?= n\nFEATURES += FOX\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_SPECTRUM\n', '#ifndef CONFIG_FOX\n#define CONFIG_FOX 0\n#endif\n#ifndef CONFIG_SPECTRUM\n'),
    ])
    transform('core/radio/beep.h', [
        ('\tBEEP_TX_PERMIT_SHORT,', '\tBEEP_TX_PERMIT_SHORT,\n\tBEEP_TONE,\t\t// DM32 Plus: one tone of any pitch and length (beep_set_tone)'),
        ('void beep_init(const hal_audio_t *audio);', 'void beep_init(const hal_audio_t *audio);\n'
         '// DM32 Plus: the pitch and length BEEP_TONE plays next\nvoid beep_set_tone(uint16_t hz, uint16_t ms);'),
    ])
    transform('core/radio/beep.c', [
        ('static const beep_seg_t p_message[] =', 'static beep_seg_t p_tone[] = { { 1000, 50 } };\t// DM32 Plus: beep_set_tone\n'
         'static const beep_seg_t p_message[] ='),
        ('\t[BEEP_TX_PERMIT_SHORT] = PAT(p_permit_short),', '\t[BEEP_TX_PERMIT_SHORT] = PAT(p_permit_short),\n\t[BEEP_TONE] = PAT(p_tone),'),
        ('void beep_init(const hal_audio_t *audio)\n{', 'void beep_set_tone(uint16_t hz, uint16_t ms)\n{\n\tp_tone[0].hz = hz;\n'
         '\tp_tone[0].ms = ms;\n}\n\nvoid beep_init(const hal_audio_t *audio)\n{'),
    ])
    transform('core/radio/radio_sm.h', [
        ('(radio/spectrum.h)\n} radio_req_type_t;', '(radio/spectrum.h)\n'
         '\tRREQ_FOX,\t\t// DM32 Plus: arg 1 on / 0 off, arg2 attenuator dB (radio/fox.h)\n} radio_req_type_t;'),
    ])
    transform('core/radio/radio_task.c', [
        ('#if CONFIG_SPECTRUM\n#include "radio/spectrum.h"\n#endif\n',
         '#if CONFIG_SPECTRUM\n#include "radio/spectrum.h"\n#endif\n#if CONFIG_FOX\n#include "radio/fox.h"\n#endif\n'),
        ('{ post_pair(RREQ_SPECTRUM, (int32_t)center_hz, span_hz, 0); }\n#endif\n',
         '{ post_pair(RREQ_SPECTRUM, (int32_t)center_hz, span_hz, 0); }\n#endif\n'
         '#if CONFIG_FOX\nvoid radio_req_fox(bool on, uint8_t att_db) { post_pair(RREQ_FOX, on, att_db, 0); }\n#endif\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#if CONFIG_SPECTRUM\n#include "radio/spectrum.h"\n#endif\n',
         '#if CONFIG_SPECTRUM\n#include "radio/spectrum.h"\n#endif\n#if CONFIG_FOX\n#include "radio/fox.h"\n#endif\n'),
        # The vario tone from the reading just taken; the screen is redrawn twice a second
        ('\tint drift = sm->view.rssi_dbm - sm->rssi_published;', '''#if CONFIG_FOX && CONFIG_BEEP
	if (fox_on()) {
		static uint32_t fox_shown_ms;
		uint16_t fox_hz = fox_service(sm->view.rssi_dbm, now_ms);
		if (fox_hz != 0) {
			beep_set_tone(fox_hz, FOX_TONE_MS);
			beep(sm, BEEP_TONE);
		}
		if (now_ms - fox_shown_ms >= 500u) {
			fox_shown_ms = now_ms;
			changed(sm);
		}
	}
#endif
	int drift = sm->view.rssi_dbm - sm->rssi_published;'''),
        ('#if CONFIG_SPECTRUM\n\tcase RREQ_SPECTRUM: {', '''#if CONFIG_FOX
	case RREQ_FOX:
		fox_set(req->arg != 0, (uint8_t)req->arg2, now_ms);
		changed(sm);
		break;
#endif
#if CONFIG_SPECTRUM
	case RREQ_SPECTRUM: {'''),
    ])
    transform('core/ui/ui_nav.h', [
        ('(CONFIG_SPECTRUM)\n\tSCR_COUNT', '(CONFIG_SPECTRUM)\n\tSCR_FOX,\t\t\t// DM32 Plus: Tools > Fox hunt (CONFIG_FOX)\n\tSCR_COUNT'),
    ])
    transform('core/ui/ui_menus.c', [
        ('// ui_spectrum.c (DM32 Plus)', '// ui_spectrum.c (DM32 Plus)\nextern const ui_scr_def_t ui_fox_screen;\t// ui_fox.c (DM32 Plus)'),
        ('\t[SCR_SPECTRUM] = &ui_spectrum_screen,\n#endif\n',
         '\t[SCR_SPECTRUM] = &ui_spectrum_screen,\n#endif\n#if CONFIG_FOX\n\t[SCR_FOX] = &ui_fox_screen,\n#endif\n'),
        ('T_SPECTRUM, T_BATTERY,', 'T_SPECTRUM, T_FOX, T_BATTERY,'),
        ('(i != T_SPECTRUM || CONFIG_SPECTRUM) &&', '(i != T_SPECTRUM || CONFIG_SPECTRUM) && (i != T_FOX || CONFIG_FOX) &&'),
        ('TRN("Spectrum"), TRN("Battery"),', 'TRN("Spectrum"), TRN("Fox hunt"), TRN("Battery"),'),
        ('\tcase T_SPECTRUM: ui_push(SCR_SPECTRUM, 0); break;',
         '\tcase T_SPECTRUM: ui_push(SCR_SPECTRUM, 0); break;\n\tcase T_FOX: ui_push(SCR_FOX, 0); break;'),
    ])


def prepare_aprs_msg(transform, install):
    """APRS messages and SOS (CONFIG_APRS_MSG, docs/plus.md third round): Tools > APRS gets Messages (the messages to
    us, read and replied to), New message (To and Text typed with the radio's text entry or a canned text picked; the
    engine's acks and three tries; sending / delivered / not delivered shown and toasted) and SOS (emergency): after
    a confirmation page, the Mic-E "Emergency" position (APRS101 ch. 10) at once and every 2 minutes until turned off,
    through the engine's own RF queue (CSMA, keying, the beacon gates: callsign, amateur frequency, a position). A
    Mic-E emergency heard from another station is toasted. The engine side (state that needs aprs.c's own) is
    patched into core/aprs/aprs.c; the Mic-E encoder, canned texts and the composer's check are
    integration/radiofoundry/aprsmsg/aprs_plus.c, the screens ui_aprsmsg.c. Runs after prepare_messages."""
    install('core/aprs/aprs_plus.h', 'integration/radiofoundry/aprsmsg/aprs_plus.h')
    install('core/aprs/aprs_plus.c', 'integration/radiofoundry/aprsmsg/aprs_plus.c')
    install('core/ui/ui_aprsmsg.c', 'integration/radiofoundry/aprsmsg/ui_aprsmsg.c')
    transform('config/features.mk', [
        ('CONFIG_MSG_RX ?= n\nFEATURES += MSG_RX\n',
         'CONFIG_APRS_MSG ?= n\nFEATURES += APRS_MSG\n'
         'ifeq ($(CONFIG_APRS_MSG),y)\nifneq ($(CONFIG_APRS),y)\n$(error APRS_MSG needs APRS=y)\nendif\nendif\n'
         'CONFIG_MSG_RX ?= n\nFEATURES += MSG_RX\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_MSG_RX\n', '#ifndef CONFIG_APRS_MSG\n#define CONFIG_APRS_MSG 0\n#endif\n#ifndef CONFIG_MSG_RX\n'),
    ])
    transform('core/aprs/aprs.c', [
        ('#include "aprs/aprs_dmr.h"\n', '#include "aprs/aprs_dmr.h"\n#if CONFIG_APRS_MSG\n#include "aprs/aprs_plus.h"\n#endif\n'),
        # A frame with another destination than APRS_TOCALL: the Mic-E position carries its latitude there
        ('static bool queue_frame(uint8_t kind, const char *path, const char *info, size_t n)\n{\n',
         '#if CONFIG_APRS_MSG\n'
         'static bool queue_frame_to(uint8_t kind, const char *dst, const char *path, const char *info, size_t n);\n'
         'static bool queue_frame(uint8_t kind, const char *path, const char *info, size_t n)\n{\n'
         '\treturn queue_frame_to(kind, APRS_TOCALL, path, info, n);\n}\n\n'
         '// DM32 Plus: to dst (APRS_TOCALL, or a Mic-E position\'s destination)\n'
         'static bool queue_frame_to(uint8_t kind, const char *dst, const char *path, const char *info, size_t n)\n'
         '#else\n'
         'static bool queue_frame(uint8_t kind, const char *path, const char *info, size_t n)\n'
         '#endif\n{\n'),
        ('\tax25_addr_parse(APRS_TOCALL, sizeof(APRS_TOCALL) - 1u, &f.dst);\n',
         '#if CONFIG_APRS_MSG\n\tax25_addr_parse(dst, strlen(dst), &f.dst);\n#else\n'
         '\tax25_addr_parse(APRS_TOCALL, sizeof(APRS_TOCALL) - 1u, &f.dst);\n#endif\n'),
        ('\n// --- Receive ---\n', '''
#if CONFIG_APRS_MSG
// DM32 Plus SOS (aprs_plus.h): the Mic-E "Emergency" position at once and every APRS_SOS_PERIOD_MS until turned off,
// RAM only. On RF only, gated as a beacon: callsign, an amateur frequency, AFSK, a position; while one of them is
// missing it waits (and goes once it's there). Also what the UI's toasts need: emergencies and messages heard
static struct {
	bool on;
	uint32_t next_ms;
	uint16_t sent;
	uint8_t emergencies, received;
	char heard[AX25_CALL_LEN + 4];	// the last emergency's station
} sos;

static void sos_service(const fix_t *f)
{
	if (!sos.on || f == NULL || a.block != APRS_OK || (int32_t)(a.now - sos.next_ms) < 0) {
		return;
	}
	aprs_pos_t p;
	memset(&p, 0, sizeof(p));
	p.lat = f->lat;
	p.lon = f->lon;
	p.course = f->course == 0xFFFFu ? APRS_COURSE_NONE : (f->course == 0 ? 360u : f->course);
	p.speed_kn = kmh_to_kn(f->speed_kmh);
	p.alt_m = f->alt_m;
	p.sym_table = a.cfg.sym_table;
	p.sym_code = a.cfg.sym_code;
	char dst[APRS_MICE_DST_LEN + 1];
	size_t n = aprs_mice_build(&p, APRS_MICE_EMERGENCY, APRS_SOS_COMMENT, dst, a.info, sizeof(a.info));
	if (n != 0 && queue_frame_to(K_BEACON, dst, aprs_path_text(a.cfg.path), a.info, n)) {
		note_info(n);
		sos.sent++;
		sos.next_ms = a.now + APRS_SOS_PERIOD_MS;
		changed();
	}
}
#endif

// --- Receive ---
'''),
        ('\tmemset(&a, 0, sizeof(a));\n', '\tmemset(&a, 0, sizeof(a));\n#if CONFIG_APRS_MSG\n\tmemset(&sos, 0, sizeof(sos));\n#endif\n'),
        ('\tmessage_service();\n\t// The view\n',
         '\tmessage_service();\n#if CONFIG_APRS_MSG\n\tsos_service(f);\n#endif\n\t// The view\n'),
        ('\t\tm->ms = a.now;\n', '\t\tm->ms = a.now;\n#if CONFIG_APRS_MSG\n\t\tsos.received++;\n#endif\n'),
        ('\taprs_parse(&f.dst, f.info, f.info_len, &p);\n\tstation_update(&f, &p);\n',
         '\taprs_parse(&f.dst, f.info, f.info_len, &p);\n\tstation_update(&f, &p);\n'
         '#if CONFIG_APRS_MSG\n'
         '\tif (p.type == APRS_T_MICE && p.mice_msg == APRS_MICE_EMERGENCY) {\n'
         '\t\taddr_text(&f.src, sos.heard);\t// DM32 Plus: a station\'s emergency, for the UI\'s toast\n'
         '\t\tsos.emergencies++;\n\t}\n#endif\n'),
        ('\n// --- Host link and GPS ---\n', '''
#if CONFIG_APRS_MSG
// --- DM32 Plus (aprs_plus.h) ---

void aprs_plus_state(aprs_plus_t *s)
{
	memset(s, 0, sizeof(*s));
	LOCK();
	uint8_t src;
	s->block = a.block;
	s->pos = position(&src) != NULL;
	s->out = a.out.acked ? APRS_OUT_DELIVERED : a.out.active ? APRS_OUT_SENDING :
		 a.out.tries >= APRS_MSG_TRIES ? APRS_OUT_FAILED : APRS_OUT_NONE;
	s->tries = a.out.tries;
	s->sos = sos.on;
	int32_t left = (int32_t)(sos.next_ms - a.now);
	s->sos_next_s = (uint16_t)(sos.on && left > 0 ? ((uint32_t)left + 999u) / 1000u : 0u);
	s->sos_sent = sos.sent;
	s->emergencies = sos.emergencies;
	s->received = sos.received;
#if CONFIG_APRS_RX
	s->messages = a.nmsg;
	for (uint8_t i = 0; i < a.nmsg; i++) {
		s->unread = (uint8_t)(s->unread + (a.msgs[i].read ? 0u : 1u));
	}
#endif
	UNLOCK();
}

uint8_t aprs_sos_set(bool on)
{
	LOCK();
	uint8_t why = APRS_OK;
	if (on) {
		uint8_t src;
		why = !a.me_ok ? APRS_NO_CALL : a.block != APRS_OK ? a.block :
		      position(&src) == NULL ? APRS_NO_POSITION : APRS_OK;
	}
	if (why == APRS_OK) {
		if (on && !sos.on) {
			sos.next_ms = a.now;	// the first one at the next tick
			sos.sent = 0;
		}
		sos.on = on;
		changed();
	}
	UNLOCK();
	return why;
}

void aprs_sos_heard(char *call)
{
	LOCK();
	memcpy(call, sos.heard, sizeof(sos.heard));
	UNLOCK();
}

#if CONFIG_APRS_RX
bool aprs_msg_peek(uint8_t i, aprs_msg_t *out)
{
	LOCK();
	bool ok = i < a.nmsg;
	if (ok) {
		*out = a.msgs[i];
	}
	UNLOCK();
	return ok;
}
#endif
#endif

// --- Host link and GPS ---
'''),
    ])
    transform('core/ui/ui_nav.h', [
        ('\tSCR_COUNT\n} ui_scr_t;',
         '\tSCR_APRS_INBOX, SCR_APRS_READ,\t// DM32 Plus: Tools > APRS > Messages, one of them (CONFIG_APRS_MSG)\n'
         '\tSCR_APRS_COMPOSE, SCR_APRS_CANNED, SCR_APRS_TEXT,\t// ... New message, its canned texts, its text entry\n'
         '\tSCR_APRS_SOS,\t\t\t// ... SOS (emergency): the confirmation\n'
         '\tSCR_COUNT\n} ui_scr_t;'),
        ('// Recent (ui_app.c): newest first\n',
         '#if CONFIG_APRS_MSG\n'
         '// DM32 Plus APRS messages and SOS (ui_aprsmsg.c): Tools > APRS\'s last rows (which: 0 Messages, 1 New message,\n'
         '// 2 SOS), and every UI tick its toasts (true: draw again)\n'
         'void ui_aprsmsg_item(uint8_t which, ui_item_t *it);\nvoid ui_aprsmsg_select(uint8_t which);\n'
         'bool ui_aprsmsg_tick(uint8_t scr);\n'
         '// Why APRS can\'t send (ui_aprs.c; NULL: no text for that reason)\nconst char *ui_aprs_why(uint8_t block);\n'
         '// The text entry (ui_menus.c) on target, up to max characters (67 at most); done after OK keeps it (NULL: none);\n'
         '// arg 0 titles it "To", 1 "Text"\n'
         'void ui_text_edit(char *target, uint8_t max, void (*done)(void), uint16_t arg);\n#endif\n\n'
         '// Recent (ui_app.c): newest first\n'),
    ])
    transform('core/ui/ui_app.c', [
        ('\tif (ui_st.toast != NULL && (int32_t)(now - toast_until) >= 0) {\n',
         '#if CONFIG_APRS_MSG\n'
         '\t// DM32 Plus APRS messages and SOS: their toasts, and Tools > APRS\'s screens drawn again on a change\n'
         '\tif (ui_aprsmsg_tick(stack[sp].scr))\n\t\tdirty = true;\n#endif\n'
         '\tif (ui_st.toast != NULL && (int32_t)(now - toast_until) >= 0) {\n'),
        ('\tbool typing = stack[sp].scr == SCR_TEXT;\n',
         '#if CONFIG_APRS_MSG\n\tbool typing = stack[sp].scr == SCR_TEXT || stack[sp].scr == SCR_APRS_TEXT;\n#else\n'
         '\tbool typing = stack[sp].scr == SCR_TEXT;\n#endif\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('extern const ui_scr_def_t ui_aprs_screen, ui_aprs_heard_screen, ui_aprs_pos_screen;\t// ui_aprs.c\n',
         'extern const ui_scr_def_t ui_aprs_screen, ui_aprs_heard_screen, ui_aprs_pos_screen;\t// ui_aprs.c\n'
         'extern const ui_scr_def_t ui_aprs_inbox_screen, ui_aprs_read_screen, ui_aprs_compose_screen,\n'
         '\tui_aprs_canned_screen, ui_aprs_sos_screen;\t// ui_aprsmsg.c (DM32 Plus)\n'),
        ('\t[SCR_MSG_VIEW] = &ui_msgview_screen,\n#endif\n',
         '\t[SCR_MSG_VIEW] = &ui_msgview_screen,\n#endif\n'
         '#if CONFIG_APRS_MSG\n'
         '#if CONFIG_APRS_RX\n\t[SCR_APRS_INBOX] = &ui_aprs_inbox_screen,\n\t[SCR_APRS_READ] = &ui_aprs_read_screen,\n#endif\n'
         '\t[SCR_APRS_COMPOSE] = &ui_aprs_compose_screen,\n\t[SCR_APRS_CANNED] = &ui_aprs_canned_screen,\n'
         '\t[SCR_APRS_TEXT] = &aprs_text_screen,\n\t[SCR_APRS_SOS] = &ui_aprs_sos_screen,\n#endif\n'),
        # The text entry holds an APRS message (67 characters) when this is built: 51 bytes more
        ('static char text_buf[CODEPLUG_NAME_LEN + 1];\n',
         '#if CONFIG_APRS_MSG\n#define TEXT_MAX\t67\t// DM32 Plus: an APRS message (APRS_MSG_TEXT_MAX)\n#else\n'
         '#define TEXT_MAX\tCODEPLUG_NAME_LEN\n#endif\nstatic char text_buf[TEXT_MAX + 1];\n'),
        ('\ttext_max = max < CODEPLUG_NAME_LEN ? max : CODEPLUG_NAME_LEN;\n',
         '\ttext_max = max < TEXT_MAX ? max : TEXT_MAX;\n'),
        ("\tsb_str(&b, text_buf);\n\tsb_chr(&b, '_');\t// the cursor; the mode is in the title\n",
         '#if CONFIG_APRS_MSG\n'
         '\tsb_str(&b, text_len > 24u ? text_buf + text_len - 24u : text_buf);\t// a long text: its end, where one types\n'
         '#else\n\tsb_str(&b, text_buf);\n#endif\n'
         "\tsb_chr(&b, '_');\t// the cursor; the mode is in the title\n"),
        ('\t.count = text_count, .item = text_item, .key = text_key_fn, .enter = text_enter,\n};\n',
         '''\t.count = text_count, .item = text_item, .key = text_key_fn, .enter = text_enter,
};

#if CONFIG_APRS_MSG
// DM32 Plus APRS messages (ui_aprsmsg.c): the same entry for the addressee (arg 0) and the text (arg 1), the whole text
// above the row while it is longer than a name
static const char *aprs_text_title(void)
{
	static char t[16];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	sb_str(&b, ui_frame()->arg != 0 ? TR("Text") : TR("To"));
	sb_chr(&b, ' ');
	sb_str(&b, text_title());
	return t;
}

static const char *aprs_text_body(void)
{
	return text_len > 24u ? text_buf : NULL;
}

static const ui_scr_def_t aprs_text_screen = {
	.name = "aprs-text", .flags = SF_LIST, .prefix = "APRS", .title = aprs_text_title, .body = aprs_text_body,
	.hint = TRN(TEXT_ALT_OPEN "Back delete  # ABC/123/abc  OK keep" TEXT_ALT_NEXT "Back del  # mode  OK" TEXT_ALT_CLOSE),
	.count = text_count, .item = text_item, .key = text_key_fn, .enter = text_enter,
};

void ui_text_edit(char *target, uint8_t max, void (*done)(void), uint16_t arg)
{
	text_target = target;
	text_done = done;
	text_max = max < TEXT_MAX ? max : TEXT_MAX;
	ui_push(SCR_APRS_TEXT, arg);
}
#endif
'''),
    ])
    transform('core/ui/ui_aprs.c', [
        ('AP_KISS, AP_HEARD, AP_COUNT };',
         'AP_KISS, AP_HEARD,\n#if CONFIG_APRS_MSG\n\tAP_MSGS, AP_NEW, AP_SOS,\t// DM32 Plus (ui_aprsmsg.c)\n#endif\n\tAP_COUNT };'),
        ('TRN("KISS on USB"), TRN("Stations heard") };',
         'TRN("KISS on USB"), TRN("Stations heard"),\n#if CONFIG_APRS_MSG\n'
         '\t\t\t\t\t     TRN("Messages"), TRN("New message"), TRN("SOS (emergency)"),\n#endif\n\t};'),
        ('\tcase APRS_NO_AFSK: return TR("No APRS tones on this radio; DMR positions work.");\n\tdefault: return NULL;\n\t}\n}\n',
         '\tcase APRS_NO_AFSK: return TR("No APRS tones on this radio; DMR positions work.");\n\tdefault: return NULL;\n\t}\n}\n'
         '\n#if CONFIG_APRS_MSG\nconst char *ui_aprs_why(uint8_t block) { return why_text(block); }\n#endif\n'),
        ('\t\t\tsb_u32(&b, v.stations, 1);\n\t\t}\n\t\tbreak;\n\tdefault: break;\n',
         '\t\t\tsb_u32(&b, v.stations, 1);\n\t\t}\n\t\tbreak;\n'
         '#if CONFIG_APRS_MSG\n\tcase AP_MSGS:\n\tcase AP_NEW:\n\tcase AP_SOS: ui_aprsmsg_item((uint8_t)(i - AP_MSGS), it); break;\n#endif\n'
         '\tdefault: break;\n'),
        ('\t\t\tui_push(SCR_APRS_HEARD, 0);\n\t\t}\n\t\treturn;\n\tdefault: return;\n',
         '\t\t\tui_push(SCR_APRS_HEARD, 0);\n\t\t}\n\t\treturn;\n'
         '#if CONFIG_APRS_MSG\n\tcase AP_MSGS:\n\tcase AP_NEW:\n\tcase AP_SOS: ui_aprsmsg_select((uint8_t)(i - AP_MSGS)); return;\n#endif\n'
         '\tdefault: return;\n'),
    ])


def prepare_p25_trunk(transform, install):
    """P25 Phase 1 trunk following (CONFIG_P25_TRUNK, receive only): Tools > P25 trunk takes the selected P25 channel
    as a trunked system's control channel. The native P25 receiver frames its TSDUs too (src/p25: the framer takes
    DUID 7 while asked to) and decodes the TSBKs (trunk/p25_tsbk.c, in the receive worker); the follower
    (trunk/p25trunk.c, radio task) keeps the channel plan and the system, and on a group voice grant for a followed
    group moves the receiver to the voice channel and back when the call ends (native_lab.c trunk_service). Like the
    P25 search it owns the receiver while its screen is open and any other request ends it; PTT is denied (RX only).
    Hooked last, its anchors keep their own text so other features can anchor there too."""
    install('core/p25/p25_tsbk.h', 'integration/radiofoundry/trunk/p25_tsbk.h')
    install('core/p25/p25_tsbk.c', 'integration/radiofoundry/trunk/p25_tsbk.c')
    install('core/radio/p25trunk.h', 'integration/radiofoundry/trunk/p25trunk.h')
    install('core/radio/p25trunk.c', 'integration/radiofoundry/trunk/p25trunk.c')
    install('core/ui/ui_p25trunk.c', 'integration/radiofoundry/trunk/ui_p25trunk.c')
    transform('config/features.mk', [
        ('CONFIG_P25_ANY ?= n\nFEATURES += P25_ANY\n', 'CONFIG_P25_ANY ?= n\nFEATURES += P25_ANY\n'
         'CONFIG_P25_TRUNK ?= n\nFEATURES += P25_TRUNK\n'
         'ifeq ($(CONFIG_P25_TRUNK),y)\nifneq ($(CONFIG_P25_CHANNELS)$(CONFIG_DEV_TX),yy)\n'
         '$(error P25_TRUNK needs P25_CHANNELS=y and DEV_TX=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_P25_ANY\n#define CONFIG_P25_ANY 0\n#endif\n',
         '#ifndef CONFIG_P25_ANY\n#define CONFIG_P25_ANY 0\n#endif\n#ifndef CONFIG_P25_TRUNK\n#define CONFIG_P25_TRUNK 0\n#endif\n'),
    ])
    transform('core/radio/radio_sm.h', [
        ('\tRREQ_P25_SEARCH,\t// DM32 Plus: arg low Hz',
         '\tRREQ_P25_TRUNK,\t\t// DM32 Plus: arg 1 = follow the selected P25 channel\'s system (0 = stop), arg2 1 = every '
         'group (radio/p25trunk.h)\n\tRREQ_P25_SEARCH,\t// DM32 Plus: arg low Hz'),
    ])
    transform('core/radio/radio_task.c', [
        ('#if CONFIG_DMR_SEARCH\n#include "radio/dmrsearch.h"\n#endif\n',
         '#if CONFIG_DMR_SEARCH\n#include "radio/dmrsearch.h"\n#endif\n#if CONFIG_P25_TRUNK\n#include "radio/p25trunk.h"\n#endif\n'),
        ('{ post_pair(RREQ_DMR_SEARCH, (int32_t)lo_hz, hi_hz, 0); }\n#endif\n',
         '{ post_pair(RREQ_DMR_SEARCH, (int32_t)lo_hz, hi_hz, 0); }\n#endif\n'
         '#if CONFIG_P25_TRUNK\nvoid radio_req_p25_trunk(bool on, bool all) { post_pair(RREQ_P25_TRUNK, on, all, 0); }\n#endif\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#if CONFIG_DMR_SEARCH\n#include "radio/dmrsearch.h"\n#endif\n',
         '#if CONFIG_DMR_SEARCH\n#include "radio/dmrsearch.h"\n#endif\n#if CONFIG_P25_TRUNK\n#include "radio/p25trunk.h"\n#endif\n'),
        # The native receiver has the radio while following (native_lab.c); a request of the user's ends it first
        ("#if CONFIG_P25_SEARCH\n\t// Anything the user does but the search's own request ends the search", '''#if CONFIG_P25_TRUNK
	// Anything the user does but the follower's own request ends trunk following: the channel is tuned again (PTT
	// never gets here while following: native_lab.c denies it)
	if (!alert && req->type != RREQ_P25_TRUNK && req->type != RREQ_SETTINGS && req->type != RREQ_RETUNE &&
	    req->type != RREQ_DMR_FOCUS && p25tk_on()) {
		p25tk_stop();
		tune(sm, now_ms);
	}
#endif
#if CONFIG_P25_SEARCH
	// Anything the user does but the search's own request ends the search'''),
        ('#if CONFIG_P25_SEARCH\n\tcase RREQ_P25_SEARCH: {', '''#if CONFIG_P25_TRUNK
	case RREQ_P25_TRUNK: {
		const channel_t *tch = &sm->view.ch;
		if (req->arg == 0) {
			if (p25tk_on()) {
				p25tk_stop();
				tune(sm, now_ms);
			}
			break;
		}
		if (p25tk_on()) {
			p25tk_follow(tch->p25_tg, req->arg2 != 0);	// the same system: only the groups followed change
			changed(sm);
			break;
		}
		bool aprs_busy = false;
#if CONFIG_APRS
		aprs_busy = sm->aprs_phase != APRS_RF_OFF;
#endif
		// The selected channel is the control channel: a P25 one (not YSF) this radio receives
		if (busy || sm->ptt || aprs_busy || sm->view.vfo || !codeplug_mode_p25(tch->mode) ||
		    codeplug_mode_ysf(tch->mode) || !board_in_band(tch->rx_hz, false))
			break;
		if (scan_on(&sm->scan))
			scan_stop(sm, false, now_ms);
		if (sm->board_mode == RADIO_MODE_DMR)
			dmr_end(sm);
		if (sm->board_mode != RADIO_MODE_FM) {
			sm->board_mode = RADIO_MODE_FM;
			board.radio->set_mode(RADIO_MODE_FM);
		}
		p25tk_start(tch->rx_hz, tch->p25_nac, tch->p25_tg, req->arg2 != 0, now_ms);
		outputs(sm);
		changed(sm);
		break;
	}
#endif
#if CONFIG_P25_SEARCH
	case RREQ_P25_SEARCH: {'''),
    ])
    transform('core/ui/ui_nav.h', [
        ('\tSCR_P25SEARCH,\t\t\t// DM32 Plus: Tools > P25 search (CONFIG_P25_SEARCH)\n',
         '\tSCR_P25TRUNK,\t\t\t// DM32 Plus: Tools > P25 trunk (CONFIG_P25_TRUNK)\n'
         '\tSCR_P25SEARCH,\t\t\t// DM32 Plus: Tools > P25 search (CONFIG_P25_SEARCH)\n'),
    ])
    # Tools > P25 trunk: the last tool before Battery (tool 24), so no other tool's number changes
    transform('core/ui/ui_menus.c', [
        ('extern const ui_scr_def_t ui_p25search_screen;\t// ui_p25search.c (DM32 Plus)\n',
         'extern const ui_scr_def_t ui_p25search_screen;\t// ui_p25search.c (DM32 Plus)\n'
         'extern const ui_scr_def_t ui_p25trunk_screen;\t// ui_p25trunk.c (DM32 Plus)\n'),
        ('#if CONFIG_P25_SEARCH\n\t[SCR_P25SEARCH] = &ui_p25search_screen,\n#endif\n',
         '#if CONFIG_P25_SEARCH\n\t[SCR_P25SEARCH] = &ui_p25search_screen,\n#endif\n'
         '#if CONFIG_P25_TRUNK\n\t[SCR_P25TRUNK] = &ui_p25trunk_screen,\n#endif\n'),
        ('T_BATTERY, T_ZONE_OFFER, T_COUNT };', 'T_P25TRUNK, T_BATTERY, T_ZONE_OFFER, T_COUNT };'),
        ('(i != T_SSTV || CONFIG_SSTV) &&', '(i != T_SSTV || CONFIG_SSTV) && (i != T_P25TRUNK || CONFIG_P25_TRUNK) &&'),
        ('TRN("Battery"), TRN("Zones/Chs Received") };', 'TRN("P25 trunk"), TRN("Battery"), TRN("Zones/Chs Received") };'),
        ('\t\t[T_ZONE_OFFER] = ART_ICON_ZONE_OFFER + 1,\n',
         '\t\t[T_P25TRUNK] = ART_ICON_P25SEARCH + 1,\t// the magnifier, as the P25 search\n'
         '\t\t[T_ZONE_OFFER] = ART_ICON_ZONE_OFFER + 1,\n'),
        ('\tcase T_DMRSEARCH: ui_push(SCR_DMRSEARCH, 0); break;\n',
         '\tcase T_DMRSEARCH: ui_push(SCR_DMRSEARCH, 0); break;\n\tcase T_P25TRUNK: ui_push(SCR_P25TRUNK, 0); break;\n'),
    ])


def prepare_dmr_trunk(transform, install):
    """DMR Tier III trunk following (CONFIG_DMR_TRUNK, receive only): Tools > DMR trunk takes the selected DMR channel
    as a trunked site's control channel (TSCC). The HR_C7000 decodes CSBKs and MBC blocks itself (BPTC, CRC) and the
    call stack hands them over as control bursts; while following they go to the follower (dmrtrunk/dmrtrunk.c, radio
    task) instead of the data service, so nothing is answered. The follower keeps the system (C_ALOHA / C_BCAST), the
    channel plan (MBC absolute channel parameters) and the voice grants; on a grant for a followed group it moves the
    receiver to the payload channel, its slot locked, and back when the call is over. Like the P25 trunk it owns the
    receiver while its screen is open and any other request ends it; PTT is denied (RX only). Hooked last, its anchors
    keep their own text so other features can anchor there too."""
    install('core/radio/dmr_csbk.h', 'integration/radiofoundry/dmrtrunk/dmr_csbk.h')
    install('core/radio/dmr_csbk.c', 'integration/radiofoundry/dmrtrunk/dmr_csbk.c')
    install('core/radio/dmrtrunk.h', 'integration/radiofoundry/dmrtrunk/dmrtrunk.h')
    install('core/radio/dmrtrunk.c', 'integration/radiofoundry/dmrtrunk/dmrtrunk.c')
    install('core/ui/ui_dmrtrunk.c', 'integration/radiofoundry/dmrtrunk/ui_dmrtrunk.c')
    transform('config/features.mk', [
        ('$(error P25_TRUNK needs P25_CHANNELS=y and DEV_TX=y)\nendif\nendif\n',
         '$(error P25_TRUNK needs P25_CHANNELS=y and DEV_TX=y)\nendif\nendif\n'
         'CONFIG_DMR_TRUNK ?= n\nFEATURES += DMR_TRUNK\n'
         'ifeq ($(CONFIG_DMR_TRUNK),y)\nifneq ($(CONFIG_DMR)$(CONFIG_DMR_ANY),yy)\n'
         '$(error DMR_TRUNK needs DMR=y and DMR_ANY=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_P25_TRUNK\n#define CONFIG_P25_TRUNK 0\n#endif\n',
         '#ifndef CONFIG_P25_TRUNK\n#define CONFIG_P25_TRUNK 0\n#endif\n#ifndef CONFIG_DMR_TRUNK\n#define CONFIG_DMR_TRUNK 0\n#endif\n'),
    ])
    # MBC continuation blocks carry no CRC of their own the chip could check (the last block's covers the whole MBC):
    # passed on with the chip's flag, as data blocks are, instead of dropped; the follower checks what it reads
    transform('drivers/modem/hrc7000/hrc7000_dmr.c', [
        ('\tbool header = s.kind != DMR_BURST_VOICE && s.kind != DMR_BURST_RATE_12_DATA && s.kind != DMR_BURST_RATE_34_DATA &&\n'
         '\t\t      s.kind != DMR_BURST_RATE_1_DATA;\n',
         '\tbool header = s.kind != DMR_BURST_VOICE && s.kind != DMR_BURST_RATE_12_DATA && s.kind != DMR_BURST_RATE_34_DATA &&\n'
         '\t\t      s.kind != DMR_BURST_RATE_1_DATA;\n'
         '#if CONFIG_DMR_TRUNK\n'
         '\theader = header && s.kind != DMR_BURST_MBC_CONT;\t// DM32 Plus Tier III: an MBC\'s channel parameters\n'
         '#endif\n'),
    ])
    # The call stack's control bursts (CSBK, MBC, data headers and blocks) go to the follower while it runs: never to
    # the data service (nothing answered) and not shown as data being received (a TSCC sends without a pause)
    transform('core/dmr/call/dmr_call.h', [
        ('void dmr_call_set_data_rx(dmr_data_rx_fn fn, void *ctx);\n',
         'void dmr_call_set_data_rx(dmr_data_rx_fn fn, void *ctx);\n'
         '#if CONFIG_DMR_TRUNK\n'
         '// DM32 Plus Tier III trunk following (radio/dmrtrunk.h): asked first for every such burst; true = taken (the data\n'
         '// side never sees it). NULL = none\n'
         'void dmr_call_set_trunk_rx(bool (*fn)(const dmr_burst_t *b, uint32_t now_ms));\n'
         '#endif\n'),
    ])
    transform('core/dmr/call/dmr_call.c', [
        ('static void rx_burst(const dmr_burst_t *b, uint32_t now, uint32_t slot)\n{\n',
         '#if CONFIG_DMR_TRUNK\n'
         'static bool (*trunk_rx)(const dmr_burst_t *b, uint32_t now_ms);\t// DM32 Plus Tier III following\n\n'
         'void dmr_call_set_trunk_rx(bool (*fn)(const dmr_burst_t *b, uint32_t now_ms))\n{\n\ttrunk_rx = fn;\n}\n\n'
         '#endif\n'
         'static void rx_burst(const dmr_burst_t *b, uint32_t now, uint32_t slot)\n{\n'),
        ('\t\tif (b->kind >= DMR_BURST_CSBK && b->kind <= DMR_BURST_USBD && b->kind != DMR_BURST_IDLE) {\n'
         '\t\t\ts.last_data_ms = now;',
         '\t\tif (b->kind >= DMR_BURST_CSBK && b->kind <= DMR_BURST_USBD && b->kind != DMR_BURST_IDLE) {\n'
         '#if CONFIG_DMR_TRUNK\n'
         '\t\t\tif (trunk_rx != NULL && trunk_rx(b, now))\n'
         '\t\t\t\tbreak;\t// DM32 Plus: the trunk follower\'s\n'
         '#endif\n'
         '\t\t\ts.last_data_ms = now;'),
    ])
    transform('core/radio/radio_sm.h', [
        ('every group (radio/p25trunk.h)\n',
         'every group (radio/p25trunk.h)\n'
         '\tRREQ_DMR_TRUNK,\t\t// DM32 Plus: arg 1 = follow the selected DMR channel\'s Tier III system (0 = stop), arg2 1 = '
         'every group (radio/dmrtrunk.h)\n'),
    ])
    transform('core/radio/radio_task.c', [
        ('#if CONFIG_P25_TRUNK\n#include "radio/p25trunk.h"\n#endif\n',
         '#if CONFIG_P25_TRUNK\n#include "radio/p25trunk.h"\n#endif\n#if CONFIG_DMR_TRUNK\n#include "radio/dmrtrunk.h"\n#endif\n'),
        ('void radio_req_p25_trunk(bool on, bool all) { post_pair(RREQ_P25_TRUNK, on, all, 0); }\n#endif\n',
         'void radio_req_p25_trunk(bool on, bool all) { post_pair(RREQ_P25_TRUNK, on, all, 0); }\n#endif\n'
         '#if CONFIG_DMR_TRUNK\nvoid radio_req_dmr_trunk(bool on, bool all) { post_pair(RREQ_DMR_TRUNK, on, all, 0); }\n#endif\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#if CONFIG_P25_TRUNK\n#include "radio/p25trunk.h"\n#endif\n',
         '#if CONFIG_P25_TRUNK\n#include "radio/p25trunk.h"\n#endif\n#if CONFIG_DMR_TRUNK\n#include "radio/dmrtrunk.h"\n#endif\n'),
        # Any retune of the channel (a request, a reload, the reference trim) ends following first
        ('\tif (dmrs_on())\n\t\tdmrs_stop();\t// DM32 Plus: the DMR search lets the receiver go\n#endif\n',
         '\tif (dmrs_on())\n\t\tdmrs_stop();\t// DM32 Plus: the DMR search lets the receiver go\n#endif\n'
         '#if CONFIG_DMR_TRUNK\n\tif (dmrtk_on())\n\t\tdmrtk_stop();\t// DM32 Plus: trunk following lets the receiver go\n#endif\n'),
        # Receive only: nothing transmits while following (PTT, data, emergency, a repeater role)
        ('\t\treturn TXDENY_MODE;\t// DM32 Plus: the DMR search is receive only\n#endif\n',
         '\t\treturn TXDENY_MODE;\t// DM32 Plus: the DMR search is receive only\n#endif\n'
         '#if CONFIG_DMR_TRUNK\n\tif (dmrtk_on())\n\t\treturn TXDENY_RX_ONLY;\t// DM32 Plus: trunk following is receive only\n#endif\n'),
        ('\t\treturn false;\t// no APRS job while the DMR search has the receiver\n\t}\n#endif\n',
         '\t\treturn false;\t// no APRS job while the DMR search has the receiver\n\t}\n#endif\n'
         '#if CONFIG_DMR_TRUNK\n\tif (dmrtk_on()) {\n\t\treturn false;\t// nor while trunk following has it\n\t}\n#endif\n'),
        # Every (re)configuration of the call stack while following is the follower's
        ('\t\tcfg.listen.hear_all = cfg.listen.any_cc = cfg.listen.any_ts = true;\n\t}\n#endif\n',
         '\t\tcfg.listen.hear_all = cfg.listen.any_cc = cfg.listen.any_ts = true;\n\t}\n#endif\n'
         '#if CONFIG_DMR_TRUNK\n'
         '\tif (dmrtk_on()) {\t// DM32 Plus Tier III following: the channel\'s color code, every group (the follower chose)\n'
         '\t\tuint8_t tts = dmrtk_ts();\n'
         '\t\tcfg.ts = tts != 0 ? tts : 1;\n'
         '\t\tcfg.rmo = tts != 0;\t// a payload channel: a base station\'s outbound, its slot locked as on a repeater\n'
         '\t\tcfg.sfr_user = false;\n\t\tcfg.cc2 = -1;\n'
         '\t\tcfg.listen.hear_all = true;\n'
         '\t\tcfg.listen.any_ts = tts == 0;\t// the TSCC: both slots\n'
         '\t}\n'
         '#endif\n'),
        ('// --- Scan (radio/scan.c decides; this tunes) ---\n', '''#if CONFIG_DMR_TRUNK
// DM32 Plus DMR Tier III trunk following (radio/dmrtrunk.h): the board stays in DMR, the call stack configured by
// dmr_configure for where the follower is (the TSCC: both slots; a payload channel: its slot). The control bursts the
// stack hears go to the follower instead of the data service (dmr_call_set_trunk_rx). tune() ends it.
static uint32_t dmrtk_on_hz;
static uint8_t dmrtk_on_ts;

static bool dmrtk_rx(const dmr_burst_t *b, uint32_t now_ms)
{
\tif (!dmrtk_on())
\t\treturn false;
\tif (b->len >= 12)
\t\tdmrtk_burst(b->kind, b->ts, b->payload, now_ms);
\treturn true;\t// while following every control and data burst is the follower's: nothing is answered
}

bool dmrtk_band_ok(uint32_t hz)
{
\treturn board_in_band(hz, false);
}

// The receiver to hz, the stack to the slot ts (0: both); a call heard on the last channel ends here
static void __attribute__((noinline)) dmrtk_tune(radio_sm_t *sm, uint32_t hz, uint8_t ts)
{
\tstatic const subtone_t none = { TONE_NONE, 0 };
\tboard.radio->rx(hz, RADIO_BW_12K5, &none);
\tdmrtk_on_hz = hz;
\tdmrtk_on_ts = ts;
\tsm->dmr_rx_hz = hz;
\tif (sm->dmr_running && dmr_call_view()->state != DMR_CALL_STATE_IDLE) {
\t\tdmr_call_stop();
\t\tsm->dmr_running = false;
\t}
\tdmr_configure(sm);\t// dmrtk_on: the follower's slot
\tif (sm->dmr_running)
\t\tboard.dmr->rx_restart();\t// the last channel's slot timing dropped (as dmr_begin)
\telse
\t\tsm->dmr_running = dmr_call_start();
\tsm->view.dmr.active = sm->dmr_running;
\tsm->dmr_serial = dmr_call_view()->serial - 1u;
}

// Every poll while following: what the stack hears to the follower; retune when it moves
static void __attribute__((noinline)) dmrtk_service(radio_sm_t *sm, uint32_t now_ms)
{
\tstatic uint32_t shown_ms, serial_seen;
\tconst dmr_call_view_t *cv = dmr_call_view();
\tdmrtk_rx_t rx;
\tmemset(&rx, 0, sizeof rx);
\trx.on_hz = dmrtk_on_hz;
\trx.on_ts = dmrtk_on_ts;
\tif (cv->phase == DMR_CALL_PHASE_RX_VOICE || cv->phase == DMR_CALL_PHASE_RX_BUSY) {
\t\trx.call = cv->phase == DMR_CALL_PHASE_RX_VOICE ? DMRTK_RX_PLAY : DMRTK_RX_MUTED;
\t\trx.enc = cv->caller.encrypted;
\t\trx.src = cv->caller.src;
\t\trx.dst = cv->caller.dst;
\t}
\tuint32_t hz = dmrtk_poll(&rx, now_ms);
\tif (!dmrtk_on()) {
\t\ttune(sm, now_ms);\t// nobody looks at the follower any more: the channel again
\t\treturn;
\t}
\tif (hz != dmrtk_on_hz || dmrtk_ts() != dmrtk_on_ts)
\t\tdmrtk_tune(sm, hz, dmrtk_ts());
\tif (dmrtk_serial() != serial_seen || now_ms - shown_ms >= 250u) {
\t\tserial_seen = dmrtk_serial();
\t\tshown_ms = now_ms;
\t\tchanged(sm);
\t}
\toutputs(sm);
}
#endif

// --- Scan (radio/scan.c decides; this tunes) ---
'''),
        ("#if CONFIG_P25_TRUNK\n\t// Anything the user does but the follower's own request ends trunk following", '''#if CONFIG_DMR_TRUNK
\t// Anything the user does but the follower's own request ends DMR trunk following: the channel is tuned again. PTT
\t// is not one: it is denied (radio_sm_tx_check: receive only) and following goes on
\tif (!alert && req->type != RREQ_DMR_TRUNK && req->type != RREQ_PTT && req->type != RREQ_SETTINGS &&
\t    req->type != RREQ_RETUNE && req->type != RREQ_DMR_FOCUS && dmrtk_on()) {
\t\tdmrtk_stop();
\t\ttune(sm, now_ms);
\t}
#endif
#if CONFIG_P25_TRUNK
\t// Anything the user does but the follower's own request ends trunk following'''),
        # Its request in a function of its own (noinline): radio_sm_request's frame is on the radio task's deepest path
        ('void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)\n{', '''#if CONFIG_DMR_TRUNK
// RREQ_DMR_TRUNK: start following from the selected channel (the TSCC), change the groups, or stop
static void __attribute__((noinline)) dmrtk_request(radio_sm_t *sm, const radio_req_t *req, bool busy, uint32_t now_ms)
{
\tconst channel_t *tch = &sm->view.ch;
\t// The groups: the channel's talkgroup and RX group list, or every group (RX group list Any, or asked)
\tuint32_t ttg = sm->talk_private ? 0 : sm->talk_id;
\tbool tall = req->arg2 != 0 || tch->listen_list == CODEPLUG_LIST_ANY;
\tif (req->arg == 0) {
\t\tif (dmrtk_on()) {
\t\t\tdmrtk_stop();
\t\t\ttune(sm, now_ms);
\t\t}
\t\treturn;
\t}
\tif (dmrtk_on()) {
\t\tdmrtk_follow(ttg, tall);\t// the same system: only the groups followed change
\t\tchanged(sm);
\t\treturn;
\t}
#if CONFIG_APRS
\tbusy = busy || sm->aprs_phase != APRS_RF_OFF;
#endif
\t// The selected channel is the TSCC: a DMR channel (not the VFO) this radio receives
\tif (busy || sm->ptt || sm->view.vfo || tch->mode != RADIO_MODE_DMR || board.dmr == NULL ||
\t    !board_in_band(tch->rx_hz, false))
\t\treturn;
\tif (scan_on(&sm->scan))
\t\tscan_stop(sm, false, now_ms);
\tend_reception(sm, now_ms);
\tif (sm->board_mode == RADIO_MODE_DMR)
\t\tdmr_end(sm);\t// the channel's call, a repeater role: following starts the stack anew
\tif (sm->board_mode != RADIO_MODE_DMR) {
\t\tsm->board_mode = RADIO_MODE_DMR;
\t\tboard.radio->set_mode(RADIO_MODE_DMR);
\t}
\tdmr_call_set_trunk_rx(dmrtk_rx);
\tdmrtk_start(tch->rx_hz, ttg, tall, sm->listen, sm->listen_count, now_ms);
\tdmrtk_tune(sm, tch->rx_hz, 0);
\tif (!sm->dmr_running) {
\t\ttune(sm, now_ms);\t// no DMR modem: following ends, the channel again
\t\treturn;
\t}
\toutputs(sm);
\tchanged(sm);
}
#endif

void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)
{'''),
        ('#if CONFIG_P25_TRUNK\n\tcase RREQ_P25_TRUNK: {', '#if CONFIG_DMR_TRUNK\n\tcase RREQ_DMR_TRUNK:\n\t\tdmrtk_request(sm, req, busy, now_ms);\n\t\tbreak;\n#endif\n'
         '#if CONFIG_P25_TRUNK\n\tcase RREQ_P25_TRUNK: {'),
        ('#if CONFIG_DMR_SEARCH\n\tif (dmrs_on()) {\n\t\tdmrs_service(sm, now_ms);\n\t\treturn;\n\t}\n#endif\n',
         '#if CONFIG_DMR_SEARCH\n\tif (dmrs_on()) {\n\t\tdmrs_service(sm, now_ms);\n\t\treturn;\n\t}\n#endif\n'
         '#if CONFIG_DMR_TRUNK\n\tif (dmrtk_on()) {\n\t\tdmrtk_service(sm, now_ms);\n\t\treturn;\n\t}\n#endif\n'),
    ])
    transform('core/ui/ui_nav.h', [
        ('\tSCR_P25TRUNK,\t\t\t// DM32 Plus: Tools > P25 trunk (CONFIG_P25_TRUNK)\n',
         '\tSCR_P25TRUNK,\t\t\t// DM32 Plus: Tools > P25 trunk (CONFIG_P25_TRUNK)\n'
         '\tSCR_DMRTRUNK,\t\t\t// DM32 Plus: Tools > DMR trunk (CONFIG_DMR_TRUNK)\n'),
    ])
    # Tools > DMR trunk: right after P25 trunk (tool 25; Battery and the zone offer move one on)
    transform('core/ui/ui_menus.c', [
        ('extern const ui_scr_def_t ui_p25trunk_screen;\t// ui_p25trunk.c (DM32 Plus)\n',
         'extern const ui_scr_def_t ui_p25trunk_screen;\t// ui_p25trunk.c (DM32 Plus)\n'
         'extern const ui_scr_def_t ui_dmrtrunk_screen;\t// ui_dmrtrunk.c (DM32 Plus)\n'),
        ('#if CONFIG_P25_TRUNK\n\t[SCR_P25TRUNK] = &ui_p25trunk_screen,\n#endif\n',
         '#if CONFIG_P25_TRUNK\n\t[SCR_P25TRUNK] = &ui_p25trunk_screen,\n#endif\n'
         '#if CONFIG_DMR_TRUNK\n\t[SCR_DMRTRUNK] = &ui_dmrtrunk_screen,\n#endif\n'),
        ('T_P25TRUNK, ', 'T_P25TRUNK, T_DMRTRUNK, '),
        ('(i != T_P25TRUNK || CONFIG_P25_TRUNK) &&', '(i != T_P25TRUNK || CONFIG_P25_TRUNK) && (i != T_DMRTRUNK || CONFIG_DMR_TRUNK) &&'),
        ('TRN("P25 trunk"), ', 'TRN("P25 trunk"), TRN("DMR trunk"), '),
        ('\t\t[T_P25TRUNK] = ART_ICON_P25SEARCH + 1,\t// the magnifier, as the P25 search\n',
         '\t\t[T_P25TRUNK] = ART_ICON_P25SEARCH + 1,\t// the magnifier, as the P25 search\n'
         '\t\t[T_DMRTRUNK] = ART_ICON_P25SEARCH + 1,\t// and the DMR trunk\n'),
        ('\tcase T_P25TRUNK: ui_push(SCR_P25TRUNK, 0); break;\n',
         '\tcase T_P25TRUNK: ui_push(SCR_P25TRUNK, 0); break;\n\tcase T_DMRTRUNK: ui_push(SCR_DMRTRUNK, 0); break;\n'),
    ])


def prepare_dpmr(transform, install):
    """dPMR receive channels (CONFIG_DPMR, docs/dpmr.md; ours, from ETSI TS 102 490 / TS 102 658). Like NXDN48 and
    through NXDN's own glue: the radio stays on the FM path, the NXDN 4FSK demodulator (same 2400 symbols/s, deviation,
    mapping and filter) reads the 8 kHz FM RX audio, src/dpmr frames it, voice (the 72-bit AMBE+2 air frames) goes to
    the DMR vocoder. One receiver object serves both modes (radio_nxdn.c: a union, one listens at a time). Record:
    CH_MODE 0xA8, the colour code filter in CH_RX_GROUP (0 = any, else code number + 1), always RX only.
    codeplug_mode_p25() stays false for it. Runs last in prepare(): it anchors on the NXDN, FM ID and call history
    insertions and leaves each anchor's text whole (a parallel job may anchor on the same lines)."""
    for f in ('dpmr.c', 'dpmr.h', 'dpmr_rx.c', 'dpmr_rx.h'):
        install('core/dpmr/' + f, 'src/dpmr/' + f)
    install('core/radio/radio_dpmr.h', 'integration/radiofoundry/dpmr/radio_dpmr.h')  # the code is in radio_nxdn.c
    transform('config/features.mk', [
        ('CONFIG_NXDN ?= n\nFEATURES += NXDN\n',
         'CONFIG_NXDN ?= n\nFEATURES += NXDN\nCONFIG_DPMR ?= n\nFEATURES += DPMR\n'
         'ifeq ($(CONFIG_DPMR),y)\nifneq ($(CONFIG_NXDN),y)\n$(error DPMR needs NXDN=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_NXDN\n', '#ifndef CONFIG_DPMR\n#define CONFIG_DPMR 0\n#endif\n#ifndef CONFIG_NXDN\n'),
    ])
    transform('Makefile', [
        ('ifeq ($(CONFIG_NXDN),y)\nCORE_DIRS += core/nxdn\nendif\n',
         'ifeq ($(CONFIG_NXDN),y)\nCORE_DIRS += core/nxdn\nendif\nifeq ($(CONFIG_DPMR),y)\nCORE_DIRS += core/dpmr\nendif\n'),
    ])
    valid = '#if CONFIG_P25_CHANNELS\nstatic inline bool codeplug_p25_valid(const channel_t *ch)\n{\n'
    transform('core/codeplug/codeplug.h', [
        (valid,
         '// DM32 Plus: a dPMR channel (FM path, the NXDN demodulator; not a native-modem channel). The colour code\n'
         '// filter is p25_tg: 0 any, else code number + 1 (1..64)\n'
         'enum { RADIO_MODE_DPMR = 5 };\n'
         'static inline bool codeplug_mode_dpmr(uint8_t mode)\n{\n#if CONFIG_P25_CHANNELS && CONFIG_DPMR\n'
         '\treturn mode == RADIO_MODE_DPMR;\n#else\n\t(void)mode;\n\treturn false;\n#endif\n}\n' + valid +
         '\tif (codeplug_mode_dpmr(ch->mode))\n\t\treturn ch->bw == RADIO_BW_12K5 && ch->rx_only && ch->p25_tg <= 64u;\n'),
    ])
    transform('core/codeplug/codeplug.c', [
        ('!(CONFIG_YSF && r[CH_MODE] == 0xA6u) &&\n',
         '!(CONFIG_YSF && r[CH_MODE] == 0xA6u) &&\n\t    !(CONFIG_DPMR && r[CH_MODE] == 0xA8u) &&\n'),
        ('\t\tout->bw = RADIO_BW_12K5;\n\t\treturn codeplug_p25_valid(out);\n\t}\n#endif\n',
         '\t\tout->bw = RADIO_BW_12K5;\n\t\treturn codeplug_p25_valid(out);\n\t}\n#endif\n'
         '#if CONFIG_DPMR\n\tif (r[CH_MODE] == 0xA8u) {\t// DM32 Plus dPMR: the colour code filter in the RX group byte\n'
         '\t\tout->mode = RADIO_MODE_DPMR;\n\t\tout->p25_nac = 0;\n'
         '\t\tout->p25_tg = (uint16_t)(r[CH_RX_GROUP] & 0x7Fu);\n\t\tout->p25_id = 0;\n'
         '\t\tout->dmo = out->roam = out->vox = false;\n\t\tout->rx_only = true;\n\t\tout->bw = RADIO_BW_12K5;\n'
         '\t\treturn codeplug_p25_valid(out);\n\t}\n#endif\n'),
        ('\tr[CH_POWER] = ch->power == CODEPLUG_POWER_DEFAULT',
         '\tif (codeplug_mode_dpmr(ch->mode))\n\t\tr[CH_MODE] = 0xA8u;\t// DM32 Plus dPMR\n'
         '\tr[CH_POWER] = ch->power == CODEPLUG_POWER_DEFAULT'),
        ('\tif (codeplug_mode_nxdn(ch->mode))\n\t\tr[CH_RX_GROUP] = (uint8_t)(ch->p25_tg & 0x3Fu);\n',
         '\tif (codeplug_mode_nxdn(ch->mode))\n\t\tr[CH_RX_GROUP] = (uint8_t)(ch->p25_tg & 0x3Fu);\n'
         '\tif (codeplug_mode_dpmr(ch->mode))\n\t\tr[CH_RX_GROUP] = (uint8_t)(ch->p25_tg & 0x7Fu);\n'),
        # a dPMR channel is stored only as one the load takes back (RX only, colour code 0..63 or any)
        ('\tif (codeplug_mode_p25(ch->mode) && !codeplug_p25_valid(ch)) return false;\n',
         '\tif (codeplug_mode_p25(ch->mode) && !codeplug_p25_valid(ch)) return false;\n'
         '\tif (codeplug_mode_dpmr(ch->mode) && !codeplug_p25_valid(ch)) return false;\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#include "radio/radio_nxdn.h"\n#include "hal_time.h"\n#endif\n',
         '#include "radio/radio_nxdn.h"\n#include "hal_time.h"\n#endif\n#if CONFIG_DPMR\n#include "radio/radio_dpmr.h"\n#endif\n'),
        # (inside NXDN's #if: DPMR needs NXDN)
        ('\t\tradio_nxdn_poll(nxdn, sm->view.ch.rx_hz, (uint8_t)sm->view.ch.p25_tg, now_ms);\n\t}\n',
         '\t\tradio_nxdn_poll(nxdn, sm->view.ch.rx_hz, (uint8_t)sm->view.ch.p25_tg, now_ms);\n\t}\n'
         '''#if CONFIG_DPMR
	{
		// DM32 Plus: a dPMR channel listens the same way, through NXDN's receiver (radio_nxdn.c: one at a time)
		bool dpmr = codeplug_mode_dpmr(sm->view.ch.mode) && sm->view.state != RS_TX &&
			    sm->board_mode == RADIO_MODE_FM && !sm->view.bcast;
#if CONFIG_SCAN
		dpmr = dpmr && !scan_on(&sm->scan);
#endif
#if CONFIG_SPECTRUM
		dpmr = dpmr && !spectrum_on();
#endif
#if CONFIG_P25_SEARCH
		dpmr = dpmr && !p25s_on();
#endif
		radio_dpmr_poll(dpmr, sm->view.ch.rx_hz, (uint8_t)sm->view.ch.p25_tg, now_ms);
	}
#endif
'''),
        ('\tif (codeplug_mode_nxdn(sm->view.ch.mode)) { open = false; green = radio_nxdn_heard(hal_millis()); }\n#endif\n',
         '\tif (codeplug_mode_nxdn(sm->view.ch.mode)) { open = false; green = radio_nxdn_heard(hal_millis()); }\n#endif\n'
         '#if CONFIG_DPMR\n\tif (codeplug_mode_dpmr(sm->view.ch.mode)) { open = false; green = radio_dpmr_heard(hal_millis()); }\n'
         '#endif\n'),
        ('\tif (codeplug_mode_nxdn(ch->mode)) return TXDENY_MODE;\t// DM32 Plus: NXDN is receive only\n',
         '\tif (codeplug_mode_nxdn(ch->mode)) return TXDENY_MODE;\t// DM32 Plus: NXDN is receive only\n'
         '\tif (codeplug_mode_dpmr(ch->mode)) return TXDENY_MODE;\t// DM32 Plus: dPMR is receive only\n'),
    ])
    dpmr_label = 'codeplug_mode_dpmr(%s) ? "dPMR" : codeplug_mode_nxdn(%s) ? "NXDN"'
    transform('core/ui/ui_menus.c', [
        ('#include "config.h"\n', '#include "config.h"\n#if CONFIG_DPMR\n#include "dpmr/dpmr.h"\n#endif\n'),
        ('codeplug_mode_nxdn(ch->mode) ? "NXDN"', dpmr_label % ('ch->mode', 'ch->mode'), 2),
        # Mode: ... -> NXDN -> dPMR -> FM
        ('\t\t} else if (codeplug_mode_p25(c->mode) || codeplug_mode_nxdn(c->mode)) {\n',
         '\t\t} else if (CONFIG_DPMR && c->mode == RADIO_MODE_NXDN) {\n'
         '\t\t\tc->mode = RADIO_MODE_DPMR; c->p25_nac = 0; c->p25_tg = 0; c->p25_id = 0; c->rx_only = true;\n'
         '\t\t\tc->bw = RADIO_BW_12K5;\n'
         '\t\t} else if (codeplug_mode_dpmr(c->mode)) {\n'
         '\t\t\tc->mode = RADIO_MODE_FM; c->contact = c->listen_list = c->dmr_id = 0;\n'
         '\t\t} else if (codeplug_mode_p25(c->mode) || codeplug_mode_nxdn(c->mode)) {\n'),
        ('\t\tbool p25 = codeplug_mode_p25(ced_ch.mode) || codeplug_mode_nxdn(ced_ch.mode);\t// NXDN: RAN row, no FM rows\n',
         '\t\tbool p25 = codeplug_mode_p25(ced_ch.mode) || codeplug_mode_nxdn(ced_ch.mode);\t// NXDN: RAN row, no FM rows\n'
         '\t\tp25 = p25 || codeplug_mode_dpmr(ced_ch.mode);\t// dPMR: the CC row, no FM rows\n'
         '\t\tif (codeplug_mode_dpmr(ced_ch.mode) && (r == CE_P25_NAC || r == CE_P25_TX_NAC || r == CE_P25_ID))\n\t\t\tcontinue;\n'),
        ('\tif (r == CE_P25_TG && codeplug_mode_nxdn(ch->mode))\n\t\tstr_copy(it->label, sizeof it->label, "RAN");\n',
         '\tif (r == CE_P25_TG && codeplug_mode_nxdn(ch->mode))\n\t\tstr_copy(it->label, sizeof it->label, "RAN");\n'
         '\tif (r == CE_P25_TG && codeplug_mode_dpmr(ch->mode))\n\t\tstr_copy(it->label, sizeof it->label, "CC");\n'),
        # the CC row shows the code number (stored + 1), "Any" for 0
        ('\t\tif (ch->p25_tg == 0) { sb_str(&b, TR("Any")); break; }\n#endif\n',
         '\t\tif (ch->p25_tg == 0) { sb_str(&b, TR("Any")); break; }\n#endif\n'
         '#if CONFIG_DPMR\n\t\tif (codeplug_mode_dpmr(ch->mode)) {\n'
         '\t\t\tif (ch->p25_tg == 0) sb_str(&b, TR("Any")); else sb_u32(&b, ch->p25_tg - 1u, 1u);\n\t\t\tbreak;\n\t\t}\n#endif\n'),
        ('\t\tif (ced_len) { c->p25_tg = (uint16_t)ced_val; c->contact=c->listen_list=0; }\n',
         '\t\tif (ced_len) { c->p25_tg = (uint16_t)ced_val; c->contact=c->listen_list=0; }\n'
         '#if CONFIG_DPMR\n\t\tif (ced_len && codeplug_mode_dpmr(c->mode)) c->p25_tg = (uint16_t)(ced_val + 1);\t// CC 0..63 typed\n#endif\n'),
        # the context key while typing the CC: any
        ("        else { ced_val = 0; ced_len = 1; ced_digits[0] = '0'; ced_digits[1] = 0; }\n",
         "        else { ced_val = 0; ced_len = 1; ced_digits[0] = '0'; ced_digits[1] = 0; }\n"
         '#if CONFIG_DPMR\n'
         '        if (ced_edit == CE_P25_TG && codeplug_mode_dpmr(ced_ch.mode)) { ced_ch.p25_tg = 0; ced_edit = CE_NONE; }\n'
         '#endif\n'),
        ('\tif ((codeplug_mode_p25(c->mode) || codeplug_mode_nxdn(c->mode)) && !codeplug_p25_valid(c))\n',
         '\tif (codeplug_mode_dpmr(c->mode) && !codeplug_p25_valid(c))\n\t\treturn TR("CC goes up to 63");\n'
         '\tif ((codeplug_mode_p25(c->mode) || codeplug_mode_nxdn(c->mode)) && !codeplug_p25_valid(c))\n'),
        # Recent: "dPMR 1234567"  "> 7654321 CC 16 5m"
        ('\tif (r->mode != 0) {\t// DM32 Plus: "P25 77285"',
         '''#if CONFIG_DPMR
	if (r->mode == RADIO_MODE_DPMR) {	// DM32 Plus dPMR: "dPMR 1234567"  "> 7654321 CC 16 5m"
		char id[12];
		sbuf_t lb, vb;
		sb_init(&lb, it->label, sizeof it->label);
		sb_init(&vb, it->value, sizeof it->value);
		sb_str(&lb, "dPMR ");
		dpmr_id_text(r->src, id, sizeof id);
		sb_str(&lb, id);
		sb_str(&vb, "> ");
		if (r->group) {
			sb_str(&vb, "ALL");
		} else {
			dpmr_id_text(r->dst, id, sizeof id);
			sb_str(&vb, id);
		}
		if (r->nac < DPMR_CC_COUNT) {
			sb_str(&vb, " CC ");
			sb_u32(&vb, r->nac, 1);
		}
		sb_chr(&vb, ' ');
		sb_age(&vb, ui_st.now_ms - r->ms);
		return;
	}
#endif
	if (r->mode != 0) {	// DM32 Plus: "P25 77285"'''),
    ])
    transform('core/ui/ui_screens.c', [
        ('codeplug_mode_nxdn(v->ch.mode) ? "NXDN"', dpmr_label % ('v->ch.mode', 'v->ch.mode')),
        ('#if CONFIG_NXDN\n#include "radio/radio_nxdn.h"\n#endif\n',
         '#if CONFIG_NXDN\n#include "radio/radio_nxdn.h"\n#endif\n#if CONFIG_DPMR\n#include "radio/radio_dpmr.h"\n#endif\n'),
        ('\t\t\tbottom = nxdn_line;\n\t\t}\n\t}\n#endif\n',
         '\t\t\tbottom = nxdn_line;\n\t\t}\n\t}\n#endif\n'
         '''#if CONFIG_DPMR
	{
		// DM32 Plus: who is talking on a dPMR channel ("dPMR 1234567 > 7654321 CC 16", "dPMR call CC 16")
		static char dpmr_line[80];	// static: the UI task's stack is tight. Three alternates
		if (codeplug_mode_dpmr(v->ch.mode) && radio_dpmr_caller_line(dpmr_line, sizeof dpmr_line) != 0)
			bottom = dpmr_line;
	}
#endif
'''),
    ])
    transform('core/ui/ui_app.c', [
        ('#if CONFIG_NXDN\n#include "radio/radio_nxdn.h"\n#endif\n',
         '#if CONFIG_NXDN\n#include "radio/radio_nxdn.h"\n#endif\n#if CONFIG_DPMR\n#include "radio/radio_dpmr.h"\n#endif\n'),
        ('\t\t\tcall_log_add(RADIO_MODE_NXDN, nc.source, nc.destination, nc.group, nc.ran, nc.encrypted, 0, NULL);\n'
         '\t}\n#endif\n',
         '\t\t\tcall_log_add(RADIO_MODE_NXDN, nc.source, nc.destination, nc.group, nc.ran, nc.encrypted, 0, NULL);\n'
         '\t}\n#endif\n'
         '''#if CONFIG_DPMR
	static uint32_t dpmr_seen;
	static radio_dpmr_caller_t dc;	// static: the UI task's stack is tight
	uint32_t dq = radio_dpmr_caller(&dc);
	if (dq != dpmr_seen) {
		dpmr_seen = dq;
		if (dc.active && dc.have_source && dc.have_destination)	// the CC rides in the NAC field
			call_log_add(RADIO_MODE_DPMR, dc.source, dc.destination, dc.all_call, dc.cc, false, 0, NULL);
	}
#endif
'''),
    ])
    transform('core/debuglink/debuglink_view.c', [
        ('codeplug_mode_nxdn(v->ch.mode) ? "nxdn"', 'codeplug_mode_dpmr(v->ch.mode) ? "dpmr" : codeplug_mode_nxdn(v->ch.mode) ? "nxdn"'),
        ('codeplug_mode_nxdn(c->mode) ? "nxdn"', 'codeplug_mode_dpmr(c->mode) ? "dpmr" : codeplug_mode_nxdn(c->mode) ? "nxdn"'),
    ])


def prepare_repeaters(transform, install):
    """Repeater list (CONFIG_REPEATERS, needs APRS for the position, docs/plus.md): Tools > Repeaters lists the
    built-in repeaters nearest first from the position APRS uses (or by frequency without one), with distance and
    direction; OK shows one, OK again tunes the VFO to it (FM with its tone, DMR with its colour code and slot), #
    jumps to the nearest. The table is made by tools/rpt_list.py at every prepare from research-data/rptrs.json
    (RadioID.net's repeater list, git-ignored, or $RPT_JSON; country $RPT_COUNTRY, default Turkey) and / or the
    owner's research-data/repeaters.csv ($RPT_CSV); neither: the committed fictional sample, said in the build
    output. Program flash only; a few bytes of RAM for the screen (the order is walked, not stored)."""
    import os
    from pathlib import Path
    from tools import rpt_list
    root = Path(__file__).resolve().parents[1]
    data = root / 'research-data'
    csv_path = Path(os.environ.get('RPT_CSV') or data / 'repeaters.csv')
    json_path = Path(os.environ.get('RPT_JSON') or data / 'rptrs.json')
    csv_arg = str(csv_path) if csv_path.is_file() else None
    json_arg = str(json_path) if json_path.is_file() else None
    countries = [c.strip() for c in os.environ.get('RPT_COUNTRY', 'Turkey').split(',') if c.strip()]
    gen = root / 'build' / 'rpt' / 'rpt_data.c'
    try:
        text, stats = rpt_list.generate(csv_arg, json_arg, countries)
    except (rpt_list.RptError, OSError) as e:
        raise SystemExit(f'rpt_list: {e}')
    print(rpt_list.describe(stats, csv_arg is None and json_arg is None), flush=True)
    gen.parent.mkdir(parents=True, exist_ok=True)
    if not gen.exists() or gen.read_text(encoding='ascii') != text:
        gen.write_text(text, encoding='ascii', newline='\n')
    install('core/ui/rpt.h', 'integration/radiofoundry/rpt/rpt.h')
    install('core/ui/rpt.c', 'integration/radiofoundry/rpt/rpt.c')
    install('core/ui/rpt_data.c', 'build/rpt/rpt_data.c')
    install('core/ui/ui_rpt.c', 'integration/radiofoundry/rpt/ui_rpt.c')
    transform('config/features.mk', [
        ('CONFIG_ID_DB ?= n\n',
         'CONFIG_REPEATERS ?= n\nFEATURES += REPEATERS\n'
         'ifeq ($(CONFIG_REPEATERS),y)\nifneq ($(CONFIG_APRS),y)\n$(error REPEATERS needs APRS=y)\nendif\nendif\n'
         'CONFIG_ID_DB ?= n\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_ID_DB\n', '#ifndef CONFIG_REPEATERS\n#define CONFIG_REPEATERS 0\n#endif\n#ifndef CONFIG_ID_DB\n'),
    ])
    transform('core/ui/ui_nav.h', [
        ('\tSCR_DMRTRUNK,\t\t\t// DM32 Plus: Tools > DMR trunk (CONFIG_DMR_TRUNK)\n',
         '\tSCR_DMRTRUNK,\t\t\t// DM32 Plus: Tools > DMR trunk (CONFIG_DMR_TRUNK)\n'
         '\tSCR_REPEATERS,\t\t\t// DM32 Plus: Tools > Repeaters (CONFIG_REPEATERS)\n'),
    ])
    # Tools > Repeaters: right after DMR trunk (tool 26; Battery and the zone offer move one on)
    transform('core/ui/ui_menus.c', [
        ('extern const ui_scr_def_t ui_dmrtrunk_screen;\t// ui_dmrtrunk.c (DM32 Plus)\n',
         'extern const ui_scr_def_t ui_dmrtrunk_screen;\t// ui_dmrtrunk.c (DM32 Plus)\n'
         'extern const ui_scr_def_t ui_rpt_screen;\t// ui_rpt.c (DM32 Plus)\n'),
        ('#if CONFIG_DMR_TRUNK\n\t[SCR_DMRTRUNK] = &ui_dmrtrunk_screen,\n#endif\n',
         '#if CONFIG_DMR_TRUNK\n\t[SCR_DMRTRUNK] = &ui_dmrtrunk_screen,\n#endif\n'
         '#if CONFIG_REPEATERS\n\t[SCR_REPEATERS] = &ui_rpt_screen,\n#endif\n'),
        ('T_DMRTRUNK, ', 'T_DMRTRUNK, T_REPEATERS, '),
        ('(i != T_DMRTRUNK || CONFIG_DMR_TRUNK) &&',
         '(i != T_DMRTRUNK || CONFIG_DMR_TRUNK) && (i != T_REPEATERS || CONFIG_REPEATERS) &&'),
        ('TRN("DMR trunk"), ', 'TRN("DMR trunk"), TRN("Repeaters"), '),
        ('\t\t[T_DMRTRUNK] = ART_ICON_P25SEARCH + 1,\t// and the DMR trunk\n',
         '\t\t[T_DMRTRUNK] = ART_ICON_P25SEARCH + 1,\t// and the DMR trunk\n'
         '\t\t[T_REPEATERS] = ART_ICON_GOTO + 1,\t// the way to one\n'),
        ('\tcase T_DMRTRUNK: ui_push(SCR_DMRTRUNK, 0); break;\n',
         '\tcase T_DMRTRUNK: ui_push(SCR_DMRTRUNK, 0); break;\n'
         '\tcase T_REPEATERS: ui_push(SCR_REPEATERS, 0); break;\n'),
    ])


def prepare_nac_decimal(transform):
    """NAC in decimal first (CONFIG_NAC_DEC, docs/plus.md; owner 2026-11-21, round 15 item 5): the NAC is a 12-bit value
    that factory radios and their CPS show and take in decimal (659 = $293; a factory "293" is $125). The channel
    editor's and VFO settings' NAC rows show "659 ($293)" and take the decimal value: digits build it (0-4095, a digit
    that would go past 4095 is refused with a toast), Up / Down and the knob step it by one, Back takes the last digit
    back; while it is edited "[659] ($293)". # once is Any (RX NAC, "Any 3966 ($F7E)") / Same as RX (TX NAC) as
    before; # again switches to the hex digits ("$2[9]3 (659)", the cursor and the knob as before), # there back to
    decimal. OK refuses an RX NAC of 3967 ($F7F, not a NAC: # is Any) and a TX NAC of 3966 / 3967 ($F7E / $F7F),
    keeping the editor open. P25 search and P25 trunk list the NAC both ways. Until 2026-11-21 the rows were hex first
    ("$293 = 659") with the decimal entry behind # #."""
    transform('config/features.mk', [
        ('CONFIG_ID_DB ?= n\n',
         'CONFIG_NAC_DEC ?= n\nFEATURES += NAC_DEC\n'
         'ifeq ($(CONFIG_NAC_DEC),y)\nifneq ($(CONFIG_P25_CHANNELS),y)\n$(error NAC_DEC needs P25_CHANNELS=y)\nendif\nendif\n'
         'CONFIG_ID_DB ?= n\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_ID_DB\n', '#ifndef CONFIG_NAC_DEC\n#define CONFIG_NAC_DEC 0\n#endif\n#ifndef CONFIG_ID_DB\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('static void chinfo_item(uint16_t i, ui_item_t *it)\n',
         '#if CONFIG_NAC_DEC\n'
         'static bool ced_nac_hex;\t// DM32 Plus: the NAC is being typed in hex digits (# twice); else in decimal\n'
         '\n// "$293", three hex digits\n'
         'static void sb_nac_hex(sbuf_t *b, unsigned v)\n{\n\tsb_chr(b, \'$\');\n'
         '\tfor (unsigned d = 0; d < 3; d++)\n\t\tsb_chr(b, "0123456789ABCDEF"[(v >> (4 * (2 - d))) & 15u]);\n}\n'
         '#endif\n\n'
         'static void chinfo_item(uint16_t i, ui_item_t *it)\n'),
        # RX Any: "Any 3966 ($F7E)"
        ('\t\tif (r == CE_P25_NAC && value == 0xF7Eu && cursor == 3) { sb_str(&b, TR("Any (F7E)")); break; }\n',
         '#if CONFIG_NAC_DEC\n'
         '\t\tif (r == CE_P25_NAC && value == 0xF7Eu && cursor == 3) { sb_str(&b, TR("Any")); sb_str(&b, " 3966 ($F7E)"); break; }\n'
         '#else\n'
         '\t\tif (r == CE_P25_NAC && value == 0xF7Eu && cursor == 3) { sb_str(&b, TR("Any (F7E)")); break; }\n'
         '#endif\n'),
        # The row: "659 ($293)"; typing in decimal: "[659] ($293)"; in hex digits: "$2[9]3 (659)"
        ('\t\tsb_chr(&b, \'$\');\n\t\tfor (unsigned digit = 0; digit < 3; digit++) {\n',
         '#if CONFIG_NAC_DEC\n'
         '\t\tif (!editing || !ced_nac_hex) {\n'
         '\t\t\tif (editing)\n\t\t\t\tsb_chr(&b, \'[\');\n\t\t\tsb_u32(&b, value, 1);\n'
         '\t\t\tif (editing)\n\t\t\t\tsb_chr(&b, \']\');\n'
         '\t\t\tsb_str(&b, " (");\n\t\t\tsb_nac_hex(&b, value);\n\t\t\tsb_chr(&b, \')\');\n'
         '\t\t\tbreak;\n\t\t}\n'
         '#endif\n'
         '\t\tsb_chr(&b, \'$\');\n\t\tfor (unsigned digit = 0; digit < 3; digit++) {\n'),
        ('\t\t\tif (digit == cursor) sb_chr(&b, \']\');\n\t\t}\n\t\tbreak;\n\t}\n#endif\n\tcase CE_NUMBER:',
         '\t\t\tif (digit == cursor) sb_chr(&b, \']\');\n\t\t}\n'
         '#if CONFIG_NAC_DEC\n\t\tsb_str(&b, " (");\n\t\tsb_u32(&b, value, 1);\n\t\tsb_chr(&b, \')\');\n#endif\n'
         '\t\tbreak;\n\t}\n#endif\n\tcase CE_NUMBER:'),
        # A digit in decimal builds the value (the first one starts over, Same as RX included); past 4095: refused
        ('\tcase CE_P25_TX_NAC: {\n        if (ced_len==3) ced_len=0;\n\t\tunsigned shift = 4u * (2u - ced_len);\n'
         '\t\tced_val = (ced_val & ~(15u << shift)) | ((unsigned)(d - \'0\') << shift);\n',
         '\tcase CE_P25_TX_NAC: {\n'
         '#if CONFIG_NAC_DEC\n'
         '\t\tif (!ced_nac_hex) {\n'
         '\t\t\tint32_t n = ced_typed && ced_len != 3 ? ced_val * 10 + (d - \'0\') : (d - \'0\');\n'
         '\t\t\tif (n > 4095) {\n\t\t\t\tui_toast(TR("NAC goes 0 to 4095"));\n\t\t\t\tbreak;\n\t\t\t}\n'
         '\t\t\tced_val = n;\n\t\t\tced_len = 0;\n\t\t\tced_typed = true;\n\t\t\tbreak;\n\t\t}\n'
         '#endif\n'
         '        if (ced_len==3) ced_len=0;\n\t\tunsigned shift = 4u * (2u - ced_len);\n'
         '\t\tced_val = (ced_val & ~(15u << shift)) | ((unsigned)(d - \'0\') << shift);\n'),
        # # once: Any (RX NAC F7E) / Same as RX (TX NAC); # again: the hex digits; # in hex: decimal again (* opens the
        # Quick menu on every screen, so it is not used). In decimal the arrows and the knob step the value, Back takes
        # the last digit back
        ('#if CONFIG_P25_ANY\n    if ((ced_edit == CE_P25_NAC || ced_edit == CE_P25_TX_NAC || ced_edit == CE_P25_TG) &&\n',
         '#if CONFIG_NAC_DEC\n'
         '    if ((ced_edit == CE_P25_NAC || ced_edit == CE_P25_TX_NAC) && ev->action == ACT_CONTEXT &&\n'
         '        ev->gesture == GESTURE_PRESS && (ced_nac_hex || (ced_edit == CE_P25_NAC ? ced_val == 0xF7E && ced_len == 0 :\n'
         '                                                     ced_len == 3))) {\n'
         '        ced_nac_hex = !ced_nac_hex;\n'
         '        if (ced_nac_hex && ced_edit == CE_P25_TX_NAC)\n            ced_val = codeplug_p25_tx_nac(&ced_ch);\n'
         '        ced_len = 0;\n        ced_typed = false;\n'
         '        ui_toast(ced_nac_hex ? TR("NAC: type it in hex") : TR("NAC: type it in decimal"));\n'
         '        ui_dirty();\n        return true;\n    }\n'
         '    if ((ced_edit == CE_P25_NAC || ced_edit == CE_P25_TX_NAC) && !ced_nac_hex && dir != 0 && ev->gesture != GESTURE_UP) {\n'
         '        if (ced_len == 3)\n            ced_len = 0;\t// off Same as RX: the NAC it sends\n'
         '        else\n            ced_val = (ced_val + dir + 4096) % 4096;\n'
         '        ced_typed = false;\n        ui_dirty();\n        return true;\n    }\n'
         '    if ((ced_edit == CE_P25_NAC || ced_edit == CE_P25_TX_NAC) && !ced_nac_hex && ced_typed &&\n'
         '        ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS) {\n'
         '        ced_val /= 10;\n        ced_typed = ced_val != 0;\n        ui_dirty();\n        return true;\n    }\n'
         '#endif\n'
         '#if CONFIG_P25_ANY\n    if ((ced_edit == CE_P25_NAC || ced_edit == CE_P25_TX_NAC || ced_edit == CE_P25_TG) &&\n'),
        ('\tcase CE_P25_NAC: ced_edit = r; ced_val = c->p25_nac; ced_len = 0; break;\n'
         '    case CE_P25_TX_NAC: ced_edit=r; ced_val=codeplug_p25_tx_nac(c); ced_len=c->p25_tx_nac_override ? 0 : 3; break;',
         '\tcase CE_P25_NAC: ced_edit = r; ced_val = c->p25_nac; ced_len = 0;\n'
         '#if CONFIG_NAC_DEC\n\t\tced_nac_hex = false;\n\t\tced_typed = false;\n#endif\n\t\tbreak;\n'
         '    case CE_P25_TX_NAC: ced_edit=r; ced_val=codeplug_p25_tx_nac(c); ced_len=c->p25_tx_nac_override ? 0 : 3;\n'
         '#if CONFIG_NAC_DEC\n\t\tced_nac_hex = false;\n\t\tced_typed = false;\n#endif\n\t\tbreak;'),
        # OK: an RX NAC $F7F (3967) or a TX NAC $F7E / $F7F (3966, 3967) is refused, the editor stays open
        ('\tcase CE_P25_NAC: c->p25_nac = (uint16_t)ced_val; break;\n',
         '\tcase CE_P25_NAC:\n'
         '#if CONFIG_NAC_DEC\n'
         '\t\tif (ced_val == 0xF7F) {\n\t\t\tui_toast(TR("3967 ($F7F) is not a NAC: # is Any"));\n\t\t\treturn;\n\t\t}\n'
         '#endif\n'
         '\t\tc->p25_nac = (uint16_t)ced_val;\n\t\tbreak;\n'),
        ('    case CE_P25_TX_NAC: c->p25_tx_nac_override=ced_len==3 ? 0 : (uint16_t)(ced_val+1); break;\n',
         '    case CE_P25_TX_NAC:\n'
         '#if CONFIG_NAC_DEC\n'
         '\t\tif (ced_len != 3 && (ced_val == 0xF7E || ced_val == 0xF7F)) {\n'
         '\t\t\tui_toast(TR("TX NAC can\'t be 3966 or 3967"));\n\t\t\treturn;\n\t\t}\n'
         '#endif\n'
         '\t\tc->p25_tx_nac_override=ced_len==3 ? 0 : (uint16_t)(ced_val+1);\n\t\tbreak;\n'),
    ])

def mw_if(old, new, count=1):
    """(old, new under CONFIG_MULTI_WATCH, else old): a multi-watch change that leaves the line as it was without it"""
    return (old, '#if CONFIG_MULTI_WATCH\n' + new + '#else\n' + old + '#endif\n', count)


def prepare_multi_watch(transform, install):
    """Multi-watch (CONFIG_MULTI_WATCH, docs/plus.md "Multi-watch"): Settings > Talk & listen > Watch channels, 2..6.
    With 3 or more, dual watch's pair becomes a ring of channels A..F on the one receiver: the radio core alternates
    through dual[0..n-1] with dual watch's dwell, hold, wait and looks (integration/radiofoundry/mwatch/radio_mwatch.inc),
    PTT on the selected one; Home shows a compact row per channel (ui_mwatch_draw.inc). The UI keeps dual watch's two
    panes: A, and the selected one of B..F (ui_mwatch.inc), so every dual watch rule acts on the selected channel.
    2 is dual watch exactly. Stored in spare bits of settings_t.dual_state (bits 2-4 the count - 2, 5-7 pane 1's
    letter): the record's layout stays upstream's, an older record reads 2. Runs last (it anchors on the others')."""
    install('core/radio/radio_mwatch.inc', 'integration/radiofoundry/mwatch/radio_mwatch.inc')
    install('core/radio/radio_mwatch.h', 'integration/radiofoundry/mwatch/radio_mwatch.h')
    install('core/ui/ui_mwatch.inc', 'integration/radiofoundry/mwatch/ui_mwatch.inc')
    install('core/ui/ui_mwatch_draw.inc', 'integration/radiofoundry/mwatch/ui_mwatch_draw.inc')
    transform('config/features.mk', [
        ('CONFIG_NAC_DEC ?= n\n',
         'CONFIG_MULTI_WATCH ?= n\nFEATURES += MULTI_WATCH\n'
         'ifeq ($(CONFIG_MULTI_WATCH),y)\nifneq ($(CONFIG_DUAL_WATCH),y)\n'
         '$(error MULTI_WATCH needs DUAL_WATCH=y)\nendif\nendif\n'
         'CONFIG_NAC_DEC ?= n\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_NAC_DEC\n', '#ifndef CONFIG_MULTI_WATCH\n#define CONFIG_MULTI_WATCH 0\n#endif\n#ifndef CONFIG_NAC_DEC\n'),
    ])

    # ---- The radio core ----
    transform('core/radio/radio.h', [
        ('void radio_req_dual(const radio_dual_ref_t ref[2], uint8_t sel);\n',
         'void radio_req_dual(const radio_dual_ref_t ref[2], uint8_t sel);\n'
         '#if CONFIG_MULTI_WATCH\n'
         '// DM32 Plus multi-watch (docs/plus.md): the ring ref[0..n-1] (A..F, n 2..RADIO_DUAL_SLOTS), ref[sel] selected:\n'
         '// dual watch\'s rules for n channels (radio_mwatch.inc). The radio task takes it with radio_mwatch_take\n'
         '#define RADIO_DUAL_SLOTS\t6\n'
         'void radio_req_mwatch(const radio_dual_ref_t ref[], uint8_t n, uint8_t sel);\n'
         'bool radio_mwatch_take(radio_dual_ref_t ref[], uint8_t *n, uint8_t *sel);\n'
         '#else\n#define RADIO_DUAL_SLOTS\t2\n#endif\n'),
    ])
    transform('core/radio/radio_sm.h', [
        ('\n} radio_req_type_t;',
         '\n#if CONFIG_MULTI_WATCH\n\tRREQ_MWATCH,\t\t// DM32 Plus multi-watch: the ring radio_req_mwatch left (radio_mwatch_take)\n'
         '#endif\n} radio_req_type_t;'),
        ('\tradio_dual_ref_t dual[2];\t// SCAN_DUAL: the two channels\n',
         '\tradio_dual_ref_t dual[RADIO_DUAL_SLOTS];\t// SCAN_DUAL: the two channels (DM32 Plus multi-watch: dual_n)\n'),
        ('\tradio_dual_keep_t dual_keep[2];\n', '\tradio_dual_keep_t dual_keep[RADIO_DUAL_SLOTS];\n'),
        ('\tuint32_t dual_wait_ms;\n',
         '\tuint32_t dual_wait_ms;\n'
         '#if CONFIG_MULTI_WATCH\n'
         '\tuint8_t dual_n;\t\t\t// DM32 Plus multi-watch: channels in the ring (0 or 2: the pair)\n'
         '\tuint8_t dual_home;\t\t// ... a look in the wait goes back to this one\n'
         '\tuint8_t dual_look;\t\t// ... the one looked at last (the looks take the others in turn)\n'
         '#endif\n'),
    ])
    transform('core/radio/radio_task.c', [
        ('bool radio_get_dual(radio_dual_ref_t ref[2], uint8_t *sel, uint8_t *at)\n',
         '#if CONFIG_MULTI_WATCH\n'
         '// DM32 Plus multi-watch: the ring for the radio task, under view_lock (a request carries two channels)\n'
         'static radio_dual_ref_t shared_mw[RADIO_DUAL_SLOTS];\n'
         'static uint8_t shared_mw_n, shared_mw_sel;\n'
         'void radio_req_mwatch(const radio_dual_ref_t ref[], uint8_t n, uint8_t sel)\n{\n'
         '\tif (view_lock == NULL || n < 2 || n > RADIO_DUAL_SLOTS) {\n\t\treturn;\n\t}\n'
         '\txSemaphoreTake(view_lock, portMAX_DELAY);\n'
         '\tmemcpy(shared_mw, ref, n * sizeof ref[0]);\n'
         '\tshared_mw_n = n;\n\tshared_mw_sel = sel;\n'
         '\txSemaphoreGive(view_lock);\n'
         '\tpost_pair(RREQ_MWATCH, 0, 0, 0);\n}\n\n'
         'bool radio_mwatch_take(radio_dual_ref_t ref[], uint8_t *n, uint8_t *sel)\n{\n'
         '\txSemaphoreTake(view_lock, portMAX_DELAY);\n'
         '\tmemcpy(ref, shared_mw, sizeof shared_mw);\n'
         '\t*n = shared_mw_n;\n\t*sel = shared_mw_sel;\n'
         '\txSemaphoreGive(view_lock);\n'
         '\treturn *n >= 2;\n}\n#endif\n'
         'bool radio_get_dual(radio_dual_ref_t ref[2], uint8_t *sel, uint8_t *at)\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#include "radio/tones.h"\n',
         '#include "radio/tones.h"\n#if CONFIG_MULTI_WATCH\n#include "radio/radio_mwatch.h"\n'
         '_Static_assert(RADIO_MW_DWELL_MS == RADIO_SCAN_DWELL_MS && RADIO_MW_POLL_MS == RADIO_POLL_MS,\n'
         '\t       "radio_mwatch.h: the ring estimate\'s timing is the radio core\'s");\n#endif\n'),
        # The ring's order: the next channel after the one the receiver is on (2: the other one, as before)
        ('static bool dual_running(const radio_sm_t *sm)\n{\n\treturn sm->scan.kind == SCAN_DUAL;\n}\n',
         'static bool dual_running(const radio_sm_t *sm)\n{\n\treturn sm->scan.kind == SCAN_DUAL;\n}\n\n'
         '// DM32 Plus multi-watch: the channels watched (2: dual watch), and the next one after the one the receiver is on\n'
         'static uint8_t dual_count(const radio_sm_t *sm)\n{\n'
         '#if CONFIG_MULTI_WATCH\n\treturn sm->dual_n > 2 ? sm->dual_n : 2;\n#else\n\t(void)sm;\n\treturn 2;\n#endif\n}\n\n'
         'static uint8_t dual_next(const radio_sm_t *sm)\n{\n\treturn (uint8_t)((sm->dual_at + 1u) % dual_count(sm));\n}\n'),
        ('\t\tif (!dual_tune(sm, (uint8_t)!sm->dual_at, now_ms)) {\n\t\t\tscan_stop(sm, false, now_ms);\n\t\t\treturn false;\n',
         '\t\tif (!dual_tune(sm, dual_next(sm), now_ms)) {\n\t\t\tscan_stop(sm, false, now_ms);\n\t\t\treturn false;\n'),
        # The pair is the ring of two
        # The pair is the ring of two (upstream's dual_start stays as it is without multi-watch)
        ('static void dual_start(radio_sm_t *sm, const radio_dual_ref_t ref[2], uint8_t sel, uint32_t now_ms)\n{\n',
         '#if CONFIG_MULTI_WATCH\n'
         '#include "radio/radio_mwatch.inc"\t// DM32 Plus: dual_start_n, the ring of n\n\n'
         'static void dual_start(radio_sm_t *sm, const radio_dual_ref_t ref[2], uint8_t sel, uint32_t now_ms)\n{\n'
         '\tdual_start_n(sm, ref, 2, sel, now_ms);\t// dual watch: the ring of two\n}\n'
         '#else\n'
         'static void dual_start(radio_sm_t *sm, const radio_dual_ref_t ref[2], uint8_t sel, uint32_t now_ms)\n{\n'),
        ('\tscan_publish(sm);\n}\n\n#if CONFIG_DUAL_WATCH\n// PTT in dual watch',
         '\tscan_publish(sm);\n}\n#endif\t// CONFIG_MULTI_WATCH\n\n#if CONFIG_DUAL_WATCH\n// PTT in dual watch'),
        mw_if('\tsm->dual_keep[0].valid = sm->dual_keep[1].valid = false;\t// the user stopped it: the other\'s go\n',
              '\tdual_keep_drop(sm);\t// the user stopped it: the others\' go\n'),
        mw_if('\t\tsm->dual_keep[0].valid = sm->dual_keep[1].valid = false;\n', '\t\tdual_keep_drop(sm);\n'),
        # The wait after a signal: the looks take the other channels in turn, each back to the one of the wait
        mw_if('\t\tif (!dual_tune(sm, (uint8_t)!sm->dual_at, now_ms)) {\n\t\t\tscan_stop(sm, false, now_ms);\n\t\t\treturn true;\n',
              '\t\tif (!dual_tune(sm, dual_back(sm), now_ms)) {\n\t\t\tscan_stop(sm, false, now_ms);\n\t\t\treturn true;\n'),
        mw_if('\tif (!dual_tune(sm, (uint8_t)!sm->dual_at, now_ms)) {\n\t\tscan_stop(sm, false, now_ms);\n\t\treturn true;\n',
              '\tif (!dual_tune(sm, dual_look_next(sm), now_ms)) {\n\t\tscan_stop(sm, false, now_ms);\n\t\treturn true;\n'),
        # A second receiver takes the pair's other channel only
        ('\tif (sc->kind == SCAN_DUAL && board.rx2 != NULL && rx2_service(sm, now_ms)) {\n',
         '\tif (sc->kind == SCAN_DUAL && board.rx2 != NULL && dual_count(sm) == 2 && rx2_service(sm, now_ms)) {\n'),
        mw_if('\t\tconst radio_dual_ref_t *o = &sm->dual[!sm->dual_sel];\n'
              '\t\tvfo_pick = o->vfo && o->pos == sm->vfo_idx ? !sm->vfo_idx : sm->vfo_idx;\n',
              '\t\tvfo_pick = dual_vfo_taken(sm, sm->vfo_idx) ? !sm->vfo_idx : sm->vfo_idx;\t// the ring: any other\n'),
        ('\tfor (uint8_t i = 0; i < 2; i++) {\n\t\tif (!sm->dual[i].vfo && zone_edit_map(',
         '\tfor (uint8_t i = 0; i < RADIO_DUAL_SLOTS; i++) {\n\t\tif (!sm->dual[i].vfo && zone_edit_map('),
        ('\tcase RREQ_DUAL:\n\t\tdual_request(sm, req, now_ms);\n\t\tbreak;\n',
         '\tcase RREQ_DUAL:\n\t\tdual_request(sm, req, now_ms);\n\t\tbreak;\n'
         '#if CONFIG_MULTI_WATCH\n\tcase RREQ_MWATCH:\n\t\tmw_request(sm, now_ms);\n\t\tbreak;\n#endif\n'),
    ])

    # ---- The settings: the count and pane 1's letter in dual_state's spare bits ----
    transform('core/settings/settings.h', [
        ('#define SETTINGS_DUAL_B_SEL\t0x02u',
         '#define SETTINGS_MW_N_MASK\t0x1Cu\t// DM32 Plus multi-watch: dual_state bits 2-4, the watch channels - 2 (0: dual watch)\n'
         '#define SETTINGS_MW_N_SHIFT\t2\n'
         '#define SETTINGS_MW_P1_MASK\t0xE0u\t// ... bits 5-7: the letter dual watch\'s pane 1 holds in the ring (0: B)\n'
         '#define SETTINGS_MW_P1_SHIFT\t5\n'
         '#define SETTINGS_DUAL_B_SEL\t0x02u'),
    ])
    transform('core/ui/ui_svc.h', [
        ('bool ui_svc_dual(const ui_dual_ref_t ref[2], uint8_t sel);\n',
         'bool ui_svc_dual(const ui_dual_ref_t ref[2], uint8_t sel);\n'
         '#if CONFIG_MULTI_WATCH\n'
         '// DM32 Plus multi-watch (docs/plus.md): the ring ref[0..n-1] (A..F, n 2..UI_MW_MAX), ref[sel] selected. False:\n'
         '// the radio can\'t watch it (a channel twice, one it can\'t read)\n'
         '#define UI_MW_MAX\t6\n'
         'bool ui_svc_mwatch(const ui_dual_ref_t ref[], uint8_t n, uint8_t sel);\n'
         '#endif\n'),
        ('\tUI_SET_COUNT\n} ui_setting_t;',
         '#if CONFIG_MULTI_WATCH\n'
         '\tUI_SET_MWATCH,\t\t// DM32 Plus: the channels the watch alternates, 2..6 (settings_t.dual_state bits 2-4)\n'
         '\tUI_SET_MW_P1,\t\t// ... the letter dual watch\'s pane 1 holds in the ring, 1..5 (dual_state bits 5-7)\n'
         '#endif\n'
         '\tUI_SET_COUNT\n} ui_setting_t;'),
    ])
    transform('core/ui/ui_svc_stub.c', [
        ('\t[UI_SET_NOISE_REDUCTION] = 1,\n};',
         '#if CONFIG_MULTI_WATCH\n\t[UI_SET_MWATCH] = 2, [UI_SET_MW_P1] = 1,\t// DM32 Plus\n#endif\n'
         '\t[UI_SET_NOISE_REDUCTION] = 1,\n};'),
        ('WEAK bool ui_svc_dual_get(',
         '#if CONFIG_MULTI_WATCH\n'
         'WEAK bool ui_svc_mwatch(const ui_dual_ref_t ref[], uint8_t n, uint8_t sel) { (void)ref; (void)n; (void)sel; return false; }\n'
         '#endif\n'
         'WEAK bool ui_svc_dual_get('),
    ])
    transform('core/ui/ui_svc_storage.c', [
        ('\tcase UI_SET_DUAL_B: {\n\t\tsettings_read(&st);\n',
         '#if CONFIG_MULTI_WATCH\n'
         '\tcase UI_SET_MWATCH:\t// DM32 Plus: 0 (an older record, a factory reset) is dual watch\'s 2\n'
         '\t\tsettings_read(&st);\n'
         '\t\treturn clamp16(2 + ((st.dual_state & SETTINGS_MW_N_MASK) >> SETTINGS_MW_N_SHIFT), 2, 6);\n'
         '\tcase UI_SET_MW_P1:\n'
         '\t\tsettings_read(&st);\n'
         '\t\treturn (st.dual_state & SETTINGS_MW_P1_MASK) >> SETTINGS_MW_P1_SHIFT;\n'
         '#endif\n'
         '\tcase UI_SET_DUAL_B: {\n\t\tsettings_read(&st);\n'),
        ('\tcase UI_SET_DUAL_B:\n\t\tst = settings_begin();\n',
         '#if CONFIG_MULTI_WATCH\n'
         '\tcase UI_SET_MWATCH:\t// DM32 Plus: dual watch\'s own bits stay as they are\n'
         '\tcase UI_SET_MW_P1: {\n'
         '\t\tbool count = id == UI_SET_MWATCH;\n'
         '\t\tuint8_t mask = count ? SETTINGS_MW_N_MASK : SETTINGS_MW_P1_MASK;\n'
         '\t\tuint32_t bits = count ? (uint32_t)(clamp16(value, 2, 6) - 2) : (uint32_t)clamp16(value, 0, 5);\n'
         '\t\tst = settings_begin();\n'
         '\t\tst->dual_state = (uint8_t)((st->dual_state & ~mask) |\n'
         '\t\t\t\t\t   ((bits << (count ? SETTINGS_MW_N_SHIFT : SETTINGS_MW_P1_SHIFT)) & mask));\n'
         '\t\tsettings_end(hal_millis());\n'
         '\t\treturn true;\n\t}\n'
         '#endif\n'
         '\tcase UI_SET_DUAL_B:\n\t\tst = settings_begin();\n'),
        mw_if('\t\tst->dual_state = (uint8_t)(((value & UI_DUAL_ON) ? SETTINGS_DUAL_ON : 0) |\n',
              '\t\tst->dual_state = (uint8_t)((st->dual_state & (SETTINGS_MW_N_MASK | SETTINGS_MW_P1_MASK)) |\n'
              '\t\t\t\t\t   ((value & UI_DUAL_ON) ? SETTINGS_DUAL_ON : 0) |\n'),
    ])
    transform('core/ui/ui_svc_radio.c', [
        ('bool ui_svc_dual_get(ui_dual_ref_t ref[2], uint8_t *sel)\n',
         '#if CONFIG_MULTI_WATCH\n'
         '// DM32 Plus multi-watch: the ring to the radio core (radio_req_mwatch), checked as ui_svc_dual checks the pair but\n'
         '// for reading each channel: the UI has just read them for Home\'s rows (ui_mwatch.inc mw_fill), the radio core\n'
         '// reads them again and refuses one it can\'t\n'
         'bool ui_svc_mwatch(const ui_dual_ref_t ref[], uint8_t n, uint8_t sel)\n{\n'
         '\tstatic radio_dual_ref_t r[RADIO_DUAL_SLOTS];\t// static: not on the UI task\'s stack\n'
         '\tif (n < 2 || n > RADIO_DUAL_SLOTS || sel >= n)\n\t\treturn false;\n'
         '\tfor (uint8_t i = 0; i < n; i++) {\n'
         '\t\tr[i] = (radio_dual_ref_t){ ref[i].vfo, ref[i].vfo ? 0 : ref[i].zone,\n'
         '\t\t\t\t\t   ref[i].vfo ? (uint16_t)(ref[i].pos & 1u) : ref[i].pos };\n'
         '\t\tif (r[i].vfo ? !(CONFIG_VFO_FM || CONFIG_VFO_DMR) : r[i].pos >= CODEPLUG_ZONE_MAX)\n'
         '\t\t\treturn false;\n'
         '\t\tfor (uint8_t k = 0; k < i; k++)\n'
         '\t\t\tif (r[k].vfo == r[i].vfo && r[k].zone == r[i].zone && r[k].pos == r[i].pos)\n'
         '\t\t\t\treturn false;\n'
         '\t}\n'
         '\tradio_req_mwatch(r, n, sel);\n'
         '\treturn true;\n}\n\n'
         '#endif\n'
         'bool ui_svc_dual_get(ui_dual_ref_t ref[2], uint8_t *sel)\n'),
    ])
    transform('core/ui/ui_nav.h', [
        ('bool ui_dual_vfo_key(void);',
         '#if CONFIG_MULTI_WATCH\nvoid ui_dual_mw_set(int32_t n);\t\t// DM32 Plus: Settings > Watch channels, 2..6\n#endif\n'
         'bool ui_dual_vfo_key(void);'),
    ])
    transform('core/ui/ui_app.c', [
        ('\tcase UI_SET_DUAL_TALK_BACK: ui_st.dual.talk_back = v != 0; break;\n',
         '\tcase UI_SET_DUAL_TALK_BACK: ui_st.dual.talk_back = v != 0; break;\n'
         '#if CONFIG_MULTI_WATCH\n\tcase UI_SET_MWATCH: ui_dual_mw_set(v); break;\t// DM32 Plus\n#endif\n'),
        ('\t\t\t\t\t  UI_SET_DUAL_TALK_BACK,\n',
         '\t\t\t\t\t  UI_SET_DUAL_TALK_BACK,\n#if CONFIG_MULTI_WATCH\n\t\t\t\t\t  UI_SET_MWATCH,\n#endif\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('\tLIST(TRN("Dual watch delay"), "resume wait reply A B", UI_SET_DUAL_DELAY_MS, v_dual_wait, n_dual_wait),\n',
         '\tLIST(TRN("Dual watch delay"), "resume wait reply A B", UI_SET_DUAL_DELAY_MS, v_dual_wait, n_dual_wait),\n'
         '#if CONFIG_MULTI_WATCH\n'
         '\tNUMBER(TRN("Watch channels"), "multi dual watch ring triple A B C D E F", UI_SET_MWATCH, 2, 6, 1, NULL),\t// DM32 Plus\n'
         '#endif\n'),
    ])

    # ---- Home: a row per channel ----
    transform('core/ui/ui_screens.h', [
        ('\tuint32_t back_ms;\t// ... when it goes back (moved on by a call or a transmission there)\n} ui_dual_t;\n#endif\n',
         '\tuint32_t back_ms;\t// ... when it goes back (moved on by a call or a transmission there)\n} ui_dual_t;\n#endif\n'
         '#if CONFIG_MULTI_WATCH\n'
         '// DM32 Plus multi-watch (ui_dual.c): Home\'s row of a watched channel, A..F\n'
         'typedef struct {\n'
         '\tchar letter;\n'
         '\tbool vfo, sel, rx, busy;\t// the VFO; PTT talks on it; a call comes in on it; a look in the wait heard a signal\n'
         '\tuint8_t mode;\n'
         '\tuint16_t number;\t\t// its codeplug channel number\n'
         '\tuint32_t rx_hz;\n'
         '\tuint32_t heard_ms;\t\t// when its last call ended, 0: none\n'
         '\tconst char *name;\n'
         '\tconst char *caller;\t\t// while a call comes in on it\n'
         '} ui_mw_row_t;\n'
         'uint8_t ui_dual_ring(void);\t\t// channels in the ring on Home, 0: dual watch\'s two panes\n'
         'void ui_dual_ring_row(uint8_t k, const radio_view_t *v, ui_mw_row_t *out);\n'
         'uint32_t ui_dual_ring_ms(uint32_t *p25_extra_ms);\t// once round with nothing heard (radio_mwatch.h), and P25\'s\n'
         'char ui_dual_ring_sel(void);\t\t// the selected letter, \'-\' without the ring\n'
         'char ui_dual_ring_rx(void);\t\t// the letter a call comes in on, \'-\' for none\n'
         '#endif\n'),
    ])
    transform('core/ui/ui_screens.c', [
        ('void ui_draw_home_dual(const radio_view_t *v, const ui_state_t *s)\n{\n',
         '#if CONFIG_MULTI_WATCH\n#include "ui_mwatch_draw.inc"\t// DM32 Plus: Home with 3 to 6 watched channels\n#endif\n\n'
         'void ui_draw_home_dual(const radio_view_t *v, const ui_state_t *s)\n{\n'
         '#if CONFIG_MULTI_WATCH\n'
         '\tuint8_t ring = ui_dual_ring();\n'
         '\tif (ring > 2) {\n\t\tmw_draw_home(v, s, ring);\n\t\tui_draw_banner(s);\n\t\tui_draw_bottom(s, mw_hint(v, s));\n'
         '\t\treturn;\n\t}\n'
         '#endif\n'),
    ])

    # ---- Dual watch's UI (ui_dual.c): the panes carry the ring ----
    transform('core/ui/ui_dual.c', [
        ('#include "codeplug/codeplug.h"\n',
         '#include "codeplug/codeplug.h"\n#if CONFIG_MULTI_WATCH\n#include "radio/radio_mwatch.h"\n#endif\n'),
        ('static ui_dual_t *dual(void) { return &ui_st.dual; }\n',
         'static ui_dual_t *dual(void) { return &ui_st.dual; }\n\n'
         '#define MW_ANY_PANE\t0xFEu\t// native_caller: the channel on view, whatever pane (DM32 Plus multi-watch)\n'
         '#if CONFIG_MULTI_WATCH\n'
         '// DM32 Plus multi-watch (ui_mwatch.inc, included at the end): the ring of 3 to 6 channels on the two panes\n'
         '#define MW_LETTER_ARG\t0x10u\t// a picker\'s arg for a ring letter outside the panes: MW_LETTER_ARG | letter\n'
         'static void mw_reset(void);\n'
         'static bool mw_ring(void);\n'
         'static bool mw_in_ring(void);\n'
         'static bool mw_svc(const ui_dual_ref_t r[2], uint8_t sel);\n'
         'static uint8_t mw_view_sel(uint8_t radio_sel);\n'
         'static bool mw_taken(const ui_dual_ref_t *r, uint8_t pane);\n'
         'static void mw_save(void);\n'
         'static void mw_resume(void);\n'
         'static void mw_select(uint8_t k);\n'
         'static void mw_step_sel(int dir);\n'
         'static void mw_find_again(void);\n'
         'static bool mw_tick(ui_dual_t *d, const radio_view_t *v, uint32_t now);\n'
         'static uint8_t mw_sel_letter(void);\n'
         'static uint8_t mw_letter_of(uint8_t pane);\n'
         'static char mw_char(uint8_t k);\n'
         'static void mw_toast_letter(const char *before, uint8_t k, const char *after);\n'
         'static uint16_t mw_actions_count(void);\n'
         'static bool mw_actions_item(uint16_t i, ui_item_t *it);\n'
         'static bool mw_actions_select(uint16_t i, bool hold);\n'
         'static const ui_dual_ref_t *mw_pick_ref(uint16_t arg);\n'
         'static char mw_taken_tag(const ui_dual_ref_t *r, uint16_t arg);\n'
         'static void mw_pick_set(uint16_t arg, const ui_dual_ref_t *r);\n'
         'static void mw_pick_vfo(uint16_t arg);\n'
         'static const char *mw_title(uint16_t arg);\n'
         '#if CONFIG_ZONE_WRITE\n'
         'static void mw_follow_zones(uint32_t from, uint32_t to);\n'
         '#endif\n'
         '#else\n'
         '#define mw_reset()\t\t((void)0)\n'
         '#define mw_ring()\t\tfalse\n'
         '#define mw_svc\t\t\tui_svc_dual\n'
         '#define mw_view_sel(s)\t\t(s)\n'
         '#define mw_taken(r, p)\t\tfalse\n'
         '#define mw_resume()\t\t((void)0)\n'
         '#endif\n'),
        ('\toff_ms = 0;\n\tdual()->back_to = UI_DUAL_NONE;\n}\n',
         '\toff_ms = 0;\n\tdual()->back_to = UI_DUAL_NONE;\n\tmw_reset();\n}\n'),
        ('\t\tui_svc_setting_set(UI_SET_DUAL_B, pane_code(!sel) | UI_DUAL_ON | (sel ? UI_DUAL_B_SEL : 0));\n}\n',
         '\t\tui_svc_setting_set(UI_SET_DUAL_B, pane_code(!sel) | UI_DUAL_ON | (sel ? UI_DUAL_B_SEL : 0));\n'
         '#if CONFIG_MULTI_WATCH\n\tmw_save();\t// and pane 1\'s letter in the ring\n#endif\n}\n'),
        # The receiver on a letter outside the panes: the selected channel's settings, as on the other pane
        ('\tif (t == d->sel || t == UI_DUAL_NONE)\n\t\treturn v;\n',
         '\tif (t == d->sel || (t == UI_DUAL_NONE && !mw_ring()))\n\t\treturn v;\n'),
        ('\tif (!ui_svc_dual(ref, sel)) {\n\t\tui_toast(TR(TEXT_ALT_OPEN "Can\'t watch that channel',
         '\tif (!mw_svc(ref, sel)) {\n\t\tui_toast(TR(TEXT_ALT_OPEN "Can\'t watch that channel'),
        ('\tif (!ui_svc_dual(ref, d->sel)) {\n', '\tif (!mw_svc(ref, d->sel)) {\n', 2),
        ('\tif (ref_same(&ref[0], &ref[1]) || !ui_svc_dual(ref, sel))\n',
         '\tmw_resume();\n\tif (ref_same(&ref[0], &ref[1]) || !mw_svc(ref, sel))\n'),
        ('\tcp_serial = codeplug_serial();\n\tfor (uint8_t i = 0; i < 2; i++) {\n\t\tif (ref[i].vfo)\n\t\t\tcontinue;\n',
         '\tcp_serial = codeplug_serial();\n'
         '#if CONFIG_MULTI_WATCH\n\tmw_find_again();\n#endif\n'
         '\tfor (uint8_t i = 0; i < 2; i++) {\n\t\tif (ref[i].vfo)\n\t\t\tcontinue;\n'),
        mw_if('\tui_toast(d->sel ? TR("Dual watch off: on B") : TR("Dual watch off: on A"));\n',
              '\tif (mw_in_ring())\t// (off already: the ring it was)\n'
              '\t\tmw_toast_letter(TR("Watch off: on "), mw_sel_letter(), "");\n\telse\n'
              '\t\tui_toast(d->sel ? TR("Dual watch off: on B") : TR("Dual watch off: on A"));\n'),
        ('void ui_dual_select(uint8_t pane)\n{\n\tui_dual_t *d = dual();\n',
         'void ui_dual_select(uint8_t pane)\n{\n\tui_dual_t *d = dual();\n'
         '#if CONFIG_MULTI_WATCH\n\tif (mw_ring()) {\n\t\tmw_select(mw_letter_of(pane));\n\t\treturn;\n\t}\n#endif\n'),
        ('\t\tif (!ref_same(&next, &ref[!d->sel]))\n\t\t\tbreak;\n',
         '\t\tif (!ref_same(&next, &ref[!d->sel]) && !mw_taken(&next, d->sel))\n\t\t\tbreak;\n'),
        mw_if('\t\tui_dual_select(ev->action == ACT_NAV_RIGHT ? 1 : 0);\n',
              '\t\tif (mw_ring())\n\t\t\tmw_step_sel(ev->action == ACT_NAV_RIGHT ? 1 : -1);\t// the ring: the next / previous\n'
              '\t\telse\n\t\t\tui_dual_select(ev->action == ACT_NAV_RIGHT ? 1 : 0);\n'),
        mw_if('\tif (d->on)\n\t\tui_dual_select(!d->sel);\n',
              '\tif (d->on && mw_ring())\n\t\tmw_step_sel(1);\t// the ring: the next channel\n'
              '\telse if (d->on)\n\t\tui_dual_select(!d->sel);\n'),
        ('\t\twhile (r.pos < z.count && ref_same(&r, &ref[!d->sel]))\n',
         '\t\twhile (r.pos < z.count && (ref_same(&r, &ref[!d->sel]) || mw_taken(&r, d->sel)))\n'),
        mw_if('\t\tsb_str(&b, d->sel ? TR("B zone: ") : TR("A zone: "));\n',
              '\t\tif (mw_ring()) {\n\t\t\tsb_chr(&b, mw_char(mw_sel_letter()));\n\t\t\tsb_str(&b, TR(" zone: "));\n'
              '\t\t} else {\n\t\t\tsb_str(&b, d->sel ? TR("B zone: ") : TR("A zone: "));\n\t\t}\n'),
        mw_if('\tif (ref_same(r, &ref[!pane])) {\n',
              '\tif (mw_taken(r, pane)) {\t// the ring: another letter\'s\n'
              '\t\tui_toast(TR(TEXT_ALT_OPEN "Watched already: pick another" TEXT_ALT_NEXT "Watched already" TEXT_ALT_CLOSE));\n'
              '\t\treturn false;\n\t}\n'
              '\tif (ref_same(r, &ref[!pane])) {\n'),
        mw_if('\t\t\tui_toast(sel ? TR("B: VFO") : TR("A: VFO"));\n',
              '\t\t{\t// (the if above has no braces)\n'
              '\t\t\tif (mw_ring())\n\t\t\t\tmw_toast_letter("", mw_sel_letter(), TR(": VFO"));\n'
              '\t\t\telse\n\t\t\t\tui_toast(sel ? TR("B: VFO") : TR("A: VFO"));\n\t\t}\n'),
        ('\tif (sel_asked && v->dual_sel != d->sel) {\n\t\td->sel = v->dual_sel ? 1 : 0;\n',
         '\tif (sel_asked && mw_view_sel(v->dual_sel) != d->sel) {\n\t\td->sel = mw_view_sel(v->dual_sel) ? 1 : 0;\n'),
        ('\t\t\tif (d->talk_back && i != d->sel && v->state != RS_TX)\n',
         '\t\t\tif (d->talk_back && i != d->sel && v->state != RS_TX && !mw_ring())\t// the ring: no talk back\n'),
        ('\ttalk_back_poll(d, v, now);\n\treturn dirty || d->sel != sel0 || d->back_to != back0;\n',
         '\ttalk_back_poll(d, v, now);\n'
         '#if CONFIG_MULTI_WATCH\n\tdirty = mw_tick(d, v, now) || dirty;\t// the ring\'s rows\n#endif\n'
         '\treturn dirty || d->sel != sel0 || d->back_to != back0;\n'),
        ('\tif (zone_edit_map(zones_seen, to, pick_zone, &out) != ZONE_MAP_LOST)\n\t\tpick_zone = out;\n',
         '\tif (zone_edit_map(zones_seen, to, pick_zone, &out) != ZONE_MAP_LOST)\n\t\tpick_zone = out;\n'
         '#if CONFIG_MULTI_WATCH\n\tmw_follow_zones(zones_seen, to);\n#endif\n'),
        ('\tif (!codeplug_mode_p25(v->ch.mode) || tuned_pane(v) != i)\n',
         '\tif (!codeplug_mode_p25(v->ch.mode) || (i != MW_ANY_PANE && tuned_pane(v) != i))\n'),
        # The actions (#): the ring adds Change C..F as 6..9 (ui_mwatch.inc)
        mw_if('static uint16_t actions_count(void) { return DA_COUNT; }\n',
              'static uint16_t actions_count(void) { return mw_actions_count(); }\n'),
        ('static void actions_item(uint16_t i, ui_item_t *it)\n{\n',
         'static void actions_item(uint16_t i, ui_item_t *it)\n{\n'
         '#if CONFIG_MULTI_WATCH\n\tif (mw_actions_item(i, it))\n\t\treturn;\n#endif\n'),
        ('static void actions_select(uint16_t i, bool hold)\n{\n\t(void)hold;\n',
         'static void actions_select(uint16_t i, bool hold)\n{\n\t(void)hold;\n'
         '#if CONFIG_MULTI_WATCH\n\tif (mw_actions_select(i, hold))\n\t\treturn;\n#endif\n'),
        # The pickers: arg MW_LETTER_ARG | letter for a ring letter outside the panes
        mw_if('\tconst ui_dual_ref_t *r = &ref[pane];\n\tbool have = dual()->on || (pane == 1 && b_last());\n',
              '\tconst ui_dual_ref_t *r = mw_pick_ref(pane);\n\tbool have = dual()->on || (pane == 1 && b_last());\n'),
        mw_if('\tui_dual_ref_t other;\n\tif (d->on)\n\t\tother = ref[!pane];\n\telse\n\t\tref_here(ui_view(), &other);\n'
              '\tbool on_a = ref_same(&r, &other);\n\tif (on_a) {\n\t\tsb_str(&b, pane ? "A" : "B");\n',
              '\tstatic char tag[2];\t// the letter that has it (dual watch: the other pane, A while off)\n'
              '\ttag[0] = mw_taken_tag(&r, pane);\n'
              '\tbool on_a = tag[0] != 0;\n\tif (on_a) {\n\t\tsb_str(&b, tag);\n'),
        mw_if('\tif (d->on && ref_same(&r, &ref[pane]))\n',
              '\tif (d->on && ref_same(&r, mw_pick_ref(pane)))\n'),
        mw_if('\tui_go_home();\n\tset_pane(pane, &r);\n}\n', '\tui_go_home();\n\tmw_pick_set(pane, &r);\n}\n'),
        mw_if('\tif (i == 0) {\n\t\tui_dual_ref_t r = vfo_for(pane);\n\t\tui_go_home();\n\t\tset_pane(pane, &r);\n\t\treturn;\n\t}\n',
              '\tif (i == 0) {\n\t\tmw_pick_vfo(pane);\n\t\treturn;\n\t}\n'),
        mw_if('\treturn ui_frame()->arg ? "B" : "A";\n', '\treturn mw_title(ui_frame()->arg);\n', 2),
        ('\t.title = zone_title,\n};\n#endif\n',
         '\t.title = zone_title,\n};\n#if CONFIG_MULTI_WATCH\n#include "ui_mwatch.inc"\t// DM32 Plus\n#endif\n#endif\n'),
    ])

    # ---- The upstream UI simulator (tests/host/sim): the ring as the radio core takes it, for tests/plus/test_ui_mwatch.c
    transform('tests/host/sim/ui_sim.c', [
        mw_if('static ui_dual_ref_t sim_dual[2];\n',
              'static ui_dual_ref_t sim_dual[6];\t// DM32 Plus multi-watch: the ring, A..F\n'
              'static uint8_t sim_mw_n = 2;\t\t// ... channels in it (2: dual watch)\n'),
        mw_if('static radio_view_t sim_keep[2];\nstatic uint32_t sim_keep_target[2];\nstatic bool sim_keep_ok[2];\n',
              'static radio_view_t sim_keep[6];\nstatic uint32_t sim_keep_target[6];\nstatic bool sim_keep_ok[6];\n'),
        mw_if('\tif (sim_at < 2 && view.scan == 3) {\n', '\tif (sim_at < 6 && view.scan == 3) {\n'),
        ('\t\tsim_keep_ok[0] = sim_keep_ok[1] = false;\n\t\tchanged();\n\t\treturn true;\n\t}\n',
         '\t\tsim_keep_ok[0] = sim_keep_ok[1] = false;\n'
         '#if CONFIG_MULTI_WATCH\n\t\tmemset(sim_keep_ok, 0, sizeof sim_keep_ok);\n#endif\n'
         '\t\tchanged();\n\t\treturn true;\n\t}\n'),
        ('\tsim_dual[0] = ref[0];\n\tsim_dual[1] = ref[1];\n',
         '#if CONFIG_MULTI_WATCH\n\tsim_mw_n = 2;\n#endif\n\tsim_dual[0] = ref[0];\n\tsim_dual[1] = ref[1];\n'),
        ('// The selected channel for a request that acts on it (power, squelch, the monitor, a talk target), as the radio core\n',
         '#if CONFIG_MULTI_WATCH\n'
         '// DM32 Plus multi-watch: the ring as radio_mwatch.inc takes it: the receiver stays where it is when that is one of\n'
         '// the ring (a new selection alone never retunes), else it tunes the selected one; tune:a..f moves it\n'
         'bool ui_svc_mwatch(const ui_dual_ref_t ref[], uint8_t n, uint8_t sel)\n{\n'
         '\tif (n < 2 || n > 6 || sel >= n)\n\t\treturn false;\n'
         '\tfor (int i = 0; i < n; i++) {\n'
         '\t\tif (!ref[i].vfo) {\n\t\t\tzone_t z;\n'
         '\t\t\tif (!codeplug_zone(ref[i].zone, &z) || ref[i].pos >= z.count)\n\t\t\t\treturn false;\n\t\t}\n'
         '\t\tfor (int k = 0; k < i; k++)\n\t\t\tif (sim_same(&ref[k], &ref[i]))\n\t\t\t\treturn false;\n'
         '\t}\n'
         '\tfor (int i = 0; i < 6; i++)\n'
         '\t\tif (i >= n || i >= sim_mw_n || !sim_same(&sim_dual[i], &ref[i]))\n\t\t\tsim_keep_ok[i] = false;\n'
         '\tfor (int i = 0; i < n; i++)\n\t\tsim_dual[i] = ref[i];\n'
         '\tsim_mw_n = n;\n\tsim_sel = sel;\n\tview.dual_sel = sel;\n'
         '\tui_dual_ref_t here = { view.vfo, view.zone_index, view.vfo ? view.vfo_index : view.zone_pos };\n'
         '\tint at = -1;\n'
         '\tfor (int i = 0; i < n; i++)\n\t\tif (sim_same(&here, &ref[i]))\n\t\t\tat = i;\n'
         '\tif (at >= 0)\n\t\tsim_at = (uint8_t)at;\n\telse\n\t\tsim_tune(sel);\n'
         '\tview.scan = 3;\n\tchanged();\n\treturn true;\n}\n'
         '#endif\n'
         '// The selected channel for a request that acts on it (power, squelch, the monitor, a talk target), as the radio core\n'),
        mw_if('\tif (strcmp(t, "tune") == 0 && arg != NULL && (arg[0] == \'a\' || arg[0] == \'b\')) {\t// dual watch: the receiver\n'
              '\t\tsim_tune(arg[0] == \'b\');\t\t\t\t\t\t\t// on A or B\n',
              '\tif (strcmp(t, "tune") == 0 && arg != NULL && arg[0] >= \'a\' && arg[0] < \'a\' + sim_mw_n) {\t// on A..F\n'
              '\t\tsim_tune(arg[0] - \'a\');\n'),
        mw_if('\t\tview.dual_busy = arg[0] == \'a\' ? 1 : arg[0] == \'b\' ? 2 : 0;\n',
              '\t\tview.dual_busy = (uint8_t)(arg[0] >= \'a\' && arg[0] <= \'f\' ? 1u << (arg[0] - \'a\') : 0u);\n'),
        ('\telse if (strcmp(key, "calls") == 0) snprintf(got, sizeof got, "%u", ui_st.dual.calls);\n',
         '\telse if (strcmp(key, "calls") == 0) snprintf(got, sizeof got, "%u", ui_st.dual.calls);\n'
         '#if CONFIG_MULTI_WATCH\n'
         '\t// DM32 Plus multi-watch: the UI\'s ring (its selected letter, the receiving one, how many), the radio\'s (where\n'
         '\t// the receiver is, the selected letter, the channels it watches) and a letter\'s channel position: mwposd\n'
         '\telse if (strcmp(key, "mwsel") == 0) snprintf(got, sizeof got, "%c", ui_dual_ring_sel());\n'
         '\telse if (strcmp(key, "mwrx") == 0) snprintf(got, sizeof got, "%c", ui_dual_ring_rx());\n'
         '\telse if (strcmp(key, "mwn") == 0) snprintf(got, sizeof got, "%u", ui_dual_ring());\n'
         '\telse if (strcmp(key, "mwat") == 0) snprintf(got, sizeof got, "%c", sim_at < 6 ? \'A\' + sim_at : \'-\');\n'
         '\telse if (strcmp(key, "mwradiosel") == 0) snprintf(got, sizeof got, "%c", \'A\' + sim_sel);\n'
         '\telse if (strcmp(key, "mwradion") == 0) snprintf(got, sizeof got, "%u", view.scan == 3 ? sim_mw_n : 0u);\n'
         '\telse if (strncmp(key, "mwpos", 5) == 0 && key[5] >= \'a\' && key[5] <= \'f\')\n'
         '\t\tsnprintf(got, sizeof got, "%u", sim_dual[key[5] - \'a\'].pos + 1u);\n'
         '#endif\n'),
        ('\tif (strcmp(t, "receivers") == 0 && arg != NULL) {\n',
         '#if CONFIG_MULTI_WATCH\n'
         '\tif (strcmp(t, "mwatch") == 0 && arg != NULL) {\t// DM32 Plus: Settings > Watch channels\n'
         '\t\tui_svc_setting_set(UI_SET_MWATCH, atoi(arg));\n'
         '\t\tui_apply_setting(UI_SET_MWATCH, atoi(arg));\n'
         '\t\tadvance(100);\n\t\treturn true;\n\t}\n'
         '#endif\n'
         '\tif (strcmp(t, "receivers") == 0 && arg != NULL) {\n'),
        ('\tui_svc_setting_set(UI_SET_DUAL_B, 0);\n',
         '\tui_svc_setting_set(UI_SET_DUAL_B, 0);\n'
         '#if CONFIG_MULTI_WATCH\n'
         '\tui_svc_setting_set(UI_SET_MWATCH, 2);\n\tui_svc_setting_set(UI_SET_MW_P1, 1);\n'
         '\tmemset(sim_keep_ok, 0, sizeof sim_keep_ok);\n\tsim_mw_n = 2;\n'
         '#endif\n'),
    ])


def prepare_mapnav(transform, install):
    """Map navigation (docs/map.md "APRS stations, auto repeater, track"; owner's round 3 items 7-9). Needs MAP.
    - APRS stations on Tools > Map: upstream's heard list (aprs_station_t, kept by the engine; no copy) as dots with
      their callsigns from zoom 3; 0 picks the one nearest the map's centre (0 again: the next), "TA1ABC-9 4.2 km NE
      3m ago" under the map, OK its details (symbol, altitude, comment), OK again a message to it (CONFIG_APRS_MSG).
      The engine's station entry gets the altitude it parses (int16 in the struct's tail padding: no RAM).
    - Auto repeater (CONFIG_AUTO_RPT, needs REPEATERS): Tools > Auto repeater, On / Off (RAM only, Off at power on).
      With a GPS fix, every 30 s at most, the nearest FM repeater (DMR on a DMR channel) in the radio's bands from the
      built-in table; a nearer one is offered when it is 20 % and 2 km nearer than the one offered before, as a
      banner over any screen ("Nearer: <call> <MHz> <km dir>", "# tunes the VFO"); # tunes it with Tools >
      Repeaters' own tune (ui_rpt_tune). It never tunes by itself and never transmits.
    - Track (CONFIG_MAP_TRACK): the last 48 GPS points (30 s and 50 m apart, 0.001 degree cells) drawn dotted on the
      map; 7 held clears it, 9 saves its first point as a waypoint and opens Go-To on it ("Back to start").
    Our files: integration/radiofoundry/map/mapnav.{c,h} (portable), ui_mapnav.c (the UI task's tick and the Tools
    entry), ui_map.c (prepare_map)."""
    install('core/ui/mapnav.h', 'integration/radiofoundry/map/mapnav.h')
    install('core/ui/mapnav.c', 'integration/radiofoundry/map/mapnav.c')
    install('core/ui/ui_mapnav.c', 'integration/radiofoundry/map/ui_mapnav.c')
    transform('config/features.mk', [
        ('CONFIG_MAP ?= n\nFEATURES += MAP\nifeq ($(CONFIG_MAP),y)\nifneq ($(CONFIG_NAV),y)\n$(error MAP needs NAV=y)\n'
         'endif\nendif\n',
         'CONFIG_MAP ?= n\nFEATURES += MAP\nifeq ($(CONFIG_MAP),y)\nifneq ($(CONFIG_NAV),y)\n$(error MAP needs NAV=y)\n'
         'endif\nendif\n'
         'CONFIG_MAP_TRACK ?= n\nFEATURES += MAP_TRACK\n'
         'ifeq ($(CONFIG_MAP_TRACK),y)\nifneq ($(CONFIG_MAP),y)\n$(error MAP_TRACK needs MAP=y)\nendif\nendif\n'
         'CONFIG_AUTO_RPT ?= n\nFEATURES += AUTO_RPT\n'
         'ifeq ($(CONFIG_AUTO_RPT),y)\nifneq ($(CONFIG_REPEATERS)$(CONFIG_GPS),yy)\n'
         '$(error AUTO_RPT needs REPEATERS=y and GPS=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_MAP\n',
         '#ifndef CONFIG_MAP_TRACK\n#define CONFIG_MAP_TRACK 0\n#endif\n#ifndef CONFIG_AUTO_RPT\n#define CONFIG_AUTO_RPT 0\n'
         '#endif\n#ifndef CONFIG_MAP\n'),
    ])
    # The station list keeps the altitude the parser found (the map's details): an int16 in the struct's tail padding
    transform('core/aprs/aprs.h', [
        ('\tchar text[24];\t\t// comment / status (truncated)\n} aprs_station_t;',
         '\tchar text[24];\t\t// comment / status (truncated)\n'
         '#if CONFIG_MAP\n\tint16_t alt_m;\t\t// DM32 Plus: metres, INT16_MIN none (Tools > Map\'s station details)\n#endif\n'
         '} aprs_station_t;'),
    ])
    transform('core/aprs/aprs.c', [
        ('\t\ts->speed_kn = p->pos.speed_kn;\n\t}\n',
         '\t\ts->speed_kn = p->pos.speed_kn;\n'
         '#if CONFIG_MAP\n'
         '\t\ts->alt_m = p->pos.alt_m == APRS_ALT_NONE ? INT16_MIN : (int16_t)(p->pos.alt_m > 32767 ? 32767 :\n'
         '\t\t\t   p->pos.alt_m < -32767 ? -32767 : p->pos.alt_m);\n'
         '#endif\n'
         '\t}\n'),
        ('\t\tmemcpy(s->text, "DMR", 4);\n',
         '\t\tmemcpy(s->text, "DMR", 4);\n#if CONFIG_MAP\n\t\ts->alt_m = INT16_MIN;\n#endif\n'),
    ])
    # The auto repeater's offer: a banner kind, # on it, the UI tick (the track too)
    transform('core/ui/ui_screens.h', [
        ('\tUI_BANNER_ZONE_OFFER,\t// another radio offers a zone or channel: CONTEXT opens the offer\n',
         '\tUI_BANNER_ZONE_OFFER,\t// another radio offers a zone or channel: CONTEXT opens the offer\n'
         '#if CONFIG_AUTO_RPT\n\tUI_BANNER_RPT,\t\t// DM32 Plus auto repeater: a nearer repeater, CONTEXT tunes the VFO to it\n'
         '#endif\n'),
    ])
    transform('core/ui/ui_app.c', [
        ('#include "radio/tones.h"\n#include "config.h"\n',
         '#include "radio/tones.h"\n#include "config.h"\n'
         '#if CONFIG_AUTO_RPT || CONFIG_MAP_TRACK\n#include "ui/mapnav.h"\n#endif\n'),
        ('bool ui_app_take_wake(void)\n',
         '#if CONFIG_AUTO_RPT\n// DM32 Plus auto repeater (ui_mapnav.c): its offer, over any screen\n'
         'void ui_banner_offer(uint8_t kind, const char *title, const char *text)\n{\n'
         '\tbanner(kind, false, 0, title, text);\n}\n#endif\n\n'
         'bool ui_app_take_wake(void)\n'),
        ('\t\tui_push(SCR_OFFER, 0);\n#endif\n\telse\n\t\tui_open_actions(&t);',
         '\t\tui_push(SCR_OFFER, 0);\n#endif\n'
         '#if CONFIG_AUTO_RPT\n\telse if (kind == UI_BANNER_RPT)\n\t\tui_mapnav_banner_open();\n#endif\n'
         '\telse\n\t\tui_open_actions(&t);'),
        ('\tif (ui_aprsmsg_tick(stack[sp].scr))\n\t\tdirty = true;\n#endif\n',
         '\tif (ui_aprsmsg_tick(stack[sp].scr))\n\t\tdirty = true;\n#endif\n'
         '#if CONFIG_AUTO_RPT || CONFIG_MAP_TRACK\n'
         '\t// DM32 Plus: the GPS track and the auto repeater\'s check, once a second (ui_mapnav.c)\n'
         '\tif (ui_mapnav_tick(now))\n\t\tdirty = true;\n#endif\n'),
    ])
    # Tools > Auto repeater (tool 27, after Repeaters; Battery and the zone offer move one on): On / Off, OK toggles
    transform('core/ui/ui_menus.c', [
        ('#include "radio/ref_trim.h"\n',
         '#include "radio/ref_trim.h"\n#if CONFIG_AUTO_RPT\n#include "ui/mapnav.h"\n#endif\n'),
        ('T_REPEATERS, ', 'T_REPEATERS, T_AUTORPT, '),
        ('(i != T_REPEATERS || CONFIG_REPEATERS) &&',
         '(i != T_REPEATERS || CONFIG_REPEATERS) && (i != T_AUTORPT || CONFIG_AUTO_RPT) &&'),
        ('TRN("Repeaters"), ', 'TRN("Repeaters"), TRN("Auto repeater"), '),
        ('\t\t[T_REPEATERS] = ART_ICON_GOTO + 1,\t// the way to one\n',
         '\t\t[T_REPEATERS] = ART_ICON_GOTO + 1,\t// the way to one\n'
         '\t\t[T_AUTORPT] = ART_ICON_GOTO + 1,\t// and to the nearest on the way\n'),
        ('\tcase T_APRS:\n\t\tif (!CONFIG_APRS)\n\t\t\tit->flags |= UI_ITEM_DIM;\n\t\tbreak;\n',
         '\tcase T_APRS:\n\t\tif (!CONFIG_APRS)\n\t\t\tit->flags |= UI_ITEM_DIM;\n\t\tbreak;\n'
         '#if CONFIG_AUTO_RPT\n\tcase T_AUTORPT:\n\t\tsb_str(&b, ui_mapnav_auto_on() ? TR("On") : TR("Off"));\n\t\tbreak;\n'
         '#endif\n'),
        ('\tcase T_REPEATERS: ui_push(SCR_REPEATERS, 0); break;\n',
         '\tcase T_REPEATERS: ui_push(SCR_REPEATERS, 0); break;\n'
         '#if CONFIG_AUTO_RPT\n\tcase T_AUTORPT: ui_mapnav_auto_toggle(); break;\n#endif\n'),
    ])


def prepare_p25_extras(transform):
    """P25 extras (CONFIG_P25_EXTRAS, docs/plus.md "P25 emergency and unit-to-unit calls"): the link control's
    emergency bit (service options bit 7, LCF 0x00 and 0x03) and unit-to-unit voice (LCF 0x03: the called unit in
    octets 3-5) are decoded in src/p25/p25.c and voice.c. Home's caller line (and with it dual watch panes and
    multi-watch rows) says "EMERGENCY ..." / "Private 77285 > 1234" (integration/radiofoundry/p25_lab.c); while an
    emergency call comes in, Home's activity line says EMERGENCY in the warning colour; the call history marks it
    (warning colour, "EMERGENCY") and logs a unit-to-unit call as "Private > 1234". A unit-to-unit call plays on a
    channel with no talkgroup filter (Any, P25 search) and, on a talkgroup channel, only when it is for the
    channel's own unit ID; others show and stay muted, as calls on another talkgroup do. Talker alias: not built
    (no verifiable reference for the FDMA link control format in the tree; docs/plus.md)."""
    transform('config/features.mk', [
        ('CONFIG_NAC_DEC ?= n\nFEATURES += NAC_DEC\n',
         'CONFIG_NAC_DEC ?= n\nFEATURES += NAC_DEC\n'
         'CONFIG_P25_EXTRAS ?= n\nFEATURES += P25_EXTRAS\n'
         'ifeq ($(CONFIG_P25_EXTRAS),y)\nifneq ($(CONFIG_P25_CALLER),y)\n$(error P25_EXTRAS needs P25_CALLER=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_NAC_DEC\n', '#ifndef CONFIG_P25_EXTRAS\n#define CONFIG_P25_EXTRAS 0\n#endif\n#ifndef CONFIG_NAC_DEC\n'),
    ])
    # The call history: a spare byte of the entry (after to_us: no RAM) marks an emergency
    transform('core/ui/ui_nav.h', [
        ('\tbool group, out, to_us;\n',
         '\tbool group, out, to_us;\n#if CONFIG_P25_EXTRAS\n'
         '\tbool emerg;\t\t// DM32 Plus: a P25 emergency call (its link control\'s emergency bit)\n#endif\n'),
    ])
    transform('core/ui/ui_app.c', [
        ('\t\t\tcall_log_add(RADIO_MODE_P25, c.src, c.tg, true, c.nac, c.encrypted, c.n, NULL);\n',
         '#if CONFIG_P25_EXTRAS\n'
         '\t\t\t// DM32 Plus P25 extras: a unit-to-unit call goes in as one to its unit, an emergency marked\n'
         '\t\t\tcall_log_add(RADIO_MODE_P25, c.src, c.priv ? c.target : c.tg, !c.priv, c.nac, c.encrypted, c.n, NULL);\n'
         '\t\t\trecent[0].emerg = c.emergency;\n'
         '#else\n'
         '\t\t\tcall_log_add(RADIO_MODE_P25, c.src, c.tg, true, c.nac, c.encrypted, c.n, NULL);\n'
         '#endif\n'),
        ('\t\t\t   (recent[0].src != c.src || recent[0].enc != c.encrypted))) {\n',
         '#if CONFIG_P25_EXTRAS\n'
         '\t\t\t   (recent[0].src != c.src || recent[0].enc != c.encrypted || recent[0].emerg != c.emergency))) {\n'
         '#else\n'
         '\t\t\t   (recent[0].src != c.src || recent[0].enc != c.encrypted))) {\n'
         '#endif\n'),
        ('\t\t\t\t\trecent[i].enc = c.encrypted;\n',
         '\t\t\t\t\trecent[i].enc = c.encrypted;\n#if CONFIG_P25_EXTRAS\n'
         '\t\t\t\t\trecent[i].emerg = c.emergency;\n#endif\n'),
    ])
    # A P25 entry's row in Recent: the DM-32's rows are narrow, and a row keeps its label whole before anything else,
    # so the parts go as text alternates: wide "P25 77285  TG 101 NAC 293 5m ago", narrower "TG 101 NAC 293 5m", then
    # "TG 101 5m"; "Private > 1234 5m ago" | "Private > 1234 5m" | "> 1234 5m"; "EMERGENCY TG 101 5m ago" | ... |
    # "EMERGENCY 5m" (the warning colour, as an encrypted call's row already was); the label may drop "P25 "
    transform('core/ui/ui_menus.c', [
        ('static void recent_item(uint16_t i, ui_item_t *it)\n{\n\tconst ui_recent_t *r = ui_recent_get((uint8_t)i);\n'
         '\tif (r == NULL)\n\t\treturn;\n#if CONFIG_CALL_LOG\n',
         '''#if CONFIG_P25_EXTRAS
// DM32 Plus P25 extras: a P25 call's row in Recent (the call history). Its value's parts as text alternates (font.h:
// the row shows the deepest level it needs; sb_age shortens at the first): the NAC, "Private", an emergency's talkgroup
// or called unit are kept at levels 0 and 1, so they go at level 2. "EMERGENCY" and "Encrypted" always stay. A part
// is offered at both levels, at level 0 only, or not at all, as the value's 44 bytes hold it with what comes after
static size_t p25_recent_digits(uint32_t v)
{
	size_t n = 1;
	while (v >= 10u) {
		v /= 10u;
		n++;
	}
	return n;
}

static void p25_recent_part(sbuf_t *b, const ui_recent_t *r, unsigned what)
{
	if (what == 0) {		// " NAC 293"; CONFIG_NAC_DEC: both forms, " $125/293" (the "$" marks the NAC)
#if CONFIG_NAC_DEC
		sb_str(b, " $");
#else
		sb_str(b, " NAC ");
#endif
		for (int shift = 8; shift >= 0; shift -= 4)
			sb_chr(b, "0123456789ABCDEF"[(r->nac >> shift) & 15u]);
#if CONFIG_NAC_DEC
		sb_chr(b, '/');
		sb_u32(b, r->nac, 1);
#endif
	} else if (what == 1) {		// "Private "
		sb_str(b, TR("Private"));
		sb_chr(b, ' ');
	} else {			// an emergency's "TG 101 " / "> 1234 "
		sb_str(b, r->group ? "TG " : "> ");
		sb_u32(b, r->dst, 1);
		sb_chr(b, ' ');
	}
}

static void p25_recent_alt(sbuf_t *b, const ui_recent_t *r, unsigned what, size_t len, size_t after)
{
	size_t room = (size_t)(b->end - b->p);
	unsigned n = room >= 2u * len + 4u + after ? 2u : room >= len + 3u + after ? 1u : 0u;
	if (n == 0)
		return;
	sb_str(b, TEXT_ALT_OPEN);
	for (unsigned k = 0; k < n; k++) {
		p25_recent_part(b, r, what);
		sb_str(b, TEXT_ALT_NEXT);
	}
	sb_str(b, TEXT_ALT_CLOSE);
}

static void p25_recent_item(const ui_recent_t *r, ui_item_t *it)
{
	sbuf_t lb, vb;
	sb_init(&lb, it->label, sizeof it->label);
	sb_init(&vb, it->value, sizeof it->value);
	sb_str(&lb, TEXT_ALT_OPEN "P25 " TEXT_ALT_NEXT TEXT_ALT_CLOSE);
	if (r->src != 0)
		sb_u32(&lb, r->src, 1);
	else
		sb_chr(&lb, '?');
	if (r->emerg) {
		sb_str(&vb, TR("EMERGENCY"));
		sb_chr(&vb, ' ');
	}
	if (r->enc) {
		sb_str(&vb, TR("Encrypted"));
		sb_chr(&vb, ' ');
	}
	if (r->emerg || r->enc)
		it->flags |= UI_ITEM_WARN;
#if CONFIG_NAC_DEC
	char age_text[24];	// the age as sb_age writes it: the room it really takes (the NAC's two forms need every byte)
	sbuf_t ab;
	sb_init(&ab, age_text, sizeof age_text);
	sb_age(&ab, ui_st.now_ms - r->ms);
	size_t age = strlen(age_text);
#else
	size_t age = 9u + strlen(TR(" ago"));	// sb_age: two alternates of up to 2 digits and a unit (49 days at most)
#endif
	size_t unit = (r->group ? 3u : 2u) + p25_recent_digits(r->dst);	// "TG 101" / "> 1234"
	if (r->emerg) {
		p25_recent_alt(&vb, r, 2, unit + 1u, age);	// with its own space before the age
	} else if (r->group) {
		sb_str(&vb, "TG ");
		sb_u32(&vb, r->dst, 1);
#if CONFIG_NAC_DEC
		p25_recent_alt(&vb, r, 0, 6u + p25_recent_digits(r->nac), 1u + age);	// " $125/293"
#else
		p25_recent_alt(&vb, r, 0, 8, 1u + age);
#endif
		sb_chr(&vb, ' ');
	} else {			// unit to unit: the called unit
		p25_recent_alt(&vb, r, 1, strlen(TR("Private")) + 1u, unit + 1u + age);
		sb_str(&vb, "> ");
		sb_u32(&vb, r->dst, 1);
		sb_chr(&vb, ' ');
	}
	sb_age(&vb, ui_st.now_ms - r->ms);
}

#endif
static void recent_item(uint16_t i, ui_item_t *it)
{
	const ui_recent_t *r = ui_recent_get((uint8_t)i);
	if (r == NULL)
		return;
#if CONFIG_CALL_LOG
#if CONFIG_P25_EXTRAS
	if (r->mode == RADIO_MODE_P25) {
		p25_recent_item(r, it);
		return;
	}
#endif
'''),
    ])
    # Home: while a P25 emergency call comes in, the activity line's second row says so in the warning colour (the
    # caller line under it says EMERGENCY too: a word with every colour, P5)
    transform('core/ui/ui_screens.c', [
        ('\t\tif (two)\n\t\t\tgfx_text(sm, 0, y2, v->monitor ? TR("Squelch open") : TR("Receiving"), ui_col.rx);\n',
         '#if CONFIG_P25_EXTRAS\n'
         '\t\tif (two && v->state == RS_RX && codeplug_mode_p25(v->ch.mode) && !codeplug_mode_ysf(v->ch.mode) &&\n'
         '\t\t    (p25_lab_p25_kind(NULL) & P25_LAB_EMERGENCY)) {\n'
         '\t\t\tgfx_text(sm, 0, y2, TR("EMERGENCY"), ui_col.warn);\t// DM32 Plus: a P25 emergency call\n'
         '\t\t\tbreak;\n'
         '\t\t}\n'
         '#endif\n'
         '\t\tif (two)\n\t\t\tgfx_text(sm, 0, y2, v->monitor ? TR("Squelch open") : TR("Receiving"), ui_col.rx);\n'),
    ])


def prepare_p25_mode2(transform):
    """P25 channels of CH_MODE 2 (CONFIG_P25_MODE2, docs/plus.md "P25 channels of CH_MODE 2"; read compatibility):
    records written in the DM32-nxdn-p25 multi-mode derivative's format (its web CPS / firmware; mode 2 = P25,
    research/ref/DM32-nxdn-p25 patches 0001 and 0004, its open-firmware-cps.md 7.3.3.1) read as Plus P25 channels:
    RX NAC from the RX tone word (a 12-bit number, shown in hex: "293" is $293 = 659), talkgroup from the contact
    word, unit ID from 0x27-0x29 (LFLAG1 bit 7). One NAC in Plus: a TX NAC unlike the RX one, "hear every
    talkgroup" (CH_FLAG2 bit 6, read as talkgroup Any), no talkgroup or no unit ID make the channel RX only, so Plus
    never sends with a NAC, talkgroup or ID the record did not hold. Nothing is written: saving the channel (editor
    Save, saving temporary changes) stores it as a Plus P25 record (0xA5), as codeplug_openfw_encode always does."""
    transform('config/features.mk', [
        ('CONFIG_NAC_DEC ?= n\nFEATURES += NAC_DEC\n',
         'CONFIG_NAC_DEC ?= n\nFEATURES += NAC_DEC\n'
         'CONFIG_P25_MODE2 ?= n\nFEATURES += P25_MODE2\n'
         'ifeq ($(CONFIG_P25_MODE2),y)\nifneq ($(CONFIG_P25_CHANNELS),y)\n$(error P25_MODE2 needs P25_CHANNELS=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_NAC_DEC\n', '#ifndef CONFIG_P25_MODE2\n#define CONFIG_P25_MODE2 0\n#endif\n#ifndef CONFIG_NAC_DEC\n'),
    ])
    transform('core/codeplug/codeplug.c', [
        ('\t    !(CONFIG_NXDN && r[CH_MODE] == 0xA7u)) return false;\n',
         '\t    !(CONFIG_NXDN && r[CH_MODE] == 0xA7u) && !(CONFIG_P25_MODE2 && r[CH_MODE] == 2u)) return false;\n'),
        ('bool codeplug_openfw_decode(const uint8_t *r, channel_t *out)\n{',
         '''#if CONFIG_P25_MODE2
// DM32 Plus: a P25 channel in the DM32-nxdn-p25 multi-mode derivative's record (CH_MODE 2, its open-firmware-cps.md
// 7.3.3.1): the tone words hold the RX and TX NAC, the contact word the talkgroup, CH_DMR_ID with LFLAG1_ID the unit
// ID, CH_FLAG2 bit 6 "hear every talkgroup"; CH_RX_GROUP and CH_CC are unused there. out holds the record's common
// fields already. On Classic r13's model: a TX NAC unlike the RX one is the channel's TX NAC, "hear every talkgroup"
// is the Normal squelch (every group on the RX NAC; the talkgroup stays the TX one); no talkgroup or no unit ID reads
// as RX only, so Plus never sends with a talkgroup or ID the record did not hold
static bool p25_mode2(const uint8_t *r, channel_t *out)
{
	uint16_t rx = le16(&r[CH_RX_TONE]), tx = le16(&r[CH_TX_TONE]);
	if (rx > 0xFFFu || rx == 0xF7Fu)
		rx = 0xF7Eu;		// the derivative reads these as every NAC (an FM CPS's 0xFFFF too)
	if (tx >= 0xF7Eu)
		tx = 0x293u;		// ... and these TX NACs as its default
	out->mode = RADIO_MODE_P25;
	out->p25_nac = rx;
	out->p25_tx_nac_override = tx != rx ? (uint16_t)(tx + 1u) : 0u;	// 0: Same as RX
	out->p25_tg = out->contact;
	out->p25_id = out->dmr_id;
	out->contact = out->listen_list = 0;
	out->p25_squelch = (r[CH_FLAG2] & FLAG2_TS2) ? 0 : 1;	// hear every talkgroup: Normal, else Selective
	if (out->p25_id > 0xFFFFFEu)
		out->p25_id = out->dmr_id = 0;
	if (out->p25_tg == 0 || out->p25_id == 0)
		out->rx_only = true;
	out->rx_tone = out->tx_tone = (subtone_t){ TONE_NONE, 0 };
	out->color_code = 0;
	out->timeslot = 1;
	out->bw = RADIO_BW_12K5;
	out->dmo = out->roam = out->vox = false;
	out->sfr_flag1 = out->sfr_a = out->sfr_b = 0;
	return codeplug_p25_valid(out);
}

#endif
bool codeplug_openfw_decode(const uint8_t *r, channel_t *out)
{'''),
        ('\tif (CONFIG_P25_MODE2 && r[CH_MODE] == 0xA5u) {\n',
         '#if CONFIG_P25_MODE2\n'
         '\tif (r[CH_MODE] == 2u)\t// DM32 Plus: the multi-mode derivative\'s P25 record\n'
         '\t\treturn p25_mode2(r, out);\n'
         '#endif\n'
         '\tif (CONFIG_P25_MODE2 && r[CH_MODE] == 0xA5u) {\n'),
    ])


def prepare_fm_decoders(transform, install):
    """Tone finder (CONFIG_TONE_FIND) and DTMF / SelCall decoder (CONFIG_SELCALL), docs/plus.md: Tools > Tone finder
    names the CTCSS tone or DCS code of the FM signal received (software: radio/tonefind.c on the flat receive path)
    and writes it into the channel (the channel editor's ui_svc_write_channel) or the VFO (radio_req_tone); Tools >
    DTMF / SelCall lists the last six DTMF strings and ZVEI1 / ZVEI2 / CCIR / EIA 5-tone sequences heard. Both run
    only while their screen is open, through the pager's code (radio_msg.c RADIO_MSG_TONEF / _SELCALL: FM caller ID's
    audio, the decoder from the heap). The DM32 board reads the FD6818's 0x68-0x6A for the tone finder's screen
    (dm32_css.inc, experimental, read only; included by tools/build_radiofoundry.py). Receive only. Runs after
    prepare_sstv (it anchors on its insertions) and last in the list (the Tools entries go before Battery)."""
    for f in ('tonefind.c', 'tonefind.h', 'selcall.c', 'selcall.h', 'radio_fmdec.c', 'radio_fmdec.h'):
        install('core/radio/' + f, 'integration/radiofoundry/fmdec/' + f)
    install('core/ui/ui_fmdec.c', 'integration/radiofoundry/fmdec/ui_fmdec.c')
    install('boards/dm32/dm32_css.inc', 'integration/radiofoundry/fmdec/dm32_css.inc')
    transform('config/features.mk', [
        ('ifeq ($(CONFIG_SSTV),y)\nifneq ($(CONFIG_MSG_RX),y)\n$(error SSTV needs MSG_RX=y)\nendif\nendif\n',
         'ifeq ($(CONFIG_SSTV),y)\nifneq ($(CONFIG_MSG_RX),y)\n$(error SSTV needs MSG_RX=y)\nendif\nendif\n'
         'CONFIG_TONE_FIND ?= n\nFEATURES += TONE_FIND\n'
         'ifeq ($(CONFIG_TONE_FIND),y)\nifneq ($(CONFIG_MSG_RX)$(CONFIG_NXDN),yy)\n'
         '$(error TONE_FIND needs MSG_RX=y and NXDN=y (the flat receive path))\nendif\nendif\n'
         'CONFIG_SELCALL ?= n\nFEATURES += SELCALL\n'
         'ifeq ($(CONFIG_SELCALL),y)\nifneq ($(CONFIG_MSG_RX),y)\n$(error SELCALL needs MSG_RX=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_SSTV\n', '#ifndef CONFIG_TONE_FIND\n#define CONFIG_TONE_FIND 0\n#endif\n'
         '#ifndef CONFIG_SELCALL\n#define CONFIG_SELCALL 0\n#endif\n#ifndef CONFIG_SSTV\n'),
    ])
    transform('core/ui/ui_nav.h', [
        ('\tSCR_COUNT\n} ui_scr_t;',
         '\tSCR_TONEF, SCR_TONEF_WRITE,\t// DM32 Plus: Tools > Tone finder, its Write tone (CONFIG_TONE_FIND)\n'
         '\tSCR_SELCALL,\t\t\t// DM32 Plus: Tools > DTMF / SelCall (CONFIG_SELCALL)\n'
         '\tSCR_COUNT\n} ui_scr_t;'),
        ('bool ui_sstv_tick(void);\n#endif\n',
         'bool ui_sstv_tick(void);\n#endif\n'
         '#if CONFIG_TONE_FIND || CONFIG_SELCALL\n'
         '// DM32 Plus tone finder, DTMF / SelCall (ui_fmdec.c), from ui_msg_tick: the radio_msg kind a screen listens for\n'
         '// (0: not theirs); true: a new result, draw again\n'
         'uint8_t ui_fmdec_kind(uint8_t scr);\nbool ui_fmdec_tick(void);\n#endif\n'),
    ])
    # Tools > Tone finder and DTMF / SelCall: the last tools (before Battery and the zone offer)
    transform('core/ui/ui_menus.c', [
        ('extern const ui_scr_def_t ui_sstv_screen;\t// ui_sstv.c (DM32 Plus)',
         'extern const ui_scr_def_t ui_sstv_screen;\t// ui_sstv.c (DM32 Plus)\n'
         'extern const ui_scr_def_t ui_tonef_screen, ui_tonef_write_screen, ui_selcall_screen;\t// ui_fmdec.c (DM32 Plus)'),
        ('#if CONFIG_SSTV\n\t[SCR_SSTV] = &ui_sstv_screen,\n#endif\n',
         '#if CONFIG_SSTV\n\t[SCR_SSTV] = &ui_sstv_screen,\n#endif\n'
         '#if CONFIG_TONE_FIND\n\t[SCR_TONEF] = &ui_tonef_screen,\n\t[SCR_TONEF_WRITE] = &ui_tonef_write_screen,\n#endif\n'
         '#if CONFIG_SELCALL\n\t[SCR_SELCALL] = &ui_selcall_screen,\n#endif\n'),
        ('T_BATTERY, T_ZONE_OFFER, T_COUNT };', 'T_TONEF, T_SELCALL, T_BATTERY, T_ZONE_OFFER, T_COUNT };'),
        ('(i != T_BATTERY || board_volts()',
         '(i != T_TONEF || CONFIG_TONE_FIND) && (i != T_SELCALL || CONFIG_SELCALL) && (i != T_BATTERY || board_volts()'),
        ('TRN("Battery"), TRN("Zones/Chs Received") };',
         'TRN("Tone finder"), TRN("DTMF / SelCall"), TRN("Battery"), TRN("Zones/Chs Received") };'),
        ('\tcase T_ZONE_OFFER: ui_push(SCR_OFFER, 0); break;\n',
         '#if CONFIG_TONE_FIND\n\tcase T_TONEF: ui_push(SCR_TONEF, 0); break;\n#endif\n'
         '#if CONFIG_SELCALL\n\tcase T_SELCALL: ui_push(SCR_SELCALL, 0); break;\n#endif\n'
         '\tcase T_ZONE_OFFER: ui_push(SCR_OFFER, 0); break;\n'),
        ('\t\t[T_ZONE_OFFER] = ART_ICON_ZONE_OFFER + 1,\n',
         '\t\t[T_TONEF] = ART_ICON_SPECTRUM + 1, [T_SELCALL] = ART_ICON_RECENT + 1,\t// a tone; a list of calls\n'
         '\t\t[T_ZONE_OFFER] = ART_ICON_ZONE_OFFER + 1,\n'),
    ])


def prepare_scanx(transform, install):
    """Priority channel and temporary skip (CONFIG_SCAN_PRIO, docs/plus.md "Priority channel and temporary skip";
    owner's round 3 items 2 and 3). Upstream has both halves already, and Plus completes them instead of adding a
    parallel scheme: its priority (scan_cfg_t.priority, the debug link's set.scan_priority) revisits the zone scan's
    start channel after every 2 others; Plus names the channel (Settings > Talk & listen > Priority channel, # 5 while
    scanning, the zone editor's channel actions), stored in settings_t.unused_dmr_id (an ignored old field: the
    record's layout stays upstream's), looked at every 2 s in a zone scan and in dual watch / multi-watch, between
    overs too (integration/radiofoundry/scanx/radio_scanprio.inc). Its nuisance delete (scan_t.skip, cleared when the
    scan ends) gets 0 on Home as its one key while a scan has stopped, and # 4 Clear skips (RSCAN_UNSKIP)."""
    install('core/radio/scanprio.h', 'integration/radiofoundry/scanx/scanprio.h')
    install('core/radio/radio_scanprio.inc', 'integration/radiofoundry/scanx/radio_scanprio.inc')
    install('core/ui/ui_scanx.c', 'integration/radiofoundry/scanx/ui_scanx.c')
    transform('config/features.mk', [
        ('$(error P25_MODE2 needs P25_CHANNELS=y)\nendif\nendif\n',
         '$(error P25_MODE2 needs P25_CHANNELS=y)\nendif\nendif\n'
         'CONFIG_SCAN_PRIO ?= n\nFEATURES += SCAN_PRIO\n'
         'ifeq ($(CONFIG_SCAN_PRIO),y)\nifneq ($(CONFIG_SCAN),y)\n$(error SCAN_PRIO needs SCAN=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_P25_MODE2\n', '#ifndef CONFIG_SCAN_PRIO\n#define CONFIG_SCAN_PRIO 0\n#endif\n#ifndef CONFIG_P25_MODE2\n'),
    ])

    # ---- The radio core: the priority channel's stops and looks, nuisance delete on it, Clear skips ----
    transform('core/radio/radio.h', [
        ('RSCAN_NUISANCE, RSCAN_TOGGLE } radio_scan_cmd_t;',
         'RSCAN_NUISANCE, RSCAN_TOGGLE,\n'
         '\tRSCAN_UNSKIP\t// DM32 Plus (CONFIG_SCAN_PRIO): every temporary skip of this scan undone\n} radio_scan_cmd_t;'),
    ])
    transform('core/radio/radio_sm.c', [
        ('// --- Scan (radio/scan.c decides; this tunes) ---\n#if CONFIG_SCAN\n',
         '// --- Scan (radio/scan.c decides; this tunes) ---\n#if CONFIG_SCAN\n'
         '#if CONFIG_SCAN_PRIO\n'
         '// DM32 Plus priority channel and temporary skip (radio_scanprio.inc, included before scan_service)\n'
         'static void scx_config(const radio_sm_t *sm, uint8_t kind, scan_cfg_t *cfg);\n'
         'static int scx_step(radio_sm_t *sm, uint32_t now_ms);\n'
         'static bool scx_stop(radio_sm_t *sm, bool cancel, uint32_t now_ms);\n'
         '#define SCX_OWNS(sm)\tscx_owns(sm)\n'
         '#else\n#define SCX_OWNS(sm)\tfalse\n#endif\n'),
        ('static bool scan_step(radio_sm_t *sm, uint32_t now_ms)\n{\n\tscan_t *sc = &sm->scan;\n',
         'static bool scan_step(radio_sm_t *sm, uint32_t now_ms)\n{\n\tscan_t *sc = &sm->scan;\n'
         '#if CONFIG_SCAN_PRIO\n'
         '\tint prio = scx_step(sm, now_ms);\t// DM32 Plus: the priority channel\'s stop, or back from a look\n'
         '\tif (prio >= 0) {\n\t\treturn prio != 0;\n\t}\n'
         '#endif\n'),
        ('\tcfg->priority = sm->set.scan_priority;\n',
         '\tcfg->priority = sm->set.scan_priority;\n'
         '#if CONFIG_SCAN_PRIO\n\tscx_config(sm, kind, cfg);\t// DM32 Plus: a priority channel set replaces the start channel\'s revisits\n#endif\n'),
        ('\tuint8_t kind = sc->kind;\n\tif (kind == SCAN_DUAL) {\n\t\trx2_release(sm);\n\t\tdual_busy_set(sm, 0);\n\t}\n'
         '\tscan_end(sc);\n',
         '\tuint8_t kind = sc->kind;\n\tif (kind == SCAN_DUAL) {\n\t\trx2_release(sm);\n\t\tdual_busy_set(sm, 0);\n\t}\n'
         '#if CONFIG_SCAN_PRIO\n\tcancel = scx_stop(sm, cancel, now_ms);\t// DM32 Plus: out of a look, or of another zone\n#endif\n'
         '\tscan_end(sc);\n'),
        ('// Every poll while scanning: pause on the monitor, what the receiver hears, move on\n'
         'static void scan_service(radio_sm_t *sm, const radio_status_t *st, uint32_t now_ms)\n{\n\tscan_t *sc = &sm->scan;\n',
         '#if CONFIG_SCAN_PRIO\n#include "radio/radio_scanprio.inc"\t// DM32 Plus: the priority channel, Clear skips\n#endif\n\n'
         '// Every poll while scanning: pause on the monitor, what the receiver hears, move on\n'
         'static void scan_service(radio_sm_t *sm, const radio_status_t *st, uint32_t now_ms)\n{\n\tscan_t *sc = &sm->scan;\n'
         '#if CONFIG_SCAN_PRIO\n'
         '\tif (scx_service(sm, now_ms)) {\t// DM32 Plus: a look at the priority channel in the wait starts\n'
         '\t\tscan_publish(sm);\n\t\treturn;\n\t}\n'
         '#endif\n'),
        ('\tif (sc->kind == SCAN_DUAL && dual_wait_service(sm, st, now_ms)) {\n',
         '\tif (sc->kind == SCAN_DUAL && !SCX_OWNS(sm) && dual_wait_service(sm, st, now_ms)) {\n'),
        ('\tcase RSCAN_NUISANCE:\n\t\tif (scan_on(&sm->scan) && !dual_running(sm)) {',
         '#if CONFIG_SCAN_PRIO\n\tcase RSCAN_UNSKIP:\t// DM32 Plus\n\t\tscx_unskip(sm);\n\t\tbreak;\n#endif\n'
         '\tcase RSCAN_NUISANCE:\n'
         '#if CONFIG_SCAN_PRIO\n\t\tif (scx_nuisance(sm, now_ms)) {\t// DM32 Plus: the priority channel\n\t\t\tbreak;\n\t\t}\n#endif\n'
         '\t\tif (scan_on(&sm->scan) && !dual_running(sm)) {'),
        ('\tsettings_read(&st);\t// a zone add or delete a power cut interrupted is settled first (radio_core_start)\n',
         '\tsettings_read(&st);\t// a zone add or delete a power cut interrupted is settled first (radio_core_start)\n'
         '#if CONFIG_SCAN_PRIO\n\tradio_scan_prio_set(st.unused_dmr_id);\t// DM32 Plus: the priority channel (radio/scanprio.h)\n#endif\n'),
    ])

    # ---- The UI's services: the setting (settings_t.unused_dmr_id), Clear skips ----
    transform('core/ui/ui_svc.h', [
        ('bool ui_svc_scan_skip(void);\t\t\t// nuisance delete of the channel that stopped the scan\n',
         'bool ui_svc_scan_skip(void);\t\t\t// nuisance delete of the channel that stopped the scan\n'
         '#if CONFIG_SCAN_PRIO\nbool ui_svc_scan_unskip(void);\t\t// DM32 Plus: every temporary skip of this scan undone\n#endif\n'),
        ('\tUI_SET_COUNT\n} ui_setting_t;',
         '#if CONFIG_SCAN_PRIO\n'
         '\tUI_SET_SCAN_PRIO,\t// DM32 Plus: the priority channel, radio/scanprio.h\'s word, 0 Off (settings_t.unused_dmr_id)\n'
         '#endif\n\tUI_SET_COUNT\n} ui_setting_t;'),
    ])
    transform('core/ui/ui_svc_stub.c', [
        ('WEAK bool ui_svc_scan_skip(void) { return false; }\n',
         'WEAK bool ui_svc_scan_skip(void) { return false; }\n'
         '#if CONFIG_SCAN_PRIO\nWEAK bool ui_svc_scan_unskip(void) { return false; }\n#endif\n'),
    ])
    transform('core/ui/ui_svc_radio.c', [
        ('#include "radio/radio_bcast.h"\n', '#include "radio/radio_bcast.h"\n#include "radio/scanprio.h"\n'),
        ('bool ui_svc_scan_skip(void)\n{\n\tradio_req_scan(RSCAN_NUISANCE);\n\treturn true;\n}\n',
         'bool ui_svc_scan_skip(void)\n{\n\tradio_req_scan(RSCAN_NUISANCE);\n\treturn true;\n}\n'
         '#if CONFIG_SCAN_PRIO\nbool ui_svc_scan_unskip(void)\n{\n\tradio_req_scan(RSCAN_UNSKIP);\n\treturn true;\n}\n#endif\n'),
        ('\tcase UI_SET_GPS:\t\t// the module\'s power and the position sources, now (not a radio core setting)\n',
         '#if CONFIG_SCAN_PRIO\n'
         '\tcase UI_SET_SCAN_PRIO:\t\t// DM32 Plus: the radio core\'s copy of the priority channel\n'
         '\t\tradio_scan_prio_set((uint32_t)value);\n\t\treturn;\n'
         '#endif\n'
         '\tcase UI_SET_GPS:\t\t// the module\'s power and the position sources, now (not a radio core setting)\n'),
    ])
    transform('core/ui/ui_svc_storage.c', [
        ('#include "radio/ref_trim.h"\n', '#include "radio/ref_trim.h"\n#include "radio/scanprio.h"\n'),
        ('\tcase UI_SET_VFO_STEP:\n\t\tsettings_read(&st);\n',
         '#if CONFIG_SCAN_PRIO\n'
         '\tcase UI_SET_SCAN_PRIO:\t// DM32 Plus: anything but a tagged channel (an old own-ID override, 0) is Off\n'
         '\t\tsettings_read(&st);\n'
         '\t\treturn scanprio_number(st.unused_dmr_id) != 0 ? (int32_t)st.unused_dmr_id : 0;\n'
         '#endif\n'
         '\tcase UI_SET_VFO_STEP:\n\t\tsettings_read(&st);\n'),
        ('\tcase UI_SET_VFO_STEP:\n\t\treturn true;',
         '#if CONFIG_SCAN_PRIO\n'
         '\tcase UI_SET_SCAN_PRIO:\t// DM32 Plus: the priority channel in an ignored old field (radio/scanprio.h)\n'
         '\t\tif (value != 0 && scanprio_number((uint32_t)value) == 0)\n\t\t\treturn false;\n'
         '\t\tst = settings_begin();\n'
         '\t\t{\t// the word is shared with VFO A\'s DMR Any flags (bits 11-13, vfox): keep them\n'
         '\t\t\tuint32_t keep = (st->unused_dmr_id >> 24) == SCANPRIO_TAG ? st->unused_dmr_id & SCANPRIO_VFOX_BITS : 0u;\n'
         '\t\t\tuint32_t w = (uint32_t)value & ~SCANPRIO_VFOX_BITS;\n'
         '\t\t\tif (w == 0u && keep != 0u)\n\t\t\t\tw = (uint32_t)SCANPRIO_TAG << 24;\n'
         '\t\t\tst->unused_dmr_id = w | keep;\n'
         '\t\t}\n'
         '\t\tsettings_end(hal_millis());\n'
         '\t\treturn true;\n'
         '#endif\n'
         '\tcase UI_SET_VFO_STEP:\n\t\treturn true;'),
    ])

    # ---- The UI: 0 skips, # 4 / 5, the setting, the zone editor's action, the marks ----
    transform('core/ui/ui_nav.h', [
        ('void item_num_set(ui_item_t *it, uint16_t n);\n',
         'void item_num_set(ui_item_t *it, uint16_t n);\n'
         '#if CONFIG_SCAN_PRIO\n'
         '// DM32 Plus priority channel and temporary skip (ui_scanx.c)\n'
         'void ui_scx_apply(int32_t v);\t\t\t// UI_SET_SCAN_PRIO changed\n'
         'bool ui_scx_set(uint16_t number, uint16_t zone, const char *name);\t// 0: Off; with a toast\n'
         'void ui_scx_setting_value(char *out, uint8_t cap);\t// Settings > Priority channel\n'
         'void ui_scx_setting_select(void);\n'
         'bool ui_scx_home_key(const input_event_t *ev);\t// 0 while a scan has stopped: skip it\n'
         'void ui_scx_menu_item(uint16_t i, ui_item_t *it);\t// # while scanning, 4 and 5\n'
         'void ui_scx_menu_select(uint16_t i);\n'
         '#endif\n'),
    ])
    transform('core/ui/ui_screens.h', [
        ('const char *ui_power_name(uint8_t level);\n\n#endif',
         'const char *ui_power_name(uint8_t level);\n'
         '#if CONFIG_SCAN_PRIO\n'
         '// DM32 Plus priority channel (ui_scanx.c): its number (0 Off), the view on it, the strip\'s scan word with " P"\n'
         'uint16_t ui_scx_channel(void);\n'
         'bool ui_scx_marked(const radio_view_t *v);\n'
         'const char *ui_scx_word(const char *word);\n'
         '#define SCX_WORD(w)\tui_scx_word(w)\n'
         '#else\n#define SCX_WORD(w)\t(w)\n#endif\n'
         '\n#endif'),
    ])
    transform('core/ui/ui_app.c', [
        ('\tif (ui_dual_home_key(ev))\n\t\treturn true;\n#endif\n\tbool press = ev->gesture == GESTURE_PRESS;\n',
         '\tif (ui_dual_home_key(ev))\n\t\treturn true;\n#endif\n'
         '#if CONFIG_SCAN_PRIO\n\tif (ui_scx_home_key(ev))\t// DM32 Plus: 0 skips the channel a scan stopped on\n\t\treturn true;\n#endif\n'
         '\tbool press = ev->gesture == GESTURE_PRESS;\n'),
        ('\tcase UI_SET_VIBRATE: ui_st.vibrate = v != 0; break;\n',
         '\tcase UI_SET_VIBRATE: ui_st.vibrate = v != 0; break;\n'
         '#if CONFIG_SCAN_PRIO\n\tcase UI_SET_SCAN_PRIO: ui_scx_apply(v); break;\t// DM32 Plus\n#endif\n'),
        ('\t\t\t\t\t  UI_SET_STRIP, UI_SET_VIBRATE, UI_SET_VOLUME, UI_SET_LANGUAGE,\n',
         '\t\t\t\t\t  UI_SET_STRIP, UI_SET_VIBRATE, UI_SET_VOLUME, UI_SET_LANGUAGE,\n'
         '#if CONFIG_SCAN_PRIO\n\t\t\t\t\t  UI_SET_SCAN_PRIO,\n#endif\n'),
    ])
    transform('core/ui/ui_menus.c', [
        # # while scanning: upstream's three, then 4 Clear skips, 5 Set as priority (ui_scanx.c)
        ('static uint16_t scan_count(void) { return 3; }',
         'static uint16_t scan_count(void) { return CONFIG_SCAN_PRIO ? 5 : 3; }\t// DM32 Plus: 4, 5 in ui_scanx.c'),
        ('\tstatic const char *const names[] = { TRN("Skip channel for now"), TRN("Stay here"), TRN("Always skip") };\n',
         '#if CONFIG_SCAN_PRIO\n\tif (i >= 3) {\n\t\tui_scx_menu_item(i, it);\n\t\treturn;\n\t}\n#endif\n'
         '\tstatic const char *const names[] = { TRN("Skip channel for now"), TRN("Stay here"), TRN("Always skip") };\n'),
        ('static void scan_select(uint16_t i, bool hold)\n{\n\t(void)hold;\n',
         'static void scan_select(uint16_t i, bool hold)\n{\n\t(void)hold;\n'
         '#if CONFIG_SCAN_PRIO\n\tif (i >= 3) {\n\t\tui_scx_menu_select(i);\n\t\treturn;\n\t}\n#endif\n'),
        ('.prefix = TRN("Scan"), .hint = TRN("1-3 pick  Back"),',
         '.prefix = TRN("Scan"),\n\t.hint = CONFIG_SCAN_PRIO ? TRN("1-5 pick  Back") : TRN("1-3 pick  Back"),'),
        # Settings > Talk & listen > Priority channel, the last row
        ('enum { SA_DIAG, SA_FIRMWARE, SA_CODEPLUG, SA_RESET, SA_MY_RADIO, SA_CRASH, SA_LANGUAGE };',
         'enum { SA_DIAG, SA_FIRMWARE, SA_CODEPLUG, SA_RESET, SA_MY_RADIO, SA_CRASH, SA_LANGUAGE, SA_SCAN_PRIO };'),
        ('\tLIST(TRN("SFR CW ID"), "single frequency repeater morse callsign identify", UI_SET_SFR_CWID, v_sfrcwid, n_sfrcwid),\n'
         '#endif\n};\n',
         '\tLIST(TRN("SFR CW ID"), "single frequency repeater morse callsign identify", UI_SET_SFR_CWID, v_sfrcwid, n_sfrcwid),\n'
         '#endif\n'
         '#if CONFIG_SCAN_PRIO\n'
         '\tACTION(TRN("Priority channel"), "scan dual watch priority look back", SA_SCAN_PRIO),\t// DM32 Plus\n'
         '#endif\n};\n'),
        ('\telse if (s->id == SA_LANGUAGE)\n\t\tstr_copy(it->value, sizeof it->value, lang_name(lang_current()));\n',
         '\telse if (s->id == SA_LANGUAGE)\n\t\tstr_copy(it->value, sizeof it->value, lang_name(lang_current()));\n'
         '#if CONFIG_SCAN_PRIO\n\telse if (s->id == SA_SCAN_PRIO)\n\t\tui_scx_setting_value(it->value, sizeof it->value);\n#endif\n'),
        ('\t\tcase SA_LANGUAGE: ui_push(SCR_LANGUAGE, 0); break;\n',
         '\t\tcase SA_LANGUAGE: ui_push(SCR_LANGUAGE, 0); break;\n'
         '#if CONFIG_SCAN_PRIO\n\t\tcase SA_SCAN_PRIO: ui_scx_setting_select(); break;\t// DM32 Plus\n#endif\n'),
        # The zone editor's channel actions: Set as priority after Remove (the numbers before it stay)
        ('\tZC_REMOVE, ZC_COUNT };', '\tZC_REMOVE,\n#if CONFIG_SCAN_PRIO\n\tZC_PRIO,\t// DM32 Plus\n#endif\n\tZC_COUNT };'),
        ('\t\t[ZC_REMOVE] = TRN("Remove from zone") };\n',
         '\t\t[ZC_REMOVE] = TRN("Remove from zone"),\n#if CONFIG_SCAN_PRIO\n\t\t[ZC_PRIO] = TRN("Set as priority"),\n#endif\n\t};\n'),
        ('\tstr_copy(it->label, sizeof it->label, tr(names[i]));\n\titem_num(it, (uint16_t)(i + 1u));\n\tif (i == ZC_REMOVE)\n',
         '\tstr_copy(it->label, sizeof it->label, tr(names[i]));\n'
         '#if CONFIG_SCAN_PRIO\n'
         '\tif (i == ZC_PRIO && ui_frame()->arg < ze_z.count && ze_z.channels[ui_frame()->arg] == ui_scx_channel())\n'
         '\t\tstr_copy(it->label, sizeof it->label, TR("Priority off"));\n'
         '#endif\n'
         '\titem_num(it, (uint16_t)(i + 1u));\n\tif (i == ZC_REMOVE)\n'),
        ('\t} else if (i == ZC_REMOVE) {\n\t\tze_op_new(ZONE_OP_REMOVE, pos);\n',
         '#if CONFIG_SCAN_PRIO\n'
         '\t} else if (i == ZC_PRIO) {\n'
         '\t\tui_pop();\n'
         '\t\tui_scx_set(num == ui_scx_channel() ? 0 : num, ze_zone, zc_name);\n'
         '#endif\n'
         '\t} else if (i == ZC_REMOVE) {\n\t\tze_op_new(ZONE_OP_REMOVE, pos);\n'),
        ('.hint = CONFIG_ZONE_SHARE ? TRN("1-6 pick  Back") : TRN("1-5 pick  Back"),',
         '.hint = CONFIG_ZONE_SHARE ? (CONFIG_SCAN_PRIO ? TRN("1-7 pick  Back") : TRN("1-6 pick  Back")) :\n'
         '\t\t(CONFIG_SCAN_PRIO ? TRN("1-6 pick  Back") : TRN("1-5 pick  Back")),'),
    ])
    transform('core/ui/ui_screens.c', [
        ('\tif (s->dual.on && strip_fits(f, lx, rx, TR("DUAL")))\n\t\tlx = gfx_text(f, lx + f->w, ty, TR("DUAL"), swarn);\n',
         '\tif (s->dual.on && strip_fits(f, lx, rx, SCX_WORD(TR("DUAL"))))\n'
         '\t\tlx = gfx_text(f, lx + f->w, ty, SCX_WORD(TR("DUAL")), swarn);\t// DM32 Plus: "DUAL P" with a priority channel\n'),
        ('\tif (s->scanning && strip_fits(f, lx, rx, TR("SCAN")))\n\t\tlx = gfx_text(f, lx + f->w, ty, TR("SCAN"), swarn);\n',
         '\tif (s->scanning && strip_fits(f, lx, rx, SCX_WORD(TR("SCAN"))))\n'
         '\t\tlx = gfx_text(f, lx + f->w, ty, SCX_WORD(TR("SCAN")), swarn);\n'),
        ('\tsb_u32(&b, v->channel_number, 1);\n\tint x = gfx_text(L->medium, 0, base - L->medium->ascent, buf,',
         '\tsb_u32(&b, v->channel_number, 1);\n'
         '#if CONFIG_SCAN_PRIO\n\tif (ui_scx_marked(v))\n\t\tsb_chr(&b, \'P\');\t// DM32 Plus: the priority channel\n#endif\n'
         '\tint x = gfx_text(L->medium, 0, base - L->medium->ascent, buf,'),
        ('\tif (s->scanning)\n\t\treturn TR("# skip  Back stop scan");\n',
         '\tif (s->scanning)\n'
         '\t\treturn CONFIG_SCAN_PRIO ? TR("0 skip  # more  Back stop") : TR("# skip  Back stop scan");\n'),
    ])


def prepare_ysf_tx(transform, install):
    """YSF Voice FR transmit (CONFIG_YSF_TX, docs/ysf-nxdn-tx.md; default off until the owner approves on-air tests):
    a YSF channel whose record is not RX only transmits YSF VW (IMBE) through the P25 live path (p25_lab.c
    ysf_live_run, src/ysf/ysf_tx.c). The gates are P25's (radio_sm_tx_check: TX inhibit, RX only, the P25 TX bands
    144-148 / 420-450 MHz, battery, busy lockout; native_lab.c: the channel time-out, lease, drain), plus a callsign:
    the radio name must be one (as the CW beacon's), or PTT is refused with TXDENY_NO_CALL."""
    install('core/ysf/ysf_tx.h', 'src/ysf/ysf_tx.h')
    install('core/ysf/ysf_tx.c', 'src/ysf/ysf_tx.c')
    transform('config/features.mk', [
        ('CONFIG_YSF ?= n\nFEATURES += YSF\n', 'CONFIG_YSF ?= n\nFEATURES += YSF\nCONFIG_YSF_TX ?= n\nFEATURES += YSF_TX\n'
         'ifeq ($(CONFIG_YSF_TX),y)\nifneq ($(CONFIG_YSF)$(CONFIG_DEV_TX)$(CONFIG_SFR_CWID),yyy)\n'
         '$(error YSF_TX needs YSF=y, DEV_TX=y and SFR_CWID=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_YSF\n#define CONFIG_YSF 0\n#endif\n',
         '#ifndef CONFIG_YSF\n#define CONFIG_YSF 0\n#endif\n#ifndef CONFIG_YSF_TX\n#define CONFIG_YSF_TX 0\n#endif\n'),
    ])
    # A YSF channel may be a transmitting one (its record's RX only flag) only in a YSF transmit build
    transform('core/codeplug/codeplug.h', [
        ('\t\treturn ch->bw == RADIO_BW_12K5 && ch->rx_only && ch->p25_tg <= 127u;\n',
         '\t\treturn ch->bw == RADIO_BW_12K5 && (ch->rx_only || CONFIG_YSF_TX) && ch->p25_tg <= 127u;\n'),
    ])
    transform('core/codeplug/codeplug.c', [
        ('\t\tout->mode = RADIO_MODE_YSF;\n\t\tout->p25_nac = 0xF7Eu;\n\t\tout->p25_tg = (uint16_t)(r[CH_RX_GROUP] & 0x7Fu);\n'
         '\t\tout->p25_id = 0;\n\t\tout->dmo = out->roam = out->vox = false;\n\t\tout->rx_only = true;\n',
         '\t\tout->mode = RADIO_MODE_YSF;\n\t\tout->p25_nac = 0xF7Eu;\n\t\tout->p25_tg = (uint16_t)(r[CH_RX_GROUP] & 0x7Fu);\n'
         '\t\tout->p25_id = 0;\n\t\tout->dmo = out->roam = out->vox = false;\n'
         '\t\tout->rx_only = out->rx_only || !CONFIG_YSF_TX;\t// DM32 Plus: the record\'s own flag in a YSF transmit build\n'),
    ])
    transform('core/radio/radio.h', [
        ('TXDENY_AM, TXDENY_BCAST, TXDENY_SFR_RPT,\n} tx_deny_t;',
         'TXDENY_AM, TXDENY_BCAST, TXDENY_SFR_RPT,\n'
         '\tTXDENY_NO_CALL,\t// DM32 Plus YSF transmit: the radio name is not a callsign (CONFIG_YSF_TX)\n} tx_deny_t;'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#if CONFIG_CW_BEACON\n#include "radio/cw_beacon.h"\n',
         '#if CONFIG_YSF_TX\n#include "radio/radio_cwid.h"\t// DM32 Plus YSF transmit: the callsign check\n#endif\n'
         '#if CONFIG_CW_BEACON\n#include "radio/cw_beacon.h"\n'),
        ('\tif (codeplug_mode_p25(ch->mode) ? !CONFIG_P25_NATIVE : (ch->mode == RADIO_MODE_DMR ? !CONFIG_DMR : !CONFIG_FM)) {\n',
         '#if CONFIG_YSF\n\tif (codeplug_mode_ysf(ch->mode) && !CONFIG_YSF_TX) {\n'
         '\t\treturn TXDENY_MODE;\t// DM32 Plus: no YSF transmitter in this build\n\t}\n#endif\n'
         '\tif (codeplug_mode_p25(ch->mode) ? !CONFIG_P25_NATIVE : (ch->mode == RADIO_MODE_DMR ? !CONFIG_DMR : !CONFIG_FM)) {\n'),
        ('\tif (sm->batt.level == BATT_CRITICAL) {\n',
         '#if CONFIG_YSF_TX\n\tif (codeplug_mode_ysf(ch->mode)) {\n\t\tchar call[8];\n'
         '\t\tif (radio_cwid_callsign(sm->radio_name, call, sizeof call) == 0)\n'
         '\t\t\treturn TXDENY_NO_CALL;\t// DM32 Plus: YSF sends our callsign in its header\n\t}\n#endif\n'
         '\tif (sm->batt.level == BATT_CRITICAL) {\n'),
    ])
    transform('core/ui/ui_screens.c', [
        ('\tcase TXDENY_SFR_RPT: sb_str(&b, TR("Repeater channel: PTT off")); break;\n',
         '\tcase TXDENY_SFR_RPT: sb_str(&b, TR("Repeater channel: PTT off")); break;\n'
         '#if CONFIG_YSF_TX\n\tcase TXDENY_NO_CALL: sb_str(&b, TR("Radio name must be a callsign")); break;\n#endif\n'),
        ('\tcase TXDENY_SFR_RPT: return TR("Pick a user channel to talk");\n',
         '\tcase TXDENY_SFR_RPT: return TR("Pick a user channel to talk");\n'
         '#if CONFIG_YSF_TX\n\tcase TXDENY_NO_CALL: return TR("Settings > My radio");\n#endif\n'),
    ])


def prepare_misc(transform, install):
    """Round 3 "misc" job (docs/plus.md): Tools > Airfields (CONFIG_AIRFIELDS), Tools > Marine VHF (CONFIG_MARINE), the
    QSO log and its debug link query 0x21 [14] (CONFIG_QSO_LOG), Tools > Battery's history and time left
    (CONFIG_BATT_GRAPH), the PC live panel's spectrum query 0x21 [13] and plus_mode view key (CONFIG_PC_LIVE). The
    hunks are in tools/plus_misc.py (a file of its own, so parallel jobs touch this list only)."""
    try:
        from tools import plus_misc
    except ImportError:
        import plus_misc
    plus_misc.prepare(transform, install)


# DM32 Plus CW practice: the Morse played by the beep generator, sample exact (beep.c, prepare_cw_rx)
CW_BEEP_MORSE = r'''#if CONFIG_CW_RX
// DM32 Plus CW practice (radio/cw_train.h): Morse played by the beep's own segments, a mark (the tone) or a gap
// (silence) at a time, sample exact, each element faded in and out as any beep (2 ms: no clicks). Speaker only: this is
// the key beeps' PCM path, never the transmitter. The radio task sets it (beep_set_morse) just before
// beep_play(BEEP_MORSE); the audio side takes it when it starts the pattern (as req_pattern). Without a beep_set_morse
// BEEP_MORSE plays its table pattern (one dot)
#define MORSE_PLAY_MAX	12u
static char m_text[MORSE_PLAY_MAX];	// radio task
static uint8_t m_req_len;
static uint16_t m_req_hz, m_req_dot;
static uint8_t m_len, m_pos, m_code, m_count;	// audio side: the text's length, the next character, its elements left
static bool m_gap;				// the next segment is the gap after a mark
static uint16_t m_hz, m_dot;
static beep_seg_t m_seg;

void beep_set_morse(const char *text, uint8_t len, uint16_t hz, uint16_t dot_ms)
{
	if (len > MORSE_PLAY_MAX) {
		len = MORSE_PLAY_MAX;
	}
	memcpy(m_text, text, len);
	m_req_len = len;
	m_req_hz = hz;
	m_req_dot = dot_ms;
}

static uint8_t morse_elements(uint8_t code)
{
	uint8_t n = 0;
	while (code > 1u) {
		code >>= 1;
		n++;
	}
	return n;
}

// The pattern starts: the request taken, its segments counted (marks, the gaps inside characters, between them)
static void morse_load(void)
{
	m_count = 0;
	if (cb_pattern != BEEP_MORSE) {
		return;
	}
	m_len = m_req_len;
	m_hz = m_req_hz;
	m_dot = m_req_dot;
	m_pos = 0;
	m_code = 0;
	m_gap = false;
	uint16_t n = 0;
	for (uint8_t i = 0; i < m_len; i++) {
		uint8_t e = morse_elements(morse_code(m_text[i]));
		if (e != 0) {
			n = (uint16_t)(n + (n != 0 ? 1u : 0u) + 2u * e - 1u);
		}
	}
	m_count = (uint8_t)(n > 255u ? 255u : n);
}

// The next segment
static const beep_seg_t *morse_next(void)
{
	if (m_gap) {
		m_gap = false;
		if (m_code > 1u) {
			m_seg.hz = 0;
			m_seg.ms = m_dot;		// inside a character
			return &m_seg;
		}
		bool space = false;
		while (m_pos < m_len && morse_code(m_text[m_pos]) == 0) {
			space = space || m_text[m_pos] == ' ';
			m_pos++;
		}
		m_seg.hz = 0;
		m_seg.ms = (uint16_t)((space ? 7u : 3u) * m_dot);	// between characters, or words
		return &m_seg;
	}
	if (m_code <= 1u) {
		while (m_pos < m_len && morse_code(m_text[m_pos]) == 0) {
			m_pos++;
		}
		m_code = m_pos < m_len ? morse_code(m_text[m_pos++]) : 0;
	}
	uint8_t marker = 0x80;
	while (marker != 0 && (m_code & marker) == 0) {
		marker >>= 1;
	}
	uint8_t next = (uint8_t)(marker >> 1);
	bool dash = (m_code & next) != 0;
	m_code = (uint8_t)((m_code & (next - 1u)) | next);	// the marker moves down past the element
	m_seg.hz = m_hz;
	m_seg.ms = (uint16_t)((dash ? 3u : 1u) * m_dot);
	m_gap = true;
	return &m_seg;
}

bool beep_morse_playing(void)
{
	return (cb_active && cb_pattern == BEEP_MORSE) || (LOAD(req_seq) != cb_seq && req_pattern == BEEP_MORSE);
}
#endif

// The segment cb_seg of the pattern playing, and their count
static const beep_seg_t *seg_at(void)
{
#if CONFIG_CW_RX
	if (cb_pattern == BEEP_MORSE && m_count != 0) {
		return morse_next();
	}
#endif
	return &patterns[cb_pattern].segs[cb_seg];
}

static uint8_t seg_count(void)
{
#if CONFIG_CW_RX
	if (cb_pattern == BEEP_MORSE && m_count != 0) {
		return m_count;
	}
#endif
	return patterns[cb_pattern].count;
}

'''

CW_PRACTICE_HOOK = r'''#if CONFIG_CW_RX
	{
		// DM32 Plus CW practice (radio/cw_train.h): what the practice page asked for, played by the beep generator
		// to the speaker only. Nothing here keys the transmitter (no tx(), no PTT, no tx_tone()); while the radio
		// transmits or is a hotspot the request is refused
		cw_play_t play;
		if (cw_train_take(&play)) {
			bool ok = CONFIG_BEEP && !sm->ptt && sm->view.state != RS_TX && !sm->hotspot;
#if CONFIG_BEEP
			if (ok) {
				uint8_t level = sm->set.beep_level != 0 ? sm->set.beep_level : CW_TRAIN_BEEP_LEVEL;
				if (board.radio != NULL && board.radio->volume != NULL && !board.radio->volume_pcm) {
					level = radio_beep_level(level, sm->volume_sent);
				}
				beep_set_morse(play.text, play.len, play.hz, morse_dot_ms(play.wpm));
				beep_play(BEEP_MORSE, level, shared);
			}
#endif
			cw_train_done(ok);
		}
	}
#endif
'''


def prepare_cw_rx(transform, install):
    """CW decoder and practice (CONFIG_CW_RX, docs/plus.md "CW decoder and practice"; owner's round 3 item 6; needs
    MSG_RX). Tools > CW decoder decodes Morse sent as a keyed audio tone (MCW, 400-1000 Hz) on the FM or AM channel in
    use: radio_msg.c runs radio/radio_cw.c only while the screen is on top (RADIO_MSG_CW: FM caller ID's audio on FM,
    the receive ring itself on AM, as the pager and ACARS do); radio/cwdec.c is the integer DSP (Goertzel tone search,
    mixer envelope, adaptive threshold and timing, the ITU table with prosigns). # on it opens CW practice: Koch
    characters or groups played through the speaker by the beep generator (BEEP_MORSE: beep.c plays the elements from
    a text set with beep_set_morse; the radio task's poll takes the page's request, radio/cw_train.h), never the
    transmitter: the CW ID / beacon keyer's tone exists only as the FM modulator's. Key beeps are left out while the
    Morse plays. Runs last in the list (the Tools entry goes before Battery)."""
    for f in ('cwdec.c', 'cwdec.h', 'radio_cw.c', 'radio_cw.h', 'cw_train.c', 'cw_train.h'):
        install('core/radio/' + f, 'integration/radiofoundry/cw/' + f)
    install('core/ui/ui_cw.c', 'integration/radiofoundry/cw/ui_cw.c')
    transform('config/features.mk', [
        ('CONFIG_MSG_RX ?= n\nFEATURES += MSG_RX\n',
         'CONFIG_MSG_RX ?= n\nFEATURES += MSG_RX\nCONFIG_CW_RX ?= n\nFEATURES += CW_RX\n'
         'ifeq ($(CONFIG_CW_RX),y)\nifneq ($(CONFIG_MSG_RX),y)\n$(error CW_RX needs MSG_RX=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_MSG_RX\n', '#ifndef CONFIG_CW_RX\n#define CONFIG_CW_RX 0\n#endif\n#ifndef CONFIG_MSG_RX\n'),
    ])
    # The beep generator plays the practice's Morse (speaker only)
    transform('core/radio/beep.h', [
        ('\tBEEP_COUNT\n} beep_t;',
         '\tBEEP_MORSE,\t\t// DM32 Plus CW practice: the Morse set with beep_set_morse (a dot without one)\n'
         '\tBEEP_COUNT\n} beep_t;'),
        ('void beep_set_tone(uint16_t hz, uint16_t ms);\n',
         'void beep_set_tone(uint16_t hz, uint16_t ms);\n'
         '// DM32 Plus CW practice (CONFIG_CW_RX): what BEEP_MORSE plays next: text (A-Z, 0-9, morse.h\'s punctuation; a\n'
         '// space is a word gap; at most 12), the tone, the dot. Radio task, before beep_play(BEEP_MORSE)\n'
         'void beep_set_morse(const char *text, uint8_t len, uint16_t hz, uint16_t dot_ms);\n'
         '// The practice Morse is playing (asked for or sounding)\n'
         'bool beep_morse_playing(void);\n'),
    ])
    transform('core/radio/beep.c', [
        ('#include "config.h"\n', '#include "config.h"\n#if CONFIG_CW_RX\n#include "radio/morse.h"\n#endif\n'),
        ('static beep_seg_t p_tone[] = { { 1000, 50 } };\t// DM32 Plus: beep_set_tone\n',
         'static beep_seg_t p_tone[] = { { 1000, 50 } };\t// DM32 Plus: beep_set_tone\n'
         'static const beep_seg_t p_morse[] = { { 800, 60 } };\t// DM32 Plus: BEEP_MORSE without beep_set_morse\n'),
        ('\t[BEEP_TONE] = PAT(p_tone),\n', '\t[BEEP_TONE] = PAT(p_tone),\n\t[BEEP_MORSE] = PAT(p_morse),\n'),
        ('void beep_init(const hal_audio_t *audio)\n{', CW_BEEP_MORSE + 'void beep_init(const hal_audio_t *audio)\n{'),
        ('\tconst beep_seg_t *s = &patterns[cb_pattern].segs[cb_seg];\n', '\tconst beep_seg_t *s = seg_at();\n'),
        ('\t\tcb_active = true;\n\t\tload_seg();',
         '\t\tcb_active = true;\n#if CONFIG_CW_RX\n\t\tmorse_load();\n#endif\n\t\tload_seg();'),
        ('\t\t\tif (++cb_seg >= patterns[cb_pattern].count) {', '\t\t\tif (++cb_seg >= seg_count()) {'),
        ('\tif (cb_left == 0 && cb_seg + 1u >= patterns[cb_pattern].count) {',
         '\tif (cb_left == 0 && cb_seg + 1u >= seg_count()) {'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#include "settings/settings.h"\n#include "config.h"\n',
         '#include "settings/settings.h"\n#include "config.h"\n#if CONFIG_CW_RX\n#include "radio/cw_train.h"\n#endif\n'),
        ('\tbeep_service(shared);\n#endif\n', '\tbeep_service(shared);\n#endif\n' + CW_PRACTICE_HOOK),
        ('\tcase RREQ_BEEP:\n\t\tif (req->arg != BEEP_KEY || sm->set.key_beep) {',
         '\tcase RREQ_BEEP:\n'
         '#if CONFIG_CW_RX && CONFIG_BEEP\n'
         '\t\tif (req->arg == BEEP_KEY && beep_morse_playing()) {\n'
         '\t\t\tbreak;\t// DM32 Plus CW practice: a key beep would cut its Morse (typing along is how one practises)\n'
         '\t\t}\n'
         '#endif\n'
         '\t\tif (req->arg != BEEP_KEY || sm->set.key_beep) {'),
    ])
    # Tools > CW decoder (the last tool, before Battery and the zone offer) and its practice page
    transform('core/ui/ui_nav.h', [
        ('\tSCR_COUNT\n} ui_scr_t;',
         '\tSCR_CW, SCR_CW_TRAIN,\t\t// DM32 Plus: Tools > CW decoder, its CW practice (CONFIG_CW_RX)\n'
         '\tSCR_COUNT\n} ui_scr_t;'),
        ('bool ui_sstv_tick(void);\n#endif\n',
         'bool ui_sstv_tick(void);\n#endif\n'
         '#if CONFIG_CW_RX\n// DM32 Plus CW decoder and practice (ui_cw.c), from ui_msg_tick with the screen on top: true: '
         'draw again\nbool ui_cw_tick(uint8_t scr);\n#endif\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('extern const ui_scr_def_t ui_sstv_screen;\t// ui_sstv.c (DM32 Plus)',
         'extern const ui_scr_def_t ui_sstv_screen;\t// ui_sstv.c (DM32 Plus)\n'
         'extern const ui_scr_def_t ui_cw_screen, ui_cwtrain_screen;\t// ui_cw.c (DM32 Plus)'),
        ('#if CONFIG_SSTV\n\t[SCR_SSTV] = &ui_sstv_screen,\n#endif\n',
         '#if CONFIG_SSTV\n\t[SCR_SSTV] = &ui_sstv_screen,\n#endif\n'
         '#if CONFIG_CW_RX\n\t[SCR_CW] = &ui_cw_screen,\n\t[SCR_CW_TRAIN] = &ui_cwtrain_screen,\n#endif\n'),
        ('T_BATTERY, T_ZONE_OFFER, T_COUNT };', 'T_CW, T_BATTERY, T_ZONE_OFFER, T_COUNT };'),
        ('(i != T_BATTERY || board_volts()', '(i != T_CW || CONFIG_CW_RX) && (i != T_BATTERY || board_volts()'),
        ('TRN("Battery"), TRN("Zones/Chs Received") };', 'TRN("CW decoder"), TRN("Battery"), TRN("Zones/Chs Received") };'),
        ('\tcase T_ZONE_OFFER: ui_push(SCR_OFFER, 0); break;\n',
         '\tcase T_CW: ui_push(SCR_CW, 0); break;\n\tcase T_ZONE_OFFER: ui_push(SCR_OFFER, 0); break;\n'),
        ('\t\t[T_ZONE_OFFER] = ART_ICON_ZONE_OFFER + 1,\n',
         '\t\t[T_CW] = ART_ICON_BEACON + 1,\t// the CW beacon\'s\n\t\t[T_ZONE_OFFER] = ART_ICON_ZONE_OFFER + 1,\n'),
    ])


def prepare_rpt_prov(transform):
    """Repeaters by province (CONFIG_RPT_PROV, needs REPEATERS and MAP; owner's round 3 item 37, docs/plus.md
    "Repeater list and nearest repeater"): Tools > Repeaters opens on "All" and the provinces that have repeaters, in
    plate order with their counts ("06 Ankara (8)"), then "Unknown" if any; OK opens that province's list. The
    province of each entry is decided by tools/rpt_list.py from the map's own outlines (one byte in what was
    rpt_entry_t's tail padding: no flash per entry); the names are the map's (turkey_map_data.h). Our code is in
    integration/radiofoundry/rpt/ (rpt.c, ui_rpt.c), installed by prepare_repeaters; only the switch is here."""
    transform('config/features.mk', [
        # after MAP's and AUTO_RPT's own lines: both switches are set by then
        ('$(error AUTO_RPT needs REPEATERS=y and GPS=y)\nendif\nendif\n',
         '$(error AUTO_RPT needs REPEATERS=y and GPS=y)\nendif\nendif\n'
         'CONFIG_RPT_PROV ?= n\nFEATURES += RPT_PROV\n'
         'ifeq ($(CONFIG_RPT_PROV),y)\nifneq ($(CONFIG_REPEATERS)$(CONFIG_MAP),yy)\n'
         '$(error RPT_PROV needs REPEATERS=y and MAP=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_ID_DB\n', '#ifndef CONFIG_RPT_PROV\n#define CONFIG_RPT_PROV 0\n#endif\n#ifndef CONFIG_ID_DB\n'),
    ])
