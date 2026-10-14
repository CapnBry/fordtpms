#include "radio.h"

#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"

static const char *TAG = "radio";

/* SX1278 FSK registers */
#define REG_FIFO          0x00
#define REG_OPMODE        0x01
#define REG_BITRATE_MSB   0x02
#define REG_BITRATE_LSB   0x03
#define REG_FRF_MSB       0x06
#define REG_FRF_MID       0x07
#define REG_FRF_LSB       0x08
#define REG_LNA           0x0C
#define REG_RXCONFIG      0x0D
#define REG_RSSICONFIG    0x0E
#define REG_RSSITHRESH    0x10
#define REG_RSSIVALUE     0x11
#define REG_RXBW          0x12
#define REG_AFCBW         0x13
#define REG_OOKPEAK       0x14
#define REG_PREAMBLEDETECT 0x1F
#define REG_OSC           0x24
#define REG_SYNCCONFIG    0x27
#define REG_SYNCVALUE1    0x28
#define REG_SYNCVALUE8    0x2F
#define REG_PACKETCONFIG1 0x30
#define REG_PACKETCONFIG2 0x31
#define REG_PAYLOADLENGTH 0x32
#define REG_FIFOTHRESH    0x35
#define REG_IMAGECAL      0x3B
#define REG_IRQFLAGS1     0x3E
#define REG_IRQFLAGS2     0x3F
#define REG_DIOMAPPING1   0x40
#define REG_DIOMAPPING2   0x41
#define REG_VERSION       0x42

#define OPMODE_LF         0x08  /* FSK, low frequency (137-525 MHz) registers */
#define MODE_SLEEP        0x00
#define MODE_STDBY        0x01
#define MODE_RX           0x05

#define IRQ1_MODEREADY    0x80
#define IRQ1_SYNCMATCH    0x01
#define IRQ2_FIFOOVERRUN  0x10
#define IRQ2_PAYLOADREADY 0x04

#define RXCONFIG_RESTART  0x40  /* RestartRxWithoutPllLock */

#define FXOSC 32000000ULL

#define NOTIFY_DIO0 (1u << 0)
#define NOTIFY_DIO2 (1u << 1)

#define HIST_PERIOD_MS 250
#define HIST_LEN 80

static spi_device_handle_t s_spi;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static radio_packet_cb_t s_cb;
static radio_config_t s_cfg = {
    .freq_hz = 315000000,
    .bitrate = 19231,      /* 52 us chips */
    .rxbw_hz = 83333,      /* +/-83 kHz: ~35 kHz deviation + sensor crystal error */
    .lna_gain = 1,
    .sync = RADIO_SYNC_5556,
};
static radio_stats_t s_stats;
static bool s_raw_log;

static int8_t s_hist[HIST_LEN];
static int s_hist_head, s_hist_count;

/* ------------------------------------------------------------- SPI ----- */

static uint8_t rd(uint8_t reg)
{
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_RXDATA | SPI_TRANS_USE_TXDATA,
        .addr = reg & 0x7f,
        .length = 8,
        .rxlength = 8,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
    return t.rx_data[0];
}

