#!/usr/bin/env python3
"""The boards this project knows, as data: devices/NAME/board.toml and soc/SOC.toml.

  tools/boards.py list                 the boards, one a line
  tools/boards.py show BOARD           one board with its SoC's values filled in
  tools/boards.py check                validate every file; non-zero exit and the reasons if one is wrong
  tools/boards.py gen [--check]        write the kernel's table (kernel/overlay/bsd/dev/hakios_board_table.h) and
                                       the loader's (loader/platforms.h); --check: only say whether the
                                       committed ones are up to date
  tools/boards.py new NAME --soc SOC [--dts FILE [--panel NODE]]
                                       start devices/NAME/board.toml (and soc/SOC.toml if it is not there);
                                       with --dts, fill in what an Android device tree source says

A board file says what a device has; a SoC file says where the blocks of its chip are, once for every
board with that chip. The kernel is built with one table of all boards and picks its row at boot by
/chosen/darwin-board. docs/porting.md walks through adding a device.
"""
import argparse
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEVICES = ROOT / 'devices'
SOCS = ROOT / 'soc'
BOARD_HEADER = ROOT / 'kernel/overlay/bsd/dev/hakios_board.h'
TABLE = ROOT / 'kernel/overlay/bsd/dev/hakios_board_table.h'
PLATFORMS = ROOT / 'loader/platforms.h'

NAME = re.compile(r'^[a-z0-9][a-z0-9-]*$')
KEY_VOLUMEUP = 0x73     # Linux's input-event-codes.h: what gpio-keys calls the key in linux,code



class U64(int):
    """A number of up to 64 bits (an address the loader works with), where int means 32."""


# key: (type, required). Types: str, int (32-bit), U64, bool, [int] and so on; a tuple of strings is a choice.
SOC_SCHEMA = {
    '': {'name': (str, True), 'description': (str, True)},
    'platform': {'io_base': (U64, True), 'io_size': (U64, True), 'gic_dist': (U64, True), 'gic_redist': (U64, True),
                 'gic_redist_size': (U64, False), 'gic_redist_stride': (U64, False), 'uart': (U64, False),
                 'cores': (int, True), 'core_shift': (int, True), 'small_cores': (int, False),
                 'psci': (('smc', 'hvc'), True), 'timeout_scale': (int, False)},
    'clocks': {'gcc': (int, True), 'gcc_disp_ahb_cbcr': (int, True), 'dispcc': (int, True),
               'dispcc_mdss_gdscr': (int, True), 'dispcc_mdss_ahb_cbcr': (int, True),
               'dispcc_mdss_pclk0_cbcr': (int, False), 'dispcc_mdss_mdp_cbcr': (int, False)},
    'display': {'dsi0': (int, True), 'intf1': (int, False)},
    'thermal': {'tsens': ([int], True)},
    'spmi': {'core': (int, True), 'cnfg': (int, True), 'observer': (int, True), 'channels': (int, False),
             'ee': (int, False)},
    'rpmh': {'cmd_db': (int, True), 'cmd_db_size': (int, True), 'rsc': (int, True), 'tcs_offset': (int, True)},
    'cpufreq': {'epss': (int, True), 'epss_domain_size': (int, True), 'domain_cores': ([int], True)},
}
BOARD_SCHEMA = {
    '': {'name': (str, True), 'description': (str, True), 'soc': (str, True), 'features': ([str], True)},
    'backlight': {'type': (('dcs', 'soft', 'none'), True), 'panel_mode': (('command', 'video'), False),
                  'min': (int, False), 'max': (int, False), 'restore': (int, False), 'bytes': (int, False),
                  'byte_order': (('msb-first', 'lsb-first'), False)},
    'thermal': {'floor_mhz': ([int], True)},
    'buttons': {'pon': (int, True), 'volume_up': (int, True), 'pon_name': (str, False), 'volume_up_name': (str, False)},
    'adsp': {'firmware': (str, True), 'memory_size': (int, True), 'charge_counter_divisor': (int, False)},
    'gpu': {'model': (str, True), 'chip_id': (int, True), 'firmware': (str, True), 'gmem_kb': (int, True),
            'primitive_fifo': (int, True), 'hang_cycles': (int, True), 'uche_cmdq_config': (int, False),
            'speed_fuse': (int, True), 'gaming_fuse': (int, True), 'highest_bank_bit': (int, True),
            'gcc_debug_gpucc': (int, True), 'zap_must_authenticate': (bool, False),
            'speeds_khz': ([int], True), 'speed_levels': ([int], True), 'default_speed': (int, True),
            'alt_model': (str, False), 'alt_bins': ([int], False)},
    'loader': {'fb_base': (U64, False), 'log_other_cpu': (bool, False)},
    'boot': {'fastboot_product': (str, True), 'cpus': (int, True), 'bootargs': ([str], True)},
}
# what a feature needs of the SoC file: a board cannot list a feature its SoC has no addresses for
FEATURE_NEEDS = {'cpufreq-epss': 'cpufreq', 'display-scan': 'display', 'buttons-pmic': 'spmi', 'torch-pm8350c': 'spmi', 'ddr-rpmh': 'rpmh',
                 'camera-redwood': 'spmi'}
