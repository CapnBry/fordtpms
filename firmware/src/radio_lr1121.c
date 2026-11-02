/*
 * LR1121 backend: GFSK packet mode on the sub-GHz path.
 *
 * The LR11xx takes 16-bit opcode commands over SPI. Each command waits for
 * BUSY low first. A read sends the command, waits for BUSY low again, then
 * clocks out Stat1 + the response. Every transaction also returns Stat1,
 * Stat2 and the IRQ flags in its first bytes; GetStatus uses that.
 *
 * Events come from SyncWordValid (RSSI latched there) and RxDone on the
 * single IRQ line (radio_dio1 = LR1121 DIO9). The radio task polls the
 * status every ~5 ms as well, so a missed edge only costs latency.
 *
 * PLL lock: the chip reports a failed lock as PLL_LOCK_ERR in GetErrors, and
 * stays out of RX. That (plus the chip mode in Stat2) is checked after each
 * retune and every RADIO_LOCK_CHECK_MS while receiving.
 *
 * Front end setup follows the ExpressLRS LR1121 driver for this hardware:
 * crystal (no TCXO), DC-DC on, default RF switch table, RX boosted.
 */
#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radio_hw.h"

static const char *TAG = "lr1121";

/* System / register commands */
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
#define OP_SET_DIO_IRQ       0x0113
#define OP_CLEAR_IRQ         0x0114
#define OP_GET_VBAT          0x0119
#define OP_GET_TEMP          0x011A
#define OP_SET_STANDBY       0x011C
#define OP_SET_FS            0x011D
/* Radio commands */
#define OP_RESET_STATS       0x0200
#define OP_GET_STATS         0x0201
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
#define OP_SET_FALLBACK      0x0213
#define OP_SET_PA_CFG        0x0215
#define OP_SET_RX_BOOSTED    0x0227

#define IRQ_TX_DONE          (1u << 2)
#define IRQ_RX_DONE          (1u << 3)
#define IRQ_PREAMBLE         (1u << 4)
#define IRQ_SYNC_VALID       (1u << 5)
#define IRQ_CRC_ERR          (1u << 7)
#define IRQ_TIMEOUT          (1u << 10)
#define IRQ_CMD_ERROR        (1u << 22)
#define IRQ_ERROR            (1u << 23)
#define IRQ_FSK_LEN_ERR      (1u << 24)
#define IRQ_RX_MASK          (IRQ_RX_DONE | IRQ_SYNC_VALID | IRQ_FSK_LEN_ERR | IRQ_ERROR)

#define ERR_PLL_CALIB        (1u << 3)
#define ERR_IMG_CALIB        (1u << 4)
#define ERR_HF_XOSC_START    (1u << 5)
#define ERR_PLL_LOCK         (1u << 7)

/* Stat1 bits 3:1 */
#define CMD_FAIL 0
#define CMD_PERR 1
#define CMD_OK   2
#define CMD_DAT  3
/* Stat2 bits 3:1 */
#define MODE_SLEEP    0
#define MODE_STBY_RC  1
#define MODE_STBY_XOSC 2
#define MODE_FS       3
#define MODE_RX       4
#define MODE_TX       5

#define PKT_TYPE_GFSK 0x01
#define RX_CONTINUOUS 0xFFFFFF

#define RADIO_LOCK_CHECK_MS 250

/* Status polling between IRQs, as before the CC1101 went interrupt-only. */
const uint32_t hw_poll_ms = 5;

const radio_caps_t radio_hw_caps = {
    .name = "LR1121",
    .freq_min_hz = 150000000,
    .freq_max_hz = 960000000,
    .freq_note = "specified 150-960 MHz",
    .bitrate_min = 600,
    .bitrate_max = 300000,
    .tx = true,
    .tx_default_dbm = -9,
    .tx_note = "pa -9..22 dBm (high-power PA, wired on ExpressLRS LR1121 receivers) | rfo -17..14 dBm (low-power PA)",
};

/* GFSK channel filters: double-sided bandwidth (Hz) and code, ascending. */
static const struct {
    uint32_t dsb_hz;
    uint8_t code;
} k_bw[] = {
    {4800, 0x1F},   {5800, 0x17},   {7300, 0x0F},   {9700, 0x1E},   {11700, 0x16},  {14600, 0x0E},
    {19500, 0x1D},  {23400, 0x15},  {29300, 0x0D},  {39000, 0x1C},  {46900, 0x14},  {58600, 0x0C},
    {78200, 0x1B},  {93800, 0x13},  {117300, 0x0B}, {156200, 0x1A}, {187200, 0x12}, {234300, 0x0A},
    {312000, 0x19}, {373600, 0x11}, {467000, 0x09},
};
#define NBW (sizeof(k_bw) / sizeof(k_bw[0]))

