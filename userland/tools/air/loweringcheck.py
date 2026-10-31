#!/usr/bin/env python3
"""The lowerings this frontend does by hand, against LLVM on this processor.

Three things AIR has and NIR does not, so the frontend builds them out of
something else, and none is checked by anything that only counts modules:

  - **`switch`**. NIR has none, so the cases become a chain of nir_goto_if, one
    test per case in a block of its own, the last falling to the default.
  - **a call returning a struct**. A NIR function parameter carries a number, so
    the callee gets one out-parameter per member of the returned struct, and the
    members that are not numbers (the `[8 x i8]` tail padding Metal puts on a
    vertex-output struct) get none.
  - **a bitcast that changes a value's shape**. A NIR def has a width and a
    component count, so `i32 -> <2 x half>` is not a rename but a
    redistribution of bits between components: `unpack_32_2x16` one way and
    `pack_32_2x16` the other.

`tools/air/reader-check.sh` covers the first half of the switch question
exactly: our parse is printed as .ll and diffed against llvm-dis, so a case
constant read with the wrong value numbering is a textual difference on every
module that has one. What neither that nor the corpus count says is whether the
lowering *computes the right thing*: a chain that always fell to the default
would print the same .ll and compile just as well, and a struct return whose
members were swapped would translate and reach ir3 without complaint.

So this file asks LLVM. Each case is a module whose inputs come out of a buffer;
`tools/air/cpuref.py --compare` has llc compile it for this processor, runs it
over a known pattern of bytes, folds our own NIR over the same bytes, and
compares the two results field by field. The expectation is LLVM's, not this
script's, which is the point -- the only thing asserted is agreement, plus the
one thing agreement alone would not give:

  **the arms taken must differ between the switch cases.** Four modules that all
  fell to the default would all agree with LLVM and prove nothing about the
  chain, so the lane each one reaches is recorded and a run that reached fewer
  than three distinct arms fails even if every comparison agreed.

And for the pack/unpack cases a second expectation, because agreement alone is
not enough for them either. LLVM compiles the module for THIS processor, which
is little-endian, and the frontend's own reading of which half of an `i32` is
element 0 is little-endian too; agreement between two readings of the same
convention would be just as perfect if the convention were the wrong one. So
those cases also state the four numbers outright, computed here from Python's
own IEEE-754 binary16 codec and an explicit `(high << 16) | low` -- a third
statement of the same fact that comes neither from LLVM nor from us. The case
list says, for each of them, what the transposed order would have produced
instead, and those alternatives are all numbers, which is exactly why this is
checked rather than eyeballed.

The one shape NOT tested here is a pack fed by an unpack: a round trip would
come back correct with BOTH halves consistently transposed, so it would pass
for the wrong reason. The cases keep the two directions apart and the values
that go in are not the values that come out.

What could not be done this way: softpipe. Every other numeric check in this
tree can also go through a Gallium driver (`cpuref.py --draw`), which is a
second executor rather than a second opinion. Not these -- `nir_to_tgsi` has no
16-bit arithmetic at all, so a module doing nothing but `fptrunc float to half`
aborts it with "Unknown NIR opcode: u2f16" before any bitcast is reached. That
is softpipe's limit and not this lowering's, and it means the drawn reference
is unavailable for anything with a `half` in it.

usage: loweringcheck.py [-v]
       loweringcheck.py --emit DIR        the pack/unpack modules, for the phone
       loweringcheck.py --check DIR LOG   a boot log against DIR's expectations
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CPUREF = os.path.join(ROOT, "tools/air/cpuref.py")

SKELETON = """source_filename = "sw"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float> }> @sw(ptr addrspace(2) %%u) local_unnamed_addr {
entry:
  %%v = load i32, ptr addrspace(2) %%u, align 4
  %%n = and i32 %%v, 3
  %%x = xor i32 %%n, %(xor)d
%(narrow)s  switch %(ty)s %%c, label %%dflt [
%(arms)s  ]
%(blocks)s
join:
  %%r = phi <4 x float> %(phi)s
  %%o = insertvalue <{ <4 x float> }> undef, <4 x float> %%r, 0
  ret <{ <4 x float> }> %%o
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @sw, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!6 = !{!7}
!7 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"uint", !"air.arg_name", !"u"}
"""


def lane(i):
    return "<%s>" % ", ".join("float %s" % ("1.0" if k == i else "0.0") for k in range(4))


def module(ty, xor, arms, dflt_lane):
    """arms: [(case value, label name, lane)]; labels may repeat."""
    labels = []
    for _, name, ln in arms:
        if name not in [n for n, _ in labels]:
            labels.append((name, ln))
    if "dflt" not in [n for n, _ in labels]:
        labels.append(("dflt", dflt_lane))

    return SKELETON % dict(
        ty=ty, xor=xor,
        # `trunc i32 %x to i32` is not a cast LLVM accepts, so the narrowing is
        # emitted only when the condition is narrower than the word it came out
        # of -- which is also the only case that needs it.
        narrow=("  %%c = trunc i32 %%x to %s\n" % ty) if ty != "i32"
               else "  %c = add i32 %x, 0\n",
        arms="".join("    %s %d, label %%%s\n" % (ty, v, name) for v, name, _ in arms),
        blocks="".join("%s:\n  br label %%join\n" % name for name, _ in labels),
        phi=", ".join("[ %s, %%%s ]" % (lane(ln), name) for name, ln in labels))


# Each case: (label, source). The four xor keys turn one pattern of bytes into
# four different conditions, so the same shape is made to take four different
# arms -- which is what makes "they agree" mean something about the chain.
def cases():
    out = []
    three = [(0, "c0", 0), (1, "c1", 1), (2, "c2", 2)]
    for xor in range(4):
        out.append(("i32, three cases, xor %d" % xor,
                    module("i32", xor, three, 3), "arm"))

    # Two cases to one arm: the chain tests them separately and both must
    # arrive at the same block, which a lowering that deduplicated destinations
    # would get wrong.
    out.append(("two cases sharing one arm",
                module("i32", 0, [(0, "c0", 0), (1, "cx", 1), (2, "cx", 1)], 3), "arm"))

    # The LAST case goes where the default goes. That is the one place the
    # lowering emits a plain nir_goto instead of a nir_goto_if, because
    # nir_goto_if with one block on both sides is not a branch -- so it has a
    # case of its own rather than being reasoned about.
    out.append(("last case goes to the default",
                module("i32", 2, [(0, "c0", 0), (2, "dflt", 3)], 3), "arm"))

    # One case, and a narrow condition: `switch i8` is what QuartzCore's own
    # fixed-function fragment shaders carry, and an 8-bit compare is a
    # different ir3 instruction from a 32-bit one.
    out.append(("one case, i8 condition",
                module("i8", 1, [(0, "c0", 0)], 3), "arm"))
    out.append(("three cases, i8 condition",
                module("i8", 3, three, 3), "arm"))

    # A call returning a struct: one out-parameter per member. Three scalar
    # members and a `[8 x i8]` tail that nothing writes -- which is exactly the
    # shape Metal gives a vertex-output struct, and the member that must take no
    # parameter at all. The entry point reads all three members back and puts
    # each in a different lane, so a member wired to the wrong out-parameter
    # comes back as a permuted float4 and not as agreement.
    out.append(("struct return, scalars + [8 x i8] padding",
                sret("float, float, float, [8 x i8]", STRAIGHT), "agree"))
    out.append(("struct return, no padding member",
                sret("float, float, float", STRAIGHT), "agree"))
    out.append(("struct return from two blocks",
                sret("float, float, float, [8 x i8]", BRANCHED), "agree"))
    out.append(("struct return with a vector member",
                sret("float, float, float, <2 x float>, [8 x i8]", STRAIGHT), "agree"))
    out.append(("a phi of the struct (must be refused)",
                sret("float, float, float, [8 x i8]", STRUCT_PHI),
                # The phi is what cannot be translated, and that is where it is
                # now refused: a struct is a tree of values here and never a NIR
                # value, so no register carries one across an edge. It used to
                # get a register of zero bits and fail further down at "a
                # returned struct that was never built" -- true of the module,
                # and not what is wrong with it.
                "refuse:a phi of a struct, which NIR has no register for"))

    # the chosen pointer, each side taken once
    out.append(("pointer chosen at run time, constant side", psel(0), "agree"))
    out.append(("pointer chosen at run time, buffer side", psel(1), "agree"))

    # Storing a pointer to memory is the other half of the same problem and has
    # no form here: a pointer is a resolution, not bytes. Refused by name so the
    # wall has a reason rather than a symptom.
    out.append(("a pointer stored into memory (must be refused)",
                PSTORE, "refuse:a store of a pointer into memory"))

    # i32 -> <2 x half>, which 2 927 of Resident Evil 4's 7 879 modules do
    # (tools/air/typecensus.py), with the two halves chosen so that the
    # transposed answer is not merely different but absurd.
    #
    # 13 and 113: `values:` asserts them in this order, so taking the high half
    # into lane 0 would read 113, 13, +100, 126 -- lane 3 identical, lane 2
    # sign-flipped, which is what the sum and the difference are for.
    out.append(("i32 -> <2 x half>, halves 13 and 113",
                unpack(13.0, 113.0), "values:13,113,-100,126"))
    # 1 and -2, so transposition also flips the SIGN of lane 0: -2, 1, -3, -1.
    # A half pair whose members differ only in magnitude would leave a swapped
    # unpack drawing dim instead of drawing wrong, and this one would not.
    out.append(("i32 -> <2 x half>, halves 1 and -2",
                unpack(1.0, -2.0), "values:1,-2,3,-1"))
    # The buffer's own word, 0x221B140D, bitcast as it lies: an arbitrary bit
    # pattern rather than two round numbers, so the halves asked for are ones
    # nobody chose. `values:` would not serve here -- cpuref prints nine
    # significant digits and this comparison parses them back and demands
    # equality, which the exact halves (0.0009889602661132812 and
    # 0.01192474365234375) would fail on the digits that were not printed. So
    # this one asserts agreement with LLVM and nothing of its own; --emit
    # carries it to the phone, where the comparison IS at nine digits.
    out.append(("i32 -> <2 x half>, the raw buffer word",
                BITCAST % dict(body=UNPACK % dict(word=0)), "agree"))

    # <2 x half> -> i32, the other direction, and NOT fed by an unpack: the two
    # halves are built with fptrunc and what comes back out is the packed
    # word's two 16-bit fields. 19072 and 22288 are the binary16 patterns of
    # 13.0 and 113.0, and 19072 must be the LOW field -- a pack that put the
    # first element high would read 22288, 19072, +3216, 41360.
    assert (half_bits(13.0), half_bits(113.0)) == (19072, 22288)
    out.append(("<2 x half> -> i32, the halves of 13 and 113",
                BITCAST % dict(body=PACK), "values:19072,22288,-3216,41360"))
    return out


SRET = """source_filename = "sr"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

