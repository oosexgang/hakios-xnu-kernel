#!/usr/bin/env python3
# Make a sparse image of an APFS container: only the blocks a given walk actually reads,
# plus a table saying where each of them went.
#
# Why: the phone has no driver for its own storage, so a volume there has to live in a RAM
# disk, and the whole iOS root volume is 5368 MB against the 4288 MB the phone's firmware
# leaves usable.  Most of those blocks are never read by a given walk, though, so this
# writes out the ones that are and the driver cannot tell the difference -- every block it
# asks for is there.
#
# The blocks are found by walking with tools/apfs/apfs.py, which reads the image the same
# way the kernel's C does (run-tests.sh step 5 checks that), and recording every block it
# touches.  A cache of a different size only changes how often a block is re-read, not
# which blocks are needed, so the set is safe to hand to the kernel.
#
# Layout of what comes out (little-endian), as bsd/dev/redwood_sparse.c reads it:
#
#   header, one block:  "REDWOODSPARSE1\0\0", block size, block count of the container,
#                       how many blocks are present, the offsets of the table and the data
#   table:              one 32-bit entry per container block -- 0 absent, else the index
#                       into the data area plus one
#   data:               the blocks that are present, in the order the table numbers them
#
# usage: mksparse.py trace IMAGE [PATH ...] [--depth N]   just say what it would take
#        mksparse.py build IMAGE OUT [PATH ...] [--depth N] [--skip /a,/b]
#                                                     write the sparse image
# --skip leaves out the contents of those directories, the way boot-arg apfs_skip does in
# the kernel, so that several images between them cover a volume exactly once.
# With no PATH the whole volume is walked.

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import apfs

MAGIC = b"REDWOODSPARSE1\0\0"
HEADER_SIZE = 4096


class Tracing(apfs.Blocks):
    """A block reader that remembers every block it was asked for."""

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.touched = set()

    def get(self, addr, verify=True):
        self.touched.add(addr)
        return super().get(addr, verify)

    def raw(self, addr, count=1):
        for i in range(count):
            self.touched.add(addr + i)
        return super().raw(addr, count)


def walk_and_trace(image, paths, max_depth=0, skip=()):
    """
    Walk and record.  The recording reader has to be in place before the container is
    opened, not swapped in after: opening it reads block zero, the checkpoint area, the
    container's object map and the volume superblock, and without those in the image the
    driver cannot even find a superblock to mount from.
    """
    plain = apfs.Blocks
    apfs.Blocks = Tracing
    try:
        c = apfs.Container(image)
    finally:
        apfs.Blocks = plain
    tr = c.blocks
    tr.cache_limit = 60000
    v = apfs.pick_volume(c, None)

    starts = []
    if paths:
        for p in paths:
            hit = v.lookup(p)
            if hit is None:
                print("mksparse.py: %s: not found" % p, file=sys.stderr)
                return None, None, None
            starts.append((hit[0], "/" + p.strip("/")))
    else:
        starts.append((apfs.ROOT_DIR_INO_NUM, ""))

    n = 0
    for oid, prefix in starts:
        stack = [(oid, prefix, 1)]
        while stack:
            d, px, depth = stack.pop()
            for name, fid, dt in v.dir_entries(d):
                p = px + "/" + name
                ino = v.inode(fid)
                n += 1
                if ino is None:
                    continue
                if ino.is_reg or ino.is_link:
                    ino.contents()          # what makes the data blocks be read
                if dt == 4 and (max_depth == 0 or depth < max_depth) and p not in skip:
                    stack.append((fid, p, depth + 1))
                if n % 20000 == 0:
                    sys.stderr.write("\r  %d entries, %d blocks so far..."
                                     % (n, len(tr.touched)))
                    sys.stderr.flush()
    sys.stderr.write("\r")
    return c, tr, n


def main(argv):
    if len(argv) < 3:
        print(__doc__ or "", file=sys.stderr)
        return 2
    args = list(argv[1:])
    depth = 0
    skip = ()
    if "--depth" in args:
        i = args.index("--depth")
        depth = int(args[i + 1])
        del args[i:i + 2]
    if "--skip" in args:
        i = args.index("--skip")
        skip = tuple(args[i + 1].split(","))
        del args[i:i + 2]
    argv = [argv[0]] + args
    cmd = argv[1]
    if cmd == "trace":
        image, out, paths = argv[2], None, argv[3:]
    elif cmd == "build":
        if len(argv) < 4:
            print("mksparse.py: build needs an output file", file=sys.stderr)
            return 2
        image, out, paths = argv[2], argv[3], argv[4:]
    else:
        print("mksparse.py: unknown command %r" % cmd, file=sys.stderr)
        return 2

    c, tr, entries = walk_and_trace(image, paths, depth, skip)
    if c is None:
        return 1
    present = sorted(tr.touched)
    bs = c.block_size
    total = c.nx["block_count"]
    size = HEADER_SIZE + total * 4 + len(present) * bs
    print("walked %d entries over %s" % (entries, ", ".join(paths) if paths else "the whole volume"))
    print("container %d blocks of %d (%.2f GB)" % (total, bs, total * bs / 1e9))
    print("blocks needed %d (%.2f GB of data)" % (len(present), len(present) * bs / 1e9))
    print("sparse image would be %d bytes (%.2f GB): %d header, %d table, %d data"
          % (size, size / 1e9, HEADER_SIZE, total * 4, len(present) * bs))
    if out is None:
        return 0

    index = {}
    for i, a in enumerate(present):
        index[a] = i + 1
    with open(out, "wb") as g:
        hdr = bytearray(HEADER_SIZE)
        hdr[0:16] = MAGIC
        struct.pack_into("<IQQQQ", hdr, 16, bs, total, len(present),
                         HEADER_SIZE, HEADER_SIZE + total * 4)
        g.write(hdr)
        table = bytearray(total * 4)
        for a, i in index.items():
            struct.pack_into("<I", table, a * 4, i)
        g.write(table)
        done = 0
        for a in present:
            g.write(c.blocks.raw(a))
            done += 1
            if done % 20000 == 0:
                sys.stderr.write("\r  wrote %d/%d blocks..." % (done, len(present)))
                sys.stderr.flush()
        sys.stderr.write("\r")
    print("%s: %d bytes" % (out, os.path.getsize(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
