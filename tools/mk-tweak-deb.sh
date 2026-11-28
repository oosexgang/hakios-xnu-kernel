#!/bin/bash
# Build the port's test tweak (userland/src/tweaks/redtest.c) as a rootless package, the way a tweak
# from a repository comes: jb/debs/com.hakios.redtest_<version>_iphoneos-arm64.deb, with
#   /var/jb/usr/lib/TweakInject/redtest.dylib   the tweak, linked against libsubstrate (ElleKit)
#   /var/jb/usr/lib/TweakInject/redtest.plist   its Filter: SpringBoard
# and "Depends: mobilesubstrate", which ElleKit Provides in the prefix (tools/mk-jb.sh).
# Install it on the phone with dpkg -i (or apt install ./file.deb), then restart SpringBoard; the tweak
# says "hakios: test tweak loaded" and "hakios: tweak hook fired: -[SBIconView setIcon:]" on the console.
#
# Needs: xnu-clang (xnu-deps/host/bin) on PATH, the iPhoneOS 16.5 SDK, and jb/ made by tools/mk-jb.sh
# (the tweak links against the prefix's libellekit). No dpkg-deb on the computer is needed: the .deb
# (an ar archive of debian-binary, control.tar.gz and data.tar.gz) is put together here.
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
VERSION=${VERSION:-1.0}
PREFIX=$ROOT/jb/root/var/jb
[ -f "$PREFIX/usr/lib/libellekit.dylib" ] || { echo "mk-tweak-deb.sh: no ElleKit in jb/ (tools/mk-jb.sh)" >&2; exit 1; }
export PATH="$ROOT/xnu-deps/host/bin:$PATH"
SDK=${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/data/var/jb/usr/lib/TweakInject" "$TMP/control" "$TMP/lib"

# libsubstrate.dylib in the prefix is a link to libellekit; link against a copy that calls itself by
# the name the tweak is to ask for, so the tweak's load command says /var/jb/usr/lib/libsubstrate.dylib
cp "$PREFIX/usr/lib/libellekit.dylib" "$TMP/lib/libsubstrate.dylib"
install_name_tool -id /var/jb/usr/lib/libsubstrate.dylib "$TMP/lib/libsubstrate.dylib" 2>/dev/null
xnu-clang -arch arm64 -isysroot "$SDK" -O1 -dynamiclib -lobjc \
	-install_name /var/jb/usr/lib/TweakInject/redtest.dylib \
	-o "$TMP/data/var/jb/usr/lib/TweakInject/redtest.dylib" \
	"$ROOT/userland/src/tweaks/redtest.c" "$TMP/lib/libsubstrate.dylib"
codesign -f -s - "$TMP/data/var/jb/usr/lib/TweakInject/redtest.dylib" 2>/dev/null || true
cat > "$TMP/data/var/jb/usr/lib/TweakInject/redtest.plist" <<'EOF'
{ Filter = { Bundles = ( "com.apple.springboard" ); }; }
EOF

cat > "$TMP/control/control" <<EOF
Package: com.hakios.redtest
Name: HakiOS test tweak
Version: $VERSION
Architecture: iphoneos-arm64
Description: Says on the console that it loaded, and hooks -[SBIconView setIcon:] once.
Maintainer: HakiOS
Author: HakiOS
Section: Tweaks
Depends: mobilesubstrate
EOF

echo 2.0 > "$TMP/debian-binary"
export COPYFILE_DISABLE=1
tar -C "$TMP/control" --uid 0 --gid 0 --uname root --gname wheel -czf "$TMP/control.tar.gz" ./control
tar -C "$TMP/data" --uid 0 --gid 0 --uname root --gname wheel -czf "$TMP/data.tar.gz" ./var
OUT=$ROOT/jb/debs/com.hakios.redtest_${VERSION}_iphoneos-arm64.deb
python3 - "$OUT" "$TMP/debian-binary" "$TMP/control.tar.gz" "$TMP/data.tar.gz" <<'EOF'
import os, sys
out, members = sys.argv[1], sys.argv[2:]
with open(out, 'wb') as f:
    f.write(b'!<arch>\n')
    for path in members:
        data = open(path, 'rb').read()
        name = os.path.basename(path).ljust(16).encode()
        f.write(name + b'0'.ljust(12) + b'0'.ljust(6) + b'0'.ljust(6) + b'100644'.ljust(8)
                + str(len(data)).ljust(10).encode() + b'`\n' + data)
        if len(data) % 2:
            f.write(b'\n')
EOF
echo "mk-tweak-deb.sh: $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes)"