WRITES_TO_PMIC = ('torch-pm8350c', 'camera-redwood')    # ... these through the arbiter's channels as well
# ... and of the board's own file
FEATURE_NEEDS_OF_BOARD = {'gpu-a660': 'gpu', 'adsp': 'adsp', 'buttons-pmic': 'buttons'}
GPU_SPEEDS = 5          # (struct hakios_board: gpu.speeds_khz)


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
    rel = path.relative_to(ROOT)
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
    for path in sorted(SOCS.glob('*.toml')):
        try:
            data = tomllib.loads(path.read_text())
        except tomllib.TOMLDecodeError as error:
            problems.add(path.relative_to(ROOT), f'not TOML: {error}')
            continue
        check_file(path, data, SOC_SCHEMA, problems)
        if data.get('name') != path.stem:
            problems.add(path.relative_to(ROOT), f'name = {data.get("name")!r}: the file is {path.stem}.toml')
        socs[path.stem] = data
    known = features_of_header()
    for path in sorted(DEVICES.glob('*/board.toml')):
        rel = path.relative_to(ROOT)
        try:
            data = tomllib.loads(path.read_text())
        except tomllib.TOMLDecodeError as error:
            problems.add(rel, f'not TOML: {error}')
            continue
        name = path.parent.name
        if name.startswith('_'):
            continue                    # (the template)
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
            elif FEATURE_NEEDS.get(feature) and FEATURE_NEEDS[feature] not in soc:
                problems.add(rel, f'feature "{feature}" needs [{FEATURE_NEEDS[feature]}] in soc/{data.get("soc")}.toml')
            if FEATURE_NEEDS_OF_BOARD.get(feature) and FEATURE_NEEDS_OF_BOARD[feature] not in data:
                problems.add(rel, f'feature "{feature}" needs a [{FEATURE_NEEDS_OF_BOARD[feature]}] section')
        boards[name] = data
        if not well_formed:
            continue
        if 'display-scan' in data.get('features', []):
            if 'intf1' not in soc.get('display', {}) or 'clocks' not in soc:
                problems.add(rel, f'feature "display-scan" needs intf1 in [display] and [clocks] in soc/{data.get("soc")}.toml')
            if data.get('backlight', {}).get('panel_mode') == 'video':
                problems.add(rel, 'feature "display-scan" is for a command-mode panel; [backlight] says video')
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
        for feature in data.get('features', []):
            if feature in WRITES_TO_PMIC and 'spmi' in soc and 'channels' not in soc['spmi']:
                problems.add(rel, f'feature "{feature}" needs channels in [spmi] of soc/{data.get("soc")}.toml')
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
        for argument in data.get('boot', {}).get('bootargs', []):
            if not re.fullmatch(r'[A-Za-z0-9_-]+(=\S*)?', argument):
                problems.add(f'{rel} [boot]', f'bootargs: "{argument}" is not NAME or NAME=VALUE')
        if 'loader' in data:
            if 'platform' not in soc:
                problems.add(rel, f'[loader] needs [platform] in soc/{data.get("soc")}.toml')
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
           ' * To change a board, change its file there and run the tool; `tools/boards.py gen --check`',
           ' * (part of `make check`) fails while this file is not what the tool writes.',
           ' */',
           '#ifndef _HAKIOS_BOARD_TABLE_H',
           '#define _HAKIOS_BOARD_TABLE_H',
           '',
           '#include "hakios_board.h"',
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
                f'\t\t\t.gcc = {clocks["gcc"]:#010x}, .gcc_disp_ahb_cbcr = {clocks["gcc_disp_ahb_cbcr"]:#x},',
                f'\t\t\t.dispcc = {clocks["dispcc"]:#010x}, .dispcc_mdss_gdscr = {clocks["dispcc_mdss_gdscr"]:#x},',
                f'\t\t\t.dispcc_mdss_ahb_cbcr = {clocks["dispcc_mdss_ahb_cbcr"]:#x},',
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
        rpmh = soc.get('rpmh')
        if rpmh:
            out.append(f'\t\t.rpmh = {{ .cmd_db = {rpmh["cmd_db"]:#010x}, .cmd_db_size = {rpmh["cmd_db_size"]:#x}, '
                       f'.rsc = {rpmh["rsc"]:#010x}, .tcs_offset = {rpmh["tcs_offset"]:#x} }},')
        keys = board.get('buttons')
        if keys:
            names = ''.join(f', .{key} = {c_string(keys[key])}' for key in ('pon_name', 'volume_up_name') if key in keys)
            out.append(f'\t\t.buttons = {{ .pon = {keys["pon"]:#05x}, .volume_up = {keys["volume_up"]:#05x}{names} }},')
        adsp = board.get('adsp')
        if adsp:
            out.append(f'\t\t.adsp = {{ .firmware = {c_string(adsp["firmware"])}, .memory_size = {adsp["memory_size"]:#010x}, '
                       f'.charge_counter_divisor = {adsp.get("charge_counter_divisor", 1)} }},')
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
    """loader/platforms.h: the loader's description of every board with a [loader] section."""
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
        for key in ('gic_redist_size', 'gic_redist_stride', 'uart'):
            if key in platform:
                fields.append(f'.{key} = {platform[key]:#010x}')
        fields += [f'.psci_hvc = {int(platform["psci"] == "hvc")}',
                   f'.log_other_cpu = {int(loader.get("log_other_cpu", False))}',
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


# ---- an Android device tree source, read for what a board file wants -----------------------------

class Node:
    def __init__(self, name, parent=None, labels=()):
        self.name, self.props, self.children, self.parent, self.labels = name, {}, [], parent, list(labels)

    def walk(self):
        yield self
        for child in self.children:
            yield from child.walk()


def parse_dts(text):
    """A device tree source (dtc -I dtb -O dts, or a vendor's .dts with its includes already merged) as a
    tree of nodes. Properties keep their text; cells(), strings() below read it. Not a full parser: what it
    does not understand (/directives/, labels, deletions) it skips."""
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'(?m)//.*$', '', text)
    position, root = 0, Node('/')
    stack = [root]
    token = re.compile(r'\s*(?:(?P<close>\}\s*;)|(?P<node>(?P<labels>(?:[\w,.+-]+:\s*)*)(?P<name>[\w,.+@/-]+)\s*\{)|'
                       r'(?P<prop>[\w,.+#?-]+)\s*(?:=\s*(?P<value>(?:"(?:[^"\\]|\\.)*"|<[^>]*>|\[[^\]]*\]|[^;"<\[])*))?;)')
    while position < len(text):
        match = token.match(text, position)
        if match is None:
            # something of the syntax this does not know: on to the next line
            end = text.find('\n', position)
            position = len(text) if end < 0 else end + 1
            continue
        position = match.end()
        if match['close']:
            if len(stack) > 1:
                stack.pop()
        elif match['node']:
            node = Node(match['name'], stack[-1], re.findall(r'([\w,.+-]+):', match['labels']))
            stack[-1].children.append(node)
            stack.append(node)
        else:
            stack[-1].props[match['prop']] = (match['value'] or '').strip()
    return root


