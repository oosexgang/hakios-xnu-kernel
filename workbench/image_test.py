#!/usr/bin/env python3
"""tools/image.py: one image for a phone -- its payload as the loader reads payloads, its boot image as mkboot.py makes
them, what is published into a phone's repository and what is refused there, an image given the identity of an IPSW,
and what an image holds. Run on a phone of its own, with a kernel and a loader that are only their first bytes."""
import contextlib
import importlib.util
import io
import json
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
os.environ.pop('HAKIOS_DEVICES', None)  # (the phones of the machine it runs on are not the test's)
spec = importlib.util.spec_from_file_location('image', ROOT / 'tools/image.py')
image = importlib.util.module_from_spec(spec)
spec.loader.exec_module(image)
boards = image.boards

SOC = '/ {\n\tname = "sm0001";\n\tdescription = "A chip";\n};\n'
BOARD = ('/ {\n\tname = "zeta";\n\tdescription = "Phone zeta";\n\tsoc = "sm0001";\n\tfeatures = <>;\n'
         '\tbacklight {\n\t\ttype = "soft";\n\t};\n'
         '\tuefi {\n\t\tfd-base = <0x9FD00000>;\n\t\tfd-size = <0x1000>;\n\t};\n'
         '\tboot {\n\t\tfastboot-product = "zeta";\n\t\tcpus = <8>;\n\t\tbootargs = "backlight=1", "wifi=0";\n\t};\n};\n')
KERNEL = b'\xcf\xfa\xed\xfe' + bytes(range(256)) * 8 + b'\0zeta\0'
LOADER = b'MZ' + b'\0' * 62 + b'hakios-placeholder\0zeta\0' + b'\x90' * 200
APPLE_LOADER = b'MZ' + b'\0' * 62 + b'iPhone12,1\0zeta\0' + b'\x90' * 200
SHIM = struct.pack('<IIQQQQQQ4sI', 0x14000010, 0, 0x9FD00000, 0x1000, 0, 0, 0, 0, b'ARMd', 0) + b'code'
UEFI_IMAGE = bytes(range(256)) * 16
DTB = b'\xd0\x0d\xfe\xed' + b'tree' * 8


def device_tree(model=b'iPhone99,9\0', subtype=1792):
    """An Apple-format device tree of the test's own: the nodes mkidentity.py reads, with nothing of a real iPhone's."""
    def node(props, children=()):
        out = struct.pack('<II', len(props), len(children))
        for name, value in props:
            out += name.encode().ljust(32, b'\0') + struct.pack('<I', len(value)) + value + b'\0' * (-len(value) % 4)
        return out + b''.join(children)
    return node([('name', b'device-tree\0'), ('model', model)], [
        node([('name', b'arm-io\0'), ('compatible', b'arm-io,t8030\0arm-io\0')]),
        node([('name', b'product\0'), ('artwork-device-subtype', struct.pack('<I', subtype)),
              ('artwork-scale-factor', struct.pack('<I', 2))], [
            node([('name', b'haptics\0'), ('closed-loop', struct.pack('<I', 1))]),
            node([('name', b'camera\0'), ('flash', struct.pack('<I', 1))])])])


class Sandbox(unittest.TestCase):
    """A checkout with one phone (zeta, with a uefi { } and a boot { } node), a kernel, a loader, a defconfig."""

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.saved = (boards.ROOT, boards.DEVICES)
        boards.ROOT, boards.DEVICES = self.dir, self.dir / 'arch/arm64/boot/dts'
        (boards.DEVICES / 'zeta').mkdir(parents=True)
        (boards.DEVICES / 'zeta/board.dts').write_text(BOARD)
        (boards.DEVICES / 'zeta/soc.dtsi').write_text(SOC)
        (self.dir / 'kernel').write_bytes(KERNEL)
        (self.dir / 'loader.efi').write_bytes(LOADER)
        (self.dir / 'test_defconfig').write_text('# a run of the test\nwifi=1   # replaces the board\'s\ncam=0\n')
        self.out = self.dir / 'out'

    def tearDown(self):
        boards.ROOT, boards.DEVICES = self.saved
        shutil.rmtree(self.dir)

    def tool(self, *arguments):
        said, complained = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(said), contextlib.redirect_stderr(complained):
            try:
                code = image.main(list(arguments))
            except SystemExit as stop:
                code = stop.code
        return code, said.getvalue()

    def build(self, *more):
        code, said = self.tool('build', '--board', 'zeta', '--defconfig', str(self.dir / 'test_defconfig'), '--kernel',
                               str(self.dir / 'kernel'), '--loader', str(self.dir / 'loader.efi'), '--out', str(self.out),
                               '--uefi', str(self.dir / 'uefi'), *more)
        self.assertEqual(code, 0, said)
        return said

    def uefi(self):
        (self.dir / 'uefi').mkdir(exist_ok=True)
        for name, data in (('SILICIUM_UEFI.fd', UEFI_IMAGE), ('BootShim.bin', SHIM), ('device-tree.dtb', DTB)):
            (self.dir / 'uefi' / name).write_bytes(data)


