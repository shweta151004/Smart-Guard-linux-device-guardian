#!/usr/bin/env python3

import csv
import json
import signal
import ssl
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parents[2]
OUTPUT_DIR = (
    PROJECT_DIR
    / "docs/tests/section4/test_4-4_thermal_management"
)

THERMAL_URL = "https://127.0.0.1:8443/api/v1/thermal"
TELEMETRY_URL = "https://127.0.0.1:8443/api/v1/telemetry"

CSV_FILE = OUTPUT_DIR / "thermal_samples.csv"
SUMMARY_FILE = OUTPUT_DIR / "thermal_summary.txt"
STRESS_LOG_FILE = OUTPUT_DIR / "stress_ng_output.txt"
TEMPERATURE_GRAPH = OUTPUT_DIR / "temperature_over_time.png"
FPS_GRAPH = OUTPUT_DIR / "target_fps_over_time.png"

SERVICE_NAMES = (
    "mosquitto.service",
    "smart-guard.service",
    "smart-guard-watchdog.service",
    "smart-guard-mqtt.service",
    "smart-guard-api.service",
)

SAFE_START_TIMEOUT = 600
BASELINE_SECONDS = 20
MAX_HEATING_SECONDS = 300
HOT_STATE_HOLD_SECONDS = 20
RECOVERY_TIMEOUT_SECONDS = 600
SAMPLE_INTERVAL_SECONDS = 2
SAFETY_LIMIT_C = 88.0

SSL_CONTEXT = ssl._create_unverified_context()

OUTPUT_DIR.mkdir(
    parents=True,
    exist_ok=True,
)

samples = []
stress_process = None
stress_log_handle = None


def get_json(url, timeout=4):
    request = urllib.request.Request(
        url,
        headers={
            "User-Agent": "Smart-Guard-Thermal-Test",
        },
    )

    try:
        with urllib.request.urlopen(
            request,
            timeout=timeout,
            context=SSL_CONTEXT,
        ) as response:
            body = response.read().decode(
                "utf-8",
                errors="replace",
            )

            return response.status, json.loads(body)

    except (
        urllib.error.URLError,
        urllib.error.HTTPError,
        json.JSONDecodeError,
        TimeoutError,
        ConnectionError,
    ):
        return 0, {}


def get_thermal():
    status, data = get_json(THERMAL_URL)

    if status != 200:
        return None

    required_fields = (
        "thermal_status",
        "throttled",
        "cpu_temperature_c",
        "high_threshold_c",
        "recovery_threshold_c",
        "normal_fps",
        "reduced_fps",
        "target_fps",
        "incident_count",
    )

    if not all(
        field in data
        for field in required_fields
    ):
        return None

    return data


def get_cpu_usage():
    status, data = get_json(TELEMETRY_URL)

    if status != 200:
        return -1.0

    try:
        return float(
            data.get(
                "cpu_usage_percent",
                -1.0,
            )
        )
    except (TypeError, ValueError):
        return -1.0


def command_output(command):
    result = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        check=False,
    )

    return result.stdout.strip()


def service_state(service):
    result = command_output([
        "systemctl",
        "is-active",
        service,
    ])

    return result or "unknown"


def smart_guard_pid():
    result = command_output([
        "systemctl",
        "show",
        "-p",
        "MainPID",
        "--value",
        "smart-guard.service",
    ])

    try:
        return int(result)
    except ValueError:
        return 0


def restart_count():
    result = command_output([
        "systemctl",
        "show",
        "-p",
        "NRestarts",
        "--value",
        "smart-guard.service",
    ])

    try:
        return int(result)
    except ValueError:
        return 0


def read_rss_mb(pid):
    if pid <= 0:
        return -1.0

    status_path = Path(
        f"/proc/{pid}/status"
    )

    try:
        lines = status_path.read_text(
            encoding="utf-8"
        ).splitlines()
    except OSError:
        return -1.0

    for line in lines:
        if line.startswith("VmRSS:"):
            fields = line.split()

            if len(fields) >= 2:
                try:
                    return round(
                        float(fields[1]) / 1024.0,
                        3,
                    )
                except ValueError:
                    return -1.0

    return -1.0


