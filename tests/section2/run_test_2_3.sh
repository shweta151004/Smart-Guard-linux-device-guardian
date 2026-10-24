#!/usr/bin/env bash

set -Eeuo pipefail

PROJECT_DIR="${SMART_GUARD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
OUTPUT_DIR="$PROJECT_DIR/docs/tests/section2/test_2-3_load"

SERVICE="smart-guard.service"
URL="https://127.0.0.1:8443/api/v1/telemetry"

CONCURRENCY=50
LOAD_SECONDS=30
BASELINE_REQUESTS=50

TELEMETRY_PID=""
INITIAL_PID=""
INITIAL_RESTARTS=""

fail()
{
    echo
    echo "FAIL: $1"
    exit 1
}

cleanup()
{
    exit_code=$?

    trap - EXIT INT TERM

    if [ -n "${TELEMETRY_PID:-}" ]; then
        kill "$TELEMETRY_PID" 2>/dev/null || true
        wait "$TELEMETRY_PID" 2>/dev/null || true
    fi

    exit "$exit_code"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

collect_telemetry_phase()
{
    phase_name="$1"
    duration_seconds="$2"
    output_file="$3"

    echo \
"phase,elapsed_seconds,timestamp,cpu_temperature_c,cpu_usage_percent,memory_available_mb" \
        > "$output_file"

    phase_start="$(date +%s)"

    for sample_number in $(seq 0 "$duration_seconds")
    do
        target_time=$((phase_start + sample_number))
        current_time="$(date +%s)"
        remaining=$((target_time - current_time))

        if [ "$remaining" -gt 0 ]; then
            sleep "$remaining"
        fi

        timestamp="$(date --iso-8601=seconds)"

        json="$(
            curl \
                -k \
                -sS \
                --connect-timeout 2 \
                --max-time 5 \
                "$URL" \
                2>/dev/null ||
            true
        )"

        parsed="$(
            printf '%s' "$json" |
            jq -r '
                [
                    .cpu_temperature_c,
                    .cpu_usage_percent,
                    .memory_available_mb
                ]
                | @tsv
            ' \
            2>/dev/null ||
            true
        )"

        if [ -n "$parsed" ]; then
            IFS=$'\t' read -r \
                cpu_temperature \
                cpu_usage \
                memory_available \
                <<< "$parsed"
        else
            cpu_temperature="NA"
            cpu_usage="NA"
            memory_available="NA"
        fi

        printf '%s,%s,%s,%s,%s,%s\n' \
            "$phase_name" \
            "$sample_number" \
            "$timestamp" \
            "$cpu_temperature" \
            "$cpu_usage" \
            "$memory_available" \
            | tee -a "$output_file"
    done
}

cd "$PROJECT_DIR" ||
    fail "Project directory was not found."

mkdir -p "$OUTPUT_DIR"

sudo -v ||
    fail "Sudo authentication failed."

exec > >(
    tee "$OUTPUT_DIR/test_2-3_terminal.txt"
) 2>&1

echo "========== TEST 2-3: 50 CONCURRENT TELEMETRY REQUESTS =========="
date --iso-8601=seconds

echo
echo "========== PREPARING SERVICES =========="

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
echo "========== ENDPOINT PRECHECK =========="

HTTP_CODE="$(
    curl \
        -k \
        -sS \
        --max-time 10 \
        -o /dev/null \
        -w '%{http_code}' \
        "$URL"
)"

echo "Telemetry HTTP status: $HTTP_CODE"

[ "$HTTP_CODE" = "200" ] ||
    fail "Telemetry endpoint did not return HTTP 200."

TELEMETRY_JSON="$(
    curl \
        -k \
        -sS \
        --max-time 10 \
        "$URL"
)"

echo "Telemetry response:"
echo "$TELEMETRY_JSON" | jq .

INITIAL_PID="$(
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

echo
echo "========== INITIAL PROCESS STATE =========="
echo "Initial PID: $INITIAL_PID"
echo "Initial restart count: $INITIAL_RESTARTS"

[ "$INITIAL_PID" -gt 0 ] ||
    fail "Smart Guard MainPID is invalid."

SINGLE_REQUEST_SCRIPT="$OUTPUT_DIR/single_request.sh"

cat > "$SINGLE_REQUEST_SCRIPT" <<'REQUEST'
#!/usr/bin/env bash

round_number="$1"
request_number="$2"
request_url="$3"

