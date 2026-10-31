#!/usr/bin/env python3
"""The two type questions a corpus report cannot answer, counted over a corpus.

`air_to_nir.c` refuses two things about TYPES rather than about operations, and
between them they are the largest part of what Resident Evil 4 still stops at:

  - **a bitcast that changes a value's shape.** NIR has no `mov` between an
    `i32` and a `<2 x half>`: the bits have to be redistributed between
    components, and NIR has an operation for exactly one redistribution of
    exactly one width (`pack_32_2x16` / `unpack_32_2x16`).
  - **an integer of a width NIR does not have.** NIR has 1, 8, 16, 32 and 64
    bits and nothing between; an HLSL bit field reaches AIR as `i2` or `i4`
    through SPIRV-Cross.

A refusal stops a module at its FIRST offending instruction, so a report can
only ever say how many modules stop -- never which shapes or widths exist, nor
whether a second one is hiding behind the first. That is the whole reason this
exists: a fix written for what the histogram shows would MANGLE whatever was
behind it, where a fix written for a census can refuse the rest by name.

The reading is LLVM's own, not this tree's: each module is disassembled with
`llvm-dis` and the types are taken from the text. That makes it a second
reference for the same question our reader answers, which is the point --
`corpus.py` already says what our reader thinks.

`--casts` transcribes air_to_nir.c's shape test: components (a vector's length,
else 1) and the width of the scalar. `i32` is (1, 32) and so is `float`, so
`i32 -> float` is same-shape; `<2 x half>` is (2, 16), so `i32 -> <2 x half>`
is not. A pointer has no shape here and is counted apart: the frontend
translates a pointer bitcast by rewriting the pointee, never by moving bits.

`--widths` counts every instruction that mentions an integer width NIR does not
have, BY OPCODE, because what the widths are used for decides how much a fix
costs: a narrow integer only truncated into and switched on needs a mask, and
one that is added, loaded or compared needs a representation.

usage: typecensus.py [--casts] [--widths] [--list FILE] [--modules] LIB...
       --list FILE   one library path per line (a game's corpus is thousands)
       --modules     also print, per shape-changing pair, how many modules have it
       neither --casts nor --widths: both
"""
import argparse
import collections
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import metallib                                                 # noqa: E402

# An LLVM type as it is spelled in `bitcast <T> <v> to <T>`: a vector, an
# array, a pointer (with or without an address space), a struct name or a
# primitive. The operand between the two types is a single token -- `%12`, a
# named value, or a constant -- and never a type, so taking the token before
# `to` as the operand is exact rather than a heuristic.
TY = r'(?:<\d+ x [^>]+>|\[[^\]]+\]|ptr addrspace\(\d+\)|ptr|[%\w.]+)'
CAST = re.compile(r'bitcast\s+(' + TY + r')\s+(?:[^\s,]+)\s+to\s+(' + TY + r')')

NATIVE = {1, 8, 16, 32, 64}
IWIDTH = re.compile(r'\bi(\d+)\b')
# The opcode of an instruction line. A switch's CASE lines (`  i2 0, label %7`)
# start with what looks like an identifier, so they are recognised first and
# counted as case labels rather than as 10 968 instructions called "i2".
CASELBL = re.compile(r'^\s+i\d+\s+-?\d+,\s*label\s')
OPCODE = re.compile(r'^\s+(?:%\S+\s*=\s*)?([a-z][a-z0-9_.]*)[\s(]')


def shape(ty):
    """(components, bit width) the way air_nir_components/air_nir_bit_size read it.

    None for anything with no single width -- a pointer, an array, a struct --
    because those are not values this frontend moves bit for bit.
    """
    n = 1
    m = re.fullmatch(r'<(\d+) x (.+)>', ty)
    if m:
        n, ty = int(m.group(1)), m.group(2).strip()
    if ty.startswith('ptr') or ty.startswith('[') or ty.startswith('%'):
        return None
    bits = {'half': 16, 'bfloat': 16, 'float': 32, 'double': 64,
            'x86_fp80': 80, 'fp128': 128, 'ppc_fp128': 128}.get(ty)
    if bits is None:
        m = re.fullmatch(r'i(\d+)', ty)
        if not m:
            return None
        bits = int(m.group(1))
    return (n, bits)


