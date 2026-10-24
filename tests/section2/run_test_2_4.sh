#!/usr/bin/env bash

set -Eeuo pipefail

PROJECT_DIR="${SMART_GUARD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
OUTPUT_DIR="$PROJECT_DIR/docs/tests/section2/test_2-4_network"
SERVICE="smart-guard.service"

WIFI_DEVICE=""
WIFI_CONNECTION=""
OLD_IP=""
NEW_IP=""
WIFI_IS_DISCONNECTED=0

fail()
{
    echo
    echo "FAIL: $1"
    exit 1
}

http_code()
{
    url="$1"

    curl \
        -k \
        -sS \
        --connect-timeout 2 \
        --max-time 5 \
        -o /dev/null \
        -w '%{http_code}' \
        "$url" \
        2>/dev/null \
        || true
}

reconnect_wifi()
{
    if [ "$WIFI_IS_DISCONNECTED" -eq 1 ] &&
       [ -n "${WIFI_CONNECTION:-}" ] &&
       [ -n "${WIFI_DEVICE:-}" ]
    then
        echo
        echo "Restoring Wi-Fi connection..."

        sudo nmcli connection up \
            id "$WIFI_CONNECTION" \
            ifname "$WIFI_DEVICE" \
            || true

        WIFI_IS_DISCONNECTED=0
    fi
}

cleanup()
{
    exit_code=$?

    trap - EXIT INT TERM

    reconnect_wifi

    exit "$exit_code"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

cd "$PROJECT_DIR" ||
    fail "Project directory was not found."

mkdir -p "$OUTPUT_DIR"

sudo -v ||
    fail "Sudo authentication failed."

exec > >(
    tee "$OUTPUT_DIR/test_2-4_terminal.txt"
) 2>&1

echo "========== TEST 2-4: TWO-MINUTE NETWORK INTERRUPTION =========="
date --iso-8601=seconds

echo
echo "========== DETECTING ACTIVE WI-FI =========="

WIFI_DEVICE="$(
    nmcli \
        -t \
        -f DEVICE,TYPE,STATE \
        device status |
    awk -F: '
        $2 == "wifi" && $3 ~ /^connected/ {
            print $1
            exit
        }
    '
)"

[ -n "$WIFI_DEVICE" ] ||
    fail "No active Wi-Fi device was found."

WIFI_CONNECTION="$(
    nmcli \
        -g GENERAL.CONNECTION \
        device show "$WIFI_DEVICE"
)"

[ -n "$WIFI_CONNECTION" ] &&
[ "$WIFI_CONNECTION" != "--" ] ||
    fail "The active Wi-Fi connection name was not found."

OLD_IP="$(
    ip \
        -4 \
        -o \
        address show \
        dev "$WIFI_DEVICE" \
        scope global |
    awk '
        NR == 1 {
            split($4, address, "/")
            print address[1]
        }
    '
)"

[ -n "$OLD_IP" ] ||
    fail "The Wi-Fi IPv4 address was not found."

echo "Wi-Fi device: $WIFI_DEVICE"
echo "Wi-Fi connection: $WIFI_CONNECTION"
echo "Current LAN IP: $OLD_IP"
echo "Phone URL: https://${OLD_IP}:8443/"

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
    printf '%-38s %s\n' \
        "$service_name" \
        "$(
            systemctl is-active "$service_name" ||
            true
        )"
done

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

TEST_START="$(
    date '+%Y-%m-%d %H:%M:%S'
)"

LOCAL_CODE_BEFORE="$(
    http_code \
        "https://127.0.0.1:8443/"
)"

LAN_CODE_BEFORE="$(
    http_code \
        "https://${OLD_IP}:8443/"
)"

echo
echo "========== STATE BEFORE DISCONNECTION =========="
echo "Initial PID: $INITIAL_PID"
echo "Initial restart count: $INITIAL_RESTARTS"
echo "Local HTTPS status: $LOCAL_CODE_BEFORE"
echo "LAN HTTPS status: $LAN_CODE_BEFORE"

[ "$LOCAL_CODE_BEFORE" = "200" ] ||
    fail "The local HTTPS server is unavailable."

[ "$LAN_CODE_BEFORE" = "200" ] ||
    fail "The LAN HTTPS server is unavailable."

echo
echo "Open the following URL on the phone:"
echo "https://${OLD_IP}:8443/"
echo
echo "Confirm that the live stream is visible on the phone."
echo "Start recording or take the before-disconnection screenshot."
echo

read -r -p "Press Enter to disconnect Wi-Fi for 120 seconds..."

echo
echo "========== DISCONNECTING WI-FI =========="

OUTAGE_START_EPOCH="$(
    date +%s
)"

OUTAGE_START_TEXT="$(
    date --iso-8601=seconds
)"

sudo nmcli device disconnect "$WIFI_DEVICE"

WIFI_IS_DISCONNECTED=1

sleep 5

DEVICE_STATE_DURING="$(
    nmcli \
        -g GENERAL.STATE \
        device show "$WIFI_DEVICE" |
    head -n 1
)"

LOCAL_CODE_DURING="$(
    http_code \
        "https://127.0.0.1:8443/"
)"

LAN_CODE_DURING="$(
    http_code \
        "https://${OLD_IP}:8443/"
)"

