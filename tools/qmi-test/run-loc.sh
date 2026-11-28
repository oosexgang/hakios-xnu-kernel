#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/xnu-deps/build/qmi-test/loc-test"
mkdir -p "$(dirname "$out")"
${CC:-cc} -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$root/xnu/files/bsd" -I"$root/tools/qmi-test/shim" -o "$out" \
    "$root/tools/qmi-test/test_loc.c" "$root/drivers/modem/hakios.loc.c"
"$out"
