#!/usr/bin/env python3
"""usage: tools/dsc/keys.py DISASM.s START END  -- the Objective-C selectors and the CFString constants (loaded through the image's GOT: adrp x8; ldr x8,[x8,#n];
ldr x2,[x8]) a function of the iOS 16.5 cache sends and names, in order. For finding which property keys a piece of CMCapture asks for."""
import re, struct, sys
sys.path.insert(0, sys.argv[0].rsplit('/', 1)[0])
import dsc

M = 0xfffffffff
c = dsc.Cache()
lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
regs = {}


def sel(a):
    try:
        adrp, ldr = struct.unpack('<II', c.read(a, 8))
        immlo, immhi = (adrp >> 29) & 3, (adrp >> 5) & 0x7ffff
        imm = ((immhi << 2 | immlo) ^ 0x100000) - 0x100000
        page = (a & ~0xfff) + (imm << 12)
        off = ((ldr >> 10) & 0xfff) << 3
        return c.cstr(c.u64(page + off) & M, 80)
    except Exception:
        return None


def cfstr(obj):
    try:
        return c.cstr(c.u64(obj + 16) & M, min(c.u64(obj + 24), 100) + 1)
    except Exception:
        return None


for line in open(sys.argv[1]):
    m = re.match(r'^0000000([0-9a-f]{9})\t(\S+)\t?(.*)$', line.rstrip('\n'))
    if not m:
        continue
    a = int(m.group(1), 16)
    if a < lo:
        continue
    if a > hi:
        break
    op, args = m.group(2), m.group(3)
    mm = re.match(r'adrp\s+(x\d+), \d+ ; 0x([0-9a-f]+)', op + ' ' + args)
    if op == 'adrp':
        r = re.match(r'(x\d+), (-?\d+) ; 0x([0-9a-f]+)', args)
        if r:
            regs[r.group(1)] = ('page', int(r.group(3), 16))
    elif op == 'ldr':
        r = re.match(r'(x\d+), \[(x\d+), #0x([0-9a-f]+)\]', args)
        if r and r.group(2) in regs and regs[r.group(2)][0] == 'page':
            slot = regs[r.group(2)][1] + int(r.group(3), 16)
            try:
                p = c.u64(slot) & M
                regs[r.group(1)] = ('ptr', p)
            except Exception:
                regs[r.group(1)] = ('unk', 0)
        else:
            r2 = re.match(r'(x\d+), \[(x\d+)\]$', args)
            if r2 and r2.group(2) in regs and regs[r2.group(2)][0] == 'ptr':
                s = cfstr(c.u64(regs[r2.group(2)][1]) & M) if True else None
                if s:
                    print('%x  key %s = "%s"' % (a, r2.group(1), s))
    elif op == 'bl':
        b = re.match(r'0x([0-9a-f]+)', args)
        if b:
            t = int(b.group(1), 16)
            if 0x1a23d59a0 <= t < 0x1a23d59a0 + 0x30660:
                s = sel(t)
                if s:
                    print('%x  [..%s]' % (a, s))
