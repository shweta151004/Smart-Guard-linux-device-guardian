#!/usr/bin/env python3

import csv
import json
import os
import re
import statistics
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from typing import Any, Optional

MEASUREMENT_COUNT = 10
STUDENT_ID = "device-001"

OUTPUT_DIR = Path(
    "docs/tests/section5/test_5-1_latency"
)

CSV_PATH = OUTPUT_DIR / "latency_measurements.csv"
SUMMARY_PATH = OUTPUT_DIR / "latency_summary.txt"
GRAPH_PATH = OUTPUT_DIR / "latency_measurements.png"

COUNT_KEYS = (
    "persons",
    "person_count",
    "detected_persons",
    "count",
)

TIMESTAMP_KEYS = (
    "timestamp",
    "event_timestamp",
    "detection_timestamp",
    "detected_at",
    "event_time",
    "time",
)


def find_value(
    data: Any,
    keys: tuple[str, ...],
) -> Optional[Any]:
    if isinstance(data, dict):
        for key in keys:
            if key in data:
                return data[key]

        for value in data.values():
            result = find_value(value, keys)

            if result is not None:
                return result

    if isinstance(data, list):
        for item in data:
            result = find_value(item, keys)

            if result is not None:
                return result

    return None


def parse_timestamp(value: Any) -> datetime:
    text = str(value).strip()

    if text.endswith("Z"):
        text = text[:-1] + "+00:00"

    if re.search(r"[+-]\d{4}$", text):
        text = text[:-5] + text[-5:-2] + ":" + text[-2:]

    parsed = datetime.fromisoformat(text)

    if parsed.tzinfo is None:
        parsed = parsed.replace(
            tzinfo=datetime.now().astimezone().tzinfo
        )

    return parsed.astimezone()


def parse_message(
    line: str,
) -> tuple[str, dict[str, Any]]:
    stripped = line.strip()

    if " " not in stripped:
        raise ValueError(
            f"Unexpected MQTT output: {stripped}"
        )

    topic, payload_text = stripped.split(
        " ",
        1,
    )

    payload = json.loads(payload_text)

    if not isinstance(payload, dict):
        raise ValueError(
            "MQTT payload is not a JSON object."
        )

    return topic, payload


def build_subscriber_command() -> list[str]:
    host = os.environ.get(
        "MQTT_HOST",
        "127.0.0.1",
    )

    port = os.environ.get(
        "MQTT_PORT",
        "1883",
    )

    username = os.environ.get(
        "MQTT_USERNAME",
        "",
    )

    password = os.environ.get(
        "MQTT_PASSWORD",
        "",
    )

    topic = (
        os.environ.get("MQTT_PERSONS_TOPIC")
        or os.environ.get("MQTT_PERSON_TOPIC")
        or os.environ.get("MQTT_TOPIC_PERSONS")
        or f"persons/{STUDENT_ID}/home"
    )

    command = [
        "stdbuf",
        "-oL",
        "mosquitto_sub",
        "-h",
        host,
        "-p",
        str(port),
        "-t",
        topic,
        "-v",
        "-R",
        "-V",
        "mqttv311",
    ]

    if username:
        command.extend([
            "-u",
            username,
        ])

    if password:
        command.extend([
            "-P",
            password,
        ])

    print("========== MQTT SUBSCRIBER SETTINGS ==========")
    print(f"Broker: {host}:{port}")
    print(f"Persons topic: {topic}")
    print("Password output: hidden")
    print()

    return command


def save_results(
    measurements: list[dict[str, Any]],
) -> None:
    latencies = [
        float(row["latency_ms"])
        for row in measurements
    ]

    mean_latency = statistics.mean(
        latencies
    )

    sample_stddev = statistics.stdev(
        latencies
    )

    population_stddev = statistics.pstdev(
        latencies
    )

    median_latency = statistics.median(
        latencies
    )

    with CSV_PATH.open(
        "w",
        newline="",
        encoding="utf-8",
    ) as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "measurement",
                "topic",
                "persons",
                "event_timestamp",
                "receive_timestamp",
                "latency_ms",
            ],
        )

        writer.writeheader()
        writer.writerows(measurements)

    summary_lines = [
        "========== TEST 5-1 LATENCY SUMMARY ==========",
        f"Measurements: {len(latencies)}",
        f"Mean latency (ms): {mean_latency:.3f}",
        (
            "Sample standard deviation (ms): "
            f"{sample_stddev:.3f}"
        ),
        (
            "Population standard deviation (ms): "
            f"{population_stddev:.3f}"
        ),
        f"Minimum latency (ms): {min(latencies):.3f}",
        f"Maximum latency (ms): {max(latencies):.3f}",
        f"Median latency (ms): {median_latency:.3f}",
        (
            "PASS: Ten end-to-end MQTT latency "
            "measurements were completed."
        ),
    ]

    SUMMARY_PATH.write_text(
        "\n".join(summary_lines) + "\n",
        encoding="utf-8",
    )

    print()
    print("\n".join(summary_lines))

    try:
        import matplotlib.pyplot as plt

        measurement_numbers = list(
            range(1, len(latencies) + 1)
        )

        figure, axis = plt.subplots(
            figsize=(10, 5),
        )

        axis.bar(
            measurement_numbers,
            latencies,
        )

        axis.axhline(
            mean_latency,
            linestyle="--",
            label=(
                f"Mean = {mean_latency:.3f} ms"
            ),
        )

        axis.set_title(
            "End-to-end MQTT latency for 10 person entries"
        )

        axis.set_xlabel(
            "Measurement number"
        )

        axis.set_ylabel(
            "Latency (ms)"
        )

        axis.set_xticks(
            measurement_numbers
        )

        axis.grid(
            axis="y",
            alpha=0.3,
        )

        axis.legend()
        figure.tight_layout()

        figure.savefig(
            GRAPH_PATH,
            dpi=160,
        )

        plt.close(figure)

        print(f"Graph: {GRAPH_PATH}")

    except Exception as error:
        print(
            "WARNING: Graph could not be generated: "
            f"{error}"
        )

    print(f"CSV: {CSV_PATH}")
    print(f"Summary: {SUMMARY_PATH}")