%%S = type { %(members)s }

define <{ <4 x float> }> @sr(ptr addrspace(2) %%u) local_unnamed_addr {
  %%r = tail call fastcc %%S @inner(ptr addrspace(2) %%u)
  %%a = extractvalue %%S %%r, 0
  %%b = extractvalue %%S %%r, 1
  %%c = extractvalue %%S %%r, 2
  %%v0 = insertelement <4 x float> undef, float %%a, i32 0
  %%v1 = insertelement <4 x float> %%v0, float %%b, i32 1
  %%v2 = insertelement <4 x float> %%v1, float %%c, i32 2
  %%s = fadd float %%a, %%c
  %%v3 = insertelement <4 x float> %%v2, float %%s, i32 3
  %%o = insertvalue <{ <4 x float> }> undef, <4 x float> %%v3, 0
  ret <{ <4 x float> }> %%o
}

define internal fastcc %%S @inner(ptr addrspace(2) %%u) unnamed_addr {
entry:
  %%w = load <4 x float>, ptr addrspace(2) %%u, align 16
  %%x = extractelement <4 x float> %%w, i32 0
  %%y = extractelement <4 x float> %%w, i32 1
  %%z = extractelement <4 x float> %%w, i32 2
%(body)s
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @sr, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!6 = !{!7}
!7 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"float4", !"air.arg_name", !"u"}
"""

# The struct is built in one block, three scalar members in order.
STRAIGHT = """  %s0 = insertvalue %S undef, float %x, 0
  %s1 = insertvalue %S %s0, float %y, 1
  %s2 = insertvalue %S %s1, float %z, 2
  ret %S %s2"""

# ... and here with TWO returns, so the out-parameters are stored on two
# different paths. QuartzCore's path_cubic_vert_* callees have CFGs, and this is
# the property that matters: emit_return runs once per `ret`, and a member wired
# to the wrong out-parameter on only one of them would still translate.
BRANCHED = """  %t = fcmp ogt float %x, 100.0
  br i1 %t, label %big, label %small
