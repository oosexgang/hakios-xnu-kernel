#!/usr/bin/env python3
"""tools/kconfig.py and tools/gen_headers.py: the tree's own Kconfig and fragments hold together; the language as this
tree uses it (depends on, select, default if, def_bool, menus, if blocks, source) gives the values Linux's would; a
fragment's ask is taken where the dependencies allow it, and said where they do not; savedefconfig gives back a fragment
of the same configuration; kconfig.h defines every symbol under #ifndef and names what is off; and the headers user
space shares with the kernel come out without the kernel's part, each compiling on its own."""
import contextlib
import io
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
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

    def test_the_finer_points_linux_has(self):
        top = '''config X
\tbool "X"
\thelp
\t  X.

config P
\tbool "P, its prompt only where X is" if X
\tdefault y
\thelp
\t  A prompt's own if hides the prompt, not the symbol.

config Q
\tbool "Q"
\tdefault y if P = X
\thelp
\t  Two symbols compared.

config R
\tbool "R"
\thelp
\t  A help whose lines are a tab and spaces,
\t    and a line further in.
        default y
'''
        (self.dir / 'Kconfig').write_text(top)
        k = kconfig.Kconfig(self.dir)
        values, _ = k.resolve({'P': False})
        self.assertTrue(values['P'], 'hidden (X is n): the ask is not taken, the default is')
        values, _ = k.resolve({'X': True, 'P': False})
        self.assertFalse(values['P'], 'shown: taken')
        self.assertFalse(values['Q'], 'P (n) = X (y): not the same')
        self.assertTrue(k.resolve({'X': True})[0]['Q'], 'P (y) = X (y): the same')
        values, _ = k.resolve({})
        self.assertFalse(values['Q'], 'P (y) is not X (n)')
        self.assertTrue(values['R'], 'eight spaces are a tab: the default after the help is the entry\'s, not help text')
        self.assertNotIn('default', k.symbols['R'].help)
        self.assertEqual(kconfig.symbols_in(kconfig.parse_expression('P = X', 'x')), {'P', 'X'})

    def test_a_loop_is_said(self):
        (self.dir / 'Kconfig').write_text('config A\n\tbool "A"\n\tdefault y\n\tdepends on !B\n\tselect B\n\thelp\n\t  A.\n'
                                          'config B\n\tbool\n\thelp\n\t  B.\n')
        _, warnings = kconfig.Kconfig(self.dir).resolve({})
        self.assertTrue(any('does not settle' in w for w in warnings), warnings)

    def test_what_is_not_this_language_is_refused(self):
        for text, said in (('config A\n\ttristate "A"\n', 'only bool'), ('config a\n\tbool "a"\n', 'upper case'),
                           ('menu "M"\nconfig A\n\tbool "A"\n', 'menu not closed'), ('choice\n', 'not a word'),
                           ('config A\n\tbool "A"\nconfig A\n\tbool "A"\n', 'described already'),
                           ('config A\n\tbool "A"\n\tdepends on A &&\n', 'ends too soon'),
                           ('menu "M"\nvisible if A\nendmenu\n', 'not a word')):
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
        # (the command itself, as a porter runs it, and what it writes read back)
        for text in ('CONFIG_C=y\n# CONFIG_B is not set\n',
                     '# CONFIG_B is not set\n# CONFIG_D is not set\n',   # D defaults to y once B is off: its n is to be kept
                     '# CONFIG_A is not set\nCONFIG_D=y\n'):
            fragment.write_text(text)
            values, _ = k.resolve(kconfig.read_fragment(fragment))
            said = io.StringIO()
            with mock.patch.object(kconfig.Kconfig, '__init__', lambda own, *a: own.__dict__.update(k.__dict__)), \
                    contextlib.redirect_stdout(said):
                self.assertEqual(kconfig.main(['savedefconfig', str(fragment)]), 0)
            saved = self.dir / 'saved.config'
            saved.write_text(said.getvalue())
            self.assertEqual(k.resolve(kconfig.read_fragment(saved))[0], values, f'{text!r}: the same configuration back')
        self.assertIn('# CONFIG_D is not set', k.savedefconfig(k.resolve({'B': False, 'D': False})[0]),
                      "D's n is written: its default under B=n is y")
        self.assertEqual(k.savedefconfig(k.defaults()), [], 'the defaults: nothing')
        self.assertEqual(k.savedefconfig(k.resolve({'A': False})[0]), ['# CONFIG_A is not set'],
                         'what follows from A=n (B, E off) is not written: the smallest fragment')
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
    def test_a_header_that_needs_stdint_and_does_not_say_so(self):
        out = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, out)
        source = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, source)
        (source / 'bad.h').write_text('struct bad { uint32_t x; };\n')
        with mock.patch.object(gen_headers, 'USER_SOURCES', source):
            problems = gen_headers.user_headers(out)
        self.assertTrue(any(p.startswith('bad.h: does not compile on its own') for p in problems), problems)

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
        (out / 'sys/board').mkdir(parents=True)
        (out / 'sys/board/gone.h').write_text('/* a header whose source is no more */\n')
        problems = gen_headers.user_headers(out)
        self.assertEqual(problems, [])
        self.assertFalse((out / 'sys/board/gone.h').exists(), 'a header whose source is gone is taken away')
        exported = sorted(p.name for p in (out / 'sys/board').glob('*.h'))
        self.assertEqual(exported, sorted(p.name for p in (ROOT / 'include/board').glob('*.h')))
        text = (out / 'sys/board/input.h').read_text()
        self.assertNotIn('hakios_drivers_init', text, "the kernel's declarations left out")
        self.assertIn('as user space sees it', text)


if __name__ == '__main__':
    unittest.main()
