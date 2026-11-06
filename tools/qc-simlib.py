#!/usr/bin/env python3
"""
qc-simlib.py SIM_QUARTZCORE.metallib OUT.metallib [EXTRA.metallib]

QuartzCore's shaders for CoreAnimation's Metal renderer on the phone (camtl=1,
userland/build.sh QC_METALLIB), from the iOS SIMULATOR's QuartzCore: Xcode 14.3's iOS 16.4
runtime has a default.metallib in AIR (air64-apple-ios16.4.0-simulator) with QuartzCore's
functions in their iOS form -- edr_scale, p3_clamp, harmony, luma_boost and display_overdrive_*
included, which tools/qc-metallib.py could only alias from macOS. The header's simulator
platform becomes iOS 16.5, which is what Metal on the phone takes.

16.5 asks for a few functions 16.4 has not got (path_blit_vert_lph and the rest of path_*);
EXTRA (qc-metallib.py's output) supplies every function the simulator's library lacks whose
module is not zeroed: its entry, metadata, bitcode and reflection are appended.

Apple's files, so not kept in the repository (xnu-deps/sim-ios16.4, ignored).
"""
import struct, sys

def parse(path):
    b = bytes(open(path, 'rb').read())
    assert b[:4] == b'MTLB', path
    h = struct.unpack_from('<8Q', b, 0x18)   # list, public md, private md, bitcode: offset, size
    cnt = struct.unpack_from('<I', b, h[0])[0]
    q, ents = h[0] + 4, []
    for _ in range(cnt):
        size = struct.unpack_from('<I', b, q)[0]
        ents.append(bytearray(b[q:q + size]))
        q += size
    ext = b[q:h[2]]                          # the extended header's tags, then ENDT
    tags = {}
    r = 4 if ext[:4] == b'ENDT' else 0      # macOS's files put one more ENDT before the tags
    while ext[r:r + 4] != b'ENDT':
        ln = struct.unpack_from('<H', ext, r + 4)[0]
        tags[ext[r:r + 4]] = (r, ext[r + 6:r + 6 + ln])
        r += 6 + ln
    return b, h, ents, ext, tags

def tag(e, name):
    r = 4
    while r < len(e):
        t = bytes(e[r:r + 4])
        if t == b'ENDT':
            break
        ln = struct.unpack_from('<H', e, r + 4)[0]
        if t == name:
            return r + 6, ln
        r += 6 + ln
    return None

def name(e):
    o, l = tag(e, b'NAME')
    return bytes(e[o:o + l]).rstrip(b'\0').decode()

src, dst = sys.argv[1:3]
extra = sys.argv[3] if len(sys.argv) > 3 else None
b, h, ents, ext, xt = parse(src)
assert b[0xb] == 0x87, 'not an iOS simulator library (platform %#x)' % b[0xb]
pub = bytearray(b[h[2]:h[2] + h[3]])
priv = bytearray(b[h[4]:h[4] + h[5]])
code = bytearray(b[h[6]:h[6] + h[7]])
ro, rl = struct.unpack_from('<QQ', xt[b'RLST'][1])
refl = bytearray(b[ro:ro + rl])

if extra:
    eb, eh, eents, _, et = parse(extra)
    have = set(name(e) for e in ents)
    ero, erl = struct.unpack_from('<QQ', et[b'RLST'][1])
    # a blob runs from its offset to the next one any entry uses (or the section's end)
    starts = [sorted(set(struct.unpack_from('<QQQ', e, tag(e, b'OFFT')[0])[k] for e in eents)) for k in range(3)]
    rstarts = sorted(set(struct.unpack_from('<Q', e, tag(e, b'RFLT')[0])[0] for e in eents))
    def blob(sec_off, sec_size, starts, off):
        i = starts.index(off)
        end = starts[i + 1] if i + 1 < len(starts) else sec_size
        return eb[sec_off + off:sec_off + end]
    moved = [{}, {}, {}, {}]
    added = 0
    for e in eents:
        n = name(e)
        if n in have:
            continue
        o3 = list(struct.unpack_from('<QQQ', e, tag(e, b'OFFT')[0]))
        sz = struct.unpack_from('<Q', e, tag(e, b'MDSZ')[0])[0]
        if eb[eh[6] + o3[2]:eh[6] + o3[2] + 16] == bytes(16):
            continue   # a module qc-metallib.py zeroed: nothing to take
        e = bytearray(e)
        for k, (sec, so, ss) in enumerate(((pub, eh[2], eh[3]), (priv, eh[4], eh[5]))):
            if o3[k] not in moved[k]:
                moved[k][o3[k]] = len(sec)
                sec += blob(so, ss, starts[k], o3[k])
            o3[k] = moved[k][o3[k]]
        if o3[2] not in moved[2]:
            moved[2][o3[2]] = len(code)
            code += eb[eh[6] + o3[2]:eh[6] + o3[2] + sz]
        o3[2] = moved[2][o3[2]]
        struct.pack_into('<QQQ', e, tag(e, b'OFFT')[0], *o3)
        rf = struct.unpack_from('<Q', e, tag(e, b'RFLT')[0])[0]
        if rf not in moved[3]:
            moved[3][rf] = len(refl)
            refl += blob(ero, erl, rstarts, rf)
        struct.pack_into('<Q', e, tag(e, b'RFLT')[0], moved[3][rf])
        ents.append(e)
        added += 1
        print('from', extra.split('/')[-1] + ':', n)
    print(added, 'functions added')

table = struct.pack('<I', len(ents)) + b''.join(bytes(e) for e in ents)
fo = 0x58
po = fo + len(table) + len(ext)
pv = po + len(pub)
co = pv + len(priv)
rlo = co + len(code)
ext = bytearray(ext)
struct.pack_into('<QQ', ext, xt[b'RLST'][0] + 6, rlo, len(refl))
out = bytearray(b[:fo]) + table + ext + pub + priv + code + refl
out[0:16] = bytes.fromhex('4d544c42010002000700008210000500')   # iOS 16.5, as qc-metallib.py writes
struct.pack_into('<Q', out, 0x10, len(out))
# the list's size leaves out its count, as Apple's own files have it
struct.pack_into('<8Q', out, 0x18, fo, len(table) - 4, po, len(pub), pv, len(priv), co, len(code))
open(dst, 'wb').write(out)
print('%s: %d functions, %d bytes' % (dst, len(ents), len(out)))
