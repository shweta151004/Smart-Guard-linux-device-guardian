#!/usr/bin/env bash

set -Eeuo pipefail

PROJECT_DIR="${SMART_GUARD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
OUTPUT_DIR="$PROJECT_DIR/docs/tests/section4/test_4-3_watchdog_camera"

VIDEO_DEVICE="/dev/video0"
APP_SERVICE="smart-guard.service"
WATCHDOG_SERVICE="smart-guard-watchdog.service"
MQTT_SERVICE="smart-guard-mqtt.service"
API_SERVICE="smart-guard-api.service"
BROKER_SERVICE="mosquitto.service"

HEARTBEAT_FILE="/run/smart-guard/frame_heartbeat"
OUTAGE_SECONDS=60
RECOVERY_TIMEOUT=90

TERMINAL_FILE="$OUTPUT_DIR/test_4-3_terminal.txt"
WATCHDOG_LOG_FILE="$OUTPUT_DIR/watchdog_journal.txt"
APP_LOG_FILE="$OUTPUT_DIR/smart_guard_journal.txt"
FINAL_FILE="$OUTPUT_DIR/watchdog_camera_final_evidence.txt"

UNBIND_FILE="/sys/bus/usb/drivers/uvcvideo/unbind"
BIND_FILE="/sys/bus/usb/drivers/uvcvideo/bind"

CAMERA_INTERFACE=""
CAMERA_UNBOUND=0

cd "$PROJECT_DIR"

mkdir -p "$OUTPUT_DIR"

exec > >(tee "$TERMINAL_FILE") 2>&1

restore_system()
{
    set +e

    if [ "${CAMERA_UNBOUND:-0}" -eq 1 ] &&
       [ -n "${CAMERA_INTERFACE:-}" ]
    then
        echo
        echo "Restoring camera interface..."

        printf '%s\n' "$CAMERA_INTERFACE" |
            sudo tee "$BIND_FILE" \
            >/dev/null 2>&1

        CAMERA_UNBOUND=0

        sleep 5
    fi

    sudo systemctl start \
        "$BROKER_SERVICE" \
        "$APP_SERVICE" \
        "$WATCHDOG_SERVICE" \
        "$MQTT_SERVICE" \
        "$API_SERVICE" \
        >/dev/null 2>&1 \
        || true
}

trap restore_system EXIT INT TERM

fail()
{
    echo
    echo "FAIL: $1"
    exit 1
}

service_state()
{
    systemctl is-active "$1" 2>/dev/null || true
}

main_pid()
{
    systemctl show \
        -p MainPID \
        --value \
        "$APP_SERVICE" \
        2>/dev/null \
        || true
}

restart_count()
{
    systemctl show \
        -p NRestarts \
        --value \
        "$APP_SERVICE" \
        2>/dev/null \
        || true
}

http_code()
{
    local url="$1"
    local result

    result="$(
        curl \
            -k \
            -sS \
            --connect-timeout 2 \
            --max-time 5 \
            -o /dev/null \
            -w '%{http_code}' \
            "$url" \
            2>/dev/null \
            || true
    )"

    if [ -z "$result" ]
    then
        result="000"
    fi

    printf '%s' "$result"
}

heartbeat_age()
{
    if [ ! -e "$HEARTBEAT_FILE" ]
    then
        echo 999999
        return
    fi

    local now
    local modified

    now="$(date +%s)"
    modified="$(stat -c '%Y' "$HEARTBEAT_FILE")"

    echo $((now - modified))
}

sudo -v

echo "========== TEST 4-3: SOFTWARE WATCHDOG CAMERA FAILURE =========="
date --iso-8601=seconds

echo
echo "========== PREPARING SERVICES =========="

sudo systemctl start \
    "$BROKER_SERVICE" \
    "$APP_SERVICE" \
    "$WATCHDOG_SERVICE" \
    "$MQTT_SERVICE" \
    "$API_SERVICE"

sleep 8

for service_name in \
    "$BROKER_SERVICE" \
    "$APP_SERVICE" \
    "$WATCHDOG_SERVICE" \
    "$MQTT_SERVICE" \
    "$API_SERVICE"
do
    state="$(service_state "$service_name")"

    printf '%-38s %s\n' \
        "$service_name" \
        "$state"

    if [ "$state" != "active" ]
    then
        fail "$service_name is not active."
    fi
done

if [ ! -c "$VIDEO_DEVICE" ]
then
    fail "$VIDEO_DEVICE is not available."
fi

CAMERA_SYS_PATH="$(
    readlink -f \
        "/sys/class/video4linux/$(basename "$VIDEO_DEVICE")/device"
)"

if [ -z "$CAMERA_SYS_PATH" ] ||
   [ ! -d "$CAMERA_SYS_PATH" ]
then
    fail "Camera sysfs path could not be determined."
fi

CAMERA_INTERFACE="$(basename "$CAMERA_SYS_PATH")"

CAMERA_DRIVER="$(
    basename "$(
        readlink -f \
            "$CAMERA_SYS_PATH/driver"
    )"
)"

