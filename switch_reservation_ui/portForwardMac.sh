#!/usr/bin/env bash
set -euo pipefail

IP="$1"
PORT="$2"   # must be 49151, 49152, or 4443
FILE="$HOME/.ssh/lab-pi.forwards"

usage() {
  echo "Usage: $0 <device_ip> <49151|49152|4443>"
  exit 1
}

[[ $# -eq 2 ]] || usage

# Validate IP
if ! [[ "$IP" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
  echo "Invalid IP: $IP"
  exit 2
fi

OCTET="${IP##*.}"
if (( OCTET < 1 || OCTET > 254 )); then
  echo "Invalid last octet: $OCTET"
  exit 2
fi

# Choose base port
case "$PORT" in
  49151) BASE=50000 ;;
  49152) BASE=51000 ;;
  4443)  BASE=60000 ;;
  *) echo "Port must be one of: 49151, 49152, 4443"; exit 2 ;;
esac

LPORT=$((BASE + OCTET))
LINE="    LocalForward ${LPORT} ${IP}:${PORT}"

# Ensure the include file exists with Host stanza
if [[ ! -f "$FILE" ]]; then
  printf "Host lab-pi\n" > "$FILE"
fi

# Add only if not already present
if grep -qxF "$LINE" "$FILE"; then
  echo "Already present: localhost:${LPORT} -> ${IP}:${PORT}"
else
  echo "$LINE" >> "$FILE"
  echo "Added: localhost:${LPORT} -> ${IP}:${PORT}"
fi
