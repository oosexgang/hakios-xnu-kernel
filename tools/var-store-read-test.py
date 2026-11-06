#!/usr/bin/env python3
"""Read synthetic two-slot stores through the CLI, without adb or phone data."""
import binascii
import contextlib
import importlib.util
import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

TOOL = Path(__file__).with_name('var-store-read.py')
BLOCK_BYTES = 4096
SLOT_BYTES = 0x80000000
PREFIX = 'db/cache'


def entry(kind, path, body=b''):
    """Pack the documented entry layout independently of the reader's writers."""
    encoded = path.encode('utf-8')
    raw = struct.pack('<8IqQ', kind, 0o755 if kind == 1 else 0o644,
                      0, 0, 0, len(encoded), 0, 0, 1700000000, len(body)) + encoded + body
    return raw + b'\0' * (-len(raw) % 8)


def archive(slot):
    data = (entry(1, PREFIX) + entry(2, PREFIX + '/shared.bin', b'shared from slot ' + bytes([48 + slot])) +
            entry(2, PREFIX + f'/slot-{slot}.bin', b'only slot ' + bytes([48 + slot])) +
            entry(2, 'db/other', b'outside the requested prefix'))
    return data, 4


def header(slot, generation, data, count):
    raw = struct.pack('<8sIIQQQQII', b'RDWVAR\0\0', 1, slot, generation, len(data), count,
                      1700000001, binascii.crc32(data) & 0xffffffff, 0)
    return raw[:-4] + struct.pack('<I', binascii.crc32(raw) & 0xffffffff)


