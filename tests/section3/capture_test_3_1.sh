#!/usr/bin/env bash

set -Eeuo pipefail

PROJECT_DIR="${SMART_GUARD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
BASE_DIR="$PROJECT_DIR/docs/tests/section3/test_3-1_lighting"
URL="https://127.0.0.1:8443/"

CONDITION="${1:-}"
TOTAL_SAMPLES=20
INTERVAL_SECONDS=3

case "$CONDITION" in
    daylight|artificial|low_light|backlight)
        ;;
    *)
        echo "Usage:"
        echo "  $0 daylight"
        echo "  $0 artificial"
        echo "  $0 low_light"
        echo "  $0 backlight"
        exit 1
        ;;
esac

OUTPUT_DIR="$BASE_DIR/$CONDITION"

mkdir -p "$OUTPUT_DIR"

rm -f \
    "$OUTPUT_DIR"/frame_*.png \
    "$OUTPUT_DIR"/capture_log.csv \
    "$OUTPUT_DIR"/"${CONDITION}_contact_sheet.png"

if ! systemctl is-active --quiet smart-guard.service
then
    echo "ERROR: smart-guard.service is not active."
    exit 1
fi

HTTP_CODE="$(
    curl \
        -k \
        -sS \
        --connect-timeout 2 \
        --max-time 5 \
        -o /dev/null \
        -w '%{http_code}' \
        "$URL"
)"

if [ "$HTTP_CODE" != "200" ]
then
    echo "ERROR: Smart Guard dashboard did not return HTTP 200."
    exit 1
fi

echo "sample,timestamp,file" > "$OUTPUT_DIR/capture_log.csv"

echo "============================================================"
echo "TEST 3-1: LIGHTING CONDITION"
echo "============================================================"
echo "Condition: $CONDITION"
echo "Total samples: $TOTAL_SAMPLES"
echo "Sampling interval: $INTERVAL_SECONDS seconds"
echo
echo "Open and maximize the Smart Guard dashboard in Firefox."
echo "Keep exactly one person in the fixed test position."
echo
echo "Capture starts in 10 seconds."

for remaining in $(seq 10 -1 1)
do
    echo "Starting in $remaining seconds..."
    sleep 1
done

for sample_number in $(seq 1 "$TOTAL_SAMPLES")
do
    printf -v frame_file \
        "%s/frame_%02d.png" \
        "$OUTPUT_DIR" \
        "$sample_number"

    gnome-screenshot \
        -w \
        -f "$frame_file"

    timestamp="$(date --iso-8601=seconds)"

    printf '%s,%s,%s\n' \
        "$sample_number" \
        "$timestamp" \
        "$frame_file" \
        >> "$OUTPUT_DIR/capture_log.csv"

    echo "Captured sample $sample_number of $TOTAL_SAMPLES"

    if [ "$sample_number" -lt "$TOTAL_SAMPLES" ]; then
        sleep "$INTERVAL_SECONDS"
    fi
done

montage \
    "$OUTPUT_DIR"/frame_*.png \
    -thumbnail 600x338 \
    -tile 4x5 \
    -geometry +8+8 \
    "$OUTPUT_DIR/${CONDITION}_contact_sheet.png"

echo
echo "============================================================"
echo "CAPTURE COMPLETED"
echo "============================================================"
echo "Condition: $CONDITION"
echo "Captured samples: $TOTAL_SAMPLES"
echo "Contact sheet: $OUTPUT_DIR/${CONDITION}_contact_sheet.png"
