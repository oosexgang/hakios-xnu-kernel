"""A tiny writer of flattened device trees, for tests: small trees of their own, so that no part of a
real phone's tree has to be committed.

    fdt({"": ({}, {"soc": ({"#address-cells": cells(1), "#size-cells": cells(1)},
                              {"qcom,ipcc@408000": ({"compatible": compat("qcom,ipcc"),
                                                      "reg": cells(0x408000, 0x1000)}, {})})})})

A tree is {name: (properties, children)}, properties are {name: bytes}.
"""
import struct


def fdt(tree):
    """A flattened device tree for {name: (props, children)}; props is {prop: bytes}."""
    strings, offsets, body = bytearray(), {}, bytearray()

    def string_off(name):
        if name not in offsets:
            offsets[name] = len(strings)
            strings.extend(name.encode() + b"\0")
        return offsets[name]

    def pad(b):
        b.extend(b"\0" * (-len(b) % 4))

    def node(name, props, children):
        body.extend(struct.pack(">I", 1))
        body.extend(name.encode() + b"\0")
        pad(body)
        for pname, value in props.items():
            body.extend(struct.pack(">III", 3, len(value), string_off(pname)))
            body.extend(value)
            pad(body)
        for cname, (cprops, cchildren) in children.items():
            node(cname, cprops, cchildren)
        body.extend(struct.pack(">I", 2))

    node("", *tree[""])
    body.extend(struct.pack(">I", 9))
    header_size, rsvmap = 40, 16
    off_struct = header_size + rsvmap
    off_strings = off_struct + len(body)
    total = off_strings + len(strings)
    header = struct.pack(">IIIIIIIIII", 0xD00DFEED, total, off_struct, off_strings, header_size, 17, 16, 0,
                         len(strings), len(body))
    return header + b"\0" * rsvmap + bytes(body) + bytes(strings)


def cells(*values):
    return b"".join(struct.pack(">I", v) for v in values)


def compat(*names):
    return b"".join(n.encode() + b"\0" for n in names)
