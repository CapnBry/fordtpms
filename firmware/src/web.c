#include "web.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gfx.h"
#include "ha_mqtt.h"
#include "net.h"
#include "radio.h"
#include "sensors.h"
#include "tires.h"

static const char *TAG = "web";

#define WEB_PUSH_MS            500   /* one message per tick: at most 2 Hz per client */
#define WEB_OLED_POLL_MS       100   /* display frame rate; frames are sent only when they changed */
#define WEB_SENSORS_REFRESH_MS 5000
#define WEB_CLIENT_TIMEOUT_MS  10000
#define WEB_MAX_CLIENTS        4
#define JSON_MAX               8192

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char oled_html_start[] asm("_binary_oled_html_start");
extern const char oled_html_end[] asm("_binary_oled_html_end");

static httpd_handle_t s_server;
static volatile bool s_send_sensors;

/* WebSocket clients and the last time each one sent anything (heartbeat).
 * A client that sent "oled" gets display frames instead of status messages. */
typedef struct {
    int fd;
    uint32_t last_rx_ms;
    bool oled;
    uint32_t oled_gen;  /* generation of the last frame sent (0 = none) */
} ws_client_t;
static ws_client_t s_clients[WEB_MAX_CLIENTS];
static portMUX_TYPE s_clients_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------- JSON ---- */

typedef struct {
    char *buf;
    size_t len, cap;
} jbuf_t;

