#!/usr/bin/env bash
# Watch MQTT for Ford TPMS readings from rtl_433 and score the ESP32 + CC1101
# firmware on each round of transmissions: the sensors all send within a couple
# of minutes, about every 6 hours while parked. A round is perfect when every
# sensor rtl_433 heard was decoded from all 4 packets of its burst. After an
# imperfect round, Claude runs headless to diagnose it and (APPLY=1) prepare
# the next round: no change, radio settings changed live, or a firmware change
# flashed over Wi-Fi with tools/ota-push.sh, which checks that the board is
# running the new build and has confirmed it (rollback otherwise).
#
#   tools/tpms-watch.sh                          # may change settings and firmware
#   APPLY=0 tools/tpms-watch.sh                  # diagnose only, change nothing
#   FORCE=1 tools/tpms-watch.sh                  # run Claude after perfect rounds too
#
# Each round goes to tools/watch-runs/<timestamp>/: per sensor <id>.mqtt.json
# (the rtl_433 reading) and <id>.state.json / <id>.rflog.json / <id>.pkts.json
# (the firmware's /api/state, /api/rflog and /api/pkts?id=, SETTLE s later), summary.txt, and Claude's output
# (claude.md, claude.json, firmware.diff when it flashed). Claude keeps a
# running log of hypotheses and attempts in tools/watch-runs/NOTES.md.

set -u

