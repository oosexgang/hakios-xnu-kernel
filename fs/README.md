# fs

The file systems the kernel has of its own, as Linux's tree keeps them. Each directory is a directory of the
kernel's tree, where `kernel/assemble.sh` puts it (`kernel/lib.sh`, `OVERLAY_PLACES`):

| Directory | In the tree | What |
|---|---|---|
| `apfs/` | `bsd/miscfs/apfs/` | APFS, the file system of Darwin: read and written by an implementation of our own (the open kernel has none), with compressed files (`libdeflate/`, `libzbitmap/`, which keep their own licences: `NOTICE.md`). The system image Darwin starts from is an APFS image. |
| `ramfs/` | `bsd/miscfs/ramfs/` | A file system in the kernel's memory, read and written: `/private/var` and `/tmp`, made afresh at each boot (half of memory at most). |

The tools that make and check APFS images on this computer are in `userland/tools/apfs/`, built against these
same sources (`apfs-src.sh`); `workbench/apfs_image_test.sh` checks the two against each other.
