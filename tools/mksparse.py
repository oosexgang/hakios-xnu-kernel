#!/usr/bin/env python3
"""
Cut a large raw image into Android sparse images small enough for fastboot's download buffer.

  mksparse.py <image> <out-dir> [slice-MiB]

fastboot refuses a file far larger than max-download-size (128 MB here), so each piece is a
sparse image of the whole partition that says "leave everything before me alone" (a DONT_CARE
chunk) and then carries its own slice as raw data. Flashing the pieces in order writes the
image. Used for userland/system.packfs -> the xnusys partition (tools/system-flash.sh).
"""
import os
import struct
import sys

BLK = 4096
MAGIC = 0xed26ff3a
CHUNK_RAW = 0xcac1
CHUNK_DONT_CARE = 0xcac3


def main():
    src, outdir = sys.argv[1], sys.argv[2]
    slice_blocks = int(sys.argv[3] if len(sys.argv) > 3 else 96) * 1024 * 1024 // BLK
    size = os.path.getsize(src)
    if size % BLK:
        sys.exit(f"{src}: {size} bytes is not a multiple of {BLK}")
    total = size // BLK
    os.makedirs(outdir, exist_ok=True)
    names = []

    with open(src, "rb") as f:
        at = 0
        while at < total:
            n = min(slice_blocks, total - at)
            chunks = 1 if at == 0 else 2
            name = os.path.join(outdir, f"part{len(names):03d}.simg")
            with open(name, "wb") as o:
                o.write(struct.pack("<IHHHHIIII", MAGIC, 1, 0, 28, 12, BLK, total, chunks, 0))
                if at:
                    o.write(struct.pack("<HHII", CHUNK_DONT_CARE, 0, at, 12))
                o.write(struct.pack("<HHII", CHUNK_RAW, 0, n, 12 + n * BLK))
                f.seek(at * BLK)
                left = n * BLK
                while left:
                    buf = f.read(min(1 << 20, left))
                    o.write(buf)
                    left -= len(buf)
            names.append(name)
            at += n

    print(f"{src}: {size} bytes -> {len(names)} pieces in {outdir} "
          f"({slice_blocks * BLK // 1024 // 1024} MiB each)")


if __name__ == "__main__":
    main()
