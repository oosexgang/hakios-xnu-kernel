#!/usr/bin/env python3
"""Check the texture operations one at a time, on more than one texel.

tools/air/texcheck.sh runs the corpus's own texture shaders, and 6 of its 55
return the exact texel on the Adreno. Two things it cannot do:

  - Most of the other 49 are air.visible library functions, which the harness
    cannot invoke as entry points, so whole operations -- the texel fetch, the
    size queries -- have no coverage at all from it.
  - Every one of them samples the same texel, (1, 1), because that is where
    air_run's vertex shader puts the coordinate. A lowering that ignored the
    coordinate and always fetched (1, 1) would pass all six.

So this generates a fragment shader per (operation, coordinate), the way
tools/air/intrincheck.py does for the arithmetic, and asks for several texels
including the corners. air_run fills texel (x, y, z) of slot n with
(100 + x + 1000n, 200 + y, 300 + x + y + z, 400), so every coordinate has a
different expected answer in all four channels and a confusion between any two
of them gives a wrong number rather than a plausible one.

Nothing is read from the corpus but the list of declarations, and the
coordinates are immediates here rather than loaded from a buffer: a texture
fetch is not constant-folded by either NIR or ir3, so there is nothing for the
compiler to answer in the shader's place. That is the difference from the
arithmetic, where the buffer was necessary.

What is NOT checked, and why:

  - Metal's own sampler state. air_run's sampler here is the API's -- nearest,
    clamped -- and the coordinates are texel centres, where nearest filtering
    has no weights to round, so this says nothing about filtering or
    addressing. That is now a separate check rather than a gap:
    tools/air/sampcheck.py samples with the shader's own constexpr sampler and
    compares against Apple's rules (docs/air.md section 3.3).
  - The integer-typed reads and samples (read_texture_2d.s.v4i16,
    sample_texture_2d.u.v4i32 and the rest). air_run binds one
    r32g32b32a32_float texture, and a shader declaring an integer texture
    against it is a type mismatch whose result would say nothing about the
    lowering. Binding a texture whose format follows the declaration is a
    change to air_run that is not made here.
  - write_texture_2d and the multisample reads: the first needs the texture
    read back, which air_run does not do, and the second a multisample
    texture, which it does not bind.
  - The depth sample: air_run binds no depth texture, and a shader reading an
    attachment nothing stored to is one of the four tests this port has
    already had pass for the wrong reason.

usage: texopcheck.py [--keep DIR] [-v]
"""

import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AIR_RUN = os.environ.get(
    "AIR_RUN",
    os.path.join(ROOT, "build/mesa-host/build/src/gallium/drivers/freedreno/air_run"),
)

TEX = 4   # air_run binds a TEX x TEX (x TEX) texture


def texel(x, y, z=0):
    """What air_run put at (x, y, z) of slot 0."""
    return [100 + x, 200 + y, 300 + x + y + z, 400]


SKELETON = """source_filename = "tex"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

%(decl)s

define <{ <4 x %(outty)s> }> @tex(%(params)s) local_unnamed_addr {
%(body)s
  %%out = insertvalue <{ <4 x %(outty)s> }> undef, <4 x %(outty)s> %%v, 0
  ret <{ <4 x %(outty)s> }> %%out
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @tex, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"%(outname)s", !"air.arg_name", !"out"}
!6 = !{%(args)s}
%(argmd)s
"""

TEXTURE_MD = ('!{i32 %d, !"air.texture", !"air.location_index", i32 0, i32 1, !"%s", '
              '!"air.arg_type_name", !"texture%s", !"air.arg_name", !"t"}')
SAMPLER_MD = ('!{i32 %d, !"air.sampler", !"air.location_index", i32 0, i32 1, '
              '!"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}')


def build(decl, call, needs_sampler, kind, access, outty="float"):
    """One shader: `call` computes %v from %t (and %s), `decl` declares it.

    outty is the render target's component type. It has to follow the
    declaration: air_run picks the texture's format from what the shader
    samples into (its own comment says why -- "a Metal texture2d<uint> read out
    of a float texture gives numbers that are neither right nor obviously
    wrong"), and the render target has to be able to hold the result.
    """
    params = ["ptr addrspace(1) %t"]
    md = [TEXTURE_MD % (0, access, kind)]
    if needs_sampler:
        params.append("ptr addrspace(2) %s")
        md.append(SAMPLER_MD % 1)
    return SKELETON % dict(
        decl=decl, params=", ".join(params), body=call, outty=outty,
        outname="float4" if outty == "float" else "int4",
        args=", ".join("!%d" % (7 + i) for i in range(len(md))),
        argmd="\n".join("!%d = %s" % (7 + i, m) for i, m in enumerate(md)))


