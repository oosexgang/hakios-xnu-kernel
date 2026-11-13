#!/bin/bash
# The kernel's own tests (XNU's POST, osfmk/tests: zalloc, os_log, the console, thread calls, turnstiles,
# the VM, the arm64 platform's own) run in QEMU before the first process: boot-arg kernPOST=1. Each says
# PASS or FAIL as it goes; none may fail, and the first process is reached after them.
#
#   workbench/qemu_post_test.sh KERNEL
#
# Left out (exit 0, saying so) where there is no kernel, no QEMU, or no ld64.lld to build the root with.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
KERNEL=${1:-}
L=${XNU_CLANG_DIR:-$(dirname "$(command -v clang 2> /dev/null || echo /nonexistent/clang)")}
if [ -z "$KERNEL" ] || [ ! -f "$KERNEL" ] || ! command -v qemu-system-aarch64 > /dev/null || [ ! -x "$L/ld64.lld" ]; then
	echo "qemu_post_test: no kernel, QEMU or ld64.lld here: left out"
	exit 0
fi
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
XNU_CLANG_DIR=$L "$HERE/../tools/qemu-root.sh" "$W/root.img" > /dev/null || { echo "FAIL the root does not build"; exit 1; }
"$HERE/../tools/qemu-kernel.sh" --cpus 2 --ramdisk "$W/root.img" --boot-args 'rd=md0 kernPOST=1' \
	--until 'a shell on the console|panic\(' --seconds 600 --log "$W/serial.log" "$KERNEL" > "$W/out" 2>&1
tr -d '\r' < "$W/serial.log" > "$W/log"
tests=$(grep -c '^\[TEST\] TOC#' "$W/log")
passed=$(grep -c '^\[KTEST\]	PASS' "$W/log")
failed=$(grep -c '^\[KTEST\]	FAIL' "$W/log")
reached=$(grep -c 'a shell on the console' "$W/log")
echo "qemu_post_test: $tests of the kernel's tests, $passed checks passed, $failed failed"
grep '^\[KTEST\]	FAIL' "$W/log" | head -10 | sed 's/^/    /'
if [ "$tests" -lt 20 ] || [ "$passed" -lt 1000 ] || [ "$failed" != 0 ] || [ "$reached" = 0 ] || grep -aq 'panic(' "$W/log"; then
	echo "FAIL the kernel's own tests (tests listed: $tests, the first process reached: $reached)"
	tail -15 "$W/log" | sed 's/^/    /'
	exit 1
fi
echo "ok   the kernel's own tests pass, and the first process starts after them"
