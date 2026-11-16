#!/bin/bash
# Build and run the tests of the QMI helpers (drivers/board/qmi.c) on this machine,
# under the address and undefined-behaviour sanitizers. workbench/run.sh runs this.
#
# With ath11k's sources at hand (ATH11K=<the directory with its qmi.c and qmi.h>;
# kernel/lx/build.sh fetches them) its own message tables are tested too; without, only what
# needs none of them. Nothing of ath11k is kept: its tables are taken out of qmi.c into a
# temporary directory each time.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT
# (the kernel's files at their paths in its tree: tools/kernel-view.sh)
"$HERE/../../tools/kernel-view.sh" "$B/kernel"
K=$B/kernel/bsd
# shellcheck disable=SC2054  # (address,undefined is one argument)
FLAGS=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -pthread)
if [ -n "${ATH11K:-}" ] && [ -f "$ATH11K/qmi.c" ]; then
	"$HERE/extract.py" "$ATH11K" > "$B/tables.inc"
	"${CC:-cc}" "${FLAGS[@]}" -I"$K" -I"$B" -I"$HERE/shim" -I"$ATH11K" -o "$B/test" "$HERE/test_qmi.c" "$K/dev/board/qmi.c"
	echo "qmi: with ath11k's tables from $ATH11K"
else
	echo "qmi: no ath11k sources (ATH11K=DIR): only the tests that need none"
	"${CC:-cc}" "${FLAGS[@]}" -DNO_ATH11K -I"$K" -o "$B/test" "$HERE/test_qmi.c" "$K/dev/board/qmi.c"
fi
"$B/test"
