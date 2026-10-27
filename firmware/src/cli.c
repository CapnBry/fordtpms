#include "cli.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha_mqtt.h"
#include "net.h"
#include "radio.h"
#include "sensors.h"
#include "ui.h"
#include "web.h"

static const char *sync_name(radio_sync_mode_t s)
{
    return s == RADIO_SYNC_AAA9 ? "aaa9" : s == RADIO_SYNC_5555 ? "5555" : "5556";
}

static void fmt_uptime(char *buf, size_t len, uint32_t ms)
{
    uint32_t s = ms / 1000;
    snprintf(buf, len, "%ud %02u:%02u:%02u", (unsigned)(s / 86400), (unsigned)(s / 3600 % 24),
             (unsigned)(s / 60 % 60), (unsigned)(s % 60));
}

/* ------------------------------------------------------------ metrics -- */

static void print_afc(const char *prefix)
{
    radio_afc_t a;
    radio_get_afc(&a);
    printf("%s%s, est %+d Hz over %u pkts, applied %+d Hz\n", prefix, a.on ? "on" : "off", (int)a.est_hz,
           (unsigned)a.pkts, (int)a.applied_hz);
}

static int cmd_metrics(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "reset")) {
        radio_reset_stats();
        printf("radio counters reset\n");
        return 0;
    }
    radio_stats_t rs;
    radio_config_t rc;
    display_status_t ds;
    radio_get_stats(&rs);
    radio_get_config(&rc);
    display_get_status(&ds);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    char up[24];
    fmt_uptime(up, sizeof(up), now);
    const esp_app_desc_t *app = esp_app_get_description();

    printf("firmware      %s %s (IDF %s)\n", app->project_name, app->version, app->idf_ver);
    printf("uptime        %s\n", up);
    printf("heap free     %u (min %u)\n", (unsigned)esp_get_free_heap_size(),
           (unsigned)esp_get_minimum_free_heap_size());
    net_status_t ns;
    net_get_status(&ns);
    if (!ns.enabled) printf("wifi          off (no SSID in wifi_config.h)\n");
    else if (!ns.connected) printf("wifi          connecting to \"%s\"\n", ns.ssid);
    else printf("wifi          %s, %d dBm, http://%s/ (%s.local)\n", ns.ip, ns.rssi_dbm, ns.ip, ns.hostname);
    printf("--- radio (%s) ---\n", rs.present ? rs.chip : "NOT FOUND");
    printf("frequency     %.3f MHz\n", rc.freq_hz / 1e6);
    if (radio_caps()->freq_offset) print_afc("afc           ");
    if (rs.pll_locked) printf("pll           locked (%u checks unlocked since reset)\n", (unsigned)rs.pll_unlocked_polls);
    else printf("pll           NOT LOCKED: receiver is not tuned and hears nothing (%u checks unlocked)\n",
                (unsigned)rs.pll_unlocked_polls);
    printf("chip rate     %u /s (drate comp %s), rx bw %u Hz, sync %s\n", (unsigned)rc.bitrate,
           radio_drate_comp_str(rc.drate_comp), (unsigned)rc.rxbw_hz, sync_name(rc.sync));
    printf("noise floor   %d dBm\n", rs.noise_dbm);
    printf("sync matches  %u\n", (unsigned)rs.syncs);
    printf("payloads      %u\n", (unsigned)rs.payloads);
    printf("decoded ok    %u\n", (unsigned)rs.decoded);
    printf("err manchest. %u\n", (unsigned)rs.err_manchester);
    printf("err checksum  %u\n", (unsigned)rs.err_checksum);
    printf("err flags     %u\n", (unsigned)rs.err_flags);
    printf("fifo overrun  %u\n", (unsigned)rs.fifo_overruns);
    printf("irq pay/sync  %u / %u (polled %u)\n", (unsigned)rs.irq_payload, (unsigned)rs.irq_sync,
           (unsigned)rs.polled_events);
    printf("last rssi     %d dBm\n", rs.last_rssi_dbm);
    if (rs.decoded) {
        char age[16];
        ui_fmt_age(age, sizeof(age), now - rs.last_decode_ms);
        printf("rssi best/wst %d / %d dBm\n", rs.best_rssi_dbm, rs.worst_rssi_dbm);
        printf("last packet   %s ago\n", age);
    }
    if (!ds.present) return 0;
    printf("--- display ---\n");
    printf("present       %s\n", ds.present ? "yes" : "no");
    printf("screen        %s, %s, shift (%d,%d)\n", ds.screen ? ds.screen : "-", ds.dimmed ? "dimmed" : "bright",
           ds.ox, ds.oy);
    printf("frames        %u (%u us/frame), i2c errors %u\n", (unsigned)ds.frames, (unsigned)ds.frame_us,
           (unsigned)ds.i2c_errors);
    return 0;
}

