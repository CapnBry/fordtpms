/*
 * CC1101 backend: 2-FSK packet mode, 26 MHz crystal.
 *
 * GDO2 asserts on sync word received (RSSI is frozen there), GDO0 at the end
 * of a packet (RX FIFO threshold set above the packet size). The radio task
 * also polls PKTSTATUS/RXBYTES every ~5 ms, so a missed edge only costs
 * latency. After a packet the chip stays in RX (MCSM1.RXOFF_MODE), so
 * back-to-back frames are not missed; each payload carries the appended RSSI
 * and LQI status bytes. FREQEST (the frequency offset compensation loop's
 * estimate) is read when the sync word is seen: the loop has settled on the
 * preamble by then, and after the packet it drifts off on noise again.
 *
 * Chip select is driven in software: the CC1101 needs SO low (crystal running)
 * after CSn falls before the first clock edge, and only a GPIO CS can wait for
 * that. The data lines use the VSPI IO_MUX pins.
 *
 * PLL lock: after calibration FSCAL1 reads 0x3F if the synthesizer could not
 * lock (datasheet section 22.1). It is checked after each retune and every
 * RADIO_LOCK_CHECK_MS while receiving. The CC1101 is specified for 300-348,
 * 387-464 and 779-928 MHz; 315 MHz is in band.
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

static const char *TAG = "cc1101";

/* Configuration registers */
#define REG_IOCFG2    0x00
#define REG_IOCFG1    0x01
#define REG_IOCFG0    0x02
#define REG_FIFOTHR   0x03
#define REG_SYNC1     0x04
#define REG_SYNC0     0x05
#define REG_PKTLEN    0x06
#define REG_PKTCTRL1  0x07
#define REG_PKTCTRL0  0x08
#define REG_ADDR      0x09
#define REG_CHANNR    0x0A
#define REG_FSCTRL1   0x0B
#define REG_FSCTRL0   0x0C
#define REG_FREQ2     0x0D
#define REG_FREQ1     0x0E
#define REG_FREQ0     0x0F
#define REG_MDMCFG4   0x10
#define REG_MDMCFG3   0x11
#define REG_MDMCFG2   0x12
#define REG_MDMCFG1   0x13
#define REG_MDMCFG0   0x14
#define REG_DEVIATN   0x15
#define REG_MCSM2     0x16
#define REG_MCSM1     0x17
#define REG_MCSM0     0x18
#define REG_FOCCFG    0x19
#define REG_BSCFG     0x1A
#define REG_AGCCTRL2  0x1B
#define REG_AGCCTRL1  0x1C
#define REG_AGCCTRL0  0x1D
#define REG_FREND1    0x21
#define REG_FREND0    0x22
#define REG_FSCAL3    0x23
#define REG_FSCAL2    0x24
#define REG_FSCAL1    0x25
#define REG_FSCAL0    0x26
#define REG_TEST2     0x2C
#define REG_TEST1     0x2D
#define REG_TEST0     0x2E
#define NCONFIG       0x2F
/* Status registers (read with the burst bit set) */
#define ST_PARTNUM    0x30
#define ST_VERSION    0x31
#define ST_FREQEST    0x32
#define ST_LQI        0x33
#define ST_RSSI       0x34
#define ST_MARCSTATE  0x35
#define ST_PKTSTATUS  0x38
#define ST_RXBYTES    0x3B
#define REG_PATABLE   0x3E
#define REG_FIFO      0x3F
/* Command strobes */
#define SRES          0x30
#define SCAL          0x33
#define SRX           0x34
#define STX           0x35
#define SIDLE         0x36
#define SFRX          0x3A
#define SFTX          0x3B

#define RD            0x80
#define BURST         0x40

/* MARCSTATE values */
#define MARC_IDLE         0x01
#define MARC_RX           0x0D
#define MARC_RX_END       0x0E
#define MARC_RX_RST       0x0F
#define MARC_RXFIFO_OVF   0x11
#define MARC_TX           0x13
#define MARC_TXFIFO_UNF   0x16

#define PKTSTATUS_SFD     0x08
#define RXBYTES_OVERFLOW  0x80

