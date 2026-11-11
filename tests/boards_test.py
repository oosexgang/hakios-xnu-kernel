#!/usr/bin/env python3
"""tools/boards.py: what it accepts, what it refuses and says why, what it reads out of a device tree, and
that the kernel's committed table is what it writes."""
import importlib.util
import io
import contextlib
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('boards', ROOT / 'tools/boards.py')
boards = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boards)

SOC = '''name = "sm0001"
description = "a made-up SoC"
[clocks]
gcc = 0x100000
gcc_disp_ahb_cbcr = 0x27004
dispcc = 0xaf00000
dispcc_mdss_gdscr = 0x3000
dispcc_mdss_ahb_cbcr = 0x2080
[display]
dsi0 = 0xae94000
[thermal]
tsens = [0xc263000, 0xc265000]
[cpufreq]
epss = 0x18591000
epss_domain_size = 0x1000
'''
BOARD = '''name = "phone"
description = "A phone"
soc = "sm0001"
features = ["cpufreq-epss"]
[backlight]
type = "dcs"
panel_mode = "command"
min = 1
max = 2047
bytes = 2
byte_order = "msb-first"
[thermal]
floor_mhz = [1000, 1100, 1100]
'''
# what `dtc -I dtb -O dts` prints for the nodes that matter, cut down
DTS = '''/dts-v1/;
/ {
	soc {
		#address-cells = <0x02>;
		clock-controller@100000 {
			compatible = "qcom,shima-gcc\\0syscon";
			reg = <0x00 0x100000 0x00 0x1f0000>;
		};
		clock-controller@af00000 {
			compatible = "qcom,shima-dispcc", "syscon";
			reg = <0x00 0xaf00000 0x00 0x20000>;
		};
		qcom,cpufreq-hw {
			compatible = "qcom,cpufreq-hw-epss";
			reg = <0x00 0x18591000 0x00 0x1000 0x00 0x18592000 0x00 0x1000 0x00 0x18593000 0x00 0x1000>;
			reg-names = "freq-domain0\\0freq-domain1\\0freq-domain2";
		};
		tsens0: tsens@c222000 {
			compatible = "qcom,tsens24xx";
			reg = <0x00 0xc222000 0x00 0x08 0x00 0xc263000 0x00 0x1ff>;
			reg-names = "tsens_srot_physical\\0tsens_tm_physical";
		};
		tsens@c223000 {
			reg = <0x00 0xc223000 0x00 0x08 0x00 0xc265000 0x00 0x1ff>;
			reg-names = "tsens_srot_physical", "tsens_tm_physical";
		};
		qcom,mdss_dsi_ctrl0@ae94000 {
			compatible = "qcom,dsi-ctrl-hw-v2.5";
			cell-index = <0x00>;
			reg = <0x00 0xae94000 0x00 0x400 0x00 0xaf08000 0x00 0x04>; /* a comment */
			reg-names = "dsi_ctrl\\0disp_cc_base";
		};
		qcom,mdss_dsi_ctrl1@ae96000 {
			cell-index = <0x01>;
			reg = <0x00 0xae96000 0x00 0x400 0x00 0xaf08000 0x00 0x04>;
			reg-names = "dsi_ctrl\\0disp_cc_base";
		};
	};
	fragment@7 {
		__overlay__ {
			qcom,mdss_dsi_first_cmd {
				qcom,mdss-dsi-panel-name = "first panel";
				qcom,mdss-dsi-panel-type = "dsi_cmd_mode";
				qcom,mdss-dsi-bl-inverted-dbv;
				qcom,mdss-dsi-on-command = [39 01 00 00 00 00 03 f0 5a 5a];
				qcom,mdss-dsi-bl-pmic-control-type = "bl_ctrl_dcs";
				qcom,mdss-dsi-bl-min-level = <0x01>;
				qcom,mdss-dsi-bl-max-level = <0x7ff>;
				qcom,mdss-brightness-init-level = <0x133>;
				qcom,mdss-dsi-display-timings {
					timing@0 {
						qcom,mdss-dsi-panel-framerate = <0x3c>;
					};
				};
			};
			qcom,mdss_dsi_second_video {
				qcom,mdss-dsi-panel-type = "dsi_video_mode";
				qcom,mdss-dsi-bl-pmic-control-type = "bl_ctrl_wled";
			};
		};
	};
};
'''


