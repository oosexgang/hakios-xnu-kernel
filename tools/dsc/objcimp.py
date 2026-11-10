#!/usr/bin/env python3
"""usage: tools/dsc/objcimp.py ADDR [ADDR...]   -- name the Objective-C method (-[Class sel] / +[Class sel]) whose implementation starts
at or just before each (unslid) address, over every image of the iOS 16.5 cache that has the address in its __TEXT.
Small (relative) method lists only, as the cache has them; no capstone."""
import struct, sys, bisect
sys.path.insert(0, sys.argv[0].rsplit('/', 1)[0])
import dsc

M = 0xfffffffff
c = dsc.Cache()


def ptr(a):
    return c.u64(a) & M


def i32(a):
    return struct.unpack('<i', c.read(a, 4))[0]


def methods(listaddr, kind, cname, out, im):
    if not listaddr:
        return
    ent, cnt = struct.unpack('<II', c.read(listaddr, 8))
    if not ent & 0x80000000:
        # big methods: name ptr, types ptr, imp ptr
        for i in range(cnt):
            n, t, p = struct.unpack('<QQQ', c.read(listaddr + 8 + 24 * i, 24))
            try:
                out.append((p & M, '%s[%s %s]' % (kind, cname, c.cstr(n & M, 100))))
            except Exception:
                pass
        return
    for i in range(cnt):
        e = listaddr + 8 + 12 * i
        name_off, typ, imp = struct.unpack('<iii', c.read(e, 12))
        impaddr = e + 8 + imp
        sel = None
        for direct in (True, False):
            try:
                if direct:
                    sel = c.cstr(sel_base + name_off, 100)
                else:
                    sel = c.cstr(ptr(e + name_off), 100)
                if sel and all(32 <= ord(ch) < 127 for ch in sel):
                    break
            except Exception:
                sel = None
        out.append((impaddr, '%s[%s %s]' % (kind, cname, sel)))


def scan(name):
    im = dsc.Image(c, name)
    out = []
    global sel_base
    sel_base = 0x18229614b      # the cache's relative-method selector base: found by matching a method list's name offsets against the selrefs of CMCapture
    for secname in ('__objc_classlist', '__objc_catlist'):
        sec = im.sects.get(('__DATA_CONST', secname)) or im.sects.get(('__DATA', secname))
        if not sec:
            continue
        for k in range(sec[1] // 8):
            cls = ptr(sec[0] + 8 * k)
            try:
                if secname == '__objc_classlist':
                    ro = ptr(cls + 32) & ~7
                    cname = c.cstr(ptr(ro + 24), 100)
                    methods(ptr(ro + 32), '-', cname, out, im)
                    meta = ptr(cls)
                    mro = ptr(meta + 32) & ~7
                    methods(ptr(mro + 32), '+', cname, out, im)
                else:
                    cname = c.cstr(ptr(ptr(cls + 8) + 24), 100) if ptr(cls + 8) else '?'
                    cname += '(cat)'
                    methods(ptr(cls + 16), '-', cname, out, im)
                    methods(ptr(cls + 24), '+', cname, out, im)
            except Exception:
                continue
    return sorted(out)


if __name__ == '__main__':
    addrs = [int(a, 16) for a in sys.argv[1:]]
    cache = {}
    for name, base in c.images.items():
        pass
    # find the image of each address by the nearest lower image base
    bases = sorted((b, n) for n, b in c.images.items())
    ab = [b for b, _ in bases]
    done = {}
    for a in addrs:
        b, n = bases[bisect.bisect_right(ab, a) - 1]
        if n not in done:
            done[n] = scan(n)
        lst = done[n]
        keys = [x for x, _ in lst]
        j = bisect.bisect_right(keys, a) - 1
        print(hex(a), n.rsplit('/', 1)[-1], (lst[j][1], hex(a - lst[j][0])) if j >= 0 else '?')
