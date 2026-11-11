#!/usr/bin/env python3
"""components.toml: everything the project takes from elsewhere, at the exact version it takes.

  tools/components.py            list them
  tools/components.py check      every pin named there is the pin the fetching script has (`make lint`)

The manifest is checked against the scripts rather than read by them: a script stays runnable on its
own, and the manifest cannot drift from what is really fetched.
"""
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / 'components.toml'
KEYS = ('id', 'what', 'source', 'pins', 'pinned_in', 'used_for', 'licence')


def load():
    return tomllib.loads(MANIFEST.read_text())['component']


def check(components):
    problems, seen = [], set()
    for component in components:
        name = component.get('id', '?')
        for key in KEYS:
            if key not in component:
                problems.append(f'{name}: "{key}" is missing')
        if name in seen:
            problems.append(f'{name}: listed twice')
        seen.add(name)
        path = ROOT / component.get('pinned_in', '')
        if not path.is_file():
            problems.append(f'{name}: pinned_in {component.get("pinned_in")!r} is not a file of the repository')
            continue
        text = path.read_text(errors='replace')
        for pin in component.get('pins', []):
            if pin not in text:
                problems.append(f'{name}: {component["pinned_in"]} does not pin "{pin}" (it changed there, or here)')
    return problems


def main():
    components = load()
    if sys.argv[1:] == ['check']:
        problems = check(components)
        for problem in problems:
            print(f'components.toml: {problem}', file=sys.stderr)
        if not problems:
            print(f'components: {len(components)} pinned, each as its script fetches it')
        return 1 if problems else 0
    for component in components:
        print(f'{component["id"]:22} {component["pins"][0][:14]:14}  {component["used_for"]:42} {component["pinned_in"]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