big:
  %b0 = insertvalue %S undef, float %z, 0
  %b1 = insertvalue %S %b0, float %x, 1
  %b2 = insertvalue %S %b1, float %y, 2
  ret %S %b2
small:
  %m0 = insertvalue %S undef, float %x, 0
  %m1 = insertvalue %S %m0, float %y, 1
  %m2 = insertvalue %S %m1, float %z, 2
  ret %S %m2"""

# A phi whose type is the struct. This frontend keeps an aggregate as a tree of
# values and a phi as a NIR register holding a number, so there is no register
# for a struct -- it is REFUSED, by name, and the refusal is a case here rather
# than a surprise later. Nothing in either corpus needs it: Apple's own
# callees build the struct with insertvalue in the block they return from.
# Closing it would mean one register per member, which is the same shape the
# out-parameters already have.
STRUCT_PHI = """  %t = fcmp ogt float %x, 100.0
  br i1 %t, label %big, label %small
big:
  %b0 = insertvalue %S undef, float %z, 0
  %b1 = insertvalue %S %b0, float %x, 1
  %b2 = insertvalue %S %b1, float %y, 2
  br label %join
small:
  %m0 = insertvalue %S undef, float %x, 0
  %m1 = insertvalue %S %m0, float %y, 1
  %m2 = insertvalue %S %m1, float %z, 2
  br label %join
