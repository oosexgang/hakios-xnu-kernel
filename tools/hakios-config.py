#!/usr/bin/env python3
"""HakiOS configuration: defconfigs, fragments, a .config and the kernel's boot-arguments made from it.

The way Linux does it (arch/arm64/configs/<device>_defconfig, scripts/kconfig/merge_config.sh, `make
savedefconfig`), for a kernel whose options are not compiled in but given on its command line:

  tools/hakios-config.py defconfig redwood            .config = configs/defconfig + configs/redwood_defconfig
  tools/hakios-config.py defconfig redwood open       ... and the fragment configs/open.config on top
  tools/hakios-config.py merge virt                   put a fragment over the .config there is
  tools/hakios-config.py args                         the kernel's boot-arguments for the .config
  tools/hakios-config.py check | show | needs         validate it; say what it is; which firmware it wants
  tools/hakios-config.py savedefconfig                the .config as a minimal defconfig (its differences from configs/defconfig)
  tools/hakios-config.py list | help SYMBOL           what configs/ holds; what a symbol means

`make redwood_defconfig` and `make config` call it.

A symbol CONFIG_GPU=y is the boot-argument gpu=1; CONFIG_CPUS=8 is cpus=8; CONFIG_MODEM_BUFFER=0xa8000000 is
modem_buffer=0xa8000000. A symbol that is "not set" is left out of the command line (the kernel's own default
applies). A few symbols are not arguments but facts about the build (CONFIG_DEVICE, CONFIG_SOC, CONFIG_FIRMWARE).

Where this differs from Linux's, on purpose:
  - CONFIG_FIRMWARE="none" is a profile that loads NOTHING of the phone's own (no GPU, Wi-Fi, audio or modem
    firmware pulled from the device): the open configuration. The check refuses an option that needs firmware in it.
  - the kernel's command line holds 607 characters; the check says how much of them the configuration takes, and
    refuses what cannot fit, because a command line that is too long loses its tail without an error.
"""
import argparse
import collections
import difflib
import importlib.util
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
KERNEL_LINE = 607                   # BOOT_LINE_LENGTH 608 less the terminator: what the kernel's boot_args can hold
RUN_OVERHEAD = 260                  # what a run adds itself: the loader's own arguments, xl's, the runner's (measured: ~190-250)

class ConfigError(Exception):
    pass


# name, kind, boot-arg, one-line help, firmware directory it needs, what it requires
# kinds: bool (y/n, becomes arg=1), int, hex, str, and meta (a fact about the build, not an argument)
SYMBOLS = [
    ("DEVICE", "meta", None, "the directory of the phone under devices/ this configuration is for", None, None),
    ("SOC", "meta", None, "the SoC family (sc7280: SM7325, QCM6490); it is part of the kernel's version string", None, None),
    ("FIRMWARE", "meta", None, "\"device\": options may use firmware pulled from your own phone (fw-*/); \"none\": the open "
                               "configuration, nothing of the phone's own is loaded", None, None),
    ("CPUS", "int", "cpus", "CPU cores the kernel uses, 1 to 8; one if not set", None, None),
    ("BOOTCPU", "int", "bootcpu", "the core the kernel runs on, 0 to 7; 7 is the biggest core (the loader keeps core 0)", None, None),
    ("CPUFREQ", "bool", "cpufreq", "set the boot core to its top speed", None, None),
    ("UFS", "int", "ufs", "the phone's storage: 1 a survey, read only; 2 the loader's block service, through which the kernel "
                          "reads it (the modem's file systems, /private/var)", None, None),
    ("UFS_WRITE", "bool", "ufs_write", "let the kernel write, to the partition `darwin` and nowhere else (writes to the phone's "
                                       "storage: it is the one switch for it)", None, "UFS=2"),
    ("VAR_STORE", "bool", "var_store", "keep /private/var across reboots, in the partition `darwin`", None, "UFS_WRITE"),
    ("TOUCHSCREEN", "bool", "touchscreen", "the touch screen, and the power and volume buttons", None, None),
    ("GPU", "bool", "gpu", "rendering on Adreno (an image built with GL=1; the GPU's firmware from your phone)", "fw-gpu", None),
    ("ADSP", "bool", "adsp", "the audio DSP; the battery level in the UI and charging state come through it", "fw-audio", None),
    ("SOUND", "int", "sound", "the speakers (4 is what the README's phone line uses)", "fw-audio", "ADSP"),
    ("MIC", "int", "mic", "the microphone (4 is what the README's phone line uses)", "fw-audio", "ADSP"),
    ("WIFI", "bool", "wifi", "Wi-Fi: the radio, its driver, a connection (also fw-wifi/mac and fw-wifi/network)", "fw-wifi", None),
    ("HAPTIC", "int", "haptic", "the vibration motor (5 is what the README's phone line uses)", "fw-haptic", None),
    ("CAM", "int", "cam", "the cameras (5 is what the README uses)", "fw-camera", None),
    ("MODEM", "bool", "modem", "the modem beside the system: its firmware, file systems, IPA, SIM, NAS", "fw-modem", "UFS=2"),
    ("MODEM_AGAIN", "bool", "modem_again", "start the modem although a kernel before this one left it shut down", None, "MODEM"),
    ("MODEM_BUFFER", "hex", "modem_buffer", "the modem's RMTFS buffer, an address below 4 GB (0xa8000000 on the reference phone)", None, "MODEM"),
    ("NET_TRACE", "int", "net_trace", "a log line for each of the first N frames on the network interfaces", None, None),
    ("NET_NAMES", "bool", "net_names", "a log line for every name asked of a DNS server and every TCP connection begun "
                                        "(the names go to the log: for debugging only)", None, None),
    ("VIRTIO_NET", "bool", "virtio_net", "QEMU's virtio network device as en0 (QEMU only; its bus address means nothing on a phone)", None, None),
    ("SOCINFO", "bool", "socinfo", "read the chip's identity from the firmware's shared memory: kern.hakios.soc_* (SoC number, revision, board)", None, None),
    ("THERMAL", "bool", "thermal", "read the SoC's 27 temperature sensors: kern.hakios.thermal_* and a line in the log; read only", None, None),
    ("THERMAL_POLL", "int", "thermal_poll", "how often the sensors are read, in milliseconds (200 to 60000; 2000)", None, "THERMAL"),
    ("THERMAL_STOP", "int", "thermal_stop", "hand control back to the loader when the hottest sensor reads this many degrees three times in a row "
                                           "(0: never; 105): the protection of a phone left running unattended", None, "THERMAL"),
    ("BATTERY_STOP", "int", "battery_stop", "hand control back to the loader when the battery is at or below this percent three looks in a row (0: never); "
                                            "acts only on a known charge, with a battery and no charger: for a run nobody watches", None, None),
    ("BATTERY_STOP_POLL", "int", "battery_stop_poll", "how often the battery guard looks, in seconds (5 to 600; 20)", None, "BATTERY_STOP"),
    ("PMIC", "bool", "pmic", "list every block of the power-management chips the SPMI arbiter shows, and read the PMIC's clock; read only", None, None),
    # Linux's CONFIG_CMDLINE: whatever the symbols above do not cover, as name=value words in quotes. It is not an escape
    # from the checks: every name must be one the source reads (configs/boot-args.lst), so a typo is an error, not a no-op.
    ("BOOTARGS", "args", None, "more kernel arguments, as `name=value` words in quotes; each name must be one docs/boot-args.md lists", None, None),
]

