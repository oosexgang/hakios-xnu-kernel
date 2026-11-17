#!/usr/bin/env python3
"""What to run after changing a file: the parts of MAINTAINERS it belongs to, their tests, how to run each alone, and
the documents to read again.

  scripts/tests-for.py FILE...          the files named (paths from the top of the repository, or from here)
  scripts/tests-for.py --changed [REF]  the files changed since REF (default: the last commit), committed or not
  ... --run                             and run those tests, one after another: which passed, which did not

A test of the drivers (workbench/NAME.c) runs alone with `workbench/mutate.sh NAME SOURCE x x` -- SOURCE one of the files
it includes, x for x no change -- every scenario of it on this machine; a Python test with python3; a script as it is.
make check runs them all.
"""
import argparse
import fnmatch
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def areas(text=None):
    """[(name, {key: [values]})] of MAINTAINERS, in its order."""
    text = (ROOT / 'MAINTAINERS').read_text() if text is None else text
    found, current = [], None
    for line in text.split('\n'):
        # (an area's name is a line of capitals of its own; the lines of the head above the first are not)
        if line and not line.startswith(('\t', ' ')) and line[1:3] != ':\t' and line == line.upper() and any(c.isalpha() for c in line):
            current = (line.strip(), {})
            found.append(current)
        elif current is not None and len(line) > 3 and line[1] == ':' and line[2] == '\t' and line[0] in 'FTDK':
            current[1].setdefault(line[0], []).append(line[3:].strip())
    return found


def covers(pattern, path):
    """Does the F: PATTERN take PATH (both from the top of the repository)?"""
    if pattern.endswith('/'):
        return path.startswith(pattern)
    return fnmatch.fnmatchcase(path, pattern) and path.count('/') == pattern.count('/')


def run_line(test):
    """How a T: line is run alone."""
    path = test.split(' ')[0]
    if ' (' in test:
        return test
    name = Path(path)
    if name.suffix == '.c' and name.parent.as_posix() in ('workbench', 'Documentation/examples'):
        # (mutate.sh with no change -- x for x -- in a file the test includes: the test as it is, every scenario)
        try:
            text = (ROOT / path).read_text(errors='replace')
        except OSError:
            text = ''
        source = re.search(r'#include "((?:board|src)/[^"]+|example_led\.c)"', text)
        if source:
            return f'workbench/mutate.sh {name.stem} {source.group(1)} x x'
        return f'make test (workbench/run.sh: {name.stem})'
    if name.suffix == '.py':
        return f'python3 {path}'
    return path


def relative(path):
    path = Path(path)
    absolute = path.resolve() if path.exists() or path.is_absolute() else (Path.cwd() / path).resolve()
    try:
        return absolute.relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('files', nargs='*', metavar='FILE')
    parser.add_argument('--changed', nargs='?', const='HEAD', metavar='REF')
    parser.add_argument('--run', action='store_true', help='run the tests found, and say which passed')
    options = parser.parse_args(arguments)
    files = [relative(f) for f in options.files]
    if options.changed:
        done = subprocess.run(['git', '-C', str(ROOT), 'diff', '--name-only', options.changed], capture_output=True, text=True)
        if done.returncode != 0:
            raise SystemExit(f'tests-for.py: git diff {options.changed}: {done.stderr.strip()}')
        files += [f for f in done.stdout.split('\n') if f]
        if options.changed == 'HEAD':
            files += [f for f in subprocess.run(['git', '-C', str(ROOT), 'diff', '--name-only', 'HEAD~1', 'HEAD'],
                                                 capture_output=True, text=True).stdout.split('\n') if f]
    if not files:
        parser.error('which files? FILE..., or --changed')
    known = areas()
    tests, documents, nowhere = {}, {}, []
    for path in dict.fromkeys(files):
        mine = [(name, keys) for name, keys in known if any(covers(p, path) for p in keys.get('F', []))]
        if not mine:
            nowhere.append(path)
        for name, keys in mine:
            print(f'{path}: {name}')
            for test in keys.get('T', []):
                tests.setdefault(run_line(test), name)
            for document in keys.get('D', []):
                documents.setdefault(document, name)
    if tests:
        print('\nto run:')
        for line in tests:
            print(f'  {line}')
    if documents:
        print('\nto read again, if what it says has changed:')
        for line in documents:
            print(f'  {line}')
    for path in nowhere:
        print(f'{path}: in no part of MAINTAINERS (make check runs everything)', file=sys.stderr)
    if options.run:
        return run(list(tests))
    print('\nall of them: make check')
    return 0


def runnable(lines):
    """The lines that are a command to run here (not those that say how they are run elsewhere, in brackets)."""
    return [line for line in lines if '(' not in line and not line.startswith('make ')]


def run(lines):
    """Each test, one after another, from the top of the repository: 0 when every one passed."""
    failed = []
    commands = runnable(lines)
    for line in commands:
        print(f'\n== {line}', flush=True)
        if subprocess.run(['bash', '-c', line], cwd=ROOT).returncode != 0:
            failed.append(line)
    left = [line for line in lines if line not in commands]
    print(f'\n{len(commands) - len(failed)} of {len(commands)} passed' + (': not ' + '; '.join(failed) if failed else '') +
          (f' ({len(left)} run otherwise: {"; ".join(left)})' if left else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
