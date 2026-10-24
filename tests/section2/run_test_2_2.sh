#!/usr/bin/env bash

set -Eeuo pipefail

PROJECT_DIR="${SMART_GUARD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
OUTPUT_DIR="$PROJECT_DIR/docs/tests/section2/test_2-2_memory"
SERVICE="smart-guard.service"
STREAM_URL="https://127.0.0.1:8443/api/v1/stream"

STREAM_CLIENT_PID=""
MAIN_PID=""
INITIAL_RESTARTS=""

fail()
{
    echo
    echo "FAIL: $1"
    exit 1
}

stop_stream_client()
{
    if [ -n "${STREAM_CLIENT_PID:-}" ]; then
        kill "$STREAM_CLIENT_PID" 2>/dev/null || true
        wait "$STREAM_CLIENT_PID" 2>/dev/null || true
        STREAM_CLIENT_PID=""
    fi
}

cleanup()
{
    exit_code=$?

    trap - EXIT INT TERM

    stop_stream_client

    exit "$exit_code"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

read_status_value()
{
    field_name="$1"
    process_id="$2"

    awk \
        -v field="${field_name}:" \
        '$1 == field {
            print $2
            exit
        }' \
        "/proc/${process_id}/status"
}

cd "$PROJECT_DIR" ||
    fail "Project directory was not found."

mkdir -p "$OUTPUT_DIR"

sudo -v ||
    fail "Sudo authentication failed."

exec > >(
    tee "$OUTPUT_DIR/test_2-2_terminal.txt"
) 2>&1

echo "========== TEST 2-2: MEMORY USAGE DURING CONTINUOUS STREAM =========="
date --iso-8601=seconds

echo
echo "========== PREPARING PROJECT SERVICES =========="

sudo systemctl start mosquitto.service
sudo systemctl start smart-guard.service

sleep 8

sudo systemctl start smart-guard-watchdog.service
sudo systemctl start smart-guard-mqtt.service
sudo systemctl start smart-guard-api.service

for service_name in \
    smart-guard.service \
    smart-guard-watchdog.service \
    smart-guard-mqtt.service \
    smart-guard-api.service \
    mosquitto.service
do
    service_state="$(
        systemctl is-active "$service_name" ||
        true
    )"

    printf '%-38s %s\n' \
        "$service_name" \
        "$service_state"

    [ "$service_state" = "active" ] ||
        fail "$service_name is not active."
done

echo
echo "========== HTTPS PRECHECK =========="

HTTPS_CODE="$(
    curl \
        -k \
        -sS \
        --max-time 10 \
        -o /dev/null \
        -w '%{http_code}' \
        https://127.0.0.1:8443/
)"

echo "HTTPS status code: $HTTPS_CODE"

[ "$HTTPS_CODE" = "200" ] ||
    fail "The Smart Guard HTTPS server is not available."

MAIN_PID="$(
    systemctl show \
        "$SERVICE" \
        -p MainPID \
        --value
)"

INITIAL_RESTARTS="$(
    systemctl show \
        "$SERVICE" \
        -p NRestarts \
        --value
)"

[[ "$MAIN_PID" =~ ^[0-9]+$ ]] ||
    fail "The Smart Guard MainPID is invalid."

[ "$MAIN_PID" -gt 0 ] ||
    fail "The Smart Guard MainPID is zero."

[ -r "/proc/$MAIN_PID/status" ] ||
    fail "The Smart Guard process status file is unavailable."

echo
echo "========== TARGET PROCESS =========="

echo "Service: $SERVICE"
echo "Main PID: $MAIN_PID"
echo "Executable: $(readlink -f "/proc/$MAIN_PID/exe")"
echo "Initial restart count: $INITIAL_RESTARTS"
echo "Command line: $(tr '\0' ' ' < "/proc/$MAIN_PID/cmdline")"

echo
echo "========== STARTING CONTINUOUS STREAM CLIENT =========="

