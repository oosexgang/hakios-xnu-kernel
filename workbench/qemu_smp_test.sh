#!/bin/bash
# The kernel on four processors in QEMU: all four started through PSCI and running, the kernel saying so to
# programs; four threads of a program running side by side (on a computer with four processors of its own,
# taking well under the time of one doing it all); eight threads taking turns under one lock without losing
# a count; and nothing panicking. Then on one processor, where nothing is sent to another: threads and the
# lock all the same.
#
#   workbench/qemu_smp_test.sh KERNEL
#
# Left out (exit 0, saying so) where there is no kernel, no QEMU, or no ld64.lld to build the root with.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
KERNEL=${1:-}
L=${XNU_CLANG_DIR:-$(dirname "$(command -v clang 2> /dev/null || echo /nonexistent/clang)")}
if [ -z "$KERNEL" ] || [ ! -f "$KERNEL" ] || ! command -v qemu-system-aarch64 > /dev/null || [ ! -x "$L/ld64.lld" ]; then
	echo "qemu_smp_test: no kernel, QEMU or ld64.lld here: left out"
	exit 0
fi
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
XNU_CLANG_DIR=$L "$HERE/../tools/qemu-root.sh" "$W/root.img" > /dev/null || { echo "FAIL the root does not build"; exit 1; }
"$HERE/../tools/qemu-kernel.sh" --cpus 4 --ramdisk "$W/root.img" --boot-args 'rd=md0' \
	--until 'a shell on the console|panic\(' --seconds 600 --type 'sysctl hw.ncpu hw.activecpu\nthreads 4\nlocks 8\n' \
	--log "$W/serial.log" "$KERNEL" > "$W/out" 2>&1
tr -d '\r' < "$W/serial.log" > "$W/log"

failed=0
check() {
	local what=$1
	shift
	if "$@"; then
		echo "ok   $what"
	else
		echo "FAIL $what"
		failed=1
	fi
}
check "all four processors start and run" grep -q 'all started: 4 processors running' "$W/log"
check "the kernel says four to programs (hw.ncpu, hw.activecpu)" bash -c "grep -qx 'hw.ncpu: 4' '$W/log' && grep -qx 'hw.activecpu: 4' '$W/log'"
# (four threads side by side take well under the time of one doing it all -- where this computer has the four
# processors to run them on; on fewer, that they run at all)
if [ "$(nproc)" -ge 4 ]; then
	check "four threads run on four processors at once" bash -c "grep -oE '^threads: 4 threads on 4 processors did in [0-9]+ ms what one does in [0-9]+ ms' '$W/log' |
		awk '{ exit !(\$9 * 2 < \$15) }'"
else
	check "four threads run (this computer has fewer than four processors: how fast is not judged)" \
		grep -qE '^threads: 4 threads on 4 processors did in' "$W/log"
fi
check "eight threads take turns under one lock and lose no count" grep -qE '^locks: 8 threads added 1600000 of 1600000 under one lock' "$W/log"
check "no panic" bash -c "! grep -aq 'panic(' '$W/log'"

"$HERE/../tools/qemu-kernel.sh" --cpus 1 --ramdisk "$W/root.img" --boot-args 'rd=md0' \
	--until 'a shell on the console|panic\(' --seconds 600 --type 'sysctl hw.ncpu\nthreads 2\nlocks 4\n' \
	--log "$W/serial1.log" "$KERNEL" > "$W/out1" 2>&1
tr -d '\r' < "$W/serial1.log" > "$W/log1"
check "on one processor: it starts, and programs are told one" bash -c "grep -q 'all started: 1 processor running' '$W/log1' &&
	grep -qx 'hw.ncpu: 1' '$W/log1'"
check "on one processor: two threads take turns on it, and four under one lock lose no count" bash -c "
	grep -qE '^threads: 2 threads on 1 processor did in' '$W/log1' &&
	grep -qE '^locks: 4 threads added 800000 of 800000 under one lock' '$W/log1' && ! grep -aq 'panic(' '$W/log1'"
if [ $failed != 0 ]; then
	tail -15 "$W/log" | sed 's/^/    /'
	tail -15 "$W/log1" | sed 's/^/    /'
fi
exit $failed
