#!/usr/bin/env python3
"""tools/boards.py: what it accepts, what it refuses and says why, what it reads out of a device tree,
where it finds device packages, and the tables it writes for the boards that are there."""
import importlib.util
import io
import os
import re
import subprocess
import contextlib
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# The tests describe boards of their own; packages of the machine they run on (HAKIOS_DEVICES) are not theirs.
os.environ.pop('HAKIOS_DEVICES', None)
spec = importlib.util.spec_from_file_location('boards', ROOT / 'tools/boards.py')
boards = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boards)
import devicetree  # noqa: E402  (tools/, which loading boards put on the path)

SOC = '''/ {
	name = "sm0001";
	description = "a made-up SoC";
	clocks {
		gcc = <0x100000>;
		gcc-disp-ahb-cbcr = <0x27004>;
		dispcc = <0xaf00000>;
		dispcc-mdss-gdscr = <0x3000>;
		dispcc-mdss-ahb-cbcr = <0x2080>;
	};
	display {
		dsi0 = <0xae94000>;
	};
	thermal {
		tsens = <0xc263000 0xc265000>;
	};
	cpufreq {
		epss = <0x18591000>;
		epss-domain-size = <0x1000>;
		domain-cores = <4 3 1>;
	};
	platform {
		io-base = <0x400000>;
		io-size = <0x3fc00000>;
		gic-dist = <0x17a00000>;
		gic-redist = <0x17a60000>;
		cores = <8>;
		core-shift = <8>;
		small-cores = <0x0f>;
		psci = "smc";
	};
};
'''
BOARD = '''/ {
	name = "phone";
	description = "A phone";
	soc = "sm0001";
	features = "cpufreq-epss";
	backlight {
		type = "dcs";
		panel-mode = "command";
		min = <1>;
		max = <2047>;
		bytes = <2>;
		byte-order = "msb-first";
	};
	thermal {
		floor-mhz = <1000 1100 1100>;
	};
	loader {
		fb-base = <0xe1000000>;
	};
};
'''
# what `dtc -I dtb -O dts` prints for the nodes that matter, cut down
DTS = '''/dts-v1/;
/ {
	cpus {
		cpu@0 { device_type = "cpu"; reg = <0x00 0x00>; enable-method = "psci"; capacity-dmips-mhz = <0x400>; qcom,freq-domain = <0x0d 0x00 0x04>; };
		cpu@100 { device_type = "cpu"; reg = <0x00 0x100>; enable-method = "psci"; capacity-dmips-mhz = <0x400>; qcom,freq-domain = <0x0d 0x00 0x04>; };
		cpu@200 { device_type = "cpu"; reg = <0x00 0x200>; enable-method = "psci"; capacity-dmips-mhz = <0x79a>; qcom,freq-domain = <0x0d 0x01 0x04>; };
		cpu@300 { device_type = "cpu"; reg = <0x00 0x300>; enable-method = "psci"; capacity-dmips-mhz = <0x7c1>; qcom,freq-domain = <0x0d 0x02 0x04>; };
		cpu-map { cluster0 { core0 { cpu = <0x11>; }; }; };
	};
	psci {
		compatible = "arm,psci-1.0";
		method = "smc";
	};
	soc {
		#address-cells = <0x02>;
		interrupt-controller@17a00000 {
			compatible = "arm,gic-v3";
			reg = <0x00 0x17a00000 0x00 0x10000 0x00 0x17a60000 0x00 0x100000>;
		};
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
		qfprom@780000 {
			compatible = "qcom,qfprom";
			reg = <0x780000 0x7000>;
		};
		qfprom@0 {
			compatible = "qcom,qfprom-sys";
		};
		clock-controller@3d90000 {
			compatible = "qcom,shima-gpucc\\0syscon";
			reg = <0x3d90000 0x9000>;
		};
		kgsl-smmu@3da0000 {
			compatible = "qcom,qsmmu-v500";
			reg = <0x3da0000 0x20000>;
		};
		qcom,kgsl-3d0@3d00000 {
			compatible = "qcom,kgsl-3d0\\0qcom,kgsl-3d";
			reg = <0x3d00000 0x40000 0x3d61000 0x800 0x3de0000 0x10000 0x3d9e000 0x1000>;
			reg-names = "kgsl_3d0_reg_memory\\0cx_dbgc\\0rscc\\0cx_misc";
		};
		qcom,gmu@3d6a000 {
			compatible = "qcom,gpu-gmu";
			reg = <0x3d6a000 0x34000 0xb290000 0x10000>;
			reg-names = "kgsl_gmu_reg\\0kgsl_gmu_pdc_cfg";
		};
		aop_cmd_db@80860000 {
			compatible = "qcom,cmd-db";
			reg = <0x00 0x80860000 0x00 0x20000>;
		};
		rsc@18200000 {
			compatible = "qcom,rpmh-rsc";
			reg = <0x18200000 0x10000 0x18210000 0x10000 0x18220000 0x10000>;
			reg-names = "drv-0\\0drv-1\\0drv-2";
			qcom,tcs-offset = <0xd00>;
			qcom,drv-id = <0x02>;
		};
		rsc@af20000 {
			compatible = "qcom,rpmh-rsc";
			reg = <0xaf20000 0x10000>;
			reg-names = "drv-0";
			qcom,tcs-offset = <0x1c00>;
			qcom,drv-id = <0x00>;
		};
		spmi_bus: qcom,spmi@c440000 {
			compatible = "qcom,spmi-pmic-arb";
			reg = <0xc440000 0x1100 0xc600000 0x2000000 0xe600000 0x100000 0xe700000 0xa0000 0xc40a000 0x26000>;
			reg-names = "core\\0chnls\\0obsrvr\\0intr\\0cnfg";
			qcom,ee = <0x00>;
			qcom,pmk8350@0 {
				compatible = "qcom,spmi-pmic";
				reg = <0x00 0x00>;
				pon_hlos@1300 {
					compatible = "qcom,qpnp-power-on";
					reg = <0x1300 0x800>;
				};
				pmk8350_rtc: rtc@6100 {
					compatible = "qcom,pmk8350-rtc";
					reg = <0x6100 0x6200>;
				};
			};
			qcom,pm8350@1 {
				compatible = "qcom,spmi-pmic";
				reg = <0x01 0x00>;
				pinctrl@8800 {
					compatible = "qcom,pm8350-gpio";
					reg = <0x8800>;
					phandle = <0x33>;
				};
			};
		};
		gpio_keys {
			compatible = "gpio-keys";
			vol_up {
				label = "volume_up";
				gpios = <0x33 0x06 0x01>;
				linux,code = <0x73>;
			};
			camera_snapshot {
				gpios = <0x33 0x07 0x01>;
				linux,code = <0x2fe>;
			};
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
			reg = <0x00 0xae94000 0x00 0x400 0x00 0xaf08000 0x00 0x04 0x00 0xae6b800 0x00 0x300>; /* a comment */
			reg-names = "dsi_ctrl\\0disp_cc_base\\0mdp_intf_base";
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


GPU = '''/ {
	gpu {
		model = "Adreno 642";
		chip-id = <0x06060010>;
		firmware = "fw-phone";
		gmem-kb = <1536>;
		primitive-fifo = <0x00300000>;
		hang-cycles = <0x3ffff>;
		speed-fuse = <0x1e1>;
		gaming-fuse = <0x1ed>;
		highest-bank-bit = <14>;
		gcc-debug-gpucc = <0x13c>;
		speeds-khz = <285000 490000>;
		speed-levels = <64 144>;
		default-speed = <2>;
	};
};
'''


GPU_SOC = '''	adreno {
		gpu = <0x3d00000>;
		gmu = <0x3d6a000>;
		gpucc = <0x3d90000>;
		cx-misc = <0x3d9e000>;
		rscc = <0x3de0000>;
		smmu = <0x3da0000>;
		pdc = <0xb290000>;
		qfprom = <0x780000>;
	};
	rpmh {
		cmd-db = <0x80860000>;
		cmd-db-size = <0x20000>;
		rsc = <0x18220000>;
		tcs-offset = <0xd00>;
	};
