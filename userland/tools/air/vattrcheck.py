#!/usr/bin/env python3
"""Vertex attributes: `[[stage_in]]` / `[[attribute(n)]]`, which no value here can draw.

A vertex shader that reads its vertices from a vertex BUFFER -- Apple's three in
the GL renderer, all of QuartzCore's path and blit shaders -- needs nothing of
the API but a buffer binding. One with a `[[stage_in]]` input needs the whole
MTLVertexDescriptor: a format, an offset, a stride, a buffer index and a step
function per attribute, none of which is in the module. So what is decidable on
this machine is the SHADER's half of that match, and this file checks exactly
that and says plainly that it draws nothing:

  1. the attribute index reaches the hardware's fetch table. ir3 prints its
     variant's input table as `r0.x (VERT_ATTRIB_GENERICn slot=S cm=mask)`, and
     those three fields are the ones `fd6_program.cc:emit_vfd_dest` turns into
     VFD_CONTROL_0.fetch_cnt and VFD_DEST_CNTL_INSTR -- the write mask and the
     destination register of the fetch. An index that landed one slot over, or a
     float2 fetched as four components, shows there.
  2. the index is Metal's `[[attribute(n)]]` and not a position in the argument
     list. QuartzCore's std_vert0_lpf declares 0, 1 and 3, so the two readings
     differ for every real module and a translator that counted would pass
     every synthetic case with one attribute.
  3. the NIR input is 32 bits wide whatever the member's type, and the
     narrowing is an instruction. ir3 will happily put a 16-bit vertex input in
     the HALF register file -- a `half4` attribute compiles to `@in(hr0.x)` --
     while `ir3_shader_variant.inputs[]`, which has no half flag, reports it as
     `r0.x`, which is the number fd6 programs the fetch's destination from. So
     the fetch would write r0 and the shader would read hr0.
  4. what must be REFUSED by name rather than drawn over the wrong vertex: an
     attribute index past NIR's sixteen generic ones (index 16 used to translate
     onto VERT_ATTRIB_EDGEFLAG and index 30 past the end of the enum, both
     silently and both through ir3), an attribute spanning several indices, one
     with no index at all, and a type no MTLVertexFormat produces.
  5. an `air.arg_unused` attribute makes no NIR input at all -- it costs a fetch
     slot and a binding the application's descriptor may legally not have.

and then the same translation against Apple's own metadata, read a second time
and a different way: the fourteen modules of QuartzCore's library that carry
`air.vertex_input` are parsed out of `air2nir --ll` with a regular expression
and compared attribute by attribute with what `air2nir --interface` says
`air_shader_info` holds. The generated cases say the rules are implemented; the
real modules say the rules are the ones Apple's compiler writes.

NO VALUE IS COMPARED HERE and none can be: air_run's only vertex stage is the
generated quad (air_run.c, quad_vertex_shader), so there is no path on this
machine that draws with a translated vertex shader, and the contract the Metal
driver uses carries no vertex layout at all yet (docs/air.md, docs/metal.md
10.18). What a fetched attribute is worth is a question for the phone.

usage: vattrcheck.py [-v]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AIR2NIR = os.environ.get(
    "AIR2NIR", os.path.join(ROOT, "build/mesa-host/build/src/compiler/air/air2nir"))
IR3 = os.environ.get(
    "IR3", os.path.join(ROOT, "build/mesa-host/build/src/gallium/drivers/freedreno/ir3_compiler"))
CHIP_ID = os.environ.get("AIR_CHIP_ID", "0x007506030500")
IOS = os.environ.get("IOS", os.path.join(ROOT, "ios165"))
QUARTZCORE = os.path.join(
    IOS, "rootfs/System/Library/Frameworks/QuartzCore.framework/default.metallib")

# VERT_ATTRIB_GENERIC0 (shader_enums.h): the number ir3 prints as `slot=`.
GENERIC0 = 15

SKELETON = """source_filename = "vattr"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

