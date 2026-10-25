# esp-fordtpms

* `backup/`: full 4 MB flash image of the TTGO as it arrived (rdzTTGOsonde),
  plus `restore.sh` to put it back. The script needs only bash and python3;
  esptool and pyserial are bundled. `backup/lr1121-c3/` holds the ESP32-C3 +
  LR1121 board's original ExpressLRS image.
* `firmware/`: Ford TPMS receiver firmware (PlatformIO + ESP-IDF) for an ESP32
  DevKit + CC1101 and an ESP32-C3 + LR1121 board. See `firmware/README.md`.
* `docs/`: [UI.md](docs/UI.md) describes each OLED screen, with screenshots in
  `docs/images`; [CLI.md](docs/CLI.md) is the serial command reference.
* `tools/tpms-watch.sh`: watches rtl_433's MQTT readings and scores the CC1101
  board on each round of transmissions. After an imperfect round it runs Claude
  headless to diagnose it and adjust radio settings or push a firmware change
  for the next round. `tools/ota-push.sh` tests, builds, flashes over Wi-Fi and
  checks that the board is running and has confirmed the new build.
