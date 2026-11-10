#!/usr/bin/env python3
"""usage: tools/dsc/stubname.py ADDR...  -- the symbol a stub (adrp x16; ldr x16,[x16,#off]; br x16) in the iOS 16.5 cache jumps to,
by the exports of every image (nearest export at or below the target)."""
import bisect, struct, sys
sys.path.insert(0, sys.argv[0].rsplit('/', 1)[0])
import dsc

M = 0xfffffffff
c = dsc.Cache()
table = []
for name, base in c.images.items():
    try:
        for sym, a in dsc.Image(c, name).export_list().items():
            table.append((a, sym, name.rsplit('/', 1)[-1]))
    except Exception:
        pass
table.sort()
keys = [t[0] for t in table]
for arg in sys.argv[1:]:
    a = int(arg, 16)
    w = struct.unpack('<III', c.read(a, 12))
    adrp, ldr = w[0], w[1]
    immlo, immhi = (adrp >> 29) & 3, (adrp >> 5) & 0x7ffff
    page = ((a & ~0xfff) + (((immhi << 2 | immlo) ^ 0x100000) - 0x100000 << 12))
    off = ((ldr >> 10) & 0xfff) << 3
    try:
        tgt = c.u64(page + off) & M
    except KeyError:
        print(hex(a), 'unreadable'); continue
    i = bisect.bisect_right(keys, tgt) - 1
    print(hex(a), '->', hex(tgt), table[i][1], '+%#x' % (tgt - table[i][0]), table[i][2])
