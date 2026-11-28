#!/usr/bin/env python3
"""Make a camera's colour tuning, as libhakios.media's processor reads it (userland/src/camera_isp.c,
isp_load_tuning), out of the camera's tuning file of Android's camera stack
(/vendor/lib64/camera/com.qti.tuned.<name>.bin of the phone: the vendor's, kept in fw-camera/ and never in
the repository; so is what this makes of it).

usage: camtuning.py TUNED.bin OUT.isp     (camtuning.py TUNED.bin -   lists what is there only)

The tuning file is a "QTI Chromatix Header" / "Parameter Parser V3.0.1" file, laid out as the sensors'
module files are (see camsensor.py): a table of symbols { id, name[32], kind, ?, parent, offset, size }
and the data they point into. Two of its modules are taken, each as Chromatix keeps it -- a tree whose
first level is the flash (led_idx_data: 0 without one, the only one taken), the next the scene's light
(aec_data: from a lux index to another, Qualcomm's measure of how dark the scene is: higher is darker),
the last the light's colour temperature (cct_data: from a CCT to another, in kelvin):

  cc13 (the colour matrix, IPE's): per light and temperature a 3x3 matrix from the sensor's colours,
       white balanced, to linear sRGB, by rows R, G, B (each row sums to 1)
  lsc34 (lens shading): per light and temperature a gain for each of R, Gr, Gb and B at each point of
       a 17 x 13 grid laid over the sensor (corners on its corners, rows from the top)

and, of its white balance (Qualcomm's AWB), the reference lights (refPtV1): ten of them for every camera
of this phone, each as the sensor sees a grey under it, red over green and blue over green -- from the
bluest daylight to a candle, in the order 10000 K, D75, D65, D50 (whose gains are the AWB's first ones,
initDataV1), cool white and TL84 fluorescent tubes, U30, A (a filament lamp), horizon, candle. Each is
given the colour temperature that the tables above are picked by for it: its own, but 3800 K for U30, a
3000 K light without the infrared a filament gives -- as a white LED is -- whose tables are TL84's (the
wide camera under a warm white LED: its colour shading measured on a white sheet is TL84's table's to
within 3 %, the 2550-2900 K one's is 12 % off in red).

Parameter Parser V3.4.0 files (the Xiaomi 12 Pro's) keep the same symbol table but lay the modules out
as trees of "trigger" ranges: a trigger's value is a list of { from, to (floats), count, child trigger,
leaf, region } (six 32-bit words), its child the next level's trigger or, where leaf is 1, its region:
the values themselves. Each module's root (the last word of its "<module>_<block>_v2" symbol) goes
total -> gain (1-64) -> flash (0 without, the only one taken) -> lux index -> CCT -> values. The
colour matrix is cc13_ipe_v2's (9 floats, then three more not taken), the lens shading lsc41_ife_v2's
(17 x 13 per channel, as above; eight more floats after them, not taken); of each the first one whose
tree has more than one light range (the others are other use cases' or the identity). The wide
camera's: lux index 1-170 (bright), 180-290, 310-400, 410-550 with five CCT ranges each from
2450 to 10000 K, and 580-999 (dark) with the identity matrix.

OUT.isp: "RWCISP02"; the number of the matrices' light ranges and per range { lux index from, to (two
floats), the number of temperature ranges, per range { CCT from, to, the matrix (9 floats) } }; the
same for the lens shading, with per temperature range { CCT from, to, R, Gr, Gb, B grids (17 x 13 floats
each) }; the number of reference lights (0 where there are not the ten) and each { R/G, B/G, CCT }.
Everything little-endian, counts as 32-bit words.
"""
import struct
import sys

GRID = 17 * 13


def symbols(data, with_pool=False):
    count = struct.unpack_from('<I', data, 0xa4)[0]
    entries = [struct.unpack_from('<3I', data, 0xa8 + 12 * i) for i in range(count)]
    table, pool = entries[0][1], entries[1][1]
    syms, at = {}, table
    while at + 0x38 <= pool:
        sid = struct.unpack_from('<I', data, at)[0]
        name = data[at + 4:at + 0x24].split(b'\0')[0].decode('latin1')
        _, _, _, ref, size = struct.unpack_from('<5i', data, at + 0x24)
        syms[sid] = (name, data[pool + ref:pool + ref + size])
        at += 0x38
    return (syms, pool) if with_pool else syms


