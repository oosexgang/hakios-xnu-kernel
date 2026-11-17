#!/usr/bin/env python3
"""Every header the build makes, from the data and the configuration it is made of -- one place, as Linux's and
Android's gen_headers are:

  tools/gen_headers.py [--out DIR] [--devices DIR] [FRAGMENT...]
      into DIR (build/boards): the board tables of the device packages that are there (tools/boards.py gen:
      board_table.h for the kernel, platforms.h for the loader, user_boards.h for user space) and the kernel's
      configuration (tools/kconfig.py: kconfig.h, of FRAGMENT..., else of KCONFIG_FRAGMENTS, else every default).
      kernel/build.sh, the loader's build and the tests run this.

  tools/gen_headers.py --user [--out DIR]
      the headers user space shares with the kernel (include/board/*.h, sys/board/ in the kernel's tree) as user space
      is to see them, into DIR/sys/board/ (build/headers): what is the kernel's alone in them (#ifdef KERNEL ...
      #endif, the #else of an #ifndef KERNEL) is left out, and each one is compiled on its own (cc -fsyntax-only) to
      be sure it stands without the kernel. Programs of user space take them with -I build/headers.

A header is written only when it differs from what is there: a build with the same boards and configuration
recompiles nothing for it.
"""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

import kconfig  # noqa: E402

USER_SOURCES = ROOT / 'include/board'

# what a Darwin header has and a Linux machine's compiler does not, for the check alone (the headers say <sys/ioccom.h>
# as Darwin's user space has it)
SHIMS = {'sys/ioccom.h': '#include <sys/ioctl.h>\n'}


def write(path, text):
    """TEXT into PATH, only when it differs: whether it was written"""
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file() and path.read_text() == text:
        return False
    path.write_text(text)
    return True


def without_kernel(text, name):
    """TEXT with what only the kernel sees left out: the body of #ifdef KERNEL / #if defined(KERNEL) / #if KERNEL, and
    the #else of #ifndef KERNEL / #if !defined(KERNEL) -- nested conditions of other names kept as they are"""
    out = []
    stack = []                  # each open conditional: None (not about KERNEL), or whether its lines are kept
    keeping = True
    kernel_on = re.compile(r'#\s*(ifdef\s+KERNEL\b|if\s+(defined\s*\(\s*KERNEL\s*\)|KERNEL)\s*$)')
    kernel_off = re.compile(r'#\s*(ifndef\s+KERNEL\b|if\s+!\s*defined\s*\(\s*KERNEL\s*\)\s*$)')
    for number, line in enumerate(text.split('\n'), 1):
        stripped = line.strip()
        if kernel_on.match(stripped) or kernel_off.match(stripped):
            kept = bool(kernel_off.match(stripped))
            stack.append(('kernel', kept, keeping))
            keeping = keeping and kept
            continue
        if re.match(r'#\s*if', stripped):
            stack.append(('other', None, keeping))
            if keeping:
                out.append(line)
            continue
        if re.match(r'#\s*(else|elif)\b', stripped):
            if not stack:
                raise SystemExit(f'gen_headers: {name}:{number}: #else without its #if')
            kind, kept, outer = stack[-1]
            if kind == 'kernel':
                if stripped.startswith('#elif') or re.match(r'#\s*elif', stripped):
                    raise SystemExit(f'gen_headers: {name}:{number}: an #elif of a KERNEL condition is not taken apart here')
                stack[-1] = ('kernel', not kept, outer)
                keeping = outer and not kept
                continue
            if keeping:
                out.append(line)
            continue
        if re.match(r'#\s*endif\b', stripped):
            if not stack:
                raise SystemExit(f'gen_headers: {name}:{number}: #endif without its #if')
            kind, _, outer = stack.pop()
            keeping = outer
            if kind == 'other' and keeping:
                out.append(line)
            continue
        if keeping:
            out.append(line)
    if stack:
        raise SystemExit(f'gen_headers: {name}: a conditional is not closed')
    return '\n'.join(out)


def user_headers(out, compiler='cc'):
    """include/board/*.h as user space sees them, into OUT/sys/board/; each compiled on its own. Returns the problems."""
    problems = []
    written = 0
    for source in sorted(USER_SOURCES.glob('*.h')):
        name = source.relative_to(ROOT) if source.is_relative_to(ROOT) else source
        text = without_kernel(source.read_text(), name)
        note = f'/* {name}, as user space sees it (tools/gen_headers.py --user): do not edit */\n'
        written += write(out / 'sys/board' / source.name, note + text)
        if re.search(r'\bKERNEL\b', re.sub(r'/\*.*?\*/', '', text, flags=re.S)):
            problems.append(f'{source.name}: still names KERNEL after what is the kernel\'s was left out')
    if shutil.which(compiler):
        with tempfile.TemporaryDirectory() as shim:
            for name, text in SHIMS.items():
                write(Path(shim) / name, text)
            for header in sorted((out / 'sys/board').glob('*.h')):
                # (nothing included for it: a header that uses uint32_t is to include <stdint.h> itself)
                done = subprocess.run([compiler, '-fsyntax-only', '-x', 'c', '-std=gnu11', '-I', str(out), '-I',
                                       str(out / 'sys/board'), '-idirafter', shim, '-'],
                                      input=f'#include <sys/board/{header.name}>\n', capture_output=True, text=True)
                if done.returncode != 0:
                    problems.append(f'{header.name}: does not compile on its own: {done.stderr.strip().splitlines()[0] if done.stderr.strip() else "?"}')
    else:
        print(f'gen_headers: no {compiler}: the headers are not compiled on their own', file=sys.stderr)
    # (a header of user space whose source is gone is taken away with it)
    for stale in sorted((out / 'sys/board').glob('*.h')):
        if not (USER_SOURCES / stale.name).is_file():
            stale.unlink()
    print(f'gen_headers: {out}/sys/board: {len(list(USER_SOURCES.glob("*.h")))} headers ({written} written)')
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out')
    parser.add_argument('--devices', help='the device packages\' directory, as tools/boards.py --devices')
    parser.add_argument('--user', action='store_true', help='the headers user space shares with the kernel')
    parser.add_argument('fragments', nargs='*')
    options = parser.parse_args(argv)
    if options.user:
        problems = user_headers(Path(options.out or ROOT / 'build/headers'))
        for problem in problems:
            print(f'gen_headers: {problem}', file=sys.stderr)
        return 1 if problems else 0
    out = Path(options.out or ROOT / 'build/boards')
    arguments = (['--devices', options.devices] if options.devices else []) + ['gen', '--out', str(out)]
    code = subprocess.run([sys.executable, str(ROOT / 'tools/boards.py'), *arguments]).returncode
    if code:
        return code
    try:
        configuration = kconfig.Kconfig()
        values, warnings, paths = kconfig.configuration(configuration, options.fragments)
    except kconfig.KconfigError as error:
        print(f'gen_headers: {error}', file=sys.stderr)
        return 2
    for warning in warnings:
        print(f'gen_headers: kconfig: {warning}', file=sys.stderr)
    written = write(out / 'kconfig.h', kconfig.header(configuration, values, paths))
    print(f'gen_headers: {out}/kconfig.h: {sum(values.values())} of {len(values)} symbols on '
          f'({", ".join(p.stem for p in paths) or "every default"}){"" if written else ", as it was"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
