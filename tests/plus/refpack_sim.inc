/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Included right after sim/ui_sim.c by the Plus simulator wrappers whose screens read the map pack (the wrapper
 * includes ui_sim.c with sim_reset renamed sim_base_reset): every simulator reset gives the board a simulated data
 * chip and the map pack made from the tables the test links (refpack_fixture.h), unless the test turned
 * refpack_fixture_auto off (the "no map pack" flows). */
#include "sim/sim_flash.h"
#include "storage/datapack.h"
#include "refpack_fixture.h"

void sim_reset(const sim_geom_t *g, sim_map_t map);
void sim_reset(const sim_geom_t *g, sim_map_t map)
{
	sim_base_reset(g, map);
	if (board.flash == NULL || sim_flash_data == NULL) {
		if (sim_flash_data == NULL)
			sim_flash_create(0x1000000u, NULL);
		board.flash = &sim_flash;
	}
	dpk_scan();
	if (refpack_fixture_auto && dpk_active(DPK_REF) == NULL)
		(void)refpack_fixture_install(REFPACK_ALL);
}
