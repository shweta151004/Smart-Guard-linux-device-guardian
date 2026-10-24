#!/usr/bin/env bash

set -Eeuo pipefail

PROJECT_DIR="${SMART_GUARD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
OUTPUT_DIR="$PROJECT_DIR/docs/tests/section3/test_3-4_lwt"
MESSAGE_FILE="$OUTPUT_DIR/lwt_messages.txt"
FINAL_FILE="$OUTPUT_DIR/lwt_offline_online.txt"

cd "$PROJECT_DIR"

mkdir -p "$OUTPUT_DIR"

sudo -v

sudo systemctl start mosquitto.service
sudo systemctl start smart-guard.service
sudo systemctl restart smart-guard-mqtt.service

sleep 8

if ! systemctl is-active --quiet mosquitto.service
then
    echo "ERROR: mosquitto.service is not active."
    exit 1
fi

if ! systemctl is-active --quiet smart-guard-mqtt.service
then
    echo "ERROR: smart-guard-mqtt.service is not active."
    exit 1
fi

set -a
source <(sudo cat /etc/smart-guard/mqtt.env)
set +a

MQTT_HOST_VALUE="${MQTT_HOST:-127.0.0.1}"
MQTT_PORT_VALUE="${MQTT_PORT:-8883}"
MQTT_STATUS_VALUE="${MQTT_STATUS_TOPIC:-status/device-001/home}"

MQTT_USER_VALUE="${MQTT_USERNAME:-}"
MQTT_PASSWORD_VALUE="${MQTT_PASSWORD:-}"

MQTT_CA_VALUE="${MQTT_CA_FILE:-${MQTT_TLS_CA_FILE:-${MQTT_CAFILE:-}}}"

if [ -z "$MQTT_CA_VALUE" ] &&
   [ -r /etc/mosquitto/conf.d/smart-guard.conf ]
then
    MQTT_CA_VALUE="$(
        sudo awk '
            $1 == "cafile" {
                print $2
                exit
            }
        ' /etc/mosquitto/conf.d/smart-guard.conf
    )"
fi

if [ -z "$MQTT_CA_VALUE" ] &&
   [ -r /etc/mosquitto/conf.d/smart-guard.conf ]
then
    MQTT_CA_VALUE="$(
        sudo awk '
            $1 == "certfile" {
                print $2
                exit
            }
        ' /etc/mosquitto/conf.d/smart-guard.conf
    )"
fi

SUBSCRIBER_COMMAND=(
    mosquitto_sub
    -h "$MQTT_HOST_VALUE"
    -p "$MQTT_PORT_VALUE"
    -t "$MQTT_STATUS_VALUE"
    -v
    -R
    -C 2
)

if [ -n "$MQTT_USER_VALUE" ]
then
    SUBSCRIBER_COMMAND+=(
        -u "$MQTT_USER_VALUE"
    )
fi

if [ -n "$MQTT_PASSWORD_VALUE" ]
then
    SUBSCRIBER_COMMAND+=(
        -P "$MQTT_PASSWORD_VALUE"
    )
fi

if [ -n "$MQTT_CA_VALUE" ] &&
   sudo test -r "$MQTT_CA_VALUE"
then
    SUBSCRIBER_COMMAND+=(
        --cafile "$MQTT_CA_VALUE"
        --insecure
    )
fi

rm -f "$MESSAGE_FILE" "$FINAL_FILE"

echo "========== TEST 3-4B: LWT MESSAGE =========="
echo "Status topic: $MQTT_STATUS_VALUE"
echo "Broker: $MQTT_HOST_VALUE:$MQTT_PORT_VALUE"
echo

echo "Starting MQTT subscriber..."

timeout 50 \
    stdbuf -oL \
    "${SUBSCRIBER_COMMAND[@]}" \
    > "$MESSAGE_FILE" 2>&1 &

SUBSCRIBER_PID=$!

sleep 4

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

if [ -z "$OLD_CLIENT_PID" ] ||
   [ "$OLD_CLIENT_PID" = "0" ]
then
    echo "ERROR: MQTT Client PID is invalid."
    kill "$SUBSCRIBER_PID" 2>/dev/null || true
    exit 1
fi

echo
echo "========== FORCING UNEXPECTED CLIENT FAILURE =========="

sudo systemctl kill \
    --kill-who=main \
    --signal=SIGKILL \
    smart-guard-mqtt.service

echo "SIGKILL was sent to MQTT Client PID $OLD_CLIENT_PID."

NEW_CLIENT_PID="0"

for attempt in $(seq 1 25)
do
    sleep 1

    NEW_CLIENT_PID="$(
        systemctl show \
            smart-guard-mqtt.service \
            -p MainPID \
            --value
    )"

    if systemctl is-active --quiet smart-guard-mqtt.service &&
       [ "$NEW_CLIENT_PID" != "0" ] &&
       [ "$NEW_CLIENT_PID" != "$OLD_CLIENT_PID" ]
    then
        echo "MQTT Client recovered after $attempt seconds."
        break
    fi
done

wait "$SUBSCRIBER_PID" || true

NEW_RESTARTS="$(
    systemctl show \
        smart-guard-mqtt.service \
        -p NRestarts \
        --value
)"

{
    echo "========== TEST 3-4B: LWT OFFLINE/ONLINE RESULT =========="
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

    if grep -qi 'offline' "$MESSAGE_FILE"
    then
        echo "PASS: LWT offline message was received."
    else
        echo "FAIL: LWT offline message was not received."
    fi

    if grep -qi 'online' "$MESSAGE_FILE"
    then
        echo "PASS: Online recovery message was received."
    else
        echo "FAIL: Online recovery message was not received."
    fi

    if [ "$NEW_CLIENT_PID" != "0" ] &&
       [ "$NEW_CLIENT_PID" != "$OLD_CLIENT_PID" ]
    then
        echo "PASS: MQTT Client restarted with a new PID."
    else
        echo "FAIL: MQTT Client did not restart with a new PID."
    fi

    if systemctl is-active --quiet mosquitto.service &&
       systemctl is-active --quiet smart-guard-mqtt.service
    then
        echo "PASS: Broker and MQTT Client are active."
    else
        echo "FAIL: Final MQTT service state is not healthy."
    fi

} | tee "$FINAL_FILE"

echo
echo "Final evidence:"
echo "$FINAL_FILE"