static spi_device_handle_t s_spi;
static radio_config_t s_cfg;       /* last applied, for the probe */
static uint8_t s_stat1, s_stat2;   /* from the last transaction */
static uint8_t s_payload_len = TPMS_FORD_CHIP_BYTES;
static bool s_in_packet, s_locked;
static uint32_t s_sync_ms, s_lock_check_ms;
static uint32_t s_rx_restarts;     /* RX re-entered after the chip left it */
static uint16_t s_last_errors;

/* ------------------------------------------------------------- SPI ----- */

static bool wait_busy(uint32_t timeout_ms)
{
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(PIN_LORA_BUSY)) {
        int64_t dt = esp_timer_get_time() - t0;
        if (dt > (int64_t)timeout_ms * 1000) return false;
        /* Most commands finish in microseconds; calibration takes milliseconds. */
        if (dt < 2000) esp_rom_delay_us(5);
        else vTaskDelay(1);
    }
    return true;
}

static esp_err_t xfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
    spi_transaction_t t = {
        .length = n * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

/* Sends opcode + params. Returns the Stat1/Stat2/IRQ bytes clocked out in `echo` (optional). */
static esp_err_t cmd_write_echo(uint16_t op, const uint8_t *p, size_t n, uint8_t *echo, uint32_t busy_ms)
{
    uint8_t tx[2 + 32], rx[2 + 32];
    if (n > 32) return ESP_ERR_INVALID_SIZE;
    tx[0] = op >> 8;
    tx[1] = op & 0xff;
    if (n) memcpy(tx + 2, p, n);
    if (!wait_busy(busy_ms)) return ESP_ERR_TIMEOUT;
    esp_err_t err = xfer(tx, rx, 2 + n);
    if (err != ESP_OK) return err;
    s_stat1 = rx[0];
    s_stat2 = rx[1];
    if (echo) memcpy(echo, rx, 2 + n);
    return ESP_OK;
}

static esp_err_t cmd_write(uint16_t op, const uint8_t *p, size_t n)
{
    return cmd_write_echo(op, p, n, NULL, 10);
}

static esp_err_t cmd_read(uint16_t op, const uint8_t *p, size_t n, uint8_t *out, size_t out_n)
{
    uint8_t tx[1 + 32] = {0}, rx[1 + 32];
    if (out_n > 32) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = cmd_write(op, p, n);
    if (err != ESP_OK) return err;
    if (!wait_busy(10)) return ESP_ERR_TIMEOUT;
    err = xfer(tx, rx, 1 + out_n);
    if (err != ESP_OK) return err;
    s_stat1 = rx[0];
    memcpy(out, rx + 1, out_n);
    int st = (s_stat1 >> 1) & 7;
    return (st == CMD_FAIL || st == CMD_PERR) ? ESP_FAIL : ESP_OK;
}

static int cmd_status(void)
{
    return (s_stat1 >> 1) & 7;
}

static int chip_mode(void)
{
    return (s_stat2 >> 1) & 7;
}

static const char *mode_name(int m)
{
    static const char *const names[] = {"sleep", "stby_rc", "stby_xosc", "fs", "rx", "tx", "wifi/gnss", "?"};
    return names[m & 7];
}

/* Stat1, Stat2 and the 32-bit IRQ status, without clearing anything. */
static esp_err_t get_status(uint32_t *irq)
{
    uint8_t zero[4] = {0}, echo[6];
    esp_err_t err = cmd_write_echo(OP_GET_STATUS, zero, sizeof(zero), echo, 10);
    if (err == ESP_OK && irq) *irq = (uint32_t)echo[2] << 24 | echo[3] << 16 | echo[4] << 8 | echo[5];
    return err;
}

static void put32(uint8_t *b, uint32_t v)
{
    b[0] = v >> 24;
    b[1] = v >> 16;
    b[2] = v >> 8;
    b[3] = v;
}

static void clear_irq(uint32_t mask)
{
    uint8_t b[4];
    put32(b, mask);
    cmd_write(OP_CLEAR_IRQ, b, 4);
}

static uint16_t get_errors(void)
{
    uint8_t b[2] = {0};
    if (cmd_read(OP_GET_ERRORS, NULL, 0, b, 2) != ESP_OK) return 0xffff;
    return (uint16_t)(b[0] << 8 | b[1]);
}

static void clear_errors(void)
{
    cmd_write(OP_CLEAR_ERRORS, NULL, 0);
}

static int rssi_from_raw(uint8_t raw)
{
    return -(int)raw / 2;
}

static int rssi_inst(void)
{
    uint8_t b = 0;
    cmd_read(OP_GET_RSSI_INST, NULL, 0, &b, 1);
    return rssi_from_raw(b);
}

static void set_standby_rc(void)
{
    uint8_t m = 0;
    cmd_write(OP_SET_STANDBY, &m, 1);
}

static void set_rf_freq(uint32_t hz)
{
    uint8_t b[4];
    put32(b, hz);
    cmd_write(OP_SET_RF_FREQ, b, 4);
}

static void set_rx_continuous(void)
{
    uint8_t b[3] = {RX_CONTINUOUS >> 16, (RX_CONTINUOUS >> 8) & 0xff, RX_CONTINUOUS & 0xff};
    cmd_write(OP_SET_RX, b, 3);
}

static void hw_reset(void)
{
    gpio_set_level(PIN_LORA_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(PIN_LORA_RST, 1);
    /* BUSY stays high for ~230 ms while the chip boots. */
    vTaskDelay(pdMS_TO_TICKS(5));
    if (!wait_busy(500)) ESP_LOGW(TAG, "BUSY still high 500 ms after reset");
}

/* --------------------------------------------------------- config ------ */

static uint8_t bw_code(uint32_t ssb_hz, uint32_t *actual_ssb)
{
    int best = 0;
    uint32_t best_err = UINT32_MAX;
    for (int i = 0; i < (int)NBW; i++) {
        uint32_t ssb = k_bw[i].dsb_hz / 2;
        uint32_t err = ssb > ssb_hz ? ssb - ssb_hz : ssb_hz - ssb;
        if (err < best_err) {
            best_err = err;
            best = i;
        }
    }
    if (actual_ssb) *actual_ssb = k_bw[best].dsb_hz / 2;
    return k_bw[best].code;
}

uint32_t hw_rxbw(uint32_t hz)
{
    uint32_t actual;
    bw_code(hz, &actual);
    return actual;
}

int hw_bandwidths(uint32_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < (int)NBW && n < max; i++) out[n++] = k_bw[i].dsb_hz / 2;
    return n;
}

/* Checks the result of a tune: no PLL error and the chip in `want` mode. */
static bool check_lock(int want_mode, uint16_t *errors_out)
{
    uint16_t errors = get_errors();
    get_status(NULL);
    s_last_errors = errors;
    if (errors_out) *errors_out = errors;
    return errors != 0xffff && !(errors & ERR_PLL_LOCK) && chip_mode() == want_mode;
}

esp_err_t hw_apply(const radio_config_t *cfg, bool *locked)
{
    s_cfg = *cfg;
    set_standby_rc();
    clear_errors();

    uint8_t pt = PKT_TYPE_GFSK;
    cmd_write(OP_SET_PKT_TYPE, &pt, 1);

    uint8_t mod[10];
    put32(mod, cfg->bitrate);
    mod[4] = 0x00;                       /* no pulse shaping */
    mod[5] = bw_code(cfg->rxbw_hz, NULL);
    put32(mod + 6, 35000);               /* deviation (TX only): the sensors use ~35 kHz */
    cmd_write(OP_SET_MOD_PARAMS, mod, sizeof(mod));

    s_payload_len = (uint8_t)radio_payload_len(cfg->sync);
    uint8_t pkt[9] = {
        0, 32,      /* TX preamble bits (unused) */
        0x00,       /* preamble detector off: the sync word is the preamble tail (as on the CC1101) */
        16,         /* sync word bits */
        0x00,       /* no address filtering */
        0x00,       /* fixed length */
        s_payload_len,
        0x01,       /* CRC off */
        0x00,       /* no whitening */
    };
    cmd_write(OP_SET_PKT_PARAMS, pkt, sizeof(pkt));

    uint8_t sync[8] = {0, 0, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55};
    radio_sync_bytes(cfg->sync, sync);
    cmd_write(OP_SET_GFSK_SYNC, sync, sizeof(sync));

    set_rf_freq(cfg->freq_hz);

    /* Image calibration for the band around the frequency (4 MHz units). */
    uint32_t mhz = cfg->freq_hz / 1000000;
    uint8_t cal[2] = {(uint8_t)((mhz - 1) / 4), (uint8_t)(1 + (mhz + 1) / 4)};
    cmd_write_echo(OP_CALIB_IMAGE, cal, 2, NULL, 10);
    if (!wait_busy(100)) ESP_LOGW(TAG, "image calibration timed out");

    uint8_t boosted = 1;
    cmd_write(OP_SET_RX_BOOSTED, &boosted, 1);

    uint8_t irq[8] = {0};
    put32(irq, IRQ_RX_MASK);
    cmd_write(OP_SET_DIO_IRQ, irq, sizeof(irq));
    clear_irq(0xffffffff);

    set_rx_continuous();
    vTaskDelay(pdMS_TO_TICKS(2));
    s_in_packet = false;

    uint16_t errors;
    s_locked = check_lock(MODE_RX, &errors);
    s_lock_check_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (errors & ~ERR_PLL_LOCK)
        ESP_LOGW(TAG, "errors after tuning to %.3f MHz: 0x%04x", cfg->freq_hz / 1e6, errors);
    ESP_LOGD(TAG, "tuned %.3f MHz: errors 0x%04x, mode %s", cfg->freq_hz / 1e6, errors, mode_name(chip_mode()));
    *locked = s_locked;
    return ESP_OK;
}

bool hw_pll_test(uint32_t freq_hz, char *detail, size_t detail_len)
{
    set_standby_rc();
    clear_errors();
    set_rf_freq(freq_hz);
    get_status(NULL);                    /* Stat1 now holds the SetRfFrequency result */
    int freq_st = cmd_status();
    cmd_write(OP_SET_FS, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    uint16_t errors;
    bool locked = check_lock(MODE_FS, &errors);
    snprintf(detail, detail_len, "errors 0x%04x, mode %s%s", errors, mode_name(chip_mode()),
             freq_st == CMD_PERR ? ", frequency rejected" : freq_st == CMD_FAIL ? ", SetRfFrequency failed" : "");
    set_standby_rc();
    clear_errors();
    return locked;
}

/* --------------------------------------------------------- receive ----- */

static void IRAM_ATTR dio_isr(void *arg)
{
    radio_isr_notify(RADIO_NOTIFY_IRQ);
}

static void read_payload(radio_hw_poll_t *o)
{
    uint8_t bs[2] = {0}, ps[4] = {0};
    cmd_read(OP_GET_RXBUF_STATUS, NULL, 0, bs, 2);
    cmd_read(OP_GET_PKT_STATUS, NULL, 0, ps, 4);
    uint8_t len = bs[0] ? bs[0] : s_payload_len;
    if (len > sizeof(o->payload)) len = sizeof(o->payload);
    uint8_t rb[2] = {bs[1], len};
    if (cmd_read(OP_READ_BUFFER8, rb, 2, o->payload, len) != ESP_OK) return;
    o->payload_len = len;
    o->payload_rssi_valid = true;
    o->payload_rssi_dbm = rssi_from_raw(ps[0]);   /* RSSI at sync */
}

void hw_poll(uint32_t bits, uint32_t now, radio_hw_poll_t *o, radio_stats_t *st)
{
    bool notified = bits & RADIO_NOTIFY_IRQ;
    uint32_t irq = 0;
    if (get_status(&irq) != ESP_OK) {
        o->locked = s_locked;
        return;
    }
    if (irq) clear_irq(irq);

    if (irq & IRQ_SYNC_VALID) {
        if (notified) st->irq_sync++;
        else st->polled_events++;
        o->sync = true;
        o->sync_rssi_dbm = rssi_inst();
        s_sync_ms = now;
        s_in_packet = true;
    }
    if (irq & IRQ_RX_DONE) {
        if (notified) st->irq_payload++;
        else st->polled_events++;
        s_in_packet = false;
        read_payload(o);
    } else if (irq & IRQ_FSK_LEN_ERR) {
        st->fifo_overruns++;
        s_in_packet = false;
    } else if (s_in_packet && now - s_sync_ms > 50) {
        s_in_packet = false;
    } else if (!s_in_packet) {
        o->idle = true;
        o->idle_rssi_dbm = rssi_inst();
    }

    /* Lock check: a failed lock sets PLL_LOCK_ERR and drops the chip out of RX. */
    if ((irq & IRQ_ERROR) || now - s_lock_check_ms >= RADIO_LOCK_CHECK_MS) {
        s_lock_check_ms = now;
        uint16_t errors;
        bool locked = check_lock(MODE_RX, &errors);
        if (!locked) {
            st->pll_unlocked_polls++;
            /* Retry: clear the error and re-enter RX; the next check sees the new attempt. */
            clear_errors();
            set_rx_continuous();
            s_rx_restarts++;
        } else if (errors) {
            clear_errors();
        }
        s_locked = locked;
    }
    o->locked = s_locked;
}

/* ------------------------------------------------------------ init ----- */

static void chip_setup(void);

esp_err_t hw_init(radio_stats_t *st)
{
    gpio_config_t out = {
        .pin_bit_mask = 1ULL << PIN_LORA_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(PIN_LORA_RST, 1);
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_LORA_BUSY) | (1ULL << PIN_LORA_DIO),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_LORA_MOSI,
        .miso_io_num = PIN_LORA_MISO,
        .sclk_io_num = PIN_LORA_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED));
    gpio_pullup_en(PIN_LORA_MISO);       /* reads 0xff, not noise, if the chip is absent */
    spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = 8 * 1000 * 1000,
        .spics_io_num = PIN_LORA_SS,
        .queue_size = 2,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));

    hw_reset();
    uint8_t v[4] = {0};
    esp_err_t err = cmd_read(OP_GET_VERSION, NULL, 0, v, 4);
    snprintf(st->chip, sizeof(st->chip), "LR1121 hw 0x%02x type 0x%02x fw %02x.%02x", v[0], v[1], v[2], v[3]);
    get_status(NULL);
    bool all_same = (v[0] == v[1] && v[1] == v[2] && v[2] == v[3]);
    if (err != ESP_OK || all_same) {
        ESP_LOGE(TAG, "LR1121 not found (GetVersion %02x %02x %02x %02x, %s)", v[0], v[1], v[2], v[3],
                 esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }
    if (!(s_stat2 & 1)) ESP_LOGW(TAG, "LR1121 is in its bootloader (no transceiver firmware running)");
    if (v[1] != 0x03) ESP_LOGW(TAG, "use case 0x%02x (LR1121 is 0x03; ExpressLRS firmware reports 0xf3)", v[1]);
    ESP_LOGI(TAG, "%s, reset status %u", st->chip, s_stat2 >> 4);
    chip_setup();
    return ESP_OK;
}

