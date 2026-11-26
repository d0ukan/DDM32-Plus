/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus user data packs: the two-slot store (datapack.h). Installed as core/storage/datapack.c. */
#include "config.h"
#if CONFIG_DATAPACK
#include <string.h>
#include "board.h"
#include "storage/storage.h"
#include "storage/datapack.h"
#include "ui/fmt.h"
#include "storage/ref_pack.h"

/* Layout 2 (docs/mappack-design.md A.1): per type its two slot starts and its slot size. The RadioID slots keep
 * 1.0.x's starts (a pack of up to 192 KB there stays valid both ways); the map pack slots are what were the tails of
 * 1.0.x's 256 KB RadioID slots */
static const struct {
	uint32_t a, b, size;
} slots[DPK_TYPES] = {
	{ 0xE70000u, 0xEB0000u, 0x30000u },	/* DPK_RADIOID */
	{ 0xEF0000u, 0xEF4000u, 0x4000u },	/* DPK_AIR */
	{ 0xEA0000u, 0xEE0000u, 0x10000u },	/* DPK_REF */
};
/* Every slot inside the region, 4 KB aligned, no two overlapping: in address order RadioID A, map A, RadioID B,
 * map B, air A, air B (the host test checks the table itself, docs/mappack-design.md G T1) */
_Static_assert(DPK_TYPES == 3 && DPK_RADIOID == 1 && DPK_AIR == 2 && DPK_REF == 3, "slots[] is by type - 1");
_Static_assert(0xE70000u >= DPK_BASE && 0xE70000u + 0x30000u <= 0xEA0000u && 0xEA0000u + 0x10000u <= 0xEB0000u &&
	       0xEB0000u + 0x30000u <= 0xEE0000u && 0xEE0000u + 0x10000u <= 0xEF0000u &&
	       0xEF0000u + 0x4000u <= 0xEF4000u && 0xEF4000u + 0x4000u <= DPK_END, "layout 2 inside the region, apart");

static struct {
	dpk_info_t h;
	uint8_t slot;		/* 0xFF: none in force */
	uint8_t state;		/* DPK_OK / DPK_NONE / DPK_BAD / DPK_BIG / DPK_OTHER */
} cur[DPK_TYPES];

uint32_t dpk_slot_size(uint8_t type)
{
	return type >= 1u && type <= DPK_TYPES ? slots[type - 1u].size : 0u;
}

uint32_t dpk_slot_addr(uint8_t type, uint8_t slot)
{
	return type >= 1u && type <= DPK_TYPES && slot < 2u ? (slot == 0u ? slots[type - 1u].a : slots[type - 1u].b) : 0u;
}

static bool inside(uint32_t addr, uint32_t len)
{
	return addr >= DPK_BASE && len <= DPK_END - addr;
}

static bool rd(uint32_t addr, void *buf, uint32_t len)
{
	return board.flash != NULL && board.flash->read != NULL && board.flash->size >= DPK_END &&
	       board.flash->read(addr, buf, len);
}

bool dpk_raw(uint32_t addr, void *buf, uint32_t len)
{
	return inside(addr, len) && rd(addr, buf, len);
}

bool dpk_erase(uint32_t addr)
{
	if (!inside(addr, DPK_SECTOR) || addr % DPK_SECTOR != 0u || board.flash == NULL || board.flash->size < DPK_END)
		return false;
	storage_lock();
	bool ok = board.flash->erase(addr);
	storage_unlock();
	return ok;
}

bool dpk_program(uint32_t addr, const void *data, uint32_t len)
{
	if (!inside(addr, len) || board.flash == NULL || board.flash->size < DPK_END)
		return false;
	storage_lock();
	bool ok = board.flash->write(addr, data, len);
	storage_unlock();
	return ok;
}

void dpk_seal(dpk_hdr_t *h)
{
	h->hcrc = storage_crc32(0, h, offsetof(dpk_hdr_t, hcrc));
}

bool dpk_hdr_ok(const dpk_hdr_t *h, uint8_t type)
{
	/* the slot size first: a type the build does not know has size 0, and 0 - DPK_HDR would wrap */
	return memcmp(h->magic, "DPK1", 4) == 0 && h->type == type && h->fmt == 1u && dpk_slot_size(type) > DPK_HDR &&
	       h->len <= dpk_slot_size(type) - DPK_HDR && h->hcrc == storage_crc32(0, h, offsetof(dpk_hdr_t, hcrc));
}

