#!/usr/bin/env python3
# The iOS 16.5 root file system has no sparse file in it -- a walk over all 318098 entries
# finds no inode with INODE_IS_SPARSE or a sparse-bytes extended field -- so the hole
# handling in read_stream cannot be checked against that image.  This checks it against
# made-up extent lists instead: a gap between two extents, an extent whose physical block
# is zero, a hole at the front, a hole at the end, and a file with no extents at all.
#
# usage: test_sparse.py

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import apfs

BS = 4096


class FakeBlocks:
    """Blocks of made-up contents: block n is the byte n, repeated."""

    def __init__(self):
        self.block_size = BS

    def raw(self, addr, count=1):
        return b"".join(bytes([a & 0xFF]) * BS for a in range(addr, addr + count))


class FakeVolume:
    """Just enough of a Volume for read_stream: a block source and an extent list."""

    def __init__(self, extents):
        self._extents = sorted(extents)

        class C:
            pass
        self.c = C()
        self.c.blocks = FakeBlocks()
        self.c.block_size = BS

    def extents(self, _stream_id):
        return self._extents

    read_stream = apfs.Volume.read_stream


def blk(n, count=1):
    return b"".join(bytes([a & 0xFF]) * BS for a in range(n, n + count))


CASES = [
    ("one extent, no hole",
     [(0, BS, 7)], BS, blk(7)),
    ("a gap between two extents",
     [(0, BS, 7), (2 * BS, BS, 9)], 3 * BS, blk(7) + b"\0" * BS + blk(9)),
    ("an extent whose physical block is zero reads as zeros",
     [(0, BS, 0), (BS, BS, 9)], 2 * BS, b"\0" * BS + blk(9)),
    ("a hole at the front",
     [(2 * BS, BS, 5)], 3 * BS, b"\0" * (2 * BS) + blk(5)),
    ("a hole at the end",
     [(0, BS, 5)], 3 * BS, blk(5) + b"\0" * (2 * BS)),
    ("no extents at all",
     [], 2 * BS, b"\0" * (2 * BS)),
    ("a multi-block extent",
     [(0, 3 * BS, 20)], 3 * BS, blk(20, 3)),
    ("the last extent is cut off by the file's size",
     [(0, BS, 7), (BS, BS, 8)], BS + 100, blk(7) + blk(8)[:100]),
    ("a gap of several blocks",
     [(0, BS, 1), (4 * BS, BS, 2)], 5 * BS,
     blk(1) + b"\0" * (3 * BS) + blk(2)),
]


def main():
    fails = 0
    for name, extents, size, want in CASES:
        got = FakeVolume(extents).read_stream(0, size)
        if got == want:
            print("  ok    %s" % name)
        else:
            fails += 1
            where = next((i for i, (a, b) in enumerate(zip(got, want)) if a != b),
                         min(len(got), len(want)))
            print("  FAIL  %s: %d bytes out, wanted %d, first difference at byte %d"
                  % (name, len(got), len(want), where))
    print("read_stream hole handling: %d cases, %d failed" % (len(CASES), fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
