#!/usr/bin/env python3
"""tools/boards.py: what it accepts, what it refuses and says why, what it reads out of a device tree,
where it finds device packages, and the tables it writes for the boards that are there."""
import importlib.util
import io
import os
import re
import subprocess
import tomllib
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
import devicetree  # noqa: E402  (tools/, which loading boards put on the path)

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
domain_cores = [4, 3, 1]
[platform]
io_base = 0x400000
io_size = 0x3fc00000
gic_dist = 0x17a00000
gic_redist = 0x17a60000
cores = 8
core_shift = 8
small_cores = 0x0f
psci = "smc"
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
[loader]
fb_base = 0xe1000000
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


GPU = '''
[gpu]
model = "Adreno 642"
chip_id = 0x06060010
firmware = "fw-phone"
gmem_kb = 1536
primitive_fifo = 0x00300000
hang_cycles = 0x3ffff
speed_fuse = 0x1e1
gaming_fuse = 0x1ed
highest_bank_bit = 14
gcc_debug_gpucc = 0x13c
speeds_khz = [285000, 490000]
speed_levels = [64, 144]
default_speed = 2
'''


GPU_SOC = '''[adreno]
gpu = 0x3d00000
gmu = 0x3d6a000
gpucc = 0x3d90000
cx_misc = 0x3d9e000
rscc = 0x3de0000
smmu = 0x3da0000
pdc = 0xb290000
qfprom = 0x780000
[rpmh]
cmd_db = 0x80860000
cmd_db_size = 0x20000
rsc = 0x18220000
tcs_offset = 0xd00
'''


