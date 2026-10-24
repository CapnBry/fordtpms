/*
 * Board pin maps, one per build target (platformio.ini environment):
 *  - ESP32:    30-pin ESP32 DevKit (ESP32-WROOM-32) + CC1101 module
 *  - ESP32-C3: ExpressLRS-style "Unified ESP32C3 LR1121" receiver, WS2812B LED,
 *              boot button, external 12 dB LNA, no USB auto-reset circuit
 * Run `probe` on the CLI to verify the radio's control lines.
 */
#pragma once

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32

#define BOARD_NAME      "ESP32 + CC1101"
#define BOARD_HAS_OLED  0

#define PIN_LED         2    /* DevKit's onboard blue LED */

/*
 * All on the DevKit's right-hand header. SPI uses VSPI's own IO_MUX pins
 * (no GPIO matrix). GDO0/GDO2 avoid the strapping pins (the CC1101 drives
 * them from power-up) and GPIO36/39 (spurious interrupts while the ADC or
 * Wi-Fi powers up the SAR ADC, ESP32 errata 3.11). GPIO16/17 are free on
 * WROOM-32 modules; on a WROVER (PSRAM) use GPIO4 and 21 instead.
 *   header (antenna up): 23 22 TX RX 21 19 18 5 17 16 4 2 15 GND 3V3
 */
#define PIN_CC1101_MOSI 23
#define PIN_CC1101_MISO 19   /* also GDO1 / CHIP_RDYn */
#define PIN_CC1101_SCK  18
#define PIN_CC1101_CSN  5
#define PIN_CC1101_GDO0 17   /* end of packet */
#define PIN_CC1101_GDO2 16   /* sync word received (RSSI latched) */

#elif CONFIG_IDF_TARGET_ESP32C3

#define BOARD_NAME      "ESP32-C3 + LR1121"
#define BOARD_HAS_OLED  0

/* Console UART0 is GPIO20 (RX) / GPIO21 (TX), the ESP32-C3 defaults. */
#define PIN_RGB_LED     8    /* WS2812B, GRB order */
#define PIN_BUTTON      9    /* BOOT strap, active low */

#define PIN_LORA_SCK    6
#define PIN_LORA_MISO   5
#define PIN_LORA_MOSI   4
#define PIN_LORA_SS     7
#define PIN_LORA_RST    2
#define PIN_LORA_BUSY   3
#define PIN_LORA_DIO    1    /* LR1121 DIO9: IRQ */
#define BOARD_RADIO_DCDC   1

#else
#error "no board defined for this target"
#endif
