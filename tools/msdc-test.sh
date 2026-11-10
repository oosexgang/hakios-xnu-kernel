#!/bin/bash
# Flash the eMMC read test to boot_a, run it, and print what it found (it chains Android after).
set -e
S="${HAKIOS_SERIAL:?set HAKIOS_SERIAL to your phone adb serial}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/mtkloader/build.sh" -DMSDC_TEST=1 "$@" >/dev/null
python3 "$ROOT/mtkloader/build_flashed.py" >/dev/null
[ "$(adb -s $S get-state 2>/dev/null)" = device ] && adb -s $S reboot bootloader
for i in $(seq 1 60); do fastboot devices | grep -q $S && break; sleep 1; done
# (fastboot -s waits for ever for a phone that is not there)
fastboot devices | grep -q $S || { echo "the phone did not come up in fastboot"; exit 1; }
[ "$(fastboot -s $S getvar current-slot 2>&1 | sed -n 's/^current-slot: //p')" = a ] || { echo "current slot is not a"; exit 1; }
fastboot -s $S flash boot_a "$ROOT/xnu-flashed-slot_a.img" >/dev/null 2>&1
fastboot -s $S reboot >/dev/null 2>&1
for i in $(seq 1 90); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
sleep 3
adb -s $S shell 'su -c "cat /sys/fs/pstore/console-ramoops-0"' > "$ROOT/logs/msdc-last.log" 2>/dev/null
sed -n '/eMMC read test/,/^$/p' "$ROOT/logs/msdc-last.log"
