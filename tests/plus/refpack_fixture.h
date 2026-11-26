/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host tests of the DDM32 Plus map pack (docs/mappack-design.md G.1): a map pack made in C from the generated tables
 * the test links (turkey_map_data.c, turkey_district_data.c, airfield_data.c, a rpt_data.c; a table the test does not
 * link leaves its sections out), the same bytes tools/refpack.py makes (T3 compares them), and written into the
 * simulated data chip's map pack slot the way the radio takes one (dpk_begin / dpk_put / dpk_commit), so the map, the
 * lists and the refusals run on the pack exactly as on the radio. */
#ifndef REFPACK_FIXTURE_H
#define REFPACK_FIXTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { REFPACK_MAP = 1, REFPACK_RPT = 2, REFPACK_AIR = 4, REFPACK_ALL = 7 };
#define REFPACK_MAX	65472u

/* The payload of the groups asked for that the test links; its length, 0 when it does not fit cap */
size_t refpack_fixture_payload(uint8_t *out, size_t cap, unsigned groups);
/* That payload committed into the inactive map pack slot (header flags: the firmware's hint), the simulated chip made
 * first when there is none; true when it is the pack in force */
bool refpack_fixture_install(unsigned groups);
/* The UI simulator's hook (refpack_sim.inc): false = sim_reset leaves the data chip without a map pack */
extern bool refpack_fixture_auto;

#endif
