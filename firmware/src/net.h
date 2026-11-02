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
/* Wi-Fi modem sleep (WIFI_PS_MIN_MODEM) on/off; may be called before net_init. */
void net_set_powersave(bool on);
bool net_powersave(void);
/* Wi-Fi TX power limit in dBm (NET_TX_DBM_MIN..MAX, rounded down by the
 * PHY to its next step), or 0 for the driver default; may be called before
 * net_init. net_tx_power() returns the setting, net_tx_power_actual() the
 * driver's current limit in 0.25 dBm units (0 before Wi-Fi starts). */
#define NET_TX_DBM_MIN 2
#define NET_TX_DBM_MAX 20
void net_set_tx_power(int dbm);
int net_tx_power(void);
int net_tx_power_actual(void);
void net_get_status(net_status_t *out);
/* Cheap checks for frequent callers (status LED). */
bool net_enabled(void);
bool net_connected(void);
/* Wall clock (seconds) for an uptime in ms, or 0 if time isn't synced. */
time_t net_wall_time(uint32_t uptime_ms);
/* "YYYY-MM-DD hh:mm:ss" local time, or "" if time isn't synced. */
void net_fmt_time(char *buf, int len, time_t t);
