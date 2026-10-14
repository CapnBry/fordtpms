/*
 * Wi-Fi station (settings in wifi_config.h), DHCP hostname + mDNS, SNTP,
 * then the web UI and MQTT once an IP address is assigned.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

typedef struct {
    bool enabled;          /* an SSID is configured */
    bool connected;        /* associated and has an IP */
    bool time_valid;       /* SNTP synced */
    char ssid[33];
    char hostname[33];
    char ip[16];
    int8_t rssi_dbm;       /* AP signal, 0 if not connected */
    uint32_t reconnects;
} net_status_t;

esp_err_t net_init(void);
void net_get_status(net_status_t *out);
/* Wall clock (seconds) for an uptime in ms, or 0 if time isn't synced. */
time_t net_wall_time(uint32_t uptime_ms);
/* "YYYY-MM-DD hh:mm:ss" local time, or "" if time isn't synced. */
void net_fmt_time(char *buf, int len, time_t t);
