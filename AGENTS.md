# AGENTS.md

Notes for coding agents working in this repo. Read this first, then
`firmware/README.md` (the full reference) as needed.

## What this is

ESP-IDF firmware that receives **Ford TPMS tire-pressure sensors at 315 MHz**
(the `tpms_ford` decoder from rtl_433) and shows them on a serial CLI, an
optional SSD1306 OLED, a live web UI, and MQTT with Home Assistant discovery.

Two boards, one PlatformIO environment each:

| Env | Board | Radio | Status |
|---|---|---|---|
| `esp32-cc1101` | 30-pin ESP32 DevKit (WROOM-32) + CC1101 module | CC1101, 2-FSK | **Primary target.** The board at `fordtpms.local` |
| `c3-lr1121` | ExpressLRS "Unified ESP32C3 LR1121" receiver | LR1121, GFSK | Fallback only. Keep it compiling, but don't hold back CC1101 features for it |

`default_envs` is `c3-lr1121` for historical reasons, so always pass `-e`.

## Layout

```
firmware/            PlatformIO + ESP-IDF 6.1 project (espressif32@7.1.3, no Arduino)
  src/               all firmware sources (see "Architecture")
  src/web/           index.html (dashboard) and oled.html (live OLED view), embedded in the image
  test/host/         host-side C tests and the OLED screen renderer
  test/nomad_lr1121/ stand-alone LR1121 bring-up project
  tools/             PlatformIO upload hooks (c3_upload.py, ota_upload.py)
docs/                UI.md (OLED screens), CLI.md (serial commands), images/
tools/               ota-push.sh (test, build, OTA flash, verify) and tpms-watch.sh (reception scorer)
backup/              original flash images (not in git)
```

## Build, test, flash

All PlatformIO commands run from `firmware/`.

```bash
# Host tests: decoder against real rtl_433 captures (ASan + UBSan). Run before every flash.
make -C firmware/test/host

# OLED screen previews / regenerate docs/images/*.png (needs ImageMagick)
make -C firmware/test/host preview
make -C firmware/test/host screenshots

# Build
cd firmware && pio run -e esp32-cc1101
cd firmware && pio run -e c3-lr1121          # check the fallback still builds after shared changes

# Flash over USB (pass the port: ttyUSB0/ttyUSB1/ttyAMA0 change with what's plugged in)
pio run -e esp32-cc1101 -t upload --upload-port /dev/ttyUSB0
pio run -e c3-lr1121    -t upload --upload-port /dev/ttyAMA0   # hands-free: sends the CLI `bootloader` cmd first

# Flash over Wi-Fi
pio run -e esp32-cc1101-ota -t upload                          # to fordtpms.local:80
pio run -e c3-lr1121-ota -t upload --upload-port <ip>:80       # keep the :80, or PlatformIO switches to espota

# All-in-one for the CC1101 board: host tests, build, OTA, then wait until
# /api/state reports the new ELF hash with probation over (rollback otherwise)
tools/ota-push.sh http://fordtpms.local
tools/ota-push.sh --check                                      # tests + build only, no flash

# Serial console (115200, prompt "tpms>")
pio device monitor -e esp32-cc1101
```

Notes:

- The wrong board's image is refused: esptool checks the chip over USB, and the
  OTA endpoint checks the chip ID in the image header. Rely on that instead of
  guessing which port is which board. If more than one port is present and it
  matters, ask.
- OTA images boot on probation and confirm themselves once Wi-Fi and the web
  server are up. If that doesn't happen within 300 s, or the board resets
  before it, the bootloader rolls back.
- After editing `sdkconfig.defaults*`, delete `.pio/build/<env>` and
  `sdkconfig.<env>` before building. ESP-IDF won't override values that a
  generated sdkconfig already holds, and a stale bootloader can silence serial
  output.
- `src/wifi_config.h` (Wi-Fi/MQTT credentials) is git-ignored. Start from
  `src/wifi_config.example.h`. Without it the build uses the example and warns.
- Web pages in `src/web/` are embedded at build time (`board_build.embed_txtfiles`
  and `EMBED_TXTFILES` in `src/CMakeLists.txt`). A UI change needs a rebuild
  and flash before the device serves it.

## Architecture

```
radio_cc1101.c / radio_lr1121.c   chip backends behind radio_hw.h (one per build target)
        |  IRQ: sync word (RSSI latched), end of packet
radio.c        receive task: reads FIFO, Manchester + checksum + flags via tpms_ford.c,
               stats, RSSI/noise history, RF event log, kept-failure log, packet log, AFC
        |  packet sink (main.c)
sensors.c      20-entry unique-ID table; folds the ~4 repeats of a burst into one entry
tires.c        sensor ID -> wheel position (FL/FR/RL/RR), stored in NVS
   |---> display.c + ui.c + gfx.c   SSD1306 screens (ui/gfx are portable, so host preview builds)
   |---> web.c                      HTTP + WebSocket push, /api/*, OTA (ota.c)
   |---> ha_mqtt.c                  MQTT state once per burst + HA discovery (assigned wheels only)
   '---> cli.c                      esp_console commands (docs/CLI.md)
net.c          Wi-Fi station, hostname/mDNS, SNTP, TX power and power save
settings.c     NVS: radio, display, Wi-Fi tuning, tires
board.h        pin maps per target
```

