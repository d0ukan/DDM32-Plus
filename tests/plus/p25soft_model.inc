/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus P25 soft probe tests: the signal model shared by test_p25soft.c and test_p25soft_radio.c (our own, host
 * only, double precision): symbols -> the C4FM transmit filter (raised cosine 0.2 times x / sin x, P25 CAI) as the
 * frequency deviation (+-600 / +-1800 Hz) -> an FM carrier at 48 kHz complex baseband with a frequency error -> white
 * noise at a carrier to noise ratio in 12.5 kHz -> the channel filter (+-6 kHz) -> a discriminator (the phase step) ->
 * an anti-alias low-pass and every sixth sample: 8 kHz, scaled to int16 at a gain and polarity the radio does not
 * promise. Nothing here is measured on the radio. Include after <math.h>, <stdio.h>, <stdlib.h>, <string.h>,
 * "p25cai.h" and a `failures` counter. */
#define FSH	48000.0		/* the model's rate */
#define DEC	6		/* 48 kHz -> 8 kHz */
#define SPS	10		/* 48 kHz / 4800 */

/* --- random numbers (reproducible) --- */
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (uint32_t)(rng >> 16);
}
static double gauss(void)
{
	double u = (rnd() + 1.0) / 4294967297.0, v = (rnd() + 1.0) / 4294967297.0;
	return sqrt(-2.0 * log(u)) * cos(2.0 * M_PI * v);
}

/* --- filters --- */
#define TX_TAPS 161		/* +-8 symbols at 48 kHz */
static double txf[TX_TAPS];
#define CH_TAPS 63
static double chf[CH_TAPS];
#define AA_TAPS 63
static double aaf[AA_TAPS];

static void design(void)
{
	/* C4FM: raised cosine (0.2) times the shaping filter x / sin x (x = pi f / 4800), by its inverse transform */
	double sum = 0;
	for (int i = 0; i < TX_TAPS; i++) {
		double t = (i - TX_TAPS / 2) / FSH, h = 0;
		for (int k = 0; k <= 2000; k++) {
			double f = 2880.0 * k / 2000.0, H;
			if (f <= 1920.0)
				H = 1.0;
			else
				H = 0.5 + 0.5 * cos(M_PI * (f - 1920.0) / 960.0);
			double x = M_PI * f / 4800.0, P = k == 0 ? 1.0 : x / sin(x);
			h += (k == 0 || k == 2000 ? 0.5 : 1.0) * H * P * cos(2 * M_PI * f * t);
		}
		txf[i] = h;
		sum += h;
	}
	for (int i = 0; i < TX_TAPS; i++)
		txf[i] *= SPS / sum;	/* symbols held: their level (the pulses SPS apart sum to 1) */
	/* the channel (+-6 kHz) and the anti-alias (3.6 kHz) low-passes: windowed sinc (Hamming) */
	for (int i = 0; i < CH_TAPS; i++) {
		double n = i - CH_TAPS / 2, w = 0.54 - 0.46 * cos(2 * M_PI * i / (CH_TAPS - 1)), fc = 6000.0 / FSH;
		chf[i] = (n == 0 ? 2 * fc : sin(2 * M_PI * fc * n) / (M_PI * n)) * w;
	}
	for (int i = 0; i < AA_TAPS; i++) {
		double n = i - AA_TAPS / 2, w = 0.54 - 0.46 * cos(2 * M_PI * i / (AA_TAPS - 1)), fc = 3600.0 / FSH;
		aaf[i] = (n == 0 ? 2 * fc : sin(2 * M_PI * fc * n) / (M_PI * n)) * w;
	}
}

/* --- the radio: a frequency deviation (Hz at 48 kHz) to 8 kHz int16 discriminator samples --- */
typedef struct {
	double cnr_db, offset_hz, gain;	/* gain: int16 units a Hz */
	int invert;
	int carrier;			/* 0: no carrier at all (noise only) */
} chan_t;

