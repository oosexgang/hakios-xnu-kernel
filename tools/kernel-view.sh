#!/bin/bash
# The kernel's files of ours at their paths in its tree, as links to the repository's: DIR/bsd/dev/board/thermal.c
# (drivers/board), DIR/bsd/sys/board/input.h (include/board), DIR/bsd/miscfs/apfs (fs/apfs), DIR/osfmk/arm64
# (arch/arm64/kernel), ... -- what the host's tests compile them in, with the paths the kernel's build has. The
# list of where each part is kept is kernel/lib.sh's (OVERLAY_PLACES).
#
#   tools/kernel-view.sh DIR        (DIR is made; links already there are replaced)
set -euo pipefail
[ $# -eq 1 ] || { echo "usage: tools/kernel-view.sh DIR" >&2; exit 2; }
# shellcheck source=../kernel/lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/../kernel/lib.sh"
overlay_view "$1"
