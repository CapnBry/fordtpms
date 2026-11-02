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
    /* Data rate offset tracking in the bit synchroniser (CC1101 BSCFG.BS_LIMIT):
     * 0 off, 1 +/-3.125 %, 2 +/-6.25 %, 3 +/-12.5 %. Ignored by the LR1121.
     * Kept last: radio settings saved before it existed are the prefix. */
    uint8_t  drate_comp;
} radio_config_t;

/* Receive quality of one payload. */
typedef struct {
    int      rssi_dbm;        /* latched at the sync word */
    bool     quality;         /* freq_off_hz and lqi are valid (CC1101 only) */
    int32_t  freq_off_hz;     /* carrier offset from the tuned frequency, as the chip's FOC measured it */
    uint8_t  lqi;             /* CC1101 link quality, 0..127: lower = cleaner demodulation */
    bool     restarted;       /* the first packet after an early abort (radio_stats_t.rx_aborts) */
    bool     aborted;         /* partial payload of a packet abandoned at its first bad Manchester pair */
} radio_rx_info_t;

typedef struct {
    uint32_t syncs;           /* sync word matches */
    uint32_t payloads;        /* PayloadReady events */
    uint32_t decoded;         /* valid Ford packets */
    uint32_t err_manchester;
    /* Manchester failures with 1..RADIO_NEAR_MISS_BAD bad pairs: real packets
     * with a few damaged chips (noise payloads have ~30). bad_bit_hist[i]
     * counts how often payload bit i was the damaged one. */
    uint32_t near_miss;
    uint16_t bad_bit_hist[TPMS_FORD_PAYLOAD_BITS];
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
    uint32_t pll_unlocked_polls;  /* lock checks that found the PLL unlocked (CC1101: after tune/recal; LR1121: every ~250 ms) */
    /* Early aborts (CC1101): payloads abandoned at their first bad Manchester
     * pair, the receiver going straight back to sync search (also counted in
     * err_manchester). abort_decodes: packets decoded right after one, i.e.
     * a stronger sensor that started on top of a packet and was caught. */
    uint32_t rx_aborts;
    uint32_t abort_decodes;
    /* CC1101 status register reads (errata SWRZ020: read until two reads
     * agree). status_rereads: reads whose first two values disagreed, so a
     * third or later read was needed; status_unsettled: no two consecutive
     * reads agreed within 5 (the last value is used). Always 0 on the LR1121. */
    uint32_t status_reads;
    uint32_t status_rereads;
    uint32_t status_unsettled;
} radio_stats_t;

/* What the fitted chip can do, for the CLI and web UI. */
typedef struct {
    const char *name;         /* "CC1101", "LR1121" */
    uint32_t freq_min_hz, freq_max_hz;  /* accepted by radio_set_config */
    const char *freq_note;    /* where the synthesizer is specified to lock */
    uint32_t bitrate_min, bitrate_max;  /* chip rates (chips/s) accepted by radio_set_config */
    bool     freq_offset;     /* reports radio_rx_info_t.freq_off_hz: automatic frequency correction works */
    bool     tx;              /* radio_tx_ford is supported */
    int8_t   tx_default_dbm;  /* `tx` CLI power when none is given (high-power path) */
    const char *tx_note;      /* power ranges of the two PA paths */
} radio_caps_t;

const radio_caps_t *radio_caps(void);
/* Supported single-sided channel bandwidths in Hz, ascending. Returns count. */
int radio_bandwidths(uint32_t *out, int max);

/*
 * Reception diagnostics, kept in RAM:
 *  - RF events: runs of 500 ms windows whose peak RSSI is at least
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
#define RADIO_NEAR_MISS_BAD 4

typedef struct {
    uint32_t start_ms, end_ms;
    int8_t   peak_dbm, noise_dbm;
    uint16_t syncs, payloads, decoded;
} radio_event_t;

/*
 * Packet log: every decoded packet, oldest overwritten after RADIO_PKT_LOG.
 *
 * Failed payloads worth keeping go to their own ring (RADIO_FAILS), which
 * noise cannot flush. A failure (sync RSSI >= noise + RADIO_CAPTURE_DB) is
 * kept when it is likely a real sensor packet:
 *  - its payload is Manchester-clean but wrong (checksum or flag errors),
 *  - it has at most RADIO_FAIL_BAD damaged pairs,
 *  - its first 32 pairs (the sensor ID) are clean, or
 *  - it is at least RADIO_FAIL_DB above the noise and within
 *    RADIO_FAIL_WINDOW_MS before or after a decoded packet (a collision with
 *    another sensor of the same round).
 */
