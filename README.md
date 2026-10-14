# esp-fordtpms

* `backup/`: full 4 MB flash image of the board as it arrived (rdzTTGOsonde),
  plus `restore.sh` to put it back. The script needs only bash and python3;
  esptool and pyserial are bundled.
* `firmware/`: Ford TPMS receiver firmware (PlatformIO + ESP-IDF). See
  `firmware/README.md`.
* `docs/`: [UI.md](docs/UI.md) describes each OLED screen, with screenshots in
  `docs/images`; [CLI.md](docs/CLI.md) is the serial command reference.
