/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus P25 soft probe (p25soft.h, docs/p25-soft-probe-design.md). No RTOS, no hardware. */
#include "config.h"
#if CONFIG_SEARCH_SOFTP25
#include <string.h>
#include "radio/p25soft.h"
#include "p25cai.h"

#define MASK		(P25SOFT_RING - 1u)
#define N		((int64_t)P25SOFT_SYNC_N)
#define SYNC_WORD	UINT64_C(0x5575F5FF77FF)	/* 24 dibits, first sent highest: 01 = +3, 11 = -3 */
#define NID_END		57u	/* symbols from the sync's start to the NID's end (24 sync, 32 NID, 1 status) */
#define STATUS_AT	35u	/* the status symbol inside the NID */

/* Sync symbol k (0..23): +1 or -1 */
static int sync_sym(unsigned k)
{
	return ((SYNC_WORD >> (2u * (23u - k))) & 3u) == 1u ? 1 : -1;
}

/* The sync as the discriminator should show it at 8 kHz, x 64: sample i (time 3i/5 symbols) is the sum of the 24 sync
 * symbols (+-1) through the C4FM frequency pulse (raised cosine 0.2 times x / sin x), symbol k's middle at k + 1/2.
 * Computed off line (docs/p25-soft-probe-design.md "Template"); the symbols around the sync are unknown and left out */
static const int8_t tmpl[P25SOFT_SYNC_N] = {
	35, 73, 65, 64, 69, 50, 66, 99, 25, -80, -33, 92, 108, 20, -61, -93, -62, 27, 99, 82,
	-3, -81, -89, -39, -47, -116, -58, 83, 38, -89, -11, 95, -10, -113, -67, -42, -72, -66, -63, -73,
};
#define TSUM		(-178)		/* sum of tmpl */
#define TSUM2		196804		/* sum of its squares */
/* (N sum(t^2) - sum(t)^2) / 64^2, the template's part of the correlation's denominator */
#define DEN_T		((N * TSUM2 - (int64_t)TSUM * TSUM) >> 12)

void p25soft_init(p25soft_t *p)
{
	memset(p, 0, sizeof *p);
}

static int32_t at(const p25soft_t *p, uint32_t i)
{
	return p->ring[i & MASK];
}

/* The signal at time t (samples x 256, absolute): a cubic (Catmull-Rom) through the four samples around it */
static int32_t interp(const p25soft_t *p, int32_t t_q8)
{
	uint32_t i = (uint32_t)(t_q8 >> 8);
	int64_t u = t_q8 & 255;
	int64_t ym = at(p, i - 1u), y0 = at(p, i), y1 = at(p, i + 1u), y2 = at(p, i + 2u);
	int64_t c3 = 3 * (y0 - y1) + y2 - ym;
	int64_t c2 = 2 * ym - 5 * y0 + 4 * y1 - y2;
	int64_t c1 = y1 - ym;
	int64_t v = ((((c3 * u) >> 8) + c2) * u >> 8) + c1;	/* (c1 + u (c2 + u c3)) */
	return (int32_t)(y0 + ((v * u) >> 9));			/* y0 + u v / 2 */
}

/* Symbol k's middle after the sync's start t0 (samples x 256): t0 + (k + 1/2) 5/3 */
static int32_t sym_t(int32_t t0_q8, unsigned k)
{
	return t0_q8 + (int32_t)(((10u * k + 5u) * 256u + 3u) / 6u);
}

/* The sync's 24 symbols fitted (v = a s + b) and the NID after it read */
static void read_nid(p25soft_t *p)
{
	int64_t sv = 0, svp = 0;
	int32_t sp = 0;
	for (unsigned k = 0; k < 24u; k++) {
		int32_t v = interp(p, sym_t(p->t0_q8, k));
		int s = sync_sym(k);
		sv += v;
		svp += s > 0 ? v : -v;
		sp += s;
	}
	int64_t den = 24 * 24 - (int64_t)sp * sp;
	int64_t a = (24 * svp - sv * sp) / den;		/* the +-3 symbols' level (negative: inverted) */
	int64_t b = (sv - a * sp) / 24;			/* the offset */
	if (a == 0)
		return;
	bool inv = a < 0;
	if (inv)
		a = -a;
	int64_t th = 2 * a / 3;				/* between the +-1 and +-3 levels */
	uint64_t word = 0;
	for (unsigned k = 24u; k < NID_END; k++) {
		if (k == STATUS_AT)
			continue;
		int64_t d = interp(p, sym_t(p->t0_q8, k)) - b;
		if (inv)
			d = -d;
		unsigned dibit = d >= th ? 1u : d >= 0 ? 0u : d >= -th ? 2u : 3u;	/* +3, +1, -1, -3 */
		word = (word << 2) | dibit;
	}
	uint16_t nac;
	uint8_t duid;
	int c = p25cai_nid_decode(word, &nac, &duid);
	if (c >= 0 && (unsigned)c <= P25SOFT_NID_OK_BITS) {
		if (p->nids == 0)
			p->first_nid_count = p->count;
		p->nids++;
		p->nac = nac;
		p->duid = duid;
		p->corrected = (uint8_t)c;
	} else if (c >= 0) {
		p->hints++;
	}
}

