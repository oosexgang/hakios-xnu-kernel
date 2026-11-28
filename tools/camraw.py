#!/usr/bin/env python3
"""Make pictures of the frames the kernel's camera driver left in memory (boot-arg cam=3 and up;
drivers/camera/hakios.camera.c), as the loader hands them over ("xl camera").

usage: camraw.py CAMERA.raw PREFIX [--bayer N] [--isp TUNING.isp [--lux INDEX]]

CAMERA.raw is a header { "RWCAM001", width, height, stride, format, bayer, frames, frame_bytes,
source, sensor[16], last } in its first 4096 bytes (last: which place holds the last frame written), then the frames, frame_bytes each. Format 1 is MIPI
RAW10 as the sensor sends it: four pixels in five bytes, the high eight bits of each in the first
four, their low two bits together in the fifth. Each frame becomes PREFIX-N.png, debayered by
taking each 2x2 cell (bayer 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR) as one pixel -- half the resolution, no
interpolation -- with a plain grey-world white balance and a gamma of 2.2: a first look, not a
finished photograph; PREFIX-N-raw.png is the frame's raw values as they are (grey, 8 bits).
Formats 2 and 3 are a picture Darwin made of a frame (cam=5): NV12 (full-range YCbCr 4:2:0, the chroma plane
after the luma one, both of the stride) or BGRA, which becomes PREFIX.png as it is.
--isp TUNING.isp (tools/camtuning.py makes it from the camera's tuning file) makes PREFIX-N.png as
libhakios.media's processor would, cell by cell: the black level (64 of 1023) off; the lens shading of
the tuning (its 17 x 13 grids, corners on the frame's corners, between grid points bilinear); the white
balance: the grey world of the cells neither near black nor near white, moved onto the line through
the tuning's reference lights (the nearest point on it) -- that point's gains, and its colour
temperature, between those of the two lights it lies between; the colour matrix of the tuning for that
temperature and the lux index given (--lux, 250 if not: indoor light; the frame does not say), between
ranges blended as camera_isp.c does; the picture scaled so that 1 in 200 cells is at white; the sRGB
curve.
--bayer N takes the frames for that order instead of the header's. The header's is the one each
sensor's module file gives (tools/camsensor.py lists it): the OV02B10's and the OV16A1Q's picture is
BGGR, the S5KHM2SD's and the S5K4H7's GRBG. The OV02B10's own colour bars (cam_pattern=1) are taken
for GBRG, and the header says so for them. (Frames taken before the kernel had this right say
RGGB for BGGR and GBRG for GRBG: --bayer 3 and --bayer 1 for them.)
"""
import struct
import sys
import zlib

BLACK = 64
LSC_W, LSC_H = 17, 13


def load_isp(path):
    """(colour matrix ranges, lens shading ranges, reference lights) of a RWCISP02 file (tools/camtuning.py)"""
    data = open(path, 'rb').read()
    if data[:8] != b'RWCISP02':
        sys.exit('camraw.py: %s is not a tuning of camtuning.py' % path)
    at = 8

    def tree(floats):
        nonlocal at
        lights = []
        count = struct.unpack_from('<I', data, at)[0]
        at += 4
        for _ in range(count):
            lux_from, lux_to, n = struct.unpack_from('<2fI', data, at)
            at += 12
            ranges = []
            for _ in range(n):
                v = struct.unpack_from('<%df' % (2 + floats), data, at)
                ranges.append((v[0], v[1], v[2:]))
                at += 4 * (2 + floats)
            lights.append((lux_from, lux_to, ranges))
        return lights

    ccm = tree(9)
    lsc = tree(4 * LSC_W * LSC_H)
    refs = [struct.unpack_from('<3f', data, at + 4 + 12 * i) for i in range(struct.unpack_from('<I', data, at)[0])]
    return ccm, lsc, refs


def pick(ranges, x):
    """(a, b, t): where x falls among ascending ranges -- in one (a = b), or t of the way from a's end to b's start"""
    for i, (lo, hi, _) in enumerate(ranges):
        if x <= hi:
            if i > 0 and x < lo:
                return i - 1, i, (x - ranges[i - 1][1]) / (lo - ranges[i - 1][1])
            return i, i, 0.0
    return len(ranges) - 1, len(ranges) - 1, 0.0


def blend(lights, lux, cct):
    """a module's values for this light and temperature, as camera_isp.c's tune_blend()"""
    la, lb, tl = pick(lights, lux)
    out = None
    for l, w in ((la, 1 - tl), (lb, tl)):
        if w == 0:
            continue
        ranges = lights[l][2]
        ca, cb, tc = pick(ranges, cct)
        v = [w * ((1 - tc) * a + tc * b) for a, b in zip(ranges[ca][2], ranges[cb][2])]
        out = v if out is None else [x + y for x, y in zip(out, v)]
    return out