result="$(
    curl \
        -k \
        -sS \
        --connect-timeout 2 \
        --max-time 10 \
        -o /dev/null \
        -w '%{http_code},%{time_total}' \
        "$request_url" \
        2>/dev/null
)"

curl_exit=$?

if [ "$curl_exit" -eq 0 ] &&
   [[ "$result" =~ ^[0-9]{3},[0-9.]+$ ]]
then
    http_code="${result%%,*}"
    time_total="${result#*,}"
else
    http_code="000"
    time_total="NA"
fi

printf '%s,%s,%s,%s,%s\n' \
    "$round_number" \
    "$request_number" \
    "$http_code" \
    "$time_total" \
    "$curl_exit"
REQUEST

chmod +x "$SINGLE_REQUEST_SCRIPT"

echo
echo "========== BASELINE TELEMETRY =========="
echo "Collecting baseline telemetry for 10 seconds."

collect_telemetry_phase \
    baseline \
    10 \
    "$OUTPUT_DIR/telemetry_baseline.csv"

echo
echo "========== BASELINE RESPONSE LATENCY =========="

BASELINE_CSV="$OUTPUT_DIR/baseline_latency.csv"

echo \
"round,request,http_code,time_total_s,curl_exit" \
    > "$BASELINE_CSV"

for request_number in $(seq 1 "$BASELINE_REQUESTS")
do
    "$SINGLE_REQUEST_SCRIPT" \
        0 \
        "$request_number" \
        "$URL" \
        >> "$BASELINE_CSV"
done

echo "Baseline requests completed: $BASELINE_REQUESTS"

echo
echo "========== STARTING 50-CONCURRENT-REQUEST LOAD =========="

LOAD_CSV="$OUTPUT_DIR/load_requests.csv"

echo \
"round,request,http_code,time_total_s,curl_exit" \
    > "$LOAD_CSV"

collect_telemetry_phase \
    load \
    "$LOAD_SECONDS" \
    "$OUTPUT_DIR/telemetry_load.csv" &

TELEMETRY_PID=$!

sleep 1

LOAD_START_TIME="$(date +%s.%N)"

SECONDS=0
round_number=0

while [ "$SECONDS" -lt "$LOAD_SECONDS" ]
do
    round_number=$((round_number + 1))

    seq 1 "$CONCURRENCY" |
    xargs \
        -P "$CONCURRENCY" \
        -I '{}' \
        "$SINGLE_REQUEST_SCRIPT" \
        "$round_number" \
        '{}' \
        "$URL" \
        >> "$LOAD_CSV"

    echo \
"Round $round_number completed with $CONCURRENCY concurrent requests."
done

LOAD_END_TIME="$(date +%s.%N)"

ACTUAL_LOAD_DURATION="$(
    awk \
        -v start="$LOAD_START_TIME" \
        -v end="$LOAD_END_TIME" \
        'BEGIN {
            printf "%.3f", end - start
        }'
)"

echo "$ACTUAL_LOAD_DURATION" \
    > "$OUTPUT_DIR/load_duration_seconds.txt"

echo "$round_number" \
    > "$OUTPUT_DIR/load_rounds.txt"

wait "$TELEMETRY_PID"
TELEMETRY_PID=""

echo
echo "Requested load duration: ${LOAD_SECONDS} seconds"
echo "Actual load duration: ${ACTUAL_LOAD_DURATION} seconds"
echo "Completed load rounds: $round_number"
echo "Concurrent requests per round: $CONCURRENCY"

echo
echo "========== RECOVERY TELEMETRY =========="
echo "Collecting recovery telemetry for 10 seconds."

collect_telemetry_phase \
    recovery \
    10 \
    "$OUTPUT_DIR/telemetry_recovery.csv"

echo
echo "========== FINAL PROCESS VALIDATION =========="

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

echo "Initial PID: $INITIAL_PID"
echo "Final PID: $FINAL_PID"
echo "Initial restart count: $INITIAL_RESTARTS"
echo "Final restart count: $FINAL_RESTARTS"

echo
echo "========== ANALYZING RESULTS =========="

python3 <<'PY'
import csv
import math
from pathlib import Path
from statistics import mean

import matplotlib
matplotlib.use("Agg")

import matplotlib.pyplot as plt

output_dir = Path(
    "docs/tests/section2/test_2-3_load"
)


