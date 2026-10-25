#!/usr/bin/env python3
"""Upload a firmware image to the device's HTTP OTA endpoint.

Used as the `upload_command` of the `*-ota` PlatformIO environments:

    pio run -e esp32-cc1101-ota -t upload
    pio run -e c3-lr1121-ota -t upload --upload-port 192.168.1.50:80

Keep the `:80` on an IP address or .local name: without it the espressif32
builder switches the upload to espota, which this firmware does not answer.

The image is POSTed as a raw body with an X-MD5 header so the device can
verify it before swapping the boot partition.
"""

import hashlib
import http.client
import io
import os
import socket
import sys
import time

DEFAULT_HOST = "fordtpms.local"
DEFAULT_PORT = 80

# Reaching the device is either quick or hopeless, so fail fast when nothing
# answers. Once the socket is up the timeout is raised: a weak signal can stall
# a transfer for a long while without the upload being lost.
CONNECT_TIMEOUT_SECONDS = 5
TRANSFER_TIMEOUT_SECONDS = 300

# Minimum time between progress redraws, so a fast link does not spam the log
PROGRESS_INTERVAL_SECONDS = 0.2
# Percent step between progress lines when stdout is not a terminal
PROGRESS_STEP_PERCENT = 5


def _unbuffer_stdout():
    """Emit progress as it happens.

    PlatformIO runs this through a pipe, and Python block-buffers a non-tty
    stdout — without this the whole transfer log appears at once when the
    script exits, so a slow upload looks like a hang.
    """
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except (AttributeError, ValueError):
        pass  # Python < 3.7 or an stdout replacement that cannot be tuned


def _end_progress_line():
    """Close off a partially drawn progress bar before printing an error."""
    if sys.stdout.isatty():
        sys.stdout.write("\n")
        sys.stdout.flush()


class ProgressReader:
    """File-like wrapper that reports how much of the image has been sent.

    urllib hands a body with a `read` method straight to http.client, which
    pulls it in 8KB blocks — so each read() is a progress tick.
    """

    def __init__(self, data):
        self._buffer = io.BytesIO(data)
        self._total = len(data)
        self._sent = 0
        self._started = time.time()
        self._last_draw = 0.0
        self._last_percent = -PROGRESS_STEP_PERCENT
        self._tty = sys.stdout.isatty()

    def read(self, size=-1):
        chunk = self._buffer.read(size)
        self._sent += len(chunk)
        self._draw(final=not chunk)
        return chunk

    def elapsed(self):
        return time.time() - self._started

    def _draw(self, final=False):
        percent = (100.0 * self._sent / self._total) if self._total else 100.0
        now = time.time()

        if not final:
            if self._tty:
                if (now - self._last_draw) < PROGRESS_INTERVAL_SECONDS:
                    return
            elif (percent - self._last_percent) < PROGRESS_STEP_PERCENT:
                return

        self._last_draw = now
        self._last_percent = percent

        elapsed = now - self._started

        # The first chunk lands before the clock has anything useful to say
        if elapsed >= 0.5:
            rate = "%.0f KB/s" % (self._sent / elapsed / 1024.0)
        else:
            rate = "-- KB/s"

        if self._tty:
            filled = int(percent / 100.0 * 30)
            bar = "#" * filled + "-" * (30 - filled)
            sys.stdout.write("\r  [%s] %5.1f%%  %d/%d bytes  %s"
                             % (bar, percent, self._sent, self._total, rate))
            if final:
                sys.stdout.write("\n")
            sys.stdout.flush()
        else:
            print("  %5.1f%%  %d/%d bytes  %s"
                  % (percent, self._sent, self._total, rate), flush=True)


def split_host_port(host):
    """Split an `upload_port` value into (host, port), tolerating IPv6 forms."""
    if host.startswith("["):                       # [::1] or [::1]:8080
        addr, _, rest = host[1:].partition("]")
        if rest.startswith(":"):
            return addr, int(rest[1:])
        return addr, DEFAULT_PORT

    if host.count(":") == 1:                       # host:port
        addr, _, port = host.partition(":")
        return addr, int(port)

    return host, DEFAULT_PORT                      # bare name or IPv6 literal


def connect(host, port):
    """Open the socket under the short timeout, then relax it for the transfer.

    A device that is off, renumbered, or not yet on the network should fail in
    seconds rather than tying up the build for minutes.
    """
    conn = http.client.HTTPConnection(host, port, timeout=CONNECT_TIMEOUT_SECONDS)
    conn.connect()
    conn.sock.settimeout(TRANSFER_TIMEOUT_SECONDS)
    return conn


def upload(host, firmware_path):
    with open(firmware_path, "rb") as f:
        image = f.read()

    if not image:
        print("ERROR: firmware image is empty: %s" % firmware_path)
        return 1

    addr, port = split_host_port(host)
    digest = hashlib.md5(image).hexdigest()
    url = "http://%s:%d/update" % (addr, port)

    print("Uploading %s (%d bytes, md5 %s) to %s"
          % (os.path.basename(firmware_path), len(image), digest, url))

    try:
        conn = connect(addr, port)
    except socket.gaierror as e:
        print("ERROR: could not resolve '%s': %s" % (addr, e))
        print("       If you are relying on mDNS, try --upload-port <ip> instead.")
        return 1
    except (socket.timeout, TimeoutError):
        print("ERROR: no response from %s:%d within %ds — is the device powered "
              "on and on the network?" % (addr, port, CONNECT_TIMEOUT_SECONDS))
        return 1
    except OSError as e:
        print("ERROR: could not connect to %s:%d: %s" % (addr, port, e))
        return 1

    body = ProgressReader(image)

    try:
        conn.request("POST", "/update", body=body, headers={
            "Content-Type": "application/octet-stream",
            "Content-Length": str(len(image)),
            "X-MD5": digest,
        })

        # The device verifies the MD5 and finalizes flash before replying
        print("Sent in %.1fs, waiting for the device to verify and reboot..."
              % body.elapsed())

        response = conn.getresponse()
        reply = response.read().decode("utf-8", "replace").strip()

        if response.status == 200:
            print("Device replied %d: %s" % (response.status, reply))
            return 0

        print("ERROR: device rejected the update (HTTP %d): %s"
              % (response.status, reply))
    except (socket.timeout, TimeoutError):
        _end_progress_line()
        print("ERROR: transfer stalled for %ds — the device may have rebooted "
              "or the link dropped." % TRANSFER_TIMEOUT_SECONDS)
    except (OSError, http.client.HTTPException) as e:
        _end_progress_line()
        print("ERROR: upload failed: %s" % e)
    finally:
        conn.close()

    return 1


def main(argv):
    _unbuffer_stdout()

    if len(argv) < 2:
        print("usage: ota_upload.py [host] <firmware.bin>")
        return 2

    # PlatformIO substitutes an empty $UPLOAD_PORT when none is configured
    if len(argv) >= 3 and argv[1]:
        host, firmware_path = argv[1], argv[2]
    else:
        host, firmware_path = DEFAULT_HOST, argv[-1]

    if not os.path.isfile(firmware_path):
        print("ERROR: no such firmware image: %s" % firmware_path)
        return 1

    return upload(host, firmware_path)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
