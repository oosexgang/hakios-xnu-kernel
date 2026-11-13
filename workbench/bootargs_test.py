#!/usr/bin/env python3
"""tools/bootargs.py: an argument is found by its read, described by the comment that names it, and the
committed reference is what the tool writes."""
import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('bootargs', ROOT / 'tools/bootargs.py')
bootargs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootargs)

SOURCE = '''
/*
 * A driver. Nothing here is done unless a boot-arg asks for it:
 *
 *   torch=1    a look, reading only: the module's type and its registers
 *   torch=2    ... and the light on once
 *   torch_secs=N       how long (default 2)
 *
 * Other text. With rate=N the thing runs at N a second. And more words here.
 */
/* Boot-arg fbtrace=N: log the first N calls of every selector (default 3). Unrelated sentence. */
static TUNABLE(uint32_t, gTrace, "fbtrace", 3);
void init(void) {
	PE_parse_boot_argn("torch", &level, sizeof(level));
	PE_parse_boot_argn("torch_secs", &secs, sizeof(secs));
	PE_parse_boot_arg_str("who", name, sizeof(name));
	if (PE_parse_boot_argn("rate", &rate, sizeof(rate))) { }
}
'''


class Describe(unittest.TestCase):
    def setUp(self):
        self.names = sorted({a or b for a, b in bootargs.READ.findall(SOURCE)})
        self.lines = bootargs.comment_lines(SOURCE)

    def test_every_kind_of_read_is_found(self):
        self.assertEqual(self.names, ['fbtrace', 'rate', 'torch', 'torch_secs', 'who'])

    def test_a_comment_written_for_the_argument(self):
        self.assertEqual(bootargs.describe('fbtrace', self.lines),
                         'fbtrace=N: log the first N calls of every selector (default 3).')

    def test_an_entry_of_a_table_is_its_own_line(self):
        self.assertEqual(bootargs.describe('torch_secs', self.lines), 'torch_secs=N how long (default 2)')
        self.assertTrue(bootargs.describe('torch', self.lines).startswith('torch=1 a look, reading only'))
        self.assertNotIn('torch=2', bootargs.describe('torch', self.lines))

    def test_a_sentence_in_running_text(self):
        self.assertEqual(bootargs.describe('rate', self.lines), 'With rate=N the thing runs at N a second.')

    def test_an_argument_no_comment_names(self):
        self.assertEqual(bootargs.describe('who', self.lines), '')


class Repository(unittest.TestCase):
    def test_the_committed_reference_is_what_the_tool_writes(self):
        self.assertEqual(bootargs.OUT.read_text(), bootargs.render(bootargs.collect()), 'run `make docs`')

    def test_the_arguments_of_the_newer_drivers_are_described(self):
        found = bootargs.collect()
        for file, name in (('bsd/dev/board/backlight.c', 'backlight'), ('bsd/dev/board/thermal.c', 'thermal'),
                           ('bsd/dev/board/thermal.c', 'th_trip')):
            self.assertTrue(found[file][name], f'{name} in {file} has no description')


if __name__ == '__main__':
    unittest.main(argv=sys.argv[:1])
