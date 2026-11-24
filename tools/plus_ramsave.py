# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus RAM saving for the software P25 probe (v1.0.2 candidate, docs/plus.md "RAM budget"). Called from
plus_features.prepare(); one line there. Two of the three savings of RadioFoundry 454a5b76 (#792, MIT, "RAM: one
settings slot buffer, the start-up stack in the RTOS heap, a safe-area frame buffer"); the third, the start-up stack
in the RTOS heap, this tree already has (prepare_startup_stack). The C code below follows that commit; the switches,
the host tests and the glue are ours.

1. A frame buffer of the safe area only (PLUS_FB_SAFE_BYTES, plus_features.PLUS_RAM_TRIM): the DM-32UV's 240x320
   16-color frame buffer held the whole panel (38,400 B), but nothing is ever drawn in the bezel's insets (10 px left
   and right, 5 top and bottom): every drawing call is clipped to the safe area, and gfx_clear leaves the insets in
   palette entry 0, the clear color. Now the frame buffer holds the 220 x 310 safe area (34,100 B, -4,300 B), its rows
   go to that window of the panel (st7789_write_window_indexed), and the board paints the insets in the clear color
   with the first frame, after gfx_invalidate and whenever the clear color changes (hal_display_t fill_insets).
   gfx_read_rgb565 (the debug link's RGB565 screenshot) still gives the whole panel, the insets in the clear color as
   before. The packed (LZ4) screenshot answers "unavailable" for a safe-area frame buffer, as for one too large to
   pack, and every host tool then reads the RGB565 form: the same picture, more bytes. (Upstream instead sends the safe
   area with a new flag, byte 27 bit 1, which a host tool before it misreads; not taken.) The panel shows the same
   pixels: the gallery test (tests/plus/test_fbsafe_gallery.c) draws every gallery screen both ways and compares the
   emulated panel after every frame. GFX_FB_BYTES stays the panel's 38,400 (tools/boards.py's check); only gfx.c's
   array is sized by PLUS_FB_SAFE_BYTES, and display_dm32.c checks at build time that it is exactly the safe area's.
   Other boards and Classic builds keep the whole-panel frame buffer (fb_safe_area false).
2. One settings slot buffer and one storage slot buffer (PLUS_SHARED_SLOT_BUF): settings.c's next_slot and put_record's
   read-back, and storage.c's scan_region and mark, each used two 256-byte static buffers at different times only,
   all under the storage lock: each pair shares one now (-512 B). next_slot's scan is over before put_record reads its
   record back; mark runs after scan_region's loop (or alone, from storage_claim).