class Sandbox(unittest.TestCase):
    """A repository of its own for each test: one SoC, one board, the real kernel header."""

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.saved = (boards.ROOT, boards.DEVICES, boards.SOCS)
        boards.ROOT, boards.DEVICES, boards.SOCS = self.dir, self.dir / 'devices', self.dir / 'soc'
        (self.dir / 'devices/phone').mkdir(parents=True)
        (self.dir / 'soc').mkdir()
        (self.dir / 'soc/sm0001.toml').write_text(SOC)
        (self.dir / 'devices/phone/board.toml').write_text(BOARD)

    def tearDown(self):
        boards.ROOT, boards.DEVICES, boards.SOCS = self.saved
        del boards.MORE_DEVICES[:], boards.MORE_SOCS[:]
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
            (self.board, 'floor_mhz = [1000, 1100, 1100]', 'floor_mhz = [1000]', 'one value for each of the 3 groups'),
            (self.board, 'description = "A phone"\n', '', '"description" is missing'),
            (self.board, 'soc = "sm0001"', 'soc = "sm0001', 'not TOML'),
            (self.soc, '[display]\ndsi0 = 0xae94000\n', '', 'backlight type "dcs" needs [display]'),
            (self.soc, '[cpufreq]\nepss = 0x18591000\nepss_domain_size = 0x1000\n', '', 'feature "cpufreq-epss" needs [cpufreq]'),
            (self.soc, 'gcc = 0x100000', 'gcc = 0x100000000', 'a value of 32 bits'),
            (self.board, '"cpufreq-epss"', '"cpufreq-epss", "ddr-rpmh"', 'feature "ddr-rpmh" needs [rpmh] in soc/sm0001.toml'),
            (self.soc, 'name = "sm0001"', 'name = "sm0002"', 'the file is sm0001.toml'),
            (self.board, '[loader]', '[boot]\nfastboot_product = "phone"\ncpus = 8\nbootargs = ["cpus=8", "two words"]\n[loader]',
             '"two words" is not NAME or NAME=VALUE'),
            (self.board, '[loader]', '[boot]\nfastboot_product = "phone"\n[loader]', '"bootargs" is missing'),
            (self.board, '[loader]', '[boot]\nfastboot_product = "phone"\ncpus = 8\nbootargs = ["gpu=0", "cpus=8", "gpu=1"]\n[loader]',
             'bootargs: "gpu" is there twice'),
            (self.soc, 'io_base = 0x400000', 'io_base = 0', 'io_base must not be 0'),
            (self.board, '"cpufreq-epss"', '"cpufreq-epss", "display-scan"', 'feature "display-scan" needs intf1'),
            (self.board, '"cpufreq-epss"]\n[backlight]\ntype = "dcs"\npanel_mode = "command"',
             '"display-scan"]\n[backlight]\ntype = "dcs"\npanel_mode = "video"', 'needs a command-mode panel'),
            (self.soc, 'domain_cores = [4, 3, 1]', 'domain_cores = [4, 3]', 'domain_cores adds up to 7 cores, [platform] has 8'),
            (self.soc, 'domain_cores = [4, 3, 1]', 'domain_cores = [4, 2, 2]\n', None),
            (self.soc, 'domain_cores = [4, 3, 1]', 'domain_cores = [2, 2, 2, 1, 1]', 'one to four groups'),
            (self.board, 'floor_mhz = [1000, 1100, 1100]', 'floor_mhz = [1000, 1100]', 'one value for each of the 3 groups'),
            (self.soc, 'psci = "smc"', 'psci = "telepathy"', 'one of smc, hvc'),
            (self.board, '"cpufreq-epss"', '"cpufreq-epss", "virtio"', 'feature "virtio" is an emulator'),
            (self.board, '[loader]', '[wifi]\nradio_model = 2\n[loader]', 'radio_model: 0 (a WCN6750'),
            (self.board, '[loader]', '[wifi]\nradio_model = 1\nearly_pool_mb = 80\n[loader]', None),
            (self.board, '[loader]', '[wifi]\nradio_model = 0\npins = [84, 85, 86]\nsupplies = ["smpb7", "smpb1", "smpb8"]\n[loader]', None),
            (self.board, '[loader]', '[wifi]\nradio_model = 0\npins = [84, 85, 85]\n[loader]', 'pins are three different pins'),
            (self.board, '[loader]', '[wifi]\nradio_model = 0\nsupplies = ["smpb7", "S1B"]\n[loader]', 'supplies are three different names'),
            (self.soc, 'gic_dist = 0x17a00000\n', '', '"gic_dist" is missing'),
        ]
        for change, old, new, reason in cases:
            with self.subTest(reason=reason):
                saved = [(p, p.read_text()) for p in (self.dir / 'devices/phone/board.toml', self.dir / 'soc/sm0001.toml')]
                try:
                    change(old, new)
                    said = self.problems()
                    if reason is None:              # (a change that is fine)
                        self.assertEqual(said, '')
                        continue
                    self.assertIn(reason, said)
                    self.assertTrue('devices/phone/board.toml' in said or 'soc/sm0001.toml' in said, said)
                finally:
                    for path, text in saved:
                        path.write_text(text)

    def test_keys_need_the_arbiter_and_their_peripherals(self):
        path = self.dir / 'devices/phone/board.toml'
        self.board('"cpufreq-epss"', '"cpufreq-epss", "buttons-pmic"')
        said = self.problems()
        self.assertIn('feature "buttons-pmic" needs [spmi] in soc/sm0001.toml', said)
        self.assertIn('feature "buttons-pmic" needs [buttons] in the board file', said)
        self.soc('[thermal]', '[spmi]\ncore = 0xc440000\ncnfg = 0xc40a000\nobserver = 0xe600000\n[thermal]')
        listed = path.read_text()
        for section, reason in (
                ('[buttons]\npon = 0x013\nvolume_up = 0x18d\n', None),
                ('[buttons]\npon = 0x013\nvolume_up = 0x18d\nvolume_up_name = "GPIO6 of the PM8350"\n', None),
                ('[buttons]\npon = 0x013\nvolume_up = 0x8d00\n', 'SPMI slave << 8'),
                ('[buttons]\npon = 0x100\nvolume_up = 0x18d\n', 'SPMI slave << 8'),
                ('[buttons]\npon = 0x18d\nvolume_up = 0x18d\n', 'two peripherals'),
                ('[buttons]\npon = 0x013\n', '"volume_up" is missing')):
            with self.subTest(section=section):
                path.write_text(listed + section)
                if reason is None:
                    self.assertEqual(self.problems(), '')
                else:
                    self.assertIn(reason, self.problems())
        path.write_text(listed + '[buttons]\npon = 0x013\nvolume_up = 0x18d\n')
        text = boards.table_text(*boards.load())
        self.assertIn('.spmi = { .core = 0x0c440000, .cnfg = 0x0c40a000, .observer = 0x0e600000, .channels = 0x00000000, '
                      '.ee = 0 },', text)
        # a driver that writes to a PMIC needs to be told where writes go as well
        path.write_text(path.read_text().replace('"buttons-pmic"', '"buttons-pmic", "torch-pm8350c"') +
                        '[torch]\nflash = 0x2ee\nalarm = 0x20a\n')
        self.assertIn('feature "torch-pm8350c" needs channels in [spmi] of soc/sm0001.toml', self.problems())
        self.soc('observer = 0xe600000', 'observer = 0xe600000\nchannels = 0xc600000')
        self.assertEqual(self.problems(), '')
        self.assertIn('.channels = 0x0c600000,', boards.table_text(*boards.load()))
        self.assertIn('.buttons = { .pon = 0x013, .volume_up = 0x18d },', text)

    def test_an_audio_dsp_is_described(self):
        path = self.dir / 'devices/phone/board.toml'
        self.board('"cpufreq-epss"', '"cpufreq-epss", "adsp"')
        self.assertIn('feature "adsp" needs [adsp] in the board file', self.problems())
        # ... and RPMh, which its supplies are asked of, and the way to the DSP
        self.assertIn('feature "adsp" needs [rpmh] in soc/sm0001.toml', self.problems())
        self.assertIn('feature "adsp" needs [ipc] in soc/sm0001.toml', self.problems())
        self.soc('[thermal]', '[ipc]\naoss = 0xc300000\nipcc = 0x408000\nsmem = 0x80900000\nsmem_size = 0x200000\nsmem_lock = 0x1f43000\n[thermal]')
        self.soc('[thermal]', '[rpmh]\ncmd_db = 0x80860000\ncmd_db_size = 0x20000\nrsc = 0x18220000\ntcs_offset = 0xd00\n[thermal]')
        listed = path.read_text()
        for section, reason in (
                ('[adsp]\nfirmware = "fw-audio-phone"\nmemory = 0x86700000\nmemory_size = 0x3000000\n', None),
                ('[adsp]\nfirmware = "fw-audio-phone"\nmemory = 0x86700000\nmemory_size = 0x3000000\ncharge_counter_divisor = 100\n', None),
                ('[adsp]\nfirmware = "fw-audio-phone"\nmemory = 0x86700000\nmemory_size = 0x3000000\ncharge_counter_divisor = 0\n', 'must not be 0'),
                ('[adsp]\nfirmware = "fw-audio-phone"\nmemory = 0x86700000\nmemory_size = 0x3000001\n', 'whole pages'),
                ('[adsp]\nfirmware = "/fw/../etc"\nmemory = 0x86700000\nmemory_size = 0x3000000\n', 'without slashes'),
                ('[adsp]\nfirmware = "fw-audio-phone"\nmemory = 0x86700123\nmemory_size = 0x3000000\n', 'on a page'),
                ('[adsp]\nfirmware = "fw-audio-phone"\nmemory_size = 0x3000000\n', '"memory" is missing'),
                ('[adsp]\nmemory = 0x86700000\nmemory_size = 0x3000000\n', '"firmware" is missing')):
            with self.subTest(section=section):
                path.write_text(listed + section)
                if reason is None:
                    self.assertEqual(self.problems(), '')
                else:
                    self.assertIn(reason, self.problems())
        path.write_text(listed + '[adsp]\nfirmware = "fw-audio-phone"\nmemory = 0x86700000\nmemory_size = 0x3000000\n')
        self.assertIn('.adsp = { .firmware = "fw-audio-phone", .memory = 0x86700000, .memory_size = 0x03000000, '
                      '.charge_counter_divisor = 1 },',
                      boards.table_text(*boards.load()))

    def test_a_gpu_is_described_in_full(self):
        path = self.dir / 'devices/phone/board.toml'
        plain = path.read_text()
        self.board('"cpufreq-epss"', '"cpufreq-epss", "gpu-a660"')
        said = self.problems()
        self.assertIn('feature "gpu-a660" needs [gpu] in the board file', said)
        # ... and of its SoC, where the GPU's blocks are and the command database
        self.assertIn('feature "gpu-a660" needs [adreno] in soc/sm0001.toml', said)
        self.assertIn('feature "gpu-a660" needs [rpmh] in soc/sm0001.toml', said)
        self.soc('[thermal]', GPU_SOC + '[thermal]')
        with_gpu = path.read_text() + GPU
        cases = [
            ('', '', None),
            ('speed_levels = [64, 144]', 'speed_levels = [64]', 'one supply level for each of speeds_khz'),
            ('speeds_khz = [285000, 490000]', 'speeds_khz = [490000, 285000]', 'from the slowest up'),
            ('speeds_khz = [285000, 490000]', 'speeds_khz = []', 'one to 5 speeds'),
            ('default_speed = 2', 'default_speed = 3', 'which of speeds_khz, 1 to 2'),
            ('default_speed = 2', 'default_speed = 0', 'which of speeds_khz, 1 to 2'),
            ('chip_id = 0x06060010', 'chip_id = 0', 'chip_id must not be 0'),
            ('chip_id = 0x06060010\n', '', '"chip_id" is missing'),
            ('default_speed = 2', 'default_speed = 2\nalt_model = "Adreno 643"', 'alt_model goes with alt_bins'),
            ('default_speed = 2', 'default_speed = 2\nalt_model = "Adreno 643"\nalt_bins = [1, 2, 3]', 'one or two speed bins'),
            ('default_speed = 2', 'default_speed = 2\nalt_model = "Adreno 643"\nalt_bins = [0xac]', None),
            ('default_speed = 2', 'default_speed = 2\nzap_must_authenticate = "yes"', 'bool expected'),
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
                    self.assertIn('devices/phone/board.toml', said)
        # described but not listed: the data may be there before the driver is turned on
        path.write_text(plain + GPU)
        self.assertEqual(self.problems(), '')

    def test_status_says_what_can_be_turned_on_next(self):
        def status():
            socs, found = boards.load()
            return boards.status_text('phone', found['phone'], socs['sm0001'])
        said = status()
        self.assertIn('phone: A phone (sm0001)', said)
        self.assertRegex(said, r'loader +known by its framebuffer')
        self.assertRegex(said, r'backlight +the panel takes MIPI DCS brightness')
        self.assertRegex(said, r'cpufreq-epss +on \(cpufreq=1\)\n')
        self.assertRegex(said, r'battery-uefi +can be listed: what it needs is there \(then battery=1\)')
        # by the same rules as the checker: what is missing is named, file and section
        self.assertRegex(said, r'buttons-pmic +needs \[spmi\] in soc/sm0001.toml, \[buttons\] in the board file')
        self.assertRegex(said, r'ddr-rpmh +needs \[rpmh\] in soc/sm0001.toml')
        self.assertRegex(said, r'display-scan +needs intf1 in \[display\] and \[clocks\] in soc/sm0001.toml')
        self.assertRegex(said, r'mic-wcd938x +needs \[pins\] in soc/sm0001.toml, \[lpass\] in soc/sm0001.toml')
        # a part's driver whose addresses are there is still one part's: said, not offered
        self.soc('[thermal]', '[pins]\ntlmm = 0xf100000\n[thermal]')
        self.soc('[thermal]', '[lpass]\ntx_macro = 0x3220000\nva_macro = 0x3370000\ntx_swr = 0x3230000\ntx_swr_hctl = 0x32a9000\n'
                              'lpi = 0x33c0000\nlpi_slew = 0x355a000\n[thermal]')
        self.assertRegex(status(), r'mic-wcd938x +needs \[mic\] in the board file')        # (the pin its codec's reset is on)
        self.board('[loader]', '[mic]\ncodec_reset = 83\n[loader]')
        self.assertRegex(status(), r"mic-wcd938x +one part's driver")
        self.assertIn('.mic = { .wired = 1, .codec_reset = 83 },', boards.table_text(*boards.load()))
        self.assertIn('.tx_swr = 0x03230000, .tx_swr_hctl = 0x032a9000,', boards.table_text(*boards.load()))
        self.assertRegex(said, r'touch-goodix +needs \[pins\] in soc/sm0001.toml')
        self.soc('[thermal]', '[spmi]\ncore = 0xc440000\ncnfg = 0xc40a000\nobserver = 0xe600000\n[thermal]')
        self.assertRegex(status(), r'buttons-pmic +needs \[buttons\] in the board file')
        path = self.dir / 'devices/phone/board.toml'
        path.write_text(path.read_text() + '[buttons]\npon = 0x013\nvolume_up = 0x18d\n')
        self.assertRegex(status(), r'buttons-pmic +can be listed')
        self.board('"cpufreq-epss"', '"cpufreq-epss", "buttons-pmic"')
        self.assertRegex(status(), r'buttons-pmic +on \(buttons=1\)\n')
        # every feature the kernel knows has its line
        for _, feature in boards.feature_bits():
            self.assertRegex(said, rf'(?m)^  {feature} ')

    def test_the_loader_tells_boards_apart_by_their_framebuffer(self):
        for name, loader, reason in (('twin', '[loader]\nfb_base = 0xe1000000\n', 'is also that of'),
                                     ('emu1', '[loader]\n', None), ('emu2', '[loader]\n', 'more than one board')):
            (self.dir / 'devices' / name).mkdir()
            (self.dir / 'devices' / name / 'board.toml').write_text(
                f'name = "{name}"\ndescription = "x"\nsoc = "sm0001"\nfeatures = []\n[backlight]\ntype = "soft"\n{loader}')
            if reason:
                self.assertIn(reason, self.problems())
            else:
                self.assertNotIn('emu1', self.problems())

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
                         '.epss = 0x18591000, .epss_domain_size = 0x1000,', '.domain_cores = { 4, 3, 1 },', '"cpufreq-epss"'):
            self.assertIn(expected, text)
        self.assertNotIn('*/board.toml', text, 'a path with */ in it would close the header\'s comment')

    def test_the_table_carries_the_gpu(self):
        path = self.dir / 'devices/phone/board.toml'
        self.assertNotIn('.gpu = {', boards.table_text(*boards.load()))
        self.soc('[thermal]', GPU_SOC + '[thermal]')
        path.write_text(path.read_text().replace('"cpufreq-epss"', '"cpufreq-epss", "gpu-a660"') + GPU +
                        'alt_model = "Adreno 643"\nalt_bins = [0xac]\nzap_must_authenticate = true\n')
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
        self.soc('[thermal]', '[rpmh]\ncmd_db = 0x80860000\ncmd_db_size = 0x20000\nrsc = 0x18220000\ntcs_offset = 0xd00\n[thermal]')
        self.board('"cpufreq-epss"', '"cpufreq-epss", "ddr-rpmh"')
        self.assertEqual(self.problems(), '')
        self.assertIn('.rpmh = { .cmd_db = 0x80860000, .cmd_db_size = 0x20000, .rsc = 0x18220000, .tcs_offset = 0xd00 },',
                      boards.table_text(*boards.load()))

    def test_the_partition_a_write_may_reach_is_the_boards(self):
        path = self.dir / 'devices/phone/board.toml'
        plain = path.read_text()
        own = ('[storage]\ntype_guid = "7c3457ef-0000-11aa-aa11-00306543ecac"\nunique_guid = "FD6854AD-24AA-4FC3-94FE-290116858D68"\n'
               'first_lba = 25374720\nlast_lba = 30617594\nwrite_first = 2042368\n')
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
                ('type_guid = "7c3457ef-0000-11aa-aa11-00306543ecac"', 'type_guid = "7c3457ef000011aaaa1100306543ecac"',
                 'type_guid is a GUID as a partition table prints it'),
                ('unique_guid = "FD6854AD-24AA-4FC3-94FE-290116858D68"', 'unique_guid = "FD6854AD-24AA-4FC3-94FE-290116858D6G"',
                 'unique_guid is a GUID as a partition table prints it'),
                ('last_lba = 30617594', 'last_lba = 25374720', 'the first is not 0 and before the last'),
                ('first_lba = 25374720', 'first_lba = 0', 'the first is not 0 and before the last'),
                ('write_first = 2042368', 'write_first = 0', 'write_first is a block of the partition'),
                ('write_first = 2042368', 'write_first = 5242875', 'write_first is a block of the partition')):
            path.write_text(plain + own.replace(right, wrong))
            self.assertIn(said, self.problems(), wrong)
        # a board without the section has no such partition: the loader's and the kernel's rows say nothing of one
        path.write_text(plain)
        socs, found = boards.load()
        self.assertNotIn('.own', boards.platforms_text(socs, found))
        self.assertNotIn('.storage', boards.table_text(socs, found))

    def test_the_speaker_amplifiers_need_their_wiring(self):
        self.soc('[thermal]', '[pins]\ntlmm = 0xf100000\n[smmu]\napps = 0x15000000\n[thermal]')
        self.soc('[thermal]', '[rpmh]\ncmd_db = 0x80860000\ncmd_db_size = 0x20000\nrsc = 0x18220000\ntcs_offset = 0xd00\n[thermal]')
        self.board('"cpufreq-epss"', '"cpufreq-epss", "speaker-aw88261"')
        self.assertIn('feature "speaker-aw88261" needs [speaker] in the board file', self.problems())
        path = self.dir / 'devices/phone/board.toml'
        plain = path.read_text()
        path.write_text(plain + '[speaker]\nsda = 12\nscl = 13\naddresses = [0x35, 0x35]\ninterrupts = [102, 12]\nsound_pins = [97, 100, 98, 99]\nsound_function = 1\n')
        said = self.problems()
        self.assertIn('eight different pins', said)                      # (an interrupt on the bus's data line)
        self.assertIn('different I2C addresses', said)
        path.write_text(plain + '[speaker]\nsda = 12\nscl = 13\naddresses = [0x35]\ninterrupts = [102, 45]\nsound_pins = [97, 100, 98, 99]\nsound_function = 1\n')
        self.assertIn('different I2C addresses', self.problems())      # (there are two amplifiers)
        path.write_text(plain + '[speaker]\nsda = 12\nscl = 13\naddresses = [0x35, 0x34]\ninterrupts = [102, 45]\nsound_pins = [97, 100, 98, 99]\nsound_function = 1\n')
        self.assertEqual(self.problems(), '')
        self.assertIn('.speaker = { .sda = 12, .scl = 13, .address = { 0x35, 0x34 }, .interrupt = { 102, 45 },\n\t\t\t'
                      '.sound_pin = { 97, 100, 98, 99 }, .sound_function = 1 },',
                      boards.table_text(*boards.load()))

    def test_part_drivers_need_the_pins_and_the_table_carries_them(self):
        self.board('"cpufreq-epss"', '"cpufreq-epss", "haptic-aw86927", "touch-goodix"')
        said = self.problems()
        self.assertIn('feature "haptic-aw86927" needs [pins] in soc/sm0001.toml', said)
        self.assertIn('feature "haptic-aw86927" needs [haptic] in the board file', said)       # (which pins it is on)
        path = self.dir / 'devices/phone/board.toml'
        path.write_text(path.read_text() + '[haptic]\nsda = 36\nscl = 36\nreset = 35\ninterrupt = 18\nf0_calibration = 0x0d\n')
        self.assertIn('four different pins', self.problems())
        path.write_text(path.read_text().replace('scl = 36', 'scl = 37'))
        self.assertIn('feature "touch-goodix" needs [pins] in soc/sm0001.toml', said)
        self.assertNotIn('feature "touch-goodix" needs [clocks]', said)        # (which this SoC file has)
        self.assertIn('feature "touch-goodix" needs [rpmh] in soc/sm0001.toml', said)  # (its supply is asked of RPMh)
        self.assertNotIn('feature "haptic-aw86927" needs [rpmh]', said)
        self.soc('[thermal]', '[pins]\ntlmm = 0xf100000\n[thermal]')
        self.soc('[thermal]', '[rpmh]\ncmd_db = 0x80860000\ncmd_db_size = 0x20000\nrsc = 0x18220000\ntcs_offset = 0xd00\n[thermal]')
        # a touch controller's driver also needs to be told how the controller is wired
        self.assertEqual(self.problems(), 'devices/phone/board.toml: feature "touch-goodix" needs [touch] in the board file')
        wiring = ('[touch]\nserial_engine = 0xa94000\nclocks = [0x18004, 0x18008, 0x185fc]\nvote_register = 0x52008\nvotes = [18, 19, 20, 21, 27]\n'
                  'bus_pins = [52, 53, 54, 55]\ninterrupt = 81\nreset = 105\nio_supply = "ldoc2"\n')
        plain = path.read_text()
        path.write_text(plain + wiring)
        self.assertIn('feature "touch-goodix" needs analog_enable and vendor_pins in [touch] of the board file', self.problems())
        path.write_text(plain + wiring.replace('bus_pins = [52, 53, 54, 55]', 'bus_pins = [52, 53, 54, 81]') +
                        'analog_enable = 107\nvendor_pins = [68, 96]\n')
        self.assertIn('every pin is a different pin of the SoC', self.problems())      # (the interrupt's pin on the bus)
        path.write_text(plain + wiring.replace('votes = [18, 19, 20, 21, 27]', 'votes = [18, 19, 20, 21, 32]') +
                        'analog_enable = 107\nvendor_pins = [68, 96]\n')
        self.assertIn('votes are five different bits of the vote register', self.problems())
        path.write_text(plain + wiring.replace('clocks = [0x18004, 0x18008, 0x185fc]', 'clocks = [0x18004, 0x18008]') +
                        'analog_enable = 107\nvendor_pins = [68, 96]\n')
        self.assertIn('clocks are three different offsets', self.problems())
        path.write_text(plain + wiring + 'analog_enable = 107\nvendor_pins = [68, 96]\n')
        self.assertEqual(self.problems(), '')
        table = boards.table_text(*boards.load())
        self.assertIn('.pins = { .tlmm = 0x0f100000 },', table)
        for line in ('.serial_engine = 0x00a94000,', '.m_ahb_clock = 0x18004, .s_ahb_clock = 0x18008, .se_clock = 0x185fc,',
                     '.vote_register = 0x52008, .vote = { 18, 19, 20, 21, 27 },', '.data_in = 52, .data_out = 53, .clock = 54, .chip_select = 55,',
                     '.interrupt = 81, .reset = 105,', '.analog_enable = 107, .vendor_pin = { 68, 96 },', '.io_supply = "ldoc2",'):
            self.assertIn(line, table)
        # the other controller's driver needs its second supply, and no strap
        path.write_text((plain + wiring).replace('"touch-goodix"', '"touch-ft3658u"'))
        self.assertEqual(self.problems(), 'devices/phone/board.toml: feature "touch-ft3658u" needs analog_supply in [touch] of the board file')
        path.write_text((plain + wiring + 'analog_supply = "ldoc7"\n').replace('"touch-goodix"', '"touch-ft3658u"'))
        self.assertEqual(self.problems(), '')
        self.assertIn('.analog_enable = -1, .vendor_pin = { -1, -1 },', boards.table_text(*boards.load()))

    def test_the_table_carries_the_way_to_the_other_processors(self):
        self.soc('[thermal]', '[ipc]\naoss = 0xc300000\nipcc = 0x408000\nsmem = 0x80900000\nsmem_size = 0x200000\nsmem_lock = 0x1f43000\n[thermal]')
        self.assertEqual(self.problems(), '')
        self.assertIn('.ipc = { .aoss = 0x0c300000, .ipcc = 0x00408000, .smem = 0x80900000, .smem_size = 0x200000, '
                      '.smem_lock = 0x01f43000 },', boards.table_text(*boards.load()))
        # all of it or none: a mailbox without the memory is no way to anybody
        self.soc('smem_lock = 0x1f43000\n', '')
        self.assertIn('"smem_lock" is missing', self.problems())

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
        self.soc('gcc_disp_ahb_cbcr = 0x27004\ndispcc = 0xaf00000\ndispcc_mdss_gdscr = 0x3000\ndispcc_mdss_ahb_cbcr = 0x2080\n', '')
        self.assertEqual(self.problems(), 'devices/phone/board.toml: backlight type "dcs" needs gcc_disp_ahb_cbcr, dispcc, '
                         'dispcc_mdss_gdscr, dispcc_mdss_ahb_cbcr in [clocks] of soc/sm0001.toml')
        path = self.dir / 'devices/phone/board.toml'
        path.write_text(re.sub(r'(?s)\[backlight\].*?\n\[', '[backlight]\ntype = "soft"\n[', path.read_text(), count=1))
        self.assertEqual(self.problems(), '')
        self.assertIn('.gcc = 0x00100000, .gcc_disp_ahb_cbcr = 0x0,', boards.table_text(*boards.load()))
        # ... and the GPU's driver, which switches the display's bus clock on, asks for that one
        socs, found = boards.load()
        self.assertIn('gcc_disp_ahb_cbcr in [clocks] of soc/sm0001.toml', boards.feature_needs('gpu-a660', found['phone'], socs['sm0001']))
        self.assertIn('gcc_disp_ahb_cbcr, dispcc, dispcc_mdss_gdscr, dispcc_mdss_ahb_cbcr in [clocks] of soc/sm0001.toml',
                      boards.feature_needs('display-scan', found['phone'], socs['sm0001']))

    def test_the_smmu_is_needed_carried_and_found(self):
        self.board('"cpufreq-epss"', '"cpufreq-epss", "camera-redwood"')
        self.assertIn('feature "camera-redwood" needs [smmu] in soc/sm0001.toml', self.problems())
        self.assertIn('feature "camera-redwood" needs [camera] in soc/sm0001.toml', self.problems())
        self.soc('[thermal]', '[camera]\ncamcc = 0xad00000\nife = 0xacc4000\ncsid = 0xacc8000\ncsiphy = 0xace0000\n[thermal]')
        self.board('"cpufreq-epss", "camera-redwood"', '"cpufreq-epss"')
        self.soc('[thermal]', '[smmu]\napps = 0x15000000\n[thermal]')
        self.assertEqual(self.problems(), '')
        self.assertIn('.camera = { .camcc = 0x0ad00000, .ife = 0x0acc4000, .csid = 0x0acc8000, .csiphy = 0x0ace0000 },',
                      boards.table_text(*boards.load()))
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
        text = (self.dir / 'devices/phone4/board.toml').read_text()
        # offered, not switched on: the section is a comment until the firmware's directory is named
        self.assertIn('# [adsp]\n# firmware = "fw-audio-phone4"\n# memory = 0x86100000\n# memory_size = 0x02100000\n', text)
        self.assertEqual(self.problems(), '')

    def test_a_radio_on_the_socs_dsp_needs_the_dsps_memory(self):
        socs, found = boards.load()
        self.assertIn('dsp_memory and dsp_memory_size in [wifi] of the board file',
                      boards.feature_needs('wifi-wcn6750', found['phone'], socs['sm0001']))
        path = self.dir / 'devices/phone/board.toml'
        listed = path.read_text()
        path.write_text(listed + '[wifi]\nradio_model = 0\ndsp_memory = 0x9ae00000\n')
        self.assertIn('dsp_memory and dsp_memory_size go together', self.problems())
        path.write_text(listed + '[wifi]\nradio_model = 0\ndsp_memory = 0x9ae00000\ndsp_memory_size = 0x1900000\n')
        self.assertEqual(self.problems(), '')
        self.assertIn('.wifi = { .radio_model = 0, .early_pool_mb = 0, .dsp_memory = 0x9ae00000, .dsp_memory_size = 0x01900000,\n\t\t\t'
                      '.pin = { -1, -1, -1 }, .supply = { "", "", "" } },',
                      boards.table_text(*boards.load()))
        socs, found = boards.load()
        self.assertNotIn('dsp_memory and dsp_memory_size in [wifi] of the board file',
                         boards.feature_needs('wifi-wcn6750', found['phone'], socs['sm0001']))
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
        text = (self.dir / 'devices/phone7/board.toml').read_text()
        self.assertIn('# [wifi]\n# radio_model = 0\n# dsp_memory = 0x9ae00000\n# dsp_memory_size = 0x01900000\n# pins = [84, 85, 86]\n', text)
        self.assertEqual(self.problems(), '')

    def test_the_storage_controller_is_found_and_reaches_the_loader(self):
        soc, board, notes = devicetree.from_dts('''/ { soc { ufshc_mem: ufshc@1d84000 { compatible = "qcom,ufshc";
            reg = <0x1d84000 0x3000>, <0x1d88000 0x8000>; reg-names = "ufs_mem", "ufs_ice"; }; }; };''')
        self.assertEqual(soc['platform']['ufs'], 0x1d84000)
        self.assertNotIn('.ufs = ', boards.platforms_text(*boards.load()))      # (this SoC file does not say)
        self.soc('io_size = 0x3fc00000\n', 'io_size = 0x3fc00000\nufs = 0x1d84000\n')
        self.assertEqual(self.problems(), '')
        self.assertIn('.ufs = 0x01d84000', boards.platforms_text(*boards.load()))

    def test_the_flashlight_needs_its_peripherals_and_a_tree_has_them(self):
        self.soc('[thermal]', '[spmi]\ncore = 0xc440000\ncnfg = 0xc40a000\nobserver = 0xe600000\nchannels = 0xc600000\n[thermal]')
        self.board('"cpufreq-epss"', '"cpufreq-epss", "torch-pm8350c"')
        self.assertIn('feature "torch-pm8350c" needs [torch] in the board file', self.problems())
        path = self.dir / 'devices/phone/board.toml'
        listed = path.read_text()
        path.write_text(listed + '[torch]\nflash = 0x200\nalarm = 0x20a\n')
        self.assertIn('flash = 0x200: SPMI slave << 8', self.problems())        # (a slave without a peripheral: a typo)
        path.write_text(listed + '[torch]\nflash = 0x2ee\nalarm = 0x20a\n')
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
        self.assertIn('[torch]\nflash = 0x2ee\nalarm = 0x20a\n', (self.dir / 'devices/phone5/board.toml').read_text())
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
        text = (self.dir / 'devices/phone6/board.toml').read_text()
        self.assertIn('# [touch]\n# interrupt = 31\n# reset = 30\n', text)
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
        (self.dir / 'devices/emu/board.toml').write_text(
            'name = "emu"\ndescription = "An emulator"\nsoc = "sm0001"\nfeatures = []\n[backlight]\ntype = "soft"\n[loader]\n')
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
        path = self.dir / 'devices/phone/board.toml'
        path.write_text(path.read_text() + '[screen]\nstatusbar_leading_shift = 24\n')
        self.assertEqual(self.problems(), '')
        self.assertIn('\t{ "phone", 24 },', boards.user_boards_text(*boards.load()))
        path.write_text(path.read_text().replace('= 24', '= 2400'))
        self.assertIn('some tens, not hundreds', self.problems())

    def test_it_is_the_same_every_time(self):
        self.assertEqual(boards.table_text(*boards.load()), boards.table_text(*boards.load()))


