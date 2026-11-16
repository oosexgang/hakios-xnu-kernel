#!/usr/bin/env python3
"""Every relative link in the repository's Markdown points at something that is there, and every
`path` in backticks that looks like a file of this repository exists. Part of `make lint`.

The first generation's documents (base/, userland/) describe a workspace that is not this
repository's layout and are left out. A document that has to name paths that no longer exist puts them
between `<!-- paths of the old layout -->` and `<!-- end -->`; links are checked there all the same.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LEGACY = ('base/', 'userland/')
LINK = re.compile(r'\[[^\]]*\]\(([^)\s]+)\)')
# a path in backticks: starts at one of the repository's top-level directories and names a file or a directory
TOP = ('devices', 'kernel', 'loader', 'darwin', 'workbench', 'tools', 'docs')
PATH = re.compile(r'`((?:' + '|'.join(TOP) + r')/[A-Za-z0-9_./-]+)`')


def main():
    files = subprocess.run(['git', 'ls-files', '*.md'], cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
    problems = []
    for name in files:
        if name.startswith(LEGACY):
            continue
        path = ROOT / name
        text = path.read_text()
        past = False
        for number, line in enumerate(text.split('\n'), 1):
            # between these two marks a document names paths that are gone on purpose (docs/migrating.md)
            if '<!-- paths of the old layout -->' in line:
                past = True
            elif '<!-- end -->' in line:
                past = False
            for target in LINK.findall(line):
                if re.match(r'[a-z]+:', target) or target.startswith('#'):
                    continue
                if not (path.parent / target.split('#')[0]).exists():
                    problems.append(f'{name}:{number}: link to {target}, which is not there')
            for target in ([] if past else PATH.findall(line)):
                # placeholders (NAME, SOC, …) and globs are not paths
                if re.search(r'NAME|SOC|[*…]|\.\.\.', target):
                    continue
                # (from the repository's top, or from the document's own directory: components/metal/plugin/README.md names components/metal/plugin/'s
                # files as components/metal/plugin/ itself does, tools/ and tests/ of its own)
                # (or from the top of the component the document is in: components/metal/docs/air.md names tools/air/ as
                # components/metal's scripts are run, from components/metal)
                parts = Path(name).parts
                component = ROOT / parts[0] / parts[1] if len(parts) > 2 and parts[0] == 'components' else None
                if not (ROOT / target.rstrip('/.')).exists() and not (path.parent / target.rstrip('/.')).exists() and \
                        not (component and (component / target.rstrip('/.')).exists()):
                    problems.append(f'{name}:{number}: `{target}` is not in the repository')
    for problem in problems:
        print(problem, file=sys.stderr)
    if not problems:
        print(f'check-links: {len([f for f in files if not f.startswith(LEGACY)])} documents, ok')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
