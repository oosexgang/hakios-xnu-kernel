#!/usr/bin/env python3
"""Make pictures of the frames the kernel's camera driver left in memory (boot-arg cam=3 and up;
bsd/dev/redwood_camera.c), as the loader hands them over ("xl camera").

usage: camraw.py CAMERA.raw PREFIX [--bayer N]

CAMERA.raw is a header { "RWCAM001", width, height, stride, format, bayer, frames, frame_bytes,
source, sensor[16], last } in its first 4096 bytes (last: which place holds the last frame written), then the frames, frame_bytes each. Format 1 is MIPI
RAW10 as the sensor sends it: four pixels in five bytes, the high eight bits of each in the first
four, their low two bits together in the fifth. Each frame becomes PREFIX-N.png, debayered by
taking each 2x2 cell (bayer 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR) as one pixel -- half the resolution, no
interpolation -- with a plain grey-world white balance and a gamma of 2.2: a first look, not a
finished photograph; PREFIX-N-raw.png is the frame's raw values as they are (grey, 8 bits).
Formats 2 and 3 are a picture iOS made of a frame (cam=5): NV12 (full-range YCbCr 4:2:0, the chroma plane
after the luma one, both of the stride) or BGRA, which becomes PREFIX.png as it is.
--bayer N takes the frames for that order instead of the header's. The header's is the one each
sensor's module file gives (tools/camsensor.py lists it): the OV02B10's and the OV16A1Q's picture is
BGGR, the S5KHM2SD's and the S5K4H7's GRBG. The OV02B10's own colour bars (cam_pattern=1) are taken
for GBRG, and the header says so for them. (Frames taken before the kernel had this right say
RGGB for BGGR and GBRG for GRBG: --bayer 3 and --bayer 1 for them.)
"""
import struct
import sys
import zlib


def png(path, width, height, rows, channels):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    data = b''.join(b'\0' + bytes(r) for r in rows)
    kind = 2 if channels == 3 else 0
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, kind, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(data, 6)) + chunk(b'IEND', b''))


def unpack_raw10(line, width):
    out = [0] * width
    for i in range(0, width - 3, 4):
        b = line[i // 4 * 5:i // 4 * 5 + 5]
        if len(b) < 5:
            break
        low = b[4]
        out[i] = b[0] << 2 | (low & 3)
        out[i + 1] = b[1] << 2 | (low >> 2 & 3)
        out[i + 2] = b[2] << 2 | (low >> 4 & 3)
        out[i + 3] = b[3] << 2 | (low >> 6 & 3)
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], 'rb').read()
    magic, width, height, stride, fmt, bayer, frames, frame_bytes, source = struct.unpack_from('<8s8I', data, 0)
    sensor = data[40:56].split(b'\0')[0].decode(errors='replace')
    last = struct.unpack_from('<I', data, 56)[0]
    if '--bayer' in sys.argv:
        bayer = int(sys.argv[sys.argv.index('--bayer') + 1])
    if magic != b'RWCAM001':
        sys.exit('camraw.py: not frames of the camera driver')
    print(f'camraw.py: {sensor}: {width}x{height}, stride {stride}, format {fmt}, bayer {bayer}, {frames} frame(s) written whole, '
          f'the last in place {last}')
    if fmt in (2, 3):
        # a picture iOS made of a frame (cam=5, camera_test or mediaserverd): NV12 (full range) or BGRA
        base = 4096
        rows = []
        for y in range(height):
            out = bytearray()
            if fmt == 3:
                line = data[base + y * stride:base + y * stride + 4 * width]
                for x in range(width):
                    out += bytes((line[4 * x + 2], line[4 * x + 1], line[4 * x]))
            else:
                luma = data[base + y * stride:base + y * stride + width]
                chroma = data[base + stride * height + (y // 2) * stride:base + stride * height + (y // 2) * stride + width]
                for x in range(width):
                    yy, cb, cr = luma[x], chroma[x & ~1] - 128, chroma[(x & ~1) + 1] - 128
                    for v in (yy + 1.402 * cr, yy - 0.344136 * cb - 0.714136 * cr, yy + 1.772 * cb):
                        out.append(max(0, min(255, int(v + 0.5))))
            rows.append(out)
        png(f'{sys.argv[2]}.png', width, height, rows, 3)
        print(f'camraw.py: {sys.argv[2]}.png ({width}x{height}, from {"NV12" if fmt == 2 else "BGRA"})')
        return
    count = max(frames, 1)
    for n in range(count):
        base = 4096 + n * frame_bytes
        if base + frame_bytes > len(data):
            break
        lines = [unpack_raw10(data[base + y * stride:base + (y + 1) * stride], width) for y in range(height)]
        png(f'{sys.argv[2]}-{n}-raw.png', width, height, [[v >> 2 for v in line] for line in lines], 1)
        # one pixel a 2x2 cell
        offsets = {0: ((0, 0), (0, 1), (1, 0), (1, 1)), 1: ((0, 1), (0, 0), (1, 1), (1, 0)),
                   2: ((1, 0), (0, 0), (1, 1), (0, 1)), 3: ((1, 1), (0, 1), (1, 0), (0, 0))}[bayer]
        (ry, rx), (g1y, g1x), (g2y, g2x), (by, bx) = offsets
        cells = []
        sums = [0, 0, 0]
        for y in range(0, height - 1, 2):
            row = []
            for x in range(0, width - 1, 2):
                r = lines[y + ry][x + rx]
                g = (lines[y + g1y][x + g1x] + lines[y + g2y][x + g2x]) / 2
                b = lines[y + by][x + bx]
                row.append((r, g, b))
                sums[0] += r
                sums[1] += g
                sums[2] += b
            cells.append(row)
        gains = [sums[1] / s if s else 1.0 for s in sums]
        top = max(max(max(c[0] * gains[0], c[1], c[2] * gains[2]) for c in row) for row in cells) or 1
        rows = []
        for row in cells:
            out = bytearray()
            for c in row:
                for k in range(3):
                    v = min(c[k] * gains[k] / top, 1.0) ** (1 / 2.2)
                    out.append(int(v * 255))
            rows.append(out)
        png(f'{sys.argv[2]}-{n}.png', width // 2, height // 2, rows, 3)
        print(f'camraw.py: {sys.argv[2]}-{n}.png ({width // 2}x{height // 2}) and {sys.argv[2]}-{n}-raw.png')


main()