static int16_t *radio(const double *dev, size_t n, const chan_t *c, size_t *out_n)
{
	double sigma = sqrt(pow(10.0, -c->cnr_db / 10.0) * (FSH / 12500.0) / 2.0);	/* per real dimension */
	double *zi = malloc(n * sizeof *zi), *zq = malloc(n * sizeof *zq), *d = malloc(n * sizeof *d);
	double ph = rnd() / 4294967296.0 * 2 * M_PI;
	for (size_t i = 0; i < n; i++) {
		ph += 2 * M_PI * (dev[i] + c->offset_hz) / FSH;
		zi[i] = (c->carrier ? cos(ph) : 0) + sigma * gauss();
		zq[i] = (c->carrier ? sin(ph) : 0) + sigma * gauss();
	}
	double pi = 0, pq = 0;
	for (size_t i = 0; i < n; i++) {
		double fi = 0, fq = 0;
		for (int k = 0; k < CH_TAPS; k++) {
			size_t j = i >= (size_t)k ? i - k : 0;
			fi += chf[k] * zi[j];
			fq += chf[k] * zq[j];
		}
		/* the phase step: arg(z[n] conj(z[n-1])) */
		double re = fi * pi + fq * pq, im = fq * pi - fi * pq;
		d[i] = atan2(im, re) * FSH / (2 * M_PI);
		pi = fi;
		pq = fq;
	}
	size_t m = n / DEC;
	int16_t *out = malloc(m * sizeof *out);
	for (size_t o = 0; o < m; o++) {
		size_t i = o * DEC;
		double a = 0;
		for (int k = 0; k < AA_TAPS; k++)
			a += aaf[k] * d[i >= (size_t)k ? i - k : 0];
		a *= c->gain * (c->invert ? -1 : 1);
		out[o] = (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : lrint(a));
	}
	free(zi);
	free(zq);
	free(d);
	*out_n = m;
	return out;
}

/* Dibits (P25: 01 +3, 00 +1, 10 -1, 11 -3) to the C4FM deviation at 48 kHz */
static double *c4fm(const uint8_t *dibits, size_t n, size_t *out_n)
{
	static const double lvl[4] = { 600.0, 1800.0, -600.0, -1800.0 };
	size_t m = n * SPS;
	double *dev = calloc(m, sizeof *dev);
	for (size_t k = 0; k < n; k++) {
		size_t c = k * SPS + SPS / 2;
		for (int t = 0; t < TX_TAPS; t++) {
			long j = (long)c + t - TX_TAPS / 2;
			if (j >= 0 && (size_t)j < m)
				dev[j] += txf[t] * lvl[dibits[k] & 3u];
		}
	}
	*out_n = m;
	return dev;
}

/* A P25 call: HDU, LDU1/LDU2 pairs, TDU; dibits appended to out at *n */
static void frame_dibits(const p25cai_frame *f, uint8_t *out, size_t *n)
{
	uint8_t air[P25CAI_MAX_FRAME_BYTES];
	int bytes = p25cai_frame_encode(f, air, sizeof air);
	if (bytes <= 0) {
		printf("encode failed duid %u\n", f->duid);
		failures++;
		return;
	}
	for (int b = 0; b < bytes; b++)
		for (int s = 6; s >= 0; s -= 2)
			out[(*n)++] = (uint8_t)((air[b] >> s) & 3u);
}

static size_t p25_call(uint16_t nac, unsigned ldus, uint8_t *out)
{
	size_t n = 0;
	p25cai_frame f;
	memset(&f, 0, sizeof f);
	f.nac = nac;
	f.status = 2;
	f.duid = P25CAI_HDU;
	frame_dibits(&f, out, &n);
	for (unsigned l = 0; l < ldus; l++) {
		memset(f.voice, 0, sizeof f.voice);
		for (int i = 0; i < 9; i++)
			for (int j = 0; j < 8; j++)
				f.voice[i][j] = (uint16_t)(rnd() & (j < 4 ? 0xFFFu : j < 7 ? 0x7FFu : 0x7Fu));
		f.duid = (l & 1u) ? P25CAI_LDU2 : P25CAI_LDU1;
		memset(f.metadata, 0, sizeof f.metadata);
		frame_dibits(&f, out, &n);
	}
	f.duid = P25CAI_TDU;
	frame_dibits(&f, out, &n);
	return n;
}

static int16_t *p25_signal(uint16_t nac, unsigned ldus, const chan_t *c, size_t *n)
{
	uint8_t *dib = malloc(200000);
	size_t nd = p25_call(nac, ldus, dib), nm;
	double *dev = c4fm(dib, nd, &nm);
	int16_t *s = radio(dev, nm, c, n);
	free(dib);
	free(dev);
	return s;
}

