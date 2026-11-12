#!/usr/bin/env python3
"""The boards this project knows, as data: devices/NAME/board.dts and soc/SOC.dtsi.

  tools/boards.py list                 the boards, one a line
  tools/boards.py show BOARD           one board with its SoC's values filled in
  tools/boards.py status BOARD         what the board has on, what it could turn on, what the rest waits for
  tools/boards.py check                validate every file; non-zero exit and the reasons if one is wrong
  tools/boards.py gen [--out DIR]      write the tables the code is built with, for the boards that are there:
                                       the kernel's (hakios_board_table.h), the loader's (platforms.h) and
                                       user space's (hakios_boards.h), into DIR (build/boards). The builds
                                       and the tests run this themselves.
  tools/boards.py compare BOARD --dts FILE [--panel NODE]
                                       a board and its SoC file against an Android device tree: each value as
                                       in the tree, different, or not in a tree at all; non-zero if one differs
  tools/boards.py new NAME --soc SOC [--dts FILE [--panel NODE]]
                                       start devices/NAME/board.dts (and soc/SOC.dtsi if it is not there);
                                       with --dts, fill in what an Android device tree source says
  tools/boards.py convert FILE.toml...  a board.toml or SOC.toml of before as board.dts or SOC.dtsi, beside it,
                                       comments kept (the .toml is left for you to remove)

A board file says what a device has; a SoC file says where the blocks of its chip are, once for every
board with that chip. The kernel is built with one table of the boards that are there and picks its row
at boot by /chosen/darwin-board. docs/porting.md walks through adding a device.

The files are written as a device tree source is, the syntax a porter reads in the phone's Android tree
(tools/boardfile.py says what of it they take).

A board is a device package: a directory NAME with board.dts in it. Packages are looked for in
devices/ and in the directories of --devices DIR and of HAKIOS_DEVICES; SoC files in soc/ and --soc DIR.
The repository itself has the emulator's board and a template; phones are packages kept apart from it
(tools/device.py brings one in).
"""
import argparse
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import boardfile  # noqa: E402
from devicetree import from_dts  # noqa: E402  (what a device tree says of a board: tools/devicetree.py)

ROOT = Path(__file__).resolve().parents[1]
DEVICES = ROOT / 'devices'
SOCS = ROOT / 'soc'
BOARD_HEADER = ROOT / 'kernel/overlay/bsd/dev/hakios_board.h'
# What `gen` writes, into one directory (--out; build/boards unless told otherwise). None of it is kept
# in the repository: it is made from whichever device packages are there when something is built.
OUT = ROOT / 'build/boards'
KERNEL_TABLE = 'hakios_board_table.h'   # the kernel's table (bsd/dev/hakios_board.c includes it)
LOADER_TABLE = 'platforms.h'            # the loader's (loader/src/platform.h includes it)
USER_TABLE = 'hakios_boards.h'          # user space's (ios/userland/)
# Where else device packages and SoC files are looked for, besides devices/ and soc/: the directories of
# --devices and --soc, and of HAKIOS_DEVICES (as PATH is written). A package is DIR/NAME/board.dts.
MORE_DEVICES = []
MORE_SOCS = []

NAME = re.compile(r'^[a-z0-9][a-z0-9-]*$')



class U64(int):
    """A number of up to 64 bits (an address the loader works with), where int means 32."""


# key: (type, required). Types: str, int (32-bit), U64, bool, [int] and so on; a tuple of strings is a choice.
TSENS_MAX = 8   # HAKIOS_TSENS_MAX (kernel/overlay/bsd/dev/hakios_board.h)
SOC_SCHEMA = {
    '': {'name': (str, True), 'description': (str, True)},
    'platform': {'io_base': (U64, False), 'io_size': (U64, False), 'gic_dist': (U64, True), 'gic_redist': (U64, True),
                 'gic_redist_size': (U64, False), 'gic_redist_stride': (U64, False), 'uart': (U64, False),
                 'ufs': (U64, False),
                 'cores': (int, True), 'core_shift': (int, True), 'small_cores': (int, False),
                 'psci': (('smc', 'hvc'), True), 'timeout_scale': (int, False)},
    'clocks': {'gcc': (int, True), 'gcc_disp_ahb_cbcr': (int, False), 'dispcc': (int, False),
               'dispcc_mdss_gdscr': (int, False), 'dispcc_mdss_ahb_cbcr': (int, False),
               'dispcc_mdss_pclk0_cbcr': (int, False), 'dispcc_mdss_mdp_cbcr': (int, False)},
    'display': {'dsi0': (int, True), 'intf1': (int, False)},
    'thermal': {'tsens': ([int], True)},
    'spmi': {'core': (int, True), 'cnfg': (int, True), 'observer': (int, True), 'channels': (int, False),
             'ee': (int, False)},
    'adreno': {'gpu': (int, True), 'gmu': (int, True), 'gpucc': (int, True), 'cx_misc': (int, True), 'rscc': (int, True),
               'smmu': (int, True), 'pdc': (int, True), 'qfprom': (int, True), 'tsens': (int, False)},
    'pins': {'tlmm': (int, True)},
    'smmu': {'apps': (int, True), 'audio_stream': ([int], False), 'wifi_stream': ([int], False),
             'camera_stream': ([int], False)},
    'camera': {'camcc': (int, True), 'ife': (int, True), 'csid': (int, True), 'csiphy': (int, True)},
    'lpass': {'tx_macro': (int, True), 'va_macro': (int, True), 'tx_swr': (int, True), 'tx_swr_hctl': (int, True),
              'lpi': (int, True), 'lpi_slew': (int, True), 'swr_tx_pins': ([int], True), 'swr_tx_slew': ([int], True)},
    'ipc': {'aoss': (int, True), 'ipcc': (int, True), 'smem': (int, True), 'smem_size': (int, True),
            'smem_lock': (int, True)},
    'rpmh': {'cmd_db': (int, True), 'cmd_db_size': (int, True), 'rsc': (int, True), 'tcs_offset': (int, True)},
    'scmi_perf': {'shmem': (int, True), 'mailbox': (int, True), 'doorbell': (int, True)},
    'virtio': {'mmio': (int, True), 'transports': (int, True)},
    'cpufreq': {'epss': (int, True), 'epss_domain_size': (int, True), 'domain_cores': ([int], True)},
}
# a camera sensor's supply: a regulator of the sensors' PMIC at a set point, or one that a pin of the SoC switches
SUPPLY = re.compile(r'ldo([1-9]) (\d{3,4})mV|gpio(\d{1,3}) (\d{3,4})mV (\d{1,4})us')
# the supply a lens motor may need switched on: an RPMh regulator by its name in the command database, at a voltage
FOCUS_SUPPLY = re.compile(r'([a-z]{3,4}[a-z]?[0-9]{1,2}) (\d{3,4})mV')
# the sensor chips the camera driver knows (hakios_camera.c, chips[]: their registers, colour order and gains)
CAMERA_CHIPS = ('OV02B10', 'S5KHM2SD', 'OV16A1Q', 'S5K4H7')
CAMERA_SENSOR = {'name': (str, True), 'chip': (CAMERA_CHIPS, True), 'cci': ([int], True), 'pins': ([int], True),
                 'reset': (int, True), 'mclk': (int, True), 'mclk_pin': (int, True), 'csiphy': (int, True),
                 'vana': (SUPPLY, False), 'vdig': (SUPPLY, False), 'vio': (SUPPLY, False),
                 'focus_motor': (('dw9800',), False), 'focus_supply': (FOCUS_SUPPLY, False), 'upside_down': (bool, False)}
CAMERA_SENSORS_MOST = 4         # (struct hakios_board's room)
BOARD_SCHEMA = {
    '': {'name': (str, True), 'description': (str, True), 'soc': (str, True), 'features': ([str], True)},
    'backlight': {'type': (('dcs', 'soft', 'none'), True), 'panel_mode': (('command', 'video'), False),
                  'min': (int, False), 'max': (int, False), 'restore': (int, False), 'bytes': (int, False),
                  'byte_order': (('msb-first', 'lsb-first'), False)},
    'thermal': {'floor_mhz': ([int], False)},
    'torch': {'flash': (int, True), 'alarm': (int, True)},
    'haptic': {'sda': (int, True), 'scl': (int, True), 'reset': (int, True), 'interrupt': (int, True),
               'f0_calibration': (int, True)},
    'touch': {'serial_engine': (int, True), 'clocks': ([int], True), 'vote_register': (int, True), 'votes': ([int], True),
              'bus_pins': ([int], True),
              'interrupt': (int, True), 'reset': (int, True), 'io_supply': (str, True), 'analog_supply': (str, False),
              'analog_enable': (int, False), 'vendor_pins': ([int], False), 'coordinates': ([int], False)},
    'speaker': {'sda': (int, True), 'scl': (int, True), 'addresses': ([int], True), 'interrupts': ([int], True),
                'sound_pins': ([int], True), 'sound_function': (int, True), 'tdm': (int, False)},
    'buttons': {'pon': (int, True), 'volume_up': (int, True), 'pon_name': (str, False), 'volume_up_name': (str, False)},
    'adsp': {'firmware': (str, True), 'memory': (int, True), 'memory_size': (int, True),
             'charge_counter_divisor': (int, False)},
    'gpu': {'model': (str, True), 'chip_id': (int, True), 'firmware': (str, True), 'gmem_kb': (int, True),
            'primitive_fifo': (int, True), 'hang_cycles': (int, True), 'uche_cmdq_config': (int, False),
            'speed_fuse': (int, True), 'gaming_fuse': (int, True), 'highest_bank_bit': (int, True),
            'gcc_debug_gpucc': (int, True), 'zap_must_authenticate': (bool, False),
            'speeds_khz': ([int], True), 'speed_levels': ([int], True), 'default_speed': (int, True),
            'alt_model': (str, False), 'alt_bins': ([int], False)},
    'mic': {'codec_reset': (int, True)},
    'camera': {'pmic_pins': ([int], True), 'pmic_reset': (int, True), 'sensors': ([CAMERA_SENSOR], True), 'test_sensor': (str, False),
               'ios_sensor': (str, False), 'ios_front': (str, False)},
    'screen': {'statusbar_leading_shift': (int, True)},
    'wifi': {'radio_model': (int, True), 'early_pool_mb': (int, False), 'dsp_memory': (int, False),
             'board_data': (str, False), 'regdb': (str, False),
             'dsp_memory_size': (int, False), 'pins': ([int], False), 'supplies': ([str], False)},
    'storage': {'type_guid': (str, True), 'unique_guid': (str, True), 'first_lba': (U64, True), 'last_lba': (U64, True),
                'write_first': (U64, True)},
    'loader': {'fb_base': (U64, False), 'log_other_cpu': (bool, False), 'uefi_charger': (bool, False)},
    'boot': {'fastboot_product': (str, True), 'cpus': (int, True), 'bootargs': ([str], True)},
}
# every key and section name with an underscore, as a word: what as_written() writes with dashes
KEY_WITH_UNDERSCORE = re.compile(r'(?<![\w.])(' + '|'.join(sorted(
    {key for schema in (SOC_SCHEMA, BOARD_SCHEMA) for section, keys in schema.items() for key in list(keys) + [section]
     if '_' in key} | {key for key in CAMERA_SENSOR if '_' in key}, key=len, reverse=True)) + r')(?![\w(=])')
