#!/bin/bash
# Pick one daemon's story out of a run log: its own messages (stderr/os_log on the console), what
# launchd said about it, its failing system calls and crashes.
# usage: daemonlog.sh LOG NAME [max lines]
log=$1 name=$2 max=${3:-60}
sed -n '/== KERNEL LOG ==/,/--- kernel message buffer ---/p' "$log" |
	grep -a -E "(^|\] |[ 0-9:.+-]{20,40} )$name\[|\((system|user/[0-9]+|pid)/[^ ]*$name[^)]*\)|requestor = $name\[" |
	grep -a -v 'internal event\|service state: \(spawn\|xpcproxy\|running\)\|persona\|uncorking\|> created$\|shutting down\|cleaning up\|removing child\|launching: ' |
	cut -c1-${COLS:-250} | awk '!seen[$0]++' | head -"$max"
echo "--- crashes"
grep -a "usertrap: $name\[" -A5 "$log" | sed 's/^\[[^]]*\]: //' | awk '!seen[$0]++' | head -14
echo "--- failing system calls"
grep -a "systrace: $name\[" "$log" | sed 's/^\[[^]]*\]: //; s/\[[0-9]*\]/[]/; s/0x[0-9a-f]*/X/g' | sort | uniq -c | sort -rn |
	grep -a -v 'FeatureFlags\|fsgetpath\|getattrlist\|Info.plist\|libdispatch\|gettid\|#535\|__mac_syscall(X, X, X, X) = 103\|timezone\|shm_open' | cut -c1-200 | head -${SYS:-14}
