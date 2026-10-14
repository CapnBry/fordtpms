#include "battery.h"

#include "board.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "battery";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
/* Readers: display, CLI and web UI. Cached so they don't hammer the ADC. */
static SemaphoreHandle_t s_lock;
static int s_cached_mv = -1;
static int64_t s_cached_us;

#define BATTERY_CACHE_US (1000 * 1000)

void battery_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(PIN_BATT_ADC, &unit, &s_chan) != ESP_OK) return;
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = unit};
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) {
        s_adc = NULL;
        return;
    }
    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_oneshot_config_channel(s_adc, s_chan, &ccfg);
    adc_cali_line_fitting_config_t cal = {
        .unit_id = unit,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .default_vref = 1100,
    };
    if (adc_cali_create_scheme_line_fitting(&cal, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC calibration, using raw estimate");
        s_cali = NULL;
    }
}

int battery_read_mv(void)
{
    if (!s_adc) return -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    if (s_cached_mv >= 0 && now - s_cached_us < BATTERY_CACHE_US) {
        int mv = s_cached_mv;
        xSemaphoreGive(s_lock);
        return mv;
    }
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw = 0, mv = 0;
        adc_oneshot_read(s_adc, s_chan, &raw);
        if (s_cali) adc_cali_raw_to_voltage(s_cali, raw, &mv);
        else mv = raw * 3300 / 4095;
        sum += mv;
    }
    s_cached_mv = sum / 8 * BATT_DIVIDER;
    s_cached_us = now;
    int mv = s_cached_mv;
    xSemaphoreGive(s_lock);
    return mv;
}
