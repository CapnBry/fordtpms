/*
 * Table of the most recently heard sensors (unique by ID, LRU eviction).
 * Ford sensors repeat each reading ~4 times within a second; those repeats
 * are folded into one "burst".
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tpms_ford.h"

#define SENSORS_MAX 20
#define SENSORS_BURST_GAP_MS 3000

typedef struct {
    tpms_ford_t pkt;        /* latest packet */
    int16_t  rssi_dbm;      /* RSSI of latest packet */
    int16_t  rssi_best_dbm; /* best RSSI within the latest burst */
    uint32_t first_ms;      /* first time this ID was heard */
    uint32_t last_ms;       /* last packet */
    uint32_t packets;       /* all packets incl. repeats */
    uint32_t bursts;        /* distinct transmissions */
} sensor_entry_t;

void sensors_init(void);
/* Returns true if this packet starts a new burst (vs. a repeat). */
bool sensors_add(const tpms_ford_t *pkt, int rssi_dbm, uint32_t ms);
/* Copies up to `max` entries, most recently heard first. Returns count. */
int sensors_snapshot(sensor_entry_t *out, int max);
/* Increments for every new burst; lets the UI notice fresh data cheaply. */
uint32_t sensors_generation(void);
/* Copies the entry for `id`. Returns false if it isn't in the table. */
bool sensors_get(uint32_t id, sensor_entry_t *out);
/* Number of IDs in the table. */
int sensors_count(void);
void sensors_clear(void);
