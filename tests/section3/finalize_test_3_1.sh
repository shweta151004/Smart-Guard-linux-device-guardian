#!/usr/bin/env bash

set -Eeuo pipefail

PROJECT_DIR="${SMART_GUARD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
BASE_DIR="$PROJECT_DIR/docs/tests/section3/test_3-1_lighting"

CONDITIONS=(daylight artificial low_light backlight)

declare -A LABELS
LABELS[daylight]="Daylight"
LABELS[artificial]="Artificial light"
LABELS[low_light]="Low light"
LABELS[backlight]="Backlight"

echo "========== TEST 3-1 FINALIZATION =========="
echo

echo "========== VERIFYING CAPTURED FRAMES =========="

for condition in "${CONDITIONS[@]}"
do
    OUTPUT_DIR="$BASE_DIR/$condition"

    if [ ! -d "$OUTPUT_DIR" ]; then
        echo "ERROR: Missing directory: $OUTPUT_DIR"
        exit 1
    fi

    FRAME_COUNT="$(find "$OUTPUT_DIR" -maxdepth 1 -type f -name 'frame_*.png' | wc -l)"

    echo "${LABELS[$condition]}: $FRAME_COUNT frames"

    if [ "$FRAME_COUNT" -ne 20 ]; then
        echo "ERROR: Exactly 20 frames are required for $condition."
        exit 1
    fi
done

echo
echo "PASS: All four lighting conditions contain exactly 20 frames."

echo
echo "========== CREATING CONTACT SHEETS =========="

for condition in "${CONDITIONS[@]}"
do
    OUTPUT_DIR="$BASE_DIR/$condition"
    CONTACT_SHEET="$OUTPUT_DIR/${condition}_contact_sheet.png"

    rm -f "$CONTACT_SHEET"

    montage \
        "$OUTPUT_DIR"/frame_*.png \
        -thumbnail 600x338 \
        -tile 4x5 \
        -geometry +8+8 \
        "$CONTACT_SHEET"

    echo "Created: $CONTACT_SHEET"
done

echo
echo "========== CREATING FOUR-CONDITION OVERVIEW =========="

rm -f "$BASE_DIR/all_lighting_contact_sheets.png"

montage \
    "$BASE_DIR/daylight/daylight_contact_sheet.png" \
    "$BASE_DIR/artificial/artificial_contact_sheet.png" \
    "$BASE_DIR/low_light/low_light_contact_sheet.png" \
    "$BASE_DIR/backlight/backlight_contact_sheet.png" \
    -thumbnail 900x506 \
    -tile 2x2 \
    -geometry +15+15 \
    "$BASE_DIR/all_lighting_contact_sheets.png"

echo "Created: $BASE_DIR/all_lighting_contact_sheets.png"

echo
echo "========== ACCURACY ENTRY =========="
echo "Review each 20-frame contact sheet."
echo "Enter the number of correct detections from 0 to 20."
echo

rm -f \
    "$BASE_DIR/lighting_accuracy.csv" \
    "$BASE_DIR/lighting_accuracy.png"

echo "Lighting condition,Correct detections,Incorrect detections,Total samples,Accuracy percent" \
    > "$BASE_DIR/lighting_accuracy.csv"

for condition in "${CONDITIONS[@]}"
do
    while true
    do
        read -rp "${LABELS[$condition]} correct detections [0-20]: " correct

        if [[ "$correct" =~ ^[0-9]+$ ]] && [ "$correct" -ge 0 ] && [ "$correct" -le 20 ]; then
            break
        fi

        echo "Please enter an integer from 0 to 20."
    done

    incorrect=$((20 - correct))
    accuracy="$(awk -v c="$correct" 'BEGIN { printf "%.2f", (c / 20.0) * 100.0 }')"

    printf '%s,%d,%d,20,%s\n' \
        "${LABELS[$condition]}" \
        "$correct" \
        "$incorrect" \
        "$accuracy" \
        >> "$BASE_DIR/lighting_accuracy.csv"
done

echo
echo "========== GENERATING ACCURACY GRAPH =========="

python3 - "$BASE_DIR" <<'PY'
import csv
import os
import sys

import matplotlib.pyplot as plt

base_dir = sys.argv[1]
csv_path = os.path.join(base_dir, "lighting_accuracy.csv")
png_path = os.path.join(base_dir, "lighting_accuracy.png")

conditions = []
accuracy = []

with open(csv_path, newline="", encoding="utf-8") as f:
    reader = csv.DictReader(f)

    for row in reader:
        conditions.append(row["Lighting condition"])
        accuracy.append(float(row["Accuracy percent"]))

fig, ax = plt.subplots(figsize=(10, 6))

bars = ax.bar(conditions, accuracy)

ax.set_title("Person Detection Accuracy Under Different Lighting Conditions")
ax.set_xlabel("Lighting condition")
ax.set_ylabel("Accuracy (%)")
ax.set_ylim(0, 105)
ax.grid(axis="y", alpha=0.25)

for bar, value in zip(bars, accuracy):
    ax.text(
        bar.get_x() + bar.get_width() / 2,
        value + 1,
        f"{value:.1f}%",
        ha="center",
        va="bottom"
    )

fig.tight_layout()
fig.savefig(png_path, dpi=180)
plt.close(fig)

print(f"Created: {png_path}")
PY

echo
echo "========== FINAL ACCURACY RESULTS =========="
column -s, -t "$BASE_DIR/lighting_accuracy.csv"

echo
echo "========== FINAL OUTPUT FILES =========="

for condition in "${CONDITIONS[@]}"
do
    echo "$BASE_DIR/$condition/${condition}_contact_sheet.png"
done

echo "$BASE_DIR/all_lighting_contact_sheets.png"
echo "$BASE_DIR/lighting_accuracy.csv"
echo "$BASE_DIR/lighting_accuracy.png"

echo
echo "PASS: Test 3-1 finalization completed successfully."
