# PlatformIO pre-upload hook for boards without a DTR/RTS auto-reset circuit
# (env:c3-lr1121). Asks the running firmware to reboot into the ROM download
# mode with the CLI `bootloader` command; esptool then connects with
# --before no_reset. Harmless if the chip is already in download mode.
import time

Import("env")  # noqa: F821 (provided by SCons)


def enter_rom_loader(source, target, env):
    import serial

    port = env.subst("$UPLOAD_PORT")
    s = serial.Serial()
    s.port = port
    s.baudrate = int(env.GetProjectOption("monitor_speed", 115200))
    s.dtr = False
    s.rts = False
    s.timeout = 0.2
    s.open()
    try:
        # Ctrl-C/newline first to clear any half-typed line at the prompt.
        s.write(b"\x03\r\nbootloader\r\n")
        s.flush()
        time.sleep(0.6)
        reply = s.read(4096)
    finally:
        s.close()
    if b"download mode" in reply:
        print("c3_upload: firmware rebooted into the ROM download mode")
    else:
        print("c3_upload: no reply to 'bootloader' (already in download mode?), trying anyway")


env.AddPreAction("upload", enter_rom_loader)  # noqa: F821
