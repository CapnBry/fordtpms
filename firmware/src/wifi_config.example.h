/*
 * Wi-Fi / MQTT settings. Copy this file to wifi_config.h (same directory) and
 * edit it. wifi_config.h is in .gitignore so credentials stay out of git.
 * Without wifi_config.h the build uses this example, which has no SSID, so
 * Wi-Fi stays off.
 */
#pragma once

/* Wi-Fi station. An empty SSID disables Wi-Fi, the web UI and MQTT. */
#define WIFI_SSID          ""
#define WIFI_PASSWORD      ""
/* DHCP hostname and mDNS name (<hostname>.local). */
#define WIFI_HOSTNAME      "fordtpms"

/* MQTT broker. An empty host disables MQTT. */
#define MQTT_HOST          ""
#define MQTT_PORT          1883
#define MQTT_USERNAME      ""
#define MQTT_PASSWORD      ""
/* Readings go to <MQTT_BASE_TOPIC>/rtl_<id>/state, e.g. zigbee/rtl_364343a4/state */
#define MQTT_BASE_TOPIC    "zigbee"
/* Home Assistant MQTT discovery. "" disables discovery messages. */
#define MQTT_DISCOVERY_PREFIX "homeassistant"

/* Time for the "time" field: NTP server and POSIX TZ string (local time). */
#define NTP_SERVER         "pool.ntp.org"
#define TIME_ZONE          "UTC0"   /* e.g. "CST6CDT,M3.2.0,M11.1.0" */
