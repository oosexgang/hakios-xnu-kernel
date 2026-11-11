#!/bin/bash
# The kernel's source tools (kernel/assemble.sh, export.sh, verify.sh) on a small made-up release: no
# network, a few seconds. What is checked is what a kernel developer relies on: the tree is what the
# repository says, an update touches only what changed, work in a tree is never run over, and an export
# brings it back.
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
failed=0 checks=0
check() {       # check "what" command...
	local what=$1
	shift
	checks=$((checks + 1))
	if ! "$@" > "$W/out" 2>&1; then
		failed=$((failed + 1))
		echo "FAIL: $what"
		sed 's/^/    /' "$W/out" | head -12
	fi
}
refuses() {     # refuses "what" "text it must say" command...
	local what=$1 text=$2
	shift 2
	checks=$((checks + 1))
	if "$@" > "$W/out" 2>&1 || ! grep -q -- "$text" "$W/out"; then
		failed=$((failed + 1))
		echo "FAIL: $what (wanted a refusal saying \"$text\")"
		sed 's/^/    /' "$W/out" | head -12
	fi
}
says() {        # says "what" "text" command...
	local what=$1 text=$2
	shift 2
	checks=$((checks + 1))
	if ! "$@" > "$W/out" 2>&1 || ! grep -q -- "$text" "$W/out"; then
		failed=$((failed + 1))
		echo "FAIL: $what (wanted \"$text\")"
		sed 's/^/    /' "$W/out" | head -12
	fi
}
g() { git -c user.name=t -c user.email=t@localhost -c init.defaultBranch=main "$@"; }

# --- a release of three files, and a kernel directory made for it ---------------------------------
g init -q "$W/upstream"
mkdir -p "$W/upstream/dir"
printf 'int a(void)\n{\n\treturn 1;\n}\n' > "$W/upstream/a.c"
printf 'int b(void)\n{\n\treturn 2;\n}\n' > "$W/upstream/dir/b.c"
printf 'int c(void)\n{\n\treturn 3;\n}\n' > "$W/upstream/c.c"
g -C "$W/upstream" add -A
g -C "$W/upstream" commit -q -m release
g -C "$W/upstream" tag rel-1

K=$W/repo/kernel
mkdir -p "$K/overlay/drivers" "$K/patches"
cp "$HERE"/../kernel/{lib.sh,assemble.sh,export.sh,verify.sh,import-patch.sh} "$K/"
cat > "$K/UPSTREAM" <<EOF
UPSTREAM_URL=$W/upstream
UPSTREAM_TAG=rel-1
UPSTREAM_COMMIT=$(g -C "$W/upstream" rev-parse HEAD)
EOF
printf 'int ours(void)\n{\n\treturn 10;\n}\n' > "$K/overlay/drivers/ours.c"
printf '0001-a.patch a.c\n# a comment\n\n0002-dir.patch dir/\n' > "$K/patches/series"
cat > "$K/patches/0001-a.patch" <<'EOF'
diff --git a/a.c b/a.c
--- a/a.c
+++ b/a.c
@@ -1,4 +1,4 @@
 int a(void)
 {
-	return 1;
+	return 100;
 }
EOF
T=$W/tree

# --- assemble ------------------------------------------------------------------------------------
check "a new tree is made" "$K/assemble.sh" "$T"
check "the patch is applied" grep -q 'return 100' "$T/a.c"
check "the overlay is copied in" cmp "$K/overlay/drivers/ours.c" "$T/drivers/ours.c"
check "an untouched file is Apple's" cmp "$W/upstream/c.c" "$T/c.c"
check "the tree is clean" test -z "$(git -C "$T" status --porcelain)"
refuses "an existing directory is not assembled over" "exists" "$K/assemble.sh" "$T"
says "an update with nothing new says so" "up to date" "$K/assemble.sh" --update "$T"
check "the repository verifies" "$K/verify.sh"

# --- an update touches only what changed ---------------------------------------------------------
touch -d '2001-01-01' "$T/a.c" "$T/c.c" "$T/drivers/ours.c"
printf 'int ours(void)\n{\n\treturn 11;\n}\n' > "$K/overlay/drivers/ours.c"
says "a changed overlay file is brought over" "1 files updated" "$K/assemble.sh" --update "$T"
check "... with its new contents" grep -q 'return 11' "$T/drivers/ours.c"
check "... and no other file touched" test "$(stat -c %Y "$T/a.c")" = "$(stat -c %Y "$T/c.c")" -a \
	"$(date -d @"$(stat -c %Y "$T/a.c")" +%Y)" = 2001
check "... the changed one was" test "$(date -d @"$(stat -c %Y "$T/drivers/ours.c")" +%Y)" != 2001

