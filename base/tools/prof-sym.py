#!/usr/bin/env python3
"""tools/prof-sym.py LOG [CACHE]: the samples of osfmk/arm64/hakios_prof.c (boot-arg hakios_prof=NAME) summed over every
report in LOG and named: user PCs in the dyld shared cache through `ipsw dyld a2s` (CACHE, default
ios26pro/23G90__iPhone18,1/dyld_shared_cache_arm64e), in our own images by their symbol tables (userland/obj or
xnu-deps/build/mesa26 for libredwoodair), kernel PCs as they are. Prints functions by samples."""
import collections, os, re, subprocess, sys

log = sys.argv[1]
cache = sys.argv[2] if len(sys.argv) > 2 else 'ios26pro/23G90__iPhone18,1/dyld_shared_cache_arm64e'
OWN = {'libredwoodair.dylib': 'xnu-deps/build/mesa26/build/src/gallium/drivers/freedreno/libredwoodair.dylib',
       'libredwood26.dylib': 'userland/obj/libredwood26.dylib'}
pcs, images, slide, total = collections.Counter(), {}, 0, 0
for line in open(log, errors='replace'):
    m = re.search(r'dprof: ([ukl]) (0x[0-9a-f]+) (\d+)', line)
    if m:
        pcs[(m.group(1), int(m.group(2), 16))] += int(m.group(3))
        continue
    m = re.search(r'dprof: image (0x[0-9a-f]+) (\S+)', line)
    if m:
        images[os.path.basename(m.group(2))] = int(m.group(1), 16)
        continue
    m = re.search(r'dprof: images \d+, shared cache at (0x[0-9a-f]+) slide (0x[0-9a-f]+)', line)
    if m:
        slide = int(m.group(2), 16)
    m = re.search(r'dprof: \S+: (\d+) samples', line)
    if m:
        total += int(m.group(1))

def own_symbols(path):
    syms = []
    if os.path.exists(path):
        for l in subprocess.run(['nm', '-n', path], capture_output=True, text=True).stdout.splitlines():
            p = l.split()
            if len(p) >= 3 and p[1] in 'tT':
                syms.append((int(p[0], 16), p[2]))
    return syms
own = {name: own_symbols(path) for name, path in OWN.items()}

def name_of(kind, pc):
    if kind == 'k':
        return 'kernel %#x' % pc
    for img, base in images.items():
        if img in own and base <= pc < base + 0x2000000 and own[img]:
            off = pc - base + 0x0  # (the images are linked at 0)
            best = max((s for s in own[img] if s[0] <= off), default=None)
            if best:
                return '%s`%s' % (img, best[1])
    try:
        out = subprocess.run(['ipsw', 'dyld', 'a2s', cache, hex(pc - slide), '--cache', '/tmp/dsc.a2s', '-i'],
                             capture_output=True, text=True, timeout=60).stdout.strip().splitlines()
        return out[-1].strip() if out else '? %#x' % pc
    except Exception as e:
        return '? %#x (%s)' % (pc, e)

funcs, callers = collections.Counter(), collections.Counter()
for (kind, pc), n in pcs.most_common(200):
    nm = name_of('u' if kind == 'l' else kind, pc)
    if kind == 'l':
        callers[re.sub(r'\s*\+\s*\d+$', '', re.sub(r'^.*?: ', '', nm))] += n
        continue
    nm = re.sub(r'\s*\+\s*\d+$', '', re.sub(r'^.*?: ', '', nm))
    funcs[nm] += n
print('%d samples in %d reports' % (total, 0))
for nm, n in funcs.most_common(60):
    print('%6d %5.1f%%  %s' % (n, 100.0 * n / max(total, 1), nm))
print('callers (the LR of user samples):')
for nm, n in callers.most_common(40):
    print('%6d %5.1f%%  %s' % (n, 100.0 * n / max(total, 1), nm))
