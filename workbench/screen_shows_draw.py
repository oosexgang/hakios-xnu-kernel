#!/usr/bin/env python3
"""Whether a picture of the screen (a PNG, as tools/qemu-kernel.sh --screen takes it) is the one the shell's
`draw` puts there (tools/qemu-init/iokit.c): a white frame at the edges, and in the middle the colour the
middle is drawn in -- no red, half green, half blue -- and not the kernel's text console.

    workbench/screen_shows_draw.py SCREEN.png
"""
import struct
import sys
import zlib


def pixels(path):
    """Width, height and rows of RGB bytes of an 8-bit RGB PNG without interlacing (as qemu-kernel.sh writes)."""
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path}: not a PNG')
    at, chunks, width = 8, b'', 0
    while at < len(data):
        size, kind = struct.unpack('>I4s', data[at:at + 8])
        body = data[at + 8:at + 8 + size]
        if kind == b'IHDR':
            width, height, depth, colour = struct.unpack('>IIBB', body[:10])
            if depth != 8 or colour != 2:
                raise ValueError(f'{path}: not 8-bit RGB')
        elif kind == b'IDAT':
            chunks += body
        at += 12 + size
    raw = zlib.decompress(chunks)
    stride = 1 + 3 * width
    rows = []
    for y in range(height):
        line = raw[y * stride:(y + 1) * stride]
        if line[0] != 0:
            raise ValueError(f'{path}: a filtered row (this reads only the unfiltered ones it writes)')
        rows.append(line[1:])
    return width, height, rows


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    width, height, rows = pixels(argv[1])
    corner = tuple(rows[2][6:9])
    middle = tuple(rows[height // 2][3 * (width // 2):3 * (width // 2) + 3])
    framed = corner == (255, 255, 255)
    teal = middle[0] < 40 and abs(middle[1] - 127) < 40 and abs(middle[2] - 127) < 40
    print(f'screen_shows_draw: {width} x {height}, a corner {corner}, the middle {middle}')
    return 0 if framed and teal else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