if [ "$CAMERA_DRIVER" != "uvcvideo" ]
then
    fail "Camera driver is $CAMERA_DRIVER instead of uvcvideo."
fi

if [ ! -w "$UNBIND_FILE" ] &&
   ! sudo test -w "$UNBIND_FILE"
then
    fail "uvcvideo unbind interface is unavailable."
fi

INITIAL_PID="$(main_pid)"
INITIAL_RESTARTS="$(restart_count)"
INITIAL_HEARTBEAT_AGE="$(heartbeat_age)"
TEST_START="$(
    date '+%Y-%m-%d %H:%M:%S'
)"

echo
echo "========== CAMERA INFORMATION =========="
echo "Video device: $VIDEO_DEVICE"
echo "Camera interface: $CAMERA_INTERFACE"
echo "Camera driver: $CAMERA_DRIVER"
echo "Camera sysfs path: $CAMERA_SYS_PATH"

echo
echo "========== STATE BEFORE CAMERA DISCONNECTION =========="
echo "Smart Guard PID: $INITIAL_PID"
echo "Smart Guard restart count: $INITIAL_RESTARTS"
echo "Heartbeat age: $INITIAL_HEARTBEAT_AGE seconds"
echo "Telemetry HTTPS status: $(http_code 'https://127.0.0.1:8443/api/v1/telemetry')"
echo "Video device available: yes"

echo
echo "Open the Smart Guard dashboard."
echo "Verify that the live camera stream is visible."
echo "Start screen recording with the dashboard and terminal visible."

read -r -p "Press Enter after recording has started..."

echo
echo "========== DISCONNECTING CAMERA =========="

printf '%s\n' "$CAMERA_INTERFACE" |
    sudo tee "$UNBIND_FILE" \
    >/dev/null

CAMERA_UNBOUND=1

OUTAGE_START_EPOCH="$(date +%s)"

sleep 3

CAMERA_REMOVED=0

if [ ! -c "$VIDEO_DEVICE" ]
then
    CAMERA_REMOVED=1
fi

echo "Camera interface was disconnected."
echo "Video device available: $([ -c "$VIDEO_DEVICE" ] && echo yes || echo no)"
echo "Smart Guard state: $(service_state "$APP_SERVICE")"
echo "Watchdog state: $(service_state "$WATCHDOG_SERVICE")"
echo "Telemetry HTTPS status: $(http_code 'https://127.0.0.1:8443/api/v1/telemetry')"
echo "Heartbeat age: $(heartbeat_age) seconds"

echo
echo "The camera will remain disconnected for $OUTAGE_SECONDS seconds."

while true
do
    current_epoch="$(date +%s)"
    elapsed_seconds=$((current_epoch - OUTAGE_START_EPOCH))

    if [ "$elapsed_seconds" -ge "$OUTAGE_SECONDS" ]
    then
        break
    fi

    remaining_seconds=$((OUTAGE_SECONDS - elapsed_seconds))

    echo "Camera outage remaining: $remaining_seconds seconds"

    sleep 5
done

DURING_PID="$(main_pid)"
DURING_RESTARTS="$(restart_count)"
DURING_HEARTBEAT_AGE="$(heartbeat_age)"

echo
echo "========== STATE DURING CAMERA FAILURE =========="
echo "Video device available: $([ -c "$VIDEO_DEVICE" ] && echo yes || echo no)"
echo "Smart Guard state: $(service_state "$APP_SERVICE")"
echo "Watchdog state: $(service_state "$WATCHDOG_SERVICE")"
echo "Smart Guard PID: $DURING_PID"
echo "Smart Guard restart count: $DURING_RESTARTS"
echo "Heartbeat age: $DURING_HEARTBEAT_AGE seconds"
echo "Telemetry HTTPS status: $(http_code 'https://127.0.0.1:8443/api/v1/telemetry')"

echo
echo "========== RECONNECTING CAMERA =========="

printf '%s\n' "$CAMERA_INTERFACE" |
    sudo tee "$BIND_FILE" \
    >/dev/null

CAMERA_UNBOUND=0

udevadm settle

RECOVERED=0
RECOVERY_SECONDS=0

for ((second = 1; second <= RECOVERY_TIMEOUT; second++))
do
    RECOVERY_SECONDS="$second"

    current_state="$(service_state "$APP_SERVICE")"
    current_http="$(http_code 'https://127.0.0.1:8443/api/v1/telemetry')"
    current_heartbeat_age="$(heartbeat_age)"

    if [ -c "$VIDEO_DEVICE" ] &&
       [ "$current_state" = "active" ] &&
       [ "$current_http" = "200" ] &&
       [ "$current_heartbeat_age" -le 10 ]
    then
        RECOVERED=1
        break
    fi

    sleep 1
done

sleep 5

FINAL_PID="$(main_pid)"
FINAL_RESTARTS="$(restart_count)"
FINAL_HEARTBEAT_AGE="$(heartbeat_age)"
FINAL_APP_STATE="$(service_state "$APP_SERVICE")"
FINAL_WATCHDOG_STATE="$(service_state "$WATCHDOG_SERVICE")"
FINAL_HTTP_STATUS="$(http_code 'https://127.0.0.1:8443/api/v1/telemetry')"

