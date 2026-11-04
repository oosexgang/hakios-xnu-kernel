#!/usr/bin/env python3
"""merge_metallib.py OUT IN... : one MTLB container (version 1.2, as iOS 16.5's) with every function of the inputs.

The layout, as the files of the image have it: a 0x58-byte header (+0x10 total size, +0x18/+0x20 the function list's
offset and the size of its entries, +0x28/+0x38/+0x48 offset and size of the public, private and bitcode sections);
at +0x58 the function count and the entries, each a u32 size counting itself, then tags (NAME TYPE HASH OFFT VERS MDSZ
RFLT ... ENDT; OFFT is the function's offsets in the public, private and bitcode sections, RFLT its offset in the
reflection list); after the entries the header's extension (RLST: the reflection list's absolute offset and size,
UUID, ENDT); then the public, private and bitcode sections, and the reflection list (a count, then entries whose u32
size counts itself) after the bitcode.
"""
import struct
import sys


def tags(b, q, end):
    out = []
    while q < end:
        tag = b[q:q + 4]
        if tag == b'ENDT':
            break
        ln = struct.unpack_from('<H', b, q + 4)[0]
        out.append((tag, b[q + 6:q + 6 + ln]))
        q += 6 + ln
    return out


def read(path):
    b = open(path, 'rb').read()
    assert b[:4] == b'MTLB'
    funcs_off, funcs_size = struct.unpack_from('<QQ', b, 0x18)
    pub_off, pub_len, prv_off, prv_len, bc_off, bc_len = struct.unpack_from('<QQQQQQ', b, 0x28)
    count = struct.unpack_from('<I', b, funcs_off)[0]
    p = funcs_off + 4
    funcs = []
    for _ in range(count):
        size = struct.unpack_from('<I', b, p)[0]
        funcs.append(tags(b, p + 4, p + size))
        p += size
    ext = dict(tags(b, p, pub_off))
    refl_off, refl_len = struct.unpack('<QQ', ext[b'RLST'])
    lib = dict(head=b[:0x10], uuid=ext.get(b'UUID'), funcs=[])
    for order in funcs:
        f = dict(order)
        po, pr, bo = struct.unpack('<QQQ', f[b'OFFT'])
        mdsz = struct.unpack('<Q', f[b'MDSZ'])[0]
        pub = b[pub_off + po:pub_off + po + struct.unpack_from('<I', b, pub_off + po)[0]]
        prv = b[prv_off + pr:prv_off + pr + struct.unpack_from('<I', b, prv_off + pr)[0]]
        rf = struct.unpack('<Q', f[b'RFLT'])[0]
        rsize = struct.unpack_from('<I', b, refl_off + rf)[0]
        lib['funcs'].append(dict(tags=order, public=pub, private=prv, bitcode=b[bc_off + bo:bc_off + bo + mdsz],
                                 reflection=b[refl_off + rf:refl_off + rf + rsize]))
    return lib


def tag(name, value):
    return name + struct.pack('<H', len(value)) + value


def write(libs, out):
    funcs = [f for lib in libs for f in lib['funcs']]
    pub = prv = bc = b''
    refl = b''
    entries = []
    for f in funcs:
        t = dict(f['tags'])
        t[b'OFFT'] = struct.pack('<QQQ', len(pub), len(prv), len(bc))
        t[b'RFLT'] = struct.pack('<Q', 4 + len(refl))
        body = b''.join(tag(k, t[k]) for k, _ in f['tags'])
        body += b'ENDT'
        entries.append(struct.pack('<I', 4 + len(body)) + body)
        pub += f['public']
        prv += f['private']
        bc += f['bitcode']
        refl += f['reflection']
    funcs_off = 0x58
    listing = struct.pack('<I', len(funcs)) + b''.join(entries)
    ext_len = (6 + 16) + (6 + 16) + 4
    pub_off = funcs_off + len(listing) + ext_len
    prv_off = pub_off + len(pub)
    bc_off = prv_off + len(prv)
    refl_off = bc_off + len(bc)
    reflection = struct.pack('<I', len(funcs)) + refl
    total = refl_off + len(reflection)
    ext = tag(b'RLST', struct.pack('<QQ', refl_off, len(reflection))) + tag(b'UUID', libs[0]['uuid']) + b'ENDT'
    head = libs[0]['head'] + struct.pack('<QQQQQQQQQ', total, funcs_off, sum(map(len, entries)), pub_off, len(pub),
                                         prv_off, len(prv), bc_off, len(bc))
    data = head + listing + ext + pub + prv + bc + reflection
    assert len(head) == 0x58 and len(data) == total
    open(out, 'wb').write(data)
    return data


if __name__ == '__main__':
    write([read(p) for p in sys.argv[2:]], sys.argv[1])
