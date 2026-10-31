#!/bin/bash
# Check our bitcode reader against LLVM's, module by module.
#
# For each module: llvm-dis prints it, and air2nir --ll prints what our reader
# read. Neither output is compared directly -- the two printers differ in
# whitespace, in attribute groups and in how they number metadata -- so both
# go through llvm-as | llvm-dis first, which puts them in one canonical form,
# and then through the same filter (below). What is left is compared exactly.
#
# A module where llvm-as refuses our output is a reader bug or a printer bug
# either way, and is reported as such; a module where the two canonical forms
# differ is printed with its diff.
#
# usage: reader-check.sh [-v] MODULE.bc...     (or .metallib, or a directory)
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
AIR2NIR=${AIR2NIR:-$ROOT/build/mesa-host/build/src/compiler/air/air2nir}
VERBOSE=0
[ "${1:-}" = "-v" ] && { VERBOSE=1; shift; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# What both sides are reduced to: the semantics the reader keeps. Attributes,
# the module identification, and the symbol-table order are not kept, so they
# are dropped from both sides rather than silently differing.
# -a throughout: a module whose string constants print a raw NUL would make GNU
# grep treat its own input as binary, and `grep -v` would then drop every line --
# a reader check that silently compares nothing against nothing and passes.
filter() {
	grep -av '^;' |
		grep -av '^attributes #' |
		grep -av '^source_filename' |
		sed -E -e 's/ #[0-9]+//g' -e 's/^(define|declare) (dso_local |dllexport |hidden |protected )*/\1 /' -e 's/(captures|dereferenceable|dereferenceable_or_null|align|range|elementtype|byval|byref|sret|inalloca|preallocated|alignstack|allocsize)\([^)]*\)//g' -e 's/\b(noalias|readonly|readnone|writeonly|writable|nocapture|nonnull|immarg|zeroext|signext|noundef|nofree|nosync|nest|returned|inreg|initializes|allocalign|allocptr|dead_on_unwind|nocallback|willreturn|norecurse|nounwind|speculatable|mustprogress)\b//g' -e 's/ align [0-9]+ (%|@)/ \1/g' -e 's/  +/ /g; s/ +$//; s/\( /(/g; s/ \)/)/g; s/ ,/,/g; s/\(,/(/g' |
		grep -av '^ *$'
}

# Declarations carry no order in LLVM IR, and llvm-as/llvm-dis does not keep
# the one it was given, so both sides put them last and sorted.
sort_declares() {
	cat > "$WORK/c.ll"
	grep -av '^declare ' "$WORK/c.ll"
	grep -a '^declare ' "$WORK/c.ll" | sort
}

canon() { # stdin: .ll -> canonical .ll on stdout, or nothing and rc != 0
	llvm-as -o "$WORK/a.bc" 2> "$WORK/as.err" &&
		llvm-dis -o - "$WORK/a.bc" | filter | sort_declares
}

modules=0 ok=0 reject=0 differ=0 readfail=0
check_bc() { # $1 = a file holding one module's bitcode, $2 = its name
	modules=$((modules + 1))
	if ! "$AIR2NIR" --ll "$1" > "$WORK/ours.ll" 2> "$WORK/ours.err"; then
		readfail=$((readfail + 1))
		printf 'READER  %s: %s\n' "$2" "$(head -1 "$WORK/ours.err")"
		return
	fi
	llvm-dis -o - "$1" 2>/dev/null | canon > "$WORK/theirs.canon" || true
	if ! canon < "$WORK/ours.ll" > "$WORK/ours.canon"; then
		reject=$((reject + 1))
		printf 'LLVM-AS %s: %s\n' "$2" "$(head -1 "$WORK/as.err")"
		return
	fi
	if cmp -s "$WORK/ours.canon" "$WORK/theirs.canon"; then
		ok=$((ok + 1))
		[ "$VERBOSE" = 1 ] && printf 'ok      %s\n' "$2"
	else
		differ=$((differ + 1))
		printf 'DIFFER  %s\n' "$2"
		[ "$VERBOSE" = 1 ] && diff -u "$WORK/theirs.canon" "$WORK/ours.canon" | head -40
	fi
	return 0
}

for arg in "$@"; do
	if [ -d "$arg" ]; then
		while IFS= read -r f; do check_bc "$f" "${f#"$arg"/}"; done \
			< <(find "$arg" -name '*.bc' | sort)
	elif [ "${arg%.metallib}" != "$arg" ]; then
		dir=$WORK/unpack
		rm -rf "$dir"
		python3 "$ROOT/tools/metallib.py" dump "$arg" "$dir" > /dev/null
		while IFS= read -r f; do
			check_bc "$f" "$(basename "$arg"):$(basename "$f" .bc)"
		done < <(find "$dir" -name '*.bc' | sort)
	else
		check_bc "$arg" "$arg"
	fi
done

printf '\nreader-check: %d modules, %d identical to llvm-dis' "$modules" "$ok"
[ "$readfail" != 0 ] && printf ', %d our reader refused' "$readfail"
[ "$reject" != 0 ] && printf ', %d llvm-as refused' "$reject"
[ "$differ" != 0 ] && printf ', %d differ' "$differ"
printf '\n'
[ "$ok" = "$modules" ]
