#!/bin/bash
# A read-only fingerprint of the phone's flash, to tell whether anything has changed it.
#
#   flash-fingerprint.sh android DIR       from Android (adb, root): for each logical unit sda..sdf its
#                                           size, sha256 of its first and last 8 blocks of 4 KB and
#                                           `sgdisk --android-dump`; sha256 of userdata's first MB, of
#                                           darwin's first 5628755968 bytes (the iOS volume image), the
#                                           number of files under /sdcard, and whether Android is up
#                                           (sys.boot_completed, /data mounted)  -> DIR/android.txt
#   flash-fingerprint.sh survey LOG DIR    the loader's own survey (ufs=1 or 2) out of a run's log:
#                                           each unit's GPT header and table CRCs, every partition's
#                                           place, the hashes of the partitions there are images of,
#                                           super's metadata, userdata's superblock (timings dropped)
#                                           -> DIR/survey.txt
#   flash-fingerprint.sh diff DIR1 DIR2    what differs
#
# Nothing here writes to the phone: dd only reads (if=), sgdisk only prints (--android-dump), and the
# script goes to the phone's shell on its standard input, not as a file.
set -e
case "$1" in
android)
	mkdir -p "$2"
	adb shell 'su -c sh' > "$2/android.txt.new" <<'PHONE'
for d in sda sdb sdc sdd sde sdf; do
	size=$(blockdev --getsize64 /dev/block/$d)
	n=$(($(blockdev --getsz /dev/block/$d) / 8))         # (the phone's shell counts in 32 bits: sectors, not bytes)
	first=$(dd if=/dev/block/$d bs=4096 count=8 2>/dev/null | sha256sum | cut -d' ' -f1)
	last=$(dd if=/dev/block/$d bs=4096 skip=$((n - 8)) count=8 2>/dev/null | sha256sum | cut -d' ' -f1)
	echo "$d: $size bytes, first 8 blocks $first, last 8 blocks $last"
	sgdisk --android-dump /dev/block/$d 2>&1 | sed "s/^/$d sgdisk: /"
done
echo "userdata ($(readlink /dev/block/by-name/userdata)) first MB: $(dd if=/dev/block/by-name/userdata bs=1048576 count=1 2>/dev/null | sha256sum | cut -d' ' -f1)"
echo "darwin ($(readlink /dev/block/by-name/darwin)) first 5628755968 bytes: $(dd if=/dev/block/by-name/darwin bs=4194304 count=1342 2>/dev/null | sha256sum | cut -d' ' -f1)"
echo "files under /sdcard: $(find /sdcard/ -type f 2>/dev/null | wc -l)"
find /sdcard/ -type f 2>/dev/null | sort | sed 's/^/sdcard file: /'
echo "android: boot_completed=$(getprop sys.boot_completed), /data: $(mount | grep ' /data ' | cut -d' ' -f1,5)"
PHONE
	grep '^sdcard file: ' "$2/android.txt.new" > "$2/sdcard-files.txt" || true
	grep -v '^sdcard file: ' "$2/android.txt.new" > "$2/android.txt"
	rm -f "$2/android.txt.new"
	grep -E '^(sd[a-f]:|userdata|darwin|files|android)' "$2/android.txt"
	;;
survey)
	mkdir -p "$3"
	awk '/^== KERNEL LOG ==/{exit} {print}' "$2" | tr -d '\r' | grep -a '^ufs: ' |
		grep -E '^ufs: (unit [0-9]+: (media|GPT|path)|part |[A-Za-z0-9_]+: fnv |super |userdata sb |no partition)' |
		sed -E 's/, [0-9]+ ms$//' > "$3/survey.txt"
	echo "$(wc -l < "$3/survey.txt") survey lines from $2"
	grep -E 'GPT header|fnv' "$3/survey.txt" | head -40
	;;
diff)
	for f in android.txt survey.txt; do
		if [ -f "$2/$f" ] && [ -f "$3/$f" ]; then
			if diff -u "$2/$f" "$3/$f"; then
				echo "$f: the same"
			fi
		else
			echo "$f: not in both"
		fi
	done
	;;
*)
	sed -n '2,20p' "$0"
	exit 1
	;;
esac
