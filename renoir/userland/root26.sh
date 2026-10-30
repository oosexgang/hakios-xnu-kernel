#!/bin/bash
set -e
DR=/home/wannq/darwin_kernel/darwin-renoir
RN=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
T=/home/wannq/darwin_kernel/ios26/root26-tree
rm -rf $T; cp -a $DR/userland/root $T
cp $RN/userland/obj26/*.dylib $T/lib/
cp $RN/userland/obj26/gl_test $T/bin/gl_test
mkdir -p $RN/userland
cat > $RN/userland/rc.jobs.ios26-probe <<'EOF'
# iOS 26 on renoir, first boots: what runs (to the console, read back from the loader's log)
/usr/bin/uname -a
/usr/bin/sw_vers
/bin/ps -A
/usr/bin/id
EOF
cp $RN/userland/rc.jobs.ios26-probe $T/etc/rc.jobs
cd $DR/userland
M=; [ -f obj/apfs-root-source/manifest ] && M=,manifest=obj/apfs-root-source/manifest
python3 ../tools/apfs/mkapfs.py -z --plain fw --plain fw-renoir --plain fw-wifi-renoir --plain fw-audio-renoir -o /home/wannq/darwin_kernel/ios26/root26.apfs redwood=$T$M | tail -1
ls -la /home/wannq/darwin_kernel/ios26/root26.apfs
