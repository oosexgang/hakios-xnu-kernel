#!/bin/bash
# Metal's compiled shader stages off a running phone, for the next small image: no boot compiles them again.
#
#   darwin/tools/metal-cache-pull.sh ADDRESS      the phone booted with rc.jobs.ssh (or any list that starts the
#                                              server), after it has drawn what the next boots should not wait for
#
# The window server keeps each stage it compiles in /private/var/db/hakios-metal-cache (HakiosMetalCA.m:
# HM_CACHE_DIR) or, where it may not make that, /var/tmp/hakios-metal-cache; either is empty again at the next
# boot. This copies them over SSH (scp -O, the key of ../ssh/ beside the repository) into $OBJ/metal-cache-seed,
# adding to what is there; root26-darwin.sh puts that into the small image as /hakios/share/hakios-metal-cache,
# where the plug-in looks after its own cache. A stage half written when it was copied is a .tmp and is left out.
set -e
[ -n "$1" ] || { sed -n '2,6p' "$0" >&2; exit 2; }
RN=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=../userland/env.sh
source "$RN/userland/env.sh"
KEY=${SSH_KEY:-$(cd "$IOS_REPO/.." && pwd)/ssh/id_phone}
OUT=$OBJ/metal-cache-seed
NEW=$OUT.new
rm -rf "$NEW"
mkdir -p "$NEW" "$OUT"
# (the server is dropbear, whose scp speaks the old protocol: -O; the phone's host key is the same at every boot,
# made from ../ssh/ssh_host_ed25519_key)
# (HM_CACHE_DIR, or /var/tmp/hakios-metal-cache where the plug-in could not make that one: the window server's
# sandbox; whichever has them)
for dir in /private/var/db/hakios-metal-cache /private/var/tmp/hakios-metal-cache; do
	scp -O -q -i "$KEY" -o StrictHostKeyChecking=accept-new "root@$1:$dir/*.stage" "$NEW/" 2> /dev/null && break
done
stages=("$NEW"/*.stage)
n=0
[ -e "${stages[0]}" ] && n=${#stages[@]}
cp "$NEW"/*.stage "$OUT/" 2> /dev/null || true
rm -rf "$NEW"
echo "metal-cache-pull: $n stages from the phone; $(ls "$OUT" | wc -l) in $OUT ($(du -sh "$OUT" | cut -f1)); root26-darwin.sh carries them"