/* ------------------------------------------------------------ sensors -- */

static int cmd_sensors(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "clear")) {
        sensors_clear();
        printf("sensor table cleared\n");
        return 0;
    }
    static sensor_entry_t snap[SENSORS_MAX];
    int n = sensors_snapshot(snap, SENSORS_MAX);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    /* Header, rule and rows all use the same column widths (SENSOR_COLS). */
#define SENSOR_COLS "%2s %-8s %6s %4s %5s %-6s %-6s %3s %4s %6s %5s %-8s %6s\n"
    printf(SENSOR_COLS, "#", "ID", "PSI", "kPa", "Temp", "Mode", "Code", "Flg", "RSSI", "Bursts", "Pkts", "Last",
           "Age");
    printf(SENSOR_COLS, "--", "--------", "------", "----", "-----", "------", "------", "---", "----", "------",
           "-----", "--------", "------");
    for (int i = 0; i < n; i++) {
        const sensor_entry_t *e = &snap[i];
        char temp[8], age[12], at[12];
        if (e->pkt.temp_valid) snprintf(temp, sizeof(temp), "%dC", e->pkt.temp_c);
        else snprintf(temp, sizeof(temp), "--");
        ui_fmt_age(age, sizeof(age), now - e->last_ms);
        uint32_t s = e->last_ms / 1000;
        snprintf(at, sizeof(at), "%02u:%02u:%02u", (unsigned)(s / 3600 % 100), (unsigned)(s / 60 % 60),
                 (unsigned)(s % 60));
        char num[5][12], id[9], code[7], flg[3];
        snprintf(num[0], sizeof(num[0]), "%d", i + 1);
        snprintf(id, sizeof(id), "%08x", (unsigned)e->pkt.id);
        snprintf(num[1], sizeof(num[1]), "%.2f", e->pkt.psi_x4 / 4.0);
        snprintf(num[2], sizeof(num[2]), "%u", (unsigned)((e->pkt.psi_x4 * 6895u + 2000) / 4000));
        snprintf(code, sizeof(code), "%06x", (unsigned)(e->pkt.code & 0xffffff));
        snprintf(flg, sizeof(flg), "%02x", e->pkt.flags);
        snprintf(num[3], sizeof(num[3]), "%d", e->rssi_best_dbm);
        snprintf(num[4], sizeof(num[4]), "%u", (unsigned)(e->bursts % 1000000));
        char pkts[12];
        snprintf(pkts, sizeof(pkts), "%u", (unsigned)(e->packets % 100000));
        printf(SENSOR_COLS, num[0], id, num[1], num[2], temp, tpms_ford_mode_str(e->pkt.mode), code, flg, num[3],
               num[4], pkts, at, age);
    }
    if (!n) printf("(no sensors received yet)\n");
    printf("%d of max %d unique IDs; Last = uptime of last packet\n", n, SENSORS_MAX);
    bool hdr = false;
    for (int i = 0; i < n; i++) {
        const sensor_entry_t *e = &snap[i];
        if (!e->q_n) continue;
        if (!hdr) printf("\nreceive quality (freq offset in kHz; LQI lower = better):\n");
        hdr = true;
        printf("%08x  last burst %u/4 pkts  foff %+.1f mean %+.1f (%+.1f..%+.1f)  lqi %u mean %.1f  n=%u\n",
               (unsigned)e->pkt.id, e->burst_pkts, e->foff_hz / 1e3, e->foff_sum_hz / (double)e->q_n / 1e3,
               e->foff_min_hz / 1e3, e->foff_max_hz / 1e3, e->lqi, (double)e->lqi_sum / e->q_n, (unsigned)e->q_n);
    }
    return 0;
}

