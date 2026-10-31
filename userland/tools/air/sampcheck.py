#!/usr/bin/env python3
"""Metal's own sampler state, against what Apple's header says it should do.

Every texture check before this one used the API's sampler -- nearest,
clamp_to_edge -- and said so: `tools/air/texopcheck.py` ends with "nothing here
tests Metal's sampler state". A shader can declare its own instead
(`constexpr sampler` in the source, `!air.sampler_states` and a 64-bit word in
the bitcode), and ten modules of the corpus do. Until that word was decoded,
the frontend passed it to the runtime verbatim and the harness bound its own
sampler over it, so filtering and addressing were not merely unverified: they
were not applied.

This generates a fragment shader per (word, coordinate), samples with the
shader's own sampler, and compares against an expectation that comes from
Apple's documentation rather than from our code:

  coord::pixel     texel i is centred at pixel coordinate i + 0.5, so an
                   integer coordinate falls exactly between two centres
  nearest          picks floor(u) in texel space -- the tie at an integer
                   coordinate goes to the higher index, always, whatever the
                   data
  linear           weights frac(u - 0.5); at an integer coordinate that is
                   0.5/0.5, so the result is the exact mean of two texels
  clamp_to_edge    outside the texture, the nearest edge texel (the default)
  clamp_to_zero    outside, transparent black: zero in all four channels,
                   including alpha, and not a configurable border

air_run fills texel (x, y) with (100+x, 200+y, 300+x+y, 400), all integers, so
every mean of neighbours is exact in binary32 and a wrong choice of tap gives a
wrong number rather than a plausible one. The alpha channel is what separates
clamp_to_zero from everything else: 400 inside, 0 outside, and no filtering of
real texels can produce 0 there.

Only the three words the corpus actually carries are used. The encoding is
established (nir_air.h names the three sources), but a word nothing attests
would be our construction, and `repeat` has no meaning under coord::pixel
anyway.

What this does NOT test, and cannot: the precision of the filter's weights.
Every linear case here lands on an integer coordinate, where the weight is
exactly 0.5 -- representable in any weight format a texture unit might use,
including a few fixed-point bits. So agreement says the taps, their order and
the addressing are right, and says nothing about what the hardware does with a
weight of 0.6. Nor could it: Metal does not specify the precision of a linear
blend, so a case at a non-dyadic coordinate would have no expectation to
compare against -- only a difference to report.

usage: sampcheck.py [-v]                     run here, on whatever screen air_run opens
       sampcheck.py --emit DIR               write the cases out to carry to the phone
       sampcheck.py --check DIR RUN.log      judge what the phone printed

--emit and --check exist because softpipe is the weakest possible witness for
this particular question: it filters in C, in float32, with exact weights, and
hardware does not have to. A GPU may blend with reduced-precision weights --
that is a normal thing for a texture unit to do -- so "linear returns the exact
mean" is a claim about softpipe until the Adreno has been asked. The
expectations stay on the host in manifest.json, which is not carried, so the
device cannot be made to agree by reading the answer.
"""

import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools/air"))
import texopcheck  # noqa: E402  (its declaration collector and texel pattern)

AIR_RUN = os.environ.get(
    "AIR_RUN",
    os.path.join(ROOT, "build/mesa-host/build/src/gallium/drivers/freedreno/air_run"))

# The OTHER consumer of the same words. air_run and tools/mesa/air/air_metal.c each
# turn a decoded sampler word into a pipe_sampler_state, and they are different code:
# air_run is the harness and air_metal.c is the library the Metal driver on the phone
# links, so it is air_metal.c's table that serves a real draw. Two tables that must
# agree are two tables that will drift, so every comparison case is run through BOTH,
# against the one set of expectations in this file.
AIR_METAL_TEST = os.environ.get(
    "AIR_METAL_TEST",
    os.path.join(ROOT,
                 "build/mesa-host/build/src/gallium/drivers/freedreno/air_metal_test"))

TEX = texopcheck.TEX

# The three words of the corpus, as the ten modules that carry them encode
# them, with what air_decode_sampler_state makes of each. 0x807bff0000088049
# is eight of the ten; the other two are one each.
WORDS = [
    ("sampler(coord::pixel)", 0x807bff0000088049, "nearest", "clamp_to_edge"),
    ("sampler(coord::pixel, filter::linear)", 0x807bff0000088a49, "linear", "clamp_to_edge"),
    ("sampler(coord::pixel, filter::linear, address::clamp_to_zero)",
     0x807bff0000088a00, "linear", "clamp_to_zero"),
]

# ---------------------------------------------------------------------------
# The comparison function: Metal's nine, their values, and what each must
# answer.
#
# This is the one field of the word whose other eight values nothing attests:
# all three words the corpus carries hold 8, because 8 is `never` and `never`
# is what _get_sampler_state() in Apple's own header initialises the field to
# (metal_config defines __HAVE_ALWAYS_NEVER_COMPARE_FUNCTION__ for this
# language version). So a check of the other eight HAS to construct words --
# the thing the rest of this file refuses to do -- and the construction is kept
# as small as it can be: the corpus's own `sampler(coord::pixel)` word with
# four bits replaced and nothing else.
#
# The values are not read off the declaration order alone. They are clang's
# own, out of the macro table in libGPUCompilerImpl that defines
# __METAL_COMPARE_FUNC_*__ with each value emitted beside its name (NOT_EQUAL
# 6, ALWAYS 7, NEVER 8), and they are confirmed from the other side by Apple's
# DECODER: AIRConstSampler::constructFlatReply in AGXCompilerCore reads this
# field with `ubfx x8, x8, #16, #4; sub x8, #1; lsr x9,
# 0x0108070605040302, x8*8`, so field 1 becomes AGC's LESS (2), field 8 becomes
# its NEVER (1), and field 0 falls out of range to its NONE (0).
COMPARE = [
    ("none", 0),            # no MTLCompareFunction, and refused with a comparison
    ("less", 1),
    ("less_equal", 2),
    ("greater", 3),
    ("greater_equal", 4),
    ("equal", 5),
    ("not_equal", 6),
    ("always", 7),
    ("never", 8),           # the default every constexpr sampler carries
]