def cells(value):
    """The numbers of a property written as <...> (several groups are joined)."""
    return [int(c, 0) for group in re.findall(r'<([^>]*)>', value or '') for c in group.split()
            if re.fullmatch(r'(0[xX][0-9a-fA-F]+|\d+)', c)]


def strings(value):
    """The strings of a property: "a", "b" or "a\\0b"."""
    return [part for s in re.findall(r'"((?:[^"\\]|\\.)*)"', value or '') for part in s.split('\\0')]


def reg_named(node, name):
    """The address of the reg entry with that name in reg-names (64-bit cells: address then size, each of
    one or two words; taken as pairs of equal width)."""
    names = strings(node.props.get('reg-names'))
    reg = cells(node.props.get('reg'))
    if name not in names or not reg:
        return None
    index = names.index(name)
    width = len(reg) // len(names) // 2 if len(names) and len(reg) % (2 * len(names)) == 0 else 1
    words = reg[index * 2 * width: index * 2 * width + width]
    address = 0
    for word in words:
        address = address << 32 | word
    return address or None


def unit_address(node):
    return int(node.name.split('@', 1)[1], 16) if '@' in node.name and re.fullmatch(r'[0-9a-fA-F]+', node.name.split('@', 1)[1]) else None


def referred(tree, value):
    """The node a property's first reference points at: <&label ...> in a source, <phandle ...> in a
    tree that was compiled and dumped again."""
    label = re.search(r'<\s*&([\w,.+-]+)', value or '')
    if label:
        return next((node for node in tree.walk() if label[1] in node.labels), None)
    numbers = cells(value)
    if not numbers:
        return None
    return next((node for node in tree.walk() if cells(node.props.get('phandle')) == numbers[:1]), None)


