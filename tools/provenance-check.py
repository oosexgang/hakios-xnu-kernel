#!/usr/bin/env python3
"""Every binary file, and every firmware blob hidden in a source file, must say where it came from.

usage: tools/provenance-check.py [--report] [--root DIR]

The rule of this repository is that it holds only what its authors may redistribute: the source of
the project, and files built from that source. Nothing of Apple's closed software (Darwin, its images,
fonts, pictures), nothing of a phone vendor's firmware. A rule like that holds only if a machine
checks it on every push, so this does. It reads the files git tracks and fails when it finds

  - a binary file (an executable, an image, a font, a firmware image, an archive ...) that no entry of
    tools/provenance.allow explains;
  - a source file that is mostly a table of bytes (firmware compiled into a header) with no entry;
  - a file of a kind the project never carries (screenshots, disk images, IPSW or IPA files,
    cryptexes, the dyld shared cache, fonts), with no entry;
  - a file with an entry whose status is `vendor` (proprietary, not cleared for redistribution);
  - a script that would write to the phone's flash (fastboot flash, erase, format, oem unlock, dd to a
    block device): the project boots from RAM and promises not to touch the phone's storage.

tools/provenance.allow has one line per rule:   STATUS  GLOB  reason
  open     redistributable: source-built from this tree, or under an open licence, with the licence named
  pending  known and not settled; the check passes with a warning, and the entry says who must decide
  vendor   proprietary; the check fails until the file is removed from the tree
  flash    a script that is allowed to contain a command that writes to the phone's storage, because a
           person runs it by hand for a one-time job; named here so that it is a decision, not an accident
The first line whose glob matches a path decides. A path no line matches is unexplained.

--report prints the explained files as a Markdown table (docs/provenance.md is made with it).
Exit status: 0 clean, 1 something to fix.
"""
import argparse
import ast
import fnmatch
import os
import re
import subprocess
import sys

# kinds the project never carries; each needs an entry to be allowed at all
NEVER = re.compile(r"\.(png|jpe?g|gif|heic|webp|bmp|tiff?|mov|mp4|m4v|m4a|mp3|aiff?|caf|ttf|ttc|otf|woff2?|"
                   r"ipsw|ipa|dmg|img4|im4p|car|nib|tbd|mobileconfig|strings|zip|tar|tgz|gz|xz|7z)$", re.I)
NEVER_PATH = re.compile(r"cryptex|dyld_shared_cache|\.ipsw|SpringBoard\.app|(^|/)System/Library/", re.I)
# a source file that is mostly 0x.., 0x.., ... lines: firmware compiled into a header
HEXLINE = re.compile(r"^\s*(0x[0-9a-fA-F]{2},\s*){8,}")
HEX_LINES_LIMIT = 200
# commands that write to a phone's storage; the project's scripts must not contain them
FLASHY = re.compile(r"fastboot\s+(flash|erase|format|flashing|oem\s+unlock|oem\s+lock|set_active)\b|"
                    r"\bdd\s+[^|;&\n]*of=/dev/(block|mmcblk|sd[a-z]|nvme)|\bmke2fs\b.*\b/dev/|"
                    r"\badb\s+(shell\s+)?(su\s+-c\s+)?['\"]?(flash_image|erase_image|blkdiscard)")
# ... and in C: the loader's write to a block device, and the kernel's UFS driver's. Both are limited in code to
# the partitions named in the `flash` lines of tools/provenance.allow, and both are declared there.
FLASHY_C = re.compile(r"\bwrite_blocks\s*\(|\bBLOCK_OP_WRITE\b")
C_WRITERS = re.compile(r"^(xnuloader/[^/]+|drivers/storage/hakios\.ufs[^/]*)\.c$")
SCRIPT_DIRS = ("tools/", "devices/", "qemu/", "sysimage/", "xnuloader/", "userland/build.sh")
SCRIPT_FILES = ("xl", "Makefile", "xnu-build.sh")
# this file, its test and the doctor mention these commands in order to look for them (the test has them as data)
SKIP_SCAN = {"tools/provenance-check.py", "tools/provenance-test.py", "tools/doctor.sh"}


def tracked(root):
    out = subprocess.run(["git", "ls-files", "-z"], cwd=root, capture_output=True, check=True).stdout
    return [p.decode() for p in out.split(b"\0") if p]


