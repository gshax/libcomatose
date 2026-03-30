#!/bin/bash
#
# ht818-exec.sh - execute a command on the HT818 via telnet
#
# usage: ./ht818-exec.sh <command>
#
# environment:
#   HT818_HOST  (default: 10.70.3.108)
#   HT818_PORT  (default: 6969)
#   HT818_USER  (default: root)
#   HT818_PASS  (default: alpine)
#

HOST="${HT818_HOST:-10.70.3.108}"
PORT="${HT818_PORT:-6969}"
USER="${HT818_USER:-root}"
PASS="${HT818_PASS:-alpine}"

CMD="$*"

if [ -z "$CMD" ]; then
    echo "usage: $0 <command>" >&2
    exit 1
fi

MARKER="COMATOSE_MARKER_$$"

(
    sleep 1
    echo "$USER"
    sleep 1
    echo "$PASS"
    sleep 1
    echo "echo ${MARKER}_BEGIN"
    sleep 0.3
    echo "$CMD"
    sleep 5
    echo "echo ${MARKER}_END"
    sleep 0.5
    echo "exit"
) | socat - "TCP:${HOST}:${PORT},connect-timeout=10" 2>/dev/null | \
    tr -d '\r' | \
    awk "/${MARKER}_BEGIN/{found=1; next} /${MARKER}_END/{found=0} found" | \
    grep -v '^root@HT818'
