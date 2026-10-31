; A FRAGMENT shader whose every resource arrives through an argument buffer, written
; for the runtime half of section 8.2: MTLArgumentEncoder as a recorder.
;
; Why it is hand-written, and why it is a fragment shader and not a kernel:
;
;   - 0 of the 456 modules of iOS 16.5's corpus declare an `air.indirect_buffer` and
;     7 620 of Resident Evil 4's 7 879 do (docs/air.md 8.2), and no byte of that game is
;     in this repository. So a module that declares a set has to be written here, and
;     what it declares is then known rather than inferred.
;   - tools/air/argbufcheck.py already dispatches one as a KERNEL, and two things cannot
;     be checked that way: softpipe's compute path answers every texture out of the
;     sampler view bound at slot 0, so a texture member's index is invisible there, and
;     air_run binds one sampler state to every slot, so a sampler member's index is
;     invisible as well. A fragment shader distinguishes both (argbufcheck.py's
;     control() measured the first), so this one is drawn, and the picture names which
;     member answered: see air_metal_test.c --draw-argbuf.
;
; The shader, in Metal, is
;
;     struct Set {
;         constant Params *params       [[id(0)]];   // one float
;         sampler          nearest      [[id(1)]];
;         texture2d<float> texA         [[id(2)]];
;         texture2d<float> texB         [[id(3)]];
;         sampler          linear       [[id(4)]];
;     };
;     fragment float4 argbuf_set(constant Set &set [[buffer(0)]]) {
;         return float4(set.texA.sample(set.nearest, float2(0.3125, 0.375)).x,
;                       set.texB.sample(set.linear,  float2(0.3125, 0.375)).x,
;                       set.params->v,
;                       set.texA.read(uint2(2, 3)).x);
;     }
;
; so every channel is one member and one picture answers four questions:
;
;   R  texture member 2, sampled through sampler member 1
;   G  texture member 3, sampled through sampler member 4
;   B  buffer member 0, at whatever offset it was encoded at
;   A  texture member 2 again, FETCHED at another texel and with no sampler
;
; The pairing is deliberate, and it is the one shape BOTH back ends can express rather
; than the one that reads best. On the Adreno a texture index and a sampler index are two
; immediates of one instruction (`sam` with S2EN: ir3_compiler_nir.c collects
; tex->texture_index and tex->sampler_index separately), so any pairing is honoured. On
; softpipe there is only a GL texture UNIT: nir_to_tgsi.c emits
; `ureg_DECL_sampler(c->ureg, instr->sampler_index)` and never looks at
; `texture_index` at all, with the comment "It seems we can't get separate tex/sampler on
; GL". That was measured here before it was read: a first version of this shader sampled
; texA through both samplers, the frontend emitted `0 (texture), 1 (sampler)` for the
; second one -- which is right -- and softpipe answered with the texels of the texture
; bound at slot 1. So each sample here uses the sampler whose index equals its texture's,
; and the fetch uses texture 0, which is unit 0 either way.
;
; What that buys: R and G are two different textures through two different samplers, so
; exchanging the two SAMPLER records changes R and G each by its filter and leaves A (a
; fetch) alone, while exchanging the two TEXTURE records moves 1000 through R, G and A.
; The two wrong pictures are therefore different from each other and from the right one,
; which is what a test needs in order to say which member answered. A test where every
; slot holds the same thing passes for a driver that bound the wrong one.
;
; The three `getelementptr` shapes SPIRV-Cross emits are all here on purpose (the
; two-index buffer, the three-index sampler, the wrapper-then-handle texture), because
; the frontend resolves them through one lookup on the byte offset and this is the
; module that says so from the API side.
;
;   llvm-as -o argbuf_set.bc tools/air/argbuf_set.ll
;   tools/air/argbufruncheck.sh

source_filename = "argbuf_set"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios14.0.0"

%"struct.metal::sampler" = type { ptr addrspace(2) }
%"struct.metal::texture2d" = type { ptr addrspace(1) }
%struct.spvDescriptorSetBuffer0 = type { ptr addrspace(2), %"struct.metal::sampler", %"struct.metal::texture2d", %"struct.metal::texture2d", %"struct.metal::sampler" }

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32) local_unnamed_addr
declare { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, i32, i32) local_unnamed_addr

