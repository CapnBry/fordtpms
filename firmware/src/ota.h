/*
 * Firmware update over Wi-Fi: HTTP POST /update on the web server, as used by
 * the *-ota envs in platformio.ini (tools/ota_upload.py). The body is either
 * the raw image (Content-Type: application/octet-stream) or a
 * multipart/form-data upload (curl -F firmware=@firmware.bin http://<ip>/update).
 * An optional X-MD5 header (raw bodies only) has the image checked before it
 * is made bootable. The reply goes out before the reboot.
 *
 * App rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): a freshly flashed
 * image boots on probation. ota_mark_valid() (called once the web server is
 * up) keeps it; a reset before that, or OTA_VERIFY_TIMEOUT_S without it,
 * boots the previous image again. So an update that breaks Wi-Fi or the web
 * server can't strand the board.
 */
#pragma once

#include <stdbool.h>

#include "esp_http_server.h"

/* Call early in app_main: starts the probation timer if this boot is unverified. */
void ota_boot_check(void);
/* The running image works well enough to take the next update. */
void ota_mark_valid(void);
/* A new image that hasn't been confirmed yet (reported in /api/state). */
bool ota_on_probation(void);
/* Registers POST /update. */
void ota_register(httpd_handle_t server);