test_start = time.monotonic()


def elapsed_seconds():
    return round(
        time.monotonic() - test_start,
        1,
    )


def record_sample(phase):
    thermal = get_thermal()

    if thermal is None:
        print(
            f"{phase:<10} "
            "ERROR: Thermal API is unavailable.",
            flush=True,
        )

        return None

    pid = smart_guard_pid()

    sample = {
        "phase": phase,
        "elapsed_seconds": elapsed_seconds(),
        "timestamp": (
            datetime.now()
            .astimezone()
            .isoformat()
        ),
        "temperature_c": float(
            thermal["cpu_temperature_c"]
        ),
        "thermal_status": str(
            thermal["thermal_status"]
        ),
        "throttled": bool(
            thermal["throttled"]
        ),
        "high_threshold_c": float(
            thermal["high_threshold_c"]
        ),
        "recovery_threshold_c": float(
            thermal["recovery_threshold_c"]
        ),
        "normal_fps": int(
            thermal["normal_fps"]
        ),
        "reduced_fps": int(
            thermal["reduced_fps"]
        ),
        "target_fps": int(
            thermal["target_fps"]
        ),
        "incident_count": int(
            thermal["incident_count"]
        ),
        "cpu_usage_percent": round(
            get_cpu_usage(),
            3,
        ),
        "rss_mb": read_rss_mb(pid),
        "smart_guard_pid": pid,
    }

    samples.append(sample)

    print(
        f"{phase:<10} "
        f"t={sample['elapsed_seconds']:>6.1f}s "
        f"temp={sample['temperature_c']:>5.1f}C "
        f"state={sample['thermal_status']:<10} "
        f"throttled={str(sample['throttled']):<5} "
        f"target={sample['target_fps']:>2}FPS "
        f"cpu={sample['cpu_usage_percent']:>6.1f}% "
        f"incidents={sample['incident_count']}",
        flush=True,
    )

    return sample


def stop_stress():
    global stress_process

    if stress_process is None:
        return

    if stress_process.poll() is None:
        stress_process.send_signal(
            signal.SIGTERM
        )

        try:
            stress_process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            stress_process.kill()
            stress_process.wait(timeout=5)

    stress_process = None


def average(values):
    if not values:
        return -1.0

    return sum(values) / len(values)


initial_pid = smart_guard_pid()
initial_restarts = restart_count()

thermal_triggered = False
recovery_confirmed = False
threshold_reached = False
safety_stop = False
trigger_sample = None
trigger_reason = "none"
recovery_seconds = -1

