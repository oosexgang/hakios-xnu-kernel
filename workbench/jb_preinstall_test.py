"""The packages components/tweaks/tools/jb-preinstall.py puts into the prefix, run as mk-jb.sh runs it on .debs made
here: a package goes in and into dpkg's database; one whose SHA256 is not the index's, one that writes through a link
leading out of the prefix and one with a hard link to a file outside it are refused, and nothing lands outside.

    python3 workbench/jb_preinstall_test.py
"""
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'components/tweaks/tools/jb-preinstall.py'


def ar(members):
    """An ar archive, as a .deb is one."""
    out = b'!<arch>\n'
    for name, data in members:
        out += b'%-16s%-12d%-6d%-6d%-8s%-10d`\n' % (name.encode(), 0, 0, 0, b'100644', len(data)) + data
        if len(data) % 2:
            out += b'\n'
    return out


def tar_gz(entries):
    """(name, kind, what) as a .tar.gz: kind 'dir', 'file' (what: its bytes), 'symlink' or 'hardlink' (what: the link)."""
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode='w:gz') as tar:
        for name, kind, what in entries:
            info = tarfile.TarInfo(name)
            info.mode = 0o755 if kind == 'dir' else 0o644
            if kind == 'dir':
                info.type = tarfile.DIRTYPE
                tar.addfile(info)
            elif kind == 'file':
                info.size = len(what)
                tar.addfile(info, io.BytesIO(what))
            else:
                info.type = tarfile.SYMTYPE if kind == 'symlink' else tarfile.LNKTYPE
                info.linkname = what
                tar.addfile(info)
    return buffer.getvalue()


@unittest.skipUnless(shutil.which('ar'), 'no ar on this machine (binutils)')
class Preinstall(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)
        self.prefix = self.dir / 'root'
        self.outside = self.dir / 'outside'
        self.debs = self.dir / 'debs'
        for d in (self.prefix / 'var/jb/Library/dpkg', self.prefix / 'var/jb/usr/lib', self.outside, self.debs):
            d.mkdir(parents=True)
        self.status = self.prefix / 'var/jb/Library/dpkg/status'
        self.status.write_text('Package: base\nVersion: 1\nStatus: install ok installed\n')
        (self.outside / 'secret').write_text('not the image\'s\n')

    def run_tool(self, entries, sha256=None):
        """The package "pkg" 1.0 with these data entries; its stanza's SHA256 the .deb's own unless given."""
        deb = ar([('debian-binary', b'2.0\n'), ('control.tar.gz', tar_gz([('./control', 'file', b'Package: pkg\n')])),
                  ('data.tar.gz', tar_gz([('./', 'dir', None), ('./var/', 'dir', None), ('./var/jb/', 'dir', None)] + entries))])
        (self.debs / 'pkg_1.0_iphoneos-arm64.deb').write_bytes(deb)
        index = self.dir / 'Packages'
        index.write_text('Package: pkg\nVersion: 1.0\nFilename: pool/main/pkg_1.0_iphoneos-arm64.deb\nSHA256: %s\n\n'
                         % (sha256 or hashlib.sha256(deb).hexdigest()))
        return subprocess.run([sys.executable, str(TOOL), str(self.prefix), str(index), str(self.debs), 'pkg'],
                              capture_output=True, text=True, env=dict(os.environ, MIRROR='http://127.0.0.1:9'))

    def nothing_outside(self):
        self.assertEqual(sorted(p.name for p in self.outside.iterdir()), ['secret'])
        self.assertEqual((self.outside / 'secret').read_text(), 'not the image\'s\n')

    def test_a_package_goes_in(self):
        # (a link inside the prefix, relative, is followed; an absolute one is put in as it is, for the phone)
        (self.prefix / 'var/jb/lib').symlink_to('usr/lib')
        result = self.run_tool([('./var/jb/lib/libx.dylib', 'file', b'x'), ('./var/jb/bin/', 'dir', None),
                                ('./var/jb/bin/tool', 'symlink', '/var/jb/usr/bin/tool')])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.prefix / 'var/jb/usr/lib/libx.dylib').read_bytes(), b'x')
        self.assertEqual(os.readlink(self.prefix / 'var/jb/bin/tool'), '/var/jb/usr/bin/tool')
        self.assertIn('Package: pkg\nVersion: 1.0\n', self.status.read_text())
        self.assertNotIn('SHA256', self.status.read_text())

    def test_a_deb_that_is_not_the_index_s_is_refused(self):
        result = self.run_tool([('./var/jb/usr/lib/libx.dylib', 'file', b'x')], sha256='0' * 64)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('SHA256', result.stderr)
        self.assertFalse((self.prefix / 'var/jb/usr/lib/libx.dylib').exists())
        self.assertNotIn('pkg', self.status.read_text())

    def test_a_link_out_of_the_prefix_is_not_written_through(self):
        # the package's own link, then a file under it
        result = self.run_tool([('./var/jb/escape', 'symlink', str(self.outside)),
                                ('./var/jb/escape/planted', 'file', b'planted\n')])
        self.assertNotEqual(result.returncode, 0)
        self.nothing_outside()

    def test_a_link_the_prefix_has_is_not_written_through(self):
        # one of the prefix's absolute links: on this machine it leads to this machine's path, not the prefix's
        (self.prefix / 'var/jb/etc').symlink_to(self.outside)
        result = self.run_tool([('./var/jb/etc/planted', 'file', b'planted\n')])
        self.assertNotEqual(result.returncode, 0)
        self.nothing_outside()

    def test_a_hard_link_to_a_file_outside_is_refused(self):
        climb = os.path.relpath(self.outside / 'secret', self.prefix)
        result = self.run_tool([('./var/jb/usr/lib/copy', 'hardlink', climb)])
        self.assertNotEqual(result.returncode, 0)
        copy = self.prefix / 'var/jb/usr/lib/copy'
        self.assertFalse(copy.exists() and os.path.samefile(copy, self.outside / 'secret'))
        self.nothing_outside()


if __name__ == '__main__':
    unittest.main()