class Payload(unittest.TestCase):
    def test_the_payload_is_the_one_mkpayload_writes(self):
        with tempfile.TemporaryDirectory() as work:
            Path(work, 'k').write_bytes(KERNEL)
            Path(work, 'l').write_bytes(LOADER)
            subprocess.run(['python3', str(ROOT / 'arch/arm64/boot/mkpayload.py'), '-o', 'p.bin', 'loader.efi=l', 'kernel=k',
                            '--cmdline', 'a=1 b'], cwd=work, check=True, capture_output=True)
            made = Path(work, 'p.bin').read_bytes()
        mine = image.payload_pack([('loader.efi', LOADER), ('kernel', KERNEL), ('cmdline', b'a=1 b\0')])
        self.assertEqual(mine, made)
        self.assertEqual(image.payload_entries(made), [('loader.efi', LOADER), ('kernel', KERNEL), ('cmdline', b'a=1 b\0')])

    def test_what_is_not_a_payload_is_said_to_be(self):
        with self.assertRaisesRegex(SystemExit, 'not a payload'):
            image.payload_entries(b'ELF!' + bytes(20))
        cut = image.payload_pack([('kernel', KERNEL)])
        with self.assertRaisesRegex(SystemExit, 'not a payload'):
            image.payload_entries(cut[:100])
        with self.assertRaisesRegex(SystemExit, 'past the payload'):
            image.payload_entries(cut[:24] + struct.pack('<I', 1 << 20) + cut[28:])      # (the kernel's size)


