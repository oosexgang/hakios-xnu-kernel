"""The parts of the Darwin image tool (darwin/tools/mkimage26.py) that need no IPSW: which files a pruned image
leaves out, which Mach-O files are arm64e and go through pacstrip, the trimming of string tables, and
where the stage0 it carries mounts the small image. And of the tools beside it: where they find pacstrip,
fips26.py run again over a cache it has already changed, aea.py's reading of an archive's metadata, which
nothing has authenticated when it is read, and the launchd jobs mkjobs.py writes for both image builders.

    python3 workbench/darwin_tools_test.py
"""
import importlib.util
import io
import os
import plistlib
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'darwin/tools' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


mkimage26 = load('mkimage26')

ARM64, ARM64E = (0x0100000c, 0), (0x0100000c, 2)


def thin(cpu, path):
    path.write_bytes(struct.pack('<III', 0xfeedfacf, *cpu) + bytes(4096))


def fat(cpus, path, big=False):
    entries = b''.join(struct.pack('>ii', *cpu) + bytes(24 if big else 12) for cpu in cpus)
    path.write_bytes(struct.pack('>II', 0xcafebabf if big else 0xcafebabe, len(cpus)) + entries + bytes(64))


class Stage0(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)

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
        self.addCleanup(shutil.rmtree, self.dir)

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
        self.addCleanup(shutil.rmtree, d)
        with open(d / 'a.loctable', 'wb') as f:
            plistlib.dump({'en': {'k': 'v'}, 'Base': {}, 'fr': {'k': 'w'}, 'ja': {}, 'LocProvenance': {}}, f,
                          fmt=plistlib.FMT_BINARY)
        self.assertTrue(mkimage26.trim_loctable(d / 'a.loctable', d / 'b.loctable'))
        with open(d / 'b.loctable', 'rb') as f:
            self.assertEqual(set(plistlib.load(f)), {'en', 'Base', 'LocProvenance'})
        (d / 'not.loctable').write_text('not a property list')
        self.assertFalse(mkimage26.trim_loctable(d / 'not.loctable', d / 'c.loctable'))
        self.assertFalse(os.path.exists(d / 'c.loctable'))


class Pacstrip(unittest.TestCase):
    """pacstrip is arch/arm64/tools/pacstrip (darwin/userland/env.sh builds it there), or PACSTRIP"""
    def test_the_tools_find_it_where_it_is_built(self):
        self.assertTrue((ROOT / 'arch/arm64/tools/pacstrip.c').exists())
        with mock.patch.dict(os.environ):
            os.environ.pop('PACSTRIP', None)
            for name in ('mkimage26', 'fips26'):
                self.assertEqual(Path(load(name).PACSTRIP), ROOT / 'arch/arm64/tools/pacstrip', name)

    def test_pacstrip_names_another(self):
        with mock.patch.dict(os.environ, PACSTRIP='/elsewhere/pacstrip'):
            for name in ('mkimage26', 'fips26'):
                self.assertEqual(load(name).PACSTRIP, '/elsewhere/pacstrip', name)


