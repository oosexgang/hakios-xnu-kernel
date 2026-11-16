#!/usr/bin/env python3
"""Pack a phone's UEFI and a payload into an Android boot image, for `fastboot boot`.

usage: loader/mkboot.py [--board NAME] [--uefi DIR] [-p PAYLOAD] -o OUT.img

  kernel  = gzip(BootShim + UEFI image + [payload]) + the phone's device tree
  OUT.img = an Android boot image (header version from [uefi]) with that kernel

The phone's bootloader starts the "kernel": the boot shim copies the UEFI image to its base and the
payload (an "XNUP" container, loader/mkpayload.py) to the address the loader finds it at. The UEFI
then starts loader.efi from the payload. Nothing is written to the phone's storage.

What is the board's: the [uefi] section of its board.toml (fd_base, fd_size, header_version), and three
files of its own UEFI build, which carry the vendor's drivers and the phone's device tree and so are
never in the repository:

  DIR/SILICIUM_UEFI.fd   the UEFI image          (Mu-Silicium: Build/<board>Pkg/RELEASE_CLANGPDB/FV/)
  DIR/BootShim.bin       the shim that starts it (Mu-Silicium: BootShim/, built for this board)
  DIR/device-tree.dtb    the phone's device tree (Mu-Silicium: Resources/DTBs/<board>.dtb)

DIR is firmware/NAME/uefi/ at the top of the repository unless --uefi says otherwise.
"""
import argparse
import gzip
import hashlib
import os
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'tools'))
import boards  # noqa: E402  (devices/NAME/board.toml)

PAYLOAD_MAGIC = b'XNUP'
PAYLOAD_MAX = 0x04000000         # what the payload reservation of the UEFI's memory map holds (loader.c PAYLOAD_MAX_SIZE)
# Android's boot image, version 0 to 2 (system/tools/mkbootimg): the addresses mkbootimg gives by default.
# The bootloaders that start these UEFIs take the kernel from its place in the file and ignore them.
BASE, KERNEL_OFFSET, RAMDISK_OFFSET, TAGS_OFFSET, DTB_OFFSET = 0x10000000, 0x00008000, 0x01000000, 0x00000100, 0x01f00000
PAGE = 2048
RAMDISK = b'dummy'               # a bootloader may refuse an image without one; the UEFI never reads it
UEFI_FILES = ('SILICIUM_UEFI.fd', 'BootShim.bin', 'device-tree.dtb')


def uefi_board(name):
    """(name, [uefi] of its board): the board named, or the only one here that has a [uefi] section."""
    found = {n: board['uefi'] for n, board in boards.load()[1].items() if 'uefi' in board}
    if name is None:
        if len(found) != 1:
            raise SystemExit('mkboot.py: which phone? --board NAME (here: ' +
                             (', '.join(sorted(found)) or 'no package with a [uefi] section') + ')')
        name = next(iter(found))
    if name not in found:
        raise SystemExit(f'mkboot.py: no board "{name}" with a [uefi] section (tools/boards.py list)')
    return name, found[name]


def boot_image(kernel, header_version):
    """An Android boot image, version 0, 1 or 2, of a kernel and the placeholder ramdisk."""
    if header_version not in (0, 1, 2):
        raise SystemExit(f'mkboot.py: boot image header version {header_version}: only 0, 1 and 2 are made here')
    sha = hashlib.sha1()
    for part in (kernel, RAMDISK, b''):                   # kernel, ramdisk, second stage
        sha.update(part)
        sha.update(struct.pack('<I', len(part)))
    if header_version >= 1:
        sha.update(struct.pack('<I', 0))                  # recovery DTBO
    if header_version >= 2:
        sha.update(struct.pack('<I', 0))                  # DTB
    header = struct.pack('<8s10I16s512s32s1024s', b'ANDROID!', len(kernel), BASE + KERNEL_OFFSET,
                         len(RAMDISK), BASE + RAMDISK_OFFSET, 0, 0, BASE + TAGS_OFFSET,
                         PAGE, header_version, 0, b'', b'', sha.digest(), b'')
    if header_version >= 1:
        size = len(header) + 16 + (12 if header_version >= 2 else 0)
        header += struct.pack('<IQI', 0, 0, size)
    if header_version >= 2:
        header += struct.pack('<IQ', 0, BASE + DTB_OFFSET)

    def page(data):
        return data + b'\0' * (-len(data) % PAGE)
    return page(header) + page(kernel) + page(RAMDISK)


def pack(shim, image, dtb, payload, uefi):
    """The boot image of a UEFI (its shim, image and device tree, as bytes) and a payload (b'' for none), checked
    against the board's [uefi]."""
    if len(image) != uefi['fd_size']:
        raise SystemExit(f'mkboot.py: the UEFI image is {len(image):#x} bytes, [uefi] fd_size says {uefi["fd_size"]:#x}')
    # The shim is an arm64 Linux image whose header says where the UEFI goes and how big it is (text_offset,
    # image_size): built for another memory map, it would copy this one where the phone's UEFI does not run.
    if len(shim) < 64 or shim[56:60] != b'ARMd':
        raise SystemExit('mkboot.py: the shim is not an arm64 image (no ARMd at 56)')
    at, size = struct.unpack_from('<QQ', shim, 8)
    if (at, size) != (uefi['fd_base'], uefi['fd_size']):
        raise SystemExit(f'mkboot.py: the shim puts the UEFI at {at:#x}, {size:#x} bytes; [uefi] says '
                         f'{uefi["fd_base"]:#x}, {uefi["fd_size"]:#x}')
    if payload and payload[:4] != PAYLOAD_MAGIC:
        raise SystemExit('mkboot.py: the payload is not an XNUP container (loader/mkpayload.py)')
    if len(payload) > PAYLOAD_MAX - 0x1000:
        raise SystemExit(f'mkboot.py: the payload is {len(payload)} bytes; the UEFI reserves {PAYLOAD_MAX:#x} for '
                         f'it. Send big images over USB instead (xl ramdisk)')
    blob = shim + image + payload
    blob += b'\0' * (-len(blob) % 16)
    return boot_image(gzip.compress(blob, 6, mtime=0) + dtb, uefi.get('header_version', 1))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--board', default=os.environ.get('HAKIOS_BOARD'),
                        help='the phone, by its package\'s name (default: $HAKIOS_BOARD, or the only package '
                             'with a [uefi] section)')
    parser.add_argument('--uefi', type=Path, metavar='DIR', help='its UEFI build (default firmware/NAME/uefi)')
    parser.add_argument('-p', '--payload', type=Path, help='an XNUP container for the loader')
    parser.add_argument('-o', '--output', type=Path, required=True)
    options = parser.parse_args()

    name, uefi = uefi_board(options.board)
    directory = options.uefi or REPO / 'firmware' / name / 'uefi'
    files = [directory / f for f in UEFI_FILES]
    for path in files:
        if not path.is_file():
            raise SystemExit(f'mkboot.py: no {path}.\n  The UEFI of a phone is built from its own firmware and is '
                             f'never in the repository: docs/porting.md, step 3, says how to make it.')
    image, shim, dtb = (path.read_bytes() for path in files)
    payload = options.payload.read_bytes() if options.payload else b''
    options.output.write_bytes(pack(shim, image, dtb, payload, uefi))
    print(f'{options.output}: {options.output.stat().st_size} bytes for {name}'
          + (f', payload {len(payload)} bytes' if payload else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
