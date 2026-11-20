"""An Android device tree source, read for what a board file and a SoC file want.

from_dts(text) is what tools/boards.py new --dts uses: the addresses of the blocks the kernel's drivers
need, the panel's brightness control, the cores, the keys. Everything it returns is a starting point to
be checked against the device, and what it could not find it says in its notes.

The trees it is written for are what a porter has: the live tree of a running Android
(dtc -I dtb -O dts /sys/firmware/fdt), a dumped dtbo, or a vendor's .dts and .dtsi sources. The parser
keeps what it needs of the syntax and skips the rest (/directives/, deletions); workbench/boards_test.py
has trees of each kind.
"""
import re

TSENS_MAX = 8   # a SoC's TSENS controllers, at most (HAKIOS_TSENS_MAX in drivers/board/board.h; tools/boards.py)

KEY_VOLUMEUP = 0x73     # Linux's input-event-codes.h: what gpio-keys calls the key in linux,code


class Node:
    def __init__(self, name, parent=None, labels=()):
        self.name, self.props, self.children, self.parent, self.labels = name, {}, [], parent, list(labels)

    def walk(self):
        yield self
        for child in self.children:
            yield from child.walk()


def parse_dts(text):
    """A device tree source (dtc -I dtb -O dts, or a vendor's .dts with its includes already merged) as a
    tree of nodes. Properties keep their text; cells(), strings() below read it. Not a full parser: what it
    does not understand (/directives/, deletions) it skips. What a source adds to a node by its label
    (&label { ... }) goes into that node where the label was seen before, as the PMICs of a bus do."""
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'(?m)//.*$', '', text)
    position, root = 0, Node('/')
    stack = [root]
    token = re.compile(r'\s*(?:(?P<close>\}\s*;)|(?P<node>(?P<labels>(?:[\w,.+-]+:\s*)*)(?P<name>&?[\w,.+@/-]+)\s*\{)|'
                       r'(?P<prop>[\w,.+#?-]+)\s*(?:=\s*(?P<value>(?:"(?:[^"\\]|\\.)*"|<[^>]*>|\[[^\]]*\]|[^;"<\[])*))?;)')
    while position < len(text):
        match = token.match(text, position)
        if match is None:
            # something of the syntax this does not know: on to the next line
            end = text.find('\n', position)
            position = len(text) if end < 0 else end + 1
            continue
        position = match.end()
        if match['close']:
            if len(stack) > 1:
                stack.pop()
        elif match['node']:
            node = None
            if match['name'].startswith('&'):
                node = next((n for n in root.walk() if match['name'][1:] in n.labels), None)
            if node is None:        # (a label of a file that is not in the text stays a node of its own, where it is)
                node = Node(match['name'], stack[-1], re.findall(r'([\w,.+-]+):', match['labels']))
                stack[-1].children.append(node)
            stack.append(node)
        else:
            stack[-1].props[match['prop']] = (match['value'] or '').strip()
    return root


def cells(value):
    """The numbers of a property written as <...> (several groups are joined)."""
    return [int(c, 0) for group in re.findall(r'<([^>]*)>', value or '') for c in group.split()
            if re.fullmatch(r'(0[xX][0-9a-fA-F]+|\d+)', c)]


def strings(value):
    """The strings of a property: "a", "b" or "a\\0b"."""
    return [part for s in re.findall(r'"((?:[^"\\]|\\.)*)"', value or '') for part in s.split('\\0')]


def reg_named(node, name):
    """The address of the reg entry with that name in reg-names (64-bit cells: address then size, each of
    one or two words; taken as pairs of equal width)."""
    names = strings(node.props.get('reg-names'))
    reg = cells(node.props.get('reg'))
    if name not in names or not reg:
        return None
    index = names.index(name)
    width = len(reg) // len(names) // 2 if len(names) and len(reg) % (2 * len(names)) == 0 else 1
    words = reg[index * 2 * width: index * 2 * width + width]
    address = 0
    for word in words:
        address = address << 32 | word
    return address or None