class Fips26(unittest.TestCase):
    """with a pacstrip that writes down what it was asked"""
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)
        self.cache = self.dir / 'dyld-strip'
        self.cache.mkdir()
        self.calls = self.dir / 'calls'
        self.pacstrip = self.dir / 'pacstrip'
        self.pacstrip.write_text(f'#!/bin/sh\necho "$@" >> "{self.calls}"\n')
        self.pacstrip.chmod(0o755)
        self.fips26 = load('fips26')

    def run_fips26(self, pacstrip):
        return subprocess.run([sys.executable, str(ROOT / 'darwin/tools/fips26.py'), str(self.cache)],
                              env=dict(os.environ, PACSTRIP=str(pacstrip)), capture_output=True, text=True, timeout=60)

    def test_the_stored_value_is_replaced_and_the_file_resigned(self):
        part = self.cache / 'dyld_shared_cache_arm64e.01'
        part.write_bytes(bytes(100) + self.fips26.STORED + bytes(100))
        r = self.run_fips26(self.pacstrip)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(part.read_bytes(), bytes(100) + self.fips26.COMPUTED + bytes(100))
        self.assertEqual(self.calls.read_text().split(), ['resign', str(part)])

    def test_run_again_it_resigns_what_has_the_computed_value(self):
        # (an earlier run that replaced the value and stopped before its resign: the pages' hashes are still the old)
        part = self.cache / 'dyld_shared_cache_arm64e.01'
        part.write_bytes(bytes(100) + self.fips26.COMPUTED + bytes(100))
        (self.cache / 'dyld_shared_cache_arm64e.02').write_bytes(bytes(300))
        r = self.run_fips26(self.pacstrip)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(self.calls.exists(), 'not resigned')
        self.assertEqual(self.calls.read_text().split(), ['resign', str(part)])

    def test_without_pacstrip_nothing_is_written(self):
        part = self.cache / 'dyld_shared_cache_arm64e.01'
        before = bytes(100) + self.fips26.STORED + bytes(100)
        part.write_bytes(before)
        r = self.run_fips26(self.dir / 'no-pacstrip')
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(part.read_bytes(), before)

    def test_a_cache_with_neither_value_is_refused(self):
        (self.cache / 'dyld_shared_cache_arm64e.01').write_bytes(bytes(300))
        r = self.run_fips26(self.pacstrip)
        self.assertNotEqual(r.returncode, 0)
        self.assertFalse(self.calls.exists())


def deadline(test, seconds):
    """a SIGALRM that fails the test, where a loop would otherwise hold the run for ever"""
    def expired(signum, frame):
        raise AssertionError(f'still running after {seconds} s')
    previous = signal.signal(signal.SIGALRM, expired)
    signal.alarm(seconds)
    test.addCleanup(signal.signal, signal.SIGALRM, previous)
    test.addCleanup(signal.alarm, 0)


class Jobs(unittest.TestCase):
    """The launchd jobs both image builders write (userland/userland/mkjobs.py; root26-darwin.sh runs it with --rest)."""

    MKJOBS = ROOT / 'userland/userland/mkjobs.py'
    FAIRPLAYD = {'Label': 'com.apple.fairplayd.H2', 'Program': '/usr/sbin/fairplayd.H2', 'KeepAlive': True,
                 'UserName': 'mobile', 'MachServices': {'com.apple.fairplayd': True, 'com.apple.fairplayd.xpc': True,
                                                        'com.apple.unfreed': {'HostSpecialPort': 17}}}
    IDENTITY = {'Label': 'com.apple.fairplaydeviceidentityd', 'Program': '/usr/libexec/fairplaydeviceidentityd',
                'MachServices': {'com.apple.fairplaydeviceidentityd': True}}
    KEPT = {'Label': 'com.apple.notifyd', 'Program': '/usr/sbin/notifyd', 'KeepAlive': True,
            'MachServices': {'com.apple.system.notification_center': True}}

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)
        daemons = self.dir / 'rootfs/System/Library/LaunchDaemons'
        daemons.mkdir(parents=True)
        for job in (self.FAIRPLAYD, self.IDENTITY, self.KEPT):
            (daemons / (job['Label'] + '.plist')).write_bytes(plistlib.dumps(job))
            program = self.dir / 'rootfs' / job['Program'].lstrip('/')
            program.parent.mkdir(parents=True, exist_ok=True)
            program.write_bytes(b'')

    def jobs(self, *labels, rest=False):
        out = self.dir / 'jobs'
        env = {'PATH': os.environ.get('PATH', '/usr/bin:/bin')}       # (no DARWIN_FW, ALLOW or OSLOG of this shell's)
        subprocess.run([sys.executable, str(self.MKJOBS), *(['--rest'] if rest else []), str(self.dir / 'rootfs'),
                        str(out), *labels], env=env, check=True, stdout=subprocess.DEVNULL)
        return {p.stem: plistlib.loads(p.read_bytes()) for p in out.glob('*.plist')}

    def check_fairplayd(self, job):
        self.assertIs(job['KeepAlive'], False)
        self.assertEqual(job['ThrottleInterval'], 600)
        self.assertEqual(job['MachServices'], self.FAIRPLAYD['MachServices'], 'its Mach services stay registered')
        self.assertEqual(job['Program'], self.FAIRPLAYD['Program'])

    def test_fairplayd_named_is_not_restarted_every_ten_seconds(self):
        jobs = self.jobs('com.apple.fairplayd.H2', 'com.apple.notifyd')
        self.check_fairplayd(jobs['com.apple.fairplayd.H2'])
        self.assertIs(jobs['com.apple.notifyd']['KeepAlive'], True, 'another named job keeps its KeepAlive')
        self.assertNotIn('ThrottleInterval', jobs['com.apple.notifyd'])

    def test_fairplayd_on_demand_is_not_either(self):
        jobs = self.jobs('com.apple.notifyd', rest=True)
        self.check_fairplayd(jobs['com.apple.fairplayd.H2'])
        identity = jobs['com.apple.fairplaydeviceidentityd']
        self.assertNotIn('ThrottleInterval', identity, 'fairplaydeviceidentityd is another daemon')
        self.assertNotIn('KeepAlive', identity)


