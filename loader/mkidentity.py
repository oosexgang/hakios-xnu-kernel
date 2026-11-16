#!/usr/bin/env python3
"""Take the identity of an iPhone from its real device tree, for the loader to hand to XNU.

usage: mkidentity.py DeviceTree.<board>.im4p|.bin > identity.h
       mkidentity.py --placeholder > identity.h     (no IPSW: no Apple data in it; the open kernel boots, Darwin does not)

The input is Firmware/all_flash/DeviceTree.<board>.im4p of the IPSW as it is (darwin/tools/aea.py get IPSW
MEMBER OUT fetches one member), or the device tree already taken out of it. An IM4P is unwrapped here: its
DER sequence's payload, decompressed when it is LZFSE ("bvx2", through the LZFSE library aea.py uses:
liblzfse1, or LZFSE_LIB=PATH).

Darwin user space (MobileGestalt above all) decides what device it runs on from the device
tree: the root node's model and compatible, and the /product node (screen class, artwork
scale, product name ...), and the /product/haptics node: what the phone's haptics are (MobileGestalt's
DeviceSupportsClosedLoopHaptics is its "closed-loop"; with it Darwin plays the system's vibrations and
feedbacks as waveforms on the actuator, and without it looks for an old vibration motor in IOKit,
which the iPhone X does not have), and of the /product/camera node what the phone here has as well (its
"flash": MobileGestalt's camera-flash, without which there is no flashlight in Control Center). The loader builds its own device tree for the hardware we really
have, but copies these properties from the device tree in the Darwin firmware so that the
system believes it is the iPhone its files were built for.

Properties that iBoot fills in on a real phone ("syscfg/..." placeholders: serial number,
colours, calibration data) are left out; the loader adds the few that are needed itself.
"""
import struct
import sys


def parse(data, pos=0):
    """One node of a flattened Apple device tree: (properties, children, next position)."""
    nprops, nchildren = struct.unpack_from('<II', data, pos)
    pos += 8
    props = []
    for _ in range(nprops):
        name = data[pos:pos + 32].split(b'\0')[0].decode()
        length = struct.unpack_from('<I', data, pos + 32)[0] & 0x7FFFFFFF
        value = data[pos + 36:pos + 36 + length]
        props.append((name, value))
        pos += 36 + (length + 3 & ~3)
    children = []
    for _ in range(nchildren):
        child, pos = parse(data, pos)
        children.append(child)
    return (props, children), pos


def name_of(node):
    return dict(node[0]).get('name', b'').split(b'\0')[0].decode()


def emit(label, props, skip):
    kept = [(n, v) for n, v in props if n not in skip and not v.startswith((b'syscfg/', b'macaddr/', b'zeroes/'))]
    blob = bytearray()
    print(f'/* {len(kept)} properties */')
    print(f'static const struct identity_prop {label}[] = {{')
    for name, value in kept:
        print(f'\t{{ "{name}", {len(blob)}, {len(value)} }},')
        blob += value
    print('};')
    print(f'static const unsigned char {label}_data[] = {{')
    for i in range(0, len(blob), 20):
        print('\t' + ' '.join(f'0x{b:02x},' for b in blob[i:i + 20]))
    print('};\n')


def der(data, at):
    """one DER element at AT: (tag, start of its value, length)"""
    tag, length = data[at], data[at + 1]
    at += 2
    if length & 0x80:
        n = length & 0x7f
        length = int.from_bytes(data[at:at + n], 'big')
        at += n
    return tag, at, length


def unwrap(raw):
    """the device tree in an IM4P (SEQUENCE { "IM4P", type, description, OCTET STRING payload, ... }), or RAW"""
    if raw[:1] != b'\x30' or b'IM4P' not in raw[:16]:
        return raw
    tag, at, length = der(raw, 0)
    items, end = [], at + length
    while at < end:
        tag, start, length = der(raw, at)
        items.append((tag, raw[start:start + length]))
        at = start + length
    if len(items) < 4 or items[0][1] != b'IM4P' or items[3][0] != 0x04:
        sys.exit('mkidentity.py: an IM4P without a payload where it should be')
    payload = items[3][1]
    if payload[:4] != b'bvx2':
        return payload
    import ctypes
    import ctypes.util
    import os
    path = os.environ.get('LZFSE_LIB') or ctypes.util.find_library('lzfse')
    if not path:
        sys.exit('mkidentity.py: the payload is LZFSE and there is no LZFSE library (liblzfse1, or LZFSE_LIB=PATH)')
    lib = ctypes.CDLL(path)
    lib.lzfse_decode_buffer.restype = ctypes.c_size_t
    lib.lzfse_decode_buffer.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t,
                                        ctypes.c_void_p]
    out = ctypes.create_string_buffer(16 << 20)
    n = lib.lzfse_decode_buffer(out, len(out), payload, len(payload), None)
    if n == 0 or n == len(out):
        sys.exit('mkidentity.py: the LZFSE payload does not decode')
    return out.raw[:n]


