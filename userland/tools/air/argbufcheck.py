#!/usr/bin/env python3
"""Argument buffers: the member table, the indices it allocates, and the refusals.

`air.indirect_buffer` is the one interface word that cannot be checked against
Apple's own libraries: 0 of the 456 modules of iOS 16.5's corpus declare one and
7 620 of Resident Evil 4's 7 879 do (docs/air.md section 8.2). So this is the
stage that belongs with the implementation, as vattrcheck.py belongs with the
vertex attributes -- the numbers in that section are re-derivable by `--census`
here, and the behaviour is checked against two references that are not each
other.

What each part is worth:

  --census CORPUS      the metadata, parsed again by this script and NOT by the
                       frontend: how many modules declare a set, how many
                       members of each kind, how many carry `air.address_space`
                       (none), how many span more than one descriptor index
                       (none), and whether the module's own struct type agrees
                       with `air.struct_type_info` member for member. It is a
                       second reading of the same bytes, so a frontend that
                       misread them disagrees with it.

  the interface        air2nir --interface against the table this script wrote
                       the module from: every member's kind, the flat index the
                       frontend allocated, and the Metal (set, id) it came
                       from. This is the one check that looks at the ALLOCATION
                       -- a member's own air.location_index is per set and is
                       not a binding, so the frontend chooses, and a choice
                       nothing checks is a choice nobody can rely on.

  dispatched           air_run compiles the kernel and runs it on softpipe,
                       with DIFFERENT BYTES IN EVERY SLOT (--buffer N@off=).
                       That is the point: every other buffer check in this
                       project fills all buffers with one pattern, so a shader
                       that read the wrong slot would return the right answer.
                       Here slot 1 holds 256+i and slot 2 holds 512+i and the
                       kernel puts one in each half of its result, so a swap,
                       an unbound slot and an off-by-one are all visible.

  against LLVM         the same module, with its address spaces flattened and a
                       harness appended that builds the set out of real
                       pointers, compiled by clang and run on this processor.
                       LLVM -- not our reader and not our translator -- decides
                       what the shader means, and it agrees word for word with
                       what softpipe computed. Two references that share no
                       code.

  the refusals         nine shapes that must be refused BY NAME rather than
                       drawn over the wrong resource. The frontend names 23
                       around argument buffers; these nine are the ones a
                       module can be written for here, and the test is that
                       the message is the one the design names and not a
                       symptom further down.

What this does NOT show, said here rather than left to be assumed:

  - nothing about the runtime's half. There is no argument buffer to write
    (docs/air.md section 8.2): the frontend resolved every descriptor, so
    MTLArgumentEncoder has to RECORD rather than write, and no part of that is
    exercised or even present here.
  - the sampler member's INDEX is not checked, only that it resolves and
    samples. air_run binds one sampler state -- nearest, clamp -- to every
    slot, so a sampler read from the wrong slot gives the right texel. The
    texture indices ARE checked: air_run puts 1000*slot in each texel.
  - a 2-bit or 4-bit integer is refused and not translated, so the modules of
    Resident Evil 4 that carry HLSL bit fields are counted nowhere here.

usage: argbufcheck.py [--keep DIR]
       argbufcheck.py --census FILE...        (.metallib or module bitcode)
       argbufcheck.py --census --list FILE    (one path per line)
"""
import argparse
import collections
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AIR2NIR = os.environ.get(
    "AIR2NIR", os.path.join(ROOT, "build/mesa-host/build/src/compiler/air/air2nir"))
AIR_RUN = os.environ.get(
    "AIR_RUN",
    os.path.join(ROOT, "build/mesa-host/build/src/gallium/drivers/freedreno/air_run"))

THREADS = 16          # air_run dispatches 4x4 in one workgroup
TEX = 4               # air_run's texture is 4x4
SENTINEL = 0xDEADBEEF  # what the output slot starts as


def slot_base(index):
    """What this script writes in the buffer bound at `index`.

    Per SLOT and not per kind, which is the whole point: a check that filled
    every buffer alike would pass for a frontend that resolved a member to the
    wrong slot, and that is the mistake this project has already made three
    times. Slot n holds 0x100*(n+1) + i in word i, so a swap between any two
    slots, an unbound slot (zeros) and an off-by-one in the offset are each a
    different wrong answer.
    """
    return 0x100 * (index + 1)

# air_resource_kind, as nir_air.h orders it
KIND = {0: "constant buffer", 1: "device buffer", 2: "threadgroup buffer",
        3: "texture", 4: "image", 5: "sampler"}


# -- the modules, written here so that what they declare is known ------------

HEAD = '''source_filename = "%s"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios14.0.0"

%%"struct.metal::sampler" = type { ptr addrspace(2) }
%%"struct.metal::texture2d" = type { ptr addrspace(1) }
%%struct.spvDescriptorSetBuffer0 = type { %s }
'''

TAIL = '''
!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.kernel = !{!3}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 3, i32 0}
!2 = !{!"Metal", i32 2, i32 3, i32 0}
!3 = !{ptr @%s, !4, !5}
!4 = !{}
!5 = !{!6, !100}
!6 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 %d, !"air.location_index", i32 0, i32 1, !"air.read", !"air.struct_type_info", !7, !"air.arg_type_size", i32 %d, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"spvDescriptorSetBuffer0", !"air.arg_name", !"spvDescriptorSet0"}
!7 = !{%s}
%s
!100 = !{i32 1, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
'''

# One member of a generated set: how it appears in the LLVM struct type, in
# `air.struct_type_info`, in its own `air.indirect_argument` descriptor, and
# what the frontend is then expected to report for it.
MEMBERS = {
    # name:        (llvm type,                     kind key,      access,          kind, size)
    "devrw":       ("ptr addrspace(1)",            "air.buffer",  "air.read_write", 1),
    "devro":       ("ptr addrspace(1)",            "air.buffer",  "air.read",       1),
    "constro":     ("ptr addrspace(2)",            "air.buffer",  "air.read",       0),
    "sampler":     ('%"struct.metal::sampler"',    "air.sampler", None,             5),
    "texture":     ('%"struct.metal::texture2d"',  "air.texture", "air.sample",     3),
}


