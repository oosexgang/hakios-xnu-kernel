#!/bin/bash
# Write userland/system.apfs into the phone's xnusys partition, straight from the host (no
# temporary copy on the phone): that partition is where mtkloader reads XNU's root from.
# Needs Android up with root for the shell. LK's fastboot cannot take a file this large.
set -e
S="${HAKIOS_SERIAL:?set HAKIOS_SERIAL to your phone adb serial}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:-$ROOT/userland/system.apfs}"
[ -f "$IMG" ] || { echo "no such image: $IMG"; exit 1; }
adb -s $S shell 'su -c "ls -l /dev/block/by-name/xnusys"' || {
	echo "xnusys not there: reboot once so the kernel rereads the partition table"; exit 1; }
# Through a file on /data: piping into dd over "adb exec-in su -c" silently drops most of it.
TMP=/data/local/tmp/system.apfs
echo "sending $(du -h "$IMG" | cut -f1) to the phone..."
adb -s $S push "$IMG" $TMP
echo "writing it to xnusys..."
adb -s $S shell "su -c 'dd if=$TMP of=/dev/block/by-name/xnusys bs=4M'"
adb -s $S shell "su -c 'sync'; rm -f $TMP"
adb -s $S shell 'su -c "sync; dd if=/dev/block/by-name/xnusys bs=512 count=1 2>/dev/null | dd bs=1 skip=32 count=4 2>/dev/null"'
echo " <- an APFS container begins with NXSB at offset 32"
