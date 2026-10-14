/*
 * Renders each screen to PGM images (scaled up, OLED-like) using the same UI
 * code as the firmware. Used for design review and for docs/images.
 *
 *   ./ui_preview [outdir]
 */
#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "ui.h"

static const char *outdir = ".";

/* level: 0..255 brightness of lit pixels, to illustrate dimming */
static void save_level(const gfx_t *g, const char *name, int level)
{
    const int S = 4, B = 8;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.pgm", outdir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P5\n%d %d\n255\n", GFX_W * S + 2 * B, GFX_H * S + 2 * B);
    for (int y = 0; y < GFX_H * S + 2 * B; y++)
        for (int x = 0; x < GFX_W * S + 2 * B; x++) {
            int px = (x - B) / S, py = (y - B) / S;
            int inside = x >= B && y >= B && px < GFX_W && py < GFX_H;
            int on = inside && ((g->fb[(py >> 3) * GFX_W + px] >> (py & 7)) & 1);
            int gap = inside && (((x - B) % S == S - 1) || ((y - B) % S == S - 1));
            int v = !inside ? 0x30 : on ? (gap ? level * 2 / 3 : level) : (gap ? 0x00 : 0x08);
            fputc(v, f);
        }
    fclose(f);
    printf("wrote %s\n", path);
}

static void save(const gfx_t *g, const char *name)
{
    save_level(g, name, 0xf0);
}

int main(int argc, char **argv)
{
    if (argc > 1) outdir = argv[1];
    gfx_t g;
    ui_model_t m = {0};
    m.now_ms = 3723000;
    m.freq_hz = 315000000;
    m.syncs = 1234; m.decoded = 0; m.bad = 7;
    m.noise_dbm = -84;
    m.batt_mv = 4123;
    m.rssi_hist_len = UI_RSSI_HIST;
    for (int i = 0; i < UI_RSSI_HIST; i++) m.rssi_hist[i] = (int8_t)(-86 + (i * 7 % 5) + (i % 23 == 0 ? 25 : 0));

    gfx_clear(&g); gfx_set_origin(&g, 2, 1);
    ui_render_idle(&g, &m, 9);
    save(&g, "idle");

    ui_sensor_t s = {.id = 0x45bb320f, .psi_x4 = 106, .temp_valid = true, .temp_c = 23, .mode = TPMS_MODE_MOVING,
                     .code = 0x6ad446, .flags = 0x46, .rssi_dbm = -71, .count = 12, .last_ms = 3711000};
    m.s[0] = s;
    m.s[1] = (ui_sensor_t){.id = 0x45d30b69, .psi_x4 = 138, .temp_valid = true, .temp_c = 19, .rssi_dbm = -80,
                           .count = 4, .last_ms = 3600000, .code = 0x8a5446, .flags = 0x46};
    m.s[2] = (ui_sensor_t){.id = 0x454103b0, .psi_x4 = 135, .rssi_dbm = -62, .count = 8, .last_ms = 3000000,
                           .mode = TPMS_MODE_MOVING, .code = 0x87d446, .flags = 0x46};
    m.s[3] = (ui_sensor_t){.id = 0x45aa55aa, .psi_x4 = 140, .temp_valid = true, .temp_c = 7, .rssi_dbm = -84,
                           .count = 2, .last_ms = 1500000, .mode = TPMS_MODE_REST, .code = 0x8c3f05, .flags = 0x05};
    m.nshown = 4; m.nsensors = 4; m.decoded = 24;

    gfx_clear(&g); gfx_set_origin(&g, 2, 1);
    ui_render_detail(&g, &m, &m.s[0], 1);
    save(&g, "detail");
    gfx_clear(&g); gfx_set_origin(&g, 2, 1);
    ui_render_detail(&g, &m, &m.s[0], 31);
    save(&g, "detail_info");

    /* Just received: frame flashes around the digits */
    ui_model_t fresh = m;
    fresh.now_ms = m.s[0].last_ms + 400;
    gfx_clear(&g); gfx_set_origin(&g, 2, 1);
    ui_render_detail(&g, &fresh, &fresh.s[0], 0);
    save(&g, "detail_new");

    ui_sensor_t hi = s;
    hi.psi_x4 = 0x1a2; hi.temp_valid = false; hi.mode = TPMS_MODE_LEARN; hi.flags = 0x2a; hi.code = 0xa28a2a;
    gfx_clear(&g); gfx_set_origin(&g, 2, 1);
    ui_render_detail(&g, &m, &hi, 3);
    save(&g, "detail_learn");

    gfx_clear(&g); gfx_set_origin(&g, 2, 1);
    ui_render_overview(&g, &m, 1);
    save(&g, "overview");
    gfx_clear(&g); gfx_set_origin(&g, 2, 1);
    ui_render_overview(&g, &m, 41);
    save(&g, "overview_alt");

    /* Wheels assigned: FL and FR heard, RL assigned but not heard yet, RR empty */
    ui_model_t wm = m;
    wm.wheels_assigned = true;
    wm.wheel[0] = (ui_wheel_t){.id = m.s[1].id, .heard = true, .s = m.s[1]};
    wm.wheel[0].s.wheel = 1;
    wm.wheel[1] = (ui_wheel_t){.id = m.s[0].id, .heard = true, .s = m.s[0]};
    wm.wheel[1].s.wheel = 2;
    wm.wheel[2] = (ui_wheel_t){.id = 0x364318f4};
    gfx_clear(&g); gfx_set_origin(&g, 2, 1);
    ui_render_overview(&g, &wm, 15);
    save(&g, "overview_wheels");
    ui_sensor_t ws = wm.wheel[1].s;
    gfx_clear(&g); gfx_set_origin(&g, 2, 1);
    ui_render_detail(&g, &wm, &ws, 1);
    save(&g, "detail_wheel");

    /* Burn-in shift extremes and dimmed contrast */
    gfx_clear(&g); gfx_set_origin(&g, 0, 0);
    ui_render_overview(&g, &m, 1);
    save(&g, "shift_min");
    gfx_clear(&g); gfx_set_origin(&g, GFX_SHIFT_X, GFX_SHIFT_Y);
    ui_render_overview(&g, &m, 1);
    save(&g, "shift_max");
    gfx_clear(&g); gfx_set_origin(&g, 2, 1);
    ui_render_overview(&g, &m, 1);
    save_level(&g, "dimmed", 0x40);
    return 0;
}