/* GDOx_CFG values */
#define GDO_RXFIFO_THR_OR_END 0x01
#define GDO_SYNC              0x06
#define GDO_HW0               0x2F
#define GDO_INV               0x40

#define FXOSC 26000000ULL
#define RSSI_OFFSET 74       /* datasheet table 31, 315/433 MHz */
#define RADIO_LOCK_CHECK_MS 250
/* The synthesizer is calibrated on IDLE -> RX only; staying in RX for hours
 * needs a periodic recalibration to follow temperature drift. */
#define RADIO_RECAL_MS 60000

const radio_caps_t radio_hw_caps = {
    .name = "CC1101",
    .freq_min_hz = 300000000,
    .freq_max_hz = 928000000,
    .freq_note = "specified 300-348, 387-464 and 779-928 MHz",
    .bitrate_min = 600,
    .bitrate_max = 500000,
    .freq_offset = true,
    .tx = true,
    .tx_default_dbm = -10,
    .tx_note = "pa or rfo -30..10 dBm (single PA; -30/-20/-15/-10/0/5/7/10, rounded down)",
};

static spi_device_handle_t s_spi;
static radio_config_t s_cfg;       /* last applied, for the probe */
static uint8_t s_payload_len = TPMS_FORD_CHIP_BYTES;
static bool s_in_packet, s_locked, s_spi_timeout;
static int8_t s_sync_freqest;
static uint32_t s_sync_ms, s_lock_check_ms, s_recal_ms;
static uint32_t s_rx_restarts, s_recals;

/* ------------------------------------------------------------- SPI ----- */

/* One CSn-framed transfer. Waits for SO low (CHIP_RDYn) before clocking. */
static esp_err_t xfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
    gpio_set_level(PIN_CC1101_CSN, 0);
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(PIN_CC1101_MISO)) {
        if (esp_timer_get_time() - t0 > 5000) {
            gpio_set_level(PIN_CC1101_CSN, 1);
            s_spi_timeout = true;
            return ESP_ERR_TIMEOUT;
        }
    }
    spi_transaction_t t = {.length = n * 8, .tx_buffer = tx, .rx_buffer = rx};
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    gpio_set_level(PIN_CC1101_CSN, 1);
    return err;
}

static uint8_t strobe(uint8_t cmd)
{
    uint8_t st = 0xff;
    xfer(&cmd, &st, 1);
    return st;
}

static void wr(uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = {reg, val};
    xfer(tx, NULL, 2);
}

static uint8_t rd(uint8_t reg)
{
    uint8_t tx[2] = {reg | RD, 0}, rx[2] = {0xff, 0xff};
    xfer(tx, rx, 2);
    return rx[1];
}

/* Status register. Read until two reads agree: a value changing during the
 * read can come back corrupted (errata SWRZ020, SPI read synchronization). */
static uint8_t rd_status(uint8_t reg)
{
    uint8_t v = rd(reg | BURST), prev;
    for (int i = 0; i < 4; i++) {
        prev = v;
        v = rd(reg | BURST);
        if (v == prev) break;
    }
    return v;
}

static void rd_burst(uint8_t reg, uint8_t *buf, size_t n)
{
    uint8_t tx[1 + 64] = {reg | RD | BURST}, rx[1 + 64];
    if (n > 64) n = 64;
    if (xfer(tx, rx, n + 1) == ESP_OK) memcpy(buf, rx + 1, n);
}

static void wr_burst(uint8_t reg, const uint8_t *buf, size_t n)
{
    uint8_t tx[1 + 64] = {reg | BURST};
    if (n > 64) n = 64;
    memcpy(tx + 1, buf, n);
    xfer(tx, NULL, n + 1);
}

static int rssi_from_raw(uint8_t raw)
{
    return (int8_t)raw / 2 - RSSI_OFFSET;
}

/* FREQEST: two's complement, FXOSC / 2^14 (1.59 kHz) per step; positive = carrier above the tuned frequency. */
static int32_t freqest_hz(int8_t fe)
{
    return (int32_t)(fe * (int64_t)FXOSC / 16384);
}

