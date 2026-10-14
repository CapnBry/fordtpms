/*
 * MQTT publishing in rtl_433 style with Home Assistant discovery.
 *
 *   <MQTT_BASE_TOPIC>/rtl_<id>/state     reading (JSON, retained), only for
 *                                        IDs assigned to a wheel (tires.h)
 *   <hostname>/status                    online / offline (LWT, retained)
 *   <MQTT_DISCOVERY_PREFIX>/device/rtl_<id>/config   discovery, assigned IDs
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sensors.h"

typedef struct {
    bool enabled;          /* MQTT_HOST configured */
    bool connected;
    char uri[96];
    uint32_t published;    /* state messages sent */
    uint32_t connects;
} ha_mqtt_status_t;

/* Starts the client (called by net once Wi-Fi has an IP). */
void ha_mqtt_start(void);
/* Publishes one sensor's latest reading if it is assigned to a wheel. */
void ha_mqtt_publish(const sensor_entry_t *e);
/* Republishes discovery for all wheels (after an assignment change). */
void ha_mqtt_tires_changed(void);
void ha_mqtt_get_status(ha_mqtt_status_t *out);