static void jp(jbuf_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jp(jbuf_t *j, const char *fmt, ...)
{
    if (j->len >= j->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->buf + j->len, j->cap - j->len, fmt, ap);
    va_end(ap);
    if (n > 0) j->len += (size_t)n;
    if (j->len >= j->cap) j->len = j->cap;  /* truncated: caller checks */
}

static void json_sensors(jbuf_t *j);

/* Status object; with_sensors embeds the sensor table as "sensors". */
static void json_status(jbuf_t *j, bool with_sensors)
{
    radio_stats_t rs;
    radio_config_t rc;
    net_status_t ns;
    ha_mqtt_status_t ms;
    radio_get_stats(&rs);
    radio_get_config(&rc);
    net_get_status(&ns);
    ha_mqtt_get_status(&ms);
    int8_t hist[80];
    int nh = radio_rssi_history(hist, sizeof(hist));
    char tbuf[24];
    net_fmt_time(tbuf, sizeof(tbuf), net_wall_time(0));
    /* "rssi": peak of the RF samples (250 ms each) since the previous push */
    int peak = nh ? hist[nh - 1] : rs.noise_dbm;
    for (int i = nh - 2; i >= 0 && i >= nh - WEB_PUSH_MS / 250; i--)
        if (hist[i] > peak) peak = hist[i];
    jp(j, "{\"t\":\"status\",\"now\":%u,\"time\":\"%s\",\"freq\":%u,\"bw\":%u,\"noise\":%d,\"rssi\":%d,",
       (unsigned)now_ms(), tbuf, (unsigned)rc.freq_hz, (unsigned)rc.rxbw_hz, rs.noise_dbm, peak);
    jp(j, "\"pll\":%s,\"pll_unlocked\":%u,", rs.pll_locked ? "true" : "false", (unsigned)rs.pll_unlocked_polls);
    const radio_caps_t *caps = radio_caps();
    radio_config_t def;
    radio_default_config(&def);
    jp(j, "\"radio\":{\"name\":\"%s\",\"chip\":\"%s\",\"fmin\":%u,\"fmax\":%u,\"note\":\"%s\","
          "\"brmin\":%u,\"brmax\":%u,\"def\":{\"freq\":%u,\"bw\":%u,\"sync\":\"%s\",\"bitrate\":%u,\"drc\":%u},\"bws\":[",
       caps->name, rs.present ? rs.chip : "not found", (unsigned)caps->freq_min_hz, (unsigned)caps->freq_max_hz,
       caps->freq_note, (unsigned)caps->bitrate_min, (unsigned)caps->bitrate_max, (unsigned)def.freq_hz,
       (unsigned)def.rxbw_hz, def.sync == RADIO_SYNC_AAA9 ? "aaa9" : def.sync == RADIO_SYNC_5555 ? "5555" : "5556",
       (unsigned)def.bitrate, def.drate_comp);
    uint32_t bws[32];
    int nbw = radio_bandwidths(bws, 32);
    for (int i = 0; i < nbw; i++) jp(j, "%s%u", i ? "," : "", (unsigned)bws[i]);
    jp(j, "]},");
    jp(j, "\"hist\":[");
    for (int i = 0; i < nh; i++) jp(j, "%s%d", i ? "," : "", hist[i]);
    jp(j, "],\"syncs\":%u,\"payloads\":%u,\"decoded\":%u,\"err_manchester\":%u,\"err_checksum\":%u,"
          "\"err_flags\":%u,\"near_miss\":%u,\"overruns\":%u,\"last_rssi\":%d,\"best_rssi\":%d,\"worst_rssi\":%d,\"last_decode\":%u,",
       (unsigned)rs.syncs, (unsigned)rs.payloads, (unsigned)rs.decoded, (unsigned)rs.err_manchester,
       (unsigned)rs.err_checksum, (unsigned)rs.err_flags, (unsigned)rs.near_miss, (unsigned)rs.fifo_overruns,
       rs.last_rssi_dbm,
       rs.best_rssi_dbm, rs.worst_rssi_dbm, (unsigned)rs.last_decode_ms);
    display_settings_t dset;
    display_status_t dst;
    display_get_settings(&dset);
    display_get_status(&dst);
    jp(j, "\"sync\":\"%s\",\"bitrate\":%u,\"drc\":%u,\"disp\":{\"bright\":%u,\"dim\":%u,\"dim_after\":%u,\"shift\":%u,"
          "\"dimmed\":%s,\"present\":%s,\"screen\":\"%s\"},\"tires\":[",
       rc.sync == RADIO_SYNC_AAA9 ? "aaa9" : rc.sync == RADIO_SYNC_5555 ? "5555" : "5556", (unsigned)rc.bitrate,
       rc.drate_comp, dset.bright, dset.dim, (unsigned)dset.dim_after_s, (unsigned)dset.shift_period_s,
       dst.dimmed ? "true" : "false", dst.present ? "true" : "false", dst.screen ? dst.screen : "");
    for (int i = 0; i < TIRE_COUNT; i++) {
        uint32_t id = tires_get(i);
        if (id == TIRE_NONE) jp(j, "%s\"\"", i ? "," : "");
        else jp(j, "%s\"%08x\"", i ? "," : "", (unsigned)id);
    }
    jp(j, "],");
    jp(j, "\"ids\":%d,\"gen\":%u,\"heap\":%u,\"wifi_rssi\":%d,\"ip\":\"%s\",\"host\":\"%s\","
          "\"mqtt\":%d,\"mqtt_pub\":%u,\"clients\":%d",
       sensors_count(), (unsigned)sensors_generation(), (unsigned)esp_get_free_heap_size(), ns.rssi_dbm,
       ns.ip, ns.hostname, ms.enabled ? (ms.connected ? 1 : 0) : -1, (unsigned)ms.published, web_client_count());
    if (with_sensors) {
        jp(j, ",\"sensors\":");
        json_sensors(j);
    }
    jp(j, "}");
}

static void json_sensors(jbuf_t *j)
{
    sensor_entry_t snap[SENSORS_MAX];  /* on the stack: called from the push and httpd tasks */
    int n = sensors_snapshot(snap, SENSORS_MAX);
    jp(j, "{\"t\":\"sensors\",\"now\":%u,\"gen\":%u,\"max\":%d,\"list\":[", (unsigned)now_ms(),
       (unsigned)sensors_generation(), SENSORS_MAX);
    for (int i = 0; i < n; i++) {
        const sensor_entry_t *e = &snap[i];
        const tpms_ford_t *p = &e->pkt;
        char temp[8] = "null", wall[24];
        if (p->temp_valid) snprintf(temp, sizeof(temp), "%d", p->temp_c);
        net_fmt_time(wall, sizeof(wall), net_wall_time(e->last_ms));
        jp(j, "%s{\"id\":\"%08x\",\"psi\":%g,\"temp\":%s,\"mode\":\"%s\",\"code\":\"%06x\",\"flags\":\"%02x\","
              "\"rssi\":%d,\"bursts\":%u,\"pkts\":%u,\"burst_pkts\":%u,\"first\":%u,\"last\":%u,\"wall\":\"%s\"",
           i ? "," : "", (unsigned)p->id, p->psi_x4 / 4.0, temp, tpms_ford_mode_str(p->mode),
           (unsigned)(p->code & 0xffffff), p->flags, e->rssi_best_dbm, (unsigned)e->bursts, (unsigned)e->packets,
           e->burst_pkts, (unsigned)e->first_ms, (unsigned)e->last_ms, wall);
        /* Receive quality: frequency offsets in Hz, LQI lower = better. */
        if (e->q_n)
            jp(j, ",\"q_n\":%u,\"foff\":%d,\"foff_mean\":%d,\"foff_min\":%d,\"foff_max\":%d,\"lqi\":%u,"
                  "\"lqi_mean\":%.1f",
               (unsigned)e->q_n, (int)e->foff_hz, (int)(e->foff_sum_hz / (int64_t)e->q_n), (int)e->foff_min_hz,
               (int)e->foff_max_hz, e->lqi, (double)e->lqi_sum / e->q_n);
        jp(j, "}");
    }
    jp(j, "]}");
}

/* ---------------------------------------------------------- handlers --- */

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    /* EMBED_TXTFILES appends a NUL terminator. */
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t h_oled(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, oled_html_start, oled_html_end - oled_html_start - 1);
}