def disassemble(path):
    """Every function of one library as LLVM assembly text."""
    b, header, funcs = metallib.parse(path)
    for f in funcs:
        bc, _ = metallib.bitcode_of(b, header, f)
        p = subprocess.run(['llvm-dis', '-o', '-', '-'], input=bc,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if p.returncode != 0:
            raise SystemExit('typecensus: llvm-dis failed on %s %s: %s'
                             % (path, f.get('name'), p.stderr.decode()[:200]))
        yield f.get('name'), p.stdout.decode('utf-8', 'replace')


def casts_of(text):
    out = [(m.group(1), m.group(2)) for m in CAST.finditer(text)]
    # A line this does not parse would silently drop a cast, and the whole
    # value of this census is that nothing was dropped, so the two counts are
    # compared and a mismatch is fatal rather than a warning.
    if text.count('bitcast') != len(out):
        raise SystemExit('typecensus: a bitcast this does not parse')
    return out


def widths_of(text):
    """(width, opcode) for every instruction mentioning a non-native width."""
    out = []
    body = False
    for ln in text.splitlines():
        if ln.startswith('define'):
            body = True
            continue
        if ln.startswith('}'):
            body = False
            continue
        if not body or not ln.startswith('  '):
            continue
        ws = {int(x) for x in IWIDTH.findall(ln) if int(x) not in NATIVE}
        if not ws:
            continue
        if CASELBL.match(ln):
            op = '(a switch case label)'
        else:
            m = OPCODE.match(ln)
            op = m.group(1) if m else '(unparsed) ' + ln.strip()[:40]
        for w in sorted(ws):
            out.append((w, op))
    return out


def report_casts(pairs, mods, modules):
    def kind(src, dst):
        a, b = shape(src), shape(dst)
        if a is None or b is None:
            return 'pointer or aggregate'
        return 'same shape' if a == b else 'SHAPE-CHANGING'

    totals = collections.Counter()
    for (src, dst), n in pairs.items():
        totals[kind(src, dst)] += n
    print('%d bitcasts' % sum(pairs.values()))
    for k in ('SHAPE-CHANGING', 'same shape', 'pointer or aggregate'):
        print('  %-22s %7d' % (k, totals[k]))
    for (src, dst), n in pairs.most_common():
        line = '%8d  %-20s -> %-20s %s' % (n, src, dst, kind(src, dst))
        if modules and kind(src, dst) == 'SHAPE-CHANGING':
            line += '  (%d modules)' % len(mods[(src, dst)])
        print(line)


def report_widths(widths, wmods):
    byw = collections.Counter()
    for (w, op), n in widths.items():
        byw[w] += n
    print('%d instructions mention an integer width NIR does not have' % sum(widths.values()))
    for w in sorted(byw):
        ops = collections.Counter()
        for (ww, op), n in widths.items():
            if ww == w:
                ops[op] += n
        print('  i%-3d %7d in %4d modules' % (w, byw[w], len(wmods[w])))
        for op, n in ops.most_common():
            print('        %7d  %s' % (n, op))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('libs', nargs='*')
    ap.add_argument('--list')
    ap.add_argument('--casts', action='store_true')
    ap.add_argument('--widths', action='store_true')
    ap.add_argument('--modules', action='store_true')
    args = ap.parse_args()
    libs = list(args.libs)
    if args.list:
        with open(args.list) as f:
            libs += [ln.strip() for ln in f if ln.strip()]
    if not libs:
        ap.error('no libraries: give paths, or --list FILE')
    do_casts = args.casts or not args.widths
    do_widths = args.widths or not args.casts

    pairs, mods = collections.Counter(), collections.defaultdict(set)
    widths, wmods = collections.Counter(), collections.defaultdict(set)
    for path in libs:
        for _, text in disassemble(path):
            if do_casts:
                for pair in casts_of(text):
                    pairs[pair] += 1
                    mods[pair].add(path)
            if do_widths:
                for w, op in widths_of(text):
                    widths[(w, op)] += 1
                    wmods[w].add(path)

    print('%d libraries' % len(libs))
    if do_casts:
        print()
        report_casts(pairs, mods, args.modules)
    if do_widths:
        print()
        report_widths(widths, wmods)


if __name__ == '__main__':
    main()