static uint8_t marcstate(void)
{
    return rd_status(ST_MARCSTATE) & 0x1f;
}

static bool wait_state(uint8_t want, int timeout_us)
{
    int64_t t0 = esp_timer_get_time();
    while (marcstate() != want) {
        if (esp_timer_get_time() - t0 > timeout_us) return false;
        esp_rom_delay_us(20);
    }
    return true;
}

static void set_idle(void)
{
    strobe(SIDLE);
    wait_state(MARC_IDLE, 2000);
}

/* Datasheet 19.1.2 manual reset: CSn low/high, >40 us high, then SRES. */
static void hw_reset(void)
{
    gpio_set_level(PIN_CC1101_CSN, 1);
    esp_rom_delay_us(5);
    gpio_set_level(PIN_CC1101_CSN, 0);
    esp_rom_delay_us(10);
    gpio_set_level(PIN_CC1101_CSN, 1);
    esp_rom_delay_us(45);
    strobe(SRES);
    esp_rom_delay_us(200);
}

/* Enter RX from IDLE: the synthesizer calibrates on the way (MCSM0.FS_AUTOCAL). */
static void enter_rx(void)
{
    set_idle();
    strobe(SFRX);
    strobe(SRX);
    wait_state(MARC_RX, 3000);
    s_in_packet = false;
}

/* --------------------------------------------------------- config ------ */

static void set_freq(uint32_t hz)
{
    uint32_t f = (uint32_t)(((uint64_t)hz << 16) / FXOSC);
    wr(REG_FREQ2, (f >> 16) & 0xff);
    wr(REG_FREQ1, (f >> 8) & 0xff);
    wr(REG_FREQ0, f & 0xff);
    /* VCO selection calibration on the lower part of each band, as SmartRF
     * Studio sets it; on otherwise. */
    uint32_t mhz = hz / 1000000;
    bool vco_sel = !((mhz >= 323 && mhz <= 348) || (mhz >= 431 && mhz <= 464) || (mhz >= 861 && mhz <= 928));
    wr(REG_TEST0, vco_sel ? 0x0B : 0x09);
    wr(REG_FSCAL2, 0x2A);              /* high VCO; replaced by the calibration when TEST0 selects */
}

/* Lock = calibration succeeded (FSCAL1 != 0x3F) and the chip is in `want`. */
static bool check_lock(bool rx, uint8_t *fscal1_out, uint8_t *marc_out)
{
    uint8_t fscal1 = rd(REG_FSCAL1);
    uint8_t m = marcstate();
    if (fscal1_out) *fscal1_out = fscal1;
    if (marc_out) *marc_out = m;
    bool state_ok = rx ? (m == MARC_RX || m == MARC_RX_END || m == MARC_RX_RST) : m == MARC_IDLE;
    return !s_spi_timeout && fscal1 != 0x3F && state_ok;
}

/* Channel filter: double-sided BW = FXOSC / (8 * (4 + M) * 2^E), E and M 0..3. */
static uint32_t chanbw_dsb(int e, int m)
{
    return (uint32_t)(FXOSC / (8 * (4 + m) * (1u << e)));
}

static uint8_t chanbw_code(uint32_t ssb_hz, uint32_t *actual_ssb)
{
    uint8_t best = 0;
    uint32_t best_hz = 0, best_err = UINT32_MAX;
    for (int e = 0; e < 4; e++)
        for (int m = 0; m < 4; m++) {
            uint32_t bw = chanbw_dsb(e, m) / 2;
            uint32_t err = bw > ssb_hz ? bw - ssb_hz : ssb_hz - bw;
            if (err < best_err) {
                best_err = err;
                best = (uint8_t)(e << 2 | m);
                best_hz = bw;
            }
        }
    if (actual_ssb) *actual_ssb = best_hz;
    return best;
}

uint32_t hw_rxbw(uint32_t hz)
{
    uint32_t actual;
    chanbw_code(hz, &actual);
    return actual;
}

int hw_bandwidths(uint32_t *out, int max)
{
    int n = 0;
    for (int e = 3; e >= 0; e--)
        for (int m = 3; m >= 0 && n < max; m--) out[n++] = chanbw_dsb(e, m) / 2;
    return n;
}

