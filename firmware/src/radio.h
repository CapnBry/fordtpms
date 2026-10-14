/*
 * SX1278 in FSK packet mode as a Ford TPMS receiver.
 *
 * The packet engine does the hard real-time work: bit synchronisation at
 * 19.23 kchip/s, sync-word match on the last 16 preamble chips (0x5556), and a
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

/* Called from the radio task for each valid packet. */
typedef void (*radio_packet_cb_t)(const tpms_ford_t *pkt, int rssi_dbm, uint32_t ms);

esp_err_t radio_init(radio_packet_cb_t cb);
void radio_get_stats(radio_stats_t *out);
void radio_reset_stats(void);
void radio_get_config(radio_config_t *out);
esp_err_t radio_set_config(const radio_config_t *cfg);   /* reprograms the chip */

/* Noise floor history for the idle screen, one sample per ~250 ms. */
int radio_rssi_history(int8_t *out, int max);

/* Diagnostic: identify which GPIOs the SX1278 DIO0/1/2 and RESET drive. */
void radio_probe(FILE *out);

/* Dump all SX1278 registers. */
void radio_dump_regs(FILE *out);

/* Enable logging of every raw payload (hex) at INFO level. */
void radio_set_raw_log(bool on);
