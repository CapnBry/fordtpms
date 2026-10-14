#include "gfx.h"

#include <stdlib.h>
#include <string.h>

/* Classic 5x7 font, ASCII 0x20..0x7f, column-major, bit0 = top row. */
static const uint8_t font5x7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x01,0x01},{0x3E,0x41,0x41,0x51,0x32},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x04,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},
    {0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},{0x61,0x51,0x49,0x45,0x43},{0x00,0x00,0x7F,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20},{0x41,0x41,0x7F,0x00,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x00,0x7F,0x10,0x28,0x44},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x08,0x08,0x2A,0x1C,0x08},{0x06,0x09,0x09,0x06,0x00},
};

/* 3x5 font: 5 rows of 3 bits (MSB = left), packed top row first. */
#define F3(a, b, c, d, e) (uint16_t)((a) << 12 | (b) << 9 | (c) << 6 | (d) << 3 | (e))
static uint16_t font3x5(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    switch (c) {
    case '0': return F3(07, 05, 05, 05, 07);
    case '1': return F3(02, 06, 02, 02, 07);
    case '2': return F3(07, 01, 07, 04, 07);
    case '3': return F3(07, 01, 07, 01, 07);
    case '4': return F3(05, 05, 07, 01, 01);
    case '5': return F3(07, 04, 07, 01, 07);
    case '6': return F3(07, 04, 07, 05, 07);
    case '7': return F3(07, 01, 01, 01, 01);
    case '8': return F3(07, 05, 07, 05, 07);
    case '9': return F3(07, 05, 07, 01, 07);
    case 'A': return F3(02, 05, 07, 05, 05);
    case 'B': return F3(06, 05, 06, 05, 06);
    case 'C': return F3(03, 04, 04, 04, 03);
    case 'D': return F3(06, 05, 05, 05, 06);
    case 'E': return F3(07, 04, 06, 04, 07);
    case 'F': return F3(07, 04, 06, 04, 04);
    case 'G': return F3(03, 04, 05, 05, 03);
    case 'H': return F3(05, 05, 07, 05, 05);
    case 'I': return F3(07, 02, 02, 02, 07);
    case 'J': return F3(01, 01, 01, 05, 02);
    case 'K': return F3(05, 05, 06, 05, 05);
    case 'L': return F3(04, 04, 04, 04, 07);
    case 'M': return F3(05, 07, 07, 05, 05);
    case 'N': return F3(06, 05, 05, 05, 05);
    case 'O': return F3(02, 05, 05, 05, 02);
    case 'P': return F3(06, 05, 06, 04, 04);
    case 'Q': return F3(02, 05, 05, 06, 03);
    case 'R': return F3(06, 05, 06, 05, 05);
    case 'S': return F3(03, 04, 02, 01, 06);
    case 'T': return F3(07, 02, 02, 02, 02);
    case 'U': return F3(05, 05, 05, 05, 07);
    case 'V': return F3(05, 05, 05, 05, 02);
    case 'W': return F3(05, 05, 07, 07, 05);
    case 'X': return F3(05, 05, 02, 05, 05);
    case 'Y': return F3(05, 05, 02, 02, 02);
    case 'Z': return F3(07, 01, 02, 04, 07);
    case '-': return F3(00, 00, 07, 00, 00);
    case '.': return F3(00, 00, 00, 00, 02);
    case ':': return F3(00, 02, 00, 02, 00);
    case '/': return F3(01, 01, 02, 04, 04);
    case '%': return F3(05, 01, 02, 04, 05);
    case '+': return F3(00, 02, 07, 02, 00);
    case '#': return F3(05, 07, 05, 07, 05);
    case '=': return F3(00, 07, 00, 07, 00);
    case '<': return F3(01, 02, 04, 02, 01);
    case '>': return F3(04, 02, 01, 02, 04);
    case '(': return F3(02, 04, 04, 04, 02);
    case ')': return F3(02, 01, 01, 01, 02);
    case '\x7f': return F3(02, 05, 02, 00, 00); /* degree */
    default: return 0;
    }
}

void gfx_clear(gfx_t *g)
{
    memset(g->fb, 0, sizeof(g->fb));
}

void gfx_set_origin(gfx_t *g, int ox, int oy)
{
    g->ox = ox;
    g->oy = oy;
}

void gfx_pixel(gfx_t *g, int x, int y, bool on)
{
    x += g->ox;
    y += g->oy;
    if ((unsigned)x >= GFX_W || (unsigned)y >= GFX_H) return;
    uint8_t m = (uint8_t)(1u << (y & 7));
    if (on) g->fb[(y >> 3) * GFX_W + x] |= m;
    else    g->fb[(y >> 3) * GFX_W + x] &= (uint8_t)~m;
}

