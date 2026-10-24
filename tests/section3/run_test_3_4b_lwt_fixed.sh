#!/usr/bin/env bash

set -Eeuo pipefail

PROJECT_DIR="${SMART_GUARD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
OUTPUT_DIR="$PROJECT_DIR/docs/tests/section3/test_3-4_lwt"
BROKER_CONFIG="/etc/mosquitto/conf.d/smart-guard.conf"
ENV_FILE="/etc/smart-guard/mqtt.env"

MESSAGE_FILE="$OUTPUT_DIR/lwt_messages_fixed.txt"
FINAL_FILE="$OUTPUT_DIR/lwt_offline_online_fixed.txt"
TEMP_CA="/tmp/smart_guard_test_3_4_ca.crt"

SUBSCRIBER_PID=""

cleanup()
{
    if [ -n "${SUBSCRIBER_PID:-}" ]; then
        kill "$SUBSCRIBER_PID" 2>/dev/null || true
        wait "$SUBSCRIBER_PID" 2>/dev/null || true
    fi

    rm -f "$TEMP_CA"
}

trap cleanup EXIT INT TERM

cd "$PROJECT_DIR"

mkdir -p "$OUTPUT_DIR"

sudo -v

sudo systemctl start mosquitto.service
sudo systemctl start smart-guard.service
sudo systemctl restart smart-guard-mqtt.service

sleep 8

if ! systemctl is-active --quiet mosquitto.service; then
    echo "ERROR: mosquitto.service is not active."
    exit 1
fi

if ! systemctl is-active --quiet smart-guard-mqtt.service; then
    echo "ERROR: smart-guard-mqtt.service is not active."
    exit 1
fi

set -a
source <(sudo cat "$ENV_FILE")
set +a

MQTT_HOST_VALUE="${MQTT_HOST:-127.0.0.1}"
MQTT_PORT_VALUE="${MQTT_PORT:-1883}"
MQTT_USERNAME_VALUE="${MQTT_USERNAME:-}"
MQTT_PASSWORD_VALUE="${MQTT_PASSWORD:-}"
MQTT_STATUS_VALUE="${MQTT_STATUS_TOPIC:-status/device-001/home}"

BROKER_PROTOCOL="$(
    sudo awk '
        $1 == "protocol" {
            print $2
            exit
        }
    ' "$BROKER_CONFIG" 2>/dev/null || true
)"

BROKER_CA="$(
    sudo awk '
        $1 == "cafile" {
            print $2
            exit
        }
    ' "$BROKER_CONFIG" 2>/dev/null || true
)"

BROKER_CERT="$(
    sudo awk '
        $1 == "certfile" {
            print $2
            exit
        }
    ' "$BROKER_CONFIG" 2>/dev/null || true
)"

if [ "${BROKER_PROTOCOL:-mqtt}" = "websockets" ]; then
    echo "ERROR: The configured listener uses WebSockets."
    exit 1
fi

SUBSCRIBER_COMMAND=(
    mosquitto_sub
    -h "$MQTT_HOST_VALUE"
    -p "$MQTT_PORT_VALUE"
    -t "$MQTT_STATUS_VALUE"
    -v
    -R
    -C 2
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

TLS_SOURCE=""

if [ -n "$BROKER_CA" ] &&
   sudo test -r "$BROKER_CA"; then
    TLS_SOURCE="$BROKER_CA"
elif [ -n "$BROKER_CERT" ] &&
     sudo test -r "$BROKER_CERT"; then
    TLS_SOURCE="$BROKER_CERT"
fi

if [ -n "$TLS_SOURCE" ]; then
    sudo install \
        -m 600 \
        -o "$USER" \
        -g "$USER" \
        "$TLS_SOURCE" \
        "$TEMP_CA"

    SUBSCRIBER_COMMAND+=(
        --cafile "$TEMP_CA"
        --insecure
    )
fi

rm -f "$MESSAGE_FILE" "$FINAL_FILE"

echo "========== TEST 3-4B: CORRECTED LWT TEST =========="
echo "Broker: $MQTT_HOST_VALUE:$MQTT_PORT_VALUE"
echo "Status topic: $MQTT_STATUS_VALUE"
echo "Broker protocol: ${BROKER_PROTOCOL:-mqtt}"

if [ -n "$TLS_SOURCE" ]; then
    echo "TLS mode: enabled"
else
    echo "TLS mode: not detected"
fi

echo
echo "========== STARTING AND VERIFYING SUBSCRIBER =========="

timeout 60 \
    stdbuf -oL \
    "${SUBSCRIBER_COMMAND[@]}" \
    > "$MESSAGE_FILE" 2>&1 &

SUBSCRIBER_PID=$!

sleep 4

if ! kill -0 "$SUBSCRIBER_PID" 2>/dev/null; then
    echo "ERROR: MQTT Subscriber could not remain connected."
    echo
    cat "$MESSAGE_FILE"
    exit 1
fi

echo "PASS: MQTT Subscriber is connected and waiting for LWT."

OLD_CLIENT_PID="$(
    systemctl show \
        smart-guard-mqtt.service \
        -p MainPID \
        --value
)"