class Sandbox(unittest.TestCase):
    """A repository of its own for each test: one SoC, one board, the real kernel header."""

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.saved = (boards.ROOT, boards.DEVICES, boards.SOCS, boards.TABLE)
        boards.ROOT, boards.DEVICES, boards.SOCS = self.dir, self.dir / 'devices', self.dir / 'soc'
        boards.TABLE = self.dir / 'table.h'
        (self.dir / 'devices/phone').mkdir(parents=True)
        (self.dir / 'soc').mkdir()
        (self.dir / 'soc/sm0001.toml').write_text(SOC)
        (self.dir / 'devices/phone/board.toml').write_text(BOARD)

    def tearDown(self):
        boards.ROOT, boards.DEVICES, boards.SOCS, boards.TABLE = self.saved
        shutil.rmtree(self.dir)

    def problems(self):
        found = boards.Problems()
        boards.load(found)
        return '\n'.join(found)

    def board(self, old, new):
        path = self.dir / 'devices/phone/board.toml'
        text = path.read_text()
        self.assertIn(old, text)
        path.write_text(text.replace(old, new))

    def soc(self, old, new):
        path = self.dir / 'soc/sm0001.toml'
        text = path.read_text()
        self.assertIn(old, text)
        path.write_text(text.replace(old, new))


class Check(Sandbox):
    def test_a_complete_board_is_accepted(self):
        self.assertEqual(self.problems(), '')

    def test_refusals_name_the_file_and_the_reason(self):
        cases = [
            (self.board, 'soc = "sm0001"', 'soc = "sm9999"', 'there is no soc/sm9999.toml'),
            (self.board, '"cpufreq-epss"', '"warp-drive"', 'feature "warp-drive" is not one the kernel knows'),
            (self.board, 'name = "phone"', 'name = "other"', 'the directory is devices/phone'),
            (self.board, 'max = 2047', 'max = 1', 'min must be below max'),
            (self.board, 'max = 2047\n', '', 'type "dcs" needs "max"'),
            (self.board, 'max = 2047', 'max = 70000', 'max does not fit in 2 byte'),
            (self.board, 'byte_order = "msb-first"\n', '', 'two bytes need "byte_order"'),
            (self.board, 'panel_mode = "command"', 'panel_mode = "sideways"', 'one of command, video'),
            (self.board, 'min = 1', 'min = "one"', 'int expected'),
            (self.board, 'min = 1', 'min = 1\nbrightness = 5', 'unknown key "brightness"'),
            (self.board, '[thermal]', '[thermals]', 'unknown section [thermals]'),
            (self.board, 'floor_mhz = [1000, 1100, 1100]', 'floor_mhz = [1000]', 'three values'),
            (self.board, 'description = "A phone"\n', '', '"description" is missing'),
            (self.board, 'soc = "sm0001"', 'soc = "sm0001', 'not TOML'),
            (self.soc, '[display]\ndsi0 = 0xae94000\n', '', 'backlight type "dcs" needs [display]'),
            (self.soc, '[cpufreq]\nepss = 0x18591000\nepss_domain_size = 0x1000\n', '', 'feature "cpufreq-epss" needs [cpufreq]'),
            (self.soc, 'gcc = 0x100000', 'gcc = 0x100000000', 'a value of 32 bits'),
            (self.soc, 'name = "sm0001"', 'name = "sm0002"', 'the file is sm0001.toml'),
        ]
        for change, old, new, reason in cases:
            with self.subTest(reason=reason):
                saved = [(p, p.read_text()) for p in (self.dir / 'devices/phone/board.toml', self.dir / 'soc/sm0001.toml')]
                try:
                    change(old, new)
                    said = self.problems()
                    self.assertIn(reason, said)
                    self.assertTrue('devices/phone/board.toml' in said or 'soc/sm0001.toml' in said, said)
                finally:
                    for path, text in saved:
                        path.write_text(text)

    def test_a_soft_board_needs_nothing_of_its_soc(self):
        (self.dir / 'soc/sm0001.toml').write_text('name = "sm0001"\ndescription = "bare"\n')
        (self.dir / 'devices/phone/board.toml').write_text(
            'name = "phone"\ndescription = "A phone"\nsoc = "sm0001"\nfeatures = []\n[backlight]\ntype = "soft"\n')
        self.assertEqual(self.problems(), '')

    def test_the_template_is_not_a_board(self):
        (self.dir / 'devices/_template').mkdir()
        (self.dir / 'devices/_template/board.toml').write_text('name = "NAME"\n')
        socs, found = boards.load()
        self.assertEqual(list(found), ['phone'])


