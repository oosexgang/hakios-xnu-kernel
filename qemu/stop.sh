#!/bin/bash
# Stop the QEMU of this working copy (and only it): "quit" on its monitor.
# The instance is QEMU_INSTANCE, or the number in qemu/instance, or 0 (see run-ios.sh).
here=$(dirname "$(readlink -f "$0")")
port=$((4444 + 10 * ${QEMU_INSTANCE:-$(cat "$here/instance" 2>/dev/null || echo 0)}))
python3 - $port <<'PY'
import socket, sys, time
try:
    s = socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=2)
except OSError:
    sys.exit(0)                         # nothing is running there
s.sendall(b'\nquit\n'); time.sleep(0.5)
PY