/* Data rate = (256 + M) * 2^E * FXOSC / 2^28. */
static void drate_code(uint32_t baud, uint8_t *e_out, uint8_t *m_out)
{
    for (int e = 0; e < 16; e++) {
        uint64_t x = (((uint64_t)baud << (28 - e)) + FXOSC / 2) / FXOSC;  /* 256 + M */
        if (x < 512) {
            *e_out = (uint8_t)e;
            *m_out = (uint8_t)(x < 256 ? 0 : x - 256);
            return;
        }
    }
    *e_out = 15;
    *m_out = 255;
}

/* Deviation = FXOSC / 2^17 * (8 + M) * 2^E. */
static uint8_t deviatn_code(uint32_t hz)
{
    uint8_t best = 0;
    uint32_t best_err = UINT32_MAX;
    for (int e = 0; e < 8; e++)
        for (int m = 0; m < 8; m++) {
            uint32_t d = (uint32_t)((FXOSC * (8 + m) << e) >> 17);
            uint32_t err = d > hz ? d - hz : hz - d;
            if (err < best_err) {
                best_err = err;
                best = (uint8_t)(e << 4 | m);
            }
        }
    return best;
}

esp_err_t hw_apply(const radio_config_t *cfg, bool *locked)
{
    s_cfg = *cfg;
    set_idle();
    s_spi_timeout = false;

    uint32_t dsb = 2 * hw_rxbw(cfg->rxbw_hz);
    uint8_t dr_e, dr_m;
    drate_code(cfg->bitrate, &dr_e, &dr_m);
    uint8_t sync[2];
    radio_sync_bytes(cfg->sync, sync);
    s_payload_len = (uint8_t)radio_payload_len(cfg->sync);

    wr(REG_IOCFG2, GDO_SYNC);          /* GDO2: sync word received .. end of packet */
    wr(REG_IOCFG1, 0x2E);              /* GDO1 is SO: high impedance while CSn is high */
    wr(REG_IOCFG0, GDO_RXFIFO_THR_OR_END);  /* GDO0: end of packet (threshold above the packet) */
    wr(REG_FIFOTHR, 0x47);             /* ADC retention, RX threshold 32 bytes */
    wr(REG_SYNC1, sync[0]);
    wr(REG_SYNC0, sync[1]);
    wr(REG_PKTLEN, s_payload_len);
    wr(REG_PKTCTRL1, 0x04);            /* no preamble quality gate, append RSSI + LQI, no address check */
    wr(REG_PKTCTRL0, 0x00);            /* no whitening, FIFO mode, no CRC, fixed length */
    wr(REG_ADDR, 0x00);
    wr(REG_CHANNR, 0x00);
    /* IF frequency: wider filters need a higher IF (SmartRF Studio values). */
    wr(REG_FSCTRL1, dsb <= 102000 ? 0x06 : dsb <= 325000 ? 0x08 : 0x0C);
    wr(REG_FSCTRL0, 0x00);
    set_freq(cfg->freq_hz);
    wr(REG_MDMCFG4, (uint8_t)(chanbw_code(cfg->rxbw_hz, NULL) << 4 | dr_e));
    wr(REG_MDMCFG3, dr_m);
    /* DC filter on, 2-FSK, no Manchester (the chips are decoded in software),
     * 16/16 sync bits. The preamble detector stays off: the sync word is the
     * preamble's tail, as with the other backend. */
    wr(REG_MDMCFG2, 0x02);
    wr(REG_MDMCFG1, 0x02);             /* no FEC, 2 preamble bytes (TX), default channel spacing */
    wr(REG_MDMCFG0, 0xF8);
    wr(REG_DEVIATN, deviatn_code(35000));  /* TX only in 2-FSK: the sensors use ~35 kHz */
    wr(REG_MCSM2, 0x07);
    wr(REG_MCSM1, 0x3C);               /* stay in RX after a packet, IDLE after TX */
    wr(REG_MCSM0, 0x18);               /* calibrate IDLE -> RX/TX, PO_TIMEOUT 149 us */
    wr(REG_FOCCFG, 0x16);              /* frequency offset compensation +/-BW/4: sensor crystal error */
    wr(REG_BSCFG, 0x6C | (cfg->drate_comp & 3));  /* BS_LIMIT: data rate offset tracking range */
    wr(REG_AGCCTRL2, 0x43);
    wr(REG_AGCCTRL1, 0x40);
    wr(REG_AGCCTRL0, 0x91);
    wr(REG_FREND1, 0x56);
    wr(REG_FREND0, 0x10);
    wr(REG_FSCAL3, 0xE9);
    wr(REG_FSCAL1, 0x00);
    wr(REG_FSCAL0, 0x1F);
    wr(REG_TEST2, 0x81);               /* sensitivity settings for <= 100 kBaud */
    wr(REG_TEST1, 0x35);

    enter_rx();
    uint8_t fscal1, m;
    s_locked = check_lock(true, &fscal1, &m);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    s_lock_check_ms = s_recal_ms = now;
    ESP_LOGD(TAG, "tuned %.3f MHz: FSCAL1 0x%02x, MARCSTATE 0x%02x", cfg->freq_hz / 1e6, fscal1, m);
    *locked = s_locked;
    return s_spi_timeout ? ESP_ERR_TIMEOUT : ESP_OK;
}