static esp_err_t h_state(httpd_req_t *req)
{
    jbuf_t j = {.buf = malloc(JSON_MAX * 2), .cap = JSON_MAX * 2};
    if (!j.buf) return httpd_resp_send_500(req);
    jp(&j, "{\"status\":");
    json_status(&j, false);
    jp(&j, ",\"sensors\":");
    json_sensors(&j);
    jp(&j, "}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = httpd_resp_send(req, j.buf, j.len);
    free(j.buf);
    return err;
}

static void json_events(jbuf_t *j, const radio_event_t *ev, int n)
{
    char wall[24];
    for (int i = 0; i < n; i++) {
        net_fmt_time(wall, sizeof(wall), net_wall_time(ev[i].start_ms));
        jp(j, "%s{\"start\":%u,\"wall\":\"%s\",\"dur_ms\":%u,\"peak\":%d,\"noise\":%d,\"syncs\":%u,"
              "\"payloads\":%u,\"decoded\":%u}",
           i ? "," : "", (unsigned)ev[i].start_ms, wall, (unsigned)(ev[i].end_ms - ev[i].start_ms), ev[i].peak_dbm,
           ev[i].noise_dbm, ev[i].syncs, ev[i].payloads, ev[i].decoded);
    }
}

/* GET /api/rflog: radio_get_events / radio_get_captures as JSON */
static esp_err_t h_rflog(httpd_req_t *req)
{
    radio_event_t *ev = malloc(sizeof(radio_event_t) * RADIO_EVENTS);
    radio_capture_t *cap = malloc(sizeof(radio_capture_t) * RADIO_CAPTURES);
    jbuf_t j = {.buf = malloc(16384), .cap = 16384};
    if (!ev || !cap || !j.buf) {
        free(ev); free(cap); free(j.buf);
        return httpd_resp_send_500(req);
    }
    uint32_t ev_total, cap_total;
    int ne = radio_get_events(ev, RADIO_EVENTS, &ev_total);
    int nc = radio_get_captures(cap, RADIO_CAPTURES, &cap_total);
    radio_stats_t rs;
    radio_config_t rc;
    radio_get_stats(&rs);
    radio_get_config(&rc);
    char wall[24];
    net_fmt_time(wall, sizeof(wall), net_wall_time(0));
    jp(&j, "{\"now\":%u,\"time\":\"%s\",\"noise\":%d,\"syncs\":%u,\"payloads\":%u,\"decoded\":%u,"
           "\"freq\":%u,\"pll\":%s,\"bw\":%u,\"bitrate\":%u,\"drc\":%u,\"sync\":\"%s\",\"event_db\":%d,"
           "\"capture_db\":%d,\"near_miss\":%u,\"near_miss_bad\":%d,\"bad_bit_hist\":[",
       (unsigned)now_ms(), wall, rs.noise_dbm, (unsigned)rs.syncs, (unsigned)rs.payloads, (unsigned)rs.decoded,
       (unsigned)rc.freq_hz, rs.pll_locked ? "true" : "false", (unsigned)rc.rxbw_hz, (unsigned)rc.bitrate,
       rc.drate_comp, rc.sync == RADIO_SYNC_AAA9 ? "aaa9" : rc.sync == RADIO_SYNC_5555 ? "5555" : "5556",
       RADIO_EVENT_DB, RADIO_CAPTURE_DB, (unsigned)rs.near_miss, RADIO_NEAR_MISS_BAD);
    for (int i = 0; i < TPMS_FORD_PAYLOAD_BITS; i++) jp(&j, "%s%u", i ? "," : "", rs.bad_bit_hist[i]);
    jp(&j, "],\"events_total\":%u,\"events\":[", (unsigned)ev_total);
    json_events(&j, ev, ne);
    ne = radio_get_top_events(ev, RADIO_TOP_EVENTS);
    jp(&j, "],\"top_events\":[");
    json_events(&j, ev, ne);
    jp(&j, "],\"captures_total\":%u,\"captures\":[", (unsigned)cap_total);
    for (int i = 0; i < nc; i++) {
        char hex[2 * RADIO_CAPTURE_MAX + 1];
        for (int k = 0; k < cap[i].len; k++) sprintf(hex + 2 * k, "%02x", cap[i].data[k]);
        net_fmt_time(wall, sizeof(wall), net_wall_time(cap[i].ms));
        jp(&j, "%s{\"ms\":%u,\"wall\":\"%s\",\"rssi\":%d,\"noise\":%d,\"status\":\"%s\",\"hex\":\"%s\"",
           i ? "," : "", (unsigned)cap[i].ms, wall, cap[i].rssi_dbm, cap[i].noise_dbm,
           tpms_ford_status_str((tpms_ford_status_t)cap[i].status), hex);
        if (cap[i].quality) jp(&j, ",\"foff\":%d,\"lqi\":%u", (int)cap[i].freq_off_hz, cap[i].lqi);
        /* Damaged bit positions, listed when few: a real packet with a few bad chips. */
        if (rc.sync != RADIO_SYNC_5555) {
            uint64_t bad = tpms_ford_bad_pairs(cap[i].data, cap[i].len * 8, 0);
            int nbad = __builtin_popcountll(bad);
            jp(&j, ",\"bad\":%d", nbad);
            if (nbad && nbad <= RADIO_NEAR_MISS_BAD) {
                jp(&j, ",\"bad_at\":[");
                for (int b = 0, k = 0; b < TPMS_FORD_PAYLOAD_BITS; b++)
                    if (bad >> b & 1) jp(&j, "%s%d", k++ ? "," : "", b);
                jp(&j, "]");
            }
        }
        jp(&j, "}");
    }
    jp(&j, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = j.len < j.cap ? httpd_resp_send(req, j.buf, j.len) : httpd_resp_send_500(req);
    free(ev); free(cap); free(j.buf);
    return err;
}

/* ------------------------------------------------------ settings API --- */

/* Reads an application/x-www-form-urlencoded body (small) into buf. */
static esp_err_t read_form(httpd_req_t *req, char *buf, size_t len)
{
    if (req->content_len >= len) return ESP_ERR_INVALID_SIZE;
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n <= 0) return ESP_FAIL;
        got += (size_t)n;
    }
    buf[got] = 0;
    return ESP_OK;
}