/* The payload's CRC-32 as the flash holds it now */
static bool payload_crc(uint32_t addr, uint32_t len, uint32_t *crc)
{
	uint8_t b[64];
	uint32_t c = 0;
	for (uint32_t off = 0; off < len; off += sizeof b) {
		uint32_t n = len - off < sizeof b ? len - off : (uint32_t)sizeof b;
		if (!rd(addr + off, b, n))
			return false;
		c = storage_crc32(c, b, n);
	}
	*crc = c;
	return true;
}

/* What a slot holds (DPK_OK: a whole, checked pack, h its header). A slot whose first 4 bytes are not "DPK1" holds
 * nothing committed (the header is written last): empty, being written, or another layout's payload bytes */
static uint8_t slot_check(uint8_t type, uint8_t slot, dpk_hdr_t *h)
{
	uint32_t a = dpk_slot_addr(type, slot), crc;
	if (!rd(a, h, sizeof *h) || memcmp(h->magic, "DPK1", 4) != 0)
		return DPK_NONE;
	if (!dpk_hdr_ok(h, type)) {
		/* a RadioID header that checks but is longer than this layout's slot: a 1.0.x pack over 192 KB */
		return type == DPK_RADIOID && h->type == type && h->fmt == 1u && h->len > dpk_slot_size(type) - DPK_HDR &&
		       h->hcrc == storage_crc32(0, h, offsetof(dpk_hdr_t, hcrc)) ? DPK_BIG : DPK_BAD;
	}
	/* the CRC, then (a map pack) the structure: one after the other, never both 64-byte buffers live */
	if (!payload_crc(a + DPK_HDR, h->len, &crc) || crc != h->crc)
		return DPK_BAD;
	return type == DPK_REF ? ref_validate(a + DPK_HDR, h->len, h->flags) : DPK_OK;
}

/* Of two rejected slots, the state About names: OTHER, then BIG, then BAD, then NONE */
static uint8_t worse(uint8_t a, uint8_t b)
{
	static const uint8_t rank[5] = { [DPK_NONE] = 0, [DPK_OK] = 0, [DPK_BAD] = 1, [DPK_BIG] = 2, [DPK_OTHER] = 3 };
	return b < 5u && rank[b] > rank[a] ? b : a;
}

static void scan_type(uint8_t type)
{
	dpk_hdr_t h;
	uint8_t i = (uint8_t)(type - 1u), slot = 0xFF, why = DPK_NONE;
	uint32_t seq = 0, len = 0;
	for (uint8_t s = 0; s < 2u; s++) {
		uint8_t st = slot_check(type, s, &h);
		if (st != DPK_OK) {
			why = worse(why, st);
		} else if (slot == 0xFF || h.seq > seq) {
			seq = h.seq;
			len = h.len;
			slot = s;
		}
	}
	/* Found first, then taken: a reader elsewhere (another task) sees the old pack until here, not a half scan */
	cur[i].state = DPK_NONE;
	cur[i].h.seq = seq;
	cur[i].h.len = len;
	cur[i].slot = slot;
	cur[i].state = slot != 0xFF ? DPK_OK : why;
}

void dpk_scan(void)
{
	for (uint8_t t = 1; t <= DPK_TYPES; t++)
		scan_type(t);
}

const dpk_info_t *dpk_active(uint8_t type)
{
	return type >= 1u && type <= DPK_TYPES && cur[type - 1u].state == DPK_OK ? &cur[type - 1u].h : NULL;
}

uint8_t dpk_state(uint8_t type)
{
	return type >= 1u && type <= DPK_TYPES ? cur[type - 1u].state : DPK_NONE;
}

uint32_t dpk_active_addr(uint8_t type)
{
	return dpk_active(type) != NULL ? dpk_slot_addr(type, cur[type - 1u].slot) : 0u;
}

bool dpk_header(uint8_t type, dpk_hdr_t *h)
{
	return dpk_active(type) != NULL && rd(dpk_slot_addr(type, cur[type - 1u].slot), h, sizeof *h) &&
	       dpk_hdr_ok(h, type);
}

bool dpk_read(uint8_t type, uint32_t off, void *buf, uint32_t len)
{
	const dpk_info_t *h = dpk_active(type);
	return h != NULL && off <= h->len && len <= h->len - off &&
	       rd(dpk_slot_addr(type, cur[type - 1u].slot) + DPK_HDR + off, buf, len);
}

static char unpack6(uint8_t c)
{
	return c == 0u ? ' ' : c <= 10u ? (char)('0' + c - 1u) : c <= 36u ? (char)('A' + c - 11u) :
	       c <= 62u ? (char)('a' + c - 37u) : '.';
}