# The word the ten corpus modules carry, sampler(coord::pixel): nearest
# filtering, clamp_to_edge addressing, pixel coordinates. Its compare field is
# already 8.
BASE_WORD = 0x807BFF0000088049


def compare_word(func, pixel_coord=True):
    """sampler(coord::<pixel|normalized>, compare_func::<func>), as encoded.

    Bit 15 is the coordinate convention: 1 is coord::pixel, which is what all
    three corpus words carry, and 0 is the default. Only that bit and the four
    compare bits differ from the word the corpus attests.
    """
    w = (BASE_WORD & ~(0xF << 16)) | (func << 16)
    return w if pixel_coord else (w & ~(1 << 15))


def depth_texel(x, y):
    """The depth texel air_run writes at (x, y) -- its own expression.

    A depth format is normalised to [0, 1] and air_run's colour pattern starts at
    100, so the two cannot be one rule. (x + 4y)/16 gives sixteen distinct values
    over the 4x4 texture, every one exactly representable in binary32, so a
    reference placed between two of them is exact on both sides and a comparison
    that read the wrong texel answers differently.
    """
    return (x + TEX * y) / 16.0


def compares(ref, texel, func):
    """What Metal says a comparison of `ref` against a depth texel answers.

    Metal's own definition and nothing of this frontend's: the sampler's
    compare_func is applied with the VALUE PASSED AS THE FIRST OPERAND, so
    compare_func::less answers 1 when the value compared is less than the texel.
    1 and 0 and not true and false: sample_compare returns a float, which with
    nearest filtering is one comparison and so is exactly one or exactly zero.
    """
    return float({
        "less": ref < texel,
        "less_equal": ref <= texel,
        "greater": ref > texel,
        "greater_equal": ref >= texel,
        "equal": ref == texel,
        "not_equal": ref != texel,
        "always": True,
        "never": False,
    }[func])


# air.sample_compare_depth_2d, written out here because no module of the
# 456-module corpus calls it -- 29 of Resident Evil 4's 7 879 do, and that
# library is not in this repository and must not be. Two readings, which agree:
#
#   - Apple's own metal_stdlib on the device, where depth2d<T>::sample_compare
#     is a one-line forwarder:
#       __metal_sample_compare_depth_2d_t(t, s, coord, compare_value,
#                                         true, offset, false, 0.0f, 0.0f,
#                                         int(a), nullptr)
#     so the compared value comes straight after the coordinate, before the
#     offset pair, and the array form puts the layer between the two:
#       __metal_sample_compare_depth_2d_array_t(t, s, coord, array,
#                                               compare_value, ...)
#   - and the corpus's own `air.sample_depth_2d.f32`, declared by
#     texturedFragment2DDepth as
#       { float, i8 } (ptr addrspace(1), ptr addrspace(2), i32, <2 x float>,
#                      i1, <2 x i32>, i1, float, float, i32)
#     which is where the i32 ASPECT SELECTOR between the sampler and the
#     coordinate comes from. Apple's header has no such argument -- clang
#     inserts it -- so the compare form's AIR-level shape could only be read by
#     putting the two together.
COMPARE_DECL = ("declare { float, i8 } @air.sample_compare_depth_2d.f32("
                "ptr addrspace(1), ptr addrspace(2), i32, <2 x float>, float, "
                "i1, <2 x i32>, i1, float, float, i32)")

COMPARE_SKELETON = """source_filename = "sampcmp"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

@__air_sampler_state = internal addrspace(2) constant i64 %(word)d, align 8

%(decl)s

define <{ <4 x float> }> @sampcmp(ptr addrspace(1) %%t) local_unnamed_addr {
  %%r = tail call { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) %%t, ptr addrspace(2) @__air_sampler_state, i32 1, <2 x float> <float %(u).9e, float %(v).9e>, float %(ref).9e, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %%k = extractvalue { float, i8 } %%r, 0
  %%v0 = insertelement <4 x float> undef, float %%k, i32 0
  %%v1 = insertelement <4 x float> %%v0, float %%k, i32 1
  %%v2 = insertelement <4 x float> %%v1, float %%k, i32 2
  %%v = insertelement <4 x float> %%v2, float %%k, i32 3
  %%out = insertvalue <{ <4 x float> }> undef, <4 x float> %%v, 0
  ret <{ <4 x float> }> %%out
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}
!air.sampler_states = !{!8}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @sampcmp, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!6 = !{!7}
!7 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"depth2d<float>", !"air.arg_name", !"t"}
!8 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state}
"""


