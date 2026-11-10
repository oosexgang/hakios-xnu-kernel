#!/bin/bash
# Build fbdiag.img: fbdiag in front of Android's kernel (rooted boot_a backup by default).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# an ELF linker: ld.lld of this machine, or zig's (tools/get-zig.sh; macOS has none of its own)
if [ -x "$ROOT/xnu-deps/host/zig/zig" ]; then LLD="$ROOT/xnu-deps/host/zig/zig ld.lld"
elif command -v ld.lld >/dev/null; then LLD=ld.lld
else echo "$(basename "$0"): no ld.lld: run tools/get-zig.sh" >&2; exit 1; fi
CFLAGS="--target=aarch64-linux-gnu -ffreestanding -nostdlib -fno-builtin -mgeneral-regs-only -mstrict-align -O2 -Wall -Wextra"

clang $CFLAGS -c "$HERE/fbdiag_start.S" -o "$HERE/fbdiag_start.o"
clang $CFLAGS -c "$HERE/fbdiag.c" -o "$HERE/fbdiag.o"
$LLD -T "$HERE/link.ld" --oformat binary "$HERE/fbdiag_start.o" "$HERE/fbdiag.o" -o "$HERE/fbdiag.bin"
BOOT=${ANDROID_BOOT:-$ROOT/backup/boot_a_magisk.img}
python3 "$ROOT/tools/chainboot.py" "$BOOT" "$HERE/fbdiag.bin" "$HERE/fbdiag.img"
