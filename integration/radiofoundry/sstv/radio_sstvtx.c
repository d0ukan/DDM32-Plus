/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus SSTV send, the radio task's side (radio_sstvtx.h): the generator on the heap, what the screen shows. */
#include "config.h"
#if CONFIG_SSTV_TX
#include <stddef.h>
#include "radio/radio_sstvtx.h"
#include "FreeRTOS.h"
#include "task.h"

static sstv_tx_t *volatile gen;		/* RTOS heap while sending; read by the tone path's interrupt */
static radio_sstvtx_info_t info;

/* the info, its seq moved when it changed (always for an end: the screen tells each refusal) */
static void publish(const radio_sstvtx_info_t *now, bool always)
{
	if (!always && now->state == info.state && now->mode == info.mode && now->why == info.why && now->deny == info.deny &&
	    now->line == info.line && now->lines == info.lines)
		return;
	taskENTER_CRITICAL();
	uint16_t seq = (uint16_t)(info.seq + 1u);
	info = *now;
	info.seq = seq;
	taskEXIT_CRITICAL();
}

bool radio_sstvtx_begin(uint8_t mode, const char *call)
{
	if (gen != NULL)
		return false;
	sstv_tx_t *g = pvPortMalloc(sizeof *g);
	if (g == NULL)
		return false;
	sstv_tx_init(g, mode, call);
	gen = g;
	radio_sstvtx_info_t now = { SSTVTX_KEYUP, g->mode, SSTVTX_R_NONE, 0, 0, sstv_tx_lines(g->mode), 0 };
	publish(&now, false);
	return true;
}

uint16_t radio_sstvtx_gen(int16_t *out, uint16_t n, int16_t amp)
{
	sstv_tx_t *g = gen;
	return g != NULL ? sstv_tx_pcm(g, out, n, amp) : 0;
}

void radio_sstvtx_state(uint8_t state)
{
	sstv_tx_t *g = gen;
	radio_sstvtx_info_t now = info;
	now.state = state;
	if (g != NULL)
		now.line = sstv_tx_line(g);
	publish(&now, false);
}

void radio_sstvtx_end(uint8_t state, uint8_t why, uint8_t deny)
{
	sstv_tx_t *g = gen;
	radio_sstvtx_info_t now = info;
	if (g != NULL) {
		now.line = sstv_tx_line(g);
		gen = NULL;	/* (the tone path is stopped before this: its interrupt no longer reads it) */
		vPortFree(g);
	}
	now.state = state;
	now.why = why;
	now.deny = deny;
	publish(&now, true);
}

void radio_sstvtx_info(radio_sstvtx_info_t *out)
{
	taskENTER_CRITICAL();
	*out = info;
	taskEXIT_CRITICAL();
}

uint16_t radio_sstvtx_seq(void)
{
	return info.seq;
}
#endif
