#!/usr/bin/env python3
"""Take a .metallib apart: the MTLB container, its function list, and each function's LLVM bitcode.

The container was worked out by hand against the files of iOS 16.5; what is in it, and what the
numbers below mean, is written down in docs/metal.md (section 9). Every module of every .metallib
in the extracted system is ordinary LLVM bitcode in the standard 0x0B17C0DE wrapper, so after
"metallib.py dump" an ordinary llvm-dis reads them.

usage: metallib.py list FILE                  the functions, their type, air version, module size
       metallib.py dump FILE OUTDIR [N]       write each function's bitcode as OUTDIR/<name>.bc
"""
import os
import struct
import sys

# The TYPE tag. Corrected against the metadata of all 456 modules: 4 is what
# the 135 stitchable functions of PortraitFilters carry, and every one of them
# says !air.visible, so 4 is "visible" and 3 is not.
TYPES = {0: 'vertex', 1: 'fragment', 2: 'kernel', 3: 'unqualified', 4: 'visible',
         5: 'extern', 6: 'intersection'}


def parse(path):
    b = open(path, 'rb').read()
    assert b[:4] == b'MTLB', 'not a metallib: %r' % b[:4]
    (ver_maj, ver_min) = struct.unpack_from('<HH', b, 4)
    total = struct.unpack_from('<Q', b, 0x10)[0]
    funcs_off = struct.unpack_from('<Q', b, 0x18)[0]
    funcs_end = struct.unpack_from('<Q', b, 0x20)[0]
    pub_off, pub_len = struct.unpack_from('<QQ', b, 0x28)
    prv_off, prv_len = struct.unpack_from('<QQ', b, 0x38)
    bc_off, bc_len = struct.unpack_from('<QQ', b, 0x48)
    header = dict(version=(ver_maj, ver_min), total=total, file=len(b),
                  funcs=(funcs_off, funcs_end), public=(pub_off, pub_len),
                  private=(prv_off, prv_len), bitcode=(bc_off, bc_len))

    p = funcs_off
    count = struct.unpack_from('<I', b, p)[0]
    p += 4
    out = []
    for _ in range(count):
        size = struct.unpack_from('<I', b, p)[0]      # counts from this field itself
        q, end = p + 4, p + size
        f = {}
        while q < end:
            tag = b[q:q + 4]
            if tag == b'ENDT':
                break
            ln = struct.unpack_from('<H', b, q + 4)[0]
            val = b[q + 6:q + 6 + ln]
            if tag == b'NAME':
                f['name'] = val.rstrip(b'\0').decode('utf-8', 'replace')
            elif tag == b'TYPE':
                f['type'] = TYPES.get(val[0], 'type%d' % val[0])
            elif tag == b'OFFT':
                f['offt'] = struct.unpack('<QQQ', val)
            elif tag == b'VERS':
                f['vers'] = struct.unpack('<HHHH', val)
            elif tag == b'MDSZ':
                f['mdsz'] = struct.unpack('<Q', val)[0]
            elif tag == b'HASH':
                f['hash'] = val.hex()[:16]
            q += 6 + ln
        out.append(f)
        p = end
    return b, header, out


def bitcode_of(b, header, f):
    """The function's module, unwrapped from the LLVM bitcode wrapper if it has one."""
    start = header['bitcode'][0] + f['offt'][2]
    blob = b[start:start + f['mdsz']]
    if blob[:4] == bytes((0xde, 0xc0, 0x17, 0x0b)):          # 0x0B17C0DE, little endian
        off, size = struct.unpack_from('<II', blob, 8)
        return blob[off:off + size], True
    return blob, False


if __name__ == '__main__':
    what, path = sys.argv[1], sys.argv[2]
    b, header, funcs = parse(path)
    if what == 'list':
        for k, v in header.items():
            print('%-10s %s' % (k, v))
        print('%d functions' % len(funcs))
        for f in funcs:
            print('  %-52s %-13s air %s  module %d' % (f.get('name', '?'), f.get('type', '?'),
                                                       '.'.join(map(str, f.get('vers', ()))), f.get('mdsz', 0)))
    elif what == 'dump':
        outdir = sys.argv[3]
        limit = int(sys.argv[4]) if len(sys.argv) > 4 else len(funcs)
        os.makedirs(outdir, exist_ok=True)
        for f in funcs[:limit]:
            bc, wrapped = bitcode_of(b, header, f)
            name = f.get('name', 'f%d' % funcs.index(f)).replace('/', '_')[:120]
            with open(os.path.join(outdir, name + '.bc'), 'wb') as fp:
                fp.write(bc)
            print('%-52s %6d bytes %s %s' % (name, len(bc), 'unwrapped' if wrapped else 'raw   ', bc[:4].hex()))