define <{ <4 x float> }> @argbuf_set(ptr addrspace(2) %0) local_unnamed_addr {
  ; member 0: a constant buffer pointer, the two-index shape, then one float out of it
  %cbp = getelementptr inbounds %struct.spvDescriptorSetBuffer0, ptr addrspace(2) %0, i64 0, i32 0
  %cb = load ptr addrspace(2), ptr addrspace(2) %cbp, align 8
  %cv = load float, ptr addrspace(2) %cb, align 4

  ; member 1: a sampler, the three-index shape (the wrapper's field in one getelementptr)
  %s1p = getelementptr inbounds %struct.spvDescriptorSetBuffer0, ptr addrspace(2) %0, i64 0, i32 1, i32 0
  %s1 = load ptr addrspace(2), ptr addrspace(2) %s1p, align 8

  ; member 2: a texture, the wrapper-then-handle shape
  %tap = getelementptr inbounds %struct.spvDescriptorSetBuffer0, ptr addrspace(2) %0, i64 0, i32 2
  %taq = getelementptr inbounds %"struct.metal::texture2d", ptr addrspace(2) %tap, i64 0, i32 0
  %ta = load ptr addrspace(1), ptr addrspace(2) %taq, align 8

  ; member 3: the other texture, the same shape
  %tbp = getelementptr inbounds %struct.spvDescriptorSetBuffer0, ptr addrspace(2) %0, i64 0, i32 3
  %tbq = getelementptr inbounds %"struct.metal::texture2d", ptr addrspace(2) %tbp, i64 0, i32 0
  %tb = load ptr addrspace(1), ptr addrspace(2) %tbq, align 8

  ; member 4: the second sampler
  %s4p = getelementptr inbounds %struct.spvDescriptorSetBuffer0, ptr addrspace(2) %0, i64 0, i32 4, i32 0
  %s4 = load ptr addrspace(2), ptr addrspace(2) %s4p, align 8

  %r1 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %ta, ptr addrspace(2) %s1, <2 x float> <float 3.125000e-01, float 3.750000e-01>, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v1 = extractvalue { <4 x float>, i8 } %r1, 0
  %c1 = extractelement <4 x float> %v1, i64 0

  %r2 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %tb, ptr addrspace(2) %s4, <2 x float> <float 3.125000e-01, float 3.750000e-01>, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v2 = extractvalue { <4 x float>, i8 } %r2, 0
  %c2 = extractelement <4 x float> %v2, i64 0

  %r3 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %ta, <2 x i32> <i32 2, i32 3>, i32 0, i32 0)
  %v3 = extractvalue { <4 x float>, i8 } %r3, 0
  %c3 = extractelement <4 x float> %v3, i64 0

  %o0 = insertelement <4 x float> undef, float %c1, i64 0
  %o1 = insertelement <4 x float> %o0, float %c2, i64 1
  %o2 = insertelement <4 x float> %o1, float %cv, i64 2
  %o3 = insertelement <4 x float> %o2, float %c3, i64 3
  %out = insertvalue <{ <4 x float> }> undef, <4 x float> %o3, 0
  ret <{ <4 x float> }> %out
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 3, i32 0}
!1 = !{!"Metal", i32 2, i32 3, i32 0}
!2 = !{ptr @argbuf_set, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!5 = !{!6}
!6 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 40, !"air.location_index", i32 0, i32 1, !"air.read", !"air.struct_type_info", !7, !"air.arg_type_size", i32 40, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"spvDescriptorSetBuffer0", !"air.arg_name", !"spvDescriptorSet0"}
!7 = !{i32 0, i32 8, i32 0, !"type_Params", !"params", !"air.indirect_argument", !10, i32 8, i32 8, i32 0, !"sampler", !"nearestSampler", !"air.indirect_argument", !11, i32 16, i32 8, i32 0, !"texture2d<float, sample>", !"texA", !"air.indirect_argument", !12, i32 24, i32 8, i32 0, !"texture2d<float, sample>", !"texB", !"air.indirect_argument", !13, i32 32, i32 8, i32 0, !"sampler", !"linearSampler", !"air.indirect_argument", !14}
!10 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"type_Params", !"air.arg_name", !"params"}
!11 = !{i32 1, !"air.sampler", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"nearestSampler"}
!12 = !{i32 2, !"air.texture", !"air.location_index", i32 2, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"texA"}
!13 = !{i32 3, !"air.texture", !"air.location_index", i32 3, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"texB"}
!14 = !{i32 4, !"air.sampler", !"air.location_index", i32 4, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"linearSampler"}
