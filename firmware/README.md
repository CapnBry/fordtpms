# Ford TPMS receiver (ESP32 + SX1278 @ 315 MHz)

Receives Ford tire pressure sensors (VDO/Continental "S180084730Z" family, the
`tpms_ford` decoder in [rtl_433](https://github.com/merbanan/rtl_433/blob/master/src/devices/tpms_ford.c)),
shows them on the SSD1306 OLED, offers a serial CLI, and (with Wi-Fi
configured) a live web UI and MQTT publishing with Home Assistant discovery.

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

Partition table: `partitions_two_ota_large.csv`, two 1700 KB app slots (the app
is about 1.24 MB) so OTA updates can be added later without repartitioning.
OTA itself is not implemented. NVS (settings) is at 0x9000 as before.

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
* sync word `0x55 0x56`: the last 12 preamble chips plus the `0110` start
  delimiter (a Manchester '1' after the run of '0's, whose `11` chip pair the
  preamble never contains), so it matches only at the frame start. In all 7
  packets in the rtl_433 IQ captures, demodulated with "higher tone = 1", the
  data follows `…0101 0110`, and none decode inverted. `0x5555` is just
  preamble: it would match anywhere in the alternation
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
| `radio` | Show or set radio settings: `radio freq 315.0 bw 83.3 lna 1 sync 5556 bitrate 19231`, `radio defaults` |
| `raw on\|off` | Log every received payload (hex) with its decode result |
| `rflog [clear]` | RF energy events and strong raw payloads (see [Troubleshooting reception](#troubleshooting-reception)); also `GET /api/rflog` |
| `regs` | Dump SX1278 registers |
| `probe` | Identify which GPIOs the SX1278 DIO0/1/2 and RESET use |
| `inject [hex] [rssi]` | Feed a packet through the pipeline. 7 bytes gets a checksum added; with no argument it uses an rtl_433 sample |
| `display` | Show or set `bright`, `dim`, `dim_after` (s), `shift` (s); `display wake`, `display defaults` |
| `screen` | Print the current OLED frame as text |
| `loglevel <tag\|*> <level>` | e.g. `loglevel tpms debug` also shows repeats, `loglevel radio debug` shows raw packets |
| `tires` | Assign sensor IDs to wheels: `tires fl 364343a4`, `tires fl clear` |
| `wifi` | Wi-Fi, web UI, clock and MQTT status |
| `tasks` | Stack headroom |
| `reboot` | Restart |

Radio, display and tire settings are saved in NVS and survive reboots and
reflashing. Change them from the CLI or the web UI.

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
* **Overview**: a top-down car with a sensor in each corner (pressure, ID,
  age, temperature). Once IDs are assigned to wheels (see
  [Tire positions](#tire-positions)), each corner is its wheel (`--.-` /
  `WAIT` until the sensor is heard, `---` if unassigned). Before that, the
  corners are the four most recently heard sensors.

Screen schedule: the detail screen appears on each new reading and stays for 20 s.
After that the display rotates through the overview (8 s) and each sensor's detail (5 s).

Burn-in protection, on every screen:

* the whole layout is drawn on a 124x61 canvas that steps through all 20
  pixel offsets of the 128x64 panel, one pixel every 30 s (`display shift`)
* after 120 s with no new packet the contrast drops from 143 to 1. The panel is
  never turned off, and the next packet restores full brightness
  (`display dim_after`, `display dim`, `display bright`)

## Wi-Fi, web UI and MQTT

Settings live in `src/wifi_config.h`, which is in `.gitignore`. Start from the
template:

```bash
cp src/wifi_config.example.h src/wifi_config.h   # then edit SSID, password, broker
```

Without `wifi_config.h` the build falls back to the example (with a compiler
warning). The example has an empty SSID, so Wi-Fi stays off. An empty
`MQTT_HOST` turns off only MQTT.

| Setting | Default | |
|---|---|---|
| `WIFI_SSID`, `WIFI_PASSWORD` | `""` | WPA2 station. Empty SSID = Wi-Fi off |
| `WIFI_HOSTNAME` | `fordtpms` | DHCP hostname and mDNS name `fordtpms.local`; also the MQTT client ID and status topic |
| `MQTT_HOST`, `MQTT_PORT` | `""`, 1883 | Broker. Empty host = MQTT off |
| `MQTT_USERNAME`, `MQTT_PASSWORD` | `""` | Optional broker login |
| `MQTT_BASE_TOPIC` | `zigbee` | Readings go to `<base>/rtl_<id>/state` |
| `MQTT_DISCOVERY_PREFIX` | `homeassistant` | Home Assistant discovery. Empty = no discovery |
| `NTP_SERVER`, `TIME_ZONE` | `pool.ntp.org`, `UTC0` | Clock for the `time` field (POSIX TZ string, local time) |

The `wifi` CLI command shows the connection, clock and MQTT state.

### Web UI

`http://fordtpms.local/` (or the IP address from `wifi`). The page is a
1980s-dash-style console. It shows a scanner bar, the live RF level as a
tachometer-style bar graph (with noise-floor and last-packet markers), the
20 s RF history, every radio counter, the four most recent sensors around a
car outline, and the full sensor table, the same columns as the `sensors`
command. Ages tick in the browser.

* `/ws`: WebSocket. The firmware pushes one `status` message every 500 ms
  (2 Hz). The sensor table rides along in it as `sensors` when it has changed,
  and at least every 5 s. `rssi` is the peak of the 250 ms RF samples since the
  previous message, and `hist` carries the full 20 s history.
* Heartbeat: the page sends `ping` every 2 s and the firmware answers `pong`.
  If the page gets nothing for 6 s, it shows *SIGNAL LOST*, drops the socket and
  reconnects with backoff (1 s up to 10 s). The firmware closes WebSockets that
  have been silent for 10 s, so dead browser tabs don't use up the 4 client slots.
* `/api/state`: the same data as one JSON document, for scripts.

The page source is `src/web/index.html`, embedded into the firmware at build
time. It has no external dependencies, so it works on a LAN without internet
access.

### Tire positions

Packets don't say which wheel a sensor is on, so you assign each ID to FL, FR,
RL or RR. In the web UI, tap a wheel in the Tire Positions panel, or a row in
the scan log. On the CLI, use `tires`. Assignments are saved in NVS. An ID can be
assigned before it has been heard this boot, and a wheel can be cleared at any
time. **Only assigned IDs are published over MQTT**, so neighbours' cars are
seen in the web UI but don't reach Home Assistant.

### Settings dialogs

The **Radio** and **Display** buttons at the top of the page open dialogs with
the same settings as the `radio` and `display` CLI commands, plus *Defaults*
(and *Wake* for the display). Changes apply immediately and are saved in NVS.
The page has no login, so anyone on the LAN can change them.

### MQTT

On each new transmission (not on the repeats within a burst) from an
**assigned** sensor, the firmware publishes a retained message in rtl_433
style:

```
zigbee/rtl_364343a4/state
{"time":"2026-10-11 02:58:28","model":"Ford","type":"TPMS","id":"364343a4","pressure_PSI":38,
 "temperature_C":30,"moving":0,"learn":0,"code":"985606","mic":"CHECKSUM","rssi":-72}
```

* `temperature_C` is left out when the sensor reports none, and `time` is left
  out until NTP has synced. rtl_433's `unknown` fields are not sent. `rssi` (dBm,
  best packet of the burst) is an addition.
* `fordtpms/status` is `online` while connected, and `offline` through the MQTT
  last will.
* Home Assistant device discovery (retained) at
  `homeassistant/device/rtl_<id>/config`: device `TPMS <wheel>` (for example
  `TPMS Front Left`, serial number = sensor ID) with Pressure, Temperature,
  Moving and Signal (diagnostic, disabled by default). The unique IDs
  (`<id>_press`, `<id>_temp`) match hand-made rtl_433 discovery, so existing
  entities keep their history. Values expire after 18 h without a reading.
* Discovery and the latest readings are republished on every MQTT (re)connect
  and whenever an assignment changes.
* Clearing a wheel stops publishing that ID. Its discovery message and Home
  Assistant device are left in place; delete the device in Home Assistant if
  you no longer want it.

The MQTT and mDNS clients come from the ESP-IDF component registry
(`src/idf_component.yml`). The first build downloads them into
`managed_components/`.

## Troubleshooting reception

* `rflog` (or `http://fordtpms.local/api/rflog`, which needs no serial port).
  Opening the serial port can reset the board, so use the URL after an
  unattended run. It holds three RAM-only lists:
  * **RF events**: runs of 250 ms windows whose peak RSSI was at least 12 dB
    above the noise floor, with the syncs, payloads and decodes during each
    run (last 48)
  * **Strongest events**: the 16 loudest since boot, so a real burst isn't
    pushed out by later weak ones
  * **Strong payloads**: raw bytes of every payload whose sync RSSI was at
    least 6 dB above the noise floor, decoded or not (last 24)

  How to read it: strong events with **no syncs** mean the packet engine never
  matched the sync word, so suspect polarity (`radio sync aaa9` or `5555`) or
  frequency. **Syncs but no decodes** with strong payloads mean the sync
  matched but the data was wrong (bit rate, deviation, bandwidth). Paste the
  hex for offline analysis.

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
| `src/net.c` | Wi-Fi station, hostname, mDNS, SNTP |
| `src/web.c`, `src/web/index.html` | HTTP server, WebSocket push, the web UI page |
| `src/ha_mqtt.c` | MQTT state messages and Home Assistant discovery |
| `src/tires.c` | Sensor ID to wheel position assignments |
| `src/settings.c` | NVS storage for radio, display and tire settings |
| `src/wifi_config.example.h` | Template for `src/wifi_config.h` (not in git) |
| `src/battery.c` | Battery ADC |
| `test/host/` | Host tests with real captures from rtl_433_tests, UI preview |