# what a feature needs of the SoC file: a board cannot list a feature its SoC has no addresses for
FEATURE_NEEDS = {'virtio': ('virtio',), 'cpufreq-scmi': ('scmi_perf',), 'cpufreq-epss': ('cpufreq',), 'display-scan': ('display',), 'buttons-pmic': ('spmi',),
                 'torch-pm8350c': ('spmi',), 'ddr-rpmh': ('rpmh',),
                 'camera-sm7325': ('spmi', 'pins', 'clocks', 'rpmh', 'smmu', 'camera'),
                 'gpu-a660': ('adreno', 'clocks', 'rpmh'),
                 # the part drivers that drive the SoC's pins, switch a bus's clocks on in the GCC, ask RPMh
                 # for a part's supply, and give a device its addresses in the SMMU
                 'touch-goodix': ('pins', 'clocks', 'rpmh'), 'touch-ft3658u': ('pins', 'clocks', 'rpmh'),
                 'speaker-aw88261': ('pins', 'clocks', 'rpmh', 'smmu'), 'haptic-aw86927': ('pins',),
                 'adsp': ('rpmh', 'ipc'), 'wifi-wcn6750': ('rpmh', 'pins', 'spmi', 'ipc', 'smmu'), 'mic-wcd938x': ('pins', 'lpass')}
# ... and the stream of the SMMU that a part's DMA comes in on: [stream ID, mask], from the device tree's iommus
STREAM_OF = {'speaker-aw88261': 'audio_stream', 'wifi-wcn6750': 'wifi_stream', 'camera-sm7325': 'camera_stream'}
WRITES_TO_PMIC = ('torch-pm8350c', 'camera-sm7325')    # ... these through the arbiter's channels as well
# The boot arguments that start a feature's driver, as a porter is to try them: the level that only looks
# first where there is one. (docs/boot-args.md has every argument; a test holds these names to it.)
FEATURE_BOOTARGS = {
    'cpufreq-scmi': 'scmi_perf=1',
    'cpufreq-epss': 'cpufreq=1',
    'ddr-rpmh': 'ddr=1 looks; ddr=2 with cpufreq=1 votes',
    'battery-uefi': 'battery=1',
    'buttons-pmic': 'buttons=1',
    'adsp': 'adsp=1',
    'display-scan': 'fb_scan=1, the default',
    'gpu-a660': 'gpu=1',
    'virtio': 'virtio_net=1, virtio_input=1',
    'touch-goodix': 'touchscreen=1',
    'touch-ft3658u': 'touchscreen=1',
    'speaker-aw88261': 'speaker_probe=N looks; sound=N with adsp=1',
    'haptic-aw86927': 'haptic=N',
    'torch-pm8350c': 'torch=1 looks; torch=3 is the light for iOS',
    'camera-sm7325': 'cam=N',
    'wifi-wcn6750': 'wifi=1',
    'mic-wcd938x': 'mic=N with adsp=1',
}
# features that are a driver of one part as one phone has it wired, by the first word of their names
PART_DRIVERS = ('touch', 'speaker', 'haptic', 'torch', 'camera', 'wifi', 'mic')
# ... and of the board's own file
FEATURE_NEEDS_OF_BOARD = {'gpu-a660': 'gpu', 'adsp': 'adsp', 'buttons-pmic': 'buttons', 'torch-pm8350c': 'torch',
                          'haptic-aw86927': 'haptic', 'speaker-aw88261': 'speaker', 'mic-wcd938x': 'mic', 'touch-goodix': 'touch',
                          'touch-ft3658u': 'touch', 'camera-sm7325': 'camera'}
# ... and what a controller's driver uses of [touch] beyond what every one needs
TOUCH_NEEDS = {'touch-goodix': ('analog_enable', 'vendor_pins', 'coordinates'), 'touch-ft3658u': ('analog_supply',)}
GPU_SPEEDS = 5          # (struct hakios_board: gpu.speeds_khz)


def shown(path):
    """A path as it is said to people: from the repository's top where it is inside it."""
    try:
        return Path(path).resolve().relative_to(ROOT.resolve())
    except ValueError:
        return Path(path)


def soc_files():
    """Every SoC file there is, soc/'s first: a name that is there twice is the first one's."""
    seen = {}
    for directory in [SOCS] + MORE_SOCS:
        for path in sorted(Path(directory).glob('*.dtsi')):
            seen.setdefault(path.stem, path)
    return [seen[name] for name in sorted(seen)]


def board_files():
    """Every device package's board file, devices/'s first (the template is not a board)."""
    seen = {}
    for directory in [DEVICES] + MORE_DEVICES:
        for path in sorted(Path(directory).glob('*/board.dts')):
            if not path.parent.name.startswith('_'):
                seen.setdefault(path.parent.name, path)
    return [seen[name] for name in sorted(seen)]


def as_written(text):
    """What the tool says, in the files' own terms: a section is a node (`speaker { }`), a key is written with
    dashes (`dsp-memory`), a camera's sensors are its sensor@N nodes."""
    text = re.sub(r'\[([a-z][a-z0-9_]*)\](?!\()', lambda m: f'{m[1].replace("_", "-")} {{ }}', text)
    return KEY_WITH_UNDERSCORE.sub(lambda m: m[0].replace('_', '-'), text)


class Problems(list):
    def add(self, where, what):
        self.append(as_written(f'{where}: {what}'))


def feature_bits():
    """The features the kernel knows, HAKIOS_HAS_* of hakios_board.h: [(bit, name as board.dts spells it)]."""
    found = re.findall(r'^\s*HAKIOS_HAS_([A-Z0-9_]+)\s*=\s*1u\s*<<\s*(\d+)', BOARD_HEADER.read_text(), re.M)
    return sorted((int(bit), name.lower().replace('_', '-')) for name, bit in found)


def features_of_header():
    """... as a dictionary from that spelling to the C name."""
    return {name: 'HAKIOS_HAS_' + name.upper().replace('-', '_') for _, name in feature_bits()}


# what of [clocks] the display's registers need before they can be touched: asked for by what touches them
DISPLAY_CLOCKS = ('gcc_disp_ahb_cbcr', 'dispcc', 'dispcc_mdss_gdscr', 'dispcc_mdss_ahb_cbcr')


def clocks_missing(soc, keys=DISPLAY_CLOCKS):
    """The keys of the SoC file's [clocks] that are not there, as text; '' when all are (or the section is not)."""
    clocks = soc.get('clocks')
    return ', '.join(key for key in keys if key not in clocks) if isinstance(clocks, dict) else ''


def feature_needs(feature, board, soc):
    """What a board that lists the feature must have and has not, in words; [] when it has everything."""
    soc_file = f'soc/{board.get("soc")}.dtsi'
    needs = []
    for section in FEATURE_NEEDS.get(feature, ()):
        if section not in soc:
            needs.append(f'[{section}] in {soc_file}')
    section = FEATURE_NEEDS_OF_BOARD.get(feature)
    if section and section not in board:
        needs.append(f'[{section}] in the board file')
    touch = board.get('touch')
    if isinstance(touch, dict) and any(key not in touch for key in TOUCH_NEEDS.get(feature, ())):
        needs.append(' and '.join(key for key in TOUCH_NEEDS[feature] if key not in touch) + ' in [touch] of the board file')
    spmi, display = soc.get('spmi'), soc.get('display')
    if feature in WRITES_TO_PMIC and isinstance(spmi, dict) and 'channels' not in spmi:
        needs.append(f'channels in [spmi] of {soc_file}')
    smmu = soc.get('smmu')
    if feature in STREAM_OF and isinstance(smmu, dict) and STREAM_OF[feature] not in smmu:
        needs.append(f'{STREAM_OF[feature]} in [smmu] of {soc_file}')
    if feature == 'display-scan' and isinstance(display, dict) and ('intf1' not in display or 'clocks' not in soc):
        needs.append(f'intf1 in [display] and [clocks] in {soc_file}')
    if feature == 'display-scan' and clocks_missing(soc):
        needs.append(f'{clocks_missing(soc)} in [clocks] of {soc_file}')
    if feature == 'gpu-a660' and clocks_missing(soc, ('gcc_disp_ahb_cbcr',)):
        needs.append(f'gcc_disp_ahb_cbcr in [clocks] of {soc_file}')
    radio = board.get('wifi')
    if feature == 'wifi-wcn6750' and not (isinstance(radio, dict) and 'dsp_memory' in radio and 'dsp_memory_size' in radio):
        needs.append('dsp_memory and dsp_memory_size in [wifi] of the board file')
    light = board.get('backlight')
    if feature == 'display-scan' and isinstance(light, dict) and light.get('panel_mode') == 'video':
        needs.append('a command-mode panel ([backlight] says video)')
    return needs


def feature_requirements(feature):
    """What feature_needs() asks for, as data: ({section of the SoC file: [keys it names beyond the section]},
    {section of the board file: [its keys]}). A test holds the two to each other."""
    of_soc = {section: [] for section in FEATURE_NEEDS.get(feature, ())}
    if feature in WRITES_TO_PMIC:
        of_soc.setdefault('spmi', []).append('channels')
    if feature in STREAM_OF:
        of_soc['smmu'].append(STREAM_OF[feature])
    if feature == 'display-scan':
        of_soc['display'].append('intf1')
        of_soc['clocks'] = list(DISPLAY_CLOCKS)
    if feature == 'gpu-a660':
        of_soc['clocks'].append('gcc_disp_ahb_cbcr')
    of_board = {}
    section = FEATURE_NEEDS_OF_BOARD.get(feature)
    if section:
        of_board[section] = [key for key, (_, needed) in BOARD_SCHEMA[section].items() if needed] + list(TOUCH_NEEDS.get(feature, ()))
    if feature == 'wifi-wcn6750':
        of_board['wifi'] = ['radio_model', 'dsp_memory', 'dsp_memory_size']
    return of_soc, of_board


def features_text():
    """docs/features.md: every feature a board can list, with what it needs written down first. Made from
    the tables above, which are the ones `check` holds a board to."""
    out = ['# What each feature needs',
           '',
           'A board lists the drivers it may start in `features` of its `board.dts`. Each needs to be told',
           'where things are first: sections of the SoC file (shared by every phone with that SoC) and, for a',
           'part on the board, a section of the board file. `tools/boards.py check` refuses a board that lists',
           'a feature without them, and `tools/boards.py status NAME` says the same for one phone.',
           '',
           'This page is written by `tools/boards.py features` from the tables the checker uses; a test keeps',
           'the two the same. What each key means is in `devices/_template/soc.dtsi` and',
           '`devices/_template/board.dts`.',
           '',
           '| Feature | SoC file | Board file | Started with |',
           '|---|---|---|---|']
    for _, feature in feature_bits():
        of_soc, of_board = feature_requirements(feature)
        cells = ['; '.join(f'`[{section}]`' + (': ' + ', '.join(f'`{key}`' for key in keys) if keys else '')
                           for section, keys in part.items()) or 'nothing' for part in (of_soc, of_board)]
        out.append(f'| `{feature}` | {cells[0]} | {cells[1]} | `{FEATURE_BOOTARGS.get(feature, "")}` |')
    out += ['',
            'The brightness (`[backlight]`), the thermal floors (`[thermal]`) and the partition that may be',
            'written (`[storage]`) are not features: a board has them by giving the section. `display-scan`',
            'is for a command-mode panel only.',
            '']
    return as_written('\n'.join(out))