static bool form_get(const char *form, const char *key, char *val, size_t len)
{
    return httpd_query_key_value(form, key, val, len) == ESP_OK;
}

static esp_err_t reply(httpd_req_t *req, const char *err)
{
    char buf[256];
    httpd_resp_set_type(req, "application/json");
    if (err) {
        httpd_resp_set_status(req, "400 Bad Request");
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", err);
    } else {
        snprintf(buf, sizeof(buf), "{\"ok\":true}");
    }
    s_send_sensors = true;  /* push fresh state to every open page */
    return httpd_resp_sendstr(req, buf);
}

/* POST /api/radio: freq (MHz) bw (kHz) sync (5556|aaa9|5555) bitrate drc (0-3) | defaults=1 */
static esp_err_t h_radio(httpd_req_t *req)
{
    char form[256], v[24];
    if (read_form(req, form, sizeof(form)) != ESP_OK) return reply(req, "bad request body");
    radio_config_t c;
    radio_get_config(&c);
    if (form_get(form, "defaults", v, sizeof(v))) {
        radio_default_config(&c);
    } else {
        if (form_get(form, "freq", v, sizeof(v))) c.freq_hz = (uint32_t)(atof(v) * 1e6 + 0.5);
        /* The chip snaps the bandwidth to the nearest one it supports. */
        if (form_get(form, "bw", v, sizeof(v))) c.rxbw_hz = (uint32_t)(atof(v) * 1e3 + 0.5);
        if (form_get(form, "bitrate", v, sizeof(v))) c.bitrate = (uint32_t)atoi(v);
        if (form_get(form, "drc", v, sizeof(v))) {
            int d = atoi(v);
            if (d < 0 || d > 3) return reply(req, "drc must be 0 (off) .. 3 (+/-12.5 %)");
            c.drate_comp = (uint8_t)d;
        }
        if (form_get(form, "sync", v, sizeof(v))) {
            if (!strcmp(v, "5556")) c.sync = RADIO_SYNC_5556;
            else if (!strcmp(v, "aaa9")) c.sync = RADIO_SYNC_AAA9;
            else if (!strcmp(v, "5555")) c.sync = RADIO_SYNC_5555;
            else return reply(req, "sync must be 5556, aaa9 or 5555");
        }
    }
    esp_err_t err = app_set_radio(&c);
    const radio_caps_t *caps = radio_caps();
    char msg[160];
    if (err == ESP_ERR_INVALID_STATE) {
        snprintf(msg, sizeof(msg), "saved, but PLL NOT LOCKED: the %s cannot tune this frequency (%s)", caps->name,
                 caps->freq_note);
        return reply(req, msg);
    }
    if (err != ESP_OK) {
        snprintf(msg, sizeof(msg), "rejected: frequency %u-%u MHz, bitrate %u-%u",
                 (unsigned)(caps->freq_min_hz / 1000000), (unsigned)(caps->freq_max_hz / 1000000),
                 (unsigned)caps->bitrate_min, (unsigned)caps->bitrate_max);
        return reply(req, msg);
    }
    return reply(req, NULL);
}

