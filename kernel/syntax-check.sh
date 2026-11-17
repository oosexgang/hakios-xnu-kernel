#!/bin/bash
# Compile the files HakiOS adds to the kernel -- for syntax, types and what they print, with the machine's own
# clang, against the headers of an assembled tree. No Darwin toolchain, no SDK, nothing is linked: it
# says in a minute or two whether a driver still compiles, where the kernel build (kernel/build.sh)
# takes a toolchain and much longer.
#
#   kernel/syntax-check.sh [--from SRC] [--tree DIR] [--driverkit DIR] [--analyze [--why]] [FILE...]
#
#   --tree DIR      an assembled tree to check against (kernel/assemble.sh); brought up to date with
#                   the repository first. Without it one is assembled in a temporary directory.
#   --from SRC      a git repository with Apple's release, instead of fetching it (as assemble.sh)
#   --driverkit DIR the DriverKit headers of the macOS SDK the build uses (.../Kernel.framework/Headers/
#                   DriverKit; kernel/build.sh fetches it). The C++ files -- the stand-ins for Apple's
#                   kernel extensions -- include them through IOKit's; without this they are left out.
#   --analyze       instead: clang's static analyzer on the same files, and what it finds in HakiOS' own
#                   lines listed (a use of a value never set, a null pointer followed, a division by zero ...).
#                   It reports and does not fail: it does not know that uiomove() fills what it is given,
#                   for one, and says so of every read. What it says is for a person to judge.
#   --why           with --analyze: each finding with the path the analyzer took to it (its notes: which
#                   branch it assumed, where the value came from), file by file, to judge it by
#   FILE...         files of kernel/overlay, as paths in the tree (bsd/dev/board/thermal.c); all of them
#                   if none is given
#
# What it cannot do: the files of kernel/syntax-check.skip need what only the real build has (headers
# it generates, builtins of Apple's clang) and are left out, by name. A file on that list that
# compiles after all fails the check, so that the list says what is true. And a file that passes here
# can still fail to link, or be wrong: this is the first check, not the last.
set -euo pipefail
# shellcheck source=lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

from=() tree='' driverkit='' analyze=0 why=0
while [ $# -gt 0 ]; do
	case "$1" in
	--from) from=(--from "${2:?--from needs a repository}"); shift ;;
	--tree) tree=${2:?--tree needs a directory}; shift ;;
	--driverkit) driverkit=${2:?--driverkit needs a directory}; shift ;;
	--analyze) analyze=1 ;;
	--why) why=1 ;;
	-h|--help) sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
	-*) die "unknown option $1 (see --help)" ;;
	*) break ;;
	esac
	shift
done
command -v clang > /dev/null || die "needs clang"
[ -z "$driverkit" ] || [ -f "$driverkit/OSObject.h" ] ||
	die "$driverkit has no OSObject.h: give the DriverKit directory of Kernel.framework's Headers"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
if [ -n "$tree" ]; then
	"$KERNEL_DIR/assemble.sh" "${from[@]}" --update "$tree" > /dev/null
else
	tree=$work/xnu
	"$KERNEL_DIR/assemble.sh" "${from[@]}" "$tree" > /dev/null
fi
X=$(cd "$tree" && pwd)

# The headers the build generates (MIG's, the configuration's) and a source includes by name: empty
# here. What a file needs of their contents it cannot be checked for without the build.
gen=$work/gen
for header in debug.h mach_assert.h mach_ldebug.h mach/clock.h mach/clock_priv.h mach/clock_reply_server.h \
	mach/exc_server.h mach/host_priv.h mach/host_security.h mach/mach_exc_server.h mach/mach_host.h \
	mach/mach_port.h mach/notify_server.h mach/processor.h mach/processor_set.h mach/task.h mach/thread_act.h \
	mach/upl.h mach/vm_map.h san/kasan.h san/kcov_data.h security/_label.h sys/sysproto.h; do
	mkdir -p "$gen/$(dirname "$header")"
	: > "$gen/$header"
done
[ -z "$driverkit" ] || ln -s "$(cd "$driverkit" && pwd)" "$gen/DriverKit"
focaltech_stub > "$gen/focaltech_fw.h"  # (found only where the tree has none: kernel/build.sh makes the same)
# ... and the board table and the kernel's configuration (tools/gen_headers.py), for the device packages that are there (a kernel directory without the
# repository's tools around it, as the tests make one, has no boards to compile)
if [ -x "$KERNEL_DIR/../tools/gen_headers.py" ]; then
	"$KERNEL_DIR/../tools/gen_headers.py" --out "$gen" > /dev/null
fi
# (included first: a tree built before keeps the kconfig.h of its last build beside drivers.c, which "kconfig.h" would find
# before this one; every symbol in it is under #ifndef, so the fresh one's values are the ones taken)
fresh=()
[ ! -f "$gen/kconfig.h" ] || fresh=(-include "$gen/kconfig.h")

