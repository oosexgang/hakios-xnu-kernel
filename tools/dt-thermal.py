#!/usr/bin/env python3
"""The thermal zones of a Qualcomm phone's device tree, as a table: zone, temperature sensor, trip points.

usage: tools/dt-thermal.py DEVICE-TREE.dtb [--c]

drivers/power/hakios.thermal.c reads the SoC's temperature sensors (TSENS) and wants to name each one the
way the phone's own tree does (cpu-0-0, gpuss-0, ...) and to know the temperatures the vendor chose to act at.
This reads those out of the tree: for every node under /soc/thermal-zones that has a "thermal-sensors" =
<&tsensN id>, the zone's name, which TSENS block (by the block's register address) and which of its sensors,
and the zone's trip points (temperature in millidegrees, type). Only numbers and names are read out; no part
of the tree is copied. With --c it prints the C table the driver includes.
"""
import struct
import sys


def load():
    import importlib.util
    import os
    spec = importlib.util.spec_from_file_location("soc_args", os.path.join(os.path.dirname(os.path.abspath(__file__)), "soc-args.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def cells(prop):
    return list(struct.unpack('>%dI' % (len(prop) // 4), prop[:len(prop) // 4 * 4]))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        sys.exit(__doc__)
    mod = load()
    nodes = mod.nodes_of(open(args[0], "rb").read())
    phandle_reg = {}
    for path, props in nodes.items():
        if "compatible" in props and (b"qcom,tsens26xx" in props["compatible"] or b"qcom,tsens-v2" in props["compatible"]) and "phandle" in props:
            # two regions, the readings ("tm") and the control ("srot"): the vendor's qcom,tsens26xx (redwood) lists
            # srot first, two cells each; qcom,tsens-v2 (zeus, Linux's binding) lists tm first, one cell each
            reg = cells(props["reg"])
            tm_first = b"qcom,tsens-v2" in props["compatible"]
            phandle_reg[cells(props["phandle"])[0]] = reg[0] if tm_first or len(reg) < 4 else reg[2]
    zones = []
    for path, props in sorted(nodes.items()):
        parts = path.split("/")
        if len(parts) < 3 or parts[-2] != "thermal-zones" or "thermal-sensors" not in props:
            continue
        ts = cells(props["thermal-sensors"])
        if ts[0] not in phandle_reg:
            continue                              # a sensor of something else (the PMICs', the modem's)
        trips = []
        for tpath, tprops in sorted(nodes.items()):
            if tpath.startswith(path + "/trips/") and tpath.count("/") == path.count("/") + 2 and "temperature" in tprops:
                kind = tprops.get("type", b"").rstrip(b"\0").decode()
                temp = struct.unpack('>i', tprops["temperature"][:4])[0]
                trips.append((temp, kind))
        zones.append((parts[-1], phandle_reg[ts[0]], ts[1] if len(ts) > 1 else 0, sorted(trips)))
    # one row a sensor: the vendor has a "-step" zone (where it starts to slow things down) and a "-usr" zone (where
    # it gives up) for most; the name is the zone's without that suffix
    sensors = {}
    for name, tm, sensor, trips in zones:
        for suffix in ("-step", "-usr"):
            if name.endswith(suffix):
                name = name[:-len(suffix)]
        row = sensors.setdefault((tm, sensor), {"name": name, "temps": []})
        row["temps"] += [t for t, _k in trips]
    if "--c" in sys.argv:
        print("/* made by tools/dt-thermal.py from the phone's device tree: sensor, the TSENS block it is in (the address of its")
        print(" * readings), its number there, the temperature the vendor starts to slow down at, and the one it gives up at,")
        print(" * in millidegrees */")
        for (tm, sensor), row in sorted(sensors.items(), key=lambda kv: (kv[0][0], kv[0][1])):
            print('	{ "%s", %#x, %u, %d, %d },' % (row["name"], tm, sensor, min(row["temps"]), max(row["temps"])))
    else:
        for (tm, sensor), row in sorted(sensors.items(), key=lambda kv: (kv[0][0], kv[0][1])):
            print("%-14s tm=%#x sensor=%-2d slow down at %d, gives up at %d" % (row["name"], tm, sensor, min(row["temps"]), max(row["temps"])))


main()
