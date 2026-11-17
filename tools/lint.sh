#!/bin/bash
# The checks of form, for every file the repository tracks or is about to (tools/files.py --others: what git tracks
# and what it does not ignore, so that a file not yet added is looked at before it is; a release's list):
#   - every shell script parses (bash -n), every Python script parses and passes pyflakes' checks (ruff or pyflakes),
#     and so does the Python in the shell scripts' quoted here-documents (tools/heredoc-python.py)
#   - every shell script of the repository passes shellcheck (kernel/lx/build.sh apart)
#   - every link and every repository path in the documentation points at something (tools/check-links.py)
#   - components pins what the fetching scripts pin (tools/components.py)
#   - nothing that must not be published is there (tools/check-publishable.sh)
#   - no C format says a number with %#0Nx, which leaves the 0x off a zero ("0000"): 0x%0Mx says it one way
#   - every command of the QEMU shell (tools/qemu-init/main.c) has its row in Documentation/qemu.md
#   - every host test (workbench/*_test.c, Documentation/examples/*_test.c) is run by a script of workbench/
#   - the copies of the kernel's device interfaces that the Darwin side compiles against
#     (userland/userland/src/board/*.h of include/board/) say what the kernel's say
# `make lint` runs this; it changes nothing.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1
failed=0

while IFS= read -r script; do
	bash -n "$script" || failed=1
done < <(tools/files.py --others '*.sh')

while IFS= read -r script; do
	python3 -c 'import ast, sys; ast.parse(open(sys.argv[1]).read(), sys.argv[1])' "$script" || failed=1
done < <(tools/files.py --others '*.py')

if command -v shellcheck > /dev/null; then
	# every shell script of the repository: *.sh, and the files whose first line names sh or bash (bench, the
	# compiler and linker wrappers); not kernel/lx/build.sh, whose flags carry their quotes into the compiler's
	# words on purpose (-DKBUILD_MODNAME=\"ath11k\")
	scripts=()
	while IFS= read -r f; do
		case $f in kernel/lx/build.sh) continue ;; esac
		case $f in
		*.sh) scripts+=("$f") ;;
		*) [ -f "$f" ] && head -1 "$f" | grep -qE '^#!(/bin/(ba)?sh|/usr/bin/env (ba)?sh)' && scripts+=("$f") ;;
		esac
	done < <(tools/files.py --others)
	shellcheck -S warning "${scripts[@]}" build.config.* || failed=1
else
	echo "lint: no shellcheck on this machine: only the syntax was checked"
fi

# the Python the shell scripts carry in quoted here-documents (bench's, root26-darwin.sh's, ...): parsed, and
# checked with the files below, each as a file of its own in a directory that goes away
heredocs=$(mktemp -d)
trap 'rm -rf "$heredocs"' EXIT
mapfile -t shell_scripts < <(tools/files.py --others '*.sh' bench)
tools/heredoc-python.py --flakes "$heredocs" "${shell_scripts[@]}" || failed=1
# pyflakes' checks (names undefined, imported or assigned and never used), by ruff or pyflakes where there is one
if command -v ruff > /dev/null; then
	{ tools/files.py --others -z '*.py'; find "$heredocs" -name '*.py' -print0; } | xargs -0 ruff check --select F --no-cache --quiet || failed=1
elif python3 -m pyflakes --version > /dev/null 2>&1; then
	{ tools/files.py --others -z '*.py'; find "$heredocs" -name '*.py' -print0; } | xargs -0 python3 -m pyflakes || failed=1
else
	echo "lint: no ruff or pyflakes on this machine: the Python scripts were only parsed"
fi

tools/check-links.py || failed=1
tools/components.py check || failed=1

tools/check-publishable.sh > /dev/null || { tools/check-publishable.sh; failed=1; }

# (of every C, C++ and Objective-C file tools/files.py lists; grep's own status is not the answer when xargs splits the list)
zero=$(tools/files.py --others -z '*.c' '*.cpp' '*.h' '*.m' | xargs -0 -r grep -nE '%#0[0-9]+(l|ll)?x' -- || true)
if [ -n "$zero" ]; then
	echo "$zero"
	echo "lint: %#0Nx says a zero without its 0x: write 0x%0Mx (two digits fewer)"
	failed=1
fi

for test in workbench/*_test.c Documentation/examples/*_test.c; do
	name=$(basename "$test" .c)
	if ! grep -qw "$name" workbench/*.sh; then
		echo "lint: $test is run by no script of workbench/ (run.sh's list of tests, most often)"
		failed=1
	fi
done

for command in $(sed -n 's/^\t{ "\([a-z0-9-]*\)",.*/\1/p' tools/qemu-init/main.c); do
	if ! grep -q "^| \`${command}[\` ]" Documentation/qemu.md && ! grep -q "^| .*, \`${command}[\` ]" Documentation/qemu.md; then
		echo "lint: the QEMU shell's $command is not in Documentation/qemu.md's table of its commands"
		failed=1
	fi
done

# (each copy's first comment may say where it is used, and its #ifdef KERNEL part is the kernel's own; the rest is
# what a program sees, and is the kernel's word for word)
seen_by_programs() {
	sed '1,/^ \*\/$/d' "$1" | awk '/^#ifdef KERNEL$/ { skip = 1; next } skip && /^#endif/ { skip = 0; next } !skip'
}
for copy in userland/userland/src/board/*.h; do
	kernel=include/board/$(basename "$copy")
	[ -f "$kernel" ] || continue
	if ! diff -q <(seen_by_programs "$kernel") <(seen_by_programs "$copy") > /dev/null; then
		echo "lint: $copy does not say what $kernel says to programs:"
		diff <(seen_by_programs "$kernel") <(seen_by_programs "$copy") | head -20
		failed=1
	fi
done

[ $failed = 0 ] && echo "lint: ok"
exit $failed
