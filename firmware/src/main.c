/*
 * Ford TPMS receiver: SX1278 @ 315 MHz FSK -> decoder -> SSD1306 + CLI.
 * Wi-Fi/BT are never initialised, so the 2.4 GHz radio stays off.
 */
#include <stdio.h>

#include "app.h"
#include "battery.h"
#include "board.h"
#include "cli.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "radio.h"
#include "sensors.h"

static const char *TAG = "tpms";
static esp_timer_handle_t s_led_timer;

static void led_off(void *arg)
{
    gpio_set_level(PIN_LED, 0);
}

static void led_blink(void)
{
    gpio_set_level(PIN_LED, 1);
    esp_timer_stop(s_led_timer);
    esp_timer_start_once(s_led_timer, 60 * 1000);
}

void app_on_packet(const tpms_ford_t *p, int rssi_dbm, uint32_t ms)
{
    bool new_burst = sensors_add(p, rssi_dbm, ms);
    led_blink();
    char temp[8];
    if (p->temp_valid) snprintf(temp, sizeof(temp), "%dC", p->temp_c);
    else snprintf(temp, sizeof(temp), "n/a");
    esp_log_level_t lvl = new_burst ? ESP_LOG_INFO : ESP_LOG_DEBUG;
    ESP_LOG_LEVEL_LOCAL(lvl, TAG, "%s id=%08x pressure=%.2f PSI temp=%s mode=%s code=%06x flags=%02x rssi=%d dBm",
                        new_burst ? "packet" : "repeat", (unsigned)p->id, p->psi_x4 / 4.0, temp,
                        tpms_ford_mode_str(p->mode), (unsigned)p->code, p->flags, rssi_dbm);
}

int app_battery_mv(void)
{
    return battery_read_mv();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Ford TPMS receiver starting (Wi-Fi off)");

    /* CPU frequency scaling: 160 MHz when busy, 80 MHz when idle. 80 MHz keeps
     * APB at full speed for the UART/SPI/I2C peripherals. No light sleep. */
    esp_pm_config_t pm = {.max_freq_mhz = 160, .min_freq_mhz = 80, .light_sleep_enable = false};
    ESP_ERROR_CHECK(esp_pm_configure(&pm));

    gpio_config_t led = {.pin_bit_mask = 1ULL << PIN_LED, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&led);
    gpio_set_level(PIN_LED, 0);
    esp_timer_create_args_t ta = {.callback = led_off, .name = "led"};
    esp_timer_create(&ta, &s_led_timer);

    sensors_init();
    battery_init();
    /* Radio first: the display task reads radio stats from its first frame. */
    if (radio_init(app_on_packet) != ESP_OK) {
        ESP_LOGE(TAG, "radio init failed; CLI still available");
    }
    display_init();
    ESP_ERROR_CHECK(cli_start());
}
