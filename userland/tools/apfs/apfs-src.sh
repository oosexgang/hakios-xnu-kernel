# Where the kernel's APFS code is, for the tools that build it as a program here: sourced, it sets
# APFS_SRC. The kernel's own sources first (fs/apfs at the top of the repository), then the
# trees of the first generation (an assembled xnu-8796 tree, or its exported files).
# shellcheck shell=bash
apfs_src_find() {
	local root=$1 d
	for d in "$root/../fs/apfs" "$root/xnu-src/xnu-8796.121.2/bsd/miscfs/apfs" \
		"$root/xnu/files/bsd/miscfs/apfs"; do
		if [ -f "$d/apfs_lib.c" ]; then
			(cd "$d" && pwd)
			return 0
		fi
	done
	echo "apfs-src.sh: apfs_lib.c is not in fs/apfs, an xnu-8796 tree or xnu/files" >&2
	return 1
}
