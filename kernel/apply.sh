#!/bin/bash
# Make the HakiOS kernel tree: Apple's xnu-12377.121.6 with xnu-12377.121.6-hakios.patch applied.
#   kernel/apply.sh DIR      (DIR is created; then build it the way base/xnu-build.sh does)
set -euo pipefail
K=$(cd "$(dirname "$0")" && pwd)
D=${1:?usage: apply.sh DIR}
git clone --depth 1 --branch xnu-12377.121.6 https://github.com/apple-oss-distributions/xnu.git "$D"
git -C "$D" apply --whitespace=nowarn "$K/xnu-12377.121.6-hakios.patch"
echo "HakiOS kernel tree in $D"
echo "redwood only: tools/mk-focaltech-fw.py <your focaltech_ts_fw_m20.bin> $D/bsd/dev/redwood_focaltech_fw.h"
