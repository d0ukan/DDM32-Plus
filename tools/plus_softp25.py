#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus P25 soft probe (CONFIG_SEARCH_SOFTP25, branch swprobe; docs/p25-soft-probe-design.md). The owner's item
after beta.8 ("P25 search as fast as OpenDMR"; the idea only, OpenDMR's licence forbids commercial use and none of its
code is used): a hit of the P25 search is first read in software on the FM discriminator (the flat receive path and
the HR_C7000's 8 kHz FM RX RAM ring that NXDN reads) for up to 450 ms. A good NID: P25, its NAC listed at once and the
native receiver takes the frequency as before (it plays the call). Nothing P25-like on a carrier 25 dB or more over the
floor: not P25, skipped without the native receiver's 1.2 s window. Anything else: the native receiver as before.
Chosen on the P25 search screen (held 0: fast -> fast + soft probe -> classic); off by default, and off it changes
nothing. Our code is in integration/radiofoundry/spectrum/ (p25soft.*, radio_p25soft.*) and the P25 search's own files
(p25search.*, ui_p25search.c, native_lab.c); upstream files change only through the hunks below. Called last from
plus_features.prepare(); a module of its own so the shared prepare list gains one line. HARDWARE-UNVERIFIED."""

SP = 'integration/radiofoundry/spectrum/'


def prepare_softp25(transform, install):
    for f in ('p25soft.c', 'p25soft.h', 'radio_p25soft.c', 'radio_p25soft.h'):
        install('core/radio/' + f, SP + f)

    transform('config/features.mk', [
        ('$(error SEARCH_HYBRID needs SEARCH_SURE=y)\nendif\nendif\n',
         '$(error SEARCH_HYBRID needs SEARCH_SURE=y)\nendif\nendif\n'
         'CONFIG_SEARCH_SOFTP25 ?= n\nFEATURES += SEARCH_SOFTP25\n'
         'ifeq ($(CONFIG_SEARCH_SOFTP25),y)\nifneq ($(CONFIG_SEARCH_HYBRID)$(CONFIG_P25_SEARCH)$(CONFIG_NXDN),yyy)\n'
         '$(error SEARCH_SOFTP25 needs SEARCH_HYBRID=y, P25_SEARCH=y and NXDN=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_SEARCH_HYBRID\n',
         '#ifndef CONFIG_SEARCH_SOFTP25\n#define CONFIG_SEARCH_SOFTP25 0\t/* DDM32 Plus P25 soft probe (tools/plus_softp25.py) */\n'
         '#endif\n#ifndef CONFIG_SEARCH_HYBRID\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#if CONFIG_P25_SEARCH\n#include "radio/p25search.h"\n#endif\n',
         '#if CONFIG_P25_SEARCH\n#include "radio/p25search.h"\n#endif\n'
         '#if CONFIG_SEARCH_SOFTP25\n#include "radio/radio_p25soft.h"\t// DDM32 Plus P25 soft probe\n#endif\n'),
        # every poll: the soft probe of a hit runs before the search's block (which tunes the next step after a "not
        # P25"); with none pending it gives the ring and the flat path back
        ('\tif (p25s_on()) {\n\t\tstatic uint32_t p25s_shown_ms, p25s_epoch_seen;\n',
         '#if CONFIG_SEARCH_SOFTP25\n'
         '\tradio_p25soft_poll(now_ms);\t// DDM32 Plus: the P25 search\'s soft probe (radio/radio_p25soft.h)\n'
         '#endif\n'
         '\tif (p25s_on()) {\n\t\tstatic uint32_t p25s_shown_ms, p25s_epoch_seen;\n'),
        # a retune of the channel (the search ended by a request) lets a soft probe go while the board is still FM
        ('static void tune(radio_sm_t *sm, uint32_t now_ms)\n{\n',
         'static void tune(radio_sm_t *sm, uint32_t now_ms)\n{\n'
         '#if CONFIG_SEARCH_SOFTP25\n\tradio_p25soft_release();\t// DDM32 Plus: the soft probe\'s ring and flat path\n#endif\n'),
    ])