try:
    print(
        "========== TEST 4-4: "
        "ADAPTIVE THERMAL MANAGEMENT =========="
    )

    print(
        "Thermal endpoint: "
        "/api/v1/thermal"
    )

    print()
    print(
        "========== INITIAL SERVICE STATES =========="
    )

    services_ready = True

    for service in SERVICE_NAMES:
        state = service_state(service)

        print(
            f"{service:<38} {state}"
        )

        if state != "active":
            services_ready = False

    if not services_ready:
        raise RuntimeError(
            "One or more required services are not active."
        )

    initial_thermal = get_thermal()

    if initial_thermal is None:
        raise RuntimeError(
            "Thermal API did not return valid data."
        )

    print()
    print(
        "========== WAITING FOR SAFE START =========="
    )

    print(
        "The test will start automatically when:"
    )

    print(
        "- thermal status is normal"
    )

    print(
        "- target FPS is 30"
    )

    print(
        "- CPU temperature is 75 C or lower"
    )

    safe_wait_start = time.monotonic()

    while True:
        sample = record_sample("cooldown")

        if sample is None:
            time.sleep(5)
            continue

        safe_start = (
            sample["thermal_status"].lower()
            == "normal"
            and not sample["throttled"]
            and sample["target_fps"]
            == sample["normal_fps"]
            and sample["temperature_c"]
            <= sample["recovery_threshold_c"]
        )

        if safe_start:
            print()
            print(
                "SAFE START CONDITION CONFIRMED."
            )

            break

        if (
            time.monotonic() - safe_wait_start
            >= SAFE_START_TIMEOUT
        ):
            raise RuntimeError(
                "CPU did not reach a safe starting "
                "temperature within ten minutes."
            )

        time.sleep(5)

    print()
    print(
        "========== BASELINE PHASE =========="
    )

    baseline_start = time.monotonic()

    while (
        time.monotonic() - baseline_start
        < BASELINE_SECONDS
    ):
        record_sample("baseline")
        time.sleep(SAMPLE_INTERVAL_SECONDS)

    baseline_samples = [
        sample
        for sample in samples
        if sample["phase"] == "baseline"
    ]

    baseline_incidents = (
        baseline_samples[-1]["incident_count"]
    )

    baseline_target_fps = (
        baseline_samples[-1]["target_fps"]
    )

    print()
    print(
        "========== STARTING CONTROLLED CPU LOAD =========="
    )

    stress_log_handle = STRESS_LOG_FILE.open(
        "w",
        encoding="utf-8",
    )

    stress_process = subprocess.Popen(
        [
            "stress-ng",
            "--cpu",
            "0",
            "--cpu-load",
            "100",
            "--timeout",
            f"{MAX_HEATING_SECONDS}s",
            "--metrics-brief",
        ],
        stdout=stress_log_handle,
        stderr=subprocess.STDOUT,
        text=True,
    )

    heating_start = time.monotonic()
    hot_state_start = None

    while (
        time.monotonic() - heating_start
        < MAX_HEATING_SECONDS
    ):
        sample = record_sample("heating")

        if sample is None:
            time.sleep(SAMPLE_INTERVAL_SECONDS)
            continue

        if (
            sample["temperature_c"]
            >= sample["high_threshold_c"]
        ):
            threshold_reached = True

        reasons = []

        if sample["throttled"]:
            reasons.append("throttled=true")

        if (
            sample["target_fps"]
            < baseline_target_fps
        ):
            reasons.append(
                "target FPS reduction"
            )

        if (
            sample["thermal_status"].lower()
            != "normal"
        ):
            reasons.append(
                "thermal status change"
            )

        if (
            sample["incident_count"]
            > baseline_incidents
        ):
            reasons.append(
                "incident counter increase"
            )

        if reasons and not thermal_triggered:
            thermal_triggered = True
            trigger_sample = sample
            trigger_reason = ", ".join(reasons)
            hot_state_start = time.monotonic()

            print()
            print(
                "=========================================="
            )

            print(
                "TAKE THE HOT-STATE SCREENSHOT NOW."
            )

            print(
                f"Reason: {trigger_reason}"
            )

            print(
                f"Temperature: "
                f"{sample['temperature_c']:.1f} C"
            )

            print(
                f"Thermal status: "
                f"{sample['thermal_status']}"
            )

            print(
                f"Target FPS: "
                f"{sample['target_fps']}"
            )

            print(
                "=========================================="
            )

        if (
            sample["temperature_c"]
            >= SAFETY_LIMIT_C
        ):
            safety_stop = True

            print()
            print(
                "SAFETY LIMIT REACHED."
            )

            print(
                "The CPU load is being stopped."
            )

            break

        if (
            thermal_triggered
            and hot_state_start is not None
            and time.monotonic() - hot_state_start
            >= HOT_STATE_HOLD_SECONDS
        ):
            print()
            print(
                "The hot state was held for "
                f"{HOT_STATE_HOLD_SECONDS} seconds."
            )

            break

        if stress_process.poll() is not None:
            break

        time.sleep(SAMPLE_INTERVAL_SECONDS)

    stop_stress()

    if stress_log_handle is not None:
        stress_log_handle.close()
        stress_log_handle = None

    print()
    print(
        "========== RECOVERY PHASE =========="
    )

    recovery_start = time.monotonic()

    while (
        time.monotonic() - recovery_start
        < RECOVERY_TIMEOUT_SECONDS
    ):
        sample = record_sample("recovery")

        if sample is None:
            time.sleep(SAMPLE_INTERVAL_SECONDS)
            continue

        recovered = (
            sample["thermal_status"].lower()
            == "normal"
            and not sample["throttled"]
            and sample["target_fps"]
            == sample["normal_fps"]
            and sample["temperature_c"]
            <= sample["recovery_threshold_c"]
        )

        if recovered:
            recovery_confirmed = True
            recovery_seconds = round(
                time.monotonic()
                - recovery_start,
                1,
            )

            print()
            print(
                "=========================================="
            )

            print(
                "THERMAL RECOVERY CONFIRMED."
            )

            print(
                "TAKE THE RECOVERY SCREENSHOT NOW."
            )

            print(
                f"Temperature: "
                f"{sample['temperature_c']:.1f} C"
            )

            print(
                f"Thermal status: "
                f"{sample['thermal_status']}"
            )

            print(
                f"Target FPS: "
                f"{sample['target_fps']}"
            )

            print(
                "=========================================="
            )

            break

        time.sleep(SAMPLE_INTERVAL_SECONDS)

