/*
 * SSD1306 driver + UI task.
 *
 * Burn-in protection on every screen:
 *  - the whole canvas orbits through 20 pixel offsets, one step every 30 s
 *  - contrast drops to a dim level after a period with no new packets
 *    (the panel is never switched off), and returns on the next packet
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

typedef struct {
    uint8_t  bright;          /* contrast while active */
    uint8_t  dim;             /* contrast after inactivity (never off) */
    uint32_t dim_after_s;     /* inactivity timeout */
    uint32_t shift_period_s;  /* burn-in shift step period */
} display_settings_t;

esp_err_t display_init(void);
void display_get_settings(display_settings_t *out);
void display_set_settings(const display_settings_t *in);
void display_default_settings(display_settings_t *out);

typedef struct {
    bool present;
    bool dimmed;
    int  ox, oy;
    const char *screen;
    uint32_t frames;
    uint32_t i2c_errors;
    uint32_t frame_us;        /* last render + transfer time */
} display_status_t;

void display_get_status(display_status_t *out);
/* Prints the current frame as text art (2 pixel rows per line). */
void display_dump(FILE *out);

/* Wakes to full brightness (e.g. on a CLI command). */
void display_poke(void);