bool gfx_get(const gfx_t *g, int x, int y)
{
    x += g->ox;
    y += g->oy;
    if ((unsigned)x >= GFX_W || (unsigned)y >= GFX_H) return false;
    return g->fb[(y >> 3) * GFX_W + x] & (1u << (y & 7));
}

void gfx_hline(gfx_t *g, int x, int y, int w)
{
    for (int i = 0; i < w; i++) gfx_pixel(g, x + i, y, true);
}

void gfx_vline(gfx_t *g, int x, int y, int h)
{
    for (int i = 0; i < h; i++) gfx_pixel(g, x, y + i, true);
}

void gfx_hline_dotted(gfx_t *g, int x, int y, int w, int step)
{
    for (int i = 0; i < w; i += step) gfx_pixel(g, x + i, y, true);
}

void gfx_rect(gfx_t *g, int x, int y, int w, int h)
{
    gfx_hline(g, x, y, w);
    gfx_hline(g, x, y + h - 1, w);
    gfx_vline(g, x, y, h);
    gfx_vline(g, x + w - 1, y, h);
}

void gfx_rrect(gfx_t *g, int x, int y, int w, int h)
{
    gfx_hline(g, x + 1, y, w - 2);
    gfx_hline(g, x + 1, y + h - 1, w - 2);
    gfx_vline(g, x, y + 1, h - 2);
    gfx_vline(g, x + w - 1, y + 1, h - 2);
}

void gfx_fill(gfx_t *g, int x, int y, int w, int h, bool on)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) gfx_pixel(g, x + i, y + j, on);
}

void gfx_invert(gfx_t *g, int x, int y, int w, int h)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) gfx_pixel(g, x + i, y + j, !gfx_get(g, x + i, y + j));
}

void gfx_line(gfx_t *g, int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(g, x0, y0, true);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

int gfx_text(gfx_t *g, int x, int y, const char *s, int scale)
{
    int x0 = x;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7f) c = '?';
        const uint8_t *gl = font5x7[c - 0x20];
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 7; row++)
                if (gl[col] & (1 << row))
                    gfx_fill(g, x + col * scale, y + row * scale, scale, scale, true);
        x += 6 * scale;
    }
    return x - x0;
}

int gfx_text_width(const char *s, int scale)
{
    return (int)strlen(s) * 6 * scale - scale;
}

int gfx_text_small(gfx_t *g, int x, int y, const char *s)
{
    int x0 = x;
    for (; *s; s++) {
        uint16_t gl = font3x5(*s);
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (gl & (1u << ((4 - row) * 3 + (2 - col))))
                    gfx_pixel(g, x + col, y + row, true);
        x += 4;
    }
    return x - x0;
}

int gfx_text_small_width(const char *s)
{
    return (int)strlen(s) * 4 - 1;
}

/* --- 7 segment ---------------------------------------------------------- */

enum { SA = 1, SB = 2, SC = 4, SD = 8, SE = 16, SF = 32, SG = 64 };

static uint8_t seg7_mask(char c)
{
    switch (c) {
    case '0': return SA | SB | SC | SD | SE | SF;
    case '1': return SB | SC;
    case '2': return SA | SB | SG | SE | SD;
    case '3': return SA | SB | SG | SC | SD;
    case '4': return SF | SG | SB | SC;
    case '5': return SA | SF | SG | SC | SD;
    case '6': return SA | SF | SG | SE | SC | SD;
    case '7': return SA | SB | SC;
    case '8': return 0x7f;
    case '9': return SA | SB | SC | SD | SF | SG;
    case '-': return SG;
    case 'A': return SA | SB | SC | SE | SF | SG;
    case 'E': return SA | SD | SE | SF | SG;
    case 'F': return SA | SE | SF | SG;
    case 'H': return SB | SC | SE | SF | SG;
    case 'L': return SD | SE | SF;
    case 'P': return SA | SB | SE | SF | SG;
    case 'r': return SE | SG;
    case 'o': return SC | SD | SE | SG;
    case '_': return SD;
    default:  return 0;
    }
}

/* Pixel in a sheared digit box: lean grows toward the top. */
static void seg_px(gfx_t *g, int x, int y, int px, int py, const seg7_style_t *st)
{
    int lean = (st->h - 1 - py) * st->slant8 / 8;
    gfx_pixel(g, x + px + lean, y + py, true);
}

