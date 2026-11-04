#!/usr/bin/env python3
"""The catalogue of every boot-argument that the kernel, the loader and user space read.

usage: tools/boot-args.py [--markdown | --lst | --symbols | --check] [--root DIR]

  (none)      the catalogue, as text, grouped by driver
  --markdown  the catalogue as docs/boot-args.md has it
  --lst       the names, as configs/boot-args.lst has them (what tools/hakios-config.py checks CONFIG_BOOTARGS against)
  --symbols   a CONFIG_ symbol for each argument that has none in the table of tools/hakios-config.py, as configs/symbols.tsv has them
              (name, kind, argument, group, layers, files, one-line meaning where the source has a documentation line for it)
  --check     exit 1 if docs/boot-args.md, configs/boot-args.lst or configs/symbols.tsv is out of date with the source (CI runs this)

Why it exists. The kernel of this port is configured on its command line, and every driver reads its own
arguments: 194 in the kernel's files, 14 in the loader, 26 in user space. A handful of them are stable switches
that tools/hakios-config.py describes (CONFIG_GPU, CONFIG_WIFI ...). The rest are the knobs of bring-up: a driver's
steps, pauses, traces, test modes. Nobody can keep a hand-written list of two hundred of them true, so this finds
them in the source: a name that a driver reads is in the catalogue, one that nobody reads is not, and the check
fails the build when the two files disagree with the code. It says WHICH driver reads an argument and WHERE; what
an argument means is in the head of that file, and for the stable ones in tools/hakios-config.py (`help SYMBOL`).

What is looked for:
  the kernel   PE_parse_boot_argn("name" ...) and PE_parse_boot_arg_str("name" ...), in xnu/files/ and in the lines
               xnu/hakios.patch adds to Apple's files
  the loader   starts_with(c, "name=...") on the payload's command line, in xnuloader/
  user space   a name given to strstr()/strncmp() on sysctl kern.bootargs, in userland/src/ (seen: sb_delay=, touch_lat=1,
               ca_vbl=1 ...), or to the Metal plug-in's helper hm_boot_arg_set("metal_m2v=1")
"""
import argparse
import collections
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

KERNEL = re.compile(r'PE_parse_boot_arg(?:n|_str)?\(\s*"([A-Za-z0-9_.\-]+)"')
# XNU's TUNABLE(type, variable, "boot-arg name", default) reads the argument at start-up, in the files of the display and GPU
# drivers (C++) and in the sandbox stand-in: static TUNABLE(uint32_t, gOnTheBeat, "fb_beat", 1). Also TUNABLE_WRITEABLE and
# TUNABLE_STR. (TUNABLE_DT, whose third argument is a device-tree path with slashes, does not match.)
STRING = re.compile(r'PE_parse_boot_arg_str\(\s*"([A-Za-z0-9_.\-]+)"')     # an argument read as text
TUNABLE = re.compile(r'\bTUNABLE\w*\(\s*[^,()]+,\s*[^,()]+,\s*"([A-Za-z0-9_.\-]+)"')
# Three drivers read their options through a small function that is given the name as a string, and the name is the
# literal at the call: option("modem_again") in hakios.mpss.c and hakios.adsp.c (a static function in each), and for
# Wi-Fi hakios_wifi_option("wifi_keep") with lx_os_option("wifi_irq") of the Linux layer in wifi/lx/src, which calls it.
# A scan for PE_parse_boot_argn("...") alone misses all of them (about forty names), so these are scanned too.
WRAPPED = re.compile(r'\b(?:hakios_wifi_option|lx_os_option)\(\s*"([a-z][a-z0-9_]*)"')
OPTION_DEF = re.compile(r'^option\(const char \*name\)', re.M)
OPTION_CALL = re.compile(r'\boption\(\s*"([a-z][a-z0-9_]*)"')
# ... and user space has its own: boot_arg("var_store") in init.c (a static function that reads sysctl kern.bootargs)
BOOT_ARG_DEF = re.compile(r'^boot_arg\(const char \*name\)', re.M)
BOOT_ARG_CALL = re.compile(r'\bboot_arg\(\s*"([a-z][a-z0-9_]*)"')
LOADER = re.compile(r'starts_with\((?:c|cmd|p|a|s), "([a-z][a-z0-9_.]*)=')
# user space reads sysctl kern.bootargs into a buffer called arguments, args or argument, and looks in it with strstr:
# strstr(arguments, "gltrace_at="), strstr(args, "touch_lat=1"): the name is what stands before the =
USER = re.compile(r'strstr\(\s*(?:arguments|args|argument)\s*,\s*"([a-z][a-z0-9_]*)=')
# ... and the Metal plug-in has a helper that is given the whole word: hm_boot_arg_set("metal_m2v=1") in HakiosMetalCA.m
HM_ARG = re.compile(r'\bhm_boot_arg_set\(\s*"([a-z][a-z0-9_]*)=')

