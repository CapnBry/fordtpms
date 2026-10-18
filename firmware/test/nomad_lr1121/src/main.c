/*
 * 315 MHz loopback test for a Radiomaster Nomad (ESP32-PICO-D4 + 2x LR1121,
 * 900 MHz / 2.4 GHz front ends). Radio 1 transmits a carrier or Ford TPMS
 * frames; radio 2 receives with the same GFSK settings as the receiver
 * firmware (radio_lr1121.c) and reports RSSI and decodes. Another receiver
 * (the ESP32-C3 + LR1121 board) can listen at the same time.
 *
 * Transmit power is kept minimal: LR1121 low-power PA, -17 dBm by default,
 * and the external PA's control DAC (GPIO26) is left undriven until the
 * `apc` command sets it. Carrier time is capped per command.
 *
 * Pins from the ExpressLRS target "TX/Radiomaster Nomad.json". CLI on UART0
 * (CP210x, 115200).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/dac_oneshot.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tpms_ford.h"

static const char *TAG = "nomad";

#define PIN_SCK  25
#define PIN_MISO 33
#define PIN_MOSI 32

/* LR1121 commands (see firmware/src/radio_lr1121.c) */
#define OP_GET_STATUS        0x0100
#define OP_GET_VERSION       0x0101
#define OP_WRITE_BUFFER8     0x0109
#define OP_READ_BUFFER8      0x010A
#define OP_GET_ERRORS        0x010D
#define OP_CLEAR_ERRORS      0x010E
#define OP_CALIBRATE         0x010F
#define OP_SET_REGMODE       0x0110
#define OP_CALIB_IMAGE       0x0111
#define OP_SET_DIO_RF_SWITCH 0x0112
#define OP_CLEAR_IRQ         0x0114
#define OP_SET_STANDBY       0x011C
#define OP_GET_RXBUF_STATUS  0x0203
#define OP_GET_PKT_STATUS    0x0204
#define OP_GET_RSSI_INST     0x0205
#define OP_SET_GFSK_SYNC     0x0206
#define OP_SET_RX            0x0209
#define OP_SET_TX            0x020A
#define OP_SET_RF_FREQ       0x020B
#define OP_SET_PKT_TYPE      0x020E
#define OP_SET_MOD_PARAMS    0x020F
#define OP_SET_PKT_PARAMS    0x0210
#define OP_SET_TX_PARAMS     0x0211
#define OP_SET_PA_CFG        0x0215
#define OP_SET_TX_CW         0x0219
#define OP_SET_RX_BOOSTED    0x0227

#define IRQ_TX_DONE    (1u << 2)
#define IRQ_RX_DONE    (1u << 3)
#define IRQ_SYNC_VALID (1u << 5)
#define ERR_PLL_LOCK   (1u << 7)

#define BITRATE   19231
#define FDEV_HZ   35000
#define RXBW_CODE 0x1A     /* 156.2 kHz double-sided = +/-78.1 kHz, as on the C3 board */
#define MAX_CW_MS 2000

typedef struct {
    const char *name;
    int nss, busy, dio, rst;
    spi_device_handle_t spi;
    uint8_t stat1, stat2;
    bool ok;
} lr_t;

static lr_t s_r[2] = {
    {.name = "radio1 (tx)", .nss = 27, .busy = 36, .dio = 37, .rst = 15},
    {.name = "radio2 (rx)", .nss = 13, .busy = 39, .dio = 34, .rst = 21},
};
#define TX (&s_r[0])
#define RX (&s_r[1])

static dac_oneshot_handle_t s_apc;
static int s_apc_value = -1;

/* --------------------------------------------------------------- SPI --- */

static bool wait_busy(lr_t *r, uint32_t timeout_ms)
{
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(r->busy)) {
        int64_t dt = esp_timer_get_time() - t0;
        if (dt > (int64_t)timeout_ms * 1000) return false;
        if (dt < 2000) esp_rom_delay_us(5);
        else vTaskDelay(1);
    }
    return true;
}

