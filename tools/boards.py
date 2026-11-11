#!/usr/bin/env python3
"""The boards this project knows, as data: devices/NAME/board.toml and soc/SOC.toml.

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
                                       start devices/NAME/board.toml (and soc/SOC.toml if it is not there);
                                       with --dts, fill in what an Android device tree source says

A board file says what a device has; a SoC file says where the blocks of its chip are, once for every
board with that chip. The kernel is built with one table of the boards that are there and picks its row
at boot by /chosen/darwin-board. docs/porting.md walks through adding a device.

A board is a device package: a directory NAME with board.toml in it. Packages are looked for in
devices/ and in the directories of --devices DIR and of HAKIOS_DEVICES; SoC files in soc/ and --soc DIR.
The repository itself has the emulator's board and a template; phones are packages kept apart from it
(tools/device.py brings one in).
"""
import argparse
import os
import re
import sys
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from devicetree import from_dts  # noqa: E402  (what a device tree says of a board: tools/devicetree.py)

ROOT = Path(__file__).resolve().parents[1]
DEVICES = ROOT / 'devices'
SOCS = ROOT / 'soc'
BOARD_HEADER = ROOT / 'kernel/overlay/bsd/dev/hakios_board.h'
# What `gen` writes, into one directory (--out; build/boards unless told otherwise). None of it is kept
# in the repository: it is made from whichever device packages are there when something is built.
OUT = ROOT / 'build/boards'
KERNEL_TABLE = 'hakios_board_table.h'   # the kernel's table (bsd/dev/hakios_board.c includes it)
LOADER_TABLE = 'platforms.h'            # the loader's (loader/loader.c)
USER_TABLE = 'hakios_boards.h'          # user space's (ios/userland/)
# Where else device packages and SoC files are looked for, besides devices/ and soc/: the directories of
# --devices and --soc, and of HAKIOS_DEVICES (as PATH is written). A package is DIR/NAME/board.toml.
MORE_DEVICES = []
MORE_SOCS = []

NAME = re.compile(r'^[a-z0-9][a-z0-9-]*$')



class U64(int):
    """A number of up to 64 bits (an address the loader works with), where int means 32."""


# key: (type, required). Types: str, int (32-bit), U64, bool, [int] and so on; a tuple of strings is a choice.
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
    'smmu': {'apps': (int, True)},
    'camera': {'camcc': (int, True), 'ife': (int, True), 'csid': (int, True), 'csiphy': (int, True)},
    'lpass': {'tx_macro': (int, True), 'va_macro': (int, True), 'tx_swr': (int, True), 'tx_swr_hctl': (int, True),
              'lpi': (int, True), 'lpi_slew': (int, True)},
    'ipc': {'aoss': (int, True), 'ipcc': (int, True), 'smem': (int, True), 'smem_size': (int, True),
            'smem_lock': (int, True)},
    'rpmh': {'cmd_db': (int, True), 'cmd_db_size': (int, True), 'rsc': (int, True), 'tcs_offset': (int, True)},
    'cpufreq': {'epss': (int, True), 'epss_domain_size': (int, True), 'domain_cores': ([int], True)},
}
BOARD_SCHEMA = {
    '': {'name': (str, True), 'description': (str, True), 'soc': (str, True), 'features': ([str], True)},
    'backlight': {'type': (('dcs', 'soft', 'none'), True), 'panel_mode': (('command', 'video'), False),
                  'min': (int, False), 'max': (int, False), 'restore': (int, False), 'bytes': (int, False),
                  'byte_order': (('msb-first', 'lsb-first'), False)},
    'thermal': {'floor_mhz': ([int], True)},
    'torch': {'flash': (int, True), 'alarm': (int, True)},
    'buttons': {'pon': (int, True), 'volume_up': (int, True), 'pon_name': (str, False), 'volume_up_name': (str, False)},
    'adsp': {'firmware': (str, True), 'memory': (int, True), 'memory_size': (int, True),
             'charge_counter_divisor': (int, False)},
    'gpu': {'model': (str, True), 'chip_id': (int, True), 'firmware': (str, True), 'gmem_kb': (int, True),
            'primitive_fifo': (int, True), 'hang_cycles': (int, True), 'uche_cmdq_config': (int, False),
            'speed_fuse': (int, True), 'gaming_fuse': (int, True), 'highest_bank_bit': (int, True),
            'gcc_debug_gpucc': (int, True), 'zap_must_authenticate': (bool, False),
            'speeds_khz': ([int], True), 'speed_levels': ([int], True), 'default_speed': (int, True),
            'alt_model': (str, False), 'alt_bins': ([int], False)},
    'screen': {'statusbar_leading_shift': (int, True)},
    'wifi': {'radio_model': (int, True), 'early_pool_mb': (int, False), 'dsp_memory': (int, False),
             'dsp_memory_size': (int, False)},
    'loader': {'fb_base': (U64, False), 'log_other_cpu': (bool, False), 'uefi_charger': (bool, False)},
    'boot': {'fastboot_product': (str, True), 'cpus': (int, True), 'bootargs': ([str], True)},
}
# what a feature needs of the SoC file: a board cannot list a feature its SoC has no addresses for
FEATURE_NEEDS = {'cpufreq-epss': ('cpufreq',), 'display-scan': ('display',), 'buttons-pmic': ('spmi',),
                 'torch-pm8350c': ('spmi',), 'ddr-rpmh': ('rpmh',),
                 'camera-redwood': ('spmi', 'pins', 'clocks', 'rpmh', 'smmu', 'camera'),
                 'gpu-a660': ('adreno', 'clocks', 'rpmh'),
                 # the part drivers that drive the SoC's pins, switch a bus's clocks on in the GCC, ask RPMh
                 # for a part's supply, and give a device its addresses in the SMMU
                 'touch-goodix': ('pins', 'clocks', 'rpmh'), 'touch-ft3658u': ('pins', 'clocks', 'rpmh'),
                 'speaker-aw88261': ('pins', 'clocks', 'rpmh', 'smmu'), 'haptic-aw86927': ('pins',),
                 'adsp': ('rpmh', 'ipc'), 'wifi-wcn6750': ('rpmh', 'pins', 'spmi', 'ipc', 'smmu'), 'mic-wcd938x': ('pins', 'lpass')}
