# Ford TPMS receiver (315 MHz, CC1101 or LR1121)

Receives Ford tire pressure sensors (VDO/Continental "S180084730Z" family, the
`tpms_ford` decoder in [rtl_433](https://github.com/merbanan/rtl_433/blob/master/src/devices/tpms_ford.c)),
shows them with a status LED, offers a
serial CLI, and (with Wi-Fi configured) a live web UI and MQTT publishing with
Home Assistant discovery.

PlatformIO, `espressif32@7.1.3`, ESP-IDF 6.1 (no Arduino). Two boards, one
PlatformIO environment each:

| Environment | Board | Radio |
|---|---|---|
| `esp32-cc1101` | 30-pin ESP32 DevKit (ESP32-WROOM-32) + CC1101 module | CC1101, 315 MHz in band |
| `c3-lr1121` | ExpressLRS-style "Unified ESP32C3 LR1121" receiver, ESP32-C3 | LR1121, locks at 315 MHz |

## Hardware

### ESP32 DevKit + CC1101

A 30-pin ESP32 DevKit (ESP32-WROOM-32, 4 MB flash, CP2102/CH340 with auto-reset)
and a TI CC1101 module with a 26 MHz crystal and a 315 MHz antenna. The CC1101 is
specified for 300–348, 387–464 and 779–928 MHz, so 315 MHz is in band; its
lock is checked anyway (below). Use a module matched for 315 MHz; the common
433 MHz modules' filter and matching cost sensitivity at 315 MHz.

All radio signals sit on the DevKit's right-hand header, which reads from the
antenna end `23 22 TX RX 21 19 18 5 17 16 4 2 15 GND 3V3`:

| Function | GPIO | Notes |
|---|---|---|
| CC1101 MOSI (SI) | 23 | VSPI IO_MUX pin |
| CC1101 MISO (SO/GDO1) | 19 | VSPI IO_MUX pin; also read as CHIP_RDYn before each transfer |
| CC1101 SCK | 18 | VSPI IO_MUX pin, 5 MHz |
| CC1101 CSn | 5 | software chip select (waits for SO low, as the CC1101 requires) |
| CC1101 GDO0 | 17 | end-of-packet interrupt |
| CC1101 GDO2 | 16 | sync-word interrupt, RSSI latched there |
| CC1101 VCC / GND | 3V3 / GND | bottom of the same header |
| LED | 2 | DevKit's onboard LED, blinks on each decoded packet |

How the pins were picked, performance first:

* **SPI on VSPI's own IO_MUX pins** (18/19/23/5), so the SPI signals skip the
  GPIO matrix and its input delay.
* **GDO0/GDO2 on GPIO17/16**. These aren't strapping pins: the CC1101 drives
  its GDOs from power-up (GDO0 outputs a clock until it's configured), so a GDO
  on GPIO0/2/5/12/15 could change the boot mode. They also avoid GPIO36/39,
  which get spurious interrupts whenever the SAR ADC powers up, including for
  Wi-Fi (ESP32 errata 3.11). GPIO34/35 would work but are on the other
  header.
* **Adjacency, within that:** 19, 18, 5, 17 and 16 are five neighbouring header
  pins, and 3V3/GND are at the bottom of the same row. The only pin that isn't
  adjacent is MOSI on 23 (top of the row). Moving it to 21 would make all six
  contiguous, but the whole bus would then go through the GPIO matrix.
* GPIO16/17 are only free on WROOM modules. A WROVER board uses them for PSRAM;
  there, move GDO0/GDO2 to GPIO4 and 21.

Receiver setup: 2-FSK, 19 231 baud, ±81.25 kHz filter (162.5 kHz double-sided,
the nearest to ±83 kHz), AGC, frequency offset compensation ±BW/4, DC filter
on. Sync `0x5556` 16/16 bits, preamble quality gate off, fixed 16-byte payload,
appended RSSI/LQI status. After a packet the chip stays in RX, so frames sent
back to back aren't missed. The synthesizer is recalibrated every 60 s; each
recalibration leaves the receiver deaf for ~0.8 ms. Lock: after calibration, `FSCAL1`
reads 0x3F if the PLL could not lock (datasheet section 22.1). It's checked
after each retune and every 250 ms. RSSI uses the datasheet's 74 dB offset.

**Not yet tested on hardware.** Run `probe` first: it checks SO/CSn, SPI write
and readback, and drives each GDO high and low to verify the wiring. Then run
`pllscan`.

### ESP32-C3 + LR1121

ESP32-C3 (4 MB embedded flash) with a Semtech LR1121, an external 12 dB LNA, a
WS2812B LED and a boot button, on a CH340 USB-TTL adapter (`/dev/ttyAMA0`)
**without** the DTR/RTS auto-reset circuit. Pins follow the board's ExpressLRS
hardware layout (`UNIFIED_ESP32C3_LR1121_RX`).

| Function | GPIO | Notes |
|---|---|---|
| Console UART0 RX / TX | 20 / 21 | 115200 baud |
| LR1121 SCK / MISO / MOSI / NSS | 6 / 5 / 4 / 7 | SPI2, 8 MHz |
| LR1121 RESET | 2 | verified with `probe` |
| LR1121 BUSY | 3 | verified with `probe` |
| LR1121 IRQ (DIO9) | 1 | SyncWordValid + RxDone interrupt (verified with `probe`) |
| WS2812B LED | 8 | GRB, driven by the RMT peripheral |
| Button | 9 | the BOOT strap, active low |

Front end as configured by ExpressLRS for this board: crystal (no TCXO), DC-DC
regulator, ExpressLRS's default RF switch table, RX boosted mode. RSSI is
reported as the LR1121 measures it, so it includes the external LNA's gain.

**The LR1121 locks at 315 MHz.** `pllscan` (lock = no `PLL_LOCK_ERR` from
GetErrors, chip in FS mode) shows lock from 40 to 2890 MHz, including all of
300–330 MHz; at 35 and 2900 MHz the chip flags `PLL_LOCK_ERR` and drops to
standby, so the check does detect failures. The datasheet only specifies
150–960 MHz (and the 2.4 GHz band) for reception. A PLL lock doesn't prove good
sensitivity at 315 MHz. This unit is a variant whose front end (matching,
filter, LNA) is tuned for a band below the usual 868/915 MHz; its exact
passband and the LNA's gain at 315 MHz haven't been measured.

Status LED, highest priority first:

| Colour | Meaning |
|---|---|
| magenta | rebooting into the ROM download mode |
| red, fast blink | radio not found |
| red, slow blink | PLL not locked: the receiver is not tuned |
| white flash | packet decoded |
| yellow | Wi-Fi connecting |
| green | receiving, Wi-Fi connected (or disabled) |
| blue | starting up; while the button is held |

Button: a short press logs a status line on the console. Holding it 3 s
restarts the board (normal boot). Holding it while powering up enters the ROM
download mode, as usual for the BOOT strap.

## Build / flash / monitor

```bash
cd firmware
pio run -t upload                      # default env c3-lr1121: /dev/ttyAMA0, hands-free (below)
pio run -e esp32-cc1101 -t upload      # ESP32 DevKit + CC1101: /dev/ttyUSB0
pio device monitor -e c3-lr1121        # 115200 baud, CLI prompt "tpms>"
pio run -e esp32-cc1101-ota -t upload  # over Wi-Fi to fordtpms.local (below)
pio run -e c3-lr1121-ota -t upload --upload-port <ip>:80
```

The C3 board has no auto-reset circuit, so the upload is driven by the firmware
itself:

1. `tools/c3_upload.py` (a PlatformIO pre-upload hook) sends the CLI command
   `bootloader`. The firmware sets `RTC_CNTL_FORCE_DOWNLOAD_BOOT` and restarts
   into the ROM download mode.
2. esptool connects with `--before no_reset`, flashes, and resets with
   `--after watchdog_reset` (the RTC watchdog), which boots the new firmware.

If the board isn't running this firmware, hold the button while powering it
up, then upload. Crash-loop rescue: three crash resets in a row (panic or
watchdog), each within 30 s of boot, make the next boot enter the ROM download
mode by itself, so a broken build can still be replaced without touching the
board. A run that lasts 30 s resets the count.

### Over Wi-Fi (OTA)

The `*-ota` envs build the same firmware and POST it to `/update` on the web
server (`tools/ota_upload.py`, `src/ota.c`), about 10 s for the 1.1 MB image.
Both boards answer to `fordtpms.local`, so with both on the network pick one
with `--upload-port <ip>:80` (the `wifi` command shows the IP). Keep the `:80`:
a bare IP or `.local` name makes PlatformIO switch to espota, which this
firmware doesn't run. Without PlatformIO: `curl -F
firmware=@.pio/build/<env>/firmware.bin http://<ip>/update`.

The device checks the image before switching to it: the chip ID as soon as the
header arrives (the other board's image is refused right away), then the
optional `X-MD5` header and ESP-IDF's own image check (SHA-256). It replies
with the result, then reboots.

App rollback is on (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`). A new image boots
on probation and confirms itself once Wi-Fi is up and the web server has
started. If it resets before that (crash, watchdog, power cycle), or isn't
confirmed within 300 s, the bootloader goes back to the previous image. So an
update that breaks Wi-Fi or the web server falls back to one that can take the
next update. Rollback lives in the bootloader, so each board needs one USB
flash of this firmware before it can be updated over Wi-Fi.

Host-side tests (decoder against real rtl_433 captures, and UI previews):

```bash
make -C test/host            # decoder tests
make -C test/host preview    # renders each screen to test/host/*.pgm
make -C test/host screenshots  # regenerates ../docs/images/*.png (ImageMagick)
```

Partition table: `partitions_two_ota_large.csv`, two 1700 KB app slots (the app
is about 1.1 MB) for OTA updates. NVS (settings) is at 0x9000 as before.

`sdkconfig.defaults` applies to both boards; ESP-IDF adds
`sdkconfig.defaults.esp32c3` for the C3 build (console UART0, no USB-JTAG
console). After changing either, delete the generated config and build
directory (`rm -rf .pio/build/<env> sdkconfig.<env>`) before building.
Otherwise PlatformIO keeps the old sdkconfig and may not rebuild the
bootloader. A stale bootloader with different console settings, for example,
leaves the app's serial output silent.

`../backup/` holds an older board's original flash image (`restore.sh`). The C3
board's original ExpressLRS flash image is in `../backup/lr1121-c3/` (not in
git); restore it with `esptool --chip esp32c3 -p /dev/ttyAMA0 --before no_reset
write_flash 0 flash_4MB_elrs.bin` after `bootloader`.

## How reception works

Ford packets are FSK at 52 µs per chip (19 231 chip/s), deviation about ±35 kHz:

```
55 55 55 56 | 128 Manchester chips -> 8 bytes: ID ID ID ID PP TT FF CS
```

Both radios run in **FSK packet mode**, so the radio does the timing-critical
work. On the CC1101:

* bit synchronisation at 19 231 chip/s, RX filter ±81.25 kHz
* sync word `0x55 0x56`: the last 12 preamble chips plus the `0110` start
  delimiter (a Manchester '1' after the run of '0's, whose `11` chip pair the
  preamble never contains), so it matches only at the frame start. In all 7
  packets in the rtl_433 IQ captures, demodulated with "higher tone = 1", the
  data follows `…0101 0110`, and none decode inverted. `0x5555` is just
  preamble: it would match anywhere in the alternation
* fixed 16-byte payload: exactly the 128 Manchester chips of one packet
* GDO2 (sync word) interrupt: the RSSI register freezes at the sync word
* GDO0 (end of packet) interrupt: read the payload and its appended RSSI from the
  FIFO, Manchester-decode, verify checksum and flag bits exactly as rtl_433
  does; the chip is already receiving again

The radio task also polls PKTSTATUS/RXBYTES every 5 ms as a fallback, and
samples RSSI between packets to estimate the noise floor.

The LR1121 uses the same sync word, payload length and chip rate in its GFSK
engine (no CRC, no whitening, preamble detector off, nearest filter ±78.1 kHz).
It has a single IRQ line for SyncWordValid (RSSI latched there) and RxDone (the
payload is read with ReadBuffer8, and its sync RSSI from GetPacketStatus). Its
lock is checked after every retune
and every 250 ms while receiving. If the chip reports `PLL_LOCK_ERR` or has
left RX, the error is cleared and RX restarted.

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

Console and logs use **UART0** (ESP32 DevKit: GPIO1/GPIO3 to the USB bridge,
`/dev/ttyUSB0`; C3 board: GPIO21/GPIO20 to the CH340 adapter, `/dev/ttyAMA0`)
at 115200 baud, together with the ROM and bootloader messages. The default log
level is INFO.

| Command | Description |
|---|---|
| `help` | List commands |
| `metrics` / `stats` | Uptime, heap, radio chip and config, PLL lock, noise floor, sync/decode/error counters, IRQ counts, RSSI range, display state. `metrics reset` clears counters |
| `sensors` | Table of the last 20 unique sensor IDs, newest first: PSI, kPa, temperature, mode, code, flags, RSSI, bursts, packets, time last seen. `sensors clear` |
| `radio` | Show or set radio settings: `radio freq 315.0 bw 83.3 sync 5556 bitrate 19231 drc 1`, `radio defaults` (`drc`: data rate offset tracking, 0 off … 3 ±12.5 %, CC1101; `afc on\|off`: auto-tune, see below) |
| `raw on\|off` | Log every received payload (hex) with its decode result |
| `rflog [clear]` | RF energy events and strong raw payloads (see [Troubleshooting reception](#troubleshooting-reception)); also `GET /api/rflog` |
| `regs` | CC1101 registers, state, lock and last frequency offset; LR1121 status, errors, counters, temperature |
| `probe` | CC1101: check SO/CSn, SPI readback, GDO0 and GDO2. LR1121: check RESET, BUSY, SPI and the IRQ line |
| `pllscan [start stop step]` | PLL lock per frequency (MHz), then resume RX; no arguments = reference points + 300–330 MHz |
| `inject [hex] [rssi]` | Feed a packet through the pipeline. 7 bytes gets a checksum added; with no argument it uses an rtl_433 sample |
| `tx [hex] [count n] [gap ms] [pa dBm \| rfo dBm]` | Transmit test Ford packets at the current frequency (default fake ID `feedf00d`, 4 packets, lowest high-power-PA level), then resume RX |
| `display` | Show or set `bright`, `dim`, `dim_after` (s), `shift` (s); `display wake`, `display defaults` |
| `screen` | Print the current OLED frame as text |
| `loglevel <tag\|*> <level>` | e.g. `loglevel tpms debug` also shows repeats, `loglevel radio debug` shows raw packets |
| `tires` | Assign sensor IDs to wheels: `tires fl 364343a4`, `tires fl clear` |
| `wifi` | Wi-Fi, web UI, clock and MQTT status |
| `tasks` | Stack headroom |
| `reboot` | Restart |
| `bootloader` | Reboot into the ROM download mode for flashing (ESP32-C3) |

Radio, display and tire settings are saved in NVS and survive reboots and
reflashing. Change them from the CLI or the web UI.

## Display

Screen-by-screen description with screenshots: [../docs/UI.md](../docs/UI.md).

All screens use 1980s-dash-style slanted 7-segment digits and segmented bar graphs.

* **Idle / scanning** (before the first packet): a sweeping scanner bar,
  scrolling RF noise graph (peak RSSI per 250 ms), live noise floor, sync
  counter, uptime, and packet/bad/ID counters.
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
| `WIFI_HOSTNAME` | `fordtpms` | DHCP hostname and mDNS name `fordtpms.local`; also the MQTT client ID and status topic. |
| `MQTT_HOST`, `MQTT_PORT` | `""`, 1883 | Broker. Empty host = MQTT off |
| `MQTT_USERNAME`, `MQTT_PASSWORD` | `""` | Optional broker login |
| `MQTT_BASE_TOPIC` | `zigbee` | Readings go to `<base>/rtl_<id>/state` |
| `MQTT_DISCOVERY_PREFIX` | `homeassistant` | Home Assistant discovery. Empty = no discovery |
| `NTP_SERVER`, `TIME_ZONE` | `pool.ntp.org`, `UTC0` | Clock for the `time` field (POSIX TZ string, local time) |

The `wifi` CLI command shows the connection, clock and MQTT state.

### Wi-Fi power save

`net.c` turns Wi-Fi power save off (`WIFI_PS_NONE`). With ESP-IDF's default
modem sleep (`WIFI_PS_MIN_MODEM`) the radio sleeps between beacons, which on a
weak link (about −77 dBm here) made the web UI and MQTT lag and drop out.
Keeping the radio on costs about 56 mA:

| C3 board, idle, at USB 4.87 V | `WIFI_PS_NONE` | `WIFI_PS_MIN_MODEM` |
|---|---|---|
| Average current | 100.0 mA | 43.8 mA |
| Average power | 0.49 W | 0.22 W |
| Current profile | flat, 99.6–100.3 mA | ~42 mA, short ~80 mA bursts every ~8.6 s |
| Ping round trip, avg / max | 9 / 49 ms | 64 / 119 ms |

Measured 2026-10-20 with an FNIRSI FNB48 USB meter: 90 s per mode, no web UI
clients, before the size-optimized build settings. The extra ~6.5 Wh a day
doesn't matter on USB power; on battery, go back to `WIFI_PS_MIN_MODEM`.

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
* `/api/state`: the same data as one JSON document, for scripts. `status.fw`
  identifies the running build: `elf` is the first 8 hex digits of the ELF's
  SHA-256 (`sha256sum .pio/build/<env>/firmware.elf`), `probation` is true
  while a new OTA image is unconfirmed.
* `/update`: POST a firmware image to flash it (see *Over Wi-Fi (OTA)*).
* `/oled`: the display's current frame, drawn as a transflective LCD at a
  selectable integer scale (1x to 8x; by default the largest that fits the
  window, up to 6x). It works without a panel
  fitted, since the firmware always renders the screens. The page sends
  `oled` on the WebSocket and then gets binary frames instead of
  `status` messages: an 8 byte header (width, height, flags: bit 0 panel
  present, bit 1 dimmed; 0; frame generation as u32 LE), then the framebuffer
  in SSD1306 page order. A frame is sent only when its pixels changed, at most
  at the display's 10 Hz frame rate.

The page sources are `src/web/index.html` and `src/web/oled.html`, embedded into the firmware at build
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

Once per transmission from an **assigned** sensor, the firmware publishes a
retained message in rtl_433 style. The repeats within a burst are folded into
one message, sent when the burst has ended (no packet from that sensor for 3 s):

```
zigbee/rtl_364343a4/state
{"time":"2026-10-11 02:58:28","model":"Ford","type":"TPMS","id":"364343a4","pressure_PSI":38,
 "temperature_C":30,"moving":0,"rssi":-72}
```

* `temperature_C` is left out when the sensor reports none, and `time` is left
  out until NTP has synced. rtl_433's `learn`, `code`, `mic` and `unknown`
  fields are not sent. `rssi` (dBm,
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
    least 6 dB above the noise floor, decoded or not (last 24). On the CC1101
    each also has `foff` (Hz, the chip's frequency offset estimate at the sync
    word) and `lqi` (0–127, lower = cleaner). `/api/rflog` also has `bad`, the
    number of invalid Manchester pairs, and `bad_at`, the damaged bit positions
    when there are only a few.

  `near_miss` counts Manchester failures with 1–4 bad pairs. Noise payloads
  have about 30, so these are real packets with a few damaged chips.
  `bad_bit_hist` counts, per payload bit, how often that bit was the damaged
  one; damage clustered at the start points at demodulator settling (FOC, bit
  sync), damage spread out at SNR. Per sensor, `/api/state` (and `sensors` on
  the CLI) has `burst_pkts` (of 4 in the latest burst), `foff` latest / mean /
  min / max and `lqi` latest / mean. Use the mean offsets to center `radio
  freq`, and their spread to size `radio bw`.

  How to read it: strong events with **no syncs** mean the packet engine never
  matched the sync word, so suspect polarity (`radio sync aaa9` or `5555`) or
  frequency. **Syncs but no decodes** with strong payloads mean the sync
  matched but the data was wrong (bit rate, deviation, bandwidth). Paste the
  hex for offline analysis.

* `pll` in `metrics` / `"pll"` in `/api/state` and `/api/rflog` must be locked/`true`.
  If it isn't, the radio can't tune that frequency (see [Hardware](#hardware))
  and nothing else in this list matters. Sync matches still climb from noise
  when the PLL is unlocked, so they don't prove the radio is receiving.
* `metrics`: with the PLL locked, `sync matches` should slowly climb even with no sensor around
  (noise matches the sync word about once every 5–10 s). If it stays at 0 the
  radio isn't receiving.
* Sync polarity: the default (`5556`) assumes the higher FSK tone is '1', which
  matches the rtl_433 reference captures. If syncs come in but sensors never
  decode, try `radio sync 5555`. It accepts either polarity, at the cost of
  needing a little more clean preamble.
* Frequency offset: sensors can be ±20 kHz off. Use `radio bw 100` or
  `radio freq 315.02` to compensate.
* Auto-tune (AFC, CC1101 only): the *Auto-tune* checkbox in the Radio dialog
  (`radio afc on`, `afc=1` on `/api/radio`). Each decoded packet moves an
  estimate 1/10 of the way towards its carrier's offset from the configured
  frequency. With auto-tune on, the estimate is tuned in as an overlay once no
  packet has arrived for 3 s, if it moved at least 1 kHz (clamped to ±half the
  bandwidth). The header then shows the overlay, e.g. `315.023 MHz (+1,586Hz)`.
  The checkbox is saved, but the estimate and overlay are not: they start at 0
  at boot and whenever the configured frequency changes. *Apply* copies
  frequency + estimate into the Frequency field, which Engage saves. `afc` in
  `/api/state` and `/api/rflog` has `est`, `applied` (Hz) and `n` (packets).
* `raw on` shows every payload, and the decode error tells how far it got
  (`manchester` < `checksum` < `flags`).

## Source layout

| File | Purpose |
|---|---|
| `src/main.c` | Startup, packet sink (log + table + LED), ROM-loader reboot, crash-loop rescue |
| `src/board.h` | Pin maps per board (chosen by the build target) |
| `src/radio.c` | Chip-independent receive task, decoding, stats, RSSI history, RF log, PLL scan |
| `src/radio_hw.h` | Interface the chip backends implement |
| `src/radio_cc1101.c` | CC1101 2-FSK backend, lock checks, wiring probe (ESP32 build) |
| `src/radio_lr1121.c` | LR1121 GFSK backend, lock checks, wiring probe (C3 build) |
| `src/led.c` | Packet LED (ESP32 DevKit); WS2812B status LED and button (C3) |
| `src/tpms_ford.c` | Portable decoder (port of rtl_433 `tpms_ford.c`) |
| `src/sensors.c` | 20-entry unique-ID table with burst folding |
| `src/display.c` | SSD1306 driver, screen scheduling, dimming, pixel shift |
| `src/ui.c`, `src/gfx.c` | Portable screen layouts, fonts, 7-segment renderer |
| `src/cli.c` | esp_console commands |
| `src/net.c` | Wi-Fi station, hostname, mDNS, SNTP |
| `src/web.c`, `src/web/index.html` | HTTP server, WebSocket push, the web UI page |
| `src/ota.c` | Firmware update over Wi-Fi (`POST /update`) and rollback confirmation |
| `src/ha_mqtt.c` | MQTT state messages and Home Assistant discovery |
| `src/tires.c` | Sensor ID to wheel position assignments |
| `src/settings.c` | NVS storage for radio, display and tire settings |
| `src/wifi_config.example.h` | Template for `src/wifi_config.h` (not in git) |
| `tools/c3_upload.py` | PlatformIO pre-upload hook: `bootloader` command for the C3 board |
| `tools/ota_upload.py` | Uploader for the `*-ota` envs |
| `test/host/` | Host tests with real captures from rtl_433_tests, UI preview |
