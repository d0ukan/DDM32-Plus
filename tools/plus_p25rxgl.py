#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus P25 RX group list (CONFIG_P25_RXGL, branch rxgl; docs/rx-group-list-budget.md, docs/plus.md "P25 RX
group list"). A P25 channel picks one of the codeplug's 76 RX group lists in the channel editor (the "RX list" row a
DMR channel has; the picker's None / Any / lists); with a list the channel's own talkgroup and every P25 talkgroup of
the list (its group contacts of 1-65535) play, with Any every group (the Normal squelch), with None the channel's
squelch as before. Stored in the channel record's own RX group list byte (and Any in its colour code byte's 0x20 bit,
as a DMR channel), written only by the channel editor's existing save; the lists and contacts themselves are the
codeplug's (CPS, DOTA import) and only read here. The talkgroup stays explicit in the RFKV P25 block. Called last
from plus_features.prepare(); a module of its own so the shared prepare list gains one line."""

TAG = 'CONFIG_P25_RXGL'
RG = 'integration/radiofoundry/p25rxgl/'


def _if(new, old=None):
    return f'#if {TAG}\n{new}' + (f'#else\n{old}' if old is not None else '') + '#endif\n'


def prepare_p25rxgl(transform, install):
    install('core/p25/p25_rxgl.c', RG + 'p25_rxgl.c')
    install('core/p25/p25_rxgl.h', RG + 'p25_rxgl.h')

    transform('config/features.mk', [
        ('$(error CHAN_EXTRAS needs ZONE_EDIT=y, CHANNEL_EDIT=y and DMR_ANY=y)\nendif\nendif\n',
         '$(error CHAN_EXTRAS needs ZONE_EDIT=y, CHANNEL_EDIT=y and DMR_ANY=y)\nendif\nendif\n'
         'CONFIG_P25_RXGL ?= n\nFEATURES += P25_RXGL\n'
         'ifeq ($(CONFIG_P25_RXGL),y)\nifneq ($(CONFIG_P25_CHANNELS)$(CONFIG_CHAN_EXTRAS),yy)\n'
         '$(error P25_RXGL needs P25_CHANNELS=y and CHAN_EXTRAS=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_CHAN_EXTRAS\n',
         '#ifndef CONFIG_P25_RXGL\n#define CONFIG_P25_RXGL 0\t/* DDM32 Plus P25 RX group list (tools/plus_p25rxgl.py) */\n'
         '#endif\n#ifndef CONFIG_CHAN_EXTRAS\n'),
    ])
    transform('core/ui/ui_menus.c', [
        # the editor shows the RX list row on a P25 channel (not in the VFO: it has no group lists)
        ('\t\tbool dmr_only = r >= CE_CC && r <= CE_ALIAS2;\n',
         '\t\tbool dmr_only = r >= CE_CC && r <= CE_ALIAS2;\n'
         + _if('\t\tif (r == CE_LIST && ced_ch.mode == RADIO_MODE_P25 && !ced_vfo)\n'
               '\t\t\tdmr_only = false;\t// DDM32 Plus: a P25 channel\'s RX group list\n')),
        # a P25 channel's Any has no slot
        ('\t\t\tsb_list_any(&b, ch->timeslot);\n',
         _if('\t\t\tsb_list_any(&b, ch->mode == RADIO_MODE_DMR ? ch->timeslot : 0u);\n',
             '\t\t\tsb_list_any(&b, ch->timeslot);\n')),
        # the picker's line on what a P25 channel hears
        ('\t\t\tui_toast(raw ? TR("Any: every TG on this slot") : TR("None: only the TX contact\'s TG"));\n',
         _if('\t\t\tif (ced_ch.mode == RADIO_MODE_P25)\n'
             '\t\t\t\tui_toast(raw ? TR("Any: every TG") : TR("None: the channel\'s squelch"));\n'
             '\t\t\telse\n'
             '\t\t\t\tui_toast(raw ? TR("Any: every TG on this slot") : TR("None: only the TX contact\'s TG"));\n',
             '\t\t\tui_toast(raw ? TR("Any: every TG on this slot") : TR("None: only the TX contact\'s TG"));\n')),
        # typing the talkgroup keeps the list (it only adds groups heard); the contact still goes
        ('\t\tif (ced_len) { c->p25_tg = (uint16_t)ced_val; c->contact=c->listen_list=0; }\n',
         _if('\t\tif (ced_len) { c->p25_tg = (uint16_t)ced_val; c->contact=0; }\n',
             '\t\tif (ced_len) { c->p25_tg = (uint16_t)ced_val; c->contact=c->listen_list=0; }\n')),
    ])
