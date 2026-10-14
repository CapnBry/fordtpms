/*
 * Ford TPMS receiver: SX1278 @ 315 MHz FSK -> decoder -> SSD1306 + CLI,
 * plus Wi-Fi web UI and MQTT when wifi_config.h has an SSID. BT is never
 * initialised.
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
#include "ha_mqtt.h"
#include "net.h"
#include "radio.h"
#include "sensors.h"
#include "settings.h"
#include "tires.h"

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
    sensor_entry_t e;
    if (new_burst && sensors_get(p->id, &e)) ha_mqtt_publish(&e);
}

int app_battery_mv(void)
{
    return battery_read_mv();
}

esp_err_t app_set_radio(const radio_config_t *cfg)
{
    esp_err_t err = radio_set_config(cfg);
    if (err != ESP_OK) return err;
    radio_config_t actual;
    radio_get_config(&actual);  /* bandwidth snapped to what the chip supports */
    return settings_save("radio", &actual, sizeof(actual));
}

void app_set_display(const display_settings_t *set)
{
    display_set_settings(set);
    settings_save("display", set, sizeof(*set));
}

void app_assign_tire(tire_pos_t pos, uint32_t id)
{
    uint32_t old_id;
    int old_pos;
    tires_set(pos, id, &old_id, &old_pos);
    ESP_LOGI(TAG, "tire %s: %08x -> %08x", tires_short_name(pos), (unsigned)old_id, (unsigned)id);
    ha_mqtt_tires_changed();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Ford TPMS receiver starting");

    /* CPU frequency scaling: 160 MHz when busy, 80 MHz when idle. 80 MHz keeps
     * APB at full speed for the UART/SPI/I2C peripherals. No light sleep. */
    esp_pm_config_t pm = {.max_freq_mhz = 160, .min_freq_mhz = 80, .light_sleep_enable = false};
    ESP_ERROR_CHECK(esp_pm_configure(&pm));

    gpio_config_t led = {.pin_bit_mask = 1ULL << PIN_LED, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&led);
    gpio_set_level(PIN_LED, 0);
    esp_timer_create_args_t ta = {.callback = led_off, .name = "led"};
    esp_timer_create(&ta, &s_led_timer);

    settings_init();
    tires_init();
    sensors_init();
    battery_init();
    /* Radio first: the display task reads radio stats from its first frame. */
    radio_config_t rc;
    if (radio_init(app_on_packet) != ESP_OK) {
        ESP_LOGE(TAG, "radio init failed; CLI still available");
    } else if (settings_load("radio", &rc, sizeof(rc)) && radio_set_config(&rc) != ESP_OK) {
        ESP_LOGW(TAG, "saved radio settings rejected, using defaults");
        radio_default_config(&rc);
        radio_set_config(&rc);
    }
    display_settings_t ds;
    if (settings_load("display", &ds, sizeof(ds))) display_set_settings(&ds);
    display_init();
    if (net_init() != ESP_OK) ESP_LOGE(TAG, "Wi-Fi init failed");
    ESP_ERROR_CHECK(cli_start());
}
