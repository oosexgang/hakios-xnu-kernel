#!/usr/bin/env python3
"""usage: tools/dsc/objcstub.py ADDR... -- the selector an objc_msgSend stub (adrp x1; ldr x1,[x1,#off]; adrp x16; ldr x16; br) of the
iOS 16.5 cache sends."""
import struct, sys
sys.path.insert(0, sys.argv[0].rsplit('/', 1)[0])
import dsc

M = 0xfffffffff
c = dsc.Cache()
for arg in sys.argv[1:]:
    a = int(arg, 16)
    adrp, ldr = struct.unpack('<II', c.read(a, 8))
    immlo, immhi = (adrp >> 29) & 3, (adrp >> 5) & 0x7ffff
    imm = ((immhi << 2 | immlo) ^ 0x100000) - 0x100000
    page = (a & ~0xfff) + (imm << 12)
    off = ((ldr >> 10) & 0xfff) << 3
    try:
        print(hex(a), c.cstr(c.u64(page + off) & M, 100))
    except Exception as e:
        print(hex(a), 'err', e)