BROKER=${BROKER:-streaky.lan}
TOPIC=${TOPIC:-zigbee/+/state}   # MQTT can't match zigbee/rtl_+/state; filtered below
TOPIC_RE=${TOPIC_RE:-^zigbee/rtl_[0-9a-f]+/state$}
DEVICE=${DEVICE:-http://fordtpms.local}
DEVICE=${DEVICE%/}
SETTLE=${SETTLE:-5}        # seconds after the MQTT message before snapshotting the firmware
QUIET=${QUIET:-180}        # a round ends after this many seconds without a transmission
APPLY=${APPLY:-1}          # 1: Claude may change radio settings and flash firmware
FORCE=${FORCE:-0}          # 1: run Claude after perfect rounds too
MODEL=${MODEL:-}           # optional --model for claude

REPO=$(cd "$(dirname "$0")/.." && pwd)
RUNS=${RUNS:-$REPO/tools/watch-runs}
NOTES="$RUNS/NOTES.md"
LOCK="$RUNS/.claude.lock"
mkdir -p "$RUNS"
[ -f "$NOTES" ] || printf '# TPMS reception attempts\n\n' > "$NOTES"
# Mark board changes in the notes, so later diagnoses can tell which board an entry is about.
if [ "$(cat "$RUNS/.board" 2>/dev/null || echo ttgo)" != cc1101 ]; then
    printf '\n## %s: switched to the cc1101 board (%s)\n\n' "$(date '+%F %T')" "$DEVICE" >> "$NOTES"
fi
echo cc1101 > "$RUNS/.board"

round_dir=""   # the open round, if any
round_last=0   # time of its latest transmission
declare -A seen

log() { printf '%s %s\n' "$(date '+%F %T')" "$*" | tee -a "$RUNS/watch.log" >&2; }

notify() {
    command -v notify-send >/dev/null && notify-send "fordtpms" "$1" 2>/dev/null
    printf '\a' >&2
}

# One line per sensor: "<id> ok <burst_pkts>/4 ..." when the firmware has a
# packet from it in the last 30 s, "<id> MISSED" otherwise.
sensor_line() {  # id state.json
    jq -r --arg id "$1" '
        (.sensors // {}) as $s
        | ([($s.list // [])[] | select(.id == $id and ($s.now - .last) < 30000)] | first) as $x
        | if $x == null then "\($id) MISSED"
          else "\($id) ok \($x.burst_pkts)/4 rssi \($x.rssi) foff \($x.foff) (mean \($x.foff_mean), \($x.foff_min)..\($x.foff_max)) lqi \($x.lqi) (mean \($x.lqi_mean))"
          end' "$2" 2>/dev/null || echo "$1 MISSED (firmware unreachable)"
}

on_message() {  # json
    local msg=$1 id
    id=$(jq -r 'select(.model=="Ford" and .type=="TPMS") | .id // empty' <<<"$msg" 2>/dev/null)
    [ -n "$id" ] || return
    round_last=$(date +%s)
    if [ -z "$round_dir" ]; then
        round_dir="$RUNS/$(date +%Y%m%d-%H%M%S)"
        mkdir -p "$round_dir"
        seen=()
        log "round started: $round_dir"
    fi
    if [ -n "${seen[$id]:-}" ]; then return; fi   # a repeat within the round
    seen[$id]=1

    printf '%s\n' "$msg" > "$round_dir/$id.mqtt.json"
    log "rtl_433 saw $id: $(jq -c '{pressure_PSI,temperature_C,moving,learn,rssi}' <<<"$msg")"
    sleep "$SETTLE"
    curl -s -m 10 "$DEVICE/api/state" > "$round_dir/$id.state.json" || echo '{}' > "$round_dir/$id.state.json"
    curl -s -m 10 "$DEVICE/api/rflog" > "$round_dir/$id.rflog.json" || echo '{}' > "$round_dir/$id.rflog.json"
    curl -s -m 10 "$DEVICE/api/pkts?id=$id" > "$round_dir/$id.pkts.json" || echo '{}' > "$round_dir/$id.pkts.json"
    local line
    line=$(sensor_line "$id" "$round_dir/$id.state.json")
    echo "$line" >> "$round_dir/sensors.txt"
    log "firmware: $line"
}

close_round() {
    local dir=$round_dir last perfect=1 n ok
    round_dir=""
    last=$(ls -t "$dir"/*.state.json | head -1)
    n=$(wc -l < "$dir/sensors.txt")
    ok=$(grep -c ' ok ' "$dir/sensors.txt")
    grep -qv ' ok 4/4 ' "$dir/sensors.txt" && perfect=0
    {
        echo "round $(basename "$dir"): firmware decoded $ok of $n sensors rtl_433 heard"
        cat "$dir/sensors.txt"
        jq -r '.status | "radio: freq \(.freq / 1e6) MHz, bw \(.bw / 1e3) kHz, drc \(.drc), sync \(.sync), bitrate \(.bitrate), noise \(.noise) dBm",
            "counters since boot (up \(.now / 1000 | floor) s): syncs \(.syncs), decoded \(.decoded), err_manchester \(.err_manchester), near_miss \(.near_miss), err_checksum \(.err_checksum), err_flags \(.err_flags), overruns \(.overruns)",
            "firmware: elf \(.fw.elf // "?"), ver \(.fw.ver // "?"), \(.fw.part // "?")"' "$last" 2>/dev/null \
            || echo "firmware unreachable at $DEVICE"
    } > "$dir/summary.txt"
    log "round done: $(head -1 "$dir/summary.txt")$( ((perfect)) && echo ', all 4/4')"
    printf -- '- %s: %s (%s)\n' "$(date '+%F %T')" "$(head -1 "$dir/summary.txt" | sed 's/^round [^:]*: //')" \
        "$(cut -d' ' -f1-3 "$dir/sensors.txt" | paste -sd, -)" >> "$NOTES"

    if (( perfect && ! FORCE )); then
        notify "Round perfect: $ok/$n sensors, all 4/4"
        return
    fi
    # One Claude run at a time (rounds are hours apart, so this only trips on a stuck run).
    if mkdir "$LOCK" 2>/dev/null; then
        ( run_claude "$dir"; rmdir "$LOCK" ) &
    else
        log "claude still running from an earlier round; $dir not analyzed"
    fi
}

run_claude() {  # dir
    local dir=$1 mode_text
    local tools=(Read Grep Glob "Bash(curl:*)" "Bash(jq:*)" "Bash(cat:*)" "Bash(ls:*)" "Bash(grep:*)"
                 "Bash(git diff:*)" "Bash(git log:*)" "Bash(git show:*)" "Bash(git status:*)")
    if [ "$APPLY" = 1 ]; then
        mode_text="Act on your diagnosis with at most one of these, so the next round tests a single
hypothesis (unless the evidence for more is strong):
- Nothing, if the evidence is too thin or the round was as good as this setup gets.
- Radio settings, live: curl -s -d 'freq=315.02&bw=67.7' $DEVICE/api/radio (form fields freq
  (MHz), bw (kHz, snapped to status.radio.bws), sync = 5556|aaa9|5555, bitrate, drc = 0-3, or
  defaults=1). They are saved on the board. Confirm with $DEVICE/api/state afterwards.
- A firmware change: edit firmware/, then flash it with exactly
    tools/ota-push.sh $DEVICE $dir
  It runs the host tests, builds, flashes over Wi-Fi and waits until the board runs the new
  build and has confirmed it; it prints OK or FAIL and saves the diff in $dir. It is the only
  way to flash: never run pio upload or open a serial port. 'tools/ota-push.sh --check' runs
  the tests and build without flashing. On FAIL, fix or revert your edit; if it says the build
  was rolled back, the board is on the previous build, so revert your edit unless you can find
  and fix why it failed. Wi-Fi and the web server are the only way to reach the board: a
  build that fails to boot or start them rolls back, but one that breaks them later strands
  the board until someone fetches it, so don't touch net.c, web.c, ota.c or the Wi-Fi setup.
The working tree may hold uncommitted firmware changes from earlier rounds (git diff); they are
what the board runs if status.fw.elf matches their build. Do not commit, and do not edit
anything outside firmware/ except NOTES.md."
        tools+=(Edit Write "Bash(sed:*)" "Bash(head:*)" "Bash(tail:*)" "Bash(python3:*)" "Bash(make:*)"
                "Bash(tools/ota-push.sh:*)")
    else
        mode_text="Diagnose only: do not change radio settings, edit files other than NOTES.md, or flash.
Describe the exact change you recommend for the next round."
        tools+=(Write)
    fi

    local prompt
    prompt=$(cat <<EOF
A round of Ford TPMS transmissions just ended. The reference receiver (rtl_433 on another
machine, publishing to MQTT) decoded the sensors listed below; this repo's firmware at $DEVICE
did not decode all of them from every packet. This is an unattended, automated check.

$(cat "$dir/summary.txt")

The board is an ESP32 DevKit with a CC1101 at 315 MHz (PlatformIO env esp32-cc1101), out in the
garage and reachable only over Wi-Fi at $DEVICE. firmware/README.md describes it; its
"Troubleshooting reception" section explains the diagnostics: per sensor burst_pkts (of 4 in
the latest burst), foff (Hz, the CC1101's frequency offset estimate at the sync word) latest /
mean / min / max, lqi (0-127, lower = cleaner); strong payload captures with foff, lqi, bad
and bad_at; drc (data rate offset tracking, 0 off .. 3 +/-12.5 %). Since 2026-11-01 the CC1101
aborts a payload at its first bad Manchester pair and goes straight back to sync search
(aborts / abort_dec in /api/state): near_miss and bad_bit_hist no longer grow. Instead the
kept failures (<id>.pkts.json "f" rows) show each likely sensor packet that failed, with its
ID when readable and the first bad pair; the "p" rows are every decoded packet of the sensor.

Evidence for this round is in $dir:
  <id>.mqtt.json   the rtl_433 reading (its "time" is local wall time)
  <id>.state.json  firmware /api/state ${SETTLE}s after that reading ("now" and sensor "last" are
                   ms since boot; status.time is wall time; status.fw identifies the build)
  <id>.rflog.json  firmware /api/rflog at the same moment
  <id>.pkts.json   firmware /api/pkts?id=<id>: this sensor's packet log and kept failures
                   (row layout above h_pkts in firmware/src/web.c)
  summary.txt      the scores above
Earlier rounds are sibling directories of $dir. Read $NOTES first: it is the running log of
what has been tried, and you must append to it.

Constraints:
- The sensors transmit only about every 6 hours while parked. Do not wait for packets or run
  listen tests. Your job is to prepare the best next round and stop.
- Use only the HTTP API to inspect the board.
- An empty state.json means the board was unreachable (Wi-Fi, power). A small status.now
  means it had rebooted. Either is a finding in itself.

$mode_text

Work out the most likely reason for the misses and lost packets, using the evidence above, the
firmware source, the README and the history in NOTES.md. Then append a section to $NOTES headed
with the round time: the evidence, your hypothesis, what you changed (exact settings, or a diff
summary plus the ota-push.sh result and new fw elf), and what the next round should show if the
hypothesis is right or wrong. End your reply with a short summary of the same.
EOF
)
    log "launching claude for $dir"
    local args=(-p "$prompt" --output-format json --permission-mode acceptEdits --allowedTools "${tools[@]}")
    [ -n "$MODEL" ] && args+=(--model "$MODEL")
    (cd "$REPO" && claude "${args[@]}") > "$dir/claude.json" 2> "$dir/claude.err"
    local sid result
    sid=$(jq -r '.session_id // empty' "$dir/claude.json" 2>/dev/null)
    result=$(jq -r '.result // empty' "$dir/claude.json" 2>/dev/null)
    printf '%s\n' "$result" > "$dir/claude.md"
    log "claude done for $dir (resume with: claude -r $sid)"
    notify "Round $(basename "$dir"). Claude: $(printf '%s' "$result" | tail -c 300)"
}

rmdir "$LOCK" 2>/dev/null
log "watching $TOPIC on $BROKER, checking the cc1101 board at $DEVICE (APPLY=$APPLY FORCE=$FORCE)"
while true; do
    # -R: skip retained messages so only live transmissions trigger a check.
    # -v prefixes each payload with its topic. The read timeout lets a quiet
    # period close the round.
    while true; do
        if IFS=' ' read -r -t 5 topic line; then
            [[ $topic =~ $TOPIC_RE ]] && on_message "$line"
        elif (( $? <= 128 )); then
            break   # mosquitto_sub exited
        fi
        if [ -n "$round_dir" ] && (( $(date +%s) - round_last >= QUIET )); then close_round; fi
    done < <(mosquitto_sub -h "$BROKER" -t "$TOPIC" -R -v)
    log "mosquitto_sub exited; reconnecting in 10 s"
    sleep 10
done