finally:
    stop_stress()

    if stress_log_handle is not None:
        stress_log_handle.close()

fieldnames = [
    "phase",
    "elapsed_seconds",
    "timestamp",
    "temperature_c",
    "thermal_status",
    "throttled",
    "high_threshold_c",
    "recovery_threshold_c",
    "normal_fps",
    "reduced_fps",
    "target_fps",
    "incident_count",
    "cpu_usage_percent",
    "rss_mb",
    "smart_guard_pid",
]

with CSV_FILE.open(
    "w",
    newline="",
    encoding="utf-8",
) as handle:
    writer = csv.DictWriter(
        handle,
        fieldnames=fieldnames,
    )

    writer.writeheader()
    writer.writerows(samples)

try:
    import matplotlib.pyplot as plt

    elapsed = [
        sample["elapsed_seconds"]
        for sample in samples
    ]

    temperatures = [
        sample["temperature_c"]
        for sample in samples
    ]

    high_thresholds = [
        sample["high_threshold_c"]
        for sample in samples
    ]

    recovery_thresholds = [
        sample["recovery_threshold_c"]
        for sample in samples
    ]

    plt.figure(figsize=(11, 6))

    plt.plot(
        elapsed,
        temperatures,
        marker="o",
        markersize=3,
        label="CPU temperature",
    )

    plt.plot(
        elapsed,
        high_thresholds,
        linestyle="--",
        label="High threshold",
    )

    plt.plot(
        elapsed,
        recovery_thresholds,
        linestyle="--",
        label="Recovery threshold",
    )

    plt.xlabel("Elapsed time (seconds)")
    plt.ylabel("CPU temperature (C)")
    plt.title(
        "Adaptive thermal management temperature response"
    )
    plt.grid(True)
    plt.legend()
    plt.tight_layout()

    plt.savefig(
        TEMPERATURE_GRAPH,
        dpi=160,
    )

    plt.close()

    plt.figure(figsize=(11, 6))

    plt.step(
        elapsed,
        [
            sample["target_fps"]
            for sample in samples
        ],
        where="post",
        label="Target processing FPS",
    )

    plt.xlabel("Elapsed time (seconds)")
    plt.ylabel("Target FPS")
    plt.title(
        "Adaptive target FPS during thermal test"
    )
    plt.grid(True)
    plt.legend()
    plt.tight_layout()

    plt.savefig(
        FPS_GRAPH,
        dpi=160,
    )

    plt.close()

except Exception as error:
    print(
        "WARNING: Graph generation failed: "
        + str(error)
    )

baseline_temperatures = [
    sample["temperature_c"]
    for sample in samples
    if sample["phase"] == "baseline"
]

heating_temperatures = [
    sample["temperature_c"]
    for sample in samples
    if sample["phase"] == "heating"
]

