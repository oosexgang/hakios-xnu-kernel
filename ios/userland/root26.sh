#!/bin/bash
set -e
RN=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DK=${DK:-$(cd "$RN/../.." && pwd)}   # the directory holding this repository, darwin-renoir and ios26
DR=${DR:-$DK/darwin-renoir}
IOS=${IOS:-$DK/ios26}
T=$IOS/root26-tree
rm -rf $T; cp -a $DR/userland/root $T
cp $RN/userland/obj26/*.dylib $T/lib/
cp $RN/userland/obj26/gl_test $T/bin/gl_test
mkdir -p $RN/userland
cat > $RN/userland/rc.jobs.ios26-probe <<'EOF'
# Darwin 26, first boots: what runs (to the console, read back from the loader's log)
/usr/bin/uname -a
/usr/bin/sw_vers
/bin/ps -A
/usr/bin/id
EOF
cp $RN/userland/rc.jobs.ios26-probe $T/etc/rc.jobs
cd $DR/userland
M=; [ -f obj/apfs-root-source/manifest ] && M=,manifest=obj/apfs-root-source/manifest
# the firmware directories (fw, fw-wifi-..., fw-audio-...) stay uncompressed: the kernel reads them as they lie
plain=(); for d in "$T"/fw*/; do if [ -d "$d" ]; then plain+=(--plain "$(basename "$d")"); fi; done
python3 ../tools/apfs/mkapfs.py -z "${plain[@]}" -o $IOS/root26.apfs hakios=$T$M | tail -1
ls -la $IOS/root26.apfs
