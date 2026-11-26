#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus user data packs (CONFIG_DATAPACK, docs/r14.md 7, docs/plus.md "Data packs"): data the user builds from
sources they download themselves, kept on the data chip in 0xE70000-0xEFFFFF (the one region AGENTS.md rule 2 opens to
the firmware), two slots a pack type, the header written last. Called last from plus_features.prepare(). Our code is in
integration/radiofoundry/datapack/ (installed into core/storage/); upstream files change only through the hunks
below."""

PACK = 'integration/radiofoundry/datapack/'

# (installed path, our file)
FILES = [
    ('core/storage/datapack.h', 'datapack.h'),
    ('core/storage/datapack.c', 'datapack.c'),
    # the map pack (type 3, docs/mappack-design.md): its validation and the UI task's readers
    ('core/storage/ref_pack.h', 'ref_pack.h'),
    ('core/storage/ref_pack.c', 'ref_pack.c'),
    ('core/ui/air_pack.h', 'air_pack.h'),
    ('core/ui/air_pack.c', 'air_pack.c'),
]

ON = '#if CONFIG_DATAPACK\n'


def prepare_datapack(transform, install):
    for dst, src in FILES:
        install(dst, PACK + src)

    transform('config/features.mk', [
        ('CONFIG_CLOCK_TOOLS ?= n\nFEATURES += CLOCK_TOOLS\n',
         'CONFIG_CLOCK_TOOLS ?= n\nFEATURES += CLOCK_TOOLS\n'
         'CONFIG_DATAPACK ?= n\nFEATURES += DATAPACK\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_CLOCK_TOOLS\n#define CONFIG_CLOCK_TOOLS 0\n#endif\n',
         '#ifndef CONFIG_CLOCK_TOOLS\n#define CONFIG_CLOCK_TOOLS 0\n#endif\n'
         '#ifndef CONFIG_DATAPACK\n#define CONFIG_DATAPACK 0\t/* DDM32 Plus user data packs (docs/r14.md 7) */\n#endif\n'),
    ])
    # Which pack is in force: checked at boot, once the storage layer is up (a power cut's sector rewrite finished)
    transform('core/app/app.c', [
        ('#include "storage/storage.h"\n',
         '#include "storage/storage.h"\n' + ON + '#include "storage/datapack.h"\n#endif\n'),
        ('\tstorage_init();\t\t\t// finishes a codeplug sector rewrite a power cut interrupted\n',
         '\tstorage_init();\t\t\t// finishes a codeplug sector rewrite a power cut interrupted\n'
         + ON + '\tdpk_scan();\t\t\t// DDM32 Plus: the user data packs in force (docs/r14.md 7)\n#endif\n'),
    ])

    # A DMR caller's name: the codeplug's contact, then the user's RadioID pack, then the built-in table (owner
    # build), then the CPS-uploaded database (radio_sm.c find_name)
    transform('core/radio/radio_sm.c', [
        ('#include "codeplug/dmr_iddb.h"\n',
         '#include "codeplug/dmr_iddb.h"\n' + ON + '#include "storage/datapack.h"\n#endif\n'),
        ('#if CONFIG_ID_DB\n\t} else if (!group && iddb_text(id, n->name, sizeof(n->name))) {\n',
         ON + '\t} else if (!group && dpk_id_text(id, n->name, sizeof(n->name))) {\n'
         '\t\t// DDM32 Plus: the user\'s RadioID data pack (docs/r14.md 7)\n#endif\n'
         '#if CONFIG_ID_DB\n\t} else if (!group && iddb_text(id, n->name, sizeof(n->name))) {\n'),
    ])
    # Settings > About: each pack type's line ("RadioID pack: RadioID TR, 2026-11-17, 98 KB", "none", "damaged",
    # "too big" for a 1.0.x RadioID pack over 192 KB, "for another version" for a map pack of another firmware)
    transform('core/ui/ui_menus.c', [
        ('#include "rfoundry_about.h"\n', '#include "rfoundry_about.h"\n' + ON + '#include "storage/datapack.h"\n#endif\n'),
        # The About text is built in the heap while the page draws, not in a static buffer: 752 bytes of static RAM
        # back for the whole radio (the page's text lives only during its draw; without room it says so)
        ('static const char *about_text(void)\n{\n\tstatic char t[640 + 48 * CONFIG_ID_DB];\t// DM32 Plus: the ID database line\n'
         '\tsbuf_t b;\n\tsb_init(&b, t, sizeof t);',
         # three pack lines (RadioID, air band, map pack), the longest Turkish state text each (heap, while About draws)
         '#define ABOUT_BYTES\t(640u + 48u * CONFIG_ID_DB + 220u * CONFIG_DATAPACK)\t// DM32 Plus: the ID database, the packs\n'
         '#ifdef __csky__\n#include "FreeRTOS.h"\n#define about_alloc\tpvPortMalloc\n#define about_free\tvPortFree\n#else\n'
         '#include <stdlib.h>\n#define about_alloc\tmalloc\n#define about_free\tfree\n#endif\n'
         'static const char *about_text(char *t)\n{\n\tsbuf_t b;\n\tsb_init(&b, t, ABOUT_BYTES);'),
        ('\tabout_max = (uint16_t)ui_draw_text_page(ui_view(), &ui_st, TR("About"), about_text(), &about_first);',
         '\tchar *t = about_alloc(ABOUT_BYTES);\t// DDM32 Plus: the text only while the page draws\n'
         '\tabout_max = (uint16_t)ui_draw_text_page(ui_view(), &ui_st, TR("About"), t != NULL ? about_text(t) : TR("No room"),\n'
         '\t\t\t\t\t\t &about_first);\n\tabout_free(t);'),
        ('\tsb_str(&b, TR("AMBE+2 codec: DVSI (stock)"));',
         ON + '\tfor (uint8_t k = 1; k <= DPK_TYPES; k++) {\t// DDM32 Plus: the user data packs (docs/r14.md 7)\n'
         '\t\tchar d[40];\n'
         '\t\tsb_str(&b, k == DPK_RADIOID ? TR("RadioID pack: ") : k == DPK_AIR ? TR("Air band pack: ") : TR("Map pack: "));\n'
         '\t\tuint8_t st = dpk_state(k);\n'
         '\t\tsb_str(&b, dpk_about(k, d, sizeof d) ? d : st == DPK_BAD ? TR("damaged, not used") :\n'
         '\t\t       st == DPK_BIG ? TR("too big for this firmware, rebuild it") :\n'
         '\t\t       st == DPK_OTHER ? TR("for another version, load the one for this firmware") : TR("none"));\n'
         '\t\tsb_chr(&b, \'\\n\');\n\t}\n#endif\n'
         '\tsb_str(&b, TR("AMBE+2 codec: DVSI (stock)"));'),
    ])
    # From the PC: the CPS's flash area 1 takes physical addresses, and the region is inside the open-firmware window
    # (0x720000-0xEFFFFF), so a pack is written with the CPS's own sector writes (journaled, power-cut safe): its
    # payload sectors, then its header's sector (docs/plus.md "User data packs"). At the session's close the slots are
    # checked again, as the CPS-written ID database is re-read
    transform('core/cps/cps_openfw.c', [
        ('#include "cps/cps_openfw.h"\n', '#include "cps/cps_openfw.h"\n' + ON + '#include "storage/datapack.h"\n#endif\n'),
        ('\tcase 5:\t\t// close: the codeplug is re-read if it was written\n\t\tend_session();\n',
         '\tcase 5:\t\t// close: the codeplug is re-read if it was written\n\t\tend_session();\n'
         + ON + '\t\tdpk_scan();\t// DDM32 Plus: a data pack the PC wrote is in force now (docs/r14.md 7)\n#endif\n'),
    ])
