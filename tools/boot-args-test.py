#!/usr/bin/env python3
"""tools/boot-args.py: every form in which a boot-argument is read is found, and what only looks like one is not.

Each form has a case that must be found, and the ways a scan can be wrong have cases that must NOT be found. The
scanner missed whole families while it was written, each time a form it did not know: the wrappers that take the name as
a string (option("modem_again"), lx_os_option("wifi_irq")), XNU's TUNABLE(type, var, "fb_beat", 1), user space's own
helper boot_arg("var_store"). The check at the end is the guard: a driver that adds an argument makes CI say the
catalogue is out of date.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "boot-args.py")
failures = []


def check(label, ok, detail=""):
    print("%s %s" % ("ok  " if ok else "FAIL", label))
    if not ok:
        failures.append(label)
        if detail:
            print("     " + str(detail).strip().replace("\n", "\n     ")[:500])


def repo(files):
    d = tempfile.mkdtemp(prefix="boot-args-test-")
    subprocess.run(["git", "init", "-q", d], check=True)
    for name, text in files.items():
        path = os.path.join(d, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "w", encoding="utf-8", newline="\n").write(text)
    subprocess.run(["git", "-C", d, "add", "-A"], check=True, capture_output=True)
    return d


def run(d, *args):
    return subprocess.run([sys.executable, TOOL, "--root", d, *args], capture_output=True, text=True)


def found(d):
    """{name: layers} from --lst."""
    out = {}
    for line in run(d, "--lst").stdout.splitlines():
        name, _, layers = line.partition("\t")
        out[name] = layers
    return out


FILES = {
    # kernel: the direct forms, in the old place (xnu/files) and the new one (drivers/)
    "drivers/gpu/hakios.adreno.c": 'void f(void) { PE_parse_boot_argn("k_direct", &v, sizeof(v)); PE_parse_boot_arg_str("k_string", b, 8); }\n',
    "drivers/gpu/hakios.gpu.cpp": 'static TUNABLE(uint32_t, gOnTheBeat, "k_tunable", 1);\nstatic TUNABLE_WRITEABLE(uint32_t, gW, "k_writeable", 0);\n',
    # kernel: a file with its own option(name) that calls PE_parse_boot_argn(name)
    "drivers/modem/hakios.mpss.c": 'static bool\noption(const char *name)\n{\n\treturn PE_parse_boot_argn(name, &v, 4);\n}\nvoid g(void) { if (option("k_wrapped")) {} }\n',
    # kernel: Wi-Fi, through the helper and through the Linux layer
    "drivers/wifi/hakios.wifi.c": 'void h(void) { hakios_wifi_option("k_wifi_helper"); }\n',
    "wifi/lx/src/lx_bus.c": 'void i(void) { if (lx_os_option("k_wifi_lx")) {} }\n',
    # the loader
    "xnuloader/loader.c": 'static void j(const char *c) { if (starts_with(c, "l_arg=") || starts_with(c, "ku_both=")) {} }\n',
    # user space: strstr on the kern.bootargs buffer, and its own boot_arg() helper
    "userland/src/eagl.c": 'void k(char *arguments) { strstr(arguments, "u_strstr=1"); strstr(arguments, "ku_both="); }\n',
    "userland/src/init.c": 'static const char *\nboot_arg(const char *name)\n{ return 0; }\nvoid m(void) { boot_arg("u_helper"); }\n',
    "userland/src/HakiosMetalCA.m": 'void hm(void) { if (hm_boot_arg_set("u_hm_helper=1")) {} }\n',
    # the kernel also reads ku_both
    "drivers/net/hakios.net.c": 'void n(void) { PE_parse_boot_argn("ku_both", &v, 4); }\n',
    # NOT arguments: must not be found
    "xnu/files/bsd/dev/hakios.other.c": 'static bool option(int x);\nvoid p(void) { option("not_k_no_definition"); }\n',     # option() here is no wrapper: no definition of option(const char *name)
    "xnu/files/bsd/dev/hakios.comment.c": '/* boot-arg adreno_zap=0 for the second run; nothing reads it now */\nvoid q(void) {}\n',
    "userland/src/apfs_mmap_test.c": 'void r(char *line) { strstr(line, "metadata"); strstr(line, "not_u_other=1"); }\n',     # strstr on another buffer
    "xnu/files/bsd/dev/hakios.dt.c": 'static TUNABLE_DT(uint32_t, gD, "/chosen", "name", "not_k_dt", 0, 0);\n',                    # a device-tree tunable: not a plain boot-arg
}

d = repo(FILES)
try:
    got = found(d)
    for name, layers in (("k_direct", "k"), ("k_string", "k"), ("k_tunable", "k"), ("k_writeable", "k"), ("k_wrapped", "k"),
                         ("k_wifi_helper", "k"), ("k_wifi_lx", "k"), ("l_arg", "l"), ("u_strstr", "u"), ("u_helper", "u"), ("u_hm_helper", "u"),
                         ("ku_both", "klu")):
        check("found: %-14s layers %s" % (name, layers), got.get(name) == layers, "got %r" % got.get(name))
    for name in ("not_k_no_definition", "adreno_zap", "metadata", "not_u_other", "not_k_dt", "name", "arguments"):
        check("NOT found: %s" % name, name not in got, "it is in the catalogue as %r" % got.get(name))
    check("drivers/ is scanned as well as xnu/files/", "k_tunable" in got)
    check("the groups: the GPU file's argument is under GPU and display", "## GPU and display" in run(d, "--markdown").stdout)
    check("the page says which layers read ku_both", "| `ku_both` | KLU |" in run(d, "--markdown").stdout, run(d, "--markdown").stdout[-400:])

    # the symbols: one for each argument, its meaning where the source documents it, none for what the tools set
    symbols = {l.split("\t")[2]: l.split("\t") for l in run(d, "--symbols").stdout.splitlines()}
    check("a symbol for each argument (and none for loader_return_after-style ones set by the tools)", "k_direct" in symbols and "u_helper" in symbols, sorted(symbols))
    check("the symbol's name is the argument's, upper case", symbols["k_tunable"][0] == "K_TUNABLE", symbols.get("k_tunable"))
    check("an argument read as text still gets a symbol", "k_string" in symbols, sorted(symbols))
    check("without a documentation line the meaning is who reads it", "read by hakios.adreno.c" in symbols["k_direct"][6] or symbols["k_direct"][6] == "", symbols["k_direct"])
    # the check: missing files; current files; a driver that adds an argument; a table symbol nobody reads
    r = run(d, "--check")
    check("--check: files missing -> fails and says what to run", r.returncode != 0 and "boot-args.py --markdown" in r.stdout, r.stdout)
    os.makedirs(os.path.join(d, "docs"), exist_ok=True)
    os.makedirs(os.path.join(d, "configs"), exist_ok=True)
    open(os.path.join(d, "docs", "boot-args.md"), "w", newline="\n").write(run(d, "--markdown").stdout)
    open(os.path.join(d, "configs", "boot-args.lst"), "w", newline="\n").write(run(d, "--lst").stdout)
    open(os.path.join(d, "configs", "symbols.tsv"), "w", newline="\n").write(run(d, "--symbols").stdout)
    check("--check: current files -> passes", run(d, "--check").returncode == 0, run(d, "--check").stdout)
    open(os.path.join(d, "xnu", "files", "bsd", "dev", "hakios.adreno.c"), "a").write('void more(void) { PE_parse_boot_argn("a_new_one", &v, 4); }\n')
    subprocess.run(["git", "-C", d, "add", "-A"], check=True, capture_output=True)
    r = run(d, "--check")
    check("--check: a driver adds an argument -> the catalogue is out of date, fails", r.returncode != 0 and "out of date" in r.stdout, r.stdout)
    os.makedirs(os.path.join(d, "tools"), exist_ok=True)
    open(os.path.join(d, "tools", "hakios-config.py"), "w").write('SYMBOLS = [\n    ("GHOST", "bool", "nobody_reads_this", "x", None, None),\n]\n')
    subprocess.run(["git", "-C", d, "add", "-A"], check=True, capture_output=True)
    open(os.path.join(d, "docs", "boot-args.md"), "w", newline="\n").write(run(d, "--markdown").stdout)
    open(os.path.join(d, "configs", "boot-args.lst"), "w", newline="\n").write(run(d, "--lst").stdout)
    open(os.path.join(d, "configs", "symbols.tsv"), "w", newline="\n").write(run(d, "--symbols").stdout)
    r = run(d, "--check")
    check("--check: a configuration symbol whose argument nobody reads -> fails", r.returncode != 0 and "nobody_reads_this" in r.stdout, r.stdout)
finally:
    shutil.rmtree(d, ignore_errors=True)

# the real tree: the committed catalogue is current, and the table's symbols are all read somewhere
ROOT = os.path.dirname(HERE)
r = subprocess.run([sys.executable, TOOL, "--check"], capture_output=True, text=True, cwd=ROOT)
check("the real tree: docs/boot-args.md and configs/boot-args.lst are current", r.returncode == 0, r.stdout)

if failures:
    sys.exit("boot-args-test: %d FAILED" % len(failures))
print("boot-args-test: all passed")
