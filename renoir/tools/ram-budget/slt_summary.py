#!/usr/bin/env python3
"""Summarise a 7z -slt listing of an APFS image: raw/packed by category."""
import sys, collections

def entries(fn):
    e = {}
    for line in open(fn, errors='replace'):
        line = line.rstrip('\n')
        if not line:
            if 'Path' in e:
                yield e
            e = {}
        elif ' = ' in line or line.endswith(' ='):
            k, _, v = line.partition(' =')
            e[k] = v[1:] if v.startswith(' ') else v
    if 'Path' in e:
        yield e

def cat(p):
    if '/com.apple.dyld/' in p:
        n = p.rsplit('/', 1)[1]
        for s in ('dylddata', 'dyldlinkedit', 'dyldreadonly', 'symbols'):
            if n.endswith(s):
                return 'dyld.' + s
        return 'dyld.text(main+numbered)'
    parts = p.split('/')
    if parts[0] == 'System' and len(parts) > 2 and parts[1] == 'Library':
        if parts[2] in ('Frameworks', 'PrivateFrameworks', 'Fonts', 'PreferenceBundles', 'Health', 'PrivateFrameworks', 'CoreServices'):
            return 'System/Library/' + parts[2]
        return 'System/Library/(other)'
    if parts[0] == 'System' and len(parts) > 1 and parts[1] == 'Cryptexes':
        return 'Cryptexes/OS (non-dyld)'
    if parts[0] == 'usr' and len(parts) > 1:
        return 'usr/' + parts[1]
    return parts[0]

raw = collections.Counter(); packed = collections.Counter(); n = collections.Counter()
seen = set()
for e in entries(sys.argv[1]):
    if not e.get('Mode', '').startswith('-'):
        continue
    ino = e.get('iNode')
    if ino and ino in seen:
        continue
    if ino:
        seen.add(ino)
    s = int(e.get('Size') or 0); ps = e.get('Packed Size') or ''
    ps = int(ps) if ps.isdigit() else s
    c = cat(e['Path'])
    raw[c] += s; packed[c] += ps; n[c] += 1
M = 1 << 20
tr = tp = tn = 0
for c in sorted(raw, key=lambda c: -packed[c]):
    print(f'{c:40s} {n[c]:7d} files {raw[c]/M:9.1f} MiB raw {packed[c]/M:9.1f} MiB packed')
    tr += raw[c]; tp += packed[c]; tn += n[c]
print(f'{"TOTAL":40s} {tn:7d} files {tr/M:9.1f} MiB raw {tp/M:9.1f} MiB packed')