sudo journalctl \
    -u "$WATCHDOG_SERVICE" \
    --since "$TEST_START" \
    --no-pager \
    > "$WATCHDOG_LOG_FILE" \
    2>&1 \
    || true

sudo journalctl \
    -u "$APP_SERVICE" \
    --since "$TEST_START" \
    --no-pager \
    > "$APP_LOG_FILE" \
    2>&1 \
    || true

WATCHDOG_ACTION_FOUND=0

if grep -Eqi \
    'heartbeat|stale|restart|restarting|camera|frame|recover' \
    "$WATCHDOG_LOG_FILE"
then
    WATCHDOG_ACTION_FOUND=1
fi

PID_CHANGED=0

if [ -n "$INITIAL_PID" ] &&
   [ -n "$FINAL_PID" ] &&
   [ "$INITIAL_PID" != "$FINAL_PID" ]
then
    PID_CHANGED=1
fi

echo
echo "========== WATCHDOG LOGS =========="

tail -n 80 "$WATCHDOG_LOG_FILE" || true

echo
echo "========== SMART GUARD LOGS =========="

tail -n 80 "$APP_LOG_FILE" || true

{
    echo "========== TEST 4-3 FINAL EVIDENCE =========="

    echo
    echo "Camera interface: $CAMERA_INTERFACE"
    echo "Camera driver: $CAMERA_DRIVER"
    echo "Camera outage duration: $OUTAGE_SECONDS seconds"
    echo "Recovery time: $RECOVERY_SECONDS seconds"

    echo
    echo "========== BEFORE FAILURE =========="
    echo "Initial Smart Guard PID: $INITIAL_PID"
    echo "Initial restart count: $INITIAL_RESTARTS"
    echo "Initial heartbeat age: $INITIAL_HEARTBEAT_AGE seconds"

    echo
    echo "========== DURING FAILURE =========="
    echo "Camera removed: $CAMERA_REMOVED"
    echo "Smart Guard PID during failure: $DURING_PID"
    echo "Restart count during failure: $DURING_RESTARTS"
    echo "Heartbeat age during failure: $DURING_HEARTBEAT_AGE seconds"

    echo
    echo "========== AFTER RECOVERY =========="
    echo "Video device available: $([ -c "$VIDEO_DEVICE" ] && echo yes || echo no)"
    echo "Final Smart Guard state: $FINAL_APP_STATE"
    echo "Final Watchdog state: $FINAL_WATCHDOG_STATE"
    echo "Final Smart Guard PID: $FINAL_PID"
    echo "Final restart count: $FINAL_RESTARTS"
    echo "Final heartbeat age: $FINAL_HEARTBEAT_AGE seconds"
    echo "Final telemetry HTTPS status: $FINAL_HTTP_STATUS"

    echo
    echo "========== TEST RESULT =========="

    if [ "$CAMERA_REMOVED" -eq 1 ]
    then
        echo "PASS: The camera device disappeared during the test."
    else
        echo "FAIL: The camera device did not disappear."
    fi

    if [ "$FINAL_WATCHDOG_STATE" = "active" ]
    then
        echo "PASS: The software watchdog remained active."
    else
        echo "FAIL: The software watchdog is not active."
    fi

    if [ "$WATCHDOG_ACTION_FOUND" -eq 1 ] ||
       [ "$PID_CHANGED" -eq 1 ]
    then
        echo "PASS: Watchdog recovery activity was observed."
    else
        echo "FAIL: Watchdog recovery activity was not confirmed."
    fi

    if [ "$RECOVERED" -eq 1 ]
    then
        echo "PASS: The camera and Smart Guard recovered automatically."
    else
        echo "FAIL: Automatic recovery was not completed."
    fi

    if [ "$FINAL_APP_STATE" = "active" ] &&
       [ "$FINAL_HTTP_STATUS" = "200" ] &&
       [ -c "$VIDEO_DEVICE" ]
    then
        echo "PASS: Smart Guard is operational after recovery."
    else
        echo "FAIL: Smart Guard is not fully operational."
    fi

} | tee "$FINAL_FILE"

echo
echo "Refresh the dashboard only if the stream has not resumed automatically."

read -r -p "Press Enter after the live stream is visible again..."

if [ "$CAMERA_REMOVED" -ne 1 ] ||
   [ "$FINAL_WATCHDOG_STATE" != "active" ] ||
   [ "$RECOVERED" -ne 1 ] ||
   [ "$FINAL_APP_STATE" != "active" ] ||
   [ "$FINAL_HTTP_STATUS" != "200" ]
then
    exit 1
fi

echo
echo "PASS: Test 4-3 completed successfully."
echo "Final evidence: $FINAL_FILE"
echo "Watchdog log: $WATCHDOG_LOG_FILE"
echo "Smart Guard log: $APP_LOG_FILE"