/* Regulator, RF switch and calibration; needed after every reset. */
static void chip_setup(void)
{
    clear_errors();
    set_standby_rc();
#if BOARD_RADIO_DCDC
    uint8_t regmode = 1;
    cmd_write(OP_SET_REGMODE, &regmode, 1);
#endif
    /* RF switch DIO5..8: enable, standby, rx, tx, tx_hp, tx_hf, gnss, wifi (ExpressLRS default table) */
    static const uint8_t rfsw[8] = {0x0f, 0x00, 0x04, 0x08, 0x08, 0x02, 0x00, 0x01};
    cmd_write(OP_SET_DIO_RF_SWITCH, rfsw, sizeof(rfsw));
    uint8_t fb = 0x01;                   /* fall back to standby RC */
    cmd_write(OP_SET_FALLBACK, &fb, 1);

    /* Full calibration after the regulator change. */
    uint8_t cal = 0x3f;
    cmd_write(OP_CALIBRATE, &cal, 1);
    if (!wait_busy(200)) ESP_LOGW(TAG, "calibration timed out");
    uint16_t errors = get_errors();
    if (errors) ESP_LOGW(TAG, "errors after calibration: 0x%04x", errors);
    clear_errors();
}

void hw_start_irq(void)
{
    gpio_set_intr_type(PIN_LORA_DIO, GPIO_INTR_POSEDGE);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_LORA_DIO, dio_isr, NULL);
}