/* --------------------------------------------------------------- wifi -- */

static int cmd_wifi(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "sleep")) {
        if (!strcmp(argv[2], "on") || !strcmp(argv[2], "1")) app_set_wifi_ps(true);
        else if (!strcmp(argv[2], "off") || !strcmp(argv[2], "0")) app_set_wifi_ps(false);
        else { printf("sleep must be on or off\n"); return 1; }
        printf("wifi sleep %s (saved)\n", net_powersave() ? "on" : "off");
        return 0;
    }
    if (argc > 1) {
        printf("usage: wifi [sleep on|off]\n");
        return 1;
    }
    net_status_t ns;
    ha_mqtt_status_t ms;
    net_get_status(&ns);
    ha_mqtt_get_status(&ms);
    if (!ns.enabled) {
        printf("wifi off: no SSID in wifi_config.h\n");
        return 0;
    }
    printf("ssid          %s\n", ns.ssid);
    printf("state         %s\n", ns.connected ? "connected" : "connecting");
    printf("hostname      %s (mDNS %s.local)\n", ns.hostname, ns.hostname);
    if (ns.connected) {
        printf("ip            %s\n", ns.ip);
        printf("ap rssi       %d dBm\n", ns.rssi_dbm);
        printf("web ui        http://%s/  (%d live clients)\n", ns.ip, web_client_count());
    }
    printf("reconnects    %u\n", (unsigned)ns.reconnects);
    printf("sleep         %s\n", net_powersave() ? "on (modem power saving)" : "off");
    char t[24];
    net_fmt_time(t, sizeof(t), net_wall_time(0));
    printf("time          %s\n", t[0] ? t : "not synced");
    if (!ms.enabled) printf("mqtt          off (no MQTT_HOST)\n");
    else
        printf("mqtt          %s %s, %u published, %u connects\n", ms.uri,
               ms.connected ? "connected" : "disconnected", (unsigned)ms.published, (unsigned)ms.connects);
    return 0;
}

/* -------------------------------------------------------------- tires -- */

