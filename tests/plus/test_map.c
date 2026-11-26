/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus offline Turkey map (map.c, turkey_map_data.c, docs/map.md): the data's integrity (81 provinces, plates,
 * closed rings inside Turkey, UTF-8 names, labels inside their province, size budget), projection round trips,
 * zoom and pan, clipping, the province of known cities, and rendering into a 160 x 128 test framebuffer.
 * Optional: test_map out.pgm writes the whole-Turkey render for a look. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "map.h"
#include "turkey_map_data.h"
#include "turkey_district_data.h"
#include "refpack_fixture.h"

/* The names are copied out of the map pack since 1.1: a few rotating buffers for the checks below */
static const char *pname(uint8_t p)
{
	static char b[4][MAP_NAME_MAX];
	static unsigned k;
	k = (k + 1u) % 4u;
	map_province_name(p, b[k], sizeof b[k]);
	return b[k];
}

static const char *dname(uint16_t d)
{
	static char b[4][MAP_NAME_MAX];
	static unsigned k;
	k = (k + 1u) % 4u;
	map_district_name(d, b[k], sizeof b[k]);
	return b[k];
}

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define FB_W 160
#define FB_H 128

typedef struct {
	uint8_t px[FB_H][FB_W];
	int x0, y0, x1, y1;	/* the viewport the callback must stay in */
	int outside;		/* segments handed out beyond it */
	uint32_t segs;
	bool seen[TURKEY_MAP_PROVINCES];	/* provinces with a segment */
} fb_t;

static void fb_line(void *ctx, int x0, int y0, int x1, int y1, uint8_t province)
{
	fb_t *fb = ctx;
	fb->segs++;
	if (province >= TURKEY_MAP_PROVINCES)
		fb->outside++;
	else
		fb->seen[province] = true;
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
	for (;;) {
		if (x0 < fb->x0 || x0 > fb->x1 || y0 < fb->y0 || y0 > fb->y1 || x0 < 0 || y0 < 0 || x0 >= FB_W ||
		    y0 >= FB_H) {
			fb->outside++;
			return;
		}
		fb->px[y0][x0] = (uint8_t)(1u + province);
		if (x0 == x1 && y0 == y1)
			break;
		int e2 = 2 * e;
		if (e2 >= dy) {
			e += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			e += dx;
			y0 += sy;
		}
	}
}

static uint32_t fb_render(fb_t *fb, const map_view_t *v)
{
	memset(fb, 0, sizeof *fb);
	fb->x0 = v->x;
	fb->y0 = v->y;
	fb->x1 = v->x + v->w - 1;
	fb->y1 = v->y + v->h - 1;
	uint32_t n = map_draw(v, fb_line, fb);
	CHECK(n == fb->segs);
	CHECK(fb->outside == 0);
	return n;
}

static int fb_count(const fb_t *fb)
{
	int n = 0;
	for (int y = 0; y < FB_H; y++)
		for (int x = 0; x < FB_W; x++)
			n += fb->px[y][x] != 0;
	return n;
}

static bool utf8_ok(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		int n = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : -1;
		if (n < 0)
			return false;
		p++;
		while (n--)
			if ((*p++ & 0xC0) != 0x80)
				return false;
	}
	return true;
}

