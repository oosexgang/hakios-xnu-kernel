#!/usr/bin/env python3
"""MAINTAINERS and scripts/tests-for.py: every pattern names a file there is, every test and document is there, every
driver, tool, script and test belongs to a part, the parts are in order; and what tests-for.py makes of them."""
import contextlib
import importlib.util
import io
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('tests_for', ROOT / 'scripts/tests-for.py')
tests_for = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tests_for)

sys.path.insert(0, str(ROOT / 'tools'))
import files  # noqa: E402  (the repository's files: git's list, or a release's)

# (a file being added is there before it is tracked: what git does not ignore counts too)
TRACKED = files.listed(others=True, root=ROOT)
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


class Readmes(unittest.TestCase):
    def test_every_tool_and_script_is_in_its_readme(self):
        for directory in ('tools', 'scripts'):
            readme = (ROOT / directory / 'README.md').read_text()
            for path in sorted((ROOT / directory).iterdir()):
                name = path.name + ('/' if path.is_dir() else '')
                if name in ('README.md', '__pycache__/') or not any(f == f'{directory}/{path.name}' or f.startswith(f'{directory}/{path.name}/')
                                                                    for f in TRACKED):
                    continue
                self.assertTrue(f'`{name}' in readme or f'`{path.name} ' in readme,
                                f'{directory}/{name} is not in {directory}/README.md')


    def test_every_test_is_in_the_workbench_readme(self):
        readme = (ROOT / 'workbench/README.md').read_text()
        for path in TRACKED:
            if path.startswith('workbench/') and path.count('/') == 1 and Path(path).stem.endswith('_test'):
                self.assertTrue(f'`{Path(path).name}' in readme, f'{path} is not in workbench/README.md')


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
        self.assertIn('workbench/mutate.sh torch_driver_test board/board.c x x', text)
        self.assertIn('tools/image.py: IMAGES', text)
        self.assertIn('python3 workbench/image_test.py', text)
        self.assertIn('Documentation/images.md', text)

    def test_what_is_run_here_and_what_elsewhere(self):
        lines = ['workbench/mutate.sh torch_driver_test board/board.c x x', 'python3 workbench/image_test.py',
                 'workbench/qemu_boot_test.sh (with a kernel: make qemu)', 'make test (workbench/run.sh: x)', 'workbench/qmi/run.sh']
        self.assertEqual(tests_for.runnable(lines), [lines[0], lines[1], lines[4]])
        said = io.StringIO()
        with contextlib.redirect_stdout(said):
            code = tests_for.run(['true', 'false', 'kernel/verify.sh (make verify-kernel; network)'])
        self.assertEqual(code, 1)
        self.assertIn('1 of 2 passed: not false (1 run otherwise: kernel/verify.sh (make verify-kernel; network))', said.getvalue())

    def test_a_file_of_no_part_is_said_to_be(self):
        said, complained = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(said), contextlib.redirect_stderr(complained):
            tests_for.main(['CHANGELOG.md'])
        self.assertIn('CHANGELOG.md: in no part of MAINTAINERS', complained.getvalue())
        self.assertIn('make check', said.getvalue())


spec = importlib.util.spec_from_file_location('new_driver', ROOT / 'scripts/new-driver.py')
new_driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(new_driver)


class NewDriver(unittest.TestCase):
    """scripts/new-driver.py on a copy of what it changes: the example, run.sh's list, MAINTAINERS."""

    def setUp(self):
        import shutil
        import tempfile
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)
        for path in ('Documentation/examples/example_led.c', 'Documentation/examples/example_led_test.c', 'workbench/run.sh',
                     'MAINTAINERS'):
            (self.root / path).parent.mkdir(parents=True, exist_ok=True)
            (self.root / path).write_text((ROOT / path).read_text())
        (self.root / 'drivers/board').mkdir(parents=True)

    def make(self, *arguments):
        said = io.StringIO()
        with contextlib.redirect_stdout(said):
            try:
                code = new_driver.main(list(arguments), root=self.root)
            except SystemExit as stop:
                code = stop.code
        return code, said.getvalue()

    def test_a_driver_and_its_test_made_and_listed(self):
        code, said = self.make('fan_ctl', '--part', 'a fan')
        self.assertEqual(code, 0, said)
        driver = (self.root / 'drivers/board/fan_ctl.c').read_text()
        test = (self.root / 'workbench/fan_ctl_driver_test.c').read_text()
        self.assertIn('fan_ctl: what it drives', driver)
        self.assertIn('hakios_fan_ctl_init(void)', driver)
        self.assertIn('PE_parse_boot_argn("fan_ctl"', driver)
        self.assertIn('hakios_say("fan_ctl: ', driver)
        self.assertNotRegex(driver, r'\bled[:=_"]|example_led_|LED_PIN')   # (its head names the example it came from)
        self.assertIn('#include "board/fan_ctl.c"', test)
        self.assertIn('boot_args = "fan_ctl=2"', test)
        self.assertNotRegex(test, r'"led|example_led')
        self.assertRegex((self.root / 'workbench/run.sh').read_text(), r'\bfan_ctl_driver_test example_led_test; do')
        areas = tests_for.areas((self.root / 'MAINTAINERS').read_text())
        names = [name for name, _ in areas]
        self.assertIn('A FAN', names)
        self.assertEqual(names, sorted(names), 'in its place')
        self.assertEqual(dict(areas)['A FAN']['T'], ['workbench/fan_ctl_driver_test.c'])
        self.assertIn('workbench/mutate.sh fan_ctl_driver_test board/fan_ctl.c x x', said)

    def test_once_and_only_by_a_good_name(self):
        self.assertEqual(self.make('fan')[0], 0)
        self.assertIn('there already', str(self.make('fan')[0]))
        self.assertIn('lower-case', str(self.make('Fan')[0]))
        self.assertIn('lower-case', str(self.make('../fan')[0]))

    def test_a_failed_run_leaves_no_files_so_the_retry_works(self):
        # if run.sh has no list to extend, new-driver.py must fail before writing anything; otherwise the driver and
        # its test are left behind and every retry stops at "there already", run.sh and MAINTAINERS never updated
        (self.root / 'workbench/run.sh').write_text('#!/bin/sh\necho nothing to extend here\n')
        code, said = self.make('fan_ctl', '--part', 'a fan')
        self.assertIn('run.sh has no list', str(code))
        self.assertFalse((self.root / 'drivers/board/fan_ctl.c').exists(), 'no driver left behind')
        self.assertFalse((self.root / 'workbench/fan_ctl_driver_test.c').exists(), 'no test left behind')
        self.assertNotIn('a fan', (self.root / 'MAINTAINERS').read_text())
        # with run.sh restored, a plain rerun now succeeds (nothing was left to block it)
        (self.root / 'workbench/run.sh').write_text((ROOT / 'workbench/run.sh').read_text())
        self.assertEqual(self.make('fan_ctl', '--part', 'a fan')[0], 0)
        self.assertTrue((self.root / 'drivers/board/fan_ctl.c').exists())


if __name__ == '__main__':
    unittest.main()
