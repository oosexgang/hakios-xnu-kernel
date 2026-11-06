#!/usr/bin/env python3
# The hash in a directory record's key, worked out the way apfs_lib.c's lookup works it
# out, against the hash APFS stored with every name of a volume that is plain ASCII:
# CRC-32C over the name as little-endian 32-bit code points, started at all ones and not
# inverted at the end, low 22 bits.  (Other names are hashed after Unicode canonical
# decomposition, which the lookup does not do: it reads the directory through for them.)
# Also that the records of a directory sort by the word holding hash and length, then by
# name -- what lets the lookup stop at the first record past the hash.
#
# usage: test_namehash.py IMAGE

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import apfs

POLY = 0x82F63B78
TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ POLY if _c & 1 else _c >> 1
    TABLE.append(_c)


def name_hash(name):
    crc = 0xFFFFFFFF
    for ch in name:
        for b in struct.pack("<I", ord(ch)):
            crc = TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc & 0x3FFFFF


def main():
    if len(sys.argv) != 2:
        print("usage: test_namehash.py IMAGE", file=sys.stderr)
        return 2
    c = apfs.Container(sys.argv[1])
    vol = next(iter(c.volumes()))
    if vol.case_insensitive:
        print("a case-insensitive volume: names are folded before hashing; not checked here")
        return 2
    total = ascii_n = good = 0
    bad = []
    disorder = 0
    prev = None
    for k, _v in vol.tree.iter_all():
        x = struct.unpack_from("<Q", k, 0)[0]
        oid, typ = x & apfs.OBJ_ID_MASK, x >> apfs.OBJ_TYPE_SHIFT
        if typ != apfs.TYPE_DIR_REC:
            prev = None
            continue
        nlh = struct.unpack_from("<I", k, 8)[0]
        raw = k[12:12 + (nlh & apfs.J_DREC_LEN_MASK)].split(b"\0", 1)[0]
        total += 1
        if prev is not None and prev[0] == oid and prev[1:] > (nlh, raw):
            disorder += 1
        prev = (oid, nlh, raw)
        try:
            name = raw.decode("ascii")
        except UnicodeDecodeError:
            continue
        ascii_n += 1
        if name_hash(name) == nlh >> apfs.J_DREC_HASH_SHIFT and \
                (nlh & apfs.J_DREC_LEN_MASK) == len(raw) + 1:
            good += 1
        elif len(bad) < 10:
            bad.append((name, nlh))
    print("directory records %d, plain ASCII %d, hash and length as worked out %d, not %d;"
          " out of order %d" % (total, ascii_n, good, ascii_n - good, disorder))
    for name, nlh in bad:
        print("   %r: stored %06x, worked out %06x" % (name, nlh >> 10, name_hash(name)))
    return 0 if good == ascii_n and disorder == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
