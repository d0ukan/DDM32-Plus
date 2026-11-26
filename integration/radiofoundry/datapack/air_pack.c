/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus air band data pack over the airfield table (air_pack.h); since 1.1 the table is the map pack's
 * (airfield.h air_get). Installed as core/ui/air_pack.c. */
#include "config.h"
#if CONFIG_AIRFIELDS
#include <string.h>
#include "ui/air_pack.h"
#if CONFIG_DATAPACK
#include "storage/datapack.h"
#include "storage/ref_pack.h"

/* The last airfield looked up: where its list starts in the pack (no RAM copy of the list) */
static struct {
	uint32_t seq;		/* the pack it was found in (0: none looked up) */
	uint32_t ref_seq;	/* the map pack the airfield's code came from */
	uint16_t i;
	uint32_t off;		/* its first frequency in the payload, 0: not in the pack */
	uint8_t n;
} last;

/* Airfield i's code (8 bytes, NUL padded) from the map pack's record, without the whole record */
static bool code_of(uint16_t i, char code[9])
{
	code[8] = '\0';
	return ref_bytes(REF_S_AIR, 44u * i + REF_AIR_FIRST + 4u, code, 8);
}

/* Airfield i's list in the pack: true and its offset / count when the pack names it */
static bool find(uint16_t i, uint32_t *off, uint8_t *n)
{
	const dpk_info_t *p = dpk_active(DPK_AIR), *r = dpk_active(DPK_REF);
	if (p == NULL || r == NULL)
		return false;
	if (last.seq != p->seq || last.ref_seq != r->seq || last.i != i) {
		uint8_t h[AIR_PACK_HDR], b[9];
		char code[9];
		last.seq = p->seq;
		last.ref_seq = r->seq;
		last.i = i;
		last.off = 0;
		if (!code_of(i, code) || code[0] == '\0' || !dpk_read(DPK_AIR, 0, h, sizeof h))
			return false;
		uint32_t at = AIR_PACK_HDR;
		for (uint16_t k = 0, count = (uint16_t)(h[0] | h[1] << 8); k < count; k++) {
			if (!dpk_read(DPK_AIR, at, b, sizeof b) || b[8] > AIR_PACK_FREQS || 3u * b[8] > p->len - at - 9u)
				break;	/* a list past the payload's end: the pack is not used from here on */
			if (strncmp((const char *)b, code, 8) == 0) {
				last.off = at + 9u;
				last.n = b[8];
				break;
			}
			at += 9u + 3u * b[8];
		}
	}
	*off = last.off;
	*n = last.n;
	return last.off != 0u;
}
#endif

uint8_t air_count(uint16_t i)
{
	uint8_t c = 0;
	if (i >= air_nfields())
		return 0;
#if CONFIG_DATAPACK
	uint32_t off;
	uint8_t n;
	if (find(i, &off, &n))
		return n;
#endif
	(void)ref_bytes(REF_S_AIR, 44u * i + REF_AIR_FIRST + 2u, &c, 1);
	return c <= AIR_PACK_FREQS ? c : 0u;
}

/* One static copy: a caller never holds two results (copy the value where two are compared) */
const air_freq_t *air_freq(uint16_t i, uint8_t k)
{
	static air_freq_t f;
	if (k >= air_count(i))
		return NULL;
#if CONFIG_DATAPACK
	uint32_t off;
	uint8_t n, r[3];
	if (find(i, &off, &n)) {
		if (!dpk_read(DPK_AIR, off + 3u * k, r, sizeof r))
			return NULL;
		f.ch = (uint16_t)(r[0] | r[1] << 8);
		f.type = r[2] < AIR_TYPES ? r[2] : AIR_OTH;
		return &f;
	}
#endif
	uint8_t first[2];
	if (!ref_bytes(REF_S_AIR, 44u * i + REF_AIR_FIRST, first, 2) ||
	    !air_freq_get((uint32_t)(first[0] | first[1] << 8) + k, &f))
		return NULL;
	return &f;
}
#endif
