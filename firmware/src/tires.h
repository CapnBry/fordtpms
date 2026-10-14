/*
 * Which sensor ID sits on which wheel. The packets carry no position, so the
 * user assigns IDs (web UI or `tires` CLI command). Only assigned IDs are
 * published over MQTT. Stored in NVS.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum { TIRE_FL = 0, TIRE_FR, TIRE_RL, TIRE_RR, TIRE_COUNT } tire_pos_t;

#define TIRE_NONE 0u  /* no sensor assigned (ID 0 is not a valid sensor) */

void tires_init(void);              /* loads from NVS; call after settings_init */
uint32_t tires_get(tire_pos_t pos);
/* Position of `id`, or -1 if it isn't assigned. */
int tires_find(uint32_t id);
/*
 * Assigns `id` (TIRE_NONE clears) to `pos` and saves. An ID assigned elsewhere
 * moves here. Reports the ID that was at `pos` and where `id` was before (-1).
 */
void tires_set(tire_pos_t pos, uint32_t id, uint32_t *old_id, int *old_pos);

const char *tires_short_name(tire_pos_t pos);   /* "FL" */
const char *tires_long_name(tire_pos_t pos);    /* "Front Left" */
/* Parses "fl", "FR", ... Returns -1 if unknown. */
int tires_parse(const char *s);