class DeviceTree(Sandbox):
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

    def test_a_gpu_that_is_only_half_in_the_tree_is_left_to_be_filled_in(self):
        soc, board, notes = devicetree.from_dts('''/ { soc {
            qcom,kgsl-3d0@3d00000 { reg = <0x3d00000 0x40000>; reg-names = "kgsl_3d0_reg_memory"; };
            clock-controller@3d90000 { compatible = "qcom,shima-gpucc"; reg = <0x3d90000 0x9000>; }; }; };''')
        self.assertNotIn('adreno', soc)
        self.assertTrue(any('missing: gmu, cx_misc, rscc, smmu, pdc, qfprom' in note for note in notes), notes)
        # ... and a tree without a GPU node says nothing of it
        soc, board, notes = devicetree.from_dts('/ { soc { clock-controller@3d90000 { compatible = "qcom,x-gpucc"; }; }; };')
        self.assertFalse(any('GPU' in note for note in notes), notes)

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
            return boards.compare_text('phone', found['phone'], socs['sm0001'], tree_soc, tree_board, 'phone.dts')
        text, differ = compared()
        self.assertEqual(differ, 3, text)       # this SoC file's cores and their groups are not the tree's
        self.assertRegex(text, r'\[clocks\] gcc +0x00100000 +as in the tree')
        self.assertRegex(text, r'\[clocks\] gcc_disp_ahb_cbcr +0x00027004 +not in the tree')
        self.assertRegex(text, r'\[cpufreq\] domain_cores +\[4, 3, 1\] +THE TREE SAYS \[2, 1, 1\]')
        self.assertRegex(text, r'\[platform\] cores +8 +THE TREE SAYS 4')
        self.assertRegex(text, r'\[backlight\] max +2047 +as in the tree')
        # a section the tree knows nothing of is one line; one the file lacks is offered with its values
        self.assertRegex(text, r'\[thermal\] +1 value, none of which the tree says\n')
        self.assertIn('the tree also has, of [spmi]: core = 0x0c440000, cnfg = 0x0c40a000, observer = 0x0e600000, '
                      'ee = 0, channels = 0x0c600000', text)
        self.assertIn('the tree also has, of [buttons]: pon = 0x013,', text)
        self.assertIn('the tree also has, of [backlight]: restore = 307', text)
        self.assertRegex(text, r'\d+ values as in the tree, 3 differ, \d+ the tree does not say')
        # another panel of the same tree is another answer
        text, differ = compared('qcom,mdss_dsi_second_video')
        self.assertRegex(text, r'\[backlight\] type +"dcs" +THE TREE SAYS "soft"')
        # a panel's node that does not say how its light is controlled is not taken to say "soft"
        tree_soc, tree_board, notes = boards.tree_to_compare(
            '/ { qcom,mdss_dsi_half { qcom,mdss-dsi-panel-type = "dsi_video_mode"; qcom,mdss-dsi-bl-max-level = <4095>; }; };')
        self.assertNotIn('backlight', tree_board)
        self.assertTrue(any('[backlight] is not compared' in note for note in notes), notes)
        # ... and with several panels, which one was taken is said
        self.assertTrue(any('several panels' in note for note in boards.tree_to_compare(DTS)[2]))

    def test_a_new_board_is_started_from_a_device_tree(self):
        dts = self.dir / 'phone2.dts'
        dts.write_text(DTS)
        with contextlib.redirect_stdout(io.StringIO()) as said:
            boards.new_board('phone2', 'sm0002', dts, 'qcom,mdss_dsi_first_cmd', 'Another phone')
        self.assertIn('wrote devices/phone2/board.toml', said.getvalue())
        self.assertFalse((self.dir / 'devices/phone2/README.md').exists())      # (this repository of the test has no template)
        (self.dir / 'devices/_template').mkdir()
        (self.dir / 'devices/_template/README.md').write_text((ROOT / 'devices/_template/README.md').read_text())
        with contextlib.redirect_stdout(io.StringIO()) as again:
            boards.new_board('phone3', 'sm0002', dts, 'qcom,mdss_dsi_first_cmd', 'A third phone')
        readme = (self.dir / 'devices/phone3/README.md').read_text()
        self.assertIn('wrote devices/phone3/README.md', again.getvalue())
        self.assertTrue(readme.startswith('# A third phone (`phone3`)'), readme[:60])
        self.assertIn('`soc/sm0002.toml` on the main branch', readme)
        self.assertIn('tools/boards.py status phone3', readme)
        self.assertNotIn('NAME', readme)
        shutil.rmtree(self.dir / 'devices/phone3')
        shutil.rmtree(self.dir / 'devices/_template')
        self.assertIn('wrote soc/sm0002.toml', said.getvalue())
        # as written it is refused for exactly what a device tree cannot say, and accepted once that is filled in
        problems = self.problems()
        self.assertIn('backlight type "dcs" needs gcc_disp_ahb_cbcr, dispcc_mdss_gdscr, dispcc_mdss_ahb_cbcr in [clocks] of '
                      'soc/sm0002.toml', problems)
        self.assertEqual([line for line in problems.splitlines() if 'in [clocks] of' not in line], [])
        path = self.dir / 'soc/sm0002.toml'
        text = path.read_text()
        for key, value in (('gcc_disp_ahb_cbcr', '0x27004'), ('dispcc_mdss_gdscr', '0x3000'), ('dispcc_mdss_ahb_cbcr', '0x2080')):
            self.assertIn(f'# {key} = 0x...', text)
            text = text.replace(f'# {key} = 0x...', f'{key} = {value}')       # (what is said after it stays a comment)
        path.write_text(text)
        self.assertEqual(self.problems(), '')
        # the I/O window, which no tree has either, is asked for when the loader is to know the board
        board = self.dir / 'devices/phone2/board.toml'
        board.write_text(board.read_text() + '[loader]\nfb_base = 0x9c000000\n')
        self.assertEqual(self.problems(), 'devices/phone2/board.toml: [loader] needs io_base and io_size in [platform] of '
                         'soc/sm0002.toml')
        text = text.replace('# io_base = 0x00400000', 'io_base = 0x00400000').replace('# io_size = 0x3FC00000', 'io_size = 0x3FC00000')
        path.write_text(text)
        self.assertEqual(self.problems(), '')
        self.assertIn('.raw_min = 1, .raw_max = 2047, .raw_restore = 307,', boards.table_text(*boards.load()))
        # the file explains itself: each section and value with what the template says of it, addresses
        # in eight digits, offsets in hex, counts as a person counts
        for line in ('# The PMIC arbiter (qcom,spmi-pmic-arb), by its reg-names: for the features "buttons-pmic",',
                     'core = 0x0c440000                   # "core"',
                     'tcs_offset = 0xd00                  # qcom,tcs-offset of that node',
                     'tsens = [0x0c263000, 0x0c265000]    # the TSENS controllers (at most two), reg "tsens_tm_physical"',
                     'cores = 4 ', 'small_cores = 0x3 ',
                     '# TO FILL IN: gcc_disp_ahb_clk: its .halt_reg'):
            self.assertIn(line, text)
        self.assertEqual([line for line in text.splitlines() if line != line.rstrip()], [])
        # the keys were found: listing the feature is all that is left to do for them
        board = self.dir / 'devices/phone2/board.toml'
        self.assertIn('[buttons]\npon = 0x013\npon_name = "the PMK8350\'s power-on block"\nvolume_up = 0x18d\n'
                      'volume_up_name = "GPIO6 of the PM8350"\n', board.read_text())
        board.write_text(board.read_text().replace('features = []', 'features = ["buttons-pmic"]'))
        self.assertEqual(self.problems(), '')
        self.assertIn('.buttons = { .pon = 0x013, .volume_up = 0x18d, .pon_name = "the PMK8350\'s power-on block", '
                      '.volume_up_name = "GPIO6 of the PM8350" },', boards.table_text(*boards.load()))

    def test_an_existing_board_is_not_overwritten(self):
        with self.assertRaises(SystemExit):
            boards.new_board('phone', 'sm0001')


