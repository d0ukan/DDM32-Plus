# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus round 15 "channels" job (branch r15-channels, docs/plus.md "Channels, zones and DMR rows"): the owner's
reports after v1.0.0-beta.3, behind CONFIG_CHAN_EXTRAS. Called last from plus_features.prepare(); a module of its own so
the shared prepare list gains one line. Upstream files change only through the hunks below.

  1. Zone size: the zone and codeplug limits shown where channels are added, a full zone refused with its number.
  2. DMR rows: RX list Any is every talkgroup on the slot (not digital monitor), clearer rows and defaults.
  3. Digital monitor: DMon follows the toggle; a channel's Any shows AllTG.
  4. Delete channel: out of every zone, its slot freed (zone_edit_drop_channel, one zone_op_t kind).
  6. A new channel joins the zone Home is on (the true cause of item 1's "only up to 26").
  5. NAC in decimal: not here; it changes CONFIG_NAC_DEC itself (plus_features.prepare_nac_decimal, plus_p25nac.py).
"""

ON = '#if CONFIG_CHAN_EXTRAS\n'


def _if(new, old=None):
    return ON + new + ('#else\n' + old if old is not None else '') + '#endif\n'


def prepare_chux(transform, install):
    transform('config/features.mk', [
        ('CONFIG_DATAPACK ?= n\nFEATURES += DATAPACK\n',
         'CONFIG_DATAPACK ?= n\nFEATURES += DATAPACK\n'
         'CONFIG_CHAN_EXTRAS ?= n\nFEATURES += CHAN_EXTRAS\n'
         'ifeq ($(CONFIG_CHAN_EXTRAS),y)\nifneq ($(CONFIG_ZONE_EDIT)$(CONFIG_CHANNEL_EDIT)$(CONFIG_DMR_ANY),yyy)\n'
         '$(error CHAN_EXTRAS needs ZONE_EDIT=y, CHANNEL_EDIT=y and DMR_ANY=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_DATAPACK\n',
         '#ifndef CONFIG_CHAN_EXTRAS\n#define CONFIG_CHAN_EXTRAS 0\t/* DDM32 Plus r15 channels (tools/plus_chux.py) */\n#endif\n'
         '#ifndef CONFIG_DATAPACK\n'),
    ])
    prepare_zone_size(transform)
    prepare_dmr_rows(transform)
    prepare_dmon(transform)
    prepare_drop(transform)
    prepare_new_joins(transform, install)


def prepare_new_joins(transform, install):
    """6. Channels made on the radio go into zones (the real cause of "I can add a 27th channel, but on Home I can only
    go up to 26", docs/plus.md "Zone size: the real cause"). Without stored zones the radio shows the virtual "All
    channels" zone, every used channel; the first zone the radio makes (Create New Zone, Find & save's "Bulunanlar N")
    stores "All channels" as zone 0, a copy of the channels of that moment, and a channel made after it (Channels > New
    from current, the VFO's or the editor's Save as new channel) was written in no zone: Home stopped at the copy's end
    (the owner's backup: 27 channels, "All channels" 1-26). Now (integration/radiofoundry/codeplug/zone_join.inc):
      - such a channel goes into "All channels" (zone 0 while it keeps that name) and into the zone it was made for:
        Copy here's zone at its position, else the zone Home (the selected watch letter) is on, unless Home is on the
        VFO or a Find & save zone; one storage job (ZONE_OP_ADD_JOIN); a zone write that fails deletes the channel when
        it is in no zone then (the toast: not saved);
      - never silently in no zone: a toast says it (All channels full, no zone to take it), or that only one of the two
        took it;
      - Zone options > "Add unzoned channels": every channel in no zone at the zone's end (ZONE_OP_UNZONED), the count
        said; user-triggered only."""
    install('core/codeplug/zone_join.inc', 'integration/radiofoundry/codeplug/zone_join.inc')
    transform('core/codeplug/zone_edit.h', [
        ('\tZONE_OP_DROP,\t// DDM32 Plus: Delete channel (op->channel), zone_edit_drop_channel\n',
         '\tZONE_OP_DROP,\t// DDM32 Plus: Delete channel (op->channel), zone_edit_drop_channel\n'
         '\tZONE_OP_ADD_JOIN,\t// DDM32 Plus: a channel made on the radio (op->channel, op->ch) into All channels and\n'
         '\t\t\t\t// op->zone (ZONE_NONE: none) at op->pos (ZONE_POS_JOIN: Home\'s, at the end)\n'
         '\tZONE_OP_UNZONED,\t// DDM32 Plus: every channel in no zone into op->zone\n'),
        ('zone_edit_result_t zone_edit_drop_channel(uint16_t number);\n#endif\n',
         'zone_edit_result_t zone_edit_drop_channel(uint16_t number);\n'
         '// DDM32 Plus (zone_join.inc): pos for Home\'s zone (at its end; a Find & save zone does not take it); no zone\n'
         '#define ZONE_POS_JOIN\t0xFFFFu\n#define ZONE_NONE\t0xFFFFu\n'
         '// A channel made on the radio: written as number, into All channels (zone 0 while it has that name) and zone\n'
         '// at pos. OK unless the channel was not saved (NO_FREE, FAILED: a zone write failed with it in no zone)\n'
         'zone_edit_result_t zone_edit_add_join(uint16_t zone, uint16_t pos, uint16_t number, const channel_t *ch);\n'
         '// Every channel in no zone at the end of zone, while it has room\n'
         'zone_edit_result_t zone_edit_add_unzoned(uint16_t zone);\n'
         '// What the last of those two did (ZONE_NOTE_IDLE: nothing to say), once; n: the channel, or how many added\n'
         'enum { ZONE_NOTE_IDLE, ZONE_NOTE_NO_ZONE, ZONE_NOTE_ALL_FULL, ZONE_NOTE_NOT_ALL, ZONE_NOTE_ONLY_ALL,\n'
         '       ZONE_NOTE_ADDED, ZONE_NOTE_ADDED_FULL, ZONE_NOTE_NO_UNZONED };\n'
         'uint8_t zone_edit_note(uint16_t *n);\n#endif\n'),
    ])
    transform('core/codeplug/zone_edit.c', [
        ('static uint32_t applied;\nstatic uint8_t last_result;\n',
         _if('#include "codeplug/zone_join.inc"\n\n') + 'static uint32_t applied;\nstatic uint8_t last_result;\n'),
        ('\tcase ZONE_OP_DROP: return zone_edit_drop_channel(op->channel);\n',
         '\tcase ZONE_OP_DROP: return zone_edit_drop_channel(op->channel);\n'
         '\tcase ZONE_OP_ADD_JOIN: return zone_edit_add_join(op->zone, op->pos, op->channel, &op->ch);\n'
         '\tcase ZONE_OP_UNZONED: return zone_edit_add_unzoned(op->zone);\n'),
    ])
    transform('core/ui/ui_nav.h', [
        ('void ui_menus_save_failed(void);\n',
         'void ui_menus_save_failed(void);\n'
         + _if('void ui_menus_zone_note(void);\t// DDM32 Plus: the zone editor\'s note (zone_edit_note) as a toast\n')),
    ])
    transform('core/ui/ui_app.c', [
        ('\tui_svc_zone_poll();\n', '\tui_svc_zone_poll();\n' + _if('\tui_menus_zone_note();\n')),
    ])
    transform('core/ui/ui_menus.c', [
        # The editor's save: All channels and the zone it was made for, in one job
        ('#if CONFIG_ZONE_EDIT\n\tif (ced_into && ced_new && !as_new) {\n',
         '#if CONFIG_ZONE_EDIT && CONFIG_CHAN_EXTRAS\n'
         '\t// DDM32 Plus: into All channels and Copy here\'s zone at its position, else the zone Home (the selected\n'
         '\t// watch letter) is on, not on the VFO (zone_edit_add_join)\n'
         '\tif (ced_new || as_new) {\n'
         '\t\tconst radio_view_t *v = ui_view();\n'
         '\t\tbool into = ced_into;\n'
         '\t\tze_op_new(ZONE_OP_ADD_JOIN, into ? ced_into_pos : ZONE_POS_JOIN)->channel = num;\n'
         '\t\tif (!into)\n\t\t\tze_op.zone = v->vfo ? ZONE_NONE : v->zone_index;\n'
         '\t\tze_op.ch = ced_ch;\n'
         '\t\tif (!ui_svc_zone_edit(&ze_op))\n\t\t\treturn false;\n'
         '\t\tif (into) {\n\t\t\tced_into = false;\n\t\t\tzone_focus_num = num;\n\t\t}\n'
         '\t\treturn true;\n\t}\n'
         '#elif CONFIG_ZONE_EDIT\n\tif (ced_into && ced_new && !as_new) {\n'),
        # The note, in the editor's toast buffer
        ('static void ze_full_toast(void)\n{',
         '// DDM32 Plus: what the last channel into zones or Add unzoned channels did (zone_edit_note)\n'
         'void ui_menus_zone_note(void)\n{\n'
         '\tstatic const char *const text[] = {\n'
         '\t\t[ZONE_NOTE_NO_ZONE] = TRN("In no zone: channel "),\n'
         '\t\t[ZONE_NOTE_ALL_FULL] = TRN("All channels full, in no zone: ch "),\n'
         '\t\t[ZONE_NOTE_NOT_ALL] = TRN("All channels full, not in it: ch "),\n'
         '\t\t[ZONE_NOTE_ONLY_ALL] = TRN("Zone full, only in All channels: "),\n'
         '\t\t[ZONE_NOTE_ADDED] = TRN("Added to the zone: "),\n'
         '\t\t[ZONE_NOTE_ADDED_FULL] = TRN("Zone full, added: "),\n'
         '\t\t[ZONE_NOTE_NO_UNZONED] = TRN("Every channel is in a zone") };\n'
         '\tuint16_t n;\n\tuint8_t k = zone_edit_note(&n);\n'
         '\tif (k == ZONE_NOTE_IDLE || k >= sizeof text / sizeof text[0])\n\t\treturn;\n'
         '\tsbuf_t b;\n\tsb_init(&b, ze_msg, sizeof ze_msg);\n\tsb_str(&b, tr(text[k]));\n'
         '\tif (k != ZONE_NOTE_NO_UNZONED)\n\t\tsb_u32(&b, n, 1);\n'
         '\tui_toast(ze_msg);\n}\n\n'
         'static void ze_full_toast(void)\n{'),
        # Zone options > Add unzoned channels
        ('\tZO_DELETE, ZO_COUNT };\n', _if('\tZO_UNZONED,\t// DDM32 Plus: every channel in no zone into this one\n')
         + '\tZO_DELETE, ZO_COUNT };\n'),
        ('\t\t[ZO_DELETE] = TRN("Delete zone") };\n',
         _if('\t\t[ZO_UNZONED] = TRN("Add unzoned channels"),\n') + '\t\t[ZO_DELETE] = TRN("Delete zone") };\n'),
        ('\t} else if (!codeplug_zones_stored()) {\n\t\tui_toast(TR("It holds every channel: it stays"));\n',
         ON + '\t} else if (i == ZO_UNZONED) {\n\t\tze_op_new(ZONE_OP_UNZONED, 0);\n\t\tif (ze_post())\n\t\t\tui_pop();\n'
         '#endif\n'
         '\t} else if (!codeplug_zones_stored()) {\n\t\tui_toast(TR("It holds every channel: it stays"));\n'),
        ('\t.hint = CONFIG_ZONE_SHARE ? TRN("1-4 pick  Back") : TRN("1-3 pick  Back"),\n',
         _if('\t.hint = CONFIG_ZONE_SHARE ? TRN("1-5 pick  Back") : TRN("1-4 pick  Back"),\n',
             '\t.hint = CONFIG_ZONE_SHARE ? TRN("1-4 pick  Back") : TRN("1-3 pick  Back"),\n')),
    ])


def prepare_zone_size(transform):
    """1. Zone size (owner: "I can add a 27th channel to a zone, but on Home I can only go up to 26"). The limits are
    the codeplug's: 80 channels a zone (codeplug_zone_capacity; 16 in a codeplug with the old 16-channel zone records),
    68 zones, 1024 channels; nothing in the codeplug, the zone editor or the radio core stops below them
    (tests/plus/test_zone_cap.c, test_zone_radio.inc step through 80). Shown where channels are added: the zone list's
    "Add channel" row "26/80" (dimmed when full), the channel editor's Zones list "26/80" beside the zones it is not in,
    Zones > Create New Zone "5/68"; a full zone refuses with "Zone full: max 80 channels" (Add channel, Copy here,
    the channel editor's Zones; the last one's toast was untranslated)."""
    transform('core/ui/ui_menus.c', [
        # "Zone full: max 80 channels", in the zone editor's toast buffer; "26/80"
        ('\tsb_str(&b, name);\n\tui_toast(ze_msg);\n}\n#endif\n',
         '\tsb_str(&b, name);\n\tui_toast(ze_msg);\n}\n'
         + _if('// DDM32 Plus: a zone at its capacity, with the number (codeplug_zone_capacity: 80, or 16 in an old codeplug)\n'
               'static void ze_full_toast(void)\n{\n\tsbuf_t b;\n\tsb_init(&b, ze_msg, sizeof ze_msg);\n'
               '\tsb_str(&b, TR("Zone full: max "));\n\tsb_u32(&b, codeplug_zone_capacity(), 1);\n'
               '\tsb_str(&b, TR(" channels"));\n\tui_toast(ze_msg);\n}\n')
         + '#endif\n'),
        ('static bool chans_is_new(uint16_t raw)',
         _if('// DDM32 Plus: "26/80", n against a capacity\n'
             'static void sb_of(sbuf_t *b, uint32_t n, uint32_t cap)\n{\n\tsb_u32(b, n, 1);\n\tsb_chr(b, \'/\');\n'
             '\tsb_u32(b, cap, 1);\n}\n')
         + 'static bool chans_is_new(uint16_t raw)'),
        ('\t\t\tui_toast(TR("Zone full"));\n', _if('\t\t\tze_full_toast();\n', '\t\t\tui_toast(TR("Zone full"));\n')),
        ('{\n\t\tui_toast(TR("Zone full"));\n', '{\n' + _if('\t\tze_full_toast();\n', '\t\tui_toast(TR("Zone full"));\n')),
        ('\t\tui_toast("Zone full");\n', _if('\t\tze_full_toast();\n', '\t\tui_toast("Zone full");\n')),
        # The channel editor's Zones: "26/80" beside a zone the channel is not in (dimmed at the capacity)
        ('\tstr_copy(it->value, sizeof it->value, at >= 0 ? TR("In") : "-");\n',
         _if('\tif (at < 0) {\n\t\tsbuf_t b;\n\t\tsb_init(&b, it->value, sizeof it->value);\n'
             '\t\tsb_of(&b, chz_z.count, codeplug_zone_capacity());\n'
             '\t\tif (chz_z.count >= codeplug_zone_capacity())\n\t\t\tit->flags |= UI_ITEM_DIM;\n'
             '\t\treturn;\n\t}\n')
         + '\tstr_copy(it->value, sizeof it->value, at >= 0 ? TR("In") : "-");\n'),
        # The zone list's Add channel: "26/80", dimmed when full
        ('\tif (zone_edit_row(raw) != NULL) {\n\t\tstr_copy(it->label, sizeof it->label, zone_edit_row(raw));\n'
         '\t\treturn;\n\t}\n',
         '\tif (zone_edit_row(raw) != NULL) {\n\t\tstr_copy(it->label, sizeof it->label, zone_edit_row(raw));\n'
         + _if('\t\tif (raw == 0) {\n\t\t\tsbuf_t b;\n\t\t\tsb_init(&b, it->value, sizeof it->value);\n'
               '\t\t\tsb_of(&b, z->count, codeplug_zone_capacity());\n'
               '\t\t\tif (z->count >= codeplug_zone_capacity())\n\t\t\t\tit->flags |= UI_ITEM_DIM;\n\t\t}\n')
         + '\t\treturn;\n\t}\n'),
        # A zone position whose channel can't be read (Home skips it: the radio can't tune it) shows instead of
        # vanishing from the list: "27 ? Ch 123", "Can't be read"; its # menu still removes it
        ('\tuint16_t pos = (uint16_t)(raw - ZE_ADD);\n\tif (pos >= z->count || !codeplug_channel(z->channels[pos], &ch))\n'
         '\t\treturn false;\n',
         '\tuint16_t pos = (uint16_t)(raw - ZE_ADD);\n'
         + _if('\tif (pos < z->count && !codeplug_channel(z->channels[pos], &ch)) {\n'
               '\t\tsb_u32(&b, pos + 1u, 1);\n\t\tsb_str(&b, " ? Ch ");\n\t\tsb_u32(&b, z->channels[pos], 1);\n'
               '\t\treturn true;\n\t}\n')
         + '\tif (pos >= z->count || !codeplug_channel(z->channels[pos], &ch))\n\t\treturn false;\n'),
        ('\tif (raw >= z->count || !codeplug_channel(z->channels[raw], &ch))\n\t\treturn;\n',
         _if('\tif (raw < z->count && !codeplug_channel(z->channels[raw], &ch)) {\n'
             '\t\tsbuf_t ub;\n\t\tsb_init(&ub, it->label, sizeof it->label);\n\t\tsb_u32(&ub, raw + 1u, 2);\n'
             '\t\tsb_str(&ub, " ? Ch ");\n\t\tsb_u32(&ub, z->channels[raw], 1);\n'
             '\t\tstr_copy(it->value, sizeof it->value, TR("Can\'t be read"));\n\t\tit->flags |= UI_ITEM_WARN;\n'
             '\t\treturn;\n\t}\n')
         + '\tif (raw >= z->count || !codeplug_channel(z->channels[raw], &ch))\n\t\treturn;\n'),
        # Add channel on a full zone: refused at once (the list would offer channels it can't take)
        ('\t\tze_zone = zi;\n\t\tui_push(raw == 0 ? SCR_ZPICK : SCR_ZONEEDIT, zi);\n',
         '\t\tze_zone = zi;\n'
         + _if('\t\tif (raw == 0 && zone_of_frame()->count >= codeplug_zone_capacity()) {\n\t\t\tze_full_toast();\n'
               '\t\t\treturn;\n\t\t}\n')
         + '\t\tui_push(raw == 0 ? SCR_ZPICK : SCR_ZONEEDIT, zi);\n'),
        # Zones > Create New Zone: "5/68"
        ('\tif (chans_is_new(raw)) {\n\t\tstr_copy(it->label, sizeof it->label, TR("Create New Zone"));\n\t\treturn;\n\t}\n',
         '\tif (chans_is_new(raw)) {\n\t\tstr_copy(it->label, sizeof it->label, TR("Create New Zone"));\n'
         + _if('\t\tsb_of(&vb, codeplug_zones_stored() ? codeplug_zone_count() : 0u, CODEPLUG_ZONES_MAX);\n')
         + '\t\treturn;\n\t}\n'),
    ])



def prepare_dmr_rows(transform):
    """2. The channel editor's DMR rows (owner: "RX group list 'Any' doesn't work sensibly"). Any was digital monitor in
    disguise: it set the call layer's hear_all, so the channel also played private calls between other stations, Home
    showed DMon whatever the user's toggle said, and the user's toggle could not turn it off. Any is now its own
    filter, dmr_listen_filter_t.any_group: every group call on the channel's slot (the talk target, the reply and all
    call as before), nothing else; digital monitor stays the user's. In the editor: Any reads "All TGs TS1" (or
    "All TGs" with timeslot Any); the picker offers it right after None ("Any: all TGs") instead of after
    the 76 lists; picking Any or None says in one line what the channel hears; a channel made DMR gets the first group
    contact as its TX contact (it can't be saved without one) and colour code 1 / timeslot 1 if it had none; an RX only
    DMR channel saves without a TX contact; Direct mode on says what it does."""
    transform('core/dmr/call/dmr_call.h', [
        ('\tbool any_ts;\t\t// DM32 Plus: either timeslot, no slot lock (the first heard holds the call)\n#endif\n',
         '\tbool any_ts;\t\t// DM32 Plus: either timeslot, no slot lock (the first heard holds the call)\n#endif\n'
         + _if('\tbool any_group;\t\t// DDM32 Plus: every group call on the slot (RX group list Any), not others\' private calls\n')),
    ])
    transform('core/dmr/call/dmr_call.c', [
        ('\tif (f->hear_all || tg == DMR_ID_ALL_CALL)\n',
         _if('\tif (f->hear_all || f->any_group || tg == DMR_ID_ALL_CALL)\n', '\tif (f->hear_all || tg == DMR_ID_ALL_CALL)\n')),
    ])
    transform('core/radio/radio_sm.c', [
        ('\tcfg.listen.hear_all = cfg.listen.hear_all || ch->listen_list == CODEPLUG_LIST_ANY;\n',
         _if('\tcfg.listen.any_group = ch->listen_list == CODEPLUG_LIST_ANY;\t// DDM32 Plus: not digital monitor (hear_all)\n',
             '\tcfg.listen.hear_all = cfg.listen.hear_all || ch->listen_list == CODEPLUG_LIST_ANY;\n')),
    ])
    transform('core/ui/ui_menus.c', [
        # "All TGs TS1" / "All TGs"
        ('static void chinfo_item(uint16_t i, ui_item_t *it)\n',
         _if('// DDM32 Plus: what RX group list Any hears, with the slot\n'
             'static void sb_list_any(sbuf_t *b, uint8_t ts)\n{\n'
             '\tif (ts == 1u || ts == 2u) {\n\t\tsb_str(b, TR("All TGs TS"));\n\t\tsb_chr(b, (char)(\'0\' + ts));\n'
             '\t} else {\n\t\tsb_str(b, TR("All TGs"));\n\t}\n}\n\n')
         + 'static void chinfo_item(uint16_t i, ui_item_t *it)\n'),
        ('\t\tif (ch->listen_list == CODEPLUG_LIST_ANY) {\n\t\t\tsb_str(&b, TR("Any (all groups)"));\n\t\t\tbreak;\n\t\t}\n',
         '\t\tif (ch->listen_list == CODEPLUG_LIST_ANY) {\n'
         + _if('\t\t\tsb_list_any(&b, ch->timeslot);\n', '\t\t\tsb_str(&b, TR("Any (all groups)"));\n')
         + '\t\t\tbreak;\n\t\t}\n'),
        # A shorter label, so "All TGs TS1" fits beside it: "RX list"
        ('\tstr_copy(it->label, sizeof it->label, tr(labels[r]));\n',
         '\tstr_copy(it->label, sizeof it->label, tr(labels[r]));\n'
         + _if('\tif (r == CE_LIST)\n\t\tstr_copy(it->label, sizeof it->label, TR("RX list"));\n')),
        # The picker: None, Any, then the lists (raw 1 is Any, raw k + 1 list k)
        ('\tif (ui_frame()->arg != 0 && raw == CODEPLUG_LISTS_MAX + 1u) {\n\t\tname = TR("Any (all groups)");\n'
         '\t\traw = 0;\n\t}\n',
         _if('\tif (ui_frame()->arg != 0 && raw == 1u) {\n\t\tname = TR("Any: all TGs");\n\t\traw = 0;\n'
             '\t} else if (ui_frame()->arg != 0 && raw > 1u) {\n\t\traw--;\n\t}\n',
             '\tif (ui_frame()->arg != 0 && raw == CODEPLUG_LISTS_MAX + 1u) {\n\t\tname = TR("Any (all groups)");\n'
             '\t\traw = 0;\n\t}\n')),
        ('\tif (ui_frame()->arg != 0 && ced_ch.listen_list == CODEPLUG_LIST_ANY)\n'
         '\t\treturn (uint16_t)(CODEPLUG_LISTS_MAX + 1u);\n',
         _if('\tif (ui_frame()->arg != 0)\n'
             '\t\treturn ced_ch.listen_list == CODEPLUG_LIST_ANY ? 1u : ced_ch.listen_list ? (uint16_t)(ced_ch.listen_list + 1u) : 0u;\n',
             '\tif (ui_frame()->arg != 0 && ced_ch.listen_list == CODEPLUG_LIST_ANY)\n'
             '\t\treturn (uint16_t)(CODEPLUG_LISTS_MAX + 1u);\n')),
        ('\telse if (raw == CODEPLUG_LISTS_MAX + 1u)\n\t\tced_ch.listen_list = CODEPLUG_LIST_ANY;\n#endif\n'
         '\telse\n\t\tced_ch.listen_list = raw;\n\tui_pop();\n',
         '\telse if (!CONFIG_CHAN_EXTRAS && raw == CODEPLUG_LISTS_MAX + 1u)\n\t\tced_ch.listen_list = CODEPLUG_LIST_ANY;\n'
         '#endif\n\telse\n\t\tced_ch.listen_list = raw;\n'
         + _if('\tif (ui_frame()->arg != 0) {\t// DDM32 Plus: raw 1 is Any, raw k + 1 list k\n'
               '\t\tced_ch.listen_list = raw == 1u ? CODEPLUG_LIST_ANY : raw ? (uint16_t)(raw - 1u) : 0u;\n'
               '\t\tui_pop();\n'
               '\t\tif (raw <= 1u)\t// what the channel hears, in one line\n'
               '\t\t\tui_toast(raw ? TR("Any: every TG on this slot") : TR("None: only the TX contact\'s TG"));\n'
               '\t\treturn;\n\t}\n')
         + '\tui_pop();\n'),
        # Made DMR: a TX contact and a colour code / slot to start from
        ('\t\t\tc->mode = c->mode == RADIO_MODE_DMR ? RADIO_MODE_FM : RADIO_MODE_DMR;\n\t\tbreak;\n',
         '\t\t\tc->mode = c->mode == RADIO_MODE_DMR ? RADIO_MODE_FM : RADIO_MODE_DMR;\n'
         + _if('\t\tif (c->mode == RADIO_MODE_DMR)\n\t\t\tced_dmr_defaults(c);\n')
         + '\t\tbreak;\n'),
        ('\tif (CONFIG_DMR && c->mode == RADIO_MODE_DMR && c->contact == 0)\n\t\treturn TR("Pick a TX contact first");\n',
         _if('\tif (CONFIG_DMR && c->mode == RADIO_MODE_DMR && c->contact == 0 && !c->rx_only)\t// DDM32 Plus: RX only needs none\n',
             '\tif (CONFIG_DMR && c->mode == RADIO_MODE_DMR && c->contact == 0)\n')
         + '\t\treturn TR("Pick a TX contact first");\n'),
        ('// The first problem that keeps the copy from being saved, as a toast; NULL when it can be saved\n',
         _if('// DDM32 Plus: a channel just made DMR: the first group contact as its TX contact when it has none (it can\'t be\n'
             '// saved without one), colour code 1 and timeslot 1 when it has none of either\n'
             'static void ced_dmr_defaults(channel_t *c)\n{\n'
             '#if CONFIG_DMR\n'
             '\tstatic contact_t k;\t// static: the UI task\'s stack is short\n'
             '\tfor (uint16_t i = 1; c->contact == 0 && i <= CODEPLUG_CONTACTS_MAX; i++)\n'
             '\t\tif (codeplug_contact(i, &k) && k.type == CONTACT_GROUP)\n\t\t\tc->contact = i;\n'
             '#endif\n'
             '\tif (c->color_code > 15u && c->color_code != CODEPLUG_CC_ANY)\n\t\tc->color_code = 1;\n'
             '\tif (c->timeslot == 0u)\n\t\tc->timeslot = 1;\n}\n\n')
         + '// The first problem that keeps the copy from being saved, as a toast; NULL when it can be saved\n'),
        ('\tcase CE_DMO: c->dmo = !c->dmo; break;\n',
         _if('\tcase CE_DMO:\n\t\tc->dmo = !c->dmo;\n\t\tif (c->dmo)\n\t\t\tui_toast(TR("Direct: TX on the RX frequency"));\n\t\tbreak;\n',
             '\tcase CE_DMO: c->dmo = !c->dmo; break;\n')),
    ])


def prepare_dmon(transform):
    """3. Digital monitor (owner: "I turn it on and off and the screen still says DMon"). Home drew DMon for
    v->dmr.hear_all, which a channel with RX list Any kept on whatever the user's toggle said. With item 2 hear_all is
    the user's again (the toggle, the monitor key held); the channel's own Any gets its own chip, "AllTG" (not the
    warning colour: nothing to turn off), and DMon wins when both are on. The toggle says in one line what it does:
    "Digital monitor on: every call" / "Digital monitor off", and on a channel that hears every talkgroup anyway
    "Monitor off: channel hears all TGs"."""
    transform('core/ui/ui_screens.c', [
        ('\t\tx = gfx_text(f, x, L->chips_y, dmon, ui_col.warn) + f->w;\t// digital monitor (FM\'s is "Mon")\n',
         '\t\tx = gfx_text(f, x, L->chips_y, dmon, ui_col.warn) + f->w;\t// digital monitor (FM\'s is "Mon")\n'
         + _if('\telse if (v->ch.listen_list == CODEPLUG_LIST_ANY) {\t// DDM32 Plus: the channel hears every TG on its slot\n'
               '\t\tconst char *any = TR("AllTG");\n'
               '\t\tif (x + gfx_text_width(f, any) <= gfx_width())\n'
               '\t\t\tx = gfx_text(f, x, L->chips_y, any, ui_col.fg) + f->w;\n\t}\n')),
    ])
    transform('core/ui/ui_app.c', [
        ('\treturn on ? TR("Digital monitor on") : TR("Digital monitor off");\n',
         _if('\tif (on)\n\t\treturn TR("Digital monitor on: every call");\n'
             '\treturn view != NULL && view->ch.mode == RADIO_MODE_DMR && view->ch.listen_list == CODEPLUG_LIST_ANY ?\n'
             '\t\tTR("Monitor off: channel hears all TGs") : TR("Digital monitor off");\n',
             '\treturn on ? TR("Digital monitor on") : TR("Digital monitor off");\n')),
    ])


def prepare_drop(transform):
    """4. Delete channel: zone_edit_drop_channel takes the channel out of every zone that holds it, then deletes it
    (codeplug_delete_channel: its in-use bit and its RFKV record); one zone_op_t kind, so the UI posts it through the
    zone editor's queue (the storage job, the foreign codeplug question, the failure toast)."""
    transform('core/codeplug/zone_edit.h', [
        ('\tZONE_OP_NEW,\n} zone_op_kind_t;',
         '\tZONE_OP_NEW,\n' + _if('\tZONE_OP_DROP,\t// DDM32 Plus: Delete channel (op->channel), zone_edit_drop_channel\n')
         + '} zone_op_kind_t;'),
        ('zone_edit_result_t zone_edit_delete(uint16_t zone);\n',
         'zone_edit_result_t zone_edit_delete(uint16_t zone);\n'
         + _if('// DDM32 Plus (Delete channel): channel number out of every zone that holds it, then deleted (its slot is\n'
               '// free). NO_CHANNEL: no such channel. A zone left empty stays (its name is the user\'s)\n'
               'zone_edit_result_t zone_edit_drop_channel(uint16_t number);\n')),
    ])
    transform('core/codeplug/zone_edit.c', [
        ('static uint32_t applied;\nstatic uint8_t last_result;\n',
         _if('zone_edit_result_t zone_edit_drop_channel(uint16_t number)\n{\n'
             '\tstorage_lock();\n'
             '\tzone_edit_result_t r = number >= 1u && number <= CODEPLUG_CHANNELS_MAX && codeplug_channel(number, &probe) ?\n'
             '\t\t\t\t       ZONE_EDIT_OK : ZONE_EDIT_NO_CHANNEL;\n'
             '\tfor (uint16_t zi = 0; r == ZONE_EDIT_OK && codeplug_zones_stored() && zi < codeplug_zone_count(); zi++) {\n'
             '\t\tr = load(zi);\n'
             '\t\tbool changed = false;\n'
             '\t\tfor (int at; r == ZONE_EDIT_OK && (at = zone_list_find(&work, number)) >= 0; changed = true)\n'
             '\t\t\tr = zone_list_remove(&work, (uint16_t)at);\n'
             '\t\tif (r == ZONE_EDIT_OK && changed)\n\t\t\tr = write_back(zi);\n'
             '\t}\n'
             '\tif (r == ZONE_EDIT_OK && !codeplug_delete_channel(number))\n\t\tr = ZONE_EDIT_FAILED;\n'
             '\tstorage_unlock();\n\treturn r;\n}\n\n')
         + 'static uint32_t applied;\nstatic uint8_t last_result;\n'),
        ('\tcase ZONE_OP_NEW: return zone_edit_new(op->name, index);\n',
         '\tcase ZONE_OP_NEW: return zone_edit_new(op->name, index);\n'
         + _if('\tcase ZONE_OP_DROP: return zone_edit_drop_channel(op->channel);\n')),
    ])
    # The UI: the channel editor's last row and the zone's channel menu (#) ask first, No focused (SCR_CONFIRM arg 2)
    transform('core/ui/ui_menus.c', [
        ('static void ze_full_toast(void)\n',
         '// DDM32 Plus (Delete channel): the channel asked about (SCR_CONFIRM arg 2), its name, and whether its editor is\n'
         '// under the question (it closes with the channel)\n'
         'static uint16_t chdel_num;\nstatic char chdel_name[CODEPLUG_NAME_LEN + 1];\nstatic bool chdel_editor;\n'
         '#if CONFIG_XBAND\nbool xband_on(void);\t// radio/xband.h\n#endif\n\n'
         '// Refused for the channel in use (the radio would have to move first) and while cross-band runs\n'
         'static void chdel_ask(uint16_t num, const char *name, bool editor)\n{\n'
         '\tconst radio_view_t *v = ui_view();\n'
         '\tif (!v->vfo && v->channel_number == num) {\n\t\tui_toast(TR("In use: switch to another channel first"));\n'
         '\t\treturn;\n\t}\n'
         '#if CONFIG_XBAND\n\tif (xband_on()) {\n\t\tui_toast(TR("Cross-band on: stop it first"));\n\t\treturn;\n\t}\n#endif\n'
         '\tchdel_num = num;\n\tchdel_editor = editor;\n\tstr_copy(chdel_name, sizeof chdel_name, name);\n'
         '\tui_push(SCR_CONFIRM, 2);\n}\n\n'
         '// Yes: out of every zone, then deleted, in the zone editor\'s storage job (a failure says so there)\n'
         'static void chdel_do(void)\n{\n\tze_op_new(ZONE_OP_DROP, 0)->channel = chdel_num;\n'
         '\tif (ze_post())\n\t\tze_toast(TR("Deleted "), chdel_name);\n}\n\n'
         'static void ze_full_toast(void)\n'),
        # The channel editor: "Delete channel", the last row (a stored channel only)
        ('CE_SAVE, CE_SAVE_NEW, CE_COUNT, CE_NONE = 0xFF };',
         'CE_SAVE, CE_SAVE_NEW,\n#if CONFIG_CHAN_EXTRAS\n       CE_DELETE,\n#endif\n       CE_COUNT, CE_NONE = 0xFF };'),
        ('\t\tTRN("Save"), TRN("Save as new channel") };\n',
         '\t\tTRN("Save"), TRN("Save as new channel"),\n#if CONFIG_CHAN_EXTRAS\n\t\tTRN("Delete channel"),\n#endif\n\t};\n'),
        ('\t\tif ((r == CE_ZONES || r == CE_GO || r == CE_SHARE || r == CE_SAVE_NEW) && fresh)\n\t\t\tcontinue;\n',
         '\t\tif ((r == CE_ZONES || r == CE_GO || r == CE_SHARE || r == CE_SAVE_NEW) && fresh)\n\t\t\tcontinue;\n'
         + _if('\t\tif (r == CE_DELETE && (fresh || !edit))\n\t\t\tcontinue;\n')),
        ('\tif (r == CE_LIST)\n\t\tstr_copy(it->label, sizeof it->label, TR("RX list"));\n',
         '\tif (r == CE_LIST)\n\t\tstr_copy(it->label, sizeof it->label, TR("RX list"));\n'
         '\tif (r == CE_DELETE)\n\t\tit->flags |= UI_ITEM_WARN;\n'),
        ('\tcase CE_SAVE_NEW: ced_save(true); break;\n',
         '\tcase CE_SAVE_NEW: ced_save(true); break;\n' + _if('\tcase CE_DELETE: chdel_ask(ced_num, ced_ch.name, true); break;\n')),
        # The zone's channel menu: "Delete channel", the last
        ('\tZC_PRIO,\t// DM32 Plus\n#endif\n\tZC_COUNT };',
         '\tZC_PRIO,\t// DM32 Plus\n#endif\n' + _if('\tZC_DELETE,\t// DDM32 Plus\n') + '\tZC_COUNT };'),
        ('\t\t[ZC_PRIO] = TRN("Set as priority"),\n#endif\n\t};\n',
         '\t\t[ZC_PRIO] = TRN("Set as priority"),\n#endif\n' + _if('\t\t[ZC_DELETE] = TRN("Delete channel"),\n') + '\t};\n'),
        ('\tif (i == ZC_REMOVE)\n\t\tit->flags |= UI_ITEM_WARN;\n',
         _if('\tif (i == ZC_REMOVE || i == ZC_DELETE)\n', '\tif (i == ZC_REMOVE)\n') + '\t\tit->flags |= UI_ITEM_WARN;\n'),
        ('\t} else if (i == ZC_REMOVE) {\n\t\tze_op_new(ZONE_OP_REMOVE, pos);\n',
         _if('\t} else if (i == ZC_DELETE) {\n\t\tui_pop();\n\t\tchdel_ask(num, zc_name, false);\n')
         + '\t} else if (i == ZC_REMOVE) {\n\t\tze_op_new(ZONE_OP_REMOVE, pos);\n'),
        ('.hint = CONFIG_ZONE_SHARE ? (CONFIG_SCAN_PRIO ? TRN("1-7 pick  Back") : TRN("1-6 pick  Back")) :\n'
         '\t\t(CONFIG_SCAN_PRIO ? TRN("1-6 pick  Back") : TRN("1-5 pick  Back")),\n',
         '\n' + _if('\t.hint = CONFIG_ZONE_SHARE + CONFIG_SCAN_PRIO == 2 ? TRN("1-8 pick  Back") :\n'
                    '\t\tCONFIG_ZONE_SHARE + CONFIG_SCAN_PRIO == 1 ? TRN("1-7 pick  Back") : TRN("1-6 pick  Back"),\n',
                    '\t.hint = CONFIG_ZONE_SHARE ? (CONFIG_SCAN_PRIO ? TRN("1-7 pick  Back") : TRN("1-6 pick  Back")) :\n'
                    '\t\t(CONFIG_SCAN_PRIO ? TRN("1-6 pick  Back") : TRN("1-5 pick  Back")),\n')),
        # The question: "Delete channel?", the name, how many zones it leaves; No focused
        ('#if CONFIG_CHANNEL_EDIT\n    if (ui_frame()->arg == 1) {\n        str_copy(it->label',
         _if('\tif (ui_frame()->arg == 2) {\n'
             '\t\tstr_copy(it->label, sizeof it->label, i == 0 ? TR("No, keep it") : TR("Yes, delete"));\n'
             '\t\tif (i == 1)\n\t\t\tit->flags |= UI_ITEM_WARN;\n\t\treturn;\n\t}\n')
         + '#if CONFIG_CHANNEL_EDIT\n    if (ui_frame()->arg == 1) {\n        str_copy(it->label'),
        ('    (void)hold;\n#if CONFIG_CHANNEL_EDIT\n    if (ui_frame()->arg == 1) {\n',
         '    (void)hold;\n'
         + _if('\tif (ui_frame()->arg == 2) {\n\t\tui_pop();\n\t\tif (i == 1) {\n'
               '\t\t\tif (chdel_editor)\n\t\t\t\tui_pop();\t// the deleted channel\'s editor\n'
               '\t\t\tchdel_do();\n\t\t}\n\t\treturn;\n\t}\n')
         + '#if CONFIG_CHANNEL_EDIT\n    if (ui_frame()->arg == 1) {\n'),
        ('#if CONFIG_CHANNEL_EDIT\n    if (ui_frame()->arg == 1) {\n        sbuf_t b;',
         _if('\tif (ui_frame()->arg == 2) {\n\t\tuint16_t n = 0;\n'
             '\t\tfor (uint16_t zi = 0; zi < codeplug_zone_count(); zi++)\n'
             '\t\t\tif (codeplug_zone(zi, &chz_z) && zone_list_find(&chz_z, chdel_num) >= 0)\n\t\t\t\tn++;\n'
             '\t\tsbuf_t b;\n\t\tsb_init(&b, ced_msg, sizeof ced_msg);\n'
             '\t\tsb_str(&b, chdel_name);\n\t\tsb_chr(&b, \'\\n\');\n'
             '\t\tsb_str(&b, TR("Leaves zones: "));\n\t\tsb_u32(&b, n, 1);\n\t\tsb_chr(&b, \'\\n\');\n'
             '\t\tsb_str(&b, TR("Can\'t be undone."));\n\t\treturn ced_msg;\n\t}\n')
         + '#if CONFIG_CHANNEL_EDIT\n    if (ui_frame()->arg == 1) {\n        sbuf_t b;'),
        ('#if CONFIG_CHANNEL_EDIT\n    if (ui_frame()->arg == 1) return TR("Enable TX?");',
         _if('\tif (ui_frame()->arg == 2)\n\t\treturn TR("Delete channel?");\n')
         + '#if CONFIG_CHANNEL_EDIT\n    if (ui_frame()->arg == 1) return TR("Enable TX?");'),
    ])
    # The simulator (host UI tests): a built-in channel can be deleted too (Delete channel)
    transform('tests/host/sim/ui_sim.c', [
        ('\tn_edits = 0;\n', '\tn_edits = 0;\n' + _if('\tsim_gone = 0;\n')),
        ('#define SIM_EDITS\t8\n',
         '#define SIM_EDITS\t8\n' + _if('static uint64_t sim_gone;\t// DDM32 Plus Delete channel: channels 1-64 deleted\n')),
        ('bool codeplug_channel(uint16_t number, channel_t *out)\n{\n\tconst channel_t *e = sim_edit(number);\n',
         'bool codeplug_channel(uint16_t number, channel_t *out)\n{\n'
         + _if('\tif (number >= 1u && number <= 64u && (sim_gone >> (number - 1u)) & 1u)\n\t\treturn false;\n')
         + '\tconst channel_t *e = sim_edit(number);\n'),
        ('\tsim_edits[i].number = number;\n\tsim_edits[i].ch = *ch;\n',
         '\tsim_edits[i].number = number;\n\tsim_edits[i].ch = *ch;\n'
         + _if('\tif (number >= 1u && number <= 64u)\n\t\tsim_gone &= ~(1ull << (number - 1u));\n')),
        ('bool codeplug_delete_channel(uint16_t number)\n{\n\tfor (uint8_t i = 0; i < n_edits; i++) {\n'
         '\t\tif (sim_edits[i].number == number) {\n\t\t\tsim_edits[i] = sim_edits[--n_edits];\n\t\t\treturn true;\n'
         '\t\t}\n\t}\n\treturn false;\n}\n',
         'bool codeplug_delete_channel(uint16_t number)\n{\n'
         + _if('\tbool gone = number >= 1u && number <= 64u;\n\tif (gone)\n\t\tsim_gone |= 1ull << (number - 1u);\n',
               '\tbool gone = false;\n')
         + '\tfor (uint8_t i = 0; i < n_edits; i++) {\n'
         '\t\tif (sim_edits[i].number == number) {\n\t\t\tsim_edits[i] = sim_edits[--n_edits];\n\t\t\treturn true;\n'
         '\t\t}\n\t}\n\treturn gone;\n}\n'),
    ])
