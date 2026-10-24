#!/usr/bin/env bash

set -Eeuo pipefail

PROJECT_DIR="${SMART_GUARD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
OUTPUT_DIR="$PROJECT_DIR/docs/tests/section6/test_6-1_unauthorized"
ENV_FILE="/etc/smart-guard/mqtt.env"
FINAL_FILE="$OUTPUT_DIR/unauthorized_connection_test.txt"

cd "$PROJECT_DIR"

mkdir -p "$OUTPUT_DIR"

sudo -v

echo "========== TEST 6-1: UNAUTHORIZED MQTT ACCESS =========="

sudo systemctl start mosquitto.service

sleep 3

BROKER_STATE="$(
    systemctl is-active \
        mosquitto.service \
        2>/dev/null \
        || true
)"

echo "Mosquitto service state: $BROKER_STATE"

if [ "$BROKER_STATE" != "active" ]; then
    echo "ERROR: mosquitto.service is not active."
    exit 1
fi

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
MQTT_TOPIC_VALUE="${MQTT_TELEMETRY_TOPIC:-telemetry/device-001/home}"

INVALID_USERNAME="unauthorized_user_device-001"
INVALID_PASSWORD="invalid_password_device-001"

TEST_START="$(
    date '+%Y-%m-%d %H:%M:%S'
)"

echo "Broker: $MQTT_HOST_VALUE:$MQTT_PORT_VALUE"
echo "Test topic: $MQTT_TOPIC_VALUE"
echo "Real MQTT password: hidden"
echo

echo "========== ATTEMPT 1: ANONYMOUS CONNECTION =========="

set +e

ANONYMOUS_OUTPUT="$(
    timeout 8 \
        mosquitto_sub \
        -h "$MQTT_HOST_VALUE" \
        -p "$MQTT_PORT_VALUE" \
        -t "$MQTT_TOPIC_VALUE" \
        -C 1 \
        -W 5 \
        -V mqttv311 \
        -d \
        2>&1
)"

ANONYMOUS_EXIT_CODE=$?

set -e

printf '%s\n' "$ANONYMOUS_OUTPUT"
echo "Anonymous attempt exit code: $ANONYMOUS_EXIT_CODE"

ANONYMOUS_DENIED=0

if printf '%s\n' "$ANONYMOUS_OUTPUT" |
    grep -Eqi \
    'not authorised|not authorized|bad user name|authentication|denied|connection refused|connack.*5'
then
    ANONYMOUS_DENIED=1
fi

echo
echo "========== ATTEMPT 2: INVALID CREDENTIALS =========="

set +e

INVALID_OUTPUT="$(
    timeout 8 \
        mosquitto_sub \
        -h "$MQTT_HOST_VALUE" \
        -p "$MQTT_PORT_VALUE" \
        -u "$INVALID_USERNAME" \
        -P "$INVALID_PASSWORD" \
        -t "$MQTT_TOPIC_VALUE" \
        -C 1 \
        -W 5 \
        -V mqttv311 \
        -d \
        2>&1
)"

INVALID_EXIT_CODE=$?

set -e

printf '%s\n' "$INVALID_OUTPUT"
echo "Invalid-credential attempt exit code: $INVALID_EXIT_CODE"

INVALID_DENIED=0

if printf '%s\n' "$INVALID_OUTPUT" |
    grep -Eqi \
    'not authorised|not authorized|bad user name|authentication|denied|connection refused|connack.*5'
then
    INVALID_DENIED=1
fi

FINAL_BROKER_STATE="$(
    systemctl is-active \
        mosquitto.service \
        2>/dev/null \
        || true
)"

{
    echo "========== TEST 6-1 FINAL EVIDENCE =========="
    echo
    echo "Broker: $MQTT_HOST_VALUE:$MQTT_PORT_VALUE"
    echo "Test topic: $MQTT_TOPIC_VALUE"

    echo
    echo "========== ANONYMOUS CONNECTION OUTPUT =========="
    printf '%s\n' "$ANONYMOUS_OUTPUT"
    echo "Exit code: $ANONYMOUS_EXIT_CODE"

    echo
    echo "========== INVALID CREDENTIALS OUTPUT =========="
    printf '%s\n' "$INVALID_OUTPUT"
    echo "Exit code: $INVALID_EXIT_CODE"

    echo
    echo "========== FINAL SERVICE STATE =========="
    echo "mosquitto.service: $FINAL_BROKER_STATE"

    echo
    echo "========== TEST RESULT =========="

    if [ "$ANONYMOUS_DENIED" -eq 1 ]; then
        echo "PASS: Anonymous MQTT connection was rejected."
    else
        echo "FAIL: Anonymous rejection was not confirmed."
    fi

    if [ "$INVALID_DENIED" -eq 1 ]; then
        echo "PASS: Invalid MQTT credentials were rejected."
    else
        echo "FAIL: Invalid-credential rejection was not confirmed."
    fi

    if [ "$FINAL_BROKER_STATE" = "active" ]; then
        echo "PASS: MQTT Broker remained active during the test."
    else
        echo "FAIL: MQTT Broker is not active."
    fi

} | tee "$FINAL_FILE"

echo
echo "========== MOSQUITTO SECURITY LOGS =========="

sudo journalctl \
    -u mosquitto.service \
    --since "$TEST_START" \
    --no-pager \
    | grep -Ei \
    'new connection|not authorised|not authorized|bad user|denied|client|disconnect' \
    || true

echo
echo "Final evidence:"
echo "$FINAL_FILE"

if [ "$ANONYMOUS_DENIED" -ne 1 ] ||
   [ "$INVALID_DENIED" -ne 1 ] ||
   [ "$FINAL_BROKER_STATE" != "active" ]
then
    exit 1
fi

echo
echo "PASS: Test 6-1 completed successfully."
