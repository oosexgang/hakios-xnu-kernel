#!/bin/sh
# shotrun.sh LABEL ARG... : a RAM run of iOS with those extra boot args, a screenshot at ~165 s and ~185 s
cd "$(dirname "$0")/.." || exit 1
label=$1; shift
sh tools/to-fastboot.sh | grep -q sapphiren || { echo "no fastboot"; exit 3; }
cp tools/wifi-ios-ram.sh tools/ram-$label.sh
for a in "$@"; do sed -i "s/^ARGS=\"touch_lat=1 /ARGS=\"$a touch_lat=1 /" tools/ram-$label.sh; done
sed -i 's/touch_lat=1 //' tools/ram-$label.sh
cd "$(dirname "$0")/.."
(timeout 600 sh tools/ram-$label.sh 230 > /tmp/ram-$label.out 2>&1 &)
sleep 170
./xl screenshot logs/shot-$label-a.png 2>&1 | tail -1
sleep 25
./xl screenshot logs/shot-$label-b.png 2>&1 | tail -1
