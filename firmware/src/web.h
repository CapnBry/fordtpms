/*
 * Web UI on port 80: GET / (embedded web/index.html), GET /api/state (JSON),
 * POST /api/radio, /api/display, /api/tire (form-encoded, saved to NVS),
 * and /ws, a WebSocket that pushes one status message every 500 ms (2 Hz),
 * with the sensor list embedded when it changed. The page sends "ping" every 2 s and expects "pong"; the
 * server drops sockets that stay silent for WEB_CLIENT_TIMEOUT_MS.
 */
#pragma once

#include <stdint.h>

/* Starts the HTTP server and push task (called by net once Wi-Fi is up). */
void web_start(void);
/* Number of connected WebSocket clients. */
int web_client_count(void);