bool hw_pll_test(uint32_t freq_hz, char *detail, size_t detail_len)
{
    set_idle();
    s_spi_timeout = false;
    set_freq(freq_hz);
    strobe(SCAL);
    bool done = wait_state(MARC_IDLE, 3000);
    uint8_t fscal1, m;
    bool locked = check_lock(false, &fscal1, &m);
    snprintf(detail, detail_len, "FSCAL1 0x%02x FSCAL2 0x%02x%s", fscal1, rd(REG_FSCAL2),
             done ? "" : ", calibration timed out");
    return locked && done;
}

/* --------------------------------------------------------- receive ----- */

static void IRAM_ATTR gdo_isr(void *arg)
{
    radio_isr_notify((uint32_t)(uintptr_t)arg);
}

void hw_poll(uint32_t bits, uint32_t now, radio_hw_poll_t *o, radio_stats_t *st)
{
    if (bits & RADIO_NOTIFY_PAYLOAD) st->irq_payload++;
    if (bits & RADIO_NOTIFY_SYNC) st->irq_sync++;

    uint8_t pkt = rd_status(ST_PKTSTATUS);
    uint8_t rxb = rd_status(ST_RXBYTES);

    if (rxb & RXBYTES_OVERFLOW) {
        st->fifo_overruns++;
        enter_rx();
    } else {
        if ((pkt & PKTSTATUS_SFD) && !s_in_packet) {
            o->sync = true;
            o->sync_rssi_dbm = rssi_from_raw(rd_status(ST_RSSI));  /* frozen at the sync word */
            s_sync_freqest = (int8_t)rd_status(ST_FREQEST);
            s_sync_ms = now;
            s_in_packet = true;
            if (!(bits & RADIO_NOTIFY_SYNC)) st->polled_events++;
        }
        if ((rxb & 0x7f) >= s_payload_len + 2) {
            uint8_t buf[32 + 2];
            rd_burst(REG_FIFO, buf, s_payload_len + 2);
            if (!s_in_packet) {
                /* Sync and end of packet both fell between two polls. */
                o->sync = true;
                o->sync_rssi_dbm = rssi_from_raw(buf[s_payload_len]);
                s_sync_freqest = (int8_t)rd_status(ST_FREQEST);  /* late: may have moved since the packet */
            }
            if (!(bits & RADIO_NOTIFY_PAYLOAD)) st->polled_events++;
            memcpy(o->payload, buf, s_payload_len);
            o->payload_len = s_payload_len;
            o->payload_rssi_valid = true;
            o->payload_rssi_dbm = rssi_from_raw(buf[s_payload_len]);  /* appended: RSSI at sync */
            o->payload_quality = true;
            o->payload_freq_off_hz = freqest_hz(s_sync_freqest);
            o->payload_lqi = buf[s_payload_len + 1] & 0x7f;  /* appended; bit 7 is CRC_OK (unused) */
            /* The next packet's sync may already be in; the next poll sees SFD again. */
            s_in_packet = false;
        } else if (s_in_packet && !(pkt & PKTSTATUS_SFD) && (rxb & 0x7f) == 0) {
            s_in_packet = false;       /* packet dropped by the chip */
        } else if (s_in_packet && now - s_sync_ms > 50) {
            /* A 16 byte payload takes ~7 ms; don't stay stuck. */
            enter_rx();
        } else if (!s_in_packet) {
            o->idle = true;
            o->idle_rssi_dbm = rssi_from_raw(rd_status(ST_RSSI));
        }
    }

    if (!s_in_packet && (rxb & 0x7f) == 0 && now - s_recal_ms >= RADIO_RECAL_MS) {
        s_recal_ms = now;
        s_recals++;
        enter_rx();                    /* IDLE -> RX recalibrates (~0.8 ms deaf) */
        s_lock_check_ms = 0;           /* check the new calibration right away */
    }

    if (now - s_lock_check_ms >= RADIO_LOCK_CHECK_MS) {
        s_lock_check_ms = now;
        s_spi_timeout = false;
        uint8_t m;
        bool locked = check_lock(true, NULL, &m);
        if (!locked) {
            st->pll_unlocked_polls++;
            /* Retry: back to RX with a fresh calibration; the next check sees the result. */
            enter_rx();
            s_rx_restarts++;
        }
        s_locked = locked;
    }
    o->locked = s_locked;
}

