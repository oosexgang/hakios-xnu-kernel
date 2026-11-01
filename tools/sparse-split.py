#!/usr/bin/env python3
"""Cut a raw image into Android sparse images that each fit one fastboot download, to flash one after the other.

usage: sparse-split.py IMAGE OUTDIR [MAX_BYTES]          (MAX_BYTES: the phone's max-download-size, default 768 MiB)

MAX_BYTES must fit the 1 MiB header margin and at least one 4096-byte block. OUTDIR must not already contain
piece-*.simg files: use a fresh directory for a new image so that stale pieces cannot be mixed into it.

Each piece holds one stretch of IMAGE as RAW blocks (all-zero blocks as FILL, which costs nothing to send), and
DONT_CARE before and after it, so that every piece describes the whole image and the bootloader writes only its own
stretch: `fastboot flash PART piece-00.simg`, then 01, ... in any order. This is what fastboot does by itself with an
image larger than one download, done here because Google's fastboot for Windows runs out of memory on an 8 GB one.

What is checked: the pieces put together (RAW and FILL where they say, nothing else) are IMAGE again, byte for byte;
the sha256 of IMAGE is printed for the read-back on the phone.
"""
import hashlib
import os
import struct
import sys

BS = 4096
HEADER_MARGIN = 1 << 20
MAGIC, RAW, FILL, DONT_CARE = 0xED26FF3A, 0xCAC1, 0xCAC2, 0xCAC3


def chunk(kind, blocks, data_size):
    return struct.pack('<HHII', kind, 0, blocks, 12 + data_size)


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    image, out = sys.argv[1], sys.argv[2]
    try:
        limit = int(sys.argv[3], 0) if len(sys.argv) > 3 else 768 << 20
    except ValueError:
        sys.exit('sparse-split.py: MAX_BYTES must be an integer byte limit')
    if limit < HEADER_MARGIN + BS:
        sys.exit(f'sparse-split.py: MAX_BYTES must be at least {HEADER_MARGIN + BS} bytes '
                 '(1 MiB header margin plus one block)')
    size = os.path.getsize(image)
    if size % BS:
        sys.exit(f'sparse-split.py: {image} is not a whole number of {BS}-byte blocks')
    total = size // BS
    per = (limit - HEADER_MARGIN) // BS        # room for the headers, and a margin under the limit
    if os.path.isdir(out) and any(name.startswith('piece-') and name.endswith('.simg') for name in os.listdir(out)):
        sys.exit(f'sparse-split.py: {out} already contains sparse pieces; use a fresh directory')
    os.makedirs(out, exist_ok=True)
    whole = hashlib.sha256()
    check = hashlib.sha256()
    pieces = 0
    with open(image, 'rb') as f:
        for first in range(0, total, per):
            n = min(per, total - first)
            body = []                           # (kind, blocks, payload)
            run = None
            for b in range(n):
                block = f.read(BS)
                whole.update(block)
                kind = FILL if block == bytes(BS) else RAW
                if run and run[0] == kind:
                    run[1] += 1
                    if kind == RAW:
                        run[2].append(block)
                else:
                    run = [kind, 1, [block] if kind == RAW else []]
                    body.append(run)
            chunks = []
            if first:
                chunks.append(chunk(DONT_CARE, first, 0))
            for kind, blocks, data in body:
                if kind == RAW:
                    chunks.append(chunk(RAW, blocks, blocks * BS) + b''.join(data))
                else:
                    chunks.append(chunk(FILL, blocks, 4) + bytes(4))
            if first + n < total:
                chunks.append(chunk(DONT_CARE, total - first - n, 0))
            head = struct.pack('<IHHHHIIII', MAGIC, 1, 0, 28, 12, BS, total, len(chunks), 0)
            path = os.path.join(out, f'piece-{pieces:02d}.simg')
            try:
                with open(path, 'xb') as p:    # also refuse a file created after the directory check
                    p.write(head)
                    for c in chunks:
                        p.write(c)
            except FileExistsError:
                sys.exit(f'sparse-split.py: {path} already exists; use a fresh directory')
            # Verify the payload stream; the regression test also decodes the files written above.
            for kind, blocks, data in body:
                if kind == RAW:
                    for block in data:
                        check.update(block)
                else:
                    check.update(bytes(BS) * blocks)
            print(f'{path}: blocks {first}..{first + n - 1}, {os.path.getsize(path)} bytes, {len(chunks)} chunks')
            pieces += 1
    if check.digest() != whole.digest():
        sys.exit('sparse-split.py: the pieces do not put the image back together')
    print(f'{pieces} pieces; the image: {total} blocks, sha256 {whole.hexdigest()}')


if __name__ == '__main__':
    main()
