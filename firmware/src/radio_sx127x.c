/*
 * SX1278 backend: FSK packet mode. DIO0 fires on PayloadReady, DIO2 on
 * SyncAddress (RSSI is latched there); the radio task also polls the IRQ
 * flags every ~5 ms in case a DIO isn't wired.
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
#include "soc/gpio_reg.h"
#include "soc/soc.h"

static const char *TAG = "sx1278";

/* SX1278 FSK registers */
#define REG_FIFO          0x00
#define REG_OPMODE        0x01
#define REG_BITRATE_MSB   0x02
#define REG_BITRATE_LSB   0x03
#define REG_FDEV_MSB      0x04
#define REG_FDEV_LSB      0x05
#define REG_FRF_MSB       0x06
#define REG_FRF_MID       0x07
#define REG_FRF_LSB       0x08
#define REG_PACONFIG      0x09
#define REG_LNA           0x0C
#define REG_RXCONFIG      0x0D
#define REG_RSSICONFIG    0x0E
#define REG_RSSITHRESH    0x10
#define REG_RSSIVALUE     0x11
#define REG_RXBW          0x12
#define REG_AFCBW         0x13
#define REG_OOKPEAK       0x14
#define REG_PREAMBLEDETECT 0x1F
#define REG_PREAMBLE_MSB  0x25
#define REG_PREAMBLE_LSB  0x26
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
#define MODE_FSRX         0x04
#define MODE_TX           0x03
#define MODE_RX           0x05

#define IRQ1_MODEREADY    0x80
#define IRQ1_PLLLOCK      0x10
#define IRQ1_SYNCMATCH    0x01
#define IRQ2_FIFOOVERRUN  0x10
#define IRQ2_PAYLOADREADY 0x04
#define IRQ2_PACKETSENT   0x08

#define RXCONFIG_RESTART  0x40  /* RestartRxWithoutPllLock */

#define FXOSC 32000000ULL

const radio_caps_t radio_hw_caps = {
    .name = "SX1278",
    .freq_min_hz = 137000000,
    .freq_max_hz = 525000000,
    .freq_note = "specified 137-175 and 410-525 MHz",
    .lna_gain = true,
    .tx = true,
    .tx_default_dbm = 2,
    .tx_note = "pa 2..17 dBm (PA_BOOST) | rfo 0..15 dBm (RFO)",
};

static spi_device_handle_t s_spi;
static int s_payload_len = TPMS_FORD_CHIP_BYTES;
static bool s_in_packet;
static uint32_t s_sync_ms;

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