def main() -> int:
    OUTPUT_DIR.mkdir(
        parents=True,
        exist_ok=True,
    )

    subscriber_command = (
        build_subscriber_command()
    )

    process = subprocess.Popen(
        subscriber_command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )

    if process.stdout is None:
        print(
            "ERROR: Subscriber output is unavailable."
        )
        return 1

    measurements: list[dict[str, Any]] = []

    armed = False
    zero_message_count = 0
    last_valid_payload = ""

    print(
        "Keep every real person completely outside "
        "the camera frame."
    )

    print(
        "The test will become ready after two "
        "consecutive persons=0 messages."
    )

    print()

    try:
        while (
            len(measurements)
            < MEASUREMENT_COUNT
        ):
            line = process.stdout.readline()

            if line == "":
                if process.poll() is not None:
                    error_output = ""

                    if process.stderr is not None:
                        error_output = (
                            process.stderr.read()
                        )

                    print(
                        "ERROR: MQTT subscriber stopped."
                    )

                    if error_output:
                        print(error_output.strip())

                    return 1

                continue

            receive_time = (
                datetime.now().astimezone()
            )

            try:
                topic, payload = parse_message(
                    line
                )
            except (
                ValueError,
                json.JSONDecodeError,
            ):
                print(
                    f"IGNORED: {line.strip()}"
                )
                continue

            last_valid_payload = json.dumps(
                payload,
                ensure_ascii=False,
            )

            person_value = find_value(
                payload,
                COUNT_KEYS,
            )

            event_timestamp_value = find_value(
                payload,
                TIMESTAMP_KEYS,
            )

            if person_value is None:
                print(
                    "IGNORED: No supported person-count "
                    f"field in payload: {last_valid_payload}"
                )
                continue

            try:
                person_count = int(
                    float(person_value)
                )
            except (
                TypeError,
                ValueError,
            ):
                print(
                    "IGNORED: Invalid person count: "
                    f"{person_value}"
                )
                continue

            if person_count == 0:
                zero_message_count += 1

                if (
                    not armed
                    and zero_message_count >= 2
                ):
                    measurement_number = (
                        len(measurements) + 1
                    )

                    armed = True

                    print()
                    print(
                        "========================================"
                    )

                    print(
                        f"READY {measurement_number}/"
                        f"{MEASUREMENT_COUNT}"
                    )

                    print(
                        "Enter the camera frame now and "
                        "remain visible until RECORDED appears."
                    )

                    print(
                        "========================================"
                    )

                continue

            zero_message_count = 0

            if not armed:
                continue

            if event_timestamp_value is None:
                print(
                    "ERROR: Person message does not contain "
                    "a timestamp field."
                )

                print(
                    f"Payload: {last_valid_payload}"
                )

                return 1

            try:
                event_time = parse_timestamp(
                    event_timestamp_value
                )
            except (
                TypeError,
                ValueError,
            ) as error:
                print(
                    "ERROR: Event timestamp could not be "
                    f"parsed: {error}"
                )

                print(
                    f"Payload: {last_valid_payload}"
                )

                return 1

            latency_ms = (
                receive_time - event_time
            ).total_seconds() * 1000.0

            measurement_number = (
                len(measurements) + 1
            )

            measurement = {
                "measurement": measurement_number,
                "topic": topic,
                "persons": person_count,
                "event_timestamp": (
                    event_time.isoformat(
                        timespec="microseconds"
                    )
                ),
                "receive_timestamp": (
                    receive_time.isoformat(
                        timespec="microseconds"
                    )
                ),
                "latency_ms": (
                    f"{latency_ms:.3f}"
                ),
            }

            measurements.append(
                measurement
            )

            armed = False

            print()
            print(
                f"RECORDED {measurement_number}/"
                f"{MEASUREMENT_COUNT}"
            )

            print(
                f"Event timestamp: "
                f"{measurement['event_timestamp']}"
            )

            print(
                f"Receive timestamp: "
                f"{measurement['receive_timestamp']}"
            )

            print(
                f"End-to-end latency: "
                f"{latency_ms:.3f} ms"
            )

            if (
                len(measurements)
                < MEASUREMENT_COUNT
            ):
                print(
                    "Leave the camera frame completely."
                )

                print(
                    "Wait until the next READY message."
                )

        save_results(
            measurements
        )

        return 0

    except KeyboardInterrupt:
        print()
        print(
            "ERROR: Test was interrupted."
        )

        return 130

    finally:
        process.terminate()

        try:
            process.wait(
                timeout=5,
            )
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


if __name__ == "__main__":
    sys.exit(main())
