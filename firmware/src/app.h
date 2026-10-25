#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "display.h"
#include "esp_err.h"
#include "radio.h"
#include "tires.h"
#include "tpms_ford.h"

/* Packet sink shared by the radio task and the CLI 'inject' command. */
void app_on_packet(const tpms_ford_t *pkt, const radio_rx_info_t *rx, uint32_t ms);

/* Settings changes from the CLI and web UI: apply, then save to NVS. */
esp_err_t app_set_radio(const radio_config_t *cfg);
void app_set_display(const display_settings_t *set);
/* Reboot into the ROM download mode so esptool can flash without a reset
 * circuit (ESP32-C3; the classic ESP32 can't, see app_can_...). */
bool app_can_reboot_to_bootloader(void);
void app_reboot_to_bootloader(void);
/* Assigns a sensor to a wheel (TIRE_NONE clears) and updates MQTT discovery. */
void app_assign_tire(tire_pos_t pos, uint32_t id);
