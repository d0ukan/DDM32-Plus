#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus search log on the PC (docs/plus.md "Search log on the PC"; owner 2026-11-21: leave the radio and the PC
on overnight with a search running and keep every call it finds).

Two debug link view sections, read with GET_VIEW (0x09) like the live panel's: "p25s" and "dmrs", the P25 and DMR
searches' state and found lists (integration/radiofoundry/spectrum/searchlog_view.c). Read only, and not counted as
a look: a search still stops 5 s after its screen was left (p25s_peek / dmrs_peek beside the snapshots). The PC
programmers poll them every second and keep the log; the radio keeps its own 16 entries a search (8 up to
1.0.5) and a section carries the newest 8 (searchlog_view.c SEARCHLOG_ROWS).

Called from plus_features.prepare(); a module of its own so the shared prepare list gains one line. Upstream files
change only through the hunks below; the new sections go last, so every earlier section keeps its number."""

ENUM_OLD = '\tDL_VIEW_CPS,\t\t// the CPS shims\' counters (were in DL_VIEW_STORAGE until view version 5)\n\tDL_VIEW_COUNT\n'
ENUM_NEW = ('\tDL_VIEW_CPS,\t\t// the CPS shims\' counters (were in DL_VIEW_STORAGE until view version 5)\n'
            '\tDL_VIEW_P25S,\t\t// DDM32 Plus: the P25 search\'s state and found list (searchlog_view.c)\n'
            '\tDL_VIEW_DMRS,\t\t// DDM32 Plus: the DMR search\'s state and found list (searchlog_view.c)\n'
            '\tDL_VIEW_COUNT\n')


def prepare_searchlog(transform, install):
    install('core/radio/searchlog_view.c', 'integration/radiofoundry/spectrum/searchlog_view.c')
    install('core/radio/searchlog_view.h', 'integration/radiofoundry/spectrum/searchlog_view.h')
    transform('core/debuglink/debuglink.h', [(ENUM_OLD, ENUM_NEW)])
    transform('core/debuglink/debuglink_view.c', [
        ('#include "net/ppp.h"\n#endif\n#endif\n',
         '#include "net/ppp.h"\n#endif\n#endif\n'
         '#if CONFIG_PC_LIVE\n#include "radio/searchlog_view.h"\t// DDM32 Plus search log\n#endif\n'),
        ('\t\t\t\t\t\t  "net_link", "cps" };\n',
         '\t\t\t\t\t\t  "net_link", "cps", "p25s", "dmrs" };\n'),
        ('\tcase DL_VIEW_CPS:\n\t\tcps_section(t);\n\t\tbreak;\n',
         '\tcase DL_VIEW_CPS:\n\t\tcps_section(t);\n\t\tbreak;\n'
         # the plus build only: the classic build installs no searchlog_view.c (its sections then stay empty)
         '#if CONFIG_PC_LIVE\n'
         '\tcase DL_VIEW_P25S:\n\t\tsearchlog_view_p25(t);\t// DDM32 Plus search log\n\t\tbreak;\n'
         '\tcase DL_VIEW_DMRS:\n\t\tsearchlog_view_dmr(t);\n\t\tbreak;\n#endif\n'),
    ])
