#!/usr/bin/env python3
"""Darwin 26 dyld cache: per-image segments, dependency closure of the boot-to-SpringBoard programs,
and how much of the zlib-compressed cache belongs to images outside that closure.

usage: cache_images.py PIECES.pkl MODE   (PIECES.pkl from zchunks.py over the cache files)
  MODE jobs:  the launchd jobs userland/userland/build.sh loads (DARWIN_JOBS + wifid), SpringBoard
  MODE apps:  jobs + the 15 applications the 16.5 port's screenshots show, with their extensions
  MODE broad: every Mach-O executable in rootfs (daemons, CoreServices, applications, XPC services)
Static closure over LC_LOAD/WEAK/REEXPORT/UPWARD_DYLIB only: dlopen() is not seen.
"""
import os, sys, struct, pickle, plistlib, glob, collections
import cachedir

DK = os.environ.get('DK') or os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..'))
I = os.environ.get('DARWIN_FW', DK + '/darwin26')
D = I + '/dyld/'
ROOT = I + '/rootfs'
CRYPTEXES = [I + '/cryptex/os', I + '/cryptex/app']
PIECE = 65536
LC_SEGMENT_64 = 0x19
DEP_CMDS = {0xc: 'load', 0x80000018: 'weak', 0x8000001f: 'reexport', 0x80000023: 'upward'}
LC_ID_DYLIB = 0xd

JOBS = '''com.apple.notifyd com.apple.cfprefsd.xpc.daemon com.apple.cfprefsd.xpc.daemon.system com.apple.logd
com.apple.syslogd com.apple.distnoted.xpc.daemon com.apple.diagnosticd com.apple.fseventsd
com.apple.containermanagerd com.apple.containermanagerd.system com.apple.runningboardd
com.apple.backboardd com.apple.mobile.usermanagerd com.apple.mobilegestalt.xpc com.apple.lsd
com.apple.powerd com.apple.analyticsd com.apple.logd_helper
com.apple.iconservices.iconservicesagent com.apple.pluginkit.pkd
com.apple.SpringBoard com.apple.wifid'''.split()


def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]


# --- cache files and mappings -------------------------------------------------------------------
mainb = open(D + 'dyld_shared_cache_arm64e', 'rb').read()
files = ['dyld_shared_cache_arm64e']
so, sc = u32(mainb, 392), u32(mainb, 396)
for i in range(sc):
    e = mainb[so + 56 * i: so + 56 * (i + 1)]
    files.append('dyld_shared_cache_arm64e' + e[24:56].split(b'\0')[0].decode())
maps = []          # (addr, size, file, fileoff, initProt)
for fn in files:
    with open(D + fn, 'rb') as f:
        hb = f.read(0x8000)
    mo, mc = u32(hb, 312), u32(hb, 316)
    for i in range(mc):
        addr, size, foff, sio, sis, flags, maxp, initp = struct.unpack_from('<QQQQQQII', hb, mo + 56 * i)
        maps.append((addr, size, fn, foff, initp, flags))
maps.sort()
handles = {}


def a2f(addr):
    for a, s, fn, fo, p, fl in maps:
        if a <= addr < a + s:
            return fn, fo + addr - a
    return None


def read_at(addr, n):
    fn, off = a2f(addr)
    f = handles.get(fn) or handles.setdefault(fn, open(D + fn, 'rb'))
    f.seek(off)
    return f.read(n)


# --- images --------------------------------------------------------------------------------------
io_, ic = u32(mainb, 448), u32(mainb, 452)
images = []
for i in range(ic):
    addr, mtime, inode, poff = struct.unpack_from('<QQQI', mainb, io_ + 32 * i)
    path = mainb[poff:mainb.index(b'\0', poff)].decode()
    images.append((addr, path))


