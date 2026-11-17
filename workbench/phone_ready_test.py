#!/usr/bin/env python3
"""scripts/phone-ready.py: what a day with the phone needs of this machine, in a checkout of the test's own -- one phone
(zeta, with a UEFI build and a Bluetooth controller that takes a patch), a kernel and a loader that are only their first
bytes, adb and fastboot that are scripts, and a history in which the sources change after the kernel was built."""
import contextlib
import importlib.util
import io
import os
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
os.environ.pop('HAKIOS_DEVICES', None)  # (the phones of the machine it runs on are not the test's)
spec = importlib.util.spec_from_file_location('phone_ready', ROOT / 'scripts/phone-ready.py')
ready = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ready)
boards, defconfig = ready.boards, ready.defconfig

SOC = '/ {\n\tname = "sm0001";\n\tdescription = "A chip";\n};\n'
BOARD = ('/ {{\n\tname = "{name}";\n\tdescription = "Phone {name}";\n\tsoc = "sm0001";\n\tfeatures = <>;\n'
         '\tbacklight {{\n\t\ttype = "soft";\n\t}};\n'
         '\tbluetooth {{\n\t\tserial-engine = <0x898000>;\n\t\twrapper = <0x8c0000>;\n'
         '\t\tclocks = <0x23298 0x2329c 0x1e76c>;\n\t\tclock-source = <0x1e77c>;\n\t\tvote-register = <0x52010>;\n'
         '\t\tvotes = <3 0 2 1 10>;\n\t\tuart-pins = <24 25 26 27>;\n\t\tuart-function = <1>;\n\t\tenable-ppid = <0x38a>;\n'
         '\t\tsw-ctrl = <18>;\n\t\tsupplies = "L2D_E0";\n\t\tsupply-mv = <1800>;\n\t\trf-clock = "C1A_E0";\n'
         '\t\trampatch = "patch.tlv";\n\t\tnvm = "conf.bin";\n\t}};\n'
         '\tuefi {{\n\t\tfd-base = <0x9FD00000>;\n\t\tfd-size = <0x1000>;\n\t}};\n}};\n')
KERNEL = b'\xcf\xfa\xed\xfe' + bytes(range(64)) + b'\0zeta\0'
LOADER = b'MZ' + b'\0' * 62 + b'hakios-placeholder\0zeta\0' + b'\x90' * 64
DEFCONFIGS = {'phone_first': '# the first run\nbacklight=1\n', 'phone_bt': '# Bluetooth\ninclude phone_first\nbt=2\n',
              'phone_rom': '# the controller on its ROM\ninclude phone_bt\nbt_fw=0\n', 'phone_air': '# Wi-Fi\nwifi=1\n'}
HOUR = 3600


