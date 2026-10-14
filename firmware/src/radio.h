/*
 * SX1278 in FSK packet mode as a Ford TPMS receiver.
 *
 * The packet engine does the hard real-time work: bit synchronisation at
 * 19.23 kchip/s, sync-word match on 0x5556 (12 preamble chips + the "0110"
 * start delimiter, see tpms_ford.h), and a
 * fixed 16 byte payload = the 128 Manchester chips of one packet. DIO2 fires
 * on sync (we latch RSSI there), DIO0 on PayloadReady.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "tpms_ford.h"

typedef enum {
    RADIO_SYNC_5556 = 0,  /* high tone = 1 (matches rtl_433 captures) */
    RADIO_SYNC_AAA9,      /* inverted polarity */
    RADIO_SYNC_5555,      /* polarity agnostic, software search in a longer window */
} radio_sync_mode_t;

typedef struct {
    uint32_t freq_hz;
    uint32_t bitrate;     /* chips per second */
    uint32_t rxbw_hz;     /* single-sided channel filter bandwidth */
    uint8_t  lna_gain;    /* 1 (max) .. 6 (min) */
    radio_sync_mode_t sync;
} radio_config_t;

typedef struct {
    uint32_t syncs;           /* sync word matches */
    uint32_t payloads;        /* PayloadReady events */
    uint32_t decoded;         /* valid Ford packets */
    uint32_t err_manchester;
    uint32_t err_checksum;
    uint32_t err_flags;       /* includes all-zero payloads */
    uint32_t fifo_overruns;
    uint32_t irq_dio0, irq_dio2;  /* interrupt counts (0 if a DIO isn't wired) */
    uint32_t polled_events;   /* events found by polling instead of IRQ */
    int16_t  noise_dbm;       /* smoothed RSSI while idle */
    int16_t  last_rssi_dbm;   /* RSSI latched at last sync */
    int16_t  best_rssi_dbm, worst_rssi_dbm;  /* over decoded packets */
    uint32_t last_decode_ms;
    uint8_t  version;         /* RegVersion, 0x12 for SX1278 */
} radio_stats_t;

/*
 * Reception diagnostics, kept in RAM:
 *  - RF events: runs of 250 ms windows whose peak RSSI is at least
 *    RADIO_EVENT_DB above the noise floor, with the sync/payload/decode counts
 *    seen during the run. A strong event with no syncs points at polarity or
 *    frequency; syncs without decodes at timing/deviation.
 *  - Captures: raw payloads whose sync RSSI was at least RADIO_CAPTURE_DB above
 *    the noise floor (decoded or not), for offline analysis.
 */
#define RADIO_EVENT_DB    12   /* +10 caught noise peaks ~9x/min */
#define RADIO_EVENTS      48
#define RADIO_TOP_EVENTS  16   /* strongest since boot, kept however many weak ones follow */
#define RADIO_CAPTURE_DB  6
#define RADIO_CAPTURES    24
#define RADIO_CAPTURE_MAX 24   /* bytes: the longest payload (sync 5555) */

typedef struct {
    uint32_t start_ms, end_ms;
    int8_t   peak_dbm, noise_dbm;
    uint16_t syncs, payloads, decoded;
} radio_event_t;

typedef struct {
    uint32_t ms;
    int8_t   rssi_dbm, noise_dbm;
    uint8_t  status;          /* tpms_ford_status_t */
    uint8_t  len;
    uint8_t  data[RADIO_CAPTURE_MAX];
} radio_capture_t;

/* Oldest first. `total` (optional) counts all recorded since boot/clear. */
int radio_get_events(radio_event_t *out, int max, uint32_t *total);
/* The RADIO_TOP_EVENTS strongest finished events since boot/clear, oldest first. */
int radio_get_top_events(radio_event_t *out, int max);
int radio_get_captures(radio_capture_t *out, int max, uint32_t *total);
void radio_clear_log(void);

/* Called from the radio task for each valid packet. */
typedef void (*radio_packet_cb_t)(const tpms_ford_t *pkt, int rssi_dbm, uint32_t ms);

esp_err_t radio_init(radio_packet_cb_t cb);
void radio_get_stats(radio_stats_t *out);
void radio_reset_stats(void);
void radio_get_config(radio_config_t *out);
esp_err_t radio_set_config(const radio_config_t *cfg);   /* reprograms the chip */
void radio_default_config(radio_config_t *out);

/* Noise floor history for the idle screen, one sample per ~250 ms. */
int radio_rssi_history(int8_t *out, int max);

/* Diagnostic: identify which GPIOs the SX1278 DIO0/1/2 and RESET drive. */
void radio_probe(FILE *out);

/* Dump all SX1278 registers. */
void radio_dump_regs(FILE *out);

/* Enable logging of every raw payload (hex) at INFO level. */
void radio_set_raw_log(bool on);
