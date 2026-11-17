#!/bin/bash
# A day with the phone, run for you: the runs of Documentation/phone-test.ru.md in order, each through ./bench, each
# log looked at (tools/logcheck.py), the day's logs and a summary kept together in logs/day-TIME/ -- the folder to send
# back. It stops at the first run that did not go (a part STOPPED, a panic, no kernel), unless told to go on.
# Before the first, scripts/phone-ready.py looks at what the runs need of this machine (the kernel and the loader
# built and current, the loader knowing the phone, the identity, the small image): a day does not begin without them.
#
#   scripts/phone-day.sh [--keep-going] [--no-check] [RUN...]
#
# RUN: defconfigs of arch/arm64/configs, phone_first phone_look phone_ufs_check phone_perf phone_metal phone_bt when
# none is named. ./bench must already have the phone (./bench enter, ./bench ramdisk once): nothing is written to the
# phone's storage, as with every ./bench run. BENCH=PATH runs another bench, READY=PATH another check (a test's
# stand-ins); --no-check: none.
set -uo pipefail
TOP=$(cd "$(dirname "$0")/.." && pwd)
cd "$TOP" || exit 1
BENCH=${BENCH:-./bench}
READY=${READY:-scripts/phone-ready.py}
keep_going=''
check=1
runs=()
for argument in "$@"; do
	case $argument in
	--keep-going) keep_going=1 ;;
	--no-check) check='' ;;
	-*) echo "phone-day.sh: unknown option $argument" >&2; exit 2 ;;
	*) runs+=("$argument") ;;
	esac
done
[ ${#runs[@]} -gt 0 ] || runs=(phone_first phone_look phone_ufs_check phone_perf phone_metal phone_bt)
for run in "${runs[@]}"; do
	tools/defconfig.py "$run" > /dev/null || exit 2
done
day=logs/day-$(date +%Y%m%d-%H%M%S)
mkdir -p "$day"
summary=$day/summary.md
{
	echo "# A day with the phone, $(date '+%Y-%m-%d %H:%M')"
	echo
	echo "Commit: $(git log -1 --oneline 2> /dev/null || echo unknown). Runs: ${runs[*]}."
	echo
} > "$summary"
if [ -n "$check" ]; then
	echo "== what the runs need ($READY)"
	{
		echo "## Before"
		echo
		echo '```'
	} >> "$summary"
	"$READY" "${runs[@]}" 2>&1 | tee -a "$summary"
	ready=${PIPESTATUS[0]}
	printf '```\n\n' >> "$summary"
	if [ "$ready" != 0 ]; then
		echo "phone-day.sh: not ready (above): no run started. --no-check to begin all the same"
		exit 2
	fi
fi
status=0
previous=''
first=1
for run in "${runs[@]}"; do
	echo "== $run"
	# one kernel a session of the loader: a second has died at once every time it was tried (./bench fresh restarts it,
	# and sends the resident image again, when a kernel has run in it)
	if [ -z "$first" ] && ! "$BENCH" fresh 2>&1 | tee -a "$day/$run.bench"; then
		{
			echo "## $run"
			echo
			echo '```'
			echo "not run: the loader could not be started anew (the bench's own words are in $run.bench)"
			echo '```'
		} | tee -a "$summary"
		status=1
		break
	fi
	first=''
	before=$(readlink logs/last.log 2> /dev/null || true)
	touch "$day/.$run.began"
	BENCH_DEFCONFIG=$run "$BENCH" run 2>&1 | tee -a "$day/$run.bench"
	ran=${PIPESTATUS[0]}
	after=$(readlink logs/last.log 2> /dev/null || true)
	# what the kernel said while it ran, fetched as it went (all there is of a run that stopped answering)
	for live in logs/*.log.live; do
		[ -f "$live" ] && [ "$live" -nt "$day/.$run.began" ] && cp "$live" "$day/$run.live.log"
	done
	rm -f "$day/.$run.began"
	{
		echo "## $run"
		echo
		echo '```'
	} >> "$summary"
	if [ -z "$after" ] || [ "$after" = "$before" ]; then
		echo "no log: the run did not come back (the bench's own words are in $run.bench)" | tee -a "$summary"
		[ -f "$day/$run.live.log" ] && echo "what it said while it ran: $run.live.log" | tee -a "$summary"
		echo '```' >> "$summary"
		status=1
		[ -n "$keep_going" ] && continue
		break
	fi
	cp "logs/$after" "$day/$run.log"
	tools/logcheck.py "$day/$run.log" | tail -n +2 | tee -a "$summary"
	checked=${PIPESTATUS[0]}
	if [ "$ran" != 0 ]; then
		# (a kernel that hung on its core leaves a log the loader fetched, and ./bench says so and fails: not a run that went)
		echo "./bench run did not end well (exit $ran): $(grep -h 'bench: ' "$day/$run.bench" | tail -1)" | tee -a "$summary"
		checked=1
	fi
	if [ -n "$previous" ]; then
		echo "-- since $previous:" >> "$summary"
		tools/logcheck.py --against "$day/$previous.log" "$day/$run.log" | tail -n +2 >> "$summary"
	fi
	echo '```' >> "$summary"
	echo >> "$summary"
	previous=$run
	if [ "$checked" != 0 ]; then
		status=1
		echo "phone-day.sh: $run did not go all the way (above)"
		[ -n "$keep_going" ] || break
	fi
done
echo "phone-day.sh: $day: each run's log, what ./bench said, and summary.md -- the folder to send back"
exit $status