def percentile(values, percent):
    ordered = sorted(values)

    if not ordered:
        return 0.0

    position = (
        (len(ordered) - 1)
        * percent
        / 100.0
    )

    lower = math.floor(position)
    upper = math.ceil(position)

    if lower == upper:
        return ordered[int(position)]

    fraction = position - lower

    return (
        ordered[lower]
        + (
            ordered[upper]
            - ordered[lower]
        )
        * fraction
    )


def read_latency(path):
    total = 0
    success = 0
    values = []
    rounds = set()

    with path.open(
        newline="",
        encoding="utf-8",
    ) as handle:
        reader = csv.DictReader(handle)

        for row in reader:
            total += 1

            rounds.add(
                int(row["round"])
            )

            if row["http_code"] == "200":
                success += 1

            try:
                values.append(
                    float(row["time_total_s"])
                )
            except ValueError:
                pass

    if not values:
        raise RuntimeError(
            f"No valid latency values in {path}."
        )

    return {
        "total": total,
        "success": success,
        "success_rate": (
            success / total * 100.0
            if total
            else 0.0
        ),
        "rounds": len(rounds),
        "minimum": min(values),
        "mean": mean(values),
        "p50": percentile(values, 50),
        "p95": percentile(values, 95),
        "maximum": max(values),
    }


def read_telemetry(path):
    rows = []

    with path.open(
        newline="",
        encoding="utf-8",
    ) as handle:
        reader = csv.DictReader(handle)

        for row in reader:
            try:
                rows.append(
                    {
                        "elapsed": int(
                            row["elapsed_seconds"]
                        ),
                        "temperature": float(
                            row["cpu_temperature_c"]
                        ),
                        "cpu": float(
                            row["cpu_usage_percent"]
                        ),
                        "memory": float(
                            row["memory_available_mb"]
                        ),
                    }
                )
            except ValueError:
                continue

    if not rows:
        raise RuntimeError(
            f"No valid telemetry rows in {path}."
        )

    return rows


def telemetry_summary(rows):
    temperatures = [
        row["temperature"]
        for row in rows
    ]

    cpu_values = [
        row["cpu"]
        for row in rows
    ]

    memory_values = [
        row["memory"]
        for row in rows
    ]

    return {
        "samples": len(rows),
        "temperature_average": mean(
            temperatures
        ),
        "temperature_maximum": max(
            temperatures
        ),
        "cpu_average": mean(
            cpu_values
        ),
        "cpu_maximum": max(
            cpu_values
        ),
        "memory_average": mean(
            memory_values
        ),
        "memory_minimum": min(
            memory_values
        ),
    }


baseline_latency = read_latency(
    output_dir / "baseline_latency.csv"
)

load_latency = read_latency(
    output_dir / "load_requests.csv"
)

telemetry_data = {
    "Baseline": read_telemetry(
        output_dir / "telemetry_baseline.csv"
    ),
    "Load": read_telemetry(
        output_dir / "telemetry_load.csv"
    ),
    "Recovery": read_telemetry(
        output_dir / "telemetry_recovery.csv"
    ),
}

telemetry_stats = {
    phase: telemetry_summary(rows)
    for phase, rows in telemetry_data.items()
}

mean_latency_increase_seconds = (
    load_latency["mean"]
    - baseline_latency["mean"]
)

mean_latency_increase_ms = (
    mean_latency_increase_seconds
    * 1000.0
)

if baseline_latency["mean"] > 0:
    mean_latency_increase_percent = (
        mean_latency_increase_seconds
        / baseline_latency["mean"]
        * 100.0
    )
else:
    mean_latency_increase_percent = 0.0

p95_increase_ms = (
    load_latency["p95"]
    - baseline_latency["p95"]
) * 1000.0

baseline_stats = telemetry_stats["Baseline"]
load_stats = telemetry_stats["Load"]
recovery_stats = telemetry_stats["Recovery"]

cpu_average_change = (
    load_stats["cpu_average"]
    - baseline_stats["cpu_average"]
)

cpu_maximum_change = (
    load_stats["cpu_maximum"]
    - baseline_stats["cpu_maximum"]
)

temperature_average_change = (
    load_stats["temperature_average"]
    - baseline_stats["temperature_average"]
)

temperature_maximum_change = (
    load_stats["temperature_maximum"]
    - baseline_stats["temperature_maximum"]
)

memory_available_change = (
    load_stats["memory_average"]
    - baseline_stats["memory_average"]
)

memory_consumption_increase = (
    baseline_stats["memory_average"]
    - load_stats["memory_average"]
)

actual_duration = float(
    (
        output_dir
        / "load_duration_seconds.txt"
    ).read_text().strip()
)

