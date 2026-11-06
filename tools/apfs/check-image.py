#!/usr/bin/env python3
"""Check an image made by mkapfs.py against what it was made of, by two readers that are not it.

usage: check-image.py IMAGE [VOLUME=]DIR[,manifest=FILE] ...   (volumes in the order mkapfs.py had them)

1. apfsck (linux-apfs/apfsprogs, xnu-deps/build/apfsprogs; tools/apfs/check-image.sh builds it)
   with -cw has to find the container clean: every object's checksum, the trees, the space
   manager's bitmaps against every block in use, inode and link counts, extents and references.
2. tools/apfs/apfs-tool (the kernel's own apfs_lib) lists every volume as census lines -- path,
   kind, mode, owner, group, size, FNV-1a 64 of the contents (or of a link's target) -- and they
   have to be the lines of DIR (with the manifest's owners and modes), every one, in both
   directions.
"""
import os
import stat
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import mkapfs  # noqa: E402  (the same reading of DIR and manifest)

APFSCK = os.path.join(ROOT, 'xnu-deps/build/apfsprogs/apfsck/apfsck')
TOOL = os.path.join(ROOT, 'apfs-work/apfs-tool')
FNVSUM = os.path.join(ROOT, 'apfs-work/fnvsum')


def fnv1a(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def expected(root):
    lines, files = {}, []

    def walk(node, path):
        for name in sorted(node.children):
            child = node.children[name]
            p = path + '/' + name
            if stat.S_ISDIR(child.mode):
                lines[p] = [4, child.mode, child.uid, child.gid, 0, 0]
                walk(child, p)
            elif stat.S_ISLNK(child.mode):
                lines[p] = [10, child.mode, child.uid, child.gid, len(child.target), fnv1a(child.target)]
            else:
                lines[p] = [8, child.mode, child.uid, child.gid, os.path.getsize(child.source), None]
                files.append((p, child.source))
    walk(root, '')
    if files:
        out = subprocess.run([FNVSUM], input='\n'.join(s for _, s in files) + '\n', capture_output=True,
                             text=True, check=True).stdout.splitlines()
        for (p, _), line in zip(files, out):
            lines[p][5] = int(line.split('\t')[0], 16)
    return {p: '%s\t%u\t%06o\t%u\t%u\t%u\t%016x' % (p, *v) for p, v in lines.items()}


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    image = sys.argv[1]
    if not os.path.exists(FNVSUM) or os.path.getmtime(FNVSUM) < os.path.getmtime(os.path.join(HERE, 'fnvsum.c')):
        subprocess.run(['cc', '-O2', '-o', FNVSUM, os.path.join(HERE, 'fnvsum.c')], check=True)
    r = subprocess.run([APFSCK, '-cw', image], capture_output=True, text=True)
    print('check-image.py: apfsck -cw: %s%s' % ('clean' if r.returncode == 0 else 'NOT CLEAN (exit %d)' % r.returncode,
                                               (': ' + (r.stdout + r.stderr).strip()) if (r.stdout + r.stderr).strip() else ''))
    bad = r.returncode != 0
    for index, spec in enumerate(sys.argv[2:]):
        if '=' in spec.split(',')[0]:
            spec = spec.split('=', 1)[1]
        parts = spec.split(',')
        root = mkapfs.read_tree(parts[0])
        for opt in parts[1:]:
            k, _, val = opt.partition('=')
            if k == 'manifest':
                mkapfs.apply_manifest(root, val)
        want = expected(root)
        env = dict(os.environ, APFS_TOOL_VOLUME=str(index))
        r = subprocess.run([TOOL, 'census', image], capture_output=True, text=True, env=env)
        got = {line.split('\t')[0]: line for line in r.stdout.splitlines() if line}
        missing = sorted(set(want) - set(got))
        extra = sorted(set(got) - set(want))
        differ = sorted(p for p in set(want) & set(got) if want[p] != got[p])
        ok = r.returncode == 0 and not missing and not extra and not differ
        bad |= not ok
        print('check-image.py: volume %d: %d entries read back by apfs-tool, %s' % (
            index, len(got), 'every one as in %s' % parts[0] if ok else
            '%d missing, %d extra, %d different, apfs-tool exit %d' % (len(missing), len(extra), len(differ),
                                                                         r.returncode)))
        for p in (missing[:5] + extra[:5]):
            print('  %s %s' % ('missing' if p in missing else 'extra', p))
        for p in differ[:5]:
            print('  want %s\n  got  %s' % (want[p], got[p]))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