# The same comparison through a sampler THE API BINDS, which is the other half of
# struct rwa_sampler's two state objects and the only thing that checks them.
#
# Neither harness lets a caller choose this sampler's compare function: air_run and
# air_metal_test both bind MTLSamplerDescriptor's own default, compareFunction =
# MTLCompareFunctionNever, so the comparison must answer 0 -- and that single number is
# the measurement. If the NON-comparing state object were bound instead, Gallium would
# do no comparison at all and the shader would come back with the texel, 101, which is
# not a 0 or a 1 and cannot be mistaken for one.
API_COMPARE_SKELETON = """source_filename = "sampcmpapi"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

%(decl)s

define <{ <4 x float> }> @sampcmp(ptr addrspace(1) %%t, ptr addrspace(2) %%s) local_unnamed_addr {
  %%r = tail call { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) %%t, ptr addrspace(2) %%s, i32 1, <2 x float> <float %(u).9e, float %(v).9e>, float %(ref).9e, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %%k = extractvalue { float, i8 } %%r, 0
  %%v0 = insertelement <4 x float> undef, float %%k, i32 0
  %%v1 = insertelement <4 x float> %%v0, float %%k, i32 1
  %%v2 = insertelement <4 x float> %%v1, float %%k, i32 2
  %%v = insertelement <4 x float> %%v2, float %%k, i32 3
  %%out = insertvalue <{ <4 x float> }> undef, <4 x float> %%v, 0
  ret <{ <4 x float> }> %%out
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @sampcmp, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!6 = !{!7, !8}
!7 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"depth2d<float>", !"air.arg_name", !"t"}
!8 = !{i32 1, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}
"""


SKELETON = """source_filename = "samp"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

@__air_sampler_state = internal addrspace(2) constant i64 %(word)d, align 8

%(decl)s

define <{ <4 x float> }> @samp(ptr addrspace(1) %%t) local_unnamed_addr {
  %%r = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %%t, ptr addrspace(2) @__air_sampler_state, <2 x float> <float %(u).9e, float %(v).9e>, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %%v = extractvalue { <4 x float>, i8 } %%r, 0
  %%out = insertvalue <{ <4 x float> }> undef, <4 x float> %%v, 0
  ret <{ <4 x float> }> %%out
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}
!air.sampler_states = !{!8}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @samp, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!6 = !{!7}
!7 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t"}
!8 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state}
"""


def texel(x, y):
    return [float(v) for v in texopcheck.texel(x, y)]


def clamp_edge(x, y):
    """The texel a clamp_to_edge sampler reads for a tap outside the texture."""
    return texel(min(max(x, 0), TEX - 1), min(max(y, 0), TEX - 1))


def tap(x, y, address):
    """One texel of a filter's footprint, with the address mode applied."""
    inside = 0 <= x < TEX and 0 <= y < TEX
    if inside:
        return texel(x, y)
    if address == "clamp_to_edge":
        return clamp_edge(x, y)
    return [0.0, 0.0, 0.0, 0.0]          # clamp_to_zero: transparent black


def expect(u, v, filt, address):
    """What a sample at pixel coordinate (u, v) must return.

    Texel space is the pixel coordinate minus half a texel, so texel i covers
    [i, i+1) in texel space and is centred at i + 0.5 in pixel space. nearest
    takes floor of the texel-space coordinate; linear takes the two taps either
    side of the sample point with the fractional part as the weight, which is
    what makes an integer pixel coordinate an exact half-and-half mix.
    """
    s, t = u - 0.5, v - 0.5
    if filt == "nearest":
        import math
        return tap(math.floor(u), math.floor(v), address)
    import math
    x0, y0 = math.floor(s), math.floor(t)
    fx, fy = s - x0, t - y0
    out = []
    for c in range(4):
        a = tap(x0, y0, address)[c] * (1 - fx) * (1 - fy)
        b = tap(x0 + 1, y0, address)[c] * fx * (1 - fy)
        d = tap(x0, y0 + 1, address)[c] * (1 - fx) * fy
        e = tap(x0 + 1, y0 + 1, address)[c] * fx * fy
        out.append(a + b + d + e)
    return out


def distinguishes(u, v, filt, address):
    """Which of the three fields this case would catch a wrong reading of.

    A case that returns the same number whether the word was read as nearest or
    linear proves nothing about filtering, however green it looks -- the same
    shape as the half conversions that "agreed" over values needing no
    rounding. So each case is asked, before anything runs, whether the three
    fields that matter are load-bearing in it, and the summary says how many
    cases carry each one.
    """
    right = expect(u, v, filt, address)
    other_filt = "linear" if filt == "nearest" else "nearest"
    other_addr = "clamp_to_zero" if address == "clamp_to_edge" else "clamp_to_edge"
    return (right != expect(u, v, other_filt, address),
            right != expect(u, v, filt, other_addr),
            # coord::normalized would read the same numbers as fractions of the
            # texture, so the sample lands somewhere else entirely
            right != expect(u * TEX, v * TEX, filt, address),
            # no case here uses a comparison; the ones that do are below
            False)