static int cmd_tires(int argc, char **argv)
{
    if (argc == 3) {
        int pos = tires_parse(argv[1]);
        char *end;
        uint32_t id = strtoul(argv[2], &end, 16);
        if (pos < 0) {
            printf("position must be fl, fr, rl or rr\n");
            return 1;
        }
        if (!strcmp(argv[2], "clear") || !strcmp(argv[2], "none")) id = TIRE_NONE;
        else if (*end || strlen(argv[2]) > 8 || id == TIRE_NONE) {
            printf("id must be 1-8 hex digits, or 'clear'\n");
            return 1;
        }
        app_assign_tire((tire_pos_t)pos, id);
    } else if (argc != 1) {
        printf("usage: tires [fl|fr|rl|rr <id|clear>]\n");
        return 1;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    for (int pos = 0; pos < TIRE_COUNT; pos++) {
        uint32_t id = tires_get(pos);
        sensor_entry_t e;
        printf("%-2s %-11s ", tires_short_name(pos), tires_long_name(pos));
        if (id == TIRE_NONE) printf("(none)\n");
        else if (!sensors_get(id, &e)) printf("%08x  not heard since boot\n", (unsigned)id);
        else {
            char age[12];
            ui_fmt_age(age, sizeof(age), now - e.last_ms);
            printf("%08x  %6.2f PSI  %s ago\n", (unsigned)id, e.pkt.psi_x4 / 4.0, age);
        }
    }
    printf("assigned IDs are published over MQTT; saved in flash\n");
    return 0;
}

/* -------------------------------------------------------------- radio -- */

static void pll_warning(uint32_t freq_hz)
{
    printf("WARNING: PLL NOT LOCKED at %.3f MHz. The %s cannot tune here (%s); the receiver hears nothing. "
           "Settings were applied and saved anyway.\n", freq_hz / 1e6, radio_caps()->name, radio_caps()->freq_note);
}

static int cmd_radio(int argc, char **argv)
{
    radio_config_t c;
    radio_get_config(&c);
    if (argc == 2 && !strcmp(argv[1], "defaults")) {
        radio_default_config(&c);
        esp_err_t err = app_set_radio(&c);
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) app_set_afc(true);
        printf("radio settings reset to defaults\n");
        if (err == ESP_ERR_INVALID_STATE) pll_warning(c.freq_hz);
        return err == ESP_OK ? 0 : 1;
    }
    if (argc == 1) {
        printf("freq %.3f MHz, bitrate %u, drc %u (%s), bw %u Hz, sync %s\n", c.freq_hz / 1e6, (unsigned)c.bitrate,
               c.drate_comp, radio_drate_comp_str(c.drate_comp), (unsigned)c.rxbw_hz, sync_name(c.sync));
        if (radio_caps()->freq_offset) print_afc("afc ");
        return 0;
    }
    int afc = -1;
    for (int i = 1; i + 1 < argc; i += 2) {
        const char *k = argv[i], *v = argv[i + 1];
        if (!strcmp(k, "freq")) c.freq_hz = (uint32_t)(atof(v) * 1e6 + 0.5);
        else if (!strcmp(k, "bw")) c.rxbw_hz = (uint32_t)(atof(v) * 1e3 + 0.5);
        else if (!strcmp(k, "bitrate")) c.bitrate = (uint32_t)atoi(v);
        else if (!strcmp(k, "drc")) c.drate_comp = (uint8_t)atoi(v);  /* > 3 is rejected */
        else if (!strcmp(k, "afc")) {
            if (!strcmp(v, "on") || !strcmp(v, "1")) afc = 1;
            else if (!strcmp(v, "off") || !strcmp(v, "0")) afc = 0;
            else { printf("afc must be on or off\n"); return 1; }
            if (afc && !radio_caps()->freq_offset) { printf("the %s measures no frequency offset\n", radio_caps()->name); return 1; }
        }
        else if (!strcmp(k, "sync")) {
            if (!strcmp(v, "5556")) c.sync = RADIO_SYNC_5556;
            else if (!strcmp(v, "aaa9")) c.sync = RADIO_SYNC_AAA9;
            else if (!strcmp(v, "5555")) c.sync = RADIO_SYNC_5555;
            else { printf("sync must be 5556, aaa9 or 5555\n"); return 1; }
        } else {
            printf("unknown setting '%s'\n", k);
            return 1;
        }
    }
    if ((argc - 1) % 2) {
        printf("expected <key> <value> pairs\n");
        return 1;
    }
    esp_err_t err = app_set_radio(&c);
    if (afc >= 0 && (err == ESP_OK || err == ESP_ERR_INVALID_STATE)) app_set_afc(afc);
    if (err == ESP_ERR_INVALID_STATE) {
        pll_warning(c.freq_hz);
        return 1;
    }
    if (err != ESP_OK) {
        const radio_caps_t *caps = radio_caps();
        printf("invalid: %s (frequency %.0f-%.0f MHz, bitrate %u-%u, drc 0-3)\n", esp_err_to_name(err),
               caps->freq_min_hz / 1e6, caps->freq_max_hz / 1e6, (unsigned)caps->bitrate_min,
               (unsigned)caps->bitrate_max);
        return 1;
    }
    return 0;
}

static int cmd_regs(int argc, char **argv)
{
    radio_dump_regs(stdout);
    return 0;
}

static int cmd_probe(int argc, char **argv)
{
    radio_probe(stdout);
    return 0;
}

