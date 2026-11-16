#!/bin/bash
# Build and run the tests of the WPA2 handshake's arithmetic (wifi/lx/src/lx_wpa.c) on this
# machine, under the address and undefined-behaviour sanitizers. Needs Python 3 with the
# "cryptography" package: the access point the handshakes are held with is reference.py.
# usage: tools/wpa-test/run.sh [SEED]      another seed, other made-up networks
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT
# (the handshake's source: kernel/lx/src of this repository, legacy/ beside it; wifi/lx/src where it was before)
SRC=$ROOT/../kernel/lx/src
[ -f "$SRC/lx_wpa.c" ] || SRC=$ROOT/wifi/lx/src
"$HERE/reference.py" "${1:-1}" > "$B/cases.inc"
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
	-I"$B" -I"$SRC" -o "$B/test" "$HERE/test_wpa.c"
"$B/test"