esp_err_t hw_tx_ford(const radio_config_t *cfg, const uint8_t payload[8], int count, int gap_ms, bool pa_boost,
                     int power_dbm)
{
    /* pa_boost selects the high-power PA (-9..22 dBm), else the low-power PA (-17..14 dBm). */
    if (pa_boost ? (power_dbm < -9 || power_dbm > 22) : (power_dbm < -17 || power_dbm > 14))
        return ESP_ERR_INVALID_ARG;
    uint8_t frame[TPMS_FORD_FRAME_BYTES];
    tpms_ford_encode(payload, frame);

    set_standby_rc();
    clear_errors();
    uint8_t pt = PKT_TYPE_GFSK;
    cmd_write(OP_SET_PKT_TYPE, &pt, 1);
    uint8_t mod[10];
    put32(mod, cfg->bitrate);
    mod[4] = 0x00;
    mod[5] = bw_code(cfg->rxbw_hz, NULL);
    put32(mod + 6, 35000);               /* +/-35 kHz like the sensors */
    cmd_write(OP_SET_MOD_PARAMS, mod, sizeof(mod));
    /* 16 preamble bits, then the frame's own 55 55 55 56; no sync word, CRC or whitening. */
    uint8_t pkt[9] = {0, 16, 0x00, 0, 0x00, 0x00, sizeof(frame), 0x01, 0x00};
    cmd_write(OP_SET_PKT_PARAMS, pkt, sizeof(pkt));
    set_rf_freq(cfg->freq_hz);
    /* PA settings from the ExpressLRS LR1121 driver. */
    uint8_t pa_hp[4] = {0x01, 0x01, 0x04, 0x07}, pa_lp[4] = {0x00, 0x00, 0x07, 0x00};
    cmd_write(OP_SET_PA_CFG, pa_boost ? pa_hp : pa_lp, 4);
    uint8_t txp[2] = {(uint8_t)(int8_t)power_dbm, 0x02 /* 48 us ramp */};
    cmd_write(OP_SET_TX_PARAMS, txp, 2);
    uint8_t irq[8] = {0};                /* polled: keep TX_DONE off the IRQ line */
    cmd_write(OP_SET_DIO_IRQ, irq, sizeof(irq));
    clear_irq(0xffffffff);

    esp_err_t err = ESP_OK;
    for (int i = 0; i < count && err == ESP_OK; i++) {
        cmd_write(OP_WRITE_BUFFER8, frame, sizeof(frame));
        uint8_t t[3] = {0, 0, 0};
        cmd_write(OP_SET_TX, t, 3);
        int64_t t0 = esp_timer_get_time();
        uint32_t st = 0;
        while (!(st & IRQ_TX_DONE)) {
            if (esp_timer_get_time() - t0 > 100000) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            esp_rom_delay_us(200);
            get_status(&st);
        }
        uint16_t errors = get_errors();
        if (errors & ERR_PLL_LOCK) {
            ESP_LOGE(TAG, "TX PLL lock error at %.3f MHz", cfg->freq_hz / 1e6);
            err = ESP_ERR_INVALID_STATE;
        }
        clear_irq(0xffffffff);
        if (i + 1 < count && gap_ms) vTaskDelay(pdMS_TO_TICKS(gap_ms));
    }
    set_standby_rc();
    /* The caller re-applies the RX configuration. */
    return err;
}