def status_text(name, board, soc):
    """One board as a porter wants it: what is on, what could be, what each of the rest waits for."""
    listed = board.get('features', [])
    light = board.get('backlight', {})
    out = [f'{name}: {board["description"]} ({board["soc"]})', '']

    def line(what, state):
        out.append(f'  {what:18} {state}')

    line('loader', 'known by its framebuffer' if 'fb_base' in board.get('loader', {}) else
         "the loader's fallback (the emulator)" if 'loader' in board else
         'no [loader] section: started by another loader, or not yet (docs/porting.md, step 3)')
    line('backlight', {'dcs': 'the panel takes MIPI DCS brightness (backlight=1 looks, backlight=2 writes)',
                       'soft': 'the picture is darkened in software',
                       'none': 'none'}.get(light.get('type'), 'no [backlight] section'))
    line('thermal', 'its floors are set (thermal=1 looks, thermal=2 limits)' if 'thermal' in board else
         'no [thermal] section' + ('' if 'thermal' in soc and ('cpufreq' in soc or 'scmi_perf' in soc) else
                                   f': needs [thermal] and [cpufreq] or [scmi_perf] in soc/{board["soc"]}.dtsi first'))
    out.append('')
    for _, feature in feature_bits():
        needs = feature_needs(feature, board, soc)
        part = feature.split('-')[0] in PART_DRIVERS
        how = FEATURE_BOOTARGS.get(feature)
        if feature in listed:
            line(feature, 'on' + (': STILL NEEDS ' + ', '.join(needs) if needs else f' ({how})' if how else ''))
        elif feature == 'virtio':
            line(feature, "an emulator's devices: not for a phone")
        elif needs:
            line(feature, 'needs ' + ', '.join(needs))
        elif part:
            line(feature, "one part's driver: only if this phone has that part wired the same way (docs/drivers.md)")
        else:
            line(feature, 'can be listed: what it needs is there' + (f' (then {how})' if how else ''))
    out += ['', 'A feature that is on is started only with its boot argument, in brackets (docs/boot-args.md has',
            'them all); docs/porting.md, step 5, has the order to try them in and the log line that says each',
            'one worked.', '']
    return as_written('\n'.join(out))


def check_table(where, table, schema, problems):
    for key, value in table.items():
        if isinstance(value, dict):
            continue
        if key not in schema:
            problems.add(where, f'unknown key "{key}" (known: {", ".join(sorted(schema))})')
            continue
        kind = schema[key][0]
        if isinstance(kind, list) and isinstance(kind[0], dict):
            if not isinstance(value, list) or not value or any(not isinstance(item, dict) for item in value):
                problems.add(where, f'{key}: one node {key[:-1]}@0 {{ ... }} or more, not a value')
            else:
                for i, item in enumerate(value):
                    check_table(f'{where} {key}[{i}]', item, kind[0], problems)
        elif isinstance(kind, re.Pattern):
            if type(value) is not str or not kind.fullmatch(value):
                problems.add(where, f'{key} = {value!r}: not in the form {kind.pattern!r}')
        elif isinstance(kind, tuple):
            if value not in kind:
                problems.add(where, f'{key} = {value!r}: one of {", ".join(kind)}')
        elif isinstance(kind, list):
            if not isinstance(value, list) or any(type(v) is not kind[0] for v in value):
                problems.add(where, f'{key}: a list of {kind[0].__name__}')
            elif kind[0] is int and any(not 0 <= v <= 0xffffffff for v in value):
                problems.add(where, f'{key}: values of 32 bits')
        elif kind is U64:
            if type(value) is not int or not 0 <= value <= 0xffffffffffffffff:
                problems.add(where, f'{key} = {value!r}: an address expected')
        elif type(value) is not kind:
            problems.add(where, f'{key} = {value!r}: {kind.__name__} expected')
        elif kind is int and not 0 <= value <= 0xffffffff:
            problems.add(where, f'{key} = {value}: a value of 32 bits')
    for key, (kind, required) in schema.items():
        if required and key not in table:
            tables = isinstance(kind, list) and isinstance(kind[0], dict)
            problems.add(where, f'"{key}" is missing' + (f': a node {key[:-1]}@0 {{ ... }} at least' if tables else ''))


def check_file(path, data, schema, problems):
    rel = shown(path)
    check_table(rel, data, schema[''], problems)
    for section, value in data.items():
        if not isinstance(value, dict):
            continue
        if section not in schema:
            problems.add(rel, f'unknown section [{section}] (known: {", ".join(s for s in schema if s)})')
        else:
            check_table(f'{rel} [{section}]', value, schema[section], problems)


def supply_of(text):
    """A sensor's supply as the board says it: (kind, ldo, mV, gpio, ramp in us); kind 0 none, 1 an LDO, 2 a pin."""
    if text is None:
        return (0, 0, 0, 0, 0)
    found = SUPPLY.fullmatch(text)
    if found[1]:
        return (1, int(found[1]), int(found[2]), 0, 0)
    return (2, 0, int(found[4]), int(found[3]), int(found[5]))


def camera_problems(where, cameras, problems):
    """What a board's [camera] says that cannot be: the values are of the right kinds already."""
    sensors = cameras['sensors']
    pmic = cameras['pmic_pins']
    if len(pmic) != 2 or pmic[0] == pmic[1] or any(not 0 <= pin <= 255 for pin in pmic):
        problems.add(where, 'pmic_pins are the PMIC\'s bus: SDA then SCL, two different pins of the SoC, 0 to 255')
    if not 0x100 <= cameras['pmic_reset'] <= 0xfff or (cameras['pmic_reset'] & 0xff) < 0x88:
        problems.add(where, 'pmic_reset is a PMIC\'s GPIO as SPMI slave << 8 | its peripheral\'s high byte: GPIO n is 0x88 + n - 1')
    if len(sensors) > CAMERA_SENSORS_MOST:
        problems.add(where, f'{len(sensors)} sensors: the driver has room for {CAMERA_SENSORS_MOST}')
    names = [sensor['name'] for sensor in sensors]
    for i, sensor in enumerate(sensors):
        at = f'{where} sensors[{i}]'
        if not NAME.match(sensor['name']) or names.count(sensor['name']) > 1:
            problems.add(at, f'name = {sensor["name"]!r}: lower-case letters, digits and dashes, and no other sensor\'s')
        if len(sensor['cci']) != 2 or any(not 0 <= n <= 1 for n in sensor['cci']):
            problems.add(at, 'cci is the CCI and its master: [0 or 1, 0 or 1]')
        if len(sensor['pins']) != 2:
            problems.add(at, 'pins are the CCI master\'s SDA and SCL: two')
        if not 0 <= sensor['mclk'] <= 3 or not 0 <= sensor['csiphy'] <= 3:
            problems.add(at, 'mclk and csiphy are numbers of camcc\'s clocks and of the CSI receivers: 0 to 3')
        switches = [supply_of(sensor.get(key))[3] for key in ('vana', 'vdig', 'vio') if supply_of(sensor.get(key))[0] == 2]
        wires = list(sensor['pins']) + [sensor['reset'], sensor['mclk_pin']] + switches
        if any(not 0 <= pin <= 255 for pin in wires) or len(set(wires)) != len(wires):
            problems.add(at, 'its pins (the bus\'s two, reset, mclk_pin, a supply\'s switch) are different pins of the SoC, 0 to 255')
    for key in ('test_sensor', 'ios_sensor', 'ios_front'):
        if key in cameras and cameras[key] not in names:
            problems.add(where, f'{key} = {cameras[key]!r}: the name of one of its sensors ({", ".join(names)})')


