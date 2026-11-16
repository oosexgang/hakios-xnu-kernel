#!/usr/bin/env python3
"""The Python that shell scripts carry in a quoted here-document (python3 - ... <<'PY'), checked as the
repository's .py files are: it parses, and, with --flakes DIR, each is written there as a file of its own for
pyflakes' checks (tools/lint.sh). An unquoted here-document is left alone: the shell fills it in first.

    tools/heredoc-python.py [--flakes DIR] SCRIPT...

Prints each that does not parse, as SCRIPT:LINE; exit 1 when there is one.
"""
import ast
import re
import sys
from pathlib import Path

START = re.compile(r"""\bpython3\b[^<]*\s-\s.*<<(-?)'([A-Za-z_]+)'\s*$""")


def heredocs(text):
    """(line of the first line of code, code) of each quoted here-document given to python3 -"""
    lines = text.split('\n')
    i = 0
    while i < len(lines):
        m = START.search(lines[i])
        i += 1
        if not m:
            continue
        strip_tabs, tag, first, body = m[1] == '-', m[2], i + 1, []
        while i < len(lines) and (lines[i].lstrip('\t') if strip_tabs else lines[i]) != tag:
            body.append(lines[i].lstrip('\t') if strip_tabs else lines[i])
            i += 1
        i += 1
        yield first, '\n'.join(body) + '\n'


def main(argv):
    flakes = None
    if argv[:1] == ['--flakes']:
        flakes, argv = Path(argv[1]), argv[2:]
    bad = 0
    for script in argv:
        for n, (line, code) in enumerate(heredocs(Path(script).read_text(errors='replace'))):
            try:
                ast.parse(code, f'{script}:{line}')
            except SyntaxError as e:
                print(f'{script}:{line + (e.lineno or 1) - 1}: {e.msg}')
                bad = 1
                continue
            if flakes is not None:
                (flakes / f"{script.replace('/', '__')}__{line}.py").write_text(code)
    return bad


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
