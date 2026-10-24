#!/usr/bin/env bash

set -Eeuo pipefail

PROJECT_DIR="${SMART_GUARD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
OUTPUT_DIR="$PROJECT_DIR/docs/tests/section7/test_7-1_unauthorized_ssh"
FINAL_FILE="$OUTPUT_DIR/unauthorized_ssh_test.txt"
LOG_FILE="$OUTPUT_DIR/ssh_security_logs.txt"

INVALID_USERNAME="unauthorized_user_device-001"
INVALID_PASSWORD="invalid_password_device-001"
SSH_HOST="127.0.0.1"
SSH_PORT="22"

cd "$PROJECT_DIR"

mkdir -p "$OUTPUT_DIR"

sudo -v

if ! command -v ssh >/dev/null 2>&1
then
    echo "ERROR: SSH client is not installed."
    exit 1
fi

if ! command -v sshpass >/dev/null 2>&1
then
    echo "ERROR: sshpass is not installed."
    exit 1
fi

if ! dpkg-query -W -f='${Status}' openssh-server 2>/dev/null |
    grep -Fq 'install ok installed'
then
    echo "ERROR: openssh-server is not installed."
    exit 1
fi

INITIAL_SSH_STATE="$(
    systemctl is-active \
        ssh.service \
        2>/dev/null \
        || true
)"

restore_initial_state()
{
    if [ "$INITIAL_SSH_STATE" != "active" ]
    then
        sudo systemctl stop ssh.service \
            >/dev/null 2>&1 \
            || true
    fi
}

trap restore_initial_state EXIT INT TERM

echo "========== TEST 7-1: UNAUTHORIZED SSH LOGIN =========="
echo "Initial SSH service state: $INITIAL_SSH_STATE"
echo

echo "========== PREPARING SSH SERVICE =========="

sudo systemctl start ssh.service

sleep 3

SSH_STATE="$(
    systemctl is-active \
        ssh.service \
        2>/dev/null \
        || true
)"

echo "SSH service state during test: $SSH_STATE"

if [ "$SSH_STATE" != "active" ]
then
    echo "ERROR: ssh.service is not active."
    exit 1
fi

if ss -ltn |
    awk '{print $4}' |
    grep -Eq '(^|:)22$'
then
    echo "PASS: SSH port 22 is listening."
else
    echo "ERROR: SSH port 22 is not listening."
    exit 1
fi

TEST_START="$(
    date '+%Y-%m-%d %H:%M:%S'
)"

echo
echo "========== UNAUTHORIZED LOGIN ATTEMPT =========="
echo "Target: $INVALID_USERNAME@$SSH_HOST"
echo "Port: $SSH_PORT"
echo "Password: intentionally invalid"
echo

set +e

SSH_OUTPUT="$(
    timeout 15 \
        sshpass \
        -p "$INVALID_PASSWORD" \
        ssh \
        -p "$SSH_PORT" \
        -o ConnectTimeout=5 \
        -o NumberOfPasswordPrompts=1 \
        -o PreferredAuthentications=password \
        -o PubkeyAuthentication=no \
        -o KbdInteractiveAuthentication=no \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR \
        "$INVALID_USERNAME@$SSH_HOST" \
        'echo "UNAUTHORIZED LOGIN SUCCEEDED"' \
        2>&1
)"

SSH_EXIT_CODE=$?

set -e

printf '%s\n' "$SSH_OUTPUT"
echo "Unauthorized SSH attempt exit code: $SSH_EXIT_CODE"

sleep 2

sudo journalctl \
    -u ssh.service \
    --since "$TEST_START" \
    --no-pager \
    > "$LOG_FILE" \
    2>&1 \
    || true

LOGIN_REJECTED=0
SUCCESS_TEXT_FOUND=0

if printf '%s\n' "$SSH_OUTPUT" |
    grep -Eqi \
    'permission denied|authentication failed|access denied|connection closed'
then
    LOGIN_REJECTED=1
fi

if printf '%s\n' "$SSH_OUTPUT" |
    grep -q \
    'UNAUTHORIZED LOGIN SUCCEEDED'
then
    SUCCESS_TEXT_FOUND=1
fi

{
    echo "========== TEST 7-1 FINAL EVIDENCE =========="
    echo
    echo "Target host: $SSH_HOST"
    echo "Target port: $SSH_PORT"
    echo "Test username: $INVALID_USERNAME"
    echo "Password used: intentionally invalid"

    echo
    echo "========== SSH CONNECTION OUTPUT =========="

    printf '%s\n' "$SSH_OUTPUT"

    echo "SSH attempt exit code: $SSH_EXIT_CODE"

    echo
    echo "========== SSH SECURITY LOGS =========="

    grep -Ei \
        'invalid user|failed password|authentication failure|connection closed' \
        "$LOG_FILE" \
        || echo "No matching security-log line was returned."

    echo
    echo "========== SERVICE STATE =========="

    echo "ssh.service: $SSH_STATE"

    echo
    echo "========== TEST RESULT =========="

    if [ "$LOGIN_REJECTED" -eq 1 ] &&
       [ "$SSH_EXIT_CODE" -ne 0 ] &&
       [ "$SUCCESS_TEXT_FOUND" -eq 0 ]
    then
        echo "PASS: Unauthorized SSH login was rejected."
    else
        echo "FAIL: Unauthorized SSH rejection was not confirmed."
    fi

    if [ "$SSH_STATE" = "active" ]
    then
        echo "PASS: SSH service was active during the test."
    else
        echo "FAIL: SSH service was not active during the test."
    fi

} | tee "$FINAL_FILE"

if [ "$LOGIN_REJECTED" -ne 1 ] ||
   [ "$SSH_EXIT_CODE" -eq 0 ] ||
   [ "$SUCCESS_TEXT_FOUND" -ne 0 ] ||
   [ "$SSH_STATE" != "active" ]
then
    exit 1
fi

echo
echo "PASS: Test 7-1 completed successfully."
echo "Final evidence: $FINAL_FILE"