/* ------------------------------------------------------- diagnostics --- */

void hw_dump(FILE *out)
{
    uint8_t v[4] = {0}, b[8] = {0};
    uint32_t irq = 0;
    cmd_read(OP_GET_VERSION, NULL, 0, v, 4);
    fprintf(out, "version       hw 0x%02x, use case 0x%02x, fw %02x.%02x\n", v[0], v[1], v[2], v[3]);
    get_status(&irq);
    fprintf(out, "stat1/stat2   0x%02x / 0x%02x: mode %s, %s, reset status %u, last cmd status %d\n", s_stat1,
            s_stat2, mode_name(chip_mode()), (s_stat2 & 1) ? "running from flash" : "BOOTLOADER", s_stat2 >> 4,
            cmd_status());
    fprintf(out, "irq status    0x%08x (DIO pin %d)\n", (unsigned)irq, gpio_get_level(PIN_LORA_DIO));
    uint16_t errors = get_errors();
    fprintf(out, "errors        0x%04x%s%s%s%s (last check 0x%04x)\n", errors, errors & ERR_PLL_LOCK ? " PLL_LOCK" : "",
            errors & ERR_PLL_CALIB ? " PLL_CALIB" : "", errors & ERR_IMG_CALIB ? " IMG_CALIB" : "",
            errors & ERR_HF_XOSC_START ? " HF_XOSC_START" : "", s_last_errors);
    fprintf(out, "pll           %s, RX re-entered %u times\n", s_locked ? "locked" : "NOT LOCKED",
            (unsigned)s_rx_restarts);
    fprintf(out, "rssi now      %d dBm\n", rssi_inst());
    if (cmd_read(OP_GET_STATS, NULL, 0, b, 8) == ESP_OK)
        fprintf(out, "gfsk stats    %u received, %u crc err, %u length err\n", b[0] << 8 | b[1], b[2] << 8 | b[3],
                b[4] << 8 | b[5]);
    if (cmd_read(OP_GET_RXBUF_STATUS, NULL, 0, b, 2) == ESP_OK)
        fprintf(out, "rx buffer     last length %u at offset %u\n", b[0], b[1]);
    if (cmd_read(OP_GET_PKT_STATUS, NULL, 0, b, 4) == ESP_OK)
        fprintf(out, "last packet   rssi sync %d, avg %d dBm, len %u, status 0x%02x\n", rssi_from_raw(b[0]),
                rssi_from_raw(b[1]), b[2], b[3]);
    if (cmd_read(OP_GET_TEMP, NULL, 0, b, 2) == ESP_OK) {
        int raw = (b[0] << 8 | b[1]) & 0x7ff;
        fprintf(out, "temperature   %.1f C\n", 25 + (1000 / -1.7) * ((raw / 2047.0) * 1.35 - 0.7295));
    }
    if (cmd_read(OP_GET_VBAT, NULL, 0, b, 1) == ESP_OK)
        fprintf(out, "vbat          %.2f V\n", ((5.0 * b[0]) / 255 - 1) * 1.35);
}

