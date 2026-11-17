#!/usr/bin/env python3
"""scripts/release.sh in a repository of the test's own: the tarball of a commit, the list of its files in it, and that
list what tools/files.py goes over once it is unpacked where there is no git -- what make check runs on in a release's
tree; the same commit giving the same tarball; release notes from the CHANGELOG's sections not released yet; the
tag; and what is refused (changes not committed, a version that is not one, a tag that is there)."""
import hashlib
import os
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHANGELOG = """# Changelog

## Unreleased: the newest

- a thing

## Unreleased: the one before

- another

## 0.1

- released
"""
GIT = dict(os.environ, GIT_AUTHOR_NAME='t', GIT_AUTHOR_EMAIL='t@t', GIT_COMMITTER_NAME='t', GIT_COMMITTER_EMAIL='t@t',
           GIT_AUTHOR_DATE='@1700000000 +0000', GIT_COMMITTER_DATE='@1700000000 +0000')


class Release(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)
        self.repo = self.dir / 'repo'
        for path in ('scripts/release.sh', 'tools/check-publishable.sh', 'tools/files.py', '.gitignore'):
            (self.repo / path).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / path, self.repo / path)
        (self.repo / 'CHANGELOG.md').write_text(CHANGELOG)
        (self.repo / 'drivers/board').mkdir(parents=True)
        (self.repo / 'drivers/board/x.c').write_text('int x;\n')
        (self.repo / 'README.md').write_text('# A repository\n')
        self.git('init', '-q')
        self.git('add', '-A')
        self.git('commit', '-qm', 'the first')

    def git(self, *arguments):
        return subprocess.run(['git', '-C', str(self.repo), *arguments], check=True, capture_output=True, text=True,
                              env=GIT).stdout

    def release(self, *arguments):
        return subprocess.run([str(self.repo / 'scripts/release.sh'), *arguments], cwd=self.repo, capture_output=True,
                              text=True, env=GIT)

    def test_the_tarball_and_its_list(self):
        done = self.release('1.0', '--no-check')
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        out = self.repo / 'build/release'
        tarball = out / 'hakios-kernel-1.0.tar.gz'
        with tarfile.open(tarball) as archive:
            names = sorted(m.name for m in archive.getmembers() if m.isfile())
            listed = archive.extractfile('hakios-kernel-1.0/.tracked-files').read().decode().split('\n')
            archive.extractall(self.dir / 'unpacked', filter='data')
        tracked = sorted(self.git('ls-files').split())
        self.assertEqual(names, sorted(f'hakios-kernel-1.0/{n}' for n in tracked + ['.tracked-files']))
        self.assertEqual([n for n in listed if n], sorted(tracked + ['.tracked-files']), 'every file of the commit, and itself')
        # unpacked where there is no git: the checks go over what the release had
        tree = self.dir / 'unpacked/hakios-kernel-1.0'
        (tree / 'build').mkdir()
        (tree / 'build/made.h').write_text('a build output\n')
        files = subprocess.run(['python3', str(tree / 'tools/files.py')], capture_output=True, text=True, check=True).stdout.split()
        self.assertEqual(files, sorted(tracked + ['.tracked-files']))
        some = subprocess.run(['python3', str(tree / 'tools/files.py'), '*.sh', 'drivers'], capture_output=True, text=True,
                              check=True).stdout.split()
        self.assertEqual(some, ['drivers/board/x.c', 'scripts/release.sh', 'tools/check-publishable.sh'])
        # without the list (a tree copied some other way): what is on disk, without the build's output
        (tree / '.tracked-files').unlink()
        files = subprocess.run(['python3', str(tree / 'tools/files.py')], capture_output=True, text=True, check=True).stdout.split()
        self.assertEqual(files, sorted(tracked))
        # the sums and the notes
        self.assertEqual((out / 'SHA256SUMS').read_text().split(),
                         [hashlib.sha256(tarball.read_bytes()).hexdigest(), 'hakios-kernel-1.0.tar.gz'])
        notes = (out / 'NOTES.md').read_text()
        self.assertIn('# HakiOS kernel 1.0', notes)
        self.assertIn('## The newest', notes)
        self.assertIn('## The one before', notes)
        self.assertNotIn('released', notes.split('## The one before')[1], 'nothing of a released section')

    def test_the_same_commit_the_same_tarball(self):
        self.assertEqual(self.release('1.0', '--no-check').returncode, 0)
        first = (self.repo / 'build/release/hakios-kernel-1.0.tar.gz').read_bytes()
        os.utime(self.repo / 'README.md')       # (a file touched, nothing changed)
        self.assertEqual(self.release('1.0', '--no-check').returncode, 0)
        self.assertEqual((self.repo / 'build/release/hakios-kernel-1.0.tar.gz').read_bytes(), first)

    def test_what_is_refused(self):
        (self.repo / 'README.md').write_text('# changed\n')
        done = self.release('1.0', '--no-check')
        self.assertEqual(done.returncode, 1)
        self.assertIn('changes that are not committed', done.stderr)
        self.git('checkout', '--', 'README.md')
        done = self.release('1.0 beta', '--no-check')
        self.assertEqual(done.returncode, 1)
        self.assertIn('a version is letters, digits', done.stderr)
        self.assertEqual(self.release('1.0', '--no-check', '--tag').returncode, 0)
        self.assertIn('v1.0', self.git('tag'))
        done = self.release('1.0', '--no-check', '--tag')
        self.assertEqual(done.returncode, 1)
        self.assertIn('the tag v1.0 is there already', done.stderr)

    def test_nothing_unpublishable_is_released(self):
        (self.repo / 'firmware.mbn').write_bytes(b'\x7fELF' + bytes(60))
        self.git('add', '-f', 'firmware.mbn')
        self.git('commit', '-qm', 'a firmware')
        done = self.release('1.0', '--no-check')
        self.assertEqual(done.returncode, 1)
        self.assertIn('something that must not be published', done.stderr)
        self.assertFalse((self.repo / 'build/release/hakios-kernel-1.0.tar.gz').exists())


if __name__ == '__main__':
    unittest.main()
