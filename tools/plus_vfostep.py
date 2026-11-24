#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus VFO step key (CONFIG_VFO_STEPKEY, branch vfostep, v1.0.2 candidate; docs/plus.md "VFO step key"). The
owner's request of 2026-11-24: on Home with the VFO selected, the tuning step changes with one key shown on the screen,
without the menus (Settings > Talk > VFO step). Every digit is a frequency digit in the VFO, * is Quick settings, # the
caller / context key and the arrows leave the VFO for a zone, so the key is the short A/B press (SK2): with dual watch
off it only started dual watch, which the held key does as well. Home's hint row names it. Called last from
plus_features.prepare(); a module of its own so the shared prepare list gains one line."""

VS = 'integration/radiofoundry/vfostep/'


def prepare_vfostep(transform, install):
    install('core/ui/ui_vfostep.c', VS + 'ui_vfostep.c')

    transform('config/features.mk', [
        ('$(error SCAN_FAST needs SCAN=y)\nendif\nendif\n',
         '$(error SCAN_FAST needs SCAN=y)\nendif\nendif\n'
         'CONFIG_VFO_STEPKEY ?= n\nFEATURES += VFO_STEPKEY\n'
         'ifeq ($(CONFIG_VFO_STEPKEY),y)\nifneq ($(CONFIG_VFO)$(CONFIG_DUAL_WATCH)$(CONFIG_AM_RX),yyy)\n'
         '$(error VFO_STEPKEY needs VFO=y, DUAL_WATCH=y and AM_RX=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_SCAN_FAST\n',
         '#ifndef CONFIG_VFO_STEPKEY\n#define CONFIG_VFO_STEPKEY 0\t/* DDM32 Plus VFO step key (tools/plus_vfostep.py) */\n'
         '#endif\n#ifndef CONFIG_SCAN_FAST\n'),
    ])
    transform('core/ui/ui_nav.h', [
        ('bool ui_has_action(uint8_t action);',
         'bool ui_has_action(uint8_t action);\n'
         'bool ui_vfostep_key(const input_event_t *ev);\t// DDM32 Plus: short A/B on the VFO steps its tuning step\n'),
    ])
    transform('core/ui/ui_screens.h', [
        ('void ui_draw_hint(const char *text, bool toast);',
         'void ui_draw_hint(const char *text, bool toast);\n'
         'const char *ui_vfostep_hint(void);\t// DDM32 Plus: Home\'s VFO hint with the step key named, or NULL'),
    ])
    transform('core/ui/ui_app.c', [
        # the key: on Home only, before the A/B key's own work (the other screens, dual watch, a held key are unchanged)
        ('\t\tcase ACT_AB:\n\t\t\tui_dual_ab_key();\n\t\t\treturn;\n',
         '\t\tcase ACT_AB:\n'
         '#if CONFIG_VFO_STEPKEY\n'
         '\t\t\tif (stack[sp].scr == SCR_HOME && ui_vfostep_key(ev))\t// DDM32 Plus: the VFO\'s step\n\t\t\t\treturn;\n'
         '#endif\n'
         '\t\t\tui_dual_ab_key();\n\t\t\treturn;\n'),
    ])
    transform('core/ui/ui_screens.c', [
        ('\tif (v->vfo)\n\t\treturn TR("0-9 freq  * Quick  Up/Dn");\n',
         '\tif (v->vfo) {\n'
         '#if CONFIG_VFO_STEPKEY\n'
         '\t\tconst char *sk = ui_vfostep_hint();\t// DDM32 Plus: names the step key\n\t\tif (sk != NULL)\n\t\t\treturn sk;\n'
         '#endif\n'
         '\t\treturn TR("0-9 freq  * Quick  Up/Dn");\n\t}\n'),
    ])
