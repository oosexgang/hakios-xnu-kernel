#!/bin/bash
# One-time setup of a HakiOS workspace.
#
# HakiOS lives in two repositories that are built as one tree:
#
#   hakios-xnu-kernel     (this one)  the XNU kernel patches, the EFI loader, the UEFI port, Wi-Fi,
#                                      QEMU scripts, xl and these common scripts
#   hakios-xnu-userland               our init, injected libraries, launchd jobs, the image builder,
#                                      the scripts that take iOS out of Apple's IPSW, Metal/Mesa work
#
# Their scripts refer to each other by path (userland/build.sh uses the kernel's cross toolchain in
# xnu-deps/, the kernel's APFS code is tested with tools/apfs/...). So the userland repository is
# checked out into the SAME directory as this one, as a second git repository whose git dir is
# .userland.git. Every path is then exactly where every script expects it; each repository still
# commits and pushes on its own (use common/ugit for the userland one: `common/ugit status`).
#
# usage: common/setup.sh            clone hakios-xnu-userland into this tree (or update it) and
#                                   check the host for the tools the build needs
#   USERLAND_URL=...   where to clone from (default: github.com/oosexgang/hakios-xnu-kernel)
#   USERLAND_REF=...   branch or tag (default: main)
set -e
cd "$(dirname "$(readlink -f "$0")")/.."
ROOT=$PWD
URL=${USERLAND_URL:-https://github.com/oosexgang/hakios-xnu-kernel.git}
REF=${USERLAND_REF:-main}
UG=(git --git-dir="$ROOT/.userland.git" --work-tree="$ROOT")

if [ ! -d .userland.git ]; then
	echo "setup: cloning $URL ($REF) into this tree"
	tmp=$(mktemp -d)
	git clone --no-checkout --branch "$REF" "$URL" "$tmp/u"
	mv "$tmp/u/.git" .userland.git
	rm -rf "$tmp"
	"${UG[@]}" config core.bare false
	"${UG[@]}" config core.worktree "$ROOT"
	# the kernel's .gitignore is read here too and outranks info/exclude: untracked files are the
	# kernel repository's business; add a new userland file by name (common/ugit add PATH)
	"${UG[@]}" config status.showUntrackedFiles no
	cp common/userland.exclude .userland.git/info/exclude
	# README.md, .gitignore and LICENSE at the top are the kernel repository's: the userland ones stay in git
	# only (skip-worktree), every other file is written out
	"${UG[@]}" read-tree -m HEAD
	for f in README.md .gitignore LICENSE; do
		"${UG[@]}" ls-files --error-unmatch "$f" > /dev/null 2>&1 && "${UG[@]}" update-index --skip-worktree "$f"
	done
	"${UG[@]}" ls-files -z | grep -zvxE 'README\.md|\.gitignore|LICENSE' | xargs -0 "${UG[@]}" checkout --
else
	echo "setup: hakios-xnu-userland already here, updating"
	cp common/userland.exclude .userland.git/info/exclude
	"${UG[@]}" pull --ff-only || echo "setup: could not fast-forward the userland checkout (local changes?)"
fi
"${UG[@]}" log -1 --format='setup: hakios-xnu-userland at %h (%cs) %s'
git log -1 --format='setup: hakios-xnu-kernel   at %h (%cs) %s'

# host check: what is missing only matters for the step that needs it
need() { # need <command> <what for>
	command -v "$1" >/dev/null 2>&1 && return 0
	printf '  %-22s missing -- needed %s\n' "$1" "$2"; MISSING=1
}
echo "setup: checking the host"
MISSING=0
need python3 "everywhere"
need git "everywhere"
need curl "to fetch iOS, XNU and dependencies"
need 7z "by ios165/fetch.sh (7-Zip 23+, reads APFS)"
# 7-Zip before 23 does not read APFS: it lists the image as raw partitions and fetch.sh finds no files
if command -v 7z >/dev/null 2>&1; then
	v=$(7z 2>&1 | grep -m1 -oE '[0-9]+\.[0-9]+' || true)
	[ "${v%%.*}" -ge 23 ] 2>/dev/null ||
		{ printf '  %-22s %s -- needed 23 or newer by ios165/fetch.sh (reads APFS)\n' 7z "${v:-unknown version}"; MISSING=1; }
fi
need gcc "by ios165/fetch.sh (builds the LZBITMAP decoder)"
need qemu-system-aarch64 "to run in QEMU (DEVICE=qemu-virt)"
need fastboot "to boot a phone"
need adb "to take a phone's own firmware off it"
need clang "to build the kernel and the loader (not needed with prebuilt/)"
need lld-link "to build the loader (not needed with prebuilt/)"
[ -d "${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk" ] ||
	echo "  theos + iPhoneOS16.5.sdk missing -- needed only to rebuild the kernel and our programs (prebuilt/ has them)"
[ $MISSING = 0 ] && echo "  all there"
cat <<EOF

Next:
  ios165/fetch.sh                   # once: takes iOS 16.5 out of Apple's IPSW onto this machine (7.4 GB download)
  common/build.sh qemu-virt         # images (+ kernel and loader when the toolchain is there)
  common/run.sh qemu-virt           # iOS in a QEMU window
See README.md; porting to another device: docs/PORTING.md and common/new-device.sh.
EOF