/* POST /api/display: bright dim (0-255) dim_after (s) shift (s) | wake=1 | defaults=1 */
static esp_err_t h_display(httpd_req_t *req)
{
    char form[192], v[16];
    if (read_form(req, form, sizeof(form)) != ESP_OK) return reply(req, "bad request body");
    if (form_get(form, "wake", v, sizeof(v))) {
        display_poke();
        return reply(req, NULL);
    }
    display_settings_t s;
    display_get_settings(&s);
    if (form_get(form, "defaults", v, sizeof(v))) {
        display_default_settings(&s);
    } else {
        int n;
        if (form_get(form, "bright", v, sizeof(v))) {
            if ((n = atoi(v)) < 0 || n > 255) return reply(req, "bright must be 0-255");
            s.bright = (uint8_t)n;
        }
        if (form_get(form, "dim", v, sizeof(v))) {
            if ((n = atoi(v)) < 0 || n > 255) return reply(req, "dim must be 0-255");
            s.dim = (uint8_t)n;
        }
        if (form_get(form, "dim_after", v, sizeof(v))) {
            if ((n = atoi(v)) < 1 || n > 86400) return reply(req, "dim_after must be 1-86400 s");
            s.dim_after_s = (uint32_t)n;
        }
        if (form_get(form, "shift", v, sizeof(v))) {
            if ((n = atoi(v)) < 1 || n > 3600) return reply(req, "shift must be 1-3600 s");
            s.shift_period_s = (uint32_t)n;
        }
    }
    app_set_display(&s);
    display_poke();
    return reply(req, NULL);
}

