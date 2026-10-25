#include "display.h"

#include <string.h>

#include "board.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gfx.h"
#include "radio.h"
#include "sensors.h"
#include "tires.h"
#include "ui.h"

static const char *TAG = "display";

#define FRAME_MS 100

static i2c_master_dev_handle_t s_dev;
static gfx_t s_gfx;
static uint8_t s_txbuf[1 + GFX_FB_SIZE];
static const display_settings_t k_default_set = {
    .bright = 0x8f,
    .dim = 0x01,
    .dim_after_s = 120,
    .shift_period_s = 30,
};
static display_settings_t s_set = k_default_set;
static display_status_t s_status;
static volatile uint32_t s_activity_ms;
/* Last complete frame, for readers outside the display task. */
static uint8_t s_pub[GFX_FB_SIZE];
static uint32_t s_pub_gen = 1;
static portMUX_TYPE s_pub_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t cmd(const uint8_t *c, size_t n)
{
    uint8_t buf[32];
    buf[0] = 0x00;
    memcpy(buf + 1, c, n);
    esp_err_t err = i2c_master_transmit(s_dev, buf, n + 1, 50);
    if (err != ESP_OK) s_status.i2c_errors++;
    return err;
}

static esp_err_t set_contrast(uint8_t v)
{
    uint8_t c[] = {0x81, v};
    return cmd(c, sizeof(c));
}

static esp_err_t flush(void)
{
    static const uint8_t win[] = {0x21, 0, 127, 0x22, 0, 7};
    esp_err_t err = cmd(win, sizeof(win));
    if (err != ESP_OK) return err;
    s_txbuf[0] = 0x40;
    memcpy(s_txbuf + 1, s_gfx.fb, GFX_FB_SIZE);
    err = i2c_master_transmit(s_dev, s_txbuf, sizeof(s_txbuf), 100);
    if (err != ESP_OK) s_status.i2c_errors++;
    return err;
}

#if BOARD_HAS_OLED
static esp_err_t panel_init(void)
{
    static const uint8_t init[] = {
        0xAE,             /* display off */
        0xD5, 0x80,       /* clock divide */
        0xA8, 0x3F,       /* multiplex 64 */
        0xD3, 0x00,       /* display offset */
        0x40,             /* start line 0 */
        0x8D, 0x14,       /* charge pump on */
        0x20, 0x00,       /* horizontal addressing */
        0xA1,             /* segment remap */
        0xC8,             /* COM scan decrement */
        0xDA, 0x12,       /* COM pins */
        0xD9, 0xF1,       /* pre-charge */
        0xDB, 0x30,       /* VCOMH deselect */
        0xA4,             /* follow RAM */
        0xA6,             /* normal (not inverted) */
    };
    esp_err_t err = cmd(init, sizeof(init));
    if (err != ESP_OK) return err;
    set_contrast(s_set.bright);
    gfx_clear(&s_gfx);
    flush();
    static const uint8_t on[] = {0xAF};
    return cmd(on, 1);
}
#endif

/* Orbit through all GFX_SHIFT_X x GFX_SHIFT_Y offsets in a boustrophedon path,
 * so consecutive steps move by one pixel. */
static void shift_for_step(uint32_t step, int *ox, int *oy)
{
    int nx = GFX_SHIFT_X + 1, ny = GFX_SHIFT_Y + 1;
    int k = (int)(step % (uint32_t)(nx * ny));
    int row = k / nx, col = k % nx;
    *oy = row;
    *ox = (row % 2) ? nx - 1 - col : col;
}

static ui_sensor_t to_ui(const sensor_entry_t *e)
{
    return (ui_sensor_t){
        .id = e->pkt.id,
        .psi_x4 = e->pkt.psi_x4,
        .temp_valid = e->pkt.temp_valid,
        .temp_c = e->pkt.temp_c,
        .mode = e->pkt.mode,
        .code = e->pkt.code,
        .flags = e->pkt.flags,
        .rssi_dbm = e->rssi_best_dbm,
        .count = e->bursts,
        .last_ms = e->last_ms,
        .wheel = (uint8_t)(tires_find(e->pkt.id) + 1),
    };
}

