#!/usr/bin/env python3
# The name hashes and lookups of apfs_lib, checked independently over every name of a volume.
#
#   1. apfs-tool namehash IMAGE TSV       (a line per directory record: dir, name in hex, the hash
#                                          stored, apfs_lib's hash case-sensitive and case-insensitive,
#                                          the file's id)
#   2. test_namenorm.py TSV LIST          this: works the hashes out its own way -- Python's
#                                          unicodedata, NFD (and casefold of the NFD, NFD again, the
#                                          "canonical caseless form"), CRC-32C over UTF-32LE from all
#                                          ones, low 22 bits -- and compares them with apfs_lib's and
#                                          with the stored ones; and writes LIST: names to look up, in
#                                          other forms than stored, with what the lookup must give
#   3. apfs-tool lookupnames IMAGE LIST   looks them up through apfs_lookup
#
# What a lookup must give is worked out from the directory's own names: on a case-sensitive,
# normalization-insensitive volume (iOS) a name matches the entry whose NFD is the same; another
# composition of a stored name (NFC of an NFD name, say) finds that entry; a case variant finds
# only an entry with exactly that name; the name with "#" after it, nothing.

import struct
import sys
import unicodedata

POLY = 0x82F63B78
TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ POLY if _c & 1 else _c >> 1
    TABLE.append(_c)


def crc_hash(text):
    crc = 0xFFFFFFFF
    for ch in text:
        for b in struct.pack("<I", ord(ch)):
            crc = TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc & 0x3FFFFF


def nfd(s):
    return unicodedata.normalize("NFD", s)


def caseless(s):
    return nfd(nfd(s).casefold())


def main():
    if len(sys.argv) != 3:
        print("usage: test_namenorm.py TSV LIST", file=sys.stderr)
        return 2
    recs = []
    for line in open(sys.argv[1]):
        d, hx, stored, cs, ci, fid = line.split("\t")
        recs.append((int(d), bytes.fromhex(hx), int(stored, 16), int(cs, 16), int(ci, 16), int(fid)))
    dirs = {}
    for d, raw, *_rest, fid in recs:
        dirs.setdefault(d, {})[raw] = fid
    n = len(recs)
    other = [r for r in recs if any(b >= 0x80 for b in r[1])]
    cs_ref = ci_ref = stored_ref = undecodable = 0
    bad = []
    for d, raw, stored, cs, ci, fid in recs:
        try:
            name = raw.decode("utf-8")
        except UnicodeDecodeError:
            undecodable += 1
            continue
        rc, ri = crc_hash(nfd(name)), crc_hash(caseless(name))
        cs_ref += rc == cs
        ci_ref += ri == ci
        stored_ref += rc == stored
        if (rc != cs or ri != ci or rc != stored) and len(bad) < 10:
            bad.append((name, stored, cs, rc, ci, ri))
    print("names %d (%d not plain ASCII), not UTF-8 %d; Python's NFD hash = apfs_lib's case-sensitive %d, "
          "= stored %d; Python's caseless hash = apfs_lib's case-insensitive %d; Unicode %s here" %
          (n, len(other), undecodable, cs_ref, stored_ref, ci_ref, unicodedata.unidata_version))
    for b in bad:
        print("   %r: stored %06x, apfs_lib cs %06x, python %06x; apfs_lib ci %06x, python %06x" % b)

    # the lookups, for every name not plain ASCII and a sample of the rest
    out = open(sys.argv[2], "w")
    count = 0
    for i, (d, raw, stored, cs, ci, fid) in enumerate(recs):
        if all(b < 0x80 for b in raw) and i % 97 != 0:
            continue
        name = raw.decode("utf-8")
        by_nfd = {}
        for r, f in dirs[d].items():
            try:
                by_nfd.setdefault(nfd(r.decode("utf-8")), f)
            except UnicodeDecodeError:
                pass
        variants = {name, unicodedata.normalize("NFC", name), nfd(name), name.upper(), name.lower(),
                    name.swapcase(), name + "#"}
        for v in variants:
            want = by_nfd.get(nfd(v))
            out.write("%d\t%s\t%s\n" % (d, v.encode("utf-8").hex(), "none" if want is None else str(want)))
            count += 1
    out.close()
    print("lookups written: %d" % count)
    ok = cs_ref == n - undecodable and ci_ref == n - undecodable and stored_ref == n - undecodable
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