class Build(Sandbox):
    def test_one_image_of_the_loader_the_kernel_and_the_boot_arguments(self):
        said = self.build()
        payload = (self.out / 'zeta.payload').read_bytes()
        entries = image.payload_entries(payload)
        self.assertEqual([name for name, _ in entries], ['loader.efi', 'kernel', 'cmdline'])
        self.assertEqual(dict(entries)['kernel'], KERNEL)
        # the board's arguments and cpus=, then the defconfig's, each replacing one of the same name, as ./bench
        self.assertEqual(dict(entries)['cmdline'], b'backlight=1 wifi=1 cpus=8 cam=0\0')
        self.assertFalse((self.out / 'boot.img').exists(), 'no UEFI build: no boot image')
        self.assertIn('no boot.img:', said, '... and that is said')
        self.assertIn('BootShim.bin', said)
        manifest = json.loads((self.out / 'manifest.json').read_text())
        self.assertEqual((manifest['board'], manifest['loader']['identity'], manifest['identity_in_payload']), ('zeta', 'placeholder', False))
        self.assertEqual(manifest['files']['zeta.payload']['sha256'], image.sha256(payload))
        self.assertEqual(manifest['kernel']['sha256'], image.sha256(KERNEL))
        self.assertNotIn(str(self.dir), (self.out / 'manifest.json').read_text() + (self.out / 'README.md').read_text(),
                         'nothing of the machine it was built on')
        readme = (self.out / 'README.md').read_text()
        self.assertIn('backlight=1 wifi=1 cpus=8 cam=0', readme)
        self.assertIn('tools/image.py personalize zeta.payload --identity', readme)
        self.assertIn('zeta.payload', said)

    def test_a_phone_that_has_no_uefi_node_yet_is_named_not_asked_for_again(self):
        # a package just made and brought in (make phone FROM=...) has no uefi { } section to choose it by: it is here,
        # and "tools/device.py add NAME" is not what is missing
        path = boards.DEVICES / 'zeta/board.dts'
        path.write_text(path.read_text().replace('\tuefi {\n\t\tfd-base = <0x9FD00000>;\n\t\tfd-size = <0x1000>;\n\t};\n', ''))
        with self.assertRaises(SystemExit) as stop:
            image.board_of(None)
        self.assertIn('which phone? --board NAME', str(stop.exception.code))
        self.assertIn('no package with a uefi { } node to choose by; without one: zeta', str(stop.exception.code))
        self.assertNotIn('tools/device.py add', str(stop.exception.code))
        self.assertEqual(image.board_of('zeta')[0], 'zeta')            # (named, it is built for)

    def test_with_the_phone_s_uefi_a_boot_image_that_carries_the_payload(self):
        self.uefi()
        self.build()
        boot = (self.out / 'boot.img').read_bytes()
        front, payload, dtb = image.boot_image_payload(boot)
        self.assertEqual(payload, (self.out / 'zeta.payload').read_bytes())
        self.assertEqual((front, dtb), (SHIM + UEFI_IMAGE, DTB))
        self.assertIn('fastboot boot boot.img', (self.out / 'README.md').read_text())

    def test_one_image_for_each_run_of_a_phone(self):
        code, said = self.tool('build', '--board', 'zeta', '--kernel', str(self.dir / 'kernel'), '--loader',
                               str(self.dir / 'loader.efi'), '--out', str(self.out), '--uefi', str(self.dir / 'uefi'), '--every-run')
        self.assertEqual(code, 0, said)
        runs = sorted(p.name.removesuffix('_defconfig') for p in image.defconfig.CONFIGS.glob('phone_*_defconfig'))
        self.assertGreater(len(runs), 3)
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), runs)
        for run in runs:
            manifest = json.loads((self.out / run / 'manifest.json').read_text())
            self.assertEqual(manifest['defconfig'], run)
            cmdline = dict(image.payload_entries((self.out / run / 'zeta.payload').read_bytes()))['cmdline'].rstrip(b'\0').decode()
            self.assertEqual(cmdline, image.boot_arguments(boards.load()[1]['zeta'], run))
        self.assertIn(f'{len(runs)} images, one a run', said)
        self.assertIn('not with --defconfig', str(self.tool('build', '--board', 'zeta', '--every-run', '--defconfig', 'phone_bt')[0]))

    def test_what_is_not_a_kernel_or_a_loader_is_not_taken(self):
        (self.dir / 'kernel').write_bytes(b'ELF!')
        code, _ = self.tool('build', '--board', 'zeta', '--kernel', str(self.dir / 'kernel'), '--loader', str(self.dir / 'loader.efi'),
                            '--out', str(self.out))
        self.assertIn('not a 64-bit Mach-O kernel', str(code))
        code, _ = self.tool('build', '--board', 'zeta', '--kernel', str(self.dir / 'nothing'), '--out', str(self.out))
        self.assertIn('kernel/build.sh', str(code))
        code, _ = self.tool('build', '--board', 'nobody', '--out', str(self.out))
        self.assertIn('no board "nobody"', str(code))

    def test_a_loader_that_does_not_know_the_phone_is_not_taken(self):
        (self.dir / 'loader.efi').write_bytes(LOADER.replace(b'\0zeta\0', b'\0emulator\0'))
        code, _ = self.tool('build', '--board', 'zeta', '--kernel', str(self.dir / 'kernel'), '--loader', str(self.dir / 'loader.efi'),
                            '--out', str(self.out))
        self.assertIn('does not know zeta: it was built before the package was here', str(code))
        (self.dir / 'loader.efi').write_bytes(LOADER.replace(b'\0zeta\0', b'\0zetaplus\0'))
        code, _ = self.tool('build', '--board', 'zeta', '--kernel', str(self.dir / 'kernel'), '--loader', str(self.dir / 'loader.efi'),
                            '--out', str(self.out))
        self.assertIn('does not know zeta', str(code), 'another board whose name begins with it is not it')

    def test_a_kernel_that_does_not_know_the_phone_is_not_taken(self):
        (self.dir / 'kernel').write_bytes(KERNEL.replace(b'\0zeta\0', b'\0emulator\0'))
        code, _ = self.tool('build', '--board', 'zeta', '--kernel', str(self.dir / 'kernel'), '--loader', str(self.dir / 'loader.efi'),
                            '--out', str(self.out))
        self.assertIn('does not know zeta: it was built before the package was here. kernel/build.sh', str(code))
        self.assertFalse(self.out.exists(), 'nothing made')

    def test_a_build_again_leaves_nothing_of_the_last(self):
        self.uefi()
        self.build()
        shutil.rmtree(self.dir / 'uefi')
        self.build()
        self.assertFalse((self.out / 'boot.img').exists(), 'the last boot image, with the last payload, is gone')


