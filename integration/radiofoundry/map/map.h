/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus offline Turkey map (docs/map.md): the renderer core, portable (no RTOS, no float, no heap) so the host
 * tests run it as is. The 81 province outlines (turkey_map_data.h, Natural Earth, public domain) are projected
 * equirectangularly with the cos(latitude) of the view's centre, clipped to the viewport and handed to a line
 * callback; the caller draws, so the same core feeds the TFT and a test framebuffer. Also: which province a
 * position is in (point in polygon on the simplified outlines) for the il name under the GPS position. */
#ifndef PLUS_MAP_H
#define PLUS_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAP_NAME_MAX		24u	/* a province or district name with its NUL (the longest is 16 characters) */
#define MAP_ZOOMS		9u	/* 0: all of Turkey .. 4: one province .. 6: a few districts .. 8: a town (~40 m a pixel) */
#define MAP_NONE		(-1)	/* no province */
#define MAP_DISTRICT_NONE	0xFFFFu	/* no district */

typedef struct {
	int16_t x, y, w, h;	/* the viewport on the screen */
	int32_t cx, cy;		/* its centre in map units (0.001 degree east / north of the data's origin) */
	int32_t kx, ky;		/* pixels per unit, Q16 (kx carries the cos(latitude) of the centre) */
	uint8_t zoom;
} map_view_t;

/* A segment already clipped to the viewport (both ends inside it); province 0..80 (plate - 1) */
typedef void (*map_line_fn)(void *ctx, int x0, int y0, int x1, int y1, uint8_t province);

/* The viewport (screen pixels), centred on Turkey at zoom 0 */
void map_view_init(map_view_t *v, int x, int y, int w, int h);
/* Centre on a position (degrees x 1e7) and pick a zoom (clamped); the centre stays near Turkey */
void map_view_set(map_view_t *v, int32_t lat_e7, int32_t lon_e7, uint8_t zoom);
void map_view_zoom(map_view_t *v, int dir);		/* +1 in, -1 out, keeping the centre */
void map_view_pan(map_view_t *v, int dx_px, int dy_px);	/* moves the view's centre by pixels (+x east, +y south) */
void map_view_center(const map_view_t *v, int32_t *lat_e7, int32_t *lon_e7);
/* Metres across the viewport's width (for a scale bar) */
uint32_t map_view_width_m(const map_view_t *v);

/* Position -> screen pixel; false when it falls outside the viewport (x, y are still set, clamped far away) */
bool map_project(const map_view_t *v, int32_t lat_e7, int32_t lon_e7, int *x, int *y);
/* Screen pixel -> position (the pixel's top-left corner) */
void map_unproject(const map_view_t *v, int x, int y, int32_t *lat_e7, int32_t *lon_e7);

/* Cohen-Sutherland: clips the segment to [x0r, x1r] x [y0r, y1r] (inclusive); false when nothing is left */
bool map_clip(int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1, int32_t x0r, int32_t y0r, int32_t x1r, int32_t y1r);

/* The map pack (storage/ref_pack.h) holds the map's sections: without it nothing below draws or finds anything
 * (map_draw 0, map_province_at MAP_NONE, no districts, names ""). UI task only, like every map pack reader */
bool map_ok(void);
/* Every province border inside the view through fn; consecutive points on the same pixel are merged. Returns the
 * number of segments handed out */
uint32_t map_draw(const map_view_t *v, map_line_fn fn, void *ctx);
/* One province's label point on the screen; false when it is outside the viewport */
bool map_label(const map_view_t *v, uint8_t province, int *x, int *y);

/* The province containing the position, MAP_NONE outside every outline */
int map_province_at(int32_t lat_e7, int32_t lon_e7);
/* As map_province_at, else the province whose border is nearest within max_m metres (coasts and borders are
 * simplified: a GPS fix on the shore may fall just outside). max_m is capped at 50 km */
int map_province_near(int32_t lat_e7, int32_t lon_e7, uint32_t max_m);
/* The name into out (TURKEY_MAP_CHARSET, cut to n - 1, always terminated; "" for none) */
void map_province_name(uint8_t province, char *out, size_t n);
uint8_t map_province_plate(uint8_t province);		/* 1..81 */

/* Districts (ilce, turkey_district_data.h: GeoNames, CC BY 4.0): 973 points, the district centre and its name
 * (TURKEY_DISTRICT_CHARSET), numbered by province (plate order), then by name */
typedef void (*map_point_fn)(void *ctx, int x, int y, uint16_t district, const char *name);
uint16_t map_district_count(void);
uint8_t map_district_province(uint16_t d);		/* 0..80 (plate - 1), 0xFF: no such district */
void map_district_name(uint16_t d, char *out, size_t n);	/* as map_province_name; "" for no such district */
void map_district_pos(uint16_t d, int32_t *lat_e7, int32_t *lon_e7);
/* Every district centre inside the view through fn (provinces outside it are skipped whole); the name fn gets is a
 * buffer valid until fn returns. Returns how many */
uint16_t map_districts_draw(const map_view_t *v, map_point_fn fn, void *ctx);
/* The district whose centre is nearest, within max_m metres (capped at 100 km), only in that province when
 * province >= 0; MAP_DISTRICT_NONE when none is that close */
uint16_t map_district_near(int32_t lat_e7, int32_t lon_e7, int province, uint32_t max_m);

/* Room for labels: the viewport split into MAP_OCC_COLS x MAP_OCC_ROWS cells; a label takes the cells its box
 * touches, or nothing when one of them is taken already (labels never overlap; the first drawn wins) */
#define MAP_OCC_COLS	16u
#define MAP_OCC_ROWS	24u
typedef struct {
	int16_t x, y, cw, ch;		/* the area's corner and the cell size, pixels */
	uint16_t row[MAP_OCC_ROWS];	/* bit c: cell (c, row) taken */
} map_occ_t;
void map_occ_init(map_occ_t *o, int x, int y, int w, int h);
/* Takes the box's cells if all are free and the box is inside the area; false (nothing taken) otherwise */
bool map_occ_take(map_occ_t *o, int x, int y, int w, int h);

#endif