# --- work in the tree ----------------------------------------------------------------------------
sed -i 's/return 2/return 200/' "$T/dir/b.c"
printf 'int more(void)\n{\n\treturn 12;\n}\n' > "$T/drivers/more.c"
mkdir -p "$T/BUILD" && echo object > "$T/BUILD/x.o"
says "an update leaves unexported work alone" "left as it is" "$K/assemble.sh" --update "$T"
check "... the work is still there" grep -q 'return 200' "$T/dir/b.c"
check "the work is exported" "$K/export.sh" "$T"
check "... a new file into the overlay" cmp "$T/drivers/more.c" "$K/overlay/drivers/more.c"
check "... a change of Apple's file into its topic's patch" grep -q '^+	return 200;' "$K/patches/0002-dir.patch"
check "... the other patch as it was" grep -q '^+	return 100;' "$K/patches/0001-a.patch"
check "... what the build left behind is not sources" test ! -e "$K/overlay/BUILD"
check "... no abbreviated hashes in a patch" test -z "$(grep '^index ' "$K"/patches/*.patch)"
touch -d '2001-01-01' "$T/dir/b.c"
check "an update after the export adopts the tree" "$K/assemble.sh" --update "$T"
check "... without touching the files" test "$(date -d @"$(stat -c %Y "$T/dir/b.c")" +%Y)" = 2001
check "... and the tree is clean again" test -z "$(git -C "$T" status --porcelain)"
says "... and up to date" "up to date" "$K/assemble.sh" --update "$T"
check "the repository verifies with the new work" "$K/verify.sh"

# --- work in the tree and in the repository at once ----------------------------------------------
sed -i 's/return 200/return 201/' "$T/dir/b.c"
printf 'int ours(void)\n{\n\treturn 13;\n}\n' > "$K/overlay/drivers/ours.c"
refuses "an update does not run over work when the repository changed too" "not in the repository" \
	"$K/assemble.sh" --update "$T"
check "... the work is still there" grep -q 'return 201' "$T/dir/b.c"
check "--discard throws the work away" "$K/assemble.sh" --update --discard "$T"
check "... the tree is the repository's" grep -q 'return 200' "$T/dir/b.c"
check "... with the repository's change" grep -q 'return 13' "$T/drivers/ours.c"

# --- a patch written against the tree, by somebody else --------------------------------------------
cat > "$W/theirs.patch" <<'PATCH'
diff --git a/dir/b.c b/dir/b.c
--- a/dir/b.c
+++ b/dir/b.c
@@ -1,4 +1,5 @@
 int b(void)
 {
+	/* theirs */
 	return 200;
 }
diff --git a/drivers/theirs.c b/drivers/theirs.c
new file mode 100644
--- /dev/null
+++ b/drivers/theirs.c
@@ -0,0 +1,4 @@
+int theirs(void)
+{
+	return 14;
+}
PATCH
check "a patch against the tree is imported" "$K/import-patch.sh" --tree "$T" "$W/theirs.patch"
check "... its new file is in the overlay" grep -q 'return 14' "$K/overlay/drivers/theirs.c"
check "... its change of Apple's file is in the topic's patch" grep -q '^+	/\* theirs \*/' "$K/patches/0002-dir.patch"
check "... and the repository verifies" "$K/verify.sh"
cat > "$W/second.patch" <<'PATCH'
diff --git a/drivers/second.c b/drivers/second.c
new file mode 100644
--- /dev/null
+++ b/drivers/second.c
@@ -0,0 +1,4 @@
+int second(void)
+{
+	return 16;
+}
PATCH
check "a patch is imported without a tree at hand, too" env XNU_SRC="$W/none" "$K/import-patch.sh" "$W/second.patch"
check "... into the overlay" grep -q 'return 16' "$K/overlay/drivers/second.c"
check "... and the tree at hand follows at its next update" "$K/assemble.sh" --update "$T"
check "... with the file" grep -q 'return 16' "$T/drivers/second.c"
printf 'garbage\n' > "$W/bad.patch"
refuses "a patch that does not apply exports nothing" "does not apply" "$K/import-patch.sh" --tree "$T" "$W/bad.patch"
sed -i 's/return 14/return 15/' "$T/drivers/theirs.c"
refuses "patches are not applied over unexported work" "not in the repository" "$K/import-patch.sh" --tree "$T" "$W/theirs.patch"
git -C "$T" checkout -q -- drivers/theirs.c 2> /dev/null || "$K/assemble.sh" --update --discard "$T" > /dev/null

# --- what must be refused ------------------------------------------------------------------------
sed -i 's/return 3/return 300/' "$T/c.c"
refuses "a changed file no topic claims stops the export" "c.c" "$K/export.sh" "$T"
check "... and nothing of the repository was changed by it" test -f "$K/patches/0001-a.patch" -a -f "$K/overlay/drivers/ours.c"
git -C "$T" checkout -q -- c.c
sed -i '1i a note written into the patch by hand' "$K/patches/0001-a.patch"
refuses "a patch edited by hand does not verify" "0001-a.patch" "$K/verify.sh"
sed -i '1d' "$K/patches/0001-a.patch"
sed -i 's/^UPSTREAM_COMMIT=.*/UPSTREAM_COMMIT=0000000000000000000000000000000000000000/' "$K/UPSTREAM"
refuses "a release that is not the pinned commit is not used" "not using it" "$K/assemble.sh" "$W/other"
check "... and no half-made tree is left behind" test ! -e "$W/other"
refuses "a directory that is no assembled tree is not exported" "not a tree" "$K/export.sh" "$W/upstream"

echo "kernel_tools_test: $checks checks, $failed failed"
[ $failed = 0 ]
