/*
 * Chip-independent receiver: radio task, packet decoding, statistics, noise
 * floor, RF event log and captures. The chip is driven through radio_hw.h.
 */
#include "radio.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "radio_hw.h"

static const char *TAG = "radio";

#define HIST_PERIOD_MS 250
#define HIST_LEN 80

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static radio_packet_cb_t s_cb;
static const radio_config_t k_default_cfg = {
    .freq_hz = 315000000,
    .bitrate = 19231,      /* 52 us chips */
    .rxbw_hz = 83333,      /* +/-83 kHz: ~35 kHz deviation + sensor crystal error */
    .sync = RADIO_SYNC_5556,
};
static radio_config_t s_cfg = k_default_cfg;
static radio_stats_t s_stats;
static bool s_raw_log;

static int8_t s_hist[HIST_LEN];
static int s_hist_head, s_hist_count;

static radio_event_t s_events[RADIO_EVENTS];
static int s_ev_head, s_ev_count;
static uint32_t s_ev_total;
static radio_event_t s_top[RADIO_TOP_EVENTS];
static int s_top_count;
static radio_capture_t s_caps[RADIO_CAPTURES];
static int s_cap_head, s_cap_count;
static uint32_t s_cap_total;

/* ------------------------------------------------------- backend glue -- */

int radio_payload_len(radio_sync_mode_t m)
{
    return m == RADIO_SYNC_5555 ? 24 : TPMS_FORD_CHIP_BYTES;
}

void radio_sync_bytes(radio_sync_mode_t m, uint8_t out[2])
{
    switch (m) {
    case RADIO_SYNC_AAA9: out[0] = 0xaa; out[1] = 0xa9; break;
    case RADIO_SYNC_5555: out[0] = 0x55; out[1] = 0x55; break;
    default:              out[0] = 0x55; out[1] = 0x56; break;
    }
}

void IRAM_ATTR radio_isr_notify(uint32_t bits)
{
    BaseType_t woken = pdFALSE;
    xTaskNotifyFromISR(s_task, bits, eSetBits, &woken);
    portYIELD_FROM_ISR(woken);
}

static const char *sync_str(radio_sync_mode_t s)
{
    return s == RADIO_SYNC_5556 ? "5556" : s == RADIO_SYNC_AAA9 ? "aaa9" : "5555";
}