def compare_cases(decl):
    """The comparison function: eight that must answer, and one refused.

    THE TEXTURE IS A DEPTH TEXTURE, and the first device run is why. Against a
    colour texture every one of these came back as the TEXEL -- 101 where a
    comparison must answer 0 or 1, and for `always` and `never` too, which no
    compare function and no clamp can explain. The Adreno was right and the
    harness was wrong: a shadow lookup on a non-depth texture is undefined in GL
    and in Vulkan alike, and air_run now binds a real depth texture whose texel
    (x, y) is (x + 4y)/16 (its depth_texel_value, mirrored by depth_texel here).

    So texel (1,1) is 5/16 = 0.3125 and texel (3,0) is 3/16 = 0.1875, and the
    references are placed either side of the first and ON it:

        reference   0.25  0.3125  0.375
        less          1      0      0
        less_equal    1      1      0
        greater       0      0      1
        greater_equal 0      1      1
        equal         0      1      0
        not_equal     1      0      1
        always        1      1      1
        never         0      0      0

    All eight columns-of-three are distinct, so the three references together
    IDENTIFY the function: any permutation of the table in air_metal.c or in
    air_run.c turns at least one case red. The pair this check exists for -- a
    comparison's wrong answer is in range, so a direction read backwards is still
    a legal pixel -- is reference 0.25 against texel 0.3125, where `less` must
    answer 1 and `greater_equal` must answer 0, and reference 0.375 against the
    same texel, where the two swap. The adjacent pairs are pinned by the
    reference that lands exactly on the texel: `less` and `less_equal` differ
    only at 0.3125, as do `greater` and `greater_equal`.

    A fourth case per function samples texel (3,0) = 0.1875 with the reference
    0.25, which is BELOW the first texel and ABOVE this one: the four ordering
    functions must answer it the opposite way round, so a comparison that always
    read the same texel fails it. (`equal`, `not_equal`, `always` and `never`
    cannot distinguish the two texels and are not expected to.)

    AND IN BOTH COORDINATE CONVENTIONS. Every word of the corpus is coord::pixel,
    and Vulkan forbids combining unnormalizedCoordinates with a comparison at all
    -- a restriction that usually means the hardware cannot -- so a run in which
    only the pixel-coordinate cases fail would say that and nothing else. Six
    cases carry a word with coord::normalized instead, built from the same corpus
    word with that one bit cleared.

    NEAREST FILTERING ONLY, and that is a decision. softpipe filters the depth
    values and compares once (sp_tex_sample.c calls sample_compare AFTER
    funcs->filter), while a6xx's shadow sample compares each tap and filters the
    comparisons -- percentage-closer filtering, which is what Metal describes.
    The two agree on exactly one tap and disagree on four, so a linear comparison
    here would have no single expectation to compare against, only a difference
    to report.
    """
    out = []
    d11, d30 = depth_texel(1, 1), depth_texel(3, 0)
    step = 1.0 / 16.0
    for name, func in COMPARE:
        for pixel_coord in (True, False):
            word = compare_word(func, pixel_coord)
            if not pixel_coord and name not in ("less", "greater_equal",
                                                "always", "never"):
                continue          # six cases, not sixteen: see the note above
            spots = ([(1, 1, d11, r) for r in (d11 - step, d11, d11 + step)] +
                     [(3, 0, d30, d11 - step)]) if pixel_coord else \
                    [(1, 1, d11, r) for r in (d11 - step, d11 + step)]
            for (x, y, d, ref) in spots:
                u, v = (x + 0.5, y + 0.5) if pixel_coord else \
                       ((x + 0.5) / TEX, (y + 0.5) / TEX)
                src = COMPARE_SKELETON % dict(word=word - (1 << 64), decl=decl,
                                              u=u, v=v, ref=ref)
                label = ("compare_func::%s%s, %g against depth texel (%d,%d) = %g"
                         % (name, "" if pixel_coord else " coord::normalized",
                            ref, x, y, d))
                if name == "none":
                    # once, not many times: the refusal does not depend on the
                    # reference, the texel or the coordinate convention, and more
                    # copies of it would be more green rows saying the same thing
                    if any(l.startswith("compare_func::none") for l, _, _, _ in out):
                        continue
                    out.append((label, src, None, (False, False, False, True)))
                    continue
                want = compares(ref, d, name)
                other = any(compares(ref, d, n2) != want
                            for n2, _ in COMPARE if n2 not in ("none", name))
                # coord::normalized would read 1.5 as a fraction of the texture,
                # which in texel space is 5.5 -- outside, and clamp_to_edge brings
                # it back to the far corner, a different texel with a different
                # depth. The other way round, 0.375 read as a pixel coordinate is
                # inside texel 0.
                moved = depth_texel(TEX - 1, TEX - 1) if pixel_coord else depth_texel(0, 0)
                out.append((label, src, [want] * 4,
                            (False, False, compares(ref, moved, name) != want,
                             other)))

    # and the API's sampler, which is the other state object: see
    # API_COMPARE_SKELETON for why the one expected number is 0.
    #
    # NORMALISED coordinates here, and that is a correction the device made. This
    # case used to pass pixel coordinates to a sampler air_run builds with
    # normalised ones, so 1.5 landed outside and clamped to the far corner -- and
    # on softpipe the comparison answered 0 whatever texel it read, so the wrong
    # texel was invisible. The Adreno returned that texel and named the mistake.
    out.append(("an API sampler compares (compareFunction defaults to Never), "
                "%g against depth texel (1,1) = %g" % (d11 - step, d11),
                API_COMPARE_SKELETON % dict(decl=COMPARE_DECL, u=1.5 / TEX,
                                            v=1.5 / TEX, ref=d11 - step),
                [0.0] * 4, (False, False, False, True)))
    return out


