#!/bin/bash
# Flash the camera probe (read only: pins, PMIC rails, I2C controller) to boot_a, ask for it once, and print what it found. The image starts Android
# unless Android left the request in ramoops' pmsg zone before rebooting, and the loader wipes the
# request as it takes it: a probe that hangs the phone costs one reboot, after which it is Android
# again. Slot b is never touched.
set -e
S="${HAKIOS_SERIAL:?set HAKIOS_SERIAL to your phone adb serial}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/mtkloader/build.sh" -DCAM_PROBE=1 "$@" >/dev/null
python3 "$ROOT/mtkloader/build_flashed.py" >/dev/null
[ "$(adb -s $S get-state 2>/dev/null)" = device ] && adb -s $S reboot bootloader
for i in $(seq 1 60); do fastboot devices | grep -q $S && break; sleep 1; done
[ "$(fastboot -s $S getvar current-slot 2>&1 | sed -n 's/^current-slot: //p')" = a ] || { echo "current slot is not a"; exit 1; }
fastboot -s $S flash boot_a "$ROOT/xnu-flashed-slot_a.img" >/dev/null 2>&1
fastboot -s $S reboot >/dev/null 2>&1
echo "waiting for Android, to ask it for the probe..."
for i in $(seq 1 100); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
sleep 5
adb -s $S shell 'su -c "printf DARWIN-REDWOOD:CAM-PROBE > /dev/pmsg0"'
adb -s $S reboot
sleep 15
for i in $(seq 1 100); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
sleep 3
adb -s $S shell 'su -c "cat /sys/fs/pstore/console-ramoops-0"' > "$ROOT/logs/cam-probe.log" 2>/dev/null
sed -n '/CAM probe ==/,/CAM probe done/p' "$ROOT/logs/cam-probe.log"