def load(problems=None):
    """Every SoC and board, checked. Returns (socs, boards) by name; problems are collected or raised."""
    own = problems is None
    problems = Problems() if own else problems
    socs, boards = {}, {}
    for path in soc_files():
        try:
            data = boardfile.parse(path.read_text(), SOC_SCHEMA)
        except boardfile.FileError as error:
            problems.add(shown(path), str(error))
            continue
        check_file(path, data, SOC_SCHEMA, problems)
        if data.get('name') != path.stem:
            problems.add(shown(path), f'name = {data.get("name")!r}: the file is {path.stem}.dtsi')
        lpass = data.get('lpass')
        if isinstance(lpass, dict) and isinstance(lpass.get('swr_tx_pins'), list) and isinstance(lpass.get('swr_tx_slew'), list):
            pins, slew = lpass['swr_tx_pins'], lpass['swr_tx_slew']
            if not 2 <= len(pins) <= 4 or len(slew) != len(pins) or len(set(pins)) != len(pins) or \
                    any(not 0 <= pin < 16 for pin in pins) or any(bit > 30 or bit & 1 for bit in slew):
                problems.add(f'{shown(path)} [lpass]', 'swr_tx_pins are the TX SoundWire\'s clock and data pins of the LPASS '
                             '(two to four, 0 to 15), swr_tx_slew each one\'s field in the slew register (an even bit, 0 to 30)')
        if isinstance(lpass, dict) and isinstance(lpass.get('tx_swr_hctl'), int) and lpass['tx_swr_hctl'] & 3:
            problems.add(f'{shown(path)} [lpass]', 'tx_swr_hctl is a register: its address is a multiple of 4')
        smmu = data.get('smmu')
        if isinstance(smmu, dict):
            for key in ('audio_stream', 'wifi_stream', 'camera_stream'):
                if key in smmu and (len(smmu[key]) != 2 or any(not 0 <= v <= 0x7fff for v in smmu[key])):
                    problems.add(f'{shown(path)} [smmu]', f'{key} is a stream ID and its mask (the device tree\'s iommus), '
                                 'two numbers of 15 bits')
        socs[path.stem] = data
    known = features_of_header()
    for path in board_files():
        rel = shown(path)
        try:
            data = boardfile.parse(path.read_text(), BOARD_SCHEMA)
        except boardfile.FileError as error:
            problems.add(rel, str(error))
            continue
        name = path.parent.name
        before = len(problems)
        check_file(path, data, BOARD_SCHEMA, problems)
        well_formed = len(problems) == before   # (what follows compares values: only if they are of the right kind)
        if data.get('name') != name:
            problems.add(rel, f'name = {data.get("name")!r}: the directory is devices/{name}')
        if not NAME.match(name):
            problems.add(rel, 'a board is named in lower-case letters, digits and dashes')
        soc = socs.get(data.get('soc'))
        if soc is None:
            problems.add(rel, f'soc = {data.get("soc")!r}: there is no soc/{data.get("soc")}.dtsi')
            soc = {}
        for feature in data.get('features', []):
            if feature not in known:
                problems.add(rel, f'feature "{feature}" is not one the kernel knows ({", ".join(sorted(known))})')
            for need in feature_needs(feature, data, soc):
                problems.add(rel, f'feature "{feature}" needs {need}')
        boards[name] = data
        if not well_formed:
            continue
        light = data.get('backlight', {})
        if light.get('type') == 'dcs':
            for key in ('panel_mode', 'min', 'max', 'bytes'):
                if key not in light:
                    problems.add(f'{rel} [backlight]', f'type "dcs" needs "{key}"')
            if 'min' in light and 'max' in light and not light['min'] < light['max']:
                problems.add(f'{rel} [backlight]', 'min must be below max')
            if light.get('bytes') not in (None, 1, 2):
                problems.add(f'{rel} [backlight]', 'bytes is 1 or 2')
            if light.get('bytes') == 2 and 'byte_order' not in light:
                problems.add(f'{rel} [backlight]', 'two bytes need "byte_order"')
            if 'max' in light and light.get('bytes') in (1, 2) and light['max'] >= 1 << (8 * light['bytes']):
                problems.add(f'{rel} [backlight]', f'max does not fit in {light["bytes"]} byte(s)')
            for section in ('clocks', 'display'):
                if section not in soc:
                    problems.add(rel, f'backlight type "dcs" needs [{section}] in soc/{data.get("soc")}.dtsi')
            if clocks_missing(soc):
                problems.add(rel, f'backlight type "dcs" needs {clocks_missing(soc)} in [clocks] of soc/{data.get("soc")}.dtsi')
        motor = data.get('haptic')
        if motor is not None:
            wires = [motor[key] for key in ('sda', 'scl', 'reset', 'interrupt')]
            if any(not 0 <= pin <= 255 for pin in wires) or len(set(wires)) != len(wires):
                problems.add(f'{rel} [haptic]', 'sda, scl, reset and interrupt are four different pins of the SoC, 0 to 255')
            if not 0 <= motor['f0_calibration'] <= 0x3f:
                problems.add(f'{rel} [haptic]', 'f0_calibration is the six bits of the chip\'s trim register')
        written = data.get('storage')
        if written is not None:
            for key in ('type_guid', 'unique_guid'):
                if not GUID.fullmatch(written[key]):
                    problems.add(f'{rel} [storage]', f'{key} is a GUID as a partition table prints it: 8-4-4-4-12 hexadecimal digits')
            # (LBAs are the unit's blocks and so are the partition's: 4096 bytes on the storage this is for)
            if not 0 < written['first_lba'] < written['last_lba']:
                problems.add(f'{rel} [storage]', 'first_lba and last_lba are where the partition begins and ends: the first is not 0 and before the last')
            elif not 0 < written['write_first'] <= written['last_lba'] - written['first_lba']:
                problems.add(f'{rel} [storage]', 'write_first is a block of the partition, past what the build puts at its start: not 0, and inside it')
            if 'loader' not in data:
                problems.add(f'{rel} [storage]', 'the loader serves the writes: the board needs [loader] as well')
        cameras = data.get('camera')
        if cameras is not None:
            camera_problems(f'{rel} [camera]', cameras, problems)
        touch = data.get('touch')
        if touch is not None:
            wires = list(touch['bus_pins']) + [touch['interrupt'], touch['reset']] + list(touch.get('vendor_pins', [])) + \
                ([touch['analog_enable']] if 'analog_enable' in touch else [])
            if len(touch['bus_pins']) != 4 or len(touch.get('vendor_pins', [0, 0])) != 2 or \
                    any(not 0 <= pin <= 255 for pin in wires) or len(set(wires)) != len(wires):
                problems.add(f'{rel} [touch]', 'bus_pins are four, vendor_pins two; every pin is a different pin of the SoC, 0 to 255')
            if 'coordinates' in touch and (len(touch['coordinates']) != 2 or 0 in touch['coordinates']):
                problems.add(f'{rel} [touch]', 'coordinates are the largest x and y the controller reports: two numbers, not 0')
            if not 0 < touch['vote_register'] < 0x100000 or touch['vote_register'] & 3:
                problems.add(f'{rel} [touch]', 'vote_register is an offset in the clock controller: a register\'s, not 0')
            if len(touch['clocks']) != 3 or any(not 0 < offset < 0x100000 for offset in touch['clocks']) or \
                    len(set(touch['clocks'])) != 3:
                problems.add(f'{rel} [touch]', 'clocks are three different offsets in the clock controller: the two bus clocks, the engine\'s own')
            if len(touch['votes']) != 5 or any(not 0 <= bit <= 31 for bit in touch['votes']) or len(set(touch['votes'])) != 5:
                problems.add(f'{rel} [touch]', 'votes are five different bits of the vote register, 0 to 31')
            if touch['serial_engine'] == 0 or touch['serial_engine'] & 0xfff:
                problems.add(f'{rel} [touch]', 'serial_engine is where the engine\'s registers are: a page\'s address, not 0')
        amps = data.get('speaker')
        if amps is not None:
            wires = [amps['sda'], amps['scl']] + list(amps['interrupts']) + list(amps['sound_pins'])
            if len(amps['interrupts']) != 2 or len(amps['sound_pins']) != 4 or any(not 0 <= pin <= 255 for pin in wires) or \
                    len(set(wires)) != len(wires):
                problems.add(f'{rel} [speaker]', 'sda, scl, the two interrupts and the four sound_pins are eight different pins of the SoC, 0 to 255')
            if not 0 <= amps.get('tdm', 0) <= 4:
                problems.add(f'{rel} [speaker]', 'tdm is the TDM interface: 0 the primary, 1 the secondary, 2 the tertiary, 3 the quaternary, '
                             '4 the quinary')
            if not 1 <= amps['sound_function'] <= 15:
                problems.add(f'{rel} [speaker]', 'sound_function is one of a pin\'s functions, 1 to 15 (0 is the plain pin)')
            if len(amps['addresses']) != 2 or len(set(amps['addresses'])) != 2 or any(not 0x08 <= a <= 0x77 for a in amps['addresses']):
                problems.add(f'{rel} [speaker]', 'addresses are the two amplifiers\' different I2C addresses, 0x08 to 0x77')
        torch = data.get('torch')
        if torch is not None:
            for key in ('flash', 'alarm'):
                if not 0 < torch[key] <= 0xfff or torch[key] & 0xff == 0:
                    problems.add(f'{rel} [torch]', f'{key} = {torch[key]:#x}: SPMI slave << 8 | the high byte of the '
                                 "peripheral's registers (0x2ee: slave 2, registers 0xee00)")
        keys = data.get('buttons')
        if keys is not None:
            for key in ('pon', 'volume_up'):
                # slave << 8 | peripheral: sixteen slaves on the bus, and no peripheral 0 for a key
                if not 0 < keys[key] <= 0xfff or keys[key] & 0xff == 0:
                    problems.add(f'{rel} [buttons]', f'{key} = {keys[key]:#x}: SPMI slave << 8 | the high byte of the '
                                                     "peripheral's registers (0x18d: slave 1, registers 0x8d00)")
            if keys['pon'] == keys['volume_up']:
                problems.add(f'{rel} [buttons]', 'pon and volume_up are two peripherals')
        adsp = data.get('adsp')
        if adsp is not None:
            if adsp['memory_size'] == 0 or adsp['memory_size'] & 0xfff:
                problems.add(f'{rel} [adsp]', 'memory_size: whole pages, and not 0')
            if adsp['memory'] == 0 or adsp['memory'] & 0xfff:
                problems.add(f'{rel} [adsp]', 'memory: the address of the DSP\'s reserved memory, on a page, and not 0')
            if adsp.get('charge_counter_divisor', 1) == 0:
                problems.add(f'{rel} [adsp]', 'charge_counter_divisor must not be 0')
            if not re.fullmatch(r'[A-Za-z0-9._-]+', adsp['firmware']):
                problems.add(f'{rel} [adsp]', 'firmware: the name of a directory of the root, without slashes')
        gpu = data.get('gpu')
        if gpu is not None:
            speeds, levels = gpu['speeds_khz'], gpu['speed_levels']
            if not 1 <= len(speeds) <= GPU_SPEEDS:
                problems.add(f'{rel} [gpu]', f'speeds_khz: one to {GPU_SPEEDS} speeds')
            elif len(levels) != len(speeds):
                problems.add(f'{rel} [gpu]', 'speed_levels: one supply level for each of speeds_khz')
            elif speeds != sorted(set(speeds)) or 0 in speeds:
                problems.add(f'{rel} [gpu]', 'speeds_khz: from the slowest up, none of them 0')
            elif not 1 <= gpu['default_speed'] <= len(speeds):
                problems.add(f'{rel} [gpu]', f'default_speed: which of speeds_khz, 1 to {len(speeds)}')
            if not re.fullmatch(r'[A-Za-z0-9._-]+', gpu['firmware']):
                problems.add(f'{rel} [gpu]', 'firmware: the name of a directory of the small image, without slashes')
            if gpu['chip_id'] == 0:
                problems.add(f'{rel} [gpu]', 'chip_id must not be 0')
            if ('alt_model' in gpu) != ('alt_bins' in gpu) or not 1 <= len(gpu.get('alt_bins', [0])) <= 2:
                problems.add(f'{rel} [gpu]', 'alt_model goes with alt_bins, one or two speed bins')
        if 'mic' in data and not 0 <= data['mic']['codec_reset'] <= 255:
            problems.add(f'{rel} [mic]', 'codec_reset is a pin of the SoC, 0 to 255')
        radio = data.get('wifi')
        if radio is not None:
            if radio['radio_model'] not in (0, 1):
                problems.add(f'{rel} [wifi]', 'radio_model: 0 (a WCN6750 on the SoC\'s own bus) or 1 (a WCN6855 on PCIe)')
            if radio.get('early_pool_mb', 0) > 512:
                problems.add(f'{rel} [wifi]', 'early_pool_mb is in megabytes: some tens')
            if ('dsp_memory' in radio) != ('dsp_memory_size' in radio) or radio.get('dsp_memory', 0x1000) & 0xfff or \
                    radio.get('dsp_memory_size', 0x1000) & 0xfff or 0 in (radio.get('dsp_memory', 1), radio.get('dsp_memory_size', 1)):
                problems.add(f'{rel} [wifi]', 'dsp_memory and dsp_memory_size go together: an address and a size in whole pages')
            pins = radio.get('pins', [0, 1, 2])
            if len(pins) != 3 or len(set(pins)) != 3 or any(not 0 <= pin <= 255 for pin in pins):
                problems.add(f'{rel} [wifi]', 'pins are three different pins of the SoC, 0 to 255: the radio\'s enable, Bluetooth\'s reset, '
                             'Bluetooth\'s sw-ctrl')
            supplies = radio.get('supplies', ['a', 'b', 'c'])
            if len(supplies) != 3 or len(set(supplies)) != 3 or any(not re.fullmatch(r'[a-z0-9.]{1,7}', name) for name in supplies):
                problems.add(f'{rel} [wifi]', 'supplies are three different names of RPMh resources, as the command database has them '
                             '("smpb7": up to 7 lower-case letters, digits and dots): the radio\'s vdd-cx-mx, 1.8 V and 1.3 V')
        if 'virtio' in data.get('features', []) and len(data['features']) > 1:
            problems.add(rel, 'feature "virtio" is an emulator\'s: not beside the features of a phone')
        if data.get('screen', {}).get('statusbar_leading_shift', 0) > 200:
            problems.add(f'{rel} [screen]', 'statusbar_leading_shift is in points: some tens, not hundreds')
        for argument in data.get('boot', {}).get('bootargs', []):
            if not re.fullmatch(r'[A-Za-z0-9_-]+(=\S*)?', argument):
                problems.add(f'{rel} [boot]', f'bootargs: "{argument}" is not NAME or NAME=VALUE')
        named = [argument.split('=')[0] for argument in data.get('boot', {}).get('bootargs', [])]
        for argument in sorted({name for name in named if named.count(name) > 1}):
            problems.add(f'{rel} [boot]', f'bootargs: "{argument}" is there twice')
        if 'loader' in data:
            if 'platform' not in soc:
                problems.add(rel, f'[loader] needs [platform] in soc/{data.get("soc")}.dtsi')
            elif 'io_base' not in soc['platform'] or 'io_size' not in soc['platform']:
                # (no device tree has them, so they are asked for when the loader is to know the board)
                problems.add(rel, f'[loader] needs io_base and io_size in [platform] of soc/{data.get("soc")}.dtsi')
            elif soc['platform'].get('io_base') == 0:
                problems.add(rel, f'soc/{data.get("soc")}.dtsi [platform]: io_base must not be 0 (XNU then takes the '
                                  'machine for one without a SoC)')
        groups = soc.get('cpufreq', {}).get('domain_cores', [])
        if groups and (not 1 <= len(groups) <= 4 or any(not 1 <= n <= 255 for n in groups)):
            problems.add(rel, f'soc/{data.get("soc")}.dtsi [cpufreq]: domain_cores is one to four groups of at least one core')
        elif groups and 'platform' in soc and sum(groups) != soc['platform'].get('cores'):
            problems.add(rel, f'soc/{data.get("soc")}.dtsi: domain_cores adds up to {sum(groups)} cores, [platform] has '
                              f'{soc["platform"].get("cores")}')
        thermal = data.get('thermal')
        if thermal is not None:
            if 'cpufreq' in soc and 'floor_mhz' not in thermal:
                problems.add(f'{rel} [thermal]', 'floor_mhz: needed with EPSS (the SoC\'s [cpufreq])')
            if groups and len(thermal.get('floor_mhz', [])) != len(groups):
                problems.add(f'{rel} [thermal]', f'floor_mhz: one value for each of the {len(groups)} groups of cores '
                                                 f'(soc/{data.get("soc")}.dtsi [cpufreq] domain_cores)')
            if 'thermal' not in soc:
                problems.add(rel, f'[thermal] needs [thermal] in soc/{data.get("soc")}.dtsi')
            if 'cpufreq' not in soc and 'scmi_perf' not in soc:
                problems.add(rel, f'[thermal] needs [cpufreq] or [scmi_perf] in soc/{data.get("soc")}.dtsi')
            if len(soc.get('thermal', {}).get('tsens', [])) > TSENS_MAX:
                problems.add(rel, f'soc/{data.get("soc")}.dtsi: at most {TSENS_MAX} TSENS controllers')
    fallbacks = [name for name, data in boards.items() if 'loader' in data and 'fb_base' not in data['loader']]
    if len(fallbacks) > 1:
        problems.add('devices', 'more than one board with [loader] and no fb_base (' + ', '.join(fallbacks) +
                     '): the loader tells boards apart by it, and only the emulator goes without')
    seen = {}
    for name, data in boards.items():
        base = data.get('loader', {}).get('fb_base')
        if base is not None and seen.setdefault(base, name) != name:
            problems.add(f'devices/{name}/board.dts [loader]', f'fb_base {base:#x} is also that of {seen[base]}')
    if own and problems:
        raise SystemExit('\n'.join(problems))
    return socs, boards


