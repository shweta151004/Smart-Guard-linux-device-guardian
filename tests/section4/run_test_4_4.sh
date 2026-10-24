#!/usr/bin/env bash

set -uo pipefail

PROJECT_DIR="${SMART_GUARD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
OUTPUT_DIR="$PROJECT_DIR/docs/tests/section4/test_4-4_thermal_management"
TERMINAL_FILE="$OUTPUT_DIR/test_4-4_terminal.txt"

cd "$PROJECT_DIR" || exit 1

mkdir -p "$OUTPUT_DIR"

command -v stress-ng >/dev/null 2>&1 || {
    echo "ERROR: stress-ng is not installed."
    exit 1
}

echo "========== TEST 4-4: ADAPTIVE THERMAL MANAGEMENT =========="

echo
echo "The test will run automatically."
echo "No keyboard input is required."
echo
echo "Before running, keep the Smart Guard dashboard open."
echo "Take the initial screenshot when it shows NORMAL and 30 FPS."
echo
echo "When the terminal prints:"
echo "TAKE THE HOT-STATE SCREENSHOT NOW"
echo "take the hot-state dashboard screenshot."
echo
echo "When the terminal prints:"
echo "TAKE THE RECOVERY SCREENSHOT NOW"
echo "take the recovered dashboard screenshot."
echo

python3 -u \
    tests/section4/run_test_4_4.py \
    2>&1 |
    tee "$TERMINAL_FILE"

TEST_EXIT_CODE="${PIPESTATUS[0]}"

echo
echo "Test 4-4 script exit code: $TEST_EXIT_CODE"
echo "The terminal remained open."

exit "$TEST_EXIT_CODE"