void hw_probe(FILE *out)
{
    gpio_intr_disable(PIN_LORA_DIO);

    /* RESET + BUSY: BUSY must go high when reset is released, then low when the chip is ready. */
    gpio_set_level(PIN_LORA_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    int busy_in_reset = gpio_get_level(PIN_LORA_BUSY);
    gpio_set_level(PIN_LORA_RST, 1);
    esp_rom_delay_us(500);
    int busy_after = gpio_get_level(PIN_LORA_BUSY);
    int64_t t0 = esp_timer_get_time();
    bool ready = wait_busy(500);
    int boot_ms = (int)((esp_timer_get_time() - t0) / 1000);
    fprintf(out, "RESET GPIO%d / BUSY GPIO%d: busy %d in reset, %d after release, %s after %d ms -> %s\n",
            PIN_LORA_RST, PIN_LORA_BUSY, busy_in_reset, busy_after, ready ? "low" : "STILL HIGH", boot_ms,
            (busy_after && ready) ? "OK" : "CHECK WIRING");

    uint8_t v[4] = {0};
    esp_err_t err = cmd_read(OP_GET_VERSION, NULL, 0, v, 4);
    fprintf(out, "SPI           GetVersion %02x %02x %02x %02x (%s)\n", v[0], v[1], v[2], v[3], esp_err_to_name(err));
    chip_setup();

    /* IRQ line: arm the RX timeout IRQ only and let a 1 ms RX window expire. */
    bool locked;
    if (s_cfg.freq_hz) hw_apply(&s_cfg, &locked);   /* not yet configured if the chip wasn't found */
    set_standby_rc();
    uint8_t irq[8] = {0};
    put32(irq, IRQ_TIMEOUT);
    cmd_write(OP_SET_DIO_IRQ, irq, sizeof(irq));
    clear_irq(0xffffffff);
    get_status(NULL);
    int dio_idle = gpio_get_level(PIN_LORA_DIO);
    uint8_t rx[3] = {0, 0, 33};          /* 33 x 30.5 us */
    cmd_write(OP_SET_RX, rx, 3);
    vTaskDelay(pdMS_TO_TICKS(10));
    uint32_t status = 0;
    get_status(&status);
    int dio_fired = gpio_get_level(PIN_LORA_DIO);
    clear_irq(0xffffffff);
    esp_rom_delay_us(50);
    int dio_cleared = gpio_get_level(PIN_LORA_DIO);
    fprintf(out, "IRQ GPIO%d     idle %d, after RX timeout %d (irq 0x%08x), after clear %d -> %s\n", PIN_LORA_DIO,
            dio_idle, dio_fired, (unsigned)status, dio_cleared,
            (!dio_idle && dio_fired && !dio_cleared) ? "WIRED" : "NOT WIRED?");
    gpio_intr_enable(PIN_LORA_DIO);
    /* radio.c re-applies the RX configuration (and the IRQ mask) after this. */
}