def cases(decl):
    """(label, source, expected, why it is worth asking) for every word."""
    out = []
    for name, word, filt, address in WORDS:
        # a texel centre: every sampler must return that texel exactly, so a
        # case here failing means the binding is wrong, not the filtering
        for (x, y) in [(0, 0), (2, 3)]:
            u, v = x + 0.5, y + 0.5
            out.append(("%s at the centre of (%d,%d)" % (name, x, y),
                        SKELETON % dict(word=word - (1 << 64), decl=decl, u=u, v=v),
                        expect(u, v, filt, address),
                        distinguishes(u, v, filt, address)))
        # an integer coordinate: exactly between two centres, where nearest and
        # linear must answer differently
        for (u, v) in [(2.0, 2.5), (2.0, 2.0)]:
            out.append(("%s at (%g,%g), between centres" % (name, u, v),
                        SKELETON % dict(word=word - (1 << 64), decl=decl, u=u, v=v),
                        expect(u, v, filt, address),
                        distinguishes(u, v, filt, address)))
        # outside: clamp_to_zero has to give zero in all four channels, which
        # no filtering of real texels can produce
        out.append(("%s at (-1,0.5), outside" % name,
                    SKELETON % dict(word=word - (1 << 64), decl=decl, u=-1.0, v=0.5),
                    expect(-1.0, 0.5, filt, address),
                    distinguishes(-1.0, 0.5, filt, address)))
    # and the comparison function, whose cases carry their own declaration
    return out + compare_cases(COMPARE_DECL)


def run(source, workdir, i):
    ll = os.path.join(workdir, "s%02d.ll" % i)
    with open(ll, "w") as f:
        f.write(source)
    bc = os.path.join(workdir, "s%02d.bc" % i)
    q = subprocess.run(["llvm-as", "-o", bc, ll], capture_output=True)
    if q.returncode:
        return None, "llvm-as: " + q.stderr.decode(errors="replace").strip().splitlines()[0]
    out = subprocess.run([AIR_RUN, bc], capture_output=True, text=True)
    if out.returncode:
        # The REFUSAL, not just the last line: air_run prints its progress on
        # stdout and says why it stopped on stderr, so the useful sentence is
        # the one naming the thing it would not do.
        lines = (out.stderr + out.stdout).strip().splitlines()
        named = [l for l in lines
                 if any(w in l for w in ("intrinsic:", "instruction:", "interface:",
                                         "does not bind", "harness", "Assertion",
                                         "compare_func::"))]
        mine = [l for l in lines if l.startswith("air_run:")]
        last = named or mine or lines
        return None, (last[-1].strip()[:200] if last else "air_run failed")
    vals, taking = [], False
    screen = None
    for line in out.stdout.splitlines():
        m = re.match(r"air_run: \S+ on (.+?)(,|$)", line)
        if m and screen is None:
            screen = m.group(1)
        if line.startswith("air_run: the pixel"):
            taking = True
            continue
        if taking:
            if not line.startswith("  "):
                break
            vals.append(float(line.strip()))
    return (vals, screen) if vals else (None, "air_run printed no pixel")


def run_through_metal(source, workdir, i):
    """The same module through the rwa_* contract, which is what a draw uses.

    One RGBA32Float attachment and one RGBA32Float 2D texture with air_run's own
    texel pattern in it (air_metal_test writes the same (100+x, 200+y, 300+x+y+z,
    400)), so the expectation is the same number and the only thing that changed
    between the two runs is which library built the sampler.
    """
    ll = os.path.join(workdir, "m%02d.ll" % i)
    with open(ll, "w") as f:
        f.write(source)
    bc = os.path.join(workdir, "m%02d.bc" % i)
    q = subprocess.run(["llvm-as", "-o", bc, ll], capture_output=True)
    if q.returncode:
        return None, "llvm-as: " + q.stderr.decode(errors="replace").strip().splitlines()[0]
    out = subprocess.run([AIR_METAL_TEST, "--draw", bc, "sampcmp", "125", "125"],
                         capture_output=True, text=True)
    text = out.stdout + out.stderr
    vals, taking = [], False
    for line in text.splitlines():
        if "the pixel at (0, 0)" in line:
            taking = True
            continue
        if taking:
            if not line.startswith("  "):
                break
            try:
                vals.append(float(line.strip()))
            except ValueError:
                break
    if vals:
        return vals, None
    why = next((l.strip() for l in reversed(text.splitlines()) if l.strip()),
               "air_metal_test printed nothing")
    return None, why[:160]


