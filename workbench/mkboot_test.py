#!/usr/bin/env python3
"""arch/arm64/boot/mkboot.py: the boot image a phone's bootloader is given, made of a UEFI that is not a real one."""
import hashlib
import importlib.util
import struct
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('mkboot', ROOT / 'arch/arm64/boot/mkboot.py')
mkboot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mkboot)

UEFI = {'fd_base': 0x9FD00000, 'fd_size': 0x1000}
# an arm64 Linux image header as Silicium's shim has it: where the UEFI goes (text_offset) and its size
SHIM = struct.pack('<IIQQQQQQ4sI', 0x14000010, 0, UEFI['fd_base'], UEFI['fd_size'], 0, 0, 0, 0, b'ARMd', 0) + b'code'
IMAGE = bytes(range(256)) * 16
DTB = b'\xd0\x0d\xfe\xed' + b'tree' * 8
PAYLOAD = b'XNUP' + b'\x01' * 60


def fields(image):
    magic, kernel_size, _, ramdisk_size, _, second_size, _, _, page, version = struct.unpack_from('<8s9I', image)
    return magic, kernel_size, ramdisk_size, second_size, page, version


class Image(unittest.TestCase):
    def test_the_kernel_is_the_shim_the_uefi_and_the_payload_then_the_tree(self):
        image = mkboot.pack(SHIM, IMAGE, DTB, PAYLOAD, UEFI)
        magic, kernel_size, ramdisk_size, second_size, page, version = fields(image)
        self.assertEqual((magic, ramdisk_size, second_size, page, version), (b'ANDROID!', 5, 0, 2048, 1))
        kernel = image[page:page + kernel_size]
        unzip = zlib.decompressobj(31)
        blob = unzip.decompress(kernel)
        self.assertEqual(blob.rstrip(b'\0'), (SHIM + IMAGE + PAYLOAD).rstrip(b'\0'))
        self.assertEqual(len(blob) % 16, 0)
        self.assertEqual(unzip.unused_data, DTB)
        self.assertEqual(len(image) % page, 0)

    def test_version_one_says_how_long_its_header_is(self):
        image = mkboot.pack(SHIM, IMAGE, DTB, b'', UEFI)
        self.assertEqual(struct.unpack_from('<IQI', image, 1632), (0, 0, 1648))

    def test_the_id_is_android_s_digest_of_the_parts(self):
        image = mkboot.pack(SHIM, IMAGE, DTB, b'', UEFI)
        kernel_size = fields(image)[1]
        kernel = image[2048:2048 + kernel_size]
        sha = hashlib.sha1()
        for part in (kernel, b'dummy', b''):
            sha.update(part + struct.pack('<I', len(part)))
        sha.update(struct.pack('<I', 0))
        self.assertEqual(image[576:596], sha.digest())

    def test_the_same_inputs_give_the_same_file(self):
        self.assertEqual(mkboot.pack(SHIM, IMAGE, DTB, PAYLOAD, UEFI), mkboot.pack(SHIM, IMAGE, DTB, PAYLOAD, UEFI))

    def test_a_uefi_of_another_size_is_refused(self):
        with self.assertRaisesRegex(SystemExit, 'fd_size'):
            mkboot.pack(SHIM, IMAGE[:-1], DTB, b'', UEFI)

    def test_a_shim_for_another_memory_map_is_refused(self):
        with self.assertRaisesRegex(SystemExit, 'puts the UEFI at'):
            mkboot.pack(SHIM, IMAGE, DTB, b'', dict(UEFI, fd_base=0x9FC00000))
        with self.assertRaisesRegex(SystemExit, 'not an arm64 image'):
            mkboot.pack(SHIM.replace(b'ARMd', b'MZ\0\0'), IMAGE, DTB, b'', UEFI)

    def test_what_is_not_a_payload_is_refused(self):
        with self.assertRaisesRegex(SystemExit, 'not an XNUP'):
            mkboot.pack(SHIM, IMAGE, DTB, b'ELF!', UEFI)
        with self.assertRaisesRegex(SystemExit, 'over USB'):
            mkboot.pack(SHIM, IMAGE, DTB, b'XNUP' + bytes(mkboot.PAYLOAD_MAX), UEFI)

    def test_only_the_versions_made_here(self):
        with self.assertRaisesRegex(SystemExit, 'header version 3'):
            mkboot.pack(SHIM, IMAGE, DTB, b'', dict(UEFI, header_version=3))
        self.assertEqual(fields(mkboot.pack(SHIM, IMAGE, DTB, b'', dict(UEFI, header_version=2)))[5], 2)


if __name__ == '__main__':
    unittest.main()
