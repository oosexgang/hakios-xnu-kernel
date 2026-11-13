#!/usr/bin/env python3
"""tools/qupclocks.py on clocks written the way Linux's drivers/clk/qcom/gcc-*.c writes them."""
import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import qupclocks  # noqa: E402


def clock(name, halt, bit, enable=0x52008):
    return f'''static struct clk_branch {name} = {{
	.halt_reg = {halt:#x},
	.halt_check = BRANCH_HALT_VOTED,
	.hwcg_reg = {halt:#x},
	.hwcg_bit = 1,
	.clkr = {{
		.enable_reg = {enable:#x},
		.enable_mask = BIT({bit}),
		.hw.init = &(struct clk_init_data){{
			.name = "{name}",
			.ops = &clk_branch2_ops,
		}},
	}},
}};

'''


# the second group of serial engines of one SoC and the first of another, as their drivers have them
FILE = ''.join((
    clock('gcc_qupv3_wrap0_core_2x_clk', 0x23000, 9), clock('gcc_qupv3_wrap0_core_clk', 0x23008, 8),
    clock('gcc_qupv3_wrap0_s2_clk', 0x1726c, 12), clock('gcc_qupv3_wrap0_s20_clk', 0x17ffc, 30),
    clock('gcc_qupv3_wrap_0_m_ahb_clk', 0x17004, 6), clock('gcc_qupv3_wrap_0_s_ahb_clk', 0x17008, 7),
    clock('gcc_qupv3_wrap1_core_2x_clk', 0x23138, 18), clock('gcc_qupv3_wrap1_core_clk', 0x23140, 19),
    clock('gcc_qupv3_wrap1_s5_clk', 0x185fc, 27),
    clock('gcc_qupv3_wrap_1_m_ahb_clk', 0x18004, 20), clock('gcc_qupv3_wrap_1_s_ahb_clk', 0x18008, 21),
    # (a clock that is switched on in its own register, not voted for)
    '''static struct clk_branch gcc_disp_ahb_clk = {
	.halt_reg = 0x27004,
	.clkr = {
		.enable_reg = 0x27004,
		.enable_mask = BIT(0),
	},
};
''',
))


class QupClocks(unittest.TestCase):
    def values(self, wrapper, engine, text=FILE):
        lines = qupclocks.touch_lines(text, wrapper, engine)
        return [line.split('//')[0].strip() for line in lines.splitlines()]

    def test_the_two_phones_engines_come_out_as_their_board_files_have_them(self):
        self.assertEqual(self.values(1, 5), ['clocks = <0x18004 0x18008 0x185fc>;', 'vote-register = <0x52008>;',
                                             'votes = <18 19 20 21 27>;'])
        self.assertEqual(self.values(0, 2), ['clocks = <0x17004 0x17008 0x1726c>;', 'vote-register = <0x52008>;',
                                             'votes = <9 8 6 7 12>;'])

    def test_an_engine_is_its_whole_number(self):
        # engine 2 is not engine 20
        self.assertEqual(self.values(0, 20)[0], 'clocks = <0x17004 0x17008 0x17ffc>;')

    def test_what_is_printed_is_what_the_board_tool_takes(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
        import boardfile
        import boards
        text = '/ {\n\ttouch {\n' + qupclocks.touch_lines(FILE, 1, 5) + '\t};\n};\n'
        table = boardfile.parse(text, boards.BOARD_SCHEMA)['touch']
        self.assertEqual(table, {'clocks': [0x18004, 0x18008, 0x185fc], 'vote_register': 0x52008, 'votes': [18, 19, 20, 21, 27]})

    def test_what_is_not_there_is_said(self):
        with self.assertRaisesRegex(qupclocks.NotFound, 'no clock gcc_qupv3_wrap1_s6_clk'):
            qupclocks.touch_lines(FILE, 1, 6)
        with self.assertRaisesRegex(qupclocks.NotFound, 'no clock gcc_qupv3_wrap2_core_2x_clk'):
            qupclocks.touch_lines(FILE, 2, 0)
        with self.assertRaisesRegex(qupclocks.NotFound, 'more than one register'):
            qupclocks.touch_lines(FILE.replace(clock('gcc_qupv3_wrap1_s5_clk', 0x185fc, 27),
                                               clock('gcc_qupv3_wrap1_s5_clk', 0x185fc, 27, enable=0x52010)), 1, 5)
        with self.assertRaisesRegex(qupclocks.NotFound, 'not a clock the application processor votes for'):
            qupclocks.touch_lines(FILE.replace('.enable_mask = BIT(27),', '.enable_mask = 0x8000000,'), 1, 5)

    def test_from_the_command_line(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'gcc-soc.c'
            path.write_text(FILE)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                qupclocks.main(['qupclocks.py', str(path), '1', '5'])
            self.assertIn('votes = <18 19 20 21 27>;', out.getvalue())
            for wrong in (['qupclocks.py'], ['qupclocks.py', str(path), 'one', '5'], ['qupclocks.py', str(path) + '.none', '1', '5'],
                          ['qupclocks.py', str(path), '1', '9']):
                with self.assertRaises(SystemExit) as stopped:
                    qupclocks.main(wrong)
                self.assertTrue(stopped.exception.code)         # (a text, not 0)


if __name__ == '__main__':
    unittest.main(argv=sys.argv[:1] + ['-v'] if '-v' in sys.argv else sys.argv[:1])