def build(name, members, body, extra_decls="", member_override=None):
    """One AIR module: a kernel taking an argument buffer of `members`."""
    llvm = ", ".join(MEMBERS[m][0] for m in members)
    size = 8 * len(members)

    sti, nodes = [], []
    for i, m in enumerate(members):
        _, key, access, _ = MEMBERS[m]
        nid = 10 + i
        over = (member_override or {}).get(i, {})
        sti.append('i32 %d, i32 %d, i32 0, !"ty_%s_%d", !"%s%d", !"air.indirect_argument", !%d'
                   % (over.get("offset", 8 * i), over.get("size", 8), m, i, m, i, nid))
        span = over.get("span", 1)
        acc = over.get("access", access)
        nodes.append('!%d = !{i32 %d, !"%s", !"air.location_index", i32 %d, i32 %d%s, '
                     '!"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, '
                     '!"air.arg_type_name", !"ty_%s_%d", !"air.arg_name", !"%s%d"}'
                     % (nid, i, over.get("key", key), i, span,
                        ', !"%s"' % acc if acc else "", m, i, m, i))

    return (HEAD % (name, llvm) + extra_decls
            + "\ndefine void @%s(ptr addrspace(2) %%0, i32 %%1) local_unnamed_addr {\n"
              % name
            + body + "\n  ret void\n}\n"
            + TAIL % (name, size, size, ", ".join(sti), "\n".join(nodes), ))


def chain(var, idx, space, three=False, wrapper=None):
    """GEP into member `idx` and load the handle out of it.

    `three` picks the one-getelementptr form (`i64 0, i32 n, i32 0`), which is
    what SPIRV-Cross emits for a sampler; `wrapper` picks the two-step form
    (the member, then the wrapper's field), which is what it emits for a
    texture. Keying the member table on the BYTE offset is what makes all three
    one case, and all three are here so that claim is tested and not assumed.
    """
    if wrapper:
        return ('  %{v}a = getelementptr inbounds %struct.spvDescriptorSetBuffer0, '
                'ptr addrspace(2) %0, i64 0, i32 {i}\n'
                '  %{v}b = getelementptr inbounds {w}, ptr addrspace(2) %{v}a, '
                'i64 0, i32 0\n'
                '  %{v} = load ptr addrspace({s}), ptr addrspace(2) %{v}b, align 8\n'
                ).format(v=var, i=idx, s=space, w=wrapper)
    tail = ", i32 0" if three else ""
    return ('  %{v}a = getelementptr inbounds %struct.spvDescriptorSetBuffer0, '
            'ptr addrspace(2) %0, i64 0, i32 {i}{t}\n'
            '  %{v} = load ptr addrspace({s}), ptr addrspace(2) %{v}a, align 8\n'
            ).format(v=var, i=idx, s=space, t=tail)


SAMPLE = ("air.sample_texture_2d.v4f32",
          "declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), "
          "ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32) "
          "local_unnamed_addr\n")
READ = ("air.read_texture_2d.v4f32",
        "declare { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1), "
        "<2 x i32>, i32, i32) local_unnamed_addr\n")


def texel0(slot):
    """Channel 0 of texel (1,1) of the texture air_run binds at `slot`."""
    return 100.0 + 1 + 1000.0 * slot