class AeaMetadata(unittest.TestCase):
    """the metadata of an AEA archive, read before anything in it has been authenticated"""
    @classmethod
    def setUpClass(cls):
        try:
            cls.aea = load('aea')
        except ImportError as e:            # (Python's cryptography package)
            raise unittest.SkipTest(f'aea.py does not load here: {e}')

    @staticmethod
    def archive(*fields, raw=b''):
        auth = b''.join(struct.pack('<I', 4 + len(f)) + f for f in fields) + raw
        return io.BytesIO(struct.pack('<4sII', b'AEA1', 1, len(auth)) + auth)

    def test_fields_are_read(self):
        _, _, meta = self.aea.header(self.archive(b'a\0one', b'bee\0two'))
        self.assertEqual(meta, {'a': b'one', 'bee': b'two'})

    def test_a_field_of_length_zero_stops_the_reading(self):
        deadline(self, 5)
        with self.assertRaises(SystemExit):
            self.aea.header(self.archive(b'a\0one', raw=struct.pack('<I', 0) + b'more'))

    def test_a_length_under_four_or_past_the_end_is_refused(self):
        deadline(self, 5)
        for raw in (struct.pack('<I', 2) + b'abcdefgh', struct.pack('<I', 50) + b'k\0v', b'\x01\x00'):
            with self.assertRaises(SystemExit, msg=raw):
                self.aea.header(self.archive(b'a\0one', raw=raw))

    def test_the_key_is_fetched_over_https_only(self):
        fetched = mock.Mock(side_effect=AssertionError('fetched'))
        with mock.patch.object(self.aea.urllib.request, 'urlopen', fetched), \
                mock.patch.object(self.aea.urllib.request, 'build_opener', fetched):
            for url in (b'http://example.com/key.pem', b'file:///etc/passwd', b'ftp://example.com/key.pem',
                        b'/etc/passwd', b''):
                with self.assertRaises(SystemExit, msg=url):
                    self.aea.symmetric_key({'com.apple.wkms.fcs-key-url': url, 'com.apple.wkms.fcs-response': b'{}'})
        fetched.assert_not_called()

    def test_a_redirect_away_from_https_is_not_followed(self):
        handler = self.aea.HttpsOnly()
        request = self.aea.urllib.request.Request('https://example.com/key.pem')
        with self.assertRaises(self.aea.urllib.error.HTTPError):
            handler.redirect_request(request, None, 302, 'Found', {}, 'http://example.com/key.pem')
        self.assertIsNotNone(handler.redirect_request(request, None, 302, 'Found', {}, 'https://example.com/k.pem'))


if __name__ == '__main__':
    unittest.main()
