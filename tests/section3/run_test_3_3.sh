#!/usr/bin/env bash

set -Eeuo pipefail

PROJECT_DIR="${SMART_GUARD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

OUTPUT_DIR="$PROJECT_DIR/docs/tests/section3/test_3-3_resolution"

RESTORED=0

restore_services()
{
    if [ "$RESTORED" -eq 1 ]; then
        return
    fi

    RESTORED=1

    echo
    echo "========== RESTORING PROJECT SERVICES =========="

    sudo systemctl start mosquitto.service
    sudo systemctl start smart-guard.service

    sleep 8

    sudo systemctl start smart-guard-watchdog.service
    sudo systemctl start smart-guard-mqtt.service
    sudo systemctl start smart-guard-api.service

    echo
    echo "========== FINAL SERVICE STATES =========="

    for service_name in \
        smart-guard.service \
        smart-guard-watchdog.service \
        smart-guard-mqtt.service \
        smart-guard-api.service \
        mosquitto.service
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
    dashboard_code="$(curl -k -sS --max-time 10 -o /dev/null -w '%{http_code}' https://127.0.0.1:8443/ 2>/dev/null || true)"

    echo
    echo "Dashboard HTTP status: $dashboard_code"

}

cleanup()
{
    exit_code=$?

    trap - EXIT INT TERM

    restore_services

    exit "$exit_code"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

cd "$PROJECT_DIR"

mkdir -p "$OUTPUT_DIR"

sudo -v

exec > >(
    tee "$OUTPUT_DIR/test_3-3_terminal.txt"
) 2>&1

echo "========== TEST 3-3: THREE INPUT RESOLUTIONS =========="
date --iso-8601=seconds

echo
echo "========== STOPPING CAMERA-DEPENDENT SERVICES =========="

sudo systemctl stop smart-guard-watchdog.service
sudo systemctl stop smart-guard-api.service
sudo systemctl stop smart-guard-mqtt.service
sudo systemctl stop smart-guard.service

sleep 5

echo
echo "========== CAMERA AVAILABILITY =========="

if fuser /dev/video0 >/dev/null 2>&1
then
    echo "ERROR: Camera is still in use."

    fuser -v /dev/video0 || true

    exit 1
fi

echo "PASS: /dev/video0 is available."

run_resolution()
{
    width="$1"
    height="$2"
    label="${width}x${height}"

    echo
    echo "============================================================"
    echo "PREPARE TEST: $label"
    echo "============================================================"
    echo "Keep exactly one real person in the fixed position."
    echo "Do not change the camera angle, distance, lighting or clothes."
    echo "The test will run for five minutes."
    echo

    read -r -p "Press Enter when the scene is ready..."

    for remaining in $(seq 10 -1 1)
    do
        echo "Test starts in $remaining seconds..."
        sleep 1
    done

    python3 \
        tests/section3/resolution_benchmark.py \
        --width "$width" \
        --height "$height" \
        --duration 300 \
        | tee \
        "$OUTPUT_DIR/${label}_terminal.txt"
}

run_resolution 320 240

echo
echo "Cooling interval: 60 seconds."
sleep 60

run_resolution 640 480

echo
echo "Cooling interval: 60 seconds."
sleep 60

run_resolution 1280 720

echo
echo "========== ALL RESOLUTION TESTS COMPLETED =========="

restore_services

trap - EXIT INT TERM

echo
echo "PASS: Test 3-3 completed successfully."