static esp_err_t xfer(lr_t *r, const uint8_t *tx, uint8_t *rx, size_t n)
{
    spi_transaction_t t = {.length = n * 8, .tx_buffer = tx, .rx_buffer = rx};
    return spi_device_polling_transmit(r->spi, &t);
}

static esp_err_t cmd_echo(lr_t *r, uint16_t op, const uint8_t *p, size_t n, uint8_t *echo, uint32_t busy_ms)
{
    uint8_t tx[2 + 32], rx[2 + 32];
    if (n > 32) return ESP_ERR_INVALID_SIZE;
    tx[0] = op >> 8;
    tx[1] = op & 0xff;
    if (n) memcpy(tx + 2, p, n);
    if (!wait_busy(r, busy_ms)) return ESP_ERR_TIMEOUT;
    esp_err_t err = xfer(r, tx, rx, 2 + n);
    if (err != ESP_OK) return err;
    r->stat1 = rx[0];
    r->stat2 = rx[1];
    if (echo) memcpy(echo, rx, 2 + n);
    return ESP_OK;
}

static esp_err_t cmd(lr_t *r, uint16_t op, const uint8_t *p, size_t n)
{
    return cmd_echo(r, op, p, n, NULL, 10);
}

static esp_err_t cmd_read(lr_t *r, uint16_t op, const uint8_t *p, size_t n, uint8_t *out, size_t out_n)
{
    uint8_t tx[1 + 32] = {0}, rx[1 + 32];
    if (out_n > 32) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = cmd(r, op, p, n);
    if (err != ESP_OK) return err;
    if (!wait_busy(r, 10)) return ESP_ERR_TIMEOUT;
    err = xfer(r, tx, rx, 1 + out_n);
    if (err != ESP_OK) return err;
    r->stat1 = rx[0];
    memcpy(out, rx + 1, out_n);
    int st = (r->stat1 >> 1) & 7;
    return (st == 0 || st == 1) ? ESP_FAIL : ESP_OK;
}

static void put32(uint8_t *b, uint32_t v)
{
    b[0] = v >> 24;
    b[1] = v >> 16;
    b[2] = v >> 8;
    b[3] = v;
}

static uint32_t irq_status(lr_t *r)
{
    uint8_t z[4] = {0}, e[6] = {0};
    cmd_echo(r, OP_GET_STATUS, z, 4, e, 10);
    return (uint32_t)e[2] << 24 | e[3] << 16 | e[4] << 8 | e[5];
}

static void clear_irq(lr_t *r, uint32_t mask)
{
    uint8_t b[4];
    put32(b, mask);
    cmd(r, OP_CLEAR_IRQ, b, 4);
}

static uint16_t get_errors(lr_t *r)
{
    uint8_t b[2] = {0};
    if (cmd_read(r, OP_GET_ERRORS, NULL, 0, b, 2) != ESP_OK) return 0xffff;
    return (uint16_t)(b[0] << 8 | b[1]);
}

static int chip_mode(lr_t *r)
{
    return (r->stat2 >> 1) & 7;
}

static const char *mode_name(int m)
{
    static const char *const n[] = {"sleep", "stby_rc", "stby_xosc", "fs", "rx", "tx", "wifi/gnss", "?"};
    return n[m & 7];
}

/* Raw chip RSSI (no external LNA correction). */
static int rssi_inst(lr_t *r)
{
    uint8_t b = 0;
    cmd_read(r, OP_GET_RSSI_INST, NULL, 0, &b, 1);
    return -(int)b / 2;
}

static void standby(lr_t *r)
{
    uint8_t m = 0;
    cmd(r, OP_SET_STANDBY, &m, 1);
}

/* -------------------------------------------------------------- setup --- */

