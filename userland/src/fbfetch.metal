// fbfetch_test's shaders (userland/src/fbfetch_test.m): a full-screen triangle and three fragment
// functions that read the target's colour inside the shader ([[color(0)]], Metal's programmable
// blending -- how CoreAnimation's "Xm" pipelines blend, the rounded corners among them).
#include <metal_stdlib>
using namespace metal;

struct v2f { float4 position [[position]]; };

vertex v2f fbf_vertex(uint id [[vertex_id]])
{
	float2 p = float2((id << 1) & 2, id & 2);
	v2f out;
	out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
	return out;
}

// half of what is there, plus green: over red (1, 0, 0, 1) that is (0.5, 0.5, 0, 1)
fragment float4 fbf_half(v2f in [[stage_in]], float4 dst [[color(0)]])
{
	return float4(dst.rgb * 0.5 + float3(0.0, 0.5, 0.0), 1.0);
}

// source-over of a transparent source, done in the shader as CoreAnimation does: what is there, unchanged
fragment float4 fbf_over_clear(v2f in [[stage_in]], float4 dst [[color(0)]])
{
	float4 src = float4(0.0);
	return src + dst * (1.0 - src.a);
}

// the target's colour itself
fragment float4 fbf_copy(v2f in [[stage_in]], half4 dst [[color(0)]])
{
	return float4(dst);
}