class Checkout(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)
        saved = (ready.ROOT, boards.ROOT, boards.DEVICES, defconfig.CONFIGS)
        self.addCleanup(lambda: (setattr(ready, 'ROOT', saved[0]), setattr(boards, 'ROOT', saved[1]),
                                 setattr(boards, 'DEVICES', saved[2]), setattr(defconfig, 'CONFIGS', saved[3])))
        ready.ROOT, boards.ROOT = self.dir, self.dir
        boards.DEVICES = self.dir / 'arch/arm64/boot/dts'
        defconfig.CONFIGS = self.dir / 'arch/arm64/configs'
        self.phone('zeta')
        defconfig.CONFIGS.mkdir(parents=True)
        for name, text in DEFCONFIGS.items():
            (defconfig.CONFIGS / f'{name}_defconfig').write_text(text)
        for name in ready.RUNS:                 # (the day's runs, when none is named)
            if name not in DEFCONFIGS:
                (defconfig.CONFIGS / f'{name}_defconfig').write_text(f'# {name}\nbacklight=1\n')
        (self.dir / 'firmware/zeta/uefi').mkdir(parents=True)
        (self.dir / 'firmware/zeta/uefi/fd.bin').write_bytes(b'uefi')
        (self.dir / 'firmware/zeta/identity.bin').write_bytes(b'IDNT')
        self.kernel = self.dir / 'xnu/BUILD/obj/mach.development.bcm2837'
        self.kernel.parent.mkdir(parents=True)
        self.kernel.write_bytes(KERNEL)
        self.loader = self.dir / 'arch/arm64/boot/loader.efi'
        self.loader.write_bytes(LOADER)
        (self.dir / 'ios').mkdir()
        for image in ('root26-darwin.apfs', 'system.apfs'):
            (self.dir / 'ios' / image).write_bytes(b'NXSB')
        (self.dir / 'bin').mkdir()
        for tool in ('adb', 'fastboot'):
            (self.dir / 'bin' / tool).write_text('#!/bin/sh\n')
            (self.dir / 'bin' / tool).chmod(0o755)
        (self.dir / 'bin/git').symlink_to(shutil.which('git'))     # (the one thing of this machine's on its PATH)
        self.env = {'PATH': str(self.dir / 'bin'), 'XNU_SRC': str(self.dir / 'xnu'), 'IOS': str(self.dir / 'ios')}

    def phone(self, name):
        (boards.DEVICES / name).mkdir(parents=True)
        (boards.DEVICES / name / 'board.dts').write_text(BOARD.format(name=name))
        (boards.DEVICES / name / 'soc.dtsi').write_text(SOC)

    def lines(self, *runs, name='zeta'):
        """{what: (state, text)} of the check"""
        board = boards.load()[1][name]
        report = ready.check(name, board, list(runs) or ['phone_first'], self.env)
        return {what: (state, text) for state, what, text in report.lines}

    def run_main(self, *arguments):
        said = io.StringIO()
        with mock.patch.dict(os.environ, self.env, clear=True), contextlib.redirect_stdout(said), \
                contextlib.redirect_stderr(said):
            code = ready.main(list(arguments))
        return code, said.getvalue()

    def git(self, *arguments, when=None):
        env = dict(os.environ, GIT_AUTHOR_NAME='t', GIT_AUTHOR_EMAIL='t@t', GIT_COMMITTER_NAME='t',
                   GIT_COMMITTER_EMAIL='t@t')
        if when is not None:
            env.update(GIT_AUTHOR_DATE=f'@{int(when)} +0000', GIT_COMMITTER_DATE=f'@{int(when)} +0000')
        subprocess.run(['git', '-C', str(self.dir), *arguments], check=True, capture_output=True, env=env)

    def test_all_of_it_here(self):
        found = self.lines('phone_first')
        self.assertEqual({what: state for what, (state, _) in found.items()},
                         {'fastboot': 'ok', 'adb': 'ok', 'uefi': 'ok', 'kernel': 'ok', 'loader': 'ok', 'identity': 'ok',
                          'small image': 'ok', 'system image': 'ok'})
        code, said = self.run_main('phone_first')
        self.assertEqual(code, 0, said)
        self.assertIn('phone-ready: zeta, for phone_first', said)
        self.assertTrue(said.rstrip().endswith('phone-ready: ready'), said)

    def test_nothing_built(self):
        self.kernel.unlink()
        self.loader.unlink()
        (self.dir / 'ios/root26-darwin.apfs').unlink()
        found = self.lines()
        self.assertEqual(found['kernel'], ('MISSING', f'no {self.kernel}: kernel/build.sh (make kernel)'))
        self.assertEqual(found['loader'][0], 'MISSING')
        self.assertIn('arch/arm64/boot/build.sh (make loader)', found['loader'][1])
        self.assertEqual(found['small image'][0], 'MISSING')
        self.assertIn('ios/root26-darwin.apfs: darwin/userland/root26-darwin.sh', found['small image'][1])
        self.assertEqual(found['identity'][0], 'ok', 'the one given with each run is there')
        (self.dir / 'firmware/zeta/identity.bin').unlink()
        self.assertNotIn('identity', self.lines(), 'none given, and no loader to say whether it has one: nothing said')
        code, said = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn('not ready', said)

    def test_the_first_generation_s_small_image_is_taken_too(self):
        (self.dir / 'ios/root26-darwin.apfs').unlink()
        (self.dir / 'ios/root.apfs').write_bytes(b'NXSB')
        self.assertEqual(self.lines()['small image'], ('ok', str(self.dir / 'ios/root.apfs')))
        self.env['BENCH_RAMDISK'] = 'none'
        self.assertEqual(self.lines()['small image'][0], 'NOTE')

    def test_not_a_kernel(self):
        self.kernel.write_bytes(b'\x7fELF' + bytes(64))
        self.assertEqual(self.lines()['kernel'], ('MISSING', f'{self.kernel} is not a 64-bit Mach-O kernel'))

    def test_a_kernel_built_before_the_phone_was_here(self):
        self.kernel.write_bytes(KERNEL.replace(b'\0zeta\0', b'\0qemu-virt\0'))
        state, text = self.lines()['kernel']
        self.assertEqual(state, 'MISSING')
        self.assertIn('does not know zeta: it was built before the package was here. kernel/build.sh', text)
        self.env['BENCH_KERNEL'] = str(self.kernel)
        self.assertEqual(self.lines()['kernel'][0], 'MISSING', 'one named by BENCH_KERNEL as well')

    def test_a_loader_built_before_the_phone_was_here(self):
        self.loader.write_bytes(LOADER.replace(b'\0zeta\0', b'\0qemu-virt\0'))
        state, text = self.lines()['loader']
        self.assertEqual(state, 'MISSING')
        self.assertIn('does not know zeta: it was built before the package was here', text)
        self.loader.write_bytes(LOADER.replace(b'\0zeta\0', b'\0zetaplus\0'))
        self.assertEqual(self.lines()['loader'][0], 'MISSING', 'a board whose name begins with it is not it')

    def test_built_before_the_sources_changed(self):
        (self.dir / 'drivers/board').mkdir(parents=True)
        (self.dir / 'drivers/board/x.c').write_text('int x;\n')
        (self.dir / 'arch/arm64/boot/src').mkdir(parents=True)
        (self.dir / 'arch/arm64/boot/src/loader.c').write_text('int l;\n')
        now = time.time()
        for path in ('drivers/board/x.c', 'arch/arm64/boot/src/loader.c'):
            os.utime(self.dir / path, (now - 3 * HOUR, now - 3 * HOUR))     # (written, then committed)
        self.git('init', '-q')
        self.git('add', 'drivers', 'arch/arm64/boot/src')
        self.git('commit', '-qm', 'drivers: the change', when=now - HOUR)
        os.utime(self.kernel, (now - 2 * HOUR, now - 2 * HOUR))
        found = self.lines()
        self.assertEqual(found['kernel'][0], 'OLD')
        self.assertIn('before commit', found['kernel'][1])
        self.assertIn('drivers: the change', found['kernel'][1])
        self.assertEqual(found['loader'][0], 'ok', 'the loader was built after it')
        code, said = self.run_main()
        self.assertEqual(code, 0, 'OLD stops nothing')
        self.assertIn('OLD: build it again first', said)
        # built again: current; then a change not committed, newer than it
        os.utime(self.kernel, (now, now))
        self.assertEqual(self.lines()['kernel'][0], 'ok')
        (self.dir / 'drivers/board/x.c').write_text('int x = 1;\n')
        os.utime(self.dir / 'drivers/board/x.c', (now + HOUR, now + HOUR))
        self.assertEqual(self.lines()['kernel'][0], 'OLD')
        self.assertIn('drivers/board/x.c, not committed', self.lines()['kernel'][1])
        # a commit made before the build, pulled after it: its time is older than the kernel, the file's is the pull's
        (self.dir / 'drivers/board/x.c').write_text('int x = 2;\n')
        self.git('commit', '-qam', 'drivers: made this morning, pulled tonight', when=now - 3 * HOUR)
        os.utime(self.kernel, (now + 2 * HOUR, now + 2 * HOUR))
        os.utime(self.dir / 'drivers/board/x.c', (now + 3 * HOUR, now + 3 * HOUR))
        self.assertEqual(self.lines()['kernel'][0], 'OLD', 'a commit older than the build, newer here')
        self.assertIn('drivers/board/x.c as it is here (pulled or checked out then)', self.lines()['kernel'][1])
        # a kernel named by BENCH_KERNEL is the user's to know
        self.env['BENCH_KERNEL'] = str(self.kernel)
        self.assertEqual(self.lines()['kernel'][0], 'ok')

    def test_identity(self):
        self.assertEqual(self.lines()['identity'], ('ok', str(self.dir / 'firmware/zeta/identity.bin')))
        (self.dir / 'firmware/zeta/identity.bin').unlink()
        state, text = self.lines()['identity']
        self.assertEqual(state, 'MISSING')
        self.assertIn('DeviceTree.bin', text)
        self.env['BENCH_IDENTITY'] = str(self.dir / 'mine.bin')
        self.assertEqual(self.lines()['identity'][0], 'MISSING', 'BENCH_IDENTITY of a file that is not there')
        (self.dir / 'mine.bin').write_bytes(b'IDNT')
        self.assertEqual(self.lines()['identity'], ('ok', str(self.dir / 'mine.bin') + ' (BENCH_IDENTITY)'))
        del self.env['BENCH_IDENTITY']
        self.loader.write_bytes(LOADER.replace(b'hakios-placeholder', b'iPhone99,9'))
        self.assertEqual(self.lines()['identity'][0], 'ok', 'built into the loader')

    def test_tools(self):
        (self.dir / 'bin/fastboot').unlink()
        found = self.lines()
        self.assertEqual(found['fastboot'][0], 'MISSING')
        self.assertIn('scripts/setup-ubuntu.sh phone', found['fastboot'][1])
        self.assertEqual(found['adb'][0], 'ok')

    def test_no_uefi_build(self):
        shutil.rmtree(self.dir / 'firmware/zeta/uefi')
        self.assertEqual(self.lines()['uefi'][0], 'MISSING')

    def test_bluetooth_firmware_for_a_run_at_bt_2(self):
        found = self.lines('phone_first', 'phone_bt')
        self.assertNotIn('phone_first: bt firmware', found)
        state, text = found['phone_bt: bt firmware']
        self.assertEqual(state, 'NOTE', 'the run goes, and does less')
        self.assertIn('no patch.tlv, conf.bin in', text)
        (self.dir / 'userland/fw-bt').mkdir(parents=True)
        (self.dir / 'userland/fw-bt/patch.tlv').write_bytes(b'p')
        self.assertIn('no conf.bin in', self.lines('phone_bt')['phone_bt: bt firmware'][1])
        (self.dir / 'userland/fw-bt/conf.bin').write_bytes(b'c')
        self.assertEqual(self.lines('phone_bt')['phone_bt: bt firmware'][0], 'ok')
        self.assertNotIn('phone_rom: bt firmware', self.lines('phone_rom'), 'bt_fw=0 sends none')

    def test_wifi_address(self):
        self.assertEqual(self.lines('phone_air')['phone_air: wifi-mac'][0], 'NOTE')
        (self.dir / 'firmware/zeta/wifi-mac').write_text('02:00:00:00:00:01\n')
        self.assertEqual(self.lines('phone_air')['phone_air: wifi-mac'][0], 'ok')
        self.assertNotIn('phone_first: wifi-mac', self.lines('phone_first'))

    def test_which_phone_and_which_run(self):
        self.phone('eta')
        code, said = self.run_main('phone_first')
        self.assertEqual(code, 2)
        self.assertIn('which phone? BENCH_BOARD=NAME or --board NAME (here: eta, zeta)', said)
        self.assertEqual(self.run_main('--board', 'zeta', 'phone_first')[0], 0)
        self.env['BENCH_BOARD'] = 'zeta'
        self.assertEqual(self.run_main('phone_first')[0], 0)
        code, said = self.run_main('phone_nothing')
        self.assertEqual(code, 2)
        self.assertIn('no defconfig phone_nothing', said)
        code, said = self.run_main('--board', 'theta')
        self.assertEqual(code, 2)
        self.assertIn('no phone "theta" here', said)


if __name__ == '__main__':
    unittest.main()