def tree34(data, pool, syms, sid, floats):
    """the ranges of a V3.4.0 trigger: [(from, to, values or the next level's ranges)]"""
    v = syms[sid][1]
    out = []
    for i in range(len(v) // 24):
        lo, hi, _, child, leaf, region = struct.unpack_from('<2f4I', v, 24 * i)
        if leaf == 1:
            out.append((lo, hi, struct.unpack_from('<%df' % floats, syms[region][1], 0)))
        else:
            out.append((lo, hi, tree34(data, pool, syms, child, floats)))
    return out


def module34(data, pool, syms, block, floats):
    """[(lux from, lux to, [(cct from, cct to, values)])] of the first of a V3.4.0 module's trees with more
    than one light range, without the flash"""
    for name, v in syms.values():
        if name != block or len(v) < 4:
            continue
        level = tree34(data, pool, syms, struct.unpack_from('<I', v, len(v) - 4)[0], floats)
        # down through the single ranges (total, gain) and the flash's (its first: none) to the lux index
        def depth(lv):
            return 1 + depth(lv[0][2]) if isinstance(lv, list) else 0

        while level and depth(level) > 2:
            level = level[0][2]
        if len(level) > 1 and all(isinstance(r[2], list) and not isinstance(r[2][0][2], list) for r in level):
            return [(lo, hi, list(ccts)) for lo, hi, ccts in level]
    return []


def module(syms, name, floats):
    """[(lux from, lux to, [(cct from, cct to, values)])] of the module's first tree, without the flash"""
    led = next(v for n, v in syms.values() if n == 'mod_%s_led_idx_data' % name)
    _, regions, sid = next(e for e in struct.iter_unpack('<3I', led) if e[0] == 0)
    out = []
    for i in range(regions):
        lux_from, lux_to, _, _, n, cct_sid = struct.unpack_from('<4f2I', syms[sid][1], 24 * i)
        cct = syms[cct_sid][1]
        size = len(cct) // n
        ranges = []
        for j in range(n):
            v = struct.unpack_from('<%df' % (2 + floats), cct, size * j)
            ranges.append((v[0], v[1], v[2:]))
        out.append((lux_from, lux_to, ranges))
    return out


# the colour temperature each of the ten reference lights picks the tables by (see above)
REFERENCE_CCTS = (10000, 7500, 6500, 5000, 4150, 4000, 3800, 2850, 2300, 1900)


def references(syms):
    """[(R/G, B/G, CCT)] of the AWB's reference lights, or [] where they are not the ten known"""
    v = next((v for n, v in syms.values() if n == 'refPtV1'), None)
    if v is None:
        return []
    f = struct.unpack('<%df' % (len(v) // 4), v)
    points = [(f[i], f[i + 1]) for i in range(6, len(f) - 1, 2) if f[i] > 0 and f[i + 1] > 0]
    if len(points) != len(REFERENCE_CCTS):
        return []
    return [(rg, bg, cct) for (rg, bg), cct in zip(points, REFERENCE_CCTS)]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], 'rb').read()
    if not data.startswith(b'QTI Chromatix Header') or not (b'Parameter Parser V3.0.1' in data[:0x60] or
                                                             b'Parameter Parser V3.4.0' in data[:0x60]):
        sys.exit('camtuning.py: not a tuning file of the kind known here')
    syms, pool = symbols(data, True)
    if b'Parameter Parser V3.4.0' in data[:0x60]:
        ccm = module34(data, pool, syms, 'cc13_ipe_v2', 9)
        lsc = module34(data, pool, syms, 'lsc41_ife_v2', 4 * GRID)
    else:
        ccm = module(syms, 'cc13', 9)
        lsc = module(syms, 'lsc34', 4 * GRID)
    if not ccm or not lsc:
        sys.exit('camtuning.py: no colour matrix or lens shading found')
    refs = references(syms)

    for what, tree in (('colour matrix', ccm), ('lens shading', lsc)):
        for lux_from, lux_to, ranges in tree:
            for cct_from, cct_to, v in ranges:
                if what == 'colour matrix':
                    shown = ' '.join('%.3f' % x for x in v)
                else:
                    corners = [v[c * GRID] for c in range(4)]
                    shown = 'corner gains R %.2f Gr %.2f Gb %.2f B %.2f' % tuple(corners)
                print('%-13s lux index %4.0f-%-4.0f CCT %5.0f-%-5.0f %s' % (what, lux_from, lux_to, cct_from, cct_to, shown))
    for rg, bg, cct in refs:
        print('reference light R/G %.3f B/G %.3f, tables of %.0f K' % (rg, bg, cct))
    if sys.argv[2] == '-':
        return

    out = bytearray(b'RWCISP02')
    for tree in (ccm, lsc):
        out += struct.pack('<I', len(tree))
        for lux_from, lux_to, ranges in tree:
            out += struct.pack('<2fI', lux_from, lux_to, len(ranges))
            for cct_from, cct_to, v in ranges:
                out += struct.pack('<2f%df' % len(v), cct_from, cct_to, *v)
    out += struct.pack('<I', len(refs))
    for ref in refs:
        out += struct.pack('<3f', *ref)
    open(sys.argv[2], 'wb').write(out)


main()
