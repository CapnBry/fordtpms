#include "led.h"

#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if defined(PIN_LED)

/* ------------------------------------------------- plain LED (TTGO) ---- */

static esp_timer_handle_t s_led_timer;

static void led_off(void *arg)
{
    gpio_set_level(PIN_LED, 0);
}

void led_init(void)
{
    gpio_config_t led = {.pin_bit_mask = 1ULL << PIN_LED, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&led);
    gpio_set_level(PIN_LED, 0);
    esp_timer_create_args_t ta = {.callback = led_off, .name = "led"};
    esp_timer_create(&ta, &s_led_timer);
}

void led_start(void)
{
}

void led_packet(void)
{
    gpio_set_level(PIN_LED, 1);
    esp_timer_stop(s_led_timer);
    esp_timer_start_once(s_led_timer, 60 * 1000);
}

void led_bootloader(void)
{
    gpio_set_level(PIN_LED, 1);
}

#elif defined(PIN_RGB_LED)

/* -------------------------------------------- WS2812B + button (C3) ---- */

#include "driver/rmt_tx.h"
#include "net.h"
#include "radio.h"

static const char *TAG = "led";

#define LED_PERIOD_MS   50
#define FLASH_MS        80
#define HOLD_REBOOT_MS  3000

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static volatile int64_t s_flash_until_us;
static volatile bool s_bootloader;

/* 10 MHz RMT ticks: 0 = 0.3 us high + 0.9 us low, 1 = 0.9 us high + 0.3 us low. */
static void show(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_chan) return;
    uint8_t grb[3] = {g, r, b};
    rmt_transmit_config_t tc = {.loop_count = 0};
    rmt_transmit(s_chan, s_enc, grb, sizeof(grb), &tc);
    rmt_tx_wait_all_done(s_chan, 10);
}

void led_init(void)
{
    rmt_tx_channel_config_t cc = {
        .gpio_num = PIN_RGB_LED,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 48,
        .trans_queue_depth = 2,
    };
    rmt_bytes_encoder_config_t ec = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
        .flags.msb_first = 1,
    };
    if (rmt_new_tx_channel(&cc, &s_chan) != ESP_OK || rmt_new_bytes_encoder(&ec, &s_enc) != ESP_OK ||
        rmt_enable(s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "WS2812 init failed");
        s_chan = NULL;
        return;
    }
    show(0, 0, 24);  /* blue: starting */

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);
}

static void log_status(void)
{
    radio_stats_t rs;
    radio_config_t rc;
    radio_get_stats(&rs);
    radio_get_config(&rc);
    ESP_LOGI(TAG, "button: %s, %.3f MHz, PLL %s, noise %d dBm, %u decoded, wifi %s",
             rs.present ? rs.chip : "no radio", rc.freq_hz / 1e6, rs.pll_locked ? "locked" : "NOT LOCKED",
             rs.noise_dbm, (unsigned)rs.decoded, net_connected() ? "connected" : "not connected");
}

static void led_task(void *arg)
{
    uint32_t tick = 0, pressed_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LED_PERIOD_MS));
        tick++;

        bool down = gpio_get_level(PIN_BUTTON) == 0;
        if (down) {
            pressed_ms += LED_PERIOD_MS;
            if (pressed_ms == HOLD_REBOOT_MS) {
                ESP_LOGW(TAG, "button held %d s: restarting", HOLD_REBOOT_MS / 1000);
                show(0, 0, 24);
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        } else {
            if (pressed_ms >= 50 && pressed_ms < HOLD_REBOOT_MS) log_status();
            pressed_ms = 0;
        }

        if (s_bootloader) continue;
        radio_stats_t rs;
        radio_get_stats(&rs);
        uint8_t r = 0, g = 0, b = 0;
        if (down) {
            /* While held: blue, brightening toward the restart. */
            b = (uint8_t)(8 + 40 * pressed_ms / HOLD_REBOOT_MS);
        } else if (!rs.present) {
            if (tick % 6 < 3) r = 40;                       /* 300 ms on/off */
        } else if (!rs.pll_locked) {
            if (tick % 20 < 10) r = 40;                     /* 1 s period */
        } else if (esp_timer_get_time() < s_flash_until_us) {
            r = g = b = 60;
        } else if (net_enabled() && !net_connected()) {
            r = 16;
            g = 10;
        } else {
            g = 12;
        }
        show(r, g, b);
    }
}

void led_start(void)
{
    if (s_chan) xTaskCreate(led_task, "led", 3072, NULL, 3, NULL);
}

void led_packet(void)
{
    s_flash_until_us = esp_timer_get_time() + FLASH_MS * 1000;
}

void led_bootloader(void)
{
    s_bootloader = true;
    show(40, 0, 40);
}

#else
#error "board has no LED"
#endif
