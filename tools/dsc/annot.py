#!/usr/bin/env python3
"""usage: tools/dsc/annot.py DISASM.s START END  -- the otool disassembly lines of START..END (hex), with each `bl` into the image's __objc_stubs
named by the selector it sends (tools/dsc/objcstub.py's decoding) and each adrp+add/ldr of a __cfstring / __cstring shown when readable."""
import re, struct, sys
sys.path.insert(0, sys.argv[0].rsplit('/', 1)[0])
import dsc

M = 0xfffffffff
c = dsc.Cache()
lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
cache = {}


def sel(a):
    if a in cache:
        return cache[a]
    try:
        adrp, ldr = struct.unpack('<II', c.read(a, 8))
        immlo, immhi = (adrp >> 29) & 3, (adrp >> 5) & 0x7ffff
        imm = ((immhi << 2 | immlo) ^ 0x100000) - 0x100000
        page = (a & ~0xfff) + (imm << 12)
        off = ((ldr >> 10) & 0xfff) << 3
        r = c.cstr(c.u64(page + off) & M, 80)
    except Exception:
        r = None
    cache[a] = r
    return r


for line in open(sys.argv[1]):
    m = re.match(r'^0000000([0-9a-f]{9})\t', line)
    if not m:
        continue
    a = int(m.group(1), 16)
    if a < lo:
        continue
    if a > hi:
        break
    out = line.rstrip()
    b = re.search(r'\tbl\t0x([0-9a-f]+)', out)
    if b:
        t = int(b.group(1), 16)
        if 0x1a23d59a0 <= t < 0x1a23d59a0 + 0x30660:
            s = sel(t)
            if s:
                out += '   ; [..' + s + ']'
    print(out[:150])
