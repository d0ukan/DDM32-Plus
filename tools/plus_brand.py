#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus: the product name and the author's signature on the radio, and the public build profile
(docs/plus.md "Name and public build"; owner's request 2026-11-19). Called last from plus_features.prepare(); its own
module so the shared prepare list gains one line.

Name: the splash wordmark reads "DDM32 Plus" (art/wordmark.txt) with the callsign TA2DOU small at the bottom centre
(integration/radiofoundry/art/splash_plus.c); About opens with "DDM32 Plus . d0ukan . TA2DOU" (art.h ART_PRODUCT,
ART_AUTHOR, ART_CALLSIGN; names, never translated), then "Based on Open25-DM32 Classic" in place of Classic r13's
"Firmware: Open25-DM32 Classic" and its issue tracker (a Plus image is not Classic). Upstream's credits stay:
"License: MIT, (c) 2026 Open25-DM32 contributors", "Based on RadioFoundry" and its MIT line, authors, third party;
"DDM32 Plus: GPL-3.0-or-later" goes right above them (LICENSING.md) and, with the repeater list, its sources
("Repeater data: RadioID.net, TRAC") at the end. Internal names (CONFIG_*, files, --plus, prepare_*) do not change.

Public build (`build_radiofoundry.py --plus --public`, build/dm32-p25-plus-public/): since round 14 (data packs) every
profile is built without the built-in table (the names come from the user's RadioID data pack, docs/r14.md 7); the
public profile keeps its guards: without the RadioID.net user / DMR ID database (CONFIG_ID_DB=n: radio/iddb_data.c compiles to nothing, nothing in the image comes
from research-data/user.csv). The builder refuses a --public command that would turn the database on, and after the
link refuses an ELF that holds any of its tables or code (public_audit). The repeater list stays in.

  python3 tools/plus_brand.py --self-test     the checks above on made-up inputs (make test)
"""
import sys

# Everything tools/iddb.py generates and integration/radiofoundry/iddb/iddb.c defines: none may be in a public ELF
IDDB_SYMBOLS = ('iddb_info', 'iddb_index', 'iddb_records', 'iddb_name_off', 'iddb_names', 'iddb_lookup',
                'iddb_text', 'iddb_call')


def prepare_brand(transform):
    # Classic r13's About: "Firmware: Open25-DM32 Classic", its issue tracker, then "License: MIT, (c) 2026 Open25-DM32
    # contributors" and "Based on RadioFoundry". A Plus image is not Open25-DM32 Classic (AGENTS.md: never published
    # under that name) and its bugs are not upstream's to triage, so Plus names itself and credits Classic instead.
    # Worst case text (Turkish, every line, a 5-digit ID count): 540 B of the 688 B buffer, so the buffer stays
    transform('core/ui/ui_menus.c', [
        ('#include "rfoundry_about.h"\n',
         '#include "rfoundry_about.h"\n#if CONFIG_REPEATERS\n#include "rpt.h"\n#endif\n'),
        ('\tsb_str(&b, TR("Firmware: "));\n\tsb_str(&b, "Open25-DM32 Classic\\n");\n'
         '\tsb_str(&b, "Issues:\\ngithub.com/tylerwatt12/\\nOpen25-DM32/issues\\n");\n',
         '#if CONFIG_PLUS_ART\n'
         '\t// DDM32 Plus: the product, its author and his callsign first (names, not translated), then what it is\n'
         '\t// built on; upstream\'s issue tracker is for Classic builds only\n'
         '\tsb_str(&b, ART_PRODUCT " " GLYPH_DOT " " ART_AUTHOR " " GLYPH_DOT " " ART_CALLSIGN "\\n");\n'
         '\tsb_str(&b, "Based on Open25-DM32 Classic\\n");\n'
         '#else\n'
         '\tsb_str(&b, TR("Firmware: "));\n\tsb_str(&b, "Open25-DM32 Classic\\n");\n'
         '\tsb_str(&b, "Issues:\\ngithub.com/tylerwatt12/\\nOpen25-DM32/issues\\n");\n'
         '#endif\n'),
        ('\tsb_str(&b, TR("License: "));\n\tsb_str(&b, "MIT, (c) 2026 Open25-DM32 contributors\\n");\n',
         '#if CONFIG_PLUS_ART\n'
         '\tsb_str(&b, ART_PRODUCT ": GPL-3.0-or-later\\n");\t// LICENSING.md: a Plus image is GPL-3.0-or-later\n'
         '#endif\n'
         '\tsb_str(&b, TR("License: "));\n\tsb_str(&b, "MIT, (c) 2026 Open25-DM32 contributors\\n");\n'),
        ('\treturn t;\n}\n\nstatic void about_enter(void)\n',
         '#if CONFIG_REPEATERS\n'
         '\t// DDM32 Plus: where the map pack\'s repeater list comes from (tools/rpt_list.py: "own" = the TRAC list);\n'
         '\t// "none" without a map pack (the TX refusals keep their own frequency set in program flash)\n'
         '\tsb_chr(&b, \'\\n\');\n'
         '\tsb_str(&b, TR("Repeater data: "));\n'
         '\trpt_info_t ri;\n'
         '\tbool have = rpt_info_get(&ri);\n'
         '\tbool rid = strstr(ri.source, "RadioID") != NULL, own = strncmp(ri.source, "own", 3) == 0;\n'
         '\tsb_str(&b, !have ? TR("none") : ri.sample ? "sample" : rid && own ? "RadioID.net, TRAC" : rid ? "RadioID.net" : "TRAC");\n'
         '#endif\n'
         '\treturn t;\n}\n\nstatic void about_enter(void)\n'),
    ])


def public_command(command):
    """The make command of a --public build: refuses one where the ID database would be on (the last CONFIG_ID_DB
    on the command line is the one make keeps)."""
    ids = [c for c in command if str(c).startswith('CONFIG_ID_DB=')]
    if not ids or ids[-1] != 'CONFIG_ID_DB=n':
        raise SystemExit('public build: CONFIG_ID_DB must be n (the RadioID.net user database never ships)')
    return command


# The tables that moved into the map pack (docs/mappack-design.md E.1): none may be in a Plus ELF
TABLE_SYMBOLS = ('turkey_map_provinces', 'turkey_map_rings', 'turkey_map_steps', 'turkey_map_names',
                 'turkey_district_pos', 'turkey_district_first', 'turkey_district_name_first', 'turkey_district_names',
                 'rpt_table', 'rpt_info', 'air_fields', 'air_freqs', 'air_field_count')


def table_audit(nm_text):
    """csky-elf-nm output of a Plus ELF: refuses one that still links a table the map pack now holds."""
    found = sorted({w[-1] for w in (line.split() for line in nm_text.splitlines()) if w and w[-1] in TABLE_SYMBOLS})
    if found:
        raise SystemExit('Plus build: a map pack table is still in the image (%s)' % ', '.join(found))
    return {'map_pack_tables_absent': True}


def public_audit(nm_text):
    """csky-elf-nm output of a public ELF: refuses one that holds any ID database table or code."""
    found = sorted({w[-1] for w in (line.split() for line in nm_text.splitlines()) if w and w[-1] in IDDB_SYMBOLS})
    if found:
        raise SystemExit('public build: the ID database is in the image (%s)' % ', '.join(found))
    return {'public_id_db_absent': True}


def self_test():
    ok = 0
    public_command(['make', 'CONFIG_ID_DB=y', 'CONFIG_ID_DB=n'])
    for bad in (['make'], ['make', 'CONFIG_ID_DB=y'], ['make', 'CONFIG_ID_DB=n', 'CONFIG_ID_DB=y']):
        try:
            public_command(bad)
        except SystemExit:
            ok += 1
    assert ok == 3, 'public_command let an ID database through'
    clean = '00001000 T main\n00002000 R rpt_info\n00003000 t iddb_like_but_not\n'
    assert public_audit(clean) == {'public_id_db_absent': True}
    for sym in IDDB_SYMBOLS:
        try:
            public_audit(clean + '0001a2b0 00012c3a R %s\n' % sym)
        except SystemExit:
            ok += 1
    assert ok == 3 + len(IDDB_SYMBOLS), 'public_audit missed an ID database symbol'
    # the map pack's tables (T13): a clean ELF passes, each table symbol is refused
    tclean = '00001000 T main\n00002000 T rpt_count\n00003000 t air_get\n00004000 T rpt_freq_listed\n'
    assert table_audit(tclean) == {'map_pack_tables_absent': True}
    for sym in TABLE_SYMBOLS:
        try:
            table_audit(tclean + '0001a2b0 00002c3a R %s\n' % sym)
        except SystemExit:
            ok += 1
    assert ok == 3 + len(IDDB_SYMBOLS) + len(TABLE_SYMBOLS), 'table_audit missed a map pack table'
    # The public build itself, when this tree has one: its ELF holds no ID database, and its manifest says so
    import json
    import subprocess
    from pathlib import Path
    root = Path(__file__).resolve().parents[1]
    out = root / 'build/radiofoundry-p25/build/dm32-p25-plus-public'
    elf, nm = out / 'rfoundry_dm32.elf', root / 'toolchain/bin/csky-elf-nm'
    if elf.is_file() and nm.is_file():
        public_audit(subprocess.check_output([str(nm), str(elf)], text=True))
        manifest = json.loads((out / 'rfoundry_dm32.json').read_text())
        public_command(manifest['command'])
        assert manifest.get('public_enabled') and manifest['audits'].get('public_id_db_absent'), 'public manifest'
        built = 'the public build in %s has no ID database' % out.relative_to(root)
    else:
        built = 'no public build in this tree to check'
    print('plus_brand: public profile checks ok (%d refusals); %s' % (ok, built))


if __name__ == '__main__':
    if sys.argv[1:] == ['--self-test']:
        self_test()
    else:
        sys.exit(__doc__)