OLD_RESTARTS="$(
    systemctl show \
        smart-guard-mqtt.service \
        -p NRestarts \
        --value
)"

echo "Old MQTT Client PID: $OLD_CLIENT_PID"
echo "Old automatic restart count: $OLD_RESTARTS"

echo
echo "========== FORCING UNEXPECTED MQTT CLIENT FAILURE =========="

sudo systemctl kill \
    --kill-who=main \
    --signal=SIGKILL \
    smart-guard-mqtt.service

echo "SIGKILL was sent to MQTT Client PID $OLD_CLIENT_PID."

NEW_CLIENT_PID="0"

for attempt in $(seq 1 30); do
    sleep 1

    NEW_CLIENT_PID="$(
        systemctl show \
            smart-guard-mqtt.service \
            -p MainPID \
            --value
    )"

    if systemctl is-active --quiet smart-guard-mqtt.service &&
       [ "$NEW_CLIENT_PID" != "0" ] &&
       [ "$NEW_CLIENT_PID" != "$OLD_CLIENT_PID" ]; then
        echo "MQTT Client recovered after $attempt seconds."
        break
    fi
done

wait "$SUBSCRIBER_PID" || true
SUBSCRIBER_PID=""

NEW_RESTARTS="$(
    systemctl show \
        smart-guard-mqtt.service \
        -p NRestarts \
        --value
)"

OFFLINE_RECEIVED=0
ONLINE_RECEIVED=0
PID_CHANGED=0
SERVICES_HEALTHY=0

grep -qi 'offline' "$MESSAGE_FILE" &&
    OFFLINE_RECEIVED=1

grep -qi 'online' "$MESSAGE_FILE" &&
    ONLINE_RECEIVED=1

if [ "$NEW_CLIENT_PID" != "0" ] &&
   [ "$NEW_CLIENT_PID" != "$OLD_CLIENT_PID" ]; then
    PID_CHANGED=1
fi

if systemctl is-active --quiet mosquitto.service &&
   systemctl is-active --quiet smart-guard-mqtt.service; then
    SERVICES_HEALTHY=1
fi

{
    echo "========== TEST 3-4B: FINAL LWT EVIDENCE =========="
    echo
    echo "Status topic: $MQTT_STATUS_VALUE"
    echo "Old MQTT Client PID: $OLD_CLIENT_PID"
    echo "New MQTT Client PID: $NEW_CLIENT_PID"
    echo "Old automatic restart count: $OLD_RESTARTS"
    echo "New automatic restart count: $NEW_RESTARTS"

    echo
    echo "========== RECEIVED MQTT STATUS MESSAGES =========="

    cat "$MESSAGE_FILE"

    echo
    echo "========== FINAL SERVICE STATES =========="

    printf '%-38s %s\n' \
        "mosquitto.service" \
        "$(systemctl is-active mosquitto.service || true)"

    printf '%-38s %s\n' \
        "smart-guard-mqtt.service" \
        "$(systemctl is-active smart-guard-mqtt.service || true)"

    echo
    echo "========== TEST RESULT =========="

    if [ "$OFFLINE_RECEIVED" -eq 1 ]; then
        echo "PASS: LWT offline message was received."
    else
        echo "FAIL: LWT offline message was not received."
    fi

    if [ "$ONLINE_RECEIVED" -eq 1 ]; then
        echo "PASS: Online recovery message was received."
    else
        echo "FAIL: Online recovery message was not received."
    fi

    if [ "$PID_CHANGED" -eq 1 ]; then
        echo "PASS: MQTT Client restarted with a new PID."
    else
        echo "FAIL: MQTT Client PID did not change."
    fi

    if [ "$SERVICES_HEALTHY" -eq 1 ]; then
        echo "PASS: Broker and MQTT Client are active."
    else
        echo "FAIL: Final MQTT service state is not healthy."
    fi

} | tee "$FINAL_FILE"

if [ "$OFFLINE_RECEIVED" -ne 1 ] ||
   [ "$ONLINE_RECEIVED" -ne 1 ] ||
   [ "$PID_CHANGED" -ne 1 ] ||
   [ "$SERVICES_HEALTHY" -ne 1 ]; then
    exit 1
fi

echo
echo "PASS: Test 3-4B completed successfully."
echo "Final evidence: $FINAL_FILE"