WRITES_TO_PMIC = ('torch-pm8350c', 'camera-redwood')    # ... these through the arbiter's channels as well
# The boot arguments that start a feature's driver, as a porter is to try them: the level that only looks
# first where there is one. (docs/boot-args.md has every argument; a test holds these names to it.)
FEATURE_BOOTARGS = {
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
    'camera-redwood': 'cam=N',
    'wifi-wcn6750': 'wifi=1',
    'mic-wcd938x': 'mic=N with adsp=1',
}
# features that are a driver of one part as one phone has it wired, by the first word of their names
PART_DRIVERS = ('touch', 'speaker', 'haptic', 'torch', 'camera', 'wifi', 'mic')
# ... and of the board's own file
FEATURE_NEEDS_OF_BOARD = {'gpu-a660': 'gpu', 'adsp': 'adsp', 'buttons-pmic': 'buttons', 'torch-pm8350c': 'torch'}
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
        for path in sorted(Path(directory).glob('*.toml')):
            seen.setdefault(path.stem, path)
    return [seen[name] for name in sorted(seen)]


def board_files():
    """Every device package's board file, devices/'s first (the template is not a board)."""
    seen = {}
    for directory in [DEVICES] + MORE_DEVICES:
        for path in sorted(Path(directory).glob('*/board.toml')):
            if not path.parent.name.startswith('_'):
                seen.setdefault(path.parent.name, path)
    return [seen[name] for name in sorted(seen)]


class Problems(list):
    def add(self, where, what):
        self.append(f'{where}: {what}')


def feature_bits():
    """The features the kernel knows, HAKIOS_HAS_* of hakios_board.h: [(bit, name as board.toml spells it)]."""
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
    soc_file = f'soc/{board.get("soc")}.toml'
    needs = []
    for section in FEATURE_NEEDS.get(feature, ()):
        if section not in soc:
            needs.append(f'[{section}] in {soc_file}')
    section = FEATURE_NEEDS_OF_BOARD.get(feature)
    if section and section not in board:
        needs.append(f'[{section}] in the board file')
    spmi, display = soc.get('spmi'), soc.get('display')
    if feature in WRITES_TO_PMIC and isinstance(spmi, dict) and 'channels' not in spmi:
        needs.append(f'channels in [spmi] of {soc_file}')
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
         'no [thermal] section' + ('' if 'thermal' in soc and 'cpufreq' in soc else
                                   f': needs [thermal] and [cpufreq] in soc/{board["soc"]}.toml first'))
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
    return '\n'.join(out)


