#!/usr/bin/env python3
"""tools/device.py: a phone's package is brought into a checkout from the devices branch or from a
directory, checked, listed and taken out again; what is the repository's own or somebody's work is left
alone. Run in a small repository of its own."""
import contextlib
import importlib.util
import io
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# The tests describe packages of their own; those of the machine they run on (HAKIOS_DEVICES) are not theirs.
os.environ.pop('HAKIOS_DEVICES', None)
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
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)     # (also when what follows fails)
        self.saved = (boards.ROOT, boards.DEVICES)
        boards.ROOT, boards.DEVICES = self.dir, self.dir / 'arch/arm64/boot/dts'
        self.addCleanup(setattr, boards, 'DEVICES', self.saved[1])
        self.addCleanup(setattr, boards, 'ROOT', self.saved[0])
        self.git('init', '-q', '-b', 'code')
        (self.dir / 'arch/arm64/boot/dts/emulator').mkdir(parents=True)
        (self.dir / 'arch/arm64/boot/dts/emulator/board.dts').write_text(board('emulator', 'An emulator'))
        (self.dir / 'arch/arm64/boot/dts/emulator/soc.dtsi').write_text(SOC)
        (self.dir / '.gitignore').write_text('/arch/arm64/boot/dts/*/\n!/arch/arm64/boot/dts/emulator/\n')
        self.git('add', '-A')
        self.git('commit', '-q', '-m', 'the code')
        # the packages, on a branch that has nothing of the code
        self.git('checkout', '-q', '--orphan', 'devices')
        self.git('rm', '-rq', '--cached', '.')
        shutil.rmtree(self.dir / 'arch')
        (self.dir / '.gitignore').unlink()
        for name in ('alpha', 'beta'):
            (self.dir / name / 'notes').mkdir(parents=True)
            (self.dir / name / 'board.dts').write_text(board(name, f'Phone {name}'))
            (self.dir / name / 'soc.dtsi').write_text(SOC)
            (self.dir / name / 'README.md').write_text(f'# {name}\n')
            (self.dir / name / 'notes/pins.md').write_text('pins\n')
        (self.dir / 'README.md').write_text('packages\n')
        self.git('add', '-A')
        self.git('commit', '-q', '-m', 'two packages')
        self.git('checkout', '-q', 'code')

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
        self.assertIn('arch/arm64/boot/dts/alpha: from devices (', said)
        for name in ('board.dts', 'README.md', 'notes/pins.md'):
            self.assertTrue((self.dir / 'arch/arm64/boot/dts/alpha' / name).is_file(), name)
        self.assertEqual(sorted(boards.load()[1]), ['alpha', 'emulator'])
        # the checkout of the code has nothing new to commit: a package is changed where it is kept
        status = subprocess.run(['git', '-C', str(self.dir), 'status', '--porcelain'], capture_output=True, text=True).stdout
        self.assertEqual(status, '')

    def test_several_at_once_and_from_a_directory(self):
        elsewhere = Path(tempfile.mkdtemp())
        try:
            (elsewhere / 'gamma').mkdir()
            (elsewhere / 'gamma/board.dts').write_text(board('gamma', 'Phone gamma'))
            (elsewhere / 'gamma/soc.dtsi').write_text(SOC)
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
        (self.dir / 'arch/arm64/boot/dts/mine').mkdir()
        (self.dir / 'arch/arm64/boot/dts/mine/board.dts').write_text(board('mine', 'My phone'))
        (self.dir / 'arch/arm64/boot/dts/mine/soc.dtsi').write_text(SOC)
        code, said, _ = self.run_tool('list')
        self.assertEqual(code, 0)
        self.assertRegex(said, r"emulator .*\[the repository's own\]")
        self.assertRegex(said, r'alpha .*\[from devices \([0-9a-f]{12}\)\]')
        self.assertRegex(said, r'mine .*\(not brought in by this tool\)')

    def test_bringing_one_in_again_takes_the_new_one(self):
        self.run_tool('add', 'alpha')
        (self.dir / 'arch/arm64/boot/dts/alpha/board.dts').write_text(board('alpha', 'Changed here'))
        self.assertEqual(self.run_tool('add', 'alpha')[0], 0)
        self.assertEqual(boards.load()[1]['alpha']['description'], 'Phone alpha')

    def test_an_interrupted_add_leaves_the_old_package_intact(self):
        # a failure part way through add (a Ctrl-C, or git show erroring) must not leave a half package where the old
        # good one was, nor a stray staging copy
        self.assertEqual(self.run_tool('add', 'alpha')[0], 0)
        alpha = self.dir / 'arch/arm64/boot/dts/alpha'
        self.assertTrue((alpha / device.NOTE).exists())
        original = device.Source.copy
        def boom(self, name, to):
            original(self, name, to)
            raise KeyboardInterrupt('interrupted mid-copy')
        device.Source.copy = boom
        try:
            with self.assertRaises(KeyboardInterrupt):
                device.main(['add', 'alpha'])
        finally:
            device.Source.copy = original
        self.assertTrue((alpha / 'board.dts').is_file(), 'the old package is still whole')
        self.assertTrue((alpha / device.NOTE).exists(), 'and still marked as brought in')
        self.assertFalse((self.dir / 'arch/arm64/boot/dts/.alpha.new').exists(), 'no staging copy left behind')
        self.assertEqual(sorted(boards.load()[1]), ['alpha', 'emulator'])

    def test_work_that_this_tool_did_not_bring_in_is_not_run_over(self):
        (self.dir / 'arch/arm64/boot/dts/alpha').mkdir()
        (self.dir / 'arch/arm64/boot/dts/alpha/board.dts').write_text(board('alpha', 'My own alpha'))
        (self.dir / 'arch/arm64/boot/dts/alpha/soc.dtsi').write_text(SOC)
        code, _, _ = self.run_tool('add', 'alpha')
        self.assertIn('was not brought in by this tool', str(code))
        self.assertEqual(boards.load()[1]['alpha']['description'], 'My own alpha')

    def test_what_is_the_repository_s_own_is_neither_replaced_nor_removed(self):
        elsewhere = Path(tempfile.mkdtemp())
        try:
            (elsewhere / 'emulator').mkdir()
            (elsewhere / 'emulator/board.dts').write_text(board('emulator', 'Another'))
            (elsewhere / 'emulator/soc.dtsi').write_text(SOC)
            self.assertIn('part of the repository itself', str(self.run_tool('add', 'emulator', '--from', str(elsewhere))[0]))
            self.assertIn('part of the repository itself', str(self.run_tool('remove', 'emulator')[0]))
            self.assertEqual(boards.load()[1]['emulator']['description'], 'An emulator')
        finally:
            shutil.rmtree(elsewhere)

    def test_a_package_is_taken_out_again(self):
        self.run_tool('add', 'alpha', 'beta')
        code, said, _ = self.run_tool('remove', 'alpha')
        self.assertEqual(code, 0)
        self.assertFalse((self.dir / 'arch/arm64/boot/dts/alpha').exists())
        self.assertEqual(sorted(boards.load()[1]), ['beta', 'emulator'])
        self.assertIn('there is no package arch/arm64/boot/dts/alpha', str(self.run_tool('remove', 'alpha')[0]))

    def test_a_package_made_here_is_exported_to_where_packages_are_kept(self):
        (self.dir / 'arch/arm64/boot/dts/mine/notes').mkdir(parents=True)
        (self.dir / 'arch/arm64/boot/dts/mine/board.dts').write_text(board('mine', 'My phone'))
        (self.dir / 'arch/arm64/boot/dts/mine/soc.dtsi').write_text(SOC)
        (self.dir / 'arch/arm64/boot/dts/mine/notes/pins.md').write_text('pins\n')
        (self.dir / 'arch/arm64/boot/dts/mine/.from').write_text('somewhere\n')
        out = Path(tempfile.mkdtemp())
        try:
            code, said, _ = self.run_tool('export', 'mine', str(out))
            self.assertEqual(code, 0)
            self.assertIn('it has no README.md yet', said)
            template = (ROOT / 'arch/arm64/boot/dts/_template/README.md').read_text()
            self.assertIn(device.TEMPLATE_LINE, template)
            (self.dir / 'arch/arm64/boot/dts/mine/README.md').write_text(template)
            self.assertIn('its README.md is still the template', self.run_tool('export', 'mine', str(out))[1])
            (self.dir / 'arch/arm64/boot/dts/mine/README.md').write_text('# My phone\n\nBoots to the home screen.\n')
            self.assertEqual(self.run_tool('export', 'mine', str(out))[1].strip(), f'{out}/mine: the package mine')
            (self.dir / 'arch/arm64/boot/dts/mine/README.md').unlink()
            shutil.rmtree(out / 'mine')
            self.run_tool('export', 'mine', str(out))
            # (the Android tree the package was started from lies beside it, as Documentation/porting.md names it, with
            # a dump: neither is sent to where packages are published)
            (self.dir / 'arch/arm64/boot/dts/mine/phone.dts').write_text('/ { };\n')
            (self.dir / 'arch/arm64/boot/dts/mine/fdt.dtb').write_bytes(b'\xd0\x0d\xfe\xed')
            self.run_tool('export', 'mine', str(out))
            self.assertEqual(sorted(str(path.relative_to(out)) for path in out.rglob('*') if path.is_file()),
                             ['mine/board.dts', 'mine/notes/pins.md', 'mine/soc.dtsi'])    # (not the note of where it came from)
            # ... and can be brought in from there again
            shutil.rmtree(self.dir / 'arch/arm64/boot/dts/mine')
            self.assertEqual(self.run_tool('add', 'mine', '--from', str(out))[0], 0)
            # one that is not right is not sent
            (self.dir / 'arch/arm64/boot/dts/mine/board.dts').write_text(board('mine', more='\twarp {\n\t\tfactor = <9>;\n\t};\n'))
            (self.dir / 'arch/arm64/boot/dts/mine/soc.dtsi').write_text(SOC)
            code, _, complained = self.run_tool('export', 'mine', str(out))
            self.assertEqual(code, 1)
            self.assertIn('not exported', complained)
            self.assertNotIn('warp', (out / 'mine/board.dts').read_text())
            self.assertIn('there is no package arch/arm64/boot/dts/none', str(self.run_tool('export', 'none', str(out))[0]))
            self.assertIn('is not a directory', str(self.run_tool('export', 'mine', str(out / 'nowhere'))[0]))
        finally:
            shutil.rmtree(out)

    def phone_repository(self, name, packages=None, phone='zeta'):
        """Somebody's own repository NAME.git: one phone (PHONE) at its top, or (PACKAGES) a directory each."""
        work = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, work)
        source = work / 'work'
        for package in packages or [None]:
            at = source / package if package else source
            at.mkdir(parents=True)
            (at / 'board.dts').write_text(board(package or phone, f'Phone {package or phone}'))
            (at / 'soc.dtsi').write_text(SOC)
            (at / 'README.md').write_text('mine\n')
        run = ['git', '-C', str(source), '-c', 'user.name=t', '-c', 'user.email=t@localhost']
        subprocess.run(['git', 'init', '-q', '-b', 'main', str(source)], check=True)
        subprocess.run(run + ['add', '-A'], check=True)
        subprocess.run(run + ['commit', '-q', '-m', 'my phone'], check=True)
        subprocess.run(run + ['tag', 'v1'], check=True)
        bare = work / f'{name}.git'
        subprocess.run(['git', 'clone', '-q', '--bare', str(source), str(bare)], check=True)
        return bare, source

    def test_a_phone_in_a_repository_of_its_own(self):
        bare, source = self.phone_repository('My_Phone-port')         # (called anything: the board names the package)
        url = f'file://{bare}'
        code, said, _ = self.run_tool('available', '--from', url)
        self.assertEqual(code, 0, said)
        self.assertRegex(said, r'1 package\n  zeta +sm0001 +Phone zeta')
        code, said, complained = self.run_tool('add', 'zeta', '--from', url)
        self.assertEqual((code, complained), (0, ''))
        self.assertRegex(said, r'arch/arm64/boot/dts/zeta: from file://.*My_Phone-port\.git \([0-9a-f]{12}\)')
        for name in ('board.dts', 'soc.dtsi', 'README.md'):
            self.assertTrue((self.dir / 'arch/arm64/boot/dts/zeta' / name).is_file(), name)
        self.assertFalse((self.dir / 'arch/arm64/boot/dts/zeta/.git').exists(), 'the repository itself is not copied')
        self.assertEqual(sorted(boards.load()[1]), ['emulator', 'zeta'])
        self.assertRegex(self.run_tool('list')[1], r'zeta .*\[from file://')
        # taken again, as the repository has it now
        (source / 'board.dts').write_text(board('zeta', 'Phone zeta, later'))
        subprocess.run(['git', '-C', str(source), '-c', 'user.name=t', '-c', 'user.email=t@localhost', 'commit', '-qam', 'later'],
                       check=True)
        subprocess.run(['git', '-C', str(source), 'push', '-q', str(bare), 'main'], check=True)
        self.assertEqual(self.run_tool('add', 'zeta', '--from', url)[0], 0)
        self.assertEqual(boards.load()[1]['zeta']['description'], 'Phone zeta, later')
        # or as it was at a tag
        self.assertEqual(self.run_tool('add', 'zeta', '--from', url + '#v1')[0], 0)
        self.assertEqual(boards.load()[1]['zeta']['description'], 'Phone zeta')

    def test_each_package_brought_in_again_from_where_it_came(self):
        bare, source = self.phone_repository('zeta-repo')
        run = ['git', '-C', str(source), '-c', 'user.name=t', '-c', 'user.email=t@localhost']
        self.assertEqual(self.run_tool('add', '--from', f'file://{bare}')[0], 0)
        self.assertEqual(self.run_tool('add', 'alpha')[0], 0)
        (source / 'board.dts').write_text(board('zeta', 'Phone zeta, newer'))
        subprocess.run(run + ['commit', '-qam', 'newer'], check=True)
        subprocess.run(['git', '-C', str(source), 'push', '-q', str(bare), 'main'], check=True)
        code, said, complained = self.run_tool('update')
        self.assertEqual((code, complained), (0, ''), said)
        self.assertIn(f'zeta: from file://{bare}', said)
        self.assertIn('alpha: from devices', said)
        self.assertEqual(boards.load()[1]['zeta']['description'], 'Phone zeta, newer')
        code, said, _ = self.run_tool('update', 'alpha')
        self.assertEqual(code, 0)
        self.assertNotIn('zeta', said, 'only the one named')
        self.assertIn('was not brought in by this tool', str(self.run_tool('update', 'emulator')[0]))

    def test_a_repository_of_several_phones_and_one_at_a_commit(self):
        bare, source = self.phone_repository('phones', ['eta', 'theta'])
        commit = subprocess.run(['git', '-C', str(source), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
        code, said, _ = self.run_tool('add', 'eta', '--from', f'{bare}#{commit}')
        self.assertEqual(code, 0, said)
        self.assertEqual(sorted(boards.load()[1]), ['emulator', 'eta'])
        code, said, _ = self.run_tool('add', '--from', str(bare))     # (no name: all it has)
        self.assertEqual(code, 0, said)
        self.assertEqual(sorted(boards.load()[1]), ['emulator', 'eta', 'theta'])
        self.assertIn('which phone?', str(self.run_tool('add')[0]))

    def test_a_repository_that_is_not_there_is_said_to_be(self):
        code = self.run_tool('add', 'zeta', '--from', f'file://{self.dir}/nothing.git')[0]
        self.assertIn('the repository cannot be cloned', str(code))

    def test_a_directory_with_one_phone_at_its_top(self):
        elsewhere = Path(tempfile.mkdtemp()) / 'Iota_Phone'
        self.addCleanup(shutil.rmtree, elsewhere.parent)
        elsewhere.mkdir()
        (elsewhere / 'board.dts').write_text(board('iota-phone', 'Phone iota'))
        (elsewhere / 'soc.dtsi').write_text(SOC)
        code, said, _ = self.run_tool('add', 'iota-phone', '--from', str(elsewhere))
        self.assertEqual(code, 0, said)
        self.assertEqual(sorted(boards.load()[1]), ['emulator', 'iota-phone'])

    def test_an_address_that_is_an_option_is_none(self):
        marker = self.dir / 'ran'
        for given in (f'--upload-pack=touch {marker}', '-uhttps://x.git', f'https://example.org/phone.git#--upload-pack=touch {marker}'):
            with self.assertRaises(SystemExit) as stop:
                device.clone(given)
            self.assertIn('not a repository\'s address', str(stop.exception), given)
        self.assertFalse(marker.exists(), 'nothing of it run')

    def test_a_ref_that_climbs_out_removes_nothing_outside(self):
        marker = boards.ROOT / 'still-here'
        marker.write_text('x')
        # ('NAME@..', then up: three more reach build/, four the repository -- here this test's own directory, which is
        # as far as a broken one would reach; one more would be past it)
        for given in ('file:///nowhere/phone.git#../../..', 'file:///nowhere/phone.git#../../../..'):
            with self.assertRaises(SystemExit) as stop:
                device.clone(given)
            self.assertIn('cannot be cloned', str(stop.exception), given)
            self.assertTrue(marker.exists(), f'{given}: the repository itself was removed')
            self.assertTrue((boards.ROOT / 'build').is_dir(), given)

    def test_a_phone_s_repository_made_then_brought_in(self):
        target = Path(tempfile.mkdtemp()) / 'my-phone'
        self.addCleanup(shutil.rmtree, target.parent)
        code, said, complained = self.run_tool('init', str(target), '--name', 'kappa', '--soc', 'sm0001', '--description', 'Phone kappa')
        self.assertEqual(code, 0, said + complained)
        for name in ('board.dts', 'soc.dtsi', 'BUILDING.md', '.gitignore', '.github/workflows/check.yml'):
            self.assertTrue((target / name).is_file(), name)
        self.assertTrue((target / '.git').is_dir(), 'a git repository')
        self.assertIn('name = "sm0001"', (target / 'soc.dtsi').read_text())
        self.assertIn('make image DEVICE=kappa', (target / 'BUILDING.md').read_text())
        # (the phone is named: a board that is new has no uefi { } section for the image's tool to choose it by)
        self.assertIn('make phone FROM=URL-OF-THIS-REPOSITORY DEVICE=kappa', (target / 'BUILDING.md').read_text())
        self.assertFalse((self.dir / 'arch/arm64/boot/dts/kappa').exists(), 'nothing of it left in the checkout')
        # what is said is where the files are now, not where they were made on the way
        self.assertNotIn('arch/arm64/boot/dts/kappa', said)
        self.assertNotIn('next: tools/boards.py check', said)
        for name in ('board.dts', 'soc.dtsi', 'BUILDING.md', '.gitignore', '.github/workflows/check.yml'):
            self.assertIn(f'\n  {name}\n', said)
        self.assertIn(f'tools/device.py add --from {target}', said)
        # what a build reads of it, and only that: not its git, its checks, its images or how to build from it ...
        (target / 'images/v1').mkdir(parents=True)
        (target / 'images/v1/kappa.payload').write_bytes(b'XNUP')
        # ... and not what lies beside a package where it is worked on, which its .gitignore keeps out of git and a
        # plain copy would have taken into the checkout: the vendor's firmware, the Android tree, what the tools made
        (target / 'firmware/kappa/uefi').mkdir(parents=True)
        (target / 'firmware/kappa/uefi/BootShim.bin').write_bytes(b'the vendor\'s')
        (target / 'build').mkdir()
        (target / 'build/left').write_text('made\n')
        (target / 'phone.dts').write_text('/ { };\n')
        (target / 'fdt.dtb').write_bytes(b'\xd0\x0d\xfe\xed')
        (target / 'notes').mkdir()
        (target / 'notes/pins.md').write_text('pins\n')
        (target / 'notes/build').write_text('a note of that name, not at the top: the phone\'s\n')
        (target / 'notes/dump.dtb').write_bytes(b'\xd0\x0d\xfe\xed')
        code, said, complained = self.run_tool('add', '--from', str(target))
        self.assertEqual((code, complained), (0, ''), said)
        package = self.dir / 'arch/arm64/boot/dts/kappa'
        brought = {str(p.relative_to(package)) for p in package.rglob('*') if p.is_file()}
        self.assertEqual(brought, {'board.dts', 'soc.dtsi', '.from', 'notes/pins.md', 'notes/build'})
        self.assertTrue((target / 'firmware/kappa/uefi/BootShim.bin').is_file(), 'left where it was')
        self.assertEqual(sorted(boards.load()[1]), ['emulator', 'kappa'])
        self.assertIn('1 package\n', self.run_tool('available', '--from', str(target))[1])

    def test_the_sample_s_gitignore_is_what_is_never_brought_in(self):
        top, anywhere = device.never_held()
        for name in ('firmware', 'build', 'phone.dts', 'dts-android'):
            self.assertIn(name, top)
        for name in ('*.dtb', '*.ipsw', 'identity.bin', '*.payload'):
            self.assertIn(name, anywhere)
        # (a package's own two files are never among them, whatever is added to the sample)
        ignore = device.package_only(self.dir)
        self.assertEqual(ignore(str(self.dir), ['board.dts', 'soc.dtsi', 'README.md', 'notes']), [])

    def test_which_phone_is_asked_before_the_devices_branch_is_looked_for(self):
        self.git('branch', '-D', 'devices')
        code, _, _ = self.run_tool('add')
        self.assertIn('which phone?', str(code))
        self.assertIn('--from SOURCE', str(code))
        self.assertNotIn('neither a directory', str(code))

    def test_a_repository_is_made_only_where_there_is_nothing(self):
        there = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, there)
        (there / 'something').write_text('mine\n')
        self.assertIn('not empty', str(self.run_tool('init', str(there), '--name', 'kappa', '--soc', 'sm0001')[0]))
        self.assertIn('there already', str(self.run_tool('init', str(there / 'new'), '--name', 'emulator', '--soc', 'sm0001')[0]))
        self.assertEqual(sorted(p.name for p in there.iterdir()), ['something'])

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
            (elsewhere / 'odd/soc.dtsi').write_text(SOC)
            code, _, complained = self.run_tool('add', 'odd', '--from', str(elsewhere))
            self.assertEqual(code, 1)
            self.assertIn('arch/arm64/boot/dts/odd/board.dts: unknown section warp { }', complained)
            self.assertIn('does not fit this checkout', complained)
            (elsewhere / 'odd/board.dts').write_text(board('odd').replace('features = <>;', 'features = "warp-drive";'))
            self.assertIn('does not fit this checkout', self.run_tool('add', 'odd', '--from', str(elsewhere))[2])
            # one that only lacks something is the porter's to finish, not of another time than the code: a board
            # started from a tree is like this until the offsets no tree has are filled in
            (elsewhere / 'odd/board.dts').write_text(board('odd').replace('features = <>;', 'features = "buttons-pmic";'))
            code, _, complained = self.run_tool('add', 'odd', '--from', str(elsewhere))
            self.assertEqual(code, 1)
            self.assertIn('needs spmi { } in soc.dtsi', complained)
            self.assertNotIn('does not fit this checkout', complained)
            self.assertIn('is not right yet', complained)
            self.assertIn('tools/device.py update odd', complained)
        finally:
            shutil.rmtree(elsewhere)


if __name__ == '__main__':
    unittest.main()
