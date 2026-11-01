#!/usr/bin/env python3
"""Exercise sparse-split's CLI using synthetic local images; decode the written pieces independently."""
import contextlib
import hashlib
import importlib.util
import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / 'tools/sparse-split.py'
BS = 4096
MINIMUM = (1 << 20) + BS


class SplitTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='sparse split test ')
        self.root = Path(self.temp.name)
        self.image = self.root / 'input.raw'
        self.image.write_bytes(b'A' * BS * 3)
        self.out = self.root / 'pieces'

    def tearDown(self):
        self.temp.cleanup()

    def run_tool(self, limit=None):
        args = [sys.executable, str(TOOL), str(self.image), str(self.out)]
        if limit is not None:
            args.append(str(limit))
        return subprocess.run(args, text=True, capture_output=True, timeout=10)

    def decode(self, expected, files):
        """Read the on-disk sparse headers and payloads, without importing the writer."""
        result = bytearray(len(expected))
        covered = [False] * (len(expected) // BS)
        for file in files:
            raw = file.read_bytes()
            header = struct.unpack_from('<IHHHHIIII', raw)
            magic, major, minor, header_size, chunk_size, block_size, total, chunks, _ = header
            self.assertEqual((magic, major, minor, header_size, chunk_size, block_size),
                             (0xED26FF3A, 1, 0, 28, 12, BS))
            self.assertEqual(total * BS, len(expected))
            at, block = header_size, 0
            for _ in range(chunks):
                kind, reserved, count, size = struct.unpack_from('<HHII', raw, at)
                self.assertEqual(reserved, 0)
                self.assertGreater(count, 0)
                self.assertLessEqual(block + count, total)
                self.assertGreaterEqual(size, chunk_size)
                payload = raw[at + chunk_size:at + size]
                self.assertEqual(len(payload), size - chunk_size)
                if kind == 0xCAC1:
                    self.assertEqual(len(payload), count * BS)
                    data = payload
                elif kind == 0xCAC2:
                    self.assertEqual(len(payload), 4)
                    data = payload * (count * BS // 4)
                else:
                    self.assertEqual(kind, 0xCAC3)
                    self.assertEqual(payload, b'')
                    data = None
                if data is not None:
                    self.assertFalse(any(covered[block:block + count]), 'overlapping pieces')
                    covered[block:block + count] = [True] * count
                    result[block * BS:(block + count) * BS] = data
                block += count
                at += size
            self.assertEqual(block, total)
            self.assertEqual(at, len(raw))
        self.assertTrue(all(covered), 'unwritten input blocks')
        self.assertEqual(result, expected)

    def test_small_limits_fail_before_creating_outputs(self):
        for limit in [-1, 0, BS, (1 << 20) - 1, 1 << 20, MINIMUM - 1]:
            with self.subTest(limit=limit):
                p = self.run_tool(limit)
                self.assertNotEqual(p.returncode, 0, p.stdout + p.stderr)
                self.assertIn('MAX_BYTES', p.stderr)
                self.assertNotIn('Traceback', p.stderr)
                self.assertFalse(self.out.exists())

    def test_non_integer_limit_is_a_cli_error(self):
        p = self.run_tool('not-a-limit')
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('MAX_BYTES', p.stderr)
        self.assertNotIn('Traceback', p.stderr)
        self.assertFalse(self.out.exists())

    def test_minimum_default_and_hex_limits_round_trip(self):
        expected = b'A' * BS + bytes(BS) * 2 + b'B' * BS * 3 + bytes(BS) + b'C' * BS
        self.image.write_bytes(expected)
        for i, limit in enumerate([MINIMUM, MINIMUM + BS, hex(MINIMUM + 2 * BS), None]):
            with self.subTest(limit=limit):
                self.out = self.root / f'pieces-{i}'
                p = self.run_tool(limit)
                self.assertEqual(p.returncode, 0, p.stderr)
                self.assertIn(hashlib.sha256(expected).hexdigest(), p.stdout)
                pieces = sorted(self.out.glob('piece-*.simg'), reverse=True)
                self.assertTrue(pieces)
                bound = 768 << 20 if limit is None else int(str(limit), 0)
                self.assertTrue(all(f.stat().st_size <= bound for f in pieces))
                self.decode(expected, pieces)

    def test_raw_and_zero_only_images_round_trip(self):
        for i, data in enumerate([b'D' * BS * 4, bytes(BS * 4)]):
            self.out = self.root / f'content-{i}'
            self.image.write_bytes(data)
            p = self.run_tool(MINIMUM)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.decode(data, sorted(self.out.glob('piece-*.simg')))

    def test_existing_pieces_are_rejected_and_preserved(self):
        self.assertEqual(self.run_tool(MINIMUM).returncode, 0)
        before = {p.name:p.read_bytes() for p in self.out.iterdir()}
        self.image.write_bytes(b'B' * BS)
        p = self.run_tool(MINIMUM)
        self.assertNotEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn('directory', p.stderr)
        self.assertEqual(before, {p.name:p.read_bytes() for p in self.out.iterdir()})

    def test_existing_unrelated_files_are_preserved(self):
        self.out.mkdir()
        note = self.out / 'README.txt'
        note.write_text('keep this')
        p = self.run_tool(MINIMUM)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(note.read_text(), 'keep this')
        self.decode(self.image.read_bytes(), sorted(self.out.glob('piece-*.simg')))

    def test_input_named_like_a_piece_is_not_truncated(self):
        self.out.mkdir()
        self.image = self.out / 'piece-00.simg'
        original = b'input' + bytes(BS - 5)
        self.image.write_bytes(original)
        p = self.run_tool(MINIMUM)
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(self.image.read_bytes(), original)

    def test_a_racing_file_creation_is_not_overwritten(self):
        spec = importlib.util.spec_from_file_location('sparse_split', TOOL)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        makedirs = module.os.makedirs
        marker = b'created by another writer'

        def race(path, exist_ok=False):
            makedirs(path, exist_ok=exist_ok)
            (self.out / 'piece-00.simg').write_bytes(marker)

        with mock.patch.object(module.os, 'makedirs', side_effect=race), \
             mock.patch.object(sys, 'argv', [str(TOOL), str(self.image), str(self.out), str(MINIMUM)]), \
             contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(SystemExit):
                module.main()
        self.assertEqual((self.out / 'piece-00.simg').read_bytes(), marker)

    def test_non_block_aligned_input_is_rejected(self):
        self.image.write_bytes(b'short')
        p = self.run_tool(MINIMUM)
        self.assertNotEqual(p.returncode, 0)
        self.assertFalse(self.out.exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
