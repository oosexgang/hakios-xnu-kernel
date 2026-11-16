#!/usr/bin/env python3
"""Build the store volume: an APFS container whose root holds the fixed-length slot files.

The kernel's APFS driver reads and does not write, and making a file is a transaction (see
bsd/miscfs/apfs/apfs_store.c and tools/apfs/txn-probe.py for what one costs).  So the volume is
made here, once, by the same program that makes every other image this port uses -- mkapfs.py --
and from then on nothing ever changes its structure again: only the bytes inside the slot files
are overwritten, where they lie, by apfs_write_in_place().  That is the whole reason this idea
does not fold back into needing a writable APFS, and the price of it is this one step on a
computer.

The slots are pre-allocated to their full length, so each is one extent with no hole in it --
a hole would mean allocating a block, which is the thing we are avoiding.  They are stored
uncompressed for the same reason: a compressed file keeps its bytes somewhere else entirely and
apfs_write_in_place refuses one.

usage: mkstore.py -o IMAGE [--slot-bytes N] [--slots N] [--dense]
  --slot-bytes N   how long each slot is; N may end in K, M or G (default 256M)
  --slots N        how many (default 2: the two-slot discipline needs exactly two, and more are
                   only for testing what the code does with them)
  --dense          do not dig the zero blocks back out of the image file afterwards

The image is a whole container and goes onto the flash as one: into the free space of the
partition darwin, past the blocks the loader and the kernel both refuse to write before
(BLOCK_WRITE_FIRST / UFS_WRITE_FIRST, block 2042368).  Writing it there is a once-only step from
Android with tools/darwin-write.py and it destroys whatever the store held before, so the image
is made once and kept.

Check it as any other image is checked: tools/apfs/check-image.sh IMAGE HakiosStore=DIR, and
`apfs-tool store info IMAGE` says whether the driver will have it.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
VOLUME = 'HakiosStore'                 # apfs_store.h: the name is a gate, not decoration
HEAD = 4096                             # APFS_STORE_PAYLOAD: the header block at the front


def bytes_arg(text):
    mult = {'k': 1 << 10, 'm': 1 << 20, 'g': 1 << 30}.get(text[-1:].lower())
    n = int(text[:-1]) * mult if mult else int(text)
    if n <= HEAD or n % 4096:
        raise argparse.ArgumentTypeError(
            'a slot must be a whole number of 4096-byte blocks and longer than %d' % HEAD)
    return n


def main(argv):
    ap = argparse.ArgumentParser(usage=__doc__.strip().splitlines()[2].strip()[7:])
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--slot-bytes', type=bytes_arg, default=256 << 20)
    ap.add_argument('--slots', type=int, default=2)
    ap.add_argument('--dense', action='store_true')
    args = ap.parse_args(argv)
    if args.slots < 1 or args.slots > 8:
        raise SystemExit('mkstore.py: between one and eight slots')

    tmp = tempfile.mkdtemp(prefix='mkstore.')
    try:
        for i in range(args.slots):
            # sparse here; mkapfs.py gives each one a real extent of its whole length
            with open(os.path.join(tmp, 'slot%d' % i), 'wb') as f:
                f.truncate(args.slot_bytes)
        argv = [sys.executable, os.path.join(HERE, 'mkapfs.py'), '-o', args.output,
                '%s=%s' % (VOLUME, tmp)]
        if subprocess.call(argv) != 0:
            raise SystemExit('mkstore.py: mkapfs.py failed')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # mkapfs.py writes the zeros out; on this machine they need not take room (on the flash they
    # will, and the image is written there whole either way)
    if not args.dense:
        subprocess.call(['fallocate', '-d', args.output])
    size = os.path.getsize(args.output)
    print('mkstore.py: %s: volume %s, %d slots of %d bytes (%d of payload each), container %d '
          'bytes' % (args.output, VOLUME, args.slots, args.slot_bytes,
                     args.slot_bytes - HEAD, size))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