GUID = re.compile(r'[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}')


def c_guid(text):
    """A GUID as a partition table prints it, as the initialiser of the loader's efi_guid: three numbers, then eight bytes."""
    a, b, c, d, e = text.lower().split('-')
    rest = ', '.join(f'0x{byte}' for byte in re.findall('..', d + e))
    return f'{{ 0x{a}, 0x{b}, 0x{c}, {{ {rest} }} }}'


def c_string(text):
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'


def table_text(socs, boards):
    known = features_of_header()
    out = ['/*',
           ' * GENERATED by tools/boards.py gen from devices/NAME/board.dts and soc/SOC.dtsi: do not edit.',
           ' * It has a row for every device package that was there when it was made (the build makes it',
           ' * anew each time); to change a board, change its file.',
           ' */',
           '#ifndef _HAKIOS_BOARD_TABLE_H',
           '#define _HAKIOS_BOARD_TABLE_H',
           '',
           '/* (struct hakios_board and the features are hakios_board.h, which bsd/dev/hakios_board.c includes first) */',
           '',
           'const struct hakios_board hakios_boards[] = {']
    for name in sorted(boards):
        board = boards[name]
        soc = socs[board['soc']]
        features = ' | '.join(known[f] for f in board['features']) or '0'
        clocks = soc.get('clocks', {})
        light = board.get('backlight', {})
        thermal = board.get('thermal', {})
        has_thermal = 'thermal' in board         # (a section may be empty: SCMI needs no floors)
        tsens = (soc.get('thermal', {}).get('tsens', []) + [0] * TSENS_MAX)[:TSENS_MAX] if has_thermal else [0] * TSENS_MAX
        cpufreq = soc.get('cpufreq', {})
        dcs = light.get('type') == 'dcs'
        out += [
            '\t{',
            f'\t\t.name = {c_string(name)},',
            f'\t\t.description = {c_string(board["description"])},',
            f'\t\t.soc = {c_string(board["soc"])},',
            f'\t\t.features = {features},',
        ]
        if clocks:
            out += [
                '\t\t.clocks = {',
                f'\t\t\t.gcc = {clocks["gcc"]:#010x}, .gcc_disp_ahb_cbcr = {clocks.get("gcc_disp_ahb_cbcr", 0):#x},',
                f'\t\t\t.dispcc = {clocks.get("dispcc", 0):#010x}, .dispcc_mdss_gdscr = {clocks.get("dispcc_mdss_gdscr", 0):#x},',
                f'\t\t\t.dispcc_mdss_ahb_cbcr = {clocks.get("dispcc_mdss_ahb_cbcr", 0):#x},',
            ]
            if 'dispcc_mdss_pclk0_cbcr' in clocks or 'dispcc_mdss_mdp_cbcr' in clocks:
                out.append(f'\t\t\t.dispcc_mdss_pclk0_cbcr = {clocks.get("dispcc_mdss_pclk0_cbcr", 0):#x}, '
                           f'.dispcc_mdss_mdp_cbcr = {clocks.get("dispcc_mdss_mdp_cbcr", 0):#x},')
            out.append('\t\t},')
        if 'intf1' in soc.get('display', {}):
            out.append(f'\t\t.display = {{ .intf = {soc["display"]["intf1"]:#010x} }},')
        kind = {'dcs': 'HAKIOS_BACKLIGHT_DCS', 'soft': 'HAKIOS_BACKLIGHT_SOFT'}.get(light.get('type'), 'HAKIOS_BACKLIGHT_NONE')
        if dcs:
            out += [
                '\t\t.backlight = {',
                f'\t\t\t.type = {kind}, .video_mode = {int(light["panel_mode"] == "video")}, .bytes = {light["bytes"]},',
                f'\t\t\t.lsb_first = {int(light.get("byte_order") == "lsb-first")}, .dsi = {soc["display"]["dsi0"]:#010x},',
                f'\t\t\t.raw_min = {light["min"]}, .raw_max = {light["max"]}, .raw_restore = {light.get("restore", 0)},',
                '\t\t},',
            ]
        else:
            out.append(f'\t\t.backlight = {{ .type = {kind} }},')
        virtio = soc.get('virtio')
        if virtio:
            out.append(f'\t\t.virtio = {{ .mmio = {virtio["mmio"]:#010x}, .transports = {virtio["transports"]} }},')
        scmi = soc.get('scmi_perf')
        if scmi:
            out.append(f'\t\t.scmi_perf = {{ .shmem = {scmi["shmem"]:#010x}, .mailbox = {scmi["mailbox"]:#010x}, '
                       f'.doorbell = {scmi["doorbell"]:#x} }},')
        if cpufreq:
            groups = ', '.join(str(n) for n in cpufreq['domain_cores'])
            out += ['\t\t.cpufreq = {',
                    f'\t\t\t.epss = {cpufreq["epss"]:#010x}, .epss_domain_size = {cpufreq["epss_domain_size"]:#x},',
                    f'\t\t\t.domain_cores = {{ {groups} }},',
                    '\t\t},']
        spmi = soc.get('spmi')
        if spmi:
            out.append(f'\t\t.spmi = {{ .core = {spmi["core"]:#010x}, .cnfg = {spmi["cnfg"]:#010x}, '
                       f'.observer = {spmi["observer"]:#010x}, .channels = {spmi.get("channels", 0):#010x}, '
                       f'.ee = {spmi.get("ee", 0)} }},')
        adreno = soc.get('adreno')
        if adreno:
            out += ['\t\t.adreno = {',
                    f'\t\t\t.gpu = {adreno["gpu"]:#010x}, .gmu = {adreno["gmu"]:#010x}, .gpucc = {adreno["gpucc"]:#010x},',
                    f'\t\t\t.cx_misc = {adreno["cx_misc"]:#010x}, .rscc = {adreno["rscc"]:#010x}, .smmu = {adreno["smmu"]:#010x},',
                    f'\t\t\t.pdc = {adreno["pdc"]:#010x}, .qfprom = {adreno["qfprom"]:#010x}, .tsens = {adreno.get("tsens", 0):#010x},',
                    '\t\t},']
        pins = soc.get('pins')
        if pins:
            out.append(f'\t\t.pins = {{ .tlmm = {pins["tlmm"]:#010x} }},')
        camera = soc.get('camera')
        if camera:
            out.append(f'\t\t.camera = {{ .camcc = {camera["camcc"]:#010x}, .ife = {camera["ife"]:#010x}, '
                       f'.csid = {camera["csid"]:#010x}, .csiphy = {camera["csiphy"]:#010x} }},')
        lpass = soc.get('lpass')
        if lpass:
            out += ['\t\t.lpass = {',
                    f'\t\t\t.tx_macro = {lpass["tx_macro"]:#010x}, .va_macro = {lpass["va_macro"]:#010x},',
                    f'\t\t\t.tx_swr = {lpass["tx_swr"]:#010x}, .tx_swr_hctl = {lpass["tx_swr_hctl"]:#010x},',
                    f'\t\t\t.lpi = {lpass["lpi"]:#010x}, .lpi_slew = {lpass["lpi_slew"]:#010x},',
                    f'\t\t\t.swr_tx_pin_count = {len(lpass["swr_tx_pins"])}, '
                    f'.swr_tx_pins = {{ {", ".join(str(v) for v in lpass["swr_tx_pins"])} }}, '
                    f'.swr_tx_slew = {{ {", ".join(str(v) for v in lpass["swr_tx_slew"])} }},',
                    '\t\t},']
        if 'smmu' in soc:
            streams = ''.join(f', .{key} = {{ {soc["smmu"][key][0]:#x}, {soc["smmu"][key][1]:#x} }}'
                              for key in ('audio_stream', 'wifi_stream', 'camera_stream') if key in soc['smmu'])
            out.append(f'\t\t.smmu = {{ .apps = {soc["smmu"]["apps"]:#010x}{streams} }},')
        ipc = soc.get('ipc')
        if ipc:
            out.append(f'\t\t.ipc = {{ .aoss = {ipc["aoss"]:#010x}, .ipcc = {ipc["ipcc"]:#010x}, .smem = {ipc["smem"]:#010x}, '
                       f'.smem_size = {ipc["smem_size"]:#x}, .smem_lock = {ipc["smem_lock"]:#010x} }},')
        rpmh = soc.get('rpmh')
        if rpmh:
            out.append(f'\t\t.rpmh = {{ .cmd_db = {rpmh["cmd_db"]:#010x}, .cmd_db_size = {rpmh["cmd_db_size"]:#x}, '
                       f'.rsc = {rpmh["rsc"]:#010x}, .tcs_offset = {rpmh["tcs_offset"]:#x} }},')
        keys = board.get('buttons')
        if keys:
            names = ''.join(f', .{key} = {c_string(keys[key])}' for key in ('pon_name', 'volume_up_name') if key in keys)
            out.append(f'\t\t.buttons = {{ .pon = {keys["pon"]:#05x}, .volume_up = {keys["volume_up"]:#05x}{names} }},')
        motor = board.get('haptic')
        if motor:
            out.append(f'\t\t.haptic = {{ .sda = {motor["sda"]}, .scl = {motor["scl"]}, .reset = {motor["reset"]}, '
                       f'.interrupt = {motor["interrupt"]}, .f0_calibration = {motor["f0_calibration"]:#04x} }},')
        if board.get('mic'):
            out.append(f'\t\t.mic = {{ .wired = 1, .codec_reset = {board["mic"]["codec_reset"]} }},')
        own = board.get('storage')
        if own:
            out.append(f'\t\t.storage = {{ .first_lba = {own["first_lba"]}ULL, .blocks = {own["last_lba"] - own["first_lba"] + 1}ULL, '
                       f'.write_first = {own["write_first"]}ULL }},')
        touch = board.get('touch')
        if touch:
            pins, clocks = touch['bus_pins'], touch['clocks']
            straps = touch.get('vendor_pins', [-1, -1])
            supplies = f'.io_supply = {c_string(touch["io_supply"])}' + (
                f', .analog_supply = {c_string(touch["analog_supply"])}' if 'analog_supply' in touch else '')
            out += [
                '\t\t.touch = {',
                f'\t\t\t.serial_engine = {touch["serial_engine"]:#010x},',
                f'\t\t\t.m_ahb_clock = {clocks[0]:#x}, .s_ahb_clock = {clocks[1]:#x}, .se_clock = {clocks[2]:#x},',
                f'\t\t\t.vote_register = {touch["vote_register"]:#x}, .vote = {{ {", ".join(str(bit) for bit in touch["votes"])} }},',
                f'\t\t\t.data_in = {pins[0]}, .data_out = {pins[1]}, .clock = {pins[2]}, .chip_select = {pins[3]},',
                f'\t\t\t.interrupt = {touch["interrupt"]}, .reset = {touch["reset"]},',
                f'\t\t\t.analog_enable = {touch.get("analog_enable", -1)}, .vendor_pin = {{ {straps[0]}, {straps[1]} }},',
                f'\t\t\t{supplies},',
                f'\t\t\t.max_x = {touch.get("coordinates", [0, 0])[0]}, .max_y = {touch.get("coordinates", [0, 0])[1]},',
                '\t\t},',
            ]
        amps = board.get('speaker')
        if amps:
            out.append(f'\t\t.speaker = {{ .sda = {amps["sda"]}, .scl = {amps["scl"]}, '
                       f'.address = {{ {amps["addresses"][0]:#04x}, {amps["addresses"][1]:#04x} }}, '
                       f'.interrupt = {{ {amps["interrupts"][0]}, {amps["interrupts"][1]} }},\n\t\t\t'
                       f'.sound_pin = {{ {", ".join(str(pin) for pin in amps["sound_pins"])} }}, '
                       f'.sound_function = {amps["sound_function"]}, .tdm = {amps.get("tdm", 0)} }},')
        torch = board.get('torch')
        if torch:
            out.append(f'\t\t.torch = {{ .flash = {torch["flash"]:#05x}, .temp_alarm = {torch["alarm"]:#05x} }},')
        adsp = board.get('adsp')
        if adsp:
            out.append(f'\t\t.adsp = {{ .firmware = {c_string(adsp["firmware"])}, .memory = {adsp["memory"]:#010x}, '
                       f'.memory_size = {adsp["memory_size"]:#010x}, '
                       f'.charge_counter_divisor = {adsp.get("charge_counter_divisor", 1)} }},')
        radio = board.get('wifi')
        if radio:
            out.append(f'\t\t.wifi = {{ .radio_model = {radio["radio_model"]}, .early_pool_mb = {radio.get("early_pool_mb", 0)}, '
                       f'.dsp_memory = {radio.get("dsp_memory", 0):#010x}, .dsp_memory_size = {radio.get("dsp_memory_size", 0):#010x},\n\t\t\t'
                       f'.pin = {{ {", ".join(str(pin) for pin in radio.get("pins", [-1, -1, -1]))} }}, '
                       f'.supply = {{ {", ".join(f'"{name}"' for name in radio.get("supplies", ["", "", ""]))} }},\n\t\t\t'
                       f'.board_data = {c_string(radio.get("board_data", "board.bin"))}, .regdb = {c_string(radio.get("regdb", "regdb.bin"))} }},')
        cameras = board.get('camera')
        if cameras:
            names = [sensor['name'] for sensor in cameras['sensors']]
            out += ['\t\t.cameras = {',
                    f'\t\t\t.pmic_sda = {cameras["pmic_pins"][0]}, .pmic_scl = {cameras["pmic_pins"][1]}, '
                    f'.pmic_reset = {cameras["pmic_reset"]:#05x}, '
                    f'.sensor_count = {len(names)},',
                    f'\t\t\t.test_sensor = {names.index(cameras.get("test_sensor", names[0]))}, '
                    f'.ios_sensor = {names.index(cameras.get("ios_sensor", names[0]))}, '
                    f'.ios_front = {names.index(cameras["ios_front"]) if "ios_front" in cameras else "0xff"},',
                    '\t\t\t.sensors = {']
            for sensor in cameras['sensors']:
                supplies = ', '.join(f'.{key} = {{ {", ".join(str(v) for v in supply_of(sensor.get(key)))} }}'
                                     for key in ('vana', 'vdig', 'vio'))
                out += [f'\t\t\t\t{{ .name = {c_string(sensor["name"])}, .chip = {c_string(sensor["chip"])}, '
                        f'.cci = {sensor["cci"][0]}, .cci_master = {sensor["cci"][1]}, '
                        f'.sda = {sensor["pins"][0]}, .scl = {sensor["pins"][1]}, .reset = {sensor["reset"]},',
                        f'\t\t\t\t  .mclk = {sensor["mclk"]}, .mclk_pin = {sensor["mclk_pin"]}, .csiphy = {sensor["csiphy"]}, '
                        f'.focus_motor = {int("focus_motor" in sensor)}, .upside_down = {int(sensor.get("upside_down", False))}, '
                        f'.focus_supply = {c_string(FOCUS_SUPPLY.fullmatch(sensor["focus_supply"])[1] if "focus_supply" in sensor else "")}, '
                        f'.focus_mv = {FOCUS_SUPPLY.fullmatch(sensor["focus_supply"])[2] if "focus_supply" in sensor else 0},',
                        f'\t\t\t\t  {supplies} }},']
            out += ['\t\t\t},', '\t\t},']
        gpu = board.get('gpu')
        if gpu:
            out += [
                '\t\t.gpu = {',
                f'\t\t\t.model = {c_string(gpu["model"])}, .firmware = {c_string(gpu["firmware"])},',
            ]
            if 'alt_model' in gpu:
                bins = (gpu['alt_bins'] * 2)[:2]        # (one bin: twice, an empty place would be bin 0)
                out.append(f'\t\t\t.alt_model = {c_string(gpu["alt_model"])}, '
                           f'.alt_bins = {{ {bins[0]:#x}, {bins[1]:#x} }},')
            out += [
                f'\t\t\t.chip_id = {gpu["chip_id"]:#010x}, .gmem_bytes = {gpu["gmem_kb"]} * 1024,',
                f'\t\t\t.primitive_fifo = {gpu["primitive_fifo"]:#010x}, .hang_cycles = {gpu["hang_cycles"]:#x}, '
                f'.uche_cmdq_config = {gpu.get("uche_cmdq_config", 0):#x},',
                f'\t\t\t.speed_fuse = {gpu["speed_fuse"]:#x}, .gaming_fuse = {gpu["gaming_fuse"]:#x}, '
                f'.highest_bank_bit = {gpu["highest_bank_bit"]},',
                f'\t\t\t.gcc_debug_gpucc = {gpu["gcc_debug_gpucc"]:#x}, '
                f'.zap_must_authenticate = {int(gpu.get("zap_must_authenticate", False))},',
                f'\t\t\t.speed_count = {len(gpu["speeds_khz"])}, .default_speed = {gpu["default_speed"]},',
                f'\t\t\t.speeds_khz = {{ {", ".join(str(v) for v in gpu["speeds_khz"])} }},',
                f'\t\t\t.speed_levels = {{ {", ".join(str(v) for v in gpu["speed_levels"])} }},',
                '\t\t},',
            ]
        if has_thermal:
            floors = ', '.join(str(v) for v in thermal.get('floor_mhz', [])) or '0'
            out += [
                '\t\t.thermal = {',
                f'\t\t\t.tsens = {{ {", ".join(f"{v:#010x}" for v in tsens)} }},',
                f'\t\t\t.floor_mhz = {{ {floors} }},',
                '\t\t},',
            ]
        out.append('\t},')
    bits = dict(feature_bits())
    names = ', '.join(c_string(bits.get(bit, '?')) for bit in range(max(bits) + 1))
    out += ['};',
            '',
            'const unsigned hakios_board_count = sizeof(hakios_boards) / sizeof(hakios_boards[0]);',
            '',
            '/* the features by their bit, as board.dts spells them */',
            f'const char *const hakios_feature_names[] = {{ {names} }};',
            'const unsigned hakios_feature_count = sizeof(hakios_feature_names) / sizeof(hakios_feature_names[0]);',
            '/* ... and room for all of them in one line, a space between two and the end */',
            f'#define HAKIOS_FEATURES_TEXT {sum(len(bits.get(bit, "?")) + 1 for bit in range(max(bits) + 1))}',
            '',
            '#endif /* _HAKIOS_BOARD_TABLE_H */',
            '']
    return '\n'.join(out)