def pmic_of(node):
    """(SPMI slave, name) of the PMIC a peripheral's node is in: qcom,pm8350@1 { ... } is slave 1, "PM8350"."""
    pmic = node.parent
    if pmic is None or 'qcom,spmi-pmic' not in strings(pmic.props.get('compatible')):
        return None, None
    reg = cells(pmic.props.get('reg'))
    slave = reg[0] if reg else unit_address(pmic)
    return slave, pmic.name.split('@')[0].split(',')[-1].upper()


def from_dts(text, panel=None):
    """What a device tree says of a board, and what it leaves open. Returns (soc_values, board_values, notes)."""
    tree = parse_dts(text)
    soc, board, notes = {}, {}, []
    tsens, panels, cpus = [], [], []
    platform, keys, rpmh = {}, {}, {}
    for node in tree.walk():
        compatible = strings(node.props.get('compatible'))
        if 'arm,gic-v3' in compatible:
            reg = cells(node.props.get('reg'))
            if len(reg) == 4:                   # one cell an address
                platform.update(gic_dist=reg[0], gic_redist=reg[2])
            elif len(reg) >= 8:                 # two
                platform.update(gic_dist=reg[0] << 32 | reg[1], gic_redist=reg[4] << 32 | reg[5])
        if re.fullmatch(r'cpu@[0-9a-fA-F]+', node.name) and 'enable-method' in node.props:
            reg = cells(node.props.get('reg'))
            capacity = cells(node.props.get('capacity-dmips-mhz'))
            # <&cpufreq_hw GROUP ...>: in a compiled tree the label is a number too, and comes first
            domain = cells(node.props.get('qcom,freq-domain'))
            group = (domain[1] if '&' not in node.props.get('qcom,freq-domain', '') and len(domain) >= 2 else
                     domain[0] if domain else None)
            cpus.append((reg[-1] if reg else int(node.name.split('@')[1], 16), capacity[0] if capacity else 0, group))
        if node.name == 'psci' and strings(node.props.get('method')):
            platform['psci'] = strings(node.props['method'])[0]
        if reg_named(node, 'dsi_ctrl') and node.props.get('cell-index', '<0>').strip() in ('<0>', '<0x00>', '<0x0>'):
            soc.setdefault('display', {})['dsi0'] = reg_named(node, 'dsi_ctrl')
        if reg_named(node, 'mdp_intf_base') and node.props.get('cell-index', '<0>').strip() in ('<0>', '<0x00>', '<0x0>'):
            soc.setdefault('display', {})['intf1'] = reg_named(node, 'mdp_intf_base')
        if reg_named(node, 'tsens_tm_physical'):
            tsens.append(reg_named(node, 'tsens_tm_physical'))
        if any(c.startswith('qcom,cpufreq-hw') or c == 'qcom,cpufreq-epss' for c in compatible):
            reg = cells(node.props.get('reg'))
            names = strings(node.props.get('reg-names'))
            if names and reg and len(reg) % len(names) == 0:
                width = len(reg) // len(names) // 2 or 1
                first = reg[width - 1] if width else reg[0]
                second = reg[2 * width + width - 1] if len(names) > 1 else None
                soc['cpufreq'] = {'epss': first, 'epss_domain_size': (second - first) if second and second > first else 0x1000}
        if any(re.fullmatch(r'qcom,.*-dispcc(-.*)?|qcom,dispcc-.*', c) for c in compatible) and unit_address(node):
            soc.setdefault('clocks', {})['dispcc'] = unit_address(node)
        if any(re.fullmatch(r'qcom,.*-gcc(-.*)?|qcom,gcc-.*', c) for c in compatible) and unit_address(node):
            soc.setdefault('clocks', {})['gcc'] = unit_address(node)
        if 'qcom,mdss-dsi-panel-type' in node.props:
            panels.append(node)
        if 'qcom,spmi-pmic-arb' in compatible and all(reg_named(node, n) for n in ('core', 'cnfg', 'obsrvr')):
            soc['spmi'] = {'core': reg_named(node, 'core'), 'cnfg': reg_named(node, 'cnfg'),
                           'observer': reg_named(node, 'obsrvr'), 'ee': (cells(node.props.get('qcom,ee')) or [0])[0]}
            if reg_named(node, 'chnls'):
                soc['spmi']['channels'] = reg_named(node, 'chnls')
        if 'qcom,cmd-db' in compatible:
            reg = cells(node.props.get('reg'))
            if len(reg) in (2, 4):              # address and size, of one cell each or two
                half = len(reg) // 2
                rpmh.update(cmd_db=reg[half - 1], cmd_db_size=reg[-1])
        if 'qcom,rpmh-rsc' in compatible and reg_named(node, 'drv-2'):
            # the application processor's: the one whose drv-2 is this system's (qcom,drv-id = <2>)
            if cells(node.props.get('qcom,drv-id')) in ([], [2]):
                rpmh['rsc'] = reg_named(node, 'drv-2')
                offset = cells(node.props.get('qcom,tcs-offset'))
                if offset:
                    rpmh['tcs_offset'] = offset[0]
        if 'qcom,qpnp-power-on' in compatible and unit_address(node):
            # the power-on block of the PMIC with the keys: power on KPDPWR_N, volume down on RESIN_N
            slave, pmic = pmic_of(node)
            if slave is not None:
                keys.update(pon=slave << 8 | unit_address(node) >> 8, pon_name=f"the {pmic}'s power-on block")
        if 'gpio-keys' in compatible:
            for key in node.children:
                code, pins = cells(key.props.get('linux,code')), cells(key.props.get('gpios'))
                if code != [KEY_VOLUMEUP]:
                    continue
                gpios = referred(tree, key.props.get('gpios'))
                # <&gpios N flags> in a source (the label is not a number), <phandle N flags> once compiled
                number = pins[0] if '&' in key.props.get('gpios', '') else pins[1] if len(pins) > 1 else None
                slave, pmic = pmic_of(gpios) if gpios is not None else (None, None)
                if slave is None or number is None or unit_address(gpios) is None:
                    notes.append("volume up's key is not a GPIO of a PMIC (or its controller is not in this tree): "
                                 '[buttons] volume_up is to be filled in by hand')
                    continue
                # GPIO n of pinctrl@8800 has its registers at 0x8800 + 0x100 * (n - 1)
                keys.update(volume_up=slave << 8 | ((unit_address(gpios) >> 8) + number - 1),
                            volume_up_name=f'GPIO{number} of the {pmic}')
    if all(key in rpmh for key in ('cmd_db', 'cmd_db_size', 'rsc', 'tcs_offset')):
        soc['rpmh'] = rpmh
    if keys:
        board['buttons'] = keys
    if tsens:
        soc['thermal'] = {'tsens': sorted(set(tsens))[:2]}
    if cpus:
        # Qualcomm numbers its cores 0x000, 0x100 ...: in MPIDR's second affinity field
        shift = 8 if any(number >= 0x100 for number, _, _ in cpus) else 0
        capacities = [capacity for _, capacity, _ in cpus if capacity]
        small = 0
        groups = [group for _, _, group in sorted(cpus)]
        if 'cpufreq' in soc and None not in groups and groups == sorted(groups):
            soc['cpufreq']['domain_cores'] = [groups.count(group) for group in sorted(set(groups))]
        for number, capacity, _ in cpus:
            if capacities and capacity == min(capacities) and min(capacities) < max(capacities):
                small |= 1 << (number >> shift)
        platform.update(cores=len(cpus), core_shift=shift)
        if small:
            platform['small_cores'] = small
    if platform:
        soc['platform'] = platform
    if panels:
        chosen = [p for p in panels if p.name == panel] if panel else panels
        if panel and not chosen:
            raise SystemExit(f'no panel node "{panel}"; the tree has: {", ".join(p.name for p in panels)}')
        if len(chosen) > 1:
            notes.append('several panels in the tree; took the first. The phone uses one of them (Android: '
                         'cat /proc/cmdline, "msm_drm.dsi_display0="): --panel NODE picks it. They are: ' +
                         ', '.join(p.name for p in chosen))
        node = chosen[0]
        props = node.props
        kind = (strings(props.get('qcom,mdss-dsi-panel-type')) or [''])[0]
        control = (strings(props.get('qcom,mdss-dsi-bl-pmic-control-type')) or [''])[0]
        light = {'panel': node.name, 'panel_name': (strings(props.get('qcom,mdss-dsi-panel-name')) or [''])[0]}
        if control == 'bl_ctrl_dcs':
            low = cells(props.get('qcom,mdss-dsi-bl-min-level'))
            high = cells(props.get('qcom,mdss-dsi-bl-max-level'))
            start = cells(props.get('qcom,mdss-brightness-init-level'))
            light.update(type='dcs', panel_mode='video' if kind == 'dsi_video_mode' else 'command',
                         min=low[0] if low else 1, max=high[0] if high else 255)
            if start:
                light['restore'] = start[0]
            light['bytes'] = 2 if light['max'] > 255 else 1
            if light['bytes'] == 2:
                light['byte_order'] = 'msb-first' if 'qcom,mdss-dsi-bl-inverted-dbv' in props else 'lsb-first'
            if not low or not high:
                notes.append('the panel names no brightness range (qcom,mdss-dsi-bl-min/max-level): guessed')
        else:
            light['type'] = 'soft'
            notes.append(f'the panel\'s light is "{control or "not named"}", not DCS: no driver for that yet, so '
                         'the picture is darkened in software (type "soft")')
        board['backlight'] = light
    else:
        notes.append('no panel node in the tree (it may be in the dtbo: dump that as well)')
    for section, keys in (('display', ['dsi0']), ('thermal', ['tsens']), ('cpufreq', ['epss']),
                          ('platform', ['gic_dist', 'cores', 'psci'])):
        if not all(key in soc.get(section, {}) for key in keys):
            notes.append(f'[{section}] not found in the tree')
    notes.append('gcc_disp_ahb_cbcr, dispcc_mdss_gdscr and dispcc_mdss_ahb_cbcr are not in a device tree: they are '
                 'register offsets from Linux\'s clock drivers for the SoC (gcc-*.c: gcc_disp_ahb_clk; dispcc-*.c: '
                 'the MDSS GDSC and disp_cc_mdss_ahb_clk)')
    return soc, board, notes


