/*
 * Chip backend interface used by radio.c. Exactly one backend is built:
 * radio_cc1101.c or radio_lr1121.c (see CMakeLists.txt). radio.c serialises
 * all calls; everything except hw_init and hw_start_irq runs with the radio
 * lock held.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "radio.h"

/* Task notification bits the backend's ISRs send to the radio task. */
#define RADIO_NOTIFY_PAYLOAD (1u << 0)
#define RADIO_NOTIFY_SYNC    (1u << 1)
#define RADIO_NOTIFY_IRQ     (1u << 2)   /* single shared IRQ line (LR1121) */

/* One poll of the receiver. */
typedef struct {
    bool    locked;           /* PLL lock as last checked */
    bool    sync;             /* a new sync word match */
    int     sync_rssi_dbm;
    int     payload_len;      /* > 0: payload[] holds a received payload */
    bool    payload_rssi_valid;  /* else the payload uses the RSSI latched at sync */
    int     payload_rssi_dbm;
    uint8_t payload[32];
    bool    idle;             /* between packets: idle_rssi_dbm is a noise sample */
    int     idle_rssi_dbm;
} radio_hw_poll_t;

extern const radio_caps_t radio_hw_caps;

/* Bytes per payload for a sync mode (radio.c). */
int radio_payload_len(radio_sync_mode_t m);
/* Sync word bytes for a sync mode (radio.c). */
void radio_sync_bytes(radio_sync_mode_t m, uint8_t out[2]);
/* For the backend's GPIO ISRs (radio.c). */
void radio_isr_notify(uint32_t bits);

/* Bus + reset + identify. Fills st->chip; ESP_ERR_NOT_FOUND if no chip answers. */
esp_err_t hw_init(radio_stats_t *st);
/* Installs the DIO interrupt(s); called once the radio task exists. */
void hw_start_irq(void);
/* Programs `cfg` and enters continuous RX. *locked: PLL locked at the end. */
esp_err_t hw_apply(const radio_config_t *cfg, bool *locked);
/* Nearest supported single-sided channel bandwidth. */
uint32_t hw_rxbw(uint32_t hz);
int hw_bandwidths(uint32_t *out, int max);
/* Reads events since the last poll. `bits` are the notifications received. */
void hw_poll(uint32_t bits, uint32_t now_ms, radio_hw_poll_t *out, radio_stats_t *st);
/* Tunes to freq_hz without receiving and reports PLL lock. hw_apply restores RX. */
bool hw_pll_test(uint32_t freq_hz, char *detail, size_t detail_len);
void hw_probe(FILE *out);
void hw_dump(FILE *out);
esp_err_t hw_tx_ford(const radio_config_t *cfg, const uint8_t payload[8], int count, int gap_ms, bool pa_boost,
                     int power_dbm);