/* ------------------------------------------------------------ init ----- */

esp_err_t hw_init(radio_stats_t *st)
{
    gpio_config_t cs = {
        .pin_bit_mask = 1ULL << PIN_CC1101_CSN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cs);
    gpio_set_level(PIN_CC1101_CSN, 1);
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_CC1101_GDO0) | (1ULL << PIN_CC1101_GDO2),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);

    /* VSPI's own IO_MUX pins: no GPIO matrix in the SPI path. */
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_CC1101_MOSI,
        .miso_io_num = PIN_CC1101_MISO,
        .sclk_io_num = PIN_CC1101_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 1 + 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_DISABLED));
    gpio_pullup_en(PIN_CC1101_MISO);   /* SO is tri-stated with CSn high; reads "not ready" if absent */
    spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = 5 * 1000 * 1000,  /* burst access is specified up to 6.5 MHz */
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev, &s_spi));

    s_spi_timeout = false;
    hw_reset();
    uint8_t part = rd_status(ST_PARTNUM), ver = rd_status(ST_VERSION);
    snprintf(st->chip, sizeof(st->chip), "CC1101 part 0x%02x v0x%02x", part, ver);
    if (s_spi_timeout || part != 0x00 || ver == 0x00 || ver == 0xff) {
        ESP_LOGE(TAG, "CC1101 not found (PARTNUM 0x%02x VERSION 0x%02x%s)", part, ver,
                 s_spi_timeout ? ", SO never went low after CSn" : "");
        return ESP_ERR_NOT_FOUND;
    }
    if (ver != 0x14) ESP_LOGW(TAG, "VERSION 0x%02x (genuine CC1101 parts report 0x14)", ver);
    ESP_LOGI(TAG, "%s", st->chip);
    return ESP_OK;
}

void hw_start_irq(void)
{
    gpio_set_intr_type(PIN_CC1101_GDO0, GPIO_INTR_POSEDGE);
    gpio_set_intr_type(PIN_CC1101_GDO2, GPIO_INTR_POSEDGE);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_CC1101_GDO0, gdo_isr, (void *)RADIO_NOTIFY_PAYLOAD);
    gpio_isr_handler_add(PIN_CC1101_GDO2, gdo_isr, (void *)RADIO_NOTIFY_SYNC);
}

/* ------------------------------------------------------------- TX ------ */