PID_DURING="$(
    systemctl show \
        "$SERVICE" \
        -p MainPID \
        --value
)"

RESTARTS_DURING="$(
    systemctl show \
        "$SERVICE" \
        -p NRestarts \
        --value
)"

echo
echo "========== STATE DURING NETWORK OUTAGE =========="
echo "Wi-Fi state: $DEVICE_STATE_DURING"
echo "Smart Guard PID: $PID_DURING"
echo "Restart count: $RESTARTS_DURING"
echo "Local HTTPS status: $LOCAL_CODE_DURING"
echo "LAN HTTPS status: ${LAN_CODE_DURING:-000}"

echo
echo "========== SERVICE STATES DURING OUTAGE =========="

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
echo "Wi-Fi will remain disconnected for a total of 120 seconds."

while true
do
    current_epoch="$(
        date +%s
    )"

    elapsed_seconds=$((current_epoch - OUTAGE_START_EPOCH))

    if [ "$elapsed_seconds" -ge 120 ]; then
        break
    fi

    remaining_seconds=$((120 - elapsed_seconds))

    echo "Network outage remaining: ${remaining_seconds} seconds"

    if [ "$remaining_seconds" -gt 10 ]; then
        sleep 10
    else
        sleep "$remaining_seconds"
    fi
done

echo
echo "========== RECONNECTING WI-FI =========="

sudo nmcli connection up \
    id "$WIFI_CONNECTION" \
    ifname "$WIFI_DEVICE"

WIFI_IS_DISCONNECTED=0

for attempt in $(seq 1 30)
do
    NEW_IP="$(
        ip \
            -4 \
            -o \
            address show \
            dev "$WIFI_DEVICE" \
            scope global |
        awk '
            NR == 1 {
                split($4, address, "/")
                print address[1]
            }
        '
    )"

    if [ -n "$NEW_IP" ]; then
        break
    fi

    echo "Waiting for IPv4 address... attempt $attempt"
    sleep 2
done

[ -n "$NEW_IP" ] ||
    fail "Wi-Fi reconnected but no IPv4 address was assigned."

sleep 5

OUTAGE_END_TEXT="$(
    date --iso-8601=seconds
)"

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

LOCAL_CODE_AFTER="$(
    http_code \
        "https://127.0.0.1:8443/"
)"

LAN_CODE_AFTER="$(
    http_code \
        "https://${NEW_IP}:8443/"
)"

echo
echo "========== STATE AFTER RECONNECTION =========="
echo "Old LAN IP: $OLD_IP"
echo "New LAN IP: $NEW_IP"
echo "Initial PID: $INITIAL_PID"
echo "Final PID: $FINAL_PID"
echo "Initial restart count: $INITIAL_RESTARTS"
echo "Final restart count: $FINAL_RESTARTS"
echo "Local HTTPS status: $LOCAL_CODE_AFTER"
echo "LAN HTTPS status: $LAN_CODE_AFTER"
echo "Recovered phone URL: https://${NEW_IP}:8443/"

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
echo "========== NETWORKMANAGER LOGS =========="

sudo journalctl \
    -u NetworkManager \
    --since "$TEST_START" \
    --no-pager \
    > "$OUTPUT_DIR/networkmanager_journal.txt"

grep -Ei \
    'disconnect|disconnected|deactivat|activated|connected|state change' \
    "$OUTPUT_DIR/networkmanager_journal.txt" \
    | tail -n 40 \
    || true

echo
echo "========== SMART GUARD LOGS =========="

sudo journalctl \
    -u smart-guard.service \
    --since "$TEST_START" \
    --no-pager \
    > "$OUTPUT_DIR/smart_guard_journal.txt"

tail -n 30 \
    "$OUTPUT_DIR/smart_guard_journal.txt" \
    || true

echo
echo "========== TEST 2-4 RESULT =========="

echo "Outage started: $OUTAGE_START_TEXT"
echo "Outage ended: $OUTAGE_END_TEXT"

if [ "$LOCAL_CODE_DURING" = "200" ]; then
    echo "PASS: Smart Guard remained available locally during the outage."
else
    echo "WARNING: Local HTTPS was not available during the outage."
fi

if [ "$FINAL_PID" = "$INITIAL_PID" ]; then
    echo "PASS: Smart Guard MainPID remained unchanged."
else
    echo "WARNING: Smart Guard MainPID changed."
fi

if [ "$FINAL_RESTARTS" = "$INITIAL_RESTARTS" ]; then
    echo "PASS: Smart Guard service did not restart."
else
    echo "WARNING: Smart Guard restart count changed."
fi

if [ "$LOCAL_CODE_AFTER" = "200" ] &&
   [ "$LAN_CODE_AFTER" = "200" ]
then
    echo "PASS: Network access was successfully recovered."
else
    echo "FAIL: Network access did not recover correctly."
    exit 1
fi

echo "PASS: Wi-Fi was disconnected for approximately two minutes."
echo "PASS: NetworkManager and Smart Guard logs were saved."
echo "NetworkManager log: $OUTPUT_DIR/networkmanager_journal.txt"
echo "Smart Guard log: $OUTPUT_DIR/smart_guard_journal.txt"
echo "Terminal evidence: $OUTPUT_DIR/test_2-4_terminal.txt"