/* The correlation peak followed ended at cand_at (r2_after: the sample after it, same sign, else 0) */
static void peak(p25soft_t *p, uint32_t r2_after)
{
	/* The peak between samples: a parabola through the three r^2 around it (+-1/2 sample) */
	int32_t ym = (int32_t)(p->r2_before >> 4), y0 = (int32_t)(p->r2_best >> 4), yp = (int32_t)(r2_after >> 4);
	int32_t den = ym - 2 * y0 + yp, d_q8 = 0;
	if (den < 0) {
		d_q8 = (ym - yp) * 128 / den;
		d_q8 = d_q8 > 128 ? 128 : d_q8 < -128 ? -128 : d_q8;
	}
	p->t0_q8 = (int32_t)((p->cand_at - P25SOFT_SYNC_N) << 8) + d_q8;
	/* The NID's last symbol plus the interpolation's look ahead and half a sample of timing */
	p->nid_at = p->cand_at + 60u;
	p->syncs++;
}

static void sample(p25soft_t *p, int16_t x)
{
	p->ring[p->count & MASK] = x;
	p->count++;
	p->sum += x;
	p->sum2 += (int32_t)x * x;
	if (p->count > P25SOFT_SYNC_N) {
		int32_t o = at(p, p->count - P25SOFT_SYNC_N - 1u);
		p->sum -= o;
		p->sum2 -= o * o;
	}
	if (p->nid_at != 0 && p->count == p->nid_at) {
		p->nid_at = 0;
		read_nid(p);
	}
	if (p->count < P25SOFT_SYNC_N)
		return;
	/* The window's normalised correlation with the template:
	 * r = (N C - S T) / sqrt((N Q - S^2) (N sum(t^2) - T^2)), C = sum(w t), S = sum(w), Q = sum(w^2), T = sum(t) */
	uint32_t w0 = p->count - P25SOFT_SYNC_N;
	int32_t c = 0;
	for (unsigned i = 0; i < P25SOFT_SYNC_N; i++)
		c += at(p, w0 + i) * tmpl[i];
	int64_t num = (N * c - (int64_t)p->sum * TSUM) >> 6;
	int64_t ds = N * p->sum2 - (int64_t)p->sum * p->sum;
	int64_t d = ds * DEN_T;
	int32_t rs = 0;		/* r^2 in Q16, with r's sign */
	if (d > 0 && num != 0) {
		uint64_t n2 = (uint64_t)(num < 0 ? -num : num);
		n2 *= n2;
		if (n2 > (uint64_t)d >> 3) {	/* r^2 over 1/8: worth the division */
			/* a near-silent window has a small d: there the exact quotient (n2 <= d, so n2 << 16 fits) */
			uint64_t r2 = d >= ((int64_t)1 << 24) ? n2 / (((uint64_t)d >> 16) + 1u) : (n2 << 16) / (uint64_t)d;
			rs = (int32_t)(r2 > 65536u ? 65536u : r2);
			if (num < 0)
				rs = -rs;
		}
	}
	uint32_t r2 = (uint32_t)(rs < 0 ? -rs : rs);
	bool neg = rs < 0;
	if (p->cand_at != 0) {
		bool same = neg == p->cand_neg && rs != 0;
		if (same && r2 > p->r2_best) {
			p->r2_before = p->r2_prev_sign == p->cand_neg ? p->r2_prev : 0u;
			p->r2_best = r2;
			p->cand_at = p->count;
		} else {
			peak(p, same ? r2 : 0u);
			p->cand_at = 0;
		}
	} else if (p->nid_at == 0 && r2 >= P25SOFT_SYNC_R2) {
		p->cand_at = p->count;
		p->cand_neg = neg;
		p->r2_best = r2;
		p->r2_before = p->r2_prev_sign == neg ? p->r2_prev : 0u;
	}
	p->r2_prev = r2;
	p->r2_prev_sign = neg;
}

bool p25soft_samples(p25soft_t *p, const int16_t *in, size_t n)
{
	for (size_t i = 0; i < n; i++)
		sample(p, in[i]);
	return p->nids != 0;
}
#endif
