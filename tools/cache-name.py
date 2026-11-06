#!/usr/bin/env python3
"""
cache-name.py ADDRESS... (hex, unslid, as src/inject_media.c's "!media tN" lines give them): which image of
the iOS shared cache (ios165/<build>/dyld_shared_cache_arm64) each address is in, and how far into it.
A line of a log can be given whole on standard input instead: every hex word in it is named.
"""
import os, re, struct, sys

CACHE = os.path.join(os.path.dirname(__file__), '..', 'ios165', '20F66__iPhone10,3_6', 'dyld_shared_cache_arm64')

with open(CACHE, 'rb') as f:
    header = f.read(0x200)
    images_offset, images_count = struct.unpack_from('<II', header, 0x1c0)
    f.seek(images_offset)
    entries = [struct.unpack('<QQQII', f.read(32)) for _ in range(images_count)]
    images = []
    for address, _, _, path_offset, _ in entries:
        f.seek(path_offset)
        images.append((address, f.read(256).split(b'\0')[0].decode()))
images.sort()
starts = [a for a, _ in images]


def name(address):
    import bisect
    i = bisect.bisect_right(starts, address) - 1
    if i < 0 or address >= 0x300000000:
        return f'{address:#x} (not in the cache: the program itself, or a library of ours)'
    base, path = images[i]
    return f'{address:#x} {os.path.basename(path)}+{address - base:#x}'


words = sys.argv[1:] or re.findall(r'\b[0-9a-f]{6,12}\b', sys.stdin.read())
for word in words:
    print(name(int(word, 16)))