# The declarations the system corpus does not carry, written out here.
#
# Every other case in this file takes its declaration from a module of the
# device image, which is what makes the signature a measurement rather than a
# claim. These four families are not in that image at all -- an array sample
# appears once, as the half-typed air.sample_texture_2d_array.v4f16, and the
# gradient and cube forms not at all -- while Resident Evil 4 calls them in
# 1 753 of its 7 879 modules. That library is not in this repository and must
# not be, so the signature is written here instead, from the two places it was
# read:
#
#   - Apple's own metal_stdlib on the device
#     (GPUCompiler.framework/.../include/metal/metal_texture), where each
#     texture method is a one-line forwarder to the builtin and so states the
#     argument order for every dimensionality;
#   - and `llvm-dis` of the modules that call them, which agrees with it
#     exactly.
#
# A shader built from one of these lines is this file's own, like every other
# shader here: nothing of the library it describes is in the line.
EXTRA_DECLS = [
    "declare { <4 x float>, i8 } @air.sample_texture_2d_array.v4f32("
    "ptr addrspace(1), ptr addrspace(2), <2 x float>, i32, i1, <2 x i32>, "
    "i1, float, float, i32)",
    "declare { <4 x float>, i8 } @air.read_texture_2d_array.v4f32("
    "ptr addrspace(1), <2 x i32>, i32, i32, i32)",
    "declare { <4 x float>, i8 } @air.gather_texture_2d_array.v4f32("
    "ptr addrspace(1), ptr addrspace(2), <2 x float>, i32, i1, <2 x i32>, "
    "i32, i32)",
    "declare { <4 x float>, i8 } @air.sample_texture_1d_array.v4f32("
    "ptr addrspace(1), ptr addrspace(2), float, i32, i1, i32, i1, float, "
    "float, i32)",
    "declare { <4 x float>, i8 } @air.sample_texture_2d_grad.v4f32("
    "ptr addrspace(1), ptr addrspace(2), <2 x float>, <2 x float>, "
    "<2 x float>, float, i1, <2 x i32>, i32)",
    "declare { <4 x float>, i8 } @air.sample_texture_2d_array_grad.v4f32("
    "ptr addrspace(1), ptr addrspace(2), <2 x float>, i32, <2 x float>, "
    "<2 x float>, float, i1, <2 x i32>, i32)",
]