class Generate(Sandbox):
    def test_the_table_carries_the_values(self):
        text = boards.table_text(*boards.load())
        for expected in ('.name = "phone"', '.soc = "sm0001"', '.features = HAKIOS_HAS_CPUFREQ_EPSS,',
                         '.type = HAKIOS_BACKLIGHT_DCS, .video_mode = 0, .bytes = 2,', '.dsi = 0x0ae94000,',
                         '.raw_min = 1, .raw_max = 2047, .raw_restore = 0,', '.dispcc_mdss_gdscr = 0x3000,',
                         '.tsens = { 0x0c263000, 0x0c265000 },', '.floor_mhz = { 1000, 1100, 1100 },',
                         '.epss = 0x18591000, .epss_domain_size = 0x1000', '"cpufreq-epss"'):
            self.assertIn(expected, text)
        self.assertNotIn('*/board.toml', text, 'a path with */ in it would close the header\'s comment')

    def test_it_is_the_same_every_time(self):
        self.assertEqual(boards.table_text(*boards.load()), boards.table_text(*boards.load()))


class DeviceTree(Sandbox):
    def test_nodes_and_properties_are_read(self):
        tree = boards.parse_dts(DTS)
        names = [node.name for node in tree.walk()]
        for name in ('clock-controller@af00000', 'tsens@c222000', 'qcom,mdss_dsi_first_cmd', 'timing@0'):
            self.assertIn(name, names)
        panel = next(node for node in tree.walk() if node.name == 'qcom,mdss_dsi_first_cmd')
        self.assertEqual(boards.cells(panel.props['qcom,mdss-dsi-bl-max-level']), [0x7ff])
        self.assertIn('qcom,mdss-dsi-bl-inverted-dbv', panel.props)
        self.assertEqual(boards.strings('"a\\0b", "c"'), ['a', 'b', 'c'])

    def test_what_a_board_needs_is_found(self):
        soc, board, notes = boards.from_dts(DTS)
        self.assertEqual(soc['display'], {'dsi0': 0xae94000})          # controller 0, not 1
        self.assertEqual(soc['thermal'], {'tsens': [0xc263000, 0xc265000]})
        self.assertEqual(soc['cpufreq'], {'epss': 0x18591000, 'epss_domain_size': 0x1000})
        self.assertEqual(soc['clocks'], {'gcc': 0x100000, 'dispcc': 0xaf00000})
        light = board['backlight']
        self.assertEqual((light['type'], light['panel_mode'], light['min'], light['max'], light['restore'], light['bytes'],
                          light['byte_order']), ('dcs', 'command', 1, 0x7ff, 0x133, 2, 'msb-first'))
        self.assertTrue(any('several panels' in note for note in notes))
        self.assertTrue(any('dispcc_mdss_gdscr' in note for note in notes))

    def test_a_panel_without_dcs_brightness_is_dimmed_in_software(self):
        soc, board, notes = boards.from_dts(DTS, panel='qcom,mdss_dsi_second_video')
        self.assertEqual(board['backlight']['type'], 'soft')
        self.assertTrue(any('bl_ctrl_wled' in note for note in notes))

    def test_a_new_board_is_started_from_a_device_tree(self):
        dts = self.dir / 'phone2.dts'
        dts.write_text(DTS)
        with contextlib.redirect_stdout(io.StringIO()) as said:
            boards.new_board('phone2', 'sm0002', dts, 'qcom,mdss_dsi_first_cmd', 'Another phone')
        self.assertIn('wrote devices/phone2/board.toml', said.getvalue())
        self.assertIn('wrote soc/sm0002.toml', said.getvalue())
        # as written it is refused for exactly what a device tree cannot say, and accepted once that is filled in
        problems = self.problems()
        self.assertIn('"gcc_disp_ahb_cbcr" is missing', problems)
        self.assertNotIn('devices/phone2/board.toml', problems.replace('soc/sm0002.toml', ''))
        path = self.dir / 'soc/sm0002.toml'
        text = path.read_text()
        for key, value in (('gcc_disp_ahb_cbcr', '0x27004'), ('dispcc_mdss_gdscr', '0x3000'), ('dispcc_mdss_ahb_cbcr', '0x2080')):
            text = text.replace(f'# {key} = 0x...            # TO FILL IN', f'{key} = {value}')
        path.write_text(text)
        self.assertEqual(self.problems(), '')
        self.assertIn('.raw_min = 1, .raw_max = 2047, .raw_restore = 307,', boards.table_text(*boards.load()))

    def test_an_existing_board_is_not_overwritten(self):
        with self.assertRaises(SystemExit):
            boards.new_board('phone', 'sm0001')


class Repository(unittest.TestCase):
    def test_the_real_boards_are_valid(self):
        problems = boards.Problems()
        socs, found = boards.load(problems)
        self.assertEqual(list(problems), [])
        self.assertIn('renoir', found)

    def test_the_committed_table_is_what_the_tool_writes(self):
        self.assertEqual(boards.TABLE.read_text(), boards.table_text(*boards.load()),
                         'run tools/boards.py gen')


if __name__ == '__main__':
    unittest.main(argv=sys.argv[:1] + ['-v'] if '-v' in sys.argv else sys.argv[:1])