static void build_model(ui_model_t *m, uint32_t now)
{
    static sensor_entry_t snap[SENSORS_MAX];
    radio_stats_t rs;
    radio_config_t rc;
    radio_get_stats(&rs);
    radio_get_config(&rc);

    memset(m, 0, sizeof(*m));
    m->now_ms = now;
    m->freq_hz = rc.freq_hz;
    m->syncs = rs.syncs;
    m->decoded = rs.decoded;
    m->bad = rs.err_manchester + rs.err_checksum + rs.err_flags;
    m->noise_dbm = rs.noise_dbm;
    m->rssi_hist_len = radio_rssi_history(m->rssi_hist, UI_RSSI_HIST);

    int n = sensors_snapshot(snap, SENSORS_MAX);
    m->nsensors = n;
    m->nshown = n < UI_MAX_WHEELS ? n : UI_MAX_WHEELS;
    for (int i = 0; i < m->nshown; i++) m->s[i] = to_ui(&snap[i]);

    /* Wheel assignments for the overview (searched over the whole table). */
    for (int w = 0; w < UI_MAX_WHEELS && w < TIRE_COUNT; w++) {
        uint32_t id = tires_get(w);
        m->wheel[w].id = id;
        if (id == TIRE_NONE) continue;
        m->wheels_assigned = true;
        for (int i = 0; i < n; i++) {
            if (snap[i].pkt.id == id) {
                m->wheel[w].s = to_ui(&snap[i]);
                m->wheel[w].heard = true;
                break;
            }
        }
    }
}

static void display_task(void *arg)
{
    static ui_model_t model;
    uint32_t frame = 0, gen = sensors_generation();
    uint32_t cycle_t0 = 0;
    bool dimmed = false;
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        int64_t t0 = esp_timer_get_time();
        uint32_t now = (uint32_t)(t0 / 1000);
        build_model(&model, now);

        uint32_t g = sensors_generation();
        if (g != gen) {
            gen = g;
            s_activity_ms = now;
            cycle_t0 = now;     /* restart the page cycle on the new reading */
        }

        /* Screen choice: fresh packet -> detail; later, rotate overview and
         * each recent sensor so all of them get shown. */
        const char *name;
        gfx_clear(&s_gfx);
        int ox, oy;
        shift_for_step(now / 1000 / (s_set.shift_period_s ? s_set.shift_period_s : 1), &ox, &oy);
        gfx_set_origin(&s_gfx, ox, oy);
        if (model.nshown == 0) {
            name = "idle";
            ui_render_idle(&s_gfx, &model, frame);
        } else if (now - model.s[0].last_ms < 20000) {
            name = "detail";
            ui_render_detail(&s_gfx, &model, &model.s[0], frame);
        } else {
            const uint32_t over_ms = 8000, each_ms = 5000;
            uint32_t period = over_ms + each_ms * (uint32_t)model.nshown;
            uint32_t t = (now - cycle_t0) % period;
            if (t < over_ms) {
                name = "overview";
                ui_render_overview(&s_gfx, &model, frame);
            } else {
                name = "detail";
                ui_render_detail(&s_gfx, &model, &model.s[(t - over_ms) / each_ms], frame);
            }
        }

        /* Only this task writes s_pub, so the compare needs no lock. */
        if (memcmp(s_pub, s_gfx.fb, GFX_FB_SIZE)) {
            taskENTER_CRITICAL(&s_pub_mux);
            memcpy(s_pub, s_gfx.fb, GFX_FB_SIZE);
            if (++s_pub_gen == 0) s_pub_gen = 1;
            taskEXIT_CRITICAL(&s_pub_mux);
        }

        if (s_status.present) {
            bool want_dim = now - s_activity_ms > s_set.dim_after_s * 1000;
            if (want_dim != dimmed) {
                dimmed = want_dim;
                set_contrast(dimmed ? s_set.dim : s_set.bright);
                ESP_LOGD(TAG, "%s", dimmed ? "dimmed" : "bright");
            }
            flush();
        }

        s_status.dimmed = dimmed;
        s_status.ox = ox;
        s_status.oy = oy;
        s_status.screen = name;
        s_status.frames = ++frame;
        s_status.frame_us = (uint32_t)(esp_timer_get_time() - t0);
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(FRAME_MS));
    }
}