def cases(decls):
    """Everything to try: (label, source, expected four floats)."""
    out = []
    have = {}
    for d in list(decls) + EXTRA_DECLS:
        m = re.match(r"^declare\s+(.*?)\s+@(air\.[A-Za-z0-9_.]+)\((.*)\)", d)
        if m:
            have[m.group(2)] = d

    # -- the texel fetch, at four corners and the middle of the 4x4 ---------
    name = "air.read_texture_2d.v4f32"
    if name in have:
        for (x, y) in [(0, 0), (1, 1), (2, 3), (3, 3), (3, 0)]:
            body = ('  %%r = tail call { <4 x float>, i8 } @%s(ptr addrspace(1) %%t, '
                    '<2 x i32> <i32 %d, i32 %d>, i32 0, i32 0)\n'
                    '  %%v = extractvalue { <4 x float>, i8 } %%r, 0' % (name, x, y))
            out.append(("%s (%d,%d)" % (name, x, y),
                        build(have[name], body, False, "2d<float, access::read>",
                              "air.read"),
                        texel(x, y), False))

    # the same fetch with 16-bit coordinates, which is a separate declaration
    name = "air.read_texture_2d.i16.v4f32"
    if name in have:
        for (x, y) in [(1, 1), (2, 3)]:
            body = ('  %%r = tail call { <4 x float>, i8 } @%s(ptr addrspace(1) %%t, '
                    '<2 x i16> <i16 %d, i16 %d>, i16 0, i32 0)\n'
                    '  %%v = extractvalue { <4 x float>, i8 } %%r, 0' % (name, x, y))
            out.append(("%s (%d,%d)" % (name, x, y),
                        build(have[name], body, False, "2d<float, access::read>",
                              "air.read"),
                        texel(x, y), False))

    # -- the size queries ---------------------------------------------------
    for name, want in (("air.get_width_texture_2d", TEX),
                       ("air.get_height_texture_2d", TEX)):
        if name not in have:
            continue
        body = ('  %%n = tail call i32 @%s(ptr addrspace(1) %%t, i32 0)\n'
                '  %%f = sitofp i32 %%n to float\n'
                '  %%v0 = insertelement <4 x float> undef, float %%f, i32 0\n'
                '  %%v1 = insertelement <4 x float> %%v0, float %%f, i32 1\n'
                '  %%v2 = insertelement <4 x float> %%v1, float %%f, i32 2\n'
                '  %%v = insertelement <4 x float> %%v2, float %%f, i32 3' % name)
        out.append((name, build(have[name], body, False,
                                "2d<float, access::read>", "air.read"),
                    [want] * 4, False))

    # -- sampling, at the centre of several texels --------------------------
    name = "air.sample_texture_2d.v4f32"
    if name in have:
        for (x, y) in [(0, 0), (1, 1), (2, 3), (3, 3)]:
            u, v = (x + 0.5) / TEX, (y + 0.5) / TEX
            body = ('  %%r = tail call { <4 x float>, i8 } @%s(ptr addrspace(1) %%t, '
                    'ptr addrspace(2) %%s, <2 x float> <float %.9e, float %.9e>, '
                    'i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, '
                    'float 0.0, i32 0)\n'
                    '  %%v = extractvalue { <4 x float>, i8 } %%r, 0' % (name, u, v))
            out.append(("%s (%d,%d)" % (name, x, y),
                        build(have[name], body, True, "2d<float, sample>",
                              "air.sample"),
                        texel(x, y), False))

    name = "air.sample_texture_3d.v4f32"
    if name in have:
        for (x, y, z) in [(0, 0, 0), (1, 1, 1), (2, 3, 1), (3, 3, 3)]:
            u, v, w = ((x + 0.5) / TEX, (y + 0.5) / TEX, (z + 0.5) / TEX)
            body = ('  %%r = tail call { <4 x float>, i8 } @%s(ptr addrspace(1) %%t, '
                    'ptr addrspace(2) %%s, <3 x float> <float %.9e, float %.9e, '
                    'float %.9e>, i1 true, <3 x i32> zeroinitializer, i1 false, '
                    'float 0.0, float 0.0, i32 0)\n'
                    '  %%v = extractvalue { <4 x float>, i8 } %%r, 0' % (name, u, v, w))
            out.append(("%s (%d,%d,%d)" % (name, x, y, z),
                        build(have[name], body, True, "3d<float, sample>",
                              "air.sample"),
                        texel(x, y, z), False))
    # -- the array textures --------------------------------------------------
    #
    # The layer is a separate argument after the coordinate in AIR and the last
    # component of one coordinate in NIR, so what these ask is whether it
    # arrives at the right place. air_run binds a real 2D array (four layers,
    # layer z holding 300+x+y+z in its third channel), so a lowering that
    # dropped the layer reads layer 0 and comes back with a number that is
    # wrong by exactly the layer -- and the coordinates below have a different
    # layer from their x and y, so a layer read out of the wrong component
    # cannot agree either.
    name = "air.sample_texture_2d_array.v4f32"
    if name in have:
        for (x, y, layer) in [(0, 0, 0), (1, 1, 2), (2, 3, 1), (3, 0, 3)]:
            u, v = (x + 0.5) / TEX, (y + 0.5) / TEX
            body = ('  %%r = tail call { <4 x float>, i8 } @%s(ptr addrspace(1) %%t, '
                    'ptr addrspace(2) %%s, <2 x float> <float %.9e, float %.9e>, '
                    'i32 %d, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, '
                    'float 0.0, i32 0)\n'
                    '  %%v = extractvalue { <4 x float>, i8 } %%r, 0' % (name, u, v, layer))
            out.append(("%s (%d,%d) layer %d" % (name, x, y, layer),
                        build(have[name], body, True, "2d_array<float, sample>",
                              "air.sample"),
                        texel(x, y, layer), False))

    name = "air.read_texture_2d_array.v4f32"
    if name in have:
        for (x, y, layer) in [(1, 1, 2), (2, 3, 3), (0, 0, 1), (3, 0, 0)]:
            body = ('  %%r = tail call { <4 x float>, i8 } @%s(ptr addrspace(1) %%t, '
                    '<2 x i32> <i32 %d, i32 %d>, i32 %d, i32 0, i32 0)\n'
                    '  %%v = extractvalue { <4 x float>, i8 } %%r, 0'
                    % (name, x, y, layer))
            out.append(("%s (%d,%d) layer %d" % (name, x, y, layer),
                        build(have[name], body, False,
                              "2d_array<float, access::read>", "air.read"),
                        texel(x, y, layer), False))

    # A 1D array is the shape where a mistake is invisible in the 2D one: the
    # coordinate is a SCALAR and the layer becomes the second component, so a
    # lowering that appended the layer to a vector it had not made yet, or put
    # it in component 0, still produces a two-component coordinate.
    name = "air.sample_texture_1d_array.v4f32"
    if name in have:
        for (x, layer) in [(0, 0), (1, 2), (3, 1), (2, 3)]:
            u = (x + 0.5) / TEX
            body = ('  %%r = tail call { <4 x float>, i8 } @%s(ptr addrspace(1) %%t, '
                    'ptr addrspace(2) %%s, float %.9e, i32 %d, i1 false, i32 0, '
                    'i1 false, float 0.0, float 0.0, i32 0)\n'
                    '  %%v = extractvalue { <4 x float>, i8 } %%r, 0' % (name, u, layer))
            out.append(("%s (%d) layer %d" % (name, x, layer),
                        build(have[name], body, True, "1d_array<float, sample>",
                              "air.sample"),
                        texel(x, 0, layer), False))

    name = "air.gather_texture_2d_array.v4f32"
    if name in have:
        u = 1.75 / TEX
        for comp in range(4):
            for layer in (0, 2):
                want = sorted(texel(x, y, layer)[comp] for x in (1, 2) for y in (1, 2))
                body = ('  %%r = tail call { <4 x float>, i8 } @%s(ptr addrspace(1) %%t, '
                        'ptr addrspace(2) %%s, <2 x float> <float %.9e, float %.9e>, '
                        'i32 %d, i1 true, <2 x i32> zeroinitializer, i32 %d, i32 0)\n'
                        '  %%v = extractvalue { <4 x float>, i8 } %%r, 0'
                        % (name, u, u, layer, comp))
                out.append(("%s channel %d layer %d" % (name, comp, layer),
                            build(have[name], body, True, "2d_array<float, sample>",
                                  "air.sample"),
                            want, True))

    # -- gradient sampling ---------------------------------------------------
    #
    # WHAT THIS CHECKS AND WHAT IT CANNOT. The gradients are given values that
    # are not the coordinate, not zero and not each other, so a lowering that
    # read one of them as the coordinate, as the offset vector or as the level
    # flag answers with a different texel or refuses. What it does NOT check is
    # the level of detail those gradients select: air_run's texture has one mip
    # level, so every level clamps to it, and a level computed from the wrong
    # pair of numbers would still read level 0. Checking that needs a
    # mipmapped texture and a sampler whose mip filter is not NONE, which is a
    # change to the harness that would change what every other check here
    # samples -- so it is named as open rather than quietly counted.
    name = "air.sample_texture_2d_grad.v4f32"
    if name in have:
        for (x, y) in [(0, 0), (1, 1), (2, 3), (3, 3)]:
            u, v = (x + 0.5) / TEX, (y + 0.5) / TEX
            body = ('  %%r = tail call { <4 x float>, i8 } @%s(ptr addrspace(1) %%t, '
                    'ptr addrspace(2) %%s, <2 x float> <float %.9e, float %.9e>, '
                    '<2 x float> <float 0x3FD0000000000000, float 0x3FE0000000000000>, '
                    '<2 x float> <float 0x3FC0000000000000, float 0x3FB0000000000000>, '
                    'float 0.0, i1 true, <2 x i32> zeroinitializer, i32 0)\n'
                    '  %%v = extractvalue { <4 x float>, i8 } %%r, 0' % (name, u, v))
            out.append(("%s (%d,%d)" % (name, x, y),
                        build(have[name], body, True, "2d<float, sample>",
                              "air.sample"),
                        texel(x, y), False))

    name = "air.sample_texture_2d_array_grad.v4f32"
    if name in have:
        for (x, y, layer) in [(1, 1, 0), (2, 3, 2), (0, 3, 3)]:
            u, v = (x + 0.5) / TEX, (y + 0.5) / TEX
            body = ('  %%r = tail call { <4 x float>, i8 } @%s(ptr addrspace(1) %%t, '
                    'ptr addrspace(2) %%s, <2 x float> <float %.9e, float %.9e>, '
                    'i32 %d, '
                    '<2 x float> <float 0x3FD0000000000000, float 0x3FE0000000000000>, '
                    '<2 x float> <float 0x3FC0000000000000, float 0x3FB0000000000000>, '
                    'float 0.0, i1 true, <2 x i32> zeroinitializer, i32 0)\n'
                    '  %%v = extractvalue { <4 x float>, i8 } %%r, 0'
                    % (name, u, v, layer))
            out.append(("%s (%d,%d) layer %d" % (name, x, y, layer),
                        build(have[name], body, True, "2d_array<float, sample>",
                              "air.sample"),
                        texel(x, y, layer), False))

    # -- the integer-typed reads and samples ---------------------------------
    #
    # air_run picks the texture's format from what the shader samples into, so
    # these get an R32G32B32A32_SINT or _UINT texture holding the same numbers
    # cast to int. The render target has to follow too, or the bits come back
    # read the wrong way.
    for name in sorted(have):
        m = re.match(r"^air\.(read|sample)_texture_(2d|3d)"
                     r"(\.i16)?\.([su])\.v4i(16|32)$", name)
        if not m:
            continue
        kindop, nd, narrow_coord, sign, width = m.groups()
        n = 3 if nd == "3d" else 2
        rty = "<4 x i%s>" % width
        decl = have[name]
        for coord in ([(1, 1, 1), (2, 3, 1)] if nd == "3d" else [(1, 1), (2, 3)]):
            if kindop == "read":
                cty = "i16" if narrow_coord else "i32"
                cv = ", ".join("%s %d" % (cty, v) for v in coord[:n])
                body = ('  %%r = tail call { %s, i8 } @%s(ptr addrspace(1) %%t, '
                        '<%d x %s> <%s>, %s 0, i32 0)\n'
                        % (rty, name, n, cty, cv, cty))
                access, kind = "air.read", "%s<%s, access::read>" % (
                    nd, "int" if sign == "s" else "uint")
            else:
                cv = ", ".join("float %.9e" % ((v + 0.5) / TEX) for v in coord[:n])
                body = ('  %%r = tail call { %s, i8 } @%s(ptr addrspace(1) %%t, '
                        'ptr addrspace(2) %%s, <%d x float> <%s>, i1 true, '
                        '<%d x i32> zeroinitializer, i1 false, float 0.0, '
                        'float 0.0, i32 0)\n' % (rty, name, n, cv, n))
                access, kind = "air.sample", "%s<%s, sample>" % (
                    nd, "int" if sign == "s" else "uint")
            body += '  %%t0 = extractvalue { %s, i8 } %%r, 0\n' % rty
            if width == "16":
                # widen to the render target, by the declaration's own sign
                body += ('  %%v = %s <4 x i16> %%t0 to <4 x i32>'
                         % ("sext" if sign == "s" else "zext"))
            else:
                body += '  %v = bitcast <4 x i32> %t0 to <4 x i32>'
            out.append(("%s %s" % (name, coord[:n]),
                        build(decl, body, kindop == "sample", kind, access, "i32"),
                        texel(*coord[:3] if nd == "3d" else (coord[0], coord[1])),
                        False))

    # -- the gather ----------------------------------------------------------
    #
    # A gather returns one channel of the four texels around the coordinate.
    # Metal fixes the order they come back in and this does not know it, so the
    # four values are compared as a multiset: that checks the footprint and the
    # channel selection without committing to an order convention, and every
    # channel gives a different multiset, so a wrong channel still shows.
    #
    # The coordinate is 1.75/4, so in texel space it is 1.25 and the footprint
    # is x,y in {1, 2} with nothing ambiguous about the rounding.
    for name in sorted(have):
        m = re.match(r"^air\.gather_texture_2d(\.[su])?\.v4(f32|f16|i32|i16)$", name)
        if not m:
            continue
        sign, width = m.group(1), m.group(2)
        rty = "<4 x %s>" % {"f32": "float", "f16": "half",
                            "i32": "i32", "i16": "i16"}[width]
        outty = "float" if width.startswith("f") else "i32"
        base = {"f32": "float", "f16": "half",
                "i32": "uint" if sign == ".u" else "int",
                "i16": "uint" if sign == ".u" else "int"}[width]
        u = 1.75 / TEX
        for comp in range(4):
            want = sorted(texel(x, y)[comp] for x in (1, 2) for y in (1, 2))
            body = ('  %%r = tail call { %s, i8 } @%s(ptr addrspace(1) %%t, '
                    'ptr addrspace(2) %%s, <2 x float> <float %.9e, float %.9e>, '
                    'i1 true, <2 x i32> zeroinitializer, i32 %d, i32 0)\n'
                    % (rty, name, u, u, comp))
            body += '  %%g = extractvalue { %s, i8 } %%r, 0\n' % rty
            if width == "f16":
                body += '  %v = fpext <4 x half> %g to <4 x float>'
            elif width == "i16":
                body += ('  %%v = %s <4 x i16> %%g to <4 x i32>'
                         % ("zext" if sign == ".u" else "sext"))
            elif width == "i32":
                body += '  %v = bitcast <4 x i32> %g to <4 x i32>'
            else:
                body += '  %v = bitcast <4 x float> %g to <4 x float>'
            out.append(("%s channel %d" % (name, comp),
                        build(have[name], body, True, "2d<%s, sample>" % base,
                              "air.sample", outty),
                        want, True))

    # -- the half-typed reads and samples ------------------------------------
    #
    # These belong with the rest rather than in a list of things with no case:
    # they are generated, they are correct as far as this can tell, and
    # softpipe sets them aside for the one reason it sets aside every 16-bit
    # operation. On ir3 they are a real question.
    for name in sorted(have):
        m = re.match(r"^air\.(read|sample)_texture_(2d|3d)\.v4f16$", name)
        if not m:
            continue
        kindop, nd = m.groups()
        n = 3 if nd == "3d" else 2
        coord = (1, 1, 1) if nd == "3d" else (1, 1)
        if kindop == "read":
            cv = ", ".join("i32 %d" % v for v in coord[:n])
            body = ('  %%r = tail call { <4 x half>, i8 } @%s(ptr addrspace(1) %%t, '
                    '<%d x i32> <%s>, i32 0, i32 0)\n' % (name, n, cv))
            access, kind = "air.read", "%s<half, access::read>" % nd
            sampler = False
        else:
            cv = ", ".join("float %.9e" % ((v + 0.5) / TEX) for v in coord[:n])
            body = ('  %%r = tail call { <4 x half>, i8 } @%s(ptr addrspace(1) %%t, '
                    'ptr addrspace(2) %%s, <%d x float> <%s>, i1 true, '
                    '<%d x i32> zeroinitializer, i1 false, float 0.0, float 0.0, '
                    'i32 0)\n' % (name, n, cv, n))
            access, kind = "air.sample", "%s<half, sample>" % nd
            sampler = True
        body += '  %g = extractvalue { <4 x half>, i8 } %r, 0\n'
        body += '  %v = fpext <4 x half> %g to <4 x float>'
        out.append(("%s %s" % (name, coord[:n]),
                    build(have[name], body, sampler, kind, access),
                    texel(*coord[:3]) if nd == "3d" else texel(*coord[:2]), False))

    return out


