#!/usr/bin/env bash

set -Eeuo pipefail

PROJECT_DIR="${SMART_GUARD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
OUTPUT_DIR="$PROJECT_DIR/docs/tests/section4/test_4-1_guard"
ENV_FILE="/etc/smart-guard/mqtt.env"
GUARD_URL="https://127.0.0.1:8443/api/v1/guard"
MESSAGE_FILE="$OUTPUT_DIR/guard_alarm_message.txt"
FINAL_FILE="$OUTPUT_DIR/guard_mode_final_evidence.txt"

SUBSCRIBER_PID=""

cleanup()
{
    if [ -n "${SUBSCRIBER_PID:-}" ]; then
        kill "$SUBSCRIBER_PID" 2>/dev/null || true
        wait "$SUBSCRIBER_PID" 2>/dev/null || true
    fi
}

trap cleanup EXIT INT TERM

cd "$PROJECT_DIR"

mkdir -p "$OUTPUT_DIR"

sudo -v

echo "========== TEST 4-1: GUARD MODE FUNCTION =========="
date --iso-8601=seconds

echo
echo "========== PREPARING SERVICES =========="

sudo systemctl start mosquitto.service
sudo systemctl start smart-guard.service
sudo systemctl start smart-guard-mqtt.service
sudo systemctl start smart-guard-api.service

sleep 8

for service_name in \
    mosquitto.service \
    smart-guard.service \
    smart-guard-mqtt.service \
    smart-guard-api.service
do
    service_state="$(
        systemctl is-active \
            "$service_name" \
            2>/dev/null \
            || true
    )"

    printf '%-38s %s\n' \
        "$service_name" \
        "$service_state"

    if [ "$service_state" != "active" ]; then
        echo "ERROR: $service_name is not active."
        exit 1
    fi
done

if ! sudo test -r "$ENV_FILE"; then
    echo "ERROR: MQTT environment file is unavailable."
    exit 1
fi

set -a
source <(
    sudo cat "$ENV_FILE"
)
set +a

MQTT_HOST_VALUE="${MQTT_HOST:-127.0.0.1}"
MQTT_PORT_VALUE="${MQTT_PORT:-1883}"
MQTT_USERNAME_VALUE="${MQTT_USERNAME:-}"
MQTT_PASSWORD_VALUE="${MQTT_PASSWORD:-}"
MQTT_ALARM_VALUE="${MQTT_ALARM_TOPIC:-alarm/device-001/home}"

SUBSCRIBER_COMMAND=(
    mosquitto_sub
    -h "$MQTT_HOST_VALUE"
    -p "$MQTT_PORT_VALUE"
    -t "$MQTT_ALARM_VALUE"
    -v
    -C 1
    -W 120
    -V mqttv311
)

if [ -n "$MQTT_USERNAME_VALUE" ]; then
    SUBSCRIBER_COMMAND+=(
        -u "$MQTT_USERNAME_VALUE"
    )
fi

if [ -n "$MQTT_PASSWORD_VALUE" ]; then
    SUBSCRIBER_COMMAND+=(
        -P "$MQTT_PASSWORD_VALUE"
    )
fi

INITIAL_GUARD_STATE="$(
    curl \
        -k \
        -sS \
        --max-time 5 \
        "$GUARD_URL"
)"

echo
echo "========== INITIAL GUARD STATE =========="
printf '%s\n' "$INITIAL_GUARD_STATE"

rm -f "$MESSAGE_FILE" "$FINAL_FILE"

echo
echo "========== STARTING MQTT ALARM SUBSCRIBER =========="
echo "Alarm topic: $MQTT_ALARM_VALUE"
echo "MQTT password output: hidden"

timeout 130 \
    stdbuf -oL \
    "${SUBSCRIBER_COMMAND[@]}" \
    > "$MESSAGE_FILE" 2>&1 &

SUBSCRIBER_PID=$!

sleep 3

if ! kill -0 "$SUBSCRIBER_PID" 2>/dev/null; then
    echo "ERROR: MQTT alarm subscriber could not remain connected."
    cat "$MESSAGE_FILE"
    exit 1
fi

echo "PASS: MQTT alarm subscriber is connected."

echo
echo "Open the Smart Guard dashboard."
echo "Keep every person outside the camera frame."
echo "Verify that Detected Persons is 0."
echo "Enable Guard Mode from the dashboard."

read -r -p "Press Enter after the dashboard shows ARMED..."

ARMED_GUARD_STATE="$(
    curl \
        -k \
        -sS \
        --max-time 5 \
        "$GUARD_URL"
)"

echo
echo "========== ARMED GUARD STATE =========="
printf '%s\n' "$ARMED_GUARD_STATE"