def cases():
    """(label, members, source, want words, want resources, run under LLVM, want NIR)."""
    out = []

    # -- three buffer members, every one reached by the two-index shape -----
    members = ["devrw", "constro", "devro"]
    body = (chain("out", 0, 1) + chain("cb", 1, 2) + chain("db", 2, 1)
            + '  %oi = zext i32 %1 to i64\n'
              '  %cp = getelementptr inbounds i32, ptr addrspace(2) %cb, i64 %oi\n'
              '  %dp = getelementptr inbounds i32, ptr addrspace(1) %db, i64 %oi\n'
              '  %cv = load i32, ptr addrspace(2) %cp, align 4\n'
              '  %dv = load i32, ptr addrspace(1) %dp, align 4\n'
              '  %hi = shl i32 %cv, 16\n'
              '  %r = add i32 %hi, %dv\n'
              '  %op = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %oi\n'
              '  store i32 %r, ptr addrspace(1) %op, align 4\n')
    out.append(("three buffers, read at Metal's own offsets", members,
                build("argbuf_buffers", members, body),
                [((slot_base(1) + i) << 16) + slot_base(2) + i for i in range(THREADS)],
                # (kind, index, set, id) per member, in declaration order
                [(1, 0, 0, 0), (0, 1, 0, 1), (1, 2, 0, 2)], True, []))

    # -- a sampler and two textures, the wrapper shapes ---------------------
    #
    # The coordinate is 1.5 and not 1.5/4: air_run's COMPUTE path binds its
    # samplers with unnormalized_coords (sampler_for_slot(..., true, ...)), so
    # a texel centre is a pixel centre here and a normalized coordinate reads
    # texel 0. Measured, not assumed -- 0.375 returned texel (0,0)'s value.
    members = ["devrw", "sampler", "texture", "texture"]
    u = 1 + 0.5
    body = (chain("out", 0, 1)
            + chain("samp", 1, 2, three=True)
            + chain("t0", 2, 1, wrapper='%"struct.metal::texture2d"')
            + chain("t1", 3, 1, wrapper='%"struct.metal::texture2d"')
            + ('  %%sr = tail call { <4 x float>, i8 } @%s(ptr addrspace(1) %%t0, '
               'ptr addrspace(2) %%samp, <2 x float> <float %.9e, float %.9e>, '
               'i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, '
               'i32 0)\n' % (SAMPLE[0], u, u))
            + '  %sv = extractvalue { <4 x float>, i8 } %sr, 0\n'
              '  %s0 = extractelement <4 x float> %sv, i64 0\n'
            + ('  %%rr = tail call { <4 x float>, i8 } @%s(ptr addrspace(1) %%t1, '
               '<2 x i32> <i32 1, i32 1>, i32 0, i32 0)\n' % READ[0])
            + '  %rv = extractvalue { <4 x float>, i8 } %rr, 0\n'
              '  %r0 = extractelement <4 x float> %rv, i64 0\n'
              '  %si = fptoui float %s0 to i32\n'
              '  %ri = fptoui float %r0 to i32\n'
              '  %hi = shl i32 %si, 16\n'
              '  %r = add i32 %hi, %ri\n'
              '  %oi = zext i32 %1 to i64\n'
              '  %op = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %oi\n'
              '  store i32 %r, ptr addrspace(1) %op, align 4\n')
    # What softpipe gives, which is NOT what the slots hold: its compute path
    # answers both textures with the sampler view bound at 0, so texture 1's
    # 1000 never appears. That is the harness's and not this feature's, and
    # `control()` below demonstrates it with DIRECT texture arguments rather
    # than leaving it asserted here. So the value this expects carries slot 0
    # twice, and the resolution itself is checked where it is visible: the
    # texture and sampler numbers on the NIR instructions.
    out.append(("a sampler and two textures, through their wrappers", members,
                build("argbuf_resources", members, body,
                      extra_decls=SAMPLE[1] + READ[1]),
                [(int(texel0(0)) << 16) + int(texel0(0))] * THREADS,
                [(1, 0, 0, 0), (5, 0, 0, 1), (3, 0, 0, 2), (3, 1, 0, 3)], False,
                # (opcode, texture index, sampler index) the NIR must carry
                [("tex", 0, 0), ("txf", 1, None)]))

    # -- a phi of two copies of ONE getelementptr --------------------------
    # LLVM duplicates a getelementptr onto two paths and phis the copies back;
    # 207 of Resident Evil 4's pointer phis are that and none is anything else.
    members = ["devrw", "devro"]
    body = ('  %oi = zext i32 %1 to i64\n'
            + chain("out", 0, 1)
            + '  %c = icmp ult i32 %1, 8\n'
              '  br i1 %c, label %ta, label %tb\n'
              'ta:\n'
            + chain("pa", 1, 1)
            + '  br label %j\n'
              'tb:\n'
            + chain("pb", 1, 1)
            + '  br label %j\n'
              'j:\n'
              '  %db = phi ptr addrspace(1) [ %pa, %ta ], [ %pb, %tb ]\n'
              '  %dp = getelementptr inbounds i32, ptr addrspace(1) %db, i64 %oi\n'
              '  %dv = load i32, ptr addrspace(1) %dp, align 4\n'
              '  %op = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %oi\n'
              '  store i32 %dv, ptr addrspace(1) %op, align 4\n')
    out.append(("a phi of two copies of one member's pointer", members,
                build("argbuf_phi", members, body),
                [slot_base(1) + i for i in range(THREADS)],
                [(1, 0, 0, 0), (1, 1, 0, 1)], False, []))

    # -- two sets, whose members are numbered independently -----------------
    out.append(("two sets, both numbering their members from 0", ["devrw"],
                TWO_SETS,
                [((slot_base(1) + i) << 22) + ((slot_base(2) + i) << 11)
                 + slot_base(3) + i for i in range(THREADS)],
                [(1, 0, 0, 0), (0, 1, 0, 1), (1, 2, 1, 0), (0, 3, 1, 1)], False, []))
    return out


# Two sets in one kernel: the case the whole index allocation exists for.
#
# Both sets' member 0 says `air.location_index 0` and both their member 1 says
# 1, because a member's index is per set -- so the metadata alone cannot bind
# them and the frontend has to choose. 237 of Resident Evil 4's modules are
# this shape. Written out rather than generated, so that what it declares can
# be read beside what the frontend is expected to answer:
#
#   set 0 (Metal buffer 0): member 0 device read_write, member 1 constant read
#   set 1 (Metal buffer 1): member 0 device read,       member 1 constant read
#
# and the flat per-kind counter, walked in (set, member) order, must give
# buffer 0, 1, 2, 3 -- so set 1's member 0 is buffer 2 and not buffer 0.
TWO_SETS = '''source_filename = "argbuf_two_sets"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios14.0.0"

%struct.spvDescriptorSetBuffer0 = type { ptr addrspace(1), ptr addrspace(2) }
%struct.spvDescriptorSetBuffer1 = type { ptr addrspace(1), ptr addrspace(2) }

define void @argbuf_two_sets(ptr addrspace(2) %0, ptr addrspace(2) %1, i32 %2) local_unnamed_addr {
  %oi = zext i32 %2 to i64
  %pa = getelementptr inbounds %struct.spvDescriptorSetBuffer0, ptr addrspace(2) %0, i64 0, i32 0
  %out = load ptr addrspace(1), ptr addrspace(2) %pa, align 8
  %pb = getelementptr inbounds %struct.spvDescriptorSetBuffer0, ptr addrspace(2) %0, i64 0, i32 1
  %c0 = load ptr addrspace(2), ptr addrspace(2) %pb, align 8
  %pc = getelementptr inbounds %struct.spvDescriptorSetBuffer1, ptr addrspace(2) %1, i64 0, i32 0
  %d1 = load ptr addrspace(1), ptr addrspace(2) %pc, align 8
  %pd = getelementptr inbounds %struct.spvDescriptorSetBuffer1, ptr addrspace(2) %1, i64 0, i32 1
  %c1 = load ptr addrspace(2), ptr addrspace(2) %pd, align 8
  %q0 = getelementptr inbounds i32, ptr addrspace(2) %c0, i64 %oi
  %q1 = getelementptr inbounds i32, ptr addrspace(1) %d1, i64 %oi
  %q2 = getelementptr inbounds i32, ptr addrspace(2) %c1, i64 %oi
  %v0 = load i32, ptr addrspace(2) %q0, align 4
  %v1 = load i32, ptr addrspace(1) %q1, align 4
  %v2 = load i32, ptr addrspace(2) %q2, align 4
  %s0 = shl i32 %v0, 22
  %s1 = shl i32 %v1, 11
  %t0 = add i32 %s0, %s1
  %r = add i32 %t0, %v2
  %op = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %oi
  store i32 %r, ptr addrspace(1) %op, align 4
  ret void
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.kernel = !{!3}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 3, i32 0}
!2 = !{!"Metal", i32 2, i32 3, i32 0}
!3 = !{ptr @argbuf_two_sets, !4, !5}
!4 = !{}
!5 = !{!6, !20, !30}
!6 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 0, i32 1, !"air.read", !"air.struct_type_info", !7, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"spvDescriptorSetBuffer0", !"air.arg_name", !"spvDescriptorSet0"}
!7 = !{i32 0, i32 8, i32 0, !"ty_out", !"s0_out", !"air.indirect_argument", !8, i32 8, i32 8, i32 0, !"ty_cb", !"s0_cb", !"air.indirect_argument", !9}
!8 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"ty_out", !"air.arg_name", !"s0_out"}
!9 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"ty_cb", !"air.arg_name", !"s0_cb"}
!20 = !{i32 1, !"air.indirect_buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 1, i32 1, !"air.read", !"air.struct_type_info", !21, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"spvDescriptorSetBuffer1", !"air.arg_name", !"spvDescriptorSet1"}
!21 = !{i32 0, i32 8, i32 0, !"ty_db", !"s1_db", !"air.indirect_argument", !22, i32 8, i32 8, i32 0, !"ty_cb", !"s1_cb", !"air.indirect_argument", !23}
!22 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"ty_db", !"air.arg_name", !"s1_db"}
!23 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"ty_cb", !"air.arg_name", !"s1_cb"}
!30 = !{i32 2, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
'''