throughput = (
    load_latency["total"]
    / actual_duration
    if actual_duration > 0
    else 0.0
)

latency_metrics = [
    "Mean",
    "P50",
    "P95",
    "Maximum",
]

baseline_latency_ms = [
    baseline_latency["mean"] * 1000.0,
    baseline_latency["p50"] * 1000.0,
    baseline_latency["p95"] * 1000.0,
    baseline_latency["maximum"] * 1000.0,
]

load_latency_ms = [
    load_latency["mean"] * 1000.0,
    load_latency["p50"] * 1000.0,
    load_latency["p95"] * 1000.0,
    load_latency["maximum"] * 1000.0,
]

positions = list(
    range(len(latency_metrics))
)

bar_width = 0.36

plt.figure(figsize=(10, 6))

plt.bar(
    [
        position - bar_width / 2
        for position in positions
    ],
    baseline_latency_ms,
    width=bar_width,
    label="Baseline",
)

plt.bar(
    [
        position + bar_width / 2
        for position in positions
    ],
    load_latency_ms,
    width=bar_width,
    label="50 concurrent requests",
)

plt.xticks(
    positions,
    latency_metrics,
)

plt.ylabel("Response latency (ms)")
plt.title(
    "Telemetry API response latency before and during load"
)
plt.grid(
    axis="y",
    alpha=0.3,
)
plt.legend()
plt.tight_layout()

plt.savefig(
    output_dir / "latency_comparison.png",
    dpi=200,
)

plt.close()

figure, axes = plt.subplots(
    3,
    1,
    figsize=(11, 10),
    sharex=True,
)

phase_offset = 0
phase_ranges = {}

for phase, rows in telemetry_data.items():
    x_values = [
        phase_offset + row["elapsed"]
        for row in rows
    ]

    phase_ranges[phase] = (
        min(x_values),
        max(x_values),
    )

    axes[0].plot(
        x_values,
        [
            row["temperature"]
            for row in rows
        ],
        marker="o",
        label=phase,
    )

    axes[1].plot(
        x_values,
        [
            row["cpu"]
            for row in rows
        ],
        marker="o",
        label=phase,
    )

    axes[2].plot(
        x_values,
        [
            row["memory"]
            for row in rows
        ],
        marker="o",
        label=phase,
    )

    phase_offset = max(x_values) + 1

load_start, load_end = phase_ranges["Load"]

for axis in axes:
    axis.axvspan(
        load_start,
        load_end,
        alpha=0.1,
        label="Load period",
    )

    axis.grid(
        True,
        alpha=0.3,
    )

axes[0].set_ylabel(
    "CPU temperature (C)"
)

axes[1].set_ylabel(
    "CPU usage (%)"
)

axes[2].set_ylabel(
    "Available memory (MB)"
)

axes[2].set_xlabel(
    "Combined elapsed time (seconds)"
)

axes[0].set_title(
    "System telemetry before, during and after load"
)

axes[0].legend()

plt.tight_layout()

plt.savefig(
    output_dir / "telemetry_during_load.png",
    dpi=200,
)

plt.close()

