#!/usr/bin/env python3
# Check tools/apfs/apfs.py against something that is not us.
#
# Two independent references, both already in ios165/:
#
#   listing  ios165/rootfs.slt -- what 7-Zip's own APFS reader saw in the same image
#            (`7z l -slt rootfs.dmg`, made by ios165/fetch.sh).  Covers every entry in
#            the volume: path, mode, owner, size, symbolic-link target, inode number.
#
#   content  ios165/rootfs/ -- the files 7-Zip actually extracted, compared byte for
#            byte.  This is the one that proves we decompress correctly.
#
#   clines   the same lines tools/apfs/apfs-tool prints, so the C code that goes into the
#            kernel can be diffed against this reader over a whole tree.
#
#   sum      those same lines folded the way the kernel folds them (boot-arg apfs_check):
#            each line hashed on its own and the hashes added together.  Printing 318098
#            lines out of a guest over a serial line is 47 MB, so the driver running in
#            QEMU reports this sum instead and it is compared with this one.  Addition
#            does not care about the order the entries came in, which the two walks need
#            not share.
#
# usage: verify.py listing IMAGE SLT
#        verify.py content IMAGE REFDIR [-n COUNT] [--all]
#        verify.py sample  IMAGE REFDIR            (a few dozen files of every kind)
#        verify.py clines  IMAGE [PATH]
#        verify.py sum     IMAGE [PATH] [--depth N]

import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import apfs

SYSTEM_OBJ_ID_MARK = 0x0FFFFFFF00000000

# ios165/fetch.sh cuts these two fonts down after extracting them, so what is on disk in
# the reference tree is deliberately not what is in the image
TRIMMED = {
    "System/Library/Fonts/CoreAddition/AppleColorEmoji-160px.ttc",
    "System/Library/Fonts/LanguageSupport/PingFang.ttc",
}


def mode_string(mode):
    """The same spelling 7-Zip uses, so the two can be compared as text."""
    kind = {apfs.S_IFDIR: "d", apfs.S_IFLNK: "l", apfs.S_IFREG: "-",
            apfs.S_IFIFO: "p", apfs.S_IFCHR: "c", apfs.S_IFBLK: "b",
            apfs.S_IFSOCK: "s"}.get(mode & apfs.S_IFMT, "?")
    s = kind
    for who, (r, w, x) in ((6, (0o400, 0o200, 0o100)), (3, (0o40, 0o20, 0o10)),
                           (0, (0o4, 0o2, 0o1))):
        s += "r" if mode & r else "-"
        s += "w" if mode & w else "-"
        if who == 6 and mode & 0o4000:
            s += "s" if mode & x else "S"
        elif who == 3 and mode & 0o2000:
            s += "s" if mode & x else "S"
        elif who == 0 and mode & 0o1000:
            s += "t" if mode & x else "T"
        else:
            s += "x" if mode & x else "-"
    return s


def read_slt(path):
    """
    The entries of a `7z l -slt` listing.  Everything before the first entry that has a
    Mode belongs to the image and the container, not to the file system.
    """
    out = {}
    cur = {}
    with open(path, encoding="utf-8", errors="surrogateescape") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                if "Mode" in cur and "Path" in cur:
                    out[cur["Path"]] = cur
                cur = {}
                continue
            if line.startswith("----") or line == "--":
                if "Mode" in cur and "Path" in cur:
                    out[cur["Path"]] = cur
                cur = {}
                continue
            if " = " in line:
                k, v = line.split(" = ", 1)
                cur[k] = v
            elif line.endswith(" ="):
                cur[line[:-2]] = ""
    if "Mode" in cur and "Path" in cur:
        out[cur["Path"]] = cur
    return out


def build_tree(vol, note=None):
    """Every path in the volume -> (file_id, d_type), by walking the directory tree."""
    paths = {}
    stack = [(apfs.ROOT_DIR_INO_NUM, "")]
    n = 0
    while stack:
        oid, prefix = stack.pop()
        for name, fid, dt in vol.dir_entries(oid):
            p = (prefix + "/" + name) if prefix else name
            paths[p] = (fid, dt)
            n += 1
            if dt == 4:
                stack.append((fid, p))
            if note and n % 20000 == 0:
                sys.stderr.write("\r  walked %d entries..." % n)
                sys.stderr.flush()
    if note:
        sys.stderr.write("\r  walked %d entries.  \n" % n)
    return paths