export IPHONEOS_DEPLOYMENT_TARGET=16.5   # (the generic Darwin target, for this version of the SDK)
flags=(--target=arm64-apple-darwin -fsyntax-only -ferror-limit=5 -nostdinc
	-isystem "$(clang -print-resource-dir)/include"
	-include "$KERNEL_DIR/deps/hostinc/xnu_clang_compat.h" -mkernel
	-DKERNEL -DKERNEL_PRIVATE -DXNU_KERNEL_PRIVATE -DBSD_KERNEL_PRIVATE -DPRIVATE -D__arm64__
	-DARM64_BOARD_CONFIG_BCM2837 -DAPPLE -DNeXT -DCONFIG_DTRACE=0 "${fresh[@]}"
	-I"$gen" -I"$X/bsd" -I"$X/osfmk" -I"$X/osfmk/libsa" -I"$X/libkern" -I"$X/iokit" -I"$X/pexpert"
	-I"$X/EXTERNAL_HEADERS" -I"$X/security" -I"$X/san/memory")

# compile FILE: quiet when it compiles; its first errors otherwise. A file that compiles is compiled
# once more for what it prints: a format that does not go with its arguments is an error in the real
# build (-Werror=format), and the first pass, which silences every warning, cannot see it.
compile() {
	local language=(-std=gnu11)
	case "$1" in *.cpp) language=(-x c++ -std=gnu++20 -fno-exceptions -fno-rtti -fapple-kext) ;; esac
	if [ "$analyze" = 1 ]; then
		# (what it finds in Apple's headers is not ours: only lines of the overlay's files are kept)
		# (--why: the notes as well, which say how it got there, of the overlay's lines too)
		(cd "$X" && clang "${flags[@]/-fsyntax-only/--analyze}" -Xanalyzer -analyzer-output=text -w "${language[@]}" "$1" \
			-o /dev/null 2>&1) | grep -E ": warning: $([ "$why" = 1 ] && echo '|: note: ')" | sed "s|^$X/||" |
			grep -F -f "$work/ours" || true
		return 0
	fi
	(cd "$X" && clang "${flags[@]}" -w "${language[@]}" "$1" 2>&1) || return 1
	(cd "$X" && clang "${flags[@]}" -Wno-everything -Wformat -Werror=format "${language[@]}" "$1" 2>&1)
}

skipped() {
	awk -v file="$1" '!/^[[:space:]]*(#|$)/ && $1 == file { found = 1 } END { exit !found }' "$KERNEL_DIR/syntax-check.skip"
}

if [ $# -gt 0 ]; then
	files=("$@")
else
	mapfile -t files < <(overlay_files | cut -f2 | grep -E '\.(c|cpp)$' | sort)
fi

# the files of the overlay, as paths in the tree followed by a colon: what --analyze keeps of what it says
overlay_files | cut -f2 | sed 's|$|:|' > "$work/ours"

# every file at once, as many as there are processors; the answers are read in order afterwards
mkdir "$work/out"
jobs=$(getconf _NPROCESSORS_ONLN 2> /dev/null || echo 2)
running=0
for i in "${!files[@]}"; do
	[ -f "$X/${files[$i]}" ] || die "${files[$i]} is not a file of the tree (a path as in kernel/overlay)"
	(compile "${files[$i]}" > "$work/out/$i" || echo failed >> "$work/out/$i") &
	running=$((running + 1))
	if [ "$running" -ge "$jobs" ]; then
		wait -n
		running=$((running - 1))
	fi
done
wait

if [ "$analyze" = 1 ]; then
	if [ "$why" = 1 ]; then
		cat "$work"/out/* 2> /dev/null
	fi
	cat "$work"/out/* 2> /dev/null | grep ': warning: ' | sort -u > "$work/found" || true
	[ "$why" = 1 ] || cat "$work/found"
	log "the static analyzer: $(wc -l < "$work/found") findings in HakiOS' files, of ${#files[@]} files (each for a person to judge)"
	exit 0
fi
compiled=0 left_out=0 without_driverkit=0 failed=0
for i in "${!files[@]}"; do
	file=${files[$i]}
	if [ -z "$driverkit" ] && [ "${file%.cpp}" != "$file" ]; then
		without_driverkit=$((without_driverkit + 1))
	elif skipped "$file"; then
		left_out=$((left_out + 1))
		if [ ! -s "$work/out/$i" ]; then
			echo "$file compiles here: take it off kernel/syntax-check.skip" >&2
			failed=$((failed + 1))
		fi
	elif [ -s "$work/out/$i" ]; then
		failed=$((failed + 1))
		grep -E 'error:|^In file included' "$work/out/$i" | head -8 >&2 || true
	else
		compiled=$((compiled + 1))
	fi
done
[ $failed = 0 ] || die "$failed of ${#files[@]} files failed (above)"
more=
[ $without_driverkit = 0 ] || more="; $without_driverkit C++ files not checked (no --driverkit)"
log "ok: $compiled files compile (syntax, types and formats); $left_out need the real build (kernel/syntax-check.skip)$more"
