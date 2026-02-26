#!/usr/bin/env bash
set -euo pipefail
IP="$1"
# Define list of files to update
FILES=(
    "$HOME/.ssh/lab-pi.forwards"
    "$HOME/.ssh/lab-nic01.forwards"
)
usage() {
    echo "Usage: $0 <device_ip> [<port_number>]"
    echo "  - If port_number omitted: adds ALL ports (49151, 49152, 4443)"
    echo "  - If specified: must be 49151, 49152, or 4443"
    exit 1
}
# ====== DEFAULT BEHAVIOR ======
# Accept 1 OR 2 arguments
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
# Process EACH port in PORTS array
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
    
    # Process EACH file in FILES array
    for FILE in "${FILES[@]}"; do
        # 1. Extract the filename from the full path (e.g., lab-nuc01.forwards)
        filename=$(basename "$FILE")
        
        # 2. Remove the extension to get the Host name (e.g., lab-nuc01)
        # ${var%pattern} removes the shortest match of pattern from the end
        host_name="${filename%.forwards}"
        
        # Define the line to check/add
        LINE=" LocalForward ${LPORT} ${IP}:${PORT}"

        # Ensure directory exists
        mkdir -p "$(dirname "$FILE")"
        
        # Ensure file exists with dynamic Host stanza
        if [[ ! -f "$FILE" ]]; then
            # Creates "Host lab-nuc01" or "Host lab-pi" dynamically
            printf "Host %s\n" "$host_name" > "$FILE"
        fi
        
        # Add only if not already present
        if grep -qxF "$LINE" "$FILE"; then
            echo "Already present in ${FILE}: localhost:${LPORT} -> ${IP}:${PORT}"
        else
            echo "$LINE" >> "$FILE"
            echo "Added to ${FILE}: localhost:${LPORT} -> ${IP}:${PORT}"
        fi
    done
done