def emit_empty(label):
    # One property whose name no device tree uses and whose length is zero: dt_identity copies it as an
    # empty, harmless property, dt_identity_one never matches it, and no array is of length zero.
    print('/* placeholder: no properties (made without an IPSW) */')
    print(f'static const struct identity_prop {label}[] = {{ {{ "hakios-placeholder", 0, 0 }} }};')
    print(f'static const unsigned char {label}_data[] = {{ 0 }};\n')


def placeholder():
    """An identity.h with nothing of Apple's in it, for building the loader without an IPSW (and in CI).
    Enough for the open kernel under QEMU; Darwin's user space wants the real properties."""
    print('/* Generated by mkidentity.py --placeholder: no Apple device-tree data. */')
    print('struct identity_prop { const char *name; unsigned offset, length; };\n')
    print('#define IDENTITY_PLATFORM_NAME "unknown"\n')
    print_screen(*PLACEHOLDER_SCREEN)
    for label in ('identity_root', 'identity_product', 'identity_haptics', 'identity_camera'):
        emit_empty(label)


# The iPhone's screen, which the kernel's display reports (MiniIOMobileFramebuffer) and MobileGestalt holds
# against the device's class: /product's artwork-device-subtype is its height in pixels, artwork-scale-factor
# its pixels a point, and the width goes with the height (every iPhone since the iPhone 8).
WIDTHS = {1334: 750, 1792: 828, 2208: 1242, 2340: 1080, 2436: 1125, 2532: 1170, 2556: 1179, 2622: 1206,
          2688: 1242, 2778: 1284, 2796: 1290, 2868: 1320}
PLACEHOLDER_SCREEN = (1125, 2436, 3)    # the iPhone X's, which the display had before it was the identity's


def screen(product):
    """(width, height, scale) of the iPhone's screen, from its /product."""
    props = dict(product)
    try:
        height = int.from_bytes(props['artwork-device-subtype'][:4], 'little')
        scale = int.from_bytes(props['artwork-scale-factor'][:4], 'little')
    except KeyError as missing:
        raise SystemExit(f'mkidentity.py: /product has no {missing}: not an iPhone\'s device tree?')
    if height not in WIDTHS:
        raise SystemExit(f'mkidentity.py: a screen {height} pixels high (artwork-device-subtype) is not in WIDTHS')
    return WIDTHS[height], height, scale


def print_screen(width, height, scale):
    print(f'#define IDENTITY_SCREEN_WIDTH {width}       /* the iPhone\'s screen, in pixels */')
    print(f'#define IDENTITY_SCREEN_HEIGHT {height}')
    print(f'#define IDENTITY_SCREEN_SCALE {scale}        /* pixels a point */\n')


def platform_name(children):
    """The SoC's name, e.g. t8030: the root's platform-name is left for iBoot to fill in, so it is taken
    from /arm-io's compatible ("arm-io,t8030")."""
    for value in dict(children['arm-io'][0]).get('compatible', b'').split(b'\0'):
        if value.startswith(b'arm-io,'):
            return value[len(b'arm-io,'):].decode()
    raise SystemExit('mkidentity.py: no "arm-io,..." in /arm-io compatible: not an iPhone\'s device tree?')


def main():
    if sys.argv[1:] == ['--placeholder']:
        placeholder()
        return
    raw = unwrap(open(sys.argv[1], 'rb').read())
    root, _ = parse(raw)
    children = {name_of(c): c for c in root[1]}
    print('/* Generated by mkidentity.py from the device tree of the Darwin firmware. Do not edit. */')
    print('struct identity_prop { const char *name; unsigned offset, length; };\n')
    print(f'#define IDENTITY_PLATFORM_NAME "{platform_name(children)}"     /* the SoC (/arm-io compatible) */\n')
    print_screen(*screen(children['product'][0]))
    # of the root: who we are. Addresses, the phandle and what iBoot fills in stay ours.
    emit('identity_root', root[0], ('name', '#address-cells', '#size-cells', 'AAPL,phandle', 'clock-frequency',
                                    'platform-name', 'time-stamp', 'secure-root-prefix'))
    emit('identity_product', children['product'][0], ('name', 'AAPL,phandle'))
    product = {name_of(c): c for c in children['product'][1]}
    emit('identity_haptics', product['haptics'][0], ('name', 'AAPL,phandle'))
    # (the camera's: the loader takes from it what the hardware here has, the flash so far)
    emit('identity_camera', product['camera'][0], ('name', 'AAPL,phandle'))


if __name__ == '__main__':
    main()