static void wr(uint8_t reg, uint8_t val)
{
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA,
        .addr = reg | 0x80,
        .length = 8,
        .tx_data = {val},
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void rd_burst(uint8_t reg, uint8_t *buf, size_t n)
{
    spi_transaction_t t = {
        .addr = reg & 0x7f,
        .length = n * 8,
        .rxlength = n * 8,
        .rx_buffer = buf,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void set_mode(uint8_t mode)
{
    wr(REG_OPMODE, OPMODE_LF | mode);
    if (mode == MODE_SLEEP) return;
    for (int i = 0; i < 100 && !(rd(REG_IRQFLAGS1) & IRQ1_MODEREADY); i++) {
        esp_rom_delay_us(100);
    }
}

static int rssi_dbm(void)
{
    return -(int)rd(REG_RSSIVALUE) / 2;
}

static void hw_reset(void)
{
    gpio_set_level(PIN_LORA_RST, 0);
    esp_rom_delay_us(200);
    gpio_set_level(PIN_LORA_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* --------------------------------------------------------- config ------ */

static uint8_t rxbw_code(uint32_t hz, uint32_t *actual)
{
    static const uint8_t mant[3] = {16, 20, 24};
    uint8_t best = 0;
    uint32_t best_hz = 0, best_err = UINT32_MAX;
    for (int e = 1; e <= 7; e++) {
        for (int m = 0; m < 3; m++) {
            uint32_t bw = (uint32_t)(FXOSC / (mant[m] * (1u << (e + 2))));
            uint32_t err = bw > hz ? bw - hz : hz - bw;
            if (err < best_err) {
                best_err = err;
                best = (uint8_t)(m << 3 | e);
                best_hz = bw;
            }
        }
    }
    if (actual) *actual = best_hz;
    return best;
}

static int payload_len(radio_sync_mode_t m)
{
    return m == RADIO_SYNC_5555 ? 24 : TPMS_FORD_CHIP_BYTES;
}

/* Must hold s_lock. */
static esp_err_t apply_config(void)
{
    set_mode(MODE_SLEEP);
    set_mode(MODE_STDBY);

    uint32_t br = (uint32_t)(FXOSC / s_cfg.bitrate);
    wr(REG_BITRATE_MSB, br >> 8);
    wr(REG_BITRATE_LSB, br & 0xff);

    uint64_t frf = ((uint64_t)s_cfg.freq_hz << 19) / FXOSC;
    wr(REG_FRF_MSB, (frf >> 16) & 0xff);
    wr(REG_FRF_MID, (frf >> 8) & 0xff);
    wr(REG_FRF_LSB, frf & 0xff);

    /* Fixed LNA gain: AGC re-triggering mid-packet would corrupt the short
     * (~32 chip) preamble, and TPMS signals are weak anyway. */
    wr(REG_LNA, (uint8_t)((s_cfg.lna_gain & 7) << 5));
    wr(REG_RXCONFIG, 0x00);          /* no AFC/AGC, no RX trigger */
    wr(REG_RSSICONFIG, 0x02);        /* 8 sample smoothing */
    wr(REG_RSSITHRESH, 0xff);
    uint8_t bw = rxbw_code(s_cfg.rxbw_hz, NULL);
    wr(REG_RXBW, bw);
    wr(REG_AFCBW, bw);
    wr(REG_OOKPEAK, 0x28);           /* BitSyncOn */
    wr(REG_PREAMBLEDETECT, 0x00);
    wr(REG_OSC, 0x07);               /* ClkOut off */

    uint8_t sync[2];
    switch (s_cfg.sync) {
    case RADIO_SYNC_AAA9: sync[0] = 0xaa; sync[1] = 0xa9; break;
    case RADIO_SYNC_5555: sync[0] = 0x55; sync[1] = 0x55; break;
    default:              sync[0] = 0x55; sync[1] = 0x56; break;
    }
    wr(REG_SYNCCONFIG, 0x10 | (sizeof(sync) - 1));  /* SyncOn, no auto restart */
    wr(REG_SYNCVALUE1, sync[0]);
    wr(REG_SYNCVALUE1 + 1, sync[1]);
    wr(REG_PACKETCONFIG1, 0x00);     /* fixed length, no DC-free, no CRC */
    wr(REG_PACKETCONFIG2, 0x40);     /* packet mode */
    wr(REG_PAYLOADLENGTH, (uint8_t)payload_len(s_cfg.sync));
    wr(REG_FIFOTHRESH, 0x80 | 0x0f);
    wr(REG_DIOMAPPING1, 0x0C);       /* DIO0=PayloadReady, DIO2=SyncAddress */
    wr(REG_DIOMAPPING2, 0x00);

    /* Image (RX IQ) calibration for the new frequency; default is 434 MHz. */
    wr(REG_IMAGECAL, (rd(REG_IMAGECAL) & ~0x40) | 0x40);
    for (int i = 0; i < 100 && (rd(REG_IMAGECAL) & 0x20); i++) {
        vTaskDelay(1);
    }

    set_mode(MODE_RX);
    return ESP_OK;
}

static void restart_rx(void)
{
    wr(REG_RXCONFIG, RXCONFIG_RESTART);
}

/* --------------------------------------------------------- receive ----- */

static void IRAM_ATTR dio_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    xTaskNotifyFromISR(s_task, (uint32_t)(uintptr_t)arg, eSetBits, &woken);
    portYIELD_FROM_ISR(woken);
}

static void hist_push(int dbm)
{
    if (dbm < -128) dbm = -128;
    if (dbm > 0) dbm = 0;
    s_hist[s_hist_head] = (int8_t)dbm;
    s_hist_head = (s_hist_head + 1) % HIST_LEN;
    if (s_hist_count < HIST_LEN) s_hist_count++;
}

static void handle_payload(int rssi, uint32_t sync_ms)
{
    uint8_t buf[32];
    int n = payload_len(s_cfg.sync);
    rd_burst(REG_FIFO, buf, n);
    restart_rx();
    s_stats.payloads++;

    tpms_ford_t pkt;
    tpms_ford_status_t st;
    switch (s_cfg.sync) {
    case RADIO_SYNC_AAA9: st = tpms_ford_decode_chips(buf, n * 8, 0, true, &pkt); break;
    case RADIO_SYNC_5555: st = tpms_ford_search(buf, n * 8, &pkt); break;
    default:              st = tpms_ford_decode_chips(buf, n * 8, 0, false, &pkt); break;
    }

    if (s_raw_log || st == TPMS_FORD_OK) {
        char hex[2 * sizeof(buf) + 1];
        for (int i = 0; i < n; i++) sprintf(hex + 2 * i, "%02x", buf[i]);
        if (s_raw_log) ESP_LOGI(TAG, "raw %s rssi=%d -> %s", hex, rssi, tpms_ford_status_str(st));
        else ESP_LOGD(TAG, "raw %s rssi=%d", hex, rssi);
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
    bool in_packet = false;
    int pkt_rssi = 0;
    uint32_t sync_ms = 0;
    uint32_t hist_t0 = 0;
    int hist_max = -128, noise_acc = 0, noise_n = 0;
    int noise_ema_x16 = -120 * 16;

    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(5));
        if (bits & NOTIFY_DIO0) s_stats.irq_dio0++;
        if (bits & NOTIFY_DIO2) s_stats.irq_dio2++;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        uint8_t f1 = rd(REG_IRQFLAGS1);
        uint8_t f2 = rd(REG_IRQFLAGS2);

        if ((f1 & IRQ1_SYNCMATCH) && !in_packet) {
            /* Latch RSSI while the packet is on the air. */
            pkt_rssi = rssi_dbm();
            s_stats.last_rssi_dbm = (int16_t)pkt_rssi;
            sync_ms = now;
            in_packet = true;
            s_stats.syncs++;
            if (!(bits & NOTIFY_DIO2)) s_stats.polled_events++;
        }

        if (f2 & IRQ2_PAYLOADREADY) {
            if (!(bits & NOTIFY_DIO0)) s_stats.polled_events++;
            in_packet = false;
            handle_payload(pkt_rssi, sync_ms);
        } else if (f2 & IRQ2_FIFOOVERRUN) {
            s_stats.fifo_overruns++;
            wr(REG_IRQFLAGS2, IRQ2_FIFOOVERRUN);
            restart_rx();
            in_packet = false;
        } else if (in_packet && now - sync_ms > 50) {
            /* A 16 byte payload takes ~7 ms; don't stay stuck. */
            restart_rx();
            in_packet = false;
        } else if (!in_packet) {
            int r = rssi_dbm();
            if (r > hist_max) hist_max = r;
            noise_acc += r;
            noise_n++;
        }

        if (now - hist_t0 >= HIST_PERIOD_MS) {
            if (noise_n) {
                hist_push(hist_max);
                noise_ema_x16 += (noise_acc * 16 / noise_n - noise_ema_x16) / 4;
                s_stats.noise_dbm = (int16_t)(noise_ema_x16 / 16);
            }
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

    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << PIN_LORA_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst);
    gpio_set_level(PIN_LORA_RST, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_LORA_MOSI,
        .miso_io_num = PIN_LORA_MISO,
        .sclk_io_num = PIN_LORA_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_DISABLED));
    spi_device_interface_config_t dev = {
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = 8 * 1000 * 1000,
        .spics_io_num = PIN_LORA_SS,
        .queue_size = 2,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev, &s_spi));

    hw_reset();
    s_stats.version = rd(REG_VERSION);
    if (s_stats.version != 0x12) {
        ESP_LOGE(TAG, "SX1278 not found (RegVersion=0x%02x, expected 0x12)", s_stats.version);
        return ESP_ERR_NOT_FOUND;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    apply_config();
    xSemaphoreGive(s_lock);

    xTaskCreatePinnedToCore(radio_task, "radio", 4096, NULL, 10, &s_task, 1);

    gpio_config_t dio = {
        .pin_bit_mask = (1ULL << PIN_LORA_DIO0) | (1ULL << PIN_LORA_DIO2),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&dio);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_LORA_DIO0, dio_isr, (void *)NOTIFY_DIO0);
    gpio_isr_handler_add(PIN_LORA_DIO2, dio_isr, (void *)NOTIFY_DIO2);

    uint32_t bw;
    rxbw_code(s_cfg.rxbw_hz, &bw);
    ESP_LOGI(TAG, "SX1278 v0x%02x RX %.3f MHz, %u chip/s, RxBw %u Hz, LNA G%u, sync %s",
             s_stats.version, s_cfg.freq_hz / 1e6, (unsigned)s_cfg.bitrate, (unsigned)bw,
             s_cfg.lna_gain, s_cfg.sync == RADIO_SYNC_5556 ? "5556" : s_cfg.sync == RADIO_SYNC_AAA9 ? "aaa9" : "5555");
    return ESP_OK;
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
    uint8_t v = s_stats.version;
    int16_t noise = s_stats.noise_dbm;
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.version = v;
    s_stats.noise_dbm = noise;
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
    if (cfg->freq_hz < 137000000 || cfg->freq_hz > 525000000) return ESP_ERR_INVALID_ARG;
    if (cfg->lna_gain < 1 || cfg->lna_gain > 6) return ESP_ERR_INVALID_ARG;
    if (cfg->bitrate < 1200 || cfg->bitrate > 300000) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    rxbw_code(cfg->rxbw_hz, &s_cfg.rxbw_hz);  /* store the bandwidth the chip actually uses */
    esp_err_t err = apply_config();
    radio_config_t now = s_cfg;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "reconfigured: %.3f MHz, %u chip/s, RxBw %u Hz, LNA G%u", now.freq_hz / 1e6,
             (unsigned)now.bitrate, (unsigned)now.rxbw_hz, now.lna_gain);
    return err;
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

void radio_set_raw_log(bool on)
{
    s_raw_log = on;
}

void radio_dump_regs(FILE *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int r = 1; r < 0x80; r++) {
        if ((r - 1) % 16 == 0) fprintf(out, "%02x:", r);
        fprintf(out, " %02x", rd((uint8_t)r));
        if (r % 16 == 0 || r == 0x7f) fprintf(out, "\n");
    }
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------ probe ---- */

static const int probe_pins[] = {0, 2, 4, 12, 13, 14, 15, 26, 32, 33, 34, 36, 39};
#define NPROBE (sizeof(probe_pins) / sizeof(probe_pins[0]))

static inline uint64_t read_inputs(void)
{
    return (uint64_t)REG_READ(GPIO_IN_REG) | ((uint64_t)(REG_READ(GPIO_IN1_REG) & 0xff) << 32);
}

static void sample(int ms, uint32_t edges[NPROBE], int level[NPROBE])
{
    uint64_t prev = read_inputs();
    int64_t end = esp_timer_get_time() + ms * 1000;
    memset(edges, 0, sizeof(uint32_t) * NPROBE);
    while (esp_timer_get_time() < end) {
        uint64_t cur = read_inputs();
        uint64_t diff = cur ^ prev;
        if (diff) {
            for (int i = 0; i < NPROBE; i++)
                if (diff & (1ULL << probe_pins[i])) edges[i]++;
        }
        prev = cur;
    }
    for (int i = 0; i < NPROBE; i++) level[i] = (prev >> probe_pins[i]) & 1;
}

void radio_probe(FILE *out)
{
    uint32_t e_sb[NPROBE], e_rx[NPROBE];
    int l_sb[NPROBE], l_rx[NPROBE];

    for (int i = 0; i < NPROBE; i++) {
        gpio_intr_disable(probe_pins[i]);
        gpio_input_enable(probe_pins[i]);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Continuous mode: DIO0=RxReady, DIO1=DCLK, DIO2=DATA */
    set_mode(MODE_STDBY);
    wr(REG_PACKETCONFIG2, 0x00);
    wr(REG_DIOMAPPING1, 0x80);
    vTaskDelay(pdMS_TO_TICKS(5));
    sample(20, e_sb, l_sb);
    set_mode(MODE_RX);
    vTaskDelay(pdMS_TO_TICKS(5));
    sample(50, e_rx, l_rx);

    /* Reset line test: change a register, pulse RST, see if it reverts. */
    set_mode(MODE_STDBY);
    wr(REG_SYNCVALUE8, 0xa5);
    uint8_t before = rd(REG_SYNCVALUE8);
    hw_reset();
    uint8_t after = rd(REG_SYNCVALUE8);
    apply_config();
    xSemaphoreGive(s_lock);

    gpio_intr_enable(PIN_LORA_DIO0);
    gpio_intr_enable(PIN_LORA_DIO2);

    fprintf(out, "SX1278 continuous mode, DIO0=RxReady DIO1=DCLK DIO2=DATA\n");
    fprintf(out, "GPIO  standby(lvl,edges/20ms)  rx(lvl,edges/50ms)  guess\n");
    for (int i = 0; i < NPROBE; i++) {
        const char *guess = "";
        if (l_sb[i] == 0 && l_rx[i] == 1 && e_rx[i] < 4) guess = "DIO0 (RxReady)";
        else if (e_sb[i] == 0 && e_rx[i] > 1600 && e_rx[i] < 2300) guess = "DIO1 (DCLK ~19.2 kHz)";
        else if (e_sb[i] == 0 && e_rx[i] > 50) guess = "DIO2 (DATA)?";
        fprintf(out, "%4d  %d %6u                  %d %6u            %s\n", probe_pins[i], l_sb[i],
                (unsigned)e_sb[i], l_rx[i], (unsigned)e_rx[i], guess);
    }
    fprintf(out, "RESET on GPIO%d: reg 0x2f wrote 0xa5, read 0x%02x, after reset 0x%02x -> %s\n", PIN_LORA_RST,
            before, after, (before == 0xa5 && after != 0xa5) ? "WIRED" : "NOT WIRED?");
}
