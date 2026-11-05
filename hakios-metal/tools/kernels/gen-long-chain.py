#!/usr/bin/env python3
"""Writes tools/kernels/long_chain.ll: a compute kernel longer than the 127 instruction units the A6xx preloads.

    python3 -I tools/kernels/gen-long-chain.py > tools/kernels/long_chain.ll
    python3 -I tools/kernels/gen-long-chain.py STEPS NAME > tools/kernels/NAME.ll   (the shorter ones that bracket 127)
    python3 -I tools/kernels/gen-long-chain.py 700 chain_fs fs > tools/kernels/chain_fs.ll   (the same as a fragment function)

The fragment form starts from the pixel's y * 64 + x and the constant 0x1234 in place of the buffer, and returns the
chain's low three bytes as red, green and blue over 255, which an RGBA8Unorm target stores as the bytes themselves.

Each of STEPS steps mixes the running value with xor, a multiply and a rotate built from two shifts; the value starts
from the invocation's id and a constant read from a buffer, so nothing can be folded at compile time and every step
stays an instruction sequence of its own. long_chain_reference() in tools/probe.cpp computes the same chain on the
processor. Hand-written generator, MIT; the metadata's shape is the AIR convention fill_id.ll describes.
"""
import sys

STEPS = int(sys.argv[1]) if len(sys.argv) > 1 else 700
NAME = sys.argv[2] if len(sys.argv) > 2 else "long_chain"
FRAGMENT = len(sys.argv) > 3 and sys.argv[3] == "fs"


def k(i):
    # per-step constants from a fixed odd-multiplier sequence: odd multipliers keep each step a bijection
    x = (0x9E3779B1 * (i + 1)) & 0xFFFFFFFF
    return x ^ 0x5BD1E995, (x | 1) & 0xFFFFFFFF, 5 + i % 23


def s32(v):
    return v - (1 << 32) if v >= 1 << 31 else v


lines = []
lines.append("; %s: %d dependent xor/multiply/rotate steps on (gid, c), written out by gen-long-chain.py; do not edit." % (NAME, STEPS))
lines.append(";")
lines.append(";     kernel void %s(device uint *out [[buffer(0)]], constant uint &c [[buffer(1)]]," % NAME)
lines.append(";                            uint gid [[thread_position_in_grid]])")
lines.append(";     { uint v = gid ^ c; for each step i: v = rotl((v ^ X[i]) * M[i], R[i]); out[gid] = v; }")
lines.append(";")
lines.append("; Its point is its length: ir3 makes more than 127 instruction units of it, past what CP_LOAD_STATE6 preloads,")
lines.append("; so the GPU fetches the rest from SP_CS_OBJ_START while it runs. Hand-written generator, ours, MIT.")
lines.append("")
lines.append('source_filename = "%s"' % NAME)
lines.append('target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"')
lines.append('target triple = "air64-apple-ios16.5.0"')
lines.append("")
if not FRAGMENT:
    lines.append("define void @%s(" % NAME + "ptr addrspace(1) %out, ptr addrspace(2) %c, i32 %gid) local_unnamed_addr {")
    lines.append("  %cv = load i32, ptr addrspace(2) %c, align 4")
    lines.append("  %v0 = xor i32 %gid, %cv")
else:
    lines.append("define <4 x float> @%s(<4 x float> %%pos) local_unnamed_addr {" % NAME)
    lines.append("  %px = extractelement <4 x float> %pos, i32 0")
    lines.append("  %py = extractelement <4 x float> %pos, i32 1")
    lines.append("  %x = fptoui float %px to i32")
    lines.append("  %y = fptoui float %py to i32")
    lines.append("  %ys = shl i32 %y, 6")
    lines.append("  %gid = add i32 %ys, %x")
    lines.append("  %v0 = xor i32 %gid, 4660")
for i in range(STEPS):
    x, m, r = k(i)
    lines.append("  %%a%d = xor i32 %%v%d, %d" % (i, i, s32(x)))
    lines.append("  %%b%d = mul i32 %%a%d, %d" % (i, i, s32(m)))
    lines.append("  %%l%d = shl i32 %%b%d, %d" % (i, i, r))
    lines.append("  %%h%d = lshr i32 %%b%d, %d" % (i, i, 32 - r))
    lines.append("  %%v%d = or i32 %%l%d, %%h%d" % (i + 1, i, i))
if not FRAGMENT:
    lines.append("  %t = zext i32 %gid to i64")
    lines.append("  %po = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %t")
    lines.append("  store i32 %%v%d, ptr addrspace(1) %%po, align 4" % STEPS)
    lines.append("  ret void")
else:
    v = "%%v%d" % STEPS
    lines.append("  %%out_r = and i32 %s, 255" % v)
    lines.append("  %%out_g0 = lshr i32 %s, 8" % v)
    lines.append("  %out_g = and i32 %out_g0, 255")
    lines.append("  %%out_b0 = lshr i32 %s, 16" % v)
    lines.append("  %out_b = and i32 %out_b0, 255")
    for ch in "rgb":
        lines.append("  %%out_%sf = uitofp i32 %%out_%s to float" % (ch, ch))
        lines.append("  %%out_%sn = fdiv float %%out_%sf, 2.550000e+02" % (ch, ch))
    lines.append("  %c0 = insertelement <4 x float> <float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float %out_rn, i32 0")
    lines.append("  %c1 = insertelement <4 x float> %c0, float %out_gn, i32 1")
    lines.append("  %c2 = insertelement <4 x float> %c1, float %out_bn, i32 2")
    lines.append("  ret <4 x float> %c2")
lines.append("}")
lines.append("")
lines.append("!air.version = !{!0}")
lines.append("!air.language_version = !{!1}")
if not FRAGMENT:
    lines.append("!air.kernel = !{!2}")
    lines.append("")
    lines.append("!0 = !{i32 2, i32 5, i32 0}")
    lines.append('!1 = !{!"Metal", i32 3, i32 0, i32 0}')
    lines.append("!2 = !{ptr @%s, !3, !4}" % NAME)
    lines.append("!3 = !{}")
    lines.append("!4 = !{!5, !6, !7}")
    lines.append('!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}')
    lines.append('!6 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"c"}')
    lines.append('!7 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}')
else:
    lines.append("!air.fragment = !{!2}")
    lines.append("")
    lines.append("!0 = !{i32 2, i32 5, i32 0}")
    lines.append('!1 = !{!"Metal", i32 3, i32 0, i32 0}')
    lines.append("!2 = !{ptr @%s, !3, !5}" % NAME)
    lines.append("!3 = !{!4}")
    lines.append('!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}')
    lines.append("!5 = !{!6}")
    lines.append('!6 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}')
print("\n".join(lines))