def check_table(where, table, schema, problems):
    for key, value in table.items():
        if isinstance(value, dict):
            continue
        if key not in schema:
            problems.add(where, f'unknown key "{key}" (known: {", ".join(sorted(schema))})')
            continue
        kind = schema[key][0]
        if isinstance(kind, tuple):
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
    for key, (_, required) in schema.items():
        if required and key not in table:
            problems.add(where, f'"{key}" is missing')


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


def load(problems=None):
    """Every SoC and board, checked. Returns (socs, boards) by name; problems are collected or raised."""
    own = problems is None
    problems = Problems() if own else problems
    socs, boards = {}, {}
    for path in soc_files():
        try:
            data = tomllib.loads(path.read_text())
        except tomllib.TOMLDecodeError as error:
            problems.add(shown(path), f'not TOML: {error}')
            continue
        check_file(path, data, SOC_SCHEMA, problems)
        if data.get('name') != path.stem:
            problems.add(shown(path), f'name = {data.get("name")!r}: the file is {path.stem}.toml')
        socs[path.stem] = data
    known = features_of_header()
    for path in board_files():
        rel = shown(path)
        try:
            data = tomllib.loads(path.read_text())
        except tomllib.TOMLDecodeError as error:
            problems.add(rel, f'not TOML: {error}')
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
            problems.add(rel, f'soc = {data.get("soc")!r}: there is no soc/{data.get("soc")}.toml')
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
                    problems.add(rel, f'backlight type "dcs" needs [{section}] in soc/{data.get("soc")}.toml')
            if clocks_missing(soc):
                problems.add(rel, f'backlight type "dcs" needs {clocks_missing(soc)} in [clocks] of soc/{data.get("soc")}.toml')
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
        radio = data.get('wifi')
        if radio is not None:
            if radio['radio_model'] not in (0, 1):
                problems.add(f'{rel} [wifi]', 'radio_model: 0 (a WCN6750 on the SoC\'s own bus) or 1 (a WCN6855 on PCIe)')
            if radio.get('early_pool_mb', 0) > 512:
                problems.add(f'{rel} [wifi]', 'early_pool_mb is in megabytes: some tens')
            if ('dsp_memory' in radio) != ('dsp_memory_size' in radio) or radio.get('dsp_memory', 0x1000) & 0xfff or \
                    radio.get('dsp_memory_size', 0x1000) & 0xfff or 0 in (radio.get('dsp_memory', 1), radio.get('dsp_memory_size', 1)):
                problems.add(f'{rel} [wifi]', 'dsp_memory and dsp_memory_size go together: an address and a size in whole pages')
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
                problems.add(rel, f'[loader] needs [platform] in soc/{data.get("soc")}.toml')
            elif 'io_base' not in soc['platform'] or 'io_size' not in soc['platform']:
                # (no device tree has them, so they are asked for when the loader is to know the board)
                problems.add(rel, f'[loader] needs io_base and io_size in [platform] of soc/{data.get("soc")}.toml')
            elif soc['platform'].get('io_base') == 0:
                problems.add(rel, f'soc/{data.get("soc")}.toml [platform]: io_base must not be 0 (XNU then takes the '
                                  'machine for one without a SoC)')
        groups = soc.get('cpufreq', {}).get('domain_cores', [])
        if groups and (not 1 <= len(groups) <= 4 or any(not 1 <= n <= 255 for n in groups)):
            problems.add(rel, f'soc/{data.get("soc")}.toml [cpufreq]: domain_cores is one to four groups of at least one core')
        elif groups and 'platform' in soc and sum(groups) != soc['platform'].get('cores'):
            problems.add(rel, f'soc/{data.get("soc")}.toml: domain_cores adds up to {sum(groups)} cores, [platform] has '
                              f'{soc["platform"].get("cores")}')
        thermal = data.get('thermal')
        if thermal is not None:
            if groups and len(thermal.get('floor_mhz', [])) != len(groups):
                problems.add(f'{rel} [thermal]', f'floor_mhz: one value for each of the {len(groups)} groups of cores '
                                                 f'(soc/{data.get("soc")}.toml [cpufreq] domain_cores)')
            for section in ('thermal', 'cpufreq'):
                if section not in soc:
                    problems.add(rel, f'[thermal] needs [{section}] in soc/{data.get("soc")}.toml')
            if len(soc.get('thermal', {}).get('tsens', [])) > 2:
                problems.add(rel, f'soc/{data.get("soc")}.toml: at most two TSENS controllers')
    fallbacks = [name for name, data in boards.items() if 'loader' in data and 'fb_base' not in data['loader']]
    if len(fallbacks) > 1:
        problems.add('devices', 'more than one board with [loader] and no fb_base (' + ', '.join(fallbacks) +
                     '): the loader tells boards apart by it, and only the emulator goes without')
    seen = {}
    for name, data in boards.items():
        base = data.get('loader', {}).get('fb_base')
        if base is not None and seen.setdefault(base, name) != name:
            problems.add(f'devices/{name}/board.toml [loader]', f'fb_base {base:#x} is also that of {seen[base]}')
    if own and problems:
        raise SystemExit('\n'.join(problems))
    return socs, boards