# the group an argument is listed under: by the file that reads it (the first match decides)
GROUPS = [
    ("GPU and display", r"hakios\.(adreno|gpu)\.|MiniIO(Surface|MobileFramebuffer)|gl_osmesa|eagl\.c"),
    ("Security stand-ins", r"kern_minimac|miniamfi"),
    ("Wi-Fi", r"hakios\.(wifi|wlfw|lx|sta|apple80211)\.|^lx_[a-z0-9]+\.c$"),     # wifi/lx/src/lx_*.c: the Linux layer
    ("Cellular", r"hakios\.(mpss|ipa|cell|nas|qmi)\."),
    ("Audio and battery", r"hakios\.(adsp|audio|sound|speaker|mic|virtio_sound|battery|battguard)\."),
    ("Cameras", r"hakios\.camera\."),
    ("Touch and buttons", r"hakios\.(goodix|input|buttons|virtio_input)\."),
    ("Storage and APFS", r"hakios\.(ufs|ufshc|virtio_blk|sparse|apfs_check)\.|apfs_vfsops\."),
    ("Network", r"hakios\.(net|virtio_net)\."),
    ("Haptics and torch", r"hakios\.(haptic|torch)\."),
    ("Chip, thermal and PMIC", r"hakios\.(socinfo|thermal|pmic)\."),
    ("Version", r"hakios\.version\."),
    ("CPU, memory and boot", r"hakios\.cpufreq\.|GenericARMPE|loader_return|\(patch\)"),
    ("Debugging", r"user_exception_trace"),
    ("Loader", r"loader\.c"),
    ("User space", r"."),
]

# who SETS an argument when it is not the person: verified in the loader's command-line assembly and in xl
SET_BY = {
    "ufs_shared_pa": "the loader (the address of its block-service page)",
    "battery_shared_pa": "the loader (the address of its charger page)",
    "hakios_kver": "the loader (from the payload's kernel.ver)",
    "hv_wdt_pet": "the loader, when it has armed the watchdog (the runner also passes it)",
    "epoch": "xl (the host's clock)",
    "loader_return_after": "xl (XL_SECS)",
    "sb_delay": "xl (XL_SB_DELAY)",
    # wifi=1 is "all of it, by itself": hakios_wifi_option() answers these as if they were given (wifi_lx=2 wifi_smmu=2
    # wifi_station=1 wifi_keep=1 wifi_irq=1 wifi_ba=1 wifi_wide=1) unless they are given otherwise
    "wifi_lx": "implied by wifi=1 (as 2)",
    "wifi_smmu": "implied by wifi=1 (as 2)",
    "wifi_station": "implied by wifi=1",
    "wifi_keep": "implied by wifi=1",
    "wifi_irq": "implied by wifi=1",
    "wifi_ba": "implied by wifi=1",
    "wifi_wide": "implied by wifi=1",
}


def tracked(root):
    out = subprocess.run(["git", "ls-files", "-z"], cwd=root, capture_output=True, check=True).stdout
    return [p.decode() for p in out.split(b"\0") if p]