/* PATABLE values for 2-FSK, TI DN013, per band. */
static const int8_t k_pa_dbm[8] = {-30, -20, -15, -10, 0, 5, 7, 10};
static const uint8_t k_pa_315[8] = {0x12, 0x0D, 0x1C, 0x34, 0x51, 0x85, 0xCB, 0xC2};
static const uint8_t k_pa_433[8] = {0x12, 0x0E, 0x1D, 0x34, 0x60, 0x84, 0xC8, 0xC0};
static const uint8_t k_pa_868[8] = {0x03, 0x0F, 0x1E, 0x27, 0x50, 0x81, 0xCB, 0xC2};
static const uint8_t k_pa_915[8] = {0x03, 0x0E, 0x1E, 0x27, 0x8E, 0xCD, 0xC7, 0xC0};

esp_err_t hw_tx_ford(const radio_config_t *cfg, const uint8_t payload[8], int count, int gap_ms, bool pa_boost,
                     int power_dbm)
{
    (void)pa_boost;                    /* one PA: `pa` and `rfo` are the same */
    if (power_dbm < -30 || power_dbm > 10) return ESP_ERR_INVALID_ARG;
    const uint8_t *tbl = cfg->freq_hz < 370000000 ? k_pa_315 : cfg->freq_hz < 600000000 ? k_pa_433
                       : cfg->freq_hz < 890000000 ? k_pa_868 : k_pa_915;
    int pa = 0;
    while (pa < 7 && k_pa_dbm[pa + 1] <= power_dbm) pa++;
    if (k_pa_dbm[pa] != power_dbm) ESP_LOGW(TAG, "tx power %d dBm rounded down to %d dBm", power_dbm, k_pa_dbm[pa]);

    uint8_t frame[TPMS_FORD_FRAME_BYTES];
    tpms_ford_encode(payload, frame);

    set_idle();
    s_spi_timeout = false;
    wr(REG_IOCFG2, GDO_HW0);           /* keep the RX interrupts quiet */
    wr(REG_IOCFG0, GDO_HW0);
    /* 2 preamble bytes (0xAA, the minimum) + sync 0x5555, then the frame's own
     * 55 55 55 56: the receivers sync on the frame's 5556. */
    wr(REG_SYNC1, 0x55);
    wr(REG_SYNC0, 0x55);
    wr(REG_PKTLEN, sizeof(frame));
    wr(REG_PATABLE, tbl[pa]);

    esp_err_t err = ESP_OK;
    for (int i = 0; i < count && err == ESP_OK; i++) {
        strobe(SFTX);
        wr_burst(REG_FIFO, frame, sizeof(frame));
        strobe(STX);                   /* calibrates first (IDLE -> TX) */
        int64_t t0 = esp_timer_get_time();
        bool checked = false;
        for (;;) {
            uint8_t m = marcstate();
            if (m == MARC_TX && !checked) {
                checked = true;
                if (rd(REG_FSCAL1) == 0x3F) {
                    ESP_LOGE(TAG, "TX PLL not locked at %.3f MHz", cfg->freq_hz / 1e6);
                    err = ESP_ERR_INVALID_STATE;
                }
            }
            if (m == MARC_IDLE) break;
            if (m == MARC_TXFIFO_UNF) { err = ESP_FAIL; break; }
            if (esp_timer_get_time() - t0 > 100000) { err = ESP_ERR_TIMEOUT; break; }
            esp_rom_delay_us(200);
        }
        set_idle();
        if (i + 1 < count && gap_ms) vTaskDelay(pdMS_TO_TICKS(gap_ms));
    }
    if (s_spi_timeout) err = ESP_ERR_TIMEOUT;
    /* The caller re-applies the RX configuration (sync word, length, GDOs). */
    return err;
}

/* ------------------------------------------------------- diagnostics --- */

static const char *marc_name(uint8_t m)
{
    static const char *const names[] = {
        "SLEEP", "IDLE", "XOFF", "VCOON_MC", "REGON_MC", "MANCAL", "VCOON", "REGON", "STARTCAL", "BWBOOST",
        "FS_LOCK", "IFADCON", "ENDCAL", "RX", "RX_END", "RX_RST", "TXRX_SWITCH", "RXFIFO_OVERFLOW", "FSTXON",
        "TX", "TX_END", "RXTX_SWITCH", "TXFIFO_UNDERFLOW",
    };
    return m < sizeof(names) / sizeof(names[0]) ? names[m] : "?";
}

