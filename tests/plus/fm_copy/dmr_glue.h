/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host double of platform/hrc7000/dmr_glue.h for tests/plus/test_fm_copy.c: the two calls fm_rx_audio.c makes */
#ifndef FM_COPY_DMR_GLUE_H
#define FM_COPY_DMR_GLUE_H
#include <stdbool.h>
bool hrc_dmr_fm_rx_hook(void (*fn)(void));
bool hrc_dmr_fm_rx_kick(void);
#endif