def c_string(text):
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'


def table_text(socs, boards):
    known = features_of_header()
    out = ['/*',
           ' * GENERATED by tools/boards.py gen from devices/NAME/board.toml and soc/SOC.toml: do not edit.',
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
        tsens = (soc.get('thermal', {}).get('tsens', []) + [0, 0])[:2] if thermal else [0, 0]
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
                    '\t\t},']
        if 'smmu' in soc:
            out.append(f'\t\t.smmu = {{ .apps = {soc["smmu"]["apps"]:#010x} }},')
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
                       f'.dsp_memory = {radio.get("dsp_memory", 0):#010x}, .dsp_memory_size = {radio.get("dsp_memory_size", 0):#010x} }},')
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
        if thermal:
            floors = ', '.join(str(v) for v in thermal['floor_mhz'])
            out += [
                '\t\t.thermal = {',
                f'\t\t\t.tsens = {{ {tsens[0]:#010x}, {tsens[1]:#010x} }},',
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
            '/* the features by their bit, as board.toml spells them */',
            f'const char *const hakios_feature_names[] = {{ {names} }};',
            'const unsigned hakios_feature_count = sizeof(hakios_feature_names) / sizeof(hakios_feature_names[0]);',
            '',
            '#endif /* _HAKIOS_BOARD_TABLE_H */',
            '']
    return '\n'.join(out)


