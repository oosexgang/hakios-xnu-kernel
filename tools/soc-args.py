#!/usr/bin/env python3
"""Print the modem/IPA SoC addresses of a Qualcomm phone as HakiOS boot-args, from its device tree.

usage: tools/soc-args.py DEVICE-TREE.dtb

The kernel's modem and IPA drivers (drivers/modem/hakios.mpss.c, hakios.ipa.c) carry the first phone's
addresses as defaults and take any that differ from boot-args soc_* (see soc_override there). This
reads a phone's own flattened device tree -- /sys/firmware/fdt on a rooted phone, or the dtb a port
already has -- and prints the soc_* it needs, which devices/<name>/soc.args holds and the run
scripts pass along. Only numbers are read out; no part of the tree is copied.
Register tuples use their immediate parent's #address-cells and #size-cells (DT defaults: 2 and 1).
One or two address cells and zero to two size cells are supported.
Addresses wider than a kernel argument can hold are refused rather than truncated. Bus-address
translation through ranges is not performed; the selected register addresses must already be CPU addresses.
"""
import struct
import sys


def nodes_of(blob):
    if blob[:4] != b'\xd0\x0d\xfe\xed':
        sys.exit('soc-args.py: not a flattened device tree')
    off_struct, off_strings = struct.unpack('>II', blob[8:16])
    size_strings, = struct.unpack('>I', blob[32:36])
    strings = blob[off_strings:off_strings + size_strings]
    out, path, at = {}, [], off_struct
    while True:
        token, = struct.unpack('>I', blob[at:at + 4])
        at += 4
        if token == 1:                                  # FDT_BEGIN_NODE
            end = blob.index(b'\0', at)
            path.append(blob[at:end].decode())
            at = (end + 4) & ~3
            out['/'.join(path)] = {}
        elif token == 2:                                # FDT_END_NODE
            path.pop()
        elif token == 3:                                # FDT_PROP
            length, name_off = struct.unpack('>II', blob[at:at + 8])
            at += 8
            name = strings[name_off:strings.index(b'\0', name_off)].decode()
            out['/'.join(path)][name] = blob[at:at + length]
            at = (at + length + 3) & ~3
        elif token == 9:                                # FDT_END
            return out
        elif token != 4:                                # 4 = FDT_NOP
            sys.exit(f'soc-args.py: unexpected token {token}')


def find(nodes, compatible):
    for path, props in nodes.items():
        if compatible.encode() in props.get('compatible', b'').split(b'\0'):
            return path, props
    return None, None


def find_named(nodes, leaf):
    """The node whose last path component begins with `leaf` (e.g. apps-smmu@..., several SMMUs share a compatible)."""
    for path, props in nodes.items():
        if path.rsplit('/', 1)[-1].split('@', 1)[0] == leaf:
            return path, props
    return None, None


def u32(value, index=0):
    return struct.unpack_from('>I', value, index * 4)[0]


def reg_address(nodes, path, index=0):
    """An address from reg tuple `index`, or None if that tuple is absent."""
    if path is None or 'reg' not in nodes[path]:
        return None
    # These properties describe the parent's children; they are not inherited from ancestors.
    parent = nodes[path.rsplit('/', 1)[0]]

    def width(name, default, allowed):
        value = parent.get(name, struct.pack('>I', default))
        if len(value) != 4 or u32(value) not in allowed:
            sys.exit(f'soc-args.py: {path}: unsupported or malformed {name}')
        return u32(value)

    address_cells = width('#address-cells', 2, (1, 2))
    size_cells = width('#size-cells', 1, (0, 1, 2))
    stride = 4 * (address_cells + size_cells)
    data = nodes[path]['reg']
    if len(data) % stride:
        sys.exit(f'soc-args.py: {path}: reg does not contain complete address/size tuples')
    at = index * stride
    if at >= len(data):
        return None
    return int.from_bytes(data[at:at + 4 * address_cells], 'big')


def aoss_mailbox(nodes):
    """The address of the AOSS QMP mailbox, which one phone's device tree describes in two ways.

    A tree from a mainline-style kernel has a node whose compatible is qcom,<soc>-aoss-qmp (the
    reference tree here: qcom,sm8350-aoss-qmp on /soc/power-controller@c300000). A tree from the
    phone's own Android (vendor) kernel, which is what /sys/firmware/fdt gives on the phone, calls the
    same hardware /soc/qcom,qmp-aop@c300000 with the compatible qcom,qmp-mbox; the reference tree
    even has such a node as well, but without a reg. Both carry the same address. Only a node that
    has a reg counts, and the mainline form is preferred when both are there."""
    for path, props in nodes.items():
        kinds = props.get('compatible', b'').split(b'\0')
        if any(k.startswith(b'qcom,') and k.endswith(b'-aoss-qmp') for k in kinds):
            address = reg_address(nodes, path)
            if address is not None:
                return address
    for path, props in nodes.items():
        if path.rsplit('/', 1)[-1].split('@', 1)[0] == 'qcom,qmp-aop':
            address = reg_address(nodes, path)
            if address is not None:
                return address
    return None


