# esp-fordtpms

* `backup/`: full 4 MB flash image of the TTGO as it arrived (rdzTTGOsonde),
  plus `restore.sh` to put it back. The script needs only bash and python3;
  esptool and pyserial are bundled. `backup/lr1121-c3/` holds the ESP32-C3 +
  LR1121 board's original ExpressLRS image.
* `firmware/`: Ford TPMS receiver firmware (PlatformIO + ESP-IDF) for the TTGO
  LoRa32 (SX1278) and an ESP32-C3 + LR1121 board. See `firmware/README.md`.
* `docs/`: [UI.md](docs/UI.md) describes each OLED screen, with screenshots in
  `docs/images`; [CLI.md](docs/CLI.md) is the serial command reference.