def parse_macho(b):
    """(segments [(name, vmaddr, vmsize, fileoff, filesize)], deps [path], install_name)"""
    magic, cpu, sub, ftype, ncmds, sizeofcmds = struct.unpack_from('<IiiIII', b, 0)
    segs, deps, idn = [], [], None
    o = 32
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<II', b, o)
        if cmd == LC_SEGMENT_64:
            name = b[o + 8:o + 24].split(b'\0')[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', b, o + 24)
            segs.append((name, vmaddr, vmsize, fileoff, filesize))
        elif cmd in DEP_CMDS or cmd == LC_ID_DYLIB:
            no = u32(b, o + 8)
            s = b[o + no:o + cs].split(b'\0')[0].decode()
            if cmd == LC_ID_DYLIB:
                idn = s
            else:
                deps.append(s)
        o += cs
    return segs, deps, idn


info = {}          # path -> (segs, deps)
alias = {}
for addr, path in images:
    hdr = read_at(addr, 32)
    sizeofcmds = u32(hdr, 20)
    b = read_at(addr, 32 + sizeofcmds)
    segs, deps, idn = parse_macho(b)
    info[path] = (segs, deps)
    if idn:
        alias[idn] = path


def realpath_in(root, p):
    """resolve symlinks of p inside root (absolute links are relative to root)"""
    parts = [x for x in p.split('/') if x]
    cur = ''
    for depth in range(64):
        if not parts:
            return cur or '/'
        nxt = cur + '/' + parts.pop(0)
        full = root + nxt
        if os.path.islink(full):
            t = os.readlink(full)
            base = '' if t.startswith('/') else cur
            parts = [x for x in (base + '/' + t).split('/') if x] + parts
            cur = ''
            norm = []
            for x in parts:
                if x == '..':
                    norm and norm.pop()
                elif x != '.':
                    norm.append(x)
            parts = norm
        else:
            cur = nxt
    return cur


unresolved = collections.Counter()


def resolve(dep):
    if dep in info:
        return dep
    if dep in alias:
        return alias[dep]
    r = realpath_in(ROOT, dep)
    if r in info:
        return r
    return None


# --- roots ----------------------------------------------------------------------------------------
def macho_deps(path):
    b = open(path, 'rb').read(1 << 20)
    magic = u32(b, 0)
    if magic in (0xbebafeca,):                     # fat, big-endian header
        n = struct.unpack_from('>I', b, 4)[0]
        best = None
        for i in range(n):
            cpu, sub, off, size, al = struct.unpack_from('>iiIII', b, 8 + 20 * i)
            if cpu == 0x0100000c:
                best = off
        if best is None:
            return []
        b = open(path, 'rb').read()[best:best + (1 << 20)]
        magic = u32(b, 0)
    if magic != 0xfeedfacf:
        return []
    return parse_macho(b)[1]


def job_programs():
    progs = {}
    for pl in glob.glob(ROOT + '/System/Library/LaunchDaemons/*.plist'):
        try:
            d = plistlib.load(open(pl, 'rb'))
        except Exception:
            continue
        lab = d.get('Label')
        prog = d.get('Program') or (d.get('ProgramArguments') or [None])[0]
        if lab and prog:
            progs[lab] = prog
    return progs


def find_file(p):
    for r in [ROOT] + CRYPTEXES:
        q = r + p
        if os.path.isfile(q):
            return q
    return None


mode = sys.argv[2] if len(sys.argv) > 2 else 'jobs'
roots = []
if mode in ('jobs', 'apps'):
    progs = job_programs()
    for j in JOBS:
        p = progs.get(j)
        print('job', j, '->', p)
        if p:
            roots.append(p)
    roots += ['/System/Library/CoreServices/SpringBoard.app/SpringBoard', '/usr/libexec/xpcproxy', '/sbin/launchd']
    if mode == 'apps':
        # the applications the 16.5 port's screenshots show (docs/*.jpg), with their extensions
        for app in ('Preferences', 'MobileSafari', 'MobileCal', 'MobileTimer', 'Health', 'Maps', 'MobileSMS',
                    'Music', 'News', 'MobilePhone', 'Reminders', 'Passbook', 'AppStore', 'MobileNotes', 'Weather'):
            for base in ('/Applications/', '/private/var/staged_system_apps/'):
                d = ROOT + base + app + '.app'
                if os.path.isdir(d):
                    for dp, dn, fns in os.walk(d):
                        for fn in fns:
                            full = os.path.join(dp, fn)
                            if '.' in fn or os.path.islink(full):
                                continue
                            with open(full, 'rb') as f:
                                if f.read(4) in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe'):
                                    roots.append(full[len(ROOT):])
else:
    for top in ('usr/libexec', 'usr/sbin', 'usr/bin', 'sbin', 'System/Library/CoreServices', 'Applications',
                'private/var/staged_system_apps', 'System/Library/PrivateFrameworks', 'System/Library/Frameworks'):
        for dp, dn, fns in os.walk(os.path.join(ROOT, top)):
            for fn in fns:
                full = os.path.join(dp, fn)
                if os.path.islink(full) or '.' in fn and not fn.endswith(('.dylib', '.bundle')):
                    continue
                try:
                    with open(full, 'rb') as f:
                        m = f.read(4)
                except Exception:
                    continue
                if m in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe'):
                    roots.append(full[len(ROOT):])

used = set()
stack = []
missing_roots = []
for r in roots:
    f = find_file(r)
    if not f:
        missing_roots.append(r)
        continue
    for d in macho_deps(f):
        x = resolve(d)
        if x:
            stack.append(x)
        else:
            unresolved[d] += 1
while stack:
    p = stack.pop()
    if p in used:
        continue
    used.add(p)
    for d in info[p][1]:
        x = resolve(d)
        if x and x not in used:
            stack.append(x)
        elif not x:
            unresolved[d] += 1
print(f'mode {mode}: {len(roots)} root programs ({len(missing_roots)} missing: {missing_roots[:8]}), '
      f'closure {len(used)} of {len(images)} images; {len(unresolved)} unresolved deps e.g. {list(unresolved)[:5]}')

# --- attribution of compressed bytes ----------------------------------------------------------------
pieces = pickle.load(open(sys.argv[1], 'rb'))
pieces = {os.path.basename(k): v for k, v in pieces.items()}
fsize = {fn: os.path.getsize(D + fn) for fn in files}


def intervals(paths, segfilter):
    out = collections.defaultdict(list)
    for p in paths:
        for name, vmaddr, vmsize, fo, fs in info[p][0]:
            if not segfilter(name) or vmsize == 0:
                continue
            # a segment may span mappings; walk it
            a, end = vmaddr, vmaddr + vmsize
            while a < end:
                hit = None
                for ma, ms, fn, mfo, pr, fl in maps:
                    if ma <= a < ma + ms:
                        hit = (ma, ms, fn, mfo)
                        break
                if not hit:
                    break
                ma, ms, fn, mfo = hit
                e = min(end, ma + ms)
                out[fn].append((mfo + a - ma, mfo + e - ma))
                a = e
    return out


def merge(iv):
    iv = sorted(iv)
    res = []
    for s, e in iv:
        if res and s <= res[-1][1]:
            res[-1][1] = max(res[-1][1], e)
        else:
            res.append([s, e])
    return res


def subtract(a, b):
    """a minus b, both merged"""
    res = []
    j = 0
    for s, e in a:
        cur = s
        while j < len(b) and b[j][1] <= cur:
            j += 1
        k = j
        while k < len(b) and b[k][0] < e:
            if b[k][0] > cur:
                res.append([cur, b[k][0]])
            cur = max(cur, b[k][1])
            k += 1
        if cur < e:
            res.append([cur, e])
    return res


def zbytes(fn, ivs):
    """prorated compressed bytes and raw bytes covered by intervals"""
    pc = pieces[fn]
    z = r = 0
    for s, e in ivs:
        r += e - s
        c = s // PIECE
        while c * PIECE < e:
            cs, ce = c * PIECE, min((c + 1) * PIECE, fsize[fn])
            ov = min(e, ce) - max(s, cs)
            if ov > 0 and c < len(pc):
                z += pc[c] * ov / (ce - cs)
            c += 1
    return z, r


def full_chunks(fn, ivs):
    """compressed bytes of the pieces wholly inside the intervals (what zero-filling surely saves)"""
    pc = pieces[fn]
    z = 0
    for s, e in ivs:
        c0 = -(-s // PIECE)
        c1 = e // PIECE
        for c in range(c0, c1):
            if c < len(pc):
                z += pc[c] - 30
    return z


unused = set(info) - used
per_image = lambda n: not n.startswith('__LINKEDIT')
U = intervals(used, per_image)
X = intervals(unused, per_image)
M = 1 << 20
kinds = collections.defaultdict(lambda: [0, 0, 0, 0, 0, 0])
for fn in files:
    kind = fn.split('.')[-1] if fn.endswith(('dylddata', 'dyldlinkedit', 'dyldreadonly')) else 'text'
    u = merge(U.get(fn, []))
    x = subtract(merge(X.get(fn, [])), u)
    zu, ru = zbytes(fn, u)
    zx, rx = zbytes(fn, x)
    fz = full_chunks(fn, x)
    k = kinds[kind]
    k[0] += fsize[fn]; k[1] += sum(pieces[fn]); k[2] += ru; k[3] += zu; k[4] += rx; k[5] += zx
    kinds[kind + '_fullsave'][0] += fz
print(f'{"subcache kind":14s} {"raw":>9s} {"z9":>9s} | {"used raw":>9s} {"used z9":>9s} | {"unused raw":>10s} {"unused z9":>9s}  (MiB)')
for k in ('text', 'dylddata', 'dyldreadonly', 'dyldlinkedit'):
    v = kinds[k]
    print(f'{k:14s} {v[0]/M:9.1f} {v[1]/M:9.1f} | {v[2]/M:9.1f} {v[3]/M:9.1f} | {v[4]/M:10.1f} {v[5]/M:9.1f}  '
          f'whole-piece saving {kinds[k + "_fullsave"][0]/M:.1f}')
tot = [sum(kinds[k][i] for k in ('text', 'dylddata', 'dyldreadonly', 'dyldlinkedit')) for i in range(6)]
print(f'{"ALL":14s} {tot[0]/M:9.1f} {tot[1]/M:9.1f} | {tot[2]/M:9.1f} {tot[3]/M:9.1f} | {tot[4]/M:10.1f} {tot[5]/M:9.1f}  '
      f'whole-piece saving {sum(kinds[k + "_fullsave"][0] for k in ("text","dylddata","dyldreadonly","dyldlinkedit"))/M:.1f}')

# segment-name totals for all images (raw), to show what lives where
segtot = collections.Counter()
for p in info:
    for name, vmaddr, vmsize, fo, fs in info[p][0]:
        if not name.startswith('__LINKEDIT'):
            segtot[name] += vmsize
print('per-image segment totals (raw MiB, all images):', {k: round(v / M, 1) for k, v in segtot.most_common()})
cat = collections.Counter()
for p in used:
    cat[p.split('/')[1] + '/' + (p.split('/')[3] if p.startswith('/System/Library/') else p.split('/')[2])] += 1
print('used images by directory:', cat.most_common(12))
pickle.dump({'used': sorted(used), 'mode': mode}, open(cachedir.path(f'used_{mode}.pkl'), 'wb'))
