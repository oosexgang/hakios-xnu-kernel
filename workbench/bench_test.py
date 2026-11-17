#!/usr/bin/env python3
"""./bench against a phone of the test's own: fastboot and adb are one stand-in that plays Android, the phone's own
bootloader and the resident loader, and keeps what it was asked. In a copy of what bench uses, with a made-up phone
(zeta: a uefi { } node and its UEFI build, a boot { } node that names its bootloader's product), a kernel and a loader
that are only their first bytes. What it covers: where the phone is, as bench state says; a run from Android to the
log (the phone sent to its bootloader, the resident loader started there, the payload with the board's and the
defconfig's arguments, the log fetched and looked at); what bench refuses (another phone's bootloader, a loader that
took the phone for another board, a second command while a run holds the phone) and what it warns of (a kernel that
does not know the phone, a second run in one session of the loader)."""
import fcntl
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SOC = '/ {\n\tname = "sm0001";\n\tdescription = "A chip";\n};\n'
BOARD = ('/ {\n\tname = "zeta";\n\tdescription = "Phone zeta";\n\tsoc = "sm0001";\n\tfeatures = <>;\n'
         '\tbacklight {\n\t\ttype = "soft";\n\t};\n'
         '\tuefi {\n\t\tfd-base = <0x9FD00000>;\n\t\tfd-size = <0x1000>;\n\t};\n'
         '\tboot {\n\t\tfastboot-product = "zeta";\n\t\tcpus = <8>;\n\t\tbootargs = "backlight=1", "wifi=0";\n\t};\n};\n')
KERNEL = b'\xcf\xfa\xed\xfe' + bytes(range(64)) + b'\0zeta\0'
LOADER = b'MZ' + b'\0' * 62 + b'hakios-placeholder\0zeta\0' + b'\x90' * 64
SHIM = struct.pack('<IIQQQQQQ4sI', 0x14000010, 0, 0x9FD00000, 0x1000, 0, 0, 0, 0, b'ARMd', 0) + b'code'
KERNEL_LOG = 'Darwin Kernel Version 1\ntorch: THE LIGHT IS DARWIN\'S (/dev/hakios-torch)\nkernel returned: log at 0x1, 2 bytes\n'

# fastboot and adb, as one: the phone is in one of android, fastboot (its own bootloader), loader (ours) or absent
PHONE = r'''#!/usr/bin/env python3
import os, shutil, sys
from pathlib import Path
D = Path(os.environ['FAKE_PHONE'])
tool, args = Path(sys.argv[0]).name, sys.argv[1:]
with open(D / 'calls', 'a') as f:
    f.write(' '.join([tool] + args) + '\n')
mode = (D / 'mode').read_text().strip()
def to(m):
    (D / 'mode').write_text(m)
def said(*lines):
    for line in lines:
        print(line, file=sys.stderr)    # (fastboot says what it does on its standard error)
    sys.exit(0)
def read(name, default=''):
    return (D / name).read_text().strip() if (D / name).exists() else default
if tool == 'adb':
    if args == ['devices']:
        print('List of devices attached')
        if mode in ('android', 'unauthorized'):
            print('PHONE1\t' + ('device' if mode == 'android' else 'unauthorized'))
    elif args == ['reboot', 'bootloader'] and mode == 'android':
        to('fastboot')
    sys.exit(0)
if args == ['devices']:
    if mode in ('fastboot', 'loader'):
        print('PHONE1\tfastboot')
    sys.exit(0)
if mode not in ('fastboot', 'loader'):
    sys.exit(1)
if args[0] == 'getvar':
    if mode == 'loader' and args[1] == 'result':
        polls = int(read('polls', '-1'))
        if polls < 0:
            said('result: idle')
        polls += 1
        (D / 'polls').write_text(str(polls))
        if polls < 3:
            said('result: running')
        shutil.copy(D / 'kernel.log', D / 'prepared')
        (D / 'polls').write_text('-1')
        said('result: ' + read('result', 'returned (code 0)'))
    values = {'product': 'xnuloader' if mode == 'loader' else read('product'), 'is-userspace': 'no',
              'board': read('board') if mode == 'loader' else ''}
    said(f'{args[1]}: {values.get(args[1], "")}', 'Finished. Total time: 0.001s')
if args[0] == 'boot' and mode == 'fastboot':
    shutil.copy(args[1], D / 'booted')
    to('loader')
    said("Sending 'boot.img' (4 KB)  OKAY [  0.001s]", 'Booting  OKAY [  0.001s]', 'Finished. Total time: 0.002s')
if args == ['reboot']:
    to('android')
    said('Rebooting  OKAY [  0.001s]')
if mode != 'loader':
    said('FAILED (remote: unknown command)')
if args[0] == 'stage':
    shutil.copy(args[1], D / 'staged')
    said(f"Sending '{args[1]}' (1 KB)  OKAY [  0.001s]")
if args[:2] == ['oem', 'run'] or args == ['oem', 'selftest']:
    (D / 'polls').write_text('0')
    said('OKAY [  0.001s]')
if args == ['oem', 'log']:
    shutil.copy(D / 'kernel.log', D / 'prepared')
    said('OKAY [  0.001s]')
if args[0] == 'get_staged':
    shutil.copy(D / 'prepared', args[1])
    said('OKAY [  0.001s]')
said('OKAY [  0.001s]')
'''


