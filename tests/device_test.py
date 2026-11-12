#!/usr/bin/env python3
"""tools/device.py: a phone's package is brought into a checkout from the devices branch or from a
directory, checked, listed and taken out again; what is the repository's own or somebody's work is left
alone. Run in a small repository of its own."""
import contextlib
import importlib.util
import io
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('device', ROOT / 'tools/device.py')
device = importlib.util.module_from_spec(spec)
spec.loader.exec_module(device)
boards = device.boards

SOC = '/ {\n\tname = "sm0001";\n\tdescription = "A chip";\n};\n'


def board(name, description='A phone', more=''):
    return (f'/ {{\n\tname = "{name}";\n\tdescription = "{description}";\n\tsoc = "sm0001";\n\tfeatures = <>;\n'
            f'\tbacklight {{\n\t\ttype = "soft";\n\t}};\n{more}}};\n')


class Sandbox(unittest.TestCase):
    """A repository with the code on its first branch and two packages on a branch `devices`."""

    def git(self, *arguments):
        subprocess.run(['git', '-C', str(self.dir), '-c', 'user.name=t', '-c', 'user.email=t@localhost'] + list(arguments),
                       check=True, capture_output=True)

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.saved = (boards.ROOT, boards.DEVICES, boards.SOCS)
        boards.ROOT, boards.DEVICES, boards.SOCS = self.dir, self.dir / 'devices', self.dir / 'soc'
        self.git('init', '-q', '-b', 'code')
        (self.dir / 'soc').mkdir()
        (self.dir / 'soc/sm0001.dtsi').write_text(SOC)
        (self.dir / 'devices/emulator').mkdir(parents=True)
        (self.dir / 'devices/emulator/board.dts').write_text(board('emulator', 'An emulator'))
        (self.dir / '.gitignore').write_text('/devices/*/\n!/devices/emulator/\n')
        self.git('add', '-A')
        self.git('commit', '-q', '-m', 'the code')
        # the packages, on a branch that has nothing of the code
        self.git('checkout', '-q', '--orphan', 'devices')
        self.git('rm', '-rq', '--cached', '.')
        shutil.rmtree(self.dir / 'devices')
        shutil.rmtree(self.dir / 'soc')
        (self.dir / '.gitignore').unlink()
        for name in ('alpha', 'beta'):
            (self.dir / name / 'notes').mkdir(parents=True)
            (self.dir / name / 'board.dts').write_text(board(name, f'Phone {name}'))
            (self.dir / name / 'README.md').write_text(f'# {name}\n')
            (self.dir / name / 'notes/pins.md').write_text('pins\n')
        (self.dir / 'README.md').write_text('packages\n')
        self.git('add', '-A')
        self.git('commit', '-q', '-m', 'two packages')
        self.git('checkout', '-q', 'code')

    def tearDown(self):
        boards.ROOT, boards.DEVICES, boards.SOCS = self.saved
        shutil.rmtree(self.dir)

    def run_tool(self, *arguments):
        said, complained = io.StringIO(), io.StringIO()
        code = None
        with contextlib.redirect_stdout(said), contextlib.redirect_stderr(complained):
            try:
                code = device.main(list(arguments))
            except SystemExit as stop:
                code = stop.code
        return code, said.getvalue(), complained.getvalue()