def cpufreq_block(node, compatible):
    """[cpufreq] of a cpufreq-hw node, but its groups' cores: the layout (EPSS, or the OSM's before it), the first
    group's block and the blocks' size, and each group's address where they are not one size apart (an SM8150's)"""
    reg = cells(node.props.get('reg'))
    # an address and a size of as many cells as the parent says (2 and 2 unless said, as a SoC node of an arm64 tree)
    parent = node.parent
    address = (cells(parent.props.get('#address-cells')) or [2])[0] if parent is not None else 2
    sizes = (cells(parent.props.get('#size-cells')) or [2])[0] if parent is not None else 2
    names = strings(node.props.get('reg-names'))
    if names and reg and len(reg) % len(names) == 0 and len(reg) // len(names) != address + sizes:
        # the regs named say how many cells an entry has, where the parent is not in the text read (a .dtsi alone)
        address = sizes = {2: 1, 4: 2}.get(len(reg) // len(names), 0)
    if address not in (1, 2) or sizes not in (1, 2) or not reg or len(reg) % (address + sizes):
        return None
    count = len(reg) // (address + sizes)

    def number(words):
        value = 0
        for word in words:
            value = value << 32 | word
        return value
    step = address + sizes
    blocks = [(number(reg[i * step:i * step + address]), number(reg[i * step + address:(i + 1) * step])) for i in range(count)]
    bases, size = [base for base, _ in blocks], min(size for _, size in blocks)
    entry = {'epss': bases[0], 'epss_domain_size': size}
    if not any('epss' in c for c in compatible):
        entry = {'block': 'osm', **entry}
    if any(base != bases[0] + i * size for i, base in enumerate(bases)):
        entry['domains'] = bases
    return entry


def first_reg(node):
    """The address of a node's first reg entry, by its parent's #address-cells (2 unless said, as a SoC node of an arm64
    tree has it): for Linux's trees, whose nodes name their regs less often than the vendor's do."""
    reg = cells(node.props.get('reg'))
    parent = node.parent
    width = (cells(parent.props.get('#address-cells')) or [2])[0] if parent is not None else 2
    if width not in (1, 2) or len(reg) < width:
        return None
    return reg[0] << 32 | reg[1] if width == 2 else reg[0]


def unit_address(node):
    return int(node.name.split('@', 1)[1], 16) if '@' in node.name and re.fullmatch(r'[0-9a-fA-F]+', node.name.split('@', 1)[1]) else None


def only_reg(node):
    """(address, size) of a node whose reg is one entry, of one cell each or of two."""
    reg = cells(node.props.get('reg'))
    if len(reg) == 2:
        return reg[0], reg[1]
    if len(reg) == 4:
        return reg[0] << 32 | reg[1], reg[2] << 32 | reg[3]
    return None, None


def referred(tree, value):
    """The node a property's first reference points at: <&label ...> in a source, <phandle ...> in a
    tree that was compiled and dumped again."""
    label = re.search(r'<\s*&([\w,.+-]+)', value or '')
    if label:
        return next((node for node in tree.walk() if label[1] in node.labels), None)
    numbers = cells(value)
    if not numbers:
        return None
    return next((node for node in tree.walk() if cells(node.props.get('phandle')) == numbers[:1]), None)


def referred_all(tree, value):
    """Every node a list of references points at, in order (None for one that is not in the tree): <&a &b> in a
    source, <phandle phandle> once compiled."""
    labels = re.findall(r'&([\w,.+-]+)', value or '')
    if labels:
        return [next((node for node in tree.walk() if label in node.labels), None) for label in labels]
    return [next((node for node in tree.walk() if cells(node.props.get('phandle')) == [number]), None)
            for number in cells(value)]


def pin_of(node):
    """The SoC's pin a pin configuration is for: pins = "gpioN" in it or in one of its children (mux, config)."""
    for candidate in [node] + list(node.children):
        names = strings(candidate.props.get('pins'))
        if len(names) == 1 and re.fullmatch(r'gpio\d+', names[0]):
            return int(names[0][4:])
    return None


def pmic_of(node):
    """(SPMI slave, name) of the PMIC a peripheral's node is in: qcom,pm8350@1 { ... } is slave 1, "PM8350"."""
    pmic = node.parent
    if pmic is None or 'qcom,spmi-pmic' not in strings(pmic.props.get('compatible')):
        return None, None
    reg = cells(pmic.props.get('reg'))
    slave = reg[0] if reg else unit_address(pmic)
    name = pmic.name.split('@')[0].split(',')[-1]
    if name == 'pmic':
        # Linux's trees call every one pmic@N: its name is in its first compatible ("qcom,pm8998")
        name = next((c.split(',')[-1] for c in strings(pmic.props.get('compatible')) if c != 'qcom,spmi-pmic'), name)
    return slave, name.upper()


def arbiter_buses(node, notes):
    """The SPMI buses of a PMIC arbiter's node, each as {'node': where its PMICs are, 'bus': its number, 'off': switched
    off in the tree, 'spmi': the SoC file's spmi { } for it}. A tree has an arbiter in one of two forms. The older: the
    node has "core", "chnls", "obsrvr" and "cnfg" among its reg-names and is the bus itself; a version 7 arbiter's
    second bus is then a node with qcom,bus-id = <1>. The newer (an arbiter of two buses): the node has the blocks the
    buses share, "core", "chnls" and "obsrvr", and a child for each bus with that bus's "cnfg" (and "intr"), the PMICs
    under it; the buses are numbered in the order of their nodes, which is the order of their cnfg blocks. What is
    neither for certain is said in the notes and left out."""
    if not (reg_named(node, 'core') and reg_named(node, 'obsrvr')):
        return []
    shared = {'core': reg_named(node, 'core'), 'observer': reg_named(node, 'obsrvr'), 'ee': (cells(node.props.get('qcom,ee')) or [0])[0]}
    if reg_named(node, 'chnls'):
        shared['channels'] = reg_named(node, 'chnls')

    def bus(where, number, cnfg):
        spmi = {'core': shared['core'], 'cnfg': cnfg, **{k: v for k, v in shared.items() if k != 'core'}}
        if number:
            spmi['bus'] = number
        return {'node': where, 'bus': number, 'off': strings(where.props.get('status')) == ['disabled'], 'spmi': spmi}
    if reg_named(node, 'cnfg'):
        # (a version 7 arbiter's node is one of its two buses, and "cnfg" that bus's: the first unless it says)
        return [bus(node, (cells(node.props.get('qcom,bus-id')) or [0])[0], reg_named(node, 'cnfg'))]
    children = [child for child in node.children if 'cnfg' in strings(child.props.get('reg-names'))]
    if not children:
        return []
    said = [cells(child.props.get('qcom,bus-id')) for child in children]
    numbers = [n[0] for n in said] if all(len(n) == 1 for n in said) else None if any(said) else list(range(len(children)))
    blocks = [reg_named(child, 'cnfg') for child in children]
    in_order = [block for _, block in sorted(zip(numbers or [], blocks), key=lambda pair: pair[0])]
    if (numbers is None or None in blocks or len(set(numbers)) != len(numbers) or
            any(n not in (0, 1) for n in numbers) or in_order != sorted(set(in_order))):
        notes.append(f'the PMIC arbiter {node.name} has a node for each of its buses ('
                     + ', '.join(child.name for child in children) + '), but which bus is which is not plain (two at '
                     'most, numbered in the order of their nodes and of their "cnfg" blocks): spmi { } is left out; '
                     'fill it in by hand (Documentation/chipsets.md)')
        return []
    return [bus(child, number, block) for child, number, block in zip(children, numbers, blocks)]


def bus_of(node, buses):
    """The bus (of arbiter_buses) a PMIC's node is on, by the nodes above it; None where the tree does not say (a
    PMIC added by a label the text has not)."""
    while node is not None:
        for bus in buses:
            if bus['node'] is node:
                return bus
        node = node.parent
    return None


def choose_bus(buses, uses, soc, board, keys, clock, notes):
    """spmi { } of the SoC file is one bus's. Of an arbiter's two, the one with the PMIC of the power key; where the
    tree does not say which that is, the one that has everything else the board uses of a PMIC; else the first. The
    notes say which was taken and what the other is, and what was found on the other is left out of the board: an
    address there is another chip's on the bus taken."""
    there = [bus for bus in buses if not bus['off']]
    if not there:
        if buses:
            notes.append('the PMIC arbiter\'s buses are all switched off in the tree (status = "disabled"): spmi { } is left out')
        return
    names = {'pon': "the power key's block", 'volume_up': "volume up's GPIO", 'rtc': "the PMIC's clock",
             'torch': 'the flashlight', 'bt_enable': "Bluetooth's enable GPIO"}
    where = {key: bus_of(node, buses) for key, node in uses.items()}
    used = [bus for bus in there if any(on is bus for on in where.values())]
    if any(where.get('pon') is bus for bus in there):
        chosen, why = where['pon'], "the one with the power key's PMIC"
    elif len(used) == 1:
        chosen = used[0]
        why = 'the one with what the board uses of a PMIC (' + ', '.join(names[k] for k, on in where.items() if on is chosen) + ')'
    else:
        chosen = min(there, key=lambda bus: bus['bus'])
        why = ('the first: the tree has what the board uses of a PMIC on both' if used else
               'the first: the tree does not say which bus the power key\'s PMIC is on')
    soc['spmi'] = chosen['spmi']
    # (the same arbiter's: of the newer form its node's other child, of the older another node with the same core)
    others = [bus for bus in buses if bus is not chosen and bus['spmi']['core'] == chosen['spmi']['core']]
    for other in others[:1]:
        state = ' and is switched off in the tree (status = "disabled")' if other['off'] else ''
        notes.append(f'the PMIC arbiter has two SPMI buses (version 7): spmi {{ }} is bus {chosen["bus"]}\'s '
                     f'(cnfg = <{chosen["spmi"]["cnfg"]:#010x}>), {why}. The other, bus {other["bus"]} '
                     f'(cnfg = <{other["spmi"]["cnfg"]:#010x}>), exists{state}: the drivers reach the PMICs of one bus, '
                     f'so for a part on bus {other["bus"]} spmi {{ }} wants that cnfg and bus = <{other["bus"]}> '
                     '(Documentation/chipsets.md)')
    for key, on in where.items():
        if on is None or on is chosen:
            continue
        notes.append(f'{names[key]} is on bus {on["bus"]} of the PMIC arbiter, not on the bus spmi {{ }} is of '
                     f'(bus {chosen["bus"]}): left out of the board file, where its address would be another chip\'s')
        if key in ('pon', 'volume_up'):
            keys.pop(key, None)
            keys.pop(key + '_name', None)
        elif key == 'rtc':
            clock.clear()
        elif key == 'torch':
            board.pop('torch', None)
        elif key == 'bt_enable' and 'bluetooth' in board:
            board['bluetooth'].pop('enable_ppid', None)
            if not board['bluetooth']:
                board.pop('bluetooth')


def first_stream(node):
    """The first entry of a node's iommus: [stream, mask]. In a source the SMMU is a reference (&apps_smmu ...),
    in a compiled tree a number that comes first."""
    value = node.props.get('iommus', '')
    found = cells(value)
    found = found if '&' in value else found[1:]
    return found[:2] if len(found) >= 2 else None


def from_dts(text, panel=None):
    """What a device tree says of a board, and what it leaves open. Returns (soc_values, board_values, notes)."""
    tree = parse_dts(text)
    soc, board, notes = {}, {}, []
    tsens, panels, cpus = [], [], []
    platform, keys, rpmh, adreno, ipc, radio_pins, streams, lpass, clock = {}, {}, {}, {}, {}, {}, {}, {}, {}
    buses, uses = [], {}        # the PMIC arbiter's buses, and the nodes of what the board uses of a PMIC (choose_bus)
    for node in tree.walk():
        compatible = strings(node.props.get('compatible'))
        if 'arm,gic-v3' in compatible:
            reg = cells(node.props.get('reg'))
            if len(reg) == 4:                   # one cell an address
                platform.update(gic_dist=reg[0], gic_redist=reg[2])
            elif len(reg) >= 8:                 # two
                platform.update(gic_dist=reg[0] << 32 | reg[1], gic_redist=reg[4] << 32 | reg[5])
        if re.fullmatch(r'cpu@[0-9a-fA-F]+', node.name) and 'enable-method' in node.props:
            reg = cells(node.props.get('reg'))
            capacity = cells(node.props.get('capacity-dmips-mhz'))
            # <&cpufreq_hw GROUP ...>: in a compiled tree the label is a number too, and comes first
            domain = cells(node.props.get('qcom,freq-domain'))
            group = (domain[1] if '&' not in node.props.get('qcom,freq-domain', '') and len(domain) >= 2 else
                     domain[0] if domain else None)
            cpus.append((reg[-1] if reg else int(node.name.split('@')[1], 16), capacity[0] if capacity else 0, group))
        if re.fullmatch(r'ufshc@[0-9a-fA-F]+', node.name) and reg_named(node, 'ufs_mem'):
            platform['ufs'] = reg_named(node, 'ufs_mem')
        elif 'qcom,ufshc' in compatible and 'ufs' not in platform:
            # Linux's: the host controller's registers are its first reg ("std" where they are named)
            platform['ufs'] = reg_named(node, 'std') or first_reg(node)
        if node.name == 'psci' and strings(node.props.get('method')):
            platform['psci'] = strings(node.props['method'])[0]
        if reg_named(node, 'dsi_ctrl') and node.props.get('cell-index', '<0>').strip() in ('<0>', '<0x00>', '<0x0>'):
            soc.setdefault('display', {})['dsi0'] = reg_named(node, 'dsi_ctrl')
        if reg_named(node, 'mdp_intf_base') and node.props.get('cell-index', '<0>').strip() in ('<0>', '<0x00>', '<0x0>'):
            soc.setdefault('display', {})['intf1'] = reg_named(node, 'mdp_intf_base')
        if reg_named(node, 'tsens_tm_physical'):
            tsens.append(reg_named(node, 'tsens_tm_physical'))
        elif 'qcom,tsens-v2' in compatible and first_reg(node):
            # Linux's: the readings ("TM") are the first reg, the settings ("SROT") the second
            tsens.append(first_reg(node))
        # the cores' frequency block: Linux's "qcom,cpufreq-epss" and "qcom,cpufreq-hw" (with or without the chip's name
        # before), the vendor's "qcom,cpufreq-hw-epss" and "qcom,cpufreq-hw"; not the vendor's "...-debug" beside it
        if any(re.fullmatch(r'qcom,([a-z0-9]+-)?cpufreq-(epss|hw|hw-epss)', c) for c in compatible):
            entry = cpufreq_block(node, compatible)
            if entry:
                soc['cpufreq'] = entry
        if any(re.fullmatch(r'qcom,.*-dispcc(-.*)?|qcom,dispcc-.*', c) for c in compatible) and unit_address(node):
            soc.setdefault('clocks', {})['dispcc'] = unit_address(node)
        if any(re.fullmatch(r'qcom,.*-gcc(-.*)?|qcom,gcc-.*', c) for c in compatible) and unit_address(node):
            soc.setdefault('clocks', {})['gcc'] = unit_address(node)
        if any(re.fullmatch(r'qcom,.*-pinctrl|qcom,.*-tlmm', c) for c in compatible) and 'gpio-controller' in node.props:
            reg = cells(node.props.get('reg'))
            if len(reg) in (2, 4):              # one cell an address, or two
                base, size = (reg[0], reg[1]) if len(reg) == 2 else (reg[0] << 32 | reg[1], reg[2] << 32 | reg[3])
                # the vendor's trees give the whole 16 MiB block, in which the pins begin 1 MiB in; Linux's
                # give the pins themselves
                soc['pins'] = {'tlmm': base + 0x100000 if size >= 0x1000000 else base}
        # the SMMU of everything but the GPU: the vendor names it, Linux's is the iommu that is not Adreno's
        if unit_address(node) and (re.fullmatch(r'apps-smmu@[0-9a-fA-F]+', node.name) or 'apps_smmu' in node.labels or (
                node.name.startswith('iommu@') and any(re.fullmatch(r'qcom,(.*-)?smmu-500|arm,mmu-500', c) for c in compatible)
                and not any('adreno' in c for c in compatible))):
            soc.setdefault('smmu', {'apps': unit_address(node)})
        # the way to the other processors: the always-on one's mailbox, the block they ring each other
        # through, the memory they share and the lock on it
        if any(c == 'qcom,qmp-mbox' or re.fullmatch(r'qcom,(.*-)?aoss-qmp', c) for c in compatible) and only_reg(node)[0]:
            ipc['aoss'] = only_reg(node)[0]
        if 'qcom,ipcc' in compatible and only_reg(node)[0]:
            ipc['ipcc'] = only_reg(node)[0]
        if 'qcom,smem' in compatible:
            region = referred(tree, node.props.get('memory-region'))
            if region is not None and only_reg(region)[0]:
                ipc['smem'], ipc['smem_size'] = only_reg(region)
            lock, which = referred(tree, node.props.get('hwlocks')), cells(node.props.get('hwlocks'))
            if lock is not None and which:
                # Linux: the lock's node has the registers; the vendor: <&block OFFSET STRIDE> points at them
                base, stride = only_reg(lock)[0], 0x1000
                block, where = referred(tree, lock.props.get('syscon')), cells(lock.props.get('syscon'))
                if base is None and block is not None and len(where) >= 2 and only_reg(block)[0]:
                    base, stride = only_reg(block)[0] + where[-2], where[-1]
                if base:
                    ipc['smem_lock'] = base + stride * which[-1]
        # the flashlight: the flash LED module of a PM8350C, and that PMIC's temperature alarm
        if 'qcom,pm8350c-flash-led' in compatible and unit_address(node) is not None:
            slave, pmic = pmic_of(node)
            alarm = next((n for n in (node.parent.children if node.parent is not None else [])
                          if 'qcom,spmi-temp-alarm' in strings(n.props.get('compatible')) and unit_address(n) is not None), None)
            if slave is not None and alarm is not None:
                board['torch'] = {'flash': slave << 8 | unit_address(node) >> 8, 'alarm': slave << 8 | unit_address(alarm) >> 8}
                uses['torch'] = node
        # the touch controller on an SPI bus: its reset and its interrupt, pins of the SoC (<&tlmm PIN FLAGS>), as the
        # vendors' drivers name them; and the bus it is on, where the tree has the controller under it
        if 'spi-max-frequency' in node.props:
            for vendor in ('focaltech', 'goodix'):
                reset, wakes = cells(node.props.get(f'{vendor},reset-gpio')), cells(node.props.get(f'{vendor},irq-gpio'))
                if len(reset) >= 2 and len(wakes) >= 2:
                    touch = board.setdefault('touch', {})
                    touch['reset'], touch['interrupt'] = reset[-2], wakes[-2]
                    # the range of its coordinates: Goodix's two properties, or FocalTech's corners
                    size = cells(node.props.get('goodix,panel-max-x')) + cells(node.props.get('goodix,panel-max-y'))
                    corners = cells(node.props.get('focaltech,display-coords'))
                    size = size if len(size) == 2 else corners[2:4] if len(corners) == 4 else []
                    if len(size) == 2 and all(size):
                        touch['coordinates'] = size
                    bus = node.parent
                    if bus is not None and 'qcom,spi-geni' in strings(bus.props.get('compatible')) and only_reg(bus)[0]:
                        touch['serial_engine'] = only_reg(bus)[0]
                    break
        # A Bluetooth child of a GENI UART is unambiguous; a separate vendor power node is not a
        # UART selector. Never guess among the several serial engines a vendor tree may enable.
        if any('bluetooth' in c or re.fullmatch(r'qcom,wcn[0-9]+-bt', c) for c in compatible):
            bt = board.setdefault('bluetooth', {})
            bus = node.parent
            if bus is not None and any(c in strings(bus.props.get('compatible')) for c in
                                       ('qcom,msm-geni-serial-hs', 'qcom,geni-uart')):
                if only_reg(bus)[0]:
                    bt['serial_engine'] = only_reg(bus)[0]
                if bus.parent is not None and unit_address(bus.parent):
                    bt['wrapper'] = unit_address(bus.parent)
            for prop, key in (('qcom,bt-reset-gpio', 'enable_ppid'), ('qcom,bt-sw-ctrl-gpio', 'sw_ctrl')):
                controller = referred(tree, node.props.get(prop))
                pins = cells(node.props.get(prop))
                if controller is None or len(pins) < 2:
                    continue
                pin = pins[-2]
                if key == 'enable_ppid':
                    sid, _ = pmic_of(controller)
                    base = unit_address(controller)
                    if sid is not None and base is not None and 1 <= pin <= 16:
                        bt[key] = sid << 8 | ((base + (pin - 1) * 0x100) >> 8)
                        uses['bt_enable'] = controller
                elif 'gpio-controller' in controller.props and any('tlmm' in c or 'pinctrl' in c for c in strings(controller.props.get('compatible'))):
                    bt[key] = pin
            if not bt:
                board.pop('bluetooth')
        # the SMMU streams the parts' DMA comes in on: the audio DSP's reads of our memory, the Wi-Fi DSP's, the IFEs'
        if 'qcom,msm-audio-ion' in compatible and first_stream(node):
            streams.setdefault('audio_stream', first_stream(node))
        if strings(node.props.get('qcom,firmware-name')) == ['wpss'] and first_stream(node):
            streams.setdefault('wifi_stream', first_stream(node))
        if (node.name == 'msm_cam_smmu_ife' or 'msm_cam_smmu_ife' in node.labels) and first_stream(node):
            streams.setdefault('camera_stream', first_stream(node))
        # the audio DSP's reserved memory: what the node that loads "adsp" points at
        if strings(node.props.get('qcom,firmware-name')) == ['adsp']:
            region = referred(tree, node.props.get('memory-region'))
            if region is not None and only_reg(region)[0]:
                board['adsp'] = dict(zip(('memory', 'memory_size'), only_reg(region)))
        # ... and the Wi-Fi DSP's, where the SoC has one
        if strings(node.props.get('qcom,firmware-name')) == ['wpss']:
            region = referred(tree, node.props.get('memory-region'))
            if region is not None and only_reg(region)[0]:
                board['wifi'] = dict(zip(('dsp_memory', 'dsp_memory_size'), only_reg(region)))
        # the radio's pins, as the vendors' Wi-Fi and Bluetooth nodes name them (<&tlmm PIN FLAGS>): its enable,
        # Bluetooth's reset and Bluetooth's sw-ctrl
        for key, names in (('enable', ('wlan-en-gpio', 'qcom,wl-reset-gpio')), ('bt_reset', ('qcom,bt-reset-gpio',)),
                           ('bt_sw_ctrl', ('qcom,bt-sw-ctrl-gpio',))):
            for name in names:
                pin = cells(node.props.get(name))
                if len(pin) >= 2:
                    radio_pins.setdefault(key, pin[-2])
        # the sound's own pins: the MI2S port a vendor's sound card drives, whose default pin states are its bit
        # clock, its word select and its two data lines, in that order; which data line goes out to the
        # amplifiers and which comes in is in the port's rx and tx lines (bit n: data line n)
        if 'qcom,msm-mi2s-rx-lines' in node.props and 'qcom,msm-mi2s-tx-lines' in node.props:
            states = referred_all(tree, node.props.get('pinctrl-0'))
            pins = [pin_of(state) if state is not None else None for state in states]
            out, into = cells(node.props['qcom,msm-mi2s-rx-lines']), cells(node.props['qcom,msm-mi2s-tx-lines'])
            if len(pins) == 4 and None not in pins and out in ([1], [2]) and into in ([1], [2]) and out != into:
                board.setdefault('speaker', {})['sound_pins'] = [pins[0], pins[1], pins[1 + into[0]], pins[1 + out[0]]]
        # the low-power audio subsystem's blocks on the microphones' path: the capture side's codec macro (TX) and
        # the VA macro, the SoundWire master under the TX macro with its hardware control register, and the
        # subsystem's own pin controller with its slew register
        if 'qcom,tx-macro' in compatible and unit_address(node) is not None:
            lpass['tx_macro'] = unit_address(node)
            for child in node.children:
                if 'qcom,swr-mstr' in strings(child.props.get('compatible')):
                    base, hctl = cells(child.props.get('swrm-io-base')), cells(child.props.get('qcom,swrm-hctl-reg'))
                    if base:
                        lpass['tx_swr'] = base[0]
                    if hctl:
                        lpass['tx_swr_hctl'] = hctl[0]
        if 'qcom,va-macro' in compatible and unit_address(node) is not None:
            lpass['va_macro'] = unit_address(node)
        if 'qcom,lpi-pinctrl' in compatible and cells(node.props.get('reg')):
            lpass['lpi'] = cells(node.props['reg'])[0]
            if cells(node.props.get('qcom,slew-reg')):
                lpass['lpi_slew'] = cells(node.props['qcom,slew-reg'])[0]
        # the microphones' codec: its reset is a pin driven through a node of its own (qcom,msm-cdc-pinctrl),
        # whose active state names the pin
        if 'qcom,wcd-rst-gpio-node' in node.props and 'mic' not in board:
            control = referred(tree, node.props['qcom,wcd-rst-gpio-node'])
            state = referred(tree, control.props.get('pinctrl-0')) if control is not None else None
            if state is not None and pin_of(state) is not None:
                board['mic'] = {'codec_reset': pin_of(state)}
        if 'qcom,mdss-dsi-panel-type' in node.props:
            panels.append(node)
        # the PMIC arbiter: "qcom,spmi-pmic-arb", or a chip's own name for one with a node for each of its buses
        if any(re.fullmatch(r'qcom,([a-z0-9]+-)?spmi-pmic-arb', c) for c in compatible):
            buses += arbiter_buses(node, notes)
        if 'qcom,cmd-db' in compatible:
            reg = cells(node.props.get('reg'))
            if len(reg) in (2, 4):              # address and size, of one cell each or two
                half = len(reg) // 2
                rpmh.update(cmd_db=reg[half - 1], cmd_db_size=reg[-1])
        if 'qcom,rpmh-rsc' in compatible and reg_named(node, 'drv-2'):
            # the application processor's: the one whose drv-2 is this system's (qcom,drv-id = <2>)
            if cells(node.props.get('qcom,drv-id')) in ([], [2]):
                rpmh['rsc'] = reg_named(node, 'drv-2')
                offset = cells(node.props.get('qcom,tcs-offset'))
                if offset:
                    rpmh['tcs_offset'] = offset[0]
                # The drivers send through the sequencer's first slot set and take it for an active one: so it is
                # on every chip seen, where qcom,tcs-config lists ACTIVE_TCS first, with slots (Linux's rpmh-rsc.c lays
                # the sets out in that order). Said where a tree has it otherwise.
                config = node.props.get('qcom,tcs-config', '')
                first = re.match(r'\s*<\s*(\w+)\s+(\w+)\s*>', config)
                if first and not (first[1] in ('ACTIVE_TCS', '2', '0x2', '0x02') and first[2] not in ('0', '0x0', '0x00')):
                    notes.append(f'the application processor\'s RPMh sequencer lists {first[1]} {first[2]} first in its '
                                 'qcom,tcs-config, not active slots: what sends through RPMh (ddr-rpmh, the supplies of '
                                 'the part drivers) takes the first set for an active one, and would not work here')
        # the Adreno's blocks, by the names their reg entries have (qcom,kgsl-3d0 and qcom,gmu), and the
        # three that are nodes of their own
        for key, name in (('gpu', 'kgsl_3d0_reg_memory'), ('cx_misc', 'cx_misc'), ('rscc', 'rscc'),
                          ('gmu', 'kgsl_gmu_reg'), ('pdc', 'kgsl_gmu_pdc_cfg')):
            if reg_named(node, name):
                adreno.setdefault(key, reg_named(node, name))
        if any(re.fullmatch(r'qcom,.*-gpucc(-.*)?|qcom,gpucc-.*', c) for c in compatible) and unit_address(node):
            adreno['gpucc'] = unit_address(node)
        if (re.fullmatch(r'kgsl-smmu@[0-9a-fA-F]+', node.name) or 'kgsl_smmu' in node.labels) and unit_address(node):
            adreno['smmu'] = unit_address(node)
        if 'qcom,qfprom' in compatible and unit_address(node):
            adreno.setdefault('qfprom', unit_address(node))
        if ('qcom,qpnp-power-on' in compatible or any(re.fullmatch(r'qcom,pm[a-z0-9]+-pon', c) for c in compatible)) \
                and unit_address(node) and strings(node.props.get('status')) != ['disabled']:
            # the power-on block of the PMIC with the keys: power on KPDPWR_N, volume down on RESIN_N. The vendor's
            # qcom,qpnp-power-on, Linux's qcom,pm8998-pon and the like (a PMK8350's at its HLOS half, its unit address)
            slave, pmic = pmic_of(node)
            if slave is not None:
                keys.update(pon=slave << 8 | unit_address(node) >> 8, pon_name=f"the {pmic}'s power-on block")
                uses['pon'] = node
        if not clock and strings(node.props.get('status')) != ['disabled']:
            # the PMIC's real-time clock, its RTC_RW block: Linux's rtc-pm8xxx.c names it by its compatible
            # (qcom,pm8941-rtc, qcom,pmk8350-rtc) at that block's unit address; the vendor's qpnp-rtc.c has a node of
            # its own over one per block, the counter's ...rtc_rw@6000
            slave, pmic, block = None, None, None
            if any(re.fullmatch(r'qcom,pm[a-z0-9]+-rtc', c) for c in compatible) and unit_address(node):
                (slave, pmic), block = pmic_of(node), unit_address(node)
            elif 'qcom,qpnp-rtc' in compatible:
                rw = next((child for child in node.children if 'rtc_rw' in child.name and unit_address(child)), None)
                if rw is not None:
                    (slave, pmic), block = pmic_of(node), unit_address(rw)
            if slave is not None:
                clock.update(peripheral=slave << 8 | block >> 8, name=f"the {pmic}'s RTC")
                uses['rtc'] = node
        if 'gpio-keys' in compatible:
            for key in node.children:
                code, pins = cells(key.props.get('linux,code')), cells(key.props.get('gpios'))
                if code != [KEY_VOLUMEUP]:
                    continue
                gpios = referred(tree, key.props.get('gpios'))
                # <&gpios N flags> in a source (the label is not a number), <phandle N flags> once compiled
                number = pins[0] if '&' in key.props.get('gpios', '') else pins[1] if len(pins) > 1 else None
                slave, pmic = pmic_of(gpios) if gpios is not None else (None, None)
                if slave is None or number is None or unit_address(gpios) is None:
                    notes.append("volume up's key is not a GPIO of a PMIC (or its controller is not in this tree): "
                                 '[buttons] volume_up is to be filled in by hand')
                    continue
                # GPIO n of pinctrl@8800 has its registers at 0x8800 + 0x100 * (n - 1)
                keys.update(volume_up=slave << 8 | ((unit_address(gpios) >> 8) + number - 1),
                            volume_up_name=f'GPIO{number} of the {pmic}')
                uses['volume_up'] = gpios
    wanted = ('aoss', 'ipcc', 'smem', 'smem_size', 'smem_lock')
    if all(key in ipc for key in wanted):
        soc['ipc'] = {key: ipc[key] for key in wanted}
    elif ipc:
        notes.append('the way to the other processors was not all found (missing: ' +
                     ', '.join(k for k in wanted if k not in ipc) + '): [ipc] is left out; fill it in from '
                     'arch/arm64/boot/dts/_template/soc.dtsi for the features "adsp" and "wifi-wcn6750"')
    if all(key in rpmh for key in ('cmd_db', 'cmd_db_size', 'rsc', 'tcs_offset')):
        soc['rpmh'] = rpmh
    wanted = ('gpu', 'gmu', 'gpucc', 'cx_misc', 'rscc', 'smmu', 'pdc', 'qfprom')
    if all(key in adreno for key in wanted):
        soc['adreno'] = {key: adreno[key] for key in wanted}
    elif any(key in adreno for key in ('gpu', 'gmu', 'smmu')):
        notes.append("the GPU's blocks were not all found (missing: " + ', '.join(k for k in wanted if k not in adreno) +
                     '): [adreno] is left out; fill it in from arch/arm64/boot/dts/_template/soc.dtsi for the feature "gpu-a660"')
    if lpass:
        soc['lpass'] = lpass
    if len(radio_pins) == 3 and len(set(radio_pins.values())) == 3:
        board.setdefault('wifi', {})['pins'] = [radio_pins[key] for key in ('enable', 'bt_reset', 'bt_sw_ctrl')]
    choose_bus(buses, uses, soc, board, keys, clock, notes)
    if keys:
        board['buttons'] = keys
    if clock:
        board['rtc'] = clock
    if 'smmu' in soc:
        soc['smmu'].update(streams)
    if tsens:
        soc['thermal'] = {'tsens': sorted(set(tsens))[:TSENS_MAX]}
    if cpus:
        # Qualcomm numbers its cores 0x000, 0x100 ...: in MPIDR's second affinity field
        shift = 8 if any(number >= 0x100 for number, _, _ in cpus) else 0
        capacities = [capacity for _, capacity, _ in cpus if capacity]
        small = 0
        groups = [group for _, _, group in sorted(cpus)]
        if 'cpufreq' in soc and None not in groups and groups == sorted(groups):
            soc['cpufreq']['domain_cores'] = [groups.count(group) for group in sorted(set(groups))]
        for number, capacity, _ in cpus:
            if capacities and capacity == min(capacities) and min(capacities) < max(capacities):
                small |= 1 << (number >> shift)
        platform.update(cores=len(cpus), core_shift=shift)
        if small:
            platform['small_cores'] = small
    if platform:
        soc['platform'] = platform
    if panels:
        chosen = [p for p in panels if p.name == panel] if panel else panels
        if panel and not chosen:
            raise SystemExit(f'no panel node "{panel}"; the tree has: {", ".join(p.name for p in panels)}')
        if len(chosen) > 1:
            notes.append('several panels in the tree; took the first. The phone uses one of them (Android: '
                         'cat /proc/cmdline, "msm_drm.dsi_display0="): --panel NODE picks it. They are: ' +
                         ', '.join(p.name for p in chosen))
        node = chosen[0]
        props = node.props
        kind = (strings(props.get('qcom,mdss-dsi-panel-type')) or [''])[0]
        control = (strings(props.get('qcom,mdss-dsi-bl-pmic-control-type')) or [''])[0]
        light = {'panel': node.name, 'panel_name': (strings(props.get('qcom,mdss-dsi-panel-name')) or [''])[0]}
        if control == 'bl_ctrl_dcs':
            low = cells(props.get('qcom,mdss-dsi-bl-min-level'))
            high = cells(props.get('qcom,mdss-dsi-bl-max-level'))
            start = cells(props.get('qcom,mdss-brightness-init-level'))
            light.update(type='dcs', panel_mode='video' if kind == 'dsi_video_mode' else 'command',
                         min=low[0] if low else 1, max=high[0] if high else 255)
            if start:
                light['restore'] = start[0]
            light['bytes'] = 2 if light['max'] > 255 else 1
            if light['bytes'] == 2:
                light['byte_order'] = 'msb-first' if 'qcom,mdss-dsi-bl-inverted-dbv' in props else 'lsb-first'
            if not low or not high:
                notes.append('the panel names no brightness range (qcom,mdss-dsi-bl-min/max-level): guessed')
        else:
            light['type'] = 'soft'
            notes.append(f'the panel\'s light is "{control or "not named"}", not DCS: no driver for that yet, so '
                         'the picture is darkened in software (type "soft")')
        board['backlight'] = light
    else:
        notes.append('no panel node in the tree (it may be in the dtbo: dump that as well)')
    for section, keys in (('display', ['dsi0']), ('thermal', ['tsens']), ('cpufreq', ['epss']),
                          ('platform', ['gic_dist', 'cores', 'psci'])):
        if not all(key in soc.get(section, {}) for key in keys):
            notes.append(f'[{section}] not found in the tree')
    notes.append('gcc_disp_ahb_cbcr, dispcc_mdss_gdscr and dispcc_mdss_ahb_cbcr are not in a device tree: they are '
                 'register offsets from Linux\'s clock drivers for the SoC (gcc-*.c: gcc_disp_ahb_clk; dispcc-*.c: '
                 'the MDSS GDSC and disp_cc_mdss_ahb_clk)')
    return soc, board, notes