static bool lr_init(lr_t *r)
{
    gpio_config_t out = {.pin_bit_mask = 1ULL << r->rst, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&out);
    gpio_config_t in = {.pin_bit_mask = (1ULL << r->busy) | (1ULL << r->dio), .mode = GPIO_MODE_INPUT};
    gpio_config(&in);
    spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = 8 * 1000 * 1000,
        .spics_io_num = r->nss,
        .queue_size = 2,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev, &r->spi));

    gpio_set_level(r->rst, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(r->rst, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    if (!wait_busy(r, 500)) {
        ESP_LOGE(TAG, "%s: BUSY stuck high after reset", r->name);
        return false;
    }
    uint8_t v[4] = {0};
    esp_err_t err = cmd_read(r, OP_GET_VERSION, NULL, 0, v, 4);
    ESP_LOGI(TAG, "%s: GetVersion hw 0x%02x type 0x%02x fw %02x.%02x (%s)", r->name, v[0], v[1], v[2], v[3],
             esp_err_to_name(err));
    if (err != ESP_OK || (v[0] == v[1] && v[1] == v[2])) return false;

    cmd(r, OP_CLEAR_ERRORS, NULL, 0);
    standby(r);
    uint8_t regmode = 1;   /* DC-DC (radio_dcdc) */
    cmd(r, OP_SET_REGMODE, &regmode, 1);
    /* ExpressLRS default RF switch table (the Nomad target defines none) */
    static const uint8_t rfsw[8] = {0x0f, 0x00, 0x04, 0x08, 0x08, 0x02, 0x00, 0x01};
    cmd(r, OP_SET_DIO_RF_SWITCH, rfsw, sizeof(rfsw));
    uint8_t cal = 0x3f;
    cmd(r, OP_CALIBRATE, &cal, 1);
    wait_busy(r, 200);
    uint16_t e = get_errors(r);
    if (e) ESP_LOGW(TAG, "%s: errors after calibration 0x%04x", r->name, e);
    cmd(r, OP_CLEAR_ERRORS, NULL, 0);
    return true;
}

/* GFSK at the Ford chip rate. payload_len bytes, sync_bits 0 or 16. */
static void gfsk_config(lr_t *r, uint32_t freq_hz, uint8_t preamble_bits, uint8_t sync_bits, uint8_t payload_len)
{
    standby(r);
    cmd(r, OP_CLEAR_ERRORS, NULL, 0);
    uint8_t pt = 0x01;
    cmd(r, OP_SET_PKT_TYPE, &pt, 1);
    uint8_t mod[10];
    put32(mod, BITRATE);
    mod[4] = 0x00;
    mod[5] = RXBW_CODE;
    put32(mod + 6, FDEV_HZ);
    cmd(r, OP_SET_MOD_PARAMS, mod, sizeof(mod));
    uint8_t pkt[9] = {0, preamble_bits, 0x00, sync_bits, 0x00, 0x00, payload_len, 0x01, 0x00};
    cmd(r, OP_SET_PKT_PARAMS, pkt, sizeof(pkt));
    uint8_t sync[8] = {0x55, 0x56, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55};
    cmd(r, OP_SET_GFSK_SYNC, sync, sizeof(sync));
    uint8_t f[4];
    put32(f, freq_hz);
    cmd(r, OP_SET_RF_FREQ, f, 4);
    uint32_t mhz = freq_hz / 1000000;
    uint8_t cal[2] = {(uint8_t)((mhz - 1) / 4), (uint8_t)(1 + (mhz + 1) / 4)};
    cmd(r, OP_CALIB_IMAGE, cal, 2);
    wait_busy(r, 100);
}

/* Receiver: same settings as the C3 board's firmware. Returns true if locked in RX. */
static bool rx_start(uint32_t freq_hz)
{
    gfsk_config(RX, freq_hz, 32, 16, TPMS_FORD_CHIP_BYTES);
    uint8_t boosted = 1;
    cmd(RX, OP_SET_RX_BOOSTED, &boosted, 1);
    clear_irq(RX, 0xffffffff);
    uint8_t t[3] = {0xff, 0xff, 0xff};
    cmd(RX, OP_SET_RX, t, 3);
    vTaskDelay(pdMS_TO_TICKS(2));
    uint16_t e = get_errors(RX);
    irq_status(RX);
    return !(e & ERR_PLL_LOCK) && chip_mode(RX) == 4;
}

/* Transmitter: low-power PA (radio_rfo_hf), power -17..+14 dBm. */
static void tx_prepare(uint32_t freq_hz, int dbm, uint8_t payload_len)
{
    gfsk_config(TX, freq_hz, 16, 0, payload_len);
    uint8_t pa[4] = {0x00 /* LP PA */, 0x00 /* VREG */, 0x07, 0x00};
    cmd(TX, OP_SET_PA_CFG, pa, 4);
    uint8_t txp[2] = {(uint8_t)(int8_t)dbm, 0x02 /* 48 us ramp */};
    cmd(TX, OP_SET_TX_PARAMS, txp, 2);
    clear_irq(TX, 0xffffffff);
}

/* ------------------------------------------------------------ measure --- */

typedef struct {
    int avg, max, n;
} rssi_t;

static rssi_t sample_rssi(int ms)
{
    rssi_t s = {.max = -200};
    int sum = 0;
    int64_t end = esp_timer_get_time() + ms * 1000LL;
    while (esp_timer_get_time() < end) {
        int v = rssi_inst(RX);
        sum += v;
        s.n++;
        if (v > s.max) s.max = v;
        esp_rom_delay_us(500);
    }
    s.avg = s.n ? sum / s.n : 0;
    return s;
}

/* Carrier on radio 1 for `ms`, RSSI on radio 2 before and during. */
static bool cw_measure(uint32_t freq_hz, int dbm, int ms, rssi_t *before, rssi_t *during, char *note, size_t nl)
{
    bool rx_ok = rx_start(freq_hz);
    *before = sample_rssi(20);
    tx_prepare(freq_hz, dbm, 1);
    cmd(TX, OP_SET_TX_CW, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    uint16_t te = get_errors(TX);
    irq_status(TX);
    int tx_mode = chip_mode(TX);
    *during = sample_rssi(ms);
    standby(TX);
    snprintf(note, nl, "%s%s%s", rx_ok ? "" : " RX-NOT-LOCKED", (te & ERR_PLL_LOCK) ? " TX-PLL-ERR" : "",
             tx_mode == 5 ? "" : " TX-NOT-IN-TX");
    return rx_ok && !(te & ERR_PLL_LOCK) && tx_mode == 5;
}

/* ---------------------------------------------------------------- CLI --- */

static int cmd_info(int argc, char **argv)
{
    for (int i = 0; i < 2; i++) {
        lr_t *r = &s_r[i];
        uint8_t v[4] = {0};
        cmd_read(r, OP_GET_VERSION, NULL, 0, v, 4);
        uint32_t irq = irq_status(r);
        uint16_t e = get_errors(r);
        printf("%s: %s, hw 0x%02x type 0x%02x fw %02x.%02x, mode %s, errors 0x%04x, irq 0x%08x, rssi %d dBm (chip)\n",
               r->name, r->ok ? "ok" : "NOT FOUND", v[0], v[1], v[2], v[3], mode_name(chip_mode(r)), e,
               (unsigned)irq, rssi_inst(r));
    }
    printf("external PA DAC (GPIO26): %s", s_apc_value < 0 ? "not driven\n" : "");
    if (s_apc_value >= 0) printf("%d\n", s_apc_value);
    return 0;
}

static int cmd_cw(int argc, char **argv)
{
    double mhz = argc > 1 ? atof(argv[1]) : 315.0;
    int dbm = argc > 2 ? atoi(argv[2]) : -17;
    int ms = argc > 3 ? atoi(argv[3]) : 200;
    if (mhz < 30 || mhz > 1000 || dbm < -17 || dbm > 14 || ms < 1 || ms > MAX_CW_MS) {
        printf("usage: cw [MHz 30-1000] [dBm -17..14] [ms 1-%d]\n", MAX_CW_MS);
        return 1;
    }
    rssi_t b, d;
    char note[64];
    cw_measure((uint32_t)(mhz * 1e6 + 0.5), dbm, ms, &b, &d, note, sizeof(note));
    printf("%.3f MHz %d dBm %d ms: radio2 rssi before avg %d max %d, during avg %d max %d, delta %+d dB%s\n", mhz, dbm,
           ms, b.avg, b.max, d.avg, d.max, d.avg - b.avg, note);
    return 0;
}

static int cmd_sweep(int argc, char **argv)
{
    double start = 250, stop = 450, step = 10;
    int dbm = -17;
    if (argc >= 4) {
        start = atof(argv[1]);
        stop = atof(argv[2]);
        step = atof(argv[3]);
    }
    if (argc >= 5) dbm = atoi(argv[4]);
    if (argc != 1 && argc != 4 && argc != 5) {
        printf("usage: sweep [start stop step MHz] [dBm]\n");
        return 1;
    }
    if (start < 30 || stop > 1000 || stop < start || step <= 0 || (stop - start) / step > 200 || dbm < -17 ||
        dbm > 14) {
        printf("need 30 <= start <= stop <= 1000 MHz, at most 200 steps, dBm -17..14\n");
        return 1;
    }
    printf("   MHz   before  during   delta  (radio2 chip RSSI, 30 ms carrier per step)\n");
    for (double f = start; f <= stop + 1e-9; f += step) {
        rssi_t b, d;
        char note[64];
        cw_measure((uint32_t)(f * 1e6 + 0.5), dbm, 30, &b, &d, note, sizeof(note));
        int bars = d.avg - b.avg;
        printf("%7.2f  %6d  %6d  %+5d  ", f, b.avg, d.avg, bars);
        for (int i = 0; i < bars && i < 60; i += 2) putchar('#');
        printf("%s\n", note);
    }
    standby(TX);
    rx_start(315000000);
    return 0;
}

static bool parse_hex(const char *h, uint8_t *b, size_t *n)
{
    size_t len = strlen(h);
    if (len != 14 && len != 16) return false;
    for (size_t i = 0; i < len / 2; i++) {
        char t[3] = {h[2 * i], h[2 * i + 1], 0};
        if (!isxdigit((unsigned char)t[0]) || !isxdigit((unsigned char)t[1])) return false;
        b[i] = (uint8_t)strtoul(t, NULL, 16);
    }
    *n = len / 2;
    return true;
}

static int cmd_tx(int argc, char **argv)
{
    /* Default: fake ID feedf00d, 38 PSI, 29 C, rest (same as the receiver firmware's `tx`). */
    uint8_t b[8] = {0xfe, 0xed, 0xf0, 0x0d, 0x98, 0x55, 0x06, 0};
    int count = 4, dbm = -17;
    double mhz = 315.0;
    bool fix = true;
    for (int i = 1; i < argc; i++) {
        size_t n;
        if (i + 1 < argc && !strcmp(argv[i], "count")) count = atoi(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "freq")) mhz = atof(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "dbm")) dbm = atoi(argv[++i]);
        else if (parse_hex(argv[i], b, &n)) fix = n == 7;
        else {
            printf("usage: tx [7 or 8 hex bytes] [count 1-20] [freq MHz] [dbm -17..14]\n");
            return 1;
        }
    }
    if (count < 1 || count > 20 || dbm < -17 || dbm > 14 || mhz < 30 || mhz > 1000) {
        printf("count 1-20, dbm -17..14, freq 30-1000 MHz\n");
        return 1;
    }
    if (fix) tpms_ford_fix_checksum(b);
    uint8_t frame[TPMS_FORD_FRAME_BYTES];
    tpms_ford_encode(b, frame);
    uint32_t f = (uint32_t)(mhz * 1e6 + 0.5);

    bool rx_ok = rx_start(f);
    tx_prepare(f, dbm, sizeof(frame));
    printf("tx %02x%02x%02x%02x%02x%02x%02x%02x x%d at %.3f MHz %d dBm; radio2 %s\n", b[0], b[1], b[2], b[3], b[4],
           b[5], b[6], b[7], count, mhz, dbm, rx_ok ? "receiving" : "NOT LOCKED");
    int decoded = 0;
    for (int i = 0; i < count; i++) {
        cmd(TX, OP_WRITE_BUFFER8, frame, sizeof(frame));
        uint8_t t[3] = {0, 0, 0};
        cmd(TX, OP_SET_TX, t, 3);
        int64_t t0 = esp_timer_get_time();
        bool tx_done = false, got = false, sync = false;
        int sync_rssi = 0;
        uint8_t buf[TPMS_FORD_CHIP_BYTES] = {0};
        uint8_t ps[4] = {0};
        while (esp_timer_get_time() - t0 < 40000 && !(tx_done && got)) {
            uint32_t ti = irq_status(TX);
            if (ti & IRQ_TX_DONE) {
                tx_done = true;
                clear_irq(TX, ti);
            }
            uint32_t ri = irq_status(RX);
            if (ri) clear_irq(RX, ri);
            if ((ri & IRQ_SYNC_VALID) && !sync) {
                sync = true;
                sync_rssi = rssi_inst(RX);
            }
            if (ri & IRQ_RX_DONE) {
                uint8_t bs[2] = {0};
                cmd_read(RX, OP_GET_RXBUF_STATUS, NULL, 0, bs, 2);
                cmd_read(RX, OP_GET_PKT_STATUS, NULL, 0, ps, 4);
                uint8_t rb[2] = {bs[1], TPMS_FORD_CHIP_BYTES};
                cmd_read(RX, OP_READ_BUFFER8, rb, 2, buf, sizeof(buf));
                got = true;
            }
        }
        uint16_t te = get_errors(TX);
        printf("  #%d tx %s%s", i + 1, tx_done ? "done" : "TIMEOUT", (te & ERR_PLL_LOCK) ? " (TX PLL error)" : "");
        if (got) {
            tpms_ford_t pkt;
            tpms_ford_status_t st = tpms_ford_decode_chips(buf, TPMS_FORD_CHIP_BYTES * 8, 0, false, &pkt);
            char hex[2 * TPMS_FORD_CHIP_BYTES + 1];
            for (int k = 0; k < TPMS_FORD_CHIP_BYTES; k++) sprintf(hex + 2 * k, "%02x", buf[k]);
            printf(", rx sync rssi %d dBm: %s", -(int)ps[0] / 2, tpms_ford_status_str(st));
            if (st == TPMS_FORD_OK) {
                decoded++;
                printf(" id=%08x %.2f PSI", (unsigned)pkt.id, pkt.psi_x4 / 4.0);
            } else {
                printf(" raw %s", hex);
            }
        } else {
            printf(", rx %s", sync ? "sync only" : "nothing");
            if (sync) printf(" (rssi %d)", sync_rssi);
        }
        printf("\n");
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    standby(TX);
    printf("%d of %d decoded by radio2\n", decoded, count);
    return 0;
}

/* Listen on radio 2 only (no transmit): report every sync and payload. */
static int cmd_rx(int argc, char **argv)
{
    double mhz = argc > 1 ? atof(argv[1]) : 315.0;
    int secs = argc > 2 ? atoi(argv[2]) : 10;
    if (mhz < 30 || mhz > 1000 || secs < 1 || secs > 60) {
        printf("usage: rx [MHz] [seconds 1-60]\n");
        return 1;
    }
    bool ok = rx_start((uint32_t)(mhz * 1e6 + 0.5));
    printf("radio2 listening at %.3f MHz for %d s (%s)\n", mhz, secs, ok ? "locked" : "NOT LOCKED");
    int64_t t0 = esp_timer_get_time(), end = t0 + secs * 1000000LL;
    int syncs = 0, payloads = 0, decoded = 0, peak = -200;
    while (esp_timer_get_time() < end) {
        uint32_t ri = irq_status(RX);
        if (ri) clear_irq(RX, ri);
        if (ri & IRQ_SYNC_VALID) syncs++;
        if (ri & IRQ_RX_DONE) {
            uint8_t bs[2] = {0}, ps[4] = {0}, buf[TPMS_FORD_CHIP_BYTES] = {0};
            cmd_read(RX, OP_GET_RXBUF_STATUS, NULL, 0, bs, 2);
            cmd_read(RX, OP_GET_PKT_STATUS, NULL, 0, ps, 4);
            uint8_t rb[2] = {bs[1], TPMS_FORD_CHIP_BYTES};
            cmd_read(RX, OP_READ_BUFFER8, rb, 2, buf, sizeof(buf));
            payloads++;
            tpms_ford_t pkt;
            tpms_ford_status_t st = tpms_ford_decode_chips(buf, TPMS_FORD_CHIP_BYTES * 8, 0, false, &pkt);
            int rssi = -(int)ps[0] / 2;
            if (st == TPMS_FORD_OK) {
                decoded++;
                printf("  %6.3f s  rssi %d dBm  ok id=%08x %.2f PSI\n", (esp_timer_get_time() - t0) / 1e6, rssi,
                       (unsigned)pkt.id, pkt.psi_x4 / 4.0);
            } else if (rssi > -100) {
                printf("  %6.3f s  rssi %d dBm  %s\n", (esp_timer_get_time() - t0) / 1e6, rssi,
                       tpms_ford_status_str(st));
            }
        } else {
            int r = rssi_inst(RX);
            if (r > peak) peak = r;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    printf("%d syncs, %d payloads, %d decoded; peak idle rssi %d dBm (chip)\n", syncs, payloads, decoded, peak);
    return 0;
}

static int cmd_apc(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: apc <0-255|off>  (external PA control DAC on GPIO26; ExpressLRS uses 95-120)\n");
        return 1;
    }
    if (!strcmp(argv[1], "off")) {
        if (s_apc) dac_oneshot_del_channel(s_apc);
        s_apc = NULL;
        s_apc_value = -1;
        gpio_reset_pin(26);
        printf("PA DAC released\n");
        return 0;
    }
    int v = atoi(argv[1]);
    if (v < 0 || v > 255) return 1;
    if (!s_apc) {
        dac_oneshot_config_t c = {.chan_id = DAC_CHAN_1};
        if (dac_oneshot_new_channel(&c, &s_apc) != ESP_OK) {
            printf("DAC init failed\n");
            return 1;
        }
    }
    dac_oneshot_output_voltage(s_apc, (uint8_t)v);
    s_apc_value = v;
    printf("PA DAC = %d\n", v);
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    esp_restart();
    return 0;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Nomad LR1121 315 MHz loopback test");
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_DISABLED));
    gpio_pullup_en(PIN_MISO);
    for (int i = 0; i < 2; i++) s_r[i].ok = lr_init(&s_r[i]);
    if (RX->ok) ESP_LOGI(TAG, "radio2 RX at 315 MHz: %s", rx_start(315000000) ? "locked" : "NOT LOCKED");

    static const esp_console_cmd_t cmds[] = {
        {.command = "info", .help = "Both radios: version, mode, errors, RSSI; PA DAC state", .func = cmd_info},
        {.command = "cw", .help = "Carrier on radio1, RSSI on radio2 (default 315 MHz, -17 dBm, 200 ms)",
         .hint = "[MHz] [dBm] [ms]", .func = cmd_cw},
        {.command = "sweep", .help = "30 ms carrier per step, RSSI delta on radio2 (default 250-450 MHz / 10, -17 dBm)",
         .hint = "[start stop step] [dBm]", .func = cmd_sweep},
        {.command = "tx", .help = "Ford frames on radio1 (default feedf00d x4 at 315 MHz -17 dBm), decoded on radio2",
         .hint = "[hex] [count n] [freq MHz] [dbm n]", .func = cmd_tx},
        {.command = "rx", .help = "Listen on radio2 only and report Ford packets (default 315 MHz, 10 s)",
         .hint = "[MHz] [seconds]", .func = cmd_rx},
        {.command = "apc", .help = "External PA control DAC on GPIO26 (default: not driven)", .hint = "<0-255|off>",
         .func = cmd_apc},
        {.command = "reboot", .help = "Restart", .func = cmd_reboot},
    };
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rcfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rcfg.prompt = "nomad>";
    rcfg.max_cmdline_length = 128;
    rcfg.task_stack_size = 6144;
    esp_console_dev_uart_config_t ucfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&ucfg, &rcfg, &repl));
    ESP_ERROR_CHECK(esp_console_register_help_command());
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