# The group a symbol is listed under in the documented defconfig (the generated ones carry theirs in configs/symbols.tsv)
GROUP_OF = {
    "DEVICE": "Build facts", "SOC": "Build facts", "FIRMWARE": "Build facts",
    "CPUS": "CPU, memory and boot", "BOOTCPU": "CPU, memory and boot", "CPUFREQ": "CPU, memory and boot",
    "UFS": "Storage and APFS", "UFS_WRITE": "Storage and APFS", "VAR_STORE": "Storage and APFS",
    "TOUCHSCREEN": "Touch and buttons", "GPU": "GPU and display",
    "ADSP": "Audio and battery", "SOUND": "Audio and battery", "MIC": "Audio and battery",
    "WIFI": "Wi-Fi", "HAPTIC": "Haptics and torch", "CAM": "Cameras",
    "MODEM": "Cellular", "MODEM_AGAIN": "Cellular", "MODEM_BUFFER": "Cellular",
    "NET_TRACE": "Network", "NET_NAMES": "Network", "VIRTIO_NET": "Network",
    "BATTERY_STOP": "Audio and battery", "BATTERY_STOP_POLL": "Audio and battery", "SOCINFO": "Chip, thermal and PMIC", "THERMAL": "Chip, thermal and PMIC", "THERMAL_POLL": "Chip, thermal and PMIC",
    "THERMAL_STOP": "Chip, thermal and PMIC", "PMIC": "Chip, thermal and PMIC", "BOOTARGS": "Extra arguments",
}
LAYER_OF = {}
RELAXED = re.compile(r"[A-Za-z0-9_.,:/+@=\-]+")       # what a text value may hold: no space, no quote, no backslash


def load_generated():
    """The symbols tools/boot-args.py made for the arguments the table above does not name (configs/symbols.tsv): one
    CONFIG_ symbol for each argument that is read anywhere in the source. Nothing if the file is not there."""
    path = os.path.join(ROOT, "configs", "symbols.tsv")
    rows = []
    if os.path.isfile(path):
        for line in open(path, encoding="utf-8"):
            f = line.rstrip("\n").split("\t")
            if len(f) < 7:
                continue
            name, kind, arg, group, layers, files, text = f[:7]
            if name in {s[0] for s in SYMBOLS}:
                continue
            GROUP_OF[name] = group
            LAYER_OF[name] = (layers, files)
            rows.append((name, kind, arg, text or "read by %s (%s)" % (files.replace("(patch)", "the patch to Apple's files").replace(",", ", "),
                         "".join({"k": "kernel", "l": "loader", "u": "user space"}[c] + "/" for c in layers).rstrip("/").replace("/", " and ")),
                         None, None))
    return rows


GENERATED = load_generated()
SYMBOLS += GENERATED


# ---- the drivers: what the kernel is built with -----------------------------------------------------------------
#
# Every other symbol is a boot-argument. These are not: CONFIG_HAKIOS_<NAME>=y decides whether a driver is compiled into
# the kernel at all, the way XNU decides it for its own parts -- an `options HAKIOS_<NAME>` line in config/MASTER, the
# driver's files `optional hakios_<name>` in bsd/conf/files and iokit/conf/files, and -DHAKIOS_<NAME> for the code that
# calls into it. What a driver is, which files it is and what it needs is written once, in drivers/<subsystem>/conf.

def layers(root):
    """[(kind, name, directory)] of the repositories a build is put together from, common first: this one ("common"), then each
    SoC's (socs/<name>/, with a soc.conf) and each device's (devices/<name>/, with a device.conf) that is checked out here. They
    are the layers of docs/porting/repositories.md: a SoC or device repository is a directory of the same shape (configs/,
    drivers/<subsystem>/conf) that `tools/hakios-device.py sync` clones into place, or that lives in this tree until it has a
    repository of its own."""
    out = [("common", "common", root)]
    for kind, top, marker in (("soc", "socs", "soc.conf"), ("device", "devices", "device.conf")):
        base = os.path.join(root, top)
        for name in sorted(os.listdir(base)) if os.path.isdir(base) else []:
            if os.path.isfile(os.path.join(base, name, marker)):
                out.append((kind, name, os.path.join(base, name)))
    # A device repository carries its SoC's layer with it (devices/<name>/soc/, with a soc.conf that names the SoC), so that one
    # repository is all a device needs. Two devices with the same SoC each carry a copy: the copies must be the same, or a build
    # would depend on which device is checked out.
    seen = {name: (top, soc_digest(top)) for kind, name, top in out if kind == "soc"}
    for kind, name, top in list(out):
        if kind != "device":
            continue
        soc = os.path.join(top, "soc")
        named = soc_name(soc)
        if named is None:
            continue
        digest = soc_digest(soc)
        if named in seen:
            if seen[named][1] != digest:
                raise ConfigError("the SoC %s is in %s and in %s, and the two differ: make them the same (one SoC, one layer)" %
                                  (named, os.path.relpath(seen[named][0], root), os.path.relpath(soc, root)))
            continue
        seen[named] = (soc, digest)
        out.insert(1 + sum(1 for k, _n, _t in out if k == "soc"), ("soc", named, soc))
    return out


def soc_name(top):
    """The name a soc.conf in TOP gives its SoC (`soc <name>`), or None if there is none."""
    path = os.path.join(top, "soc.conf")
    if not os.path.isfile(path):
        return None
    for line in open(path, encoding="utf-8"):
        m = re.match(r"soc\s+([a-z0-9][a-z0-9_-]*)\s*$", line)
        if m:
            return m.group(1)
    return None


def soc_digest(top):
    """What a SoC layer holds, as one value: two copies of the same SoC compare equal when every file is the same."""
    import hashlib
    h = hashlib.sha256()
    for d, dirs, files in sorted(os.walk(top)):
        dirs.sort()
        for f in sorted(files):
            path = os.path.join(d, f)
            h.update(os.path.relpath(path, top).encode() + b"\0" + open(path, "rb").read() + b"\0")
    return h.hexdigest()


class Driver:
    def __init__(self, name, sub, where, origin=None):
        self.name, self.sub, self.where = name, sub, where
        self.origin = origin                # the SoC or device repository it comes from; None: this one's drivers/
        self.files, self.requires, self.default, self.help = [], [], "y", ""

    def source(self, path):
        """Where a file of it is kept: drivers/<subsystem>/<name>, in this repository or in its own."""
        return os.path.join(self.origin or "", "drivers", self.sub, os.path.basename(path))

    @property
    def symbol(self):
        return "HAKIOS_" + self.name

    @property
    def tag(self):
        return "hakios_" + self.name.lower()


