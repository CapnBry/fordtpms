#pragma once

#include "esp_err.h"

/* Starts the esp_console REPL on the console UART (see sdkconfig.defaults). */
esp_err_t cli_start(void);