def platforms_text(socs, boards):
    """platforms.h: the loader's description of every board with a [loader] section."""
    out = ['/*',
           ' * GENERATED by tools/boards.py gen from devices/NAME/board.toml ([loader]) and soc/SOC.toml',
           ' * ([platform]): do not edit. Included by loader.c, which has struct platform and says what each',
           ' * field is for.',
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
           ' * GENERATED by tools/boards.py gen from devices/NAME/board.toml ([screen]): do not edit.',
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
    a [boot] section. A SystemExit that says what there is otherwise."""
    bootable = {n: board for n, board in boards.items() if 'boot' in board}
    if name is None:
        if len(bootable) != 1:
            raise SystemExit('which phone? --board NAME (here: ' + (', '.join(sorted(bootable)) or 'no package with a '
                             '[boot] section; a phone is brought in with tools/device.py add NAME') + ')')
        name = next(iter(bootable))
    if name not in bootable:
        raise SystemExit(f'no board "{name}" with a [boot] section here (tools/boards.py list); a phone is brought in '
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


def toml_value(value):
    if isinstance(value, bool):
        return 'true' if value else 'false'
    if isinstance(value, int):
        return f'{value:#x}' if value > 4096 else str(value)
    if isinstance(value, list):
        return '[' + ', '.join(toml_value(v) for v in value) + ']'
    return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"') + '"'


COUNTS = ('cores', 'core_shift', 'ee', 'timeout_scale', 'domain_cores')     # numbers a person counts in


def soc_value(key, value):
    """A value of a SoC file as a person writes it: addresses in eight hex digits, offsets and masks in
    hex, counts in decimal."""
    if isinstance(value, bool) or not isinstance(value, (int, list)) or key in COUNTS:
        return toml_value(value)
    if isinstance(value, list):
        return '[' + ', '.join(soc_value(key, v) for v in value) + ']'
    return f'{value:#010x}' if value >= 0x10000 else f'{value:#x}'


def template_says(template):
    """What the template of a SoC file says of each section and key: {section: lines above its header},
    {(section, key): the comment after the value, with the lines that continue it}."""
    above, after, section, pending, last = {}, {}, '', [], None
    for line in template.read_text().splitlines() if template.exists() else []:
        text = line.strip()
        header, value = re.fullmatch(r'\[(\w+)\]', text), re.match(r'(\w+) = [^#]*#\s?(.*)', text)
        if header:
            section, last = header[1], None
            above[section], pending = pending, []
        elif value and not line.startswith('#'):
            last = (section, value[1])
            after[last], pending = [value[2]], []
        elif line.startswith(' ') and text.startswith('#') and last:
            after[last].append(text[1:].strip())          # a comment that goes on under the value's
        elif text.startswith('#'):
            pending, last = pending + [text], None
        else:
            pending, last = [], None
    return above, after


def soc_lines(section, values, above, after, keys=None):
    """A section of a new SoC file: each value with what the template says of its key."""
    lines = above.get(section, []) + [f'[{section}]']
    for key in keys or values:
        if key in values:
            said = after.get((section, key), [])
            lines.append(f'{key} = {soc_value(key, values[key])}'.ljust(36) + (f'# {said[0]}' if said else ''))
            lines += [' ' * 36 + f'# {more}' for more in said[1:]]
            lines[-1 - len(said[1:])] = lines[-1 - len(said[1:])].rstrip()
        else:
            lines.append(f'# {key} = 0x...            # TO FILL IN' + (f': {after[(section, key)][0]}' if (section, key) in after else ''))
    return lines + ['']


def new_board(name, soc_name, dts=None, panel=None, description=None):
    if not NAME.match(name) or not NAME.match(soc_name):
        raise SystemExit('names are lower-case letters, digits and dashes')
    board_path = DEVICES / name / 'board.toml'
    soc_path = SOCS / f'{soc_name}.toml'
    if board_path.exists():
        raise SystemExit(f'{shown(board_path)} is there already')
    soc, board, notes = from_dts(dts.read_text(errors='replace'), panel) if dts else ({}, {}, [])
    light = board.get('backlight', {'type': 'soft'})
    lines = [f'# {description or name}. Started by tools/boards.py new' + (f' from {dts.name}' if dts else '') + '.',
             '# Every value here is to be checked against the device before it is relied on (docs/porting.md).',
             f'name = "{name}"                     # what the loader puts in /chosen/darwin-board',
             f'description = {toml_value(description or name)}',
             f'soc = "{soc_name}"',
             '',
             '# Nothing yet: a feature is listed once its driver has been seen to work on this board',
             '# (docs/porting.md goes through them in the order to try).',
             'features = []',
             '']
    if light.get('panel'):
        lines.append(f'# The panel: {light["panel"]}' + (f' ("{light["panel_name"]}")' if light.get('panel_name') else ''))
    lines.append('[backlight]')
    for key in ('type', 'panel_mode', 'min', 'max', 'restore', 'bytes', 'byte_order'):
        if key in light:
            lines.append(f'{key} = {toml_value(light[key])}')
    lines.append('')
    keys = board.get('buttons', {})
    if 'pon' in keys and 'volume_up' in keys:
        lines += ['# The keys on the PMICs, for the feature "buttons-pmic": SPMI slave << 8 | the high byte of the',
                  "# peripheral's registers.",
                  '[buttons]']
        lines += [f'{key} = {keys[key]:#05x}' if isinstance(keys[key], int) else f'{key} = {toml_value(keys[key])}'
                  for key in ('pon', 'pon_name', 'volume_up', 'volume_up_name') if key in keys]
        lines.append('')
    light_part = board.get('torch', {})
    if 'flash' in light_part and 'alarm' in light_part:
        lines += ['# The flashlight\'s part on the PMIC, for the feature "torch-pm8350c": the flash LED module and that',
                  "# PMIC's temperature alarm, each as SPMI slave << 8 | the high byte of its registers.",
                  '[torch]',
                  f'flash = {light_part["flash"]:#05x}',
                  f'alarm = {light_part["alarm"]:#05x}',
                  '']
    dsp = board.get('adsp', {})
    if 'memory' in dsp:
        lines += ['# The audio DSP, for the feature "adsp": its reserved memory is from the tree; the directory with',
                  "# this phone's adsp.mdt and its parts is yours to name (devices/_template/board.toml).",
                  '# [adsp]',
                  '# firmware = "fw-audio-' + name + '"',
                  f'# memory = {dsp["memory"]:#010x}',
                  f'# memory_size = {dsp["memory_size"]:#010x}',
                  '']
    board_path.parent.mkdir(parents=True)
    board_path.write_text('\n'.join(lines))
    print(f'wrote {shown(board_path)}')
    # the package's README, from the template: what works is for the porter to say
    template = DEVICES / '_template' / 'README.md'
    if template.exists():
        readme = board_path.parent / 'README.md'
        readme.write_text(template.read_text().replace('MAKER MODEL', description or name).replace('NAME', name)
                          .replace('soc/SOC.toml', f'soc/{soc_name}.toml').replace('(SOC)', f'({soc_name})'))
        print(f'wrote {shown(readme)}: say there what works, and how it was verified')
    if not soc_path.exists():
        text = [f'# {soc_name}. Started by tools/boards.py new' + (f' from {dts.name}' if dts else '') + '.',
                '# What every board with this SoC shares: where its blocks are (docs/porting.md).',
                f'name = "{soc_name}"',
                f'description = "{soc_name}"',
                '']
        # each value with what the template says of its key: the file explains itself
        template = DEVICES / '_template' / 'soc.toml'
        above, after = template_says(template if template.exists() else
                                     Path(__file__).resolve().parent.parent / 'devices' / '_template' / 'soc.toml')
        clocks = soc.get('clocks', {})
        if light.get('type') == 'dcs' or clocks:
            text += soc_lines('clocks', clocks, above, after,
                              ('gcc', 'gcc_disp_ahb_cbcr', 'dispcc', 'dispcc_mdss_gdscr', 'dispcc_mdss_ahb_cbcr'))
        for section in ('display', 'pins', 'spmi', 'rpmh', 'ipc', 'smmu', 'adreno', 'thermal', 'cpufreq'):
            if section in soc:
                text += soc_lines(section, soc[section], above, after)
        if 'platform' in soc:
            lines = soc_lines('platform', soc['platform'], above, after)
            at = lines.index('[platform]') + 1
            text += lines[:at] + ['# io_base = 0x00400000          # TO FILL IN for [loader]: one window over the SoC\'s peripherals, never 0',
                                  '# io_size = 0x3FC00000          # (these two are what the Snapdragon 7-series ports use)'] + lines[at:]
        soc_path.write_text('\n'.join(text))
        print(f'wrote {shown(soc_path)}')
    else:
        print(f'{shown(soc_path)} is there already: left as it is')
    for note in notes:
        print(f'note: {note}')
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
                if said is None:
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

    out += side(f'soc/{board["soc"]}.toml', soc, tree_soc, SOC_SCHEMA, soc_value)
    out += side(f'{name}/board.toml', board, tree_board, BOARD_SCHEMA,
                lambda key, value: f'{value:#05x}' if key in ('pon', 'volume_up', 'flash', 'alarm') else toml_value(value))
    out.append(f'{counts["same"]} values as in the tree, {counts["differ"]} differ, {counts["unsaid"]} the tree does not say' +
               (' (clock offsets, the I/O window, firmware names and what was measured on the phone are never in one)'
                if counts['unsaid'] else ''))
    return '\n'.join(out) + '\n', counts['differ']


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--devices', action='append', default=[], type=Path, metavar='DIR',
                        help='a directory of device packages (DIR/NAME/board.toml) to take as well as devices/')
    parser.add_argument('--soc', action='append', default=[], type=Path, metavar='DIR', dest='soc_dirs',
                        help='a directory of SoC files to take as well as soc/')
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('list')
    commands.add_parser('show').add_argument('board')
    commands.add_parser('status').add_argument('board')
    commands.add_parser('check')
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
    options = parser.parse_args()
    MORE_DEVICES.extend(options.devices)
    MORE_DEVICES.extend(Path(d) for d in os.environ.get('HAKIOS_DEVICES', '').split(os.pathsep) if d)
    MORE_SOCS.extend(options.soc_dirs)

    if options.command == 'new':
        new_board(options.name, options.soc, options.dts, options.panel, options.description)
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
            print(f'note: {note}')
        return 1 if differ else 0
    elif options.command == 'show':
        if options.board not in boards:
            raise SystemExit(f'no board "{options.board}" (tools/boards.py list)')
        board = boards[options.board]
        for key, value in board.items():
            if isinstance(value, dict):
                print(f'[{key}]')
                for k, v in value.items():
                    print(f'  {k} = {toml_value(v)}')
            else:
                print(f'{key} = {toml_value(value)}')
        print(f'--- soc/{board["soc"]}.toml')
        for key, value in socs[board['soc']].items():
            if isinstance(value, dict):
                print(f'[{key}]')
                for k, v in value.items():
                    print(f'  {k} = {toml_value(v)}')
    elif options.command == 'gen':
        written = write_generated(options.out, socs, boards)
        names = ', '.join(sorted(boards))
        print(f'{shown(options.out)}: {len(boards)} boards ({names}); ' +
              (f'wrote {", ".join(written)}' if written else 'nothing changed'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
