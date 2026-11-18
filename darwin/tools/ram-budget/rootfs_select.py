#!/usr/bin/env python3
"""Apply darwin-renoir/darwin165/select-system.py's choice (wanted_file, imported unchanged) to the
extracted Darwin 26 root filesystem and cryptexes; size the result raw and as mkapfs -z stores it."""
import os, zlib, importlib.util, collections, pickle
from multiprocessing import Pool
import cachedir

DK = os.environ.get('DK') or os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..'))
spec = importlib.util.spec_from_file_location(
    'sel', os.environ.get('DR', DK + '/darwin-renoir') + '/darwin165/select-system.py')
sel = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sel)

I = os.environ.get('DARWIN_FW', DK + '/darwin26')
ROOT = I + '/rootfs'
PIECE, BS, XMAX = 65536, 4096, 3804


def blocks(path):
    """4 KB blocks mkapfs -z gives this file (compressed_fork + zlib_rsrc logic)"""
    with open(path, 'rb') as f:
        content = f.read()
    size = len(content)
    if size == 0:
        return path, 0, 0
    if size <= PIECE and 16 + len(zlib.compress(content, 9)) <= XMAX:
        return path, size, 0
    pieces = []
    for at in range(0, size, PIECE):
        chunk = content[at:at + PIECE]
        z = len(zlib.compress(chunk, 9))
        pieces.append(z if z < len(chunk) else len(chunk) + 1)
    rsrc = 0x100 + 4 + 4 + 8 * len(pieces) + sum(pieces) + 50
    return path, size, min(-(-size // BS), -(-rsrc // BS))


def category(rel):
    p = rel.split('/')
    if rel.startswith('System/Library/') and len(p) > 3:
        if p[2] in ('Frameworks', 'PrivateFrameworks', 'Fonts', 'PreferenceBundles', 'Health', 'CoreServices'):
            return 'System/Library/' + p[2]
        return 'System/Library/(other)'
    if rel.startswith('System/Cryptexes/'):
        return '/'.join(p[:3])
    if p[0] == 'usr' and len(p) > 2:
        return 'usr/' + p[1]
    return p[0]


def main():
    chosen = []          # (relpath in image, full source path)
    skipped = collections.Counter()
    for dp, dn, fns in os.walk(ROOT):
        for fn in fns:
            full = os.path.join(dp, fn)
            if os.path.islink(full) or not os.path.isfile(full):
                continue
            rel = full[len(ROOT) + 1:]
            if rel.startswith(sel.STAGED_APPS):
                rel = 'Applications/' + rel[len(sel.STAGED_APPS):]
            elif rel.startswith('private/var/'):
                continue
            size = os.path.getsize(full)
            if sel.wanted_file(rel, size):
                chosen.append((rel, full))
            else:
                skipped[category(rel)] += size
    # the OS cryptex without its dyld cache and DriverKit (fetch.sh), the App cryptex whole
    for sub, skip in (('os', ('System/Library/Caches/com.apple.dyld/', 'System/DriverKit/')), ('app', ())):
        base = f'{I}/cryptex/{sub}'
        for dp, dn, fns in os.walk(base):
            for fn in fns:
                full = os.path.join(dp, fn)
                if os.path.islink(full) or not os.path.isfile(full):
                    continue
                rel = full[len(base) + 1:]
                if rel.startswith(skip):
                    continue
                chosen.append((f'System/Cryptexes/{sub.upper() if sub == "os" else "App"}/' + rel, full))
    src = {full: rel for rel, full in chosen}
    raw = collections.Counter(); comp = collections.Counter(); cnt = collections.Counter()
    big = []
    with Pool(os.cpu_count()) as pool:
        work = sorted(src, key=lambda f: -os.path.getsize(f))
        for path, size, b in pool.imap_unordered(blocks, work, chunksize=8):
            rel = src[path]
            c = category(rel)
            raw[c] += size; comp[c] += b * BS; cnt[c] += 1
            big.append((b * BS, size, rel))
    M = 1 << 20
    print(f'{"category":32s} {"files":>7s} {"raw MiB":>9s} {"z9 MiB":>9s}')
    for c in sorted(raw, key=lambda c: -comp[c]):
        print(f'{c:32s} {cnt[c]:7d} {raw[c]/M:9.1f} {comp[c]/M:9.1f}')
    print(f'{"TOTAL":32s} {sum(cnt.values()):7d} {sum(raw.values())/M:9.1f} {sum(comp.values())/M:9.1f}')
    print('\nbiggest chosen files (z9 MiB, raw MiB, path):')
    big.sort(reverse=True)
    for b, s, rel in big[:40]:
        print(f'  {b/M:7.1f} {s/M:7.1f}  {rel}')
    # biggest directories (depth 4) by compressed size
    dirs = collections.Counter()
    for b, s, rel in big:
        dirs['/'.join(rel.split('/')[:4])] += b
    print('\nbiggest chosen subtrees (depth 4, z9 MiB):')
    for d, b in dirs.most_common(40):
        print(f'  {b/M:7.1f}  {d}')
    print('\nskipped by the 16.5 rules (raw MiB):', {k: round(v / M) for k, v in skipped.most_common(15)})
    pickle.dump(big, open(cachedir.path('rootfs26.pkl'), 'wb'))


if __name__ == '__main__':
    main()
