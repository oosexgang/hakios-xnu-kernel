#!/usr/bin/env python3
"""Ask the frontend, one declaration at a time, whether it translates that call.

The corpus report (`corpus.py`) says where a *module* stops, which is the first
thing that stops it. A module refused for its interface never reaches its
arithmetic, so a library can call a hundred operations and the report name none
of them -- and that is exactly the shape of RE4's shaders, where 7 620 of 7 879
modules stop at `air.indirect_buffer` before an intrinsic is ever translated
(docs/air.md section 11). The list of operations such a library needs is then
unmeasured: `corpus.py` knows what it *calls*, not what of that works.

So each declaration is put in a shader of its own, called on constants, and the
result sunk into a device buffer so that nothing is dead. What comes back is
one of:

  ok              translated; the printed number, if any, is the folder's
  intrinsic       the frontend refused the call by name: a gap to close
  other           refused for something else -- the message is printed, because
                  it is about this probe and not about the operation
  not probeable   an argument this cannot make a constant for (a texture, a
                  sampler, a pointer): the texture families need the interface
                  that binds them and are measured by texopcheck.py instead

The probe has a check of its own, which is that its verdicts are reproducible
from a different route on a corpus where the answer is already known. Over the
222 declarations of the system corpus it refuses exactly the names that corpus's
own modules are refused for -- `air.is_null_texture_2d`, the imageblock load,
`air.sample_texture_2d_array` with its coordinate short by one -- and adds
nothing except the two depth-plane refusals that `const` above owns up to.

A probe is weaker than a corpus module in one way and stronger in another. It
exercises the operation in isolation, so it says nothing about the shader that
called it; but it is the only measurement available for an operation whose every
caller stops earlier, and "the frontend refuses this name" is not a judgement
call -- it is the frontend's own message.

usage: decltry.py DECLS.txt [--corpus CORPUS.json] [--md FILE]

DECLS.txt holds one `declare ... @air.x(...)` line per operation, as
`air2nir --ll` prints them and `intrincheck.py` consumes them. With --corpus
(a `corpus.py --json` file) each name is weighted by how many modules call it,
so the gaps come out in the order of what closing one buys.
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AIR2NIR = os.environ.get(
    "AIR2NIR", os.path.join(ROOT, "build/mesa-host/build/src/compiler/air/air2nir"))

DECL = re.compile(r"^declare\s+(.*?)\s+@(air\.[A-Za-z0-9_.]+)\((.*)\)")
SCALAR = re.compile(r"^(float|half|double|i1|i8|i16|i32|i64)$")
VECTOR = re.compile(r"^<(\d+) x (float|half|double|i1|i8|i16|i32|i64)>$")

# The shader is a fragment one because the fragment stage is the only one where
# every family in the vocabulary is legal: air.discard_fragment and the
# derivatives exist nowhere else. The device buffer is what the result is
# stored into -- a value nothing uses is a value the frontend may never be
# asked to produce.
SKELETON = """source_filename = "decltry"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

%(decl)s
@dt_tg = internal addrspace(3) global [4 x i32] undef

