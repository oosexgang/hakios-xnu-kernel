#!/bin/bash
# A day with the phone, run for you: the runs of Documentation/phone-test.ru.md in order, each through ./bench, each
# log looked at (tools/logcheck.py), the day's logs and a summary kept together in logs/day-TIME/ -- the folder to send
# back. It stops at the first run that did not go (a part STOPPED, a panic, no kernel), unless told to go on.
#
#   scripts/phone-day.sh [--keep-going] [RUN...]
#
# RUN: defconfigs of arch/arm64/configs, phone_first phone_look phone_ufs_check phone_perf phone_metal phone_bt when
# none is named. ./bench must already have the phone (./bench enter, ./bench ramdisk once): nothing is written to the
# phone's storage, as with every ./bench run. BENCH=PATH runs another bench (a test's stand-in).
set -uo pipefail
TOP=$(cd "$(dirname "$0")/.." && pwd)
cd "$TOP" || exit 1
BENCH=${BENCH:-./bench}
keep_going=''
runs=()
for argument in "$@"; do
	case $argument in
	--keep-going) keep_going=1 ;;
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
status=0
previous=''
for run in "${runs[@]}"; do
	echo "== $run"
	before=$(readlink logs/last.log 2> /dev/null || true)
	BENCH_DEFCONFIG=$run "$BENCH" run 2>&1 | tee "$day/$run.bench"
	after=$(readlink logs/last.log 2> /dev/null || true)
	{
		echo "## $run"
		echo
		echo '```'
	} >> "$summary"
	if [ -z "$after" ] || [ "$after" = "$before" ]; then
		echo "no log: the run did not come back (the bench's own words are in $run.bench)" | tee -a "$summary"
		echo '```' >> "$summary"
		status=1
		[ -n "$keep_going" ] && continue
		break
	fi
	cp "logs/$after" "$day/$run.log"
	tools/logcheck.py "$day/$run.log" | tail -n +2 | tee -a "$summary"
	checked=${PIPESTATUS[0]}
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