/* Must hold s_lock. */
static esp_err_t apply_config(void)
{
    bool locked = false;
    esp_err_t err = hw_apply(&s_cfg, &locked);
    s_stats.pll_locked = locked;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "programming the radio failed: %s", esp_err_to_name(err));
        return err;
    }
    if (!locked) {
        ESP_LOGE(TAG, "PLL NOT LOCKED at %.3f MHz: the %s cannot tune here (%s)", s_cfg.freq_hz / 1e6,
                 radio_hw_caps.name, radio_hw_caps.freq_note);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

/* --------------------------------------------------------- receive ----- */

/* Keeps the strongest finished events; ties keep the older one. */
static void top_insert(const radio_event_t *e)
{
    if (s_top_count < RADIO_TOP_EVENTS) {
        s_top[s_top_count++] = *e;
        return;
    }
    int weakest = 0;
    for (int i = 1; i < s_top_count; i++)
        if (s_top[i].peak_dbm < s_top[weakest].peak_dbm) weakest = i;
    if (e->peak_dbm > s_top[weakest].peak_dbm) s_top[weakest] = *e;
}

static void hist_push(int dbm)
{
    if (dbm < -128) dbm = -128;
    if (dbm > 0) dbm = 0;
    s_hist[s_hist_head] = (int8_t)dbm;
    s_hist_head = (s_hist_head + 1) % HIST_LEN;
    if (s_hist_count < HIST_LEN) s_hist_count++;
}

static void handle_payload(const uint8_t *buf, int n, int rssi, uint32_t sync_ms)
{
    s_stats.payloads++;

    tpms_ford_t pkt;
    tpms_ford_status_t st;
    switch (s_cfg.sync) {
    case RADIO_SYNC_AAA9: st = tpms_ford_decode_chips(buf, n * 8, 0, true, &pkt); break;
    case RADIO_SYNC_5555: st = tpms_ford_search(buf, n * 8, &pkt); break;
    default:              st = tpms_ford_decode_chips(buf, n * 8, 0, false, &pkt); break;
    }

    if (s_raw_log || st == TPMS_FORD_OK) {
        char hex[2 * 32 + 1];
        for (int i = 0; i < n; i++) sprintf(hex + 2 * i, "%02x", buf[i]);
        if (s_raw_log) ESP_LOGI(TAG, "raw %s rssi=%d -> %s", hex, rssi, tpms_ford_status_str(st));
        else ESP_LOGD(TAG, "raw %s rssi=%d", hex, rssi);
    }

    if (rssi >= s_stats.noise_dbm + RADIO_CAPTURE_DB) {
        radio_capture_t *c = &s_caps[s_cap_head];
        s_cap_head = (s_cap_head + 1) % RADIO_CAPTURES;
        if (s_cap_count < RADIO_CAPTURES) s_cap_count++;
        s_cap_total++;
        c->ms = sync_ms;
        c->rssi_dbm = (int8_t)rssi;
        c->noise_dbm = (int8_t)s_stats.noise_dbm;
        c->status = (uint8_t)st;
        c->len = (uint8_t)(n < RADIO_CAPTURE_MAX ? n : RADIO_CAPTURE_MAX);
        memcpy(c->data, buf, c->len);
    }

    switch (st) {
    case TPMS_FORD_OK:
        s_stats.decoded++;
        s_stats.last_decode_ms = sync_ms;
        if (s_stats.decoded == 1 || rssi > s_stats.best_rssi_dbm) s_stats.best_rssi_dbm = (int16_t)rssi;
        if (s_stats.decoded == 1 || rssi < s_stats.worst_rssi_dbm) s_stats.worst_rssi_dbm = (int16_t)rssi;
        break;
    case TPMS_FORD_ERR_MANCHESTER: s_stats.err_manchester++; break;
    case TPMS_FORD_ERR_CHECKSUM: s_stats.err_checksum++; break;
    case TPMS_FORD_ERR_FLAGS:
    case TPMS_FORD_ERR_ZERO: s_stats.err_flags++; break;
    default: break;
    }

    if (st == TPMS_FORD_OK && s_cb) {
        /* Callback runs without the radio lock held. */
        xSemaphoreGive(s_lock);
        s_cb(&pkt, rssi, sync_ms);
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void radio_task(void *arg)
{
    int pkt_rssi = 0;
    uint32_t sync_ms = 0;
    uint32_t hist_t0 = 0;
    int hist_max = -128, noise_acc = 0, noise_n = 0;
    int noise_ema_x16 = -120 * 16;
    /* RF event tracking: counters at the start of the current window/event */
    radio_event_t *ev = NULL;
    uint32_t win_syncs = 0, win_payloads = 0, win_decoded = 0;
    uint32_t ev_syncs = 0, ev_payloads = 0, ev_decoded = 0;

    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(5));

        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        radio_hw_poll_t p;
        memset(&p, 0, sizeof(p));
        hw_poll(bits, now, &p, &s_stats);

        if (p.locked != s_stats.pll_locked) {
            if (p.locked) ESP_LOGW(TAG, "PLL locked again at %.3f MHz", s_cfg.freq_hz / 1e6);
            else ESP_LOGE(TAG, "PLL lost lock at %.3f MHz", s_cfg.freq_hz / 1e6);
            s_stats.pll_locked = p.locked;
        }

        if (p.sync) {
            /* RSSI latched while the packet is on the air. */
            pkt_rssi = p.sync_rssi_dbm;
            s_stats.last_rssi_dbm = (int16_t)pkt_rssi;
            sync_ms = now;
            s_stats.syncs++;
        }
        if (p.payload_len > 0) {
            if (p.payload_rssi_valid) {
                pkt_rssi = p.payload_rssi_dbm;
                s_stats.last_rssi_dbm = (int16_t)pkt_rssi;
            }
            handle_payload(p.payload, p.payload_len, pkt_rssi, sync_ms);
        }
        if (p.idle) {
            if (p.idle_rssi_dbm > hist_max) hist_max = p.idle_rssi_dbm;
            noise_acc += p.idle_rssi_dbm;
            noise_n++;
        }

        if (now - hist_t0 >= HIST_PERIOD_MS) {
            if (noise_n) {
                hist_push(hist_max);
                bool loud = noise_ema_x16 != -120 * 16 && hist_max >= s_stats.noise_dbm + RADIO_EVENT_DB;
                if (loud && !ev) {
                    ev = &s_events[s_ev_head];
                    s_ev_head = (s_ev_head + 1) % RADIO_EVENTS;
                    if (s_ev_count < RADIO_EVENTS) s_ev_count++;
                    s_ev_total++;
                    *ev = (radio_event_t){.start_ms = hist_t0, .peak_dbm = (int8_t)hist_max,
                                          .noise_dbm = (int8_t)s_stats.noise_dbm};
                    ev_syncs = win_syncs;
                    ev_payloads = win_payloads;
                    ev_decoded = win_decoded;
                }
                if (ev) {
                    /* Also counts the first quiet window: a packet can finish after the energy. */
                    ev->end_ms = now;
                    if (hist_max > ev->peak_dbm) ev->peak_dbm = (int8_t)hist_max;
                    ev->syncs = (uint16_t)(s_stats.syncs - ev_syncs);
                    ev->payloads = (uint16_t)(s_stats.payloads - ev_payloads);
                    ev->decoded = (uint16_t)(s_stats.decoded - ev_decoded);
                    if (!loud) {
                        top_insert(ev);
                        ev = NULL;
                    }
                }
                /* Noise floor: seeded by the first window, then fast from quiet
                 * windows and very slow from loud ones, so bursts barely move it
                 * but a lasting change in the noise is still followed. */
                int avg_x16 = noise_acc * 16 / noise_n;
                if (noise_ema_x16 == -120 * 16) noise_ema_x16 = avg_x16;
                else noise_ema_x16 += (avg_x16 - noise_ema_x16) / (loud ? 64 : 4);
                s_stats.noise_dbm = (int16_t)(noise_ema_x16 / 16);
            }
            win_syncs = s_stats.syncs;
            win_payloads = s_stats.payloads;
            win_decoded = s_stats.decoded;
            hist_t0 = now;
            hist_max = -128;
            noise_acc = noise_n = 0;
        }
        xSemaphoreGive(s_lock);
    }
}

/* ------------------------------------------------------------ API ------ */

esp_err_t radio_init(radio_packet_cb_t cb)
{
    s_cb = cb;
    s_lock = xSemaphoreCreateMutex();

    esp_err_t err = hw_init(&s_stats);
    if (err != ESP_OK) return err;
    s_stats.present = true;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.rxbw_hz = hw_rxbw(s_cfg.rxbw_hz);
    apply_config();
    xSemaphoreGive(s_lock);

    /* Single-core chips (ESP32-C3) only have core 0. */
    xTaskCreatePinnedToCore(radio_task, "radio", 4096, NULL, 10, &s_task, portNUM_PROCESSORS - 1);
    hw_start_irq();

    ESP_LOGI(TAG, "%s RX %.3f MHz, %u chip/s, RxBw %u Hz, sync %s", s_stats.chip, s_cfg.freq_hz / 1e6,
             (unsigned)s_cfg.bitrate, (unsigned)s_cfg.rxbw_hz, sync_str(s_cfg.sync));
    return ESP_OK;
}

const radio_caps_t *radio_caps(void)
{
    return &radio_hw_caps;
}

int radio_bandwidths(uint32_t *out, int max)
{
    return hw_bandwidths(out, max);
}

void radio_get_stats(radio_stats_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_stats;
    xSemaphoreGive(s_lock);
}

void radio_reset_stats(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    radio_stats_t keep = s_stats;
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.present = keep.present;
    memcpy(s_stats.chip, keep.chip, sizeof(s_stats.chip));
    s_stats.noise_dbm = keep.noise_dbm;
    s_stats.pll_locked = keep.pll_locked;
    xSemaphoreGive(s_lock);
}

void radio_get_config(radio_config_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

esp_err_t radio_set_config(const radio_config_t *cfg)
{
    if (cfg->freq_hz < radio_hw_caps.freq_min_hz || cfg->freq_hz > radio_hw_caps.freq_max_hz)
        return ESP_ERR_INVALID_ARG;
    if (cfg->bitrate < radio_hw_caps.bitrate_min || cfg->bitrate > radio_hw_caps.bitrate_max)
        return ESP_ERR_INVALID_ARG;
    if (!s_stats.present) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    s_cfg.rxbw_hz = hw_rxbw(cfg->rxbw_hz);  /* store the bandwidth the chip actually uses */
    esp_err_t err = apply_config();
    radio_config_t now = s_cfg;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "reconfigured: %.3f MHz, %u chip/s, RxBw %u Hz", now.freq_hz / 1e6, (unsigned)now.bitrate,
             (unsigned)now.rxbw_hz);
    return err;
}

int radio_get_events(radio_event_t *out, int max, uint32_t *total)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_ev_count < max ? s_ev_count : max;
    for (int i = 0; i < n; i++) out[i] = s_events[(s_ev_head - n + i + RADIO_EVENTS) % RADIO_EVENTS];
    if (total) *total = s_ev_total;
    xSemaphoreGive(s_lock);
    return n;
}