%(globals)s
define <{ <4 x float> }> @vattr(%(params)s) local_unnamed_addr {
%(body)s
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.vertex = !{!3}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @vattr, !4, !6}
!4 = !{!5}
!5 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!6 = !{%(inputs)s}
%(input_nodes)s
"""


def module(attrs, body, gate=None):
    """attrs: a list of (type name, LLVM type, index, unused) in parameter order.

    The index is `n` (which carries Apple's own span of 1), a pair (n, span), or
    None for a node with no air.location_index at all. gate maps a parameter
    number to the name of an implicit function-constant predicate, which Apple's
    compiler spells as a nested node before the air.vertex_input key.
    """
    params = ", ".join("%s %%a%d" % (llty, i) for i, (_, llty, _, _) in enumerate(attrs))
    inputs = ", ".join("!%d" % (10 + i) for i in range(len(attrs)))
    nodes, globs, mds = [], [], []
    for i, (tn, _, idx, unused) in enumerate(attrs):
        if idx is None:
            loc = ''
        else:
            n, span = idx if isinstance(idx, tuple) else (idx, 1)
            loc = ', !"air.location_index", i32 %d, i32 %d' % (n, span)
        fc = ''
        if gate and i in gate:
            fc = '!"air.function_constant", !%d, ' % (40 + i)
            globs.append('@fcp%d = external addrspace(2) global i8' % i)
            mds.append('!%d = !{ptr addrspace(2) @fcp%d, !"bool", !"%s"}'
                       % (40 + i, i, gate[i]))
        nodes.append('!%d = !{i32 %d, %s!"air.vertex_input"%s, '
                     '!"air.arg_type_name", !"%s", !"air.arg_name", !"a%d"%s}'
                     % (10 + i, i, fc, loc, tn, i, ', !"air.arg_unused"' if unused else ''))
    return SKELETON % dict(params=params, body=body, inputs=inputs,
                           globals="\n".join(globs),
                           input_nodes="\n".join(nodes + mds))


PASS4 = ("  %r0 = insertvalue <{ <4 x float> }> undef, <4 x float> %a0, 0\n"
         "  ret <{ <4 x float> }> %r0")


def splat(src):
    """whatever `src` names, broadcast into the float4 position"""
    return ("  %v = insertelement <4 x float> undef, float " + src + ", i32 0\n"
            "  %w = shufflevector <4 x float> %v, <4 x float> undef, <4 x i32> zeroinitializer\n"
            "  %r0 = insertvalue <{ <4 x float> }> undef, <4 x float> %w, 0\n"
            "  ret <{ <4 x float> }> %r0")


# Each case: (label, module source, what must be true).
#
# `attrs` in a want is {metal index: (components, bits, base type, used)} and is
# checked against BOTH air_shader_info and, for the used ones, ir3's input
# table; `ir3` is {metal index: component mask} as ir3 prints it.
CASES = [
    ("float4 at attribute 0",
     module([("float4", "<4 x float>", 0, False)], PASS4),
     dict(attrs={0: (4, 32, "float", True)}, ir3={0: 0xf})),

    # A two-component attribute at an index that is not zero: the write mask and
    # the slot are the two things fd6 programs the fetch from, and a float2 read
    # as four components would take a channel of whatever follows it in the
    # vertex buffer.
    ("float2 at attribute 5",
     module([("float2", "<2 x float>", 5, False)],
            "  %e = shufflevector <2 x float> %a0, <2 x float> undef, "
            "<4 x i32> <i32 0, i32 1, i32 0, i32 1>\n"
            "  %r0 = insertvalue <{ <4 x float> }> undef, <4 x float> %e, 0\n"
            "  ret <{ <4 x float> }> %r0"),
     dict(attrs={5: (2, 32, "float", True)}, ir3={5: 0x3})),

    # std_vert0_lpf's own shape: indices 0, 1 and 3, the middle one unused. The
    # index must not be the position in the list -- `a2` is the third argument
    # and attribute 3 -- and the unused one must make no input at all.
    ("attributes 0, 1 (unused) and 3, as std_vert0_lpf declares them",
     module([("float4", "<4 x float>", 0, False),
             ("float2", "<2 x float>", 1, True),
             ("float4", "<4 x float>", 3, False)],
            "  %e = fadd <4 x float> %a0, %a2\n"
            "  %r0 = insertvalue <{ <4 x float> }> undef, <4 x float> %e, 0\n"
            "  ret <{ <4 x float> }> %r0"),
     dict(attrs={0: (4, 32, "float", True), 1: (2, 32, "float", False),
                 3: (4, 32, "float", True)},
          ir3={0: 0xf, 3: 0xf}, attributes_read=0x9)),

    # half4: the input must be 32 bits with an f2f16 after it, NOT an f16vec4
    # input. ir3 is willing to do the other thing and fd6 cannot program it.
    ("half4 at attribute 1 (32-bit input, narrowed)",
     module([("half4", "<4 x half>", 1, False)],
            "  %e = fpext <4 x half> %a0 to <4 x float>\n"
            "  %r0 = insertvalue <{ <4 x float> }> undef, <4 x float> %e, 0\n"
            "  ret <{ <4 x float> }> %r0"),
     dict(attrs={1: (4, 16, "float", True)}, ir3={1: 0xf},
          nir_has=["vec4 a0 (VERT_ATTRIB_GENERIC1", "f2f16"],
          nir_hasnt=["f16vec4 a0"])),

    # ushort2, the shape a packed texture coordinate has. 16-bit integer, so the
    # narrowing is a truncation and not a conversion.
    ("ushort2 at attribute 2",
     module([("ushort2", "<2 x i16>", 2, False)],
            "  %s = add <2 x i16> %a0, <i16 7, i16 7>\n"
            "  %f = uitofp <2 x i16> %s to <2 x float>\n"
            "  %e = shufflevector <2 x float> %f, <2 x float> undef, "
            "<4 x i32> <i32 0, i32 1, i32 0, i32 1>\n"
            "  %r0 = insertvalue <{ <4 x float> }> undef, <4 x float> %e, 0\n"
            "  ret <{ <4 x float> }> %r0"),
     dict(attrs={2: (2, 16, "uint", True)}, ir3={2: 0x3},
          nir_has=["ivec2 a0 (VERT_ATTRIB_GENERIC2", "u2u16"])),

    # uchar4, which is what every engine in the world sends a vertex colour as,
    # and which CRASHED this frontend before this file existed: the input
    # variable was built 32-bit while the argument was `<4 x i8>`, and the first
    # 8-bit instruction asserted in nir_builder
    # ("src_bit_size == bit_size", nir_builder.c:93). The 8-bit add is here so
    # that the narrowing has to be real.
    ("uchar4 at attribute 3, with 8-bit arithmetic",
     module([("uchar4", "<4 x i8>", 3, False)],
            "  %s = add <4 x i8> %a0, <i8 3, i8 3, i8 3, i8 3>\n"
            "  %e = uitofp <4 x i8> %s to <4 x float>\n"
            "  %r0 = insertvalue <{ <4 x float> }> undef, <4 x float> %e, 0\n"
            "  ret <{ <4 x float> }> %r0"),
     dict(attrs={3: (4, 8, "uint", True)}, ir3={3: 0xf},
          nir_has=["ivec4 a0 (VERT_ATTRIB_GENERIC3", "u2u8"])),

    # int4 and uint4 are the SAME `<4 x i32>`: the sign is only in Apple's
    # reflection string, and a runtime matching MTLVertexFormatInt4 against
    # MTLVertexFormatUInt4 has nothing else to go on.
    ("int4 and uint4 differ only in air.arg_type_name",
     module([("int4", "<4 x i32>", 0, False), ("uint4", "<4 x i32>", 1, False)],
            "  %s = add <4 x i32> %a0, %a1\n"
            "  %e = sitofp <4 x i32> %s to <4 x float>\n"
            "  %r0 = insertvalue <{ <4 x float> }> undef, <4 x float> %e, 0\n"
            "  ret <{ <4 x float> }> %r0"),
     dict(attrs={0: (4, 32, "int", True), 1: (4, 32, "uint", True)},
          ir3={0: 0xf, 1: 0xf})),

    # A scalar attribute, as fixed_vert_lpf_spc's point_size is.
    ("float at attribute 4",
     module([("float", "float", 4, False)], splat("%a0")),
     dict(attrs={4: (1, 32, "float", True)}, ir3={4: 0x1})),

    # NIR has sixteen generic vertex attributes and Metal has thirty-one slots.
    # Unchecked, this one translated onto VERT_ATTRIB_EDGEFLAG and compiled.
    # An attribute whose read is gated by a function-constant predicate, the
    # shape fixed_vert_lp{f,h}_spc carry. It must translate -- the parameter is
    # there whatever the predicate says -- and it must SAY SO, because a runtime
    # that demanded a binding for every bit of vertex_attributes_read would
    # refuse a pipeline whose constants make the attribute absent.
    ("attribute 1 gated by a function constant (must note)",
     module([("float4", "<4 x float>", 0, False),
             ("float2", "<2 x float>", 1, False)],
            "  %r0 = insertvalue <{ <4 x float> }> undef, <4 x float> %a0, 0\n"
            "  ret <{ <4 x float> }> %r0",
            gate={1: "vtx_attr_has_texcoords"}),
     dict(note="vtx_attr_has_texcoords", fc={1: "vtx_attr_has_texcoords"})),

    ("attribute 16 (must be refused)",
     module([("float4", "<4 x float>", 16, False)], PASS4),
     dict(refuse="vertex attribute 16")),

    # ... and this one past the end of the enum, where nir_print_shader prints
    # the location as UNKNOWN. It compiled too.
    ("attribute 30 (must be refused)",
     module([("float4", "<4 x float>", 30, False)], PASS4),
     dict(refuse="vertex attribute 30")),

    # The integer after the index. Every one of the 974 air.location_index nodes
    # in the five libraries carries 1, so its meaning -- the number of
    # consecutive indices the argument takes, as it is for an array of
    # resources -- is INFERRED and nothing in reach exercises another value. A
    # module that did would otherwise be read from the first index only.
    ("an attribute spanning two indices (must be refused)",
     module([("float4", "<4 x float>", (0, 2), False)], PASS4),
     dict(refuse="spanning 2 attribute indices")),

    ("an attribute with no index at all (must be refused)",
     module([("float4", "<4 x float>", None, False)], PASS4),
     dict(refuse="no air.location_index")),

    # A matrix member. Metal gives a matrix one attribute index per column, so
    # this is the shape the span above would carry; either way it is not one
    # fetch and must not be read as one.
    ("a float4x2 attribute (must be refused)",
     module([("float4x2", "[2 x <4 x float>]", 0, False),
             ("float4", "<4 x float>", 1, False)],
            "  %r0 = insertvalue <{ <4 x float> }> undef, <4 x float> %a1, 0\n"
            "  ret <{ <4 x float> }> %r0"),
     dict(refuse="neither a scalar nor a vector")),

    # No MTLVertexFormat is 64 bits wide, and MSL has no double at all.
    # Two members at one index: two inputs at one NIR location, two fetches into
    # one register, and an element array longer than the set it describes.
    ("two attributes at index 2 (must be refused)",
     module([("float4", "<4 x float>", 2, False), ("float2", "<2 x float>", 2, False)],
            PASS4),
     dict(refuse="two vertex inputs at attribute 2")),

    ("a long4 attribute (must be refused)",
     module([("long4", "<4 x i64>", 0, False)],
            "  %t = trunc <4 x i64> %a0 to <4 x i32>\n"
            "  %e = sitofp <4 x i32> %t to <4 x float>\n"
            "  %r0 = insertvalue <{ <4 x float> }> undef, <4 x float> %e, 0\n"
            "  ret <{ <4 x float> }> %r0"),
     dict(refuse="of type long4")),
]


def assemble(ll, work, n):
    src = os.path.join(work, "v%d.ll" % n)
    bc = os.path.join(work, "v%d.bc" % n)
    with open(src, "w") as f:
        f.write(ll)
    q = subprocess.run(["llvm-as", "-o", bc, src], capture_output=True)
    if q.returncode != 0:
        return None, "llvm-as: " + q.stderr.decode().strip().splitlines()[0]
    return bc, None


def interface(bc, entry=None):
    """air_shader_info, as air2nir --interface prints it."""
    cmd = [AIR2NIR, "--interface", bc] + ([entry] if entry else [])
    q = subprocess.run(cmd, capture_output=True)
    if q.returncode != 0:
        return None, q.stderr.decode().strip()
    attrs, fields = [], {}
    for line in q.stdout.decode().splitlines():
        parts = line.split("\t")
        kv = dict(p.split("=", 1) for p in parts if "=" in p)
        if parts[0] == "attribute":
            attrs.append(kv)
        else:
            fields.update(kv)
    return (attrs, fields), None


def ir3_inputs(bc, verbose):
    """{slot: compmask} out of ir3's own variant input table, and the assembly."""
    q = subprocess.run([IR3, "-c", CHIP_ID, bc], capture_output=True)
    asm = q.stdout.decode()
    if verbose:
        print(asm)
    if q.returncode != 0:
        return None, "ir3: " + (q.stderr.decode().strip().splitlines() or ["failed"])[-1]
    line = [l for l in asm.splitlines() if ": inputs:" in l]
    if not line:
        return None, "ir3 compiled it and printed no input table"
    got = {}
    for slot, cm in re.findall(r"slot=(\d+) cm=([0-9a-f]+)", line[0]):
        got[int(slot)] = int(cm, 16)
    return got, None


def check(ll, want, work, n, verbose):
    bc, err = assemble(ll, work, n)
    if err:
        return False, err

    q = subprocess.run([AIR2NIR, bc], capture_output=True)
    nir = q.stdout.decode()
    why = q.stderr.decode().strip()
    if verbose:
        print(nir or why)

    if "note" in want:
        r = subprocess.run([AIR2NIR, "--report", bc], capture_output=True)
        line = r.stdout.decode().strip()
        if "translate=ok" not in line:
            return False, "did not translate: " + (why or line)
        if "\tnotes=" not in line:
            return False, "translated with no note at all, so the caveat is dropped"
        note = line.split("\tnotes=", 1)[1].split("\t")[0]
        if want["note"] not in note:
            return False, "noted, but without %r: %s" % (want["note"], note)
        info, err = interface(bc)
        if err:
            return False, "--interface: " + err
        attrs, _ = info
        got = {int(a["index"]): a["fc"] for a in attrs if a["fc"] != "-"}
        if got != want["fc"]:
            return False, "the gate recorded is %r, wanted %r" % (got, want["fc"])
        return True, "noted, and the predicate is in air_shader_info: " + note[:60]

    if "refuse" in want:
        if q.returncode == 0:
            return False, "translated, and it should have been refused"
        if want["refuse"] not in why:
            return False, "refused, but for %r" % why
        return True, "refused: " + why.split(".bc: ", 1)[-1]

    if q.returncode != 0:
        return False, "air2nir: " + (why or "no output")

    # nir_validate, which only --report runs
    r = subprocess.run([AIR2NIR, "--report", bc], capture_output=True)
    if "translate=ok" not in r.stdout.decode():
        return False, "did not validate: " + r.stdout.decode().strip()

    for s in want.get("nir_has", []):
        if s not in nir:
            return False, "no %r in the NIR" % s
    for s in want.get("nir_hasnt", []):
        if s in nir:
            return False, "%r is in the NIR and should not be" % s

    info, err = interface(bc)
    if err:
        return False, "--interface: " + err
    attrs, fields = info
    got = {int(a["index"]): (int(a["components"]), int(a["bits"]), a["type"],
                             a["used"] == "yes") for a in attrs}
    if got != want["attrs"]:
        return False, "air_shader_info says %r, wanted %r" % (got, want["attrs"])

    read = want.get("attributes_read")
    if read is None:
        read = sum(1 << i for i, v in want["attrs"].items() if v[3])
    if int(fields["vertex_attributes_read"], 16) != read:
        return False, "vertex_attributes_read=%s, wanted 0x%x" % (
            fields["vertex_attributes_read"], read)

    # nir_shader_info.inputs_read, one bit per VERT_ATTRIB_GENERIC the shader
    # reads: what a state tracker asks, and what nir_shader_gather_info would
    # have set had anything run it on this path.
    ir = sum(1 << (GENERIC0 + i) for i, v in want["attrs"].items() if v[3])
    if int(fields["inputs_read"], 16) != ir:
        return False, "inputs_read=%s, wanted 0x%x" % (fields["inputs_read"], ir)

    # Gallium matches vertex elements to shader inputs BY POSITION, so the
    # driver_location of the used attributes must be 0, 1, 2 ... in the order
    # AIR lists them with the unused ones skipped. A hole here would make the
    # runtime feed attribute k's element to a different attribute entirely.
    want_dl = 0
    for a in attrs:
        dl = int(a["driver_location"])
        if a["used"] == "no":
            if dl != -1:
                return False, "unused attribute %s has driver_location %d" % (a["index"], dl)
            continue
        if dl != want_dl:
            return False, ("attribute %s has driver_location %d, and Gallium's element %d "
                           "is what would feed it" % (a["index"], dl, want_dl))
        want_dl += 1

    ir3, err = ir3_inputs(bc, verbose)
    if err:
        return False, err
    wanted = {GENERIC0 + i: m for i, m in want["ir3"].items()}
    if ir3 != wanted:
        return False, ("ir3 fetches %r, wanted %r (slot = VERT_ATTRIB_GENERIC0 + n = %d + n)"
                       % (ir3, wanted, GENERIC0))
    return True, "fetched: " + ", ".join("GENERIC%d cm=%x" % (s - GENERIC0, m)
                                         for s, m in sorted(ir3.items()))


# -- the real modules -------------------------------------------------------
#
# A second reading of Apple's own metadata, done differently on purpose: a
# regular expression over `air2nir --ll` against the frontend's walk of the
# bitcode's metadata records. The two agree or one of them is wrong.
VINPUT = re.compile(r'!\{i32 (\d+), (.*?"air\.vertex_input".*?)\}\s*$')
LOCATION = re.compile(r'"air\.location_index", i32 (\d+), i32 (\d+)')
TYPENAME = re.compile(r'"air\.arg_type_name", !"([^"]+)"')
ARGNAME = re.compile(r'"air\.arg_name", !"([^"]+)"')

# `float4` -> (components, bits, base type), from the name Apple writes. Only
# the forms the five libraries' stage-in members actually use are here; a new
# one must be added rather than guessed at, which is why there is no parser.
DECL = {
    "float": (1, 32, "float"), "float2": (2, 32, "float"),
    "float3": (3, 32, "float"), "float4": (4, 32, "float"),
    "half": (1, 16, "float"), "half2": (2, 16, "float"),
    "half3": (3, 16, "float"), "half4": (4, 16, "float"),
}


def real_modules(verbose):
    """Returns (checked, failures) over QuartzCore's [[stage_in]] vertex shaders."""
    q = subprocess.run([AIR2NIR, "--list", QUARTZCORE], capture_output=True)
    if q.returncode != 0:
        print("     %s" % q.stderr.decode().strip())
        return 0, 1
    names = [l.split()[0] for l in q.stdout.decode().splitlines()
             if len(l.split()) > 1 and l.split()[1] == "vertex"]

    checked = fails = 0
    attributes = unused = 0
    for name in names:
        ll = subprocess.run([AIR2NIR, "--ll", QUARTZCORE, name],
                            capture_output=True).stdout.decode()
        apple = []
        for line in ll.splitlines():
            m = VINPUT.search(line)
            if not m:
                continue
            body = m.group(2)
            loc = LOCATION.search(body)
            tn = TYPENAME.search(body)
            apple.append(dict(param=int(m.group(1)),
                              index=int(loc.group(1)) if loc else None,
                              span=int(loc.group(2)) if loc else None,
                              type_name=tn.group(1) if tn else None,
                              name=(ARGNAME.search(body) or [None, "-"])[1]
                                   if ARGNAME.search(body) else "-",
                              unused='"air.arg_unused"' in body))
        if not apple:
            continue
        checked += 1
        attributes += len(apple)
        unused += sum(1 for a in apple if a["unused"])

        info, err = interface(QUARTZCORE, name)
        if err:
            print("FAIL %-32s --interface: %s" % (name, err.splitlines()[-1][:70]))
            fails += 1
            continue
        got, fields = info
        if len(got) != len(apple):
            print("FAIL %-32s %d attributes translated, Apple's metadata has %d"
                  % (name, len(got), len(apple)))
            fails += 1
            continue
        bad = []
        for a, g in zip(apple, got):
            want = DECL.get(a["type_name"])
            if not want:
                bad.append("%s: no declared shape for %s" % (a["name"], a["type_name"]))
                continue
            if int(g["index"]) != a["index"]:
                bad.append("%s: index %s, metadata says %s" % (a["name"], g["index"],
                                                               a["index"]))
            if (int(g["components"]), int(g["bits"]), g["type"]) != want:
                bad.append("%s: %s/%s/%s, %s means %r"
                           % (a["name"], g["components"], g["bits"], g["type"],
                              a["type_name"], want))
            if (g["used"] == "yes") == a["unused"]:
                bad.append("%s: used=%s, metadata says %s" % (a["name"], g["used"],
                            "unused" if a["unused"] else "used"))
            if g["decl"] != a["type_name"]:
                bad.append("%s: type name %s, metadata says %s" % (a["name"], g["decl"],
                                                                   a["type_name"]))
        want_dl = 0
        for a, g in zip(apple, got):
            dl = int(g["driver_location"])
            if a["unused"]:
                if dl != -1:
                    bad.append("%s: unused, driver_location %d" % (a["name"], dl))
                continue
            if dl != want_dl:
                bad.append("%s: driver_location %d, Gallium's element %d would feed it"
                           % (a["name"], dl, want_dl))
            want_dl += 1
        read = sum(1 << a["index"] for a in apple if not a["unused"])
        if int(fields["vertex_attributes_read"], 16) != read:
            bad.append("vertex_attributes_read=%s, the metadata's used set is 0x%x"
                       % (fields["vertex_attributes_read"], read))
        if bad:
            print("FAIL %-32s %s" % (name, "; ".join(bad)))
            fails += 1
            continue
        if verbose:
            print("     %-32s %s" % (name, " ".join(
                "%s@%s%s" % (a["type_name"], a["index"], "(unused)" if a["unused"] else "")
                for a in apple)))

    print("     %d of %d vertex modules carry air.vertex_input: %d attributes, "
          "%d marked air.arg_unused" % (checked, len(names), attributes, unused))
    print("     %d attributes make a NIR input and a VFD fetch; the other %d are in the "
          "interface and cost nothing" % (attributes - unused, unused))
    return checked, fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="print the NIR and the Adreno instructions of each case")
    args = ap.parse_args()

    for prog in (AIR2NIR, IR3):
        if not os.path.exists(prog):
            print("vattrcheck: run tools/air/mk-host.sh first (%s)" % prog, file=sys.stderr)
            return 2

    ok = 0
    with tempfile.TemporaryDirectory(prefix="vattrcheck") as work:
        for n, (label, ll, want) in enumerate(CASES):
            good, msg = check(ll, want, work, n, args.verbose)
            ok += good
            print("%-4s %-58s %s" % ("" if good else "FAIL", label, msg))
    print("vattrcheck: %d of %d cases as expected" % (ok, len(CASES)))

    print()
    print("== the same translation against Apple's own metadata, read a second way")
    real_fails = 0
    if not os.path.exists(QUARTZCORE):
        print("     NO REAL MODULE WAS CHECKED: %s is not in the image, so everything "
              "above is the generated cases only" % QUARTZCORE)
    else:
        checked, real_fails = real_modules(args.verbose)
        if checked == 0:
            print("     NO REAL MODULE WAS CHECKED: the library holds no vertex shader "
                  "with an air.vertex_input")
            real_fails = 1

    print("vattrcheck: no VALUE is compared here and none can be on this machine -- "
          "air_run's only vertex stage is its own generated quad, so nothing here draws "
          "with a translated vertex shader")
    return 0 if ok == len(CASES) and real_fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