recovery_temperatures = [
    sample["temperature_c"]
    for sample in samples
    if sample["phase"] == "recovery"
]

final_thermal = get_thermal()
final_pid = smart_guard_pid()
final_restarts = restart_count()

all_services_active = all(
    service_state(service) == "active"
    for service in SERVICE_NAMES
)

with SUMMARY_FILE.open(
    "w",
    encoding="utf-8",
) as output:
    def write(line=""):
        print(line)
        output.write(line + "\n")

    write()
    write(
        "========== TEST 4-4 FINAL SUMMARY =========="
    )

    write(
        "Thermal endpoint: /api/v1/thermal"
    )

    write(
        f"Samples recorded: {len(samples)}"
    )

    write(
        f"Initial Smart Guard PID: "
        f"{initial_pid}"
    )

    write(
        f"Final Smart Guard PID: "
        f"{final_pid}"
    )

    write(
        f"Initial restart count: "
        f"{initial_restarts}"
    )

    write(
        f"Final restart count: "
        f"{final_restarts}"
    )

    write()
    write(
        "========== TEMPERATURE RESULTS =========="
    )

    write(
        "Baseline average temperature: "
        f"{average(baseline_temperatures):.1f} C"
    )

    write(
        "Heating maximum temperature: "
        f"{max(heating_temperatures):.1f} C"
    )

    if recovery_temperatures:
        write(
            "Final recovery temperature: "
            f"{recovery_temperatures[-1]:.1f} C"
        )

    write(
        f"High threshold reached: "
        f"{'yes' if threshold_reached else 'no'}"
    )

    write(
        f"Adaptive action detected: "
        f"{'yes' if thermal_triggered else 'no'}"
    )

    write(
        f"Adaptive action reason: "
        f"{trigger_reason}"
    )

    if trigger_sample is not None:
        write(
            f"Target FPS during hot state: "
            f"{trigger_sample['target_fps']}"
        )

        write(
            f"Temperature at adaptive action: "
            f"{trigger_sample['temperature_c']:.1f} C"
        )

    write(
        f"Thermal recovery confirmed: "
        f"{'yes' if recovery_confirmed else 'no'}"
    )

    write(
        f"Recovery time: "
        f"{recovery_seconds} seconds"
    )

    write(
        f"Safety stop activated: "
        f"{'yes' if safety_stop else 'no'}"
    )

    if final_thermal is not None:
        write(
            f"Final thermal status: "
            f"{final_thermal['thermal_status']}"
        )

        write(
            f"Final target FPS: "
            f"{final_thermal['target_fps']}"
        )

        write(
            f"Final incident count: "
            f"{final_thermal['incident_count']}"
        )

    write()
    write(
        "========== TEST RESULT =========="
    )

    if threshold_reached:
        write(
            "PASS: CPU temperature reached "
            "the configured high threshold."
        )
    else:
        write(
            "INCOMPLETE: CPU temperature did not "
            "reach the high threshold."
        )

    if thermal_triggered:
        write(
            "PASS: Adaptive thermal action "
            "was observed."
        )
    else:
        write(
            "INCOMPLETE: Adaptive thermal action "
            "was not observed."
        )

    if recovery_confirmed:
        write(
            "PASS: Thermal recovery was confirmed."
        )
    else:
        write(
            "INCOMPLETE: Thermal recovery "
            "was not confirmed."
        )

    if all_services_active:
        write(
            "PASS: All project services are active."
        )
    else:
        write(
            "FAIL: One or more project services "
            "are not active."
        )

print()
print(
    f"CSV: {CSV_FILE}"
)

print(
    f"Temperature graph: "
    f"{TEMPERATURE_GRAPH}"
)

print(
    f"Target FPS graph: "
    f"{FPS_GRAPH}"
)

print(
    f"Summary: {SUMMARY_FILE}"
)

if not all_services_active:
    sys.exit(1)

if not threshold_reached:
    sys.exit(2)

if not thermal_triggered:
    sys.exit(2)

if not recovery_confirmed:
    sys.exit(3)

sys.exit(0)