def emit(decl, outdir):
    """Write every case as a module, with a job list and the expectations."""
    import json
    os.makedirs(outdir, exist_ok=True)
    share = "/redwood/share/" + os.path.basename(outdir.rstrip("/"))
    manifest, jobs = [], []
    skipped = 0
    for i, (label, src, want, which) in enumerate(cases(decl)):
        # A case whose answer is a REFUSAL needs no GPU: it is decided before
        # anything is drawn, on this machine, and a device that printed nothing
        # for it would be indistinguishable from a device that never ran it.
        if want is None:
            skipped += 1
            continue
        ident = "%04d" % i
        ll = os.path.join(outdir, ident + ".ll")
        with open(ll, "w") as f:
            f.write(src)
        q = subprocess.run(["llvm-as", "-o", os.path.join(outdir, ident + ".bc"), ll],
                           capture_output=True)
        if q.returncode:
            print("sampcheck: llvm-as refused %s: %s"
                  % (label, q.stderr.decode(errors="replace").strip().splitlines()[0]),
                  file=sys.stderr)
            return 1
        os.remove(ll)
        manifest.append(dict(id=ident, label=label, want=want, which=list(which)))
        jobs.append("%s/air_run %s/%s.bc" % ("/redwood/bin", share, ident))
    with open(os.path.join(outdir, "manifest.json"), "w") as f:
        json.dump(manifest, f)
    ncompare = sum(1 for m in manifest if m["label"].startswith("compare_func::")
                   or m["label"].startswith("an API sampler"))
    with open(os.path.join(outdir, "rc.jobs"), "w") as f:
        f.write("""\
# Commands run by the launchd job com.redwood.demo, generated by
# tools/air/sampcheck.py --emit. Do not edit: regenerate it.
#
# METAL'S OWN SAMPLER STATE ON THE ADRENO, and %(ncompare)d of these %(total)d lines are the
# COMPARISON FUNCTION -- the one field of the 64-bit `constexpr sampler` word whose
# values other than the default nothing in any corpus attests (docs/air.md 3.3).
#
# Each line samples a texture with the sampler state the shader itself declares.
# What each one must return stays on the host in manifest.json, which is NOT
# carried, so the device cannot be made to agree by reading the answer:
#
#   python3 tools/air/sampcheck.py --emit <dir>
#   userland/build.sh ... EXTRA_SHARE=<dir> RC_JOBS=rc.jobs.sampcmp
#   python3 tools/air/sampcheck.py --check <dir> <the boot log>
#
# The directory's own name is in the paths at the bottom of this file, so emit it
# under that name or regenerate this file from the one you used.
#
# The first line is the shader M1 is stated against, with --disasm: it proves the
# GPU is there and that ir3 compiled what ran.
#
# The greps that read the log, for a reader who has only the log. (-a on every one
# of them: a boot log has NUL bytes in it.)
#
#   grep -ac 'air_run: .*on .*Adreno' LOG      # must be > 0: a fallback to
#                                              # softpipe would mean no GPU
#   grep -a 'compare_func::' LOG               # one line per module: the function
#                                              # the word carried and whether a
#                                              # shadow sample read that slot
#   grep -a 'COMPARES, so the line above' LOG  # the comparing modules name
#                                              # themselves, so a log with none of
#                                              # these ran the wrong directory
#   grep -ac 'Unknown NIR opcode' LOG          # must be 0: that is nir_to_tgsi,
#                                              # which means softpipe
#
# WHAT A COMPARISON MUST PRINT. Its texture is a DEPTH texture now, whose texel
# (x, y) is (x + 4y)/16, so texel (1,1) is 0.3125 and texel (3,0) is 0.1875.
# Every comparing line must print four copies of 0 or of 1 -- NEVER a depth value
# and never a colour texel -- and over the references 0.25, 0.3125 and 0.375 the
# eight functions print eight DIFFERENT triples (`less` 1 0 0, `less_equal` 1 1 0,
# `greater` 0 0 1, `greater_equal` 0 1 1, `equal` 0 1 0, `not_equal` 1 0 1,
# `always` 1 1 1, `never` 0 0 0). --check does this comparison; these are the
# numbers to read by eye if the log is all there is.
#
# THE FIRST DEVICE RUN OF THESE CASES FAILED, and this set is the answer to it.
# Every comparison came back as the TEXEL -- 101 where a 0 or a 1 was required,
# and for `always` and `never` too, which no compare function and no clamped
# reference can explain. What was ruled out by that same log, so nobody chases it
# again:
#
#   the shadow instruction          ir3 emits it: `samgq.s ... s#0, t#0`
#   the sampler state               air_run printed `APPLIED (a shadow sample
#                                   reads this slot)` for all 32, so compare_mode
#                                   and the function reached the Gallium CSO, and
#                                   fd6_sampler_state_create writes
#                                   A6XX_TEX_SAMP_1_COMPARE_FUNC whenever it is set
#   the compare function            `always` and `never` failed too
#
# What was left is what the log also printed: `1 texture bound: 4x4x1
# r32g32b32a32_float` -- a COLOUR texture under a Metal depth2d<float>. A shadow
# lookup on a non-depth texture is undefined in GL and in Vulkan alike and the
# texture unit does the comparison in its depth path, so the hardware was right
# and the harness was wrong. air_run binds a real depth texture now, and
# air_metal.c refuses the colour case by name rather than returning the texel.
#
#   grep -a 'DEPTH texture' LOG     # must appear: the comparing modules say so
#   grep -a 'r32g32b32a32_float' LOG  # must NOT appear in a comparing module
#
# IF IT STILL RETURNS THE DEPTH VALUE, two candidates are left and they are
# separated without changing a line of this suite:
#
#   1. UNNORMALIZED COORDINATES. Every word the corpus carries is coord::pixel,
#      and Vulkan forbids combining unnormalizedCoordinates with a comparison at
#      all -- a restriction that usually means the hardware cannot. Six of these
#      cases carry coord::normalized instead. If ONLY the coord::pixel ones fail,
#      that is the answer, and the combination gets refused by name.
#   2. THE UNIFORM PREAMBLE. These shaders have constant coordinates, so ir3
#      hoists the whole sample into the early preamble and emits `samgq.s` rather
#      than `sam.s`. Boot once more with IR3_SHADER_DEBUG=nopreamble in the job's
#      environment (com.redwood.demo.plist): the same modules then compile to
#      `sam.s` in the main shader, and if they compare then, the preamble is the
#      place. Nothing else changes, so the two boots differ in one variable.
""" % dict(ncompare=ncompare, total=len(manifest)))
        f.write("/redwood/bin/air_run --disasm "
                "/System/Library/Extensions/AppleMetalGLRenderer.bundle/default.metallib "
                "clearFragment01\n")
        f.write("\n".join(jobs) + "\n")
    print("sampcheck: %d modules in %s, with rc.jobs and manifest.json"
          % (len(manifest), outdir))
    if skipped:
        print("sampcheck: %d case%s kept on the host: its answer is a refusal, which no "
              "GPU decides" % (skipped, "" if skipped == 1 else "s"))
    return 0