'''


class Sandbox(unittest.TestCase):
    """A repository of its own for each test: one SoC, one board, the real kernel header."""

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.saved = (boards.ROOT, boards.DEVICES)
        boards.ROOT, boards.DEVICES = self.dir, self.dir / 'devices'
        (self.dir / 'devices/phone').mkdir(parents=True)
        (self.dir / 'devices/phone/soc.dtsi').write_text(SOC)
        (self.dir / 'devices/phone/board.dts').write_text(BOARD)

    def tearDown(self):
        boards.ROOT, boards.DEVICES = self.saved
        del boards.MORE_DEVICES[:]
        shutil.rmtree(self.dir)

    def problems(self):
        found = boards.Problems()
        boards.load(found)
        return '\n'.join(found)

    def board(self, old, new):
        path = self.dir / 'devices/phone/board.dts'
        text = path.read_text()
        self.assertIn(old, text)
        path.write_text(text.replace(old, new))

    def soc(self, old, new):
        path = self.dir / 'devices/phone/soc.dtsi'
        text = path.read_text()
        self.assertIn(old, text)
        path.write_text(text.replace(old, new))


    def scmi_thermal(self):
        self.soc('\tcpufreq {\n\t\tepss = <0x18591000>;\n\t\tepss-domain-size = <0x1000>;\n\t\tdomain-cores = <4 3 1>;\n\t};\n',
                 '\tscmi-perf {\n\t\tshmem = <0x81f08100>;\n\t\tmailbox = <0x19980000>;\n\t\tdoorbell = <0x104>;\n\t};\n')
        self.board('"cpufreq-epss"', '"cpufreq-scmi"')
        self.board('\t\tfloor-mhz = <1000 1100 1100>;\n', '')


class Check(Sandbox):
    def test_empty_thermal_needs_sensors_and_a_frequency_backend(self):
        self.scmi_thermal()
        self.assertEqual(self.problems(), '')
        self.soc('scmi-perf {\n\t\tshmem = <0x81f08100>;\n\t\tmailbox = <0x19980000>;\n\t\tdoorbell = <0x104>;\n\t};', '')
        self.assertIn('thermal { } needs cpufreq { } or scmi-perf { }', self.problems())
        self.soc('thermal {\n\t\ttsens = <0xc263000 0xc265000>;\n\t};', '')
        self.assertIn('thermal { } needs thermal { }', self.problems())

    def test_the_frequency_block_holds_together(self):
        self.soc('\t\tepss-domain-size = <0x1000>;\n', '\t\tblock = "osm";\n\t\tepss-domain-size = <0x800>;\n')
        self.assertIn('epss-domain-size 0x800 is smaller than a group\'s registers of the OSM layout reach (0x924)',
                      self.problems())
        self.soc('\t\tepss-domain-size = <0x800>;\n', '\t\tepss-domain-size = <0x1400>;\n')
        self.assertEqual(self.problems(), '')
        self.soc('\t\tepss-domain-size = <0x1400>;\n', '\t\tepss-domain-size = <0x1400>;\n\t\tdomains = <0x18591000 0x18593800>;\n')
        self.assertIn('domains gives each group\'s block: 3 addresses, none 0 (it has 2)', self.problems())
        self.soc('domains = <0x18591000 0x18593800>;', 'domains = <0x18590000 0x18593800 0x18595800>;')
        self.assertIn('domains begins at 0x18590000, epss is 0x18591000', self.problems())
        self.soc('domains = <0x18590000 0x18593800 0x18595800>;', 'domains = <0x18591000 0x18593800 0x18595800>;')
        self.assertEqual(self.problems(), '')
        self.soc('block = "osm";', 'block = "osm2";')
        self.assertIn('osm2', self.problems())
        # what the schema refuses is said, not a crash of the checks after it
        self.soc('\t\tepss-domain-size = <0x1400>;\n', '')
        self.assertIn('epss-domain-size', self.problems())
        self.soc('domains = <0x18591000 0x18593800 0x18595800>;', 'domains = "here";')
        self.assertIn('domains', self.problems())

    def test_bluetooth_dt_references_are_not_guessed(self):
        tree = '''/ {
            tlmm: pinctrl@f000000 { compatible = "qcom,test-tlmm"; gpio-controller; };
            pmic@3 { compatible = "qcom,spmi-pmic"; reg = <3 0>;
                gpio: pinctrl@8800 { gpio-controller; }; };
            wrapper@8c0000 { uart@898000 { compatible = "qcom,geni-uart"; reg = <0x898000 0x4000>;
                bluetooth { compatible = "qcom,wcn7750-bt";
                    qcom,bt-reset-gpio = <&gpio 3 0>; qcom,bt-sw-ctrl-gpio = <&tlmm 18 0>; };
            }; };
        };'''
        _, board, _ = devicetree.from_dts(tree)
        self.assertEqual(board['bluetooth'], {'serial_engine': 0x898000, 'wrapper': 0x8c0000, 'enable_ppid': 0x38a, 'sw_ctrl': 18})
        _, board, _ = devicetree.from_dts(tree.replace('"qcom,geni-uart"', '"other-uart"'))
        self.assertNotIn('serial_engine', board['bluetooth'])

    def test_bluetooth_wiring_is_validated_and_generated(self):
        fixture = boards.boardfile.parse((ROOT / 'workbench/devices/phone-video/board.dts').read_text(), boards.BOARD_SCHEMA)['bluetooth']
        problems = boards.Problems()
        boards.bluetooth_problems('bluetooth', fixture, problems)
        self.assertFalse(problems)
        for key, value in (('uart_pins', [24, 24, 26, 27]), ('votes', [1, 2]),
                           ('clock_source', fixture['clocks'][2]), ('supply_mv', [1800]),
                           ('supplies', ['long_resource_name']), ('wrapper', 1), ('enable_ppid', 0x301)):
            bad = dict(fixture, **{key: value})
            problems = boards.Problems()
            boards.bluetooth_problems('bluetooth', bad, problems)
            self.assertTrue(problems, key)
        path = self.dir / 'devices/phone/board.dts'
        original = path.read_text()
        text = boards.show_text({'bluetooth': fixture})
        path.write_text(original + '/ {\n' + text + '};\n')
        self.assertEqual(self.problems(), '')
        table = boards.table_text(*boards.load())
        self.assertIn('.clock_source = 0x1e77c,', table)
        self.assertIn('.supply_count = 6,', table)
        self.assertIn('.rf_clock = "C1A_E0",', table)
        path.write_text(original.replace('"cpufreq-epss"', '"bluetooth-wcn-uart"'))
        self.assertIn('feature "bluetooth-wcn-uart" needs bluetooth { }', self.problems())

    def test_numbers_read_as_dtc_reads_them(self):
        self.assertEqual(boards.boardfile.numbers('<010 0x10 16 0 0X1f>', 1), [8, 16, 16, 0, 31])
        for bad in ('<08>', '<0x>', '<1a>'):
            with self.assertRaises(boards.boardfile.FileError, msg=bad):
                boards.boardfile.numbers(bad, 4)

    def test_a_complete_board_is_accepted(self):
        self.assertEqual(self.problems(), '')

    def test_refusals_name_the_file_and_the_reason(self):
        cases = [
            (self.board, 'soc = "sm0001";', 'soc = "sm9999";', "soc = 'sm9999': its soc.dtsi is 'sm0001'"),
            (self.board, '"cpufreq-epss"', '"warp-drive"', 'feature "warp-drive" is not one the kernel knows'),
            (self.board, 'name = "phone";', 'name = "other";', 'the directory is devices/phone'),
            (self.board, 'max = <2047>;', 'max = <1>;', 'min must be below max'),
            (self.board, 'max = <2047>;\n', '', 'type "dcs" needs "max"'),
            (self.board, 'max = <2047>;', 'max = <70000>;', 'max does not fit in 2 byte'),
            (self.board, 'byte-order = "msb-first";\n', '', 'two bytes need "byte-order"'),
            (self.board, 'panel-mode = "command";', 'panel-mode = "sideways";', 'one of command, video'),
            (self.board, 'min = <1>;', 'min = "one";', 'int expected'),
            (self.board, 'min = <1>;', 'min = <1>;\n\t\tbrightness = <5>;', 'unknown key "brightness"'),
            (self.board, '\tthermal {', '\tthermals {', 'unknown section thermals { }'),
            (self.board, 'floor-mhz = <1000 1100 1100>;', 'floor-mhz = <1000>;', 'one value for each of the 3 groups'),
            (self.board, 'floor-mhz = <1000 1100 1100>;\n', '', "floor-mhz: needed with the cores' frequency block"),
            (self.soc, 'tsens = <0xc263000 0xc265000>;', 'tsens = <' + ' '.join(['0xc263000'] * 9) + '>;',
             'at most 8 TSENS controllers'),
            (self.soc, 'tsens = <0xc263000 0xc265000>;', 'tsens = <' + ' '.join(['0xc263000'] * 8) + '>;', None),
            (self.board, 'description = "A phone";\n', '', '"description" is missing'),
            (self.board, 'soc = "sm0001";', 'soc = "sm0001', 'line 4: '),
            (self.soc, '\tdisplay {\n\t\tdsi0 = <0xae94000>;\n\t};\n', '', 'backlight type "dcs" needs display { }'),
            (self.soc, '\tcpufreq {\n\t\tepss = <0x18591000>;\n\t\tepss-domain-size = <0x1000>;\n\t\tdomain-cores = <4 3 1>;\n\t};\n', '', 'feature "cpufreq-epss" needs cpufreq { }'),
            (self.soc, 'gcc = <0x100000>;', 'gcc = /bits/ 64 <0x100000000>;', 'a value of 32 bits'),
            (self.board, '"cpufreq-epss"', '"cpufreq-epss", "ddr-rpmh"', 'feature "ddr-rpmh" needs rpmh { } in soc.dtsi'),
            (self.soc, 'name = "sm0001";', 'name = "sm0002";', "its soc.dtsi is 'sm0002'"),
            (self.board, '\tloader {', '\tboot {\n\t\tfastboot-product = "phone";\n\t\tcpus = <8>;\n\t\tbootargs = "cpus=8", "two words";\n\t};\n\tloader {',
             '"two words" is not NAME or NAME=VALUE'),
            (self.board, '\tloader {', '\tboot {\n\t\tfastboot-product = "phone";\n\t};\n\tloader {', '"bootargs" is missing'),
            (self.board, '\tloader {', '\tboot {\n\t\tfastboot-product = "phone";\n\t\tcpus = <8>;\n\t\tbootargs = "gpu=0", "cpus=8", "gpu=1";\n\t};\n\tloader {',
             'bootargs: "gpu" is there twice'),
            (self.soc, 'io-base = <0x400000>;', 'io-base = <0>;', 'io-base must not be 0'),
            (self.board, '"cpufreq-epss"', '"cpufreq-epss", "display-scan"', 'feature "display-scan" needs intf1'),
            (self.board, '"cpufreq-epss";\n\tbacklight {\n\t\ttype = "dcs";\n\t\tpanel-mode = "command";',
             '"display-scan";\n\tbacklight {\n\t\ttype = "dcs";\n\t\tpanel-mode = "video";', 'needs a command-mode panel'),
            (self.soc, 'domain-cores = <4 3 1>;', 'domain-cores = <4 3>;', 'domain-cores adds up to 7 cores, platform { } has 8'),
            (self.soc, 'domain-cores = <4 3 1>;', 'domain-cores = <4 2 2>;\n', None),
            (self.soc, 'domain-cores = <4 3 1>;', 'domain-cores = <2 2 2 1 1>;', 'one to four groups'),
            (self.board, 'floor-mhz = <1000 1100 1100>;', 'floor-mhz = <1000 1100>;', 'one value for each of the 3 groups'),
            (self.soc, 'psci = "smc";', 'psci = "telepathy";', 'one of smc, hvc'),
            (self.board, '"cpufreq-epss"', '"cpufreq-epss", "virtio"', 'feature "virtio" is an emulator'),
            (self.board, '\tloader {', '\twifi {\n\t\tradio-model = <2>;\n\t};\n\tloader {', 'radio-model: 0 (a WCN6750'),
            (self.board, '\tloader {', '\twifi {\n\t\tradio-model = <1>;\n\t\tearly-pool-mb = <80>;\n\t};\n\tloader {', None),
            (self.board, '\tloader {', '\twifi {\n\t\tradio-model = <0>;\n\t\tpins = <84 85 86>;\n\t\tsupplies = "smpb7", "smpb1", "smpb8";\n\t};\n\tloader {', None),
            (self.board, '\tloader {', '\twifi {\n\t\tradio-model = <0>;\n\t\tpins = <84 85 85>;\n\t};\n\tloader {', 'pins are three different pins'),
            (self.board, '\tloader {', '\twifi {\n\t\tradio-model = <0>;\n\t\tsupplies = "smpb7", "S1B";\n\t};\n\tloader {', 'supplies are three different names'),
            (self.soc, 'gic-dist = <0x17a00000>;\n', '', '"gic-dist" is missing'),
            (self.soc, 'cores = <8>;', 'cores = <9>;', 'cores is 9: one to eight'),
            (self.soc, 'cores = <8>;', 'cores = <0>;', 'cores is 0: one to eight'),
        ]
        for change, old, new, reason in cases:
            with self.subTest(reason=reason):
                saved = [(p, p.read_text()) for p in (self.dir / 'devices/phone/board.dts', self.dir / 'devices/phone/soc.dtsi')]
                try:
                    change(old, new)
                    said = self.problems()
                    if reason is None:              # (a change that is fine)
                        self.assertEqual(said, '')
                        continue
                    self.assertIn(reason, said)
                    self.assertTrue('devices/phone/board.dts' in said or 'devices/phone/soc.dtsi' in said, said)
                finally:
                    for path, text in saved:
                        path.write_text(text)

    def test_keys_need_the_arbiter_and_their_peripherals(self):
        path = self.dir / 'devices/phone/board.dts'
        self.board('"cpufreq-epss"', '"cpufreq-epss", "buttons-pmic"')
        said = self.problems()
        self.assertIn('feature "buttons-pmic" needs spmi { } in soc.dtsi', said)
        self.assertIn('feature "buttons-pmic" needs buttons { } in the board file', said)
        self.soc('\tthermal {', '\tspmi {\n\t\tcore = <0xc440000>;\n\t\tcnfg = <0xc40a000>;\n\t\tobserver = <0xe600000>;\n\t};\n\tthermal {')
        listed = path.read_text()
        for section, reason in (
                ('/ {\n\tbuttons {\n\t\tpon = <0x013>;\n\t\tvolume-up = <0x18d>;\n\t};\n};\n', None),
                ('/ {\n\tbuttons {\n\t\tpon = <0x013>;\n\t\tvolume-up = <0x18d>;\n\t\tvolume-up-name = "GPIO6 of the PM8350";\n\t};\n};\n', None),
                ('/ {\n\tbuttons {\n\t\tpon = <0x013>;\n\t\tvolume-up = <0x8d00>;\n\t};\n};\n', 'SPMI slave << 8'),
                ('/ {\n\tbuttons {\n\t\tpon = <0x100>;\n\t\tvolume-up = <0x18d>;\n\t};\n};\n', 'SPMI slave << 8'),
                ('/ {\n\tbuttons {\n\t\tpon = <0x18d>;\n\t\tvolume-up = <0x18d>;\n\t};\n};\n', 'two peripherals'),
                ('/ {\n\tbuttons {\n\t\tpon = <0x013>;\n\t};\n};\n', '"volume-up" is missing')):
            with self.subTest(section=section):
                path.write_text(listed + section)
                if reason is None:
                    self.assertEqual(self.problems(), '')
                else:
                    self.assertIn(reason, self.problems())
        path.write_text(listed + '/ {\n\tbuttons {\n\t\tpon = <0x013>;\n\t\tvolume-up = <0x18d>;\n\t};\n};\n')
        text = boards.table_text(*boards.load())
        self.assertIn('.spmi = { .core = 0x0c440000, .cnfg = 0x0c40a000, .observer = 0x0e600000, .channels = 0x00000000, '
                      '.ee = 0 },', text)
        # a driver that writes to a PMIC needs to be told where writes go as well
        path.write_text(path.read_text().replace('"buttons-pmic"', '"buttons-pmic", "torch-pm8350c"') +
                        '/ {\n\ttorch {\n\t\tflash = <0x2ee>;\n\t\talarm = <0x20a>;\n\t};\n};\n')
        self.assertIn('feature "torch-pm8350c" needs channels in spmi { } of soc.dtsi', self.problems())
        self.soc('observer = <0xe600000>;', 'observer = <0xe600000>;\n\t\tchannels = <0xc600000>;')
        self.assertEqual(self.problems(), '')
        self.assertIn('.channels = 0x0c600000,', boards.table_text(*boards.load()))
        self.assertIn('.buttons = { .pon = 0x013, .volume_up = 0x18d },', text)

    def test_a_version_7_arbiters_bus_is_the_soc_files(self):
        self.soc('\tthermal {', '\tspmi {\n\t\tcore = <0xc440000>;\n\t\tcnfg = <0xc40a000>;\n\t\tobserver = <0xe600000>;\n'
                                '\t\tbus = <1>;\n\t};\n\tthermal {')
        self.assertEqual(self.problems(), '')
        self.assertIn('.observer = 0x0e600000, .channels = 0x00000000, .ee = 0, .bus = 1 },', boards.table_text(*boards.load()))
        # the first bus is what a file that says nothing has, and the table says nothing of it either
        self.soc('bus = <1>;', 'bus = <0>;')
        self.assertEqual(self.problems(), '')
        self.assertIn('.observer = 0x0e600000, .channels = 0x00000000, .ee = 0 },', boards.table_text(*boards.load()))
        self.soc('bus = <0>;', 'bus = <2>;')
        self.assertIn('bus is 2: a version 7 arbiter has the buses 0 and 1', self.problems())
        # (the second bus of a version 8 arbiter, whose tables are apart, is nothing the drivers know)
        self.soc('bus = <2>;', 'bus = <1>;\n\t\tchannel-map = <0xc3f0000>;')
        self.assertIn('bus = 1 is for a version 7 arbiter, channel-map for a version 8 one', self.problems())

    def test_a_pmic_clock_is_described(self):
        # optional: a board without it has none in the table, and its driver reads nothing
        self.assertNotIn('.rtc =', boards.table_text(*boards.load()))
        path = self.dir / 'devices/phone/board.dts'
        listed = path.read_text()
        path.write_text(listed + '/ {\n\trtc {\n\t\tperipheral = <0x061>;\n\t};\n};\n')
        self.assertIn('rtc { } needs spmi { } in soc.dtsi', self.problems())
        self.soc('\tthermal {', '\tspmi {\n\t\tcore = <0xc440000>;\n\t\tcnfg = <0xc40a000>;\n\t\tobserver = <0xe600000>;\n\t};\n\tthermal {')
        for section, reason in (
                ('/ {\n\trtc {\n\t\tperipheral = <0x061>;\n\t};\n};\n', None),
                ('/ {\n\trtc {\n\t\tperipheral = <0x6100>;\n\t};\n};\n', 'SPMI slave << 8'),
                ('/ {\n\trtc {\n\t\tperipheral = <0x100>;\n\t};\n};\n', 'SPMI slave << 8'),
                ('/ {\n\trtc {\n\t\tname = "the PMK8350\'s RTC";\n\t};\n};\n', '"peripheral" is missing')):
            with self.subTest(section=section):
                path.write_text(listed + section)
                if reason is None:
                    self.assertEqual(self.problems(), '')
                else:
                    self.assertIn(reason, self.problems())
        path.write_text(listed + '/ {\n\trtc {\n\t\tperipheral = <0x061>;\n\t\tname = "the PMK8350\'s RTC";\n\t};\n};\n')
        self.assertIn('.rtc = { .peripheral = 0x061, .name = "the PMK8350\'s RTC" },', boards.table_text(*boards.load()))

    def test_an_audio_dsp_is_described(self):
        path = self.dir / 'devices/phone/board.dts'
        self.board('"cpufreq-epss"', '"cpufreq-epss", "adsp"')
        self.assertIn('feature "adsp" needs adsp { } in the board file', self.problems())
        # ... and RPMh, which its supplies are asked of, and the way to the DSP
        self.assertIn('feature "adsp" needs rpmh { } in soc.dtsi', self.problems())
        self.assertIn('feature "adsp" needs ipc { } in soc.dtsi', self.problems())
        self.soc('\tthermal {', '\tipc {\n\t\taoss = <0xc300000>;\n\t\tipcc = <0x408000>;\n\t\tsmem = <0x80900000>;\n\t\tsmem-size = <0x200000>;\n\t\tsmem-lock = <0x1f43000>;\n\t};\n\tthermal {')
        self.soc('\tthermal {', '\trpmh {\n\t\tcmd-db = <0x80860000>;\n\t\tcmd-db-size = <0x20000>;\n\t\trsc = <0x18220000>;\n\t\ttcs-offset = <0xd00>;\n\t};\n\tthermal {')
        listed = path.read_text()
        for section, reason in (
                ('/ {\n\tadsp {\n\t\tfirmware = "fw-audio-phone";\n\t\tmemory = <0x86700000>;\n\t\tmemory-size = <0x3000000>;\n\t};\n};\n', None),
                ('/ {\n\tadsp {\n\t\tfirmware = "fw-audio-phone";\n\t\tmemory = <0x86700000>;\n\t\tmemory-size = <0x3000000>;\n\t\tcharge-counter-divisor = <100>;\n\t};\n};\n', None),
                ('/ {\n\tadsp {\n\t\tfirmware = "fw-audio-phone";\n\t\tmemory = <0x86700000>;\n\t\tmemory-size = <0x3000000>;\n\t\tcharge-counter-divisor = <0>;\n\t};\n};\n', 'must not be 0'),
                ('/ {\n\tadsp {\n\t\tfirmware = "fw-audio-phone";\n\t\tmemory = <0x86700000>;\n\t\tmemory-size = <0x3000001>;\n\t};\n};\n', 'whole pages'),
                ('/ {\n\tadsp {\n\t\tfirmware = "/fw/../etc";\n\t\tmemory = <0x86700000>;\n\t\tmemory-size = <0x3000000>;\n\t};\n};\n', 'without slashes'),
                ('/ {\n\tadsp {\n\t\tfirmware = "fw-audio-phone";\n\t\tmemory = <0x86700123>;\n\t\tmemory-size = <0x3000000>;\n\t};\n};\n', 'on a page'),
                ('/ {\n\tadsp {\n\t\tfirmware = "fw-audio-phone";\n\t\tmemory-size = <0x3000000>;\n\t};\n};\n', '"memory" is missing'),
                ('/ {\n\tadsp {\n\t\tmemory = <0x86700000>;\n\t\tmemory-size = <0x3000000>;\n\t};\n};\n', '"firmware" is missing')):
            with self.subTest(section=section):
                path.write_text(listed + section)
                if reason is None:
                    self.assertEqual(self.problems(), '')
                else:
                    self.assertIn(reason, self.problems())
        path.write_text(listed + '/ {\n\tadsp {\n\t\tfirmware = "fw-audio-phone";\n\t\tmemory = <0x86700000>;\n\t\tmemory-size = <0x3000000>;\n\t};\n};\n')
        self.assertIn('.adsp = { .firmware = "fw-audio-phone", .memory = 0x86700000, .memory_size = 0x03000000, '
                      '.charge_counter_divisor = 1 },',
                      boards.table_text(*boards.load()))

    def test_a_gpu_is_described_in_full(self):
        path = self.dir / 'devices/phone/board.dts'
        plain = path.read_text()
        self.board('"cpufreq-epss"', '"cpufreq-epss", "gpu-a660"')
        said = self.problems()
        self.assertIn('feature "gpu-a660" needs gpu { } in the board file', said)
        # ... and of its SoC, where the GPU's blocks are and the command database
        self.assertIn('feature "gpu-a660" needs adreno { } in soc.dtsi', said)
        self.assertIn('feature "gpu-a660" needs rpmh { } in soc.dtsi', said)
        self.soc('\tthermal {', GPU_SOC + '\tthermal {')
        with_gpu = path.read_text() + GPU
        cases = [
            ('', '', None),
            ('speed-levels = <64 144>;', 'speed-levels = <64>;', 'one supply level for each of speeds-khz'),
            ('speeds-khz = <285000 490000>;', 'speeds-khz = <490000 285000>;', 'from the slowest up'),
            ('speeds-khz = <285000 490000>;', 'speeds-khz = <>;', 'one to 5 speeds'),
            ('default-speed = <2>;', 'default-speed = <3>;', 'which of speeds-khz, 1 to 2'),
            ('default-speed = <2>;', 'default-speed = <0>;', 'which of speeds-khz, 1 to 2'),
            ('chip-id = <0x06060010>;', 'chip-id = <0>;', 'chip-id must not be 0'),
            ('chip-id = <0x06060010>;\n', '', '"chip-id" is missing'),
            ('default-speed = <2>;', 'default-speed = <2>;\n\t\talt-model = "Adreno 643";', 'alt-model goes with alt-bins'),
            ('default-speed = <2>;', 'default-speed = <2>;\n\t\talt-model = "Adreno 643";\n\t\talt-bins = <1 2 3>;', 'one or two speed bins'),
            ('default-speed = <2>;', 'default-speed = <2>;\n\t\talt-model = "Adreno 643";\n\t\talt-bins = <0xac>;', None),
            ('default-speed = <2>;', 'default-speed = <2>;\n\t\tzap-must-authenticate = "yes";', 'bool expected'),
        ]
        for old, new, reason in cases:
            with self.subTest(reason=reason):
                self.assertIn(old, with_gpu)
                path.write_text(with_gpu.replace(old, new))
                said = self.problems()
                if reason is None:
                    self.assertEqual(said, '')
                else:
                    self.assertIn(reason, said)
                    self.assertIn('devices/phone/board.dts', said)
        # described but not listed: the data may be there before the driver is turned on
        path.write_text(plain + GPU)
        self.assertEqual(self.problems(), '')

    def test_status_says_what_can_be_turned_on_next(self):
        def status():
            socs, found = boards.load()
            return boards.status_text('phone', found['phone'], socs['phone'])
        said = status()
        self.assertIn('phone: A phone (sm0001)', said)
        self.assertRegex(said, r'loader +known by its framebuffer')
        self.assertRegex(said, r'backlight +the panel takes MIPI DCS brightness')
        self.assertRegex(said, r'cpufreq-epss +on \(cpufreq=1\)\n')
        # (the loader reads the battery for the kernel only when the board says its firmware has the charger's protocols:
        # arch/arm64/boot/src/charger_start.c. Said, not refused: another loader may give it)
        self.assertRegex(said, r'battery-uefi +needs uefi-charger in loader \{ \} of the board file')
        self.board('fb-base = <0xe1000000>;', 'fb-base = <0xe1000000>;\n\t\tuefi-charger;')
        self.assertRegex(status(), r'battery-uefi +can be listed: what it needs is there \(then battery=1\)')
        self.board('"cpufreq-epss"', '"cpufreq-epss", "battery-uefi"')
        self.assertEqual(self.problems(), '')
        self.board('"cpufreq-epss", "battery-uefi"', '"cpufreq-epss"')
        # the PMIC's clock is a section's driver, as the brightness and the floors are: it has its line, with what it needs
        self.assertRegex(said, r'rtc +no rtc \{ \} section: the time is not read from the PMIC; needs spmi \{ \} in soc.dtsi first')
        # by the same rules as the checker: what is missing is named, file and section
        self.assertRegex(said, r'buttons-pmic +needs spmi \{ \} in soc.dtsi, buttons \{ \} in the board file')
        self.assertRegex(said, r'ddr-rpmh +needs rpmh \{ \} in soc.dtsi')
        self.assertRegex(said, r'display-scan +needs intf1 in display \{ \} and clocks \{ \} in soc.dtsi')
        self.assertRegex(said, r'mic-wcd938x +needs pins \{ \} in soc.dtsi, lpass \{ \} in soc.dtsi')
        # a part's driver whose addresses are there is still one part's: said, not offered
        self.soc('\tthermal {', '\tpins {\n\t\ttlmm = <0xf100000>;\n\t};\n\tthermal {')
        self.soc('\tthermal {', '\tlpass {\n\t\ttx-macro = <0x3220000>;\n\t\tva-macro = <0x3370000>;\n\t\ttx-swr = <0x3230000>;\n'
                              '\t\ttx-swr-hctl = <0x32a90a8>;\n\t\tlpi = <0x33c0000>;\n\t\tlpi-slew = <0x355a000>;\n\t\tswr-tx-pins = <0 1 2 14>;\n\t\tswr-tx-slew = <0 2 4 6>;\n\t};\n\tthermal {')
        self.assertRegex(status(), r'mic-wcd938x +needs mic \{ \} in the board file')        # (the pin its codec's reset is on)
        self.board('\tloader {', '\tmic {\n\t\tcodec-reset = <83>;\n\t};\n\tloader {')
        self.assertRegex(status(), r"mic-wcd938x +one part's driver")
        self.assertIn('.mic = { .wired = 1, .codec_reset = 83 },', boards.table_text(*boards.load()))
        self.assertIn('.tx_swr = 0x03230000, .tx_swr_hctl = 0x032a90a8,', boards.table_text(*boards.load()))
        self.assertIn('.swr_tx_pin_count = 4, .swr_tx_pins = { 0, 1, 2, 14 }, .swr_tx_slew = { 0, 2, 4, 6 },',
                      boards.table_text(*boards.load()))
        self.soc('<0x32a90a8>', '<0x32a90a9>')
        self.assertIn('tx-swr-hctl is a register: its address is a multiple of 4', self.problems())
        self.soc('<0x32a90a9>', '<0x32a90a8>')
        self.soc('swr-tx-slew = <0 2 4 6>;', 'swr-tx-slew = <0 2 4 5>;')
        self.assertIn('swr-tx-slew each one\'s field in the slew register', self.problems())     # (a field is two bits: an even one)
        self.soc('swr-tx-slew = <0 2 4 5>;', 'swr-tx-slew = <0 2 4 6>;')
        self.assertRegex(said, r'touch-goodix +needs pins \{ \} in soc.dtsi')
        self.soc('\tthermal {', '\tspmi {\n\t\tcore = <0xc440000>;\n\t\tcnfg = <0xc40a000>;\n\t\tobserver = <0xe600000>;\n\t};\n\tthermal {')
        self.assertRegex(status(), r'buttons-pmic +needs buttons \{ \} in the board file')
        path = self.dir / 'devices/phone/board.dts'
        path.write_text(path.read_text() + '/ {\n\tbuttons {\n\t\tpon = <0x013>;\n\t\tvolume-up = <0x18d>;\n\t};\n};\n')
        self.assertRegex(status(), r'buttons-pmic +can be listed')
        self.board('"cpufreq-epss"', '"cpufreq-epss", "buttons-pmic"')
        self.assertRegex(status(), r'buttons-pmic +on \(buttons=1\)\n')
        self.assertRegex(status(), r'rtc +no rtc \{ \} section: the time is not read from the PMIC\n')
        path.write_text(path.read_text() + '/ {\n\trtc {\n\t\tperipheral = <0x061>;\n\t};\n};\n')
        self.assertRegex(status(), r"rtc +the PMIC's clock is read once at boot \(rtc=0 leaves it alone\)")
        # every feature the kernel knows has its line
        for _, feature in boards.feature_bits():
            self.assertRegex(said, rf'(?m)^  {feature} ')

    def tool(self, *arguments):
        """tools/boards.py ARGUMENTS: (what it returned or stopped with, what it printed)."""
        said = io.StringIO()
        with contextlib.redirect_stdout(said):
            try:
                code = boards.main(list(arguments))
            except SystemExit as stop:
                code = stop.code
        return code, said.getvalue()

    def test_status_is_one_phone_s_and_not_stopped_by_another_package(self):
        # a second package that is not right yet (somebody's work in progress beside the phone's)
        (self.dir / 'devices/other').mkdir()
        (self.dir / 'devices/other/soc.dtsi').write_text(SOC)
        (self.dir / 'devices/other/board.dts').write_text(BOARD.replace('name = "phone"', 'name = "other"')
                                                          .replace('fb-base = <0xe1000000>', 'fb-base = <0xe2000000>')
                                                          .replace('\tloader {', '\twarp {\n\t\tfactor = <9>;\n\t};\n\tloader {'))
        self.assertIn('devices/other/board.dts: unknown section warp { }', self.problems())
        code, said = self.tool('status', 'phone')
        self.assertEqual(code, 0)
        self.assertIn('phone: A phone (sm0001)', said)
        self.assertRegex(said, r'cpufreq-epss +on \(cpufreq=1\)\n')
        # its own are said, and only its own
        code, said = self.tool('status', 'other')
        self.assertIn('devices/other/board.dts: unknown section warp { }', str(code))
        self.assertIn('other: its files are not right yet', str(code))
        self.assertNotIn('devices/phone/', str(code))
        self.assertEqual(said, '')
        self.assertIn('no board "nosuch"', str(self.tool('status', 'nosuch')[0]))

    def test_each_option_of_new_says_what_it_is(self):
        code, said = self.tool('new', '--help')
        self.assertEqual(code, 0)
        said = ' '.join(said.split())            # (as one line: the help is folded to the terminal's width)
        for text in ('NAME', 'lower-case letters, digits and dashes', '--soc SOC', 'copied from a', '--dts FILE',
                     'dtc -I dtb -O dts', '--panel NODE', '--description TEXT'):
            self.assertIn(text, said)

    def test_status_says_which_bluetooth_firmware_is_sent(self):
        socs, found = boards.load()
        board = dict(found['phone'], bluetooth={'rampatch': 'msbtfw11.mbn', 'nvm': 'msnv11.bin', 'firmware': 'fw-bt'})
        self.assertRegex(boards.status_text('phone', board, socs['phone']),
                         r'bluetooth firmware +msbtfw11.mbn and msnv11.bin from the small image\'s fw-bt/, sent at bt=2')
        board = dict(found['phone'], bluetooth={'rampatch': 'msbtfw11.tlv'})
        self.assertRegex(boards.status_text('phone', board, socs['phone']), r'bluetooth firmware +msbtfw11.tlv from .* fw/, sent')
        board = dict(found['phone'], bluetooth={})
        self.assertRegex(boards.status_text('phone', board, socs['phone']), r'bluetooth firmware +none named: bt=2 hands .* ROM')
        self.assertNotIn('bluetooth firmware', boards.status_text('phone', found['phone'], socs['phone']), 'no bluetooth { }: nothing said')

    def test_the_loader_tells_boards_apart_by_their_framebuffer(self):
        for name, loader, reason in (('twin', 'fb-base = <0xe1000000>;', 'is also that of'),
                                     ('emu1', '', None), ('emu2', '', 'more than one board')):
            (self.dir / 'devices' / name).mkdir()
            shutil.copy(self.dir / 'devices/phone/soc.dtsi', self.dir / 'devices' / name)
            (self.dir / 'devices' / name / 'board.dts').write_text(
                f'/ {{\n\tname = "{name}";\n\tdescription = "x";\n\tsoc = "sm0001";\n\tfeatures = <>;\n'
                f'\tbacklight {{ type = "soft"; }};\n\tloader {{ {loader} }};\n}};\n')
            if reason:
                self.assertIn(reason, self.problems())
            else:
                self.assertNotIn('emu1', self.problems())

    def test_a_soft_board_needs_nothing_of_its_soc(self):
        (self.dir / 'devices/phone/soc.dtsi').write_text('/ {\n\tname = "sm0001";\n\tdescription = "bare";\n};\n')
        (self.dir / 'devices/phone/board.dts').write_text(
            '/ {\n\tname = "phone";\n\tdescription = "A phone";\n\tsoc = "sm0001";\n\tfeatures = <>;\n'
            '\tbacklight {\n\t\ttype = "soft";\n\t};\n};\n')
        self.assertEqual(self.problems(), '')

    def test_the_template_is_not_a_board(self):
        (self.dir / 'devices/_template').mkdir()
        (self.dir / 'devices/_template/board.dts').write_text('/ {\n\tname = "NAME";\n};\n')
        socs, found = boards.load()
        self.assertEqual(list(found), ['phone'])


class Generate(Sandbox):
    def test_empty_scmi_thermal_carries_all_eight_controllers(self):
        self.scmi_thermal()
        addresses = [0xc229000 + i * 0x1000 for i in range(8)]
        self.soc('tsens = <0xc263000 0xc265000>;', 'tsens = <' + ' '.join(hex(v) for v in addresses) + '>;')
        self.assertEqual(self.problems(), '')
        text = boards.table_text(*boards.load())
        self.assertIn('.tsens = { ' + ', '.join(f'{v:#010x}' for v in addresses) + ' },', text)
        self.assertIn('.floor_mhz = { 0 },', text)
        self.board('\tthermal {\n\t};\n', '')
        self.assertNotIn('.thermal = {', boards.table_text(*boards.load()))

    def test_the_table_carries_the_values(self):
        text = boards.table_text(*boards.load())
        for expected in ('.name = "phone"', '.soc = "sm0001"', '.features = HAKIOS_HAS_CPUFREQ_EPSS,',
                         '.type = HAKIOS_BACKLIGHT_DCS, .video_mode = 0, .bytes = 2,', '.dsi = 0x0ae94000,',
                         '.raw_min = 1, .raw_max = 2047, .raw_restore = 0,', '.dispcc_mdss_gdscr = 0x3000,',
                         '.tsens = { 0x0c263000, 0x0c265000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },', '.floor_mhz = { 1000, 1100, 1100 },',
                         '.epss = 0x18591000, .epss_domain_size = 0x1000,', '.domain_cores = { 4, 3, 1 },', '"cpufreq-epss"'):
            self.assertIn(expected, text)
        self.assertNotIn('*/board.dts', text, 'a path with */ in it would close the header\'s comment')

    def test_the_table_carries_the_osm_layout(self):
        self.soc('\t\tepss = <0x18591000>;\n\t\tepss-domain-size = <0x1000>;\n',
                 '\t\tblock = "osm";\n\t\tepss = <0x18591000>;\n\t\tepss-domain-size = <0x1400>;\n'
                 '\t\tdomains = <0x18591000 0x18593800 0x18595800>;\n')
        text = boards.table_text(*boards.load())
        self.assertIn('.domains = { 0x18591000, 0x18593800, 0x18595800 },', text)
        self.assertIn('.block = HAKIOS_CPUFREQ_OSM,', text)

    def test_the_table_leaves_the_epss_defaults_unsaid(self):
        text = boards.table_text(*boards.load())
        self.assertNotIn('.block =', text)
        self.assertNotIn('.domains =', text)

    def test_the_table_carries_the_gpu(self):
        path = self.dir / 'devices/phone/board.dts'
        self.assertNotIn('.gpu = {', boards.table_text(*boards.load()))
        self.soc('\tthermal {', GPU_SOC + '\tthermal {')
        path.write_text(path.read_text().replace('"cpufreq-epss"', '"cpufreq-epss", "gpu-a660"') +
                        GPU.replace('\t};\n};', '\t\talt-model = "Adreno 643";\n\t\talt-bins = <0xac>;\n\t\tzap-must-authenticate;\n\t};\n};'))
        text = boards.table_text(*boards.load())
        for expected in ('HAKIOS_HAS_CPUFREQ_EPSS | HAKIOS_HAS_GPU_A660,', '.model = "Adreno 642", .firmware = "fw-phone",',
                         '.alt_model = "Adreno 643", .alt_bins = { 0xac, 0xac },',       # (not bin 0 in the empty place)
                         '.chip_id = 0x06060010, .gmem_bytes = 1536 * 1024,', '.uche_cmdq_config = 0x0,',
                         '.gcc_debug_gpucc = 0x13c, .zap_must_authenticate = 1,', '.speed_count = 2, .default_speed = 2,',
                         '.speeds_khz = { 285000, 490000 },', '.speed_levels = { 64, 144 },',
                         '.gpu = 0x03d00000, .gmu = 0x03d6a000, .gpucc = 0x03d90000,',
                         '.pdc = 0x0b290000, .qfprom = 0x00780000, .tsens = 0x00000000,'):
            self.assertIn(expected, text)

    def test_the_table_carries_rpmh(self):
        self.soc('\tthermal {', '\trpmh {\n\t\tcmd-db = <0x80860000>;\n\t\tcmd-db-size = <0x20000>;\n\t\trsc = <0x18220000>;\n\t\ttcs-offset = <0xd00>;\n\t};\n\tthermal {')
        self.board('"cpufreq-epss"', '"cpufreq-epss", "ddr-rpmh"')
        self.assertEqual(self.problems(), '')
        self.assertIn('.rpmh = { .cmd_db = 0x80860000, .cmd_db_size = 0x20000, .rsc = 0x18220000, .tcs_offset = 0xd00 },',
                      boards.table_text(*boards.load()))

    def test_the_partition_a_write_may_reach_is_the_boards(self):
        path = self.dir / 'devices/phone/board.dts'
        plain = path.read_text()
        own = ('/ {\n\tstorage {\n\t\ttype-guid = "7c3457ef-0000-11aa-aa11-00306543ecac";\n\t\tunique-guid = "FD6854AD-24AA-4FC3-94FE-290116858D68";\n'
               '\t\tfirst-lba = <25374720>;\n\t\tlast-lba = <30617594>;\n\t\twrite-first = <2042368>;\n\t};\n};\n')
        path.write_text(plain + own)
        self.assertEqual(self.problems(), '')
        socs, found = boards.load()
        platforms = boards.platforms_text(socs, found)
        # the GUIDs as the loader's efi_guid has them: three numbers, then the eight bytes in the order they are printed
        self.assertIn('.own = { .type = { 0x7c3457ef, 0x0000, 0x11aa, { 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac } }, '
                      '.unique = { 0xfd6854ad, 0x24aa, 0x4fc3, { 0x94, 0xfe, 0x29, 0x01, 0x16, 0x85, 0x8d, 0x68 } },', platforms)
        self.assertIn('.first_lba = 25374720ULL, .last_lba = 30617594ULL, .write_first = 2042368ULL }', platforms)
        # ... and the kernel has where it begins, how long it is and from where it may write: the same file
        self.assertIn('.storage = { .first_lba = 25374720ULL, .blocks = 5242875ULL, .write_first = 2042368ULL },',
                      boards.table_text(socs, found))
        for right, wrong, said in (
                ('type-guid = "7c3457ef-0000-11aa-aa11-00306543ecac";', 'type-guid = "7c3457ef000011aaaa1100306543ecac";',
                 'type-guid is a GUID as a partition table prints it'),
                ('unique-guid = "FD6854AD-24AA-4FC3-94FE-290116858D68";', 'unique-guid = "FD6854AD-24AA-4FC3-94FE-290116858D6G";',
                 'unique-guid is a GUID as a partition table prints it'),
                ('last-lba = <30617594>;', 'last-lba = <25374720>;', 'the first is not 0 and before the last'),
                ('first-lba = <25374720>;', 'first-lba = <0>;', 'the first is not 0 and before the last'),
                ('write-first = <2042368>;', 'write-first = <0>;', 'write-first is a block of the partition'),
                ('write-first = <2042368>;', 'write-first = <5242875>;', 'write-first is a block of the partition')):
            path.write_text(plain + own.replace(right, wrong))
            self.assertIn(said, self.problems(), wrong)
        # a board without the section has no such partition: the loader's and the kernel's rows say nothing of one
        path.write_text(plain)
        socs, found = boards.load()
        self.assertNotIn('.own', boards.platforms_text(socs, found))
        self.assertNotIn('.storage', boards.table_text(socs, found))

    def test_the_speaker_amplifiers_need_their_wiring(self):
        self.soc('\tthermal {', '\tpins {\n\t\ttlmm = <0xf100000>;\n\t};\n\tsmmu {\n\t\tapps = <0x15000000>;\n\t};\n\tthermal {')
        self.soc('\tthermal {', '\trpmh {\n\t\tcmd-db = <0x80860000>;\n\t\tcmd-db-size = <0x20000>;\n\t\trsc = <0x18220000>;\n\t\ttcs-offset = <0xd00>;\n\t};\n\tthermal {')
        self.board('"cpufreq-epss"', '"cpufreq-epss", "speaker-aw88261"')
        # the audio DSP reads the samples through the SMMU, on a stream the SoC file names
        self.assertIn('feature "speaker-aw88261" needs audio-stream in smmu { } of soc.dtsi', self.problems())
        self.soc('\t\tapps = <0x15000000>;', '\t\tapps = <0x15000000>;\n\t\taudio-stream = <0x1801 0x10000>;')
        self.assertIn('audio-stream is a stream ID and its mask', self.problems())
        self.soc('<0x1801 0x10000>', '<0x1801 0x0>')
        self.assertIn('feature "speaker-aw88261" needs speaker { } in the board file', self.problems())
        path = self.dir / 'devices/phone/board.dts'
        plain = path.read_text()
        path.write_text(plain + '/ {\n\tspeaker {\n\t\tsda = <12>;\n\t\tscl = <13>;\n\t\taddresses = <0x35 0x35>;\n\t\tinterrupts = <102 12>;\n\t\tsound-pins = <97 100 98 99>;\n\t\tsound-function = <1>;\n\t};\n};\n')
        said = self.problems()
        self.assertIn('eight different pins', said)                      # (an interrupt on the bus's data line)
        self.assertIn('different I2C addresses', said)
        path.write_text(plain + '/ {\n\tspeaker {\n\t\tsda = <12>;\n\t\tscl = <13>;\n\t\taddresses = <0x35 0x34>;\n\t\tinterrupts = <102 45>;\n\t\tsound-pins = <97 100 98 99>;\n'
                        '\t\tsound-function = <1>;\n\t\ttdm = <5>;\n\t};\n};\n')
        self.assertIn('tdm is the TDM interface', self.problems())          # (there is no sixth)
        path.write_text(plain + '/ {\n\tspeaker {\n\t\tsda = <12>;\n\t\tscl = <13>;\n\t\taddresses = <0x35>;\n\t\tinterrupts = <102 45>;\n\t\tsound-pins = <97 100 98 99>;\n\t\tsound-function = <1>;\n\t};\n};\n')
        self.assertIn('different I2C addresses', self.problems())      # (there are two amplifiers)
        path.write_text(plain + '/ {\n\tspeaker {\n\t\tsda = <12>;\n\t\tscl = <13>;\n\t\taddresses = <0x35 0x34>;\n\t\tinterrupts = <102 45>;\n\t\tsound-pins = <97 100 98 99>;\n\t\tsound-function = <1>;\n\t};\n};\n')
        self.assertEqual(self.problems(), '')
        self.assertIn('.speaker = { .sda = 12, .scl = 13, .address = { 0x35, 0x34 }, .interrupt = { 102, 45 },\n\t\t\t'
                      '.sound_pin = { 97, 100, 98, 99 }, .sound_function = 1, .tdm = 0 },',
                      boards.table_text(*boards.load()))

    def test_the_cameras_are_described_one_table_a_sensor(self):
        path = self.dir / 'devices/phone/board.dts'
        plain = path.read_text()
        wide = ('\t\tsensor@0 {\n\t\t\tname = "wide";\n\t\t\tchip = "S5KHM2SD";\n\t\t\tcci = <0 1>;\n\t\t\tpins = <71 72>;\n'
                '\t\t\treset = <21>;\n\t\t\tmclk = <1>;\n\t\t\tmclk-pin = <65>;\n\t\t\tcsiphy = <1>;\n'
                '\t\t\tvana = "gpio106 2800mV 135us";\n\t\t\tvdig = "ldo1 1000mV";\n\t\t\tfocus-motor = "dw9800";\n'
                '\t\t\tfocus-supply = "ldoc9 2800mV";\n\t\t};\n')
        front = ('\t\tsensor@1 {\n\t\t\tname = "front";\n\t\t\tchip = "OV16A1Q";\n\t\t\tcci = <0 0>;\n\t\t\tpins = <69 70>;\n'
                 '\t\t\treset = <20>;\n\t\t\tmclk = <0>;\n\t\t\tmclk-pin = <64>;\n\t\t\tcsiphy = <0>;\n\t\t\tvana = "ldo3 2800mV";\n\t\t};\n')
        head = '\t\tpmic-pins = <4 5>;\n\t\tpmic-reset = <0x28a>;\n\t\tdarwin-sensor = "wide";\n'

        def camera(*parts, head=head):
            return plain + '/ {\n\tcamera {\n' + head + ''.join(parts) + '\t};\n};\n'
        path.write_text(camera(wide, front))
        self.assertEqual(self.problems(), '')
        table = boards.table_text(*boards.load())
        for line in ('.pmic_sda = 4, .pmic_scl = 5, .pmic_reset = 0x28a, .sensor_count = 2,', '.test_sensor = 0, .darwin_sensor = 0, .darwin_front = 0xff,',
                     '{ .name = "wide", .chip = "S5KHM2SD", .cci = 0, .cci_master = 1, .sda = 71, .scl = 72, .reset = 21,',
                     '.mclk = 1, .mclk_pin = 65, .csiphy = 1, .focus_motor = 1, .upside_down = 0, .focus_supply = "ldoc9", .focus_mv = 2800,',
                     '.vana = { 2, 0, 2800, 106, 135 }, .vdig = { 1, 1, 1000, 0, 0 }, .vio = { 0, 0, 0, 0, 0 } },',
                     '.vana = { 1, 3, 2800, 0, 0 }, .vdig = { 0, 0, 0, 0, 0 }, .vio = { 0, 0, 0, 0, 0 } },'):
            self.assertIn(line, table)
        # what a sensor's node may not say, each with the reason
        for wrong, said in ((wide.replace('"S5KHM2SD"', '"IMX766"'), 'chip = \'IMX766\': one of OV02B10'),
                            (wide.replace('"ldo1 1000mV"', '"ldo1 1V"'), "vdig = 'ldo1 1V': not in the form"),
                            (wide.replace('"ldo1 1000mV"', '"ldo8 1000mV"'), "vdig = 'ldo8 1000mV': not in the form"),   # (a PM8008 has 7)
                            (wide.replace('"ldo1 1000mV"', '"ldo0 1000mV"'), "vdig = 'ldo0 1000mV': not in the form"),
                            (wide.replace('"ldoc9 2800mV"', '"L9C"'), "focus-supply = 'L9C': not in the form"),
                            (wide.replace('mclk = <1>;', 'mclk = <8>;'), 'mclk and csiphy are numbers'),
                            (wide.replace('cci = <0 1>;', 'cci = <2 1>;'), 'cci is the CCI and its master'),
                            (wide.replace('reset = <21>;', 'reset = <106>;'), 'different pins of the SoC'),    # (the supply's switch)
                            (wide.replace('"wide"', '"front"'), "no other sensor's"),
                            (wide.replace('\t\t\tname = "wide";\n', ''), '"name" is missing'),
                            (wide.replace('focus-motor = "dw9800";', 'lens = <1>;'), 'unknown key "lens"')):
            path.write_text(camera(wrong, front))
            self.assertIn(said, self.problems())
        path.write_text(camera(wide, front, head=head.replace('<4 5>', '<4 4>')))
        self.assertIn('pmic-pins are the PMIC', self.problems())
        path.write_text(camera(wide, front, head=head.replace('0x28a', '0x203')))
        self.assertIn("pmic-reset is a PMIC's GPIO", self.problems())         # (a peripheral, not a GPIO's)
        path.write_text(camera(wide, front, head=head.replace('"wide"', '"tele"')))
        self.assertIn("darwin-sensor = 'tele': the name of one of its sensors (wide, front)", self.problems())
        # Darwin's front camera, and a sensor whose pictures are upside down
        path.write_text(camera(wide, front.replace('csiphy = <0>;', 'csiphy = <0>;\n\t\t\tupside-down;'),
                               head=head + '\t\tdarwin-front = "front";\n'))
        self.assertEqual(self.problems(), '')
        table = boards.table_text(*boards.load())
        self.assertIn('.darwin_sensor = 0, .darwin_front = 1,', table)
        self.assertIn('.csiphy = 0, .focus_motor = 0, .upside_down = 1,', table)
        path.write_text(camera(wide, front, head=head + '\t\tdarwin-front = "selfie";\n'))
        self.assertIn("darwin-front = 'selfie': the name of one of its sensors (wide, front)", self.problems())
        more = [front.replace('@1', f'@{n}').replace('"front"', f'"s{n}"') for n in (2, 3, 4)]
        path.write_text(camera(wide, front, *more))
        self.assertIn('5 sensors: the driver has room for 4', self.problems())
        path.write_text(camera(front.replace('@1', '@0'), wide.replace('@0', '@2')))
        self.assertIn('the sensor@N of camera are numbered 0, 1, 2, ... in order', self.problems())
        path.write_text(camera())
        self.assertIn('"sensors" is missing: a node sensor@0 { ... } at least', self.problems())
        path.write_text(camera(head=head + '\t\tsensors = <1>;\n'))
        self.assertIn('sensors: one node sensor@0 { ... } or more', self.problems())

    def test_the_chips_a_board_may_name_are_the_camera_drivers(self):
        driver = (ROOT / 'drivers/board/camera.c').read_text()
        listed = re.findall(r'^\t\{ "(\w+)", "camera-[\w-]+\.tab"', driver, re.M)
        self.assertEqual(tuple(listed), boards.CAMERA_CHIPS)

    def test_part_drivers_need_the_pins_and_the_table_carries_them(self):
        self.board('"cpufreq-epss"', '"cpufreq-epss", "haptic-aw86927", "touch-goodix"')
        said = self.problems()
        self.assertIn('feature "haptic-aw86927" needs pins { } in soc.dtsi', said)
        self.assertIn('feature "haptic-aw86927" needs haptic { } in the board file', said)       # (which pins it is on)
        path = self.dir / 'devices/phone/board.dts'
        path.write_text(path.read_text() + '/ {\n\thaptic {\n\t\tsda = <36>;\n\t\tscl = <36>;\n\t\treset = <35>;\n\t\tinterrupt = <18>;\n\t\tf0-calibration = <0x0d>;\n\t};\n};\n')
        self.assertIn('four different pins', self.problems())
        path.write_text(path.read_text().replace('scl = <36>;', 'scl = <37>;'))
        self.assertIn('feature "touch-goodix" needs pins { } in soc.dtsi', said)
        self.assertNotIn('feature "touch-goodix" needs clocks { }', said)        # (which this SoC file has)
        self.assertIn('feature "touch-goodix" needs rpmh { } in soc.dtsi', said)  # (its supply is asked of RPMh)
        self.assertNotIn('feature "haptic-aw86927" needs rpmh { }', said)
        self.soc('\tthermal {', '\tpins {\n\t\ttlmm = <0xf100000>;\n\t};\n\tthermal {')
        self.soc('\tthermal {', '\trpmh {\n\t\tcmd-db = <0x80860000>;\n\t\tcmd-db-size = <0x20000>;\n\t\trsc = <0x18220000>;\n\t\ttcs-offset = <0xd00>;\n\t};\n\tthermal {')
        # a touch controller's driver also needs to be told how the controller is wired
        self.assertEqual(self.problems(), 'devices/phone/board.dts: feature "touch-goodix" needs touch { } in the board file')
        wiring = ('/ {\n\ttouch {\n\t\tserial-engine = <0xa94000>;\n\t\tclocks = <0x18004 0x18008 0x185fc>;\n\t\tvote-register = <0x52008>;\n\t\tvotes = <18 19 20 21 27>;\n'
                  '\t\tbus-pins = <52 53 54 55>;\n\t\tinterrupt = <81>;\n\t\treset = <105>;\n\t\tio-supply = "ldoc2";\n')
        end = '\t};\n};\n'
        plain = path.read_text()
        path.write_text(plain + wiring + end)
        self.assertIn('feature "touch-goodix" needs analog-enable and vendor-pins and coordinates in touch { } of the board file',
                      self.problems())
        path.write_text(plain + wiring.replace('bus-pins = <52 53 54 55>;', 'bus-pins = <52 53 54 81>;') +
                        '\t\tanalog-enable = <107>;\n\t\tvendor-pins = <68 96>;\n\t\tcoordinates = <10800 24000>;\n' + end)
        self.assertIn('every pin is a different pin of the SoC', self.problems())      # (the interrupt's pin on the bus)
        path.write_text(plain + wiring.replace('votes = <18 19 20 21 27>;', 'votes = <18 19 20 21 32>;') +
                        '\t\tanalog-enable = <107>;\n\t\tvendor-pins = <68 96>;\n\t\tcoordinates = <10800 24000>;\n' + end)
        self.assertIn('votes are five different bits of the vote register', self.problems())
        path.write_text(plain + wiring.replace('clocks = <0x18004 0x18008 0x185fc>;', 'clocks = <0x18004 0x18008>;') +
                        '\t\tanalog-enable = <107>;\n\t\tvendor-pins = <68 96>;\n\t\tcoordinates = <10800 24000>;\n' + end)
        self.assertIn('clocks are three different offsets', self.problems())
        path.write_text(plain + wiring + '\t\tanalog-enable = <107>;\n\t\tvendor-pins = <68 96>;\n\t\tcoordinates = <10800 0>;\n' + end)
        self.assertIn('coordinates are the largest x and y the controller reports', self.problems())
        path.write_text(plain + wiring + '\t\tanalog-enable = <107>;\n\t\tvendor-pins = <68 96>;\n\t\tcoordinates = <10800 24000>;\n' + end)
        self.assertEqual(self.problems(), '')
        table = boards.table_text(*boards.load())
        self.assertIn('.pins = { .tlmm = 0x0f100000 },', table)
        for line in ('.serial_engine = 0x00a94000,', '.m_ahb_clock = 0x18004, .s_ahb_clock = 0x18008, .se_clock = 0x185fc,',
                     '.vote_register = 0x52008, .vote = { 18, 19, 20, 21, 27 },', '.data_in = 52, .data_out = 53, .clock = 54, .chip_select = 55,',
                     '.interrupt = 81, .reset = 105,', '.analog_enable = 107, .vendor_pin = { 68, 96 },', '.io_supply = "ldoc2",',
                     '.max_x = 10800, .max_y = 24000,'):
            self.assertIn(line, table)
        # the other controller's driver needs its second supply, and no strap
        path.write_text((plain + wiring + end).replace('"touch-goodix"', '"touch-ft3658u"'))
        self.assertEqual(self.problems(), 'devices/phone/board.dts: feature "touch-ft3658u" needs analog-supply in touch { } of the board file')
        path.write_text((plain + wiring + '\t\tanalog-supply = "ldoc7";\n' + end).replace('"touch-goodix"', '"touch-ft3658u"'))
        self.assertEqual(self.problems(), '')
        self.assertIn('.analog_enable = -1, .vendor_pin = { -1, -1 },', boards.table_text(*boards.load()))

    def test_a_touch_controller_on_i2c_needs_its_wiring_and_rpmh_only_for_a_supply_it_names(self):
        self.board('"cpufreq-epss"', '"cpufreq-epss", "touch-ft5x06"')
        said = self.problems()
        self.assertIn('feature "touch-ft5x06" needs pins { } in soc.dtsi', said)
        self.assertIn('feature "touch-ft5x06" needs touch-i2c { } in the board file', said)
        self.assertNotIn('feature "touch-ft5x06" needs rpmh { }', said)
        self.soc('\tthermal {', '\tpins {\n\t\ttlmm = <0xf100000>;\n\t};\n\tthermal {')
        path = self.dir / 'devices/phone/board.dts'
        plain = path.read_text()
        good = {'sda': '<42>', 'scl': '<43>', 'interrupt': '<59>', 'reset': '<58>', 'address': '<0x38>', 'contacts': '<10>',
                'coordinates': '<1080 2340>'}

        def wired(**changed):
            keys = {**good, **{key.replace('_', '-'): value for key, value in changed.items()}}
            path.write_text(plain + '/ {\n\ttouch-i2c {\n' + ''.join(f'\t\t{key} = {value};\n' for key, value in keys.items() if value) +
                            '\t};\n};\n')
            return self.problems()

        self.assertEqual(wired(), '')
        table = boards.table_text(*boards.load())
        self.assertIn('HAKIOS_HAS_TOUCH_FT5X06', table)
        for line in ('.sda = 42, .scl = 43, .interrupt = 59, .reset = 58,', '.address = 0x38, .contacts = 10, .analog_enable = -1,',
                     '\t\t\t.analog_mv = 0,', '.max_x = 1080, .max_y = 2340,'):
            self.assertIn(line, table)
        self.assertIn('sda, scl, interrupt, reset and analog-enable are different pins of the SoC', wired(reset='<43>'))
        self.assertIn('different pins of the SoC', wired(sda='<256>'))
        self.assertIn('different pins of the SoC', wired(analog_enable='<59>'))
        self.assertIn('address is the chip\'s 7-bit I2C address', wired(address='<0x78>'))
        self.assertIn('contacts is how many fingers the chip reports at most: 1 to 10', wired(contacts='<11>'))
        self.assertIn('contacts is how many fingers', wired(contacts='<0>'))
        self.assertIn('"contacts" is missing', wired(contacts=''))
        self.assertIn('"coordinates" is missing', wired(coordinates=''))
        for coordinates in ('<1080>', '<1080 0>', '<4096 2340>', '<1080 2340 1>'):
            self.assertIn('coordinates are the largest x and y the controller reports', wired(coordinates=coordinates), coordinates)
        self.assertEqual(wired(coordinates='<4095 4095>', contacts='<1>', analog_enable='<78>'), '')
        self.assertIn('.analog_enable = 78,', boards.table_text(*boards.load()))
        # a supply it names is asked of RPMh: the SoC file must say where that is; and the analog one has its voltage
        said = wired(io_supply='"ldoa8"')
        self.assertEqual(said, 'devices/phone/board.dts: feature "touch-ft5x06" needs rpmh { } in soc.dtsi (its touch-i2c { } names a '
                         'supply, which is asked of RPMh)')
        self.soc('\tthermal {', '\trpmh {\n\t\tcmd-db = <0x80860000>;\n\t\tcmd-db-size = <0x20000>;\n\t\trsc = <0x18220000>;\n\t\ttcs-offset = <0xd00>;\n\t};\n\tthermal {')
        self.assertEqual(wired(io_supply='"ldoa8"'), '')
        self.assertIn('analog-supply and analog-millivolts go together', wired(analog_supply='"ldoa17"'))
        self.assertIn('analog-supply and analog-millivolts go together', wired(analog_millivolts='<3008>'))
        self.assertIn('2600 to 3600', wired(analog_supply='"ldoa17"', analog_millivolts='<1800>'))
        self.assertIn('2600 to 3600', wired(analog_supply='"ldoa17"', analog_millivolts='<3700>'))
        self.assertIn('two different names of RPMh regulators', wired(io_supply='"ldoa17"', analog_supply='"ldoa17"', analog_millivolts='<3008>'))
        self.assertIn('two different names of RPMh regulators', wired(io_supply='"L8A"'))
        self.assertEqual(wired(io_supply='"ldoa8"', analog_supply='"ldoa17"', analog_millivolts='<3008>'), '')
        self.assertIn('.io_supply = "ldoa8", .analog_supply = "ldoa17", .analog_mv = 3008,', boards.table_text(*boards.load()))
        # a board has one touch controller: this driver's feature beside another's is refused
        path.write_text(path.read_text().replace('"touch-ft5x06"', '"touch-ft5x06", "touch-ft3658u"'))
        self.assertIn('features "touch-ft3658u", "touch-ft5x06" are drivers of different touch controllers: a board has one', self.problems())

    def test_the_motion_sensor_needs_its_wiring_and_a_real_turn_of_the_chip(self):
        self.board('"cpufreq-epss"', '"cpufreq-epss", "imu-lsm6dso"')
        said = self.problems()
        self.assertIn('feature "imu-lsm6dso" needs pins { } in soc.dtsi', said)
        self.assertIn('feature "imu-lsm6dso" needs imu { } in the board file', said)
        self.soc('\tthermal {', '\tpins {\n\t\ttlmm = <0xf100000>;\n\t};\n\tthermal {')
        path = self.dir / 'devices/phone/board.dts'
        plain = path.read_text()
        good = {'sda': '<116>', 'scl': '<117>', 'interrupt': '<112>', 'address': '<0x6a>', 'full-scale': '<4>', 'mount': '"-y", "x", "-z"'}

        def wired(**changed):
            keys = {**good, **{key.replace('_', '-'): value for key, value in changed.items()}}
            path.write_text(plain + '/ {\n\timu {\n' + ''.join(f'\t\t{key} = {value};\n' for key, value in keys.items() if value) +
                            '\t};\n};\n')
            return self.problems()

        self.assertEqual(wired(), '')
        self.assertIn('.imu = { .sda = 116, .scl = 117, .interrupt = 112, .address = 0x6a, .full_scale = 4, '
                      '.axis = { 1, 0, 2 }, .sign = { -1, 1, -1 } },', boards.table_text(*boards.load()))
        self.assertIn('sda, scl and interrupt are three different pins of the SoC, 0 to 255', wired(interrupt='<117>'))
        self.assertIn('three different pins', wired(sda='<256>'))
        self.assertIn('address is the chip\'s 7-bit I2C address', wired(address='<0xd4>'))     # (the 8-bit form: refused)
        self.assertIn('full-scale is the accelerometer\'s range in g: one of 2, 4, 8, 16', wired(full_scale='<3>'))
        self.assertIn('"full-scale" is missing', wired(full_scale=''))
        for mount in ('"x", "x", "z"', '"x", "y"', '"x", "y", "z", "x"', '"x", "y", "w"', '"--x", "y", "z"', '"x", "-y", "z "'):
            self.assertIn('mount is Darwin\'s x, y and z as the chip\'s axes', wired(mount=mount), mount)
        self.assertEqual(wired(mount='"x", "y", "z"'), '')
        # a BMI160 instead: the same section, its own feature (and its bit in the table); never both chips for the one section
        lsm6dso = path.read_text()
        self.assertIn('HAKIOS_HAS_IMU_LSM6DSO', boards.table_text(*boards.load()))
        path.write_text(lsm6dso.replace('"imu-lsm6dso"', '"imu-bmi160"').replace('<0x6a>', '<0x68>'))
        self.assertEqual(self.problems(), '')
        table = boards.table_text(*boards.load())
        self.assertIn('HAKIOS_HAS_IMU_BMI160', table)
        self.assertNotIn('HAKIOS_HAS_IMU_LSM6DSO', table)
        self.assertIn('.address = 0x68, .full_scale = 4, .axis = { 0, 1, 2 }, .sign = { 1, 1, 1 } },', table)
        path.write_text(lsm6dso.replace('"imu-lsm6dso"', '"imu-lsm6dso", "imu-bmi160"'))
        self.assertIn('features "imu-lsm6dso" and "imu-bmi160" are two chips for the one imu { }', self.problems())
        path.write_text(plain.replace('"imu-lsm6dso"', '"imu-bmi160"'))
        self.assertIn('feature "imu-bmi160" needs imu { } in the board file', self.problems())
        self.assertEqual(wired(mount='"+z", "-x", "y"'), '')
        self.assertIn('.axis = { 2, 0, 1 }, .sign = { 1, -1, 1 } },', boards.table_text(*boards.load()))

    def test_the_table_carries_the_way_to_the_other_processors(self):
        self.soc('\tthermal {', '\tipc {\n\t\taoss = <0xc300000>;\n\t\tipcc = <0x408000>;\n\t\tsmem = <0x80900000>;\n\t\tsmem-size = <0x200000>;\n\t\tsmem-lock = <0x1f43000>;\n\t};\n\tthermal {')
        self.assertEqual(self.problems(), '')
        self.assertIn('.ipc = { .aoss = 0x0c300000, .ipcc = 0x00408000, .smem = 0x80900000, .smem_size = 0x200000, '
                      '.smem_lock = 0x01f43000 },', boards.table_text(*boards.load()))
        # all of it or none: a mailbox without the memory is no way to anybody
        self.soc('smem-lock = <0x1f43000>;\n', '')
        self.assertIn('"smem-lock" is missing', self.problems())

    def test_the_way_to_the_other_processors_is_found_in_a_vendors_tree_and_in_linuxs(self):
        # the vendor's: the lock is a slice of a syscon block, the mailbox is a "qmp-mbox"
        vendor = '''/ {
            reserved-memory { smem_mem: smem_region@80900000 { no-map; reg = <0x0 0x80900000 0x0 0x200000>; }; };
            soc {
                ipcc_mproc: qcom,ipcc@408000 { compatible = "qcom,ipcc"; reg = <0x408000 0x1000>; };
                tcsr_mutex_block: syscon@1f40000 { compatible = "syscon"; reg = <0x1f40000 0x20000>; };
                tcsr_mutex: hwlock { compatible = "qcom,tcsr-mutex"; syscon = <&tcsr_mutex_block 0 0x1000>; };
                smem: qcom,smem { compatible = "qcom,smem"; memory-region = <&smem_mem>; hwlocks = <&tcsr_mutex 3>; };
                qmp_aop: qcom,qmp-aop@c300000 { compatible = "qcom,qmp-mbox"; reg = <0xc300000 0x400>; reg-names = "msgram"; };
            }; };'''
        expected = {'aoss': 0xc300000, 'ipcc': 0x408000, 'smem': 0x80900000, 'smem_size': 0x200000, 'smem_lock': 0x1f43000}
        self.assertEqual(devicetree.from_dts(vendor)[0]['ipc'], expected)
        # the same tree compiled and dumped: the references are numbers
        dumped = '''/ {
            reserved-memory { smem_region@80900000 { reg = <0x00 0x80900000 0x00 0x200000>; phandle = <0x21>; }; };
            soc {
                qcom,ipcc@408000 { compatible = "qcom,ipcc"; reg = <0x408000 0x1000>; };
                syscon@1f40000 { compatible = "syscon"; reg = <0x1f40000 0x20000>; phandle = <0x54>; };
                hwlock { compatible = "qcom,tcsr-mutex"; syscon = <0x54 0x00 0x1000>; phandle = <0x55>; };
                qcom,smem { compatible = "qcom,smem"; memory-region = <0x21>; hwlocks = <0x55 0x03>; };
                qcom,qmp-aop@c300000 { compatible = "qcom,qmp-mbox"; reg = <0xc300000 0x400>; };
            }; };'''
        self.assertEqual(devicetree.from_dts(dumped)[0]['ipc'], expected)
        # Linux's: the lock's node has the registers, the mailbox is an "aoss-qmp"
        linux = '''/ {
            reserved-memory { smem_mem: memory@80900000 { reg = <0 0x80900000 0 0x200000>; no-map; }; };
            smem { compatible = "qcom,smem"; memory-region = <&smem_mem>; hwlocks = <&tcsr_mutex 3>; };
            soc {
                ipcc: mailbox@408000 { compatible = "qcom,sc7280-ipcc", "qcom,ipcc"; reg = <0 0x00408000 0 0x1000>; };
                tcsr_mutex: hwlock@1f40000 { compatible = "qcom,tcsr-mutex"; reg = <0 0x01f40000 0 0x20000>; };
                aoss_qmp: power-management@c300000 { compatible = "qcom,sc7280-aoss-qmp", "qcom,aoss-qmp"; reg = <0 0x0c300000 0 0x400>; };
            }; };'''
        self.assertEqual(devicetree.from_dts(linux)[0]['ipc'], expected)
        # half of it: said, and the section left out
        soc, board, notes = devicetree.from_dts('/ { soc { m@408000 { compatible = "qcom,ipcc"; reg = <0x408000 0x1000>; }; }; };')
        self.assertNotIn('ipc', soc)
        self.assertTrue(any('missing: aoss, smem, smem_size, smem_lock' in note for note in notes), notes)

    def test_the_display_clocks_are_asked_for_by_what_touches_the_display(self):
        # a SoC file with the clock controller's address alone is one a part's driver can use
        self.soc('gcc-disp-ahb-cbcr = <0x27004>;\n\t\tdispcc = <0xaf00000>;\n\t\tdispcc-mdss-gdscr = <0x3000>;\n\t\tdispcc-mdss-ahb-cbcr = <0x2080>;\n', '')
        self.assertEqual(self.problems(), 'devices/phone/board.dts: backlight type "dcs" needs gcc-disp-ahb-cbcr, dispcc, '
                         'dispcc-mdss-gdscr, dispcc-mdss-ahb-cbcr in clocks { } of soc.dtsi')
        path = self.dir / 'devices/phone/board.dts'
        path.write_text(re.sub(r'(?s)\tbacklight \{.*?\n\t\};', '\tbacklight {\n\t\ttype = "soft";\n\t};', path.read_text(), count=1))
        self.assertEqual(self.problems(), '')
        self.assertIn('.gcc = 0x00100000, .gcc_disp_ahb_cbcr = 0x0,', boards.table_text(*boards.load()))
        # ... and the GPU's driver, which switches the display's bus clock on, asks for that one
        socs, found = boards.load()
        self.assertIn('gcc_disp_ahb_cbcr in [clocks] of soc.dtsi', boards.feature_needs('gpu-a660', found['phone'], socs['phone']))
        self.assertIn('gcc_disp_ahb_cbcr, dispcc, dispcc_mdss_gdscr, dispcc_mdss_ahb_cbcr in [clocks] of soc.dtsi',
                      boards.feature_needs('display-scan', found['phone'], socs['phone']))

    def test_the_smmu_is_needed_carried_and_found(self):
        self.board('"cpufreq-epss"', '"cpufreq-epss", "camera-sm7325"')
        self.assertIn('feature "camera-sm7325" needs smmu { } in soc.dtsi', self.problems())
        self.assertIn('feature "camera-sm7325" needs camera { } in soc.dtsi', self.problems())
        # (the template's camera { }: the blocks, and where camcc has its clocks)
        blocks = re.search(r'\tcamera \{\n.*?\n\t\};\n', (ROOT / 'arch/arm64/boot/dts/_template/soc.dtsi').read_text(), re.S)[0]
        self.soc('\tthermal {', blocks + '\tthermal {')
        self.board('"cpufreq-epss", "camera-sm7325"', '"cpufreq-epss"')
        self.soc('\tthermal {', '\tsmmu {\n\t\tapps = <0x15000000>;\n\t};\n\tthermal {')
        self.assertEqual(self.problems(), '')
        table = boards.table_text(*boards.load())
        for line in ('\t\t\t.camcc = 0x0ad00000, .ife = 0x0acc4000, .csid = 0x0acc8000, .csiphy = 0x0ace0000,',
                     '\t\t\t.top_gdsc = 0xc194, .pll0 = 0x0, .pll2 = 0x2000, .slow_ahb_rcg = 0x7058,',
                     '\t\t\t.mclk_count = 4, .csiphy_count = 4,',
                     '\t\t\t.mclk_rcg = { 0xe000, 0xe01c, 0xe038, 0xe054 },',
                     '\t\t\t.csiphy_timer_branch = { 0xe0c4, 0xe0e8, 0xe10c, 0xe134 },'):
            self.assertIn(line, table)
        # one MCLK fewer than branches, or an offset that is no register's
        for old, new, said in (('<0xe000 0xe01c 0xe038 0xe054>', '<0xe000 0xe01c 0xe038>', 'one each of the MCLKs'),
                               ('<0x26028>', '<0x26029>', 'multiples of 4')):
            path = self.dir / 'devices/phone/soc.dtsi'
            text = path.read_text()
            self.soc(old, new)
            self.assertIn(said, self.problems())
            path.write_text(text)
        self.assertIn('.smmu = { .apps = 0x15000000 },', boards.table_text(*boards.load()))
        # the vendor's node is named; the GPU's own is not this one
        soc, board, notes = devicetree.from_dts('''/ { soc {
            kgsl_smmu: kgsl-smmu@3da0000 { compatible = "qcom,qsmmu-v500"; reg = <0x3da0000 0x20000>; };
            apps_smmu: apps-smmu@15000000 { compatible = "qcom,qsmmu-v500"; reg = <0x15000000 0x100000>; }; }; };''')
        self.assertEqual(soc['smmu'], {'apps': 0x15000000})
        # Linux's is the iommu that is not Adreno's
        soc, board, notes = devicetree.from_dts('''/ { soc {
            iommu@3da0000 { compatible = "qcom,sc7280-smmu-500", "qcom,adreno-smmu", "qcom,smmu-500", "arm,mmu-500"; };
            iommu@15000000 { compatible = "qcom,sc7280-smmu-500", "qcom,smmu-500", "arm,mmu-500"; }; }; };''')
        self.assertEqual(soc['smmu'], {'apps': 0x15000000})

    def test_the_audio_dsps_memory_is_found_and_offered(self):
        source = '''/ {
            reserved-memory { pil_adsp_mem: pil_adsp_region@86100000 { no-map; reg = <0x0 0x86100000 0x0 0x2100000>; };
                              pil_cdsp_mem: cdsp@88200000 { no-map; reg = <0x0 0x88200000 0x0 0x1400000>; }; };
            soc { qcom,turing@a300000 { qcom,firmware-name = "cdsp"; memory-region = <&pil_cdsp_mem>; };
                  qcom,lpass@17300000 { qcom,firmware-name = "adsp"; memory-region = <&pil_adsp_mem>; qcom,pas-id = <1>; }; }; };'''
        soc, board, notes = devicetree.from_dts(source)
        self.assertEqual(board['adsp'], {'memory': 0x86100000, 'memory_size': 0x2100000})    # the audio DSP's, not another's
        dts = self.dir / 'dsp.dts'
        dts.write_text(source)
        with contextlib.redirect_stdout(io.StringIO()):
            boards.new_board('phone4', 'sm0001', dts)
        text = (self.dir / 'devices/phone4/board.dts').read_text()
        # offered, not switched on: the section is a comment until the firmware's directory is named
        self.assertIn('\t// adsp {\n\t\t// firmware = "fw-audio-phone4";\n\t\t// memory = <0x86100000>;\n\t\t// memory-size = <0x02100000>;\n\t// };\n', text)
        self.assertEqual(self.problems(), '')

    def test_a_radio_on_the_socs_dsp_needs_the_dsps_memory(self):
        socs, found = boards.load()
        self.assertIn('dsp_memory and dsp_memory_size in [wifi] of the board file',
                      boards.feature_needs('wifi-wcn6750', found['phone'], socs['phone']))
        path = self.dir / 'devices/phone/board.dts'
        listed = path.read_text()
        path.write_text(listed + '/ {\n\twifi {\n\t\tradio-model = <0>;\n\t\tdsp-memory = <0x9ae00000>;\n\t};\n};\n')
        self.assertIn('dsp-memory and dsp-memory-size go together', self.problems())
        path.write_text(listed + '/ {\n\twifi {\n\t\tradio-model = <0>;\n\t\tdsp-memory = <0x9ae00000>;\n\t\tdsp-memory-size = <0x1900000>;\n\t};\n};\n')
        self.assertEqual(self.problems(), '')
        self.assertIn('.wifi = { .radio_model = 0, .early_pool_mb = 0, .dsp_memory = 0x9ae00000, .dsp_memory_size = 0x01900000,\n\t\t\t'
                      '.pin = { -1, -1, -1 }, .supply = { "", "", "" },\n\t\t\t.board_data = "board.bin", .regdb = "regdb.bin" },',
                      boards.table_text(*boards.load()))
        socs, found = boards.load()
        self.assertNotIn('dsp_memory and dsp_memory_size in [wifi] of the board file',
                         boards.feature_needs('wifi-wcn6750', found['phone'], socs['phone']))
        # ... and a device tree says where it is
        soc, board, notes = devicetree.from_dts('''/ {
            reserved-memory { pil_wpss_mem: wlan@9ae00000 { no-map; reg = <0x0 0x9ae00000 0x0 0x1900000>; }; };
            soc { qcom,wpss@8a00000 { qcom,firmware-name = "wpss"; memory-region = <&pil_wpss_mem>; }; }; };''')
        self.assertEqual(board['wifi'], {'dsp_memory': 0x9ae00000, 'dsp_memory_size': 0x1900000})
        # ... and which pins the radio has, where the Bluetooth node names all three
        source = '''/ {
            reserved-memory { pil_wpss_mem: wlan@9ae00000 { no-map; reg = <0x0 0x9ae00000 0x0 0x1900000>; }; };
            soc { qcom,wpss@8a00000 { qcom,firmware-name = "wpss"; memory-region = <&pil_wpss_mem>; };
                bt_wcn6750 { compatible = "qcom,wcn6750-bt"; qcom,bt-reset-gpio = <&tlmm 85 0>;
                    qcom,bt-sw-ctrl-gpio = <&tlmm 86 0>; qcom,wl-reset-gpio = <&tlmm 84 0>; }; }; };'''
        soc, board, notes = devicetree.from_dts(source)
        self.assertEqual(board['wifi'], {'dsp_memory': 0x9ae00000, 'dsp_memory_size': 0x1900000, 'pins': [84, 85, 86]})
        # (a radio on PCIe: its enable is on its own node, and there is no DSP's memory)
        soc, board, notes = devicetree.from_dts('''/ { soc { cnss { wlan-en-gpio = <&tlmm 64 0>; };
            bt { qcom,bt-reset-gpio = <&tlmm 65 0>; qcom,bt-sw-ctrl-gpio = <&tlmm 91 0>; }; }; };''')
        self.assertEqual(board['wifi'], {'pins': [64, 65, 91]})
        # (two of the three, or one pin under two names, are not the radio's pins)
        soc, board, notes = devicetree.from_dts('/ { bt { qcom,bt-reset-gpio = <&tlmm 65 0>; qcom,bt-sw-ctrl-gpio = <&tlmm 91 0>; }; };')
        self.assertNotIn('wifi', board)
        # a board started from the tree has them, in a section still to be switched on
        dts = self.dir / 'radio.dts'
        dts.write_text(source)
        with contextlib.redirect_stdout(io.StringIO()):
            boards.new_board('phone7', 'sm0001', dts)
        text = (self.dir / 'devices/phone7/board.dts').read_text()
        self.assertIn('\t// wifi {\n\t\t// radio-model = <0>;\n\t\t// dsp-memory = <0x9ae00000>;\n\t\t// dsp-memory-size = <0x01900000>;\n'
                      '\t\t// pins = <84 85 86>;\n\t// };\n', text)
        self.assertEqual(self.problems(), '')

    def test_the_microphones_blocks_are_found_and_wait_commented_out(self):
        source = '''/ { soc {
            lpi_pinctrl@33c0000 { compatible = "qcom,lpi-pinctrl"; reg = <0x33c0000 0x0>; qcom,slew-reg = <0x355a000 0x0>; };
            va-macro@3370000 { compatible = "qcom,va-macro"; reg = <0x3370000 0x0>; };
            tx-macro@3220000 { compatible = "qcom,tx-macro"; reg = <0x3220000 0x0>;
                tx_swr_master { compatible = "qcom,swr-mstr"; swrm-io-base = <0x3230000 0x0>; qcom,swrm-hctl-reg = <0x32a90a8>; }; }; }; };'''
        soc, board, notes = devicetree.from_dts(source)
        self.assertEqual(soc['lpass'], {'lpi': 0x33c0000, 'lpi_slew': 0x355a000, 'va_macro': 0x3370000, 'tx_macro': 0x3220000,
                                        'tx_swr': 0x3230000, 'tx_swr_hctl': 0x32a90a8})
        # a new SoC file has them in its lpass { }, commented out until the SoundWire pins are filled in
        dts = self.dir / 'mics.dts'
        dts.write_text(source)
        with contextlib.redirect_stdout(io.StringIO()):
            boards.new_board('phone6', 'sm0006', dts)
        text = (self.dir / 'devices/phone6/soc.dtsi').read_text()
        self.assertRegex(text, r'\t// lpass \{\n\t\t// tx-macro = <0x03220000>; +// ')
        self.assertRegex(text, r'\t\t// tx-swr-hctl = <0x032a90a8>; +// ')
        self.assertRegex(text, r'\t\t// swr-tx-pins = <0x\.\.\.>; +// TO FILL IN')
        self.assertNotIn('lpass', boards.boardfile.parse(text, boards.SOC_SCHEMA))      # (a section not yet there)

    def test_the_sounds_pins_are_found_in_a_tree(self):
        # the port's four pin states in order -- bit clock, word select, data 0, data 1 -- and data 1 the line out
        source = '''/ { soc { tlmm {
                mi2s0_sck: sck { mux { pins = "gpio97"; function = "mi2s0_sck"; }; };
                mi2s0_ws: ws { mux { pins = "gpio100"; }; };
                mi2s0_sd0: sd0 { mux { pins = "gpio98"; }; };
                mi2s0_sd1: sd1 { mux { pins = "gpio99"; }; }; };
            dai_mi2s0 { qcom,msm-mi2s-rx-lines = <2>; qcom,msm-mi2s-tx-lines = <1>;
                pinctrl-0 = <&mi2s0_sck &mi2s0_ws &mi2s0_sd0 &mi2s0_sd1>; }; }; };'''
        soc, board, notes = devicetree.from_dts(source)
        self.assertEqual(board['speaker'], {'sound_pins': [97, 100, 98, 99]})
        # the other way round, and compiled: phandles
        soc, board, notes = devicetree.from_dts('''/ { a { phandle = <0x10>; pins = "gpio5"; }; b { phandle = <0x11>; pins = "gpio6"; };
            c { phandle = <0x12>; pins = "gpio7"; }; d { phandle = <0x13>; pins = "gpio8"; };
            dai { qcom,msm-mi2s-rx-lines = <0x01>; qcom,msm-mi2s-tx-lines = <0x02>; pinctrl-0 = <0x10 0x11 0x12 0x13>; }; };''')
        self.assertEqual(board['speaker'], {'sound_pins': [5, 6, 8, 7]})
        # an overlay whose pin states are in the tree it is laid on: nothing
        soc, board, notes = devicetree.from_dts('''/ { fragment@55 { __overlay__ { qcom,msm-mi2s-rx-lines = <0x02>;
            qcom,msm-mi2s-tx-lines = <0x01>; pinctrl-0 = <0xffffffff 0xffffffff 0xffffffff 0xffffffff>; }; }; };''')
        self.assertNotIn('speaker', board)
        # a board started from the tree has them, in a section still to be completed
        dts = self.dir / 'sound.dts'
        dts.write_text(source)
        with contextlib.redirect_stdout(io.StringIO()):
            boards.new_board('phone9', 'sm0001', dts)
        self.assertIn('\t// speaker {\n\t\t// sound-pins = <97 100 98 99>;\n\t// };\n', (self.dir / 'devices/phone9/board.dts').read_text())
        self.assertEqual(self.problems(), '')

    def test_the_codecs_reset_is_found_in_a_tree(self):
        # as a vendor's overlay has it, compiled and dumped again: the codec names a node by its phandle, that
        # node's active state names the pin
        dumped = '''/ { fragment@1 { __overlay__ {
            wcd938x-codec { compatible = "qcom,wcd938x-codec"; qcom,wcd-rst-gpio-node = <0x43>; };
            msm_cdc_pinctrl@35 { compatible = "qcom,msm-cdc-pinctrl"; pinctrl-0 = <0x4a>; pinctrl-1 = <0x4b>; phandle = <0x43>; };
            wcd938x_reset_active { phandle = <0x4a>; mux { pins = "gpio132"; function = "gpio"; };
                config { pins = "gpio132"; drive-strength = <0x10>; output-high; }; };
            wcd938x_reset_sleep { phandle = <0x4b>; mux { pins = "gpio132"; }; }; }; };
            fragment@2 { __overlay__ { wcd938x-codec { qcom,wcd-rst-gpio-node = <0x00>; }; }; }; };'''
        soc, board, notes = devicetree.from_dts(dumped)
        self.assertEqual(board['mic'], {'codec_reset': 132})
        # in a source, by labels
        soc, board, notes = devicetree.from_dts('''/ { codec { qcom,wcd-rst-gpio-node = <&wcd_gpio>; };
            wcd_gpio: msm_cdc_pinctrl { pinctrl-0 = <&wcd_reset_active>; };
            wcd_reset_active: reset_active { pins = "gpio83"; }; };''')
        self.assertEqual(board['mic'], {'codec_reset': 83})
        # a node that points nowhere (the overlay's placeholder) is no pin
        soc, board, notes = devicetree.from_dts('/ { codec { qcom,wcd-rst-gpio-node = <0x00>; }; };')
        self.assertNotIn('mic', board)
        # a board started from the tree has the section
        dts = self.dir / 'codec.dts'
        dts.write_text(dumped)
        with contextlib.redirect_stdout(io.StringIO()):
            boards.new_board('phone8', 'sm0001', dts)
        self.assertIn('\tmic {\n\t\tcodec-reset = <132>;\n\t};\n', (self.dir / 'devices/phone8/board.dts').read_text())
        self.assertEqual(self.problems(), '')

    def test_the_storage_controller_is_found_and_reaches_the_loader(self):
        soc, board, notes = devicetree.from_dts('''/ { soc { ufshc_mem: ufshc@1d84000 { compatible = "qcom,ufshc";
            reg = <0x1d84000 0x3000>, <0x1d88000 0x8000>; reg-names = "ufs_mem", "ufs_ice"; }; }; };''')
        self.assertEqual(soc['platform']['ufs'], 0x1d84000)
        self.assertNotIn('.ufs = ', boards.platforms_text(*boards.load()))      # (this SoC file does not say)
        self.soc('io-size = <0x3fc00000>;\n', 'io-size = <0x3fc00000>;\n\t\tufs = <0x1d84000>;\n')
        self.assertEqual(self.problems(), '')
        self.assertIn('.ufs = 0x01d84000', boards.platforms_text(*boards.load()))

    def test_the_flashlight_needs_its_peripherals_and_a_tree_has_them(self):
        self.soc('\tthermal {', '\tspmi {\n\t\tcore = <0xc440000>;\n\t\tcnfg = <0xc40a000>;\n\t\tobserver = <0xe600000>;\n\t\tchannels = <0xc600000>;\n\t};\n\tthermal {')
        self.board('"cpufreq-epss"', '"cpufreq-epss", "torch-pm8350c"')
        self.assertIn('feature "torch-pm8350c" needs torch { } in the board file', self.problems())
        path = self.dir / 'devices/phone/board.dts'
        listed = path.read_text()
        path.write_text(listed + '/ {\n\ttorch {\n\t\tflash = <0x200>;\n\t\talarm = <0x20a>;\n\t};\n};\n')
        self.assertIn('flash = 0x200: SPMI slave << 8', self.problems())        # (a slave without a peripheral: a typo)
        path.write_text(listed + '/ {\n\ttorch {\n\t\tflash = <0x2ee>;\n\t\talarm = <0x20a>;\n\t};\n};\n')
        self.assertEqual(self.problems(), '')
        self.assertIn('.torch = { .flash = 0x2ee, .temp_alarm = 0x20a },', boards.table_text(*boards.load()))
        # the module's PMIC is found by its place in the tree; the alarm is that PMIC's, not another's
        soc, board, notes = devicetree.from_dts('''/ { soc { qcom,spmi@c440000 { compatible = "qcom,spmi-pmic-arb";
            qcom,pm8350@1 { compatible = "qcom,spmi-pmic"; reg = <1 0>;
                qcom,temp-alarm@a00 { compatible = "qcom,spmi-temp-alarm"; reg = <0xa00>; }; };
            qcom,pm8350c@3 { compatible = "qcom,spmi-pmic"; reg = <3 0>;
                qcom,temp-alarm@a00 { compatible = "qcom,spmi-temp-alarm"; reg = <0xa00>; };
                qcom,flash_led@ee00 { compatible = "qcom,pm8350c-flash-led"; reg = <0xee00>; }; }; }; }; };''')
        self.assertEqual(board['torch'], {'flash': 0x3ee, 'alarm': 0x30a})
        # ... and a board started from that tree has the section, with the feature still to be listed
        dts = self.dir / 'light.dts'
        dts.write_text('''/ { soc { qcom,spmi@c440000 { compatible = "qcom,spmi-pmic-arb";
            qcom,pm8350c@2 { compatible = "qcom,spmi-pmic"; reg = <2 0>;
                qcom,temp-alarm@a00 { compatible = "qcom,spmi-temp-alarm"; reg = <0xa00>; };
                qcom,flash_led@ee00 { compatible = "qcom,pm8350c-flash-led"; reg = <0xee00>; }; }; }; }; };''')
        with contextlib.redirect_stdout(io.StringIO()):
            boards.new_board('phone5', 'sm0001', dts)
        self.assertIn('\ttorch {\n\t\tflash = <0x2ee>;\n\t\talarm = <0x20a>;\n\t};\n', (self.dir / 'devices/phone5/board.dts').read_text())
        self.assertEqual(self.problems(), '')

    def test_the_touch_controllers_reset_and_interrupt_are_found_in_a_tree(self):
        # under its bus, as a whole tree has it: the bus's registers too
        soc, board, notes = devicetree.from_dts('''/ { soc { spi@a94000 { compatible = "qcom,spi-geni"; reg = <0xa94000 0x4000>;
            touch@0 { compatible = "goodix,9916r-spi"; reg = <0>; spi-max-frequency = <12000000>;
                goodix,reset-gpio = <&tlmm 105 0x00>; goodix,irq-gpio = <&tlmm 81 0x2800>; }; }; }; };''')
        self.assertEqual(board['touch'], {'reset': 105, 'interrupt': 81, 'serial_engine': 0xa94000})
        # in an overlay, whose target is not in the file: the two pins
        overlay = '''/ { fragment@66 { target = <0xffffffff>; __overlay__ { status = "ok";
            xiaomi_k9_touch@0 { compatible = "xiaomi,k9-spi"; reg = <0x00>; spi-max-frequency = <0x5b8d80>;
                focaltech,reset-gpio = <0xffffffff 0x1e 0x00>; focaltech,irq-gpio = <0xffffffff 0x1f 0x2008>; }; }; }; };'''
        soc, board, notes = devicetree.from_dts(overlay)
        self.assertEqual(board['touch'], {'reset': 30, 'interrupt': 31})
        # a controller on I2C that the same vendor's driver also knows is another part: not taken
        soc, board, notes = devicetree.from_dts('''/ { soc { i2c@980000 { focaltech@38 { compatible = "focaltech,fts_ts"; reg = <0x38>;
            focaltech,reset-gpio = <&tlmm 22 0>; focaltech,irq-gpio = <&tlmm 23 0x2008>; }; }; }; };''')
        self.assertNotIn('touch', board)
        # a board started from the tree has the two, in a section still to be completed and switched on
        dts = self.dir / 'touch.dts'
        dts.write_text(overlay)
        with contextlib.redirect_stdout(io.StringIO()):
            boards.new_board('phone6', 'sm0001', dts)
        text = (self.dir / 'devices/phone6/board.dts').read_text()
        self.assertIn('\t// touch {\n\t\t// interrupt = <31>;\n\t\t// reset = <30>;\n\t// };\n', text)
        self.assertEqual(self.problems(), '')

    def test_the_pins_are_found_in_a_vendors_tree_and_in_linuxs(self):
        # the vendor's node is the whole block, in which the pins begin 1 MiB in
        soc, board, notes = devicetree.from_dts('''/ { soc { tlmm: pinctrl@f000000 { compatible = "qcom,shima-pinctrl";
            reg = <0xf000000 0x1000000>; gpio-controller; }; }; };''')
        self.assertEqual(soc['pins'], {'tlmm': 0xf100000})
        # Linux's is the pins themselves, with two cells an address
        soc, board, notes = devicetree.from_dts('''/ { soc { tlmm: pinctrl@f100000 { compatible = "qcom,sc7280-pinctrl";
            reg = <0 0x0f100000 0 0x300000>; gpio-controller; }; }; };''')
        self.assertEqual(soc['pins'], {'tlmm': 0xf100000})
        # a PMIC's GPIOs are not the SoC's pins
        soc, board, notes = devicetree.from_dts('/ { pinctrl@8800 { compatible = "qcom,pm7325-gpio"; reg = <0x8800>; gpio-controller; }; };')
        self.assertNotIn('pins', soc)

    def test_the_loader_gets_its_platforms(self):
        (self.dir / 'devices/emu').mkdir()
        shutil.copy(self.dir / 'devices/phone/soc.dtsi', self.dir / 'devices/emu')
        (self.dir / 'devices/emu/board.dts').write_text(
            '/ {\n\tname = "emu";\n\tdescription = "An emulator";\n\tsoc = "sm0001";\n\tfeatures = <>;\n'
            '\tbacklight {\n\t\ttype = "soft";\n\t};\n\tloader {\n\t};\n};\n')
        text = boards.platforms_text(*boards.load())
        for expected in ('static const struct platform platform_phone = {', '.name = "phone",', '.gic_redist = 0x17a60000,',
                         '.psci_hvc = 0,', '.core_shift = 8,', '.small_cores = 0xf,', '.fb_base = 0xe1000000,',
                         'static const struct platform platform_emu = {'):
            self.assertIn(expected, text)
        # only a board with a framebuffer to be known by is in the list the loader searches
        listed = text[text.index('platforms[] = {'):]
        self.assertIn('&platform_phone,', listed)
        self.assertNotIn('&platform_emu,', listed)

    def test_user_space_gets_what_it_is_told_of_each_board(self):
        text = boards.user_boards_text(*boards.load())
        self.assertIn('\t{ "phone", 0 },', text)        # a board that says nothing of its screen: no shift
        path = self.dir / 'devices/phone/board.dts'
        path.write_text(path.read_text() + '/ {\n\tscreen {\n\t\tstatusbar-leading-shift = <24>;\n\t};\n};\n')
        self.assertEqual(self.problems(), '')
        self.assertIn('\t{ "phone", 24 },', boards.user_boards_text(*boards.load()))
        path.write_text(path.read_text().replace('<24>', '<2400>'))
        self.assertIn('some tens, not hundreds', self.problems())

    def test_it_is_the_same_every_time(self):
        self.assertEqual(boards.table_text(*boards.load()), boards.table_text(*boards.load()))


class DeviceTree(Sandbox):
    def test_the_frequency_blocks_layout_is_read(self):
        def found(node):
            soc, _, _ = devicetree.from_dts('/ {\n' + node + '\n};\n')
            return soc.get('cpufreq')
        # Linux's, an SM8250's kind: qcom,cpufreq-hw, the OSM's layout, one size apart
        self.assertEqual(found('cpufreq@18591000 { compatible = "qcom,cpufreq-hw"; '
                               'reg = <0 0x18591000 0 0x1000>, <0 0x18592000 0 0x1000>, <0 0x18593000 0 0x1000>; '
                               'reg-names = "freq-domain0", "freq-domain1", "freq-domain2"; };'),
                         {'block': 'osm', 'epss': 0x18591000, 'epss_domain_size': 0x1000})
        # not one size apart, with the chip's name before, and without reg-names: each group's address
        self.assertEqual(found('cpufreq@5591000 { compatible = "qcom,sm0000-cpufreq-hw", "qcom,cpufreq-hw"; '
                               'reg = <0 0x05591000 0 0x1400>, <0 0x05593800 0 0x1400>, <0 0x05595800 0 0x1400>; };'),
                         {'block': 'osm', 'epss': 0x05591000, 'epss_domain_size': 0x1400,
                          'domains': [0x05591000, 0x05593800, 0x05595800]})
        # EPSS, Linux's name and the vendor's
        for compatible in ('"qcom,sm0000-cpufreq-epss", "qcom,cpufreq-epss"', '"qcom,cpufreq-hw-epss"'):
            self.assertEqual(found(f'cpufreq@18591000 {{ compatible = {compatible}; reg = <0x18591000 0x1000>, '
                                   '<0x18592000 0x1000>; reg-names = "freq-domain0", "freq-domain1"; };'),
                             {'epss': 0x18591000, 'epss_domain_size': 0x1000})
        # one cell an address, as the parent says, and no reg-names
        self.assertEqual(devicetree.from_dts('/ { soc { #address-cells = <1>; #size-cells = <1>; cpufreq@18591000 { '
                                             'compatible = "qcom,cpufreq-hw"; reg = <0x18591000 0x1000>, <0x18592000 0x1000>; '
                                             '}; }; };')[0]['cpufreq'],
                         {'block': 'osm', 'epss': 0x18591000, 'epss_domain_size': 0x1000})
        # the vendor's debug node beside it is not the block
        self.assertIsNone(found('debug@18591000 { compatible = "qcom,cpufreq-hw-epss-debug"; reg = <0x18591000 0x800>; '
                                'reg-names = "domain-top"; };'))

    def test_linuxs_own_trees_are_read(self):
        # Linux's trees name the TSENS controllers' and the UFS host's regs less often than the vendor's: their first
        soc, _, _ = devicetree.from_dts('''/ { soc@0 { #address-cells = <2>; #size-cells = <2>;
            thermal-sensor@c263000 { compatible = "qcom,sm0000-tsens", "qcom,tsens-v2";
                reg = <0 0x0c263000 0 0x1ff>, <0 0x0c222000 0 0x1ff>; };
            thermal-sensor@c265000 { compatible = "qcom,sm0000-tsens", "qcom,tsens-v2";
                reg = <0 0x0c265000 0 0x1ff>, <0 0x0c223000 0 0x1ff>; };
            ufshc@1d84000 { compatible = "qcom,sm0000-ufshc", "qcom,ufshc", "jedec,ufs-2.0";
                reg = <0 0x01d84000 0 0x3000>, <0 0x01d88000 0 0x8000>; reg-names = "std", "ice"; };
        }; };''')
        self.assertEqual(soc['thermal'], {'tsens': [0xc263000, 0xc265000]})
        self.assertEqual(soc['platform']['ufs'], 0x1d84000)
        # the power-on block of a PMIC of Linux's tree, named by its compatible
        _, board, _ = devicetree.from_dts('''/ { spmi@c440000 { compatible = "qcom,spmi-pmic-arb";
            pmic@0 { compatible = "qcom,pm8998", "qcom,spmi-pmic"; reg = <0x0 0>;
                pon@800 { compatible = "qcom,pm8998-pon"; reg = <0x800>; }; }; }; };''')
        self.assertEqual(board['buttons'], {'pon': 0x008, 'pon_name': "the PM8998's power-on block"})
        # one switched off is not the keys' block
        _, board, _ = devicetree.from_dts('''/ { spmi@c440000 { compatible = "qcom,spmi-pmic-arb";
            pmic@0 { compatible = "qcom,pm8998", "qcom,spmi-pmic"; reg = <0x0 0>;
                pon@800 { compatible = "qcom,pm8998-pon"; reg = <0x800>; }; };
            pmic@2 { compatible = "qcom,pm8150b", "qcom,spmi-pmic"; reg = <0x2 0>;
                pon@800 { compatible = "qcom,pm8916-pon"; reg = <0x800>; status = "disabled"; }; }; }; };''')
        self.assertEqual(board['buttons']['pon'], 0x008)
        # the PMIC's clock: Linux's node at its RTC_RW block, or the vendor's over one node a block; one switched off is not it
        _, board, _ = devicetree.from_dts('''/ { spmi@c440000 { compatible = "qcom,spmi-pmic-arb";
            pmic@0 { compatible = "qcom,pm8150", "qcom,spmi-pmic"; reg = <0x0 0>;
                rtc@6000 { compatible = "qcom,pm8941-rtc"; reg = <0x6000>, <0x6100>; }; }; }; };''')
        self.assertEqual(board['rtc'], {'peripheral': 0x060, 'name': "the PM8150's RTC"})
        _, board, _ = devicetree.from_dts('''/ { spmi@c440000 { compatible = "qcom,spmi-pmic-arb";
            qcom,pm8150@0 { compatible = "qcom,spmi-pmic"; reg = <0x0 0>;
                qcom,pm8150_rtc { compatible = "qcom,qpnp-rtc"; qcom,qpnp-rtc-write = <0>;
                    qcom,pm8150_rtc_rw@6000 { reg = <0x6000 0x100>; };
                    qcom,pm8150_rtc_alarm@6100 { reg = <0x6100 0x100>; }; }; }; }; };''')
        self.assertEqual(board['rtc'], {'peripheral': 0x060, 'name': "the PM8150's RTC"})
        _, board, _ = devicetree.from_dts('''/ { spmi@c440000 { compatible = "qcom,spmi-pmic-arb";
            pmic@1 { compatible = "qcom,pm8350", "qcom,spmi-pmic"; reg = <0x1 0>;
                rtc@6000 { compatible = "qcom,pm8941-rtc"; reg = <0x6000>; status = "disabled"; }; };
            pmic@0 { compatible = "qcom,pmk8350", "qcom,spmi-pmic"; reg = <0x0 0>;
                rtc@6100 { compatible = "qcom,pmk8350-rtc"; reg = <0x6100>, <0x6200>; }; }; }; };''')
        self.assertEqual(board['rtc'], {'peripheral': 0x061, 'name': "the PMK8350's RTC"})
        # an RPMh sequencer whose first slot set is not an active one is said
        rsc = '''/ {{ rsc@18200000 {{ compatible = "qcom,rpmh-rsc"; reg = <0x18200000 0x10000>, <0x18210000 0x10000>,
            <0x18220000 0x10000>; reg-names = "drv-0", "drv-1", "drv-2"; qcom,drv-id = <2>; qcom,tcs-offset = <0xd00>;
            qcom,tcs-config = {}; }}; }};'''
        _, _, notes = devicetree.from_dts(rsc.format('<ACTIVE_TCS 2>, <SLEEP_TCS 3>, <WAKE_TCS 3>, <CONTROL_TCS 0>'))
        self.assertFalse([n for n in notes if 'tcs-config' in n], notes)
        _, _, notes = devicetree.from_dts(rsc.format('<0x02 0x02>, <0x00 0x03>, <0x01 0x03>, <0x03 0x00>'))
        self.assertFalse([n for n in notes if 'tcs-config' in n], 'compiled: ACTIVE_TCS is 2')
        for config in ('<SLEEP_TCS 3>, <WAKE_TCS 3>, <ACTIVE_TCS 2>', '<ACTIVE_TCS 0>, <SLEEP_TCS 3>, <WAKE_TCS 3>'):
            _, _, notes = devicetree.from_dts(rsc.format(config))
            self.assertTrue([n for n in notes if 'qcom,tcs-config, not active slots' in n], (config, notes))
        # one cell an address, and no reg-names at all
        soc, _, _ = devicetree.from_dts('''/ { soc { #address-cells = <1>; #size-cells = <1>;
            ufshc@1d84000 { compatible = "qcom,ufshc"; reg = <0x01d84000 0x3000>; }; }; };''')
        self.assertEqual(soc['platform']['ufs'], 0x1d84000)

    def test_nodes_and_properties_are_read(self):
        tree = devicetree.parse_dts(DTS)
        names = [node.name for node in tree.walk()]
        for name in ('clock-controller@af00000', 'tsens@c222000', 'qcom,mdss_dsi_first_cmd', 'timing@0'):
            self.assertIn(name, names)
        panel = next(node for node in tree.walk() if node.name == 'qcom,mdss_dsi_first_cmd')
        self.assertEqual(devicetree.cells(panel.props['qcom,mdss-dsi-bl-max-level']), [0x7ff])
        self.assertIn('qcom,mdss-dsi-bl-inverted-dbv', panel.props)
        self.assertEqual(devicetree.strings('"a\\0b", "c"'), ['a', 'b', 'c'])

    def test_what_a_board_needs_is_found(self):
        soc, board, notes = devicetree.from_dts(DTS)
        self.assertEqual(soc['display'], {'dsi0': 0xae94000, 'intf1': 0xae6b800})      # of controller 0, not 1
        self.assertEqual(soc['thermal'], {'tsens': [0xc263000, 0xc265000]})
        self.assertEqual(soc['cpufreq'], {'epss': 0x18591000, 'epss_domain_size': 0x1000, 'domain_cores': [2, 1, 1]})
        self.assertEqual(soc['clocks'], {'gcc': 0x100000, 'dispcc': 0xaf00000})
        self.assertEqual(soc['platform'], {'gic_dist': 0x17a00000, 'gic_redist': 0x17a60000, 'psci': 'smc',
                                           'cores': 4, 'core_shift': 8, 'small_cores': 0b0011})
        self.assertEqual(soc['spmi'], {'core': 0xc440000, 'cnfg': 0xc40a000, 'observer': 0xe600000, 'ee': 0,
                                       'channels': 0xc600000})
        # the GPU's blocks, each by the name of its reg entry or by its own node
        self.assertEqual(soc['adreno'], {'gpu': 0x3d00000, 'gmu': 0x3d6a000, 'gpucc': 0x3d90000, 'cx_misc': 0x3d9e000,
                                         'rscc': 0x3de0000, 'smmu': 0x3da0000, 'pdc': 0xb290000, 'qfprom': 0x780000})
        # the application processor's sequencer, not the display's
        self.assertEqual(soc['rpmh'], {'cmd_db': 0x80860000, 'cmd_db_size': 0x20000, 'rsc': 0x18220000, 'tcs_offset': 0xd00})
        # the power-on block of the PMIC that is slave 0; GPIO6 of the one that is slave 1 (0x8800 + 0x100 * 5)
        self.assertEqual(board['buttons'], {'pon': 0x013, 'pon_name': "the PMK8350's power-on block",
                                            'volume_up': 0x18d, 'volume_up_name': 'GPIO6 of the PM8350'})
        light = board['backlight']
        self.assertEqual((light['type'], light['panel_mode'], light['min'], light['max'], light['restore'], light['bytes'],
                          light['byte_order']), ('dcs', 'command', 1, 0x7ff, 0x133, 2, 'msb-first'))
        self.assertTrue(any('several panels' in note for note in notes))
        self.assertTrue(any('dispcc_mdss_gdscr' in note for note in notes))

    def test_the_keys_are_found_in_a_source_tree_too(self):
        # as a vendor writes it: the GPIO controller by its label, the PMIC's slave in its node's name only
        source = '''/ { soc { spmi_bus: qcom,spmi@c440000 {
            compatible = "qcom,spmi-pmic-arb";
            reg = <0xc440000 0x1100>, <0xc600000 0x2000000>, <0xe600000 0x100000>, <0xe700000 0xa0000>, <0xc40a000 0x26000>;
            reg-names = "core", "chnls", "obsrvr", "intr", "cnfg";
            qcom,ee = <0>;
            qcom,pm7325@1 { compatible = "qcom,spmi-pmic"; reg = <1 SPMI_USID>;
                pm7325_gpios: pinctrl@8800 { compatible = "qcom,pm7325-gpio"; reg = <0x8800>; }; };
        }; };
        gpio_keys { compatible = "gpio-keys";
            vol_up { gpios = <&pm7325_gpios 6 GPIO_ACTIVE_LOW>; linux,code = <0x73>; }; }; };'''
        soc, board, notes = devicetree.from_dts(source)
        self.assertEqual(soc['spmi'], {'core': 0xc440000, 'cnfg': 0xc40a000, 'observer': 0xe600000, 'ee': 0,
                                       'channels': 0xc600000})
        self.assertEqual(board['buttons'], {'volume_up': 0x18d, 'volume_up_name': 'GPIO6 of the PM7325'})
        # a version 7 arbiter's node is one of its two buses: which, when it is not the first
        soc, _, _ = devicetree.from_dts(source.replace('qcom,ee = <0>;', 'qcom,ee = <0>; qcom,bus-id = <1>;'))
        self.assertEqual(soc['spmi'], {'core': 0xc440000, 'cnfg': 0xc40a000, 'observer': 0xe600000, 'ee': 0,
                                       'channels': 0xc600000, 'bus': 1})
        soc, _, _ = devicetree.from_dts(source.replace('qcom,ee = <0>;', 'qcom,ee = <0>; qcom,bus-id = <0>;'))
        self.assertNotIn('bus', soc['spmi'])

    # A made-up chip's arbiter in the newer form of a tree: the node has the blocks its two buses share, a child for
    # each bus that bus's "cnfg", and the PMICs are under the buses. {0} and {1}: what is on each bus.
    BUSES = """/ {{ soc@0 {{ #address-cells = <2>; #size-cells = <2>;
        arbiter@a400000 {{ compatible = "qcom,sm0000-spmi-pmic-arb";
            reg = <0 0x0a400000 0 0x3000>, <0 0x0a500000 0 0x400000>, <0 0x0a480000 0 0x80000>;
            reg-names = "core", "chnls", "obsrvr";
            qcom,ee = <0>; qcom,channel = <0>; #address-cells = <2>; #size-cells = <2>; ranges;
            spmi_bus0: spmi@a42d000 {{ reg = <0 0x0a42d000 0 0x4000>, <0 0x0a4c0000 0 0x10000>; reg-names = "cnfg", "intr";
                #address-cells = <2>; #size-cells = <0>; {0} }};
            spmi_bus1: spmi@a432000 {{ reg = <0 0x0a432000 0 0x4000>, <0 0x0a4d0000 0 0x10000>; reg-names = "cnfg", "intr";
                #address-cells = <2>; #size-cells = <0>; {1} }};
        }}; }};
        gpio-keys {{ compatible = "gpio-keys";
            key-volume-up {{ gpios = <&pmy300_gpios 4 1>; linux,code = <0x73>; }}; }}; }};"""
    KEYS = '''pmic@1 { compatible = "qcom,pmy300", "qcom,spmi-pmic"; reg = <0x1 0>;
        pon@1300 { compatible = "qcom,pmk8350-pon"; reg = <0x1300>; };
        pmy300_gpios: gpio@8800 { compatible = "qcom,pmy300-gpio", "qcom,spmi-gpio"; reg = <0x8800>; }; };'''
    VOLUME = '''pmic@1 { compatible = "qcom,pmy300", "qcom,spmi-pmic"; reg = <0x1 0>;
        pmy300_gpios: gpio@8800 { compatible = "qcom,pmy300-gpio", "qcom,spmi-gpio"; reg = <0x8800>; }; };'''
    CLOCK = '''pmic@0 { compatible = "qcom,pmz100", "qcom,spmi-pmic"; reg = <0x0 0>;
        rtc@6100 { compatible = "qcom,pmk8350-rtc"; reg = <0x6100>, <0x6200>; }; };'''
    BUS0 = {'core': 0xa400000, 'cnfg': 0xa42d000, 'observer': 0xa480000, 'ee': 0, 'channels': 0xa500000}
    BUS1 = {'core': 0xa400000, 'cnfg': 0xa432000, 'observer': 0xa480000, 'ee': 0, 'channels': 0xa500000, 'bus': 1}

    @staticmethod
    def of_the_arbiter(notes):
        return [note for note in notes if 'arbiter' in note]

    def test_an_arbiter_with_a_node_for_each_bus_is_read(self):
        # the power key's PMIC on the second bus: that bus's cnfg and its number, and the first is said to be there
        soc, board, notes = devicetree.from_dts(self.BUSES.format('', self.KEYS))
        self.assertEqual(soc['spmi'], self.BUS1)
        self.assertEqual(board['buttons'], {'pon': 0x113, 'pon_name': "the PMY300's power-on block",
                                            'volume_up': 0x18b, 'volume_up_name': 'GPIO4 of the PMY300'})
        self.assertEqual(self.of_the_arbiter(notes), [
            "the PMIC arbiter has two SPMI buses (version 7): spmi { } is bus 1's (cnfg = <0x0a432000>), the one with the "
            "power key's PMIC. The other, bus 0 (cnfg = <0x0a42d000>), exists: the drivers reach the PMICs of one bus, so for "
            "a part on bus 0 spmi { } wants that cnfg and bus = <0> (Documentation/chipsets.md)"])
        # ... on the first: the first, without `bus', and the second is said to be there
        soc, board, notes = devicetree.from_dts(self.BUSES.format(self.KEYS, self.CLOCK))
        self.assertEqual(soc['spmi'], self.BUS0)
        self.assertIn("spmi { } is bus 0's (cnfg = <0x0a42d000>), the one with the power key's PMIC. The other, bus 1 "
                      "(cnfg = <0x0a432000>), exists", self.of_the_arbiter(notes)[0])
        # what is on the bus not taken is not put into the board file: there its address is another chip's
        self.assertNotIn('rtc', board)
        self.assertEqual(self.of_the_arbiter(notes)[1:], [
            "the PMIC's clock is on bus 1 of the PMIC arbiter, not on the bus spmi { } is of (bus 0): left out of the board "
            "file, where its address would be another chip's"])
        soc, board, notes = devicetree.from_dts(self.BUSES.format(self.CLOCK, self.KEYS))
        self.assertEqual((soc['spmi'], 'rtc' in board, len(self.of_the_arbiter(notes))), (self.BUS1, False, 2))
        # ... the flashlight and Bluetooth's enable GPIO as well, which a driver would write to
        parts = """pmic@3 { compatible = "qcom,pmw350c", "qcom,spmi-pmic"; reg = <0x3 0>;
            led-controller@ee00 { compatible = "qcom,pm8350c-flash-led"; reg = <0xee00>; };
            temp-alarm@a00 { compatible = "qcom,spmi-temp-alarm"; reg = <0xa00>; };
            pmw_gpios: gpio@8800 { compatible = "qcom,spmi-gpio"; reg = <0x8800>; }; };"""
        radio = '\n/ { bluetooth { compatible = "qcom,wcn0000-bt"; qcom,bt-reset-gpio = <&pmw_gpios 7 0>; }; };'
        soc, board, notes = devicetree.from_dts(self.BUSES.format(parts, self.KEYS) + radio)
        self.assertEqual((soc['spmi'], 'torch' in board, 'bluetooth' in board), (self.BUS1, False, False))
        self.assertEqual([note[:note.index(' is on bus')] for note in self.of_the_arbiter(notes)[1:]],
                         ['the flashlight', "Bluetooth's enable GPIO"])
        soc, board, notes = devicetree.from_dts(self.BUSES.format('', self.KEYS + parts) + radio)
        self.assertEqual((soc['spmi'], board['torch'], board['bluetooth']),
                         (self.BUS1, {'flash': 0x3ee, 'alarm': 0x30a}, {'enable_ppid': 0x38e}))
        self.assertEqual(len(self.of_the_arbiter(notes)), 1)
        # all on one bus: nothing left out
        soc, board, notes = devicetree.from_dts(self.BUSES.format('', self.KEYS + self.CLOCK))
        self.assertEqual((soc['spmi'], board['rtc']['peripheral'], len(self.of_the_arbiter(notes))), (self.BUS1, 0x061, 1))

    def test_without_a_power_key_the_bus_is_that_of_what_the_board_uses_or_the_first(self):
        # no power key in the tree: the bus that has everything else the board uses of a PMIC
        soc, board, notes = devicetree.from_dts(self.BUSES.format('', self.VOLUME + self.CLOCK))
        self.assertEqual(soc['spmi'], self.BUS1)
        self.assertEqual((board['buttons'], board['rtc']['peripheral']),
                         ({'volume_up': 0x18b, 'volume_up_name': 'GPIO4 of the PMY300'}, 0x061))
        self.assertIn("spmi { } is bus 1's (cnfg = <0x0a432000>), the one with what the board uses of a PMIC (the PMIC's "
                      "clock, volume up's GPIO). The other, bus 0 (cnfg = <0x0a42d000>), exists", self.of_the_arbiter(notes)[0])
        # ... on both: the first, and what is on the second left out
        soc, board, notes = devicetree.from_dts(self.BUSES.format(self.CLOCK, self.VOLUME))
        self.assertEqual((soc['spmi'], board['rtc']['peripheral'], 'buttons' in board), (self.BUS0, 0x061, False))
        said = self.of_the_arbiter(notes)
        self.assertIn("spmi { } is bus 0's (cnfg = <0x0a42d000>), the first: the tree has what the board uses of a PMIC on "
                      "both. The other, bus 1 (cnfg = <0x0a432000>), exists", said[0])
        self.assertIn("volume up's GPIO is on bus 1 of the PMIC arbiter", said[1])
        # nothing of a PMIC in the tree (the PMICs in a file that is not in it): the first, and why
        soc, board, notes = devicetree.from_dts(self.BUSES.format('', ''))
        self.assertEqual(soc['spmi'], self.BUS0)
        self.assertNotIn('buttons', board)
        self.assertEqual(self.of_the_arbiter(notes), [
            "the PMIC arbiter has two SPMI buses (version 7): spmi { } is bus 0's (cnfg = <0x0a42d000>), the first: the tree "
            "does not say which bus the power key's PMIC is on. The other, bus 1 (cnfg = <0x0a432000>), exists: the drivers "
            "reach the PMICs of one bus, so for a part on bus 1 spmi { } wants that cnfg and bus = <1> "
            "(Documentation/chipsets.md)"])

    def test_the_pmics_of_a_bus_are_found_where_a_source_adds_them_by_its_label(self):
        # as Linux's sources have them: the PMIC's file adds its node to the bus by the bus's label, after the chip's
        source = self.BUSES.format('', '') + '\n&spmi_bus1 {\n' + self.KEYS + '\n};\n&spmi_bus0 {\n' + self.CLOCK + '\n};\n'
        soc, board, notes = devicetree.from_dts(source)
        self.assertEqual(soc['spmi'], self.BUS1)
        self.assertEqual(board['buttons']['pon'], 0x113)
        self.assertNotIn('rtc', board)
        self.assertEqual(len(self.of_the_arbiter(notes)), 2)
        tree = devicetree.parse_dts(source)
        bus = next(node for node in tree.walk() if 'spmi_bus1' in node.labels)
        self.assertEqual([child.name for child in bus.children], ['pmic@1'])
        # a label the text has not: its nodes stay where they are written, and its properties are not the root's
        tree = devicetree.parse_dts('/ { model = "a"; };\n&elsewhere { status = "okay"; pmic@2 { reg = <2 0>; }; };\n/ { later { }; };')
        self.assertEqual(tree.props, {})
        self.assertEqual([node.name for node in tree.walk()], ['/', '/', '&elsewhere', 'pmic@2', '/', 'later'])
        self.assertEqual(tree.children[0].props, {'model': '"a"'})
        # ... and there the tree does not say which bus the power key's PMIC is on
        soc, board, notes = devicetree.from_dts(self.BUSES.format('', '') + '\n&pmics {\n' + self.KEYS + '\n};\n')
        self.assertEqual((soc['spmi'], board['buttons']['pon']), (self.BUS0, 0x113))
        self.assertIn('the first: the tree does not say which bus', self.of_the_arbiter(notes)[0])

    def test_a_bus_of_the_arbiter_that_is_not_plain_is_not_guessed(self):
        tree = self.BUSES.format('', self.KEYS)
        # one cell an address, as a vendor's tree may have it; the generic name beside the chip's
        narrow = tree.replace('<0 0x', '<0x').replace(' 0 0x', ' 0x').replace(
            '"qcom,sm0000-spmi-pmic-arb"', '"qcom,sm0000-spmi-pmic-arb", "qcom,spmi-pmic-arb"')
        self.assertIn('reg = <0x0a42d000 0x4000>, <0x0a4c0000 0x10000>;', narrow)
        self.assertEqual(devicetree.from_dts(narrow)[0]['spmi'], self.BUS1)
        # a bus switched off is not taken, whatever is under it, and is said to be off
        off = 'reg-names = "cnfg", "intr"; status = "disabled";'
        soc, board, notes = devicetree.from_dts(self.BUSES.format('', 'status = "disabled"; ' + self.KEYS))
        self.assertEqual(soc['spmi'], self.BUS0)
        self.assertIn('The other, bus 1 (cnfg = <0x0a432000>), exists and is switched off in the tree (status = "disabled")',
                      self.of_the_arbiter(notes)[0])
        self.assertNotIn('buttons', board)      # (its keys are not the first bus's)
        self.assertEqual(len(self.of_the_arbiter(notes)), 3, notes)
        soc, _, notes = devicetree.from_dts(tree.replace('reg-names = "cnfg", "intr";', off))
        self.assertNotIn('spmi', soc)
        self.assertEqual(self.of_the_arbiter(notes),
                         ['the PMIC arbiter\'s buses are all switched off in the tree (status = "disabled"): spmi { } is left out'])
        # the buses numbered by their nodes (qcom,bus-id in each) are taken by those numbers
        numbered = tree.replace('spmi_bus0: spmi@a42d000 {', 'spmi_bus0: spmi@a42d000 { qcom,bus-id = <0>;').replace(
            'spmi_bus1: spmi@a432000 {', 'spmi_bus1: spmi@a432000 { qcom,bus-id = <1>;')
        self.assertEqual(devicetree.from_dts(numbered)[0]['spmi'], self.BUS1)
        # not plain, and so left to the porter: the nodes' order against their blocks', a number in one node only,
        # the same number twice, a third bus, a bus without its block
        for odd in (tree.replace('0x0a42d000', '0x0a439000'),
                    numbered.replace(' qcom,bus-id = <0>;', ''),
                    numbered.replace('qcom,bus-id = <0>;', 'qcom,bus-id = <1>;'),
                    numbered.replace('qcom,bus-id = <1>;', 'qcom,bus-id = <2>;'),
                    tree.replace('spmi_bus0:', 'spmi@a428000 { reg = <0 0x0a428000 0 0x4000>; reg-names = "cnfg"; };\n spmi_bus0:'),
                    tree.replace('reg = <0 0x0a432000 0 0x4000>, <0 0x0a4d0000 0 0x10000>;', '')):
            soc, board, notes = devicetree.from_dts(odd)
            self.assertNotIn('spmi', soc, odd)
            self.assertEqual(len(self.of_the_arbiter(notes)), 1, notes)
            self.assertIn('which bus is which is not plain', self.of_the_arbiter(notes)[0])
            self.assertIn('spmi { } is left out; fill it in by hand (Documentation/chipsets.md)', self.of_the_arbiter(notes)[0])
            self.assertEqual(board['buttons']['pon'], 0x113)       # (the keys are still the tree's word)
        # an arbiter's node with no bus under it and no cnfg of its own is no arbiter this reads; nothing is said of it
        soc, _, notes = devicetree.from_dts(self.BUSES.format('', '').replace('reg-names = "cnfg", "intr";', ''))
        self.assertEqual(('spmi' in soc, self.of_the_arbiter(notes)), (False, []))

    def test_the_older_form_of_two_buses_is_chosen_from_alike(self):
        # a node a bus, each with all the blocks' names and its number: the bus with the power key's PMIC, whichever
        # node comes first, and the other said to be there
        def node(number, cnfg, under):
            return f"""spmi@{cnfg:x} {{ compatible = "qcom,spmi-pmic-arb";
                reg = <0xa400000 0x3000>, <0xa500000 0x400000>, <0xa480000 0x80000>, <{cnfg:#x} 0x4000>;
                reg-names = "core", "chnls", "obsrvr", "cnfg"; qcom,ee = <0>; qcom,bus-id = <{number}>; {under} }};"""
        for order in (0, 1):
            nodes = [node(0, 0xa42d000, self.CLOCK), node(1, 0xa432000, self.KEYS)]
            soc, board, notes = devicetree.from_dts('/ { soc { ' + ' '.join(nodes[::-1] if order else nodes) + ' }; };')
            self.assertEqual(soc['spmi'], self.BUS1)
            self.assertEqual((board['buttons']['pon'], 'rtc' in board), (0x113, False))
            self.assertIn("spmi { } is bus 1's (cnfg = <0x0a432000>), the one with the power key's PMIC. The other, bus 0 "
                          "(cnfg = <0x0a42d000>), exists", self.of_the_arbiter(notes)[0])
            # nothing to tell them by: the first bus, wherever its node is
            soc, _, notes = devicetree.from_dts('/ { soc { ' + ' '.join(
                (nodes[::-1] if order else nodes)).replace(self.KEYS, '').replace(self.CLOCK, '') + ' }; };')
            self.assertEqual(soc['spmi'], self.BUS0)
            self.assertIn('the first: the tree does not say which bus', self.of_the_arbiter(notes)[0])
        # one node, as before: its bus, and nothing said
        soc, _, notes = devicetree.from_dts('/ { soc { ' + node(1, 0xa432000, self.KEYS) + ' }; };')
        self.assertEqual((soc['spmi'], self.of_the_arbiter(notes)), (self.BUS1, []))

    def test_a_new_board_from_a_tree_of_two_buses_says_which_was_taken(self):
        dts = self.dir / 'phone4.dts'
        dts.write_text(self.BUSES.format(self.CLOCK, self.KEYS))
        with contextlib.redirect_stdout(io.StringIO()) as said:
            boards.new_board('phone4', 'sm0004', dts, None, 'A phone of two buses')
        text = (self.dir / 'devices/phone4/soc.dtsi').read_text()
        for line in ('core = <0x0a400000>;', 'cnfg = <0x0a432000>;', 'observer = <0x0a480000>;', 'channels = <0x0a500000>;',
                     'bus = <1>;'):
            self.assertIn(line, text)
        said = re.sub(r'\s+', ' ', said.getvalue())
        self.assertIn("note: the PMIC arbiter has two SPMI buses (version 7): spmi { } is bus 1's (cnfg = <0x0a432000>), the "
                      "one with the power key's PMIC. The other, bus 0 (cnfg = <0x0a42d000>), exists", said)
        self.assertIn("note: the PMIC's clock is on bus 0 of the PMIC arbiter", said)
        board = (self.dir / 'devices/phone4/board.dts').read_text()
        self.assertIn('pon = <0x113>;', board)
        self.assertNotIn('rtc {', board)
        self.assertIn('.bus = 1 },', boards.table_text(*boards.load()))
        # compared with the same tree, which bus the tree's values are of is said again
        tree_soc, _, notes = boards.tree_to_compare(dts.read_text())
        self.assertEqual(tree_soc['spmi'], self.BUS1)
        self.assertEqual([note[:44] for note in notes],
                         ['the PMIC arbiter has two SPMI buses (version', "the PMIC's clock is on bus 0 of the PMIC arb"])

    def test_a_gpu_that_is_only_half_in_the_tree_is_left_to_be_filled_in(self):
        soc, board, notes = devicetree.from_dts('''/ { soc {
            qcom,kgsl-3d0@3d00000 { reg = <0x3d00000 0x40000>; reg-names = "kgsl_3d0_reg_memory"; };
            clock-controller@3d90000 { compatible = "qcom,shima-gpucc"; reg = <0x3d90000 0x9000>; }; }; };''')
        self.assertNotIn('adreno', soc)
        self.assertTrue(any('missing: gmu, cx_misc, rscc, smmu, pdc, qfprom' in note for note in notes), notes)
        # ... and a tree without a GPU node says nothing of it
        soc, board, notes = devicetree.from_dts('/ { soc { clock-controller@3d90000 { compatible = "qcom,x-gpucc"; }; }; };')
        self.assertFalse(any('GPU' in note for note in notes), notes)

    def test_the_smmu_streams_and_the_touch_range_are_found(self):
        parts = """
            qcom,msm-audio-ion { compatible = "qcom,msm-audio-ion"; iommus = <SMMU0x1801 0x00>; };
            remoteproc-wpss@8a00000 { qcom,firmware-name = "wpss"; iommus = <SMMU0x1c00 0x01>; };
            msm_cam_smmu_ife { compatible = "qcom,msm-cam-smmu-cb"; iommus = <SMMU0x800 0x4e0>, <SMMU0x820 0x4e0>; };
            spi@a94000 { compatible = "qcom,spi-geni"; reg = <0xa94000 0x4000>;
                touch@0 { spi-max-frequency = <0x5f5e10>; goodix,reset-gpio = <&tlmm 105 0>; goodix,irq-gpio = <&tlmm 81 0x2008>;
                    goodix,panel-max-x = <10800>; goodix,panel-max-y = <24000>; }; }; };"""
        # a vendor's source, and the same compiled (where the SMMU is a number that comes first)
        for smmu, head in (('&apps_smmu ', '/ { apps_smmu: apps-smmu@15000000 { compatible = "qcom,qsmmu-v500"; reg = <0x15000000 0x100000>; };'),
                           ('0x05 ', '/ { apps-smmu@15000000 { compatible = "qcom,qsmmu-v500"; reg = <0x15000000 0x100000>; };')):
            soc, board, notes = devicetree.from_dts(head + parts.replace('SMMU', smmu))
            self.assertEqual(soc['smmu'], {'apps': 0x15000000, 'audio_stream': [0x1801, 0], 'wifi_stream': [0x1c00, 1],
                                           'camera_stream': [0x800, 0x4e0]}, smmu)
            self.assertEqual(board['touch']['coordinates'], [10800, 24000])
        soc, board, notes = devicetree.from_dts("""/ { spi@a94000 { compatible = "qcom,spi-geni"; reg = <0xa94000 0x4000>;
            touch@0 { spi-max-frequency = <1>; focaltech,reset-gpio = <&tlmm 105 0>; focaltech,irq-gpio = <&tlmm 81 0>;
                focaltech,display-coords = <0 0 10800 24000>; }; }; };""")
        self.assertEqual(board['touch']['coordinates'], [10800, 24000])

    def test_a_key_that_is_not_on_a_pmic_is_left_to_be_filled_in(self):
        soc, board, notes = devicetree.from_dts('''/ { tlmm: pinctrl@f000000 { phandle = <0x10>; };
            gpio_keys { compatible = "gpio-keys"; vol_up { gpios = <0x10 0x20 0x01>; linux,code = <0x73>; }; }; };''')
        self.assertNotIn('buttons', board)
        self.assertTrue(any('volume_up is to be filled in by hand' in note for note in notes))

    def test_a_panel_without_dcs_brightness_is_dimmed_in_software(self):
        soc, board, notes = devicetree.from_dts(DTS, panel='qcom,mdss_dsi_second_video')
        self.assertEqual(board['backlight']['type'], 'soft')
        self.assertTrue(any('bl_ctrl_wled' in note for note in notes))

    def test_a_board_is_compared_with_a_device_tree(self):
        def compared(panel='qcom,mdss_dsi_first_cmd'):
            socs, found = boards.load()
            tree_soc, tree_board, notes = devicetree.from_dts(DTS, panel)
            return boards.compare_text('phone', found['phone'], socs['phone'], tree_soc, tree_board, 'phone.dts')
        text, differ = compared()
        self.assertEqual(differ, 3, text)       # this SoC file's cores and their groups are not the tree's
        self.assertRegex(text, r'clocks \{ \} gcc +<0x00100000> +as in the tree')
        self.assertRegex(text, r'clocks \{ \} gcc-disp-ahb-cbcr +<0x00027004> +not in the tree')
        self.assertRegex(text, r'cpufreq \{ \} domain-cores +<4 3 1> +THE TREE SAYS <2 1 1>')
        self.assertRegex(text, r'platform \{ \} cores +<8> +THE TREE SAYS <4>')
        self.assertRegex(text, r'backlight \{ \} max +<2047> +as in the tree')
        # a section the tree knows nothing of is one line; one the file lacks is offered with its values
        self.assertRegex(text, r'thermal \{ \} +1 value, none of which the tree says\n')
        self.assertIn('the tree also has, of spmi { }: core = <0x0c440000>, cnfg = <0x0c40a000>, observer = <0x0e600000>, '
                      'ee = <0>, channels = <0x0c600000>', text)
        self.assertIn('the tree also has, of buttons { }: pon = <0x013>,', text)
        self.assertIn('the tree also has, of backlight { }: restore = <307>', text)
        self.assertRegex(text, r'\d+ values as in the tree, 3 differ, \d+ the tree does not say')
        # another panel of the same tree is another answer
        text, differ = compared('qcom,mdss_dsi_second_video')
        self.assertRegex(text, r'backlight \{ \} type +"dcs" +THE TREE SAYS "soft"')
        # a panel's node that does not say how its light is controlled is not taken to say "soft"
        tree_soc, tree_board, notes = boards.tree_to_compare(
            '/ { qcom,mdss_dsi_half { qcom,mdss-dsi-panel-type = "dsi_video_mode"; qcom,mdss-dsi-bl-max-level = <4095>; }; };')
        self.assertNotIn('backlight', tree_board)
        self.assertTrue(any('[backlight] is not compared' in note for note in notes), notes)
        # ... and with several panels, which one was taken is said
        self.assertTrue(any('several panels' in note for note in boards.tree_to_compare(DTS)[2]))

    def test_a_tree_that_cannot_be_read_is_said_to_be_and_nothing_is_started(self):
        def refused(*arguments, **more):
            with self.assertRaises(SystemExit) as stop, contextlib.redirect_stdout(io.StringIO()):
                boards.new_board(*arguments, **more)
            return str(stop.exception.code)
        # the file is not there: its name and why, not a traceback
        said = refused('phone2', 'sm0002', self.dir / 'nowhere.dts')
        self.assertIn('nowhere.dts: No such file or directory', said)
        # the tree as the phone gives it (/sys/firmware/fdt), not its source: a board with nothing in it was written
        compiled = self.dir / 'fdt.dtb'
        compiled.write_bytes(b'\xd0\x0d\xfe\xed' + bytes(60))
        said = refused('phone2', 'sm0002', compiled)
        self.assertIn('is a compiled device tree', said)
        self.assertIn(f'dtc -I dtb -O dts {compiled} > phone.dts', said)
        self.assertFalse((self.dir / 'devices/phone2').exists())
        with self.assertRaises(SystemExit) as stop:
            boards.tree_source(compiled)                 # (compare reads its tree the same way)
        self.assertIn('compiled device tree', str(stop.exception.code))
        # which of the two names is not one, and what it could be
        self.assertIn('"SM0002": the SoC (--soc) is named in lower-case letters, digits and dashes ("sm0002")',
                      refused('phone2', 'SM0002'))
        self.assertIn('"Phone_2": the board is named in lower-case letters, digits and dashes ("phone-2")',
                      refused('Phone_2', 'sm0002'))
        self.assertEqual(sorted(p.name for p in (self.dir / 'devices').iterdir()), ['phone'])

    def test_a_package_that_is_moved_away_is_not_said_to_be_here(self):
        # tools/device.py init makes the package here and takes it to the phone's own repository: where it was on the
        # way, and that `check` is next here, would send the porter to files that are gone
        with contextlib.redirect_stdout(io.StringIO()) as said:
            boards.new_board('phone2', 'sm0002', None, None, 'Another phone', moved=True)
        self.assertEqual(said.getvalue(), '')
        self.assertTrue((self.dir / 'devices/phone2/board.dts').is_file())

    def test_a_new_board_is_started_from_a_device_tree(self):
        dts = self.dir / 'phone2.dts'
        dts.write_text(DTS)
        with contextlib.redirect_stdout(io.StringIO()) as said:
            boards.new_board('phone2', 'sm0002', dts, 'qcom,mdss_dsi_first_cmd', 'Another phone')
        self.assertIn('wrote devices/phone2/board.dts', said.getvalue())
        self.assertFalse((self.dir / 'devices/phone2/README.md').exists())      # (this repository of the test has no template)
        (self.dir / 'devices/_template').mkdir()
        (self.dir / 'devices/_template/README.md').write_text((ROOT / 'arch/arm64/boot/dts/_template/README.md').read_text())
        with contextlib.redirect_stdout(io.StringIO()) as again:
            boards.new_board('phone3', 'sm0002', dts, 'qcom,mdss_dsi_first_cmd', 'A third phone')
        readme = (self.dir / 'devices/phone3/README.md').read_text()
        self.assertIn('wrote devices/phone3/README.md', again.getvalue())
        self.assertTrue(readme.startswith('# A third phone (`phone3`)'), readme[:60])
        self.assertIn('(sm0002); `soc.dtsi`', readme)
        self.assertIn('tools/boards.py status phone3', readme)
        self.assertNotIn('NAME', readme)
        shutil.rmtree(self.dir / 'devices/phone3')
        shutil.rmtree(self.dir / 'devices/_template')
        self.assertIn('wrote devices/phone2/soc.dtsi', said.getvalue())
        self.assertIn('wrote devices/phone3/soc.dtsi: a copy of devices/phone2/soc.dtsi', again.getvalue())
        # as written it is refused for exactly what a device tree cannot say, and accepted once that is filled in
        problems = self.problems()
        self.assertIn('backlight type "dcs" needs gcc-disp-ahb-cbcr, dispcc-mdss-gdscr, dispcc-mdss-ahb-cbcr in clocks { } of '
                      'soc.dtsi', problems)
        self.assertEqual([line for line in problems.splitlines() if 'in clocks { } of' not in line], [])
        path = self.dir / 'devices/phone2/soc.dtsi'
        text = path.read_text()
        for key, value in (('gcc_disp_ahb_cbcr', '0x27004'), ('dispcc_mdss_gdscr', '0x3000'), ('dispcc_mdss_ahb_cbcr', '0x2080')):
            name = key.replace('_', '-')
            self.assertIn(f'// {name} = <0x...>;', text)
            text = text.replace(f'// {name} = <0x...>;', f'{name} = <{value}>;')     # (what is said after it stays a comment)
        path.write_text(text)
        self.assertEqual(self.problems(), '')
        # the I/O window, which no tree has either, is asked for when the loader is to know the board
        board = self.dir / 'devices/phone2/board.dts'
        board.write_text(board.read_text() + '/ {\n\tloader {\n\t\tfb-base = <0x9c000000>;\n\t};\n};\n')
        self.assertEqual(self.problems(), 'devices/phone2/board.dts: loader { } needs io-base and io-size in platform { } of '
                         'soc.dtsi')
        text = text.replace('// io-base = <0x00400000>;', 'io-base = <0x00400000>;').replace('// io-size = <0x3fc00000>;', 'io-size = <0x3fc00000>;')
        path.write_text(text)
        self.assertEqual(self.problems(), '')
        self.assertIn('.raw_min = 1, .raw_max = 2047, .raw_restore = 307,', boards.table_text(*boards.load()))
        # the file explains itself: each section and value with what the template says of it, addresses
        # in eight digits, offsets in hex, counts as a person counts
        for line in ('\t// The PMIC arbiter (qcom,spmi-pmic-arb), by its reg-names: for the features "buttons-pmic",',
                     'core = <0x0c440000>;',
                     'tcs-offset = <0xd00>;',
                     'tsens = <0x0c263000 0x0c265000>;',
                     'cores = <4>;', 'small-cores = <0x3>;',
                     '// TO FILL IN: gcc_disp_ahb_clk: its .halt_reg'):
            self.assertIn(line, text)
        self.assertEqual([line for line in text.splitlines() if line != line.rstrip()], [])
        # the keys were found: listing the feature is all that is left to do for them
        board = self.dir / 'devices/phone2/board.dts'
        self.assertIn('\tbuttons {\n\t\tpon = <0x013>;\n\t\tpon-name = "the PMK8350\'s power-on block";\n\t\tvolume-up = <0x18d>;\n'
                      '\t\tvolume-up-name = "GPIO6 of the PM8350";\n\t};\n', board.read_text())
        # ... and the PMIC's clock, which needs no feature
        self.assertIn('\trtc {\n\t\tperipheral = <0x061>;\n\t\tname = "the PMK8350\'s RTC";\n\t};\n', board.read_text())
        board.write_text(board.read_text().replace('features = <>;', 'features = "buttons-pmic";'))
        self.assertEqual(self.problems(), '')
        self.assertIn('.buttons = { .pon = 0x013, .volume_up = 0x18d, .pon_name = "the PMK8350\'s power-on block", '
                      '.volume_up_name = "GPIO6 of the PM8350" },', boards.table_text(*boards.load()))

    def test_an_existing_board_is_not_overwritten(self):
        with self.assertRaises(SystemExit):
            boards.new_board('phone', 'sm0001')


class Format(unittest.TestCase):
    """The files' syntax (tools/boardfile.py): a device tree source's, a small part of it."""

    def parse(self, text):
        return boards.boardfile.parse(text, boards.BOARD_SCHEMA)

    def refused(self, text, line, said):
        with self.assertRaises(boards.boardfile.FileError) as caught:
            self.parse(text)
        self.assertEqual(caught.exception.line, line, str(caught.exception))
        self.assertIn(said, str(caught.exception))

    def test_values_come_out_as_their_keys_want_them(self):
        data = self.parse('/dts-v1/;\n/* a board */\n/ {\n\tname = "x"; // its name\n\tfeatures = "a", "b";\n'
                          '\tgpu { gmem-kb = <1536>; speeds-khz = <285000>; zap-must-authenticate; };\n'
                          '\tloader { fb-base = /bits/ 64 <0x1c0000000>; };\n\tthermal { floor-mhz = <1000 1100>; };\n};\n')
        self.assertEqual(data['name'], 'x')
        self.assertEqual(data['features'], ['a', 'b'])
        self.assertEqual(data['gpu'], {'gmem_kb': 1536, 'speeds_khz': [285000], 'zap_must_authenticate': True})  # (one speed: a list)
        self.assertEqual(data['loader'], {'fb_base': 0x1c0000000})
        self.assertEqual(data['thermal'], {'floor_mhz': [1000, 1100]})
        # the root may be opened again lower in the file, as dtc allows
        self.assertEqual(self.parse('/ { name = "x"; };\n/ { mic { codec-reset = <83>; }; };\n'),
                         {'name': 'x', 'mic': {'codec_reset': 83}})

    def test_what_is_not_this_syntax_is_said_with_its_line(self):
        self.refused('/ {\n\tname = "x"\n\tsoc = "y";\n};\n', 3, '"," or ";" expected')
        self.refused('/ {\n\tname = "x";\n', 3, 'a property or a node expected')
        self.refused('name = "x";\n', 1, 'the root node')
        self.refused('/ {\n\tlabel: mic { codec-reset = <83>; };\n};\n', 2, 'is not of this format')
        self.refused('/ {\n\tmic { codec-reset = <&tlmm 83>; };\n};\n', 2, 'only numbers in <>')
        self.refused('/ {\n\tmic { codec-reset = "a", <83>; };\n};\n', 2, 'strings and numbers in one property')
        self.refused('/ {\n\tcamera { sensor@0 { x { }; }; };\n};\n', 2, 'two deep at most')
        self.refused('/ {\n\tname = "x";\n\tname = "y";\n};\n', 3, 'given twice')
        self.refused('/ {\n\tcamera { sensor@1 { }; };\n};\n', 2, 'numbered 0, 1, 2')
        self.refused('/ {\n\tmic { codec-reset = <83>; }\n};\n', 3, '"};"')

    def test_a_file_of_before_is_converted_with_its_comments(self):
        toml = ('# A phone.\nname = "x"                # its name\nfeatures = ["a", "b"]\n\n# Its GPU.\n[gpu]\n'
                'gmem_kb = 1536\nzap_must_authenticate = true   # the firmware has none\n#   going on\n'
                '[[camera.sensors]]\nname = "wide"\n')
        text = boards.boardfile.from_toml(toml.replace('[[camera.sensors]]\nname = "wide"\n', ''))
        self.assertIn('// A phone.\n/ {\n\tname = "x";', text)
        self.assertIn('\t// Its GPU.\n\tgpu {\n\t\tgmem-kb = <1536>;\n\t\tzap-must-authenticate;', text)
        self.assertIn('// the firmware has none\n', text)
        self.assertEqual(self.parse(text), {'name': 'x', 'features': ['a', 'b'], 'gpu': {'gmem_kb': 1536, 'zap_must_authenticate': True}})
        text = boards.boardfile.from_toml('name = "x"\n[camera]\npmic_pins = [4, 5]\n[[camera.sensors]]\nname = "wide"\n'
                                          '[[camera.sensors]]\nname = "front"\n')
        self.assertEqual(self.parse(text)['camera'], {'pmic_pins': [4, 5], 'sensors': [{'name': 'wide'}, {'name': 'front'}]})

    def test_the_repositorys_files_are_this_syntax(self):
        for path in list((ROOT / 'devices').glob('*/soc.dtsi')) + list((ROOT / 'devices').glob('*/board.dts')):
            schema = boards.SOC_SCHEMA if path.suffix == '.dtsi' else boards.BOARD_SCHEMA
            boards.boardfile.parse(path.read_text(), schema)
        self.assertEqual(list((ROOT / 'devices').glob('*/*.toml')), [])


class Packages(Sandbox):
    """Device packages kept somewhere else than devices/, and the tables written for what is there."""

    def package(self, directory, name, soc='sm0001'):
        (directory / name).mkdir(parents=True)
        (directory / name / 'soc.dtsi').write_text(f'/ {{\n\tname = "{soc}";\n\tdescription = "A chip";\n}};\n')
        (directory / name / 'board.dts').write_text(
            f'/ {{\n\tname = "{name}";\n\tdescription = "A package";\n\tsoc = "{soc}";\n\tfeatures = <>;\n'
            f'\tbacklight {{\n\t\ttype = "soft";\n\t}};\n}};\n')

    def test_packages_are_found_in_the_other_directories_too(self):
        elsewhere = self.dir / 'elsewhere'
        self.package(elsewhere, 'other')
        self.assertEqual(sorted(boards.load()[1]), ['phone'])
        boards.MORE_DEVICES.append(elsewhere)
        self.assertEqual(self.problems(), '')
        self.assertEqual(sorted(boards.load()[1]), ['other', 'phone'])
        self.assertIn('.name = "other"', boards.table_text(*boards.load()))

    def test_a_package_of_the_repository_comes_before_one_of_the_same_name_elsewhere(self):
        elsewhere = self.dir / 'elsewhere'
        self.package(elsewhere, 'phone')
        boards.MORE_DEVICES.append(elsewhere)
        self.assertEqual(boards.load()[1]['phone']['description'], 'A phone')

    def test_what_is_wrong_with_a_package_elsewhere_is_said_with_its_path(self):
        elsewhere = self.dir.parent / (self.dir.name + '-elsewhere')
        try:
            self.package(elsewhere, 'other')
            (elsewhere / 'other/soc.dtsi').unlink()
            boards.MORE_DEVICES.append(elsewhere)
            self.assertIn(f'{elsewhere}/other/board.dts: there is no soc.dtsi beside it', self.problems())
        finally:
            shutil.rmtree(elsewhere)

    def test_each_package_has_its_own_soc_file(self):
        # two packages of one chip, each with its copy: a section added to one is that board's alone
        self.package(self.dir / 'devices', 'second')
        self.assertEqual(self.problems(), '')
        socs, found = boards.load()
        self.assertIn('platform', socs['phone'])
        self.assertNotIn('platform', socs['second'])

    def test_the_tables_are_written_once_and_left_alone_while_nothing_changes(self):
        out = self.dir / 'out'
        socs, found = boards.load()
        self.assertEqual(sorted(boards.write_generated(out, socs, found)),
                         ['board_table.h', 'platforms.h', 'user_boards.h'])
        stamp = (out / 'board_table.h').stat().st_mtime_ns
        self.assertEqual(boards.write_generated(out, socs, found), [])
        self.assertEqual((out / 'board_table.h').stat().st_mtime_ns, stamp)   # nothing to recompile for
        # a board that says something new of its screen: user space's table, and only it
        path = self.dir / 'devices/phone/board.dts'
        path.write_text(path.read_text() + '/ {\n\tscreen {\n\t\tstatusbar-leading-shift = <24>;\n\t};\n};\n')
        self.assertEqual(boards.write_generated(out, *boards.load()), ['user_boards.h'])


class Repository(unittest.TestCase):
    def tearDown(self):
        del boards.MORE_DEVICES[:]

    def test_the_boards_of_the_repository_are_valid(self):
        problems = boards.Problems()
        socs, found = boards.load(problems)
        self.assertEqual(list(problems), [])
        self.assertIn('qemu-virt', found)

    def test_a_boot_tool_is_told_which_phone_and_with_what_arguments(self):
        plain = {'boot': {'fastboot_product': 'a', 'cpus': 8, 'bootargs': ['gpu=0', 'cpufreq=1', '-v']}}
        found = {'emulator': {}, 'alpha': plain}
        # the only phone here is the one, named or not; another name is said not to be here
        self.assertEqual(boards.board_to_boot(found)[0], 'alpha')
        self.assertEqual(boards.board_to_boot(found, 'alpha'), ('alpha', plain))
        with self.assertRaises(SystemExit) as refused:
            boards.board_to_boot(found, 'beta')
        self.assertIn('tools/device.py add beta', str(refused.exception))
        with self.assertRaises(SystemExit) as refused:
            boards.board_to_boot(found, 'emulator')         # (a board without [boot] is not one to boot)
        self.assertIn('no board "emulator" with a boot { } section', str(refused.exception))
        # two phones: the tool does not choose
        with self.assertRaises(SystemExit) as refused:
            boards.board_to_boot({**found, 'beta': plain})
        self.assertIn('which phone? --board NAME (here: alpha, beta)', str(refused.exception))
        with self.assertRaises(SystemExit) as refused:
            boards.board_to_boot({'emulator': {}})
        self.assertIn('no package with a boot { } section', str(refused.exception))
        # the board's arguments, one replaced in its place, new ones after them
        self.assertEqual(boards.boot_arguments(plain), ['gpu=0', 'cpufreq=1', '-v'])
        self.assertEqual(boards.boot_arguments(plain, ['gpu=1', 'cpus=1', 'thermal=2', 'cpus=2']),
                         ['gpu=1', 'cpufreq=1', '-v', 'cpus=2', 'thermal=2'])
        self.assertEqual(plain['boot']['bootargs'], ['gpu=0', 'cpufreq=1', '-v'])       # (the board is not changed)
        with self.assertRaises(SystemExit) as refused:
            boards.boot_arguments(plain, ['two words'])
        self.assertIn('"two words" is not NAME or NAME=VALUE', str(refused.exception))

    def test_every_feature_says_how_it_is_started_and_the_argument_is_one_the_kernel_reads(self):
        reference = (ROOT / 'Documentation/boot-args.md').read_text()
        for _, feature in boards.feature_bits():
            how = boards.FEATURE_BOOTARGS.get(feature)
            self.assertTrue(how, f'{feature} has no boot argument in FEATURE_BOOTARGS')
            for argument in re.findall(r'\b([a-z_]+)=', how):
                self.assertIn(f'| `{argument}` ', reference, f'{feature}: Documentation/boot-args.md has no {argument}=')
        self.assertEqual(sorted(set(boards.FEATURE_BOOTARGS) - {name for _, name in boards.feature_bits()}), [])

    def test_the_repository_names_no_phone(self):
        # phones are device packages, kept on the devices branch: what the repository tracks under
        # arch/arm64/boot/dts/ is the emulator's board and the templates
        import files                    # (tools/, on the path: git's list, or a release's)
        tracked = files.listed(['arch/arm64/boot/dts'], root=ROOT)
        self.assertEqual(sorted({name.split('/')[4] for name in tracked}), ['README.md', '_template', 'qemu-virt'])

    def test_the_tests_phones_are_valid_packages(self):
        boards.MORE_DEVICES.append(ROOT / 'workbench/devices')
        problems = boards.Problems()
        socs, found = boards.load(problems)
        self.assertEqual(list(problems), [])
        for name in ('phone-cmd', 'phone-video', 'phone-keys', 'phone-buses', 'phone-i2c'):
            self.assertIn(name, found)
            names = [argument.split('=')[0] for argument in found[name].get('boot', {}).get('bootargs', [])]
            self.assertEqual(len(names), len(set(names)))

    def test_the_tool_takes_packages_from_where_it_is_told(self):
        out = Path(tempfile.mkdtemp())
        try:
            for arguments, environment in ((['--devices', str(ROOT / 'workbench/devices')], {}),
                                           ([], {'HAKIOS_DEVICES': str(ROOT / 'workbench/devices')})):
                done = subprocess.run([str(ROOT / 'tools/boards.py')] + arguments +
                                      ['gen', '--out', str(out)],
                                      capture_output=True, text=True, env={**os.environ, **environment})
                self.assertEqual(done.returncode, 0, done.stderr)
                self.assertIn('phone-buses, phone-cmd, phone-i2c, phone-keys, phone-scmi, phone-video', done.stdout)
                self.assertIn('.name = "phone-cmd"', (out / 'board_table.h').read_text())
        finally:
            shutil.rmtree(out)

    def test_the_page_of_what_each_feature_needs_is_what_the_checker_asks(self):
        # the page is the tool's own output
        self.assertEqual((ROOT / 'Documentation/features.md').read_text(), boards.features_text(),
                         'Documentation/features.md is not what tools/boards.py features prints: write it again')
        for _, feature in boards.feature_bits():
            of_soc, of_board = boards.feature_requirements(feature)
            soc = {section: {key: 1 for key in keys} for section, keys in of_soc.items()}
            board = {'soc': 'x', **{section: {key: 1 for key in keys} for section, keys in of_board.items()}}
            # a board with what the page names lacks nothing ...
            self.assertEqual(boards.feature_needs(feature, board, soc), [], feature)
            # ... and without any one section or key of it, the checker has something to say
            for part, sections in ((soc, of_soc), (board, of_board)):
                for section, keys in sections.items():
                    kept = part.pop(section)
                    self.assertNotEqual(boards.feature_needs(feature, board, soc), [], f'{feature} without [{section}]')
                    part[section] = kept
                    for key in keys if part is soc or section in ('touch', 'wifi') else ():
                        if section == 'wifi' and key == 'radio_model' or section == 'touch' and key not in boards.TOUCH_NEEDS[feature]:
                            continue        # (the schema's, for every board with the section)
                        value = part[section].pop(key)
                        self.assertNotEqual(boards.feature_needs(feature, board, soc), [], f'{feature} without {key} in [{section}]')
                        part[section][key] = value

    def test_what_the_tool_takes_is_explained_where_a_porter_looks(self):
        root = boards.ROOT
        template = (root / 'arch/arm64/boot/dts/_template/board.dts').read_text()
        readme = (root / 'arch/arm64/boot/dts/README.md').read_text()
        drivers = (root / 'Documentation/drivers.md').read_text()
        for _, feature in boards.feature_bits():
            self.assertTrue(feature in template, f'the template does not list the feature {feature}')
            self.assertTrue(f'`{feature}`' in drivers, f'Documentation/drivers.md does not have the feature {feature}')
        for section, keys in boards.BOARD_SCHEMA.items():
            if section:
                self.assertTrue(re.search(rf'(?m)^\t(// )?{boards.boardfile.name_of(section)} {{', template),
                                f'the template does not show {section} {{ }}')
                self.assertTrue(f'`{boards.boardfile.name_of(section)} {{ }}`' in readme, f'devices/README.md does not name {section} {{ }}')
            for key, (kind, _) in keys.items():
                if isinstance(kind, list) and isinstance(kind[0], dict):
                    # tables: a node NAME@0, and each of its keys
                    self.assertTrue(re.search(rf'(?m)^\t\t(// )?{key[:-1]}@0 {{', template), f'the template does not show {key[:-1]}@0 {{ }}')
                    for inner in kind[0]:
                        self.assertTrue(re.search(rf'(?m)^\t*(// )?{boards.boardfile.name_of(inner)}( = |;)', template),
                                        f'{key[:-1]}@N {inner} is not explained in the template')
                    continue
                self.assertTrue(re.search(rf'(?m)^\t*(// )?{boards.boardfile.name_of(key)}( = |;)', template),
                                f'[{section}] {key} is not explained in the template')
        for section in boards.SOC_SCHEMA:
            if section:
                self.assertTrue(f'`{boards.boardfile.name_of(section)} {{ }}`' in readme,
                                f'devices/README.md does not name the SoC section {section} {{ }}')
        soc_template = (root / 'arch/arm64/boot/dts/_template/soc.dtsi').read_text()
        for section, keys in boards.SOC_SCHEMA.items():
            if section:
                self.assertTrue(re.search(rf'(?m)^\t{boards.boardfile.name_of(section)} {{', soc_template),
                                f'the SoC template does not show {section} {{ }}')
            for key in keys:
                self.assertTrue(re.search(rf'(?m)^\t*{boards.boardfile.name_of(key)} = ', soc_template),
                                f'[{section}] {key} is not explained in the SoC template')
        # the template is a SoC file the tool takes as it is
        problems = boards.Problems()
        boards.check_file(root / 'arch/arm64/boot/dts/_template/soc.dtsi', boards.boardfile.parse(soc_template, boards.SOC_SCHEMA),
                          boards.SOC_SCHEMA, problems)
        self.assertEqual(list(problems), [])

    def test_the_loader_has_its_fallback(self):
        # the loader names platform_qemu_virt: the machine whose display matches no board
        self.assertIn('static const struct platform platform_qemu_virt = {', boards.platforms_text(*boards.load()))


if __name__ == '__main__':
    unittest.main(argv=sys.argv[:1] + ['-v'] if '-v' in sys.argv else sys.argv[:1])