def scan(root, keep=None):
    """{name: {"kernel": {files}, "loader": {files}, "user": {files}, "str": {files}}} found in the tracked source.
    `keep`, if given, is filled with {file name: text} of every source file read (the descriptions are taken from it)."""
    found = collections.defaultdict(lambda: {"kernel": set(), "loader": set(), "user": set(), "str": set()})
    for f in tracked(root):
        if not f.endswith((".c", ".cpp", ".m", ".h", ".patch")):
            continue
        try:
            text = open(os.path.join(root, f), encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        base = os.path.basename(f)
        if keep is not None and f != "xnu/hakios.patch":
            keep[base] = text
        if f == "xnu/hakios.patch":
            text = "\n".join(l[1:] for l in text.splitlines() if l.startswith("+") and not l.startswith("+++"))
            base = "(patch)"
        # the kernel's side: where the drivers are kept (xnu/files/ until they moved to drivers/<subsystem>/) and the Linux layer
        if f.startswith(("xnu/", "drivers/", "wifi/lx/src/")):
            for m in KERNEL.finditer(text):
                found[m.group(1)]["kernel"].add(base)
            for m in STRING.finditer(text):
                found[m.group(1)]["str"].add(base)
            for m in WRAPPED.finditer(text):
                found[m.group(1)]["kernel"].add(base)
            for m in TUNABLE.finditer(text):
                found[m.group(1)]["kernel"].add(base)
            if OPTION_DEF.search(text):             # a file that has its own option(name): its callers name arguments
                for m in OPTION_CALL.finditer(text):
                    found[m.group(1)]["kernel"].add(base)
        if f.startswith("xnuloader/") and f.endswith(".c"):
            for m in LOADER.finditer(text):
                found[m.group(1)]["loader"].add(base)
        if f.startswith("userland/src/"):
            for m in USER.finditer(text):
                found[m.group(1)]["user"].add(base)
            for m in HM_ARG.finditer(text):
                found[m.group(1)]["user"].add(base)
            if BOOT_ARG_DEF.search(text):
                for m in BOOT_ARG_CALL.finditer(text):
                    found[m.group(1)]["user"].add(base)
    return found


def group_of(files):
    for group, pattern in GROUPS:
        if any(re.search(pattern, f) for f in sorted(files)):
            return group
    return "User space"


def symbols(root):
    """{boot-arg: CONFIG_SYMBOL} of the table in tools/hakios-config.py."""
    out = {}
    path = os.path.join(root, "tools", "hakios-config.py")
    if os.path.isfile(path):
        for line in open(path, encoding="utf-8"):
            m = re.match(r'\s+\("([A-Z0-9_]+)", "(?:bool|int|hex|str)", "([a-z0-9_]+)"', line)
            if m:
                out[m.group(2)] = "CONFIG_" + m.group(1)
    return out


# ---- symbols: one CONFIG_ symbol for each argument the table of tools/hakios-config.py does not already have -----------------

# arguments that the loader or the runner sets, not the person (SET_BY, except the ones wifi=1 implies): no symbol
def given_by_tools(name):
    why = SET_BY.get(name, "")
    return bool(why) and not why.startswith("implied")


# arguments whose value is an address or a window: 0x... (a decimal one would be a mistake)
HEX_NAME = re.compile(r"(_pa|_base|_buffer|_addr|_mem)$|^soc_(?!ipa_fw|pas_|ipa_stream)")

COMMENT_LINE = re.compile(r"^\s*(?:/\*+|\*+/?|//+)\s?(.*)$")


def _clean(text):
    return re.sub(r"\s+", " ", text).strip(" */-:;,")


def _sentence(text, limit=150):
    text = _clean(text)
    m = re.search(r"(?<=[a-z0-9\)])\.(?:\s|$)", text)
    if m and m.start() > 30:
        text = text[:m.start()]
    if len(text) > limit:
        text = text[:limit].rsplit(" ", 1)[0]
    return text


# a description that ends like this was cut in the middle of a sentence: it is not used
UNFINISHED = re.compile(r"(,|\band|\bor|\bwhich is what|\bthe|\bof|\bto|\bwith|\bin|\bfor|\bis|\bas|\ba|\bby|\bwhere|\bthat|\bnot|\bit|\(\w*)$")


def describe(name, files, texts):
    """The one-line meaning of an argument, when a file that reads it has a documentation line for it: a comment that starts
    with `name=value` (or just the name) and then, after two spaces or a colon or a dash, says what it does. "" if not."""
    pattern = re.compile(r"^(?:boot-?args?[:\s]+)?(?:the )?" + re.escape(name) + r"(?:=[^\s:]*)?(?:\s{2,}|\s*:\s+|\s+--?\s+)(?:[-:]\s*)?"
                         r"([A-Za-z(].{9,})$", re.I)
    for f in sorted(files):
        lines = texts.get(f, "").splitlines()
        for i, line in enumerate(lines):
            m = COMMENT_LINE.match(line)
            hit = pattern.match(m.group(1).strip()) if m else None
            if not hit:
                continue
            text = hit.group(1)
            if i + 1 < len(lines) and len(_clean(text)) < 90 and not _clean(text).endswith("."):
                nxt = COMMENT_LINE.match(lines[i + 1])
                if nxt and nxt.group(1).strip() and not re.match(r"^[a-z0-9_]+(=|\s{2,})", nxt.group(1).strip()):
                    text += " " + nxt.group(1)
            text = _sentence(text)
            if len(text) >= 15 and not UNFINISHED.search(text) and text.count("(") == text.count(")") and not re.match(r"(and|or|then|but)", text):
                return text
    return ""


def sym_name(arg):
    return re.sub(r"[^A-Z0-9]", "_", arg.upper())


def symbol_rows(found, texts, curated):
    """[(NAME, kind, arg, group, layers, files, description)] for every argument without a symbol of its own in the table of
    tools/hakios-config.py. kind is "arg": y (= 1), a number, or a quoted string; "hex" for an address."""
    rows = []
    for arg, d in sorted(found.items()):
        if arg in curated or given_by_tools(arg):
            continue
        files = sorted(d["kernel"] | d["loader"] | d["user"])
        kind = "hex" if HEX_NAME.search(arg) else "arg"
        rows.append((sym_name(arg), kind, arg, group_of(d["kernel"] | d["loader"] | d["user"]), layers(d), ",".join(files),
                     describe(arg, d["kernel"] | d["loader"] | d["user"], texts)))
    return rows


def tsv(rows):
    return "".join("\t".join(r) + "\n" for r in rows)


def layers(d):
    return "".join(letter for key, letter in (("kernel", "k"), ("loader", "l"), ("user", "u")) if d[key])


def lst(found):
    return "".join("%s\t%s\n" % (name, layers(d)) for name, d in sorted(found.items()))


def markdown(found, config):
    groups = collections.defaultdict(list)
    for name, d in found.items():
        groups[group_of(d["kernel"] | d["loader"] | d["user"])].append((name, d))
    lines = [
        "# Boot-arguments",
        "",
        "Made by `tools/boot-args.py --markdown`; CI fails if it is out of date with the source (`tools/boot-args.py --check`).",
        "",
        "Every argument that the kernel, the loader or user space of this port reads, and which file reads it. **%d arguments**: "
        "%d read by the kernel, %d by the loader, %d by user space (one is read in more than one place)." % (
            len(found), sum(1 for d in found.values() if d["kernel"]), sum(1 for d in found.values() if d["loader"]),
            sum(1 for d in found.values() if d["user"])),
        "",
        "This is a catalogue of what exists and who reads it, not a manual. What an argument means is in the head of the file "
        "that reads it. The stable switches have a symbol in the configuration (`tools/hakios-config.py help SYMBOL`, "
        "`docs/configs.md`); the rest are the knobs of bring-up (a driver's steps, pauses, traces, test modes) and can change "
        "with the driver. Any of them can be given in a configuration with `CONFIG_BOOTARGS=\"name=value ...\"`, which checks "
        "that each name is in this list. What the arguments of the two Metal paths do: "
        "[Metal on the Adreno](reference/metal.md#boot-arguments).",
        "",
        "Layers: **K** the kernel, **L** the loader, **U** user space (through `sysctl kern.bootargs`).",
        "",
    ]
    for group in [g for g, _ in GROUPS if g in groups]:
        rows = sorted(groups[group])
        lines += ["## %s (%d)" % (group, len(rows)), "", "| Argument | Layer | Read by | Configuration | Set by |", "|---|---|---|---|---|"]
        for name, d in rows:
            files = sorted(d["kernel"] | d["loader"] | d["user"])
            lines.append("| `%s` | %s | %s | %s | %s |" % (
                name, "".join(x.upper() for x in layers(d)), ", ".join("`%s`" % f for f in files),
                "`%s`" % config[name] if name in config else "", SET_BY.get(name, "")))
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    what = ap.add_mutually_exclusive_group()
    what.add_argument("--markdown", action="store_true")
    what.add_argument("--lst", action="store_true")
    what.add_argument("--symbols", action="store_true")
    what.add_argument("--check", action="store_true")
    ap.add_argument("--root", default=ROOT)
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    texts = {}
    found = scan(root, keep=texts)
    config = symbols(root)
    rows = symbol_rows(found, texts, set(config))
    for row in rows:
        config[row[2]] = "CONFIG_" + row[0]

    if args.markdown:
        sys.stdout.write(markdown(found, config))
    elif args.symbols:
        sys.stdout.write(tsv(rows))
    elif args.lst:
        sys.stdout.write(lst(found))
    elif args.check:
        bad = 0
        for rel, want in (("docs/boot-args.md", markdown(found, config)), ("configs/boot-args.lst", lst(found)),
                          ("configs/symbols.tsv", tsv(rows))):
            path = os.path.join(root, rel)
            have = open(path, encoding="utf-8", newline="").read().replace("\r\n", "\n") if os.path.isfile(path) else None
            if have != want:
                bad += 1
                print("boot-args: %s is %s: run  tools/boot-args.py --%s > %s" % (
                    rel, "missing" if have is None else "out of date with the source", "markdown" if rel.endswith(".md") else "symbols" if rel.endswith(".tsv") else "lst", rel))
        unknown = sorted(a for a in config if a not in found)
        for a in unknown:
            bad += 1
            print("boot-args: the configuration table names %s, which no driver reads" % a)
        if not bad:
            print("boot-args: %d arguments; docs/boot-args.md and configs/boot-args.lst are current; every configuration symbol "
                  "is an argument that is read" % len(found))
        sys.exit(1 if bad else 0)
    else:
        by = collections.defaultdict(list)
        for name, d in found.items():
            by[group_of(d["kernel"] | d["loader"] | d["user"])].append((name, d))
        for group in [g for g, _ in GROUPS if g in by]:
            print("%s (%d)" % (group, len(by[group])))
            for name, d in sorted(by[group]):
                print("  %-26s %-3s %s" % (name, layers(d).upper(), ", ".join(sorted(d["kernel"] | d["loader"] | d["user"]))))


if __name__ == "__main__":
    main()
