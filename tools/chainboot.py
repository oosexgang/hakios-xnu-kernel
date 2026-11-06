#!/usr/bin/env python3
"""
Put a bare-metal program in front of Android's kernel in a boot image (header v2).

  chainboot.py <android-boot.img> <program.bin> <out.img> [payload.bin]

LK decompresses the kernel to 0x40080000 and enters it there. The new kernel is gzip of:
program.bin, padded to CHAIN_OFFSET, then Android's own (uncompressed) Image. The program runs
first and then moves the Image down to 0x40080000 and starts it (mtkloader/fbdiag.c). An optional
payload (mtkloader's "XNUP") follows the Image, 16-aligned. The program's own arm64 header gets
the Image's length in res2 (0x28), the payload's offset in res3 (0x30) and a covering image_size.
Everything else - header fields, cmdline, ramdisk, dtb - is copied from the Android image.
"""
import gzip
import hashlib
import struct
import sys

CHAIN_OFFSET = 0x40000
BOOT_PARTITION = 32 << 20


def pad(b, n):
    return b + b'\0' * (-len(b) % n)


def main():
    src, prog_path, out = sys.argv[1:4]
    payload = open(sys.argv[4], 'rb').read() if len(sys.argv) > 4 else b''
    d = open(src, 'rb').read()
    assert d[:8] == b'ANDROID!', 'not an Android boot image'
    ksize, _, rsize, _, ssize, _, _, page, hver = struct.unpack_from('<9I', d, 8)
    assert hver == 2, f'header v{hver}, expected 2'
    rdo_size = struct.unpack_from('<I', d, 1632)[0]
    dtb_size = struct.unpack_from('<I', d, 1648)[0]

    def pages(n):
        return (n + page - 1) // page * page

    off = page
    kernel = d[off:off + ksize]; off += pages(ksize)
    ramdisk = d[off:off + rsize]; off += pages(rsize)
    second = d[off:off + ssize]; off += pages(ssize)
    rdo = d[off:off + rdo_size]; off += pages(rdo_size)
    dtb = d[off:off + dtb_size]

    image = gzip.decompress(kernel) if kernel[:2] == b'\x1f\x8b' else kernel
    assert image[0x38:0x3c] == b'ARM\x64', 'Android kernel is not an arm64 Image'
    linux_image_size = struct.unpack_from('<Q', image, 0x10)[0]

    prog = bytearray(open(prog_path, 'rb').read())
    assert prog[0x38:0x3c] == b'ARM\x64', 'program has no arm64 header'
    assert len(prog) <= CHAIN_OFFSET, 'program too large'
    payload_off = CHAIN_OFFSET + len(pad(image, 16)) if payload else 0
    total = max(CHAIN_OFFSET + linux_image_size, payload_off + len(payload))
    struct.pack_into('<Q', prog, 0x10, (total + 0xfffff) & ~0xfffff)
    struct.pack_into('<Q', prog, 0x28, len(image))
    struct.pack_into('<Q', prog, 0x30, payload_off)
    blob = bytes(prog) + b'\0' * (CHAIN_OFFSET - len(prog)) + image
    if payload:
        blob = pad(blob, 16) + payload
    new_kernel = gzip.compress(blob, 9, mtime=0)

    hdr = bytearray(d[:page])
    struct.pack_into('<I', hdr, 8, len(new_kernel))
    sha = hashlib.sha1()
    for part in (new_kernel, ramdisk, second, rdo, dtb):
        sha.update(part)
        sha.update(struct.pack('<I', len(part)))
    hdr[576:608] = pad(sha.digest(), 32)

    img = bytes(hdr) + pad(new_kernel, page) + pad(ramdisk, page) + pad(second, page) + pad(rdo, page) + pad(dtb, page)
    # boot_a on the X6826B is 32 MB: an image any larger is refused by fastboot, and a script that
    # sends fastboot's output nowhere would carry on as if it had been flashed
    if len(img) > BOOT_PARTITION:
        raise SystemExit(f'{out}: {len(img)} bytes, more than the {BOOT_PARTITION} of the boot partition '
                         f'({len(img) - BOOT_PARTITION} too many): the payload has to shrink')
    open(out, 'wb').write(img)
    print(f'{out}: {len(img)} bytes (program {len(prog)}, Image {len(image)}, payload {len(payload)}, kernel gz {len(new_kernel)})')


if __name__ == '__main__':
    main()
