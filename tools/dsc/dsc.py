"""Minimal reader for the iOS 16.5 split dyld shared cache: VM reads, images, exports, symbols."""
import mmap, os, struct

DIR = __import__('os').path.join(__import__('os').path.dirname(__file__), '..', '..', 'ios165', '20F66__iPhone10,3_6')


class Cache:
    def __init__(self, d=DIR):
        self.maps = []          # (addr, size, mm, fileoff)
        base = os.path.join(d, 'dyld_shared_cache_arm64')
        files = [base] + sorted(os.path.join(d, n) for n in os.listdir(d)
                                if n.startswith('dyld_shared_cache_arm64.') and '.symbols' not in n)
        for p in files:
            fd = open(p, 'rb')
            mm = mmap.mmap(fd.fileno(), 0, access=mmap.ACCESS_READ)
            mo, mc = struct.unpack_from('<II', mm, 0x10)
            for i in range(mc):
                a, s, o, _, _ = struct.unpack_from('<QQQII', mm, mo + 32 * i)
                self.maps.append((a, s, mm, o))
        self.main = self.maps[0][2]
        io, ic = struct.unpack_from('<II', self.main, 0x1c0)
        self.images = {}
        for i in range(ic):
            a, _, _, po, _ = struct.unpack_from('<QQQII', self.main, io + 32 * i)
            name = self.main[po:self.main.find(b'\0', po)].decode()
            self.images[name] = a

    def read(self, addr, n):
        for a, s, mm, o in self.maps:
            if a <= addr < a + s:
                return mm[o + addr - a:o + addr - a + n]
        raise KeyError(hex(addr))

    def u32(self, a): return struct.unpack('<I', self.read(a, 4))[0]
    def u64(self, a): return struct.unpack('<Q', self.read(a, 8))[0]

    def cstr(self, a, n=512):
        b = self.read(a, n)
        return b[:b.find(b'\0')].decode(errors='replace')


class Image:
    def __init__(self, cache, name):
        self.c = cache
        self.base = cache.images[name]
        h = cache.read(self.base, 32)
        _, _, _, _, ncmds, sz, _, _ = struct.unpack('<IIIIIIII', h)
        self.segs, self.sects = {}, {}
        p = self.base + 32
        self.symtab = self.exports = None
        for _ in range(ncmds):
            cmd, cs = struct.unpack('<II', cache.read(p, 8))
            if cmd == 0x19:
                segname = cache.read(p + 8, 16).rstrip(b'\0').decode()
                vm, vs, fo, fs = struct.unpack('<QQQQ', cache.read(p + 24, 32))
                self.segs[segname] = (vm, vs, fo)
                ns = struct.unpack('<I', cache.read(p + 64, 4))[0]
                for k in range(ns):
                    q = p + 72 + 80 * k
                    sn = cache.read(q, 16).rstrip(b'\0').decode()
                    sa, ss = struct.unpack('<QQ', cache.read(q + 32, 16))
                    self.sects[(segname, sn)] = (sa, ss)
            elif cmd == 0x26:
                self.fstarts = struct.unpack('<II', cache.read(p + 8, 8))
            elif cmd == 2:
                self.symtab = struct.unpack('<IIII', cache.read(p + 8, 16))
            elif cmd == 0x80000033:
                self.exports = struct.unpack('<II', cache.read(p + 8, 8))
            elif cmd == 0x80000022 or cmd == 0x22:
                d = struct.unpack('<10I', cache.read(p + 8, 40))
                self.exports = (d[8], d[9])
            p += cs

    def le_addr(self, fileoff):
        vm, vs, fo = self.segs['__LINKEDIT']
        return vm + fileoff - fo

    def export_list(self):
        off, size = self.exports
        data = self.c.read(self.le_addr(off), size)
        out = {}

        def uleb(i):
            r = s = 0
            while True:
                b = data[i]; i += 1
                r |= (b & 0x7f) << s; s += 7
                if b < 0x80: return r, i

        def walk(i, pfx):
            ts, i2 = uleb(i)
            if ts:
                fl, j = uleb(i2)
                if not fl & 8:
                    a, _ = uleb(j)
                    out[pfx] = self.base + a
            j = i2 + ts
            n = data[j]; j += 1
            for _ in range(n):
                e = data.index(b'\0', j)
                lbl = data[j:e].decode(); j = e + 1
                child, j = uleb(j)
                walk(child, pfx + lbl)
        walk(0, '')
        return out

    def local_symbols(self):
        so, ns, stro, strs = self.symtab
        out = {}
        for i in range(ns):
            e = self.c.read(self.le_addr(so + 16 * i), 16)
            sx, ty, sect, desc, val = struct.unpack('<IBBHQ', e)
            if ty & 0xe == 0xe and val:
                out[val] = self.c.cstr(self.le_addr(stro + sx), 256)
        return out

    def function_starts(self):
        off, size = self.fstarts
        data = self.c.read(self.le_addr(off), size)
        out, a, i = [], self.segs['__TEXT'][0], 0
        while i < len(data):
            r = s = 0
            while True:
                b = data[i]; i += 1
                r |= (b & 0x7f) << s; s += 7
                if b < 0x80: break
            if r == 0: break
            a += r; out.append(a)
        return out