join:
  %p = phi %S [ %b2, %big ], [ %m2, %small ]
  ret %S %p"""


def sret(members, body):
    return SRET % dict(members=members, body=body)


# A POINTER chosen at run time: `select i1 %c, ptr %a, ptr %b`, then loaded
# through, with a getelementptr after the select for good measure. This frontend
# has no select of pointers -- a pointer is a binding and a byte offset decided
# while translating -- so it keeps the choice, loads BOTH sides where the
# pointer is used and selects the values, and pushes a later index into both.
# Whether that computes what LLVM computes is exactly what cpuref can say.
#
# The two sides are two OFFSETS of one buffer, and that is the whole care taken
# here: cpuref fills every buffer with the same pattern, so a select between two
# buffers would have both sides holding the same bytes and a lowering that
# always took one of them would agree with LLVM and prove nothing. Two offsets
# of one buffer differ because the pattern makes every byte differ. (QuartzCore's
# own shape is a buffer against a module constant array, which cpuref cannot
# fold -- it replaces buffer loads with the harness's bytes and leaves
# load_constant alone -- so that shape is not the one generated here.) The two
# cases take opposite sides of the same select.
#
# The indices are STRUCT FIELDS and not array elements, which is the second care
# taken: cpuref folds buffer loads before it folds anything else, so a load
# whose offset is still an iadd of an ishl is left alone and the comparison says
# "did not fold to a constant". A struct field's offset is a constant the
# frontend emits directly (air_ptr_add of an immediate); an array index of 4
# comes out as ishl, even when it is a literal. For the same reason the index
# AFTER the select is field 0: the frontend adds a constant field offset to an
# already-constant offset with a plain iadd and leaves the two immediates
# unfolded, which NIR and ir3 fold later and cpuref's one-pass fold does not --
# so the push-down is exercised with an offset of zero on both sides.
PSEL = """source_filename = "psel"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

%%S = type { <4 x float>, <4 x float>, <4 x float> }
%%T = type { <4 x float>, <4 x float> }