def open_volume(image, want=None):
    c = apfs.Container(image)
    c.blocks.cache_limit = 60000        # the whole file-system tree fits, so no evictions
    v = apfs.pick_volume(c, want)
    return c, v


def cmd_listing(image, slt):
    print("reference: %s" % slt)
    ref = read_slt(slt)
    print("  %d file-system entries in the 7-Zip listing" % len(ref))
    c, v = open_volume(image)
    print("volume %r" % v.volname)
    mine = build_tree(v, note=True)
    print("  %d entries from our reader" % len(mine))

    only_ref = sorted(set(ref) - set(mine))
    only_mine = sorted(set(mine) - set(ref))
    print()
    print("paths only 7-Zip saw : %d" % len(only_ref))
    for p in only_ref[:10]:
        print("    %s" % p)
    print("paths only we saw    : %d" % len(only_mine))
    for p in only_mine[:10]:
        print("    %s" % p)

    both = sorted(set(ref) & set(mine))
    bad = {"mode": [], "size": [], "uid": [], "gid": [], "link": [], "inode": [],
           "noinode": []}
    checked = 0
    for p in both:
        r = ref[p]
        fid, dt = mine[p]
        ino = v.inode(fid)
        if ino is None:
            bad["noinode"].append(p)
            continue
        checked += 1
        if mode_string(ino.mode) != r["Mode"]:
            bad["mode"].append((p, mode_string(ino.mode), r["Mode"]))
        if (fid & ~SYSTEM_OBJ_ID_MARK) != int(r["iNode"]):
            bad["inode"].append((p, fid & ~SYSTEM_OBJ_ID_MARK, r["iNode"]))
        if r.get("User ID") and int(r["User ID"]) != ino.owner:
            bad["uid"].append((p, ino.owner, r["User ID"]))
        if r.get("Group ID") and int(r["Group ID"]) != ino.group:
            bad["gid"].append((p, ino.group, r["Group ID"]))
        if ino.is_link:
            got = (ino.symlink_target_bytes() or b"").decode("utf-8", "surrogateescape")
            if got != r.get("Symbolic Link", ""):
                bad["link"].append((p, got, r.get("Symbolic Link")))
        elif ino.is_reg and r.get("Size"):
            # 7-Zip reports a symbolic link's size including the trailing zero byte, so
            # only regular files can be compared on size directly
            if int(r["Size"]) != ino.size:
                bad["size"].append((p, ino.size, r["Size"]))
        if checked % 20000 == 0:
            sys.stderr.write("\r  compared %d entries..." % checked)
            sys.stderr.flush()
    sys.stderr.write("\r")
    print()
    print("compared %d entries that both readers saw:" % checked)
    for what in ("mode", "size", "uid", "gid", "link", "inode"):
        rows = bad[what]
        print("  %-6s differing: %d" % (what, len(rows)))
        for row in rows[:8]:
            print("      %s: ours %r, 7-Zip %r" % row)
    print("  entries with no inode record: %d" % len(bad["noinode"]))
    for p in bad["noinode"][:8]:
        print("      %s" % p)
    print()
    print("blocks: %d checksums verified, %d bad" % (c.blocks.cksum_checked,
                                                     c.blocks.cksum_bad))
    total_bad = sum(len(bad[k]) for k in bad)
    print("RESULT: %s" % ("everything matched" if total_bad == 0 and not only_ref
                          else "%d differences, %d paths only 7-Zip saw"
                               % (total_bad, len(only_ref))))
    c.close()
    return 0 if (total_bad == 0 and not only_ref) else 1


def classify(vol, ino):
    """What kind of file this is, for picking a spread to compare."""
    if ino.is_link:
        return "symlink"
    if not ino.is_reg:
        return "other"
    h = ino.decmpfs
    if h is not None and len(h) >= 16 and \
            struct.unpack_from("<I", h, 0)[0] == apfs.DECMPFS_MAGIC:
        return "decmpfs-%d" % struct.unpack_from("<I", h, 4)[0]
    if ino.is_sparse:
        return "sparse"
    if ino.size == 0:
        return "empty"
    if ino.size > (1 << 20):
        return "plain-large"
    return "plain"