CONTROL = '''source_filename = "two_direct_textures"
target triple = "air64-apple-ios14.0.0"

declare { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, i32, i32) local_unnamed_addr

define void @two_direct_textures(ptr addrspace(1) %0, ptr addrspace(1) %1, ptr addrspace(1) %2, i32 %3) local_unnamed_addr {
  %a = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %1, <2 x i32> <i32 1, i32 1>, i32 0, i32 0)
  %av = extractvalue { <4 x float>, i8 } %a, 0
  %a0 = extractelement <4 x float> %av, i64 0
  %b = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %2, <2 x i32> <i32 1, i32 1>, i32 0, i32 0)
  %bv = extractvalue { <4 x float>, i8 } %b, 0
  %b0 = extractelement <4 x float> %bv, i64 0
  %ai = fptoui float %a0 to i32
  %bi = fptoui float %b0 to i32
  %hi = shl i32 %ai, 16
  %r = add i32 %hi, %bi
  %oi = zext i32 %3 to i64
  %op = getelementptr inbounds i32, ptr addrspace(1) %0, i64 %oi
  store i32 %r, ptr addrspace(1) %op, align 4
  ret void
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.kernel = !{!3}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 3, i32 0}
!2 = !{!"Metal", i32 2, i32 3, i32 0}
!3 = !{ptr @two_direct_textures, !4, !5}
!4 = !{}
!5 = !{!6, !7, !8, !9}
!6 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 256, !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!7 = !{i32 1, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"texture2d<float, access::read>", !"air.arg_name", !"t0"}
!8 = !{i32 2, !"air.texture", !"air.location_index", i32 1, i32 1, !"air.read", !"air.arg_type_name", !"texture2d<float, access::read>", !"air.arg_name", !"t1"}
!9 = !{i32 3, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
'''


def control(d):
    """Can this harness tell one texture slot from another at all?

    It claims to: air_run's texel_value puts 1000*slot in channel 0 precisely
    so that a misbinding shows. For a FRAGMENT shader it does. For a COMPUTE
    kernel on softpipe it does not -- both a sample and a texel fetch come back
    with the view bound at slot 0 -- and this says so with two DIRECT texture
    arguments, where no argument buffer is involved and the Metal indices are
    the shader's own. Without this control, the texture case above would read
    as a bug in the member table; with it, the argument-buffer case is checked
    where the slot is visible instead, on the NIR instruction.
    """
    bc, why = assemble(d, "two_direct_textures", CONTROL)
    if not bc:
        return None, why
    q = subprocess.run([AIR_RUN, bc], capture_output=True, text=True)
    bufs = readback(q.stdout + q.stderr)
    if 0 not in bufs or not bufs[0]:
        return None, "air_run read no buffer back"
    w = bufs[0][0]
    return (w >> 16, w & 0xffff), None


# -- the refusals ------------------------------------------------------------