def check(outdir, logpath):
    """Judge a device run against the expectations kept here."""
    import json
    mani = os.path.join(outdir, "manifest.json")
    if os.path.getmtime(logpath) < os.path.getmtime(mani):
        print("sampcheck: %s is older than %s, so it cannot be this suite's log"
              % (os.path.basename(logpath), os.path.basename(mani)), file=sys.stderr)
        return 1
    with open(mani) as f:
        manifest = json.load(f)

    # the values each module printed, keyed on the id in its path
    got, ident, block, screen = {}, None, [], None
    for line in open(logpath, errors="replace"):
        m = re.match(r"^air_run: (?:\S*/)?(\d+)\.bc( |$)", line)
        if m:
            if ident is not None:
                got[ident] = block
            ident, block = m.group(1), []
            continue
        m = re.match(r"^air_run: \S+ on (.+?)(,|$)", line)
        if m and screen is None and "Adreno" in m.group(1):
            screen = m.group(1)
        if ident is not None:
            block.append(line)
    if ident is not None:
        got[ident] = block

    agree, differ, missing = [], [], []
    tells = [0, 0, 0, 0]
    for m in manifest:
        for k in range(len(tells)):
            tells[k] += bool(m["which"][k]) if k < len(m["which"]) else False
        b = got.get(m["id"])
        if b is None:
            missing.append((m["label"], "no block in the log"))
            continue
        vals, taking = [], False
        for line in b:
            if line.startswith("air_run: the pixel"):
                taking = True
                continue
            if taking:
                if not line.startswith("  "):
                    break
                try:
                    vals.append(float(line.strip()))
                except ValueError:
                    break
        if len(vals) != 4:
            why = next((l.strip() for l in b if "air_run:" in l), "no pixel in the log")
            missing.append((m["label"], why[:100]))
        elif any(a != b2 for a, b2 in zip(vals, m["want"])):
            differ.append((m["label"], m["want"], vals))
        else:
            agree.append(m["label"])

    print("sampcheck: %d cases, as %s reports them (%s)"
          % (len(manifest), os.path.basename(logpath), screen or "screen not named"))
    print("  %2d  returned exactly what Apple's rules say" % len(agree))
    print("  %2d  DIFFER" % len(differ))
    print("  %2d  are not in the log, or printed no pixel" % len(missing))
    for label, want, vals in differ:
        print("        %-60s want %s got %s" % (label, want, vals))
    for label, why in missing:
        print("        %-60s %s" % (label, why))
    print("  of those cases, %d would catch the filter read wrongly, %d the address mode, "
          "%d the coordinate convention, %d the comparison function" % tuple(tells))
    if len(agree) + len(differ) + len(missing) != len(manifest):
        print("sampcheck: the rows do not add up", file=sys.stderr)
        return 1
    return 1 if (differ or missing) else 0


