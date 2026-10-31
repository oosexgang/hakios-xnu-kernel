#!/usr/bin/env python3
"""
font_trim.py: smaller copies of the two fonts iOS has that are too big for a system image kept in
the phone's memory, made from the files themselves (nothing of them is in the repository).

  font_trim.py emoji SRC DST [PPEM,...]
      Apple Color Emoji (AppleColorEmoji-160px.ttc, 130 MB) with only the bitmap strikes asked for
      (default 40,64,96: what the screen's text sizes use at three pixels a point; the 160-pixel
      strike alone is 71 MB). The 'sbix' table is rebuilt with those strikes, every other table is
      kept as it is; CoreText scales the nearest strike for sizes in between.

  font_trim.py faces SRC DST REGEX
      A font collection (PingFang.ttc: 18 faces, 78 MB) with only the faces whose PostScript name
      matches REGEX, and only the tables they use.

Both write a TrueType collection that CoreText reads as it reads the original; DST may be SRC.
ios165/fetch.sh makes both for the system image this way.
"""
import os
import re
import struct
import sys


def checksum(data):
    data += b'\0' * (-len(data) % 4)
    return sum(struct.unpack('>%dI' % (len(data) // 4), data)) & 0xffffffff


def read_collection(d):
    """[(sfnt version, [(tag, offset, length), ...]), ...] for each face of a TTC"""
    if d[:4] != b'ttcf':
        raise SystemExit('font_trim.py: not a TrueType collection')
    count = struct.unpack('>I', d[8:12])[0]
    faces = []
    for at in struct.unpack('>%dI' % count, d[12:12 + 4 * count]):
        version, tables = struct.unpack('>IH', d[at:at + 6])
        records = []
        for i in range(tables):
            tag, _, offset, length = struct.unpack('>4sIII', d[at + 12 + 16 * i:at + 28 + 16 * i])
            records.append((tag, offset, length))
        faces.append((version, records))
    return faces


def write_collection(faces, tables):
    """faces: [(version, [tag, ...])]; tables: {(face, tag): bytes}, identical bytes stored once"""
    header = 12 + 4 * len(faces)
    directories = [12 + 16 * len(tags) for _, tags in faces]
    at = header + sum(directories)
    at += -at % 4
    stored, blobs, place = {}, [], {}
    for f, (_, tags) in enumerate(faces):
        for tag in tags:
            blob = tables[(f, tag)]
            key = (len(blob), checksum(blob), blob[:64])
            if key not in stored or tables[stored[key]] != blob:
                stored[key] = (f, tag)
                place[(f, tag)] = at
                blobs.append(blob + b'\0' * (-len(blob) % 4))
                at += len(blobs[-1])
            else:
                place[(f, tag)] = place[stored[key]]
    out = bytearray(b'ttcf' + struct.pack('>HHI', 1, 0, len(faces)))
    face_at = header
    for size in directories:
        out += struct.pack('>I', face_at)
        face_at += size
    for f, (version, tags) in enumerate(faces):
        n = len(tags)
        power = 1 << (n.bit_length() - 1)
        out += struct.pack('>IHHHH', version, n, power * 16, power.bit_length() - 1, n * 16 - power * 16)
        for tag in sorted(tags):
            blob = tables[(f, tag)]
            out += struct.pack('>4sIII', tag, checksum(blob), place[(f, tag)], len(blob))
    out += b'\0' * (-len(out) % 4)
    for blob in blobs:
        out += blob
    return bytes(out)


def save(dst, data):
    """dst replaced whole: written beside it and renamed, so that a dst that is the source, or a hard link shared
    with another copy of the tree, is never written into"""
    with open(dst + '.part', 'wb') as f:
        f.write(data)
    os.replace(dst + '.part', dst)


def emoji(src, dst, keep):
    d = open(src, 'rb').read()
    faces = read_collection(d)
    tables = {}
    for f, (_, records) in enumerate(faces):
        for tag, offset, length in records:
            tables[(f, tag)] = d[offset:offset + length]
    maxp = tables[(0, b'maxp')]
    glyphs = struct.unpack('>H', maxp[4:6])[0]
    sbix = tables[(0, b'sbix')]
    version, flags, strikes = struct.unpack('>HHI', sbix[:8])
    offsets = struct.unpack('>%dI' % strikes, sbix[8:8 + 4 * strikes])
    kept = []
    for o in offsets:
        ppem = struct.unpack('>H', sbix[o:o + 2])[0]
        ends = struct.unpack('>%dI' % (glyphs + 1), sbix[o + 4:o + 8 + 4 * glyphs])
        if ppem in keep:
            kept.append(sbix[o:o + ends[-1]])
        print('font_trim.py: strike %3d px: %5.1f MB, %s' % (ppem, ends[-1] / 1048576, 'kept' if ppem in keep else 'left out'))
    if not kept:
        raise SystemExit('font_trim.py: none of the strikes asked for is there')
    head = 8 + 4 * len(kept)
    new = bytearray(struct.pack('>HHI', version, flags, len(kept)))
    at = head
    for strike in kept:
        new += struct.pack('>I', at)
        at += len(strike) + (-len(strike) % 4)
    for strike in kept:
        new += strike + b'\0' * (-len(strike) % 4)
    for f in range(len(faces)):
        tables[(f, b'sbix')] = bytes(new)
    out = write_collection([(v, [t for t, _, _ in r]) for v, r in faces], tables)
    save(dst, out)
    print('font_trim.py: %s: %.1f MB (was %.1f MB)' % (dst, len(out) / 1048576, len(d) / 1048576))


def postscript_name(name_table):
    fmt, count, strings = struct.unpack('>HHH', name_table[:6])
    for i in range(count):
        platform, encoding, language, name_id, length, offset = struct.unpack('>6H', name_table[6 + 12 * i:18 + 12 * i])
        if name_id == 6:
            raw = name_table[strings + offset:strings + offset + length]
            return raw.decode('utf-16-be') if platform in (0, 3) else raw.decode('mac_roman')
    return ''


def faces_of(src, dst, pattern):
    d = open(src, 'rb').read()
    faces = read_collection(d)
    chosen, tables = [], {}
    for version, records in faces:
        table = {tag: d[offset:offset + length] for tag, offset, length in records}
        name = postscript_name(table[b'name'])
        wanted = re.search(pattern, name) is not None
        print('font_trim.py: %-28s %s' % (name, 'kept' if wanted else 'left out'))
        if wanted:
            f = len(chosen)
            chosen.append((version, list(table)))
            for tag, blob in table.items():
                tables[(f, tag)] = blob
    if not chosen:
        raise SystemExit('font_trim.py: no face matches %s' % pattern)
    out = write_collection(chosen, tables)
    save(dst, out)
    print('font_trim.py: %s: %.1f MB (was %.1f MB)' % (dst, len(out) / 1048576, len(d) / 1048576))


if __name__ == '__main__':
    if len(sys.argv) >= 4 and sys.argv[1] == 'emoji':
        emoji(sys.argv[2], sys.argv[3], set(int(x) for x in (sys.argv[4] if len(sys.argv) > 4 else '40,64,96').split(',')))
    elif len(sys.argv) == 5 and sys.argv[1] == 'faces':
        faces_of(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        print(__doc__)
        sys.exit(2)
