#!/usr/bin/env bash

set -Eeuo pipefail

PROJECT_DIR="${SMART_GUARD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
OUTPUT_DIR="$PROJECT_DIR/docs/tests/section2/test_2-1_temperature"

FFMPEG_PID=""
CLIENT_PID=""
TEMP_SOURCE=""
TEMP_LABEL=""

stop_background_processes()
{
    if [ -n "${CLIENT_PID:-}" ]; then
        kill "$CLIENT_PID" 2>/dev/null || true
        wait "$CLIENT_PID" 2>/dev/null || true
        CLIENT_PID=""
    fi

    if [ -n "${FFMPEG_PID:-}" ]; then
        kill "$FFMPEG_PID" 2>/dev/null || true
        wait "$FFMPEG_PID" 2>/dev/null || true
        FFMPEG_PID=""
    fi
}

restore_services()
{
    echo
    echo "========== RESTORING PROJECT SERVICES =========="

    sudo systemctl start mosquitto.service || true
    sudo systemctl start smart-guard.service || true

    sleep 8

    sudo systemctl start smart-guard-watchdog.service || true
    sudo systemctl start smart-guard-mqtt.service || true
    sudo systemctl start smart-guard-api.service || true

    echo "smart-guard.service: $(systemctl is-active smart-guard.service || true)"
    echo "smart-guard-watchdog.service: $(systemctl is-active smart-guard-watchdog.service || true)"
    echo "smart-guard-mqtt.service: $(systemctl is-active smart-guard-mqtt.service || true)"
    echo "smart-guard-api.service: $(systemctl is-active smart-guard-api.service || true)"
    echo "mosquitto.service: $(systemctl is-active mosquitto.service || true)"
}

