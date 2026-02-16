#!/usr/bin/env bash
set -euo pipefail

IP="$1"
FILE="$HOME/.ssh/lab-pi.forwards"

usage() {
  echo "Usage: $0 <device_ip> [<port_number>]"
  echo "  - If port_number omitted: adds ALL ports (49151, 49152, 4443)"
  echo "  - If specified: must be 49151, 49152, or 4443"
  exit 1
}

# ====== DEFAULT BEHAVIOR ======
# Accept 1 OR 2 arguments (was: exactly 2)
[[ $# -eq 1 || $# -eq 2 ]] || usage

# If port arg missing, process all 3 ports. Else use provided port.
if [[ $# -eq 1 ]]; then
  PORTS=("49151" "49152" "4443")
else
  PORTS=("$2")
fi
# ====== DEF BEHAVIOR END ======

# Validate IP
if ! [[ "$IP" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
  echo "Invalid IP: $IP" >&2
  exit 2
fi

OCTET="${IP##*.}"
if (( OCTET < 1 || OCTET > 254 )); then
  echo "Invalid last octet: $OCTET" >&2
  exit 2
fi

# Process EACH port in PORTS array (preserves ALL original logic per port)
for PORT in "${PORTS[@]}"; do
  # ====== ORIGINAL PORT VALIDATION LOGIC ======
  case "$PORT" in
    49151) BASE=50000 ;;
    49152) BASE=51000 ;;
    4443) BASE=60000 ;;
    *) 
      echo "Port must be one of: 49151, 49152, 4443" >&2
      exit 2
      ;;
  esac
  # ====== END ORIGINAL LOGIC ======

  LPORT=$((BASE + OCTET))
  LINE=" LocalForward ${LPORT} ${IP}:${PORT}"

  # Ensure include file exists with Host stanza
  if [[ ! -f "$FILE" ]]; then
    printf "Host lab-pi\n" > "$FILE"  # FIXED: Added \n for valid SSH config (critical fix)
  fi

  # Add only if not already present
  if grep -qxF "$LINE" "$FILE"; then
    echo "Already present: localhost:${LPORT} -> ${IP}:${PORT}"
  else
    echo "$LINE" >> "$FILE"
    echo "Added: localhost:${LPORT} -> ${IP}:${PORT}"
  fi
done