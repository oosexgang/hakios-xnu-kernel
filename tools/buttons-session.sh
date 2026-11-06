#!/bin/bash
# A session for somebody with a hand on the phone's keys: one run of iOS with the buttons' driver
# on (boot-arg buttons=1), in which everything the driver sees and everything iOS makes of it is
# collected, and a summary at the end.
#
#   tools/buttons-session.sh [SECONDS]        (default 120)
#
# What it does: gets the phone into the loader (uploads the system image if it had to come from
# Android), starts the run, says when to start pressing, follows the log (tools/buttons-watch.sh:
# every press and release as the kernel's driver logs it, backboardd's own "[Button]" lines, a
# picture of the screen after each press), and afterwards fetches the screen recording the run
# made and prints what was seen. Everything goes to logs/buttons-HHMMSS/.
#
# The small image must be one that unlocks by itself (the keys are then tried at the home
# screen): REST=1 ZSTD=1 RC_JOBS=rc.jobs.unlock userland/build.sh
# Environment: XL_CMDLINE_MORE="..." more boot-args (gpu=1, -v, ...); RAMDISK=1 upload the system
# image even if the phone is already in the loader; RECORD=0 no screen recording.
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
PHONE=${XDG_RUNTIME_DIR:-/tmp}/darwin-redwood-phone
secs=${1:-120}
out=logs/buttons-$(date +%H%M%S)
mkdir -p "$out"

flock -n "$PHONE.run" true 2>/dev/null || { echo "buttons-session.sh: a run is in progress (started from $(cat "$PHONE.who" 2>/dev/null)): not now"; exit 1; }
grep -q swipe userland/root/etc/rc.jobs 2>/dev/null ||
	echo "buttons-session.sh: note: the small image was not built with RC_JOBS=rc.jobs.unlock: nothing will swipe the lock screen away"
before=$(./xl state)
echo "buttons-session.sh: the phone is in: $before"
./xl enter || exit 1
if [ "$before" != loader ] || [ -n "$RAMDISK" ]; then
	echo "buttons-session.sh: uploading the system image (about 70 s)"
	./xl ramdisk || exit 1
fi

record="rec_start=30 rec_secs=$((secs - 45)) rec_mb=640"
[ "$RECORD" = 0 ] && record=
export XL_SECS=$secs
XL_CMDLINE="touchscreen=1 cpufreq=1 bootcpu=7 cpus=4 buttons=1 $record $XL_CMDLINE_MORE" ./xl run > "$out/run.out" 2>&1 &
echo
echo "=== $(date +%T): the run has started; it lasts $secs seconds."
echo "=== In about 20 s the lock screen appears, at about 35 s it is swiped away by a script."
echo "=== FROM THE MOMENT THE HOME SCREEN (the icons) IS THERE the keys can be pressed."
echo
sleep 2
tools/buttons-watch.sh "$out" | grep --line-buffered -v -E '^buttons: (step [1-4]|next:|channel|PMIC arbiter|the PMK8350|GPIO6 of)'
wait
cat "$out/run.out"
log=$(sed -n 's/.*\[\(logs\/[^]]*\)\].*/\1/p' "$out/run.out" | tail -1)
[ -f "$log" ] || { echo "buttons-session.sh: the run left no log"; exit 1; }
cp "$log" "$out/run.log"

if [ -n "$record" ] && ./xl recording "$out/recording.mp4" >/dev/null 2>&1; then
	echo "screen recording: $out/recording.mp4"
fi

echo
echo "=== what the driver saw (the kernel's log, $out/run.log) ==="
grep -a -E '^buttons: (power-on block|volume up.s pin|step 5|.*(PRESSED|released at)|so far|reading|a key reads|more than half)' "$out/run.log" | cut -c1-230
echo
echo "=== what iOS's event system made of it (backboardd) ==="
grep -a -E '\[Button\] [A-Za-z]+ page|Set Blanked' "$out/run.log" | sed -E 's/^[0-9-]+ //; s/\+0[0-9]+ backboardd\[[0-9:]+\]//' | cut -c1-120
echo
for key in power "volume up" "volume down"; do
	printf '%-12s %s presses, %s releases seen by the driver\n' "$key:" \
		"$(grep -a -c "^buttons: $key PRESSED" "$out/run.log")" "$(grep -a -c "^buttons: $key released at" "$out/run.log")"
done
printf 'backboardd:  %s lock, %s volume-up, %s volume-down button events (a press and a release are two)\n' \
	"$(grep -a -c '\[Button\] Lock page' "$out/run.log")" "$(grep -a -c '\[Button\] VolumeIncrement page' "$out/run.log")" \
	"$(grep -a -c '\[Button\] VolumeDecrement page' "$out/run.log")"
echo "pictures taken after presses: $(ls "$out"/*.png 2>/dev/null | wc -l) in $out"