static int cmd_pllscan(int argc, char **argv)
{
    static uint32_t f[400];
    int n = 0;
    if (argc == 1) {
        /* The 315 MHz TPMS band plus reference points; 100 and 1000 MHz are outside the
         * sub-GHz synthesizer ranges, so they show what a failed lock looks like. */
        static const double ref[] = {100, 137, 175, 250, 433.92, 868, 915, 1000};
        for (size_t i = 0; i < sizeof(ref) / sizeof(ref[0]); i++) f[n++] = (uint32_t)(ref[i] * 1e6 + 0.5);
        for (int k = 300; k <= 330; k += 5) f[n++] = (uint32_t)k * 1000000u;
    } else if (argc == 4) {
        double start = atof(argv[1]), stop = atof(argv[2]), step = atof(argv[3]);
        if (start < 1 || stop < start || step < 0.001 || stop > 3000 || (stop - start) / step >= 400) {
            printf("need 1 <= start <= stop <= 3000 MHz and at most 400 steps\n");
            return 1;
        }
        for (double x = start; x <= stop + 1e-9 && n < 400; x += step) f[n++] = (uint32_t)(x * 1e6 + 0.5);
    } else {
        printf("usage: pllscan [start_MHz stop_MHz step_MHz]\n");
        return 1;
    }
    radio_pll_scan(f, n, stdout);
    return 0;
}

static int cmd_raw(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off"))) {
        printf("usage: raw on|off\n");
        return 1;
    }
    radio_set_raw_log(!strcmp(argv[1], "on"));
    return 0;
}

/* -------------------------------------------------------------- rflog -- */

static void fmt_when(char *buf, size_t len, uint32_t ms)
{
    char wall[24], up[24];
    net_fmt_time(wall, sizeof(wall), net_wall_time(ms));
    fmt_uptime(up, sizeof(up), ms);
    snprintf(buf, len, "%s", wall[0] ? wall : up);
}

static int cmd_rflog(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "clear")) {
        radio_clear_log();
        printf("RF log cleared\n");
        return 0;
    }
    static radio_event_t ev[RADIO_EVENTS];
    static radio_capture_t cap[RADIO_CAPTURES];
    uint32_t ev_total, cap_total;
    int ne = radio_get_events(ev, RADIO_EVENTS, &ev_total);
    int nc = radio_get_captures(cap, RADIO_CAPTURES, &cap_total);
    char when[24];

    printf("RF events (peak >= noise + %d dB), %u since boot, last %d:\n", RADIO_EVENT_DB, (unsigned)ev_total, ne);
    printf("%-19s %6s %5s %5s %5s %5s %5s\n", "start", "dur ms", "peak", "noise", "syncs", "pays", "dec");
    for (int i = 0; i < ne; i++) {
        fmt_when(when, sizeof(when), ev[i].start_ms);
        printf("%-19s %6u %5d %5d %5u %5u %5u\n", when, (unsigned)(ev[i].end_ms - ev[i].start_ms), ev[i].peak_dbm,
               ev[i].noise_dbm, ev[i].syncs, ev[i].payloads, ev[i].decoded);
    }
    static radio_event_t top[RADIO_TOP_EVENTS];
    int nt = radio_get_top_events(top, RADIO_TOP_EVENTS);
    printf("\nStrongest %d events since boot:\n", nt);
    for (int i = 0; i < nt; i++) {
        fmt_when(when, sizeof(when), top[i].start_ms);
        printf("%-19s %6u %5d %5d %5u %5u %5u\n", when, (unsigned)(top[i].end_ms - top[i].start_ms),
               top[i].peak_dbm, top[i].noise_dbm, top[i].syncs, top[i].payloads, top[i].decoded);
    }
    printf("\nStrong payloads (sync RSSI >= noise + %d dB), %u since boot, last %d:\n", RADIO_CAPTURE_DB,
           (unsigned)cap_total, nc);
    for (int i = 0; i < nc; i++) {
        char hex[2 * RADIO_CAPTURE_MAX + 1];
        for (int k = 0; k < cap[i].len; k++) sprintf(hex + 2 * k, "%02x", cap[i].data[k]);
        fmt_when(when, sizeof(when), cap[i].ms);
        char q[32] = "";
        if (cap[i].quality) snprintf(q, sizeof(q), " foff %+.1f kHz lqi %u", cap[i].freq_off_hz / 1e3, cap[i].lqi);
        printf("%-19s %4d dBm (noise %d) %-10s %s%s\n", when, cap[i].rssi_dbm, cap[i].noise_dbm,
               tpms_ford_status_str((tpms_ford_status_t)cap[i].status), hex, q);
    }
    printf("(also at http://<ip>/api/rflog)\n");
    return 0;
}

