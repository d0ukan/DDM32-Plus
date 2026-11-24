/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus P25 soft probe (radio/p25soft.c, docs/p25-soft-probe-design.md), against synthesized radio signals.
 *
 * The signal model (our own, host only, double precision): symbols -> the C4FM transmit filter (raised cosine 0.2
 * times x / sin x, P25 CAI) as the frequency deviation (+-600 / +-1800 Hz) -> an FM carrier at 48 kHz complex baseband
 * with a frequency error -> white noise at a carrier to noise ratio in 12.5 kHz -> the channel filter (+-6 kHz) ->
 * a discriminator (the phase step) -> an anti-alias low-pass and every sixth sample: 8 kHz, scaled to int16 at a
 * gain and polarity the radio does not promise. Nothing here is measured on the radio: the FM RX RAM's real filter,
 * level and noise are what the radio must confirm (docs/p25-soft-probe-design.md "Risks").
 *
 * Checks: P25 (HDU, LDU1, LDU2 ... TDU) with known NACs is found with its NAC from a random point of the call, either
 * polarity, with a frequency error and gains 10x apart, within the probe window; the detection rate against the
 * carrier to noise ratio; no P25 (and few "hints") on noise, an unmodulated carrier, FM speech with CTCSS, NXDN48,
 * DMR-like 4800-baud 4FSK and P25-modulated random dibits (no frame structure); the time a probe takes. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "radio/p25soft.h"
#include "p25cai.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define WINDOW_MS 450u		/* the radio core's soft probe window (radio_p25soft.c P25SOFT_WINDOW_MS) */

#include "p25soft_model.inc"

/* Runs the probe over 8 kHz samples from `from`, in 64-sample reads as the radio core does, for at most `ms`.
 * Returns the ms of samples it took to find P25 (0 = not found) */
static unsigned probe(const int16_t *s, size_t n, size_t from, unsigned ms, p25soft_t *p)
{
	p25soft_init(p);
	size_t end = from + (size_t)ms * 8u;
	if (end > n)
		end = n;
	for (size_t i = from; i < end; i += 64) {
		size_t k = end - i < 64 ? end - i : 64;
		if (p25soft_samples(p, s + i, k))
			return (unsigned)((i + k - from) / 8u) + 1u;
	}
	return 0;
}

/* --- P25 found, its NAC, from anywhere in a call --- */
static void test_found(void)
{
	static const uint16_t nacs[] = { 0x293, 0x659, 0x1C8, 0xF7E, 0x001, 0xABC };
	unsigned found = 0, total = 0, wrong = 0, worst = 0, sum = 0;
	unsigned hist[5] = { 0 };	/* found within 100, 200, 300, 450 ms, not */
	for (unsigned t = 0; t < 48u; t++) {
		chan_t c = { 30.0, (double)((int)(rnd() % 1001u) - 500), 0.3 + (rnd() % 1000u) / 100.0, (int)(t & 1u), 1 };
		uint16_t nac = nacs[t % 6u];
		size_t n;
		int16_t *s = p25_signal(nac, 12, &c, &n);
		for (unsigned e = 0; e < 8u; e++) {
			size_t from = rnd() % (n - WINDOW_MS * 8u);
			p25soft_t p;
			unsigned ms = probe(s, n, from, WINDOW_MS, &p);
			total++;
			if (ms) {
				found++;
				sum += ms;
				worst = ms > worst ? ms : worst;
				hist[ms <= 100 ? 0 : ms <= 200 ? 1 : ms <= 300 ? 2 : 3]++;
				if (p.nac != nac)
					wrong++;
			} else {
				hist[4]++;
			}
		}
		free(s);
	}
	printf("p25soft found: %u of %u (wrong NAC %u), mean %u ms, worst %u ms; <=100 %u, <=200 %u, <=300 %u, <=450 %u, "
	       "missed %u\n", found, total, wrong, found ? sum / found : 0, worst, hist[0], hist[1], hist[2], hist[3], hist[4]);
	CHECK(found == total);
	CHECK(wrong == 0);
	CHECK(worst <= 250u);	/* a frame sync every 180 ms (LDU), the NID 12 ms after it, the reads 8 ms */
	CHECK(found && sum / found <= 130u);
}

