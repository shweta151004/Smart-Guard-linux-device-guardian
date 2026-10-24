#!/usr/bin/env bash

set -Eeuo pipefail

PROJECT_DIR="${SMART_GUARD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
OUTPUT_DIR="$PROJECT_DIR/docs/tests/section4/test_4-2_black_box"

cd "$PROJECT_DIR"

mkdir -p "$OUTPUT_DIR"

echo "========== TEST 4-2: BLACK BOX DATABASE =========="

python3 \
    tests/section4/inspect_black_box_db.py \
    | tee \
    "$OUTPUT_DIR/test_4-2_terminal.txt"

echo
echo "PASS: Test 4-2 database inspection finished."
