# Ford TPMS receiver (ESP32 + SX1278 @ 315 MHz)

Receives Ford tire pressure sensors (VDO/Continental "S180084730Z" family, the
`tpms_ford` decoder in [rtl_433](https://github.com/merbanan/rtl_433/blob/master/src/devices/tpms_ford.c)),
shows them on the SSD1306 OLED, and offers a serial CLI.

PlatformIO, `espressif32@7.1.3`, ESP-IDF 6.1 (no Arduino).

## Hardware

TTGO/LilyGO LoRa32 v2.1 (T3 v1.6), ESP32-PICO-D4, 4 MB flash.

| Function | GPIO | Notes |
|---|---|---|
| OLED SDA / SCL | 21 / 22 | SSD1306 128x64 @ 0x3C, 400 kHz |
| LED | 25 | blinks on each decoded packet |
| Battery | 35 | ADC1_CH7, 2:1 divider |
| SX1278 SCK / MISO / MOSI / NSS | 5 / 19 / 27 / 18 | SPI3, 8 MHz |
| SX1278 RESET | 23 | verified with `probe` |
| SX1278 DIO0 | 26 | PayloadReady interrupt (verified with `probe`) |
| SX1278 DIO1 | 33 | wired, unused (DCLK in continuous mode, verified) |
| SX1278 DIO2 | 32 | SyncAddress interrupt, used to latch RSSI (verified) |

The SX1278 covers 137–525 MHz, so 315 MHz works, but this board's antenna and
matching network are tuned for 433 MHz. A 315 MHz antenna (~23.8 cm quarter
wave) will improve range.

## Build / flash / monitor

```bash
cd firmware
pio run -t upload            # builds and flashes /dev/ttyUSB0
pio device monitor           # 115200 baud, CLI prompt "tpms>"
```

Host-side tests (decoder against real rtl_433 captures, and UI previews):

```bash
make -C test/host            # decoder tests
make -C test/host preview    # renders each screen to test/host/*.pgm
make -C test/host screenshots  # regenerates ../docs/images/*.png (ImageMagick)
```

After changing `sdkconfig.defaults`, delete the generated config and build
directory (`rm -rf .pio/build sdkconfig.ttgo-lora32-v21`) before building.
Otherwise PlatformIO keeps the old sdkconfig and may not rebuild the
bootloader. A stale bootloader with different console settings, for example,
leaves the app's serial output silent.

To restore the original firmware, see `../backup/restore.sh`.

## How reception works

Ford packets are FSK at 52 µs per chip (19 231 chip/s), deviation about ±35 kHz:

```
55 55 55 56 | 128 Manchester chips -> 8 bytes: ID ID ID ID PP TT FF CS
```

The SX1278 runs in **FSK packet mode**, so the radio does the timing-critical work:

* bit synchroniser at 19 231 chip/s, RX filter ±83 kHz, fixed LNA gain (no AGC,
  so gain changes can't corrupt the short ~32-chip preamble)
* sync word `0x55 0x56`: the last 16 preamble chips
* fixed 16-byte payload: exactly the 128 Manchester chips of one packet
* DIO2 (SyncAddress) interrupt: latch RSSI while the packet is still on air
* DIO0 (PayloadReady) interrupt: read FIFO, Manchester-decode, verify checksum
  and flag bits exactly as rtl_433 does, then restart RX

The radio task also polls the IRQ flags every 5 ms as a fallback, and samples
RSSI between packets to estimate the noise floor.

Sensors repeat each reading about 4 times in a burst. Repeats are folded into
one entry and logged at DEBUG; the first packet of a burst is logged at INFO:

```
I (8513) tpms: packet id=45bb320f pressure=26.50 PSI temp=n/a mode=MOVING code=6ad446 flags=46 rssi=-80 dBm
```

Ford sensors transmit while driving and **only once every ~6 h when parked**,
so a parked car may stay silent for hours. Driving, deflating a tire slightly,
or a TPMS trigger tool makes them transmit.

## Serial console and logging

Full command reference: [../docs/CLI.md](../docs/CLI.md).

Console and logs use **UART0** (GPIO1/GPIO3), the UART wired to the CP2104 USB
bridge, so everything shows up on `/dev/ttyUSB0` at 115200 baud, together
with the ROM and bootloader messages. The default log level is INFO.

| Command | Description |
|---|---|
| `help` | List commands |
| `metrics` / `stats` | Uptime, heap, battery, radio config, noise floor, sync/decode/error counters, IRQ counts, RSSI range, display state. `metrics reset` clears counters |
| `sensors` | Table of the last 20 unique sensor IDs, newest first: PSI, kPa, temperature, mode, code, flags, RSSI, bursts, packets, time last seen. `sensors clear` |
| `radio` | Show or set radio settings: `radio freq 315.0 bw 83.3 lna 1 sync 5556 bitrate 19231` |
| `raw on\|off` | Log every received payload (hex) with its decode result |
| `regs` | Dump SX1278 registers |
| `probe` | Identify which GPIOs the SX1278 DIO0/1/2 and RESET use |
| `inject [hex] [rssi]` | Feed a packet through the pipeline. 7 bytes gets a checksum added; with no argument it uses an rtl_433 sample |
| `display` | Show or set `bright`, `dim`, `dim_after` (s), `shift` (s); `display wake` |
| `screen` | Print the current OLED frame as text |
| `loglevel <tag\|*> <level>` | e.g. `loglevel tpms debug` also shows repeats, `loglevel radio debug` shows raw packets |
| `tasks` | Stack headroom |
| `reboot` | Restart |

Radio settings live in RAM and reset to the defaults on reboot.

## Display

Screen-by-screen description with screenshots: [../docs/UI.md](../docs/UI.md).

All screens use 1980s-dash-style slanted 7-segment digits and segmented bar graphs.

* **Idle / scanning** (before the first packet): a sweeping scanner bar,
  scrolling RF noise graph (peak RSSI per 250 ms), live noise floor, sync
  counter, uptime, battery voltage, and packet/bad/ID counters.
* **Detail**: sensor ID, burst count, large pressure in PSI, temperature, kPa,
  a 0–60 PSI wedge bar graph, signal-strength bars, and a bottom line that
  alternates between *mode · time since received · RSSI* and *code · flags ·
  uptime at reception*. A fresh packet flashes a frame around the digits.
* **Overview**: a top-down car with the four most recently heard sensors in the
  corners (pressure, ID, age, temperature). The packets don't say which wheel
  a sensor is on, so these are just the four latest.

Screen schedule: the detail screen appears on each new reading and stays for 20 s.
After that the display rotates through the overview (8 s) and each sensor's detail (5 s).

Burn-in protection, on every screen:

* the whole layout is drawn on a 124x61 canvas that steps through all 20
  pixel offsets of the 128x64 panel, one pixel every 30 s (`display shift`)
* after 120 s with no new packet the contrast drops from 143 to 1. The panel is
  never turned off, and the next packet restores full brightness
  (`display dim_after`, `display dim`, `display bright`)

## Wi-Fi

Wi-Fi and Bluetooth are never initialised, so the 2.4 GHz radio stays
powered down. To add Wi-Fi later, call `nvs_flash_init()`, `esp_netif_init()`
and `esp_wifi_init()` in `app_main`. The default partition table already has an
`nvs` and a `phy_init` partition.

## Troubleshooting reception

* `metrics`: `sync matches` should slowly climb even with no sensor around
  (noise matches the sync word about once every 5–10 s). If it stays at 0 the
  radio isn't receiving.
* Sync polarity: the default (`5556`) assumes the higher FSK tone is '1', which
  matches the rtl_433 reference captures. If syncs come in but sensors never
  decode, try `radio sync 5555`. It accepts either polarity, at the cost of
  needing a little more clean preamble.
* Frequency offset: sensors can be ±20 kHz off. Use `radio bw 100` or
  `radio freq 315.02` to compensate.
* Very close/strong sensors: try `radio lna 3`.
* `raw on` shows every payload, and the decode error tells how far it got
  (`manchester` < `checksum` < `flags`).

## Source layout

| File | Purpose |
|---|---|
| `src/main.c` | Startup, packet sink (log + table + LED) |
| `src/radio.c` | SX1278 FSK driver, receive task, RSSI history, DIO probe |
| `src/tpms_ford.c` | Portable decoder (port of rtl_433 `tpms_ford.c`) |
| `src/sensors.c` | 20-entry unique-ID table with burst folding |
| `src/display.c` | SSD1306 driver, screen scheduling, dimming, pixel shift |
| `src/ui.c`, `src/gfx.c` | Portable screen layouts, fonts, 7-segment renderer |
| `src/cli.c` | esp_console commands |
| `src/battery.c` | Battery ADC |
| `test/host/` | Host tests with real captures from rtl_433_tests, UI preview |
