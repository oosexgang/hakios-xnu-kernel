#!/bin/bash
# One XNU run on the X6826B: build mtkloader (extra args go to the compiler, e.g.
# -DXNU_CHECKPOINT=17), pack it with Android and the XNU payload, flash boot_a, let XNU run and
# Android come back, then print what XNU left in pstore. Slot b is never touched.
set -e
S="${HAKIOS_SERIAL:?set HAKIOS_SERIAL to your phone adb serial}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${XNU_LOG_OUT:-$ROOT/logs/xnu-last.log}
# test settings: XNU picked by default, watchdog and watcher on, XNU comes back by itself
# XNU_SECS: how long the kernel runs before it hands the log back (the watcher waits a bit longer)
SECS=${XNU_SECS:-45}
# XNU_ARGS: the kernel's own command line (cores and so on); the clock comes from this machine
XNU_ARGS=${XNU_ARGS:-cpus=1}
# the device tree offers the kernel exactly this many cores; "cpus=" in XNU_ARGS should match
CORES=${XNU_CORES:-$(expr "$XNU_ARGS" : '.*cpus=\([0-9]*\)' || echo 1)}
[ -n "$CORES" ] || CORES=1
WATCH=${XNU_WATCH:-$((SECS + 240))}
# No MENU_DEFAULT_XNU: the image boots Android, and XNU is asked for once (tools/boot-xnu.sh), so
# a kernel that hangs costs a power cycle and not a rescue through BROM.
TEST_FLAGS=(-DXNU_WDT_SECONDS=0 -DXNU_CORES=$CORES -DXNU_WATCH_SECONDS=$WATCH
	"-DXNU_RETURN_AFTER=\"$SECS\"" "-DXNU_EXTRA_ARGS=\"$XNU_ARGS epoch=$(date +%s)\"")
"$ROOT/mtkloader/build.sh" "${TEST_FLAGS[@]}" "$@" >/dev/null
python3 "$ROOT/mtkloader/build_flashed.py" >/dev/null
[ "$(adb -s $S get-state 2>/dev/null)" = device ] && adb -s $S reboot bootloader
for i in $(seq 1 120); do fastboot devices | grep -q $S && break; sleep 1; done
# (fastboot -s waits for ever for a phone that is not there: a run would hang for hours)
fastboot devices | grep -q $S || { echo "the phone did not come up in fastboot"; exit 1; }
[ "$(fastboot -s $S getvar current-slot 2>&1 | sed -n 's/^current-slot: //p')" = a ] || { echo "current slot is not a"; exit 1; }
fastboot -s $S flash boot_a "$ROOT/xnu-flashed-slot_a.img" >/dev/null 2>&1
fastboot -s $S reboot >/dev/null 2>&1
echo "waiting for Android, to ask it for one XNU boot..."
for i in $(seq 1 100); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
sleep 4
"$ROOT/tools/boot-xnu.sh"
echo "XNU is running; Android comes back by itself"
WAIT_ITERS=$(( (SECS + 300) / 3 ))
for i in $(seq 1 $WAIT_ITERS); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
sleep 3
for i in $(seq 1 10); do adb -s $S shell 'su -c "cat /sys/fs/pstore/console-ramoops-0"' > "$OUT" 2>/dev/null && [ -s "$OUT" ] && break; sleep 2; done
# what the kernel kept for the host (kern.redwood.host_keep: the pipeline recipes, whole however long the run was),
# which mtkloader put into the pmsg zone: appended to the log, when it is this run's and not Android's own
adb -s $S shell 'su -c "cat /sys/fs/pstore/pmsg-ramoops-0"' > "$OUT.keep" 2>/dev/null || true
if head -c 64 "$OUT.keep" | grep -q 'kept for the host by the kernel'; then
	{ echo; cat "$OUT.keep"; } >> "$OUT"
	echo "kept for the host: $(head -1 "$OUT.keep" | sed 's/^=== kept for the host by the kernel: //; s/ ===$//')"
fi
rm -f "$OUT.keep"
echo "log: $OUT ($(wc -c < "$OUT") bytes)"
