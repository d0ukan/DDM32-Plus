/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus user data packs' UI flows (tests/plus/test_ui_datapack.c): the air items' simulator
 * (tests/plus/ui_sim_air.c: a radio that receives AM, the test's position, the radio core's air watch) with a
 * simulated 16 MiB data chip on the board, so the packs are written and read where the radio keeps them
 * (core/storage/datapack.c, 0xE70000-0xEFFFFF). */
#include "ui_sim_air.c"
#include "sim/sim_flash.h"
#include "ui_sim_dpk.h"

void dpk_sim_setup(void)
{
	air_sim_setup();
	sim_flash_create(0x1000000u, NULL);
	board.flash = &sim_flash;
	if (refpack_fixture_auto)	/* the new chip gets the map pack again (refpack_sim.inc) */
		(void)refpack_fixture_install(REFPACK_ALL);
}
