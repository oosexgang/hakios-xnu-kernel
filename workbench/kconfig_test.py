#!/usr/bin/env python3
"""tools/kconfig.py and tools/gen_headers.py: the tree's own Kconfig and fragments hold together; the language as this
tree uses it (depends on, select, default if, def_bool, menus, if blocks, source) gives the values Linux's would; a
fragment's ask is taken where the dependencies allow it, and said where they do not; savedefconfig gives back a fragment
of the same configuration; kconfig.h defines every symbol under #ifndef and names what is off; and the headers user
space shares with the kernel come out without the kernel's part, each compiling on its own."""
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

import gen_headers  # noqa: E402
import kconfig  # noqa: E402

TOP = '''mainmenu "A test"
source "sub/Kconfig"

menu "Things"

config A
	bool "A, chosen"
	default y
	help
	  The first.

config B
	bool "B, which needs A"
	default y
	depends on A
	help
	  The second.

config C
	bool "C, off unless asked"
	select H
	help
	  The third.

config D
	bool "D, on when A is and B is not"
	default y if A && !B
	default n
	help
	  The fourth.

endmenu

if A
config E
	bool "E, inside if A"
	default y
	help
	  The fifth.
endif

config H
	bool
	help
	  A helper nobody chooses.

config F
	def_bool A || C
	help
	  Not a choice: A or C.
'''
SUB = '''config S
	bool "S, from another file"
	depends on !C
	help
	  Sourced.
'''


class Language(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)
        (self.dir / 'Kconfig').write_text(TOP)
        (self.dir / 'sub').mkdir()
        (self.dir / 'sub/Kconfig').write_text(SUB)

    def resolve(self, **asked):
        k = kconfig.Kconfig(self.dir)
        return k.resolve({name: value == 'y' for name, value in asked.items()})

    def test_the_defaults(self):
        k = kconfig.Kconfig(self.dir)
        self.assertEqual(k.title, 'A test')
        self.assertEqual(list(k.symbols), ['S', 'A', 'B', 'C', 'D', 'E', 'H', 'F'])
        values, warnings = self.resolve()
        self.assertEqual({n for n, v in values.items() if v}, {'A', 'B', 'E', 'F'})
        self.assertEqual(warnings, [])
        self.assertEqual(k.symbols['B'].menu, 'Things')
        self.assertEqual(k.symbols['A'].help, 'The first.')

    def test_dependencies_and_selections(self):
        values, _ = self.resolve(A='n')
        self.assertFalse(values['B'], 'B needs A')
        self.assertFalse(values['E'], 'E is inside if A')
        self.assertFalse(values['D'], 'D defaults to y only with A')
        values, _ = self.resolve(B='n')
        self.assertTrue(values['D'], 'with A and not B: D')
        values, _ = self.resolve(C='y')
        self.assertTrue(values['C'] and values['H'], 'C selects H, which nobody chooses')
        self.assertFalse(values['S'], 'S needs !C')
        self.assertTrue(values['F'], 'def_bool A || C')

    def test_what_cannot_be_had_is_said(self):
        values, warnings = self.resolve(A='n', B='y', NOPE='y')
        self.assertFalse(values['B'])
        self.assertIn('CONFIG_B=y asked for, and its dependencies are not met: n', warnings)
        self.assertIn('CONFIG_NOPE: no Kconfig describes it: left out', warnings)

    def test_a_selection_over_unmet_dependencies_is_said(self):
        (self.dir / 'sub/Kconfig').write_text(SUB + '\nconfig T\n\tbool "T"\n\tselect S\n\thelp\n\t  Selects S.\n')
        values, warnings = self.resolve(C='y', T='y')
        self.assertTrue(values['S'], 'selected: on, as in Linux')
        self.assertIn('CONFIG_S is selected, and its own dependencies are not met', warnings)

    def test_what_is_not_this_language_is_refused(self):
        for text, said in (('config A\n\ttristate "A"\n', 'only bool'), ('config a\n\tbool "a"\n', 'upper case'),
                           ('menu "M"\nconfig A\n\tbool "A"\n', 'menu not closed'), ('choice\n', 'not a word'),
                           ('config A\n\tbool "A"\nconfig A\n\tbool "A"\n', 'described already'),
                           ('config A\n\tbool "A"\n\tdepends on A &&\n', 'ends too soon')):
            (self.dir / 'Kconfig').write_text(text)
            with self.assertRaises(kconfig.KconfigError) as raised:
                kconfig.Kconfig(self.dir)
            self.assertIn(said, str(raised.exception))

    def test_check(self):
        k = kconfig.Kconfig(self.dir)
        self.assertEqual(k.check(), [])
        self.assertEqual(k.check([('NOPE', 'x.c:1')]), ['x.c:1: CONFIG_NOPE is used, and no Kconfig describes NOPE'])
        (self.dir / 'sub/Kconfig').write_text(SUB + '\nconfig U\n\tbool "U"\n\tdepends on GONE\n')
        problems = kconfig.Kconfig(self.dir).check()
        self.assertTrue(any('GONE, which no Kconfig describes' in p for p in problems), problems)
        self.assertTrue(any('U can be chosen and says nothing' in p for p in problems), problems)

    def test_fragments_and_savedefconfig(self):
        fragment = self.dir / 'mine.config'
        fragment.write_text('# mine\nCONFIG_C=y\n# CONFIG_B is not set\n')
        asked = kconfig.read_fragment(fragment)
        self.assertEqual(asked, {'C': True, 'B': False})
        k = kconfig.Kconfig(self.dir)
        values, _ = k.resolve(asked)
        saved = self.dir / 'saved.config'
        defaults = k.defaults()
        saved.write_text(''.join((f'CONFIG_{s.name}=y\n' if values[s.name] else f'# CONFIG_{s.name} is not set\n')
                                 for s in k.symbols.values() if s.prompt is not None and values[s.name] != defaults[s.name]))
        self.assertEqual(k.resolve(kconfig.read_fragment(saved))[0], values, 'the saved fragment makes the same configuration')
        (self.dir / 'bad.config').write_text('CONFIG_A=maybe\n')
        with self.assertRaises(kconfig.KconfigError):
            kconfig.read_fragment(self.dir / 'bad.config')

    def test_the_header(self):
        k = kconfig.Kconfig(self.dir)
        values, _ = k.resolve({'B': False})
        text = kconfig.header(k, values, [Path('mine.config')])
        for name in k.symbols:
            self.assertRegex(text, rf'#ifndef CONFIG_{name}\n#define CONFIG_{name} [01]\n#endif')
        self.assertIn('#define CONFIG_B 0', text)
        self.assertIn('#define KCONFIG_OFF "S B C"', text, 'the choices that are off, not the helpers (H)')
        self.assertIn('#define KCONFIG_MADE_OF "mine.config"', text)
        # and it compiles, with a symbol set before it as a test does
        done = subprocess.run(['cc', '-fsyntax-only', '-x', 'c', '-'], input='#define CONFIG_A 0\n' + text +
                              '\n_Static_assert(CONFIG_A == 0 && CONFIG_E == 1, "");\n', capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)


