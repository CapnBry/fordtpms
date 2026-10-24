/*
 * Status LED and button.
 *
 * ESP32 DevKit (plain LED): blinks once per decoded packet.
 *
 * ESP32-C3 board (WS2812B RGB LED), highest priority first:
 *   magenta          rebooting into the ROM download (flash) mode
 *   red, fast blink  radio not found
 *   red, slow blink  PLL not locked: the receiver is not tuned
 *   white flash      a packet was decoded
 *   yellow           radio fine, Wi-Fi connecting
 *   green            radio fine, Wi-Fi connected (or disabled)
 *   blue             starting up
 * The boot button (GPIO9) is read while running: a short press logs a
 * status line; holding it 3 s restarts the board (normal boot).
 */
#pragma once

#include <stdbool.h>

void led_init(void);
/* Starts the status task once the radio and network exist (no-op with a plain LED). */
void led_start(void);
/* A packet was decoded. */
void led_packet(void);
/* Solid magenta: about to reboot into the ROM loader. */
void led_bootloader(void);