def load_drivers(root):
    """([Driver], [(path, tags, subsystem, help)]) from drivers/*/conf: the drivers that can be left out, and the "standard"
    files every kernel has. The format (docs/drivers-config.md):

        standard <path in the XNU tree> [<XNU option>...]     always built (with the XNU options given, if any)
        driver <NAME>                                           CONFIG_HAKIOS_<NAME>; the lines under it, indented:
            file <path in the XNU tree> [<XNU option>...]       one of its files (an iokit .cpp also wants iokitcpp)
            requires <NAME>...                                  drivers it cannot be built without
            default y|n                                         what a configuration that does not name it gets (y)
            help <text>                                         what it is; more lines of help go on
    """
    drivers, standard, seen_files = [], [], {}
    confs = []
    for kind, _name, top in layers(root):
        base = os.path.join(top, "drivers")
        for sub in sorted(os.listdir(base)) if os.path.isdir(base) else []:
            if os.path.isfile(os.path.join(base, sub, "conf")):
                confs.append((kind, top, sub, os.path.join(base, sub, "conf")))
    for kind, top, sub, path in confs:
        origin = None if kind == "common" else top
        shown = os.path.relpath(path, root).replace(os.sep, "/")
        block = None
        for n, raw in enumerate(open(path, encoding="utf-8"), 1):
            where = "%s:%d" % (shown, n)
            line = raw.split("#", 1)[0].rstrip()
            if not line.strip():
                continue
            words = line.split()
            if not raw[0].isspace():
                if words[0] == "driver" and len(words) == 2 and re.fullmatch(r"[A-Z][A-Z0-9_]*", words[1]):
                    block = Driver(words[1], sub, where, origin)
                    drivers.append(block)
                elif words[0] == "standard" and len(words) >= 2:
                    if origin is not None:
                        raise ConfigError("%s: a SoC or device repository has drivers, not `standard` files (every kernel would have "
                                          "them, which only the common repository decides)" % where)
                    block = [words[1], words[2:], sub, ""]
                    standard.append(block)
                    files = [words[1]]
                else:
                    raise ConfigError("%s: want `driver NAME` (NAME in capitals) or `standard PATH`, got %r" % (where, line.strip()))
                if isinstance(block, Driver):
                    continue
            else:
                if block is None:
                    raise ConfigError("%s: an indented line before any `driver` or `standard`" % where)
                key, rest = words[0], line.strip()[len(words[0]):].strip()
                if key == "help":
                    if isinstance(block, Driver):
                        block.help = (block.help + " " + rest).strip()
                    else:
                        block[3] = (block[3] + " " + rest).strip()
                    continue
                if not isinstance(block, Driver):
                    raise ConfigError("%s: a `standard` file takes only `help` lines, got %r" % (where, key))
                if key == "file" and len(words) >= 2:
                    block.files.append((words[1], words[2:]))
                    files = [words[1]]
                elif key == "requires" and len(words) >= 2:
                    block.requires += words[1:]
                    continue
                elif key == "default" and len(words) == 2 and words[1] in ("y", "n"):
                    block.default = words[1]
                    continue
                else:
                    raise ConfigError("%s: want file PATH, requires NAME..., default y|n or help TEXT, got %r" % (where, line.strip()))
            for f in files:
                if not f.startswith(("bsd/dev/hakios.", "iokit/Kernel/hakios.")) or not f.endswith((".c", ".cpp")):
                    raise ConfigError("%s: %s is not a hakios.*.c or .cpp of bsd/dev/ or iokit/Kernel/" % (where, f))
                if f in seen_files:
                    raise ConfigError("%s: %s is already built by %s" % (where, f, seen_files[f]))
                seen_files[f] = where
    names = {d.name: d for d in drivers}
    if len(names) != len(drivers):
        dup = [n for n, c in collections.Counter(d.name for d in drivers).items() if c > 1]
        raise ConfigError("drivers/*/conf: the driver %s is declared twice" % ", ".join(dup))
    for d in drivers:
        if not d.files:
            raise ConfigError("%s: the driver %s has no file" % (d.where, d.name))
        for r in d.requires:
            if r not in names:
                close = difflib.get_close_matches(r, names, n=1)
                raise ConfigError("%s: %s requires %s, which no drivers/*/conf declares%s" % (
                    d.where, d.name, r, " (did you mean %s?)" % close[0] if close else ""))
    return drivers, [tuple(s) for s in standard]


def driver_symbols(drivers):
    return [(d.symbol, "driver", None, d.help or "the driver %s (%s)" % (d.name, d.where), None, None) for d in drivers]


try:
    DRIVERS, STANDARD = load_drivers(ROOT)
except ConfigError as e:                # said again by every command, with the rest of what it does
    DRIVERS, STANDARD, DRIVERS_ERROR = [], [], str(e)
else:
    DRIVERS_ERROR = None
DRIVER_BY_SYMBOL = {d.symbol: d for d in DRIVERS}
for _d in DRIVERS:
    GROUP_OF[_d.symbol] = "Drivers built in"
SYMBOLS[3:3] = driver_symbols(DRIVERS)      # after the build facts: they are facts about the build as well
BY_NAME = {s[0]: s for s in SYMBOLS}
FIRMWARE_VALUES = ("device", "none")
RANGES = {"CPUS": (1, 8), "BOOTCPU": (0, 7), "UFS": (1, 2), "THERMAL_POLL": (200, 60000), "THERMAL_STOP": (0, 160), "BATTERY_STOP": (0, 99), "BATTERY_STOP_POLL": (5, 600)}


# ---- reading and writing --------------------------------------------------------------------------------------

def parse(path):
    """A defconfig, a fragment or a .config as (set, unset): {NAME: value} and a set of NAMEs `# CONFIG_X is not set`.
    Linux's rules: CONFIG_NAME=value, strings in quotes, no comment after a value, `# CONFIG_NAME is not set` is the
    way to say a symbol is off (so that a fragment can switch off what an earlier file switched on). `=n` is accepted
    for a bool as the same thing."""
    values, unset = {}, set()
    for n, raw in enumerate(open(path, encoding="utf-8"), 1):
        line = raw.strip()
        m = re.fullmatch(r"#\s*CONFIG_([A-Z0-9_]+) is not set", line)
        if m and m.group(1) in DRIVER_BY_SYMBOL:
            values[m.group(1)] = "n"        # a driver left out: not "the kernel's default", which for a driver is to build it
            unset.discard(m.group(1))
            continue
        if m:
            unset.add(m.group(1))
            values.pop(m.group(1), None)
            continue
        if not line or line.startswith("#"):
            continue
        m = re.fullmatch(r"CONFIG_([A-Z0-9_]+)=(.*)", line)
        if not m:
            raise ConfigError("%s:%d: want CONFIG_NAME=value or # CONFIG_NAME is not set, got %r" % (path, n, line))
        name, value = m.group(1), m.group(2)
        if name not in BY_NAME:
            close = difflib.get_close_matches(name, BY_NAME, n=1)
            raise ConfigError("%s:%d: no symbol CONFIG_%s%s" % (path, n, name, " (did you mean CONFIG_%s?)" % close[0] if close else ""))
        if value == "n" and BY_NAME[name][1] in ("bool", "arg"):
            unset.add(name)
            values.pop(name, None)
            continue
        values[name] = value
        unset.discard(name)
    return values, unset