cleanup()
{
    exit_code=$?

    trap - EXIT INT TERM

    stop_background_processes
    restore_services

    exit "$exit_code"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

fail()
{
    echo
    echo "FAIL: $1"
    exit 1
}

select_temperature_sensor()
{
    python3 <<'PY'
from pathlib import Path

candidates = []

def add_candidate(path: Path, description: str, score: int) -> None:
    try:
        raw = float(path.read_text().strip())
    except (OSError, ValueError):
        return

    temperature = raw / 1000.0 if raw > 1000 else raw

    if not 0.0 < temperature < 150.0:
        return

    candidates.append((score, str(path), description))

for path in Path("/sys/class/hwmon").glob("hwmon*/temp*_input"):
    label_path = Path(str(path).replace("_input", "_label"))
    name_path = path.parent / "name"

    try:
        label = label_path.read_text().strip()
    except OSError:
        label = ""

    try:
        name = name_path.read_text().strip()
    except OSError:
        name = ""

    description = " ".join(part for part in (name, label) if part)
    text = description.lower()

    score = 10

    if "package id 0" in text:
        score = 100
    elif "tctl" in text:
        score = 99
    elif "tdie" in text:
        score = 98
    elif "cpu package" in text:
        score = 97
    elif "cpu" in text:
        score = 90
    elif "core 0" in text:
        score = 85

    add_candidate(path, description or str(path), score)

for zone in Path("/sys/class/thermal").glob("thermal_zone*"):
    path = zone / "temp"
    type_path = zone / "type"

    try:
        zone_type = type_path.read_text().strip()
    except OSError:
        zone_type = ""

    text = zone_type.lower()
    score = 10

    if zone_type == "x86_pkg_temp":
        score = 100
    elif "cpu" in text:
        score = 90
    elif "soc" in text:
        score = 80
    elif zone_type == "acpitz":
        score = 20

    add_candidate(path, zone_type or str(path), score)

if not candidates:
    raise SystemExit("No valid CPU temperature sensor was found.")

best = max(candidates, key=lambda item: item[0])

print(best[1])
print(best[2])
PY
}

read_temperature()
{
    raw_value="$(cat "$TEMP_SOURCE")"

    awk -v raw="$raw_value" '
        BEGIN {
            if (raw > 1000)
                raw = raw / 1000.0

            printf "%.1f\n", raw
        }
    '
}

collect_temperature()
{
    state_name="$1"
    output_file="$2"

    echo "state,elapsed_seconds,timestamp,cpu_temperature_c" \
        > "$output_file"

    for sample_number in $(seq 0 10)
    do
        elapsed_seconds=$((sample_number * 30))
        timestamp="$(date --iso-8601=seconds)"
        temperature="$(read_temperature)"

        printf '%s,%s,%s,%s\n' \
            "$state_name" \
            "$elapsed_seconds" \
            "$timestamp" \
            "$temperature" \
            | tee -a "$output_file"

        if [ "$sample_number" -lt 10 ]; then
            sleep 30
        fi
    done
}

show_state_summary()
{
    state_title="$1"
    csv_file="$2"

    echo
    echo "========== ${state_title} FINAL STATE =========="

    awk -F, '
        NR == 2 {
            count = 0
            minimum = $4
            maximum = $4
            first = $4
        }

        NR > 1 {
            count++
            sum += $4

            if ($4 < minimum)
                minimum = $4

            if ($4 > maximum)
                maximum = $4

            final = $4
        }

        END {
            printf "Samples: %d\n", count
            printf "Initial temperature: %.1f C\n", first
            printf "Final temperature: %.1f C\n", final
            printf "Minimum temperature: %.1f C\n", minimum
            printf "Average temperature: %.1f C\n", sum / count
            printf "Maximum temperature: %.1f C\n", maximum
            printf "End-start difference: %.1f C\n", final - first
        }
    ' "$csv_file"
}

cd "$PROJECT_DIR" ||
    fail "Project directory was not found."

mkdir -p "$OUTPUT_DIR"

sudo -v ||
    fail "Sudo authentication failed."

sudo rm -f /run/smart-guard/thermal_test_temperature

exec > >(
    tee "$OUTPUT_DIR/test_2-1_terminal.txt"
) 2>&1

echo "========== TEST 2-1: CPU TEMPERATURE IN THREE STATES =========="
date --iso-8601=seconds

mapfile -t sensor_information < <(
    select_temperature_sensor
)

if [ "${#sensor_information[@]}" -lt 2 ]; then
    fail "Temperature sensor selection failed."
fi

TEMP_SOURCE="${sensor_information[0]}"
TEMP_LABEL="${sensor_information[1]}"

echo
echo "========== SELECTED TEMPERATURE SENSOR =========="
echo "Sensor label: $TEMP_LABEL"
echo "Sensor path: $TEMP_SOURCE"
echo "Current temperature: $(read_temperature) C"

echo
echo "========== STATE A: IDLE =========="

sudo systemctl stop smart-guard-api.service || true
sudo systemctl stop smart-guard-mqtt.service || true
sudo systemctl stop smart-guard-watchdog.service || true
sudo systemctl stop smart-guard.service || true
sudo systemctl stop mosquitto.service || true

sleep 5

echo "smart-guard.service: $(systemctl is-active smart-guard.service || true)"
echo "smart-guard-watchdog.service: $(systemctl is-active smart-guard-watchdog.service || true)"
echo "smart-guard-mqtt.service: $(systemctl is-active smart-guard-mqtt.service || true)"
echo "smart-guard-api.service: $(systemctl is-active smart-guard-api.service || true)"
echo "mosquitto.service: $(systemctl is-active mosquitto.service || true)"

echo
echo "Idle stabilization: 60 seconds."
sleep 60

collect_temperature \
    "idle" \
    "$OUTPUT_DIR/temperature_idle.csv"

show_state_summary \
    "STATE A - IDLE" \
    "$OUTPUT_DIR/temperature_idle.csv"

echo
echo "SCREENSHOT 1: Take a screenshot of this terminal now."
read -r -p "Press Enter after taking the State A screenshot..."

echo
echo "========== STATE B: STREAM ONLY =========="

sudo systemctl stop smart-guard-api.service || true
sudo systemctl stop smart-guard-mqtt.service || true
sudo systemctl stop smart-guard-watchdog.service || true
sudo systemctl stop smart-guard.service || true

sleep 5

if sudo fuser /dev/video0 >/dev/null 2>&1; then
    echo "Camera is still in use:"
    sudo fuser -v /dev/video0 || true
    fail "The camera is not available for the stream-only state."
fi

ffmpeg \
    -hide_banner \
    -loglevel warning \
    -f v4l2 \
    -input_format mjpeg \
    -framerate 30 \
    -video_size 1280x720 \
    -i /dev/video0 \
    -an \
    -c:v copy \
    -f mjpeg \
    -listen 1 \
    http://127.0.0.1:8554/stream.mjpg \
    > "$OUTPUT_DIR/ffmpeg_stream_only.log" 2>&1 &

FFMPEG_PID=$!

sleep 2

curl \
    -sS \
    --max-time 450 \
    http://127.0.0.1:8554/stream.mjpg \
    -o /dev/null \
    2> "$OUTPUT_DIR/stream_only_client.log" &

CLIENT_PID=$!

sleep 5

kill -0 "$FFMPEG_PID" 2>/dev/null ||
    fail "FFmpeg stopped unexpectedly. Check ffmpeg_stream_only.log."

kill -0 "$CLIENT_PID" 2>/dev/null ||
    fail "The stream-only client stopped unexpectedly."

echo "FFmpeg PID: $FFMPEG_PID"
echo "Stream client PID: $CLIENT_PID"

echo
echo "Stream-only stabilization: 60 seconds."
sleep 60

collect_temperature \
    "stream_only" \
    "$OUTPUT_DIR/temperature_stream_only.csv"

show_state_summary \
    "STATE B - STREAM ONLY" \
    "$OUTPUT_DIR/temperature_stream_only.csv"

echo
echo "========== STREAM-ONLY PROCESS EVIDENCE =========="

ps \
    -p "$FFMPEG_PID","$CLIENT_PID" \
    -o pid,stat,etime,comm,args \
    || true

echo
echo "SCREENSHOT 2: Take a screenshot of this terminal now."
read -r -p "Press Enter after taking the State B screenshot..."

stop_background_processes

sleep 5

echo
echo "========== STATE C: STREAM WITH ACTIVE DETECTION =========="

sudo systemctl start mosquitto.service
sudo systemctl start smart-guard.service

sleep 8

sudo systemctl start smart-guard-watchdog.service
sudo systemctl start smart-guard-mqtt.service
sudo systemctl start smart-guard-api.service

systemctl is-active --quiet smart-guard.service ||
    fail "Smart Guard did not start."

systemctl is-active --quiet smart-guard-watchdog.service ||
    fail "Watchdog did not start."

curl \
    -k \
    -sS \
    --max-time 450 \
    https://127.0.0.1:8443/api/v1/stream \
    -o /dev/null \
    2> "$OUTPUT_DIR/detection_stream_client.log" &

CLIENT_PID=$!

sleep 5

kill -0 "$CLIENT_PID" 2>/dev/null ||
    fail "The Smart Guard stream client stopped unexpectedly."

echo "Smart Guard stream client PID: $CLIENT_PID"

echo
echo "Stream-with-detection stabilization: 60 seconds."
sleep 60

collect_temperature \
    "stream_detection" \
    "$OUTPUT_DIR/temperature_stream_detection.csv"

show_state_summary \
    "STATE C - STREAM WITH ACTIVE DETECTION" \
    "$OUTPUT_DIR/temperature_stream_detection.csv"

echo
echo "========== ACTIVE SERVICE EVIDENCE =========="

for service_name in \
    smart-guard.service \
    smart-guard-watchdog.service \
    smart-guard-mqtt.service \
    smart-guard-api.service \
    mosquitto.service
do
    printf '%-38s %s\n' \
        "$service_name" \
        "$(systemctl is-active "$service_name" || true)"
done

echo
echo "========== TELEMETRY EVIDENCE =========="

curl \
    -k \
    -sS \
    https://127.0.0.1:8443/api/v1/telemetry \
    || true

echo
echo
echo "SCREENSHOT 3: Take a screenshot of this terminal now."
read -r -p "Press Enter after taking the State C screenshot..."

stop_background_processes

echo
echo "========== GENERATING GRAPH AND SUMMARY TABLE =========="

python3 <<'PY'
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")

import matplotlib.pyplot as plt

output_dir = Path(
    "docs/tests/section2/test_2-1_temperature"
)

datasets = [
    (
        "Idle",
        output_dir / "temperature_idle.csv",
    ),
    (
        "Stream only",
        output_dir / "temperature_stream_only.csv",
    ),
    (
        "Stream + detection",
        output_dir / "temperature_stream_detection.csv",
    ),
]

summary_rows = []

plt.figure(figsize=(10, 6))

for label, csv_path in datasets:
    elapsed = []
    temperatures = []

    with csv_path.open(
        newline="",
        encoding="utf-8",
    ) as handle:
        reader = csv.DictReader(handle)

        for row in reader:
            elapsed.append(
                int(row["elapsed_seconds"])
            )

            temperatures.append(
                float(row["cpu_temperature_c"])
            )

    if len(temperatures) != 11:
        raise RuntimeError(
            f"{csv_path} contains "
            f"{len(temperatures)} samples instead of 11."
        )

    plt.plot(
        elapsed,
        temperatures,
        marker="o",
        label=label,
    )

    summary_rows.append(
        {
            "state": label,
            "minimum_c": min(temperatures),
            "average_c": (
                sum(temperatures) / len(temperatures)
            ),
            "maximum_c": max(temperatures),
            "initial_c": temperatures[0],
            "final_c": temperatures[-1],
            "increase_c": (
                temperatures[-1] - temperatures[0]
            ),
        }
    )

plt.xlabel("Elapsed time (seconds)")
plt.ylabel("CPU temperature (C)")
plt.title(
    "CPU temperature in three operating states"
)
plt.grid(True, alpha=0.3)
plt.legend()
plt.tight_layout()

plt.savefig(
    output_dir / "temperature_three_states.png",
    dpi=200,
)

plt.close()

fieldnames = [
    "state",
    "minimum_c",
    "average_c",
    "maximum_c",
    "initial_c",
    "final_c",
    "increase_c",
]

with (
    output_dir / "temperature_summary.csv"
).open(
    "w",
    newline="",
    encoding="utf-8",
) as handle:
    writer = csv.DictWriter(
        handle,
        fieldnames=fieldnames,
    )

    writer.writeheader()

    for row in summary_rows:
        writer.writerow(
            {
                key: (
                    f"{value:.1f}"
                    if isinstance(value, float)
                    else value
                )
                for key, value in row.items()
            }
        )

with (
    output_dir / "temperature_analysis.txt"
).open(
    "w",
    encoding="utf-8",
) as handle:
    for row in summary_rows:
        line = (
            f"{row['state']}: "
            f"min={row['minimum_c']:.1f} C, "
            f"avg={row['average_c']:.1f} C, "
            f"max={row['maximum_c']:.1f} C, "
            f"initial={row['initial_c']:.1f} C, "
            f"final={row['final_c']:.1f} C, "
            f"end-start={row['increase_c']:.1f} C"
        )

        print(line)
        handle.write(line + "\n")
PY

echo
echo "========== FINAL SUMMARY TABLE =========="

column \
    -s, \
    -t \
    < "$OUTPUT_DIR/temperature_summary.csv" \
    2>/dev/null \
    || cat "$OUTPUT_DIR/temperature_summary.csv"

echo
echo "========== GENERATED FILES =========="

ls -lh \
    "$OUTPUT_DIR/temperature_idle.csv" \
    "$OUTPUT_DIR/temperature_stream_only.csv" \
    "$OUTPUT_DIR/temperature_stream_detection.csv" \
    "$OUTPUT_DIR/temperature_summary.csv" \
    "$OUTPUT_DIR/temperature_analysis.txt" \
    "$OUTPUT_DIR/temperature_three_states.png"

echo
echo "========== TEST 2-1 RESULT =========="

echo "PASS: Three operating states were tested."
echo "PASS: Each state contains 11 samples at 30-second intervals."
echo "PASS: Each state covers a five-minute measurement period."
echo "PASS: The three-curve temperature graph was generated."
echo "PASS: The temperature summary table was generated."
echo "Graph: $OUTPUT_DIR/temperature_three_states.png"
echo "Table: $OUTPUT_DIR/temperature_summary.csv"
