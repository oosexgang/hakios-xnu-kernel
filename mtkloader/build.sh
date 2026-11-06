#!/bin/bash
# Build mtkloader.bin. MMU off means device memory: no SIMD, strict alignment, no builtins.
# Extra arguments go to the compiler (e.g. -DXNU_WDT_SECONDS=0).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

LLD="$ROOT/xnu-deps/host/zig/zig ld.lld"
CFLAGS="--target=aarch64-linux-gnu -ffreestanding -nostdlib -fno-builtin -mgeneral-regs-only -mstrict-align -Os -Wall -Wextra"

clang $CFLAGS "$@" -c "$HERE/start.S" -o "$HERE/start.o"
clang $CFLAGS "$@" -c "$HERE/mtkloader.c" -o "$HERE/mtkloader.o"
$LLD -T "$HERE/link.ld" --oformat binary "$HERE/start.o" "$HERE/mtkloader.o" -o "$HERE/mtkloader.bin"
ls -la "$HERE/mtkloader.bin"