def typed(name, value):
    """The value of a symbol as Python has it, or a ConfigError that says what is wrong."""
    kind = BY_NAME[name][1]
    if kind == "driver":
        if value not in ("y", "n"):
            raise ConfigError("CONFIG_%s is a driver: y (built in), or not set (left out) (got %s)" % (name, value))
        return value == "y"
    if kind == "bool":
        if value != "y":
            raise ConfigError("CONFIG_%s is a bool: y, or not set (got %s)" % (name, value))
        return True
    if kind == "arg":
        # a switch (y: the argument is 1), a number as it is written, or a quoted text (a list, a name, a path)
        if value == "y":
            return "1"
        if re.fullmatch(r"-?(0[xX][0-9a-fA-F]+|[0-9]+)", value):
            return value
        if len(value) >= 2 and value[0] == '"' and value[-1] == '"' and RELAXED.fullmatch(value[1:-1]):
            return value[1:-1]
        raise ConfigError('CONFIG_%s is y, a number, or a text in quotes of letters, digits and . , : / + @ = - (got %s)' % (name, value))
    if kind in ("int", "hex"):
        if kind == "hex" and not value.lower().startswith("0x"):
            raise ConfigError("CONFIG_%s is a hex number and starts with 0x (got %s)" % (name, value))
        try:
            number = int(value, 16 if value.lower().startswith("0x") else 10)
        except ValueError:
            raise ConfigError("CONFIG_%s wants a %s, got %s" % (name, "hex number (0x...)" if kind == "hex" else "number", value))
        low, high = RANGES.get(name, (0, None))
        if number < low or (high is not None and number > high):
            raise ConfigError("CONFIG_%s is %s to %s, got %s" % (name, low, high if high is not None else "any", value))
        return number
    if not (len(value) >= 2 and value[0] == '"' and value[-1] == '"'):
        raise ConfigError("CONFIG_%s is a string in quotes (got %s)" % (name, value))
    text = value[1:-1]
    if kind == "args":
        return split_args(text)
    if name == "FIRMWARE" and text not in FIRMWARE_VALUES:
        raise ConfigError('CONFIG_FIRMWARE is "device" or "none" (got "%s")' % text)
    if not re.fullmatch(r"[A-Za-z0-9._-]*", text):
        raise ConfigError("CONFIG_%s holds letters, digits, dots, dashes and underscores (got %s)" % (name, value))
    return text


def split_args(text):
    """CONFIG_BOOTARGS as a list of name=value words. A word is name=value with no space, quote or backslash in it (the
    value is carried through a shell, a payload and a command line that has a length limit and no quoting)."""
    words = text.split()
    for word in words:
        if not re.fullmatch(r"[a-z][a-z0-9_.\-]*=[A-Za-z0-9_.:,/+@\-]+", word):
            raise ConfigError('CONFIG_BOOTARGS: "%s" is not name=value (lowercase name, a value of letters, digits and . _ : , / + @ -)' % word)
    return words


def known_args(root=None):
    """The names the source reads, from configs/boot-args.lst (tools/boot-args.py --lst makes it); None if there is no list."""
    path = os.path.join(root or os.path.dirname(HERE), "configs", "boot-args.lst")
    if not os.path.isfile(path):
        return None
    return {line.split("\t")[0].strip() for line in open(path, encoding="utf-8") if line.strip()}


def config_dir(root):
    return os.path.join(root, "configs")


def config_dirs(root):
    """configs/ of this repository, then of every SoC and device repository there is (layers())."""
    return [os.path.join(top, "configs") for _kind, _name, top in layers(root) if os.path.isdir(os.path.join(top, "configs"))]


def find_config(root, name):
    """<name>, <name>_defconfig or <name>.config in configs/ here or in a SoC's or device's repository: the file for NAME. A name
    two repositories both have is an error, not a guess: which one a build took would depend on what is checked out."""
    base = os.path.basename(name)
    for candidate in (base, base + "_defconfig", base + ".config"):
        found = [os.path.join(d, candidate) for d in config_dirs(root) if os.path.isfile(os.path.join(d, candidate))]
        if len(found) > 1:
            raise ConfigError("%s is in more than one repository: %s" % (candidate, ", ".join(rel(root, f) for f in found)))
        if found:
            return found[0]
    available = sorted({n for d in config_dirs(root) for n in os.listdir(d)})
    raise ConfigError("no %s, %s_defconfig or %s.config in configs/, socs/*/configs/ or devices/*/configs/ (there is: %s)" %
                      (base, base, base, ", ".join(available) or "nothing"))


def overlay(config, files):
    """`config` with the files laid over it, later ones winning; `is not set` in a file removes a symbol.
    The same for the first file on an empty config (defconfig) and for a fragment over an existing one (merge)."""
    result = dict(config)
    for path in files:
        values, unset = parse(path)
        for name in unset:
            result.pop(name, None)
        result.update(values)
    return result


def expand(root, path, seen=()):
    """`path` preceded by the files it includes: a line `#include NAME` in a defconfig or fragment (a comment to every other
    reader) stands for configs/NAME, NAME_defconfig or NAME.config, laid first. This is how a device's defconfig is the common one
    plus its SoC's and its own (the way Android's build.config puts a kernel's configuration together), and it is why redwood_defconfig
    is three lines."""
    if path in seen:
        raise ConfigError("%s includes itself (%s)" % (rel(root, path), " -> ".join(os.path.basename(p) for p in seen + (path,))))
    files = []
    for line in open(path, encoding="utf-8"):
        m = re.match(r"#include\s+(\S+)\s*$", line)
        if m:
            files += expand(root, find_config(root, m.group(1)), seen + (path,))
    return files + [path]


def resolve(root, names):
    """The files for `defconfig NAME FRAGMENT...`: the base, the device's defconfig, then the fragments (each with what it includes)."""
    files = [find_config(root, "defconfig")]
    device = names[0] if names else "defconfig"
    if device not in ("defconfig", ""):
        files += expand(root, find_config(root, device if device.endswith("_defconfig") else device + "_defconfig"))
    for f in names[1:]:
        files += expand(root, find_config(root, f))
    return files


def rel(base, path):
    """`path` relative to `base` for the messages; as it is when it cannot be (another drive on Windows)."""
    try:
        return os.path.relpath(path, base).replace(os.sep, "/")
    except ValueError:
        return path.replace(os.sep, "/")


