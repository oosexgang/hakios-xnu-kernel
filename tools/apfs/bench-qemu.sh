#!/bin/bash
# Readers at once over an APFS volume in QEMU, the way they are measured on the phone
# (boot-arg apfs_check=5): one line per run, so two kernels can be put side by side.
#
# The volume is the root device -- IMAGE becomes md0 -- and every reader walks all of it.
#
# usage: tools/apfs/bench-qemu.sh IMAGE "1 2 4 8" [extra boot-args]
# env:   QEMU_SMP (default 8) processors of the machine, and cpus=N to the kernel to match
#        BENCH_LOGS directory for the logs (default apfs-work)
#
# Runs only this working copy's QEMU instance (qemu/instance), and waits for its ports.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
IMAGE=$1
COUNTS=${2:-"1 2 4 8"}
EXTRA=${3:-}
SMP=${QEMU_SMP:-8}
LOGS=${BENCH_LOGS:-$ROOT/apfs-work}
INSTANCE=$(cat "$ROOT/qemu/instance" 2>/dev/null || echo 0)
PORT=$((4444 + 10 * INSTANCE))
cd "$ROOT" || exit 1
touch "$IMAGE"          # run-ios.sh rebuilds its RAM image only from a newer file

printf "%-7s %9s %11s %12s %12s %8s %s\n" readers wall_ms MB/s lock_wait_ms lock_held_ms "bad" sums
for n in $COUNTS; do
	for i in $(seq 1 60); do
		avail=$(awk '/^MemAvailable:/ { print int($2 / 1024) }' /proc/meminfo)
		ss -ltn 2>/dev/null | grep -q ":$PORT " || { [ "$avail" -ge 3300 ] && break; }
		sleep 5
	done
	log="$LOGS/bench-$(date +%H%M%S)-$n.log"
	QEMU_SMP=$SMP IMG="$IMAGE" ROOT=userland/root.apfs \
		CMDLINE="cpus=$SMP apfs_check=5 apfs_threads=$n $EXTRA" \
		timeout 900 qemu/run-ios.sh 600 > "$log" 2>&1
	wall=$(grep -ao "$n readers done in [0-9]*" "$log" | head -1 | awk '{print $5}')
	kbs=$(grep -ao "bytes  [0-9]* KB/s" "$log" | head -1 | awk '{print $2}')
	lock=$(grep -a "apfs: lock" "$log" | head -1 | sed 's/^\[[^]]*\]: //')
	wait=$(echo "$lock" | awk '{print $5}')
	held=$(echo "$lock" | awk '{print $8}')
	bad=$(grep -ao "readers that disagreed or failed: [0-9]*" "$log" | head -1 | awk '{print $6}')
	sums=$(grep -ao "sum [0-9a-f]\{16\}" "$log" | sort -u | awk '{print $2}' | tr '\n' ' ')
	printf "%-7s %9s %11s %12s %12s %8s %s\n" "$n" "${wall:-?}" \
		"$( [ -n "${kbs:-}" ] && echo "scale=1; $kbs / 1024" | bc || echo ?)" \
		"${wait:-?}" "${held:-?}" "${bad:-?}" "${sums:-?}"
done