class Tree(unittest.TestCase):
    def test_the_tree_holds_together(self):
        done = subprocess.run([sys.executable, str(ROOT / 'tools/kconfig.py'), 'check'], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertRegex(done.stdout, r'kconfig: \d+ files, \d+ symbols, \d+ fragments: ok')

    def test_every_driver_started_has_its_symbol(self):
        k = kconfig.Kconfig()
        drivers = (ROOT / 'drivers/board/drivers.c').read_text()
        used = set(re.findall(r'if \(CONFIG_([A-Z0-9_]+)\)', drivers))
        self.assertGreater(len(used), 20)
        self.assertTrue(used <= set(k.symbols), used - set(k.symbols))
        calls = set(re.findall(r'^\t(hakios_[a-z0-9_]+)\(\);', drivers, re.M))
        self.assertEqual(calls, {'hakios_input_init', 'hakios_board_init', 'hakios_net_init', 'hakios_audio_init'},
                         'started unasked: only what is not a choice')

    def test_each_fragment(self):
        k = kconfig.Kconfig()
        on = {}
        for name in ('hakios', 'phone', 'qemu', 'bringup'):
            values, warnings, _ = kconfig.configuration(k, [name])
            self.assertEqual(warnings, [], name)
            on[name] = {n for n, v in values.items() if v}
        self.assertEqual(on['hakios'], set(k.symbols), 'hakios: every symbol')
        self.assertNotIn('VIRTIO_BLK', on['phone'])
        self.assertNotIn('ADRENO_A6XX', on['qemu'])
        self.assertIn('VIRTIO_INPUT', on['qemu'])
        self.assertIn('QCOM_UFS', on['bringup'])
        self.assertNotIn('WIFI_WCN6750', on['bringup'])
        self.assertNotIn('QCOM_QRTR', on['bringup'], 'no driver of the common ones needs QRTR')


class UserHeaders(unittest.TestCase):
    def test_the_kernels_part_left_out(self):
        text = ('#include <stdint.h>\nstruct a { uint32_t x; };\n#ifdef KERNEL\n#if X\nvoid k(void);\n#endif\nint kk;\n'
                '#else\nint user_only;\n#endif\n#ifndef KERNEL\nint also_user;\n#else\nint kernel_again;\n#endif\n'
                '#if defined(KERNEL)\nint third;\n#endif\n#if OTHER\nint other;\n#endif\n')
        out = gen_headers.without_kernel(text, 'x.h')
        self.assertNotIn('kk', out)
        self.assertNotIn('void k', out)
        self.assertNotIn('kernel_again', out)
        self.assertNotIn('third', out)
        self.assertIn('int user_only;', out)
        self.assertIn('int also_user;', out)
        self.assertIn('#if OTHER\nint other;\n#endif', out, 'a condition of another name kept as it is')
        self.assertNotIn('KERNEL', out)

    def test_the_trees_headers_for_user_space(self):
        out = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, out)
        problems = gen_headers.user_headers(out)
        self.assertEqual(problems, [])
        exported = sorted(p.name for p in (out / 'sys/board').glob('*.h'))
        self.assertEqual(exported, sorted(p.name for p in (ROOT / 'include/board').glob('*.h')))
        text = (out / 'sys/board/input.h').read_text()
        self.assertNotIn('hakios_drivers_init', text, "the kernel's declarations left out")
        self.assertIn('as user space sees it', text)


if __name__ == '__main__':
    unittest.main()
