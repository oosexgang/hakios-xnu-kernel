"""tools/identity.py: a phone's seed is made once and kept, outside any checkout, and gives what the loader
(arch/arm64/boot/src/unit.c) makes of it -- workbench/loader_test.c holds the loader to the same values.

    python3 workbench/identity_test.py
"""
import importlib.util
import os
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('identity', ROOT / 'tools/identity.py')
identity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(identity)


class Identity(unittest.TestCase):
    def test_a_seed_gives_values_of_the_real_forms(self):
        u = identity.unit(0x0123456789abcdef)
        # (the same as the loader's, workbench/loader_test.c "unit")
        self.assertEqual((u['serial'], u['mlb'], u['ecid']), ('7BN0E0VZPK', '5JTH1PNG6UYUS1AA7', 0xb77554620b3dd))
        self.assertEqual(u['wifi'], [0x2e, 0x2c, 0x55, 0x46, 0x7d, 0x6d])
        for seed in range(500):
            u = identity.unit(seed * 0x9e3779b1)
            self.assertTrue(len(u['serial']) == 10 and set(u['serial']) <= set(identity.ALPHABET))
            self.assertTrue(len(u['mlb']) == 17 and set(u['mlb']) <= set(identity.ALPHABET))
            self.assertNotIn('I', u['serial'] + u['mlb'])
            self.assertNotIn('O', u['serial'] + u['mlb'])
            self.assertTrue(0 < u['ecid'] < 1 << 52)
            self.assertEqual((u['wifi'][0] & 3, u['bluetooth'][0] & 3), (2, 2))
            self.assertNotEqual(u['wifi'], u['bluetooth'])
            self.assertEqual(len(u['manifest_hash']), 48)

    def test_a_phone_keeps_its_seed_and_another_gets_its_own(self):
        with tempfile.TemporaryDirectory() as home:
            old = os.environ.get('XDG_CONFIG_HOME')
            os.environ['XDG_CONFIG_HOME'] = home
            try:
                first = identity.seed_of('phone')
                self.assertEqual(identity.seed_of('phone'), first)
                self.assertNotEqual(identity.seed_of('other'), first)
                kept = Path(home) / 'hakios/identity/phone'
                self.assertEqual(kept.read_text(), f'{first:016x}\n')
                self.assertEqual(kept.stat().st_mode & 0o777, 0o600)
                self.assertFalse(str(kept).startswith(str(ROOT)))
            finally:
                if old is None:
                    del os.environ['XDG_CONFIG_HOME']
                else:
                    os.environ['XDG_CONFIG_HOME'] = old

    def test_what_it_says_of_a_seed(self):
        text = identity.describe(0x0123456789abcdef)
        self.assertIn('identity=0123456789abcdef', text)
        self.assertIn('serial number  7BN0E0VZPK', text)


if __name__ == '__main__':
    unittest.main()
