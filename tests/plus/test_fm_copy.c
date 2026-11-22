/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus: the FM receive copy's watchdog (platform/hrc7000/fm_rx_audio.c copy_watch, after RadioFoundry 3d5940b),
 * built by tools/test_rfports.py with CONFIG_APRS_RX=1 and CONFIG_RF_FIXES=1 on the prepared fm_rx_audio.c and host
 * doubles of its platform, RTOS and glue headers (tests/plus/fm_copy). The copy's halves come from the fake system
 * interrupt (the hook the reader registered); the reader polls every 25 ms as the radio task does.
 *
 * Checked: halves every 32 ms never kick; none for 200 ms kicks once, then once every 200 ms while none comes; a kick
 * the glue refuses (DMR, a native session's handler) is not counted; halves dropped for a full ring hold the watchdog
 * off as taken ones do; a stop and start begin the count again. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fm_rx_audio.c"

static uint32_t clock_us = 5000000u;
uint32_t hrc_micros(void) { return clock_us; }

static void (*hook)(void);
bool hrc_dmr_fm_rx_hook(void (*fn)(void)) { hook = fn; return true; }

static unsigned kicks, kick_ok = 1;
bool hrc_dmr_fm_rx_kick(void) { kicks++; return kick_ok != 0; }

static unsigned mask_calls;
void hrc7000_fm_rx_audio_irq(hrc7000_t *d, bool on) { (void)d; (void)on; mask_calls++; }

static uint32_t regs[0x600 / 4];
static uint8_t chip_buf[0x2000];
static hrc7000_t chip = { .regs = regs, .buf = chip_buf };
static int16_t out[RING];

/* ms of time: a half every 32 ms when on, the reader every 25 ms (reading when read) */
static void run_ms(uint32_t ms, bool halves_on, bool read)
{
	static uint32_t half_acc, poll_acc;
	for (uint32_t i = 0; i < ms; i++) {
		clock_us += 1000u;
		if (halves_on && ++half_acc >= 32u) {
			half_acc = 0;
			regs[C7_FM_ADDR_SW / 4] = (regs[C7_FM_ADDR_SW / 4] == 1u) ? 2u : 1u;
			hook();
		}
		if (++poll_acc >= 25u) {
			poll_acc = 0;
			if (read) {
				(void)hrc_fm_rx_read(out, (uint16_t)RING);
			} else {
				(void)hrc_fm_rx_read(out, 0);	/* polled, nothing taken: the ring fills */
			}
		}
	}
}

int main(void)
{
	hrc_fm_rx_attach(&chip);
	assert(hrc_fm_rx_start() && hook != NULL);
	unsigned m0 = mask_calls;

	/* Halves flowing: no kick in 10 s */
	run_ms(10000, true, true);
	assert(kicks == 0 && hrc_fm_rx_restarts() == 0);

	/* The copy stops: kicked once 200 ms after the last half (within a poll), then every 200 ms while none comes */
	/* (the last poll that saw a half is up to one poll after it, and a try comes at the first poll past 200 ms) */
	run_ms(199, false, true);
	assert(kicks == 0);
	run_ms(60, false, true);
	assert(kicks == 1 && hrc_fm_rx_restarts() == 1);
	run_ms(450, false, true);
	assert(kicks >= 2 && kicks <= 3 && hrc_fm_rx_restarts() == kicks);
	unsigned k = kicks;
	/* ... and back: no more */
	run_ms(2000, true, true);
	assert(kicks == k);

	/* The glue refuses (DMR mode or a native session owns the interrupt): tried, not counted */
	kick_ok = 0;
	run_ms(260, false, true);
	assert(kicks == k + 1 && hrc_fm_rx_restarts() == k);
	kick_ok = 1;
	run_ms(1000, true, true);
	assert(kicks == k + 1);

	/* A reader that doesn't drain: halves dropped for a full ring still come, so no kick */
	uint32_t h0, d0, h1, d1;
	hrc_fm_rx_stats(&h0, &d0);
	run_ms(2000, true, false);
	hrc_fm_rx_stats(&h1, &d1);
	assert(d1 > d0 && kicks == k + 1);
	run_ms(500, true, true);

	/* Stop and start: the count begins again at the start, not at the last half before the stop */
	hrc_fm_rx_stop();
	run_ms(5000, false, true);	/* stopped: the reader returns 0 and checks nothing */
	assert(kicks == k + 1);
	assert(hrc_fm_rx_start());
	run_ms(150, false, true);
	assert(kicks == k + 1);
	run_ms(100, false, true);
	assert(kicks == k + 2);
	hrc_fm_rx_stop();
	assert(mask_calls >= m0);

	puts("PASS FM copy watchdog: no half for 200 ms kicks the copy, flowing or overrunning halves never do, refused kicks not counted");
	return 0;
}
