#!/bin/bash
# Build fbdiag.img: fbdiag in front of Android's kernel (rooted boot_a backup by default).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
LLD="$ROOT/xnu-deps/host/zig/zig ld.lld"
CFLAGS="--target=aarch64-linux-gnu -ffreestanding -nostdlib -fno-builtin -mgeneral-regs-only -mstrict-align -O2 -Wall -Wextra"

clang $CFLAGS -c "$HERE/fbdiag_start.S" -o "$HERE/fbdiag_start.o"
clang $CFLAGS -c "$HERE/fbdiag.c" -o "$HERE/fbdiag.o"
$LLD -T "$HERE/link.ld" --oformat binary "$HERE/fbdiag_start.o" "$HERE/fbdiag.o" -o "$HERE/fbdiag.bin"
BOOT=${ANDROID_BOOT:-$ROOT/backup/boot_a_magisk.img}
python3 "$ROOT/tools/chainboot.py" "$BOOT" "$HERE/fbdiag.bin" "$HERE/fbdiag.img"
