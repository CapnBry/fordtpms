#include "ha_mqtt.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "net.h"
#include "net_config.h"
#include "tires.h"

static const char *TAG = "mqtt";

#define AVAIL_TOPIC WIFI_HOSTNAME "/status"

/* How often ended bursts are looked for: a reading goes out 3-3.25 s after
 * the sensor's last repeat. */
#define BURST_CHECK_MS 250

static esp_mqtt_client_handle_t s_client;
static esp_timer_handle_t s_burst_timer;
static SemaphoreHandle_t s_lock;
static volatile bool s_connected;
static uint32_t s_published, s_connects;
static char s_uri[96];

static void publish(const char *topic, const char *payload, bool retain)
{
    /* enqueue: never blocks the caller (radio task) on the network. */
    esp_mqtt_client_enqueue(s_client, topic, payload, 0, 0, retain, true);
}

/*
 * Home Assistant device discovery, one retained message per assigned sensor:
 * <prefix>/device/rtl_<id>/config. Unique IDs <id>_press / <id>_temp match
 * hand-made rtl_433 discovery, so existing entities and history carry on.
 * The device name carries the wheel position. Clearing a wheel only stops
 * publishing; the Home Assistant device is left alone (delete it there).
 */
static void publish_discovery(tire_pos_t pos, uint32_t id)
{
    if (!MQTT_DISCOVERY_PREFIX[0] || id == TIRE_NONE) return;
    char topic[96], payload[1400];
    snprintf(topic, sizeof(topic), MQTT_DISCOVERY_PREFIX "/device/rtl_%08x/config", (unsigned)id);
    snprintf(payload, sizeof(payload),
             "{\"dev\":{\"ids\":\"rtl_%08x\",\"name\":\"TPMS %s\",\"mf\":\"Ford\",\"mdl\":\"TPMS\",\"sn\":\"%08x\"},"
             "\"o\":{\"name\":\"" WIFI_HOSTNAME "\"},"
             "\"cmps\":{"
             "\"rtl_tpms_press\":{\"p\":\"sensor\",\"exp_aft\":64800,\"dev_cla\":\"pressure\",\"unit_of_meas\":\"psi\","
             "\"stat_cla\":\"measurement\",\"value_template\":\"{{value_json.pressure_PSI}}\",\"unique_id\":\"%08x_press\"},"
             "\"rtl_tpms_temp\":{\"p\":\"sensor\",\"exp_aft\":64800,\"dev_cla\":\"temperature\",\"unit_of_meas\":\"\xc2\xb0" "C\","
             "\"stat_cla\":\"measurement\",\"value_template\":\"{{value_json.temperature_C | default(None)}}\","
             "\"unique_id\":\"%08x_temp\"},"
             "\"rtl_tpms_moving\":{\"p\":\"binary_sensor\",\"exp_aft\":64800,\"dev_cla\":\"moving\","
             "\"value_template\":\"{{value_json.moving}}\",\"pl_on\":\"1\",\"pl_off\":\"0\",\"unique_id\":\"%08x_moving\"},"
             "\"rtl_tpms_rssi\":{\"p\":\"sensor\",\"exp_aft\":64800,\"dev_cla\":\"signal_strength\",\"unit_of_meas\":\"dBm\","
             "\"ent_cat\":\"diagnostic\",\"en\":false,\"value_template\":\"{{value_json.rssi}}\",\"unique_id\":\"%08x_rssi\"}"
             "},\"state_topic\":\"" MQTT_BASE_TOPIC "/rtl_%08x/state\",\"qos\":0}",
             (unsigned)id, tires_long_name(pos), (unsigned)id, (unsigned)id, (unsigned)id, (unsigned)id, (unsigned)id,
             (unsigned)id);
    publish(topic, payload, true);
}

