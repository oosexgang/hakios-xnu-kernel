#!/bin/bash
# The checks of form, for every file the repository tracks:
#   - every shell script parses (bash -n), every Python script parses
#   - the scripts of the current workflow (kernel/, loader/, darwin/, workbench/, tools/) pass shellcheck
#   - every link and every repository path in the documentation points at something (tools/check-links.py)
#   - components pins what the fetching scripts pin (tools/components.py)
#   - nothing that must not be published is there (tools/check-publishable.sh)
#   - no C format says a number with %#0Nx, which leaves the 0x off a zero ("0000"): 0x%0Mx says it one way
#   - every command of the QEMU shell (tools/qemu-init/main.c) has its row in Documentation/qemu.md
#   - every host test (workbench/*_test.c, Documentation/examples/*_test.c) is run by a script of workbench/
#   - the copies of the kernel's device interfaces that the Darwin side compiles against
#     (userland/userland/src/board/*.h of kernel/overlay/bsd/sys/board/) say what the kernel's say
# `make lint` runs this; it changes nothing.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1
failed=0

while IFS= read -r script; do
	bash -n "$script" || failed=1
done < <(git ls-files '*.sh')

while IFS= read -r script; do
	python3 -c 'import ast, sys; ast.parse(open(sys.argv[1]).read(), sys.argv[1])' "$script" || failed=1
done < <(git ls-files '*.py')

if command -v shellcheck > /dev/null; then
	shellcheck -S warning kernel/*.sh kernel/deps/*.sh kernel/deps/build/*/*.sh loader/*.sh darwin/userland/*.sh \
		workbench/*.sh tools/*.sh bench build.config.* || failed=1
else
	echo "lint: no shellcheck on this machine: only the syntax was checked"
fi

tools/check-links.py || failed=1
tools/components.py check || failed=1

tools/check-publishable.sh > /dev/null || { tools/check-publishable.sh; failed=1; }

if git grep -nE '%#0[0-9]+(l|ll)?x' -- '*.c' '*.cpp' '*.h' '*.m'; then
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
	kernel=kernel/overlay/bsd/sys/board/$(basename "$copy")
	[ -f "$kernel" ] || continue
	if ! diff -q <(seen_by_programs "$kernel") <(seen_by_programs "$copy") > /dev/null; then
		echo "lint: $copy does not say what $kernel says to programs:"
		diff <(seen_by_programs "$kernel") <(seen_by_programs "$copy") | head -20
		failed=1
	fi
done

[ $failed = 0 ] && echo "lint: ok"
exit $failed