/* ------------------------------------------------------------- inject -- */

static int cmd_inject(int argc, char **argv)
{
    /* Default: a real packet from rtl_433_tests (gfile059), 26.5 PSI, moving. */
    uint8_t b[8] = {0x45, 0xbb, 0x32, 0x0f, 0x6a, 0xd4, 0x46, 0};
    bool fix = true;
    int rssi = -80;
    if (argc > 1) {
        const char *h = argv[1];
        size_t len = strlen(h);
        if (len != 14 && len != 16) {
            printf("usage: inject [7 or 8 hex bytes] [rssi]  (7 bytes: checksum is added)\n");
            return 1;
        }
        for (size_t i = 0; i < len / 2; i++) {
            char t[3] = {h[2 * i], h[2 * i + 1], 0};
            if (!isxdigit((unsigned char)t[0]) || !isxdigit((unsigned char)t[1])) {
                printf("bad hex\n");
                return 1;
            }
            b[i] = (uint8_t)strtoul(t, NULL, 16);
        }
        fix = len == 14;
    }
    if (argc > 2) rssi = atoi(argv[2]);
    if (fix) tpms_ford_fix_checksum(b);
    tpms_ford_t pkt;
    tpms_ford_status_t st = tpms_ford_decode_bytes(b, &pkt);
    if (st != TPMS_FORD_OK) {
        printf("rejected: %s\n", tpms_ford_status_str(st));
        return 1;
    }
    radio_rx_info_t rx = {.rssi_dbm = rssi};  /* no quality data: injected packets stay out of the offset stats */
    app_on_packet(&pkt, &rx, (uint32_t)(esp_timer_get_time() / 1000));
    return 0;
}

/* ----------------------------------------------------------------- tx -- */

static bool parse_hex(const char *h, uint8_t *b, size_t *n)
{
    size_t len = strlen(h);
    if (len != 14 && len != 16) return false;
    for (size_t i = 0; i < len / 2; i++) {
        char t[3] = {h[2 * i], h[2 * i + 1], 0};
        if (!isxdigit((unsigned char)t[0]) || !isxdigit((unsigned char)t[1])) return false;
        b[i] = (uint8_t)strtoul(t, NULL, 16);
    }
    *n = len / 2;
    return true;
}