static void publish_state(const sensor_entry_t *e)
{
    const tpms_ford_t *p = &e->pkt;
    char topic[64], payload[320], tbuf[24], timef[40] = "", tempf[24] = "";
    net_fmt_time(tbuf, sizeof(tbuf), net_wall_time(e->last_ms));
    if (tbuf[0]) snprintf(timef, sizeof(timef), "\"time\":\"%s\",", tbuf);
    if (p->temp_valid) snprintf(tempf, sizeof(tempf), ",\"temperature_C\":%d", p->temp_c);
    snprintf(topic, sizeof(topic), MQTT_BASE_TOPIC "/rtl_%08x/state", (unsigned)p->id);
    snprintf(payload, sizeof(payload),
             "{%s\"model\":\"Ford\",\"type\":\"TPMS\",\"id\":\"%08x\",\"pressure_PSI\":%g%s,"
             "\"moving\":%d,\"rssi\":%d}",
             timef, (unsigned)p->id, p->psi_x4 / 4.0, tempf, p->mode == TPMS_MODE_MOVING,
             e->rssi_best_dbm);
    publish(topic, payload, true);
    s_published++;
}

/*
 * One state message per burst, sent once the burst has ended so `rssi` is the
 * best of its repeats. While disconnected nothing is taken, so a burst that
 * ends then stays unpublished and goes out on the first check after reconnect.
 */
static void burst_check(void *arg)
{
    if (!s_connected) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    sensor_entry_t e;
    while (sensors_take_finished(now, &e)) {
        /* Only sensors the user assigned to a wheel are published. */
        if (tires_find(e.pkt.id) < 0) continue;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        publish_state(&e);
        xSemaphoreGive(s_lock);
    }
}

void ha_mqtt_tires_changed(uint32_t new_id)
{
    if (!s_client || !s_connected) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int pos = 0; pos < TIRE_COUNT; pos++) publish_discovery(pos, tires_get(pos));
    sensor_entry_t e;
    if (new_id != TIRE_NONE && sensors_get(new_id, &e)) publish_state(&e);
    xSemaphoreGive(s_lock);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "connected to %s", s_uri);
        s_connected = true;
        s_connects++;
        publish(AVAIL_TOPIC, "online", true);
        /* Re-announce discovery (also covers a broker restart). Readings are
         * retained; only ones never sent go out, via burst_check(). */
        ha_mqtt_tires_changed(TIRE_NONE);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) ESP_LOGW(TAG, "disconnected");
        s_connected = false;
        break;
    case MQTT_EVENT_ERROR: {
        esp_mqtt_event_handle_t ev = data;
        if (ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
            ESP_LOGW(TAG, "transport error (errno %d)", ev->error_handle->esp_transport_sock_errno);
        break;
    }
    default:
        break;
    }
}

void ha_mqtt_start(void)
{
    if (!MQTT_HOST[0] || s_client) return;
    s_lock = xSemaphoreCreateMutex();
    snprintf(s_uri, sizeof(s_uri), "mqtt://%s:%d", MQTT_HOST, MQTT_PORT);
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_uri,
        .credentials.client_id = WIFI_HOSTNAME,
        .credentials.username = MQTT_USERNAME[0] ? MQTT_USERNAME : NULL,
        .credentials.authentication.password = MQTT_PASSWORD[0] ? MQTT_PASSWORD : NULL,
        .session.last_will = {.topic = AVAIL_TOPIC, .msg = "offline", .retain = 1},
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 5000,
    };
    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "connecting to %s", s_uri);
    esp_timer_create_args_t ta = {.callback = burst_check, .name = "mqtt_burst"};
    esp_timer_create(&ta, &s_burst_timer);
    esp_timer_start_periodic(s_burst_timer, BURST_CHECK_MS * 1000ULL);
}

void ha_mqtt_get_status(ha_mqtt_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->enabled = MQTT_HOST[0] != 0;
    out->connected = s_connected;
    out->published = s_published;
    out->connects = s_connects;
    if (out->enabled) snprintf(out->uri, sizeof(out->uri), "mqtt://%s:%d", MQTT_HOST, MQTT_PORT);
}