/* --- the detection rate against the carrier to noise ratio (printed; the floor asserted) --- */
static void test_noise(void)
{
	static const double cnr[] = { 25, 15, 10, 8, 6, 5, 4, 3 };
	for (unsigned i = 0; i < sizeof cnr / sizeof cnr[0]; i++) {
		unsigned found = 0, hinted = 0, total = 0;
		for (unsigned t = 0; t < 12u; t++) {
			chan_t c = { cnr[i], (double)((int)(rnd() % 601u) - 300), 6.0, (int)(t & 1u), 1 };
			size_t n;
			int16_t *s = p25_signal(0x659, 10, &c, &n);
			for (unsigned e = 0; e < 8u; e++) {
				p25soft_t p;
				total++;
				if (probe(s, n, rnd() % (n - WINDOW_MS * 8u), WINDOW_MS, &p))
					found++;
				else if (p25soft_hint(&p))
					hinted++;
			}
			free(s);
		}
		printf("p25soft CNR %4.1f dB: found %3u of %u, else hinted %u (native decides), else missed %u\n", cnr[i], found,
		       total, hinted, total - found - hinted);
		/* Under about 8 dB the model's discriminator clicks and the probe starts to miss: the radio core trusts a
		 * "not P25" only on a carrier P25SOFT_TRUST_DB over the floor (radio_p25soft.c), else the native receiver
		 * decides as before */
		if (cnr[i] >= 8.0)
			CHECK(found >= total * 98u / 100u);
	}
}

/* --- not P25: nothing found; how often a window would still go to the native receiver (a hint) --- */
static void not_p25(const char *what, double *dev, size_t nm, chan_t c, unsigned max_hint_pct)
{
	size_t n;
	int16_t *s = radio(dev, nm, &c, &n);
	unsigned windows = 0, hints = 0, p25 = 0;
	for (size_t from = 0; from + WINDOW_MS * 8u <= n; from += WINDOW_MS * 8u) {
		p25soft_t p;
		windows++;
		if (probe(s, n, from, WINDOW_MS, &p))
			p25++;
		else if (p25soft_hint(&p))
			hints++;
	}
	printf("p25soft %-30s %3u windows: P25 %u, hinted %u (%u%%)\n", what, windows, p25, hints,
	       windows ? hints * 100u / windows : 0);
	CHECK(p25 == 0);
	CHECK(hints * 100u <= windows * max_hint_pct);
	free(s);
}