int radio_get_captures(radio_capture_t *out, int max, uint32_t *total)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_cap_count < max ? s_cap_count : max;
    for (int i = 0; i < n; i++) out[i] = s_caps[(s_cap_head - n + i + RADIO_CAPTURES) % RADIO_CAPTURES];
    if (total) *total = s_cap_total;
    xSemaphoreGive(s_lock);
    return n;
}

int radio_get_top_events(radio_event_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_top_count < max ? s_top_count : max;
    for (int i = 0; i < n; i++) out[i] = s_top[i];
    xSemaphoreGive(s_lock);
    /* oldest first (n <= 16) */
    for (int i = 1; i < n; i++)
        for (int k = i; k > 0 && out[k].start_ms < out[k - 1].start_ms; k--) {
            radio_event_t t = out[k];
            out[k] = out[k - 1];
            out[k - 1] = t;
        }
    return n;
}

void radio_clear_log(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ev_count = s_cap_count = s_top_count = 0;
    s_ev_total = s_cap_total = 0;
    xSemaphoreGive(s_lock);
}

void radio_default_config(radio_config_t *out)
{
    *out = k_default_cfg;
    out->rxbw_hz = hw_rxbw(out->rxbw_hz);
}

int radio_rssi_history(int8_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_hist_count < max ? s_hist_count : max;
    for (int i = 0; i < n; i++) {
        out[i] = s_hist[(s_hist_head - n + i + HIST_LEN) % HIST_LEN];
    }
    xSemaphoreGive(s_lock);
    return n;
}