def collect_declarations():
    air2nir = os.environ.get(
        "AIR2NIR", os.path.join(ROOT, "build/mesa-host/build/src/compiler/air/air2nir"))
    out = set()
    for dirpath, _, names in os.walk(os.path.join(ROOT, "ios165/rootfs")):
        for n in sorted(names):
            if not n.endswith(".metallib"):
                continue
            ll = subprocess.run([air2nir, "--ll", os.path.join(dirpath, n)],
                                capture_output=True).stdout
            for line in ll.decode("utf-8", "replace").splitlines():
                if line.startswith("declare ") and "@air." in line:
                    line = re.sub(r"\s+local_unnamed_addr.*$", "", line)
                    out.add(re.sub(r"\s+#\d+$", "", line).strip())
    return sorted(out)


def run(d, i, source):
    ll, bc = os.path.join(d, "t%03d.ll" % i), os.path.join(d, "t%03d.bc" % i)
    with open(ll, "w") as f:
        f.write(source)
    q = subprocess.run(["llvm-as", "-o", bc, ll], capture_output=True)
    if q.returncode:
        return None, "llvm-as: " + q.stderr.decode().strip().splitlines()[0]
    q = subprocess.run([AIR_RUN, bc], capture_output=True)
    out = q.stdout.decode() + q.stderr.decode()
    i = out.find("came back as")
    if i < 0:
        for line in out.splitlines():
            if any(w in line for w in ("this harness", "Assertion", "error",
                                       "does not bind", "stores nothing")):
                return None, line.strip()
        return None, (out.strip().splitlines() or ["no output"])[-1]
    vals = []
    for line in out[i:].splitlines()[1:]:
        t = line.strip()
        if not re.match(r"^-?[0-9]", t):
            break
        vals.append(float(t))
    return vals, None