static void test_data(void)
{
	size_t bytes = sizeof turkey_map_provinces + sizeof turkey_map_rings + sizeof turkey_map_steps +
		       sizeof turkey_map_names;
	printf("map data: %zu bytes (%u rings, %u steps, %zu name bytes)\n", bytes, TURKEY_MAP_RINGS, TURKEY_MAP_STEPS,
	       sizeof turkey_map_names);
	CHECK(bytes <= 40u * 1024u);
	CHECK(TURKEY_MAP_PROVINCES == 81);
	/* Turkey: 35.8 .. 42.2 N, 25.6 .. 44.9 E, in units from the origin */
	CHECK(TURKEY_MAP_XMIN >= 600 && TURKEY_MAP_XMAX <= 19900 && TURKEY_MAP_YMIN >= 300 && TURKEY_MAP_YMAX <= 6700);
	uint16_t next_ring = 0, next_step = 0;
	for (uint8_t p = 0; p < TURKEY_MAP_PROVINCES; p++) {
		const turkey_map_province_t *pr = &turkey_map_provinces[p];
		CHECK(pr->plate == p + 1u);
		CHECK(pr->ring_count >= 1 && pr->first_ring == next_ring);
		next_ring = (uint16_t)(next_ring + pr->ring_count);
		CHECK(pr->name < sizeof turkey_map_names && pname(p)[0]);
		if (strcmp(TURKEY_MAP_CHARSET, "utf-8") == 0)
			CHECK(utf8_ok(pname(p)));
		CHECK(pr->min.x >= TURKEY_MAP_XMIN && pr->max.x <= TURKEY_MAP_XMAX && pr->min.y >= TURKEY_MAP_YMIN &&
		      pr->max.y <= TURKEY_MAP_YMAX && pr->min.x < pr->max.x && pr->min.y < pr->max.y);
		for (uint16_t r = pr->first_ring; r < pr->first_ring + pr->ring_count && r < TURKEY_MAP_RINGS; r++) {
			const turkey_map_ring_t *ring = &turkey_map_rings[r];
			CHECK(ring->first == next_step && ring->count >= 3);
			next_step = (uint16_t)(next_step + ring->count);
			int32_t x = ring->start.x, y = ring->start.y, area2 = 0;
			bool in_box = true, moves = true;
			for (uint16_t i = ring->first; i < ring->first + ring->count && i < TURKEY_MAP_STEPS; i++) {
				int32_t nx = x + turkey_map_steps[i][0], ny = y + turkey_map_steps[i][1];
				moves &= turkey_map_steps[i][0] != 0 || turkey_map_steps[i][1] != 0;
				area2 += x * ny - nx * y;
				x = nx;
				y = ny;
				in_box &= x >= pr->min.x && x <= pr->max.x && y >= pr->min.y && y <= pr->max.y;
			}
			CHECK(x == ring->start.x && y == ring->start.y);	/* closed */
			CHECK(in_box && moves);
			CHECK(area2 > 0);	/* counter-clockwise */
		}
		/* the label point (the middle of its unit) lies in its own province */
		int32_t lat = TURKEY_MAP_LAT0_E7 + pr->label.y * TURKEY_MAP_UNIT_E7 + TURKEY_MAP_UNIT_E7 / 2;
		int32_t lon = TURKEY_MAP_LON0_E7 + pr->label.x * TURKEY_MAP_UNIT_E7 + TURKEY_MAP_UNIT_E7 / 2;
		int at = map_province_at(lat, lon);
		if (at != p)
			printf("label of %s falls in %d\n", pname(p), at);
		CHECK(at == p);
	}
	CHECK(next_ring == TURKEY_MAP_RINGS && next_step == TURKEY_MAP_STEPS);
	/* names: 81 strings back to back, UTF-8 Turkish letters kept */
	size_t off = 0;
	int count = 0;
	while (off < sizeof turkey_map_names) {
		off += strlen(&turkey_map_names[off]) + 1;
		count++;
	}
	CHECK(count == 81 && off == sizeof turkey_map_names);
	CHECK(strcmp(pname(5), "Ankara") == 0 && map_province_plate(5) == 6);
	if (strcmp(TURKEY_MAP_CHARSET, "utf-8") == 0) {
		CHECK(strcmp(pname(33), "\xC4\xB0stanbul") == 0);
		CHECK(strcmp(pname(62), "\xC5\x9E" "anl\xC4\xB1urfa") == 0);
		CHECK(strcmp(pname(80), "D\xC3\xBC" "zce") == 0);
	} else if (strcmp(TURKEY_MAP_CHARSET, "iso-8859-9") == 0) {
		CHECK(strcmp(pname(33), "\xDDstanbul") == 0);
		CHECK(strcmp(pname(62), "\xDE" "anl\xFDurfa") == 0);
	} else {
		CHECK(strcmp(pname(33), "Istanbul") == 0);
	}
	CHECK(strcmp(pname(81), "") == 0 && map_province_plate(200) == 0);
}

