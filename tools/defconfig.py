#!/usr/bin/env python3
"""The boot arguments of a defconfig of arch/arm64/configs, as Android's kernels keep their configurations there.

  tools/defconfig.py NAME        the arguments of arch/arm64/configs/NAME_defconfig (or of the file NAME), on one line
  tools/defconfig.py --list      the defconfigs there are, each with its first line of description

A defconfig is the boot arguments of one kind of run, one a line: NAME or NAME=VALUE, a comment after #. A line
"include OTHER" takes OTHER_defconfig's first. A later argument of the same name replaces an earlier one, as ./bench
does with a board's arguments (NAME=KEY=VALUE, as caenv=CA_X=1, is one of several of that NAME, one for each KEY).
./bench reads one (BENCH_DEFCONFIG=NAME), and so does tools/qemu-kernel.sh (--defconfig NAME).
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONFIGS = ROOT / 'arch/arm64/configs'


def path_of(name):
    path = Path(name)
    if path.is_file():
        return path
    path = CONFIGS / (name if name.endswith('_defconfig') else name + '_defconfig')
    if not path.is_file():
        raise SystemExit(f'defconfig.py: no defconfig {name} (tools/defconfig.py --list)')
    return path


def key(argument):
    parts = argument.split('=', 2)
    return '='.join(parts[:2]) if len(parts) == 3 else parts[0]


def arguments(name, seen=()):
    """{key: argument} of a defconfig, its includes first, in the order they come"""
    path = path_of(name)
    if path.resolve() in seen:
        raise SystemExit(f'defconfig.py: {path.name} includes itself')
    found = {}
    for number, line in enumerate(path.read_text().split('\n'), 1):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        if line.startswith('include '):
            found.update(arguments(line.split(None, 1)[1].strip(), seen + (path.resolve(),)))
            continue
        if ' ' in line or '\t' in line:
            raise SystemExit(f'defconfig.py: {path.name}:{number}: one argument a line, not "{line}"')
        found[key(line)] = line
    return found


def description(path):
    for line in path.read_text().split('\n'):
        if line.startswith('#'):
            return line.lstrip('# ').strip()
    return ''


def main(argv):
    if argv == ['--list']:
        for path in sorted(CONFIGS.glob('*_defconfig')):
            print(f'{path.name[:-len("_defconfig")]:24s} {description(path)}')
        return 0
    if len(argv) != 1 or argv[0].startswith('-'):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    print(' '.join(arguments(argv[0]).values()))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
