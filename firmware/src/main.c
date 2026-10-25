/*
 * Ford TPMS receiver: CC1101 or LR1121 @ 315 MHz FSK -> decoder -> status LED
 * + CLI, plus Wi-Fi web UI and MQTT when
 * wifi_config.h has an SSID. BT is never initialised.
 */
#include <stddef.h>
#include <stdio.h>

#include "app.h"
#include "board.h"
#include "cli.h"
#include "display.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha_mqtt.h"
#include "led.h"
#include "net.h"
#include "radio.h"
#include "sensors.h"
#include "settings.h"
#include "soc/rtc_cntl_reg.h"
#include "tires.h"

static const char *TAG = "tpms";

/*
 * Crash-loop rescue for boards without an auto-reset circuit: if the firmware
 * crashes CRASH_LOOP_LIMIT times in a row, each within CRASH_STABLE_MS of
 * boot, the next boot goes to the ROM download mode so it can be reflashed
 * without touching the board.
 */
#define CRASH_LOOP_LIMIT 3
#define CRASH_STABLE_MS  30000
#define CRASH_MAGIC      0x7e5c0de5u
static RTC_NOINIT_ATTR uint32_t s_crash_magic, s_crash_count;
static esp_timer_handle_t s_stable_timer;

void app_on_packet(const tpms_ford_t *p, const radio_rx_info_t *rx, uint32_t ms)
{
    int rssi_dbm = rx->rssi_dbm;
    bool new_burst = sensors_add(p, rx, ms);
    led_packet();
    char temp[8];
    if (p->temp_valid) snprintf(temp, sizeof(temp), "%dC", p->temp_c);
    else snprintf(temp, sizeof(temp), "n/a");
    esp_log_level_t lvl = new_burst ? ESP_LOG_INFO : ESP_LOG_DEBUG;
    char q[32] = "";
    if (rx->quality) snprintf(q, sizeof(q), " foff=%+.1f kHz lqi=%u", rx->freq_off_hz / 1e3, rx->lqi);
    ESP_LOG_LEVEL_LOCAL(lvl, TAG, "%s id=%08x pressure=%.2f PSI temp=%s mode=%s code=%06x flags=%02x rssi=%d dBm%s",
                        new_burst ? "packet" : "repeat", (unsigned)p->id, p->psi_x4 / 4.0, temp,
                        tpms_ford_mode_str(p->mode), (unsigned)p->code, p->flags, rssi_dbm, q);
    /* MQTT publishes once the burst has ended (ha_mqtt.c burst timer). */
}

/* Saved radio settings. Blobs from before drate_comp are its prefix: loaded
 * with the new fields at their defaults, so a reflash keeps the tuning. */
static bool radio_load_settings(radio_config_t *rc)
{
    if (settings_load("radio", rc, sizeof(*rc))) return true;
    radio_default_config(rc);
    return settings_load("radio", rc, offsetof(radio_config_t, drate_comp));
}

esp_err_t app_set_radio(const radio_config_t *cfg)
{
    esp_err_t err = radio_set_config(cfg);
    /* An unlockable frequency is still applied and saved (so the UI shows what
     * was asked for); the caller reports ESP_ERR_INVALID_STATE as a warning. */
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    radio_config_t actual;
    radio_get_config(&actual);  /* bandwidth snapped to what the chip supports */
    esp_err_t serr = settings_save("radio", &actual, sizeof(actual));
    return serr != ESP_OK ? serr : err;
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

bool app_can_reboot_to_bootloader(void)
{
#ifdef RTC_CNTL_FORCE_DOWNLOAD_BOOT
    return true;
#else
    return false;
#endif
}

void app_reboot_to_bootloader(void)
{
#ifdef RTC_CNTL_FORCE_DOWNLOAD_BOOT
    ESP_LOGW(TAG, "rebooting into the ROM download mode");
    led_bootloader();
    vTaskDelay(pdMS_TO_TICKS(50));  /* let the log drain */
    /* The ROM checks this bit after a software reset; esptool's watchdog
     * reset (--after watchdog-reset) boots the app again. */
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
#endif
}

static void boot_stable(void *arg)
{
    s_crash_count = 0;
}

static void crash_loop_check(void)
{
    esp_reset_reason_t why = esp_reset_reason();
    bool crash = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT;
    if (s_crash_magic != CRASH_MAGIC) {
        s_crash_magic = CRASH_MAGIC;
        s_crash_count = 0;
    }
    s_crash_count = crash ? s_crash_count + 1 : 0;
    if (s_crash_count >= CRASH_LOOP_LIMIT && app_can_reboot_to_bootloader()) {
        ESP_LOGE(TAG, "%u crashes in a row: entering ROM download mode for reflashing", (unsigned)s_crash_count);
        s_crash_count = 0;
        app_reboot_to_bootloader();
    }
    if (s_crash_count) ESP_LOGW(TAG, "crash reset %u of %d before the ROM loader rescue", (unsigned)s_crash_count,
                                CRASH_LOOP_LIMIT);
    esp_timer_create_args_t ta = {.callback = boot_stable, .name = "stable"};
    esp_timer_create(&ta, &s_stable_timer);
    esp_timer_start_once(s_stable_timer, CRASH_STABLE_MS * 1000ULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Ford TPMS receiver starting on %s", BOARD_NAME);
    led_init();
    crash_loop_check();

    /* CPU frequency scaling: 160 MHz when busy, 80 MHz when idle. 80 MHz keeps
     * APB at full speed for the UART/SPI/I2C peripherals. No light sleep. */
    esp_pm_config_t pm = {.max_freq_mhz = 160, .min_freq_mhz = 80, .light_sleep_enable = false};
    ESP_ERROR_CHECK(esp_pm_configure(&pm));

    settings_init();
    tires_init();
    sensors_init();
    /* Radio first: the display task reads radio stats from its first frame. */
    radio_config_t rc;
    if (radio_init(app_on_packet) != ESP_OK) {
        ESP_LOGE(TAG, "radio init failed; CLI still available");
    } else if (radio_load_settings(&rc) && radio_set_config(&rc) != ESP_OK) {
        ESP_LOGW(TAG, "saved radio settings rejected, using defaults");
        radio_default_config(&rc);
        radio_set_config(&rc);
    }
    display_settings_t ds;
    if (settings_load("display", &ds, sizeof(ds))) display_set_settings(&ds);
    display_init();
    if (net_init() != ESP_OK) ESP_LOGE(TAG, "Wi-Fi init failed");
    led_start();
    ESP_ERROR_CHECK(cli_start());
}
