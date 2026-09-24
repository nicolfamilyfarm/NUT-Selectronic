#!/bin/bash
set -euo pipefail

: "${SELECTRONIC_IP:?Set SELECTRONIC_IP to the address of the Selectronic device}"
: "${SELECTRONIC_DEVICE_ID:?Set SELECTRONIC_DEVICE_ID}"

SELECTRONIC_MODEL="${SELECTRONIC_MODEL:-SP PRO}"
METADATA_URL="http://${SELECTRONIC_IP}/cgi-bin/solarmonweb/devices/${SELECTRONIC_DEVICE_ID}"
metadata=$(curl --fail --silent --show-error --max-time 5 "$METADATA_URL")
SELECTRONIC_SERIAL=$(jq -er '.serialnum' <<<"$metadata")
SELECTRONIC_INVERTER_CAPACITY_KW=$(jq -er '(.power_rating | tonumber) / 1000' <<<"$metadata")
SELECTRONIC_FIRMWARE=$(jq -er '.firmware' <<<"$metadata")
SELECTRONIC_MODEL="${SELECTRONIC_MODEL} (${SELECTRONIC_FIRMWARE})"
export SELECTRONIC_SERIAL SELECTRONIC_INVERTER_CAPACITY_KW SELECTRONIC_MODEL

mkdir -p /run/nut /var/run/nut
chown nut:nut /run/nut /var/run/nut

cat > /etc/nut/ups.conf <<EOF
[selectronic]
    driver = dummy-ups
    port = /etc/nut/selectronic.dev
    mode = dummy-once
    desc = "Selectronic ${SELECTRONIC_MODEL}"
    pollinterval = 5
EOF

cat > /etc/nut/upsd.conf <<'EOF'
LISTEN 0.0.0.0 3493
EOF

cat > /etc/nut/upsd.users <<EOF
[${NUT_USER:-nutmon}]
    password = ${NUT_PASSWORD:-change-me}
    upsmon primary
EOF
chmod 0640 /etc/nut/upsd.users
chown root:nut /etc/nut/upsd.users

cat > /etc/nut/nut.conf <<'EOF'
MODE=netserver
EOF

cat > /etc/nut/selectronic.dev <<EOF
ups.mfr: Selectronic
ups.model: ${SELECTRONIC_MODEL}
ups.status: OB LB
battery.charge: 0
battery.runtime: 0
battery.voltage: 0
ups.load: 0
ups.loadhigh: 90
device.mfr: Selectronic
device.model: ${SELECTRONIC_MODEL}
EOF
chown nut:nut /etc/nut/selectronic.dev

poller_loop() {
    while true; do
        if ! /usr/local/sbin/selectronic-nut-poller; then
            echo "Selectronic poll failed; retaining last data" >&2
        fi
        sleep "${POLL_INTERVAL:-5}"
    done
}

poller_loop &
POLLER_PID=$!
sleep 1
upsdrvctl start selectronic
upsd -F &
UPSD_PID=$!

cleanup() {
    kill "$UPSD_PID" "$POLLER_PID" 2>/dev/null || true
    upsdrvctl stop selectronic 2>/dev/null || true
}
trap cleanup EXIT INT TERM
wait "$UPSD_PID"
