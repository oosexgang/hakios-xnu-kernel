#!/usr/bin/env python3
"""A new driver, started: the guide's example driver (Documentation/examples/example_led.c) and its host test copied
under the new name, the test added to make check, and the driver given its part in MAINTAINERS -- the mechanical steps of
Documentation/writing-a-driver.md, done the same way each time; the rest is said at the end.

  scripts/new-driver.py NAME [--part "THE PART'S NAME"]

NAME is the driver's file (drivers/board/NAME.c), its boot argument (NAME=1 looks, NAME=2 acts) and the prefix of its
log lines ("NAME: ..."): lower-case letters, digits and underscores. What it makes builds and passes its test at once:
`workbench/mutate.sh NAME_driver_test board/NAME.c x x` runs it. What the light on pin 42 was is for you to make the part.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def started(text, name):
    """The example's text, made the new driver's: its functions, boot argument, log prefix, constants and file."""
    upper = name.upper()
    for old, new in ((r'#include "example_led\.c"', f'#include "board/{name}.c"'),
                     (r'\bexample_led_(init|quiesce)\b', rf'hakios_{name}_\1'),
                     (r'\bled_thread\b', f'{name}_thread'),
                     (r'"led: ', f'"{name}: '), (r'"led:"', f'"{name}:"'), (r'"led"', f'"{name}"'),
                     (r'"led=', f'"{name}='), (r'\bled=', f'{name}='),
                     (r'\bLED_PIN\b', f'{upper}_PIN'),
                     (r'example_led_test', f'{name}_driver_test'), (r'example_led\.c', f'drivers/board/{name}.c')):
        text = re.sub(old, new, text)
    return text


def main(arguments=None, root=ROOT):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('name', metavar='NAME')
    parser.add_argument('--part', help='its part\'s name in MAINTAINERS (default: NAME in capitals)')
    options = parser.parse_args(arguments)
    name = options.name
    if not re.fullmatch(r'[a-z][a-z0-9_]{1,23}', name):
        raise SystemExit(f'new-driver.py: "{name}": lower-case letters, digits and underscores, a letter first')
    driver, test = root / 'drivers/board' / f'{name}.c', root / 'workbench' / f'{name}_driver_test.c'
    for path in (driver, test):
        if path.exists():
            raise SystemExit(f'new-driver.py: {path.relative_to(root)} is there already')
    examples = root / 'Documentation/examples'
    head = (f'/*\n * {name}: what it drives, and on which boards (write it here). Started from the guide\'s example\n'
            f' * (Documentation/examples/example_led.c) by scripts/new-driver.py: a light on one of the SoC\'s pins, to be made\n'
            f' * the part. Documentation/writing-a-driver.md has the steps and the rules.\n')
    text = started((examples / 'example_led.c').read_text(), name)
    text = re.sub(r'\A/\*\n( \*[^\n]*\n)+? \*\n', head + ' *\n', text, count=1)
    driver_text = text
    test_text = started((examples / 'example_led_test.c').read_text(), name)

    # work everything out, and fail, before writing any file: a run that stopped after writing the driver and its test
    # but before run.sh was changed used to leave them in place, so every rerun stopped at "is there already" with
    # run.sh and MAINTAINERS never updated
    run = root / 'workbench/run.sh'
    listed = run.read_text()
    run_text = None
    if f' {name}_driver_test ' not in listed:
        run_text, n = re.subn(r'(\nfor test in [^\n]*?) example_led_test; do', rf'\1 {name}_driver_test example_led_test; do', listed, count=1)
        if n != 1:
            raise SystemExit('new-driver.py: workbench/run.sh has no list of tests ending with example_led_test')

    maintainers = root / 'MAINTAINERS'
    part = (options.part or name.replace('_', ' ')).upper()
    text = maintainers.read_text()
    maintainers_text = None
    if f'F:\tdrivers/board/{name}.c\n' not in text:
        entry = f'{part}\nF:\tdrivers/board/{name}.c\nT:\tworkbench/{name}_driver_test.c\nK:\t{name}=\n'
        blocks = text.split('\n\n')
        # (the head, then one block a part, in alphabetical order: the new one where its name falls)
        at = next((i for i, block in enumerate(blocks) if i > 0 and block.split('\n')[0] == block.split('\n')[0].upper()
                   and block.split('\n')[0] > part), len(blocks))
        blocks.insert(at, entry.rstrip('\n'))
        maintainers_text = '\n\n'.join(blocks)

    # all the changes worked out: write them
    driver.write_text(driver_text)
    test.write_text(test_text)
    if run_text is not None:
        run.write_text(run_text)
    if maintainers_text is not None:
        maintainers.write_text(maintainers_text)
    for path in (driver, test):
        print(f'wrote {path.relative_to(root)}')
    print(f'workbench/run.sh runs {name}_driver_test; MAINTAINERS has the part {part}')
    print(f"""next (Documentation/writing-a-driver.md, "Steps"):
  workbench/mutate.sh {name}_driver_test board/{name}.c x x   it passes as it is: make it the part's, scenario by scenario
  a feature for it: a HAKIOS_HAS_ bit in drivers/board/board.h, its name in tools/boards.py
  the build: bsd/dev/board/{name}.c in bsd/conf/files of an assembled tree, hakios_{name}_init() in drivers.c's list,
             then kernel/export.sh; kernel/syntax-check.sh --tree ../xnu-12377-hakios bsd/dev/board/{name}.c
  its row in Documentation/drivers.md; its boot argument's words in the file's head ({name}=1, {name}=2): make docs""")
    return 0


if __name__ == '__main__':
    sys.exit(main())