static void test_cities(void)
{
	static const struct {
		const char *name;
		int32_t lat, lon;
		uint8_t plate;
	} c[] = {
		{ "Ankara", 399334000, 328597000, 6 },
		{ "Istanbul", 410082000, 289784000, 34 },
		{ "Istanbul Umraniye", 410256000, 290963000, 34 },
		{ "Izmir", 384237000, 271428000, 35 },
		{ "Van", 385012000, 433730000, 65 },
		{ "Trabzon", 410027000, 397168000, 61 },
		{ "Antalya", 368969000, 307133000, 7 },
		{ "Erzurum", 399055000, 412658000, 25 },
		{ "Gaziantep", 370662000, 373833000, 27 },
		{ "Konya", 378746000, 324932000, 42 },
		{ "Edirne", 416771000, 265557000, 22 },
	};
	for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
		int p = map_province_at(c[i].lat, c[i].lon);
		if (p == MAP_NONE || map_province_plate((uint8_t)p) != c[i].plate)
			printf("%s: got %d\n", c[i].name, p);
		CHECK(p != MAP_NONE && map_province_plate((uint8_t)p) == c[i].plate);
	}
	CHECK(map_province_at(379838000, 237275000) == MAP_NONE);	/* Athens */
	CHECK(map_province_at(430000000, 350000000) == MAP_NONE);	/* the Black Sea */
	CHECK(map_province_at(-337000000, -1510000000) == MAP_NONE);	/* far away: no overflow */
	CHECK(map_province_near(430000000, 350000000, 5000) == MAP_NONE);
	CHECK(map_province_near(-337000000, -1510000000, 100000) == MAP_NONE);
	/* just off the shore of Samsun (41.36 N 36.33 E is the city on the coast): 3 km north */
	int s = map_province_at(413900000, 363300000);
	int n = map_province_near(413900000, 363300000, 10000);
	CHECK(n != MAP_NONE && map_province_plate((uint8_t)n) == 55);
	if (s != MAP_NONE)
		CHECK(s == n);
	/* inside stays inside */
	CHECK(map_province_near(399334000, 328597000, 10000) == 5);
}

static void test_projection(void)
{
	map_view_t v;
	map_view_init(&v, 0, 16, FB_W, 96);
	CHECK(v.zoom == 0 && v.kx < v.ky && v.kx > v.ky * 3 / 4);
	/* all of Turkey fits in the zoom 0 view */
	int x, y;
	CHECK(map_project(&v, 420000000, 260000000, &x, &y));
	CHECK(map_project(&v, 360000000, 446000000, &x, &y));
	uint32_t w0 = map_view_width_m(&v);
	CHECK(w0 > 1500000u && w0 < 2200000u);
	for (uint8_t z = 0; z < MAP_ZOOMS; z++) {
		map_view_set(&v, 399334000, 328597000, z);
		CHECK(v.zoom == z);
		/* the centre lands in the middle of the viewport: within a pixel, plus a unit's pixels from zoom 7 on (the
		 * view's centre is a whole unit, the position is projected exactly) */
		int tx = 1 + (int)(v.kx >> 16), ty = 1 + (int)(v.ky >> 16);
		CHECK(map_project(&v, 399334000, 328597000, &x, &y) && abs(x - FB_W / 2) <= tx && abs(y - (16 + 48)) <= ty);
		/* pixel -> position -> the same pixel, everywhere in the viewport */
		bool exact = true;
		for (int py = v.y; py < v.y + v.h; py += 7)
			for (int px = v.x; px < v.x + v.w; px += 5) {
				int32_t la, lo;
				int qx, qy;
				map_unproject(&v, px, py, &la, &lo);
				exact &= map_project(&v, la, lo, &qx, &qy) && qx == px && qy == py;
			}
		CHECK(exact);
		/* position -> pixel -> position within one pixel */
		int32_t la, lo;
		map_project(&v, 391234567, 331234567, &x, &y);
		map_unproject(&v, x, y, &la, &lo);
		int32_t tol_lat = (int32_t)(65536 / v.ky + 1) * TURKEY_MAP_UNIT_E7, tol_lon = (int32_t)(65536 / v.kx + 1) * TURKEY_MAP_UNIT_E7;
		CHECK(abs(la - 391234567) <= tol_lat && abs(lo - 331234567) <= tol_lon);
		if (z)
			CHECK(map_view_width_m(&v) * 2 > w0 >> z && map_view_width_m(&v) < (w0 >> (z - 1)));
	}
	/* the closest zooms (7, 8: finer than the data's 0.001 degree unit): marks 0.0005 degree (~50 m) apart are apart
	 * on screen, not snapped to the unit grid (owner's request 2026-11-20) */
	CHECK(MAP_ZOOMS == 9u);
	map_view_set(&v, 399334000, 328597000, MAP_ZOOMS - 1);
	{
		int ax = 0, ay = 0, bx = 0, by = 0;
		CHECK(map_project(&v, 399334000, 328597000, &ax, &ay) && map_project(&v, 399339000, 328603000, &bx, &by));
		CHECK(bx - ax >= 1 && ay - by >= 1);
	}
	/* zoom in / out keeps the centre and stops at the ends */
	map_view_set(&v, 385012000, 433730000, 2);
	int32_t la0, lo0, la, lo;
	map_view_center(&v, &la0, &lo0);
	for (int i = 0; i < (int)MAP_ZOOMS; i++)
		map_view_zoom(&v, 1);
	CHECK(v.zoom == MAP_ZOOMS - 1);
	map_view_zoom(&v, -1);
	map_view_center(&v, &la, &lo);
	CHECK(v.zoom == MAP_ZOOMS - 2 && la == la0 && lo == lo0);
	for (int i = 0; i < 10; i++)
		map_view_zoom(&v, -1);
	CHECK(v.zoom == 0);
	/* pan: east moves the centre east, back again returns within a unit */
	map_view_set(&v, 399334000, 328597000, 3);
	map_view_pan(&v, 40, 0);
	map_view_center(&v, &la, &lo);
	CHECK(lo > 328597000 && abs(la - 399330000) <= TURKEY_MAP_UNIT_E7);
	map_view_pan(&v, -40, 30);
	map_view_center(&v, &la, &lo);
	CHECK(la < 399330000 && abs(lo - 328590000) <= 2 * TURKEY_MAP_UNIT_E7);
	/* the centre cannot run away */
	for (int i = 0; i < 1000; i++)
		map_view_pan(&v, 80, -80);
	map_view_center(&v, &la, &lo);
	CHECK(lo <= TURKEY_MAP_LON0_E7 + (TURKEY_MAP_XMAX + 2000) * TURKEY_MAP_UNIT_E7);
	CHECK(la <= TURKEY_MAP_LAT0_E7 + (TURKEY_MAP_YMAX + 2000) * TURKEY_MAP_UNIT_E7);
	CHECK(!map_project(&v, 399334000, 328597000, &x, &y));
	/* far positions are clamped, not overflowed */
	CHECK(!map_project(&v, -899999999, -1799999999, &x, &y) && x < 0);
}