class Publish(Sandbox):
    def publish(self, *more):
        repository = self.dir / 'phone-repo'
        repository.mkdir(exist_ok=True)
        return repository, self.tool('publish', str(self.out), str(repository), '--version', 'v1', *more)

    def test_the_image_into_the_phone_s_repository(self):
        self.uefi()
        self.build()
        repository, (code, said) = self.publish()
        self.assertEqual(code, 0, said)
        published = sorted(p.name for p in (repository / 'images/v1').iterdir())
        self.assertEqual(published, ['README.md', 'manifest.json', 'zeta.payload'], 'boot.img only when asked: it carries the vendor\'s UEFI')
        self.assertIn('boot.img left out', said)
        index = (repository / 'images/README.md').read_text()
        self.assertIn('| [v1](v1/README.md) | zeta |', index)
        _, (code, _) = self.publish('--with-uefi')
        self.assertIn('boot.img', [p.name for p in (repository / 'images/v1').iterdir()])

    def test_a_published_image_verifies(self):
        # (publish must leave an image that the phone's repository's own `verify` accepts, whether or not boot.img went
        # with it: its manifest.json names only the files put beside it, never boot.img that a default publish omits)
        self.uefi()
        self.build()
        repository, (code, said) = self.publish()
        self.assertEqual(code, 0, said)
        manifest = json.loads((repository / 'images/v1/manifest.json').read_text())
        self.assertNotIn('boot.img', manifest['files'], 'the published manifest names no file that is not here')
        code, said = self.tool('verify', str(repository / 'images'))
        self.assertEqual(code, 0, f'a default-published image is rejected by verify: {said}')
        repository, (code, said) = self.publish('--with-uefi')
        self.assertEqual(code, 0, said)
        manifest = json.loads((repository / 'images/v1/manifest.json').read_text())
        self.assertIn('boot.img', manifest['files'], 'with --with-uefi the manifest names boot.img, which is here')
        code, said = self.tool('verify', str(repository / 'images'))
        self.assertEqual(code, 0, f'--with-uefi published image is rejected by verify: {said}')

    def test_apple_s_data_is_never_published(self):
        (self.dir / 'loader.efi').write_bytes(APPLE_LOADER)
        self.build()
        _, (code, _) = self.publish()
        self.assertIn('identity from an IPSW', str(code))
        (self.dir / 'loader.efi').write_bytes(LOADER)
        (self.dir / 'blob').write_bytes(image.identity_blob(self.tree()))
        self.build('--identity', str(self.dir / 'blob'))
        _, (code, _) = self.publish()
        self.assertIn('carries an identity', str(code))

    def rehash(self, *files):
        """manifest.json made to say of FILES what they now are, as anybody may edit it"""
        manifest = json.loads((self.out / 'manifest.json').read_text())
        for file in files:
            manifest['files'][file] = {'sha256': image.sha256((self.out / file).read_bytes())}
        (self.out / 'manifest.json').write_text(json.dumps(manifest))
        return manifest

    def test_the_bytes_are_judged_not_the_manifest(self):
        (self.dir / 'loader.efi').write_bytes(APPLE_LOADER)
        self.build()
        manifest = json.loads((self.out / 'manifest.json').read_text())
        manifest['loader']['identity'] = 'placeholder'          # (edited: it says what is not so)
        (self.out / 'manifest.json').write_text(json.dumps(manifest))
        _, (code, _) = self.publish()
        self.assertIn('identity from an IPSW', str(code))

    def test_apple_s_data_under_any_name_or_after_the_entries(self):
        blob = image.identity_blob(self.tree())
        payload = self.out / 'zeta.payload'
        for hidden in ('Identity', 'identity.bin', 'cmdline-after', 'trailing', 'device-tree', 'im4p'):
            self.build()
            entries = image.payload_entries(payload.read_bytes())
            if hidden == 'cmdline-after':
                entries = [(n, d + blob if n == 'cmdline' else d) for n, d in entries]
            elif hidden in ('Identity', 'identity.bin'):
                entries.append((hidden, blob))
            elif hidden == 'device-tree':
                entries.append(('board-notes', b'AAPL,phandle\0' * 20))
            elif hidden == 'im4p':
                entries.append(('notes', b'0\x82\x01\x00\x16\x04IM4P\x16\x04dtre'))
            data = image.payload_pack(entries) + (blob if hidden == 'trailing' else b'')
            payload.write_bytes(data)
            self.rehash('zeta.payload')
            _, (code, said) = self.publish()
            self.assertNotEqual(code, 0, f'{hidden}: published: {said}')
            self.assertIn('Apple', str(code), hidden)

    def test_a_boot_image_is_looked_inside(self):
        self.uefi()
        self.build()
        blob = image.identity_blob(self.tree())
        code, _ = self.tool('personalize', str(self.out / 'boot.img'), '--identity', str(self.dir / 'DeviceTree.bin'))
        self.assertEqual(code, 0)
        (self.out / 'boot-mine.img').replace(self.out / 'boot.img')
        self.rehash('boot.img')
        _, (code, said) = self.publish('--with-uefi')
        self.assertNotEqual(code, 0, said)
        self.assertIn('boot.img', str(code))
        self.assertTrue(blob)

    def test_a_boot_image_the_manifest_does_not_list_is_not_taken(self):
        self.build()
        self.uefi()
        (self.out / 'boot.img').write_bytes(b'ANDROID!' + bytes(100))     # (not the image's own)
        _, (code, _) = self.publish('--with-uefi')
        self.assertIn('boot.img is not the image\'s own', str(code))

    def test_a_version_is_a_name_and_no_more(self):
        self.build()
        repository = self.dir / 'phone-repo'
        (repository / 'images').mkdir(parents=True)
        (repository / 'keep').write_text('the phone\'s own\n')
        for version in ('..', '../x', '/tmp/x', '.hidden', 'a/b'):
            code, _ = self.tool('publish', str(self.out), str(repository), '--version', version)
            self.assertIn('--version', str(code), version)
        self.assertEqual((repository / 'keep').read_text(), 'the phone\'s own\n', 'nothing of the repository touched')
        (repository / 'images/notes').mkdir()
        (repository / 'images/notes/a.md').write_text('x\n')
        code, _ = self.tool('publish', str(self.out), str(repository), '--version', 'notes')
        self.assertIn('is there and is no image', str(code))
        self.assertTrue((repository / 'images/notes/a.md').is_file())

    def test_an_image_that_is_not_what_its_manifest_says_is_not_published(self):
        self.build()
        with open(self.out / 'zeta.payload', 'ab') as payload:
            payload.write(b'more')
        _, (code, _) = self.publish()
        self.assertIn('not what manifest.json says', str(code))

    def test_every_image_of_a_repository_verified(self):
        self.build()
        repository, (code, said) = self.publish()
        self.assertEqual(code, 0, said)
        code, said = self.tool('verify', str(repository / 'images'))
        self.assertEqual(code, 0, said)
        self.assertIn('zeta, ', said)
        self.assertIn('as its manifest says', said)
        with open(repository / 'images/v1/zeta.payload', 'ab') as payload:
            payload.write(b'!')
        code, said = self.tool('verify', str(repository / 'images'))
        self.assertEqual(code, 1)
        self.assertIn('NOT AS PUBLISHED: ', said)
        self.assertIn('not what manifest.json says', said)
        self.assertIn('no image', str(self.tool('verify', str(self.dir / 'uefi-nothing'))[0]))

    def test_verify_sees_what_no_manifest_lists(self):
        self.build()
        repository, (code, said) = self.publish()
        self.assertEqual(code, 0, said)
        images = repository / 'images'
        for stray in ('v1/zeta-mine.payload', 'v1/DeviceTree.bin', 'old/n104ap.ipsw'):
            (images / stray).parent.mkdir(parents=True, exist_ok=True)
            (images / stray).write_bytes(b'something')
            code, said = self.tool('verify', str(images))
            self.assertEqual(code, 1, f'{stray}: {said}')
            self.assertIn('NOT AS PUBLISHED', said)
            self.assertIn(Path(stray).name, said)
            (images / stray).unlink()
        (images / 'v1/README.md').unlink()
        (images / 'v1/README.md').symlink_to(self.tree())
        code, said = self.tool('verify', str(images))
        self.assertEqual(code, 1, said)
        self.assertIn('links, not files of its own', said)

    def test_verify_does_not_crash_on_a_malformed_or_hostile_manifest(self):
        # verify runs against repositories other people push to: a broken or crafted manifest.json must come back as
        # "NOT AS PUBLISHED", never a traceback, and must not read a file outside the image
        self.build()
        repository, (code, said) = self.publish()
        self.assertEqual(code, 0, said)
        m = repository / 'images/v1/manifest.json'
        import json as _json
        good = _json.loads(m.read_text())
        for label, text in [
            ('invalid json', '{not json'),
            ('not an object', '[]'),
            ('files not an object', _json.dumps(dict(good, files=[]))),
            ('a path out of the image', _json.dumps(dict(good, files={'../../etc/passwd': {'sha256': 'x'}}))),
            ('a member with no sha256', _json.dumps(dict(good, files={'zeta.payload': {}}))),
        ]:
            m.write_text(text)
            code, said = self.tool('verify', str(repository / 'images'))
            self.assertEqual(code, 1, f'{label}: {said}')
            self.assertIn('NOT AS PUBLISHED', said, label)
            self.assertNotIn('Traceback', said, label)

    def tree(self):
        path = self.dir / 'DeviceTree.bin'
        path.write_bytes(device_tree())
        return path


