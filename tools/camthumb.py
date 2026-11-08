#!/usr/bin/env python3
"""Make a PNG of the 64x48 thumbnail redwood_mtkcam.c logs ("THUMB nn: <64 hex pairs>").
usage: tools/camthumb.py logs/xnu-camtlNNN.log out.png   (the last thumbnail in the log; stretched to the full range, 8x enlarged)"""
import re, sys, zlib, struct
rows = {}
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'THUMB (\d\d): ([0-9a-f]{128})', line)
    if m:
        n = int(m.group(1))
        if n == 0:
            rows = {}
        rows[n] = bytes.fromhex(m.group(2))
if len(rows) != 48:
    sys.exit("no whole thumbnail in the log (%d rows)" % len(rows))
px = [list(rows[y]) for y in range(48)]
lo = min(min(r) for r in px); hi = max(max(r) for r in px)
print("thumbnail: min %d max %d" % (lo, hi))
S = 8
raw = b''
for y in range(48):
    line = bytes((px[y][x] - lo) * 255 // max(1, hi - lo) for x in range(64) for _ in range(S))
    raw += (b'\0' + line) * S
def chunk(t, d):
    c = struct.pack('>I', len(d)) + t + d
    return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 64 * S, 48 * S, 8, 0, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)