static void wr_burst(uint8_t reg, const uint8_t *buf, size_t n)
{
    spi_transaction_t t = {
        .addr = reg | 0x80,
        .length = n * 8,
        .tx_buffer = buf,
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

static void restart_rx(void)
{
    wr(REG_RXCONFIG, RXCONFIG_RESTART);
}

static void set_frf(uint32_t freq_hz)
{
    uint64_t frf = ((uint64_t)freq_hz << 19) / FXOSC;
    wr(REG_FRF_MSB, (frf >> 16) & 0xff);
    wr(REG_FRF_MID, (frf >> 8) & 0xff);
    wr(REG_FRF_LSB, frf & 0xff);
}

/* Lock takes well under 1 ms when it is possible. */
static bool wait_lock(void)
{
    bool locked = false;
    for (int i = 0; i < 20 && !(locked = rd(REG_IRQFLAGS1) & IRQ1_PLLLOCK); i++) {
        esp_rom_delay_us(250);
    }
    return locked;
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

uint32_t hw_rxbw(uint32_t hz)
{
    uint32_t actual;
    rxbw_code(hz, &actual);
    return actual;
}

int hw_bandwidths(uint32_t *out, int max)
{
    static const uint8_t mant[3] = {16, 20, 24};
    int n = 0;
    for (int e = 7; e >= 1; e--)
        for (int m = 2; m >= 0 && n < max; m--) out[n++] = (uint32_t)(FXOSC / (mant[m] * (1u << (e + 2))));
    return n;
}

esp_err_t hw_apply(const radio_config_t *cfg, bool *locked)
{
    set_mode(MODE_SLEEP);
    set_mode(MODE_STDBY);

    uint32_t br = (uint32_t)(FXOSC / cfg->bitrate);
    wr(REG_BITRATE_MSB, br >> 8);
    wr(REG_BITRATE_LSB, br & 0xff);

    set_frf(cfg->freq_hz);

    /* Fixed LNA gain: AGC re-triggering mid-packet would corrupt the short
     * (~32 chip) preamble, and TPMS signals are weak anyway. */
    wr(REG_LNA, (uint8_t)((cfg->lna_gain & 7) << 5));
    wr(REG_RXCONFIG, 0x00);          /* no AFC/AGC, no RX trigger */
    wr(REG_RSSICONFIG, 0x02);        /* 8 sample smoothing */
    wr(REG_RSSITHRESH, 0xff);
    uint8_t bw = rxbw_code(cfg->rxbw_hz, NULL);
    wr(REG_RXBW, bw);
    wr(REG_AFCBW, bw);
    wr(REG_OOKPEAK, 0x28);           /* BitSyncOn */
    wr(REG_PREAMBLEDETECT, 0x00);
    wr(REG_OSC, 0x07);               /* ClkOut off */

    uint8_t sync[2];
    radio_sync_bytes(cfg->sync, sync);
    wr(REG_SYNCCONFIG, 0x10 | (sizeof(sync) - 1));  /* SyncOn, no auto restart */
    wr(REG_SYNCVALUE1, sync[0]);
    wr(REG_SYNCVALUE1 + 1, sync[1]);
    wr(REG_PACKETCONFIG1, 0x00);     /* fixed length, no DC-free, no CRC */
    wr(REG_PACKETCONFIG2, 0x40);     /* packet mode */
    s_payload_len = radio_payload_len(cfg->sync);
    wr(REG_PAYLOADLENGTH, (uint8_t)s_payload_len);
    wr(REG_FIFOTHRESH, 0x80 | 0x0f);
    wr(REG_DIOMAPPING1, 0x0C);       /* DIO0=PayloadReady, DIO2=SyncAddress */
    wr(REG_DIOMAPPING2, 0x00);

    /* Image (RX IQ) calibration for the new frequency; default is 434 MHz. */
    wr(REG_IMAGECAL, (rd(REG_IMAGECAL) & ~0x40) | 0x40);
    for (int i = 0; i < 100 && (rd(REG_IMAGECAL) & 0x20); i++) {
        vTaskDelay(1);
    }

    set_mode(MODE_RX);
    s_in_packet = false;

    /* The SX1278 is only specified for 137-175 and 410-525 MHz. In between,
     * the synthesizer may not lock (e.g. 315 MHz: no lock from ~200 to
     * ~355 MHz on the test board) and the receiver then hears nothing at all,
     * silently. */
    *locked = wait_lock();
    return ESP_OK;
}

bool hw_pll_test(uint32_t freq_hz, char *detail, size_t detail_len)
{
    set_mode(MODE_STDBY);
    set_frf(freq_hz);
    set_mode(MODE_FSRX);
    bool locked = wait_lock();
    snprintf(detail, detail_len, "RegIrqFlags1 0x%02x", rd(REG_IRQFLAGS1));
    set_mode(MODE_STDBY);
    return locked;
}

/* --------------------------------------------------------- receive ----- */

static void IRAM_ATTR dio_isr(void *arg)
{
    radio_isr_notify((uint32_t)(uintptr_t)arg);
}

void hw_poll(uint32_t bits, uint32_t now, radio_hw_poll_t *o, radio_stats_t *st)
{
    if (bits & RADIO_NOTIFY_PAYLOAD) st->irq_payload++;
    if (bits & RADIO_NOTIFY_SYNC) st->irq_sync++;

    uint8_t f1 = rd(REG_IRQFLAGS1);
    uint8_t f2 = rd(REG_IRQFLAGS2);

    o->locked = f1 & IRQ1_PLLLOCK;
    if (!o->locked) st->pll_unlocked_polls++;

    if ((f1 & IRQ1_SYNCMATCH) && !s_in_packet) {
        o->sync = true;
        o->sync_rssi_dbm = rssi_dbm();
        s_sync_ms = now;
        s_in_packet = true;
        if (!(bits & RADIO_NOTIFY_SYNC)) st->polled_events++;
    }

    if (f2 & IRQ2_PAYLOADREADY) {
        if (!(bits & RADIO_NOTIFY_PAYLOAD)) st->polled_events++;
        s_in_packet = false;
        rd_burst(REG_FIFO, o->payload, s_payload_len);
        restart_rx();
        o->payload_len = s_payload_len;
    } else if (f2 & IRQ2_FIFOOVERRUN) {
        st->fifo_overruns++;
        wr(REG_IRQFLAGS2, IRQ2_FIFOOVERRUN);
        restart_rx();
        s_in_packet = false;
    } else if (s_in_packet && now - s_sync_ms > 50) {
        /* A 16 byte payload takes ~7 ms; don't stay stuck. */
        restart_rx();
        s_in_packet = false;
    } else if (!s_in_packet) {
        o->idle = true;
        o->idle_rssi_dbm = rssi_dbm();
    }
}

/* ------------------------------------------------------------ init ----- */

esp_err_t hw_init(radio_stats_t *st)
{
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
    uint8_t version = rd(REG_VERSION);
    snprintf(st->chip, sizeof(st->chip), "SX1278 v0x%02x", version);
    if (version != 0x12) {
        ESP_LOGE(TAG, "SX1278 not found (RegVersion=0x%02x, expected 0x12)", version);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

void hw_start_irq(void)
{
    gpio_config_t dio = {
        .pin_bit_mask = (1ULL << PIN_LORA_DIO0) | (1ULL << PIN_LORA_DIO2),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&dio);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_LORA_DIO0, dio_isr, (void *)RADIO_NOTIFY_PAYLOAD);
    gpio_isr_handler_add(PIN_LORA_DIO2, dio_isr, (void *)RADIO_NOTIFY_SYNC);
}

/* ------------------------------------------------------------- TX ------ */

esp_err_t hw_tx_ford(const radio_config_t *cfg, const uint8_t payload[8], int count, int gap_ms, bool pa_boost,
                     int power_dbm)
{
    /* PA_BOOST: Pout = 2..17 dBm. RFO with MaxPower 7 (Pmax 15 dBm): Pout = 0..15 dBm. */
    if (pa_boost ? (power_dbm < 2 || power_dbm > 17) : (power_dbm < 0 || power_dbm > 15)) return ESP_ERR_INVALID_ARG;
    uint8_t frame[TPMS_FORD_FRAME_BYTES];
    tpms_ford_encode(payload, frame);

    set_mode(MODE_SLEEP);               /* clears the FIFO */
    set_mode(MODE_STDBY);
    uint16_t fdev = (uint16_t)((35000ULL << 19) / FXOSC);  /* +/-35 kHz like the sensors */
    wr(REG_FDEV_MSB, fdev >> 8);
    wr(REG_FDEV_LSB, fdev & 0xff);
    wr(REG_PACONFIG, pa_boost ? (uint8_t)(0x80 | 0x70 | (power_dbm - 2)) : (uint8_t)(0x70 | power_dbm));
    wr(REG_PREAMBLE_MSB, 0);
    wr(REG_PREAMBLE_LSB, 2);            /* 2 x 0x55, then the frame's own 55 55 55 56 */
    wr(REG_SYNCCONFIG, 0x20);           /* no sync word, preamble polarity 0x55 */
    wr(REG_PAYLOADLENGTH, sizeof(frame));
    wr(REG_FIFOTHRESH, 0x80 | 0x0f);    /* start TX as soon as the FIFO is not empty */

    esp_err_t err = ESP_OK;
    for (int i = 0; i < count && err == ESP_OK; i++) {
        wr_burst(REG_FIFO, frame, sizeof(frame));
        set_mode(MODE_TX);
        int64_t t0 = esp_timer_get_time();
        while (!(rd(REG_IRQFLAGS2) & IRQ2_PACKETSENT)) {
            if (esp_timer_get_time() - t0 > 100000) { err = ESP_ERR_TIMEOUT; break; }
            esp_rom_delay_us(200);
        }
        set_mode(MODE_STDBY);
        if (i + 1 < count && gap_ms) vTaskDelay(pdMS_TO_TICKS(gap_ms));
    }
    /* The caller re-applies the RX config (PA off: RX mode doesn't use it). */
    return err;
}

/* ------------------------------------------------------- diagnostics --- */

void hw_dump(FILE *out)
{
    for (int r = 1; r < 0x80; r++) {
        if ((r - 1) % 16 == 0) fprintf(out, "%02x:", r);
        fprintf(out, " %02x", rd((uint8_t)r));
        if (r % 16 == 0 || r == 0x7f) fprintf(out, "\n");
    }
}

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

void hw_probe(FILE *out)
{
    uint32_t e_sb[NPROBE], e_rx[NPROBE];
    int l_sb[NPROBE], l_rx[NPROBE];

    for (int i = 0; i < NPROBE; i++) {
        gpio_intr_disable(probe_pins[i]);
        gpio_input_enable(probe_pins[i]);
    }

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
