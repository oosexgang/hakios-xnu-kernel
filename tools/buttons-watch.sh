#!/bin/bash
# While a run is in progress on the phone: follow what the kernel's log says about the buttons
# (the driver's "buttons:" lines, backboardd's "[Button]" lines, button presses a script makes
# with rwtouch), print each new line, and take pictures of the screen after every press -- one at
# once and one two seconds later -- so that a session in which somebody presses the phone's keys
# also shows what iOS made of them (a volume key: the volume indicator; the side button: the
# screen off, or back on).
#
# usage: XL_SECS=N tools/buttons-watch.sh [DIR]   (N as for the run; pictures go to DIR, default
#                                                  logs/buttons-HHMMSS)
# Start the run first (./xl run in another terminal, or in the background); this ends with it.
# In the last seconds of the run it leaves the phone alone: what "xl log" and "xl screenshot" ask
# the loader for travels the same way as the run's own log, and must not be in its place when
# the run fetches that.
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
PHONE=${XDG_RUNTIME_DIR:-/tmp}/darwin-redwood-phone
out=${1:-logs/buttons-$(date +%H%M%S)}
mkdir -p "$out"
pattern='^buttons: |\[Button\] [A-Za-z]+ page|^\$ /redwood/bin/rwtouch .*button |Set Blanked'
press='PRESSED|rwtouch .*button '
shots=0 started= again=0 last=
running() { ! flock -n "$PHONE.run" true 2>/dev/null; }
picture() { # picture <suffix>
	local file
	file="$out/$(printf '%02d' $shots)-$name$1.png"
	./xl screenshot "$file" >/dev/null 2>&1 && echo "    (picture $(date +%T): $file)"
}
for ((i = 0; i < 20; i++)); do          # wait for the run to start
	running && { started=$(stat -c %Y "$PHONE.run"); break; }
	sleep 0.5
done
[ -n "$started" ] || { echo "buttons-watch.sh: no run in progress"; exit 1; }
quiet=$((started + ${XL_SECS:-8} - 4))
while running; do
	if [ "$(date +%s)" -ge "$quiet" ]; then sleep 1; continue; fi
	./xl log > "$out/log.now" 2>/dev/null || { sleep 1; continue; }
	# what is new: the lines after the last one already shown (the log is a ring: lines do not keep their numbers)
	new=$(grep -a -E "$pattern" "$out/log.now" | awk -v last="$last" '
		{ line[NR] = $0; if ($0 == last) at = NR }
		END { if (last != "" && !at) at = NR - 1;       # (it has left the ring: go on from the newest)
		      for (i = at + 1; i <= NR; i++) print line[i] }')
	if [ -n "$new" ]; then
		last=$(printf '%s\n' "$new" | tail -1)
		printf '%s\n' "$new" | cut -c1-180
		if printf '%s\n' "$new" | grep -q -E "$press"; then
			shots=$((shots + 1))
			name=$(printf '%s\n' "$new" | grep -E "$press" | tail -1 |
				sed -E 's/^buttons: (.*) PRESSED.*/\1/; s/^.*button ([a-z]+).*/script-\1/; s/ /-/g')
			picture ""
			again=$(( $(date +%s) + 2 ))
		fi
	fi
	if [ "$again" != 0 ] && [ "$(date +%s)" -ge "$again" ]; then
		again=0
		picture "-later"
	fi
	sleep 0.2
done
echo "buttons-watch.sh: the run is over; pictures in $out, the whole log in logs/last.log"