void hw_dump(FILE *out)
{
    uint8_t r[NCONFIG];
    rd_burst(0x00, r, NCONFIG);
    for (int i = 0; i < NCONFIG; i++) {
        if (i % 16 == 0) fprintf(out, "%02x:", i);
        fprintf(out, " %02x", r[i]);
        if (i % 16 == 15 || i == NCONFIG - 1) fprintf(out, "\n");
    }
    uint8_t m = marcstate();
    uint8_t fscal1 = rd(REG_FSCAL1);
    fprintf(out, "part/version  0x%02x / 0x%02x\n", rd_status(ST_PARTNUM), rd_status(ST_VERSION));
    fprintf(out, "marcstate     0x%02x %s\n", m, marc_name(m));
    fprintf(out, "pll           %s (FSCAL1 0x%02x), RX re-entered %u times, %u periodic recalibrations\n",
            s_locked ? "locked" : "NOT LOCKED", fscal1, (unsigned)s_rx_restarts, (unsigned)s_recals);
    fprintf(out, "rssi now      %d dBm\n", rssi_from_raw(rd_status(ST_RSSI)));
    int8_t fe = (int8_t)rd_status(ST_FREQEST);
    fprintf(out, "freqest now   %+d Hz (FOC loop, follows noise between packets), LQI %u\n", (int)freqest_hz(fe),
            rd_status(ST_LQI) & 0x7f);
    uint8_t pkt = rd_status(ST_PKTSTATUS);
    fprintf(out, "pktstatus     0x%02x (GDO0 pin %d, GDO2 pin %d), rxbytes 0x%02x\n", pkt,
            gpio_get_level(PIN_CC1101_GDO0), gpio_get_level(PIN_CC1101_GDO2), rd_status(ST_RXBYTES));
}

/* Drives one GDO high, then low (IOCFGx = HW1 / HW0), with the ESP32 pull
 * set against it so an unconnected line reads wrong. */
static bool probe_gdo(FILE *out, const char *name, uint8_t iocfg, int pin)
{
    gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
    wr(iocfg, GDO_HW0 | GDO_INV);
    esp_rom_delay_us(100);
    int hi = gpio_get_level(pin);
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    wr(iocfg, GDO_HW0);
    esp_rom_delay_us(100);
    int lo = gpio_get_level(pin);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    bool ok = hi == 1 && lo == 0;
    fprintf(out, "%s GPIO%-2d   driven high reads %d, driven low reads %d -> %s\n", name, pin, hi, lo,
            ok ? "WIRED" : "NOT WIRED?");
    return ok;
}

void hw_probe(FILE *out)
{
    gpio_intr_disable(PIN_CC1101_GDO0);
    gpio_intr_disable(PIN_CC1101_GDO2);

    s_spi_timeout = false;
    hw_reset();
    fprintf(out, "SO/CSn        %s\n", s_spi_timeout ? "SO never went low after CSn: check CSn, SO (MISO), power"
                                                     : "chip ready (SO low after CSn)");
    uint8_t part = rd_status(ST_PARTNUM), ver = rd_status(ST_VERSION);
    wr(REG_SYNC1, 0xA5);
    uint8_t a = rd(REG_SYNC1);
    wr(REG_SYNC1, 0x5A);
    uint8_t b = rd(REG_SYNC1);
    fprintf(out, "SPI           PARTNUM 0x%02x VERSION 0x%02x, write/read 0xa5 -> 0x%02x, 0x5a -> 0x%02x -> %s\n",
            part, ver, a, b, (a == 0xA5 && b == 0x5A) ? "OK" : "CHECK SCK/MOSI/MISO");
    probe_gdo(out, "GDO0 (end of packet)", REG_IOCFG0, PIN_CC1101_GDO0);
    probe_gdo(out, "GDO2 (sync word)    ", REG_IOCFG2, PIN_CC1101_GDO2);

    gpio_intr_enable(PIN_CC1101_GDO0);
    gpio_intr_enable(PIN_CC1101_GDO2);
    /* radio.c re-applies the RX configuration after this (the reset cleared it). */
}
