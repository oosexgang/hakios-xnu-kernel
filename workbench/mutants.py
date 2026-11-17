#!/usr/bin/env python3
"""Which small changes to a driver does no scenario of its test notice? The simple ones are made here --
a comparison turned the other way or made inclusive, an `&&` made `||`, a `+ 1` taken away -- one at a time,
and each is run by workbench/mutate.sh; what survives is printed. A survivor is a scenario to write, or a
change that makes no difference (a `>` that may as well be `>=` where two equal values give the same
result): a person tells which.

    workbench/mutants.py TEST SOURCE [MAX]

    TEST     a test of workbench/run.sh: guard_test, backlight_driver_test, ...
    SOURCE   the file it includes, as it includes it: board/guard.c
    MAX      at most this many changes (default 60; each builds and runs the test, some seconds)

The changes are taken in the file's order; MUTANTS_FROM=N leaves out the first N, so that a long file is gone
through a part at a time (MUTANTS_FROM=60 workbench/mutants.py ufs_driver_test board/ufs.c 60: its second sixty).

Exit 0 when nothing survived, 1 when something did.
"""
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCES = (HERE / '../drivers', HERE / '..', HERE / '../arch/arm64/boot', HERE / '../Documentation/examples')
SWAPS = [(' <= ', ' < '), (' >= ', ' > '), (' < ', ' <= '), (' > ', ' >= '), (' == ', ' != '), (' != ', ' == '),
         (' && ', ' || '), (' || ', ' && '), (' + 1', ''), (' - 1', '')]


def code_lines(text):
    """The lines of code: not comments, not what only says something (printf and the like)."""
    in_comment = in_assert = False
    for line in text.split('\n'):
        stripped = line.strip()
        # (a layout asserted when the file is compiled, over as many lines as it takes: a change to it is a build that
        # fails, or one that does not, never a run)
        if in_assert or stripped.startswith(('_Static_assert', 'static_assert')):
            in_assert = not stripped.endswith(';')
            continue
        if '/*' in stripped and '*/' not in stripped:
            in_comment = True
        if in_comment:
            in_comment = '*/' not in stripped
            continue
        if not stripped or stripped.startswith(('/*', '*', '//', '#')) or re.search(r'\b(printf|say|log|note)\(', line):
            continue
        yield re.sub(r'/\*.*?\*/', '', line).strip()


def mutants(text):
    """(old, new) pairs: each `old` occurs once in the file, as mutate.sh replaces the first occurrence."""
    seen = set()
    for code in code_lines(text):
        if len(code) < 12 or text.count(code) != 1:
            continue
        for old, new in SWAPS:
            if old in code and (code, old) not in seen:
                seen.add((code, old))
                yield code, code.replace(old, new, 1)


def main(argv):
    if len(argv) not in (3, 4):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    test, source = argv[1], argv[2]
    limit = int(argv[3]) if len(argv) == 4 else 60
    path = next((root / source for root in SOURCES if (root / source).is_file()), None)
    if path is None:
        print(f'mutants: no {source} among the drivers, the loader or the examples', file=sys.stderr)
        return 2
    tried = survived = 0
    first = int(os.environ.get('MUTANTS_FROM', '0'))
    for old, new in list(mutants(path.read_text()))[first:first + limit]:
        result = subprocess.run([str(HERE / 'mutate.sh'), test, source, old, new], capture_output=True, text=True)
        if result.returncode == 2:
            continue                    # (does not compile: not a change to judge)
        tried += 1
        if result.returncode == 1:
            survived += 1
            print(f'survived: {old}\n      as: {new}', flush=True)
    print(f'mutants: {tried} changes to {source}, {survived} not noticed by {test}')
    return 1 if survived else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