def _compare(vol, fid, ref_path):
    ino = vol.inode(fid)
    if ino is None:
        return "no inode record", None
    try:
        got = ino.contents()
    except Exception as e:                      # noqa: BLE001 - report, do not stop
        return "read failed: %s" % e, classify(vol, ino)
    kind = classify(vol, ino)
    with open(ref_path, "rb") as f:
        want = f.read()
    if got == want:
        return None, kind
    if len(got) != len(want):
        return "length %d, reference %d" % (len(got), len(want)), kind
    for i, (a, b) in enumerate(zip(got, want)):
        if a != b:
            return "first difference at byte %d of %d" % (i, len(want)), kind
    return "differs", kind


def cmd_content(image, refdir, limit, do_all, stratified):
    c, v = open_volume(image)
    print("volume %r, reference tree %s" % (v.volname, refdir))
    mine = build_tree(v, note=True)

    # every regular file that exists in both, minus the two fonts fetch.sh rewrites
    cands = []
    for root, _dirs, files in os.walk(refdir):
        for fn in files:
            full = os.path.join(root, fn)
            rel = os.path.relpath(full, refdir)
            if rel in TRIMMED or rel not in mine:
                continue
            cands.append((rel, full, mine[rel][0]))
    print("  %d files present in both the image and the reference tree" % len(cands))

    if stratified:
        # a few of every kind, smallest to largest, so the sample is not all one shape
        by_kind = {}
        rnd = random.Random(20231017)
        rnd.shuffle(cands)
        for rel, full, fid in cands:
            ino = v.inode(fid)
            if ino is None:
                continue
            k = classify(v, ino)
            by_kind.setdefault(k, [])
            if len(by_kind[k]) < 200:
                by_kind[k].append((rel, full, fid, ino.size))
        pick = []
        for k in sorted(by_kind):
            rows = sorted(by_kind[k], key=lambda r: r[3])
            take = min(len(rows), 12)
            # the smallest, the largest, and a spread in between
            idx = sorted({round(i * (len(rows) - 1) / max(1, take - 1))
                          for i in range(take)})
            for i in idx:
                pick.append((rows[i][0], rows[i][1], rows[i][2], k))
        cands = pick
        print("  stratified sample: %d files over %d kinds" % (len(cands), len(by_kind)))
    elif not do_all:
        rnd = random.Random(20231017)
        rnd.shuffle(cands)
        cands = cands[:limit]
        print("  random sample: %d files" % len(cands))

    ok = 0
    fails = []
    kinds = {}
    total_bytes = 0
    for i, row in enumerate(cands):
        rel, full, fid = row[0], row[1], row[2]
        err, kind = _compare(v, fid, full)
        kinds.setdefault(kind, [0, 0])
        if err is None:
            ok += 1
            kinds[kind][0] += 1
            total_bytes += os.path.getsize(full)
        else:
            kinds[kind][1] += 1
            fails.append((rel, kind, err))
        if (i + 1) % 200 == 0:
            sys.stderr.write("\r  compared %d/%d, %d bytes..." % (i + 1, len(cands),
                                                                  total_bytes))
            sys.stderr.flush()
    sys.stderr.write("\r")
    print()
    print("compared %d files, %d bytes (%.2f GB)"
          % (len(cands), total_bytes, total_bytes / 1e9))
    print("  identical: %d    differing: %d" % (ok, len(fails)))
    print("  by kind:")
    for k in sorted(kinds):
        good, bad = kinds[k]
        print("    %-14s %6d identical, %4d differing" % (k, good, bad))
    if fails:
        print("  differences (first 25):")
        for rel, kind, err in fails[:25]:
            print("    [%s] %s: %s" % (kind, rel, err))
    print()
    print("blocks: %d checksums verified, %d bad" % (c.blocks.cksum_checked,
                                                     c.blocks.cksum_bad))
    print("RESULT: %s" % ("every compared file is byte-for-byte identical" if not fails
                          else "%d of %d files differ" % (len(fails), len(cands))))
    c.close()
    return 0 if not fails else 1


FNV_INIT = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1


def fnv1a(buf, h=FNV_INIT):
    for b in buf:
        h = ((h ^ b) * FNV_PRIME) & MASK64
    return h