static void test_not_p25(void)
{
	size_t secs = 30, nm = (size_t)(secs * FSH);
	double *dev = calloc(nm, sizeof *dev);
	chan_t c = { 25.0, 120.0, 6.0, 0, 1 };

	/* an unmodulated carrier, then no carrier at all (the receiver's noise) */
	not_p25("unmodulated carrier", dev, nm, c, 5);
	chan_t none = { 0.0, 0.0, 6.0, 0, 0 };
	not_p25("noise only", dev, nm, none, 5);
	chan_t faint = { 0.0, 0.0, 0.0005, 0, 0 };	/* a near-silent ring: a few LSB of noise */
	not_p25("near-silent noise", dev, nm, faint, 5);

	/* FM speech-like: tones 300-3000 Hz changing every 40-200 ms, +-2.5 kHz, CTCSS 88.5 Hz at 400 Hz */
	{
		double f1 = 500, f2 = 1200, a1 = 1500, a2 = 800, p1 = 0, p2 = 0, pc = 0;
		size_t next = 0;
		for (size_t i = 0; i < nm; i++) {
			if (i == next) {
				f1 = 300 + rnd() % 1200u;
				f2 = 900 + rnd() % 2100u;
				a1 = rnd() % 1800u;
				a2 = rnd() % 900u;
				next = i + (size_t)(FSH * (0.04 + (rnd() % 160u) / 1000.0));
			}
			p1 += 2 * M_PI * f1 / FSH;
			p2 += 2 * M_PI * f2 / FSH;
			pc += 2 * M_PI * 88.5 / FSH;
			dev[i] = a1 * sin(p1) + a2 * sin(p2) + 400 * sin(pc) + 300 * gauss();
		}
		not_p25("FM speech + CTCSS", dev, nm, c, 10);
	}
	/* NXDN48: 2400 symbols/s, +-1050 / +-350 Hz, raised cosine-ish (the C4FM filter at half the rate is close enough) */
	{
		size_t nd = (size_t)(secs * 2400), m;
		uint8_t *d = malloc(nd * 2);
		for (size_t k = 0; k < nd; k++)
			d[2 * k] = d[2 * k + 1] = (uint8_t)(rnd() & 3u);
		double *x = c4fm(d, nd * 2, &m);
		for (size_t i = 0; i < m; i++)
			x[i] *= 1050.0 / 1800.0;
		not_p25("NXDN48 4FSK", x, m < nm ? m : nm, c, 10);
		free(x);
		free(d);
	}
	/* DMR-like: 4800 symbols/s 4FSK (+-1944 / +-648 Hz) random, with DMR's BS voice sync now and then */
	{
		static const uint64_t dmr_sync = UINT64_C(0x755FD7DF75F7);
		size_t nd = (size_t)(secs * 4800), m;
		uint8_t *d = malloc(nd);
		for (size_t k = 0; k < nd; k++) {
			d[k] = (uint8_t)(rnd() & 3u);
			if (k % 288u == 108u && k + 24 < nd)
				for (unsigned j = 0; j < 24u; j++)
					d[k + j] = (uint8_t)((dmr_sync >> (2u * (23u - j))) & 3u);
		}
		double *x = c4fm(d, nd, &m);
		for (size_t i = 0; i < m; i++)
			x[i] *= 1944.0 / 1800.0;
		not_p25("DMR-like 4800 4FSK", x, m < nm ? m : nm, c, 10);
		free(x);
		free(d);
	}
	/* P25's own modulation with random dibits: no frame sync, no NID */
	{
		size_t nd = (size_t)(secs * 4800), m;
		uint8_t *d = malloc(nd);
		for (size_t k = 0; k < nd; k++)
			d[k] = (uint8_t)(rnd() & 3u);
		double *x = c4fm(d, nd, &m);
		not_p25("C4FM random dibits", x, m < nm ? m : nm, c, 10);
		free(x);
		free(d);
	}
	free(dev);
}

/* --- the time a probe costs: host time for 1 s of audio, as a check that nothing grew by an order of magnitude --- */
static void test_cost(void)
{
	chan_t c = { 30.0, 200.0, 6.0, 0, 1 };
	size_t n;
	int16_t *s = p25_signal(0x659, 6, &c, &n);
	p25soft_t p;
	clock_t t0 = clock();
	unsigned reps = 20;
	for (unsigned r = 0; r < reps; r++) {
		p25soft_init(&p);
		for (size_t i = 0; i + 64 <= n; i += 64)
			p25soft_samples(&p, s + i, 64);
	}
	double sec = (double)(clock() - t0) / CLOCKS_PER_SEC, audio = reps * (double)n / 8000.0;
	printf("p25soft cost: %.1f us of host CPU per second of audio; sizeof(p25soft_t) %u B\n",
	       sec / audio * 1e6, (unsigned)sizeof(p25soft_t));
	CHECK(sizeof(p25soft_t) <= 360u);
	free(s);
}

int main(void)
{
	design();
	test_found();
	test_noise();
	test_not_p25();
	test_cost();
	if (failures) {
		printf("test_p25soft: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_p25soft: OK\n");
	return 0;
}