define <{ <4 x float> }> @psel(ptr addrspace(2) %%u) local_unnamed_addr {
  %%n = load i32, ptr addrspace(2) %%u, align 4
  %%t = and i32 %%n, 1
  %%c = icmp eq i32 %%t, %(take)d
  %%lo = getelementptr inbounds %%S, ptr addrspace(2) %%u, i64 0, i32 0
  %%hi = getelementptr inbounds %%S, ptr addrspace(2) %%u, i64 0, i32 1
  %%p = select i1 %%c, ptr addrspace(2) %%lo, ptr addrspace(2) %%hi
  %%g = getelementptr inbounds %%T, ptr addrspace(2) %%p, i64 0, i32 0
  %%v = load <4 x float>, ptr addrspace(2) %%g, align 16
  %%o = insertvalue <{ <4 x float> }> undef, <4 x float> %%v, 0
  ret <{ <4 x float> }> %%o
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @psel, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!6 = !{!7}
!7 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 48, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"S", !"air.arg_name", !"u"}
"""


def psel(take):
    return PSEL % dict(take=take)


PSTORE = """source_filename = "pstore"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float> }> @pstore(ptr addrspace(2) %u) local_unnamed_addr {
  %s = alloca ptr addrspace(2), align 8
  store ptr addrspace(2) %u, ptr %s, align 8
  %v = load <4 x float>, ptr addrspace(2) %u, align 16
  %o = insertvalue <{ <4 x float> }> undef, <4 x float> %v, 0
  ret <{ <4 x float> }> %o
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @pstore, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!6 = !{!7}
!7 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"float4", !"air.arg_name", !"u"}
"""

# A bitcast that changes shape, both directions. One buffer, one word read out
# of it, four floats out -- the same frame as every case above.
#
# Every one of these is made to depend on the buffer, and it is worth saying
# how, because the obvious way does not: a module that bitcast a literal
# constant would be folded on both sides to the same literal and would agree
# with LLVM whatever the frontend did with the load. So the word under test is
# `(the buffer's first byte - 13) + K`. The pattern cpuref fills a buffer with
# is `(i * 7 + 13) & 0xff`, so the first byte is 13 and the subtraction is
# zero -- for a frontend that read the buffer. One that read the wrong bytes
# gets a different word and two different halves out of it.
BITCAST = """source_filename = "bc"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float> }> @bc(ptr addrspace(2) %%u) local_unnamed_addr {
  %%v = load i32, ptr addrspace(2) %%u, align 4
  %%zz = and i32 %%v, 255
  %%dd = sub i32 %%zz, 13
%(body)s
  %%q0 = insertelement <4 x float> undef, float %%a, i32 0
  %%q1 = insertelement <4 x float> %%q0, float %%b, i32 1
  %%q2 = insertelement <4 x float> %%q1, float %%c, i32 2
  %%q3 = insertelement <4 x float> %%q2, float %%d, i32 3
  %%o = insertvalue <{ <4 x float> }> undef, <4 x float> %%q3, 0
  ret <{ <4 x float> }> %%o
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @bc, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!6 = !{!7}
!7 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"uint", !"air.arg_name", !"u"}
"""

# i32 -> <2 x half>: the two halves out as lanes 0 and 1, then their difference
# and their sum. The difference is there to make the ORDER visible as a sign
# and the sum is there to make it visible that the pair itself was right:
# transposing the halves negates lane 2 and leaves lane 3 alone, so a failure
# says which of the two went wrong instead of only that something did.
UNPACK = """  %%w = add i32 %%dd, %(word)d
  %%p = bitcast i32 %%w to <2 x half>
  %%h0 = extractelement <2 x half> %%p, i32 0
  %%h1 = extractelement <2 x half> %%p, i32 1
  %%a = fpext half %%h0 to float
  %%b = fpext half %%h1 to float
  %%c = fsub float %%a, %%b
  %%d = fadd float %%a, %%b"""

# <2 x half> -> i32, built the other way round so that nothing an unpack does
# can cancel out: two halves made with fptrunc from two floats the buffer gave,
# packed, and then the packed word's two 16-bit FIELDS reported as integers.
# Those integers are half bit patterns, exactly representable in a float32, so
# the comparison stays bit-exact -- which `uitofp` of the whole 32-bit word
# would not be, since a 32-bit integer does not fit a 24-bit mantissa.
# Literal text and not a template, so the per-cent signs are single here where
# UNPACK's are doubled: UNPACK is formatted once with its word and this is not.
PACK = """  %zf = uitofp i32 %zz to float
  %yf = fadd float %zf, 100.0
  %hx = fptrunc float %zf to half
  %hy = fptrunc float %yf to half
  %e0 = insertelement <2 x half> undef, half %hx, i32 0
  %e1 = insertelement <2 x half> %e0, half %hy, i32 1
  %i = bitcast <2 x half> %e1 to i32
  %lo = and i32 %i, 65535
  %hi = lshr i32 %i, 16
  %a = uitofp i32 %lo to float
  %b = uitofp i32 %hi to float
  %c = fsub float %a, %b
  %d = fadd float %a, %b"""


def half_bits(x):
    """The IEEE-754 binary16 bit pattern of x, from Python's own codec.

    Not from LLVM and not from this tree: this is what makes the expected
    numbers below a second reference rather than a restatement of the first.
    """
    return struct.unpack("<H", struct.pack("<e", x))[0]


def word_of(lo, hi):
    """The i32 whose LOW half is `lo` and whose high half is `hi`.

    The shift is written out rather than taken from a bitcast, because which
    half is element 0 is the thing under test.
    """
    w = (half_bits(hi) << 16) | half_bits(lo)
    return w - (1 << 32) if w >= (1 << 31) else w        # LLVM wants it signed


def unpack(lo, hi):
    return BITCAST % dict(body=UNPACK % dict(word=word_of(lo, hi)))


def half(bits):
    """The float value of a binary16 bit pattern, again from Python's codec."""
    return struct.unpack("<e", struct.pack("<H", bits))[0]