def platforms_text(socs, boards):
    """platforms.h: the loader's description of every board with a [loader] section."""
    out = ['/*',
           ' * GENERATED by tools/boards.py gen from devices/NAME/board.dts ([loader]) and soc/SOC.dtsi',
           ' * ([platform]): do not edit. Included by src/platform.h of the loader, which has struct platform',
           ' * and says what each field is for.',
           ' */',
           '']
    listed = []
    for name in sorted(boards):
        board = boards[name]
        if 'loader' not in board:
            continue
        loader, platform = board['loader'], socs[board['soc']]['platform']
        symbol = 'platform_' + name.replace('-', '_')
        fields = [f'.name = {c_string(name)}',
                  f'.io_base = {platform["io_base"]:#010x}', f'.io_size = {platform["io_size"]:#010x}',
                  f'.gic_dist = {platform["gic_dist"]:#010x}', f'.gic_redist = {platform["gic_redist"]:#010x}']
        for key in ('gic_redist_size', 'gic_redist_stride', 'uart', 'ufs'):
            if key in platform:
                fields.append(f'.{key} = {platform[key]:#010x}')
        fields += [f'.psci_hvc = {int(platform["psci"] == "hvc")}',
                   f'.log_other_cpu = {int(loader.get("log_other_cpu", False))}',
                   f'.uefi_charger = {int(loader.get("uefi_charger", False))}',
                   f'.cores = {platform["cores"]}', f'.core_shift = {platform["core_shift"]}',
                   f'.timeout_scale = {platform.get("timeout_scale", 0)}',
                   f'.small_cores = {platform.get("small_cores", 0):#x}',
                   f'.fb_base = {loader.get("fb_base", 0):#010x}']
        own = board.get('storage')
        if own:
            fields.append(f'.own = {{ .type = {c_guid(own["type_guid"])}, .unique = {c_guid(own["unique_guid"])},\n'
                          f'\t\t.first_lba = {own["first_lba"]}ULL, .last_lba = {own["last_lba"]}ULL, '
                          f'.write_first = {own["write_first"]}ULL }}')
        out += [f'/* {board["description"]} ({board["soc"]}) */',
                f'static const struct platform {symbol} = {{']
        out += [f'\t{field},' for field in fields]
        out += ['};', '']
        if 'fb_base' in loader:
            listed.append(symbol)
    out += ['/* the boards told apart by the framebuffer their firmware reports; one that matches none is the emulator */',
            'static const struct platform *const platforms[] = {']
    out += [f'\t&{symbol},' for symbol in listed]
    out += ['};', '']
    return '\n'.join(out)


