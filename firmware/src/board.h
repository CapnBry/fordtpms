/*
 * TTGO / LilyGO LoRa32 v2.1 (T3 v1.6.x), ESP32-PICO-D4 + SX1278 + SSD1306.
 * Run `probe` on the CLI to verify the SX1278 DIO/RESET wiring.
 */
#pragma once

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
