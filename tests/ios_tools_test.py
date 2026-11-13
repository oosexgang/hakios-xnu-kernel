"""The parts of the Darwin image tool (ios/tools/mkimage26.py) that need no IPSW: which files a pruned image
leaves out, which Mach-O files are arm64e and go through pacstrip, the trimming of string tables, and
where the stage0 it carries mounts the small image.

    python3 tests/ios_tools_test.py
"""
import importlib.util
import os
import plistlib
import struct
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('mkimage26', ROOT / 'ios/tools/mkimage26.py')
mkimage26 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mkimage26)

ARM64, ARM64E = (0x0100000c, 0), (0x0100000c, 2)


def thin(cpu, path):
    path.write_bytes(struct.pack('<III', 0xfeedfacf, *cpu) + bytes(4096))


def fat(cpus, path, big=False):
    entries = b''.join(struct.pack('>ii', *cpu) + bytes(24 if big else 12) for cpu in cpus)
    path.write_bytes(struct.pack('>II', 0xcafebabf if big else 0xcafebabe, len(cpus)) + entries + bytes(64))


class Stage0(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())

    def stage0(self, strings):
        path = self.dir / 'stage0'
        path.write_bytes(struct.pack('<III', 0xfeedfacf, *ARM64) + bytes(64) + b'\x00'.join(strings) + b'\x00')
        return str(path)

    def test_the_mount_point_is_where_it_runs_init_from(self):
        self.assertEqual(mkimage26.stage0_root(self.stage0([b'apfs', b'/hakios/sbin/init', b'/dev/md1'])), 'hakios')
        # one built from a tree that still says another name: that name, which the system volume then links
        self.assertEqual(mkimage26.stage0_root(self.stage0([b'/older/sbin/init', b'/older'])), 'older')

    def test_without_one_it_is_the_image_root(self):
        self.assertEqual(mkimage26.stage0_root(str(self.dir / 'none')), mkimage26.IMAGE_ROOT)
        self.assertEqual(mkimage26.stage0_root(self.stage0([b'/bin/sh', b'/sbin/initx'])), mkimage26.IMAGE_ROOT)


class Arm64e(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())

    def test_a_thin_file_is_arm64e_by_its_subtype(self):
        thin(ARM64E, self.dir / 'e')
        thin(ARM64, self.dir / 'plain')
        thin((0x0100000c, 0x80000002), self.dir / 'e-with-caps')     # (the capability bits are not the subtype)
        self.assertTrue(mkimage26.is_arm64e_macho(self.dir / 'e'))
        self.assertFalse(mkimage26.is_arm64e_macho(self.dir / 'plain'))
        self.assertTrue(mkimage26.is_arm64e_macho(self.dir / 'e-with-caps'))

    def test_a_fat_file_is_arm64e_when_one_of_its_slices_is(self):
        fat([ARM64, ARM64E], self.dir / 'both')
        fat([ARM64], self.dir / 'one')
        fat([(7, 3), ARM64E], self.dir / 'big', big=True)
        self.assertTrue(mkimage26.is_arm64e_macho(self.dir / 'both'))
        self.assertFalse(mkimage26.is_arm64e_macho(self.dir / 'one'))
        self.assertTrue(mkimage26.is_arm64e_macho(self.dir / 'big'))

    def test_what_is_no_mach_o_is_not_one(self):
        # a Java class file: the fat magic, then its version where the count of slices would be
        (self.dir / 'A.class').write_bytes(struct.pack('>IHH', 0xcafebabe, 0, 52) + bytes(30))
        (self.dir / 'short').write_bytes(struct.pack('>II', 0xcafebabe, 4) + bytes(24))
        (self.dir / 'text').write_text('#!/bin/sh\necho hello, this is long enough to be read\n')
        (self.dir / 'tiny').write_bytes(b'\xcf\xfa\xed\xfe')
        for name in ('A.class', 'short', 'text', 'tiny', 'missing'):
            self.assertFalse(mkimage26.is_arm64e_macho(self.dir / name), name)


class Pruned(unittest.TestCase):
    def test_what_a_pruned_image_leaves_out(self):
        for rel in ('Applications/Maps.app/Maps', 'Applications/CollectionsPosterApp.app/x',
                    'System/Library/Isp/fw.bin', 'System/Library/Fonts/AppFonts/a.ttf',
                    'System/Library/CoreImage/PortraitFilters.cifilter/x.metallib',
                    'System/Library/PrivateFrameworks/CameraEffectsKit.framework/Assets.car'):
            self.assertTrue(mkimage26.pruned(rel), rel)

    def test_what_it_keeps(self):
        for rel in ('Applications/Preferences.app/Preferences', 'Applications/MapsLike.app/x',
                    'System/Library/CoreImage/PortraitFilters.cifilter/Info.plist',
                    'System/Library/PrivateFrameworks/CameraEffectsKit.framework/Info.plist',
                    'System/Library/Fonts/Core/SFUI.ttf', 'usr/lib/dyld'):
            self.assertFalse(mkimage26.pruned(rel), rel)


class Loctable(unittest.TestCase):
    def test_only_english_base_and_the_loc_keys_stay(self):
        d = Path(tempfile.mkdtemp())
        with open(d / 'a.loctable', 'wb') as f:
            plistlib.dump({'en': {'k': 'v'}, 'Base': {}, 'fr': {'k': 'w'}, 'ja': {}, 'LocProvenance': {}}, f,
                          fmt=plistlib.FMT_BINARY)
        self.assertTrue(mkimage26.trim_loctable(d / 'a.loctable', d / 'b.loctable'))
        with open(d / 'b.loctable', 'rb') as f:
            self.assertEqual(set(plistlib.load(f)), {'en', 'Base', 'LocProvenance'})
        (d / 'not.loctable').write_text('not a property list')
        self.assertFalse(mkimage26.trim_loctable(d / 'not.loctable', d / 'c.loctable'))
        self.assertFalse(os.path.exists(d / 'c.loctable'))


if __name__ == '__main__':
    unittest.main()