def user_boards_text(socs, boards):
    """hakios_boards.h: what the libraries injected into iOS's processes know of each board."""
    out = ['/*',
           ' * GENERATED by tools/boards.py gen from devices/NAME/board.dts ([screen]): do not edit.',
           ' * What user space is told of a board. The libraries of ios/userland/ read the board\'s name from',
           ' * IODeviceTree:/chosen/darwin-board, as the loader set it, and look it up here.',
           ' */',
           '#ifndef _HAKIOS_BOARDS_H',
           '#define _HAKIOS_BOARDS_H',
           '',
           'struct hakios_user_board {',
           '\tconst char *name;',
           '\tint statusbar_leading_shift;    /* points the status bar\'s leading items move right, past a camera there */',
           '};',
           '',
           'static const struct hakios_user_board hakios_user_boards[] = {']
    for name in sorted(boards):
        screen = boards[name].get('screen', {})
        out.append(f'\t{{ {c_string(name)}, {screen.get("statusbar_leading_shift", 0)} }},')
    out += ['};',
            '',
            '#endif /* _HAKIOS_BOARDS_H */',
            '']
    return '\n'.join(out)


def generated(socs, boards):
    """What gen writes: {file name: text}."""
    return {KERNEL_TABLE: table_text(socs, boards), LOADER_TABLE: platforms_text(socs, boards),
            USER_TABLE: user_boards_text(socs, boards)}


def write_generated(out, socs, boards):
    """Write the tables into the directory `out`; a file that is what it would be is left alone, so that
    nothing is recompiled for it. Returns the names of the files that were written."""
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    written = []
    for name, text in generated(socs, boards).items():
        path = out / name
        if not path.exists() or path.read_text() != text:
            path.write_text(text)
            written.append(name)
    return written


def board_to_boot(boards, name=None):
    """The board a boot tool is to start, as (name, board): the one named, or the only package here that has
    a boot { } section. A SystemExit that says what there is otherwise."""
    bootable = {n: board for n, board in boards.items() if 'boot' in board}
    if name is None:
        if len(bootable) != 1:
            raise SystemExit('which phone? --board NAME (here: ' + (', '.join(sorted(bootable)) or 'no package with a '
                             'boot { } section; a phone is brought in with tools/device.py add NAME') + ')')
        name = next(iter(bootable))
    if name not in bootable:
        raise SystemExit(f'no board "{name}" with a boot {{ }} section here (tools/boards.py list); a phone is brought in '
                         f'with tools/device.py add {name}')
    return name, bootable[name]


def boot_arguments(board, more=()):
    """The board's boot arguments of a plain run with `more` (NAME or NAME=VALUE each) added: one that
    names an argument the board has takes its place, the others follow in the order given."""
    arguments = {argument.split('=', 1)[0]: argument for argument in board['boot']['bootargs']}
    for argument in more:
        if not re.fullmatch(r'[A-Za-z0-9_-]+(=\S*)?', argument):
            raise SystemExit(f'"{argument}" is not NAME or NAME=VALUE')
        arguments[argument.split('=', 1)[0]] = argument
    return list(arguments.values())


def dts_value(key, value, number=None):
    """A value as a board file writes it; a SoC file's numbers as soc_value() writes them."""
    if number is not None and isinstance(value, (int, list)) and not isinstance(value, bool) and \
            not (isinstance(value, list) and value and isinstance(value[0], str)):
        return boardfile.value_text(value, lambda v: number(key, v))
    return boardfile.value_text(value) if value is not True else '(set)'


def show_text(data, depth=0):
    """A board or SoC as it was read, in the files' syntax (show)."""
    out, pad = [], '\t' * depth
    for key, value in data.items():
        if isinstance(value, dict):
            out += [f'{pad}{boardfile.name_of(key)} {{', show_text(value, depth + 1).rstrip('\n'), f'{pad}}};']
        elif isinstance(value, list) and value and isinstance(value[0], dict):
            singular = key[:-1] if key.endswith('s') else key
            for i, item in enumerate(value):
                out += [f'{pad}{boardfile.name_of(singular)}@{i} {{', show_text(item, depth + 1).rstrip('\n'), f'{pad}}};']
        elif value is not False:
            out.append(pad + boardfile.property_line(key, value))
    return '\n'.join(line for line in out if line) + '\n'


COUNTS = ('cores', 'core_shift', 'ee', 'timeout_scale', 'domain_cores')     # numbers a person counts in


def soc_number(key, value):
    """A number of a SoC file as a person writes it: addresses in eight hex digits, offsets and masks in
    hex, counts in decimal."""
    if key in COUNTS:
        return str(value)
    return f'{value:#010x}' if value >= 0x10000 else f'{value:#x}'


def soc_value(key, value):
    return boardfile.value_text(value, lambda v: soc_number(key, v))


class Writer:
    """A board or SoC file being written: comments, nodes and properties, indented with tabs, a value's
    comment at the files' column."""

    def __init__(self):
        self.lines, self.open_nodes = [], []

    def comment(self, *texts):
        depth = len(self.open_nodes)
        self.lines += ['\t' * depth + ('// ' + text if text else '//') for text in texts]

    def blank(self):
        self.lines.append('')

    def node(self, name, commented=False):
        self.lines.append('\t' * len(self.open_nodes) + ('// ' if commented else '') + name + ' {')
        self.open_nodes.append(commented)

    def end(self):
        while self.lines and self.lines[-1] == '':
            self.lines.pop()
        commented = self.open_nodes.pop()
        self.lines.append('\t' * len(self.open_nodes) + ('// ' if commented else '') + '};')

    def prop(self, text, said=(), commented=None):
        """A property, as `name = <...>;` text; said: the lines of the comment after it."""
        commented = self.open_nodes[-1] if commented is None and self.open_nodes else bool(commented)
        indent = '\t' * len(self.open_nodes)
        line = indent + ('// ' if commented else '') + text
        for i, more in enumerate(said):
            if i:
                line = indent + ('//' if commented else '')
            width = len(line.expandtabs(8))
            self.lines.append(line + ' ' * max(1, boardfile.COMMENT_COLUMN - width) + '// ' + more)
        if not said:
            self.lines.append(line)

    def text(self):
        return '/dts-v1/;\n\n' + '\n'.join(self.lines).rstrip() + '\n'


def template_says(template):
    """What the template of a SoC file says of each section and key: {section: the comment lines above
    its node}, {(section, key): the comment after the value, with the lines that continue it}."""
    above, after, section, pending, last = {}, {}, '', [], None
    for line in template.read_text().splitlines() if template.exists() else []:
        text = line.strip()
        header = re.fullmatch(r'([\w-]+) \{', text)
        value = re.match(r'([\w-]+) = [^;]*;\s*//\s?(.*)', text)
        if header:
            section, last = boardfile.key_of(header[1]), None
            above[section], pending = pending, []
        elif value:
            last = (section, boardfile.key_of(value[1]))
            after[last], pending = [value[2]], []
        elif text.startswith('//') and last and len(line.expandtabs(8)) - len(line.expandtabs(8).lstrip()) >= 24:
            after[last].append(text[2:].strip())                  # a comment that goes on under the value's
        elif text.startswith('//'):
            pending, last = pending + [text[2:].strip()], None
        else:
            pending, last = [], None
    return above, after


def soc_section(out, section, values, above, after, keys=None, more=(), commented=False):
    """A section of a new SoC file: each value with what the template says of its key; a key it has no
    value for is left commented out, to be filled in. more: (key, value, comment) to offer commented out.
    commented: the whole section, for one that cannot be complete from a tree and is needed by one feature only."""
    out.comment(*above.get(section, []))
    out.node(boardfile.name_of(section), commented=commented)
    for key, value, why in more:
        out.prop(f'{boardfile.name_of(key)} = {soc_value(key, value)};', [why], commented=True)
    for key in keys or values:
        name = boardfile.name_of(key)
        if key in values:
            out.prop(f'{name} = {soc_value(key, values[key])};', after.get((section, key), []))
        else:
            said = after.get((section, key), [''])
            out.prop(f'{name} = <0x...>;', [('TO FILL IN: ' + said[0]).rstrip(': ')], commented=True)
    out.end()
    out.blank()


