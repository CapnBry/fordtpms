#pragma once

#include <stdint.h>

#include "tpms_ford.h"

/* Packet sink shared by the radio task and the CLI 'inject' command. */
void app_on_packet(const tpms_ford_t *pkt, int rssi_dbm, uint32_t ms);
int app_battery_mv(void);
