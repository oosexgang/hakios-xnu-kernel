#!/usr/bin/env python3
"""Every relative link in the repository's Markdown points at something that is there, and every
`path` in backticks that looks like a file of this repository exists. Part of `make lint`.

The first generation's documents (userland/) describe a workspace that is not this
repository's layout and are left out. A document that has to name paths that no longer exist puts them
between `<!-- paths of the old layout -->` and `<!-- end -->`; links are checked there all the same.
"""
import fnmatch
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import files  # noqa: E402  (the repository's files: git's list, or a release's)

ROOT = Path(__file__).resolve().parents[1]
LEGACY = ('userland/',)


def made_here():
    """What .gitignore keeps out at a place of its own (its lines that begin with /): what the builds and the tools write,
    which a document may name though a fresh checkout or a release's tree has none of it until it is built"""
    ignore = ROOT / '.gitignore'
    lines = ignore.read_text().split('\n') if ignore.is_file() else []
    return [line.strip()[1:].rstrip('/') for line in lines if line.startswith('/')]


def made(target, outputs):
    target = target.rstrip('/.')
    return any(fnmatch.fnmatchcase(target, p) or target.startswith(p + '/') for p in outputs)
LINK = re.compile(r'\[[^\]]*\]\(([^)\s]+)\)')
# a path in backticks: starts at one of the repository's top-level directories and names a file or a directory
TOP = ('arch', 'devices', 'drivers', 'fs', 'include', 'init', 'lib', 'crypto', 'security', 'kernel', 'loader', 'darwin', 'workbench', 'tools', 'Documentation', 'components', 'docs')
PATH = re.compile(r'`((?:' + '|'.join(TOP) + r')/[A-Za-z0-9_./-]+)`')


def main():
    problems = []
    documents = files.listed(['*.md'], root=ROOT)
    outputs = made_here()
    for name in documents:
        if name.startswith(LEGACY):
            continue
        path = ROOT / name
        text = path.read_text()
        past = False
        for number, line in enumerate(text.split('\n'), 1):
            # between these two marks a document names paths that are gone on purpose (Documentation/migrating.md)
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
                        not (component and (component / target.rstrip('/.')).exists()) and not made(target, outputs):
                    problems.append(f'{name}:{number}: `{target}` is not in the repository')
    for problem in problems:
        print(problem, file=sys.stderr)
    if not problems:
        print(f'check-links: {len([f for f in documents if not f.startswith(LEGACY)])} documents, ok')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
