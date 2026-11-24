/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus P25 soft probe, radio task side (radio_p25soft.h, docs/p25-soft-probe-design.md). Radio task only. */
#include "config.h"
#if CONFIG_SEARCH_SOFTP25
#if !CONFIG_NXDN || !CONFIG_P25_SEARCH
#error "SEARCH_SOFTP25 needs NXDN (the flat FM receive path) and P25_SEARCH"
#endif
#include <string.h>
#include "radio/radio_p25soft.h"
#include "radio/radio_nxdn.h"
#include "radio/p25search.h"
#include "radio/p25soft.h"
#include "board.h"
#include "FreeRTOS.h"
#if CONFIG_APRS_RX
#include "aprs/aprs.h"
#endif

#define READ_N		64u
/* Samples a "not P25" needs: 400 ms, two LDU frame syncs and their NIDs */
#define P25SOFT_MIN_SAMPLES	(400u * P25SOFT_FS / 1000u)

typedef struct {
	p25soft_t p;
	int16_t buf[READ_N];
} soft_state_t;

static soft_state_t *st;	/* while a soft probe listens */
static uint32_t start_ms;

static void let_go(void)
{
	if (st == NULL)
		return;
	if (board.afsk != NULL && board.afsk->rx_stop != NULL)
		board.afsk->rx_stop();
	board_nxdn_flat(false);
	vPortFree(st);
	st = NULL;
}

void radio_p25soft_release(void)
{
	let_go();
}

/* The verdict to the engine, after the ring and the flat path were given back */
static void verdict(uint8_t v, uint16_t nac, uint32_t now_ms)
{
	let_go();
	(void)p25s_soft_verdict(v, nac, now_ms);	/* NONE: the radio core's sweep block tunes the next step */
}

static bool start(uint32_t now_ms)
{
	const hal_afsk_t *a = board.afsk;
	if (a == NULL || a->rx_start == NULL || a->rx_read == NULL || a->rx_rate_hz != P25SOFT_FS)
		return false;
#if CONFIG_APRS_RX
	/* APRS receive (an FM channel on the APRS frequency, or DOTA over FM) holds the ring: our rx_start would fail
	 * (hrc_fm_rx_start refuses a running ring) and its listen(false) would take APRS's route away, and our rx_stop
	 * would leave APRS deaf. The native receiver probes as before */
	if (aprs_rx_listening())
		return false;
#endif
	st = pvPortMalloc(sizeof *st);
	if (st == NULL)
		return false;
	p25soft_init(&st->p);
	if (!board_nxdn_flat(true) || !a->rx_start()) {
		board_nxdn_flat(false);
		vPortFree(st);
		st = NULL;
		return false;
	}
	start_ms = now_ms;
	return true;
}

void radio_p25soft_poll(uint32_t now_ms)
{
	if (!p25s_soft_pending()) {
		let_go();	/* the search stopped or moved on meanwhile */
		return;
	}
	if (st == NULL) {
		/* A weak carrier: what the soft probe misses there may still be P25 (the native receiver decides, as before) */
		if (p25s_probe_margin() < P25SOFT_TRUST_DB || !start(now_ms)) {
			verdict(P25S_SOFT_UNSURE, 0, now_ms);
			return;
		}
	}
	bool skip = now_ms - start_ms < P25SOFT_SKIP_MS;
	for (unsigned guard = 0; guard < 32u; guard++) {
		uint16_t n = board.afsk->rx_read(st->buf, READ_N);
		if (n == 0)
			break;
		if (!skip && p25soft_samples(&st->p, st->buf, n))
			break;
	}
	if (p25soft_p25(&st->p))
		verdict(P25S_SOFT_P25, st->p.nac, now_ms);
	else if (now_ms - start_ms >= P25SOFT_WINDOW_MS)
		/* "not P25" only when the ring really brought the window's samples (two frame syncs' worth) and not one sync
		 * was seen: a sync whose NID did not decode (a receive path that blurs the symbols more than modelled) is
		 * left to the native receiver rather than skipped (no non-P25 model signal shows a sync) */
		verdict(p25soft_hint(&st->p) || st->p.syncs != 0 || st->p.count < P25SOFT_MIN_SAMPLES ? P25S_SOFT_UNSURE :
			P25S_SOFT_NONE, 0, now_ms);
}
#endif