# The same four modules, for the Adreno. Everything above runs on this machine
# and ends at LLVM; `--emit` writes these into an image and `--check` judges the
# boot log, because the one executor that can actually run a 16-bit pack is the
# GPU this port is for. The expectations are the same numbers, and the `means`
# is what a wrong answer would say -- in every case the transposed order, which
# is the mistake this operation invites and the one that still draws.
RAW = 0x221B140D                                  # the buffer's own first word
BC_CASES = [
    dict(id="unpack_13_113", src=unpack(13.0, 113.0), expect=[13, 113, -100, 126],
         means="the two halves of one i32, 13 then 113, their difference and their sum. "
               "113 13 100 126 is the transposed order: the halves exchanged, the sum "
               "unchanged and the difference the wrong way round, which is what a pair "
               "of 16-bit values swapped inside a 32-bit register looks like."),
    dict(id="unpack_1_neg2", src=unpack(1.0, -2.0), expect=[1, -2, 3, -1],
         means="the same with a negative high half, so transposition also moves the sign: "
               "-2 1 -3 -1 would be it. A pair differing only in magnitude would leave a "
               "swapped unpack drawing dim rather than drawing wrong; this one would not."),
    dict(id="unpack_raw", src=BITCAST % dict(body=UNPACK % dict(word=0)),
         expect=[half(RAW & 0xffff), half(RAW >> 16),
                 half(RAW & 0xffff) - half(RAW >> 16),
                 half(RAW & 0xffff) + half(RAW >> 16)],
         means="the buffer's own word 0x221B140D bitcast as it lies -- an arbitrary bit "
               "pattern and not two round numbers, so the hardware is asked for halves "
               "nobody chose. The first two exchanged and the third negated is the "
               "transposed order."),
    dict(id="pack_13_113", src=BITCAST % dict(body=PACK),
         expect=[19072, 22288, -3216, 41360],
         means="the other direction: two halves made with fptrunc from 13 and 113, packed, "
               "and the packed word's two 16-bit fields read back as integers. 19072 and "
               "22288 are the binary16 patterns of 13 and 113, and 19072 must be the LOW "
               "field; 22288 19072 3216 41360 is a pack that put the first element high."),
]

JOBS = """# Commands run by the launchd job com.redwood.demo, generated by
# tools/air/loweringcheck.py --emit. Do not edit: regenerate it.
#
# A 16-BIT PACK ON THE ADRENO (RC_JOBS=rc.jobs.pack16). `i32 <-> <2 x half>` is
# the bitcast 2 927 of Resident Evil 4's 7 879 modules do, translated as NIR's
# pack_32_2x16 / unpack_32_2x16 (air_to_nir.c says why, and docs/air.md 8.6
# counts them). Nothing on this host can EXECUTE one: softpipe's nir_to_tgsi
# has no 16-bit arithmetic at all, so a module doing nothing but `fptrunc float
# to half` already aborts it with "Unknown NIR opcode: u2f16", bitcast or no
# bitcast. LLVM on the host agrees with our folder on all four of these and
# Python's own binary16 codec agrees with both; what none of that gives is a
# GPU. These four are that measurement.
#
# Carried into %(there)s by build.sh EXTRA_SHARE. The directory's own name is in
# the paths at the bottom of this file, so emit it under that name or
# regenerate this file from the one you used:
#   python3 tools/air/loweringcheck.py --emit <dir>
#   userland/build.sh ... EXTRA_SHARE=<dir> RC_JOBS=rc.jobs.pack16
# and judged afterwards, on the host, against expectations that are NOT in the
# image:
#   python3 tools/air/loweringcheck.py --check <dir> <the boot log>
#
# Read the "air_run: read back from ..." line of each block before its numbers:
# it names the screen the pixel came out of, and a fallback to softpipe would
# abort rather than answer -- so a block with numbers in it was the Adreno.
#
# The greps that read the log, for a reader who has only the log:
%(greps)s#   grep -ac 'Unknown NIR opcode' LOG    # must be 0: that is nir_to_tgsi,
#                                     # which means softpipe, which means no GPU
# (-a on every one of them: a boot log has NUL bytes in it.)
#
%(expect)s
"""


RESULT = re.compile(r"^\s+(-?[0-9.einf+-]+)\s*$")


def result_values(out):
    """The numbers LLVM printed: the block after "says the result is:"."""
    vals = []
    for line in out.splitlines():
        if "says the result is" in line:
            vals = []
            continue
        m = RESULT.match(line)
        if m:
            vals.append(m.group(1))
        elif vals:
            break
    return vals