def toml_value(value):
    if isinstance(value, bool):
        return 'true' if value else 'false'
    if isinstance(value, int):
        return f'{value:#x}' if value > 4096 else str(value)
    if isinstance(value, list):
        return '[' + ', '.join(toml_value(v) for v in value) + ']'
    return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"') + '"'


def new_board(name, soc_name, dts=None, panel=None, description=None):
    if not NAME.match(name) or not NAME.match(soc_name):
        raise SystemExit('names are lower-case letters, digits and dashes')
    board_path = DEVICES / name / 'board.toml'
    soc_path = SOCS / f'{soc_name}.toml'
    if board_path.exists():
        raise SystemExit(f'{board_path.relative_to(ROOT)} is there already')
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
    board_path.parent.mkdir(parents=True)
    board_path.write_text('\n'.join(lines))
    print(f'wrote {board_path.relative_to(ROOT)}')
    if not soc_path.exists():
        text = [f'# {soc_name}. Started by tools/boards.py new' + (f' from {dts.name}' if dts else '') + '.',
                '# What every board with this SoC shares: where its blocks are (docs/porting.md).',
                f'name = "{soc_name}"',
                f'description = "{soc_name}"',
                '']
        clocks = soc.get('clocks', {})
        if light.get('type') == 'dcs' or clocks:
            text.append('[clocks]')
            for key in ('gcc', 'gcc_disp_ahb_cbcr', 'dispcc', 'dispcc_mdss_gdscr', 'dispcc_mdss_ahb_cbcr'):
                text.append(f'{key} = {toml_value(clocks[key])}' if key in clocks else f'# {key} = 0x...            # TO FILL IN')
            text.append('')
        for section in ('display', 'spmi', 'rpmh', 'thermal', 'cpufreq'):
            if section in soc:
                text.append(f'[{section}]')
                text += [f'{key} = {toml_value(value)}' for key, value in soc[section].items()]
                text.append('')
        if 'platform' in soc:
            text += ['# What the loader tells the kernel of the machine (loader/loader.c, struct platform).',
                     '[platform]',
                     '# io_base = 0x00400000          # TO FILL IN: one window over the SoC\'s peripherals, never 0',
                     '# io_size = 0x3FC00000          # (these two are what the Snapdragon 7-series ports use)']
            text += [f'{key} = {toml_value(value)}' for key, value in soc['platform'].items()]
            text.append('')
        soc_path.write_text('\n'.join(text))
        print(f'wrote {soc_path.relative_to(ROOT)}')
    else:
        print(f'{soc_path.relative_to(ROOT)} is there already: left as it is')
    for note in notes:
        print(f'note: {note}')
    print('next: tools/boards.py check, then docs/porting.md')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('list')
    commands.add_parser('show').add_argument('board')
    commands.add_parser('check')
    commands.add_parser('gen').add_argument('--check', action='store_true')
    new = commands.add_parser('new')
    new.add_argument('name')
    new.add_argument('--soc', required=True)
    new.add_argument('--dts', type=Path)
    new.add_argument('--panel')
    new.add_argument('--description')
    options = parser.parse_args()

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
        stale = 0
        for path, text in ((TABLE, table_text(socs, boards)), (PLATFORMS, platforms_text(socs, boards))):
            if not options.check:
                path.write_text(text)
                print(f'wrote {path.relative_to(ROOT)}')
            elif not path.exists() or path.read_text() != text:
                print(f'{path.relative_to(ROOT)} is not what devices/ and soc/ say: run tools/boards.py gen', file=sys.stderr)
                stale = 1
            else:
                print(f'{path.relative_to(ROOT)}: up to date')
        return stale
    return 0


if __name__ == '__main__':
    sys.exit(main())