esp_err_t display_init(void)
{
#if !BOARD_HAS_OLED
    /* Nothing to drive, but the task still renders into the framebuffer for
     * the `screen` command (s_status.present stays false: no I2C writes). */
    ESP_LOGI(TAG, "no display on this board, UI runs headless");
    s_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
    xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 5, NULL, 0);
    return ESP_ERR_NOT_SUPPORTED;
#else
    i2c_master_bus_config_t bus = {
        .i2c_port = -1,
        .sda_io_num = PIN_OLED_SDA,
        .scl_io_num = PIN_OLED_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    /* None of these failures are fatal: without a panel the task still renders
     * into the framebuffer (for the `screen` command), it just skips the I2C writes. */
    i2c_master_bus_handle_t bh;
    esp_err_t err = i2c_new_master_bus(&bus, &bh);
    if (err == ESP_OK) {
        err = i2c_master_probe(bh, OLED_I2C_ADDR, 50);
    }
    if (err == ESP_OK) {
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = OLED_I2C_ADDR,
            .scl_speed_hz = 400000,
        };
        err = i2c_master_bus_add_device(bh, &dev, &s_dev);
    }
    if (err == ESP_OK) {
        err = panel_init();
    }
    s_status.present = err == ESP_OK;
    if (s_status.present) ESP_LOGI(TAG, "SSD1306 at 0x%02x", OLED_I2C_ADDR);
    else ESP_LOGW(TAG, "no SSD1306 at 0x%02x (%s), UI runs headless", OLED_I2C_ADDR, esp_err_to_name(err));

    s_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
    xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 5, NULL, 0);
    return err;
#endif
}

void display_get_settings(display_settings_t *out)
{
    *out = s_set;
}

void display_set_settings(const display_settings_t *in)
{
    s_set = *in;
    if (s_status.present) set_contrast(s_status.dimmed ? s_set.dim : s_set.bright);
}

void display_default_settings(display_settings_t *out)
{
    *out = k_default_set;
}

void display_get_status(display_status_t *out)
{
    *out = s_status;
}

void display_poke(void)
{
    s_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

uint32_t display_frame_gen(void)
{
    return s_pub_gen;
}

uint32_t display_snapshot(uint8_t *fb)
{
    taskENTER_CRITICAL(&s_pub_mux);
    memcpy(fb, s_pub, GFX_FB_SIZE);
    uint32_t gen = s_pub_gen;
    taskEXIT_CRITICAL(&s_pub_mux);
    return gen;
}

void display_dump(FILE *out)
{
    static uint8_t fb[GFX_FB_SIZE];
    display_snapshot(fb);
    fprintf(out, "+%.*s+\n", GFX_W, "--------------------------------------------------------------------------------------------------------------------------------");
    for (int y = 0; y < GFX_H; y += 2) {
        fputc('|', out);
        for (int x = 0; x < GFX_W; x++) {
            int a = (fb[(y >> 3) * GFX_W + x] >> (y & 7)) & 1;
            int b = (fb[((y + 1) >> 3) * GFX_W + x] >> ((y + 1) & 7)) & 1;
            fputs(a && b ? "\u2588" : a ? "\u2580" : b ? "\u2584" : " ", out);
        }
        fputs("|\n", out);
    }
    fprintf(out, "+%.*s+\n", GFX_W, "--------------------------------------------------------------------------------------------------------------------------------");
}
