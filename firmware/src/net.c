#include "net.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha_mqtt.h"
#include "mdns.h"
#include "net_config.h"
#include "web.h"

static const char *TAG = "net";

static esp_netif_t *s_netif;
static volatile bool s_connected;
static uint32_t s_reconnects;
static bool s_services_started;

#define RETRY_MIN_MS 1000
#define RETRY_MAX_MS 30000
static uint32_t s_retry_ms = RETRY_MIN_MS;
static esp_timer_handle_t s_retry_timer;

static void retry_connect(void *arg)
{
    esp_wifi_connect();
}

static void start_services(void)
{
    if (s_services_started) return;
    s_services_started = true;

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(WIFI_HOSTNAME);
        mdns_instance_name_set("Ford TPMS receiver");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    } else {
        ESP_LOGW(TAG, "mDNS init failed");
    }

    setenv("TZ", TIME_ZONE, 1);
    tzset();
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    esp_netif_sntp_init(&sntp);

    web_start();
    ha_mqtt_start();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        if (s_connected) s_reconnects++;
        s_connected = false;
        ESP_LOGW(TAG, "disconnected (reason %d), retry in %u ms", d->reason, (unsigned)s_retry_ms);
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, (uint64_t)s_retry_ms * 1000);
        s_retry_ms = s_retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : s_retry_ms * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "connected to \"%s\", IP " IPSTR ", hostname %s", WIFI_SSID, IP2STR(&e->ip_info.ip),
                 WIFI_HOSTNAME);
        s_connected = true;
        s_retry_ms = RETRY_MIN_MS;
        start_services();
    }
}

esp_err_t net_init(void)
{
    if (!WIFI_SSID[0]) {
        ESP_LOGI(TAG, "no SSID in wifi_config.h, Wi-Fi off");
        return ESP_OK;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, WIFI_HOSTNAME);

    esp_timer_create_args_t ta = {.callback = retry_connect, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_retry_timer));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASSWORD, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_PASSWORD[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting to \"%s\"", WIFI_SSID);
    return ESP_OK;
}

void net_get_status(net_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->enabled = WIFI_SSID[0] != 0;
    out->connected = s_connected;
    out->reconnects = s_reconnects;
    strlcpy(out->ssid, WIFI_SSID, sizeof(out->ssid));
    strlcpy(out->hostname, WIFI_HOSTNAME, sizeof(out->hostname));
    out->time_valid = net_wall_time(0) != 0;
    if (s_connected && s_netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip.ip));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) out->rssi_dbm = ap.rssi;
    }
}

bool net_enabled(void)
{
    return WIFI_SSID[0] != 0;
}

bool net_connected(void)
{
    return s_connected;
}

time_t net_wall_time(uint32_t uptime_ms)
{
    time_t now = time(NULL);
    if (now < 1700000000) return 0;  /* before 2023: not synced yet */
    if (!uptime_ms) return now;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return now - (time_t)((now_ms - uptime_ms) / 1000);
}

void net_fmt_time(char *buf, int len, time_t t)
{
    if (!t) {
        buf[0] = 0;
        return;
    }
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
}