def payload_entries(data):
    """The entries of an XNUP payload (arch/arm64/boot/mkpayload.py): {name: bytes}"""
    assert data[:4] == b'XNUP', data[:4]
    at, found = 8, {}
    while at + 24 <= len(data):
        name, size, _ = struct.unpack_from('<16sII', data, at)
        name = name.split(b'\0')[0].decode()
        if not name:
            break
        found[name] = data[at + 24:at + 24 + size]
        at += 24 + size + (-size % 16)
    return found


class Bench(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)
        self.top = self.dir / 'checkout'
        for path in ('bench', 'kernel/env.sh', 'kernel/UPSTREAM', 'build.config.arm64', 'build.config.common',
                     'build.config.bench', 'arch/arm64/boot/mkpayload.py', 'arch/arm64/boot/mkboot.py',
                     'arch/arm64/boot/mkidentity.py', 'drivers/board/board.h'):
            (self.top / path).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / path, self.top / path)
        for directory in ('tools', 'arch/arm64/configs', 'arch/arm64/boot/dts/qemu-virt'):
            shutil.copytree(ROOT / directory, self.top / directory, ignore=shutil.ignore_patterns('__pycache__'))
        (self.top / 'arch/arm64/boot/dts/zeta').mkdir()
        (self.top / 'arch/arm64/boot/dts/zeta/board.dts').write_text(BOARD)
        (self.top / 'arch/arm64/boot/dts/zeta/soc.dtsi').write_text(SOC)
        uefi = self.top / 'firmware/zeta/uefi'
        uefi.mkdir(parents=True)
        (uefi / 'SILICIUM_UEFI.fd').write_bytes(bytes(0x1000))
        (uefi / 'BootShim.bin').write_bytes(SHIM)
        (uefi / 'device-tree.dtb').write_bytes(b'\xd0\x0d\xfe\xed' + bytes(12))
        (self.top / 'arch/arm64/boot/loader.efi').write_bytes(LOADER)
        self.kernel = self.dir / 'kernel'
        self.kernel.write_bytes(KERNEL)
        self.phone = self.dir / 'phone'
        self.phone.mkdir()
        self.bin = self.dir / 'bin'
        self.bin.mkdir()
        for tool in ('fastboot', 'adb'):
            (self.bin / tool).write_text(PHONE)
            (self.bin / tool).chmod(0o755)
        (self.bin / 'sleep').write_text('#!/bin/sh\nexit 0\n')     # (the stand-in phone does everything at once)
        (self.bin / 'sleep').chmod(0o755)
        self.runtime = self.dir / 'run'
        self.runtime.mkdir()
        self.at('android', product='zeta', board='zeta')
        (self.phone / 'kernel.log').write_text(KERNEL_LOG)

    def at(self, mode, **files):
        (self.phone / 'mode').write_text(mode)
        for name, text in files.items():
            (self.phone / name).write_text(text)

    def bench(self, *arguments, **env):
        environment = {'PATH': f'{self.bin}:/usr/bin:/bin', 'HOME': str(self.dir), 'FAKE_PHONE': str(self.phone),
                       'XDG_RUNTIME_DIR': str(self.runtime), 'BENCH_KERNEL': str(self.kernel), 'BENCH_RAMDISK': 'none',
                       'BENCH_FOLLOW': '', 'XNU_SRC': str(self.dir / 'xnu')}
        environment.update(env)
        return subprocess.run([str(self.top / 'bench'), *arguments], capture_output=True, text=True, env=environment,
                              timeout=120)

    def calls(self):
        path = self.phone / 'calls'
        return path.read_text().split('\n') if path.exists() else []

    def test_where_the_phone_is(self):
        for mode, said in (('android', 'android: the phone is in Android: bench enter takes it to its bootloader'),
                           ('absent', 'absent: the phone is not on USB'),
                           ('unauthorized', 'unauthorized: the phone is in Android, but this computer is not allowed'),
                           ('fastboot', 'fastboot: the phone is in its own bootloader (fastboot)'),
                           ('loader', 'loader: the phone is in the resident loader (zeta), ready for a run')):
            self.at(mode)
            done = self.bench('state')
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertIn(said, done.stdout)

    def test_a_run_from_android_to_its_log(self):
        done = self.bench('run', BENCH_DEFCONFIG='phone_first')
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        calls = self.calls()
        self.assertIn('adb reboot bootloader', calls)
        self.assertIn('fastboot getvar product', calls)
        self.assertTrue((self.phone / 'booted').read_bytes().startswith(b'ANDROID!'), 'a boot image for the resident loader')
        self.assertLess(calls.index('adb reboot bootloader'), next(i for i, c in enumerate(calls) if c.startswith('fastboot boot ')))
        entries = payload_entries((self.phone / 'staged').read_bytes())
        self.assertEqual(entries['kernel'], KERNEL)
        cmdline = entries['cmdline'].rstrip(b'\0').decode()
        self.assertIn('loader_return_after=180', cmdline)
        self.assertIn('wifi=0', cmdline, 'the board\'s')
        self.assertIn(' cpus=1 ', cmdline, 'the defconfig\'s in place of the board\'s cpus=8')
        self.assertIn(' backlight=0 ', cmdline, '... and of its backlight=1')
        self.assertNotIn('cpus=8', cmdline)
        self.assertNotIn('ramdisk', entries, 'BENCH_RAMDISK=none')
        self.assertIn('fastboot oem run', calls)
        self.assertIn('result: returned (code 0)', done.stdout)
        logs = self.top / 'logs'
        self.assertEqual((logs / 'last.log').read_text(), KERNEL_LOG)
        self.assertRegex(done.stdout, r'Flashlight +ok', 'what each part said, looked at')
        self.assertNotIn('does not know', done.stdout + done.stderr)

    def test_another_phones_bootloader_is_not_started_from(self):
        self.at('fastboot', product='omega')
        done = self.bench('run')
        self.assertEqual(done.returncode, 1)
        self.assertIn('the phone\'s bootloader says it is "omega", the board zeta is "zeta": nothing started', done.stderr)
        self.assertFalse((self.phone / 'booted').exists())

    def test_a_loader_that_took_the_phone_for_another(self):
        self.at('loader', board='qemu-virt')
        done = self.bench('run')
        self.assertEqual(done.returncode, 1)
        self.assertIn('the resident loader took the phone for "qemu-virt", not zeta: bench reboot, then again', done.stderr)
        self.assertFalse((self.phone / 'staged').exists())

    def test_a_kernel_that_does_not_know_the_phone(self):
        self.kernel.write_bytes(KERNEL.replace(b'\0zeta\0', b'\0qemu-virt\0'))
        self.at('loader')
        done = self.bench('run')
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn('does not know zeta: it was built before the package was here', done.stderr)

    def test_a_second_run_in_one_session(self):
        self.at('loader')
        self.assertEqual(self.bench('run').returncode, 0)
        done = self.bench('run')
        self.assertIn('warning: a kernel has already run in this session of the loader', done.stdout)

    def test_a_fresh_loader_before_the_next_run(self):
        # a kernel has run in this session: the phone restarted, the loader started anew, the resident image sent again --
        # one too big for the restart's own resending (over 64 MB) as well
        self.at('loader')
        image = self.dir / 'system image.apfs'
        with open(image, 'wb') as f:
            f.truncate(65 << 20)
        self.assertEqual(self.bench('ramdisk', str(image)).returncode, 0)
        self.assertEqual(self.bench('run').returncode, 0)
        (self.phone / 'calls').write_text('')
        done = self.bench('fresh')
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        calls = self.calls()
        self.assertIn('fastboot reboot', calls)
        self.assertLess(calls.index('fastboot reboot'), calls.index('adb reboot bootloader'))
        self.assertIn(f'fastboot stage {image}', calls, 'the resident image sent again')
        self.assertIn('no kernel run in it yet', done.stdout)
        done = self.bench('run')
        self.assertNotIn('warning: a kernel has already run', done.stdout)

    def test_a_fresh_loader_is_left_as_it_is(self):
        self.at('loader')
        done = self.bench('fresh')
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertNotIn('fastboot reboot', self.calls(), 'no kernel has run in it: nothing restarted')

    def test_a_small_image_whose_path_has_a_space(self):
        self.at('loader')
        image = self.dir / 'my images' / 'root.apfs'
        image.parent.mkdir()
        image.write_bytes(b'APFS' * 64)
        done = self.bench('run', BENCH_RAMDISK=str(image))
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertEqual(payload_entries((self.phone / 'staged').read_bytes())['ramdisk'], b'APFS' * 64)

    def test_the_identity_goes_with_the_run(self):
        self.at('loader')
        blob = b'IDNT' + bytes(60)
        (self.top / 'firmware/zeta/identity.bin').write_bytes(blob)
        done = self.bench('run')
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn('identity: firmware/zeta/identity.bin', done.stdout)
        self.assertEqual(payload_entries((self.phone / 'staged').read_bytes())['identity'], blob, 'the phone\'s own, as it is')
        done = self.bench('run', BENCH_IDENTITY=str(self.dir / 'gone.bin'))
        self.assertEqual(done.returncode, 1)
        self.assertIn('no identity', done.stderr)

    def test_no_kernel(self):
        self.at('loader')
        done = self.bench('run', BENCH_KERNEL=str(self.dir / 'none'))
        self.assertEqual(done.returncode, 1)
        self.assertIn('no kernel', done.stderr)
        self.assertFalse((self.phone / 'staged').exists())

    def test_one_command_at_a_time_while_a_run_holds_the_phone(self):
        self.at('loader')
        with open(self.runtime / 'hakios-phone.run', 'w') as held:
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            done = self.bench('ramdisk', 'none')
            self.assertEqual(done.returncode, 1)
            self.assertIn('a run is in progress: only state, screenshot, log, stop and reboot now', done.stderr)
            done = self.bench('state')
            self.assertEqual(done.returncode, 0, 'state is taken')


if __name__ == '__main__':
    unittest.main()
