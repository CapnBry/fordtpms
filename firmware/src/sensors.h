/*
 * Table of the most recently heard sensors (unique by ID, LRU eviction).
 * Ford sensors repeat each reading ~4 times within a second; those repeats
 * are folded into one "burst".
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "radio.h"
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
    uint8_t  burst_pkts;    /* packets received in the latest burst (the sensors send 4) */
    /* Receive quality (radio_rx_info_t), over all packets with quality data. */
    uint32_t q_n;           /* packets with quality data */
    int32_t  foff_hz;       /* frequency offset of the latest packet */
    int32_t  foff_min_hz, foff_max_hz;
    int64_t  foff_sum_hz;   /* mean = foff_sum_hz / q_n */
    uint8_t  lqi;           /* LQI of the latest packet */
    uint32_t lqi_sum;       /* mean = lqi_sum / q_n */
    bool     published;     /* latest burst handed out by sensors_take_finished() */
} sensor_entry_t;

void sensors_init(void);
/* Returns true if this packet starts a new burst (vs. a repeat). */
bool sensors_add(const tpms_ford_t *pkt, const radio_rx_info_t *rx, uint32_t ms);
/* Copies up to `max` entries, most recently heard first. Returns count. */
int sensors_snapshot(sensor_entry_t *out, int max);
/* Increments for every new burst; lets the UI notice fresh data cheaply. */
uint32_t sensors_generation(void);
/* Finds one sensor whose latest burst has ended (no packet for more than
 * SENSORS_BURST_GAP_MS before `now_ms`) and isn't published yet, marks it
 * published and copies it. Returns false when there is none. */
bool sensors_take_finished(uint32_t now_ms, sensor_entry_t *out);
/* Copies the entry for `id`. Returns false if it isn't in the table. */
bool sensors_get(uint32_t id, sensor_entry_t *out);
/* Number of IDs in the table. */
int sensors_count(void);
void sensors_clear(void);