/* POST /api/tire: pos (fl|fr|rl|rr) id (hex, empty = clear) */
static esp_err_t h_tire(httpd_req_t *req)
{
    char form[96], v[16];
    if (read_form(req, form, sizeof(form)) != ESP_OK) return reply(req, "bad request body");
    int pos = form_get(form, "pos", v, sizeof(v)) ? tires_parse(v) : -1;
    if (pos < 0) return reply(req, "pos must be fl, fr, rl or rr");
    uint32_t id = TIRE_NONE;
    if (form_get(form, "id", v, sizeof(v)) && v[0]) {
        char *end;
        id = strtoul(v, &end, 16);
        if (*end || strlen(v) > 8 || id == TIRE_NONE) return reply(req, "id must be 1-8 hex digits");
    }
    app_assign_tire((tire_pos_t)pos, id);
    return reply(req, NULL);
}

static void client_touch(int fd)
{
    taskENTER_CRITICAL(&s_clients_mux);
    ws_client_t *slot = NULL;
    for (int i = 0; i < WEB_MAX_CLIENTS; i++) {
        if (s_clients[i].fd == fd) { slot = &s_clients[i]; break; }
        if (!slot && s_clients[i].fd < 0) slot = &s_clients[i];
    }
    if (slot) {
        if (slot->fd != fd) *slot = (ws_client_t){.fd = fd};
        slot->last_rx_ms = now_ms();
    }
    taskEXIT_CRITICAL(&s_clients_mux);
}

static void client_subscribe_oled(int fd)
{
    taskENTER_CRITICAL(&s_clients_mux);
    for (int i = 0; i < WEB_MAX_CLIENTS; i++) {
        if (s_clients[i].fd == fd) {
            s_clients[i].oled = true;
            s_clients[i].oled_gen = 0;
        }
    }
    taskEXIT_CRITICAL(&s_clients_mux);
}

static void client_forget(int fd)
{
    taskENTER_CRITICAL(&s_clients_mux);
    for (int i = 0; i < WEB_MAX_CLIENTS; i++)
        if (s_clients[i].fd == fd) s_clients[i].fd = -1;
    taskEXIT_CRITICAL(&s_clients_mux);
}

static esp_err_t ws_connected(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    ESP_LOGI(TAG, "websocket client connected (fd %d)", fd);
    client_touch(fd);
    s_send_sensors = true;
    return ESP_OK;
}

static esp_err_t h_ws(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    httpd_ws_frame_t f = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK) return err;
    char buf[32] = {0};
    if (f.len >= sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    if (f.len) {
        f.payload = (uint8_t *)buf;
        if ((err = httpd_ws_recv_frame(req, &f, f.len)) != ESP_OK) return err;
    }
    client_touch(fd);
    if (f.type == HTTPD_WS_TYPE_TEXT && !strcmp(buf, "ping")) {
        httpd_ws_frame_t pong = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)"{\"t\":\"pong\"}",
                                 .len = 12, .final = true};
        return httpd_ws_send_frame(req, &pong);
    }
    if (f.type == HTTPD_WS_TYPE_TEXT && !strcmp(buf, "sensors")) s_send_sensors = true;
    if (f.type == HTTPD_WS_TYPE_TEXT && !strcmp(buf, "oled")) client_subscribe_oled(fd);
    return ESP_OK;
}

static void on_close(httpd_handle_t hd, int fd)
{
    client_forget(fd);
    close(fd);
}

/* ------------------------------------------------------------- push ---- */

int web_client_count(void)
{
    int n = 0;
    for (int i = 0; i < WEB_MAX_CLIENTS; i++) n += s_clients[i].fd >= 0;
    return n;
}