class Diff(Sandbox):
    def test_what_differs_between_two_images(self):
        self.build()
        first = self.dir / 'first'
        self.out.rename(first)
        (self.dir / 'test_defconfig').write_text('wifi=1\ncam=1\nbt=2\n')
        self.build()
        code, said = self.tool('diff', str(first), str(self.out))
        self.assertEqual(code, 1, said)
        self.assertIn('boot argument cam: cam=0  ->  cam=1', said)
        self.assertIn('boot argument bt: (none)  ->  bt=2', said)
        self.assertNotIn('boot argument wifi', said, 'the same on both: not said')
        self.assertNotIn('kernel:', said)
        (self.dir / 'kernel').write_bytes(KERNEL + b'more')
        self.build()
        self.assertRegex(self.tool('diff', str(first), str(self.out / 'manifest.json'))[1], r'kernel: [0-9a-f]{16}  ->  [0-9a-f]{16}')
        code, said = self.tool('diff', str(self.out), str(self.out))
        self.assertEqual(code, 0)
        self.assertIn('the same image', said)
        self.assertIn('no manifest.json', str(self.tool('diff', str(self.dir), str(self.out))[0]))


class Personalize(Sandbox):
    def test_a_payload_given_the_identity_of_an_ipsw(self):
        self.build()
        tree = self.dir / 'DeviceTree.bin'
        tree.write_bytes(device_tree())
        code, said = self.tool('personalize', str(self.out / 'zeta.payload'), '--identity', str(tree))
        self.assertEqual(code, 0, said)
        mine = image.payload_entries((self.out / 'zeta-mine.payload').read_bytes())
        self.assertEqual([name for name, _ in mine], ['loader.efi', 'kernel', 'cmdline', 'identity'])
        blob = dict(mine)['identity']
        self.assertEqual((blob[:4], struct.unpack_from('<II', blob, 4)[1], blob[12:17]), (b'IDNT', len(blob), b't8030'))
        # once more: one identity, the newer
        tree.write_bytes(device_tree(model=b'iPhone98,1\0'))
        self.tool('personalize', str(self.out / 'zeta-mine.payload'), '--identity', str(tree), '-o', str(self.out / 'again.payload'))
        again = image.payload_entries((self.out / 'again.payload').read_bytes())
        self.assertEqual([name for name, _ in again].count('identity'), 1)
        self.assertIn(b'iPhone98,1', dict(again)['identity'])

    def test_a_boot_image_given_it_keeps_its_uefi(self):
        self.uefi()
        self.build()
        tree = self.dir / 'DeviceTree.bin'
        tree.write_bytes(device_tree())
        code, said = self.tool('personalize', str(self.out / 'boot.img'), '--identity', str(tree))
        self.assertEqual(code, 0, said)
        front, payload, dtb = image.boot_image_payload((self.out / 'boot-mine.img').read_bytes())
        self.assertEqual((front, dtb), (SHIM + UEFI_IMAGE, DTB))
        self.assertIn('identity', dict(image.payload_entries(payload)))

    def test_the_identity_of_an_ipsw_itself(self):
        import zipfile
        self.build()

        def der(tag, body):
            return bytes([tag, 0x82]) + len(body).to_bytes(2, 'big') + body
        tree = device_tree()
        im4p = der(0x30, der(0x16, b'IM4P') + der(0x16, b'dtre') + der(0x16, b'test') + der(0x04, tree))
        ipsw = self.dir / 'phone.ipsw'
        with zipfile.ZipFile(ipsw, 'w') as z:
            z.writestr('Firmware/all_flash/DeviceTree.n104ap.im4p', im4p)
            z.writestr('Firmware/all_flash/DeviceTree.d421ap.im4p', im4p.replace(b'iPhone99,9', b'iPhone77,7'))
            z.writestr('BuildManifest.plist', b'<plist/>')
        self.assertIn('which iPhone', str(self.tool('personalize', str(self.out / 'zeta.payload'), '--identity', str(ipsw))[0]))
        self.assertIn('no device tree for n999ap (it has: d421ap, n104ap)',
                      str(self.tool('personalize', str(self.out / 'zeta.payload'), '--identity', str(ipsw), '--model', 'n999ap')[0]))
        code, said = self.tool('personalize', str(self.out / 'zeta.payload'), '--identity', str(ipsw), '--model', 'N104AP')
        self.assertEqual(code, 0, said)
        blob = dict(image.payload_entries((self.out / 'zeta-mine.payload').read_bytes()))['identity']
        self.assertIn(b'iPhone99,9', blob)
        self.assertNotIn(b'iPhone77,7', blob)

    def test_what_is_no_device_tree_gives_no_identity(self):
        self.build()
        (self.dir / 'junk').write_bytes(b'\0' * 64)
        code, _ = self.tool('personalize', str(self.out / 'zeta.payload'), '--identity', str(self.dir / 'junk'))
        self.assertIn('no identity made of it', str(code))