def load_rules(root):
    rules = []
    path = os.path.join(root, "tools", "provenance.allow")
    if not os.path.exists(path):
        return rules
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        line = line.rstrip("\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split(None, 2)
        if len(parts) < 3 or parts[0] not in ("open", "pending", "vendor", "flash"):
            sys.exit("provenance.allow:%d: want  open|pending|vendor|flash  GLOB  reason  (got: %s)" % (n, line))
        rules.append((parts[0], parts[1], parts[2], n))
    return rules


def match(rules, path, flash=False):
    """The first rule for `path`: among the `flash` rules when asked for those, among the others if not."""
    for status, glob, reason, _ in rules:
        if (status == "flash") == flash and fnmatch.fnmatchcase(path, glob):
            return status, reason
    return None


def kind_of(head):
    if head[:4] in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe"):
        return "Mach-O"
    if head[:2] == b"MZ":
        return "PE/EFI"
    if head[:4] == b"\x7fELF":
        return "ELF"
    if head[:4] == b"\xd0\x0d\xfe\xed":
        return "device tree"
    if head[:8] == b"\x89PNG\r\n\x1a\n":
        return "PNG"
    if head[:3] == b"\xff\xd8\xff":
        return "JPEG"
    if head[:4] == b"NXSB" or head[32:36] == b"NXSB":
        return "APFS"
    return "binary"


def without_python_docstrings(text):
    """Mask actual docstrings, retaining executable text and diagnostic line numbers."""
    try:
        tree = ast.parse(text)
    except (SyntaxError, ValueError, RecursionError):
        return text                         # malformed Python must not hide a command
    lines = text.split("\n")
    for owner in ast.walk(tree):
        if not isinstance(owner, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)) or not owner.body:
            continue
        doc = owner.body[0]
        if not (isinstance(doc, ast.Expr) and isinstance(doc.value, ast.Constant) and isinstance(doc.value.value, str)):
            continue
        for i in range(doc.lineno - 1, doc.end_lineno):
            # AST columns are UTF-8 byte offsets, not Python string indices.
            # Mask only the string: code after a same-line docstring is still checked.
            line = lines[i].encode("utf-8")
            first = doc.col_offset if i == doc.lineno - 1 else 0
            last = doc.end_col_offset if i == doc.end_lineno - 1 else len(line)
            lines[i] = (line[:first] + b" " * (last - first) + line[last:]).decode("utf-8")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    rules = load_rules(root)
    used = set()
    problems, warnings, explained = [], [], []

    files = tracked(root)
    for path in files:
        full = os.path.join(root, path)
        if not os.path.isfile(full) or os.path.islink(full):
            continue
        with open(full, "rb") as f:
            head = f.read(8192)
        binary = b"\0" in head
        why = None
        if binary:
            why = kind_of(head)
        elif NEVER.search(path) or NEVER_PATH.search(path):
            why = "a kind of file the project does not carry"
        elif path.endswith((".h", ".c", ".inc", ".S")):
            with open(full, "r", errors="replace") as f:
                hexlines = sum(1 for line in f if HEXLINE.match(line))
            if hexlines >= HEX_LINES_LIMIT:
                why = "firmware or data compiled into source (%d lines of bytes)" % hexlines
        if why is not None:
            hit = match(rules, path)
            if hit is None:
                problems.append("%s: %s, and no line of tools/provenance.allow explains it" % (path, why))
                continue
            status, reason = hit
            used.add(path)
            explained.append((path, why, status, reason, os.path.getsize(full)))
            if status == "vendor":
                problems.append("%s: %s -- marked vendor (proprietary): remove it from the tree" % (path, why))
            elif status == "pending":
                warnings.append("%s: %s -- pending: %s" % (path, why, reason))
        # scripts, and C code of the loader and the UFS driver, that would write to the phone's storage
        scan_c = bool(C_WRITERS.match(path))
        is_script = (path in SCRIPT_FILES or path.startswith(SCRIPT_DIRS)) and path.endswith((".sh", ".py", "xl", "Makefile"))
        if path not in SKIP_SCAN and (scan_c or is_script):
            try:
                text = open(full, "r", errors="replace").read()
            except OSError:
                continue
            if path.endswith(".py"):
                text = without_python_docstrings(text)
            for n, line in enumerate(text.splitlines(), 1):
                code = line.split("#", 1)[0] if path.endswith((".sh", "xl", "Makefile")) else line
                found = (FLASHY_C if scan_c else FLASHY).search(code)
                if not found:
                    continue
                hit = match(rules, path, flash=True)
                if hit is None:
                    problems.append("%s:%d: writes to the phone's storage (%s): the project boots from RAM; a "
                                    "deliberate exception needs a `flash` line in tools/provenance.allow"
                                    % (path, n, found.group(0).strip()))
                elif path not in used:
                    used.add(path)
                    explained.append((path, "writes to the phone's storage, by a person's hand", "flash", hit[1],
                                      os.path.getsize(full)))
                    warnings.append("%s: declared exception to \"nothing is written to the phone\": %s"
                                    % (path, hit[1]))
                break

    # a declared writer is a statement about a file, so it is listed even if no pattern fired in it today
    for status, glob, reason, _ in rules:
        if status == "flash":
            for p in files:
                if fnmatch.fnmatchcase(p, glob) and p not in used:
                    used.add(p)
                    explained.append((p, "declared writer to the phone's storage", "flash", reason,
                                      os.path.getsize(os.path.join(root, p))))
    # an entry that matches nothing has gone stale: a rule for a file that left the tree is clutter that
    # would explain a file that comes back
    # (a `vendor` line is a tripwire: it stays after the files are gone, so that putting them back fails)
    stale = [(s, g, n) for s, g, _, n in rules if s != "vendor" and not any(fnmatch.fnmatchcase(p, g) for p in files)]
    for s, g, n in stale:
        warnings.append("tools/provenance.allow:%d: '%s' matches no tracked file (stale)" % (n, g))

    if args.report:
        print("| File | What | Status | Why it may be here | Bytes |")
        print("|---|---|---|---|---:|")
        for path, why, status, reason, size in sorted(explained):
            print("| `%s` | %s | %s | %s | %d |" % (path, why, status, reason, size))
        return 0

    for w in warnings:
        print("warn: " + w)
    for p in problems:
        print("FAIL: " + p)
    print("provenance: %d tracked files, %d explained binaries or blobs, %d warnings, %d problems"
          % (len(files), len(explained), len(warnings), len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
