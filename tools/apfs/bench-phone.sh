#!/bin/bash
# Readers at once over an APFS volume on the phone (boot-arg apfs_check=5), one line per run.
# The volume has to be the phone's resident image already (`./xl ramdisk IMAGE`); it stays
# there across runs as long as none of them hangs.
#
# usage: tools/apfs/bench-phone.sh "1 2 4 8" [extra boot-args]
# env:   BENCH_KERNEL  a kernel to put in place first (default: whatever is built)
#        BENCH_CPUS    cpus=N for the kernel (default 8)
#        BENCH_FREQ    cpufreq=N (default 1: every group of cores at its top speed). Without it the
#                      cores keep whatever speed the run before left them at -- the loader does not
#                      reset it -- and the same kernel measured 3.4 times slower one evening than
#                      the afternoon before.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
COUNTS=${1:-"1 2 4 8"}
EXTRA=${2:-}
CPUS=${BENCH_CPUS:-8}
FREQ=${BENCH_FREQ:-1}
cd "$ROOT" || exit 1
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
[ -n "${BENCH_KERNEL:-}" ] && cp -p "$BENCH_KERNEL" "$K"

printf "%-7s %9s %8s %12s %12s %10s %8s %4s %s\n" readers wall_ms MB/s lock_wait_ms lock_held_ms inflate_ms hash_ms bad sums
for n in $COUNTS; do
	out=$(XL_CMDLINE="cpus=$CPUS cpufreq=$FREQ apfs_check=5 apfs_threads=$n $EXTRA" XL_SECS=300 \
		timeout 500 ./xl run 2>&1)
	log=$(echo "$out" | grep -o 'logs/[0-9]*-run.log' | tail -1)
	if [ -z "$log" ] || ! grep -aq "readers done in" "$log" 2>/dev/null; then
		printf "%-7s run did not come back: %s\n" "$n" "$(echo "$out" | tail -1)"
		break
	fi
	wall=$(grep -ao "$n readers done in [0-9]*" "$log" | head -1 | awk '{print $5}')
	kbs=$(grep -ao "bytes  [0-9]* KB/s" "$log" | head -1 | awk '{print $2}')
	lock=$(grep -a "apfs: lock" "$log" | head -1 | sed 's/^\[[^]]*\]: //')
	infl=$(grep -a "apfs: inflate" "$log" | head -1 | sed 's/^\[[^]]*\]: //' | awk '{print $5}')
	# the checker's own FNV time, all readers together ("off" with apfs_hash=0)
	hash=$(grep -ao "ms  hash [a-z ]*[0-9]* ms" "$log" | awk '{ if ($3 == "off") off = 1; t += $(NF-1) }
		END { if (NR == 0) print "-"; else if (off) print "off"; else print t }')
	bad=$(grep -ao "readers that disagreed or failed: [0-9]*" "$log" | head -1 | awk '{print $6}')
	sums=$(grep -ao "sum [0-9a-f]\{16\}" "$log" | sort -u | awk '{print $2}' | tr '\n' ' ')
	freq=$(grep -ao "cpufreq: on core [0-9]* a loop ran at [0-9]*, now at [0-9]*" "$log" | head -1 |
		awk '{print $9 $12}' | tr ',' '>')
	printf "%-7s %9s %8s %12s %12s %10s %8s %4s %s  [%s %s]\n" "$n" "$wall" \
		"$(echo "scale=1; $kbs / 1024" | bc)" "$(echo "$lock" | awk '{print $5}')" \
		"$(echo "$lock" | awk '{print $8}')" "$infl" "$hash" "$bad" "$sums" "$log" "${freq:-freq?}"
done