def main(argv):
    keep = None
    verbose = "-v" in argv
    if "--keep" in argv:
        keep = argv[argv.index("--keep") + 1]
    if not os.path.exists(AIR_RUN):
        print("texopcheck: no air_run at %s (run tools/air/mk-host.sh)" % AIR_RUN,
              file=sys.stderr)
        return 1

    todo = cases(collect_declarations())
    if not todo:
        print("texopcheck: no texture declaration in the corpus has a case here",
              file=sys.stderr)
        return 1

    d = keep or tempfile.mkdtemp(prefix="texopcheck.")
    os.makedirs(d, exist_ok=True)
    exact = other = refused = 0
    narrow = set()
    for i, (label, source, want, unordered) in enumerate(todo):
        got, err = run(d, i, source)
        if got is None:
            # TGSI's registers are 32-bit, so softpipe cannot carry a 16-bit
            # texture result: nir_to_tgsi asserts. That is softpipe's limit and
            # not the frontend's, and it is the same one intrincheck.py sets
            # aside for the narrow arithmetic. ir3 has real 16-bit registers,
            # so these are a question for the phone.
            if "nir_to_tgsi" in err and re.search(r"v4i16|v4f16", label):
                narrow.add(label.split(" ")[0])
                continue
            print("refused %-44s %s" % (label, err[:80]))
            refused += 1
            continue
        g4 = sorted(got[:4]) if unordered else got[:4]
        w4 = sorted(float(w) for w in want) if unordered else [float(w) for w in want]
        if w4 == g4:
            exact += 1
            if verbose:
                print("EXACT   %-44s %s" % (label, " ".join("%g" % g for g in got[:4])))
        else:
            other += 1
            print("WRONG   %-44s got %s  want %s%s"
                  % (label, " ".join("%g" % g for g in g4),
                     " ".join("%g" % w for w in w4),
                     " (in any order)" if unordered else ""))

    print()
    print("texopcheck: %d texture calls run: %d returned exactly what was put there, "
          "%d did not, %d refused" % (len(todo) - sum(1 for l, _, _, _ in todo
                                          if l.split(" ")[0] in narrow),
                          exact, other, refused))
    if narrow:
        print("texopcheck: %d declarations left out: a 16-bit result, and TGSI's registers"
              % len(narrow))
        print("texopcheck: are 32-bit, so softpipe asserts rather than answering wrongly")
    print("texopcheck: the coordinates differ between calls, so a lowering that ignored")
    print("texopcheck: them and always fetched (1,1) would fail here and not in texcheck.sh")
    print("texopcheck: a gather is compared as a multiset: it came back in GL's order,")
    print("texopcheck: (i0j1, i1j1, i1j0, i0j0), but Metal's own order is not established")
    print("texopcheck: here, so the footprint and the channel are checked and not the order")
    print("texopcheck: nothing here tests Metal's sampler state: the sampler is the API's")
    print("texopcheck: (nearest, clamped); the shader's own sampler is sampcheck.py's job")
    if keep:
        print("texopcheck: kept in %s" % d)
    return 1 if (other or refused) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