class Packages(Sandbox):
    def test_the_devices_branch_is_where_packages_are_looked_for(self):
        code, said, _ = self.run_tool('available')
        self.assertEqual(code, 0)
        self.assertIn('2 packages', said)
        self.assertRegex(said, r'alpha +sm0001 +Phone alpha')
        self.assertRegex(said, r'beta +sm0001 +Phone beta')
        self.assertNotIn('README', said)

    def test_a_package_is_brought_in_whole_and_checked(self):
        code, said, complained = self.run_tool('add', 'alpha')
        self.assertEqual((code, complained), (0, ''))
        self.assertIn('devices/alpha: from devices (', said)
        for name in ('board.dts', 'README.md', 'notes/pins.md'):
            self.assertTrue((self.dir / 'devices/alpha' / name).is_file(), name)
        self.assertEqual(sorted(boards.load()[1]), ['alpha', 'emulator'])
        # the checkout of the code has nothing new to commit: a package is changed where it is kept
        status = subprocess.run(['git', '-C', str(self.dir), 'status', '--porcelain'], capture_output=True, text=True).stdout
        self.assertEqual(status, '')

    def test_several_at_once_and_from_a_directory(self):
        elsewhere = Path(tempfile.mkdtemp())
        try:
            (elsewhere / 'gamma').mkdir()
            (elsewhere / 'gamma/board.dts').write_text(board('gamma', 'Phone gamma'))
            self.assertEqual(self.run_tool('add', 'alpha', 'beta')[0], 0)
            code, said, _ = self.run_tool('add', 'gamma', '--from', str(elsewhere))
            self.assertEqual(code, 0)
            self.assertEqual(sorted(boards.load()[1]), ['alpha', 'beta', 'emulator', 'gamma'])
            code, said, _ = self.run_tool('available', '--from', str(elsewhere))
            self.assertRegex(said, r'gamma +sm0001 +Phone gamma')
        finally:
            shutil.rmtree(elsewhere)

    def test_the_list_says_where_each_is_from(self):
        self.run_tool('add', 'alpha')
        (self.dir / 'devices/mine').mkdir()
        (self.dir / 'devices/mine/board.dts').write_text(board('mine', 'My phone'))
        code, said, _ = self.run_tool('list')
        self.assertEqual(code, 0)
        self.assertRegex(said, r"emulator .*\[the repository's own\]")
        self.assertRegex(said, r'alpha .*\[from devices \([0-9a-f]{12}\)\]')
        self.assertRegex(said, r'mine .*\(not brought in by this tool\)')

    def test_bringing_one_in_again_takes_the_new_one(self):
        self.run_tool('add', 'alpha')
        (self.dir / 'devices/alpha/board.dts').write_text(board('alpha', 'Changed here'))
        self.assertEqual(self.run_tool('add', 'alpha')[0], 0)
        self.assertEqual(boards.load()[1]['alpha']['description'], 'Phone alpha')

    def test_work_that_this_tool_did_not_bring_in_is_not_run_over(self):
        (self.dir / 'devices/alpha').mkdir()
        (self.dir / 'devices/alpha/board.dts').write_text(board('alpha', 'My own alpha'))
        code, _, _ = self.run_tool('add', 'alpha')
        self.assertIn('was not brought in by this tool', str(code))
        self.assertEqual(boards.load()[1]['alpha']['description'], 'My own alpha')

    def test_what_is_the_repository_s_own_is_neither_replaced_nor_removed(self):
        elsewhere = Path(tempfile.mkdtemp())
        try:
            (elsewhere / 'emulator').mkdir()
            (elsewhere / 'emulator/board.dts').write_text(board('emulator', 'Another'))
            self.assertIn('part of the repository itself', str(self.run_tool('add', 'emulator', '--from', str(elsewhere))[0]))
            self.assertIn('part of the repository itself', str(self.run_tool('remove', 'emulator')[0]))
            self.assertEqual(boards.load()[1]['emulator']['description'], 'An emulator')
        finally:
            shutil.rmtree(elsewhere)

    def test_a_package_is_taken_out_again(self):
        self.run_tool('add', 'alpha', 'beta')
        code, said, _ = self.run_tool('remove', 'alpha')
        self.assertEqual(code, 0)
        self.assertFalse((self.dir / 'devices/alpha').exists())
        self.assertEqual(sorted(boards.load()[1]), ['beta', 'emulator'])
        self.assertIn('there is no package devices/alpha', str(self.run_tool('remove', 'alpha')[0]))

    def test_a_package_made_here_is_exported_to_where_packages_are_kept(self):
        (self.dir / 'devices/mine/notes').mkdir(parents=True)
        (self.dir / 'devices/mine/board.dts').write_text(board('mine', 'My phone'))
        (self.dir / 'devices/mine/notes/pins.md').write_text('pins\n')
        (self.dir / 'devices/mine/.from').write_text('somewhere\n')
        out = Path(tempfile.mkdtemp())
        try:
            code, said, _ = self.run_tool('export', 'mine', str(out))
            self.assertEqual(code, 0)
            self.assertIn('it has no README.md yet', said)
            template = (ROOT / 'devices/_template/README.md').read_text()
            self.assertIn(device.TEMPLATE_LINE, template)
            (self.dir / 'devices/mine/README.md').write_text(template)
            self.assertIn('its README.md is still the template', self.run_tool('export', 'mine', str(out))[1])
            (self.dir / 'devices/mine/README.md').write_text('# My phone\n\nBoots to the home screen.\n')
            self.assertEqual(self.run_tool('export', 'mine', str(out))[1].strip(), f'{out}/mine: the package mine')
            (self.dir / 'devices/mine/README.md').unlink()
            shutil.rmtree(out / 'mine')
            self.run_tool('export', 'mine', str(out))
            self.assertEqual(sorted(str(path.relative_to(out)) for path in out.rglob('*') if path.is_file()),
                             ['mine/board.dts', 'mine/notes/pins.md'])        # (not the note of where it came from)
            # ... and can be brought in from there again
            shutil.rmtree(self.dir / 'devices/mine')
            self.assertEqual(self.run_tool('add', 'mine', '--from', str(out))[0], 0)
            # one that is not right is not sent
            (self.dir / 'devices/mine/board.dts').write_text(board('mine', more='\twarp {\n\t\tfactor = <9>;\n\t};\n'))
            code, _, complained = self.run_tool('export', 'mine', str(out))
            self.assertEqual(code, 1)
            self.assertIn('not exported', complained)
            self.assertNotIn('warp', (out / 'mine/board.dts').read_text())
            self.assertIn('there is no package devices/none', str(self.run_tool('export', 'none', str(out))[0]))
            self.assertIn('is not a directory', str(self.run_tool('export', 'mine', str(out / 'nowhere'))[0]))
        finally:
            shutil.rmtree(out)

    def test_what_is_not_there_is_said_with_what_is(self):
        code, _, _ = self.run_tool('add', 'delta')
        self.assertIn('has no package "delta" (it has: alpha, beta)', str(code))
        self.assertIn('lower-case letters', str(self.run_tool('add', '../alpha')[0]))
        self.assertIn('neither a directory nor a branch', str(self.run_tool('available', '--from', 'no-such-branch')[0]))

    def test_a_package_that_does_not_fit_the_code_is_said_to(self):
        elsewhere = Path(tempfile.mkdtemp())
        try:
            (elsewhere / 'odd').mkdir()
            (elsewhere / 'odd/board.dts').write_text(board('odd', more='\twarp {\n\t\tfactor = <9>;\n\t};\n'))
            code, _, complained = self.run_tool('add', 'odd', '--from', str(elsewhere))
            self.assertEqual(code, 1)
            self.assertIn('devices/odd/board.dts: unknown section [warp]', complained)
            self.assertIn('does not fit this checkout', complained)
        finally:
            shutil.rmtree(elsewhere)


if __name__ == '__main__':
    unittest.main()
