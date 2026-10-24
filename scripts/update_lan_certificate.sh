#!/usr/bin/env bash

set -Eeuo pipefail

PROJECT_DIR="${SMART_GUARD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
CERT_DIR="$PROJECT_DIR/certs"
CERT_FILE="$CERT_DIR/server.crt"
KEY_FILE="$CERT_DIR/server.key"

LAN_IP="$(hostname -I | awk '{print $1}')"
LOCAL_HOSTNAME="$(hostname)"
STAMP="$(date +%Y%m%d_%H%M%S)"
BACKUP_DIR="$CERT_DIR/backups/$STAMP"
CONFIG_FILE="$CERT_DIR/openssl_lan.cnf"

cd "$PROJECT_DIR"

sudo -v

if [ -z "$LAN_IP" ]; then
    echo "ERROR: LAN IP could not be detected."
    exit 1
fi

if [ ! -f "$CERT_FILE" ] || [ ! -f "$KEY_FILE" ]; then
    echo "ERROR: Existing certificate or private key was not found."
    exit 1
fi

mkdir -p "$BACKUP_DIR"

cp -a "$CERT_FILE" "$BACKUP_DIR/server.crt"
cp -a "$KEY_FILE" "$BACKUP_DIR/server.key"

rollback()
{
    echo
    echo "ERROR: Certificate update failed. Restoring previous files."

    cp -a "$BACKUP_DIR/server.crt" "$CERT_FILE"
    cp -a "$BACKUP_DIR/server.key" "$KEY_FILE"

    sudo systemctl restart smart-guard.service || true
    sudo systemctl restart smart-guard-api.service || true

    echo "Previous certificate restored."
}

trap rollback ERR

cat > "$CONFIG_FILE" <<CONFIG
[req]
prompt = no
distinguished_name = distinguished_name
x509_extensions = server_extensions

[distinguished_name]
C = IR
ST = Fars
L = Shiraz
O = Embedded Systems
OU = Smart Guard
CN = smart-guard.local

[server_extensions]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alternative_names

[alternative_names]
DNS.1 = smart-guard.local
DNS.2 = localhost
DNS.3 = ${LOCAL_HOSTNAME}
DNS.4 = ${LOCAL_HOSTNAME}.local
IP.1 = 127.0.0.1
IP.2 = ${LAN_IP}
CONFIG

echo "========== GENERATING CERTIFICATE =========="
echo "LAN IP: $LAN_IP"
echo "Hostname: $LOCAL_HOSTNAME"
echo "Backup directory: $BACKUP_DIR"

openssl req \
    -x509 \
    -nodes \
    -newkey rsa:2048 \
    -sha256 \
    -days 365 \
    -keyout "$CERT_DIR/server.key.new" \
    -out "$CERT_DIR/server.crt.new" \
    -config "$CONFIG_FILE"

openssl x509 \
    -in "$CERT_DIR/server.crt.new" \
    -noout \
    -subject \
    -dates \
    -ext subjectAltName

openssl pkey \
    -in "$CERT_DIR/server.key.new" \
    -check \
    -noout

mv "$CERT_DIR/server.crt.new" "$CERT_FILE"
mv "$CERT_DIR/server.key.new" "$KEY_FILE"

chmod 644 "$CERT_FILE"
chmod 600 "$KEY_FILE"

echo
echo "========== RESTARTING SERVICES =========="

sudo systemctl restart smart-guard.service
sudo systemctl restart smart-guard-api.service

sleep 5

systemctl is-active --quiet smart-guard.service
systemctl is-active --quiet smart-guard-api.service

echo "smart-guard.service: $(systemctl is-active smart-guard.service)"
echo "smart-guard-api.service: $(systemctl is-active smart-guard-api.service)"

echo
echo "========== FINAL CERTIFICATE =========="

echo |
openssl s_client \
    -connect "${LAN_IP}:8443" \
    -servername "${LAN_IP}" \
    2>/dev/null |
openssl x509 \
    -noout \
    -subject \
    -issuer \
    -dates \
    -ext subjectAltName

echo
echo "========== HTTPS CHECK =========="

curl \
    -k \
    -sS \
    -o /dev/null \
    -w 'HTTPS status code: %{http_code}\n' \
    "https://${LAN_IP}:8443/"

trap - ERR

echo
echo "========== RESULT =========="
echo "PASS: Certificate CN is smart-guard.local."
echo "PASS: LAN IP was added to Subject Alternative Name."
echo "PASS: HTTPS services restarted successfully."
echo "Phone URL: https://${LAN_IP}:8443/"
echo "Stream URL: https://${LAN_IP}:8443/api/v1/stream"
