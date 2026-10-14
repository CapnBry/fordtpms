/* Picks the local wifi_config.h, or the example (Wi-Fi disabled) if missing. */
#pragma once

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#warning "wifi_config.h not found: using wifi_config.example.h (Wi-Fi disabled)"
#include "wifi_config.example.h"
#endif
