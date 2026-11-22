// SPDX-License-Identifier: GPL-3.0-or-later
// DDM32 Plus Home details (docs/plus.md "Dual watch and multi-watch: the TX target", Home's details): a channel's own
// settings in its mode's terms, for the single channel's chips row (P25 / YSF / NXDN / dPMR: FM's tones and DMR's
// talk row were there already), dual watch's panes and the multi-watch rows. Included by core/ui/ui_screens.c after
// sb_nac_both (tools/plus_mwfix.py). The caller takes the wider form that fits:
//            UI_DETAIL_WIDE            UI_DETAIL_TIGHT        TALK (what PTT talks to)   REST
//   P25      NAC $293/659 TG 42        NAC$293/659 TG42       TG 42                      NAC $293/659
//            (RX NAC Any with a TX NAC of its own: NAC Any>$293/659)
//   DMR      CC1 TS1 TG 91             CC1 TS1 TG91           TG 91 (private: ID 1234)   CC1 TS1  (Any: CC* TS*)
//   YSF      DG-ID 12                  DG12                   DG-ID 12
//   NXDN     RAN 5                     RAN5                   RAN 5
//   dPMR     CC 7                      CC7                    CC 7
//   FM       T88.5 R100.0              T88.5                                             T88.5 R100.0 (No tone)

// Channel ch packed for a row: a, b, c as ui_sb_detail reads them (DMR: the caller adds the talk target to b, c)
void ui_detail_of(const channel_t *ch, ui_detail_t *d)
{
	d->mode = ch->mode;
	d->a = d->b = d->c = 0;
	if (codeplug_mode_p25(ch->mode) || codeplug_mode_nxdn(ch->mode) || codeplug_mode_dpmr(ch->mode)) {
		d->a = ch->p25_nac;
		d->b = ch->p25_tg;
		d->c = codeplug_p25_tx_nac(ch);
	} else if (ch->mode == RADIO_MODE_DMR) {
		d->a = (uint16_t)(ch->color_code | ch->timeslot << 8);
	} else {
		d->a = (uint16_t)(ch->tx_tone.type << 12 | (ch->tx_tone.value & 0xFFFu));
		d->b = (uint16_t)(ch->rx_tone.type << 12 | (ch->rx_tone.value & 0xFFFu));
	}
}

static void sb_detail_tone(sbuf_t *b, char tag, uint16_t packed)
{
	subtone_t t = { (uint8_t)(packed >> 12), (uint16_t)(packed & 0xFFFu) };
	if (tag != 0)
		sb_chr(b, tag);
	sb_tone(b, &t);
}

void ui_sb_detail(sbuf_t *b, const ui_detail_t *d, uint8_t form)
{
	bool tight = form == UI_DETAIL_TIGHT, talk = form != UI_DETAIL_REST, rest = form != UI_DETAIL_TALK_ONLY;
	const char *gap = tight ? "" : " ";
	uint8_t m = d->mode;
	bool p25 = codeplug_mode_p25(m) && !codeplug_mode_ysf(m);
	if (m == RADIO_MODE_DMR) {
		uint8_t cc = (uint8_t)d->a, ts = (uint8_t)(d->a >> 8);
		if (rest) {
			sb_str(b, "CC");
			if (cc > 15u)
				sb_chr(b, '*');
			else
				sb_u32(b, cc, 1);
			sb_str(b, " TS");
			if (ts > 2u)
				sb_chr(b, '*');
			else
				sb_u32(b, ts ? ts : 1u, 1);
		}
		if (talk && (d->c & UI_DETAIL_TALK)) {
			if (rest)
				sb_chr(b, ' ');
			sb_str(b, d->c & UI_DETAIL_PRIVATE ? "ID" : "TG");
			sb_str(b, gap);
			sb_u32(b, (uint32_t)d->b | (uint32_t)(d->c & 0xFFu) << 16, 1);
		}
		return;
	}
	if (!p25 && !codeplug_mode_ysf(m) && !codeplug_mode_nxdn(m) && !codeplug_mode_dpmr(m)) {
		if (!rest)
			return;
		if (d->a >> 12 == TONE_NONE && d->b >> 12 == TONE_NONE) {
			sb_str(b, TR("No tone"));
			return;
		}
		if (d->a >> 12 != TONE_NONE)
			sb_detail_tone(b, (d->a >> 12) == TONE_CTCSS ? 'T' : 0, d->a);
		if (d->b >> 12 != TONE_NONE && d->b != d->a && !(tight && d->a >> 12 != TONE_NONE)) {
			if (d->a >> 12 != TONE_NONE)
				sb_chr(b, ' ');
			sb_detail_tone(b, 'R', d->b);
		}
		return;
	}
	if (p25 && rest) {
		sb_str(b, "NAC");
		sb_str(b, gap);
		if (d->a == 0xF7Eu)
			sb_str(b, TR("Any"));
		else
			sb_nac_both(b, d->a);
		if (d->c != d->a) {
			sb_chr(b, '>');	// a TX NAC of its own (RX Any inherits $293, Classic r13)
			sb_nac_both(b, d->c);
		}
		if (talk)
			sb_chr(b, ' ');
	}
	if (!talk)
		return;
	// The P25 talk group, the YSF DG-ID, the NXDN RAN, the dPMR colour code (stored + 1); 0 is Any
	sb_str(b, p25 ? "TG" : codeplug_mode_ysf(m) ? (tight ? "DG" : "DG-ID") : codeplug_mode_nxdn(m) ? "RAN" : "CC");
	sb_str(b, gap);
	if (d->b == 0)
		sb_str(b, TR("Any"));
	else
		sb_u32(b, codeplug_mode_dpmr(m) ? d->b - 1u : d->b, 1);
}

// Draws channel ch's details at (x, y) in f, the wider form that fits before `right`, in buf (len bytes, the caller's:
// Home is the UI task's deepest path). Returns the x after them
static int __attribute__((noinline)) detail_draw(const font_t *f, int x, int y, int right, const channel_t *ch,
						 char *buf, size_t len, color_t c)
{
	ui_detail_t d;
	sbuf_t b;
	ui_detail_of(ch, &d);
	for (uint8_t form = UI_DETAIL_WIDE; form <= UI_DETAIL_TIGHT; form++) {
		sb_init(&b, buf, len);
		ui_sb_detail(&b, &d, form);
		if (x + gfx_text_width(f, buf) <= right)
			break;
	}
	size_t n = strlen(buf);	// not even the tight form: cut at the end (no second buffer on this path)
	while (n > 0 && x + gfx_text_width(f, buf) > right)
		buf[--n] = '\0';
	return gfx_text(f, x, y, buf, c);
}
