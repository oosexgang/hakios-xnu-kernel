#!/usr/bin/env python3
"""MAINTAINERS and scripts/tests-for.py: every pattern names a file there is, every test and document is there, every
driver, tool, script and test belongs to a part, the parts are in order; and what tests-for.py makes of them."""
import contextlib
import importlib.util
import io
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('tests_for', ROOT / 'scripts/tests-for.py')
tests_for = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tests_for)

TRACKED = [f for f in subprocess.run(['git', '-C', str(ROOT), 'ls-files'], capture_output=True, text=True,
                                     check=True).stdout.split('\n') if f]
# (a file being added is there before it is tracked: what git does not ignore counts too)
TRACKED += [f for f in subprocess.run(['git', '-C', str(ROOT), 'ls-files', '--others', '--exclude-standard'],
                                      capture_output=True, text=True, check=True).stdout.split('\n') if f]
AREAS = tests_for.areas()


def taken(path):
    return [name for name, keys in AREAS if any(tests_for.covers(p, path) for p in keys.get('F', []))]


class Maintainers(unittest.TestCase):
    def test_every_pattern_names_a_file_there_is(self):
        for name, keys in AREAS:
            for pattern in keys.get('F', []):
                self.assertTrue(any(tests_for.covers(pattern, f) for f in TRACKED), f'{name}: F: {pattern} names nothing')

    def test_every_test_and_document_is_there(self):
        for name, keys in AREAS:
            for line in keys.get('T', []) + keys.get('D', []):
                first = line.split(' ')[0]
                if first == 'make':
                    continue
                self.assertTrue((ROOT / first).exists(), f'{name}: {first} is not there')

    def test_every_driver_tool_and_script_belongs_to_a_part(self):
        prefixes = ('drivers/', 'tools/', 'scripts/', 'arch/arm64/boot/', 'include/board/', 'samples/')
        for path in TRACKED:
            if path.startswith(prefixes) and '__pycache__' not in path and not path.endswith('README.md'):
                self.assertTrue(taken(path), f'{path} belongs to no part of MAINTAINERS')

    def test_every_test_belongs_to_a_part(self):
        named = {line.split(' ')[0] for _, keys in AREAS for line in keys.get('T', [])}
        for path in TRACKED:
            if path.startswith('workbench/') and Path(path).stem.endswith('_test') and path.count('/') == 1:
                self.assertIn(path, named, f'{path} is the test of no part of MAINTAINERS')

    def test_the_parts_are_in_order_and_each_has_files(self):
        names = [name for name, _ in AREAS]
        self.assertGreater(len(names), 20)
        self.assertEqual(names, sorted(names))
        for name, keys in AREAS:
            self.assertTrue(keys.get('F'), f'{name} has no files')


class TestsFor(unittest.TestCase):
    def test_patterns(self):
        self.assertTrue(tests_for.covers('drivers/board/ufs.c', 'drivers/board/ufs.c'))
        self.assertFalse(tests_for.covers('drivers/board/ufs.c', 'drivers/board/ufshc.c'))
        self.assertTrue(tests_for.covers('fs/apfs/', 'fs/apfs/x/y.c'))
        self.assertFalse(tests_for.covers('fs/apfs/', 'fs/apfs_other.c'))
        self.assertTrue(tests_for.covers('build.config.*', 'build.config.bench'))
        self.assertFalse(tests_for.covers('drivers/board/*.c', 'drivers/board/sub/x.c'))

    def test_what_to_run_for_a_file(self):
        said = io.StringIO()
        with contextlib.redirect_stdout(said):
            tests_for.main(['drivers/board/torch.c', str(ROOT / 'tools/image.py')])
        text = said.getvalue()
        self.assertIn('drivers/board/torch.c: CAMERA AND FLASHLIGHT', text)
        self.assertIn('workbench/mutate.sh torch_driver_test x x', text)
        self.assertIn('tools/image.py: IMAGES', text)
        self.assertIn('python3 workbench/image_test.py', text)
        self.assertIn('Documentation/images.md', text)

    def test_a_file_of_no_part_is_said_to_be(self):
        said, complained = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(said), contextlib.redirect_stderr(complained):
            tests_for.main(['CHANGELOG.md'])
        self.assertIn('CHANGELOG.md: in no part of MAINTAINERS', complained.getvalue())
        self.assertIn('make check', said.getvalue())


if __name__ == '__main__':
    unittest.main()
