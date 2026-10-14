/*
 * Tiny 1bpp framebuffer graphics for a 128x64 SSD1306 (page-major layout, so
 * the buffer can be sent to the panel as-is). Plain C, no IDF dependencies.
 *
 * All drawing goes through a movable origin, used for OLED burn-in protection:
 * screens are laid out on a GFX_CANVAS_W x GFX_CANVAS_H canvas and the whole
 * canvas is nudged around within the panel.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GFX_W 128
#define GFX_H 64
#define GFX_FB_SIZE (GFX_W * GFX_H / 8)

/* Logical canvas size; the remaining pixels are the burn-in shift range. */
#define GFX_CANVAS_W 124
#define GFX_CANVAS_H 61
#define GFX_SHIFT_X (GFX_W - GFX_CANVAS_W)  /* 0..4 */
#define GFX_SHIFT_Y (GFX_H - GFX_CANVAS_H)  /* 0..3 */

typedef struct {
    uint8_t fb[GFX_FB_SIZE];
    int ox, oy;  /* origin (burn-in shift) */
} gfx_t;

void gfx_clear(gfx_t *g);
void gfx_set_origin(gfx_t *g, int ox, int oy);
void gfx_pixel(gfx_t *g, int x, int y, bool on);
bool gfx_get(const gfx_t *g, int x, int y);  /* canvas coordinates */
void gfx_hline(gfx_t *g, int x, int y, int w);
void gfx_vline(gfx_t *g, int x, int y, int h);
void gfx_hline_dotted(gfx_t *g, int x, int y, int w, int step);
void gfx_rect(gfx_t *g, int x, int y, int w, int h);
void gfx_fill(gfx_t *g, int x, int y, int w, int h, bool on);
void gfx_rrect(gfx_t *g, int x, int y, int w, int h);   /* 1px rounded corners */
void gfx_line(gfx_t *g, int x0, int y0, int x1, int y1);
void gfx_invert(gfx_t *g, int x, int y, int w, int h);

/* 5x7 font (6px advance), scale >= 1. '\x7f' renders a degree sign. Returns width. */
int gfx_text(gfx_t *g, int x, int y, const char *s, int scale);
int gfx_text_width(const char *s, int scale);
/* 3x5 "LCD label" font (4px advance), uppercase only. Returns width. */
int gfx_text_small(gfx_t *g, int x, int y, const char *s);
int gfx_text_small_width(const char *s);

/*
 * Slanted 7-segment glyph with beveled (hexagonal) segments, 1980s dash style.
 * Supports '0'-'9', '-', ' ', and a few letters. `slant` = pixels of lean per
 * 8 rows (0 = upright). Returns advance width.
 */
typedef struct {
    int w, h, t;     /* digit box width, height, segment thickness */
    int gap;         /* spacing between digits */
    int slant8;      /* lean: x shift per 8 rows */
    int seg_gap;     /* clearance where segments meet (0 = tips touch) */
    bool flat;       /* square segment ends instead of hexagonal */
} seg7_style_t;

int gfx_seg7_char(gfx_t *g, int x, int y, char c, const seg7_style_t *st);
/* Draws a string; '.' becomes a decimal point attached to the previous digit. */
int gfx_seg7_str(gfx_t *g, int x, int y, const char *s, const seg7_style_t *st);
int gfx_seg7_width(const char *s, const seg7_style_t *st);