curl \
    -k \
    -sS \
    --no-buffer \
    --max-time 330 \
    "$STREAM_URL" \
    -o /dev/null \
    2> "$OUTPUT_DIR/stream_client.log" &

STREAM_CLIENT_PID=$!

sleep 5

kill -0 "$STREAM_CLIENT_PID" 2>/dev/null ||
    fail "The continuous stream client stopped unexpectedly."

echo "Stream client PID: $STREAM_CLIENT_PID"
echo "Stream URL: $STREAM_URL"
echo "PASS: Continuous stream client is active."

CSV_FILE="$OUTPUT_DIR/memory_samples.csv"

echo \
"sample,elapsed_seconds,timestamp,pid,rss_kb,rss_mb,vmsize_kb,vmdata_kb,pss_kb,threads" \
    > "$CSV_FILE"

echo
echo "========== FIVE-MINUTE MEMORY SAMPLING =========="
echo "Sampling interval: 5 seconds"
echo "Measurement duration: 300 seconds"
echo "Expected samples: 61"
echo

START_EPOCH="$(date +%s)"

for sample_number in $(seq 0 60)
do
    target_epoch=$((START_EPOCH + sample_number * 5))
    current_epoch="$(date +%s)"
    sleep_seconds=$((target_epoch - current_epoch))

    if [ "$sleep_seconds" -gt 0 ]; then
        sleep "$sleep_seconds"
    fi

    current_pid="$(
        systemctl show \
            "$SERVICE" \
            -p MainPID \
            --value
    )"

    [ "$current_pid" = "$MAIN_PID" ] ||
        fail "The Smart Guard MainPID changed during the test."

    kill -0 "$MAIN_PID" 2>/dev/null ||
        fail "The Smart Guard process stopped during the test."

    kill -0 "$STREAM_CLIENT_PID" 2>/dev/null ||
        fail "The continuous stream client stopped during the test."

    elapsed_seconds=$((sample_number * 5))
    timestamp="$(date --iso-8601=seconds)"

    rss_kb="$(
        read_status_value \
            VmRSS \
            "$MAIN_PID"
    )"

    vmsize_kb="$(
        read_status_value \
            VmSize \
            "$MAIN_PID"
    )"

    vmdata_kb="$(
        read_status_value \
            VmData \
            "$MAIN_PID"
    )"

    threads="$(
        awk \
            '$1 == "Threads:" {
                print $2
                exit
            }' \
            "/proc/$MAIN_PID/status"
    )"

    pss_kb="$(
        awk \
            '$1 == "Pss:" {
                print $2
                exit
            }' \
            "/proc/$MAIN_PID/smaps_rollup" \
            2>/dev/null ||
        true
    )"

    if [ -z "$pss_kb" ]; then
        pss_kb="NA"
    fi

    rss_mb="$(
        awk \
            -v value="$rss_kb" \
            'BEGIN {
                printf "%.3f", value / 1024.0
            }'
    )"

    vmdata_mb="$(
        awk \
            -v value="$vmdata_kb" \
            'BEGIN {
                printf "%.3f", value / 1024.0
            }'
    )"

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$sample_number" \
        "$elapsed_seconds" \
        "$timestamp" \
        "$MAIN_PID" \
        "$rss_kb" \
        "$rss_mb" \
        "$vmsize_kb" \
        "$vmdata_kb" \
        "$pss_kb" \
        "$threads" \
        >> "$CSV_FILE"

    printf \
        'Sample %02d | elapsed=%3ds | RSS=%8.3f MB | VmData=%8.3f MB | threads=%s\n' \
        "$sample_number" \
        "$elapsed_seconds" \
        "$rss_mb" \
        "$vmdata_mb" \
        "$threads"
done

stop_stream_client

echo
echo "========== POST-TEST PROCESS VALIDATION =========="

FINAL_PID="$(
    systemctl show \
        "$SERVICE" \
        -p MainPID \
        --value
)"

