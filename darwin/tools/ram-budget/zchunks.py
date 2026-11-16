#!/usr/bin/env python3
"""Compressed size of files the way tools/apfs/mkapfs.py -z stores them:
64 KB pieces, zlib level 9 each (stored with a 0xff prefix when not smaller), in an HFS resource
fork (0x100 header + 4 + 4 + 8*count table + pieces + 50-byte map), rounded up to 4 KB blocks;
a file <= 64 KB whose zlib stream fits 3804-16 bytes goes inline (0 blocks); a file stays raw when
the fork would not take fewer blocks.

usage: zchunks.py OUT.pkl FILE...   (writes {path: [compressed piece sizes]} and prints a table)
"""
import os, sys, zlib, pickle
from multiprocessing import Pool

PIECE = 65536
BS = 4096
SPAN = 64 * PIECE        # one task = 4 MB of a file


def work(task):
    path, off, length = task
    out = []
    with open(path, 'rb') as f:
        f.seek(off)
        data = f.read(length)
    for at in range(0, len(data), PIECE):
        chunk = data[at:at + PIECE]
        z = len(zlib.compress(chunk, 9))
        out.append(z if z < len(chunk) else len(chunk) + 1)
    return path, off, out


def file_blocks(size, pieces, small_inline=None):
    """blocks mkapfs gives a file of this size whose pieces compress to these sizes"""
    raw_blocks = -(-size // BS)
    if size <= PIECE and small_inline is not None and 16 + small_inline <= 3804:
        return 0
    rsrc = 0x100 + 4 + 4 + 8 * len(pieces) + sum(pieces) + 50
    return min(raw_blocks, -(-rsrc // BS))


def main():
    out_pkl, files = sys.argv[1], sys.argv[2:]
    tasks = []
    for p in files:
        size = os.path.getsize(p)
        for off in range(0, size, SPAN):
            tasks.append((p, off, min(SPAN, size - off)))
    tasks.sort(key=lambda t: -t[2])
    res = {p: {} for p in files}
    with Pool(os.cpu_count()) as pool:
        for path, off, out in pool.imap_unordered(work, tasks, chunksize=1):
            res[path][off] = out
    pieces = {p: [z for off in sorted(res[p]) for z in res[p][off]] for p in files}
    pickle.dump(pieces, open(out_pkl, 'wb'))
    M = 1 << 20
    tr = tc = 0
    for p in files:
        size = os.path.getsize(p)
        c = file_blocks(size, pieces[p]) * BS
        tr += size; tc += c
        print(f'{os.path.basename(p):45s} {size/M:10.2f} MiB raw {c/M:10.2f} MiB z9  ({c/size*100 if size else 0:5.1f}%)')
    print(f'{"TOTAL":45s} {tr/M:10.2f} MiB raw {tc/M:10.2f} MiB z9  ({tc/tr*100:5.1f}%)')


if __name__ == '__main__':
    main()
