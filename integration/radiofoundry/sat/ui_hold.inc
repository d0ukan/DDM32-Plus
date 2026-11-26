// SPDX-License-Identifier: GPL-3.0-or-later
// DDM32 Plus hold keys (docs/plus.md "Hold keys on Home"; installed as core/ui/ui_hold.inc and included by ui_menus.c
// right after the Tools list, tools/plus_satauto.py). A digit held on a Home channel opens the tool picked for it in
// Settings > Keys: settings_t.hold_tool[d] is 0 for none, else the tool's number in the Tools list (T_ index + 1; hold 4
// starts as the ISS). The ISS keeps hold 4's own start of the held track (ui_sat.c ui_sat_home_iss). hold_c caches what
// is usable (the tool is in this build and on this radio, the board has the digit key): it is read in the key and tick
// paths and again after a change in Settings, never in a draw (the settings read there is a deep frame)

_Static_assert(T_ISS + 1 == UI_HOLD_ISS, "hold 4's default is the ISS");

void ui_sat_home_iss(void);		// ui_sat.c
bool ui_has_action(uint8_t action);	// ui_app.c

static uint8_t hold_c[10];
static bool hold_ok;

static void hold_load(void)
{
	for (uint8_t d = 0; d < 10; d++) {
		int32_t c = ui_svc_setting_get((uint8_t)(UI_SET_HOLD_FIRST + d));
		bool ok = c > 0 && c <= T_ZONE_OFFER && tool_shown((uint16_t)(c - 1)) && ui_has_action((uint8_t)(ACT_DIGIT_0 + d));
		hold_c[d] = ok ? (uint8_t)c : 0;
	}
	hold_ok = true;
}

void ui_hold_forget(void) { hold_ok = false; }	// ui_app_init: after a start or a reset

// ui_home_key: a held digit on a channel. false: nothing is set for it (the speed dial toast as before)
bool ui_hold_tool(uint8_t d)
{
	if (!hold_ok)
		hold_load();
	if (d > 9 || hold_c[d] == 0)
		return false;
	if (hold_c[d] == UI_HOLD_ISS)
		ui_sat_home_iss();
	else
		tool_open((uint16_t)(hold_c[d] - 1u));
	return true;
}

#if CONFIG_AUTO_SCAN
// DDM32 Plus auto scan: its held digit works on a VFO too (the scan's letters are VFOs; the user may start it from the
// VFO), not while a frequency is being typed (ui_app.c ui_home_key)
bool ui_hold_any(uint8_t d)
{
	if (!hold_ok)
		hold_load();
	return d <= 9 && (hold_c[d] == T_AIRSCAN + 1 || hold_c[d] == T_RPTSCAN + 1);
}
#endif

// Home's hint group: "4 ISS", the first digit set and its tool's name, "+" after it when more are set; NULL when none is.
// load: read the settings first (the tick; a draw passes false and sees what the tick last read)
const char *ui_hold_hint(bool load)
{
	static char buf[28];
	uint8_t first = 10, n = 0;
	if (load && !hold_ok)
		hold_load();
	for (uint8_t d = 0; d < 10; d++) {
		if (hold_c[d] != 0 && n++ == 0)
			first = d;
	}
	if (n == 0)
		return NULL;
	sbuf_t b;
	sb_init(&b, buf, sizeof buf);
	sb_chr(&b, (char)('0' + first));
	sb_chr(&b, ' ');
	sb_str(&b, tr(tool_names[hold_c[first]]));
	if (n > 1)
		sb_chr(&b, '+');
	return buf;
}

// The dual and multi-watch hint (ui_screens.c dual_hint, ui_mwatch_draw.inc mw_hint): the template is translated whole
// with "%" where the group goes, which here is cut to 5 characters (the digit, a space, three of the name: "4 ISS" in
// full) so that the second form still fits the 220 px row
const char *ui_hold_dual_hint(void)
{
	static char buf[96];
	const char *g = ui_hold_hint(false);
	char *o = buf;
	if (g == NULL)
		return NULL;
	for (const char *t = TR(TEXT_ALT_OPEN "# options  %  Back stop" TEXT_ALT_NEXT "# more  %  Back stop" TEXT_ALT_CLOSE);
	     *t != 0 && o < buf + sizeof buf - 8; t++) {
		if (*t != '%') {
			*o++ = *t;
			continue;
		}
		uint8_t cp = 0;
		for (const char *p = g; *p != 0 && *p != '+' && o < buf + sizeof buf - 8; p++) {
			if (((unsigned char)*p & 0xC0) != 0x80 && cp++ == 5)
				break;
			*o++ = *p;
		}
	}
	*o = 0;
	return buf;
}

// Settings > Keys rows "0 hold" .. "9 hold" (the language's word for hold after the digit)
static char hold_label[10][8];
static uint8_t hold_lang = 0xFF;

static void hold_labels(void)
{
	if (hold_lang == lang_current())
		return;
	hold_lang = lang_current();
	for (uint8_t d = 0; d < 10; d++) {
		sbuf_t b;
		sb_init(&b, hold_label[d], sizeof hold_label[d]);
		sb_chr(&b, (char)('0' + d));
		sb_str(&b, TR(" hold"));
	}
}