def refusals():
    """(label, source, the words the message must contain)."""
    out = []
    ok = chain("out", 0, 1) + (
        '  %oi = zext i32 %1 to i64\n'
        '  %op = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %oi\n'
        '  store i32 7, ptr addrspace(1) %op, align 4\n')

    # a member that is none of the three kinds this frontend binds
    out.append(("a member that is not a buffer, a texture or a sampler",
                build("r1", ["devrw", "devro"], ok,
                      member_override={1: {"key": "air.command_buffer"}}),
                "is air.command_buffer, and this frontend binds a buffer"))

    # a member that spans more than one descriptor index: an array of them,
    # which the shader could index at run time
    out.append(("a member spanning more than one descriptor index",
                build("r2", ["devrw", "devro"], ok,
                      member_override={1: {"span": 2}}),
                "spans 2 descriptor indices"))

    # a set inside a set
    out.append(("a nested argument buffer",
                build("r3", ["devrw", "devro"], ok,
                      member_override={1: {"key": "air.indirect_buffer"}}),
                "is itself an argument buffer"))

    # a load through the set at an offset that is not constant
    body = (chain("out", 0, 1)
            + '  %oi = zext i32 %1 to i64\n'
              '  %q = getelementptr inbounds ptr addrspace(1), ptr addrspace(2) %0, '
              'i64 %oi\n'
              '  %db = load ptr addrspace(1), ptr addrspace(2) %q, align 8\n'
              '  %dp = getelementptr inbounds i32, ptr addrspace(1) %db, i64 %oi\n'
              '  %dv = load i32, ptr addrspace(1) %dp, align 4\n'
              '  %op = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %oi\n'
              '  store i32 %dv, ptr addrspace(1) %op, align 4\n')
    out.append(("a descriptor chosen at run time",
                build("r4", ["devrw", "devro"], body),
                "at an offset that is not constant"))

    # a phi between two DIFFERENT members
    body = ('  %oi = zext i32 %1 to i64\n'
            + chain("out", 0, 1)
            + '  %c = icmp ult i32 %1, 8\n'
              '  br i1 %c, label %ta, label %tb\n'
              'ta:\n'
            + chain("pa", 1, 1)
            + '  br label %j\n'
              'tb:\n'
            + chain("pb", 2, 1)
            + '  br label %j\n'
              'j:\n'
              '  %db = phi ptr addrspace(1) [ %pa, %ta ], [ %pb, %tb ]\n'
              '  %dp = getelementptr inbounds i32, ptr addrspace(1) %db, i64 %oi\n'
              '  %dv = load i32, ptr addrspace(1) %dp, align 4\n'
              '  %op = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %oi\n'
              '  store i32 %dv, ptr addrspace(1) %op, align 4\n')
    out.append(("a phi between two different members",
                build("r5", ["devrw", "devro", "devro"], body),
                "name different places"))

    # A select between two different places IN THE SET -- the set's own pointer
    # chosen, not two handles already loaded out of it. The difference matters:
    # a select between two LOADED buffer pointers is a choice between two
    # bindings, which AIR_VAL_PTR_SELECT already answers by loading both sides,
    # and it is left alone. This one would have to choose a DESCRIPTOR, and
    # there is no loading both sides of that.
    body = ('  %oi = zext i32 %1 to i64\n'
            + chain("out", 0, 1)
            + '  %pa = getelementptr inbounds %struct.spvDescriptorSetBuffer0, '
              'ptr addrspace(2) %0, i64 0, i32 1\n'
              '  %pb = getelementptr inbounds %struct.spvDescriptorSetBuffer0, '
              'ptr addrspace(2) %0, i64 0, i32 2\n'
              '  %c = icmp ult i32 %1, 8\n'
              '  %q = select i1 %c, ptr addrspace(2) %pa, ptr addrspace(2) %pb\n'
              '  %db = load ptr addrspace(1), ptr addrspace(2) %q, align 8\n'
              '  %dp = getelementptr inbounds i32, ptr addrspace(1) %db, i64 %oi\n'
              '  %dv = load i32, ptr addrspace(1) %dp, align 4\n'
              '  %op = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %oi\n'
              '  store i32 %dv, ptr addrspace(1) %op, align 4\n')
    out.append(("a select between two different members",
                build("r6", ["devrw", "devro", "devro"], body),
                "two different places in an argument buffer"))

    # the metadata and the module's own struct type disagreeing about a member
    out.append(("a member the module's struct type puts somewhere else",
                build("r7", ["devrw", "devro"], ok,
                      member_override={1: {"offset": 12}}),
                "the module's struct type puts"))

    # a value, not a descriptor, read out of the set: Metal's
    # constantDataAtIndex:, which the runtime's recorder has no bytes for
    body = (chain("out", 0, 1)
            + '  %oi = zext i32 %1 to i64\n'
              '  %q = getelementptr inbounds %struct.spvDescriptorSetBuffer0, '
              'ptr addrspace(2) %0, i64 0, i32 1\n'
              '  %dv = load i32, ptr addrspace(2) %q, align 4\n'
              '  %op = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %oi\n'
              '  store i32 %dv, ptr addrspace(1) %op, align 4\n')
    out.append(("a number read out of the set instead of a descriptor",
                build("r8", ["devrw", "devro"], body),
                "is a resource and has no bytes to read"))

    # the pointer the shader loads disagreeing with the struct type's space
    body = (chain("out", 0, 1) + chain("db", 1, 2)   # declared addrspace(1)
            + '  %oi = zext i32 %1 to i64\n'
              '  %dp = getelementptr inbounds i32, ptr addrspace(2) %db, i64 %oi\n'
              '  %dv = load i32, ptr addrspace(2) %dp, align 4\n'
              '  %op = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %oi\n'
              '  store i32 %dv, ptr addrspace(1) %op, align 4\n')
    out.append(("a pointer loaded in the wrong address space",
                build("r9", ["devrw", "devro"], body),
                "which the module's struct type puts in address space"))
    return out


# -- running them ------------------------------------------------------------

def assemble(d, stem, source):
    ll, bc = os.path.join(d, stem + ".ll"), os.path.join(d, stem + ".bc")
    with open(ll, "w") as f:
        f.write(source)
    q = subprocess.run(["llvm-as", "-o", bc, ll], capture_output=True)
    if q.returncode:
        return None, "llvm-as: " + q.stderr.decode().strip().splitlines()[0]
    return bc, None


def interface_of(bc):
    q = subprocess.run([AIR2NIR, "--interface", bc], capture_output=True, text=True)
    if q.returncode:
        return None, (q.stderr or q.stdout).strip().splitlines()[-1]
    got = []
    for line in q.stdout.splitlines():
        if not line.startswith("resource\t"):
            continue
        kv = dict(p.split("=", 1) for p in line.split("\t") if "=" in p)
        got.append((int(kv["kind"]), int(kv["index"]), int(kv["set"]), int(kv["id"])))
    return got, None


def nir_tex_of(bc):
    """The (opcode, texture, sampler) of every texture instruction in the NIR.

    Where the member table's answer for a texture is VISIBLE: softpipe's
    compute path cannot tell one texture slot from another (see control()), so
    the only place the resolved index can be read is the instruction the
    frontend emitted. `32x4 %20 = (float32)tex %8 (coord), 0 (texture), 0
    (sampler)` is the printer's form.
    """
    q = subprocess.run([AIR2NIR, bc], capture_output=True, text=True)
    out = []
    for m in re.finditer(r"\(float32\)(\w+) .*?(\d+) \(texture\)"
                         r"(?:, (\d+) \(sampler\))?", q.stdout):
        out.append((m.group(1), int(m.group(2)),
                    int(m.group(3)) if m.group(3) else None))
    return out


def patch(index, words):
    return "%d@0=%s" % (index, b"".join(struct.pack("<I", w) for w in words).hex())


