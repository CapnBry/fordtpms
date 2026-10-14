/*
 * Persistent settings in NVS (namespace "tpms"). Each setting is a blob keyed
 * by name; a stored blob whose size doesn't match the current struct is
 * ignored, so changing a struct falls back to defaults instead of loading junk.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* Initialises NVS (also used by Wi-Fi). Call first in app_main. */
esp_err_t settings_init(void);
bool settings_load(const char *key, void *out, size_t len);
esp_err_t settings_save(const char *key, const void *data, size_t len);
esp_err_t settings_erase(const char *key);
