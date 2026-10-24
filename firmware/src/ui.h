/*
 * Screen layouts. Pure functions of a ui_model_t snapshot so they can be
 * rendered on the host for previews (test/host/ui_preview.c).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"
#include "tpms_ford.h"

#define UI_RSSI_HIST 80     /* noise-floor history samples shown on idle screen */
#define UI_MAX_WHEELS 4

typedef struct {
    uint32_t id;
    uint16_t psi_x4;
    bool     temp_valid;
    int8_t   temp_c;
    tpms_ford_mode_t mode;
    uint32_t code;
    uint8_t  flags;
    int16_t  rssi_dbm;
    uint32_t count;          /* packets received from this ID */
    uint32_t last_ms;        /* uptime of last reception */
    uint8_t  wheel;          /* 0 = not assigned, 1..4 = FL, FR, RL, RR */
} ui_sensor_t;

/* A wheel position (FL, FR, RL, RR) and the sensor assigned to it. */
typedef struct {
    uint32_t id;             /* 0 = no sensor assigned */
    bool     heard;          /* `s` is valid: the ID was heard since boot */
    ui_sensor_t s;
} ui_wheel_t;

typedef struct {
    uint32_t now_ms;
    uint32_t freq_hz;
    uint32_t syncs, decoded, bad;   /* bad = manchester + checksum + flag rejects */
    int16_t  noise_dbm;
    int8_t   rssi_hist[UI_RSSI_HIST];  /* dBm, oldest first */
    int      rssi_hist_len;
    int      nsensors;                 /* known IDs (up to 20) */
    ui_sensor_t s[UI_MAX_WHEELS];      /* most recent first */
    int      nshown;
    ui_wheel_t wheel[UI_MAX_WHEELS];   /* FL, FR, RL, RR */
    bool     wheels_assigned;          /* any wheel has an ID: overview shows wheels */
} ui_model_t;

typedef enum {
    UI_SCREEN_IDLE,
    UI_SCREEN_DETAIL,
    UI_SCREEN_OVERVIEW,
} ui_screen_t;

void ui_render_idle(gfx_t *g, const ui_model_t *m, uint32_t frame);
void ui_render_detail(gfx_t *g, const ui_model_t *m, const ui_sensor_t *s, uint32_t frame);
void ui_render_overview(gfx_t *g, const ui_model_t *m, uint32_t frame);

/* Formats "mm:ss" or "h:mm:ss"/"NNh" style ages compactly. */
void ui_fmt_age(char *buf, int len, uint32_t ms);