"""

FB_HEAD_OLD = ('static uint32_t fb_words[GFX_FB_BYTES / 4];\nstatic const hal_display_t *display;\n'
               'static int width, height, bpp, stride;\t// stride in bytes\n')
FB_HEAD_NEW = '''// DDM32 Plus (tools/plus_ramsave.py, from RadioFoundry 454a5b76, MIT): with PLUS_FB_SAFE_BYTES the array holds a board's
// safe area only (hal_display_t fb_safe_area); GFX_FB_BYTES stays the panel's. Words for the alignment, rounded up: a
// safe area need not be a multiple of 4 bytes
#ifdef PLUS_FB_SAFE_BYTES
#define GFX_FB_CAP\tPLUS_FB_SAFE_BYTES
#else
#define GFX_FB_CAP\tGFX_FB_BYTES
#endif
static uint32_t fb_words[(GFX_FB_CAP + 3u) / 4u];
static const hal_display_t *display;
static int width, height, bpp, stride;\t// the frame buffer's: the panel's, or its safe area's (fb_safe); stride in bytes
// The panel, and where the frame buffer's top left pixel is on it: 0, 0 unless the frame buffer holds the safe area
// only (hal_display_t fb_safe_area). Then the insets are never in RAM: margin is their color, the last gfx_clear's
// (palette entry 0, which no other color takes), and gfx_flush has the board paint them when it changed (margin_sent)
static int pw, ph, fx, fy;
static bool fb_safe, insets_dirty;\t// insets_dirty: the first frame, or gfx_invalidate (the panel may have lost them)
static color_t margin, margin_sent;
'''

INIT_OLD = '''\tdisplay = disp;
\twidth = disp->width;
\theight = disp->height;
\tbpp = disp->bpp;
\tstride = bpp == 1 ? (width + 7) / 8 : bpp == 4 ? (width + 1) / 2 : width * 2;
\tif ((bpp != 1 && bpp != 16 && (bpp != 4 || !GFX_INDEXED || disp->flush_indexed == NULL)) ||
\t    (uint32_t)stride * (uint32_t)height > GFX_FB_BYTES || height > (int)GFX_MAX_ROWS) {
\t\twidth = height = 0;
\t\tdisplay = NULL;
\t\treturn false;
\t}
\tox = disp->inset_left;
\toy = disp->inset_top;
\tvw = width - disp->inset_left - disp->inset_right;
\tvh = height - disp->inset_top - disp->inset_bottom;
\tif (vw < 32 || vh < 32) {\t// insets that leave nothing usable: ignore them
\t\tox = oy = 0;
\t\tvw = width;
\t\tvh = height;
\t}
\tgfx_clip_off();
'''
INIT_NEW = '''\tdisplay = disp;
\tpw = width = disp->width;
\tph = height = disp->height;
\tfx = fy = 0;
\tbpp = disp->bpp;
\tox = disp->inset_left;
\toy = disp->inset_top;
\tvw = width - disp->inset_left - disp->inset_right;
\tvh = height - disp->inset_top - disp->inset_bottom;
\tif (vw < 32 || vh < 32) {\t// insets that leave nothing usable: ignore them
\t\tox = oy = 0;
\t\tvw = width;
\t\tvh = height;
\t}
\t// The safe area only: every drawing call is clipped to it, so the frame buffer's coordinates are the safe area's and
\t// the insets are the board's to paint (fill_insets). Palette entry 0 is their color: bpp 4 only
\tfb_safe = disp->fb_safe_area && bpp == 4 && disp->fill_insets != NULL && (ox != 0 || oy != 0 || vw != width ||
\t\t  vh != height);
\tif (fb_safe) {
\t\tfx = ox;
\t\tfy = oy;
\t\twidth = vw;
\t\theight = vh;
\t\tox = oy = 0;
\t}
\tstride = bpp == 1 ? (width + 7) / 8 : bpp == 4 ? (width + 1) / 2 : width * 2;
\tif ((bpp != 1 && bpp != 16 && (bpp != 4 || !GFX_INDEXED || disp->flush_indexed == NULL)) ||
\t    (uint32_t)stride * (uint32_t)height > GFX_FB_CAP || ph > (int)GFX_MAX_ROWS) {
\t\twidth = height = pw = ph = 0;
\t\tvw = vh = 0;
\t\tdisplay = NULL;
\t\treturn false;
\t}
\tmargin = margin_sent = 0;\t// the frame buffer starts as zeros, the palette's entry 0 black
\tinsets_dirty = true;
\tgfx_clip_off();
'''

CLEAR_OLD = ('\ttrace(GFX_TRACE_CLEAR, 0, 0, vw, vh, NULL);\n#endif\n#if GFX_INDEXED\n'
             '\tif (bpp == 4) {\t\t// nothing drawn before is left: the palette starts over with c\n')
CLEAR_NEW = ('\ttrace(GFX_TRACE_CLEAR, 0, 0, vw, vh, NULL);\n#endif\n\tmargin = c;\n#if GFX_INDEXED\n'
             '\tif (bpp == 4) {\t\t// nothing drawn before is left: the palette starts over with c\n')

READ_OLD = '''void gfx_read_rgb565(uint32_t off, uint8_t *out, size_t n)
{
\tuint32_t total = (uint32_t)width * (uint32_t)height * 2u;
\tfor (size_t k = 0; k < n; k++, off++) {
\t\tcolor_t c = 0;
\t\tif (off < total && bpp != 1)
\t\t\tc = raw_pixel((int)(off / 2u % (uint32_t)width), (int)(off / 2u / (uint32_t)width));
\t\tout[k] = (uint8_t)((off & 1u) ? c >> 8 : c);
\t}
}
'''
READ_NEW = '''// A panel pixel's frame buffer coordinates: false for one in the insets of a safe-area frame buffer
static bool on_fb(int *x, int *y)
{
\t*x -= fx;
\t*y -= fy;
\treturn *x >= 0 && *y >= 0 && *x < width && *y < height;
}

void gfx_read_rgb565(uint32_t off, uint8_t *out, size_t n)
{
\tuint32_t total = (uint32_t)pw * (uint32_t)ph * 2u;
\tfor (size_t k = 0; k < n; k++, off++) {
\t\tcolor_t c = 0;
\t\tif (off < total && bpp != 1) {
\t\t\tint x = (int)(off / 2u % (uint32_t)pw), y = (int)(off / 2u / (uint32_t)pw);
\t\t\tc = on_fb(&x, &y) ? raw_pixel(x, y) : margin;
\t\t}
\t\tout[k] = (uint8_t)((off & 1u) ? c >> 8 : c);
\t}
}

void gfx_read_packed(uint32_t off, uint8_t *out, size_t n)
{
\tif (!fb_safe) {\t\t// the frame buffer is the panel
\t\tuint32_t total = (uint32_t)stride * (uint32_t)height;
\t\tfor (size_t k = 0; k < n; k++, off++)
\t\t\tout[k] = off < total ? ((const uint8_t *)fb_words)[off] : 0;
\t\treturn;
\t}
\t// bpp 4 (gfx_init): two pixels a byte, the insets palette entry 0
\tuint32_t pstride = ((uint32_t)pw + 1u) / 2u, total = pstride * (uint32_t)ph;
\tfor (size_t k = 0; k < n; k++, off++) {
\t\tuint8_t b = 0;
\t\tfor (int h = 0; h < 2 && off < total; h++) {
\t\t\tint x = (int)(off % pstride * 2u) + h, y = (int)(off / pstride);
\t\t\tif (x < pw && on_fb(&x, &y))
\t\t\t\tb |= (uint8_t)((((const uint8_t *)fb_words)[y * stride + (x >> 1)] >> ((x & 1) ? 0 : 4) & 0x0Fu) <<
\t\t\t\t\t       (h ? 0 : 4));
\t\t}
\t\tout[k] = b;
\t}
}

void gfx_fb_rect(gfx_fb_rect_t *r)
{
\tr->x = (uint16_t)fx;
\tr->y = (uint16_t)fy;
\tr->w = (uint16_t)width;
\tr->h = (uint16_t)height;
\tr->stride = (uint32_t)stride;
\tr->safe_area = fb_safe;
}
'''

FLUSH_OLD = '''#endif
\tint band = -1;
\tfor (int y = 0; y < height; y++) {
\t\tuint32_t h = hash_row(y);
\t\tbool changed = all_dirty || h != row_hash[y];
\t\trow_hash[y] = h;
#if CONFIG_NET_WEBCTL
\t\tif (changed)
\t\t\trow_new[y >> 3] |= (uint8_t)(1u << (y & 7));
#endif
'''
FLUSH_NEW = '''#endif
\t// The insets of a safe-area frame buffer: painted with the first frame and when the background changed (entry 0,
\t// their color, only changes with gfx_clear), before the rows. Every panel row has some of them (the left and right
\t// ones), so a copy elsewhere gets every row again
\tif (fb_safe && (insets_dirty || margin != margin_sent)) {
\t\tdisplay->fill_insets(margin);
\t\tmargin_sent = margin;
\t\tinsets_dirty = false;
#if CONFIG_NET_WEBCTL
\t\tgfx_rows_mark_all();
#endif
\t}
\tint band = -1;
\tfor (int y = 0; y < height; y++) {
\t\tuint32_t h = hash_row(y);
\t\tbool changed = all_dirty || h != row_hash[y];
\t\trow_hash[y] = h;
#if CONFIG_NET_WEBCTL
\t\tif (changed)
\t\t\trow_new[(y + fy) >> 3] |= (uint8_t)(1u << ((y + fy) & 7));
#endif
'''

ROWS_OLD = ('\tfor (int y = from < 0 ? 0 : from; y < height; y++)\n',
            '\tfor (int y = from; y <= to && y < height; y++)\n')
ROWS_NEW = ('\tfor (int y = from < 0 ? 0 : from; y < ph; y++)\n',
            '\tfor (int y = from; y <= to && y < ph; y++)\n')

INVALIDATE_OLD = 'void gfx_invalidate(void)\n{\n\tall_dirty = true;\n}\n'
INVALIDATE_NEW = 'void gfx_invalidate(void)\n{\n\tall_dirty = true;\n\tinsets_dirty = true;\n}\n'

GFX_H_OLD = '''// n bytes of the frame as RGB565 (little endian, rows of width * 2 bytes) from byte off: screen captures of a bpp 4
// panel. At bpp 16 the frame buffer itself
void gfx_read_rgb565(uint32_t off, uint8_t *out, size_t n);
'''
GFX_H_NEW = '''// n bytes of the frame as RGB565 (little endian, rows of width * 2 bytes) from byte off: screen captures of a bpp 4
// panel. At bpp 16 the frame buffer itself. Always the whole panel: a safe-area frame buffer (hal_display_t
// fb_safe_area) has its insets filled in with the background
void gfx_read_rgb565(uint32_t off, uint8_t *out, size_t n);
// The same at the panel's own depth: n bytes of the whole panel as a frame buffer of it would hold them (rows of
// (width * bpp + 7) / 8 bytes, the leftmost pixel in the high bits), from byte off. A safe-area frame buffer's insets
// are palette entry 0, the background
void gfx_read_packed(uint32_t off, uint8_t *out, size_t n);
// Where the frame buffer lies on the panel: the whole panel (x = y = 0), or with fb_safe_area its safe area. stride:
// bytes a row. For a capture that sends the frame buffer as it is (the debug link's packed screenshot)
typedef struct {
\tuint16_t x, y, w, h;
\tuint32_t stride;
\tbool safe_area;
} gfx_fb_rect_t;
void gfx_fb_rect(gfx_fb_rect_t *r);
'''

HAL_OLD = ('\tuint8_t inset_top, inset_bottom, inset_left, inset_right;\n\tvoid (*init)(void);\n'
           '\t// Show rows y0 .. y1-1 of a full width x height frame buffer\n')
HAL_NEW = '''\tuint8_t inset_top, inset_bottom, inset_left, inset_right;
\t// The frame buffer holds the safe area only, not the whole panel (bpp 4 only): nothing is drawn in the insets, so a
\t// board whose RAM is short keeps (width x height - safe area) pixels fewer. flush_indexed then gets rows of a
\t// (width - inset_left - inset_right) x (height - inset_top - inset_bottom) buffer, its row 0 the safe area's top
\t// row, and shows them inside the insets; fill_insets(color) paints the four insets in the background color, which
\t// gfx asks for with the first frame and whenever the background changes. Screen captures rebuild the whole panel
\t// (gfx_read_rgb565, gfx_read_packed). DDM32 Plus: RadioFoundry 454a5b76 (tools/plus_ramsave.py)
\tbool fb_safe_area;
\tvoid (*fill_insets)(uint16_t rgb565);
\tvoid (*init)(void);
\t// Show rows y0 .. y1-1 of a full width x height frame buffer (with fb_safe_area: of the safe area's buffer)
'''

ST_H_OLD = 'void st7789_fill(const st7789_t *p, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);\n'
ST_H_NEW = ('// Rows y0 .. y1-1 of a 16-color frame buffer of w pixels a row (stride (w + 1) / 2) that covers a part of the panel\n'
            '// from (x, y) (hal_display_t fb_safe_area: the safe area inside the insets), at scale 1. Clipped to the panel\n'
            'void st7789_write_window_indexed(const st7789_t *p, const uint8_t *fb, const uint16_t *palette, uint16_t x, uint16_t y,\n'
            '\t\t\t\t uint16_t w, uint16_t y0, uint16_t y1);\n' + ST_H_OLD)
ST_C_OLD = 'void st7789_invert(const st7789_t *p, bool on)\n{\n'
ST_C_NEW = '''void st7789_write_window_indexed(const st7789_t *p, const uint8_t *fb, const uint16_t *palette, uint16_t x, uint16_t y,
\t\t\t\t uint16_t w, uint16_t y0, uint16_t y1)
{
\tconst st7789_bus_t *b = p->bus;
\tuint32_t stride = (w + 1u) / 2u;
\tif (x >= p->width || y >= p->height || w > p->width - x || w == 0 || w > ST7789_MAX_LINE) {
\t\treturn;
\t}
\tif (y1 > p->height - y) {
\t\ty1 = (uint16_t)(p->height - y);
\t}
\tif (y0 >= y1) {
\t\treturn;
\t}
\tb->begin();
\twindow(p, x, (uint16_t)(y + y0), (uint16_t)(x + w), (uint16_t)(y + y1));
\tfor (uint32_t r = y0; r < y1; r++) {
\t\tconst uint8_t *row = fb + r * stride;
\t\tfor (uint32_t i = 0; i < w; i++) {
\t\t\tline[i] = palette[(row[i >> 1] >> ((i & 1u) ? 0 : 4)) & 0x0Fu];
\t\t}
\t\tb->pixels(line, w);
\t}
\tb->end();
}

''' + ST_C_OLD

DM32_DEFS_OLD = '#define PANEL_H\t\t\t320u\n'
DM32_DEFS_NEW = '''#define PANEL_H\t\t\t320u
#ifdef PLUS_FB_SAFE_BYTES
// DDM32 Plus: a frame buffer of the safe area only (tools/plus_ramsave.py, RadioFoundry 454a5b76): its rows go to that
// window of the panel and the insets are painted in the background color (fill_insets). PLUS_FB_SAFE_BYTES
// (plus_features.PLUS_RAM_TRIM) sizes gfx.c's array: exactly the safe area's, so a change of the insets fails here
#define SAFE_W\t\t\t(PANEL_W - CONFIG_DISPLAY_INSET_LEFT - CONFIG_DISPLAY_INSET_RIGHT)
#define SAFE_H\t\t\t(PANEL_H - CONFIG_DISPLAY_INSET_TOP - CONFIG_DISPLAY_INSET_BOTTOM)
_Static_assert((SAFE_W + 1u) / 2u * SAFE_H == PLUS_FB_SAFE_BYTES, "PLUS_FB_SAFE_BYTES is not the safe area's frame buffer");
#if CONFIG_NET_WEBCTL || CONFIG_NET_HTTP
#error "the web control page and the HTTP screenshot read the frame buffer as the whole panel's (not ported)"
#endif
#endif
'''
DM32_FLUSH_OLD = '''\tst7789_write_rows_indexed(&panel, (const uint8_t *)fb, palette, y0, y1);
}
'''
DM32_FLUSH_NEW = '''#ifdef PLUS_FB_SAFE_BYTES
\tst7789_write_window_indexed(&panel, (const uint8_t *)fb, palette, CONFIG_DISPLAY_INSET_LEFT,
\t\t\t\t    CONFIG_DISPLAY_INSET_TOP, SAFE_W, y0, y1);
#else
\tst7789_write_rows_indexed(&panel, (const uint8_t *)fb, palette, y0, y1);
#endif
}

#ifdef PLUS_FB_SAFE_BYTES
// The four insets in the background color: the strips above and below the safe area across the whole width, then the
// two beside it (st7789_fill skips an empty one)
static void fill_insets(uint16_t color)
{
\tif (!ready) {
\t\treturn;
\t}
\tconst uint16_t l = CONFIG_DISPLAY_INSET_LEFT, t = CONFIG_DISPLAY_INSET_TOP;
\tst7789_fill(&panel, 0, 0, PANEL_W, t, color);
\tst7789_fill(&panel, 0, (uint16_t)(t + SAFE_H), PANEL_W, PANEL_H, color);
\tst7789_fill(&panel, 0, t, l, (uint16_t)(t + SAFE_H), color);
\tst7789_fill(&panel, (uint16_t)(l + SAFE_W), t, PANEL_W, (uint16_t)(t + SAFE_H), color);
}
#endif
'''
DM32_STRUCT_OLD = '\t.bpp = 4,\n\t.init = init,\n'
DM32_STRUCT_NEW = '\t.bpp = 4,\n#ifdef PLUS_FB_SAFE_BYTES\n\t.fb_safe_area = true,\n\t.fill_insets = fill_insets,\n#endif\n\t.init = init,\n'

DL_PACKED_OLD = '''	uint32_t stride = d->bpp == 1 ? (d->width + 7u) / 8u : d->bpp == 4 ? (d->width + 1u) / 2u : d->width * 2u;

	s.fb = gfx_framebuffer();
'''
DL_PACKED_NEW = '''	uint32_t stride = d->bpp == 1 ? (d->width + 7u) / 8u : d->bpp == 4 ? (d->width + 1u) / 2u : d->width * 2u;
	gfx_fb_rect_t r;

	// DDM32 Plus (tools/plus_ramsave.py): a frame buffer of the safe area only is not the panel's rows. The host then
	// takes the RGB565 form, as for a frame buffer too large to pack, which rebuilds the whole panel as before
	gfx_fb_rect(&r);
	if (r.safe_area) {
		return status_only(out, DL_ST_UNAVAILABLE);
	}
	s.fb = gfx_framebuffer();
'''
DL_RAW_OLD = '\t\tmemcpy(out + 19, (const uint8_t *)gfx_framebuffer() + offset, n);\n'
DL_RAW_NEW = '\t\tgfx_read_packed(offset, out + 19, n);\t// the whole panel\'s rows, whatever the frame buffer holds\n'
# The UI simulator: the DM-32UV class draws into a safe-area frame buffer as the radio does, and an emulated panel
# takes what gfx_flush sends (the gallery test compares it with the whole-panel frame buffer's)
SIM_H_OLD = '\tbool big;\t\t// lists and labels one size up: the radio is built with CONFIG_FONT_BIG\n} sim_geom_t;\n'
SIM_H_NEW = ('\tbool big;\t\t// lists and labels one size up: the radio is built with CONFIG_FONT_BIG\n'
             '\tbool safe;\t\t// the frame buffer holds the safe area only, as the radio builds it (hal_display_t fb_safe_area)\n'
             '} sim_geom_t;\n')
SIM_H_TAIL_OLD = '// Save the frame buffer as a PNG (scale 1..4)\n'
SIM_H_TAIL_NEW = ('// The panel as the board\'s display driver gets it from gfx_flush (flush_indexed rows, fill_insets), width x height\n'
                  '// RGB565, row after row: what the screen shows. Zeros (black, as the panel starts) after sim_reset. DDM32 Plus\n'
                  'const uint16_t *sim_panel(void);\n' + SIM_H_TAIL_OLD)
SIM_GEOMS_OLD = '''\t{ "160x120i5", 160, 120, 4, 5, 0, false, false },\t// DM-1701: the lens hides 5 px left and right (boards/dm1701/config.mk)
\t{ "160x128", 160, 128, 4, 0, 0, false, false },\t// MD-380 / MD-UV380 / MD-9600 class
\t{ "128x64", 128, 64, 1, 0, 0, false, false },\t// GD-77 class, mono
\t// DM-32UV: 16 colors, the bezel hides 10 px left and right, 5 top and bottom; FONT_BIG
\t{ "240x320i10x5", 240, 320, 4, 10, 5, true, true },
\t{ "240x320", 240, 320, 16, 0, 0, true, false },\t// 240x320 class at full resolution, standard text size
'''
SIM_GEOMS_NEW = '''\t{ "160x120i5", 160, 120, 4, 5, 0, false, false, false },\t// DM-1701: the lens hides 5 px left and right (boards/dm1701/config.mk)
\t{ "160x128", 160, 128, 4, 0, 0, false, false, false },\t// MD-380 / MD-UV380 / MD-9600 class
\t{ "128x64", 128, 64, 1, 0, 0, false, false, false },\t// GD-77 class, mono
\t// DM-32UV: 16 colors, the bezel hides 10 px left and right, 5 top and bottom; FONT_BIG; a frame buffer of the
\t// safe area only (DDM32 Plus PLUS_FB_SAFE_BYTES, tools/plus_ramsave.py)
\t{ "240x320i10x5", 240, 320, 4, 10, 5, true, true, true },
\t{ "240x320", 240, 320, 16, 0, 0, true, false, false },\t// 240x320 class at full resolution, standard text size
'''
SIM_DISP_OLD = '''static void d_flush_indexed(const void *fb, const uint16_t *pal, uint16_t y0, uint16_t y1)
{
\t(void)fb; (void)pal; (void)y0; (void)y1;
}
static void d_backlight(uint8_t p) { (void)p; }
static hal_display_t disp = {'''
SIM_DISP_NEW = '''static hal_display_t disp;
// DDM32 Plus: the panel as the DM-32UV's driver writes it (st7789_write_rows_indexed, and with fb_safe_area
// st7789_write_window_indexed inside the insets and st7789_fill for them)
static uint16_t panel_px[320 * 320];
const uint16_t *sim_panel(void) { return panel_px; }
static void d_flush_indexed(const void *fb, const uint16_t *pal, uint16_t y0, uint16_t y1)
{
\tint x0 = disp.fb_safe_area ? disp.inset_left : 0, yo = disp.fb_safe_area ? disp.inset_top : 0;
\tint w = disp.fb_safe_area ? disp.width - disp.inset_left - disp.inset_right : disp.width, stride = (w + 1) / 2;
\tfor (int y = y0; y < y1 && yo + y < disp.height; y++)
\t\tfor (int x = 0; x < w; x++)
\t\t\tpanel_px[(yo + y) * disp.width + x0 + x] =
\t\t\t\tpal[(((const uint8_t *)fb)[y * stride + x / 2] >> ((x & 1) ? 0 : 4)) & 15];
}
static void d_fill_insets(uint16_t c)
{
\tfor (int y = 0; y < disp.height; y++)
\t\tfor (int x = 0; x < disp.width; x++)
\t\t\tif (x < disp.inset_left || x >= disp.width - disp.inset_right || y < disp.inset_top ||
\t\t\t    y >= disp.height - disp.inset_bottom)
\t\t\t\tpanel_px[y * disp.width + x] = c;
}
static void d_backlight(uint8_t p) { (void)p; }
static hal_display_t disp = {'''
SIM_RESET_OLD = '\tdisp.inset_top = disp.inset_bottom = g->inset_tb;\n\tfont_aa_on = g->aa;\n'
SIM_RESET_NEW = ('\tdisp.inset_top = disp.inset_bottom = g->inset_tb;\n\tdisp.fb_safe_area = g->safe;\n'
                 '\tdisp.fill_insets = d_fill_insets;\n\tmemset(panel_px, 0, sizeof panel_px);\n\tfont_aa_on = g->aa;\n')

SETTINGS_OLD = 'static uint8_t slot_buf[SLOT_SIZE];\nstatic uint8_t slot_buf2[SLOT_SIZE];\n'
SETTINGS_NEW = '''static uint8_t slot_buf[SLOT_SIZE];
#ifdef PLUS_SHARED_SLOT_BUF
// DDM32 Plus (tools/plus_ramsave.py, RadioFoundry 454a5b76): one is enough. next_slot's scan is over before put_record
// reads its record back into slot_buf, and settings_init calls neither; the record put_record builds has its own
#define slot_buf2\tslot_buf
#else
static uint8_t slot_buf2[SLOT_SIZE];
#endif
'''
STORAGE_REPL = [
    ('static void mark(const hal_flash_map_t *m)\n{\n\tstatic uint8_t r[STORAGE_SLOT_SIZE];\n',
     '#ifdef PLUS_SHARED_SLOT_BUF\n'
     '// DDM32 Plus (tools/plus_ramsave.py, RadioFoundry 454a5b76): one slot\'s buffer for scan_region and the mark it may\n'
     '// write after its scan (or storage_claim\'s), under the storage lock (off the start-up task\'s stack)\n'
     'static uint8_t region_slot[STORAGE_SLOT_SIZE];\n#endif\n\n'
     'static void mark(const hal_flash_map_t *m)\n{\n#ifdef PLUS_SHARED_SLOT_BUF\n\tuint8_t *r = region_slot;\n#else\n'
     '\tstatic uint8_t r[STORAGE_SLOT_SIZE];\n#endif\n'),
    ('\t\tif (!rd(a, r, sizeof(r)) || storage_slot_check(r) != STORAGE_SLOT_FREE) {\n',
     '\t\tif (!rd(a, r, STORAGE_SLOT_SIZE) || storage_slot_check(r) != STORAGE_SLOT_FREE) {\n'),
    ('\t\tmemset(r, 0xFF, sizeof(r));\n', '\t\tmemset(r, 0xFF, STORAGE_SLOT_SIZE);\n'),
    ('\tstatic uint8_t r[STORAGE_SLOT_SIZE];\t// under the storage lock (off the start-up task\'s stack)\n',
     '#ifdef PLUS_SHARED_SLOT_BUF\n\tuint8_t *r = region_slot;\n#else\n'
     '\tstatic uint8_t r[STORAGE_SLOT_SIZE];\t// under the storage lock (off the start-up task\'s stack)\n#endif\n'),
    ('\t\tstorage_slot_t s = rd(a, r, sizeof(r)) ? storage_slot_check(r) : STORAGE_SLOT_OTHER;\n',
     '\t\tstorage_slot_t s = rd(a, r, STORAGE_SLOT_SIZE) ? storage_slot_check(r) : STORAGE_SLOT_OTHER;\n'),
]


def prepare_ramsave(transform, install):
    transform('core/ui/gfx.c', [(FB_HEAD_OLD, FB_HEAD_NEW), (INIT_OLD, INIT_NEW), (CLEAR_OLD, CLEAR_NEW),
                                (READ_OLD, READ_NEW), (FLUSH_OLD, FLUSH_NEW), (ROWS_OLD[0], ROWS_NEW[0]),
                                (ROWS_OLD[1], ROWS_NEW[1]), (INVALIDATE_OLD, INVALIDATE_NEW)])
    transform('core/ui/gfx.h', [(GFX_H_OLD, GFX_H_NEW)])
    transform('hal/hal_display.h', [(HAL_OLD, HAL_NEW)])
    transform('drivers/display/st7789/st7789.h', [(ST_H_OLD, ST_H_NEW)])
    transform('drivers/display/st7789/st7789.c', [(ST_C_OLD, ST_C_NEW)])
    transform('boards/dm32/display_dm32.c', [(DM32_DEFS_OLD, DM32_DEFS_NEW), (DM32_FLUSH_OLD, DM32_FLUSH_NEW),
                                             (DM32_STRUCT_OLD, DM32_STRUCT_NEW)])
    transform('core/debuglink/debuglink.c', [(DL_PACKED_OLD, DL_PACKED_NEW), (DL_RAW_OLD, DL_RAW_NEW)])
    transform('tests/host/sim/ui_sim.h', [(SIM_H_OLD, SIM_H_NEW), (SIM_H_TAIL_OLD, SIM_H_TAIL_NEW)])
    transform('tests/host/sim/ui_sim.c', [(SIM_GEOMS_OLD, SIM_GEOMS_NEW), (SIM_DISP_OLD, SIM_DISP_NEW),
                                          (SIM_RESET_OLD, SIM_RESET_NEW)])
    transform('core/settings/settings.c', [(SETTINGS_OLD, SETTINGS_NEW)])
    transform('core/storage/storage.c', STORAGE_REPL)
