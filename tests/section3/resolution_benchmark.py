#!/usr/bin/env python3

import argparse
import csv
import time
from pathlib import Path

import cv2


def read_cpu_temperature() -> float:
    hwmon_root = Path("/sys/class/hwmon")

    for hwmon in hwmon_root.glob("hwmon*"):
        try:
            name = (hwmon / "name").read_text().strip().lower()
        except OSError:
            continue

        if name != "coretemp":
            continue

        for label_path in hwmon.glob("temp*_label"):
            try:
                label = label_path.read_text().strip().lower()
            except OSError:
                continue

            if "package id 0" not in label:
                continue

            input_path = label_path.with_name(
                label_path.name.replace("_label", "_input")
            )

            try:
                return float(input_path.read_text().strip()) / 1000.0
            except (OSError, ValueError):
                continue

    for zone in Path("/sys/class/thermal").glob("thermal_zone*"):
        try:
            zone_type = (zone / "type").read_text().strip().lower()
            raw_value = int((zone / "temp").read_text().strip())
        except (OSError, ValueError):
            continue

        if "x86_pkg_temp" in zone_type or "cpu" in zone_type:
            return raw_value / 1000.0

    return float("nan")


def read_rss_mb() -> float:
    with open("/proc/self/status", encoding="utf-8") as handle:
        for line in handle:
            if line.startswith("VmRSS:"):
                return float(line.split()[1]) / 1024.0

    return float("nan")


parser = argparse.ArgumentParser()

parser.add_argument(
    "--width",
    type=int,
    required=True,
)

parser.add_argument(
    "--height",
    type=int,
    required=True,
)

parser.add_argument(
    "--duration",
    type=int,
    default=300,
)

args = parser.parse_args()

label = f"{args.width}x{args.height}"

output_dir = Path(
    "docs/tests/section3/test_3-3_resolution"
)

output_dir.mkdir(
    parents=True,
    exist_ok=True,
)

model_config = Path(
    "image_processing/models/"
    "MobileNetSSD_deploy.prototxt"
)

model_weights = Path(
    "image_processing/models/"
    "MobileNetSSD_deploy.caffemodel"
)

if not model_config.exists():
    raise SystemExit(
        f"ERROR: Model configuration not found: {model_config}"
    )

if not model_weights.exists():
    raise SystemExit(
        f"ERROR: Model weights not found: {model_weights}"
    )

network = cv2.dnn.readNetFromCaffe(
    str(model_config),
    str(model_weights),
)

camera = cv2.VideoCapture(
    "/dev/video0",
    cv2.CAP_V4L2,
)

camera.set(
    cv2.CAP_PROP_FOURCC,
    cv2.VideoWriter_fourcc(*"MJPG"),
)

camera.set(
    cv2.CAP_PROP_FRAME_WIDTH,
    args.width,
)

camera.set(
    cv2.CAP_PROP_FRAME_HEIGHT,
    args.height,
)

camera.set(
    cv2.CAP_PROP_FPS,
    30,
)

time.sleep(2)

if not camera.isOpened():
    raise SystemExit(
        "ERROR: Camera could not be opened."
    )

actual_width = int(
    camera.get(cv2.CAP_PROP_FRAME_WIDTH)
)

actual_height = int(
    camera.get(cv2.CAP_PROP_FRAME_HEIGHT)
)

actual_fps_setting = camera.get(
    cv2.CAP_PROP_FPS
)

print("========== RESOLUTION BENCHMARK ==========")
print(f"Requested resolution: {label}")
print(
    "Actual camera resolution: "
    f"{actual_width}x{actual_height}"
)
print(
    "Camera FPS setting: "
    f"{actual_fps_setting:.2f}"
)
print(f"Duration: {args.duration} seconds")
print("Person confidence threshold: 0.35")

if (
    actual_width != args.width
    or actual_height != args.height
):
    camera.release()

    raise SystemExit(
        "ERROR: Camera did not accept the requested resolution."
    )

start_time = time.monotonic()
interval_start = start_time

interval_frames = 0
processed_frames = 0
detected_frames = 0
failed_reads = 0

rows = []
sample_saved = False
interrupted = False

window_title = f"Resolution benchmark {label}"

cv2.namedWindow(
    window_title,
    cv2.WINDOW_NORMAL,
)

