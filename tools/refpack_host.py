# SPDX-License-Identifier: GPL-3.0-or-later
"""Host test builds of the DDM32 Plus map pack era (docs/mappack-design.md G.1): since 1.1 the firmware reads the map,
the districts, the repeaters and the airfields from the map pack on the data chip, so a host test that links those
screens or that math also links the pack's reader and the fixture that writes the pack (tests/plus/refpack_fixture.c)
on the simulated chip; the generated tables come from where they are made (the committed map / airfield .c files,
build/rpt/rpt_data.c), no longer from the prepared tree, and the TX refusal set from build/rpt/rpt_freqs.c.

patch(argv, ROOT, APP, host) takes a compile command and returns it with all of that in place (each file once)."""
from pathlib import Path

TABLES = {
    'turkey_map_data.c': 'integration/radiofoundry/map/turkey_map_data.c',
    'turkey_district_data.c': 'integration/radiofoundry/map/turkey_district_data.c',
    'airfield_data.c': 'integration/radiofoundry/air/airfield_data.c',
    'rpt_data.c': 'build/rpt/rpt_data.c',
}
USERS = ('map.c', 'rpt.c', 'airfield.c', 'air_pack.c', 'ui_map.c', 'ui_rpt.c', 'ui_airfield.c',
         'datapack.c')


def patch(argv, ROOT=None, APP=None, host=None, rpt_c=None):
    """argv with the map pack's reader, its fixture and the tables; rpt_c: a test's own rpt_data.c instead of the
    build's (its refusal set is then the build's rpt_freqs.c too, unless the test passes its own)"""
    ROOT = Path(ROOT) if ROOT else Path(__file__).resolve().parents[1]
    APP = Path(APP) if APP else ROOT / 'build/radiofoundry-p25'
    host = Path(host) if host else APP / 'tests/host'
    out = []
    names = set()
    if not argv or Path(str(argv[0])).name not in ('clang', 'cc', 'gcc', 'clang++', 'c++', 'g++'):
        return [str(a) for a in argv]		# a compile command only (never a generator's output path)
    for a in argv:
        s = str(a)
        base = Path(s).name
        if base in TABLES and s.startswith(str(APP)):
            s = str(rpt_c) if base == 'rpt_data.c' and rpt_c else str(ROOT / TABLES[base])
        out.append(s)
        names.add(Path(s).name)
    if not any(n in names for n in USERS):
        return out
    core = APP / 'core'
    extra = [core / 'storage/datapack.c', core / 'storage/ref_pack.c', core / 'storage/storage.c', core / 'ui/fmt.c',
             host / 'sim/sim_flash.c', ROOT / 'tests/plus/refpack_fixture.c']
    if 'rpt.c' in names and not any(n.endswith('freqs.c') for n in names):	# a test's own set wins
        extra.append(ROOT / 'build/rpt/rpt_freqs.c')
    for e in extra:
        if e.name not in names:
            out.append(str(e))
            names.add(e.name)
    for flag in ('-DCONFIG_DATAPACK=1', '-I' + str(host), '-I' + str(ROOT / 'tests/plus'), '-I' + str(core)):
        if flag not in out:
            out.insert(1, flag)
    return out