class Packages(Sandbox):
    """Device packages kept somewhere else than devices/, and the tables written for what is there."""

    def package(self, directory, name, soc='sm0001'):
        (directory / name).mkdir(parents=True)
        (directory / name / 'board.toml').write_text(
            f'name = "{name}"\ndescription = "A package"\nsoc = "{soc}"\nfeatures = []\n[backlight]\ntype = "soft"\n')

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
            self.package(elsewhere, 'other', soc='sm9999')
            boards.MORE_DEVICES.append(elsewhere)
            self.assertIn(f'{elsewhere}/other/board.toml: soc = \'sm9999\': there is no soc/sm9999.toml', self.problems())
        finally:
            shutil.rmtree(elsewhere)

    def test_soc_files_are_found_elsewhere_too(self):
        more = self.dir / 'more-soc'
        more.mkdir()
        (more / 'sm0002.toml').write_text('name = "sm0002"\ndescription = "Another chip"\n')
        self.package(self.dir / 'devices', 'second', soc='sm0002')
        self.assertIn('there is no soc/sm0002.toml', self.problems())
        boards.MORE_SOCS.append(more)
        self.assertEqual(self.problems(), '')

    def test_the_tables_are_written_once_and_left_alone_while_nothing_changes(self):
        out = self.dir / 'out'
        socs, found = boards.load()
        self.assertEqual(sorted(boards.write_generated(out, socs, found)),
                         ['hakios_board_table.h', 'hakios_boards.h', 'platforms.h'])
        stamp = (out / 'hakios_board_table.h').stat().st_mtime_ns
        self.assertEqual(boards.write_generated(out, socs, found), [])
        self.assertEqual((out / 'hakios_board_table.h').stat().st_mtime_ns, stamp)   # nothing to recompile for
        # a board that says something new of its screen: user space's table, and only it
        path = self.dir / 'devices/phone/board.toml'
        path.write_text(path.read_text() + '[screen]\nstatusbar_leading_shift = 24\n')
        self.assertEqual(boards.write_generated(out, *boards.load()), ['hakios_boards.h'])