try:
    while True:
        current_time = time.monotonic()
        elapsed = current_time - start_time

        if elapsed >= args.duration:
            break

        success, frame = camera.read()

        if not success:
            failed_reads += 1
            time.sleep(0.01)
            continue

        blob = cv2.dnn.blobFromImage(
            frame,
            scalefactor=0.007843,
            size=(300, 300),
            mean=127.5,
        )

        network.setInput(blob)
        detections = network.forward()

        persons = 0

        for index in range(
            detections.shape[2]
        ):
            confidence = float(
                detections[0, 0, index, 2]
            )

            class_id = int(
                detections[0, 0, index, 1]
            )

            if (
                class_id != 15
                or confidence < 0.35
            ):
                continue

            persons += 1

            box = (
                detections[0, 0, index, 3:7]
                * [
                    args.width,
                    args.height,
                    args.width,
                    args.height,
                ]
            )

            left, top, right, bottom = (
                box.astype(int)
            )

            left = max(
                0,
                min(left, args.width - 1),
            )

            top = max(
                0,
                min(top, args.height - 1),
            )

            right = max(
                0,
                min(right, args.width - 1),
            )

            bottom = max(
                0,
                min(bottom, args.height - 1),
            )

            cv2.rectangle(
                frame,
                (left, top),
                (right, bottom),
                (0, 255, 0),
                2,
            )

            cv2.putText(
                frame,
                f"Person {confidence:.2f}",
                (left, max(20, top - 8)),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.55,
                (0, 255, 0),
                2,
            )

        processed_frames += 1
        interval_frames += 1

        if persons >= 1:
            detected_frames += 1

        current_time = time.monotonic()
        elapsed = current_time - start_time

        cv2.putText(
            frame,
            f"Resolution: {label}",
            (10, 25),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.65,
            (255, 255, 255),
            2,
        )

        cv2.putText(
            frame,
            f"Persons: {persons}",
            (10, 52),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.65,
            (255, 255, 255),
            2,
        )

        if (
            not sample_saved
            and elapsed >= 10
        ):
            sample_path = (
                output_dir
                / f"{label}_sample.jpg"
            )

            cv2.imwrite(
                str(sample_path),
                frame,
            )

            sample_saved = True

            print(
                f"Representative image saved: "
                f"{sample_path}"
            )

        if current_time - interval_start >= 5:
            interval_duration = (
                current_time
                - interval_start
            )

            interval_fps = (
                interval_frames
                / interval_duration
            )

            row = {
                "resolution": label,
                "elapsed_seconds": round(
                    elapsed,
                    2,
                ),
                "fps": round(
                    interval_fps,
                    3,
                ),
                "cpu_temperature_c": round(
                    read_cpu_temperature(),
                    2,
                ),
                "rss_mb": round(
                    read_rss_mb(),
                    3,
                ),
                "persons": persons,
            }

            rows.append(row)
            print(row, flush=True)

            interval_start = current_time
            interval_frames = 0

        cv2.imshow(
            window_title,
            frame,
        )

        key = cv2.waitKey(1) & 0xFF

        if key == ord("q"):
            interrupted = True
            break

finally:
    camera.release()
    cv2.destroyAllWindows()

elapsed_total = (
    time.monotonic()
    - start_time
)

if interrupted:
    raise SystemExit(
        "ERROR: Test was interrupted with the q key."
    )

if elapsed_total < args.duration - 2:
    raise SystemExit(
        "ERROR: Test did not run for the full duration."
    )

if not rows or processed_frames == 0:
    raise SystemExit(
        "ERROR: No valid benchmark data was collected."
    )

average_fps = (
    processed_frames
    / elapsed_total
)

accuracy = (
    detected_frames
    / processed_frames
    * 100.0
)

final_temperature = (
    read_cpu_temperature()
)

rss_values = [
    row["rss_mb"]
    for row in rows
]

timeseries_path = (
    output_dir
    / f"{label}_timeseries.csv"
)

with timeseries_path.open(
    "w",
    newline="",
    encoding="utf-8",
) as handle:
    writer = csv.DictWriter(
        handle,
        fieldnames=[
            "resolution",
            "elapsed_seconds",
            "fps",
            "cpu_temperature_c",
            "rss_mb",
            "persons",
        ],
    )

    writer.writeheader()
    writer.writerows(rows)

summary = {
    "resolution": label,
    "actual_capture": (
        f"{actual_width}x{actual_height}"
    ),
    "duration_seconds": round(
        elapsed_total,
        2,
    ),
    "average_fps": round(
        average_fps,
        3,
    ),
    "final_cpu_temperature_c": round(
        final_temperature,
        2,
    ),
    "average_rss_mb": round(
        sum(rss_values) / len(rss_values),
        3,
    ),
    "maximum_rss_mb": round(
        max(rss_values),
        3,
    ),
    "detection_accuracy_percent": round(
        accuracy,
        3,
    ),
    "processed_frames": processed_frames,
    "detected_frames": detected_frames,
    "failed_camera_reads": failed_reads,
}

summary_path = (
    output_dir
    / f"{label}_summary.csv"
)

with summary_path.open(
    "w",
    newline="",
    encoding="utf-8",
) as handle:
    writer = csv.DictWriter(
        handle,
        fieldnames=summary.keys(),
    )

    writer.writeheader()
    writer.writerow(summary)

print()
print("========== FINAL RESULT ==========")

for key, value in summary.items():
    print(f"{key}: {value}")

print(f"Timeseries: {timeseries_path}")
print(f"Summary: {summary_path}")
print("PASS: Five-minute resolution test completed.")