FINAL_RESTARTS="$(
    systemctl show \
        "$SERVICE" \
        -p NRestarts \
        --value
)"

echo "Initial PID: $MAIN_PID"
echo "Final PID: $FINAL_PID"
echo "Initial restart count: $INITIAL_RESTARTS"
echo "Final restart count: $FINAL_RESTARTS"

[ "$FINAL_PID" = "$MAIN_PID" ] ||
    fail "The Smart Guard process changed during the test."

[ "$FINAL_RESTARTS" = "$INITIAL_RESTARTS" ] ||
    fail "The Smart Guard service restarted during the test."

SAMPLE_COUNT="$(
    awk \
        'END {
            print NR - 1
        }' \
        "$CSV_FILE"
)"

echo "Recorded samples: $SAMPLE_COUNT"

[ "$SAMPLE_COUNT" -eq 61 ] ||
    fail "The CSV file does not contain 61 samples."

echo
echo "========== GENERATING MEMORY GRAPH AND ANALYSIS =========="

python3 <<'PY'
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")

import matplotlib.pyplot as plt

output_dir = Path(
    "docs/tests/section2/test_2-2_memory"
)

csv_path = output_dir / "memory_samples.csv"

rows = []

with csv_path.open(
    newline="",
    encoding="utf-8",
) as handle:
    reader = csv.DictReader(handle)

    for row in reader:
        rows.append(
            {
                "elapsed": int(
                    row["elapsed_seconds"]
                ),
                "rss_mb": float(
                    row["rss_mb"]
                ),
            }
        )

if len(rows) != 61:
    raise RuntimeError(
        f"Expected 61 samples, found {len(rows)}."
    )


def linear_fit(data):
    x_values = [
        item["elapsed"]
        for item in data
    ]

    y_values = [
        item["rss_mb"]
        for item in data
    ]

    x_average = sum(x_values) / len(x_values)
    y_average = sum(y_values) / len(y_values)

    denominator = sum(
        (x - x_average) ** 2
        for x in x_values
    )

    if denominator == 0:
        return 0.0, y_average

    slope = sum(
        (x - x_average) * (y - y_average)
        for x, y in zip(
            x_values,
            y_values,
        )
    ) / denominator

    intercept = y_average - slope * x_average

    return slope, intercept


elapsed = [
    row["elapsed"]
    for row in rows
]

rss_values = [
    row["rss_mb"]
    for row in rows
]

full_slope_per_second, _ = linear_fit(rows)

steady_rows = [
    row
    for row in rows
    if row["elapsed"] >= 60
]

steady_slope_per_second, steady_intercept = (
    linear_fit(steady_rows)
)

steady_slope_per_minute = (
    steady_slope_per_second * 60.0
)

early_window = [
    row["rss_mb"]
    for row in rows
    if 60 <= row["elapsed"] <= 120
]

late_window = [
    row["rss_mb"]
    for row in rows
    if 240 <= row["elapsed"] <= 300
]

early_average = (
    sum(early_window) /
    len(early_window)
)

late_average = (
    sum(late_window) /
    len(late_window)
)

initial_rss = rss_values[0]
final_rss = rss_values[-1]
minimum_rss = min(rss_values)
maximum_rss = max(rss_values)
average_rss = sum(rss_values) / len(rss_values)

delta_rss = final_rss - initial_rss

relative_change = (
    delta_rss / initial_rss * 100.0
    if initial_rss != 0
    else 0.0
)

window_difference = (
    late_average - early_average
)

allowed_growth = max(
    5.0,
    initial_rss * 0.05,
)

if (
    steady_slope_per_minute <= 0.5
    and window_difference <= allowed_growth
):
    verdict = "NO_CLEAR_LEAK"

    conclusion = (
        "No clear evidence of a sustained memory leak "
        "was observed during the five-minute test."
    )
else:
    verdict = "POSSIBLE_MEMORY_GROWTH"

    conclusion = (
        "A sustained memory-growth pattern may be present. "
        "A longer test with Valgrind or AddressSanitizer "
        "is recommended before confirming a memory leak."
    )

