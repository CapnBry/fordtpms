/*
 * FSK packet receiver for Ford TPMS: CC1101 (radio_cc1101.c) or LR1121
 * (radio_lr1121.c), picked by the build target. This file and radio.c are chip
 * independent; the chip backends implement radio_hw.h.
 *
 * The packet engine does the hard real-time work: bit synchronisation at
 * 19.23 kchip/s, sync-word match on 0x5556 (12 preamble chips + the "0110"
 * start delimiter, see tpms_ford.h), and a
 * fixed 16 byte payload = the 128 Manchester chips of one packet. RSSI is
 * latched at the sync match.
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
    uint32_t irq_payload, irq_sync;  /* interrupts seen for payload / sync events (0 if not wired) */
    uint32_t polled_events;   /* events found by polling instead of IRQ */
    int16_t  noise_dbm;       /* smoothed RSSI while idle */
    int16_t  last_rssi_dbm;   /* RSSI latched at last sync */
    int16_t  best_rssi_dbm, worst_rssi_dbm;  /* over decoded packets */
    uint32_t last_decode_ms;
    bool     present;         /* chip found at init */
    char     chip[40];        /* e.g. "CC1101 part 0x00 v0x14", "LR1121 hw 0x22 fw 01.04" */
    bool     pll_locked;      /* synthesizer locked: false = receiver is not tuned, hears nothing */
    uint32_t pll_unlocked_polls;  /* lock checks that found the PLL unlocked (every ~250 ms) */
} radio_stats_t;

/* What the fitted chip can do, for the CLI and web UI. */
typedef struct {
    const char *name;         /* "CC1101", "LR1121" */
    uint32_t freq_min_hz, freq_max_hz;  /* accepted by radio_set_config */
    const char *freq_note;    /* where the synthesizer is specified to lock */
    uint32_t bitrate_min, bitrate_max;  /* chip rates (chips/s) accepted by radio_set_config */
    bool     tx;              /* radio_tx_ford is supported */
    int8_t   tx_default_dbm;  /* `tx` CLI power when none is given (high-power path) */
    const char *tx_note;      /* power ranges of the two PA paths */
} radio_caps_t;

const radio_caps_t *radio_caps(void);
/* Supported single-sided channel bandwidths in Hz, ascending. Returns count. */
int radio_bandwidths(uint32_t *out, int max);

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
/* Reprograms the chip. The config is applied even if the PLL can't lock at
 * the new frequency; that case returns ESP_ERR_INVALID_STATE. */
esp_err_t radio_set_config(const radio_config_t *cfg);
/* Defaults, with the bandwidth snapped to what the fitted chip supports. */
void radio_default_config(radio_config_t *out);

/* Noise floor history for the idle screen, one sample per ~250 ms. */
int radio_rssi_history(int8_t *out, int max);

/* Diagnostic: check the chip's control lines (CC1101: SPI, GDO0 and GDO2;
 * LR1121: RESET, BUSY and the DIO IRQ line). */
void radio_probe(FILE *out);

/* Dump the chip's registers and status (CC1101) or status, errors and counters (LR1121). */
void radio_dump_regs(FILE *out);

/*
 * PLL lock scan: tunes to each of the `n` frequencies, reports whether the
 * synthesizer locked, then restores the receive configuration. Frequencies
 * outside radio_caps() are allowed so the lock detection itself can be
 * checked. Returns the number locked.
 */
int radio_pll_scan(const uint32_t *freqs_hz, int n, FILE *out);

/*
 * Transmit test: send `count` Ford frames of the 8 byte payload (checksum
 * included) at the current frequency, FSK +/-35 kHz, then return to RX.
 * pa_boost: high-power path (LR1121 HP PA -9..22 dBm) or low-power path
 * (LR1121 LP PA -17..14). The CC1101 has one PA (-30..10 dBm) for both.
 */
esp_err_t radio_tx_ford(const uint8_t payload[8], int count, int gap_ms, bool pa_boost, int power_dbm);

/* Enable logging of every raw payload (hex) at INFO level. */
void radio_set_raw_log(bool on);
