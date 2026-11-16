#!/usr/bin/env python3
"""airdump.py RUN.log OUTDIR: the fragment modules a run with boot-arg metal_air_dump=1 printed (the plug-in's
"airdump: FNV LENGTH OFFSET HEX" lines), put together again as OUTDIR/FNV.air, each checked against its length and its
64-bit FNV-1a. For tools/sweep.cpp's -a: CoreAnimation builds its uber shader at run time, so no metallib holds it.
The console may repeat a line or drop one; a module with a hole is said so and not written."""
import os, re, sys

parts = {}
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'airdump: ([0-9a-f]{16}) (\d+) (\d+) ([0-9a-f]+)', line)
    if m:
        fnv, length, off, hexed = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
        # (a line another process's output broke into is not its whole piece: the next copy of it may be)
        if len(hexed) != 2 * min(400, length - off):
            continue
        parts.setdefault((fnv, length), {})[off] = bytes.fromhex(hexed)
os.makedirs(sys.argv[2], exist_ok=True)
for (fnv, length), chunks in sorted(parts.items()):
    data = bytearray(length)
    have = 0
    for off, b in chunks.items():
        data[off:off + len(b)] = b
        have += len(b)
    h = 0xcbf29ce484222325
    for c in data:
        h = ((h ^ c) * 0x100000001b3) & 0xffffffffffffffff
    if have < length or '%016x' % h != fnv:
        print(f"airdump: {fnv}: {have} of {length} bytes, fnv {h:016x}: not whole, not written")
        continue
    path = os.path.join(sys.argv[2], fnv + '.air')
    open(path, 'wb').write(data)
    print(f"airdump: {path}: {length} bytes")
