/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The TX refusal set (rpt_freqs.c, docs/mappack-design.md C.7) of tests/plus/test_ui_xband.c's one repeater (output
 * 430.400, input 432.000 MHz), as tools/rpt_list.py would generate it: the test runs with no map pack on the board, so
 * cross-band must refuse both from this program-flash set alone (B1). */
#include "config.h"
#include "ui/rpt.h"

const uint16_t rpt_fq_vhf[] = { 0u };
const uint16_t rpt_fq_vhf_n = 0u;
const uint16_t rpt_fq_uhf[] = { (430400000u - RPT_FQ_UHF_BASE) / RPT_STEP_HZ, (432000000u - RPT_FQ_UHF_BASE) / RPT_STEP_HZ };
const uint16_t rpt_fq_uhf_n = 2u;