def readback(out):
    """{buffer index: [words]} from air_run's print of each device buffer."""
    bufs, cur = {}, None
    for line in out.splitlines():
        m = re.match(r"air_run: device buffer (\d+) after the dispatch", line)
        if m:
            cur = int(m.group(1))
            bufs[cur] = []
            continue
        if cur is not None:
            m = re.match(r"\s*\[\s*(\d+)\]\s+\d+\s+0x([0-9a-f]+)", line)
            if m:
                bufs[cur].append(int(m.group(2), 16))
            elif line.startswith("air_run:"):
                cur = None
    return bufs


def dispatch(bc, resources):
    """air_run with a different fill in every buffer slot."""
    args = [AIR_RUN]
    for kind, index, _, _ in resources:
        if kind not in (0, 1):
            continue
        if index == 0:
            # every kernel here writes its result through member 0, which the
            # allocation therefore puts at buffer 0; a sentinel there makes an
            # unwritten slot a wrong answer rather than a plausible one
            args += ["--buffer", patch(index, [SENTINEL] * THREADS)]
        else:
            args += ["--buffer",
                     patch(index, [slot_base(index) + i for i in range(THREADS)])]
    args.append(bc)
    q = subprocess.run(args, capture_output=True, text=True)
    out = q.stdout + q.stderr
    screen = "an unnamed screen"
    m = re.search(r"air_run: \S+ on (.+?), as a compute kernel", out)
    if m:
        screen = m.group(1)
    return readback(out), screen, out


HOST_HARNESS = '''
@outbuf = global [64 x i32] zeroinitializer
@cbuf = global [64 x i32] zeroinitializer
@dbuf = global [64 x i32] zeroinitializer
@theset = global %struct.spvDescriptorSetBuffer0 { ptr @outbuf, ptr @cbuf, ptr @dbuf }
@fmt = private unnamed_addr constant [10 x i8] c"out %08x\\0A\\00"
declare i32 @printf(ptr, ...)
define i32 @main() {
entry:
  br label %fill
fill:
  %i = phi i32 [ 0, %entry ], [ %i1, %fill ]
  %ix = zext i32 %i to i64
  %hcp = getelementptr [64 x i32], ptr @cbuf, i64 0, i64 %ix
  %hdp = getelementptr [64 x i32], ptr @dbuf, i64 0, i64 %ix
  %hcv = add i32 %i, CBBASE
  %hdv = add i32 %i, DBBASE
  store i32 %hcv, ptr %hcp
  store i32 %hdv, ptr %hdp
  %i1 = add i32 %i, 1
  %fd = icmp eq i32 %i1, 16
  br i1 %fd, label %go, label %fill
go:
  %j = phi i32 [ 0, %fill ], [ %j1, %go ]
  call void @KERNEL(ptr @theset, i32 %j)
  %j1 = add i32 %j, 1
  %gd = icmp eq i32 %j1, 16
  br i1 %gd, label %show, label %go
show:
  %k = phi i32 [ 0, %go ], [ %k1, %show ]
  %kx = zext i32 %k to i64
  %hop = getelementptr [64 x i32], ptr @outbuf, i64 0, i64 %kx
  %hov = load i32, ptr %hop
  %pc = call i32 (ptr, ...) @printf(ptr @fmt, i32 %hov)
  %k1 = add i32 %k, 1
  %sd = icmp eq i32 %k1, 16
  br i1 %sd, label %fin, label %show
fin:
  ret i32 0
}
'''


def under_llvm(d, stem, source, kernel):
    """The same module, compiled and run by LLVM on this processor.

    The transformation is three textual steps and no more, so that what runs
    here is the module and not a rewrite of it: the air64 triple and datalayout
    go (this is x86-64), the address spaces are flattened (a host has one), and
    a harness is appended that builds the set out of real pointers. LLVM then
    decides what the shader means, which is the whole point -- it shares no
    code with our reader, our translator or the driver.
    """
    src = re.sub(r"^target (datalayout|triple).*\n", "", source, flags=re.M)
    src = re.sub(r" addrspace\(\d+\)", "", src)
    src += (HOST_HARNESS.replace("KERNEL", kernel)
            .replace("CBBASE", str(slot_base(1))).replace("DBBASE", str(slot_base(2))))
    ll = os.path.join(d, stem + "-host.ll")
    exe = os.path.join(d, stem + "-host")
    with open(ll, "w") as f:
        f.write(src)
    try:
        q = subprocess.run(["clang", "-O0", "-w", "-x", "ir", ll, "-o", exe],
                           capture_output=True, text=True)
    except FileNotFoundError:
        # a reference this machine cannot give is not a result this frontend
        # got wrong, and the difference has to stay visible in the output
        return "no clang", None
    if q.returncode:
        return None, "clang: " + (q.stderr or q.stdout).strip().splitlines()[-1][:140]
    q = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    if q.returncode:
        return None, "the program exited %d" % q.returncode
    words = [int(m, 16) for m in re.findall(r"^out ([0-9a-f]+)$", q.stdout, re.M)]
    if len(words) != THREADS:
        return None, "printed %d words, wanted %d" % (len(words), THREADS)
    return words, None


# -- the census: the metadata, read again -----------------------------------

def md_nodes(text):
    """{id: [(kind, value)]} for every `!N = !{...}` line, and the named nodes.

    A parse of `air2nir --ll` and not of the frontend's conclusions: the point
    of the census is to be a second reading of Apple's bytes, so it must not go
    through the code it is checking.
    """
    nodes, named, types = {}, {}, {}
    for line in text.splitlines():
        m = re.match(r"^!(\d+) = (?:distinct )?!\{(.*)\}$", line)
        if m:
            nodes[int(m.group(1))] = [classify(t) for t in split_ops(m.group(2))]
            continue
        m = re.match(r"^!([\w.]+) = !\{(.*)\}$", line)
        if m:
            named[m.group(1)] = [int(x[1:]) for x in split_ops(m.group(2))
                                 if x.startswith("!")]
            continue
        m = re.match(r"^(%[\w.:<>\"]+) = type \{(.*)\}$", line)
        if m:
            types[m.group(1).strip('"')] = [t.strip() for t in split_ops(m.group(2))]
    return nodes, named, types