/* Bevel inset for row/column i of a t-thick segment: 0 in the middle, more at the edges. */
static int bevel(int i, int t, bool flat)
{
    if (flat) return 0;
    int edge = i < t - 1 - i ? i : t - 1 - i;
    return (t - 1) / 2 - edge;
}

/* Horizontal hexagonal segment, top row y0, spanning columns xa..xb. */
static void seg_h(gfx_t *g, int x, int y, int xa, int xb, int y0, const seg7_style_t *st)
{
    for (int i = 0; i < st->t; i++) {
        int d = bevel(i, st->t, st->flat);
        for (int px = xa + d; px <= xb - d; px++) seg_px(g, x, y, px, y0 + i, st);
    }
}

/* Vertical hexagonal segment, left column x0, spanning rows ya..yb. */
static void seg_v(gfx_t *g, int x, int y, int ya, int yb, int x0, const seg7_style_t *st)
{
    for (int i = 0; i < st->t; i++) {
        int d = bevel(i, st->t, st->flat);
        for (int py = ya + d; py <= yb - d; py++) seg_px(g, x, y, x0 + i, py, st);
    }
}

static void seg7_draw_mask(gfx_t *g, int x, int y, uint8_t m, const seg7_style_t *st)
{
    /* Work on segment centre lines; pointed ends stop 1px short of where the
     * centre lines cross, leaving a diagonal gap like a real LCD/VFD glyph.
     * Odd thickness and odd height give the cleanest points. */
    int w = st->w, h = st->h, t = st->t, half = (t - 1) / 2;
    int cl = half, cr = w - 1 - half;               /* vertical centre columns */
    int ct = half, cm = (h - 1) / 2, cb = h - 1 - half; /* horizontal centre rows */
    if (st->flat) {
        /* Blocky LCD style: full-height verticals, horizontals between them. */
        int mt = cm - half;
        if (m & SA) seg_h(g, x, y, t, w - 1 - t, 0, st);
        if (m & SG) seg_h(g, x, y, t, w - 1 - t, mt, st);
        if (m & SD) seg_h(g, x, y, t, w - 1 - t, h - t, st);
        if (m & SF) seg_v(g, x, y, 0, mt + t - 1, 0, st);
        if (m & SB) seg_v(g, x, y, 0, mt + t - 1, w - t, st);
        if (m & SE) seg_v(g, x, y, mt, h - 1, 0, st);
        if (m & SC) seg_v(g, x, y, mt, h - 1, w - t, st);
        return;
    }
    int gap = st->seg_gap;
    if (m & SA) seg_h(g, x, y, cl + 1 + gap, cr - 1 - gap, ct - half, st);
    if (m & SG) seg_h(g, x, y, cl + 1 + gap, cr - 1 - gap, cm - half, st);
    if (m & SD) seg_h(g, x, y, cl + 1 + gap, cr - 1 - gap, cb - half, st);
    if (m & SF) seg_v(g, x, y, ct + 1 + gap, cm - 1 - gap, cl - half, st);
    if (m & SB) seg_v(g, x, y, ct + 1 + gap, cm - 1 - gap, cr - half, st);
    if (m & SE) seg_v(g, x, y, cm + 1 + gap, cb - 1 - gap, cl - half, st);
    if (m & SC) seg_v(g, x, y, cm + 1 + gap, cb - 1 - gap, cr - half, st);
}

int gfx_seg7_char(gfx_t *g, int x, int y, char c, const seg7_style_t *st)
{
    seg7_draw_mask(g, x, y, seg7_mask(c), st);
    return st->w + st->gap;
}

static int dp_size(const seg7_style_t *st)
{
    return st->t < 3 ? 2 : st->t - 1;
}

int gfx_seg7_str(gfx_t *g, int x, int y, const char *s, const seg7_style_t *st)
{
    int x0 = x;
    for (; *s; s++) {
        if (*s == '.') {
            /* Decimal point sits in the gap, at the baseline. */
            int d = dp_size(st);
            gfx_fill(g, x - st->gap + 1, y + st->h - d, d, d, true);
            x += d + 1;
            continue;
        }
        x += gfx_seg7_char(g, x, y, *s, st);
    }
    return x - x0;
}

int gfx_seg7_width(const char *s, const seg7_style_t *st)
{
    int w = 0;
    for (; *s; s++) w += (*s == '.') ? dp_size(st) + 1 : st->w + st->gap;
    return w - st->gap + (st->h - 1) * st->slant8 / 8;
}