int radio_pll_scan(const uint32_t *freqs_hz, int n, FILE *out)
{
    if (!s_stats.present || n <= 0) return 0;
    int n_locked = 0;
    char detail[64];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < n; i++) {
        detail[0] = 0;
        bool locked = hw_pll_test(freqs_hz[i], detail, sizeof(detail));
        n_locked += locked;
        fprintf(out, "%9.3f MHz  %-8s %s\n", freqs_hz[i] / 1e6, locked ? "locked" : "NO LOCK", detail);
    }
    apply_config();
    xSemaphoreGive(s_lock);
    fprintf(out, "%d of %d frequencies locked; receiver back on %.3f MHz (%s)\n", n_locked, n, s_cfg.freq_hz / 1e6,
            s_stats.pll_locked ? "locked" : "NOT LOCKED");
    return n_locked;
}

esp_err_t radio_tx_ford(const uint8_t payload[8], int count, int gap_ms, bool pa_boost, int power_dbm)
{
    if (!radio_hw_caps.tx) return ESP_ERR_NOT_SUPPORTED;
    if (count < 1 || count > 20 || gap_ms < 0 || gap_ms > 1000) return ESP_ERR_INVALID_ARG;
    if (!s_stats.present) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = hw_tx_ford(&s_cfg, payload, count, gap_ms, pa_boost, power_dbm);
    /* Back to receive with the saved settings. */
    apply_config();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "tx %d frame(s) %s %d dBm at %.3f MHz: %s", count, pa_boost ? "pa" : "rfo", power_dbm,
             s_cfg.freq_hz / 1e6, esp_err_to_name(err));
    return err;
}

void radio_set_raw_log(bool on)
{
    s_raw_log = on;
}

void radio_dump_regs(FILE *out)
{
    if (!s_stats.present) {
        fprintf(out, "no radio\n");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    hw_dump(out);
    xSemaphoreGive(s_lock);
}

void radio_probe(FILE *out)
{
    /* The probe also works without a detected chip: it is how wiring faults are found. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    hw_probe(out);
    if (s_stats.present) apply_config();
    xSemaphoreGive(s_lock);
}