static void test_clip(void)
{
	int32_t a, b, c, d;
	a = 10; b = 10; c = 20; d = 20;
	CHECK(map_clip(&a, &b, &c, &d, 0, 0, 99, 49) && a == 10 && b == 10 && c == 20 && d == 20);
	a = -10; b = 5; c = 200; d = 5;
	CHECK(map_clip(&a, &b, &c, &d, 0, 0, 99, 49) && a == 0 && c == 99 && b == 5 && d == 5);
	a = 5; b = -100; c = 5; d = 100;
	CHECK(map_clip(&a, &b, &c, &d, 0, 0, 99, 49) && b == 0 && d == 49 && a == 5 && c == 5);
	a = -10; b = -10; c = -1; d = 60;	/* left of the box */
	CHECK(!map_clip(&a, &b, &c, &d, 0, 0, 99, 49));
	a = -50; b = 0; c = 0; d = -50;		/* misses the corner */
	CHECK(!map_clip(&a, &b, &c, &d, 0, 0, 99, 49));
	a = -50; b = -50; c = 150; d = 150;	/* the diagonal */
	CHECK(map_clip(&a, &b, &c, &d, 0, 0, 99, 49) && a == 0 && b == 0 && c == 49 && d == 49);
	a = 50; b = 25; c = 3000; d = -4000;	/* one end far out */
	CHECK(map_clip(&a, &b, &c, &d, 0, 0, 99, 49) && a == 50 && b == 25 && c >= 0 && c <= 99 && d == 0);
	a = 99; b = 49; c = 99; d = 49;		/* a point on the corner */
	CHECK(map_clip(&a, &b, &c, &d, 0, 0, 99, 49));
}

