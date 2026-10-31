#!/usr/bin/env python3
"""The CPU reference: run an AIR shader through LLVM on this machine.

This is the strongest check the project has (docs/air.md section 4) and it
needs no Apple hardware and no GPU. LLVM -- not our reader and not our
translator -- decides what the shader means:

  1. the module's triple is changed to the host's and llc compiles it,
  2. a harness is generated from the entry point's own metadata: one buffer
     per air.buffer argument, filled with a seeded pattern, and the result
     printed field by field,
  3. the same shader goes through our frontend, and the two results are
     compared.

Step 3 is not automatic for every shader: a fragment shader's result depends
on where it is in the buffer, and only the GPU knows that. What is automatic
is the class this covers -- an entry point whose arguments are all constant or
device buffers and whose result is a struct of vectors, which is 256 of the
corpus's 271 fragment shaders. For those the NIR is interpreted here too, by
nir_opt_constant_folding on a shader whose buffer loads have been replaced
with the harness's bytes, so both sides are numbers and the comparison is
exact.

usage: cpuref.py FILE[.metallib] [FUNCTION]      run and print LLVM's result
       cpuref.py --compare FILE [FUNCTION]       and compare against our NIR
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import metallib  # noqa: E402
sys.path.insert(0, os.path.join(ROOT, "tools/air"))
import airdefs  # noqa: E402  (LLVM-IR definitions for the exactly defined air.*)

AIR2NIR = os.environ.get(
    "AIR2NIR", os.path.join(ROOT, "build/mesa-host/build/src/compiler/air/air2nir"))
AIR_RUN = os.environ.get(
    "AIR_RUN",
    os.path.join(ROOT, "build/mesa-host/build/src/gallium/drivers/freedreno/air_run"))

# The pattern every buffer is filled with. Arbitrary, but fixed and not all
# zeros: a shader that ignores its input and one that reads it must differ.
def pattern(n):
    return bytes((i * 7 + 13) & 0xff for i in range(n))


class CType:
    """One LLVM type, as C and as a struct.unpack format."""

    def __init__(self, c, fmt, size):
        self.c, self.fmt, self.size = c, fmt, size


SCALARS = {
    "float": CType("float", "f", 4),
    "half": CType("unsigned short", "H", 2),   # no C half; compared as bits
    "double": CType("double", "d", 8),
    "i1": CType("unsigned char", "B", 1),
    "i8": CType("unsigned char", "B", 1),
    "i16": CType("unsigned short", "H", 2),
    "i32": CType("unsigned int", "I", 4),
    "i64": CType("unsigned long long", "Q", 8),
}


def parse_type(t):
    """<4 x float> -> (CType, 4); float -> (CType, 1)."""
    t = t.strip()
    m = re.match(r"<(\d+) x (.+)>$", t)
    if m:
        base = SCALARS.get(m.group(2).strip())
        return (base, int(m.group(1))) if base else (None, 0)
    base = SCALARS.get(t)
    return (base, 1) if base else (None, 0)


def param_type(t):
    """The type out of a define's parameter: "<4 x float> %0" -> "<4 x float>".

    Taken as the leading type rather than by stripping a list of attribute
    words: LLVM has dozens of parameter attributes and they sit between the
    type and the name, so a blacklist misses one -- "returned" was the one that
    made a library function unreadable here, and the next version of LLVM would
    have found another.
    """
    m = re.match(r"\s*(<\s*\d+\s+x\s+[A-Za-z_0-9]+\s*>|\[[^\]]*\]|[A-Za-z_0-9]+)",
                 t)
    return re.sub(r"\s+", " ", m.group(1)).strip() if m else t.strip()


def split_fields(s):
    """The top-level comma-separated members of a struct body."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "<[{":
            depth += 1
        elif ch in ">]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return out


def disassemble(bc_path):
    return subprocess.run(["llvm-dis", "-o", "-", bc_path], capture_output=True,
                          text=True, check=True).stdout


def entry_of(ll):
    """The entry point's name, its parameter types and its result type."""
    stage = None
    for name in ("vertex", "fragment", "kernel", "visible"):
        if re.search(r"^!air\.%s = " % name, ll, re.M):
            stage = name
            break
    m = re.search(r"^define[^@\n]*@([A-Za-z_0-9.$]+)\(([^)]*)\)[^\n]*\{$", ll, re.M)
    if not m:
        return None
    # the LAST define is not necessarily the entry point; find the one the
    # metadata names
    ep = re.search(r"^!air\.(?:vertex|fragment|kernel|visible) = !\{!(\d+)\}", ll, re.M)
    if ep:
        node = re.search(r"^!%s = !\{ptr @([A-Za-z_0-9.$]+)," % ep.group(1), ll, re.M)
        if node:
            want = node.group(1)
            for m2 in re.finditer(r"^define([^@\n]*)@([A-Za-z_0-9.$]+)\(([^)]*)\)[^\n]*\{$",
                                  ll, re.M):
                if m2.group(2) == want:
                    m = m2
                    break
    ret = m.group(1).strip().split()[-1] if m.group(1).strip() else "void"
    # "define <{ <4 x float> }> @f(...)": the return type may contain spaces
    head = ll[m.start():m.start(1) + len(m.group(1))]
    ret = head[len("define"):].strip()
    for word in ("local_unnamed_addr", "internal", "fastcc", "unnamed_addr", "dso_local"):
        ret = ret.replace(word, "").strip()
    return dict(stage=stage, name=m.group(2), params=split_fields(m.group(3)), ret=ret)


def buffer_args(ll, entry):
    """Which parameters are buffers, from the entry-point metadata."""
    ep = re.search(r"^!air\.(?:vertex|fragment|kernel|visible) = !\{!(\d+)\}", ll, re.M)
    if not ep:
        return {}
    node = re.search(r"^!%s = !\{ptr @[^,]+, !(\d+), !(\d+)" % ep.group(1), ll, re.M)
    if not node:
        return {}
    ins = re.search(r"^!%s = !\{([^}]*)\}" % node.group(2), ll, re.M)
    if not ins:
        return {}
    out = {}
    for ref in re.findall(r"!(\d+)", ins.group(1)):
        line = re.search(r"^!%s = !\{(.*)\}$" % ref, ll, re.M)
        if not line:
            continue
        body = line.group(1)
        pm = re.match(r"\s*i32 (\d+)", body)
        if not pm or '"air.buffer"' not in body:
            continue
        size = re.search(r'"air\.buffer_size", i32 (\d+)', body)
        out[int(pm.group(1))] = int(size.group(1)) if size else 256
    return out


# Apple's name for each type, from the air.visible_input/output metadata beside
# a library function. It is a second statement of what the arguments are: the
# LLVM signature says <4 x float>, the metadata says float4, and a harness that
# agrees with only one of them is guessing. parse_type reads the LLVM side; this
# reads Apple's, and the two are checked against each other before anything runs.
APPLE_TYPES = {
    "float": ("float", 1), "float2": ("float", 2),
    "float3": ("float", 3), "float4": ("float", 4),
    "half": ("half", 1), "half2": ("half", 2),
    "half3": ("half", 3), "half4": ("half", 4),
}


def library_types(ll):
    """(inputs, output) as Apple names them, in parameter order."""
    ins = {}
    for body in re.findall(r"^!\d+ = !\{(.*)\}$", ll, re.M):
        if '"air.visible_input"' in body:
            pm = re.match(r"\s*i32 (\d+)", body)
            tn = re.search(r'"air\.arg_type_name", !"([^"]*)"', body)
            if pm and tn:
                ins[int(pm.group(1))] = tn.group(1)
    out = None
    for body in re.findall(r"^!\d+ = !\{(.*)\}$", ll, re.M):
        if '"air.visible_output"' in body:
            tn = re.search(r'"air\.arg_type_name", !"([^"]*)"', body)
            if tn:
                out = tn.group(1)
    return [ins.get(i) for i in range(len(ins))], out


def check_against_metadata(entry, ll):
    """Refuse when the LLVM types and Apple's names do not say the same thing."""
    names, out = library_types(ll)
    if len(names) != len(entry["params"]):
        return ("the metadata names %d arguments and the signature has %d"
                % (len(names), len(entry["params"])))
    for i, (t, name) in enumerate(zip(entry["params"], names)):
        base, n = parse_type(param_type(t))
        if base is None:
            return "argument %d is %s, which this harness cannot make" % (i, param_type(t))
        if name is None:
            return "argument %d has no air.arg_type_name" % i
        want = APPLE_TYPES.get(name)
        if want is None:
            return "argument %d is called %s, a type this harness does not make" % (i, name)
        if (want[0] == "float") != (base.fmt == "f") or want[1] != n:
            return ("argument %d is %s in the signature and %s in the metadata"
                    % (i, param_type(t), name))
    if out is not None and out not in APPLE_TYPES:
        return "the result is called %s, which this harness cannot read" % out
    return None


def air_calls(ll):
    """Every air.* function the module declares."""
    return sorted(set(re.findall(r"^declare[^@\n]*@(air\.[A-Za-z0-9_.]+)\(", ll, re.M)))


def with_definitions(ll):
    """The module plus definitions for the air.* it calls, or why not.

    LLVM cannot evaluate a call it has no body for, so a function calling
    air.* was outside this check entirely -- 21 of the corpus's 135 library
    functions call none and were the only ones it could reach. airdefs.py
    supplies bodies for the families whose result is exactly defined, matched
    to intrincheck's reference, which is itself pinned to the Adreno on 1 748
    cases.

    That makes the comparison weaker than the 21 and it must not be counted
    with them: LLVM decides the function's own arithmetic, and we decide what
    its air.* calls mean. A family with no bit-exact reference anywhere
    (air.fast_pow and the rest) is refused by name rather than approximated,
    because a body invented for it would make the agreement circular.
    """
    names = air_calls(ll)
    if not names:
        return ll, None, 0
    refused = airdefs.refusals_for(names)
    if refused:
        first = sorted(refused)[0]
        return None, "%s has no bit-exact reference: %s" % (first, refused[first]), 0
    ir = airdefs.definitions_for(names)
    if not ir.strip():
        return None, "no definition for %s" % ", ".join(names[:3]), 0
    # the module declares each of them; textual IR allows a name once, so the
    # declaration has to go before the definition can come in ("invalid
    # redefinition of function", which is what LLVM says rather than merging
    # the two the way a linker would)
    defined = set(re.findall(r"^define[^@\n]*@(air\.[A-Za-z0-9_.]+)\(", ir, re.M))
    kept = []
    for line in ll.splitlines():
        m = re.match(r"^declare[^@\n]*@(air\.[A-Za-z0-9_.]+)\(", line)
        if m and m.group(1) in defined:
            continue
        kept.append(line)
    return "\n".join(kept) + "\n" + ir + "\n", None, len(defined)


def build_and_run(ll, entry, bufs, workdir, library=False):
    """Compile the module for this machine and call the entry point.

    The call is made from a wrapper written in LLVM IR, not from C. A shader
    returns a struct of vectors by value, and how llc lowers that is its own
    business -- a C declaration of "the same" struct is not guaranteed to
    agree, and for anything past a single vector it does not. So LLVM compiles
    both sides of that call, the wrapper stores each result field into a flat
    buffer, and the only thing C ever sees is void(const void *, void *).
    """
    ret = entry["ret"].strip()
    body = re.match(r"<?\{(.*)\}>?$", ret)
    members = split_fields(body.group(1)) if body else ([ret] if ret != "void" else [])
    fields = []
    for i, t in enumerate(members):
        base, n = parse_type(t)
        if base is None:
            return None, "result field %d is %s, which this harness cannot read" % (i, t)
        fields.append((base, n, t.strip()))
    if not fields:
        return None, "the entry point returns nothing to compare"

    # A library function takes values, not buffers, so the wrapper reads each
    # argument out of one buffer at byte 16*i -- the same slot the frontend's
    # generated entry point reads it from (wrap_library_function in
    # air_to_nir.c). Both sides read the same offsets, so this comparison
    # cannot catch a slot layout that is wrong in the same way twice; what it
    # does catch is the two sides disagreeing about which slot is which
    # argument, because the pattern makes every slot's bytes different.
    if library:
        wrap_params, call_args, pre = ["ptr %in"], [], []
        for i, t in enumerate(entry["params"]):
            t = param_type(t)
            pre.append("  %%g%d = getelementptr i8, ptr %%in, i64 %d" % (i, 16 * i))
            pre.append("  %%a%d = load %s, ptr %%g%d, align 16" % (i, t, i))
            call_args.append("%s %%a%d" % (t, i))
        return _finish(ll, entry, {0: 16 * max(len(entry["params"]), 1)}, workdir,
                       fields, ret, wrap_params, call_args, pre, True)

    # the wrapper's own arguments: one pointer per buffer, and the output
    wrap_params, call_args, pre = [], [], []
    for i, t in enumerate(entry["params"]):
        t = t.strip()
        if "ptr" in t:
            if i not in bufs:
                return None, "parameter %d is a pointer that is not a buffer" % i
            wrap_params.append("ptr %%in%d" % i)
            m = re.search(r"addrspace\((\d+)\)", t)
            if m:
                pre.append("  %%p%d = addrspacecast ptr %%in%d to ptr addrspace(%s)"
                           % (i, i, m.group(1)))
                call_args.append("ptr addrspace(%s) %%p%d" % (m.group(1), i))
            else:
                call_args.append("ptr %%in%d" % i)
        else:
            base, n = parse_type(t)
            if base is None:
                return None, "parameter %d is %s, which this harness cannot make" % (i, t)
            # a non-buffer argument (a varying, a vertex id) is zero: what it
            # really is only the GPU knows, so such a shader is not compared
            return None, "parameter %d is %s, not a buffer" % (i, t)

    return _finish(ll, entry, bufs, workdir, fields, ret, wrap_params, call_args, pre)


def _finish(ll, entry, bufs, workdir, fields, ret, wrap_params, call_args, pre,
            library=False):
    """Compile the wrapper and the C harness around it, and run them."""
    # store each field at its own 16-byte slot of the output buffer. A shader
    # returns a struct of fields and each one is extracted; a library function
    # returns one value and there is nothing to extract -- extractvalue on a
    # vector is not valid IR, and llc says so with a caret and no sentence.
    is_struct = bool(re.match(r"<?\{", ret.strip()))
    stores, slots = [], []
    for i, (base, n, t) in enumerate(fields):
        if is_struct:
            stores.append("  %%f%d = extractvalue %s %%r, %d" % (i, ret, i))
        else:
            stores.append("  %%f%d = bitcast %s %%r to %s" % (i, t, t))
        stores.append("  %%o%d = getelementptr i8, ptr %%out, i64 %d" % (i, i * 16))
        stores.append("  store %s %%f%d, ptr %%o%d, align 1" % (t, i, i))
        slots.append((base, n))

    wrapper = """
define void @__cpuref_call(%s, ptr %%out) {
%s
  %%r = call %s @%s(%s)
%s
  ret void
}
""" % (", ".join(wrap_params + []) if wrap_params else "ptr %unused",
       "\n".join(pre), ret, entry["name"], ", ".join(call_args), "\n".join(stores))

    host_ll = os.path.join(workdir, "m.ll")
    with open(host_ll, "w") as f:
        f.write(re.sub(r'target triple = "[^"]*"',
                       'target triple = "%s"' % host_triple(), ll))
        f.write(wrapper)
    obj = os.path.join(workdir, "m.o")
    llc = subprocess.run(["llc", "-filetype=obj", "-o", obj, host_ll],
                         capture_output=True, text=True)
    if llc.returncode:
        last = (llc.stderr or "").strip().splitlines()
        return None, "llc refused it: " + (last[-1][:140] if last else "no message")

    setup, args = [], []
    for i in sorted(bufs):
        setup.append("    static unsigned char buf%d[%d];" % (i, bufs[i]))
        if library:
            # floats, not bytes: these modules are compiled with no-nans and
            # no-infs, so arbitrary bytes put the call outside the contract it
            # was compiled under. The same words air2nir and air_run use
            # (library_byte in both).
            setup.append("    for (int w = 0; w < %d; w++) {"
                         " float f = (float)((w * 13) %% 29 - 14) / 4.0f;"
                         " __builtin_memcpy(buf%d + 4 * w, &f, 4); }"
                         % (bufs[i] // 4, i))
        else:
            setup.append("    for (int i = 0; i < %d; i++) buf%d[i] = (i * 7 + 13) & 0xff;"
                         % (bufs[i], i))
        args.append("buf%d" % i)
    decl = ", ".join(["const void *"] * len(args) + ["void *"]) if args else "void *"

    prints = []
    for i, (base, n) in enumerate(slots):
        for k in range(n):
            at = "out + %d" % (i * 16 + k * base.size)
            if base.fmt == "f":
                prints.append('    { float v; __builtin_memcpy(&v, %s, 4);'
                              ' printf("%%.9g\\n", (double)v); }' % at)
            elif base.fmt == "d":
                prints.append('    { double v; __builtin_memcpy(&v, %s, 8);'
                              ' printf("%%.17g\\n", v); }' % at)
            else:
                prints.append('    { %s v; __builtin_memcpy(&v, %s, %d);'
                              ' printf("%%llu\\n", (unsigned long long)v); }'
                              % (base.c, at, base.size))

    src = """#include <stdio.h>
extern void __cpuref_call(%s);
int main(void) {
%s
    static unsigned char out[%d];
    __cpuref_call(%s);
%s
    return 0;
}
""" % (decl, "\n".join(setup), max(16 * len(fields), 16),
       ", ".join(args + ["out"]) if args else "out", "\n".join(prints))

    csrc = os.path.join(workdir, "harness.c")
    with open(csrc, "w") as f:
        f.write(src)
    exe = os.path.join(workdir, "harness")
    # -no-pie: llc compiles the module with the default (static) relocation
    # model, so a module with a global of its own gets an absolute relocation
    # (R_X86_64_32) that a PIE link refuses. No corpus module had reached this
    # -- none of the comparable ones has a global -- and the first generated
    # module with a constant array of its own found it. It changes how the
    # harness is LINKED and not what it computes.
    cc = subprocess.run(["cc", "-O0", "-no-pie", "-o", exe, csrc, obj, "-lm"],
                        capture_output=True, text=True)
    if cc.returncode:
        last = cc.stderr.strip().splitlines()
        return None, "the harness did not build: " + (last[-1][:140] if last else "?")
    run = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    if run.returncode:
        return None, "the harness crashed (signal %d)" % -run.returncode
    return [l for l in run.stdout.splitlines() if l], None


def host_triple():
    out = subprocess.run(["llc", "--version"], capture_output=True, text=True).stdout
    m = re.search(r"Default target: (\S+)", out)
    return m.group(1) if m else "x86_64-unknown-linux-gnu"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("function", nargs="?")
    ap.add_argument("--compare", action="store_true",
                    help="run ours too and compare the numbers")
    ap.add_argument("--all", action="store_true",
                    help="every function of the file, comparing each")
    ap.add_argument("--tolerance", type=float, default=0.0,
                    help="relative difference allowed (default: none, bit-exact)")
    ap.add_argument("--draw", action="store_true",
                    help="also draw with it (air_run) and compare the pixel")
    args = ap.parse_args()
    if args.all:
        return run_all(args)

    with tempfile.TemporaryDirectory() as work:
        if args.file.endswith(".metallib"):
            b, header, funcs = metallib.parse(args.file)
            picked = None
            for f in funcs:
                if not args.function or f.get("name") == args.function:
                    picked = f
                    break
            if not picked:
                sys.exit("cpuref.py: no function %s in %s" % (args.function, args.file))
            bc, _ = metallib.bitcode_of(b, header, picked)
            bc_path = os.path.join(work, "m.bc")
            with open(bc_path, "wb") as f:
                f.write(bc)
        else:
            bc_path = args.file

        ll = disassemble(bc_path)
        entry = entry_of(ll)
        if not entry:
            sys.exit("cpuref.py: no entry point found")
        library = entry["stage"] == "visible"
        bufs = buffer_args(ll, entry)
        if library:
            why = check_against_metadata(entry, ll)
            if why:
                print("cpuref: cannot run this one: %s" % why)
                return 1
            bufs = {0: 16 * max(len(entry["params"]), 1)}
            print("cpuref: %s (a library function), %d arguments, each read from "
                  "byte 16*i of one buffer" % (entry["name"], len(entry["params"])))
        else:
            print("cpuref: %s (%s), %d parameters, %d of them buffers" %
                  (entry["name"], entry["stage"], len(entry["params"]), len(bufs)))
        for i, n in sorted(bufs.items()):
            print("  parameter %d: a buffer of %d bytes, filled with %s"
                  % (i, n, "the floats ((w * 13) % 29 - 14) / 4" if library
                     else "(i * 7 + 13) & 0xff"))

        defs = 0
        if library:
            ll2, why, defs = with_definitions(ll)
            if why:
                print("cpuref: cannot run this one: %s" % why)
                return 1
            ll = ll2
            if defs:
                print("  with our own definitions for %d air.* operations, whose semantics "
                      "come from intrincheck's reference and not from LLVM" % defs)
        values, why = build_and_run(ll, entry, bufs, work, library)
        if why:
            print("cpuref: cannot run this one: %s" % why)
            return 1
        print("cpuref: LLVM on this machine says the result is:")
        for v in values:
            print("  %s" % v)

        if args.compare:
            ours, why = evaluate_ours(args.file, entry["name"], library)
            if why:
                print("cpuref: ours could not be evaluated: %s" % why)
                return 1
            print("cpuref: and our frontend, folded over the same bytes, says:")
            for v in ours:
                print("  %s" % v)
            bad = compare(values, ours, args.tolerance)
            print("cpuref: %s" % ("they agree" if not bad else "they DIFFER: " + bad))
            return 1 if bad else 0
    return 0


def draw_ours(path, function):
    """air_run: the same shader drawn by a Gallium driver, pixel read back."""
    out = subprocess.run([AIR_RUN, path, function], capture_output=True, text=True)
    if out.returncode:
        return None, (out.stderr or out.stdout).strip().splitlines()[-1][:160] \
            if (out.stderr or out.stdout).strip() else "air_run failed"
    values, taking = [], False
    for line in out.stdout.splitlines():
        if line.startswith("air_run: the pixel"):
            taking = True
            continue
        if taking and line.startswith("  "):
            values.append(line.strip())
        elif taking:
            break
    return (values, None) if values else (None, "air_run printed no pixel")


def evaluate_ours(path, function, library=False):
    """air2nir --eval: the same shader folded over the same buffer bytes."""
    cmd = [AIR2NIR] + (["--library"] if library else []) + ["--eval", path, function]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode:
        return None, (out.stderr or out.stdout).strip().splitlines()[-1][:160]
    values = []
    for line in out.stdout.splitlines():
        if not line.startswith("  ") or line.startswith("  constant") or \
           line.startswith("  device") or line.startswith("  ("):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        if "did not fold" in line:
            return None, "an output did not fold to a constant"
        values.extend(parts[1:])
    return values, None


def compare(theirs, ours, tol):
    """What differs between LLVM's numbers and ours, or None."""
    if len(theirs) != len(ours):
        return "LLVM printed %d values, we printed %d" % (len(theirs), len(ours))
    for i, (a, b) in enumerate(zip(theirs, ours)):
        fa, fb = float(a), float(b)
        if fa == fb:
            continue
        if tol > 0:
            scale = max(abs(fa), abs(fb))
            if scale > 0 and abs(fa - fb) / scale <= tol:
                continue
        return "value %d: LLVM %s, ours %s" % (i, a, b)
    return None


def run_all(args):
    """Every function of a .metallib, with a count at the end."""
    b, header, funcs = metallib.parse(args.file)
    agree = differ = skipped = with_air = drawn = not_drawn = 0
    for f in funcs:
        name = f.get("name")
        if not name:
            continue
        with tempfile.TemporaryDirectory() as work:
            bc, _ = metallib.bitcode_of(b, header, f)
            bc_path = os.path.join(work, "m.bc")
            with open(bc_path, "wb") as fp:
                fp.write(bc)
            try:
                ll = disassemble(bc_path)
                entry = entry_of(ll)
            except subprocess.CalledProcessError:
                entry = None
            if not entry:
                skipped += 1
                continue
            theirs, why = build_and_run(ll, entry, buffer_args(ll, entry), work)
            if why:
                print("skip   %-44s LLVM: %s" % (name, why))
                skipped += 1
                continue
        ours, why = evaluate_ours(args.file, name)
        if why:
            print("skip   %-44s ours: %s" % (name, why))
            skipped += 1
            continue
        bad = compare(theirs, ours, args.tolerance)
        drawn_note = ""
        if not bad and args.draw:
            pixel, why = draw_ours(args.file, name)
            if why:
                drawn_note = "  (not drawn: %s)" % why
                not_drawn += 1
            else:
                # the pixel is render target 0, which is the result's first
                # field for every shader of this corpus
                dbad = compare(theirs[:len(pixel)], pixel, args.tolerance)
                if dbad:
                    print("DRAWN DIFFERS %-38s %s" % (name, dbad))
                    differ += 1
                    continue
                drawn += 1
                drawn_note = "  drawn and matching"
        if bad:
            print("DIFFER %-44s %s" % (name, bad))
            differ += 1
        else:
            agree += 1
            if calls_air(b, header, f):
                with_air += 1
            print("agree  %-44s %s%s" % (name, " ".join(theirs[:4]), drawn_note))
    print("\ncpuref: %d agree with LLVM, %d differ, %d could not be compared (of %d)"
          % (agree, differ, skipped, len(funcs)))
    # Which part of the frontend this actually exercised. A shader that calls
    # no air.* checks the reader, the interface and the instructions, and says
    # nothing whatever about air_intrinsics.c.
    print("cpuref: of the %d that agree, %d call an air.* function; %d are plain LLVM IR, "
          "so they test the reader, the interface and the instructions but no intrinsic"
          % (agree, with_air, agree - with_air))
    if args.draw:
        print("cpuref: %d of them were also DRAWN by a Gallium driver and the pixel matched; "
              "%d could not be drawn" % (drawn, not_drawn))
    return 1 if differ else 0


def calls_air(b, header, f):
    """Does this module declare any air.* function?"""
    bc, _ = metallib.bitcode_of(b, header, f)
    with tempfile.TemporaryDirectory() as work:
        p = os.path.join(work, "m.bc")
        with open(p, "wb") as fp:
            fp.write(bc)
        try:
            ll = disassemble(p)
        except subprocess.CalledProcessError:
            return False
    return bool(re.search(r"^declare[^@\n]*@air\.", ll, re.M))


if __name__ == "__main__":
    sys.exit(main())