Key design points:

- **Packet mode, interrupt-only reception.** The radio does bit sync, matches
  the sync word `0x5556` (the end of the preamble plus the start delimiter),
  and collects a fixed 16-byte payload (128 Manchester chips -> 8 bytes
  `ID ID ID ID PP TT FF CS`). On the CC1101, **any SPI traffic while a packet
  is arriving damages it**, so the radio task reads only GDO pin levels until
  the packet ends. Don't add register polling to the receive path.
- **Portable cores.** `tpms_ford.c`, `ui.c` and `gfx.c` have no ESP-IDF
  dependencies. They are compiled on the host by `test/host/`. Keep them
  portable.
- **Web UI.** One self-contained page (`index.html`, no external assets,
  served with an ETag). It loads `/api/state` and then gets status pushes over
  `/ws`. A client that sends `oled` gets display frames instead (`/oled`).
  Other endpoints: `/api/rflog`, `/api/pkts?id=`, and POSTs to `/api/radio`,
  `/api/display`, `/api/tire` and `/update` (OTA). The style is a deliberate
  late-80s car dashboard: 7-segment readouts, red/amber/green, and modal
  dialogs that use Engage/Abort buttons.
- **ESP32 web server limits.** The HTTP server has only a few sockets. Several
  pages open at once (for example several headless browser tabs) can get
  `ERR_CONNECTION_RESET`. Open one page at a time.
- **Settings vs. RAM.** Radio, display, Wi-Fi tuning and tire assignments live
  in NVS and survive reboots. The sensor table, packet history, RF logs and the
  AFC estimate are RAM only.

## Working with the hardware

- **Ford sensors at rest transmit only about every 6 hours.** Don't wait for
  real packets. Keep listen tests under a minute. Exercise the receive path with
  the host tests, the `inject` CLI command and short `metrics` snapshots (the
  sync count climbs slowly from noise when the radio is alive).
- **Opening the serial port can reset the board** through DTR/RTS. That clears
  the RAM sensor table and drops WebSocket clients. With pyserial, set
  `dtr = rts = False` before `open()`. Don't open the port during a network
  measurement, and allow about 7 s for Wi-Fi to come back.
- **Hostnames:** the CC1101 board is `fordtpms.local` and the LR1121 board is
  `fordtpms-lr.local`. If two devices claim the same name, mDNS silently renames
  the later one (`fordtpms-2.local`). There's no reboot API. Fix it with an OTA
  push addressed by IP.
- **Reference receiver:** an rtl_433 instance at `http://rotorhaz.lan:8433/`
  (315 MHz, Ford decoder only). It's **read-only**: don't change its settings.
  `ws://rotorhaz.lan:8433/ws` replays its last 100 events (time in UTC; the
  firmware uses local time).
- **Reception philosophy:** improve RF/demodulator settings and diagnostics.
  **Don't add software Manchester error correction.** Noise produces thousands
  of near-valid payloads, so correcting them would invent packets.
- `pll` in `/api/state` must be `true`. If it isn't, the radio can't tune that
  frequency and no other reception counter means anything.
- `tools/tpms-watch.sh` scores each sensor round against rtl_433 over MQTT. It
  can run Claude headless to tune settings or push firmware. Its runs go to
  `tools/watch-runs/` (git-ignored).

## Repo conventions

- **Git:** don't `git add` or commit unless the user asks in the current
  conversation. When asked, commit straight to `main` (no feature branches).
- **Commit messages:** subject `Area: what changed` (for example `Radio: …`,
  `Wi-Fi: …`, `Web UI: …`, `CC1101: …`), imperative or noun phrase, no trailing
  period. The body explains what changed and why. Don't put test results in
  commit messages.
- **Docs move with code.** User-visible changes update `firmware/README.md`,
  `docs/CLI.md` (CLI commands) or `docs/UI.md` (OLED screens) in the same
  change. Regenerate `docs/images/*.png` with `make screenshots` when screens
  change.
- **C style:** plain C (the host tests build with `-std=gnu11`), 4-space indent, `snake_case`, module prefix on
  public functions (`radio_…`, `sensors_…`, `web_…`). Comments explain *why*,
  with datasheet section or errata references where relevant. Keep host-tested
  modules free of ESP-IDF headers.
- **Web UI:** plain HTML/CSS/vanilla JS in a single file. No build step,
  frameworks or CDN assets (the device may have no internet). Keep the page
  small. It's served from flash over a marginal Wi-Fi link (536-byte TCP
  segments).
- **Generated files:** `sdkconfig.<env>`, `.pio/`, `managed_components/`,
  `dependencies.lock` and the host test binaries are git-ignored. Don't commit
  them.
- **Scratch output:** overwrite generated files in place. Don't `rm` them
  first, because each delete needs the user's approval.
