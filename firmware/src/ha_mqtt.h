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
/* Republishes discovery for all wheels (after an assignment change), plus the
 * latest reading of `new_id` (TIRE_NONE for none): a sensor that wasn't on a
 * wheel never had its readings published. */
void ha_mqtt_tires_changed(uint32_t new_id);
void ha_mqtt_get_status(ha_mqtt_status_t *out);
