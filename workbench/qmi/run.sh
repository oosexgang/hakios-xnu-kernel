#!/bin/bash
# Build and run the tests of the QMI helpers (kernel/overlay/bsd/dev/hakios_qmi.c) on this machine,
# under the address and undefined-behaviour sanitizers. workbench/run.sh runs this.
#
# With ath11k's sources at hand (ATH11K=<the directory with its qmi.c and qmi.h>;
# base/tools/mk-ath11k.sh fetches them) its own message tables are tested too; without, only what
# needs none of them. Nothing of ath11k is kept: its tables are taken out of qmi.c into a
# temporary directory each time.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
K=$HERE/../../kernel/overlay/bsd
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT
FLAGS=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -pthread)
if [ -n "${ATH11K:-}" ] && [ -f "$ATH11K/qmi.c" ]; then
	"$HERE/extract.py" "$ATH11K" > "$B/tables.inc"
	"${CC:-cc}" "${FLAGS[@]}" -I"$K" -I"$B" -I"$HERE/shim" -I"$ATH11K" -o "$B/test" "$HERE/test_qmi.c" "$K/dev/hakios_qmi.c"
	echo "qmi: with ath11k's tables from $ATH11K"
else
	echo "qmi: no ath11k sources (ATH11K=DIR): only the tests that need none"
	"${CC:-cc}" "${FLAGS[@]}" -DNO_ATH11K -I"$K" -o "$B/test" "$HERE/test_qmi.c" "$K/dev/hakios_qmi.c"
fi
"$B/test"