def new_board(name, soc_name, dts=None, panel=None, description=None):
    if not NAME.match(name) or not NAME.match(soc_name):
        raise SystemExit('names are lower-case letters, digits and dashes')
    board_path = DEVICES / name / 'board.dts'
    soc_path = SOCS / f'{soc_name}.dtsi'
    if board_path.exists():
        raise SystemExit(f'{shown(board_path)} is there already')
    soc, board, notes = from_dts(dts.read_text(errors='replace'), panel) if dts else ({}, {}, [])
    started = 'Started by tools/boards.py new' + (f' from {dts.name}' if dts else '') + '.'
    out = Writer()
    out.comment(f'{description or name}. {started}',
                'Every value here is to be checked against the device before it is relied on (docs/porting.md).')
    out.node('/')
    out.prop(f'name = "{name}";', ['what the loader puts in /chosen/darwin-board'])
    out.prop(f'description = {boardfile.value_text(description or name)};')
    out.prop(f'soc = "{soc_name}";')
    out.blank()
    out.comment('Nothing yet: a feature is listed once its driver has been seen to work on this board',
                '(docs/porting.md goes through them in the order to try).')
    out.prop('features = <>;')
    out.blank()
    light = board.get('backlight', {'type': 'soft'})
    if light.get('panel'):
        out.comment(f'The panel: {light["panel"]}' + (f' ("{light["panel_name"]}")' if light.get('panel_name') else ''))
    out.node('backlight')
    for key in ('type', 'panel_mode', 'min', 'max', 'restore', 'bytes', 'byte_order'):
        if key in light:
            out.prop(boardfile.property_line(key, light[key]))
    out.end()

    def section(title, node, values, commented=False):
        out.blank()
        out.comment(*title)
        out.node(node, commented)
        for key, value in values:
            out.prop(boardfile.property_line(key, value, lambda v: f'{v:#05x}' if node in ('buttons', 'torch') else
                                                     f'{v:#010x}' if v >= 0x10000 else str(v)))
        out.end()

    keys = board.get('buttons', {})
    if 'pon' in keys and 'volume_up' in keys:
        section(('The keys on the PMICs, for the feature "buttons-pmic": SPMI slave << 8 | the high byte of the',
                 "peripheral's registers."), 'buttons',
                [(key, keys[key]) for key in ('pon', 'pon_name', 'volume_up', 'volume_up_name') if key in keys])
    torch = board.get('torch', {})
    if 'flash' in torch and 'alarm' in torch:
        section(('The flashlight\'s part on the PMIC, for the feature "torch-pm8350c": the flash LED module and that',
                 "PMIC's temperature alarm, each as SPMI slave << 8 | the high byte of its registers."), 'torch',
                [('flash', torch['flash']), ('alarm', torch['alarm'])])
    wired = board.get('touch', {})
    if 'reset' in wired and 'interrupt' in wired:
        section(('The touch controller\'s wiring, for a feature "touch-...": its reset and its interrupt are from the tree'
                 + (', and the serial engine it is on' if 'serial_engine' in wired else '') + ';',
                 "the engine's clocks and votes, the bus's pins and the supplies are yours to fill in",
                 '(devices/_template/board.dts says where each is).'), 'touch',
                [(key, wired[key]) for key in ('serial_engine', 'interrupt', 'reset', 'coordinates') if key in wired], commented=True)
    dsp = board.get('adsp', {})
    if 'memory' in dsp:
        section(('The audio DSP, for the feature "adsp": its reserved memory is from the tree; the directory with',
                 "this phone's adsp.mdt and its parts is yours to name (devices/_template/board.dts)."), 'adsp',
                [('firmware', f'fw-audio-{name}'), ('memory', dsp['memory']), ('memory_size', dsp['memory_size'])], commented=True)
    amplifiers = board.get('speaker', {})
    if 'sound_pins' in amplifiers:
        section(('The speaker\'s amplifiers, for the feature "speaker-aw88261": the sound\'s four pins are from the tree',
                 '(bit clock, frame sync, data in, data out); the bus, the addresses, the interrupts and the pins\'',
                 'function are yours to fill in (devices/_template/board.dts says where each is).'), 'speaker',
                [('sound_pins', amplifiers['sound_pins'])], commented=True)
    codec = board.get('mic', {})
    if 'codec_reset' in codec:
        section(('The microphones\' codec, for the feature "mic-wcd938x": its reset pin is from the tree.',), 'mic',
                [('codec_reset', codec['codec_reset'])])
    radio = board.get('wifi', {})
    if 'dsp_memory' in radio:
        section(('The Wi-Fi radio on the SoC\'s own DSP, for the feature "wifi-wcn6750": the DSP\'s reserved memory is',
                 'from the tree,', 'with the radio\'s pins where the Bluetooth node names them (devices/_template/board.dts has the rest).'),
                'wifi', [('radio_model', 0), ('dsp_memory', radio['dsp_memory']), ('dsp_memory_size', radio['dsp_memory_size'])] +
                ([('pins', radio['pins'])] if 'pins' in radio else []), commented=True)
    out.end()
    board_path.parent.mkdir(parents=True)
    board_path.write_text(out.text())
    print(f'wrote {shown(board_path)}')
    # the package's README, from the template: what works is for the porter to say
    template = DEVICES / '_template' / 'README.md'
    if template.exists():
        readme = board_path.parent / 'README.md'
        readme.write_text(template.read_text().replace('MAKER MODEL', description or name).replace('NAME', name)
                          .replace('soc/SOC.dtsi', f'soc/{soc_name}.dtsi').replace('(SOC)', f'({soc_name})'))
        print(f'wrote {shown(readme)}: say there what works, and how it was verified')
    if not soc_path.exists():
        out = Writer()
        out.comment(f'{soc_name}. {started}', 'What every board with this SoC shares: where its blocks are (docs/porting.md).')
        out.node('/')
        out.prop(f'name = "{soc_name}";')
        out.prop(f'description = "{soc_name}";')
        out.blank()
        # each value with what the template says of its key: the file explains itself
        template = DEVICES / '_template' / 'soc.dtsi'
        above, after = template_says(template if template.exists() else
                                     Path(__file__).resolve().parent.parent / 'devices' / '_template' / 'soc.dtsi')
        clocks = soc.get('clocks', {})
        if light.get('type') == 'dcs' or clocks:
            soc_section(out, 'clocks', clocks, above, after,
                        ('gcc', 'gcc_disp_ahb_cbcr', 'dispcc', 'dispcc_mdss_gdscr', 'dispcc_mdss_ahb_cbcr'))
        for part in ('display', 'pins', 'spmi', 'rpmh', 'ipc', 'smmu', 'adreno', 'thermal', 'cpufreq'):
            if part in soc:
                soc_section(out, part, soc[part], above, after)
        if 'lpass' in soc:
            # the microphones' SoundWire pins are in no tree's words the tool reads: the section waits, commented out,
            # until they are filled in for the feature that needs it
            soc_section(out, 'lpass', soc['lpass'], above, after, commented=True,
                        keys=('tx_macro', 'va_macro', 'tx_swr', 'tx_swr_hctl', 'lpi', 'lpi_slew', 'swr_tx_pins', 'swr_tx_slew'))
        if 'platform' in soc:
            soc_section(out, 'platform', soc['platform'], above, after, more=(
                ('io_base', 0x00400000, 'TO FILL IN for loader { }: one window over the SoC\'s peripherals, never 0'),
                ('io_size', 0x3FC00000, '(these two are what the Snapdragon 7-series ports use)')))
        out.end()
        soc_path.write_text(out.text())
        print(f'wrote {shown(soc_path)}')
    else:
        print(f'{shown(soc_path)} is there already: left as it is')
    for note in notes:
        print(as_written(f'note: {note}'))
    print('next: tools/boards.py check, then docs/porting.md')


def tree_to_compare(text, panel=None):
    """What a device tree says, for comparing: as from_dts() has it, without what the reader assumed where
    the tree said nothing. Returns (soc_values, board_values, notes to show)."""
    tree_soc, tree_board, notes = from_dts(text, panel)
    said = [note for note in notes if 'several panels' in note]
    if any('"not named"' in note for note in notes):
        # a panel node that does not say how its light is controlled (a fragment whose other half is
        # elsewhere): "soft" was the reader's choice for a new board, not the tree's word
        tree_board.pop('backlight', None)
        said.append("the panel's node does not say how its light is controlled (qcom,mdss-dsi-bl-pmic-control-type): "
                    '[backlight] is not compared')
    return tree_soc, tree_board, said


def compare_text(name, board, soc, tree_soc, tree_board, tree_name):
    """A board and its SoC file against what a device tree says: (the text, how many values differ)."""
    counts = {'same': 0, 'differ': 0, 'unsaid': 0}
    out = [f'{name} against {tree_name}', '']

    def side(title, mine, tree, schema, key_value):
        lines = []
        for section, values in mine.items():
            if not isinstance(values, dict) or section not in schema:
                continue
            if not any(key in tree.get(section, {}) for key in values):
                # nothing of the section is in the tree: one line, not one for each value
                counts['unsaid'] += len(values)
                lines.append(f'    {"[" + section + "]":34} {len(values)} value{"s" if len(values) != 1 else ""}, '
                             'none of which the tree says')
                continue
            for key, value in values.items():
                said = tree.get(section, {}).get(key)
                where = f'[{section}] {key}'
                if isinstance(value, list) and value and isinstance(value[0], dict):
                    counts['unsaid'] += 1
                    lines.append(f'    {where:34} {len(value)} table{"s" if len(value) != 1 else ""}, not looked for in the tree')
                elif said is None:
                    counts['unsaid'] += 1
                    lines.append(f'    {where:34} {key_value(key, value):28} not in the tree')
                elif said == value:
                    counts['same'] += 1
                    lines.append(f'    {where:34} {key_value(key, value):28} as in the tree')
                else:
                    counts['differ'] += 1
                    lines.append(f'    {where:34} {key_value(key, value):28} THE TREE SAYS {key_value(key, said)}')
        # what the tree has and the file does not: a section to add, or a key
        for section, values in tree.items():
            if section not in schema:
                continue
            missing = [key for key in values if key in schema[section] and key not in mine.get(section, {})]
            if missing:
                lines.append(f'    the tree also has, of [{section}]: ' +
                             ', '.join(f'{key} = {key_value(key, values[key])}' for key in missing))
        return [f'  {title}'] + (lines or ['    nothing a device tree says']) + ['']

    out += side(f'soc/{board["soc"]}.dtsi', soc, tree_soc, SOC_SCHEMA, lambda key, value: dts_value(key, value, soc_number))
    out += side(f'{name}/board.dts', board, tree_board, BOARD_SCHEMA,
                lambda key, value: f'<{value:#05x}>' if key in ('pon', 'volume_up', 'flash', 'alarm') else dts_value(key, value))
    out.append(f'{counts["same"]} values as in the tree, {counts["differ"]} differ, {counts["unsaid"]} the tree does not say' +
               (' (clock offsets, the I/O window, firmware names and what was measured on the phone are never in one)'
                if counts['unsaid'] else ''))
    return as_written('\n'.join(out)) + '\n', counts['differ']


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--devices', action='append', default=[], type=Path, metavar='DIR',
                        help='a directory of device packages (DIR/NAME/board.dts) to take as well as devices/')
    parser.add_argument('--soc', action='append', default=[], type=Path, metavar='DIR', dest='soc_dirs',
                        help='a directory of SoC files to take as well as soc/')
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('list')
    commands.add_parser('show').add_argument('board')
    commands.add_parser('status').add_argument('board')
    commands.add_parser('check')
    commands.add_parser('features')
    commands.add_parser('gen').add_argument('--out', type=Path, default=OUT, metavar='DIR')
    compare = commands.add_parser('compare')
    compare.add_argument('board')
    compare.add_argument('--dts', type=Path, required=True)
    compare.add_argument('--panel')
    new = commands.add_parser('new')
    new.add_argument('name')
    new.add_argument('--soc', required=True)
    new.add_argument('--dts', type=Path)
    new.add_argument('--panel')
    new.add_argument('--description')
    commands.add_parser('convert').add_argument('files', nargs='+', type=Path, metavar='FILE.toml')
    options = parser.parse_args()
    MORE_DEVICES.extend(options.devices)
    MORE_DEVICES.extend(Path(d) for d in os.environ.get('HAKIOS_DEVICES', '').split(os.pathsep) if d)
    MORE_SOCS.extend(options.soc_dirs)

    if options.command == 'new':
        new_board(options.name, options.soc, options.dts, options.panel, options.description)
        return 0
    if options.command == 'convert':
        for path in options.files:
            out = path.with_suffix('.dts' if path.name == 'board.toml' else '.dtsi')
            if out.exists():
                raise SystemExit(f'{out} is there already')
            out.write_text(boardfile.from_toml(path.read_text()))
            print(f'wrote {out}: check it (tools/boards.py check), then remove {path.name}')
        return 0
    if options.command == 'features':
        print(features_text(), end='')
        return 0
    if options.command == 'check':
        problems = Problems()
        socs, boards = load(problems)
        for problem in problems:
            print(problem, file=sys.stderr)
        if not problems:
            print(f'{len(boards)} boards, {len(socs)} SoCs: ok')
        return 1 if problems else 0
    socs, boards = load()
    if options.command == 'list':
        for name, board in sorted(boards.items()):
            print(f'{name:12} {board["soc"]:10} {board["description"]}')
    elif options.command == 'status':
        if options.board not in boards:
            raise SystemExit(f'no board "{options.board}" (tools/boards.py list)')
        print(status_text(options.board, boards[options.board], socs[boards[options.board]['soc']]), end='')
    elif options.command == 'compare':
        if options.board not in boards:
            raise SystemExit(f'no board "{options.board}" (tools/boards.py list)')
        board = boards[options.board]
        tree_soc, tree_board, notes = tree_to_compare(options.dts.read_text(errors='replace'), options.panel)
        text, differ = compare_text(options.board, board, socs[board['soc']], tree_soc, tree_board, options.dts.name)
        print(text, end='')
        for note in notes:
            print(as_written(f'note: {note}'))
        return 1 if differ else 0
    elif options.command == 'show':
        if options.board not in boards:
            raise SystemExit(f'no board "{options.board}" (tools/boards.py list)')
        board = boards[options.board]
        for title, data in ((f'devices/{options.board}/board.dts', board), (f'soc/{board["soc"]}.dtsi', socs[board['soc']])):
            print(f'// {title}')
            print(show_text(data), end='')
    elif options.command == 'gen':
        written = write_generated(options.out, socs, boards)
        names = ', '.join(sorted(boards))
        print(f'{shown(options.out)}: {len(boards)} boards ({names}); ' +
              (f'wrote {", ".join(written)}' if written else 'nothing changed'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
