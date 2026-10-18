/*
 * Board pin maps, one per build target (platformio.ini environment):
 *  - ESP32:    TTGO / LilyGO LoRa32 v2.1 (T3 v1.6.x), ESP32-PICO-D4 + SX1278 + SSD1306
 *  - ESP32-C3: ExpressLRS-style "Unified ESP32C3 LR1121" receiver, WS2812B LED,
 *              boot button, external 12 dB LNA, no USB auto-reset circuit
 * Run `probe` on the CLI to verify the radio's control lines.
 */
#pragma once

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32

#define BOARD_NAME      "TTGO LoRa32 v2.1 (SX1278)"
#define BOARD_HAS_OLED  1
#define BOARD_HAS_BATT  1

#define PIN_OLED_SDA    21
#define PIN_OLED_SCL    22
#define OLED_I2C_ADDR   0x3C

#define PIN_LED         25
#define PIN_BATT_ADC    35   /* ADC1_CH7, behind a 100k/100k divider */
#define BATT_DIVIDER    2

#define PIN_LORA_SCK    5
#define PIN_LORA_MISO   19
#define PIN_LORA_MOSI   27
#define PIN_LORA_SS     18
#define PIN_LORA_RST    23
#define PIN_LORA_DIO0   26   /* PayloadReady */
#define PIN_LORA_DIO1   33
#define PIN_LORA_DIO2   32   /* SyncAddress (timestamp + RSSI capture) */

#elif CONFIG_IDF_TARGET_ESP32C3

#define BOARD_NAME      "ESP32-C3 + LR1121"
#define BOARD_HAS_OLED  0
#define BOARD_HAS_BATT  0

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
#define BOARD_LNA_GAIN_DB  12   /* external LNA ahead of the LR1121, removed from RSSI */

#else
#error "no board defined for this target"
#endif