if printf '%s\n' "$ARMED_GUARD_STATE" |
    grep -Eq '"guard_enabled"[[:space:]]*:[[:space:]]*true'
then
    echo "PASS: Guard Mode is armed."
else
    echo "ERROR: Guard Mode is not armed."
    exit 1
fi

echo
echo "The system is now waiting for a new person."
echo "Enter the camera frame and remain visible for at least 5 seconds."

read -r -p "Press Enter immediately before entering the frame..."

echo
echo "========== WAITING FOR GUARD ALARM =========="

set +e
wait "$SUBSCRIBER_PID"
SUBSCRIBER_EXIT_CODE=$?
set -e

SUBSCRIBER_PID=""

ALARM_RECEIVED=0
EVENT_CONFIRMED=0
STUDENT_CONFIRMED=0
GUARD_CONFIRMED=0
PERSON_CONFIRMED=0

if [ -s "$MESSAGE_FILE" ]; then
    ALARM_RECEIVED=1
fi

if grep -Eq \
    '"event"[[:space:]]*:[[:space:]]*"guard_human_detected"' \
    "$MESSAGE_FILE"
then
    EVENT_CONFIRMED=1
fi

if grep -Eq \
    '"student_id"[[:space:]]*:[[:space:]]*"device-001"' \
    "$MESSAGE_FILE"
then
    STUDENT_CONFIRMED=1
fi

if grep -Eq \
    '"guard_enabled"[[:space:]]*:[[:space:]]*true' \
    "$MESSAGE_FILE"
then
    GUARD_CONFIRMED=1
fi

if grep -Eq \
    '"persons"[[:space:]]*:[[:space:]]*[1-9][0-9]*' \
    "$MESSAGE_FILE"
then
    PERSON_CONFIRMED=1
fi

CURRENT_GUARD_STATE="$(
    curl \
        -k \
        -sS \
        --max-time 5 \
        "$GUARD_URL"
)"

{
    echo "========== TEST 4-1 FINAL EVIDENCE =========="
    echo
    echo "Alarm topic: $MQTT_ALARM_VALUE"
    echo "Subscriber exit code: $SUBSCRIBER_EXIT_CODE"

    echo
    echo "========== ARMED GUARD API RESPONSE =========="
    printf '%s\n' "$ARMED_GUARD_STATE"

    echo
    echo "========== RECEIVED GUARD ALARM =========="
    cat "$MESSAGE_FILE"

    echo
    echo "========== CURRENT GUARD API RESPONSE =========="
    printf '%s\n' "$CURRENT_GUARD_STATE"

    echo
    echo "========== FINAL SERVICE STATES =========="

    for service_name in \
        mosquitto.service \
        smart-guard.service \
        smart-guard-mqtt.service \
        smart-guard-api.service
    do
        printf '%-38s %s\n' \
            "$service_name" \
            "$(
                systemctl is-active \
                    "$service_name" \
                    2>/dev/null \
                    || true
            )"
    done

    echo
    echo "========== TEST RESULT =========="

    if [ "$ALARM_RECEIVED" -eq 1 ]; then
        echo "PASS: A Guard Mode alarm message was received."
    else
        echo "FAIL: No Guard Mode alarm message was received."
    fi

    if [ "$EVENT_CONFIRMED" -eq 1 ]; then
        echo "PASS: Guard human-detection event was confirmed."
    else
        echo "FAIL: Guard event type was not confirmed."
    fi

    if [ "$STUDENT_CONFIRMED" -eq 1 ]; then
        echo "PASS: Student ID was confirmed in the alarm."
    else
        echo "FAIL: Student ID was not confirmed."
    fi

    if [ "$GUARD_CONFIRMED" -eq 1 ]; then
        echo "PASS: Alarm was generated while Guard Mode was enabled."
    else
        echo "FAIL: Enabled Guard Mode was not confirmed."
    fi

    if [ "$PERSON_CONFIRMED" -eq 1 ]; then
        echo "PASS: At least one detected person was confirmed."
    else
        echo "FAIL: Detected person count was not confirmed."
    fi

} | tee "$FINAL_FILE"

if [ "$ALARM_RECEIVED" -ne 1 ] ||
   [ "$EVENT_CONFIRMED" -ne 1 ] ||
   [ "$STUDENT_CONFIRMED" -ne 1 ] ||
   [ "$GUARD_CONFIRMED" -ne 1 ] ||
   [ "$PERSON_CONFIRMED" -ne 1 ]
then
    exit 1
fi

echo
echo "PASS: Test 4-1 completed successfully."
echo "Final evidence: $FINAL_FILE"