/* Sends to client slot i; drops the client if it is gone, silent or failing. */
static bool client_send(int i, httpd_ws_frame_t *f)
{
    int fd = s_clients[i].fd;
    if (fd < 0) return false;
    if (httpd_ws_get_fd_info(s_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        client_forget(fd);
        return false;
    }
    if (now_ms() - s_clients[i].last_rx_ms > WEB_CLIENT_TIMEOUT_MS) {
        ESP_LOGW(TAG, "websocket fd %d missed heartbeats, closing", fd);
        client_forget(fd);
        httpd_sess_trigger_close(s_server, fd);
        return false;
    }
    if (httpd_ws_send_data(s_server, fd, f) != ESP_OK) {
        ESP_LOGW(TAG, "websocket fd %d send failed, closing", fd);
        client_forget(fd);
        httpd_sess_trigger_close(s_server, fd);
        return false;
    }
    return true;
}

/* Status messages go to every client except the OLED viewers. */
static void broadcast(const char *msg, size_t len)
{
    httpd_ws_frame_t f = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)msg, .len = len, .final = true};
    for (int i = 0; i < WEB_MAX_CLIENTS; i++)
        if (!s_clients[i].oled) client_send(i, &f);
}

/*
 * OLED frame, binary: 8 byte header (width, height, flags: bit 0 panel
 * present, bit 1 dimmed; 0; generation, u32 LE), then the framebuffer as-is
 * (page-major: byte [y/8 * width + x], bit y%8).
 */
static void push_oled(void)
{
    static uint8_t buf[8 + GFX_FB_SIZE];
    uint32_t gen = display_frame_gen(), have = 0;
    for (int i = 0; i < WEB_MAX_CLIENTS; i++) {
        if (s_clients[i].fd < 0 || !s_clients[i].oled || s_clients[i].oled_gen == gen) continue;
        if (!have) {
            display_status_t ds;
            display_get_status(&ds);
            have = display_snapshot(buf + 8);
            buf[0] = GFX_W;
            buf[1] = GFX_H;
            buf[2] = (ds.present ? 1 : 0) | (ds.dimmed ? 2 : 0);
            buf[3] = 0;
            memcpy(buf + 4, &have, 4);
        }
        httpd_ws_frame_t f = {.type = HTTPD_WS_TYPE_BINARY, .payload = buf, .len = sizeof(buf), .final = true};
        if (client_send(i, &f)) s_clients[i].oled_gen = have;
    }
}

static void push_task(void *arg)
{
    static char buf[JSON_MAX];
    uint32_t last_gen = 0, last_sensors = 0, last_push = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WEB_OLED_POLL_MS));
        if (!web_client_count()) continue;
        push_oled();
        if (now_ms() - last_push < WEB_PUSH_MS - WEB_OLED_POLL_MS / 2) continue;
        last_push = now_ms();
        /* One message per tick; the sensor table rides along when it changed
         * (or a page asked for it), and at least every WEB_SENSORS_REFRESH_MS. */
        uint32_t gen = sensors_generation(), now = now_ms();
        bool with_sensors = s_send_sensors || gen != last_gen || now - last_sensors >= WEB_SENSORS_REFRESH_MS;
        if (with_sensors) {
            s_send_sensors = false;
            last_gen = gen;
            last_sensors = now;
        }
        jbuf_t j = {.buf = buf, .cap = sizeof(buf)};
        json_status(&j, with_sensors);
        if (j.len < j.cap) broadcast(buf, j.len);
        else ESP_LOGW(TAG, "status JSON truncated (%u bytes)", (unsigned)j.cap);
    }
}

void web_start(void)
{
    if (s_server) return;
    for (int i = 0; i < WEB_MAX_CLIENTS; i++) s_clients[i].fd = -1;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = WEB_MAX_CLIENTS + 3;
    cfg.lru_purge_enable = true;
    cfg.close_fn = on_close;
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 12;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/oled", .method = HTTP_GET, .handler = h_oled},
        {.uri = "/api/state", .method = HTTP_GET, .handler = h_state},
        {.uri = "/api/rflog", .method = HTTP_GET, .handler = h_rflog},
        {.uri = "/api/radio", .method = HTTP_POST, .handler = h_radio},
        {.uri = "/api/display", .method = HTTP_POST, .handler = h_display},
        {.uri = "/api/tire", .method = HTTP_POST, .handler = h_tire},
        {.uri = "/ws", .method = HTTP_GET, .handler = h_ws, .is_websocket = true,
         .ws_post_handshake_cb = ws_connected},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_server, &uris[i]);
    xTaskCreatePinnedToCore(push_task, "web", 6144, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "web UI on port 80");
}
