#include "ui.h"

#include <stdio.h>
#include <string.h>

#define W GFX_CANVAS_W
#define H GFX_CANVAS_H

static const seg7_style_t SEG_BIG = {.w = 19, .h = 33, .t = 5, .gap = 4, .slant8 = 1, .seg_gap = 1};
static const seg7_style_t SEG_SMALL = {.w = 8, .h = 15, .t = 2, .gap = 2, .slant8 = 1, .flat = true};

void ui_fmt_age(char *buf, int len, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s < 3600) snprintf(buf, len, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    else if (s < 100 * 3600) snprintf(buf, len, "%uH%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60));
    else snprintf(buf, len, "%uD", (unsigned)(s / 86400));
}

static void fmt_hms(char *buf, int len, uint32_t ms)
{
    uint32_t s = ms / 1000;
    snprintf(buf, len, "%02u:%02u:%02u", (unsigned)(s / 3600 % 100), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
}

/* Pressure as shown on the big digits: "26.5", "8.2", "102" */
static void fmt_psi(char *buf, int len, uint16_t psi_x4)
{
    unsigned tenths = (psi_x4 * 10 + 2) / 4;  /* round quarter PSI to 0.1 */
    if (tenths >= 1000) snprintf(buf, len, "%u", tenths / 10);
    else snprintf(buf, len, "%u.%u", tenths / 10, tenths % 10);
}

/* Useful range for both the bars and the history graph. */
#define RSSI_MIN (-95)
#define RSSI_MAX (-35)

static int rssi_level(int dbm)  /* 0..5 bars */
{
    if (dbm >= -50) return 5;
    if (dbm >= -60) return 4;
    if (dbm >= -70) return 3;
    if (dbm >= -78) return 2;
    if (dbm >= -88) return 1;
    return 0;
}

/* Stepped signal bars, right-aligned at x+w. Unlit bars are a dot base. */
static void draw_rssi_bars(gfx_t *g, int x, int y, int dbm)
{
    int lvl = rssi_level(dbm);
    for (int i = 0; i < 5; i++) {
        int bh = 2 + i;               /* 2..6 px */
        int bx = x + i * 3;
        if (i < lvl) gfx_fill(g, bx, y + 6 - bh, 2, bh, true);
        else gfx_pixel(g, bx, y + 5, true);
    }
}

/*
 * Wedge bar graph: segments get taller left to right, like a 1980s
 * digital tach. `value` in 0..max.
 */
static void draw_wedge(gfx_t *g, int x, int y, int w, int h, int value, int max)
{
    int nseg = (w + 1) / 4;
    int lit = max > 0 ? (value * nseg + max / 2) / max : 0;
    for (int i = 0; i < nseg; i++) {
        int sh = 2 + (h - 2) * i / (nseg - 1);
        int sx = x + i * 4;
        if (i < lit) gfx_fill(g, sx, y + h - sh, 3, sh, true);
        else gfx_pixel(g, sx + 1, y + h - 1, true);
    }
}

static const char *mode_str(tpms_ford_mode_t m)
{
    switch (m) {
    case TPMS_MODE_MOVING: return "MOVING";
    case TPMS_MODE_LEARN: return "LEARN";
    default: return "AT REST";
    }
}

/* ---------------------------------------------------------------- idle -- */

void ui_render_idle(gfx_t *g, const ui_model_t *m, uint32_t frame)
{
    char buf[40];

    /* Header: title + frequency readout */
    gfx_text(g, 0, 0, "TPMS SCAN", 1);
    snprintf(buf, sizeof(buf), "%u.%02u", (unsigned)(m->freq_hz / 1000000),
             (unsigned)(m->freq_hz % 1000000 / 10000));
    int fw = gfx_text_width(buf, 1);
    gfx_text(g, W - 13 - fw, 0, buf, 1);
    gfx_text_small(g, W - 11, 2, "MHZ");

    /* Scanner: a lit block sweeps back and forth with a fading tail. */
    const int nblk = 15, by = 10;
    int period = 2 * (nblk - 1);
    int ph = (int)(frame % (uint32_t)period);
    int head = ph < nblk ? ph : period - ph;
    for (int i = 0; i < nblk; i++) {
        int bx = i * 8 + 2;
        int d = i > head ? i - head : head - i;
        if (d == 0) gfx_fill(g, bx, by, 7, 5, true);
        else if (d == 1) gfx_rect(g, bx, by, 7, 5);
        else if (d == 2) gfx_fill(g, bx + 2, by + 1, 3, 3, true);
        else gfx_pixel(g, bx + 3, by + 2, true);
    }

    /* Noise-floor history graph */
    const int gx = 0, gy = 18, gw = UI_RSSI_HIST + 2, gh = 27;
    gfx_rect(g, gx, gy, gw, gh);
    const int span = RSSI_MAX - RSSI_MIN;
    for (int db = -80; db <= -50; db += 15) {
        int yy = gy + gh - 2 - (db - RSSI_MIN) * (gh - 3) / span;
        gfx_hline_dotted(g, gx + 2, yy, gw - 4, 4);
    }
    for (int i = 0; i < m->rssi_hist_len && i < UI_RSSI_HIST; i++) {
        int db = m->rssi_hist[i];
        if (db < RSSI_MIN) db = RSSI_MIN;
        if (db > RSSI_MAX) db = RSSI_MAX;
        int bh = (db - RSSI_MIN) * (gh - 3) / span + 1;
        int xx = gx + 1 + (UI_RSSI_HIST - m->rssi_hist_len) + i;
        gfx_vline(g, xx, gy + gh - 1 - bh, bh);
    }
    gfx_text_small(g, gx + 2, gy + 2, "RF");

    /* Right column: live numbers */
    int rx = gw + 4;
    gfx_text_small(g, rx, gy, "NOISE");
    snprintf(buf, sizeof(buf), "%d", m->noise_dbm);
    gfx_text(g, rx, gy + 6, buf, 1);
    gfx_text_small(g, rx, gy + 15, "SYNC");
    snprintf(buf, sizeof(buf), "%u", (unsigned)(m->syncs % 1000000));
    gfx_text(g, rx, gy + 21, buf, 1);

    /* Bottom: uptime, counters */
    fmt_hms(buf, sizeof(buf), m->now_ms);
    gfx_text_small(g, 0, 48, "UP");
    gfx_text(g, 10, 47, buf, 1);
    snprintf(buf, sizeof(buf), "PKT %u  BAD %u  IDS %d", (unsigned)m->decoded, (unsigned)m->bad, m->nsensors);
    gfx_text_small(g, 0, 56, buf);

    /* Blinking "receiving" lamp */
    if ((frame / 6) % 2) gfx_fill(g, W - 3, 57, 3, 3, true);
    else gfx_rect(g, W - 3, 57, 3, 3);
}

/* -------------------------------------------------------------- detail -- */

void ui_render_detail(gfx_t *g, const ui_model_t *m, const ui_sensor_t *s, uint32_t frame)
{
    char buf[48];
    uint32_t age = m->now_ms - s->last_ms;

    /* Header: ID and packet counter */
    snprintf(buf, sizeof(buf), "ID %08X", (unsigned)s->id);
    gfx_text(g, 0, 0, buf, 1);
    static const char *const wheel_names[] = {"", "FL ", "FR ", "RL ", "RR "};
    snprintf(buf, sizeof(buf), "%s#%u", s->wheel <= 4 ? wheel_names[s->wheel] : "", (unsigned)s->count);
    gfx_text(g, W - gfx_text_width(buf, 1), 0, buf, 1);
    gfx_hline_dotted(g, 0, 9, W, 2);

    /* Big pressure, right-aligned in its slot so the decimal point stays put */
    fmt_psi(buf, sizeof(buf), s->psi_x4);
    int slot = 74;
    int dw = gfx_seg7_width(buf, &SEG_BIG);
    gfx_seg7_str(g, slot - dw, 12, buf, &SEG_BIG);

    /* Right column: unit, temperature, kPa */
    int rx = 80;
    gfx_text(g, rx + 2, 12, "PSI", 2);
    gfx_text_small(g, rx, 31, "TMP");
    if (s->temp_valid) snprintf(buf, sizeof(buf), "%d\x7f" "C", s->temp_c);
    else snprintf(buf, sizeof(buf), "--");
    gfx_text(g, W - gfx_text_width(buf, 1), 30, buf, 1);
    gfx_text_small(g, rx, 40, "KPA");
    snprintf(buf, sizeof(buf), "%u", (unsigned)((s->psi_x4 * 6895u + 2000) / 4000));
    gfx_text(g, W - gfx_text_width(buf, 1), 39, buf, 1);

    /* Pressure wedge, 0..60 PSI */
    draw_wedge(g, 0, 48, W, 6, s->psi_x4, 60 * 4);

    /* Bottom line alternates between status and packet details. */
    char age_s[12];
    ui_fmt_age(age_s, sizeof(age_s), age);
    if ((frame / 30) % 2 == 0) {
        snprintf(buf, sizeof(buf), "%s  AGO %s  %dDB", mode_str(s->mode), age_s, s->rssi_dbm);
    } else {
        char t[12];
        fmt_hms(t, sizeof(t), s->last_ms);
        snprintf(buf, sizeof(buf), "C%06X F%02X @%s", (unsigned)s->code, s->flags, t);
    }
    gfx_text_small(g, 0, 56, buf);
    draw_rssi_bars(g, W - 14, 55, s->rssi_dbm);

    /* Fresh packet: flash a frame around the digits for ~1.5 s */
    if (age < 1500 && (frame / 3) % 2 == 0) {
        gfx_rrect(g, 0, 11, 78, 37);
    }
}

/* ------------------------------------------------------------ overview -- */

/* 1982 Pontiac Trans Am from above, nose up (same car as the web UI). */
static void draw_car(gfx_t *g, int cx, int y, int h)
{
    int bw = 20, x = cx - bw / 2, r = x + bw - 1;
    /* Body: wedge nose, rounded tail */
    gfx_hline(g, x + 7, y, 6);
    gfx_line(g, x + 6, y, x + 2, y + 2);
    gfx_line(g, r - 6, y, r - 2, y + 2);
    gfx_line(g, x + 1, y + 3, x, y + 5);
    gfx_line(g, r - 1, y + 3, r, y + 5);
    gfx_vline(g, x, y + 5, h - 8);
    gfx_vline(g, r, y + 5, h - 8);
    gfx_line(g, x, y + h - 3, x + 2, y + h - 1);
    gfx_line(g, r, y + h - 3, r - 2, y + h - 1);
    gfx_hline(g, x + 3, y + h - 1, bw - 6);
    /* Pop-up headlight doors */
    gfx_hline(g, x + 2, y + 4, 3);
    gfx_hline(g, r - 4, y + 4, 3);
    /* Hood bulge with twin scoops */
    gfx_hline(g, x + 7, y + 4, 6);
    gfx_line(g, x + 7, y + 4, x + 6, y + 19);
    gfx_line(g, r - 7, y + 4, r - 6, y + 19);
    gfx_vline(g, x + 8, y + 6, 3);
    gfx_vline(g, r - 8, y + 6, 3);
    /* Raked windshield, short roof */
    gfx_hline(g, x + 3, y + 22, bw - 6);
    gfx_line(g, x + 2, y + 22, x + 5, y + 29);
    gfx_line(g, r - 2, y + 22, r - 5, y + 29);
    gfx_hline(g, x + 5, y + 29, bw - 10);
    gfx_pixel(g, x + 5, y + 31, true);
    gfx_pixel(g, r - 5, y + 31, true);
    /* Big glass hatch */
    gfx_hline(g, x + 5, y + 33, bw - 10);
    gfx_line(g, x + 5, y + 33, x + 4, y + 39);
    gfx_line(g, r - 5, y + 33, r - 4, y + 39);
    gfx_vline(g, x + 4, y + 39, 7);
    gfx_vline(g, r - 4, y + 39, 7);
    gfx_line(g, x + 4, y + 46, x + 6, y + 48);
    gfx_line(g, r - 4, y + 46, r - 6, y + 48);
    gfx_hline(g, x + 7, y + 49, bw - 14);
    /* Rear spoiler */
    gfx_hline(g, x + 1, y + h - 5, bw - 2);
    /* Mirrors */
    gfx_pixel(g, x - 1, y + 23, true);
    gfx_pixel(g, r + 1, y + 23, true);
    /* Wheels */
    int wy[2] = {y + 10, y + 38};
    for (int i = 0; i < 2; i++) {
        gfx_fill(g, x - 3, wy[i], 3, 7, true);
        gfx_fill(g, x + bw, wy[i], 3, 7, true);
    }
}

static void draw_corner(gfx_t *g, const ui_model_t *m, const ui_sensor_t *s, int x, int y, bool right,
                        uint32_t frame)
{
    char buf[24];
    int bw = 46;
    if (!s) {
        gfx_text_small(g, x + (right ? bw - 11 : 0), y + 6, "---");
        return;
    }
    fmt_psi(buf, sizeof(buf), s->psi_x4);
    int dw = gfx_seg7_width(buf, &SEG_SMALL);
    /* Digits right-aligned in a fixed slot so the decimal point stays put. */
    if (right) {
        gfx_seg7_str(g, x + bw - dw, y + 1, buf, &SEG_SMALL);
        gfx_text_small(g, x + 2, y + 11, "PSI");
    } else {
        gfx_seg7_str(g, x + 31 - dw, y + 1, buf, &SEG_SMALL);
        gfx_text_small(g, x + 34, y + 11, "PSI");
    }

    char age_s[12];
    ui_fmt_age(age_s, sizeof(age_s), m->now_ms - s->last_ms);
    if ((frame / 40) % 2 == 0) snprintf(buf, sizeof(buf), "%04X %s", (unsigned)(s->id & 0xffff), age_s);
    else if (s->temp_valid) snprintf(buf, sizeof(buf), "%d\x7f" "C %s", s->temp_c, s->mode == TPMS_MODE_MOVING ? "MOV" : "");
    else snprintf(buf, sizeof(buf), "%04X %s", (unsigned)(s->id & 0xffff), s->mode == TPMS_MODE_MOVING ? "MOV" : "");
    int tw = gfx_text_small_width(buf);
    gfx_text_small(g, right ? x + bw - tw : x, y + 18, buf);
}

/* Assigned wheel whose sensor hasn't been heard since boot. */
static void draw_corner_waiting(gfx_t *g, uint32_t id, int x, int y, bool right, uint32_t frame)
{
    char buf[16];
    int bw = 46, dw = gfx_seg7_width("--.-", &SEG_SMALL);
    gfx_seg7_str(g, right ? x + bw - dw : x + 31 - dw, y + 1, "--.-", &SEG_SMALL);
    gfx_text_small(g, right ? x + 2 : x + 34, y + 11, "PSI");
    snprintf(buf, sizeof(buf), "%04X %s", (unsigned)(id & 0xffff), (frame / 10) % 2 ? "WAIT" : "");
    gfx_text_small(g, right ? x + bw - gfx_text_small_width(buf) : x, y + 18, buf);
}

void ui_render_overview(gfx_t *g, const ui_model_t *m, uint32_t frame)
{
    static const int cx[4] = {0, W - 46, 0, W - 46}, cy[4] = {4, 4, 34, 34};
    draw_car(g, W / 2, 2, 57);
    for (int i = 0; i < 4; i++) {
        bool right = i & 1;
        if (m->wheels_assigned) {
            /* Corners are the assigned wheels: FL, FR, RL, RR. */
            const ui_wheel_t *w = &m->wheel[i];
            if (w->id && !w->heard) draw_corner_waiting(g, w->id, cx[i], cy[i], right, frame);
            else draw_corner(g, m, w->id ? &w->s : NULL, cx[i], cy[i], right, frame);
        } else {
            /* No wheels assigned: the four most recently heard IDs, newest first. */
            draw_corner(g, m, i < m->nshown ? &m->s[i] : NULL, cx[i], cy[i], right, frame);
        }
    }
}