define <{ <4 x float> }> @dt(%(params)s) local_unnamed_addr {
%(body)s
  %%r = insertvalue <{ <4 x float> }> undef, <4 x float> zeroinitializer, 0
  ret <{ <4 x float> }> %%r
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @dt, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!6 = !{%(args)s}
!7 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 4096, !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"buf"}
%(argmd)s
"""

# A texture and a sampler argument, exactly as tools/air/texopcheck.py writes
# them: the frontend reads the access word and the location index out of these
# and nothing else (air_to_nir.c, the air.texture branch), so the type name
# only has to be the shape Apple's compiler writes.
TEXTURE_MD = ('!{i32 1, !"air.texture", !"air.location_index", i32 1, i32 1, !"%s", '
              '!"air.arg_type_name", !"texture%s", !"air.arg_name", !"t"}')
SAMPLER_MD = ('!{i32 2, !"air.sampler", !"air.location_index", i32 0, i32 1, '
              '!"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}')

# The texture families, named the way air_translate_call names them, and the
# dimension suffixes parse_tex_name knows. A name is a texture call here if the
# dispatcher would send it to texture_op; what texture_op then makes of it is
# the thing being measured.
def is_texture_name(op):
    return ("texture" in op or "_depth_" in op or op.startswith("sample")
            or op.startswith("gather") or op.startswith("read_")
            or op.startswith("write_") or "_lod_" in op)


def texture_kind(name):
    """(access word, Metal type-name tail) for the texture this call wants."""
    op = name[4:].split(".")[0]
    access = ("air.write" if op.startswith("write_") else
              "air.sample" if op.startswith(("sample", "gather")) or "_lod_" in op else
              "air.read")
    for suffix in ("cube_array", "2d_ms_array", "2d_array", "1d_array", "3d",
                   "2d_ms", "cube", "1d", "2d"):
        if ("_" + suffix) in op or op.endswith(suffix):
            dim = suffix
            break
    else:
        dim = "2d"
    comp = "uint" if ".u." in name else "int" if ".s." in name else "float"
    tail = "<%s>" % comp if access == "air.sample" else \
           "<%s, access::%s>" % (comp, access.split(".")[1])
    return access, "%s%s" % (dim, tail)


def parse_type(t):
    """(scalar, lanes) for a type a constant can be written for, else None."""
    t = t.strip()
    m = SCALAR.match(t)
    if m:
        return (m.group(1), 1)
    m = VECTOR.match(t)
    if m:
        return (m.group(2), int(m.group(1)))
    return None


def const(ty):
    """A small constant of that type.

    An integer is zero, and that is not a free choice: the trailing integers of
    these signatures are flags, and the frontend reads them. Passing 3 for all
    of them -- which this did at first -- asked every texture write for mip
    level 3 and every atomic for memory order 3, and the frontend refused 20
    declarations for the probe's own arguments rather than for the operation.
    Zero is the level and the order that a shader passes when it asks for
    nothing special, so "ok" means the operation translates in its plain form
    and says nothing about the stronger ones.

    A float is 2.0 rather than 0.0 so that the folded result printed beside the
    verdict distinguishes a few lowerings that all answer zero at zero;
    interesting inputs are intrincheck.py's job and not this one's.

    One integer argument in the vocabulary is not plain at zero: a depth
    texture's plane selector, where the frontend understands 1 and refuses 0.
    So `air.sample_depth_2d` and `air.read_depth_2d_ms` come back refused "with
    aspect 0" here, and that refusal is this probe's argument and not a gap.
    """
    if ty in ("float", "double"):
        return "2.000000e+00"
    if ty == "half":
        return "0xH4000"            # 2.0
    if ty == "i1":
        return "true"
    return "0"


def argument(ty):
    s = parse_type(ty)
    if s is None:
        return None
    base, n = s
    if n == 1:
        return "%s %s" % (base, const(base))
    return "<%d x %s> <%s>" % (n, base, ", ".join("%s %s" % (base, const(base))
                                                  for _ in range(n)))


def first_member(retty):
    """The first member of a struct return type, as text, or None.

    A texture call returns { <4 x float>, i8 }: the texels and a word Metal
    uses for sparse residency. Only the first member is sunk.
    """
    s = retty.strip()
    if not (s.startswith("{") and s.endswith("}")):
        return None
    inner, depth, part = s[1:-1], 0, ""
    for ch in inner:
        if ch in "<{[":
            depth += 1
        elif ch in ">}]":
            depth -= 1
        elif ch == "," and depth == 0:
            break
        part += ch
    return part.strip()


def resource(name, ty, taken):
    """The value to pass for a pointer argument, from the entry point's own
    interface, or None when this probe cannot honestly bind one.

    Which resource a pointer is depends on the operation and not on the type:
    with opaque pointers a texture, a sampler and a device buffer are all
    `ptr addrspace(n)`, and only the name says which.
    """
    space = "3" if "addrspace(3)" in ty else "2" if "addrspace(2)" in ty else "1"
    op = name[4:].split(".")[0]
    if name.startswith("air.atomic."):
        if space == "3":
            return "ptr addrspace(3) getelementptr inbounds ([4 x i32], " \
                   "ptr addrspace(3) @dt_tg, i64 0, i64 0)", None
        if space == "1":
            return "ptr addrspace(1) %buf", None
        return None, "an atomic on address space %s" % space
    if is_texture_name(op):
        if space == "2":
            taken.add("sampler")
            return "ptr addrspace(2) %s", None
        if space == "1" and "texture" not in taken:
            taken.add("texture")
            return "ptr addrspace(1) %t", None
    return None, "argument %s" % ty.strip()


def body_for(name, retty, argtys, taken):
    """The call, and the arithmetic that sinks its result into the buffer."""
    args = []
    for t in argtys:
        a = argument(t)
        if a is None:
            a, why = resource(name, t, taken)
            if a is None:
                return None, why
        args.append(a)
    lines = []
    if retty.strip() == "void":
        lines.append("  call void @%s(%s)" % (name, ", ".join(args)))
        # Nothing to store: the call itself is the whole of what is measured,
        # and a void intrinsic (air.discard_fragment, the atomics that return
        # nothing) is never dead.
        return "\n".join(lines), None

    call_ty = retty.strip()
    member = first_member(call_ty)
    ret = parse_type(member if member is not None else call_ty)
    if ret is None:
        return None, "return type %s" % call_ty
    base, n = ret
    lines.append("  %%c0 = call %s @%s(%s)" % (call_ty, name, ", ".join(args)))
    val, valty = "%c0", call_ty
    if member is not None:
        lines.append("  %%c = extractvalue %s %%c0, 0" % call_ty)
        val, valty = "%c", member
    if n > 1:
        lines.append("  %%e = extractelement %s %s, i32 0" % (valty, val))
        val = "%e"
    # One scalar word, because a store of exactly 32 bits is a store this
    # frontend certainly handles: a refusal has to be about the operation, not
    # about the way the probe put its answer away.
    if base == "half":
        lines.append("  %%w = fpext half %s to float" % val)
        sty = "float"
    elif base == "double":
        lines.append("  %%w = fptrunc double %s to float" % val)
        sty = "float"
    elif base == "float":
        lines.append("  %%w = fadd float %s, 0.000000e+00" % val)
        sty = "float"
    elif base == "i64":
        lines.append("  %%w = trunc i64 %s to i32" % val)
        sty = "i32"
    elif base == "i32":
        lines.append("  %%w = add i32 %s, 0" % val)
        sty = "i32"
    else:
        lines.append("  %%w = zext %s %s to i32" % (base, val))
        sty = "i32"
    lines.append("  store %s %%w, ptr addrspace(1) %%buf, align 4" % sty)
    return "\n".join(lines), None


def probe(decl, name, retty, argtys, tmp):
    """(verdict, detail) for one declaration."""
    taken = set()
    body, why = body_for(name, retty, argtys, taken)
    if body is None:
        return "not probeable", why
    params = ["ptr addrspace(1) %buf"]
    md = []
    if "texture" in taken:
        params.append("ptr addrspace(1) %t")
        md.append(TEXTURE_MD % texture_kind(name))
    if "sampler" in taken:
        params.append("ptr addrspace(2) %s")
        md.append(SAMPLER_MD)
    src = SKELETON % dict(
        decl=decl, body=body, params=", ".join(params),
        args=", ".join("!%d" % (7 + i) for i in range(1 + len(md))),
        argmd="\n".join("!%d = %s" % (8 + i, m) for i, m in enumerate(md)))
    ll, bc = os.path.join(tmp, "dt.ll"), os.path.join(tmp, "dt.bc")
    with open(ll, "w") as f:
        f.write(src)
    q = subprocess.run(["llvm-as", "-o", bc, ll], capture_output=True)
    if q.returncode:
        return "other", "llvm-as: " + q.stderr.decode().strip().splitlines()[0]
    q = subprocess.run([AIR2NIR, "--eval", bc], capture_output=True)
    text = (q.stdout + b"\n" + q.stderr).decode("utf-8", "replace")
    # Every refusal of a call is worded "intrinsic: ...", whether it names the
    # operation (air.fast_acos.f32) or what it could not make of the call
    # (argument 5 is not a constant, when the cube signature has no offset
    # vector where the 2d one does). Both are the frontend declining the call.
    m = re.search(r"intrinsic: ([^\n]*)", text)
    if m:
        return "intrinsic", m.group(1).strip()
    if q.returncode == 0 and "eval: the outputs" in text:
        got = ""
        for line in text.splitlines():
            if line.strip().startswith("buffer0"):
                got = line.split(None, 1)[1].strip()
        return "ok", got
    err = [l for l in text.strip().splitlines() if l.strip()]
    return "other", (err[-1][:160] if err else "no message")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("decls")
    ap.add_argument("--corpus", help="a corpus.py --json file, to weight each name")
    ap.add_argument("--md")
    ap.add_argument("--json")
    args = ap.parse_args()

    weight = collections.Counter()
    if args.corpus:
        with open(args.corpus) as f:
            for row in json.load(f)["rows"]:
                for c in row.get("calls", []):
                    weight[c] += 1

    decls = [l.strip() for l in open(args.decls) if l.strip()]
    tmp = tempfile.mkdtemp(prefix="decltry.")
    rows = []
    for d in decls:
        m = DECL.match(d)
        if not m:
            continue
        retty, name, argstr = m.group(1), m.group(2), m.group(3)
        argtys = [a for a in argstr.split(",") if a.strip()] if argstr.strip() else []
        verdict, detail = probe(d, name, retty, argtys, tmp)
        rows.append(dict(name=name, verdict=verdict, detail=detail,
                         modules=weight.get(name, 0)))

    by = collections.Counter(r["verdict"] for r in rows)
    out = ["# What the frontend does with each declaration\n",
           "Generated by `tools/air/decltry.py`: one shader per operation, called on "
           "constants, result stored. See that file for what a verdict is worth.\n",
           "| declarations | verdict |", "| --- | --- |"]
    for v, n in by.most_common():
        out.append("| %d | %s |" % (n, v))
    out.append("")
    for v in ("intrinsic", "other", "not probeable", "ok"):
        rs = [r for r in rows if r["verdict"] == v]
        if not rs:
            continue
        out.append("## %s: %d\n" % (v, len(rs)))
        if args.corpus:
            out.append("| modules calling it | name | detail |")
            out.append("| --- | --- | --- |")
            for r in sorted(rs, key=lambda r: (-r["modules"], r["name"])):
                out.append("| %d | `%s` | %s |" % (r["modules"], r["name"],
                                                   (r["detail"] or "").replace("|", "\\|")))
        else:
            out.append("| name | detail |")
            out.append("| --- | --- |")
            for r in sorted(rs, key=lambda r: r["name"]):
                out.append("| `%s` | %s |" % (r["name"], (r["detail"] or "").replace("|", "\\|")))
        out.append("")
    text = "\n".join(out)
    if args.md:
        with open(args.md, "w") as f:
            f.write(text)
    else:
        print(text)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(rows, f, indent=1)
    print("decltry: %s" % ", ".join("%s %d" % (v, n) for v, n in by.most_common()),
          file=sys.stderr)


if __name__ == "__main__":
    main()