#define RADIO_PKT_LOG        128
#define RADIO_FAILS          48
#define RADIO_FAIL_BAD       8
#define RADIO_FAIL_DB        12
#define RADIO_FAIL_WINDOW_MS 3000
#define RADIO_CAP_RESTARTED 0x01   /* radio_capture_t.flags / radio_pkt_t.flags: see radio_rx_info_t.restarted */
#define RADIO_CAP_ABORTED   0x02   /* see radio_rx_info_t.aborted: data holds the payload up to the bad pair */
#define RADIO_CAP_KEPT      0x04   /* also copied to the failure ring */
#define RADIO_PKT_QUALITY   0x08   /* radio_pkt_t: freq_off_hz and lqi are valid */

typedef struct {
    uint32_t ms;
    int8_t   rssi_dbm, noise_dbm;
    uint8_t  flags;           /* RADIO_CAP_* */
    bool     quality;         /* see radio_rx_info_t */
    uint8_t  lqi;
    int32_t  freq_off_hz;
    uint8_t  status;          /* tpms_ford_status_t */
    uint8_t  len;
    uint8_t  data[RADIO_CAPTURE_MAX];
} radio_capture_t;

typedef struct {
    uint32_t ms;              /* sync time */
    uint32_t id;
    uint8_t  code[3];         /* payload bytes 4..6: pressure, temperature, flags */
    int8_t   rssi_dbm, noise_dbm;
    uint8_t  lqi;
    uint8_t  flags;           /* RADIO_PKT_QUALITY, RADIO_CAP_RESTARTED */
    int32_t  freq_off_hz;     /* measured, relative to the tuned frequency */
    int32_t  afc_hz;          /* AFC overlay tuned in at the time: carrier = freq + afc_hz + freq_off_hz */
} radio_pkt_t;

/* Oldest first. `total` (optional) counts all recorded since boot/clear. */
int radio_get_events(radio_event_t *out, int max, uint32_t *total);
/* The RADIO_TOP_EVENTS strongest finished events since boot/clear, oldest first. */
int radio_get_top_events(radio_event_t *out, int max);
int radio_get_captures(radio_capture_t *out, int max, uint32_t *total);
/* Packet log and failure ring (above), oldest first. */
int radio_get_packets(radio_pkt_t *out, int max, uint32_t *total);
int radio_get_fails(radio_capture_t *out, int max, uint32_t *total);
/* Sensor ID from a capture's first 32 Manchester pairs; false if any of them is
 * damaged, the capture is too short, or the sync mode has no fixed payload start. */
bool radio_capture_id(const radio_capture_t *c, uint32_t *id);
/* Damaged Manchester pairs of a capture (bit i = payload bit i), over the pairs
 * it holds (*npairs, at most 64; 0 in sync mode 5555). */
uint64_t radio_capture_bad(const radio_capture_t *c, int *npairs);
void radio_clear_log(void);

/* Called from the radio task for each valid packet. */
typedef void (*radio_packet_cb_t)(const tpms_ford_t *pkt, const radio_rx_info_t *rx, uint32_t ms);

esp_err_t radio_init(radio_packet_cb_t cb);
void radio_get_stats(radio_stats_t *out);
void radio_reset_stats(void);
void radio_get_config(radio_config_t *out);
/* Reprograms the chip. The config is applied even if the PLL can't lock at
 * the new frequency; that case returns ESP_ERR_INVALID_STATE. */
esp_err_t radio_set_config(const radio_config_t *cfg);
/* Defaults, with the bandwidth snapped to what the fitted chip supports. */
void radio_default_config(radio_config_t *out);

/*
 * Automatic frequency correction. Every decoded packet with a measured offset
 * moves est_hz 1/10 of the way to the carrier's offset from the configured
 * frequency (applied_hz + the packet's freq_off_hz). With AFC on, est_hz is
 * tuned in as an overlay on the configured frequency once no packet has been
 * decoded for RADIO_AFC_QUIET_MS (never during a burst), if it moved by at
 * least RADIO_AFC_DEADBAND_HZ, clamped to +/- half the channel bandwidth. The
 * overlay and estimate live in RAM: they start at 0 at boot and are reset
 * whenever the configured frequency changes.
 */
#define RADIO_AFC_QUIET_MS    3000  /* = SENSORS_BURST_GAP_MS */
#define RADIO_AFC_DEADBAND_HZ 1000

typedef struct {
    bool     on;
    int32_t  est_hz;          /* estimated carrier offset from the configured frequency */
    int32_t  applied_hz;      /* overlay tuned in now: the radio is on freq_hz + applied_hz */
    uint32_t pkts;            /* packets in est_hz since boot / the last reset */
} radio_afc_t;

void radio_get_afc(radio_afc_t *out);
/* Turning AFC on tunes in est_hz now (deadband and clamp apply); off goes back to freq_hz. */
void radio_set_afc(bool on);

/* Noise floor history for the idle screen, one sample per ~500 ms. */
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

/* "off", "3.125%", "6.25%", "12.5%" for radio_config_t.drate_comp. */
const char *radio_drate_comp_str(uint8_t drate_comp);

/* Enable logging of every raw payload (hex) at INFO level. */
void radio_set_raw_log(bool on);
