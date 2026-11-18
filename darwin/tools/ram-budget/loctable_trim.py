#!/usr/bin/env python3
"""What keeping only English/Base in every chosen .loctable would save (mkapfs -z blocks before/after)."""
import pickle, plistlib, os, importlib.util
from multiprocessing import Pool
import cachedir
spec = importlib.util.spec_from_file_location('r', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rootfs_select.py'))
r = importlib.util.module_from_spec(spec); spec.loader.exec_module(r)
ROOT = r.ROOT + '/'
KEEP = ('en', 'Base', 'English', 'en_US', 'en_GB')


def src(rel):
    p = ROOT + rel
    if os.path.exists(p):
        return p
    if rel.startswith('Applications/'):
        q = ROOT + 'private/var/staged_system_apps/' + rel[len('Applications/'):]
        if os.path.exists(q):
            return q
    return None


def work(rel):
    p = src(rel)
    if not p:
        return rel, 0, 0, 0, 0
    _, size, b0 = r.blocks(p)
    try:
        d = plistlib.load(open(p, 'rb'))
        if not isinstance(d, dict):
            return rel, size, b0 * 4096, size, b0 * 4096
        kept = {k: v for k, v in d.items() if k in KEEP or k.startswith('Loc')}
        data = plistlib.dumps(kept, fmt=plistlib.FMT_BINARY)
    except Exception:
        return rel, size, b0 * 4096, size, b0 * 4096
    tmp = cachedir.path(f'loctable.{os.getpid()}')
    with open(tmp, 'wb') as f:
        f.write(data)
    _, s1, b1 = r.blocks(tmp)
    os.unlink(tmp)
    return rel, size, b0 * 4096, s1, b1 * 4096


if __name__ == '__main__':
    big = pickle.load(open(cachedir.path('rootfs26.pkl'), 'rb'))
    rels = [x[2] for x in big if x[2].endswith('.loctable')]
    t = [0, 0, 0, 0]
    with Pool(os.cpu_count()) as pool:
        for rel, a, b, c, d in pool.imap_unordered(work, rels, chunksize=16):
            t[0] += a; t[1] += b; t[2] += c; t[3] += d
    M = 1 << 20
    print(f'{len(rels)} loctables: raw {t[0]/M:.1f} -> {t[2]/M:.1f} MiB; z9 {t[1]/M:.1f} -> {t[3]/M:.1f} MiB')
