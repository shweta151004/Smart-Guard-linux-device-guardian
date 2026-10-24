#!/usr/bin/env bash

set -Eeuo pipefail

PROJECT_DIR="${SMART_GUARD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
OUTPUT_DIR="$PROJECT_DIR/docs/tests/section5/test_5-1_latency"
ENV_FILE="/etc/smart-guard/mqtt.env"

cd "$PROJECT_DIR"

mkdir -p "$OUTPUT_DIR"

sudo -v

echo "========== TEST 5-1: END-TO-END MQTT LATENCY =========="
date --iso-8601=seconds

echo
echo "========== PREPARING SERVICES =========="

sudo systemctl start mosquitto.service
sudo systemctl start smart-guard.service
sudo systemctl start smart-guard-mqtt.service

sleep 8

for service_name in \
    mosquitto.service \
    smart-guard.service \
    smart-guard-mqtt.service
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

echo
echo "========== STARTING TEN MEASUREMENTS =========="

python3 -u \
    tests/section5/end_to_end_mqtt_latency.py \
    2>&1 \
    | tee \
    "$OUTPUT_DIR/test_5-1_terminal.txt"

echo
echo "PASS: Test 5-1 execution finished."