def run(ll, want, work, n, verbose):
    """Returns (ok, message, arm or None). want is "arm", "agree",
    "values:a,b,c,d" (agreement AND these exact numbers) or
    "refuse:<text that must be in the refusal>"."""
    src = os.path.join(work, "m%d.ll" % n)
    bc = os.path.join(work, "m%d.bc" % n)
    with open(src, "w") as f:
        f.write(ll)
    q = subprocess.run(["llvm-as", "-o", bc, src], capture_output=True)
    if q.returncode != 0:
        return False, "llvm-as: " + q.stderr.decode().strip().splitlines()[0], None

    q = subprocess.run([sys.executable, CPUREF, "--compare", bc], capture_output=True)
    out = q.stdout.decode()
    if verbose:
        print(out)

    if want.startswith("refuse:"):
        why = want.split(":", 1)[1]
        if "they agree" in out:
            return False, "translated, and it should have been refused", None
        if why not in out:
            last = [l for l in out.strip().splitlines() if l.strip()]
            return False, "refused, but for %r" % (last[-1] if last else "?"), None
        return True, "refused: " + why, None

    if "they agree" not in out:
        last = [l for l in out.strip().splitlines() if l.strip()]
        return False, (last[-1] if last else "cpuref said nothing"), None
    if want == "agree":
        return True, "agree with LLVM", None

    if want.startswith("values:"):
        # Agreement is LLVM's; these numbers are this script's, and the two
        # being independent is the whole reason for asserting both. The values
        # are integers on purpose, so "equal" means equal and not "close".
        expect = [float(v) for v in want.split(":", 1)[1].split(",")]
        vals = result_values(out)
        got = [float(v) for v in vals[:len(expect)]]
        if len(vals) != len(expect) or got != expect:
            return False, "agreed with LLVM, but on %r and not %r" % (vals, expect), None
        return True, "agree with LLVM, and on %s" % " ".join(vals), None

    # which arm was reached: the lane LLVM set. The block after "says the
    # result is:" is the four floats.
    vals = result_values(out)
    arm = None
    for i, v in enumerate(vals[:4]):
        if float(v) == 1.0:
            arm = i
    if arm is None:
        return False, "agreed, but no arm set a lane: %r" % vals[:4], None
    return True, "agree with LLVM, arm %d taken" % arm, arm


def wrap(s, n):
    out, line = [], ""
    for w in s.split():
        if len(line) + len(w) + 1 > n:
            out.append(line)
            line = w
        else:
            line = (line + " " + w).strip()
    if line:
        out.append(line)
    return out


def emit(outdir):
    """The pack/unpack modules as bitcode, plus the rc.jobs that runs them."""
    os.makedirs(outdir, exist_ok=True)
    there = "/redwood/share/" + os.path.basename(os.path.normpath(outdir))
    manifest, greps, expect = [], [], []
    for case in BC_CASES:
        src = os.path.join(outdir, case["id"] + ".ll")
        bc = os.path.join(outdir, case["id"] + ".bc")
        with open(src, "w") as f:
            f.write(case["src"])
        q = subprocess.run(["llvm-as", "-o", bc, src], capture_output=True)
        os.remove(src)
        if q.returncode != 0:
            print("loweringcheck: %s: %s" % (case["id"], q.stderr.decode().strip()),
                  file=sys.stderr)
            return 1
        manifest.append(dict(id=case["id"], expect=case["expect"], means=case["means"]))
        # The slash and the extension are not decoration: `pack_13_113` is a
        # substring of `unpack_13_113`, so a grep for the bare id prints both
        # blocks and the reader compares the wrong numbers.
        greps.append("#   grep -a 'air_run: .*/%s[.]bc' -A 8 LOG   # expect %s\n"
                     % (case["id"], " ".join("%.9g" % v for v in case["expect"])))
        expect.append("# %s must print %s --\n#   %s"
                      % (case["id"], " ".join("%.9g" % v for v in case["expect"]),
                         "\n#   ".join(wrap(case["means"], 72))))
    with open(os.path.join(outdir, "manifest.json"), "w") as f:
        json.dump(manifest, f)
    with open(os.path.join(outdir, "rc.jobs"), "w") as f:
        f.write(JOBS % dict(there=there, greps="".join(greps),
                            expect="\n".join(expect) + "\n"))
        for m in manifest:
            f.write("/redwood/bin/air_run %s/%s.bc\n" % (there, m["id"]))
    print("loweringcheck: %d modules in %s, with rc.jobs and manifest.json"
          % (len(manifest), outdir))
    print("loweringcheck: the greps that read its log, whatever carried it:")
    for g in greps:
        print("  " + g.split("#   ")[1].rstrip())
    print("  grep -ac 'Unknown NIR opcode' LOG    # must be 0: that is softpipe, "
          "so the Adreno was never asked")
    return 0


