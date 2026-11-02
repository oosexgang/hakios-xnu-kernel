#!/bin/bash
# Metal's compiled shader stages from the last run into the next image: no boot compiles them again.
#
#   tools/metal-cache-seed.sh            the phone in Android (adb, root): read var_store's newest archive off
#                                        darwin (read only), take db/hakios-metal-cache out of it into
#                                        userland/hakios-metal-cache; userland/build.sh puts that into the image
#                                        at /redwood/share/hakios-metal-cache, where the window server looks after
#                                        its own cache (HakiosMetalCA.m: HM_CACHE_SEED)
#   tools/metal-cache-seed.sh --image F  the same from a copy of darwin taken from byte 8589934592
#
# What fills the cache in the first place: a Metal run with var_store=1 ufs_write=1 (tools/metal-run.sh does so by
# default), whose orderly end saves /private/var into darwin. Works under Git Bash on Windows (python3 and adb on PATH).
set -e
cd "$(dirname "$(readlink -f "$0")")/.."
PY=$(command -v python3 || command -v python)
OUT=userland/hakios-metal-cache
rm -rf "$OUT.new"
"$PY" -I tools/var-store-read.py "$@" extract db/hakios-metal-cache "$OUT.new"
n=$(ls "$OUT.new" 2>/dev/null | grep -c '\.stage$' || true)
if [ "$n" = 0 ]; then
	rm -rf "$OUT.new"
	echo "metal-cache-seed: the store holds no compiled stages (was the run a Metal one, with var_store=1 ufs_write=1?)"
	exit 1
fi
# (a stage half written when the save came is a .tmp: never one of the cache's)
rm -f "$OUT.new"/*.tmp
# what was seeded before and is not in this store is kept: another build's stages are only room
mkdir -p "$OUT"
cp "$OUT.new"/*.stage "$OUT"/
rm -rf "$OUT.new"
echo "metal-cache-seed: $n stages from the phone; $(ls "$OUT" | wc -l | tr -d ' ') in $OUT; rebuild the image (userland/build.sh) to carry them"