static void write_pgm(const char *path, const fb_t *fb)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P5\n%d %d\n255\n", FB_W, FB_H);
	for (int y = 0; y < FB_H; y++)
		for (int x = 0; x < FB_W; x++)
			fputc(fb->px[y][x] ? 255 : 0, f);
	fclose(f);
}

static void test_render(const char *pgm)
{
	static fb_t fb;
	map_view_t v;
	/* all of Turkey under a 16 px title and above a 16 px hint line */
	map_view_init(&v, 0, 16, FB_W, 96);
	uint32_t n = fb_render(&fb, &v);
	int lit = fb_count(&fb);
	printf("zoom 0: %u segments, %d pixels\n", n, lit);
	CHECK(n > 500 && lit > 1000);
	for (int x = 0; x < FB_W; x++)
		CHECK(fb.px[0][x] == 0 && fb.px[15][x] == 0 && fb.px[112][x] == 0);
	int provinces = 0;
	for (int p = 0; p < (int)TURKEY_MAP_PROVINCES; p++)
		provinces += fb.seen[p];
	CHECK(provinces == 81);
	int lx, ly;
	CHECK(map_label(&v, 5, &lx, &ly));	/* Ankara's label on screen */
	if (pgm)
		write_pgm(pgm, &fb);

	/* one province: Van at zoom 4; the view is inside Van, so there is little border but the label is central */
	map_view_set(&v, 385012000, 433730000, 4);
	n = fb_render(&fb, &v);
	CHECK(map_label(&v, 64, &lx, &ly));
	/* the closest zoom (8, ~0.06 degree across this viewport) draws too */
	map_view_set(&v, 385012000, 433730000, MAP_ZOOMS - 1);
	n = fb_render(&fb, &v);
	CHECK(n > 0 || fb_count(&fb) == 0);
	CHECK(n > 0 || fb_count(&fb) == 0);
	/* Istanbul at zoom 3: the Bosphorus provinces' borders cross the view, all clipped */
	map_view_set(&v, 410082000, 289784000, 3);
	n = fb_render(&fb, &v);
	CHECK(n > 10 && fb_count(&fb) > 50);
	/* open sea at zoom 4: nothing to draw */
	map_view_set(&v, 435000000, 350000000, MAP_ZOOMS - 1);
	CHECK(fb_render(&fb, &v) == 0 && fb_count(&fb) == 0);
	/* a viewport not at the screen's origin */
	map_view_init(&v, 20, 30, 100, 60);
	n = fb_render(&fb, &v);
	CHECK(n > 100);
	for (int y = 0; y < FB_H; y++)
		for (int x = 0; x < FB_W; x++)
			if (fb.px[y][x])
				CHECK(x >= 20 && x < 120 && y >= 30 && y < 90);
}

/* The district with this name (in the table's charset) in that province, MAP_DISTRICT_NONE without one */
static uint16_t district_named(uint8_t province, const char *name)
{
	for (uint16_t d = turkey_district_first[province]; d < turkey_district_first[province + 1u]; d++)
		if (strcmp(dname(d), name) == 0)
			return d;
	return MAP_DISTRICT_NONE;
}

/* Province p within km kilometres of the position: probes every km in eight directions */
static bool province_within(int32_t lat, int32_t lon, uint8_t p, int km)
{
	static const int dir[8][2] = { { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 } };
	for (int r = 1; r <= km; r++)
		for (int i = 0; i < 8; i++) {
			int32_t la = lat + dir[i][0] * r * 89830 * (dir[i][1] ? 7 : 10) / 10;	/* 1 km of latitude */
			int32_t lo = lon + dir[i][1] * r * 117000 * (dir[i][0] ? 7 : 10) / 10;	/* ~1 km at 40 N */
			if (map_province_at(la, lo) == p)
				return true;
		}
	printf("  no %s within %d km\n", pname(p), km);
	return false;
}

typedef struct {
	map_view_t v;
	uint16_t calls, bad;
} pt_ctx_t;

static void pt_fn(void *ctx, int x, int y, uint16_t district, const char *name)
{
	pt_ctx_t *c = ctx;
	c->calls++;
	if (x < c->v.x || x >= c->v.x + c->v.w || y < c->v.y || y >= c->v.y + c->v.h || district >= TURKEY_DISTRICTS ||
	    strcmp(name, dname(district)) != 0)
		c->bad++;
}

