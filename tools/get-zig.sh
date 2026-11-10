#!/bin/bash
# Fetch zig 0.13.0 into xnu-deps/host/zig. Only its linker is used: `zig ld.lld` links the loader
# (mtkloader/build.sh; an ELF linker, which macOS does not have) and `zig lld-link` the UEFI one (xnuloader).
# A machine with ld.lld of its own (Linux: the package lld) does not need this.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=0.13.0
case "$(uname -s)-$(uname -m)" in
Darwin-x86_64) NAME=zig-macos-x86_64-$VERSION SHA256=8b06ed1091b2269b700b3b07f8e3be3b833000841bae5aa6a09b1a8b4773effd ;;
Darwin-arm64)  NAME=zig-macos-aarch64-$VERSION SHA256=46fae219656545dfaf4dce12fb4e8685cec5b51d721beee9389ab4194d43394c ;;
Linux-x86_64)  NAME=zig-linux-x86_64-$VERSION SHA256=d45312e61ebcc48032b77bc4cf7fd6915c11fa16e4aad116b66c9468211230ea ;;
Linux-aarch64) NAME=zig-linux-aarch64-$VERSION SHA256=041ac42323837eb5624068acd8b00cd5777dac4cf91179e8dad7a7e90dd0c556 ;;
*) echo "get-zig.sh: no zig named here for $(uname -s) $(uname -m): see ziglang.org/download" >&2; exit 1 ;;
esac
DST=$ROOT/xnu-deps/host/zig
if [ -x "$DST/zig" ] && [ "$("$DST/zig" version 2>/dev/null)" = $VERSION ]; then echo "get-zig.sh: $DST is zig $VERSION already"; exit 0; fi
TMP=$(mktemp -d "$ROOT/xnu-deps/host/zig.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
echo "get-zig.sh: downloading $NAME.tar.xz (about 47 MB)"
curl -fL --retry 3 -o "$TMP/zig.tar.xz" "https://ziglang.org/download/$VERSION/$NAME.tar.xz"
if command -v sha256sum >/dev/null; then GOT=$(sha256sum "$TMP/zig.tar.xz"); else GOT=$(shasum -a 256 "$TMP/zig.tar.xz"); fi
[ "${GOT%% *}" = $SHA256 ] || { echo "get-zig.sh: the download's SHA-256 is ${GOT%% *}, not $SHA256" >&2; exit 1; }
tar -xf "$TMP/zig.tar.xz" -C "$TMP"
rm -rf "$DST"
mv "$TMP/$NAME" "$DST"
echo "get-zig.sh: $("$DST/zig" version) in $DST"
