# Serial CLI

The firmware has an `esp_console` command line on the USB serial port. Log
messages and command output share the same port.

## Connecting

| Setting | Value |
|---|---|
| Port | `/dev/ttyUSB0` (CP2104 bridge, `/dev/serial/by-id/usb-Silicon_Labs_CP2104_…`) |
| Speed | 115200 8N1 |
| UART | ESP32 **UART0**, GPIO1 (TX) / GPIO3 (RX), wired to the USB bridge |
| Prompt | `tpms>` |

```bash
cd firmware && pio device monitor     # or: picocom -b 115200 /dev/ttyUSB0
```

Opening the port may reset the board (the CP2104's DTR/RTS auto-reset), so
the boot log scrolls past first. Wait for `tpms>` before typing.

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
| [`regs`](#regs) | Dump SX1278 registers |
| [`probe`](#probe) | Find the SX1278 DIO/RESET wiring |
| [`inject`](#inject) | Feed a test packet through the pipeline |
| [`display`](#display) | Show or change display brightness, dimming and shift (saved) |
| [`screen`](#screen) | Print the current OLED frame as text |
| [`loglevel`](#loglevel) | Change log verbosity |
| [`tasks`](#tasks) | Task stack headroom |
| [`reboot`](#reboot) | Restart the ESP32 |

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
counters (the noise floor is kept). `stats` is an alias.

```
tpms> metrics
firmware      fordtpms 1 (IDF 6.1.0)
uptime        0d 00:00:11
heap free     273816 (min 270044)
wifi          192.168.2.136, -44 dBm, http://192.168.2.136/ (fordtpms.local)
battery       4012 mV
--- radio (SX1278 v0x12) ---
frequency     315.000 MHz
chip rate     19231 /s, rx bw 83333 Hz, lna G1, sync 5556
noise floor   -83 dBm
sync matches  2
payloads      2
decoded ok    0
err manchest. 2
err checksum  0
err flags     0
fifo overrun  0
irq dio0/dio2 2 / 2 (polled 0)
last rssi     -83 dBm
--- display ---
present       yes
screen        detail, bright, shift (0,0)
frames        117 (26713 us/frame), i2c errors 0
```

| Field | Meaning |
|---|---|
| `heap free` | Current free heap, and the lowest it has been since boot |
| `wifi` | IP address, AP signal and web UI URL; `connecting` or `off (no SSID in wifi_config.h)` otherwise. Details: [`wifi`](#wifi) |
| `battery` | Battery voltage after the 2:1 divider. About 4.0–4.2 V on USB power |
| `SX1278 v0x12` | Radio version register; 0x12 means an SX1278 was found |
| `chip rate` / `rx bw` / `lna` / `sync` | Current [`radio`](#radio) settings |
| `noise floor` | Smoothed RSSI between packets. About −83 dBm is normal at 315 MHz |
| `sync matches` | Times the radio matched the sync word `0x55 0x56`. Noise produces one roughly every 5–10 s, so a slow climb shows the radio is receiving |
| `payloads` | Payloads read from the FIFO after a sync match |
| `decoded ok` | Valid Ford packets received **over the air**. `inject` doesn't count here |
| `err manchest.` | Payload contained an invalid Manchester chip pair: almost always noise |
| `err checksum` | Manchester-valid but the checksum failed: a weak or colliding real packet, or a different protocol |
| `err flags` | Checksum ok but flag bits rtl_433 rejects, or an all-zero payload |
| `fifo overrun` | The radio FIFO overflowed. Should stay 0 |
| `irq dio0/dio2` | PayloadReady / SyncAddress interrupts. `polled` counts events found by the 5 ms fallback poll instead of an interrupt. It should stay 0 when the DIO lines work |
| `last rssi` | RSSI latched at the most recent sync match |
| `rssi best/wst` | Best and worst RSSI of decoded packets (only shown after one has been decoded) |
| `last packet` | Time since the last packet decoded over the air (only shown after one has been decoded) |
| `present` | Whether the SSD1306 answered at boot |
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
radio [freq <MHz>] [bw <kHz>] [lna <1-6>] [sync 5556|aaa9|5555] [bitrate <chips/s>]
radio defaults
```

Without arguments, shows the current settings. With `key value` pairs, changes
them, reprograms the SX1278 immediately (including image calibration for the
new frequency) and saves them. `radio defaults` goes back to 315.000 MHz,
83.3 kHz, LNA 1, sync 5556, 19231 chip/s. The web UI's **Radio** dialog does
the same.

```
tpms> radio
freq 315.000 MHz, bitrate 19231, bw 83333 Hz, lna 1, sync 5556
tpms> radio freq 315.02 bw 100
I (6015) radio: reconfigured: 315.020 MHz, 19231 chip/s, RxBw 100000 Hz, LNA G1
```

A bandwidth the chip can't do exactly is rounded to the nearest supported value:

```
tpms> radio bw 90
I (6013) radio: reconfigured: 315.000 MHz, 19231 chip/s, RxBw 83333 Hz, LNA G1
```

| Key | Default | Range / values | Notes |
|---|---|---|---|
| `freq` | 315.0 | 137–525 MHz | Receive frequency. Sensor crystals can be ±20 kHz off |
| `bw` | 83.3 | 2.6–250 kHz | Single-sided channel filter. Rounded to the nearest value the chip supports (e.g. 50, 62.5, 83.3, 100, 125, 166.7, 200, 250); `radio` then shows the value actually used. Ford deviation is about ±35 kHz, so don't go much below 62.5 |
| `lna` | 1 | 1 (max gain) … 6 (min) | Fixed LNA gain; AGC is off. Use a lower gain only for very strong nearby signals |
| `sync` | `5556` | `5556`, `aaa9`, `5555` | `5556`: higher FSK tone = 1, matching the rtl_433 reference captures. `aaa9`: inverted polarity. `5555`: accepts either polarity. It syncs inside the preamble, reads 24 bytes and searches for the packet in software, but needs a little more clean preamble |
| `bitrate` | 19231 | 1200–300000 | Chip rate (52 µs per chip). Only change this for experiments |

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

Dumps SX1278 registers 0x01–0x7F, 16 per line. The label at the start of each
line is the first register's address.

```
tpms> regs
01: 0d 06 7f 00 52 4e c0 00 4f 09 2b 20 00 02 0a ff
11: a4 12 12 28 0c 12 47 32 3e 00 00 00 00 50 00 00
...
```

Line `01:` shows, for example, RegOpMode=0x0d (FSK, LF band, RX), bit rate
0x067f, and Frf=0x4ec000 (315 MHz).

## probe

Hardware diagnostic that finds which ESP32 GPIOs the SX1278's DIO pins and
RESET are wired to. It briefly puts the radio in continuous mode
(DIO0=RxReady, DIO1=bit clock, DIO2=data), counts edges on candidate GPIOs in
standby and in RX, pulses the RESET line, then restores the normal
configuration. Reception pauses for about 0.1 s.

```
tpms> probe
SX1278 continuous mode, DIO0=RxReady DIO1=DCLK DIO2=DATA
GPIO  standby(lvl,edges/20ms)  rx(lvl,edges/50ms)  guess
   0  1      0                  1      0
 ...
  26  0      0                  1      0            DIO0 (RxReady)
  32  0      0                  1    449            DIO2 (DATA)?
  33  0      0                  0   1974            DIO1 (DCLK ~19.2 kHz)
  34  0    120                  0    286
 ...
RESET on GPIO23: reg 0x2f wrote 0xa5, read 0xa5, after reset 0x55 -> WIRED
```

On this board DIO0 = GPIO26, DIO1 = GPIO33, DIO2 = GPIO32 and RESET = GPIO23.
GPIO34 toggles even in standby, so it isn't driven by the radio.

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
display    stack free 2852
web        stack free 4364
httpd      stack free 4068
mqtt_task  stack free 4772
IDLE0      stack free 920
IDLE1      stack free 980
```

## reboot

Restarts the ESP32. The sensor table and `raw`/`loglevel` changes are lost;
radio, display and tire settings are kept.

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
