#!/usr/bin/env python3
"""The clocks of a serial engine, for touch { } of a board file, out of Linux's clock driver for the SoC.

usage: tools/qupclocks.py gcc-SOC.c WRAPPER ENGINE

A touch controller on SPI hangs on one of the SoC's serial engines. The engines come in groups
("wrappers"), and Linux's drivers/clk/qcom/gcc-SOC.c names the clocks by them: the controller's bus node
in the device tree says which (its clocks property: <&gcc GCC_QUPV3_WRAP1_S5_CLK> is wrapper 1,
engine 5). For that engine this prints the three lines of touch { } that are in no device tree:

    clocks = <0x18004 0x18008 0x185fc>;
    vote-register = <0x52008>;
    votes = <18 19 20 21 27>;

It reads the file; nothing is fetched. The vendor's kernel for the phone has the same file with the
same structures (gcc-CODENAME.c)."""
import re
import sys


class NotFound(Exception):
    pass


def branch(text, name):
    """(halt_reg, enable_reg, bit) of the clk_branch of that name."""
    found = re.search(r'struct\s+clk_branch\s+' + re.escape(name) + r'\s*=\s*\{(.*?)\n\};', text, re.S)
    if found is None:
        raise NotFound(f'no clock {name} in the file')
    body = found[1]
    halt = re.search(r'\.halt_reg\s*=\s*(0x[0-9a-fA-F]+|\d+)', body)
    enable = re.search(r'\.enable_reg\s*=\s*(0x[0-9a-fA-F]+|\d+)', body)
    mask = re.search(r'\.enable_mask\s*=\s*BIT\(\s*(\d+)\s*\)', body)
    if halt is None or enable is None or mask is None:
        raise NotFound(f'{name}: no halt_reg, enable_reg and enable_mask = BIT(n) (not a clock the application processor votes for)')
    return int(halt[1], 0), int(enable[1], 0), int(mask[1])


def touch_lines(text, wrapper, engine):
    """The three lines of touch { } for that engine, as text."""
    names = (f'gcc_qupv3_wrap{wrapper}_core_2x_clk', f'gcc_qupv3_wrap{wrapper}_core_clk',
             f'gcc_qupv3_wrap_{wrapper}_m_ahb_clk', f'gcc_qupv3_wrap_{wrapper}_s_ahb_clk',
             f'gcc_qupv3_wrap{wrapper}_s{engine}_clk')
    found = [branch(text, name) for name in names]
    registers = sorted({enable for halt, enable, bit in found})
    if len(registers) != 1:
        raise NotFound('the five clocks are voted for in more than one register (' +
                       ', '.join(f'{r:#x}' for r in registers) + '): the drivers take one')
    clocks = ' '.join(f'{found[i][0]:#x}' for i in (2, 3, 4))
    votes = ' '.join(str(bit) for halt, enable, bit in found)
    return (f'clocks = <{clocks}>;        // {names[2]}, {names[3]}, {names[4]}: halt_reg\n'
            f'vote-register = <{registers[0]:#x}>;      // their enable_reg\n'
            f'votes = <{votes}>;          // enable_mask of {names[0]}, {names[1]} and the three above\n')


def main(argv):
    if len(argv) != 4 or not argv[2].isdigit() or not argv[3].isdigit():
        sys.exit(__doc__)
    try:
        text = open(argv[1], encoding='utf-8', errors='replace').read()
    except OSError as error:
        sys.exit(f'qupclocks.py: {error}')
    try:
        sys.stdout.write(touch_lines(text, int(argv[2]), int(argv[3])))
    except NotFound as error:
        sys.exit(f'qupclocks.py: {argv[1]}: {error}')


if __name__ == '__main__':
    main(sys.argv)