summary_rows = [
    (
        "Baseline total requests",
        baseline_latency["total"],
    ),
    (
        "Load total requests",
        load_latency["total"],
    ),
    (
        "Load successful HTTP 200 responses",
        load_latency["success"],
    ),
    (
        "Load success rate (%)",
        f"{load_latency['success_rate']:.3f}",
    ),
    (
        "Load throughput (requests/second)",
        f"{throughput:.3f}",
    ),
    (
        "Baseline mean latency (ms)",
        f"{baseline_latency_ms[0]:.3f}",
    ),
    (
        "Load mean latency (ms)",
        f"{load_latency_ms[0]:.3f}",
    ),
    (
        "Mean latency increase (ms)",
        f"{mean_latency_increase_ms:.3f}",
    ),
    (
        "Mean latency increase (%)",
        f"{mean_latency_increase_percent:.3f}",
    ),
    (
        "Baseline P95 latency (ms)",
        f"{baseline_latency_ms[2]:.3f}",
    ),
    (
        "Load P95 latency (ms)",
        f"{load_latency_ms[2]:.3f}",
    ),
    (
        "P95 latency increase (ms)",
        f"{p95_increase_ms:.3f}",
    ),
    (
        "Baseline average CPU usage (%)",
        f"{baseline_stats['cpu_average']:.3f}",
    ),
    (
        "Load average CPU usage (%)",
        f"{load_stats['cpu_average']:.3f}",
    ),
    (
        "Average CPU usage change (percentage points)",
        f"{cpu_average_change:.3f}",
    ),
    (
        "Maximum CPU usage change (percentage points)",
        f"{cpu_maximum_change:.3f}",
    ),
    (
        "Baseline average CPU temperature (C)",
        f"{baseline_stats['temperature_average']:.3f}",
    ),
    (
        "Load average CPU temperature (C)",
        f"{load_stats['temperature_average']:.3f}",
    ),
    (
        "Average CPU temperature change (C)",
        f"{temperature_average_change:.3f}",
    ),
    (
        "Maximum CPU temperature change (C)",
        f"{temperature_maximum_change:.3f}",
    ),
    (
        "Baseline average available memory (MB)",
        f"{baseline_stats['memory_average']:.3f}",
    ),
    (
        "Load average available memory (MB)",
        f"{load_stats['memory_average']:.3f}",
    ),
    (
        "Available memory change (MB)",
        f"{memory_available_change:.3f}",
    ),
    (
        "Estimated memory consumption increase (MB)",
        f"{memory_consumption_increase:.3f}",
    ),
    (
        "Recovery average CPU usage (%)",
        f"{recovery_stats['cpu_average']:.3f}",
    ),
    (
        "Recovery average CPU temperature (C)",
        f"{recovery_stats['temperature_average']:.3f}",
    ),
]

with (
    output_dir / "test_2-3_summary.csv"
).open(
    "w",
    newline="",
    encoding="utf-8",
) as handle:
    writer = csv.writer(handle)
    writer.writerow(
        [
            "metric",
            "value",
        ]
    )
    writer.writerows(summary_rows)

with (
    output_dir / "test_2-3_analysis.txt"
).open(
    "w",
    encoding="utf-8",
) as handle:
    for metric, value in summary_rows:
        line = f"{metric}: {value}"

        print(line)
        handle.write(line + "\n")

    conclusion = (
        "\nAverage response latency changed from "
        f"{baseline_latency_ms[0]:.3f} ms to "
        f"{load_latency_ms[0]:.3f} ms. "
        f"The change was {mean_latency_increase_ms:.3f} ms "
        f"({mean_latency_increase_percent:.3f}%). "
        f"Average CPU usage changed by "
        f"{cpu_average_change:.3f} percentage points, "
        f"average CPU temperature changed by "
        f"{temperature_average_change:.3f} C, and "
        f"available memory changed by "
        f"{memory_available_change:.3f} MB."
    )

    print(conclusion)
    handle.write(conclusion + "\n")
PY

echo
echo "========== TEST 2-3 ANALYSIS =========="

cat "$OUTPUT_DIR/test_2-3_analysis.txt"

echo
echo "========== GENERATED FILES =========="

ls -lh \
    "$OUTPUT_DIR/baseline_latency.csv" \
    "$OUTPUT_DIR/load_requests.csv" \
    "$OUTPUT_DIR/telemetry_baseline.csv" \
    "$OUTPUT_DIR/telemetry_load.csv" \
    "$OUTPUT_DIR/telemetry_recovery.csv" \
    "$OUTPUT_DIR/test_2-3_summary.csv" \
    "$OUTPUT_DIR/test_2-3_analysis.txt" \
    "$OUTPUT_DIR/latency_comparison.png" \
    "$OUTPUT_DIR/telemetry_during_load.png"

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
echo "========== TEST 2-3 RESULT =========="

echo "PASS: Fifty concurrent curl requests were executed in repeated rounds."
echo "PASS: The load phase continued for at least 30 seconds."
echo "PASS: Temperature, CPU usage and memory were recorded."
echo "PASS: Baseline and loaded response latency were compared."
echo "PASS: Latency and telemetry graphs were generated."

if [ "$INITIAL_PID" = "$FINAL_PID" ]; then
    echo "PASS: Smart Guard MainPID remained unchanged."
else
    echo "WARNING: Smart Guard MainPID changed during the test."
fi

if [ "$INITIAL_RESTARTS" = "$FINAL_RESTARTS" ]; then
    echo "PASS: Smart Guard service did not restart."
else
    echo "WARNING: Smart Guard service restart count changed."
fi

echo "Latency graph: $OUTPUT_DIR/latency_comparison.png"
echo "Telemetry graph: $OUTPUT_DIR/telemetry_during_load.png"
echo "Analysis: $OUTPUT_DIR/test_2-3_analysis.txt"