def main():
    sys.stdout.reconfigure(newline="\n")        # LF on Windows too: the output becomes a file of boot-args
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    nodes = nodes_of(open(sys.argv[1], 'rb').read())
    args = []

    def address(arg, value, bits=32):
        if value is not None:
            if not 0 <= value < 1 << bits:
                sys.exit(f'soc-args.py: {arg}={value:#x} does not fit the kernel\'s {bits}-bit argument')
            args.append((arg, value))

    path, _ = find(nodes, 'qcom,ipa')
    ipa_region, gsi_base = reg_address(nodes, path), reg_address(nodes, path, 1)
    if ipa_region is not None and gsi_base is not None:
        address('soc_ipa_base', ipa_region + 0x40000, bits=64)  # the IPA registers are 0x40000 into the region
        address('soc_gsi_base', gsi_base, bits=64)

    def block(compatible, arg, index=0):
        path, _ = find(nodes, compatible)
        address(arg, reg_address(nodes, path, index))

    block('qcom,ipcc', 'soc_ipcc')
    aoss = aoss_mailbox(nodes)                          # the AOSS mailbox: see aoss_mailbox() for the two ways it is described
    if aoss is not None:
        address('soc_aoss', aoss)
    block('qcom,rpmh-rsc', 'soc_rsc', index=2)         # drv-2 is the third tuple (reg-names drv-0/1/2)
    path, _ = find_named(nodes, 'apps-smmu')           # not by compatible: the GPU's SMMU shares qcom,qsmmu-v500
    address('soc_apps_smmu', reg_address(nodes, path))

    # the memory addresses: the command DB and the modem's region are reserved-memory nodes named by
    # their address; shared memory is the reserved-memory the /soc/qcom,smem node points at; the SoC's
    # internal memory is the msm-imem node rounded down to its 2 MB block (what the IPA's table maps).
    by_phandle = {}
    for path, props in nodes.items():
        if 'phandle' in props:
            by_phandle[u32(props['phandle'])] = (path, props)

    def reserved(leaf, arg):
        path, _ = find_named(nodes, leaf)
        address(arg, reg_address(nodes, path))

    reserved('cmd_db', 'soc_cmd_db')
    reserved('mpss', 'soc_mpss_mem')
    reserved('ipa_gsi', 'modem_ipa_fw_at')              # where the IPA's firmware is loaded (the ipa_gsi reservation)
    _, smem = find(nodes, 'qcom,smem')
    if smem and 'memory-region' in smem:
        target = by_phandle.get(u32(smem['memory-region']))
        if target:
            address('soc_smem', reg_address(nodes, target[0]))
    path, _ = find(nodes, 'qcom,msm-imem')
    imem = reg_address(nodes, path)
    if imem is not None:
        address('soc_imem', imem & ~0x1fffff)         # the 2 MB block that holds it

    def stream(compatible, arg):
        _, props = find(nodes, compatible)
        if props and 'iommus' in props and len(props['iommus']) >= 8:
            args.append((arg, u32(props['iommus'], 1)))   # iommus = <phandle stream mask>; the stream is the second cell

    stream('qcom,ipa-smmu-ap-cb', 'soc_ipa_stream')
    stream('qcom,ipa-smmu-uc-cb', 'soc_ipa_stream_uc')

    def pas_id(leaf, arg):
        _, props = find_named(nodes, leaf)
        if props and 'qcom,pas-id' in props:
            args.append((arg, u32(props['qcom,pas-id'])))

    pas_id('qcom,mss', 'soc_pas_mpss')
    pas_id('qcom,ipa_fws', 'soc_pas_ipa')

    _, ipa_fws = find_named(nodes, 'qcom,ipa_fws')
    ipa_fw_name = None
    if ipa_fws and 'qcom,firmware-name' in ipa_fws:
        ipa_fw_name = ipa_fws['qcom,firmware-name'].split(b'\0')[0].decode()

    for name, value in args:
        print(f'{name}={value:#x}')
    if ipa_fw_name:
        print(f'soc_ipa_fw={ipa_fw_name}')
    if not args:
        sys.exit('soc-args.py: found none of the expected nodes; is this the phone\'s own device tree?')


if __name__ == '__main__':
    main()
