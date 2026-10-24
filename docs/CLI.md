# Serial CLI

The firmware has an `esp_console` command line on the USB serial port. Log
messages and command output share the same port.

## Connecting

| Setting | ESP32 DevKit + CC1101 | ESP32-C3 + LR1121 |
|---|---|---|
| Port | `/dev/ttyUSB0` (the DevKit's CP2102/CH340 bridge) | `/dev/ttyAMA0` (CH340 USB-TTL adapter) |
| UART | ESP32 **UART0**, GPIO1 (TX) / GPIO3 (RX) | ESP32-C3 **UART0**, GPIO21 (TX) / GPIO20 (RX) |
| Speed | 115200 8N1 | 115200 8N1 |
| Prompt | `tpms>` | `tpms>` |

```bash
cd firmware && pio device monitor -e esp32-cc1101      # or: picocom -b 115200 /dev/ttyUSB0
cd firmware && pio device monitor -e c3-lr1121         # or: picocom -b 115200 /dev/ttyAMA0
```

On the ESP32 DevKit, opening the port may reset the board (the bridge's DTR/RTS
auto-reset), so the boot log scrolls past first. Wait for `tpms>` before
typing. The C3 board has no auto-reset circuit: opening the port never resets
it. Use [`reboot`](#reboot) to see a boot log.

If the terminal doesn't answer the console's escape-sequence probe, the
console prints *"Your terminal application does not support escape
sequences"*. Commands still work, but arrow-key history and line editing are off.

Commands that fail print `Command returned non-zero error code: 0x1 (ERROR)`.
`radio`, `display` and `tires` settings are saved in flash (NVS) and survive
reboots and firmware updates. The web UI changes the same settings. Other
CLI changes (`raw`, `loglevel`) last until reboot.

## Command summary

| Command | Purpose |
|---|---|
| [`help`](#help) | List commands |
| [`metrics`](#metrics--stats) / `stats` | Receiver, radio and display metrics |
| [`sensors`](#sensors) | Table of the last 20 unique sensor IDs |
| [`tires`](#tires) | Assign sensor IDs to wheels (only assigned IDs go to MQTT) |
| [`wifi`](#wifi) | Wi-Fi, web UI, clock and MQTT status |
| [`radio`](#radio) | Show or change radio settings (saved) |
| [`raw`](#raw) | Log every received payload |
| [`rflog`](#rflog) | RF energy events and strong raw payloads |
| [`regs`](#regs) | Dump CC1101 registers and state, or LR1121 status, errors and counters |
| [`probe`](#probe) | Check the radio's control lines (DIO/IRQ, RESET, BUSY) |
| [`pllscan`](#pllscan) | Report PLL lock across frequencies |
| [`inject`](#inject) | Feed a test packet through the pipeline |
| [`tx`](#tx) | Transmit test Ford packets over the air |
| [`display`](#display) | Show or change display brightness, dimming and shift (saved) |
| [`screen`](#screen) | Print the current OLED frame as text |
| [`loglevel`](#loglevel) | Change log verbosity |
| [`tasks`](#tasks) | Task stack headroom |
| [`reboot`](#reboot) | Restart the ESP32 |
| [`bootloader`](#bootloader) | Reboot into the ROM download mode for flashing (ESP32-C3) |

---

## help

```
help [<command>] [-v 0|1]
```

Without arguments, lists every command with its arguments and a one-line
description. `help <command>` shows a single command.

## metrics / stats

```
metrics [reset]
```

Prints a snapshot of the whole receiver. `metrics reset` zeroes the radio
counters (the noise floor is kept). `stats` is an alias. Lines for hardware the
board doesn't have (display) are left out. C3 board example:

```
tpms> metrics
firmware      fordtpms 1 (IDF 6.1.0)
uptime        0d 00:00:11
heap free     273816 (min 270044)
wifi          192.168.2.136, -44 dBm, http://192.168.2.136/ (fordtpms.local)
--- radio (LR1121 hw 0x22 type 0xf3 fw 01.04) ---
frequency     315.000 MHz
pll           locked (0 checks unlocked since reset)
chip rate     19231 /s, rx bw 78100 Hz, sync 5556
noise floor   -110 dBm
sync matches  2
payloads      2
decoded ok    0
err manchest. 2
err checksum  0
err flags     0
fifo overrun  0
irq pay/sync  2 / 2 (polled 0)
last rssi     -83 dBm
```

On the CC1101 board the radio header reads `--- radio (CC1101 part 0x00 v0x14) ---`
and `rx bw` is 81250 Hz.

| Field | Meaning |
|---|---|
| `heap free` | Current free heap, and the lowest it has been since boot |
| `wifi` | IP address, AP signal and web UI URL; `connecting` or `off (no SSID in wifi_config.h)` otherwise. Details: [`wifi`](#wifi) |
| chip in the header | CC1101: PARTNUM and VERSION (0x14 on genuine parts; some clones differ, which only logs a warning). The LR1121 shows GetVersion: hardware, use case (0x03 stock, 0xf3 ExpressLRS firmware) and firmware version. `NOT FOUND` if the chip didn't answer |
| `pll` | `locked`, or `NOT LOCKED` when the radio can't tune the set frequency. Unlocked means the receiver hears nothing. The count is lock checks that failed (every 250 ms) |
| `chip rate` / `rx bw` / `sync` | Current [`radio`](#radio) settings. Both radios use AGC |
| `noise floor` | Smoothed RSSI between packets, as the radio reports it. On the C3 board this includes its external LNA's gain |
| `sync matches` | Times the radio matched the sync word `0x55 0x56`. Noise produces one roughly every 5–10 s. It climbs even with the PLL unlocked, so check `pll` too |
| `payloads` | Payloads read from the FIFO after a sync match |
| `decoded ok` | Valid Ford packets received **over the air**. `inject` doesn't count here |
| `err manchest.` | Payload contained an invalid Manchester chip pair: almost always noise |
| `err checksum` | Manchester-valid but the checksum failed: a weak or colliding real packet, or a different protocol |
| `err flags` | Checksum ok but flag bits rtl_433 rejects, or an all-zero payload |
| `fifo overrun` | The radio FIFO overflowed (CC1101: RX FIFO overflow; LR1121: packet length error). Should stay 0 |
| `irq pay/sync` | Payload / sync events signalled by interrupt (CC1101 GDO0 / GDO2, LR1121 IRQ line). `polled` counts events found by the 5 ms fallback poll instead. It should stay 0 when the interrupt lines work |
| `last rssi` | RSSI latched at the most recent sync match |
| `rssi best/wst` | Best and worst RSSI of decoded packets (only shown after one has been decoded) |
| `last packet` | Time since the last packet decoded over the air (only shown after one has been decoded) |
| `present` | Whether an SSD1306 answered at boot (the section is left out on boards without a display, currently all of them) |
| `screen` | Current screen (`idle`, `detail`, `overview`), brightness state, and burn-in shift offset |
| `frames` | Frames drawn, time to render and send one frame, and I2C error count |

## tires

```
tires [fl|fr|rl|rr <id|clear>]
```

Shows or sets which sensor ID is on which wheel. Packets carry no position,
so the receiver only knows this from you. Only assigned IDs are published over
MQTT. Assignments are saved in flash. An ID can be assigned before it has been
heard, and assigning an ID that is on another wheel moves it. The web UI does
the same from the Tire Positions panel.

```
tpms> tires rl 364343a4
I (21233) tpms: tire RL: 00000000 -> 364343a4
FL Front Left  (none)
FR Front Right (none)
RL Rear Left   364343a4   38.00 PSI  0:02 ago
RR Rear Right  (none)
assigned IDs are published over MQTT; saved in flash
```

`not heard since boot` means the ID is assigned but no packet has arrived yet.
`tires rl clear` stops publishing that sensor. Its Home Assistant device is
not deleted.

## wifi

```
wifi
```

Network status. Settings come from `firmware/src/wifi_config.h` at build time
(see the [firmware README](../firmware/README.md#wi-fi-web-ui-and-mqtt)).

```
tpms> wifi
ssid          myssid
state         connected
hostname      fordtpms (mDNS fordtpms.local)
ip            192.168.2.136
ap rssi       -44 dBm
web ui        http://192.168.2.136/  (1 live clients)
reconnects    0
time          2026-10-14 21:37:29
mqtt          mqtt://mqtt.lan:1883 connected, 2 published, 1 connects
```

| Field | Meaning |
|---|---|
| `state` | `connecting` until associated with an IP address. Retries back off from 1 s to 30 s |
| `web ui` | URL and number of open WebSocket connections (browser tabs) |
| `reconnects` | Wi-Fi drops since boot |
| `time` | Local time from NTP (`TIME_ZONE`), or `not synced` |
| `mqtt` | Broker, state, readings published, and connections since boot |

## sensors

```
sensors [clear]
```

Table of the last 20 **unique** sensor IDs, most recently heard first. Each ID
appears once. When a 21st ID arrives, the ID heard least recently is dropped.
`sensors clear` empties the table, and the display returns to the idle screen.

```
tpms> sensors
 # ID          PSI  kPa  Temp Mode   Code   Flg RSSI Bursts  Pkts Last        Age
-- -------- ------ ---- ----- ------ ------ --- ---- ------ ----- -------- ------
 1 45aa55aa  35.00  241    7C REST   8c3f05  05 -101      1     1 00:00:08   0:01
 2 45d30b69  32.50  224    -- MOVING 82d446  46  -64      1     1 00:00:07   0:03
 3 45bb320f  26.50  183    -- MOVING 6ad446  46  -80      1     1 00:00:05   0:04
3 of max 20 unique IDs; Last = uptime of last packet
```

| Column | Meaning |
|---|---|
| `ID` | Sensor ID (32 bits, hex), same as rtl_433 |
| `PSI` / `kPa` | Pressure. The packet carries quarter-PSI steps, plus a 9th bit for >127.75 PSI |
| `Temp` | °C, or `--` if this packet's temperature byte is not a temperature (0x80 set) |
| `Mode` | `MOVING`, `REST` or `LEARN` (triggered by a TPMS tool) |
| `Code` | rtl_433 `code`: packet bytes 4–6 in hex |
| `Flg` | Flags byte (byte 6) |
| `RSSI` | Best RSSI within the latest burst, in dBm |
| `Bursts` | Distinct transmissions. Packets from one ID less than 3 s apart are repeats of one burst |
| `Pkts` | All packets including repeats (a burst is usually about 4) |
| `Last` | Device uptime (`hh:mm:ss`) when the last packet arrived |
| `Age` | Time since then (`m:ss`, `hHmm` after an hour, `nD` after 100 h) |

## radio

```
radio [freq <MHz>] [bw <kHz>] [sync 5556|aaa9|5555] [bitrate <chips/s>]
radio defaults
```

Without arguments, shows the current settings. With `key value` pairs, changes
them, reprograms the radio immediately (including image calibration for the
new frequency) and saves them. `radio defaults` goes back to 315.000 MHz,
the chip's filter nearest ±83.3 kHz, sync 5556, 19231 chip/s. The web UI's
**Radio** dialog does the same; its limits, bandwidth list and defaults come
from the device, so they always match the fitted chip.

```
tpms> radio
freq 315.000 MHz, bitrate 19231, bw 78100 Hz, sync 5556
tpms> radio freq 315.02 bw 100
I (6015) radio: reconfigured: 315.020 MHz, 19231 chip/s, RxBw 93600 Hz
```

A bandwidth the chip can't do exactly is rounded to the nearest supported value:

```
tpms> radio bw 85
I (6013) radio: reconfigured: 315.000 MHz, 19231 chip/s, RxBw 78100 Hz
```

| Key | Default | Range / values | Notes |
|---|---|---|---|
| `freq` | 315.0 | CC1101 300–928 MHz (specified 300–348, 387–464, 779–928), LR1121 150–960 MHz | Receive frequency. Sensor crystals can be ±20 kHz off |
| `bw` | 83.3 (snapped: CC1101 81.3, LR1121 78.1) | the chip's filters | Single-sided channel filter. Rounded to the nearest value the chip supports (CC1101 e.g. 50.8, 58.0, 67.7, 81.3, 101.6, 116.1; LR1121 e.g. 58.6, 78.1, 93.6, 117.2); `radio` then shows the value actually used. Ford deviation is about ±35 kHz, so don't go much below 62.5 |
| `sync` | `5556` | `5556`, `aaa9`, `5555` | `5556`: higher FSK tone = 1, matching the rtl_433 reference captures. `aaa9`: inverted polarity. `5555`: accepts either polarity. It syncs inside the preamble, reads 24 bytes and searches for the packet in software, but needs a little more clean preamble |
| `bitrate` | 19231 | CC1101 600–500000, LR1121 600–300000 | Chip rate (52 µs per chip). Only change this for experiments |

Invalid values are rejected with `invalid: ESP_ERR_INVALID_ARG`.

## raw

```
raw on|off
```

Logs every payload the radio delivers after a sync match, as hex, with the
RSSI and the decode result. This is useful for spotting near-misses: an error
that got further into the packet (`manchester` < `checksum` < `flags`) points
to a real but damaged packet.

```
I (98824) radio: raw d0d45431c0826a2445a855f13391f6ef rssi=-84 -> manchester
```

The 16 bytes are the 128 raw chips after the sync word, before Manchester
decoding.

## rflog

```
rflog [clear]
```

Reception diagnostics collected in RAM since boot (or `rflog clear`). The
same data is served as JSON at `http://<ip>/api/rflog`, so an unattended
run can be checked without opening the serial port (opening it can reset
the board).

```
tpms> rflog            (illustrative output)
RF events (peak >= noise + 12 dB), 2 since boot, last 2:
start                dur ms  peak noise syncs  pays   dec
2026-10-15 03:12:41     757   -61   -85     4     4     4
2026-10-15 04:02:10     505   -70   -85     0     0     0

Strongest 2 events since boot:
...

Strong payloads (sync RSSI >= noise + 6 dB), 4 since boot, last 4:
2026-10-15 03:12:41  -62 dBm (noise -85) ok         <raw payload hex>
(also at http://<ip>/api/rflog)
```

| List | Contents |
|---|---|
| RF events | Last 48 runs of 250 ms windows whose peak RSSI was at least 12 dB above the noise floor: start (local time, or uptime before NTP), duration, peak, noise floor, and syncs/payloads/decodes during the run |
| Strongest events | The 16 loudest events since boot, so a real burst isn't pushed out by later weak ones |
| Strong payloads | Last 24 raw payloads whose sync RSSI was at least 6 dB above the noise floor, decoded or not, with the decode result |

A strong event with no syncs means the sync word never matched (polarity or
frequency). Syncs without decodes mean the data was wrong (bit rate,
deviation or bandwidth).

## regs

On the CC1101, dumps the configuration registers 0x00–0x2E, 16 per line (the
label is the first register's address), then the state:

```
tpms> regs
00: <16 config registers>
10: ...
20: ...
part/version  0x00 / 0x14
marcstate     0x0d RX
pll           locked (FSCAL1 0x..), RX re-entered 0 times, 0 periodic recalibrations
rssi now      ... dBm
last packet   frequency offset +... Hz, LQI ...
pktstatus     0x.. (GDO0 pin 0, GDO2 pin 0), rxbytes 0x00
```

`frequency offset` is FREQEST, the offset the CC1101 measured on the last
packet: how far that sensor's crystal is off. `RX re-entered` counts lock checks
that found the chip out of RX or uncalibrated and restarted it.

The LR1121 has no register map to dump; `regs` shows its status instead:

```
tpms> regs
version       hw 0x22, use case 0xf3, fw 01.04
stat1/stat2   0x04 / 0x09: mode rx, running from flash, reset status 0, last cmd status 2
irq status    0x00000000 (DIO pin 0)
errors        0x0000 (last check 0x0000)
pll           locked, RX re-entered 0 times
rssi now      -103 dBm
gfsk stats    0 received, 0 crc err, 0 length err
rx buffer     last length 0 at offset 0
last packet   rssi sync 0, avg 0 dBm, len 0, status 0x00
temperature   34.4 C
vbat          3.31 V
```

`errors` decodes GetErrors (`PLL_LOCK`, `PLL_CALIB`, `IMG_CALIB`,
`HF_XOSC_START`). `RX re-entered` counts the times the lock check found the chip
out of RX and restarted it.

## probe

Hardware diagnostic for the radio's wiring. Reception pauses briefly and the
chip is reset and reconfigured afterwards.

On the CC1101 it resets the chip, checks that SO goes low after CSn (chip
ready), reads PARTNUM/VERSION, writes and reads back a register, then drives
GDO0 and GDO2 high and low (with the ESP32's pull set against each level, so
an unconnected line reads wrong):

```
tpms> probe
SO/CSn        chip ready (SO low after CSn)
SPI           PARTNUM 0x00 VERSION 0x14, write/read 0xa5 -> 0xa5, 0x5a -> 0x5a -> OK
GDO0 (end of packet) GPIO17   driven high reads 1, driven low reads 0 -> WIRED
GDO2 (sync word)     GPIO16   driven high reads 1, driven low reads 0 -> WIRED
```

On the LR1121 the pins are known, so `probe` checks them instead: BUSY must be
high in reset and go low ~230 ms after release, GetVersion must answer over
SPI, and the IRQ line must rise on an RX timeout interrupt and fall when it is
cleared. The chip is reset during the test and then reconfigured.

```
tpms> probe
RESET GPIO2 / BUSY GPIO3: busy 1 in reset, 1 after release, low after 229 ms -> OK
SPI           GetVersion 22 f3 01 04 (ESP_OK)
IRQ GPIO1     idle 0, after RX timeout 1 (irq 0x00000400), after clear 0 -> WIRED
```

## pllscan

```
pllscan [<start MHz> <stop MHz> <step MHz>]
```

Tunes the synthesizer to each frequency, reports whether it locked, then puts
the receiver back on the configured frequency. Frequencies outside the
`radio` range are allowed, so you can check that the lock detection really
catches a failure. Without arguments it tests reference points (100, 137, 175,
250, 433.92, 868, 915, 1000 MHz) and 300–330 MHz in 5 MHz steps. At most 400
steps; reception pauses during the scan.

CC1101: lock = a manual calibration (SCAL) leaves `FSCAL1` != 0x3F. LR1121: lock = no `PLL_LOCK`
error after SetFs and the chip in FS mode; on a failed lock the chip falls
back to standby. Measured on the C3 board's LR1121: lock from 40 to 2890 MHz,
including all of 300–330 MHz.

```
tpms> pllscan 20 110 5
   20.000 MHz  NO LOCK  errors 0x0080, mode stby_xosc
 ...
   35.000 MHz  NO LOCK  errors 0x0080, mode stby_xosc
   40.000 MHz  locked   errors 0x0000, mode fs
 ...
15 of 19 frequencies locked; receiver back on 315.000 MHz (locked)
```

## inject

```
inject [<7 or 8 hex bytes>] [<rssi dBm>]
```

Feeds a packet through the same path as a received one: validity checks,
sensor table, log line, LED and display. Use it to test the UI without a
sensor. The bytes are the decoded packet `ID ID ID ID PP TT FF CS`. With 7
bytes the checksum is calculated for you; with 8 it is checked. RSSI defaults
to −80. With no arguments, it injects an rtl_433 sample packet (ID `45bb320f`,
26.5 PSI, moving).

```
tpms> inject
I (7512) tpms: packet id=45bb320f pressure=26.50 PSI temp=n/a mode=MOVING code=6ad446 flags=46 rssi=-80 dBm
tpms> inject 45d30b698a5446 -64
I (9018) tpms: packet id=45d30b69 pressure=34.50 PSI temp=28C mode=MOVING code=8a5446 flags=46 rssi=-64 dBm
tpms> inject 45bb320f6ad44600
rejected: checksum
tpms> inject 00000000000000
rejected: zero
```

Building a packet: `PP` = PSI x 4; `TT` = °C + 56 (add 0x80 for "no
temperature"); `FF` = 0x04 at rest, 0x44 moving, 0x08 learn, with 0x20 as the
9th pressure bit. Injected packets don't change the radio counters in `metrics`.

## tx

```
tx [<7 or 8 hex bytes>] [count <n>] [gap <ms>] [pa <dBm> | rfo <dBm>]
```

Transmits real Ford TPMS packets at the current [`radio`](#radio) frequency
(FSK, ±35 kHz deviation, 19 231 chip/s), then returns to receive. Use it to
test another receiver (rtl_433, a second board) or a board's transmit path.
Unlike [`inject`](#inject), the packets go over the air, so this board's own
counters don't change. Reception pauses while it sends.

The packet bytes work as for `inject`: 7 bytes get the checksum added, 8 are
sent as given (with a warning if they don't decode). The default is a fake
sensor, ID `feedf00d`, 38 PSI, 29 °C, at rest.

| Option | Default | Range | Meaning |
|---|---|---|---|
| `count` | 4 | 1–20 | Packets to send (real sensors send about 4 per burst) |
| `gap` | 50 | 0–1000 ms | Pause between packets |
| `pa` | see below | see below | Power on the high-power PA path |
| `rfo` | | see below | Power on the low-power PA path |

| Chip | `pa` (high-power path) | `rfo` (low-power path) | Default |
|---|---|---|---|
| CC1101 | one PA, −30 to 10 dBm: −30, −20, −15, −10, 0, 5, 7, 10 (rounded down) | same as `pa` | `pa -10` |
| LR1121 | HP PA, −9 to 22 dBm | LP PA, −17 to 14 dBm | `pa -9` |

Use the path the board's antenna is wired to. ExpressLRS LR1121 receivers,
including the C3 board, use the HP PA for sub-GHz. A board whose ExpressLRS
target sets `radio_rfo_hf` (e.g. the Radiomaster Nomad) uses the LP PA. The
other path mostly leaks, so it radiates very little.

```
tpms> tx
I (27997) radio: tx 4 frame(s) pa -9 dBm at 315.000 MHz: ESP_OK
tx feedf00d985506db x4 pa -9 dBm: ESP_OK
tpms> tx 45bb320f6ad446 count 2 gap 100 rfo -17
```

From the C3 board with its 315 MHz antenna, `tx` at the minimum `pa -9` was
decoded 4 of 4 by the rtl_433 reference receiver.

Notes:

* Keep the power low and the bursts short: 315 MHz is a licence-exempt band
  only for low-power, intermittent transmissions, and a nearby receiver needs
  very little.
* If the PLL isn't locked at the current frequency, `tx`
  warns and sends nothing usable on that frequency.
* The return value is `ESP_ERR_INVALID_ARG` for a power outside the path's
  range, `ESP_ERR_TIMEOUT` if a packet never finishes, and
  `ESP_ERR_INVALID_STATE` if the PLL reports a lock error while transmitting.

## display

```
display [bright <0-255>] [dim <0-255>] [dim_after <s>] [shift <s>]
display wake
display defaults
```

Without arguments, shows the settings. Changes apply immediately and are saved.
`display defaults` restores the defaults below. The web UI's **Display**
dialog does the same.

```
tpms> display
bright 143, dim 1, dim_after 120 s, shift 30 s
tpms> display dim_after 300 bright 200
```

| Key | Default | Meaning |
|---|---|---|
| `bright` | 143 | Contrast while active |
| `dim` | 1 | Contrast after inactivity. The display is never switched off |
| `dim_after` | 120 | Seconds without a new transmission before dimming |
| `shift` | 30 | Seconds per one-pixel burn-in shift step (minimum 1) |

`display wake` restores full brightness and restarts the inactivity timer.
See [UI.md](UI.md) for the screens and burn-in protection.

## screen

Prints the frame currently on the OLED as text, using block characters
(two pixel rows per line). Use it to check the display remotely.

```
tpms> screen
+--------------------------------------------------------------------------------------------------------------------------------+
| ▀█▀  █▀▀▄          ▄█  █▀▀▀▀ █▀▀▀▄ █▀▀▀▄ ▀▀▀█▀ ▄▀▀▀▄ ▄▀▀▀▄ █▀▀▀▀                                                 █ █   ▄█      |
|  █   █   █       ▄▀ █  ▀▀▀▀▄ █▄▄▄▀ █▄▄▄▀   ▀▄     ▄▀ █ ▄▀█ █▄▄                                                  ▀█▀█▀   █      |
...
```

## loglevel

```
loglevel <tag|*> <none|error|warn|info|debug|verbose>
```

Sets the ESP-IDF log level for one tag, or for all tags with `*`. The default is
`info`.

| Tag | INFO | DEBUG adds |
|---|---|---|
| `tpms` | First packet of each burst | Repeat packets within a burst (`repeat id=…`) |
| `radio` | Startup, reconfiguration, `raw` output | Raw hex of every **valid** packet |
| `display` | Panel detected or missing | Dim/bright transitions |

Example: `loglevel tpms debug` to see every repeat transmission.

## tasks

Shows free stack space (in bytes) for the firmware's main tasks.

```
tpms> tasks
radio      stack free 3316
web        stack free 4364
httpd      stack free 4068
mqtt_task  stack free 4772
IDLE0      stack free 920
IDLE1      stack free 980
```

The single-core ESP32-C3 has one idle task, `IDLE`.

## reboot

Restarts the ESP32. The sensor table and `raw`/`loglevel` changes are lost;
radio, display and tire settings are kept.

## bootloader

ESP32-C3 only. Reboots into the ROM download mode (the LED turns magenta), so
esptool can flash the board even though it has no DTR/RTS auto-reset circuit.
Flash with `--before no_reset --after watchdog_reset`; the watchdog reset then
starts the new firmware. `pio run -e c3-lr1121 -t upload` sends this command
for you. On the classic ESP32 it prints "not supported".

---

## Log line reference

```
I (8513) tpms: packet id=45bb320f pressure=26.50 PSI temp=n/a mode=MOVING code=6ad446 flags=46 rssi=-80 dBm
│  │      │     │      │           │                   │        │           │           │        └ RSSI at sync
│  │      │     │      │           │                   │        │           │           └ flags byte
│  │      │     │      │           │                   │        │           └ rtl_433 code (bytes 4-6)
│  │      │     │      │           │                   │        └ MOVING / REST / LEARN
│  │      │     │      │           │                   └ °C, or n/a
│  │      │     │      │           └ PSI
│  │      │     │      └ sensor ID
│  │      │     └ "packet" = first of a burst (INFO), "repeat" = later copies (DEBUG)
│  │      └ log tag
│  └ ms since boot
└ level (E/W/I/D/V)
```