static int cmd_tx(int argc, char **argv)
{
    /* Default: fake ID feedf00d, 38 PSI, 29 C, rest (same code as a real sensor here). */
    uint8_t b[8] = {0xfe, 0xed, 0xf0, 0x0d, 0x98, 0x55, 0x06, 0};
    int count = 4, gap_ms = 50, dbm = radio_caps()->tx_default_dbm;
    bool pa_boost = true, fix = true;
    for (int i = 1; i < argc; i++) {
        size_t n;
        if (i + 1 < argc && !strcmp(argv[i], "count")) count = atoi(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "gap")) gap_ms = atoi(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "pa")) pa_boost = true, dbm = atoi(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "rfo")) pa_boost = false, dbm = atoi(argv[++i]);
        else if (parse_hex(argv[i], b, &n)) fix = n == 7;
        else {
            printf("usage: tx [7 or 8 hex bytes] [count n] [gap ms] [pa dBm | rfo dBm]\n  %s: %s\n",
                   radio_caps()->name, radio_caps()->tx_note);
            return 1;
        }
    }
    if (fix) tpms_ford_fix_checksum(b);
    tpms_ford_t pkt;
    tpms_ford_status_t st = tpms_ford_decode_bytes(b, &pkt);
    if (st != TPMS_FORD_OK) printf("warning: payload does not decode (%s), sending anyway\n", tpms_ford_status_str(st));
    if (!radio_caps()->tx) {
        printf("the %s transmit test is not implemented\n", radio_caps()->name);
        return 1;
    }
    radio_stats_t rs;
    radio_get_stats(&rs);
    if (!rs.pll_locked) printf("warning: PLL not locked at this frequency, nothing will be transmitted on it\n");
    esp_err_t err = radio_tx_ford(b, count, gap_ms, pa_boost, dbm);
    printf("tx %02x%02x%02x%02x%02x%02x%02x%02x x%d %s %d dBm: %s\n", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
           count, pa_boost ? "pa" : "rfo", dbm, esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

/* ------------------------------------------------------------ display -- */

static int cmd_display(int argc, char **argv)
{
    display_settings_t s;
    display_get_settings(&s);
    if (argc == 1) {
        printf("bright %u, dim %u, dim_after %u s, shift %u s\n", s.bright, s.dim, (unsigned)s.dim_after_s,
               (unsigned)s.shift_period_s);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "defaults")) {
        display_default_settings(&s);
        app_set_display(&s);
        printf("display settings reset to defaults\n");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "wake")) {
        display_poke();
        return 0;
    }
    for (int i = 1; i + 1 < argc; i += 2) {
        int v = atoi(argv[i + 1]);
        if (!strcmp(argv[i], "bright")) s.bright = (uint8_t)v;
        else if (!strcmp(argv[i], "dim")) s.dim = (uint8_t)v;
        else if (!strcmp(argv[i], "dim_after")) s.dim_after_s = (uint32_t)v;
        else if (!strcmp(argv[i], "shift")) s.shift_period_s = v > 0 ? (uint32_t)v : 1;
        else {
            printf("unknown setting '%s'\n", argv[i]);
            return 1;
        }
    }
    app_set_display(&s);
    return 0;
}

/* ----------------------------------------------------------- loglevel -- */

static int cmd_loglevel(int argc, char **argv)
{
    static const char *names[] = {"none", "error", "warn", "info", "debug", "verbose"};
    if (argc != 3) {
        printf("usage: loglevel <tag|*> <none|error|warn|info|debug|verbose>\n");
        return 1;
    }
    for (int i = 0; i < 6; i++) {
        if (!strcmp(argv[2], names[i])) {
            esp_log_level_set(argv[1], (esp_log_level_t)i);
            return 0;
        }
    }
    printf("unknown level\n");
    return 1;
}

static int cmd_screen(int argc, char **argv)
{
    display_dump(stdout);
    return 0;
}

static int cmd_tasks(int argc, char **argv)
{
    const char *names[] = {"radio", "display", "led", "web", "httpd", "mqtt_task", "esp_timer", "IDLE", "IDLE0", "IDLE1"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        TaskHandle_t t = xTaskGetHandle(names[i]);
        if (t) printf("%-10s stack free %u\n", names[i], (unsigned)uxTaskGetStackHighWaterMark(t));
    }
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    esp_restart();
    return 0;
}

static int cmd_bootloader(int argc, char **argv)
{
    if (!app_can_reboot_to_bootloader()) {
        printf("not supported on this chip: hold BOOT and reset instead\n");
        return 1;
    }
    printf("entering ROM download mode; flash with esptool --before no-reset --after watchdog-reset\n");
    fflush(stdout);
    app_reboot_to_bootloader();
    return 0;
}