def write_config(config, path, sources):
    """The .config, in the form Linux's has: every symbol, set or `is not set`, in the order of the table."""
    lines = ["#",
             "# Made by tools/hakios-config.py from %s." % ", ".join(sources),
             "# Do not edit it: change a defconfig or a fragment in configs/, and make the .config again.",
             "#"]
    built = drivers_built(config)
    for name, kind, _arg, _help, _needs, _requires in SYMBOLS:
        if kind == "driver":
            lines.append("CONFIG_%s=y" % name if built[name] else "# CONFIG_%s is not set" % name)
        elif name in config:
            lines.append("CONFIG_%s=%s" % (name, config[name]))
        elif kind != "meta":
            lines.append("# CONFIG_%s is not set" % name)
    open(path, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")


def drivers_built(config):
    """{CONFIG name: True if built in} for every driver: its value in `config`, or its default where the configuration does
    not name it (a .config made before there were driver symbols builds every driver, as the kernel was built then)."""
    return {d.symbol: config.get(d.symbol, d.default) == "y" for d in DRIVERS}


def kernel_ver(root, config_path):
    """The kernel.ver string this .config gives (tools/kernel-version.py reads CONFIG_SOC from it)."""
    out = subprocess.run([sys.executable, os.path.join(HERE, "kernel-version.py"), "--config", config_path, "--root", root],
                         capture_output=True, text=True)
    return out.stdout.strip() or "unknown"


# ---- what a configuration means -------------------------------------------------------------------------------

def check(config):
    """(problems, boot-arguments) of a merged configuration: the problems are strings, empty if it is fine."""
    problems = []
    parsed = {}
    for name, value in config.items():
        try:
            parsed[name] = typed(name, value)
        except ConfigError as e:
            problems.append(str(e))
    firmware = parsed.get("FIRMWARE", "device")
    problems += check_drivers(config, parsed)
    for name in parsed:
        _n, _kind, _arg, _help, needs, requires = BY_NAME[name]
        if _kind == "driver":
            continue
        if needs and firmware == "none":
            problems.append('CONFIG_%s needs the phone\'s own firmware (%s/), and this configuration says CONFIG_FIRMWARE="none": '
                            "it loads nothing from the device" % (name, needs))
        if requires:
            wanted, _, value = requires.partition("=")
            have = parsed.get(wanted)
            if have is None or (value and str(have) != value):
                problems.append("CONFIG_%s requires CONFIG_%s%s" % (name, wanted, "=" + value if value else ""))
    args = boot_args(parsed)
    extra = parsed.get("BOOTARGS", [])
    known = known_args() if extra else None
    owned = {s[2]: s[0] for s in SYMBOLS if s[2] and s[0] in parsed}      # argument -> the symbol that already sets it
    seen = set()
    for word in extra:
        arg = word.partition("=")[0]
        if known is not None and arg not in known:
            close = difflib.get_close_matches(arg, known, n=1)
            problems.append("CONFIG_BOOTARGS: nothing reads the argument %s (docs/boot-args.md lists them)%s" %
                            (arg, "; did you mean %s?" % close[0] if close else ""))
        if arg in owned:
            problems.append("CONFIG_BOOTARGS: %s is already set by CONFIG_%s: say it there" % (arg, owned[arg]))
        if arg in seen:
            problems.append("CONFIG_BOOTARGS: %s twice" % arg)
        seen.add(arg)
    length = len(" ".join(args))
    if length > KERNEL_LINE - RUN_OVERHEAD:
        problems.append("the boot-arguments take %d characters; a run adds about %d of its own and the kernel's command line holds %d: "
                        "the end of it would be lost" % (length, RUN_OVERHEAD, KERNEL_LINE))
    return problems, args


def check_drivers(config, parsed):
    """The problems of the drivers' choice: a driver built without one it requires, and a boot-argument set for a driver that
    is not built (it would be read by nothing: the kernel does not say so, the argument is just never looked at)."""
    problems = []
    if DRIVERS_ERROR:
        return [DRIVERS_ERROR]
    built = drivers_built(config)
    for d in DRIVERS:
        if built[d.symbol]:
            for r in d.requires:
                if not built["HAKIOS_" + r]:
                    problems.append("CONFIG_%s requires CONFIG_HAKIOS_%s (%s)" % (d.symbol, r, d.where))
    left_out = {os.path.basename(f) for d in DRIVERS if not built[d.symbol] for f, _tags in d.files}
    if not left_out:
        return problems
    readers = arg_readers()
    if readers is None:
        return problems
    owner = {os.path.basename(f): d.symbol for d in DRIVERS for f, _tags in d.files}
    wanted = [(name, BY_NAME[name][2]) for name in parsed if BY_NAME[name][2]]
    wanted += [("BOOTARGS", w.partition("=")[0]) for w in parsed.get("BOOTARGS", [])]
    for name, arg in wanted:
        read_by = readers.get(arg)
        if read_by and read_by <= left_out:
            problems.append("CONFIG_%s gives %s=, which only %s read%s, and %s not built (%s)" % (
                name, arg, ", ".join(sorted(read_by)), "s" if len(read_by) == 1 else "", "it is" if len(read_by) == 1 else "they are",
                ", ".join(sorted({"CONFIG_%s" % owner[f] for f in read_by}))))
    return problems


_READERS = []


def arg_readers():
    """{argument: {file names}} of the arguments only the kernel reads (tools/boot-args.py's scan of the source); None when
    the source cannot be scanned (not a git checkout)."""
    if not _READERS:
        try:
            spec = importlib.util.spec_from_file_location("boot_args", os.path.join(HERE, "boot-args.py"))
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            found = module.scan(ROOT)
            _READERS.append({arg: set(where["kernel"]) for arg, where in found.items()
                             if where["kernel"] and not where["loader"] and not where["user"]})
        except (OSError, subprocess.CalledProcessError, AttributeError):
            _READERS.append(None)
    return _READERS[0]


def boot_args(parsed):
    """['cpus=8', 'gpu=1', ...] in the order of the symbol table."""
    args = []
    for name, kind, arg, _help, _needs, _requires in SYMBOLS:
        if name not in parsed or (arg is None and kind != "args"):
            continue
        value = parsed[name]
        if kind == "args":
            args.extend(value)
        elif kind == "bool":
            args.append("%s=1" % arg)
        elif kind == "hex":
            args.append("%s=0x%x" % (arg, value))
        else:
            args.append("%s=%s" % (arg, value))
    return args


def load(path):
    if not os.path.isfile(path):
        raise ConfigError("no %s: make one with `tools/hakios-config.py defconfig NAME` (or `make NAME_defconfig`)" % path)
    return parse(path)[0]


def report(problems):
    for p in problems:
        print("hakios-config: PROBLEM: " + p)


# ---- commands ---------------------------------------------------------------------------------------------------

def cmd_defconfig(a):
    files = resolve(a.root, [a.name] + a.fragments)
    config = overlay({}, files)
    problems, args = check(config)
    write_config(config, a.output, [rel(a.root, f) for f in files])
    print("hakios-config: %s made from %s" % (rel(os.getcwd(), a.output), ", ".join(os.path.basename(f) for f in files)))
    print("hakios-config: %d characters of boot-arguments; kernel.ver=%s" % (len(" ".join(args)), kernel_ver(a.root, a.output)))
    report(problems)
    return 1 if problems else 0


def cmd_merge(a):
    files = []
    for f in a.fragments:
        files += expand(a.root, find_config(a.root, f))
    config = overlay(load(a.config), files)
    problems, args = check(config)
    write_config(config, a.config, [".config"] + [rel(a.root, f) for f in files])
    print("hakios-config: %s merged into %s; %d characters of boot-arguments" %
          (", ".join(os.path.basename(f) for f in files), rel(os.getcwd(), a.config), len(" ".join(args))))
    report(problems)
    return 1 if problems else 0


def cmd_args(a):
    if a.defconfig:
        # the arguments of a defconfig and fragments, without making a .config (what `make qemu-open` asks for)
        config = overlay({}, resolve(a.root, [a.defconfig] + a.fragment))
    else:
        config = load(a.config)
    problems, args = check(config)
    if problems and not a.force:
        for p in problems:
            print("hakios-config: PROBLEM: " + p, file=sys.stderr)
        return 1
    print(" ".join(args))
    return 0


def cmd_check(a):
    config = load(a.config)
    problems, args = check(config)
    for p in problems:
        print("PROBLEM: " + p)
    print("%s: %d symbols set, %d characters of boot-arguments (the kernel holds %d; a run adds about %d)%s" % (
        rel(os.getcwd(), a.config), len(config), len(" ".join(args)), KERNEL_LINE, RUN_OVERHEAD,
        "" if not problems else ", %d problem(s)" % len(problems)))
    return 1 if problems else 0


def cmd_show(a):
    config = load(a.config)
    problems, args = check(config)
    given = {x.split("=", 1)[0]: x for x in args}
    print("HakiOS configuration %s, kernel.ver=%s" % (rel(os.getcwd(), a.config), kernel_ver(a.root, a.config)))
    built = drivers_built(config)
    print("drivers built in: %s" % (" ".join(d.name.lower() for d in DRIVERS if built[d.symbol]) or "none"))
    left_out = [d.name.lower() for d in DRIVERS if not built[d.symbol]]
    if left_out:
        print("drivers left out: %s" % " ".join(left_out))
    for name, _kind, arg, _help, _needs, _requires in SYMBOLS:
        if name in config and _kind != "driver":
            print("  CONFIG_%-13s = %-14s %s" % (name, config[name], "-> " + given[arg] if arg in given else "(a fact about the build)"))
    print("boot-arguments: %s" % " ".join(args))
    print("firmware wanted from the phone: %s" % (", ".join(sorted({BY_NAME[n][4] for n in config if BY_NAME[n][4]})) or "none"))
    for p in problems:
        print("PROBLEM: " + p)
    return 1 if problems else 0


def cmd_needs(a):
    config = load(a.config)
    needed = sorted({BY_NAME[n][4] for n in config if BY_NAME[n][4]})
    firmware = typed("FIRMWARE", config["FIRMWARE"]) if "FIRMWARE" in config else "device"
    if firmware == "none" and needed:
        print('this configuration says CONFIG_FIRMWARE="none" and still sets options that need firmware: %s' % ", ".join(needed))
        return 1
    missing = 0
    for d in needed:
        path = os.path.join(a.root, d)
        have = os.path.isdir(path) and any(os.scandir(path))
        missing += not have
        print("%-12s %s" % (d + "/", "pulled" if have else "MISSING: devices/<name>/pull-firmware.sh (or pull-firmware-root.sh)"))
    if not needed:
        print("nothing of the phone's own is needed")
    return 1 if missing else 0


def cmd_savedefconfig(a):
    config = load(a.config)
    base = parse(find_config(a.root, "defconfig"))[0]
    lines = ["# A minimal defconfig: what this configuration sets that configs/defconfig does not, or sets differently."]
    built, base_built = drivers_built(config), drivers_built(base)
    for name, kind, *_ in SYMBOLS:
        if kind == "driver":
            if built[name] != base_built[name]:
                lines.append("CONFIG_%s=y" % name if built[name] else "# CONFIG_%s is not set" % name)
            continue
        if name in config and base.get(name) != config[name]:
            lines.append("CONFIG_%s=%s" % (name, config[name]))
        elif name not in config and name in base:
            lines.append("# CONFIG_%s is not set" % name)
    print("\n".join(lines))
    return 0


def first_comment(path):
    """The first comment of a file, which says what it is (`# CONFIG_X is not set` is not a comment, it is a setting)."""
    for line in open(path, encoding="utf-8"):
        if line.startswith("#") and not re.fullmatch(r"#\s*CONFIG_[A-Z0-9_]+ is not set\s*", line):
            return line.lstrip("# ").strip()
    return ""


def cmd_list(a):
    files = sorted((n, os.path.join(d, n)) for d in config_dirs(a.root) for n in os.listdir(d))
    where = lambda path: "" if os.path.dirname(path) == config_dir(a.root) else "  [%s]" % rel(a.root, os.path.dirname(os.path.dirname(path)))
    print("defconfigs (tools/hakios-config.py defconfig NAME, or make NAME_defconfig):")
    for n, path in files:
        if n == "defconfig" or n.endswith("_defconfig"):
            print("  %-16s %s%s" % (n[:-len("_defconfig")] if n.endswith("_defconfig") else n, first_comment(path), where(path)))
    print("fragments (put over a configuration: tools/hakios-config.py merge NAME, or defconfig NAME FRAGMENT):")
    for n, path in files:
        if n.endswith(".config"):
            print("  %-16s %s%s" % (n[:-len(".config")], first_comment(path), where(path)))
    return 0


GROUP_ORDER = ["Build facts", "Drivers built in", "CPU, memory and boot", "Storage and APFS", "Touch and buttons", "GPU and display", "Audio and battery",
               "Wi-Fi", "Cellular", "Cameras", "Haptics and torch", "Chip, thermal and PMIC", "Network", "Security stand-ins",
               "Debugging", "Loader", "User space", "Version", "Extra arguments"]


def external(name):
    """A symbol of a driver that a SoC or device repository brings (drivers/*/conf there): the common configuration does not name it,
    because the common repository must hold together without that repository checked out. Built like any driver unless left out."""
    d = DRIVER_BY_SYMBOL.get(name)
    return d is not None and d.origin is not None


def listing(config, title_lines, common=False):
    """The documented defconfig of `config`: every symbol there is, by group, each with its meaning in a comment above it and its value
    (or `# CONFIG_X is not set`) under it. Linux's defconfigs list only what differs from the defaults; this one lists everything,
    because here a symbol's default is "the kernel decides" and a reader should see every setting there is.
    common: the common defconfig, which leaves out the symbols of SoC and device repositories' own drivers (external())."""
    by_group = collections.defaultdict(list)
    for sym in SYMBOLS:
        if common and external(sym[0]):
            continue
        by_group[GROUP_OF.get(sym[0], "User space")].append(sym)
    built = drivers_built(config)
    if common:
        built = {k: on for k, on in built.items() if not external(k)}
    config = {k: v for k, v in config.items() if k not in built and not (common and external(k))}
    config.update({k: "y" for k, on in built.items() if on})
    out = list(title_lines)
    out += ["# %d symbols in %d groups; %d are set, %d of them drivers built in." % (
        sum(len(v) for v in by_group.values()), len([g for g in by_group if by_group[g]]), len(config), sum(built.values())), "#", ""]
    for group in GROUP_ORDER + sorted(g for g in by_group if g not in GROUP_ORDER):
        syms = by_group.get(group)
        if not syms:
            continue
        out += ["#", "# %s (%d)" % (group, len(syms)), "#", ""]
        for name, kind, arg, help_, needs, requires in syms:
            text = help_ if len(help_) <= 118 else help_[:117].rsplit(" ", 1)[0] + " ..."
            note = ""
            if kind == "driver":
                d = DRIVER_BY_SYMBOL[name]
                note += " [%s%s]" % (", ".join(os.path.basename(f) for f, _t in d.files),
                                     "; requires " + ", ".join("CONFIG_HAKIOS_" + r for r in d.requires) if d.requires else "")
            if needs:
                note += " [needs %s/ from the phone]" % needs
            if requires:
                note += " [requires CONFIG_%s]" % requires
            out.append("# %s%s" % (text, note))
            out.append("CONFIG_%s=%s" % (name, config[name]) if name in config else "# CONFIG_%s is not set" % name)
            out.append("")
    return "\n".join(out) + "\n"


def cmd_listing(a):
    """listing NAME: the documented defconfig of NAME (a defconfig or fragment, with what it includes) on stdout."""
    files = resolve(a.root, [a.name] + a.fragments)
    config = overlay({}, files)
    title = ["#", "# HakiOS %s defconfig: every setting there is, with what it does." % a.name,
             "# Made by tools/hakios-config.py listing %s -o <this file>; the values are the ones that file sets, and tests/CI check that it names every symbol." % a.name,
             "#"]
    text = listing(config, title, common=(a.name == "common"))
    if a.output_listing:
        # read first, written after: `listing common > configs/common_defconfig` has the shell empty the file before it is read,
        # and the configuration written is then the base one's alone (which is how common_defconfig once lost its values)
        with open(a.output_listing + ".new", "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        os.replace(a.output_listing + ".new", a.output_listing)
        print("hakios-config: %s written" % rel(os.getcwd(), a.output_listing))
    else:
        sys.stdout.write(text)
    return 0


def cmd_complete(a):
    """complete NAME: a defconfig (NAME) must name every symbol, set or `is not set`; the ones it leaves out are said."""
    path = find_config(a.root, a.name if a.name.endswith("_defconfig") else a.name + "_defconfig")
    named = set()
    for line in open(path, encoding="utf-8"):
        m = re.match(r"(?:# )?CONFIG_([A-Z0-9_]+)(?:=| is not set)", line.strip())
        if m:
            named.add(m.group(1))
    common = os.path.basename(path) == "common_defconfig"
    missing = [s[0] for s in SYMBOLS if s[0] not in named and not (common and external(s[0]))]
    for name in missing[:30]:
        print("hakios-config: %s does not name CONFIG_%s" % (rel(a.root, path), name))
    if len(missing) > 30:
        print("hakios-config: ... and %d more" % (len(missing) - 30))
    if not missing:
        print("hakios-config: %s names all %d symbols" % (rel(a.root, path), len(SYMBOLS)))
    return 1 if missing else 0


def cmd_help(a):
    name = a.symbol.upper()
    name = name[len("CONFIG_"):] if name.startswith("CONFIG_") else name
    if name not in BY_NAME:
        close = difflib.get_close_matches(name, BY_NAME, n=3)
        print("no symbol CONFIG_%s%s" % (name, " (did you mean %s?)" % ", ".join("CONFIG_" + c for c in close) if close else ""))
        return 1
    _n, kind, arg, help_, needs, requires = BY_NAME[name]
    print("CONFIG_%s  (%s)" % (name, kind))
    print("  " + help_)
    if kind == "driver":
        d = DRIVER_BY_SYMBOL[name]
        print("  compiled into the kernel, not a boot-argument: XNU's option %s (%s)" % (d.symbol, d.where))
        print("  files: %s" % ", ".join(f for f, _t in d.files))
        print("  requires: %s" % (", ".join("CONFIG_HAKIOS_" + r for r in d.requires) or "nothing"))
        needed_by = [o.symbol for o in DRIVERS if d.name in o.requires]
        if needed_by:
            print("  required by: %s" % ", ".join("CONFIG_" + n for n in needed_by))
        print("  default: %s" % ("y (built in)" if d.default == "y" else "not set (left out)"))
    if arg:
        print("  boot-argument: %s=%s" % (arg, {"bool": "1", "hex": "0x...", "int": "N", "str": "...", "arg": "1, N or text", "args": "..."}[kind]))
    if needs:
        print("  needs firmware pulled from your phone: %s/" % needs)
    if requires:
        print("  requires: CONFIG_%s" % requires)
    return 0


# ---- the kernel tree: the drivers' choice where XNU's own build reads it ----------------------------------------------

FILES_BEGIN = "# HakiOS drivers: made by tools/hakios-config.py kernel from drivers/*/conf. Edit those, not these lines."
FILES_END = "# end of the HakiOS drivers"
GENERATED_IN_TREE = ("config/MASTER.hakios", "bsd/dev/hakios.drivers.h")     # made per configuration; never exported (xnu/export.sh)
USES_OPTION = re.compile(r"^\s*#\s*(?:if|ifdef|ifndef|elif)\b.*\bHAKIOS_[A-Z][A-Z0-9_]*", re.M)


def files_block(component):
    """The lines of bsd/conf/files or iokit/conf/files for every hakios file of that component: `standard`, or
    `optional hakios_<driver>` (and the file's own XNU options; config(8) builds a file whose options are all on)."""
    lines = [FILES_BEGIN]
    for path, tags, _sub, _help in STANDARD:
        if path.startswith(component + "/"):
            lines.append("%-40s %s" % (path, "optional " + " ".join(tags) if tags else "standard"))
    for d in DRIVERS:
        for path, tags in d.files:
            if path.startswith(component + "/"):
                lines.append("%-40s optional %s" % (path, " ".join([d.tag] + tags)))
    return lines + [FILES_END]


def put_block(text, block):
    """`text` (a conf/files) with its HakiOS block replaced by `block`; a tree from before there was a block has its loose
    hakios lines taken out and the block put where the first of them was."""
    lines = text.split("\n")
    if FILES_BEGIN in lines:
        start = lines.index(FILES_BEGIN)
        end = lines.index(FILES_END, start)
        return "\n".join(lines[:start] + block + lines[end + 1:])
    loose = [i for i, l in enumerate(lines) if re.match(r"(bsd/dev|iokit/Kernel)/hakios\.", l)]
    if not loose:
        raise ConfigError("no HakiOS lines in it to replace")
    kept = [l for i, l in enumerate(lines) if i not in set(loose)]
    at = loose[0]
    return "\n".join(kept[:at] + block + kept[at:])


def master_hakios(built, sources):
    """config/MASTER.hakios: the configuration's choice, in doconf's terms. HAKIOS_DRIVERS is a list of attributes that the
    kernel configurations of MASTER.arm64.bcm2837 include; an `options` line is taken when its attribute is in the list."""
    on = [d.tag for d in DRIVERS if built[d.symbol]]
    lines = ["#", "# The HakiOS drivers of this build, made by tools/hakios-config.py kernel from %s." % sources,
             "# Not part of the patch: xnu/export.sh leaves it out, and every `make kernel` makes it again.", "#",
             "#  HAKIOS_DRIVERS = [ %s ]" % " ".join(on), "#"]
    for d in DRIVERS:
        what = re.sub(r"[#<>]", "", d.help)        # doconf reads the last `# <attribute>` of a line: nothing in the comment may look like it
        lines.append("options\t\t%-24s # %-48s # <%s>" % (d.symbol, what[:48].rsplit(" ", 1)[0] if len(what) > 48 else what, d.tag))
    return "\n".join(lines) + "\n"


def drivers_header(built, sources):
    on = [d.name.lower() for d in DRIVERS if built[d.symbol]]
    return ("/* The drivers this kernel is built with: made by tools/hakios-config.py kernel from %s. Not part of the patch. */\n"
            "#define HAKIOS_DRIVERS_LIST     \"%s\"\n#define HAKIOS_DRIVERS_COUNT    %d\n#define HAKIOS_DRIVERS_CONFIG   \"%s\"\n" %
            (sources, " ".join(on), len(on), sources))


def write_if_changed(path, text):
    old = open(path, encoding="utf-8").read() if os.path.isfile(path) else None
    if old == text:
        return False
    with open(path + ".new", "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    os.replace(path + ".new", path)
    return True


def stale_objects(tree):
    """The objects of the files whose code asks about a driver (#if HAKIOS_*): XNU's build sees the option in the compiler's
    command line, not as a file the object depends on, so a changed choice would leave them as they were built."""
    stems = set()
    for top in ("bsd", "iokit", "osfmk", "libkern", "pexpert"):
        for d, _dirs, files in os.walk(os.path.join(tree, top)):
            for name in files:
                if name.endswith((".c", ".cpp", ".h")):
                    try:
                        text = open(os.path.join(d, name), encoding="utf-8", errors="replace").read()
                    except OSError:
                        continue
                    if "HAKIOS_" in text and USES_OPTION.search(text):
                        stems.add(os.path.splitext(name)[0])
    removed = 0
    headers = {s for s in stems if os.path.isfile(os.path.join(tree, "bsd", "sys", s + ".h"))}
    obj = os.path.join(tree, "BUILD", "obj")
    for d, _dirs, files in os.walk(obj):
        for name in files:
            stem, ext = os.path.splitext(name)
            if ext in (".o", ".cpo") and stem in stems:     # XNU names a C++ object .cpo
                os.remove(os.path.join(d, name))
                removed += 1
    if headers:
        # a header that tests an option (a stand-in for a driver left out) changes every object that includes it: those are
        # found by the dependency files the compiler wrote, which name the headers each object was built from
        for d, _dirs, files in os.walk(obj):
            for name in files:
                if name.endswith((".d", ".cpd")):
                    text = open(os.path.join(d, name), encoding="utf-8", errors="replace").read()
                    if any("/" + h + ".h" in text for h in headers):
                        o = os.path.join(d, name[:-2] + ".o" if name.endswith(".d") else name[:-4] + ".cpo")
                        if os.path.isfile(o):
                            os.remove(o)
                            removed += 1
    return removed


def cmd_kernel(a):
    """kernel [--tree DIR]: put the drivers' choice of the .config (every driver's default if there is no .config) into the
    kernel tree, where XNU's build reads it: config/MASTER.hakios, the HakiOS block of bsd/conf/files and iokit/conf/files,
    bsd/dev/hakios.drivers.h. Files are written only when they change, so an unchanged choice rebuilds nothing."""
    if DRIVERS_ERROR:
        raise ConfigError(DRIVERS_ERROR)
    tree = a.tree or os.environ.get("XNU_SRC") or os.path.join(a.root, "xnu-src", "xnu-8796.121.2")
    if not os.path.isfile(os.path.join(tree, "bsd", "conf", "files")):
        raise ConfigError("no kernel tree at %s (xnu/apply.sh makes it)" % tree)
    if os.path.isfile(a.config):
        config, sources = load(a.config), rel(a.root, a.config)
    else:
        config, sources = {}, "the drivers' defaults (no .config)"
    problems = check_drivers(config, {})
    if problems:
        report(problems)
        return 1
    built = drivers_built(config)
    changed = []
    for component in ("bsd", "iokit"):
        path = os.path.join(tree, component, "conf", "files")
        try:
            text = put_block(open(path, encoding="utf-8").read(), files_block(component))
        except ConfigError as e:
            raise ConfigError("%s/conf/files: %s" % (component, e))
        if write_if_changed(path, text):
            changed.append(component + "/conf/files")
    choice_changed = write_if_changed(os.path.join(tree, "config", "MASTER.hakios"), master_hakios(built, sources))
    if choice_changed:
        changed.append("config/MASTER.hakios")
    if write_if_changed(os.path.join(tree, "bsd", "dev", "hakios.drivers.h"), drivers_header(built, sources)):
        changed.append("bsd/dev/hakios.drivers.h")
    removed = stale_objects(tree) if choice_changed and os.path.isdir(os.path.join(tree, "BUILD", "obj")) else 0
    on = [d.name.lower() for d in DRIVERS if built[d.symbol]]
    off = [d.name.lower() for d in DRIVERS if not built[d.symbol]]
    print("hakios-config: kernel: %d of %d drivers built in, from %s%s" % (len(on), len(DRIVERS), sources,
          "; left out: " + " ".join(off) if off else ""))
    if changed:
        print("hakios-config: kernel: %s written%s" % (", ".join(changed), "; %d objects to be built again" % removed if removed else ""))
    return 0


def cmd_external(a):
    """external-files: "<file in a SoC's or device's repository>\t<path in the kernel tree>" for each file of their drivers, for
    xnu/apply.sh and xnu/sync-drivers.sh to copy into the tree and xnu/export.sh to leave where it is (it is not this repository's)."""
    if DRIVERS_ERROR:
        raise ConfigError(DRIVERS_ERROR)
    for d in DRIVERS:
        if d.origin:
            for path, _tags in d.files:
                print("%s\t%s" % (d.source(path), path))
    return 0


def main():
    # --root and --config are accepted before the command and after it
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--root", default=argparse.SUPPRESS, help="the top of the repository (tests use another)")
    common.add_argument("--config", default=argparse.SUPPRESS, help="the .config to use (default: .config at the top)")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter, parents=[common])
    sub = ap.add_subparsers(dest="command", required=True)
    p = sub.add_parser("defconfig", parents=[common], help="make .config from a defconfig and fragments")
    p.add_argument("name")
    p.add_argument("fragments", nargs="*")
    p.add_argument("-o", "--output")
    p.set_defaults(fn=cmd_defconfig)
    p = sub.add_parser("merge", parents=[common], help="put fragments over the .config")
    p.add_argument("fragments", nargs="+")
    p.set_defaults(fn=cmd_merge)
    p = sub.add_parser("args", parents=[common], help="the kernel's boot-arguments")
    p.add_argument("--force", action="store_true", help="print them even if the check finds a problem")
    p.add_argument("--defconfig", help="the arguments of this defconfig (and --fragment) instead of the .config's")
    p.add_argument("--fragment", action="append", default=[], help="a fragment over it; more than one is allowed")
    p.set_defaults(fn=cmd_args)
    for name, fn, text in (("check", cmd_check, "validate the .config"), ("show", cmd_show, "say what the .config is"),
                           ("needs", cmd_needs, "which firmware the .config wants, and whether it is there"),
                           ("savedefconfig", cmd_savedefconfig, "the .config as a minimal defconfig"),
                           ("list", cmd_list, "the defconfigs and fragments in configs/")):
        sub.add_parser(name, parents=[common], help=text).set_defaults(fn=fn)
    p = sub.add_parser("listing", parents=[common], help="the documented defconfig of NAME, every symbol, on stdout or into -o FILE")
    p.add_argument("name")
    p.add_argument("fragments", nargs="*")
    p.add_argument("-o", dest="output_listing", help="write it here (safe for the file it is made from, as `>` is not)")
    p.set_defaults(fn=cmd_listing)
    p = sub.add_parser("complete", parents=[common], help="a defconfig must name every symbol")
    p.add_argument("name")
    p.set_defaults(fn=cmd_complete)
    p = sub.add_parser("kernel", parents=[common], help="put the .config's choice of drivers into the kernel tree (make kernel runs it)")
    p.add_argument("--tree", help="the kernel tree (default: $XNU_SRC, else xnu-src/xnu-8796.121.2)")
    p.set_defaults(fn=cmd_kernel)
    sub.add_parser("external-files", parents=[common], help="the driver files of SoC and device repositories, and their place in the tree").set_defaults(fn=cmd_external)
    p = sub.add_parser("help", parents=[common], help="what a symbol means")
    p.add_argument("symbol")
    p.set_defaults(fn=cmd_help)
    a = ap.parse_args()
    a.root = os.path.abspath(getattr(a, "root", ROOT))
    a.config = os.path.abspath(getattr(a, "config", os.path.join(a.root, ".config")))
    if getattr(a, "output", None) is None:
        a.output = a.config
    if DRIVERS_ERROR:
        # a conf file that does not read leaves the drivers unknown, and every answer about a configuration would be wrong
        print("hakios-config: %s" % DRIVERS_ERROR, file=sys.stderr)
        sys.exit(1)
    try:
        sys.exit(a.fn(a))
    except ConfigError as e:
        print("hakios-config: %s" % e, file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
