#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus dual watch / multi-watch TX target fix (docs/plus.md "Multi-watch", "The TX target"): the owner's report on
v1.0.0-beta.7 (A DMR, B P25, C FM; PTT on C keyed P25 on B, PTT on the P25 letter did nothing, TALKING stayed on
after release, the P25 letter went deaf until a power cycle). Called from plus_features.prepare(); a module of its own
so the shared prepare list gains one line. The native side is integration/radiofoundry/native/native_lab.c (our file);
the radio core only gains the hunks below:

- radio_sm_dual_away(): the native P25 owner, which sees PTT before the radio core (radio_task.c), asks whether the
  receiver is on a letter other than the selected one and has the radio core tune the selected one first (dual_talk,
  as the radio core's own PTT does). Before, native_lab.c looked at the channel the receiver happened to be on: a PTT
  with C selected while the ring was on (or held by a call on) B keyed P25 on B, and with B selected while the ring
  was on A the radio core keyed an FM carrier on B's frequency.
- ptt_down(): never an FM carrier on a P25 / YSF letter, whatever reaches it (refused, TXDENY_MODE).
No switch: a fix wherever dual watch and P25 channels are built."""


def prepare_mwfix(transform):
    transform('core/radio/radio_sm.h', [
        ('#if CONFIG_P25_CHANNELS\nvoid radio_sm_native_maintenance(radio_sm_t *sm, uint32_t now_ms);\n#endif\n',
         '#if CONFIG_P25_CHANNELS\nvoid radio_sm_native_maintenance(radio_sm_t *sm, uint32_t now_ms);\n'
         '#if CONFIG_DUAL_WATCH\n'
         '// DDM32 Plus: PTT in dual watch / the ring talks on the selected letter. 0: the receiver is on it (or no watch);\n'
         '// else (tune) tuned there now: 1, or 2 when it can\'t be read (the watch ended, the PTT refused: TXDENY_MODE)\n'
         'uint8_t radio_sm_dual_away(radio_sm_t *sm, bool tune, uint32_t now_ms);\n'
         '#endif\n#endif\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('\tscan_tuned(&sm->scan, now_ms);\n\tscan_publish(sm);\n}\n\n'
         '// Requests that act on the channel the receiver is on: in dual watch that must be the selected one\n',
         '\tscan_tuned(&sm->scan, now_ms);\n\tscan_publish(sm);\n}\n\n'
         '#if CONFIG_P25_CHANNELS\n'
         '// DDM32 Plus (docs/plus.md "Multi-watch", the TX target): the native P25 owner sees PTT first (radio_task.c) and\n'
         '// asks here, so it never keys the letter the receiver happens to be on. tune: the selected one is tuned, as\n'
         '// dual_talk does for the radio core\'s own PTT; one that can\'t be read ends the watch and refuses the PTT\n'
         'uint8_t radio_sm_dual_away(radio_sm_t *sm, bool tune, uint32_t now_ms)\n{\n'
         '\tif (!dual_running(sm) || dual_here(sm, sm->dual_sel)) {\n\t\treturn 0;\n\t}\n'
         '\tif (tune) {\n\t\tdual_talk(sm, now_ms);\n'
         '\t\tif (!dual_running(sm)) {\n\t\t\tsm->ptt = true;\n\t\t\tdeny(sm, TXDENY_MODE);\n\t\t\treturn 2;\n\t\t}\n\t}\n'
         '\treturn 1;\n}\n#endif\n\n'
         '// Requests that act on the channel the receiver is on: in dual watch that must be the selected one\n'),
        ('\tif (dual_running(sm)) {\n\t\tdual_talk(sm, now_ms);\n\t} else\n',
         '\tif (dual_running(sm)) {\n\t\tdual_talk(sm, now_ms);\n'
         '#if CONFIG_P25_CHANNELS\n'
         '\t\tif (codeplug_mode_p25(sm->view.ch.mode)) {\n'
         '\t\t\tsm->ptt = true;\t// DDM32 Plus: never an FM carrier on a P25 letter (native_lab.c keys P25)\n'
         '\t\t\tdeny(sm, TXDENY_MODE);\n\t\t\treturn;\n\t\t}\n'
         '#endif\n'
         '\t} else\n'),
    ])