class Show(Sandbox):
    def test_what_an_image_holds_and_whether_it_is_its_manifest_s(self):
        self.uefi()
        self.build()
        code, said = self.tool('show', str(self.out / 'boot.img'))
        self.assertEqual(code, 0)
        self.assertRegex(said, r'a boot image with a payload')
        self.assertRegex(said, r'loader.efi +\d+ bytes .*placeholder identity built in')
        self.assertIn('backlight=1 wifi=1 cpus=8 cam=0', said)
        self.assertIn('manifest.json: the same', said)
        with open(self.out / 'boot.img', 'ab') as boot:
            boot.write(b'\0')
        self.assertIn('NOT THE SAME FILE', self.tool('show', str(self.out / 'boot.img'))[1])

    def test_its_payload_and_its_loader_as_files(self):
        self.uefi()
        self.build()
        code, _ = self.tool('extract', str(self.out / 'boot.img'), '--payload', str(self.dir / 'p'), '--loader', str(self.dir / 'l'))
        self.assertEqual(code, 0)
        self.assertEqual((self.dir / 'p').read_bytes(), (self.out / 'zeta.payload').read_bytes())
        self.assertEqual((self.dir / 'l').read_bytes(), LOADER)
        unzip = zlib.decompressobj(31)
        self.assertTrue(unzip.decompress(image.boot_image_parts((self.out / 'boot.img').read_bytes())[1]))


if __name__ == '__main__':
    unittest.main()
