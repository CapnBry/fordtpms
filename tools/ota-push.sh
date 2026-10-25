#!/usr/bin/env bash
# Test, build and flash the ESP32 + CC1101 firmware over Wi-Fi, then confirm
# the board is running the new build and has confirmed it (no rollback).
#
#   tools/ota-push.sh http://fordtpms.local [run-dir]
#   tools/ota-push.sh --check                    # host tests and build only
#
# With a run dir, the uncommitted firmware diff being flashed is saved there
# as firmware.diff. Exit status 0 only when /api/state reports the new ELF
# hash with probation over.

set -eu

DEVICE=${1:?usage: ota-push.sh <device-url> [run-dir] | --check}
RUN=${2:-}
DEVICE=${DEVICE%/}
HOST=${DEVICE#http://}
ENV=esp32-cc1101-ota
REPO=$(cd "$(dirname "$0")/.." && pwd)
FW="$REPO/firmware"
WAIT=330   # the board rolls back on its own 300 s after boot if never confirmed

running() {  # prints "<elf> <probation>" from the board, or nothing
    curl -s -m 5 "$DEVICE/api/state" | jq -r '.status.fw | "\(.elf) \(.probation)"' 2>/dev/null || true
}

echo "== host tests"
if ! out=$(make -s -C "$FW/test/host" 2>&1); then
    printf '%s\n' "$out" | tail -30
    echo "FAIL: host tests"
    exit 1
fi
printf '%s\n' "$out" | tail -1

echo "== build"
(cd "$FW" && pio run -s -e "$ENV")
[ "$DEVICE" = --check ] && { echo "OK: tests pass and it builds"; exit 0; }
want=$(sha256sum "$FW/.pio/build/$ENV/firmware.elf" | cut -c1-8)
read -r old _ <<<"$(running)"
[ -n "${old:-}" ] || { echo "FAIL: $DEVICE/api/state not reachable before the push"; exit 1; }
if [ "$old" = "$want" ]; then
    echo "OK: board already runs $want, nothing to flash"
    exit 0
fi
[ -n "$RUN" ] && git -C "$REPO" diff -- firmware > "$RUN/firmware.diff"

echo "== flash $want over Wi-Fi (board runs $old)"
(cd "$FW" && pio run -s -e "$ENV" -t upload --upload-port "$HOST:80")

echo "== waiting for the board to boot and confirm the new build"
start=$(date +%s)
sleep 5   # the board reboots 1 s after its reply; don't mistake the old build still answering for a rollback
while (( $(date +%s) - start < WAIT )); do
    sleep 3
    read -r elf probation <<<"$(running)"
    [ -n "${elf:-}" ] || continue
    if [ "$elf" = "$want" ] && [ "$probation" = false ]; then
        echo "OK: board runs $want, confirmed after $(( $(date +%s) - start )) s"
        exit 0
    fi
    if [ "$elf" = "$old" ]; then
        echo "FAIL: board is back on the old build $old: the new one was rolled back"
        exit 1
    fi
done
echo "FAIL: board not running a confirmed $want after $WAIT s (last seen: ${elf:-unreachable} probation=${probation:-?})"
exit 1