def check(outdir, logpath):
    """The boot log against the expectations, which are not in the image.

    The numbers are the halves of a 32-bit word and are exactly representable,
    so this compares them exactly. A tolerance here would accept the one wrong
    answer worth catching: two halves that are each a little off is not a
    failure mode of a bit movement, and two halves in the wrong order is.
    """
    with open(os.path.join(outdir, "manifest.json")) as f:
        manifest = json.load(f)
    with open(logpath, "rb") as f:
        log = f.read().decode("utf-8", "replace")
    if "Unknown NIR opcode" in log:
        print("loweringcheck: the log has 'Unknown NIR opcode' in it, which is "
              "nir_to_tgsi: this ran on softpipe and says nothing about the Adreno")
        return 1
    # One chunk per air_run invocation, cut at its own first line. Searching
    # the whole log for the next "came back as" after a module's name would
    # read the NEXT module's pixel when this one aborted, and report a pass.
    chunks = re.split(r"(?=air_run: \S+ on )", log)
    bad = 0
    for m in manifest:
        # The whole file name and not the id as a substring: `pack_13_113` is a
        # substring of `unpack_13_113`, so a looser match reads the unpack's
        # pixel for the pack and reports whichever of them ran.
        named = re.compile(r"(?:^|[/\s])" + re.escape(m["id"]) + r"\.bc(?:\s|$)")
        mine = [c for c in chunks
                if c.splitlines() and named.search(c.splitlines()[0])]
        got = numbers_after(mine[0]) if mine else None
        if not got:
            print("FAIL %-16s %s" % (m["id"], "no pixel in its block" if mine
                                     else "it did not run at all"))
            bad += 1
            continue
        # Compared at the precision air_run prints, which is nine significant
        # digits: enough to tell any two distinct float32 values apart, so this
        # is an exact comparison and not a tolerance. A tolerance would accept
        # the one wrong answer worth catching -- two halves in the wrong order.
        fmt = ["%.9g" % v for v in got[:len(m["expect"])]]
        if fmt != ["%.9g" % v for v in m["expect"]]:
            print("FAIL %-16s %s, wanted %s"
                  % (m["id"], " ".join(fmt), " ".join("%.9g" % v for v in m["expect"])))
            bad += 1
            continue
        print("     %-16s %s" % (m["id"], " ".join(fmt)))
    print("loweringcheck: %d of %d modules drew what LLVM and the binary16 codec "
          "both say they should" % (len(manifest) - bad, len(manifest)))
    return 1 if bad else 0


def numbers_after(text, marker="came back as"):
    i = text.find(marker)
    if i < 0:
        return None
    out = []
    for line in text[i:].splitlines()[1:]:
        t = line.strip()
        if t.startswith("air_run:") or t.startswith("=== "):
            break
        try:
            out.append(float(t))
        except ValueError:
            continue
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--emit", metavar="DIR",
                    help="write the pack/unpack modules and an rc.jobs for the phone")
    ap.add_argument("--check", nargs=2, metavar=("DIR", "LOG"),
                    help="judge a boot log against the expectations in DIR")
    args = ap.parse_args()

    if args.emit:
        return emit(args.emit)
    if args.check:
        return check(args.check[0], args.check[1])

    if not os.path.exists(os.path.join(
            ROOT, "build/mesa-host/build/src/compiler/air/air2nir")):
        print("loweringcheck: run tools/air/mk-host.sh first", file=sys.stderr)
        return 2

    ok, arms = 0, set()
    all_cases = cases()
    with tempfile.TemporaryDirectory(prefix="loweringcheck") as work:
        for n, (label, ll, want) in enumerate(all_cases):
            good, msg, arm = run(ll, want, work, n, args.verbose)
            ok += good
            if arm is not None:
                arms.add(arm)
            print("%-4s %-34s %s" % ("" if good else "FAIL", label, msg))

    refusals = sum(1 for _, _, w in all_cases if w.startswith("refuse:"))
    pinned = sum(1 for _, _, w in all_cases if w.startswith("values:"))
    print("loweringcheck: %d of %d modules behaved as expected -- %d compared against LLVM "
          "on this processor, %d of those also against numbers computed here, "
          "%d refused by name"
          % (ok, len(all_cases), len(all_cases) - refusals, pinned, refusals))
    enough = len(arms) >= 3
    print("loweringcheck: %d distinct switch arms were reached (%s) -- %s"
          % (len(arms), ", ".join(str(a) for a in sorted(arms)),
             "so agreement is about the chain and not about one path"
             if enough else
             "TOO FEW: a lowering that always took one arm would pass this"))
    return 0 if ok == len(all_cases) and enough else 1


if __name__ == "__main__":
    sys.exit(main())