def white_point(rg, bg, refs):
    """the nearest point to (R/G, B/G) on the line through the reference lights: (R/G, B/G, CCT)"""
    best = None
    for (r0, b0, c0), (r1, b1, c1) in zip(refs, refs[1:]):
        dr, db = r1 - r0, b1 - b0
        t = max(0.0, min(1.0, ((rg - r0) * dr + (bg - b0) * db) / (dr * dr + db * db or 1)))
        r, b = r0 + t * dr, b0 + t * db
        d = (r - rg) ** 2 + (b - bg) ** 2
        if best is None or d < best[0]:
            best = (d, r, b, c0 + t * (c1 - c0))
    return best[1:]


def processed(cells, width, height, isp, lux):
    """the cells (R, G, B above black, half the frame's size) made a picture with the tuning: 8-bit RGB rows"""
    ccm_lights, lsc_lights, refs = isp
    # lens shading of a middle temperature first, for the white balance; then again at the light found
    def shade(cct):
        g = blend(lsc_lights, lux, cct)
        grid = LSC_W * LSC_H
        chans = [g[0:grid], [(a + b) / 2 for a, b in zip(g[grid:2 * grid], g[2 * grid:3 * grid])], g[3 * grid:4 * grid]]
        out = []
        for y in range(height):
            gy = y * (LSC_H - 1) / max(height - 1, 1)
            y0 = min(int(gy), LSC_H - 2)
            fy = gy - y0
            row = []
            for x in range(width):
                gx = x * (LSC_W - 1) / max(width - 1, 1)
                x0 = min(int(gx), LSC_W - 2)
                fx = gx - x0
                at = y0 * LSC_W + x0
                row.append(tuple((c[at] * (1 - fx) + c[at + 1] * fx) * (1 - fy) + (c[at + LSC_W] * (1 - fx) + c[at + LSC_W + 1] * fx) * fy
                                 for c in chans))
            out.append(row)
        return out

    gains = shade(4000)
    sums, top = [0.0, 0.0, 0.0], 1023 - BLACK
    for row, grow in zip(cells, gains):
        for c, g in zip(row, grow):
            if max(c) < top * 0.95 and min(c) > 2:
                for k in range(3):
                    sums[k] += c[k] * g[k]
    rg, bg = (sums[0] / sums[1], sums[2] / sums[1]) if sums[1] else (1.0, 1.0)
    if rg > 1.1 and bg > 1.1:
        print('camraw.py: warning: red and blue both above green (R/G %.2f B/G %.2f): the colour filter order in the header '
              'is likely wrong (green is the strongest under any usual light)' % (rg, bg))
    wr, wb, cct = white_point(rg, bg, refs) if len(refs) > 1 else (rg, bg, 5000.0)
    m = blend(ccm_lights, lux, cct)
    print('camraw.py: grey world R/G %.3f B/G %.3f; white point R/G %.3f B/G %.3f, %.0f K; colour matrix %s' %
          (rg, bg, wr, wb, cct, ' '.join('%.3f' % v for v in m)))
    gains = shade(cct)
    wbg = (1 / wr, 1.0, 1 / wb)
    lin = []
    for row, grow in zip(cells, gains):
        out = []
        for c, g in zip(row, grow):
            r, gg, b = c[0] * g[0] * wbg[0], c[1] * g[1], c[2] * g[2] * wbg[2]
            out.append((m[0] * r + m[1] * gg + m[2] * b, m[3] * r + m[4] * gg + m[5] * b, m[6] * r + m[7] * gg + m[8] * b))
        lin.append(out)
    peaks = sorted(max(c) for row in lin for c in row[::4])
    white = peaks[int(len(peaks) * 0.995)] or 1
    curve = [int(255 * (12.92 * v if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055) + 0.5) for v in (i / 4095 for i in range(4096))]
    rows = []
    for row in lin:
        out = bytearray()
        for c in row:
            for v in c:
                out.append(curve[max(0, min(4095, int(v / white * 4095)))])
        rows.append(out)
    return rows


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
    isp = load_isp(sys.argv[sys.argv.index('--isp') + 1]) if '--isp' in sys.argv else None
    lux = float(sys.argv[sys.argv.index('--lux') + 1]) if '--lux' in sys.argv else 250.0
    if magic != b'RWCAM001':
        sys.exit('camraw.py: not frames of the camera driver')
    print(f'camraw.py: {sensor}: {width}x{height}, stride {stride}, format {fmt}, bayer {bayer}, {frames} frame(s) written whole, '
          f'the last in place {last}')
    if fmt in (2, 3):
        # a picture Darwin made of a frame (cam=5, camera_test or mediaserverd): NV12 (full range) or BGRA
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
        if isp is not None:
            rows = processed([[(max(r - BLACK, 0), max(g - BLACK, 0), max(b - BLACK, 0)) for r, g, b in row] for row in cells],
                             width // 2, height // 2, isp, lux)
            png(f'{sys.argv[2]}-{n}.png', width // 2, height // 2, rows, 3)
            print(f'camraw.py: {sys.argv[2]}-{n}.png ({width // 2}x{height // 2}, with the tuning) and {sys.argv[2]}-{n}-raw.png')
            continue
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