def split_ops(s):
    out, depth, cur, q = [], 0, [], False
    i = 0
    while i < len(s):
        ch = s[i]
        if q:
            cur.append(ch)
            if ch == "\\":
                cur.append(s[i + 1])
                i += 2
                continue
            if ch == '"':
                q = False
            i += 1
            continue
        if ch == '"':
            q = True
        elif ch in "{[(":
            depth += 1
        elif ch in "}])":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
            i += 1
            continue
        cur.append(ch)
        i += 1
    if "".join(cur).strip():
        out.append("".join(cur).strip())
    return out


def classify(tok):
    m = re.match(r"^i\d+ (-?\d+)$", tok)
    if m:
        return ("int", int(m.group(1)))
    if tok.startswith('!"'):
        return ("str", tok[2:-1])
    m = re.match(r"^!(\d+)$", tok)
    if m:
        return ("ref", int(m.group(1)))
    if tok == "null":
        return ("int", 0)
    return ("other", tok)


def key_at(ops, key):
    for i, (k, v) in enumerate(ops):
        if k == "str" and v == key:
            return i
    return -1


def census_module(ll, counts):
    """One module's argument buffers, counted."""
    if "air.indirect_buffer" not in ll:
        return
    nodes, named, types = md_nodes(ll)
    argnodes = []
    for st in ("air.kernel", "air.vertex", "air.fragment", "air.visible"):
        for fid in named.get(st, []):
            for k, v in nodes.get(fid, []):
                if k != "ref":
                    continue
                for k2, v2 in nodes.get(v, []):
                    if k2 == "ref":
                        argnodes.append(v2)
    sets = [nodes[a] for a in argnodes
            if a in nodes and key_at(nodes[a], "air.indirect_buffer") >= 0]
    if not sets:
        return
    counts["modules with an argument buffer"] += 1
    counts["argument buffers"] += len(sets)
    counts["modules with %d of them" % len(sets)] += 1
    for ops in sets:
        i = key_at(ops, "air.struct_type_info")
        if i < 0 or ops[i + 1][0] != "ref":
            counts["sets with no air.struct_type_info"] += 1
            continue
        i2 = key_at(ops, "air.arg_type_name")
        tn = "%struct." + ops[i2 + 1][1] if i2 >= 0 and ops[i2 + 1][0] == "str" else None
        llvm = types.get(tn)
        counts["sets whose struct type the module declares" if llvm is not None
               else "sets whose struct type is NOT in the module"] += 1
        members, bad = members_of(nodes, ops[i + 1][1])
        if bad:
            counts["sets whose member list this cannot take apart"] += 1
            continue
        if llvm is not None:
            counts["sets whose struct type has as many members"
                   if len(llvm) == len(members)
                   else "sets whose struct type has a DIFFERENT member count"] += 1
        for n, m in enumerate(members):
            counts["members"] += 1
            counts["members at 8 times their number" if m["offset"] == 8 * n
                   else "members NOT at 8 times their number"] += 1
            desc = nodes.get(m["node"], [])
            kinds = [v for k, v in desc if k == "str" and v in
                     ("air.buffer", "air.texture", "air.sampler", "air.indirect_buffer",
                      "air.command_buffer", "air.render_pipeline_state")]
            counts["members of kind %s" % (kinds[0] if kinds else "?")] += 1
            if key_at(desc, "air.address_space") >= 0:
                counts["members carrying air.address_space"] += 1
            li = key_at(desc, "air.location_index")
            if li < 0:
                counts["members with no air.location_index"] += 1
                continue
            idx = desc[li + 1][1] if desc[li + 1][0] == "int" else None
            span = (desc[li + 2][1] if li + 2 < len(desc) and desc[li + 2][0] == "int"
                    else None)
            counts["members whose location_index is their number" if idx == n
                   else "members whose location_index is NOT their number"] += 1
            counts["members spanning one index" if span == 1
                   else "members spanning %s indices" % span] += 1
            if llvm is not None and n < len(llvm):
                counts["llvm member type %s" % llvm[n]] += 1


def members_of(nodes, nid):
    """The member list, or (partial, why not).

    Five fixed fields -- offset, size, a flag, the type name, the member name --
    and then `air.indirect_argument` and a node. A key BEFORE the five fields
    is how Apple writes a member whose own type is a struct, which in an
    argument buffer is inline constant data; it is reported rather than
    skipped, because a skipped member is a wrong index for every member after
    it.
    """
    ops = nodes.get(nid, [])
    out, i = [], 0
    while i < len(ops):
        five = ops[i:i + 5]
        if ([k for k, _ in five] != ["int", "int", "int", "str", "str"]):
            return out, "member %d is not five fields (%r)" % (len(out), ops[i][:2])
        m = {"offset": five[0][1], "size": five[1][1], "flag": five[2][1],
             "type_name": five[3][1], "name": five[4][1], "node": None}
        if (i + 6 < len(ops) and ops[i + 5] == ("str", "air.indirect_argument")
                and ops[i + 6][0] == "ref"):
            m["node"] = ops[i + 6][1]
            i += 7
        else:
            return out, "member %d has no air.indirect_argument" % len(out)
        out.append(m)
    return out, None


def census(paths):
    # Seeded with the rows whose VALUE IS THE POINT when it is zero. A counter
    # that is merely absent says nothing -- "no member carries
    # air.address_space" and "no module was printed with a key missing" are the
    # two measurements the frontend's reading of the address space rests on, so
    # they are printed as zeros rather than left out.
    counts = collections.Counter({
        "members carrying air.address_space": 0,
        "members with no air.location_index": 0,
        "sets whose struct type is NOT in the module": 0,
        "sets whose struct type has a DIFFERENT member count": 0,
        "sets whose member list this cannot take apart": 0,
        "sets with no air.struct_type_info": 0,
        "modules the reader could not print": 0,
    })
    for path in paths:
        q = subprocess.run([AIR2NIR, "--ll", path], capture_output=True, text=True)
        if q.returncode:
            counts["modules the reader could not print"] += 1
            continue
        counts["modules"] += 1
        census_module(q.stdout, counts)
    width = max((len(k) for k in counts), default=10)
    for k in sorted(counts, key=lambda k: (-counts[k], k)):
        print("  %*s  %d" % (width, k, counts[k]))
    return 0