plt.figure(figsize=(10, 6))

plt.plot(
    elapsed,
    rss_values,
    marker="o",
    markersize=3,
    label="Resident memory (RSS)",
)

steady_elapsed = [
    row["elapsed"]
    for row in steady_rows
]

steady_trend = [
    steady_intercept
    + steady_slope_per_second * value
    for value in steady_elapsed
]

plt.plot(
    steady_elapsed,
    steady_trend,
    linestyle="--",
    label="Trend after 60 seconds",
)

plt.xlabel("Elapsed time (seconds)")
plt.ylabel("Resident memory RSS (MB)")
plt.title(
    "Smart Guard memory usage during continuous stream"
)
plt.grid(True, alpha=0.3)
plt.legend()
plt.tight_layout()

plt.savefig(
    output_dir / "memory_usage_over_time.png",
    dpi=200,
)

plt.close()

summary_rows = [
    ("Samples", str(len(rows))),
    ("Duration (seconds)", "300"),
    ("Sampling interval (seconds)", "5"),
    ("Initial RSS (MB)", f"{initial_rss:.3f}"),
    ("Final RSS (MB)", f"{final_rss:.3f}"),
    ("Minimum RSS (MB)", f"{minimum_rss:.3f}"),
    ("Average RSS (MB)", f"{average_rss:.3f}"),
    ("Maximum RSS (MB)", f"{maximum_rss:.3f}"),
    ("Final minus initial RSS (MB)", f"{delta_rss:.3f}"),
    ("Relative change (%)", f"{relative_change:.3f}"),
    (
        "Full-test slope (MB/min)",
        f"{full_slope_per_second * 60.0:.4f}",
    ),
    (
        "Steady-state slope after 60 s (MB/min)",
        f"{steady_slope_per_minute:.4f}",
    ),
    (
        "Average RSS from 60 to 120 s (MB)",
        f"{early_average:.3f}",
    ),
    (
        "Average RSS from 240 to 300 s (MB)",
        f"{late_average:.3f}",
    ),
    (
        "Late minus early average (MB)",
        f"{window_difference:.3f}",
    ),
    ("Verdict", verdict),
    ("Conclusion", conclusion),
]

with (
    output_dir / "memory_summary.csv"
).open(
    "w",
    newline="",
    encoding="utf-8",
) as handle:
    writer = csv.writer(handle)
    writer.writerow(["metric", "value"])
    writer.writerows(summary_rows)

with (
    output_dir / "memory_analysis.txt"
).open(
    "w",
    encoding="utf-8",
) as handle:
    for metric, value in summary_rows:
        line = f"{metric}: {value}"
        print(line)
        handle.write(line + "\n")
PY

echo
echo "========== MEMORY ANALYSIS =========="

cat "$OUTPUT_DIR/memory_analysis.txt"

echo
echo "========== GENERATED FILES =========="

ls -lh \
    "$OUTPUT_DIR/memory_samples.csv" \
    "$OUTPUT_DIR/memory_summary.csv" \
    "$OUTPUT_DIR/memory_analysis.txt" \
    "$OUTPUT_DIR/memory_usage_over_time.png" \
    "$OUTPUT_DIR/test_2-2_terminal.txt"

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
            systemctl is-active "$service_name" ||
            true
        )"
done

echo
echo "========== TEST 2-2 RESULT =========="

echo "PASS: Continuous streaming remained active for five minutes."
echo "PASS: 61 memory samples were recorded at five-second intervals."
echo "PASS: The Smart Guard MainPID remained unchanged."
echo "PASS: The Smart Guard service did not restart."
echo "PASS: The memory-over-time graph was generated."
echo "PASS: The memory-leak trend analysis was generated."

echo "Graph: $OUTPUT_DIR/memory_usage_over_time.png"
echo "Analysis: $OUTPUT_DIR/memory_analysis.txt"