static void test_districts(void)
{
	size_t bytes = sizeof turkey_district_pos + sizeof turkey_district_first + sizeof turkey_district_name_first +
		       sizeof turkey_district_names;
	size_t all = bytes + sizeof turkey_map_provinces + sizeof turkey_map_rings + sizeof turkey_map_steps +
		     sizeof turkey_map_names;
	printf("district data: %zu bytes (%u districts, %zu name bytes); map data in all %zu bytes\n", bytes,
	       TURKEY_DISTRICTS, sizeof turkey_district_names, all);
	CHECK(TURKEY_DISTRICTS == 973 && map_district_count() == 973);
	CHECK(bytes <= 20u * 1024u && all <= 60u * 1024u);
	CHECK(strcmp(TURKEY_DISTRICT_CHARSET, TURKEY_MAP_CHARSET) == 0);
	/* by province, contiguous, every province has some (Bayburt has the fewest: 3), names back to back */
	CHECK(turkey_district_first[0] == 0 && turkey_district_first[TURKEY_MAP_PROVINCES] == TURKEY_DISTRICTS);
	size_t off = 0;
	int outside = 0;
	for (uint8_t p = 0; p < TURKEY_MAP_PROVINCES; p++) {
		uint16_t a = turkey_district_first[p], b = turkey_district_first[p + 1u];
		CHECK(b >= a + 3u && b <= a + 39u);
		CHECK(turkey_district_name_first[p] == off);
		for (uint16_t d = a; d < b; d++) {
			char name[MAP_NAME_MAX];
			memcpy(name, dname(d), sizeof name);
			CHECK(strcmp(name, &turkey_district_names[off]) == 0 && name[0] != '\0');
			off += strlen(name) + 1u;
			CHECK(map_district_province(d) == p);
			if (strcmp(TURKEY_DISTRICT_CHARSET, "utf-8") == 0)
				CHECK(utf8_ok(name));
			for (uint16_t e = a; e < d; e++)
				CHECK(strcmp(dname(e), name) != 0);	/* unique in the province */
			/* the centre lies in its own province, or (towns on a border or a shore: Natural Earth's outlines
			 * are ~1 km coarse and drop small islands) its province is at most 12 km away */
			int32_t lat, lon;
			map_district_pos(d, &lat, &lon);
			if (map_province_near(lat, lon, 3000) != p) {
				outside++;
				CHECK(province_within(lat, lon, p, 12));
			}
		}
	}
	printf("district centres off their simplified province: %d (all within 12 km)\n", outside);
	CHECK(outside <= 20);
	CHECK(off == sizeof turkey_district_names);
	CHECK(map_district_province(TURKEY_DISTRICTS) == 0xFF && dname(TURKEY_DISTRICTS)[0] == '\0');

	/* known names (official spellings) and the nearest-centre lookup */
	bool iso = strcmp(TURKEY_DISTRICT_CHARSET, "iso-8859-9") == 0, utf = strcmp(TURKEY_DISTRICT_CHARSET, "utf-8") == 0;
	const char *kadikoy = iso ? "Kad\xFDk\xF6y" : utf ? "Kad\xC4\xB1k\xC3\xB6y" : "Kadikoy";
	uint16_t k = district_named(33, kadikoy);
	CHECK(k != MAP_DISTRICT_NONE);
	/* GeoNames puts Kadikoy at 40.982 N 29.090 E (the middle of the district, not the pier) */
	CHECK(map_district_near(409850000, 290850000, -1, 5000) == k);
	CHECK(map_district_near(409850000, 290850000, 33, 5000) == k);
	CHECK(map_district_near(409850000, 290850000, 5, 5000) == MAP_DISTRICT_NONE);	/* not in Ankara */
	CHECK(map_district_near(430000000, 350000000, -1, 20000) == MAP_DISTRICT_NONE);	/* the Black Sea */
	CHECK(map_district_near(-337000000, -1510000000, -1, 100000) == MAP_DISTRICT_NONE);
	CHECK(district_named(5, iso ? "\xC7" "ankaya" : utf ? "\xC3\x87" "ankaya" : "Cankaya") != MAP_DISTRICT_NONE);
	CHECK(district_named(54, "19 May\xFDs") != MAP_DISTRICT_NONE || !iso);	/* not "Ondokuzmayis" */
	CHECK(district_named(29, "Derecik") != MAP_DISTRICT_NONE);		/* Hakkari: the 2018 district */
	CHECK(district_named(29, "Hakkari") != MAP_DISTRICT_NONE);		/* the centre, after its province */
	CHECK(district_named(78, "Kilis") != MAP_DISTRICT_NONE);		/* "Kilis Merkez" */
	CHECK(turkey_district_first[30] - turkey_district_first[29] == 5);	/* Hakkari: no tribal area */
	CHECK(turkey_district_first[34] - turkey_district_first[33] == 39);	/* Istanbul */
	CHECK(turkey_district_first[6] - turkey_district_first[5] == 25);	/* Ankara */
	/* no a / i / u with a circumflex (the fonts lack them; they would show in ISO-8859-9 as E2 / EE / FB) */
	for (size_t i = 0; i < sizeof turkey_district_names; i++)
		if (iso)
			CHECK((uint8_t)turkey_district_names[i] != 0xE2 && (uint8_t)turkey_district_names[i] != 0xEE &&
			      (uint8_t)turkey_district_names[i] != 0xFB);

	/* drawing: only centres inside the view, each once, with its name */
	pt_ctx_t c;
	memset(&c, 0, sizeof c);
	map_view_init(&c.v, 0, 16, FB_W, 96);
	CHECK(map_districts_draw(&c.v, pt_fn, &c) == c.calls && c.bad == 0);
	CHECK(c.calls > 900);		/* zoom 0: nearly all of them (a few share a pixel row at the edges) */
	memset(&c, 0, sizeof c);
	map_view_init(&c.v, 0, 16, FB_W, 96);
	map_view_set(&c.v, 410082000, 289784000, 5);	/* Istanbul, zoom 5 */
	uint16_t n = map_districts_draw(&c.v, pt_fn, &c);
	printf("Istanbul zoom 5: %u districts in view\n", n);
	CHECK(n == c.calls && c.bad == 0 && n >= 20);
	memset(&c, 0, sizeof c);
	map_view_init(&c.v, 0, 16, FB_W, 96);
	map_view_set(&c.v, 435000000, 350000000, MAP_ZOOMS - 1);	/* open sea */
	CHECK(map_districts_draw(&c.v, pt_fn, &c) == 0 && c.calls == 0);
}