# -- the whole run -----------------------------------------------------------

def run(d):
    bad = []
    screens = set()

    slots, why = control(d)
    if slots is None:
        bad.append("the two-direct-texture control: %s" % why)
        distinguishes = False
    else:
        distinguishes = slots[0] != slots[1]
        print("  control: two DIRECT textures, texel (1,1) of each: %d and %d -- "
              "this harness %s\n           one compute texture slot from another, so a "
              "texture member is checked on the\n           NIR instruction and not on "
              "the pixel"
              % (slots[0], slots[1],
                 "tells" if distinguishes else "CANNOT tell"))
    print()

    for label, members, source, want, resources, run_it, want_tex in cases():
        kernel = re.search(r"@(\w+)\(ptr addrspace\(2\)", source).group(1)
        bc, why = assemble(d, kernel, source)
        if not bc:
            bad.append("%s: %s" % (label, why))
            continue

        got, why = interface_of(bc)
        if got is None:
            bad.append("%s: air2nir refused it: %s" % (label, why))
            continue
        if got != resources:
            bad.append("%s: the interface is %s, wanted %s"
                       % (label, pretty(got), pretty(resources)))
            continue
        print("  %-52s %s" % (label, ", ".join(
            "%s %d = (set %d, id %d)" % (KIND[k], i, s, m) for k, i, s, m in got)))

        if want_tex:
            tex = nir_tex_of(bc)
            if tex != want_tex:
                bad.append("%s: the NIR's texture instructions are %r, wanted %r"
                           % (label, tex, want_tex))
                continue
            print("      the NIR asks for %s" % ", ".join(
                "%s texture %d%s" % (op, t, "" if s is None else " sampler %d" % s)
                for op, t, s in tex))

        bufs, screen, out = dispatch(bc, resources)
        screens.add(screen)
        if 0 not in bufs:
            tail = "\n      ".join(out.strip().splitlines()[-4:])
            bad.append("%s: air_run read no buffer back:\n      %s" % (label, tail))
            continue
        if bufs[0][:THREADS] != want:
            bad.append("%s: softpipe wrote %s, wanted %s"
                       % (label, hexes(bufs[0][:THREADS]), hexes(want)))
        else:
            print("      dispatched on %s: %s" % (screen, hexes(bufs[0][:4]) + " ..."))

        if not run_it:
            print("      not run under LLVM: %s" % why_not(label))
            continue
        host, why = under_llvm(d, kernel, source, kernel)
        if host == "no clang":
            print("      not run under LLVM: there is no clang on this machine, so the "
                  "second reference is missing and NOT satisfied")
        elif host is None:
            bad.append("%s: under LLVM: %s" % (label, why))
        elif host != want:
            bad.append("%s: LLVM computed %s, softpipe %s"
                       % (label, hexes(host), hexes(want)))
        else:
            print("      LLVM on this processor agrees, all %d words" % THREADS)

    print()
    for label, source, want in refusals():
        kernel = re.search(r"@(\w+)\(ptr addrspace\(2\)", source).group(1)
        bc, why = assemble(d, kernel, source)
        if not bc:
            bad.append("refusal %s: %s" % (label, why))
            continue
        q = subprocess.run([AIR2NIR, "--interface", bc], capture_output=True, text=True)
        msg = (q.stderr or "").strip().splitlines()
        msg = msg[-1] if msg else ""
        if q.returncode == 0:
            bad.append("refusal %s: TRANSLATED, and it must not" % label)
        elif want not in msg:
            bad.append("refusal %s: refused as %r, wanted %r in it" % (label, msg, want))
        else:
            # air2nir prefixes its own name and the module's path; the message
            # is what has to be read, so the prefix goes
            print("  refused: %-46s %s"
                  % (label, re.sub(r"^air2nir: \S*: ", "", msg)[:78]))
    return bad


def why_not(label):
    if "sampler" in label:
        return ("it samples, and LLVM has no texture unit -- what softpipe "
                "computed is the only execution of it")
    if "two sets" in label:
        return ("the harness builds one set of three pointers; what this case is "
                "for is the indices, which the interface above states")
    return ("the harness builds a set of three pointers and this one has two; "
            "the buffers case is the one both sides run")


def pretty(rows):
    return "[" + ", ".join("%s %d (set %d, id %d)" % (KIND.get(k, k), i, s, m)
                           for k, i, s, m in rows) + "]"


def hexes(words):
    return " ".join("%08x" % w for w in words)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", metavar="DIR", help="leave the generated modules here")
    ap.add_argument("--census", action="store_true",
                    help="count the argument-buffer metadata of a corpus")
    ap.add_argument("--list", metavar="FILE", help="read the corpus paths from FILE")
    ap.add_argument("paths", nargs="*")
    args = ap.parse_args()

    for prog, what in ((AIR2NIR, "air2nir"), (AIR_RUN, "air_run")):
        if not os.path.exists(prog) and not args.census:
            print("argbufcheck.py: no %s: run tools/air/mk-host.sh first" % what)
            return 2

    if args.census:
        paths = args.paths
        if args.list:
            paths = [l.strip() for l in open(args.list) if l.strip()]
        if not paths:
            print("argbufcheck.py: --census wants modules to count")
            return 2
        return census(paths)

    if args.keep:
        os.makedirs(args.keep, exist_ok=True)
        bad = run(args.keep)
    else:
        with tempfile.TemporaryDirectory() as d:
            bad = run(d)

    print()
    if bad:
        for b in bad:
            print("FAIL  " + b)
        print("argbufcheck.py: %d of them" % len(bad))
        return 1
    print("argbufcheck.py: the member table, the indices it allocates and nine of the 23 "
          "refusals, with\n              every slot holding different bytes and LLVM "
          "agreeing on the one case it can run.\n              Nothing here touches the "
          "runtime's half: there is no argument buffer to write.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