class Repository(unittest.TestCase):
    def tearDown(self):
        del boards.MORE_DEVICES[:], boards.MORE_SOCS[:]

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
        self.assertIn('no board "emulator" with a [boot] section', str(refused.exception))
        # two phones: the tool does not choose
        with self.assertRaises(SystemExit) as refused:
            boards.board_to_boot({**found, 'beta': plain})
        self.assertIn('which phone? --board NAME (here: alpha, beta)', str(refused.exception))
        with self.assertRaises(SystemExit) as refused:
            boards.board_to_boot({'emulator': {}})
        self.assertIn('no package with a [boot] section', str(refused.exception))
        # the board's arguments, one replaced in its place, new ones after them
        self.assertEqual(boards.boot_arguments(plain), ['gpu=0', 'cpufreq=1', '-v'])
        self.assertEqual(boards.boot_arguments(plain, ['gpu=1', 'cpus=1', 'thermal=2', 'cpus=2']),
                         ['gpu=1', 'cpufreq=1', '-v', 'cpus=2', 'thermal=2'])
        self.assertEqual(plain['boot']['bootargs'], ['gpu=0', 'cpufreq=1', '-v'])       # (the board is not changed)
        with self.assertRaises(SystemExit) as refused:
            boards.boot_arguments(plain, ['two words'])
        self.assertIn('"two words" is not NAME or NAME=VALUE', str(refused.exception))

    def test_every_feature_says_how_it_is_started_and_the_argument_is_one_the_kernel_reads(self):
        reference = (ROOT / 'docs/boot-args.md').read_text()
        for _, feature in boards.feature_bits():
            how = boards.FEATURE_BOOTARGS.get(feature)
            self.assertTrue(how, f'{feature} has no boot argument in FEATURE_BOOTARGS')
            for argument in re.findall(r'\b([a-z_]+)=', how):
                self.assertIn(f'| `{argument}` ', reference, f'{feature}: docs/boot-args.md has no {argument}=')
        self.assertEqual(sorted(set(boards.FEATURE_BOOTARGS) - {name for _, name in boards.feature_bits()}), [])

    def test_the_repository_names_no_phone(self):
        # phones are device packages, kept on the devices branch: what the repository tracks under
        # devices/ is the emulator's board and the templates
        tracked = subprocess.run(['git', 'ls-files', 'devices'], cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
        self.assertEqual(sorted({name.split('/')[1] for name in tracked}), ['README.md', '_template', 'qemu-virt'])

    def test_the_tests_phones_are_valid_packages(self):
        boards.MORE_DEVICES.append(ROOT / 'tests/devices')
        boards.MORE_SOCS.append(ROOT / 'tests/soc')
        problems = boards.Problems()
        socs, found = boards.load(problems)
        self.assertEqual(list(problems), [])
        for name in ('phone-cmd', 'phone-video', 'phone-keys'):
            self.assertIn(name, found)
            names = [argument.split('=')[0] for argument in found[name].get('boot', {}).get('bootargs', [])]
            self.assertEqual(len(names), len(set(names)))

    def test_the_tool_takes_packages_from_where_it_is_told(self):
        out = Path(tempfile.mkdtemp())
        try:
            for arguments, environment in ((['--devices', str(ROOT / 'tests/devices')], {}),
                                           ([], {'HAKIOS_DEVICES': str(ROOT / 'tests/devices')})):
                done = subprocess.run([str(ROOT / 'tools/boards.py'), '--soc', str(ROOT / 'tests/soc')] + arguments +
                                      ['gen', '--out', str(out)],
                                      capture_output=True, text=True, env={**os.environ, **environment})
                self.assertEqual(done.returncode, 0, done.stderr)
                self.assertIn('phone-cmd, phone-keys, phone-video', done.stdout)
                self.assertIn('.name = "phone-cmd"', (out / 'hakios_board_table.h').read_text())
        finally:
            shutil.rmtree(out)

    def test_the_page_of_what_each_feature_needs_is_what_the_checker_asks(self):
        # the page is the tool's own output
        self.assertEqual((ROOT / 'docs/features.md').read_text(), boards.features_text(),
                         'docs/features.md is not what tools/boards.py features prints: write it again')
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
        template = (root / 'devices/_template/board.toml').read_text()
        readme = (root / 'devices/README.md').read_text()
        drivers = (root / 'docs/drivers.md').read_text()
        for _, feature in boards.feature_bits():
            self.assertTrue(feature in template, f'the template does not list the feature {feature}')
            self.assertTrue(f'`{feature}`' in drivers, f'docs/drivers.md does not have the feature {feature}')
        for section, keys in boards.BOARD_SCHEMA.items():
            if section:
                self.assertTrue(f'[{section}]' in template, f'the template does not show [{section}]')
                self.assertTrue(f'`[{section}]`' in readme, f'devices/README.md does not name [{section}]')
            for key in keys:
                self.assertTrue(re.search(rf'(?m)^(# )?{key} = ', template), f'[{section}] {key} is not explained in the template')
        for section in boards.SOC_SCHEMA:
            if section:
                self.assertTrue(f'`[{section}]`' in readme, f'devices/README.md does not name the SoC section [{section}]')
        soc_template = (root / 'devices/_template/soc.toml').read_text()
        for section, keys in boards.SOC_SCHEMA.items():
            if section:
                self.assertTrue(f'[{section}]' in soc_template, f'the SoC template does not show [{section}]')
            for key in keys:
                self.assertTrue(re.search(rf'(?m)^{key} = ', soc_template), f'[{section}] {key} is not explained in the SoC template')
        # the template is a SoC file the tool takes as it is
        problems = boards.Problems()
        boards.check_file(root / 'devices/_template/soc.toml', tomllib.loads(soc_template), boards.SOC_SCHEMA, problems)
        self.assertEqual(list(problems), [])

    def test_the_loader_has_its_fallback(self):
        # the loader names platform_qemu_virt: the machine whose display matches no board
        self.assertIn('static const struct platform platform_qemu_virt = {', boards.platforms_text(*boards.load()))


if __name__ == '__main__':
    unittest.main(argv=sys.argv[:1] + ['-v'] if '-v' in sys.argv else sys.argv[:1])
