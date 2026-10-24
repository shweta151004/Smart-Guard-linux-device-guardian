#!/usr/bin/env bash

set -u

PROJECT_DIR="${SMART_GUARD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
OUTPUT_DIR="$PROJECT_DIR/docs/tests/section4/test_4-4_thermal_management"

cd "$PROJECT_DIR" || exit 1

clear

cat \
  "$OUTPUT_DIR/thermal_summary.txt"

echo
echo "========== GENERATED FILES =========="

ls -lh \
  "$OUTPUT_DIR/thermal_samples.csv" \
  "$OUTPUT_DIR/temperature_over_time.png" \
  "$OUTPUT_DIR/target_fps_over_time.png"

xdg-open \
  "$OUTPUT_DIR/temperature_over_time.png" \
  >/dev/null 2>&1 &

sleep 1

xdg-open \
  "$OUTPUT_DIR/target_fps_over_time.png" \
  >/dev/null 2>&1 &
