#!/bin/bash
# (tools/ldid-codesign.sh, for userland/build.sh LDID=...: ldid is not on this Mac)
# ldid -S<entitlements> FILE, done by macOS codesign: an ad hoc signature with those entitlements
ent=; for a in "$@"; do case "$a" in -S*) ent=${a#-S};; *) f=$a;; esac; done
exec /usr/bin/codesign -f -s - ${ent:+--entitlements "$ent"} --generate-entitlement-der "$f"