esp_err_t cli_start(void)
{
    static const esp_console_cmd_t cmds[] = {
        {.command = "metrics", .help = "Receiver, radio and display metrics. 'metrics reset' clears counters",
         .hint = "[reset]", .func = cmd_metrics},
        {.command = "stats", .help = "Alias for metrics", .hint = "[reset]", .func = cmd_metrics},
        {.command = "sensors", .help = "Table of the last 20 unique sensor IDs and their latest values",
         .hint = "[clear]", .func = cmd_sensors},
        {.command = "tires", .help = "Show or assign sensor IDs to wheels (only assigned IDs go to MQTT)",
         .hint = "[fl|fr|rl|rr <id|clear>]", .func = cmd_tires},
        {.command = "wifi", .help = "Wi-Fi, web UI, time and MQTT status. 'wifi sleep on|off' sets modem power saving (saved)",
         .hint = "[sleep on|off]", .func = cmd_wifi},
        {.command = "radio",
         .help = "Show or set radio (saved): radio freq 315.0 bw 83.3 sync 5556 bitrate 19231 drc 1 | defaults\n"
                 "  drc: data rate offset compensation 0 off, 1 +/-3.125%, 2 +/-6.25%, 3 +/-12.5% (CC1101)\n"
                 "  afc: tune in the averaged packet frequency offset between bursts (CC1101, default on; the offset starts at 0 at boot)",
         .hint = "[freq MHz] [bw kHz] [sync 5556|aaa9|5555] [bitrate cps] [drc 0-3] [afc on|off] | defaults",
         .func = cmd_radio},
        {.command = "rflog", .help = "RF energy events and strong raw payloads (reception diagnostics)",
         .hint = "[clear]", .func = cmd_rflog},
        {.command = "regs", .help = "Dump radio registers and status (CC1101) or status, errors and counters (LR1121)",
         .func = cmd_regs},
        {.command = "probe", .help = "Check the radio's control lines (CC1101: SPI, GDO0/2; LR1121: IRQ, RESET, BUSY)", .func = cmd_probe},
        {.command = "pllscan",
         .help = "Report PLL lock per frequency, then resume RX. No args: reference points + 300-330 MHz",
         .hint = "[start_MHz stop_MHz step_MHz]", .func = cmd_pllscan},
        {.command = "raw", .help = "Log every raw payload (hex) at INFO", .hint = "on|off", .func = cmd_raw},
        {.command = "inject", .help = "Feed a packet through the pipeline (default: rtl_433 sample)",
         .hint = "[hex bytes] [rssi]", .func = cmd_inject},
        {.command = "tx", .help = "Transmit test Ford frames at the RX frequency (default fake ID feedf00d)",
         .hint = "[hex bytes] [count n] [gap ms] [pa dBm | rfo dBm]", .func = cmd_tx},
        {.command = "display",
         .help = "Show or set display (saved): display bright 143 dim 1 dim_after 120 shift 30 | wake | defaults",
         .hint = "[bright n] [dim n] [dim_after s] [shift s] | wake | defaults", .func = cmd_display},
        {.command = "loglevel", .help = "Set log level for a tag", .hint = "<tag|*> <level>", .func = cmd_loglevel},
        {.command = "screen", .help = "Print the current OLED frame as text", .func = cmd_screen},
        {.command = "tasks", .help = "Task stack headroom", .func = cmd_tasks},
        {.command = "reboot", .help = "Restart the ESP32", .func = cmd_reboot},
        {.command = "bootloader", .help = "Reboot into the ROM download mode for flashing (ESP32-C3)",
         .func = cmd_bootloader},
    };

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rcfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rcfg.prompt = "tpms>";
    rcfg.max_cmdline_length = 128;
    rcfg.task_stack_size = 6144;
    esp_console_dev_uart_config_t ucfg = {
        .channel = CONFIG_ESP_CONSOLE_UART_NUM,
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .tx_gpio_num = -1,  /* default console pins -> USB bridge (ESP32: GPIO1/3, ESP32-C3: GPIO21/20) */
        .rx_gpio_num = -1,
    };
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&ucfg, &rcfg, &repl));
    ESP_ERROR_CHECK(esp_console_register_help_command());
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    return esp_console_start_repl(repl);
}
