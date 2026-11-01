#!/usr/bin/env python3
"""tools/mkesp.py: the tree it lays out, and what it refuses before a phone would."""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MKESP = os.path.join(HERE, "mkesp.py")
MKPAYLOAD = os.path.join(ROOT, "xnuloader", "mkpayload.py")
failures = []


def check(label, ok, detail=""):
    print("%s %s" % ("ok  " if ok else "FAIL", label))
    if not ok:
        failures.append(label)
        if detail:
            print("     " + str(detail).strip().replace("\n", "\n     ")[:400])


def run(*args):
    return subprocess.run([sys.executable, MKESP, *args], capture_output=True, text=True)


work = tempfile.mkdtemp(prefix="mkesp-test-")
try:
    loader = os.path.join(work, "loader.efi")
    open(loader, "wb").write(b"MZ" + b"\0" * 62)
    kernel = os.path.join(work, "kernel")
    open(kernel, "wb").write(b"\xcf\xfa\xed\xfe" + b"k" * 100)
    ramdisk = os.path.join(work, "ramdisk")
    open(ramdisk, "wb").write(b"r" * 300)

    def payload(name, *extra):
        path = os.path.join(work, name)
        subprocess.run([sys.executable, MKPAYLOAD, "-o", path, "kernel=" + kernel, "ramdisk=" + ramdisk, "--cmdline", "cpus=1", *extra],
                       check=True, capture_output=True)
        return path

    good = payload("good.bin", "--kernel-version", "2.1.13-hakios-darwin22.5-sc7280-gabc12345")
    out = os.path.join(work, "esp")
    r = run(out, good, "--loader", loader)
    check("a good payload: exit 0", r.returncode == 0, r.stdout + r.stderr)
    check("the layout is the one the loader looks for",
          sorted(os.path.relpath(os.path.join(d, f), out).replace(os.sep, "/") for d, _, fs in os.walk(out) for f in fs) ==
          ["EFI/BOOT/BOOTAA64.EFI", "EFI/hakios/kernel.ver", "EFI/hakios/payload.bin"])
    check("the payload is copied byte for byte", open(os.path.join(out, "EFI/hakios/payload.bin"), "rb").read() == open(good, "rb").read())
    check("the loader is copied byte for byte", open(os.path.join(out, "EFI/BOOT/BOOTAA64.EFI"), "rb").read() == open(loader, "rb").read())
    check("kernel.ver is text, with the version", open(os.path.join(out, "EFI/hakios/kernel.ver")).read() == "2.1.13-hakios-darwin22.5-sc7280-gabc12345\n")
    check("it says what the payload holds", "kernel (104)" in r.stdout and "ramdisk (300)" in r.stdout and "kernel.ver=2.1.13" in r.stdout, r.stdout)

    r = run(out, good, "--loader", loader)
    check("a directory that is not empty is refused", r.returncode != 0 and "not empty" in r.stderr, r.stderr)

    nover = payload("nover.bin", "--no-version")
    r = run(os.path.join(work, "esp2"), nover, "--loader", loader)
    check("a payload without kernel.ver: fine, and says unknown", r.returncode == 0 and "unknown" in r.stdout, r.stdout + r.stderr)

    junk = os.path.join(work, "junk.bin")
    open(junk, "wb").write(os.urandom(4096))
    r = run(os.path.join(work, "esp3"), junk, "--loader", loader)
    check("a file that is not a payload is refused", r.returncode != 0 and "XNUP" in r.stderr, r.stderr)
    check("... and nothing is made", not os.path.exists(os.path.join(work, "esp3")))

    nokernel = os.path.join(work, "nokernel.bin")
    body = struct.pack("<16sII", b"cmdline", 8, 0) + b"cpus=1\0\0" + b"\0" * 8 + b"\0" * 24
    open(nokernel, "wb").write(struct.pack("<II", 0x50554E58, len(body)) + body)
    r = run(os.path.join(work, "esp4"), nokernel, "--loader", loader)
    check("a payload with no kernel is refused", r.returncode != 0 and "no kernel" in r.stderr, r.stderr)

    def entry(name, data):
        return struct.pack("<16sII", name, len(data), 0) + data + b"\0" * (-len(data) % 16)

    def packed(body, size=None):
        return struct.pack("<II", 0x50554E58, len(body) if size is None else size) + body

    end = b"\0" * 24
    kentry = entry(b"kernel", b"k" * 16)
    malformed = [
        ("missing-kernel-bytes", packed(struct.pack("<16sII", b"kernel", 4096, 0)), "extends past"),
        ("empty-kernel", packed(entry(b"kernel", b"") + end), "no kernel data"),
        ("truncated-body", packed(kentry + end, len(kentry + end) + 1), "truncated payload"),
        ("entry-crosses-body", packed(kentry + end, 24), "extends past"),
        ("truncated-padding", packed(struct.pack("<16sII", b"kernel", 1, 0) + b"k"), "truncated padding"),
        ("partial-entry-header", packed(kentry + b"\0" * 23), "entry header"),
        ("missing-end-entry", packed(kentry), "no end entry"),
        ("truncated-ramdisk", packed(kentry + struct.pack("<16sII", b"ramdisk", 4096, 0) + end), "extends past"),
        ("huge-entry-size", packed(struct.pack("<16sII", b"kernel", 0xffffffff, 0) + end), "extends past"),
        ("last-kernel-empty", packed(kentry + entry(b"kernel", b"") + end), "no kernel data"),
    ]
    for name, data, reason in malformed:
        path, target = os.path.join(work, name + ".bin"), os.path.join(work, name + "-esp")
        open(path, "wb").write(data)
        r = run(target, path, "--loader", loader)
        check(name + " is refused with a reason", r.returncode != 0 and reason in r.stderr and "Traceback" not in r.stderr, r.stdout + r.stderr)
        check(name + " leaves no output directory", not os.path.exists(target))

    compatible = [
        ("empty-optional-entry", packed(kentry + entry(b"ramdisk", b"") + end)),
        ("unknown-entry", packed(kentry + entry(b"future", b"future data") + end)),
        ("file-padding", packed(kentry + end) + b"\0" * 16),
    ]
    for name, data in compatible:
        path, target = os.path.join(work, name + ".bin"), os.path.join(work, name + "-esp")
        open(path, "wb").write(data)
        r = run(target, path, "--loader", loader)
        check(name + " is still accepted", r.returncode == 0, r.stdout + r.stderr)
        check(name + " is copied unchanged", r.returncode == 0 and open(os.path.join(target, "EFI/hakios/payload.bin"), "rb").read() == data)

    big = os.path.join(work, "big.bin")
    kbody = struct.pack("<16sII", b"kernel", 0, 0) + b"\0" * 24
    open(big, "wb").write(struct.pack("<II", 0x50554E58, len(kbody)) + kbody + b"\0" * (63 * 1024 * 1024 + 4096))
    r = run(os.path.join(work, "esp5"), big, "--loader", loader)
    check("a payload over the loader's 63 MiB is refused, and says why", r.returncode != 0 and "too big" in r.stderr, r.stderr)

    notefi = os.path.join(work, "notefi")
    open(notefi, "wb").write(b"ELF\0" * 8)
    r = run(os.path.join(work, "esp6"), good, "--loader", notefi)
    check("a loader that is not an EFI application is refused", r.returncode != 0 and "MZ" in r.stderr, r.stderr)

    r = run(os.path.join(work, "esp7"), os.path.join(work, "no-such.bin"), "--loader", loader)
    check("a missing payload is refused", r.returncode != 0 and "no payload at" in r.stderr, r.stderr)
finally:
    shutil.rmtree(work, ignore_errors=True)

if failures:
    sys.exit("mkesp-test: %d FAILED" % len(failures))
print("mkesp-test: all passed")