class ValidSlot(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='var-store-read-test-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.serial = 0

    def path(self, name):
        self.serial += 1
        return self.root / f'{self.serial}-{name}'

    def image(self, generations=(10, 20), states=('good', 'good')):
        path = self.path('store.img')
        with path.open('wb') as out:
            for slot, (generation, state) in enumerate(zip(generations, states)):
                data, count = (b'', 0) if state == 'empty' else archive(slot)
                head = header(slot, generation, data, count)
                if state == 'header-bad':
                    head = head[:-1] + bytes([head[-1] ^ 0xff])
                out.seek(slot * SLOT_BYTES)
                out.write(head)
                out.seek(slot * SLOT_BYTES + BLOCK_BYTES)
                if state == 'crc-bad':
                    data = data[:-1] + bytes([data[-1] ^ 0xff])
                elif state == 'truncated':
                    # Use slot 1 for real EOF truncation; a hole before slot 1 reads as zeroes.
                    self.assertEqual(slot, 1)
                    data = data[:-7]
                out.write(data)
        return path

    def cli(self, image, *arguments):
        return subprocess.run([sys.executable, str(TOOL), '--image', str(image), *map(str, arguments)],
                              capture_output=True, text=True)

    def selected(self, image, slot, generation, command):
        if command == 'list':
            result = self.cli(image, 'list', PREFIX)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual([line.split()[-1] for line in result.stdout.splitlines()],
                             [PREFIX, PREFIX + '/shared.bin', PREFIX + f'/slot-{slot}.bin'])
        else:
            dest = self.path('extract')
            result = self.cli(image, 'extract', PREFIX, dest)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(f'generation {generation}:', result.stdout)
            self.assertEqual({p.relative_to(dest).as_posix(): p.read_bytes() for p in dest.rglob('*') if p.is_file()},
                             {'shared.bin': b'shared from slot ' + bytes([48 + slot]),
                              f'slot-{slot}.bin': b'only slot ' + bytes([48 + slot])})

    def test_bad_content_crc_falls_back_in_either_slot(self):
        for newest in (0, 1):
            generations = (20, 10) if newest == 0 else (10, 20)
            states = ('crc-bad', 'good') if newest == 0 else ('good', 'crc-bad')
            image = self.image(generations, states)
            for command in ('list', 'extract'):
                with self.subTest(newest=newest, command=command):
                    self.selected(image, 1 - newest, 10, command)

    def test_truncated_newest_body_falls_back(self):
        image = self.image(states=('good', 'truncated'))
        for command in ('list', 'extract'):
            with self.subTest(command=command):
                self.selected(image, 0, 10, command)

    def test_healthy_newest_wins_in_either_slot(self):
        for newest in (0, 1):
            generations = (20, 10) if newest == 0 else (10, 20)
            image = self.image(generations)
            for command in ('list', 'extract'):
                with self.subTest(newest=newest, command=command):
                    self.selected(image, newest, 20, command)

    def test_header_corrupt_newest_is_skipped(self):
        for newest in (0, 1):
            generations = (20, 10) if newest == 0 else (10, 20)
            states = ('header-bad', 'good') if newest == 0 else ('good', 'header-bad')
            image = self.image(generations, states)
            for command in ('list', 'extract'):
                with self.subTest(newest=newest, command=command):
                    self.selected(image, 1 - newest, 10, command)

    def test_all_bad_or_no_valid_headers_fail_without_extracting(self):
        empty = self.path('empty.img')
        empty.write_bytes(b'')
        images = {'bad-content': self.image(states=('crc-bad', 'truncated')),
                  'bad-headers': self.image(states=('header-bad', 'header-bad')), 'no-headers': empty}
        for label, image in images.items():
            for command in ('list', 'extract'):
                with self.subTest(image=label, command=command):
                    dest = self.path('must-not-exist')
                    arguments = ('extract', PREFIX, dest) if command == 'extract' else ('list', PREFIX)
                    result = self.cli(image, *arguments)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse(dest.exists())
                    self.assertNotIn('Traceback', result.stderr)

    def test_empty_valid_newest_is_not_treated_as_failure(self):
        for newest in (0, 1):
            generations = (20, 10) if newest == 0 else (10, 20)
            states = ('empty', 'good') if newest == 0 else ('good', 'empty')
            image = self.image(generations, states)
            listed = self.cli(image, 'list', PREFIX)
            with self.subTest(newest=newest, command='list'):
                self.assertEqual(listed.returncode, 0, listed.stderr)
                self.assertEqual(listed.stdout, '')
            dest = self.path('empty-extract')
            extracted = self.cli(image, 'extract', PREFIX, dest)
            with self.subTest(newest=newest, command='extract'):
                self.assertEqual(extracted.returncode, 0, extracted.stderr)
                self.assertIn('generation 20: 0 files', extracted.stdout)
                self.assertFalse(any(p.is_file() for p in dest.rglob('*')))

    def test_equal_generations_keep_slot_zero_tie_break(self):
        image = self.image((15, 15))
        for command in ('list', 'extract'):
            with self.subTest(command=command):
                self.selected(image, 0, 15, command)

    def test_show_reports_headers_even_when_bodies_are_bad(self):
        image = self.image(states=('crc-bad', 'truncated'))
        result = self.cli(image, 'show')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(result.stdout.splitlines()), 2)
        self.assertIn('slot 0: generation 10, 4 entries', result.stdout)
        self.assertIn('slot 1: generation 20, 4 entries', result.stdout)
        self.assertEqual(result.stderr, '')

    def test_source_read_errors_are_not_hidden_by_fallback(self):
        image = self.image()
        spec = importlib.util.spec_from_file_location('var_store_read_target', TOOL)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        original = module.Source.read

        def read(source, offset, length):
            if offset == SLOT_BYTES + BLOCK_BYTES:
                raise OSError('synthetic source read failure')
            return original(source, offset, length)

        output = io.StringIO()
        with mock.patch.object(module.Source, 'read', read), contextlib.redirect_stdout(output):
            with self.assertRaisesRegex(OSError, 'synthetic source read failure'):
                module.main(['test', '--image', str(image), 'list', PREFIX])
        self.assertEqual(output.getvalue(), '')


if __name__ == '__main__':
    unittest.main()