def main(argv):
    verbose = "-v" in argv
    emit_dir = check_dir = log = None
    if "--emit" in argv:
        emit_dir = argv[argv.index("--emit") + 1]
    if "--check" in argv:
        i = argv.index("--check")
        check_dir, log = argv[i + 1], argv[i + 2]
    if not os.path.exists(AIR_RUN):
        print("sampcheck: run tools/air/mk-host.sh first", file=sys.stderr)
        return 1
    # the same declaration text the corpus carries, taken from the corpus
    have = {}
    for d in texopcheck.collect_declarations():
        m = re.match(r"^declare\s+(.*?)\s+@(air\.[A-Za-z0-9_.]+)\((.*)\)", d)
        if m:
            have[m.group(2)] = d
    decl = have.get("air.sample_texture_2d.v4f32")
    if check_dir:
        return check(check_dir, log)
    if not decl:
        print("sampcheck: the corpus declares no air.sample_texture_2d.v4f32, so there "
              "is nothing to sample with", file=sys.stderr)
        return 1

    if emit_dir:
        return emit(decl, emit_dir)

    agree, differ, refused = [], [], []
    metal_agree, metal_differ = [], []     # the same cases through air_metal.c
    by_name = []          # (label, refused-as-expected?) for the refusal cases
    screen = None
    with tempfile.TemporaryDirectory() as work:
        tells = [0, 0, 0, 0]
        for i, (label, src, want, which) in enumerate(cases(decl)):
            for k in range(len(tells)):
                tells[k] += bool(which[k])
            got, info = run(src, work, i)
            # A case whose expected answer IS a refusal. It passes by being
            # refused and fails by returning a pixel, which is the opposite of
            # every other row here and is why it is counted apart: a sampler
            # that was not made to compare must not quietly compare as `never`.
            if want is None:
                ok = got is None and "compare_func::none" in (info or "")
                by_name.append((label, ok, info if got is None else "returned a pixel"))
                if not ok:
                    differ.append((label, "a refusal naming compare_func::none",
                                   info if got is None else got))
                continue
            if got is None:
                refused.append((label, info))
                continue
            screen = screen or info
            if len(got) != 4 or any(a != b for a, b in zip(got, want)):
                differ.append((label, want, got))
            else:
                agree.append(label)
            if verbose:
                print("  %-64s want %s got %s" % (label, want, got))
            # and the same case through air_metal.c, the library a draw uses --
            # where it must be REFUSED, because air_metal_test binds the colour
            # texture its own texel pattern needs and a comparison of a colour
            # texture is what the Adreno answered with the texel. The refusal is
            # the rule this check exercises; the numbers are air_run's job.
            if "sample_compare" in src and os.path.exists(AIR_METAL_TEST):
                mgot, mwhy = run_through_metal(src, work, i)
                if mgot is None and "with a comparison and the texture bound" in (mwhy or ""):
                    metal_agree.append(label)
                else:
                    metal_differ.append((label, "a refusal naming the colour texture",
                                         mgot if mgot else mwhy))

    total = len(agree) + len(differ) + len(refused) + len(by_name)
    print("sampcheck: %d cases over %d sampler words and %d comparison functions, as RUN "
          "on %s" % (total, len(WORDS), len(COMPARE) - 1,
                     screen or "an unnamed screen"))
    print("  %2d  returned exactly what Apple's rules say" % len(agree))
    print("  %2d  DIFFER" % len(differ))
    print("  %2d  could not be run" % len(refused))
    print("  %2d  were refused by name, which is the answer asked for"
          % sum(1 for _, ok, _ in by_name if ok))
    for label, want, got in differ:
        print("        %-60s want %s got %s" % (label, want, got))
    for label, why in refused:
        print("        %-60s %s" % (label, why))
    print("  of those cases, %d would catch the filter read wrongly, %d the address mode, "
          "%d the coordinate convention, %d the comparison function" % tuple(tells))
    thin = [n for n, c in zip(("filter", "address mode", "coordinate convention",
                               "comparison function"), tells) if not c]
    if thin:
        print("sampcheck: no case distinguishes %s, so agreement says nothing about it"
              % ", ".join(thin))
    print("sampcheck: the expectations are Apple's own -- texel i centred at i+0.5, "
          "nearest by floor, linear by frac(u-0.5), clamp_to_zero transparent black -- "
          "and not this frontend's reading of the word, which is what makes a match "
          "evidence about the word.")
    # What the comparison cases are worth, stated rather than left to be inferred: the
    # three references identify the function, so this is not eight cases that happen to
    # pass but eight distinguishable answers.
    vectors = {}
    d11, step = depth_texel(1, 1), 1.0 / 16.0
    refs = (d11 - step, d11, d11 + step)
    for name, _ in COMPARE:
        if name == "none":
            continue
        vectors[name] = tuple(compares(r, d11, name) for r in refs)
    if metal_agree or metal_differ:
        print("sampcheck: the same %d comparison cases through air_metal.c -- the library "
              "the Metal driver links: %d were REFUSED by name for the colour texture "
              "bound under a depth sampler, which is the answer asked for, and %d were not"
              % (len(metal_agree) + len(metal_differ), len(metal_agree),
                 len(metal_differ)))
        for label, want, got in metal_differ:
            print("        %-60s want %s got %s" % (label, want, got))
    elif os.path.exists(AIR_METAL_TEST):
        print("sampcheck: no case reached air_metal.c, so its own table is unchecked here",
              file=sys.stderr)
    else:
        print("sampcheck: no air_metal_test, so air_metal.c's table is unchecked here",
              file=sys.stderr)
    print("sampcheck: the %d comparison functions give %d DISTINCT answer triples over "
          "the references %g, %g, %g against DEPTH texel (1,1) = %g, so the three "
          "together identify the function and no permutation of the table passes"
          % (len(vectors), len(set(vectors.values())), refs[0], refs[1], refs[2], d11))
    print("sampcheck: the direction case is %g against %g -- compare_func::less must "
          "answer %g and compare_func::greater_equal %g -- and %g against the same texel, "
          "where the two swap"
          % (refs[0], d11, vectors["less"][0], vectors["greater_equal"][0], refs[2]))
    print("sampcheck: the texture is a DEPTH texture, which the first device run is the "
          "reason for: against a colour one every comparison came back as the TEXEL, "
          "`always` and `never` included, and a shadow lookup on a non-depth texture is "
          "undefined in GL and in Vulkan alike")
    print("sampcheck: six cases carry coord::normalized instead of the corpus's "
          "coord::pixel, because Vulkan forbids unnormalized coordinates with a "
          "comparison at all -- so a run where only the pixel-coordinate cases fail says "
          "that and nothing else")
    print("sampcheck: no comparison here filters: softpipe filters the depth values and "
          "compares once, a6xx compares each tap and filters the comparisons (Metal's "
          "percentage-closer filtering), and the two agree on one tap only -- so a linear "
          "comparison has a difference to report and no expectation to compare against")
    if len(set(vectors.values())) != len(vectors):
        print("sampcheck: two comparison functions share an answer triple, so these cases "
              "cannot tell them apart", file=sys.stderr)
        return 1
    return 1 if (differ or refused or thin or metal_differ or not metal_agree) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