bool dpk_id_text(uint32_t id, char *out, size_t len)
{
	uint8_t h[DPK_ID_HDR], r[3u + DPK_ID_TEXT_MAX * 3u / 4u];
	if (len != 0u)
		out[0] = '\0';
	if (len == 0u || !dpk_read(DPK_RADIOID, 0, h, sizeof h))
		return false;
	uint32_t n = h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
	uint32_t rec = h[4], chars = h[5];
	if (chars == 0u || chars > DPK_ID_TEXT_MAX || chars % 4u != 0u || rec != 3u + chars * 3u / 4u)
		return false;
	uint32_t lo = 0, hi = n;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2u;
		if (!dpk_read(DPK_RADIOID, DPK_ID_HDR + mid * rec, r, 3))
			return false;
		uint32_t m = r[0] | (uint32_t)r[1] << 8 | (uint32_t)r[2] << 16;
		if (m == id) {
			if (!dpk_read(DPK_RADIOID, DPK_ID_HDR + mid * rec, r, rec))
				return false;
			size_t k = 0;
			for (uint32_t i = 0; i < chars && k + 1u < len; i += 4u) {
				const uint8_t *p = &r[3u + i / 4u * 3u];
				uint32_t v = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
				for (int s = 18; s >= 0 && k + 1u < len; s -= 6)
					out[k++] = unpack6((uint8_t)((v >> s) & 63u));
			}
			while (k > 0u && out[k - 1u] == ' ')
				k--;
			out[k] = '\0';
			return k != 0u;
		}
		if (m < id)
			lo = mid + 1u;
		else
			hi = mid;
	}
	return false;
}

bool dpk_about(uint8_t type, char *out, size_t len)
{
	dpk_hdr_t h;
	if (len != 0u)
		out[0] = '\0';
	if (!dpk_header(type, &h))
		return false;
	dpk_describe(&h, out, len);
	return true;
}

void dpk_describe(const dpk_hdr_t *h, char *out, size_t len)
{
	sbuf_t b;
	sb_init(&b, out, len);
	char name[sizeof h->name + 1];
	memcpy(name, h->name, sizeof h->name);
	name[sizeof h->name] = '\0';
	sb_str(&b, name);
	sb_str(&b, ", ");
	for (unsigned i = 0; i < 8u; i++) {
		if (i == 4u || i == 6u)
			sb_chr(&b, '-');
		sb_chr(&b, h->date[i]);
	}
	sb_str(&b, ", ");
	sb_u32(&b, (h->len + 1023u) / 1024u, 1);
	sb_str(&b, " KB");
}

bool dpk_begin(dpk_writer_t *w, const dpk_hdr_t *h)
{
	w->addr = 0;
	if (h->type < 1u || h->type > DPK_TYPES || h->len > dpk_slot_size(h->type) - DPK_HDR)
		return false;
	uint8_t i = (uint8_t)(h->type - 1u);
	uint8_t slot = cur[i].state == DPK_OK ? (uint8_t)(1u - cur[i].slot) : 0u;
	uint32_t a = dpk_slot_addr(h->type, slot);
	for (uint32_t s = 0; s < DPK_HDR + h->len; s += DPK_SECTOR)
		if (!dpk_erase(a + s))
			return false;
	w->h = *h;
	w->h.seq = cur[i].state == DPK_OK ? cur[i].h.seq + 1u : 1u;
	w->addr = a;
	return true;
}

bool dpk_put(dpk_writer_t *w, uint32_t off, const void *data, uint32_t len)
{
	return w->addr != 0u && off <= w->h.len && len <= w->h.len - off && dpk_program(w->addr + DPK_HDR + off, data, len);
}

bool dpk_commit(dpk_writer_t *w)
{
	uint32_t crc, a = w->addr;
	w->addr = 0;		/* closed, whatever happens now */
	if (a == 0u || !payload_crc(a + DPK_HDR, w->h.len, &crc) || crc != w->h.crc)
		return false;
	/* A map pack's structure is checked before its header is programmed: one that checks by CRC but not by
	 * structure (or was built for another version) is never committed; the pack in force stays */
	if (w->h.type == DPK_REF && ref_validate(a + DPK_HDR, w->h.len, w->h.flags) != DPK_OK)
		return false;
	memcpy(w->h.magic, "DPK1", 4);
	dpk_seal(&w->h);
	bool ok = dpk_program(a, &w->h, sizeof w->h);
	scan_type(w->h.type);
	return ok && dpk_active(w->h.type) != NULL && dpk_active(w->h.type)->seq == w->h.seq;
}
#endif