def cmd_sum(image, where, max_depth=0):
    """The totals the kernel's apfs_check prints, computed the same way."""
    import io
    out = io.StringIO()
    rc = _clines(image, where, out, max_depth)
    if rc:
        return rc
    total = 0
    entries = dirs = files = links = others = 0
    content = 0
    for line in out.getvalue().splitlines():
        total = (total + fnv1a((line + "\n").encode())) & MASK64
        entries += 1
        f = line.split("\t")
        mode = int(f[2], 8)
        if (mode & 0o170000) == 0o040000:
            dirs += 1
        elif (mode & 0o170000) == 0o120000:
            links += 1
            content += int(f[5])
        elif (mode & 0o170000) == 0o100000:
            files += 1
            content += int(f[5])
        else:
            others += 1
    print("entries %d  dirs %d  files %d  symlinks %d  other %d"
          % (entries, dirs, files, links, others))
    print("content bytes %d" % content)
    print("line-hash sum %016x" % total)
    return 0


def cmd_clines(image, where, max_depth=0):
    """
    One line per directory entry, the same shape apfs-tool's census prints:
        path, d_type, mode, uid, gid, logical size, FNV-1a of the contents
    Only regular files and symbolic links get a hash; everything else gets zero.
    """
    c, v = open_volume(image)
    start = apfs.ROOT_DIR_INO_NUM
    prefix = ""
    if where:
        hit = v.lookup(where)
        if hit is None:
            print("verify.py: %s: not found" % where, file=sys.stderr)
            return 1
        start = hit[0]
        prefix = "/" + where.strip("/")
    return _walk_lines(v, c, start, prefix, sys.stdout, max_depth)


def _clines(image, where, out, max_depth=0):
    c, v = open_volume(image)
    start = apfs.ROOT_DIR_INO_NUM
    prefix = ""
    if where:
        hit = v.lookup(where)
        if hit is None:
            print("verify.py: %s: not found" % where, file=sys.stderr)
            return 1
        start = hit[0]
        prefix = "/" + where.strip("/")
    return _walk_lines(v, c, start, prefix, out, max_depth)


def _walk_lines(v, c, start, prefix, out, max_depth=0):
    stack = [(start, prefix, 1)]
    while stack:
        oid, px, depth = stack.pop()
        for name, fid, dt in v.dir_entries(oid):
            p = px + "/" + name
            ino = v.inode(fid)
            if ino is None:
                out.write("%s\t?\tno-inode\n" % p)
                continue
            h = 0
            if ino.is_reg or ino.is_link:
                h = fnv1a(ino.contents())
            out.write("%s\t%d\t%06o\t%d\t%d\t%d\t%016x\n"
                      % (p, dt, ino.mode, ino.owner, ino.group, ino.size, h))
            if dt == 4 and (max_depth == 0 or depth < max_depth):
                stack.append((fid, p, depth + 1))
    c.close()
    return 0


def main(argv):
    limit = 200
    depth = 0
    do_all = False
    args = []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "-n" and i + 1 < len(argv):
            limit = int(argv[i + 1])
            i += 2
            continue
        if a == "--all":
            do_all = True
            i += 1
            continue
        if a == "--depth" and i + 1 < len(argv):
            depth = int(argv[i + 1])
            i += 2
            continue
        args.append(a)
        i += 1
    if len(args) < 2 or (args[0] not in ("clines", "sum") and len(args) < 3):
        print("usage: verify.py listing IMAGE SLT\n"
              "       verify.py content IMAGE REFDIR [-n COUNT] [--all]\n"
              "       verify.py sample  IMAGE REFDIR\n"
              "       verify.py clines  IMAGE [PATH]\n"
              "       verify.py sum     IMAGE [PATH]", file=sys.stderr)
        return 2
    cmd = args[0]
    if cmd == "listing":
        return cmd_listing(args[1], args[2])
    if cmd == "content":
        return cmd_content(args[1], args[2], limit, do_all, False)
    if cmd == "sample":
        return cmd_content(args[1], args[2], limit, False, True)
    if cmd == "clines":
        return cmd_clines(args[1], args[2] if len(args) > 2 else None, depth)
    if cmd == "sum":
        return cmd_sum(args[1], args[2] if len(args) > 2 else None, depth)
    print("verify.py: unknown command %r" % cmd, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