static void test_occ(void)
{
	map_occ_t o;
	map_occ_init(&o, 0, 16, 320, 192);	/* cells 20 x 8 */
	CHECK(o.cw == 20 && o.ch == 8);
	CHECK(map_occ_take(&o, 10, 20, 30, 8));		/* cells 0..1, rows 0..1 */
	CHECK(!map_occ_take(&o, 35, 25, 10, 2));	/* overlaps cell 1 row 1 */
	CHECK(map_occ_take(&o, 45, 20, 10, 4));		/* cell 2 row 0: free */
	CHECK(map_occ_take(&o, 10, 40, 30, 8));		/* rows 3..4 */
	CHECK(!map_occ_take(&o, -1, 40, 5, 5) && !map_occ_take(&o, 0, 10, 5, 5));	/* outside the area */
	CHECK(!map_occ_take(&o, 300, 20, 30, 5) && !map_occ_take(&o, 10, 200, 5, 10));
	CHECK(map_occ_take(&o, 300, 200, 20, 8));	/* the last cell */
	CHECK(!map_occ_take(&o, 100, 100, 0, 5));
	map_occ_init(&o, 0, 0, 160, 96);
	CHECK(map_occ_take(&o, 0, 0, 160, 96) && !map_occ_take(&o, 80, 50, 1, 1));
	map_occ_init(&o, 0, 0, 160, 96);
	CHECK(map_occ_take(&o, 80, 50, 1, 1));
}

int main(int argc, char **argv)
{
	/* the tables are the map pack's on the data chip since 1.1 (refpack_fixture.h) */
	if (!refpack_fixture_install(REFPACK_MAP)) {
		printf("FAIL: the map pack fixture\n");
		return 1;
	}
	test_data();
	test_cities();
	test_projection();
	test_clip();
	test_render(argc > 1 ? argv[1] : NULL);
	test_districts();
	test_occ();
	if (failures) {
		printf("test_map: %d FAILED\n", failures);
		return 1;
	}
	printf("test_map: OK\n");
	return 0;
}
