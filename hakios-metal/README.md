# hakios-metal

The Metal driver of [hakios](https://github.com/oosexgang/hakios), in C++: Metal's command model on the phone's
Adreno GPU (an Adreno 7c+ Gen 3, chip id 0x06030500), through the kernel's own GPU driver (HakiosGPU), without Mesa
in between.

What there is today, and what each part is checked by:

| Part | What it does | Checked by |
|---|---|---|
| `pm4.hpp` | the command processor's packets (type 4 and type 7, with their parity bits) | every header against Mesa's `pm4_pkt4_hdr` / `pm4_pkt7_hdr`, all 128 opcodes; opcodes against `adreno_pm4.xml` |
| `a6xx.hpp` | the registers, packet words and descriptors (buffer, 2D texture, 2D image, sampler) a compute dispatch writes, and every register, field, enum, opcode and whole-register encoder one sysmem draw writes, under Mesa's names, each with its `a6xx.xml.h` line | `tools/check-a6xx.py`: 1652 constants compared by name with the headers Mesa generates from its register XML; a changed value fails it; `a6xx_test` holds the encoders' words against literals worked out by hand (the RGBA8Unorm 64x64 descriptor, Metal's default sampler, the first triangle's register words with a twin each) |
| `gpu.hpp` | buffers (GEM objects), submissions, fences, through the msm DRM requests the kernel driver answers | the fake GPU; on the phone, `hakios-metal-probe` |
| `command.hpp` | `CommandQueue`, `CommandBuffer` (Metal's states, completed handlers in commit order, `waitUntilCompleted`), blits: copy and fill done by the GPU's command processor (CP_MEMCPY, CP_MEM_WRITE) | tests against the fake GPU, which runs the packets; under ThreadSanitizer and AddressSanitizer too; on the phone, the probe (28 October: a copy of 500 000 bytes exact, fills of every awkward length, 40 command buffers in order) |
| `compute.hpp` | the compute dispatch: a `Kernel` over a grid, as Mesa's computerator runs one on an A6XX, plus the driver params and pushed constant ranges an AIR-compiled kernel needs, and its textures and samplers: an image (a texture the kernel writes) as a 2D storage descriptor at IBO slot `numSsbos + index`, sampled textures and samplers in the two tables Gallium loads into `SB6_CS_TEX` (`SP_CS_TEX_CONST/SAMP/COUNT`), the TP's ISAM mode and border colour registers | the fake GPU, which checks that the dispatch holds together (below), and sixteen tests that mutate an accepted dispatch into one it must refuse; every refusal by its sentence; on the phone, two buffer kernels of our own writing every word as computed (28 October), and the texture path on 29 October: `tex_fill`, `tex_copy`, `tex_sample_gid`, `tex_fill_h` every pixel as computed (`logs/011104`, `012127`, `013214`) |
| `texture.hpp` | `Texture`: a 2D image of one level in GPU memory laid out as Mesa's `fdl6_layout` lays a linear image out (pitch = `align(width * cpp, 64 * cpp)`, `align(align(height, 4) * pitch, 4096)` bytes), with `replace`/`read` converting the caller's rows to the GPU's, and its two descriptors; made with `MTLTextureUsageRenderTarget` (4) it is also a draw's colour target, the same layout; `Sampler`: `MTLSamplerDescriptor` as the four `TEX_SAMP` words Gallium's `fd6_sampler_state_create` would write | `texture_test`: the pitch and size rule as a table, a round trip at a stride wider than a row, a sub-region with every other byte of the object canary-checked, the descriptor literals, the five `RB_MRT[0]` words of the 64x64 RGBA8 target (0x30, 4, 0x100, base, 0) off the same fields, the sampler words, every refusal by name; `compute_test`: the descriptors the fake reads back at `CP_EXEC_CS` carry the texture's format, size, pitch and address |
| `render.hpp` | the first draw: a `Program` of two `Stage`s (a vertex and a fragment function as plain data, `Kernel` plus what the 3D registers read of an ir3 variant: inputs, outputs, the sysval regids, the colour outputs) linked by `render::link` (a port of `ir3_link_shaders`), drawn by `CommandBuffer::draw` as ONE sysmem pass into ONE colour target: turnip's `tu6_sysmem_render_begin`/r3d register sequence with fd6 as the second witness, every register under a `render::Options` switch (for `--bisect`), Metal's orientation as a NEGATIVE `GRAS_CL_VPORT_YSCALE`, no buffer in either stage, no depth, no blending, no MSAA, no vertex descriptor; the tail puts the colour in memory with `PC_CCU_FLUSH_COLOR_TS` (4 words, no TIMESTAMP bit) and `CACHE_FLUSH_TS`, each waited for | `render_test`: every refusal on its sentence with nothing submitted (34 of them); the stream's words against the literals worked out by hand from the Mesa sources (`a6xx_test`'s and the critic's); each `Options` switch shown to remove or change exactly its registers; `tri_flat`, `tri_color` and a hand-built pair completing on the fake, `lastDraw` holding the texture's and the draw's numbers; and for every check of the fake a mutation twin it refuses (61 twins). **Not yet measured on the phone**: every value here is a reading of turnip/fd6, and `hakios-metal-probe` stage 9 is what measures it (below) |
| `kernel.hpp`, `tools/kernels/` | a kernel as the dispatch registers want it: machine code and every layout field; our own kernels, as ir3 text and as AIR (LLVM IR) | `tools/ir3-kernel` assembles the text with Mesa's ir3 assembler and reads the words back; `tools/mesa/ir3-dump.patch` gives Mesa's `ir3_compiler` the same output for the AIR; both regenerate the fixtures byte for byte |
| `compiler.hpp` | the compiler inside the driver: AIR to NIR by hakios's front end, NIR to ir3 by Mesa, for this chip, in this process; built in with `MESA=1`, a refusal by name without | `compiler_test`: fill_id.ll compiled here is `tests/fixtures/fill_id_air.hpp` field for field and word for word (a changed field fails it); on the phone, the same bitcode compiled on the device gave the same 64 words and ran (28 October) |
| `metallib.hpp` | `.metallib` files: thin and universal (the AIR64 slice), every offset checked | built files, good and broken; every truncation of a good one is refused |
| `hakios_metal.h` | the core as C, for the plug-in (`HakiosMetal.m`): device, buffers, textures and samplers in Metal's own numbers, libraries, kernels compiled in-process with what they bind (and, per texture resource, the dimension, element class and access the instructions make), queue, command buffers with copy, fill and dispatch (`dispatch2` with textures and samplers), one completion callback; and the render path: a `program` (a vertex and a fragment function compiled and linked for one colour format, `hakios_metal_program_new`/`_new_bitcode`, its per-stage resources), textures with usage bit 4, `hakios_metal_cb_draw` (one pass: `MTLRenderPassDescriptor`'s attachment 0, `MTLViewport`, `MTLScissorRect`, the primitive, vertex start and count; buffer tables of 0 only, refused by name otherwise) | `c_api_test` on the fake GPU, with the compiler (fill_id's resources by kind and index, a dispatch missing a buffer refused by name, one with both completing; tri_flat's program from its two bitcodes, its draws completing with and without a viewport and scissor, the program and the texture freed in flight, nine draw and program refusals on their sentences) and without (compiling and `program_new_bitcode` refuse by name); on the phone, through Metal's own objects, hakios's `metal_compute_test` (`logs/222820-run.log`) -- the draw not yet |
| `fake_gpu.hpp` | a GPU for the tests: the requests answered in the process, the packets run by an interpreter as strict as the command processor (bad parity, a count past the end, an unknown opcode, an address outside every buffer: a fault), a dispatch checked against the registers it wrote, its texture state included (the two `SB6_CS_TEX` loads against `SP_CS_TEX_*`, every 2D descriptor's format, pitch and rows against the buffers, every sampler's wrap modes and border table); a draw (`CP_DRAW_INDX_OFFSET`) checked the same way: the sysmem pass begun, both stages enabled and preloaded from their 128-aligned programs by their own opcode, the constants within the decoded constlen, `RB_MRT[0]` inside a buffer and agreeing with `SP_FS_MRT_REG`, the output path to it, no depth or stencil, the scissors inside the window, the viewport finite, the link's stride and count agreed, the VARYING and barycentric copies agreed, MSAA off, `RB_CCU_CNTL` after a WFI, and the two flushes after the last draw | itself, by the tests above; and it fails as the kernel fails: a hang ends the submission's wait, and either the GPU is reset and FAULTS counts it (boot-arg `gpu_recover=1`) or nothing is waited for again. It rasterizes nothing: coverage, the fill rule, interpolation, the y orientation and the bytes are the phone's |

What the fake GPU does not do: run a shader instruction, or judge a register's value beyond the checks named in
`fake_gpu.hpp`. A dispatch it accepts is well formed and names only memory the GPU was given; whether the GPU runs it
is the phone's to say. It said no once for a reason the fake could not see: CP_EVENT_WRITE's TIMESTAMP bit makes the
GPU write a timestamp where the end-of-dispatch wait expects a value, and every dispatch hung until the bit was
cleared (`--bisect` found it, one boot). The fake writes a timestamp for that bit now, so that stream fails here too.

Apple's shaders, so far: the probe reads a `.metallib` off the phone's own filesystem, compiles a function of it on the
device and runs it over canary-filled buffers (`--metallib FILE FUNCTION`), printing per buffer the words written, a
hash of the whole buffer and the first eight words. The reference is hakios's `tools/air/kernelref.py`: the same kernel
run on a Linux host through LLVM over the same buffers, printing the same line, so the two logs diff. On 28 October
(hakios `logs/215528-run.log`) eight buffer-only kernels of PortraitFilters compiled on the phone and completed, and all
eight came out identical to LLVM's execution, hash included (`MetalFaceMask_InitMinMax`, `MetalFaceMask_InitSpanTable`,
`image_calcminmax0`, `image_calcminmax2`, `fbs_bistochastize_init`, `fbs_pcg_iter3`, `MetalFaceMask_PropagateParents`,
`MetalFaceMask_CalcCubeInputScaling`). The last one first read as a GPU difference of one unit in the last place in three
floats, and the unit was the host's: under the fast-math flags Apple's IR carries, LLVM's x86 backend had divided a vector
by a reciprocal estimate and one Newton step, one ulp low, where the Adreno's reciprocal and multiply by 32 was correctly
rounded; the reference divides exactly now. Kernels with textures, samplers, threadgroup memory, barriers or atomics are
not run (refused by name on the phone, out of the reference's scope on the host; the two `InitMinMax`/`InitSpanTable`
atomic stores are compared as plain stores, which says something about the values and nothing about ordering).
Metal's own objects, so far: hakios's plug-in (`userland/src/HakiosMetal.m`: an `MTLDevice` and the objects it hands out,
each a subclass of Metal's private base class, over `hakios_metal.h`) is what a Metal program reaches, and hakios's
`userland/src/metal_compute_test.m` drives it through Metal.framework alone, with nothing of this repository linked, as
the GPU's only client. On 28 October (hakios `logs/222820-run.log`, 0 failures on both roads) Metal's own discovery,
with boot-arg `metal=1` and the two properties on the service, returned our class from `MTLCreateSystemDefaultDevice()`;
`MetalFaceMask_InitMinMax` from the image's library and `fill_id` from a `.metallib` built in memory ran through
`MTLComputeCommandEncoder`, 1024 of 1024 words, the constant from a buffer and from `-setBytes:length:atIndex:`, a blit
copy and a fill in the same command buffer, `-dispatchThreads:` over half a grid. The first run (`logs/222249-run.log`)
failed twice without the GPU being wrong: the driver refused a 4-byte binding against ir3's padded 64-byte constant range
(fixed: the range is measured against the object's pages), and the test's table had been written from a restatement of
the probe's line and expected four words where six were measured. Render encoders, textures, samplers, heaps, fences
and events refuse by name.
Textures and samplers in 2D compute are served (sampled textures, images written with `stib`, Metal's samplers with
clamp-to-zero through the border table), checked on the fake GPU, and measured on the phone on 29 October (hakios
`logs/011104-run.log`, `012127-run.log`) with `tools/kernels/tex_fill.ll`, `tex_copy.ll`, `tex_sample_gid.ll` and
`tex_fill_h.ll`: an image written through the IBO table lands every pixel of R32Uint and all four lanes of RGBA32Uint
(2048 of 2048, the row padding untouched, a half grid leaving the other half canary), a texture read through the texture
table beside it copies 2048 of 2048, a sampler of Metal's returns the texel exactly at the centres with nearest and with
linear filtering and the bilinear mean at quarter positions within 0.000123 (60 of 64 pixels differing from nearest, as
expected), and a half image receives 2048 of 2048 exact half bit patterns. That last one found a bug in hakios's front
end: it typed every texture write by the call's void return type, so half and float values were stored as integers
(`stib.b.typed.2d.u16`), and the driver, reading the kernel's class off that, refused RGBA16Float for Apple's
`MetalFaceMask_Clear`; the front end types the store by its value now, and a declaration that disagrees with the
instructions' class is refused at compile time, by the compiler here and by the fixture writer. Not yet: textures other
than 2D, one level, one slice (1D, 3D, cube, arrays, mipmaps, MSAA), border colours other than transparent black, compare
samplers, the front end's null texture in a dispatch (a draw serves it, below), textures over buffers (`bytesPerRow` of the caller's
choosing), private memory, threadgroup memory (each refused by name, not served). An indexed `constant` load ir3 did
not push is served, through a UBO descriptor, in a dispatch as in a draw.

The first draw (`render.hpp`, 29 October) is written, checked on the fake GPU, and measured on the phone the same day
(hakios `logs/051851-run.log`, first run, every line passing): one triangle, then the pair, then per-vertex colours,
into a 64x64 RGBA8Unorm texture in sysmem (direct render) mode, read back by the CPU and compared pixel by pixel against
the rule the probe computes (stage 9 below): 2016 of 2016 interior pixels the colour, 2016 of 2016 exterior pixels
canary, 0 of the 64 diagonal centres painted by the lower-left triangle and 64 of 64 by its complement (the top-left
rule, and the two together cover the target exactly once: 4096 of 4096), the half viewport right, `vertexStart` 3
drawing the second triangle alone (so `vertex_id` includes `VFD_INDEX_OFFSET`), the per-vertex colours within 1/255 at
every one of 4032 off-diagonal pixels (largest deviation 0), the four stages compiled on the phone word for word the
host's fixtures, and 41 to 68 us a pass. Through Metal's own objects (hakios `logs/051907-run.log`, `metal_render_test`,
0 failures on both roads) the same draws land with the same counts: `MTLRenderPipelineState` linked on the device,
`MTLRenderCommandEncoder`, `drawPrimitives:` -- and a texture without the render usage ends the command buffer in
error with the driver's sentence, the texture untouched. The shaders are
ours (`tools/kernels/tri_flat_*.ll`, `tri_color_*.ll`: the positions chosen by `[[vertex_id]]` with no buffer in either
stage, as turnip's own clear path draws), the register sequence is turnip's sysmem pass and r3d path with fd6 as the
second witness, every register cited at its line, and each group sits under a `render::Options` switch so that one boot
of `--bisect` can say which the hardware wants where the two witnesses or the kernel's own boot picture disagree
(`RB_CCU_CNTL` bit 2, the LRZ feedback bits of `BIN_CONTROL`, `RB_RENDER_CNTL`, `SP_FLOAT_CNTL`, `GRAS_UNKNOWN_8110`,
USE/IGNORE_VISIBILITY, `LRZ_FLUSH`, `RB_DONE_TS`). The sign of `GRAS_CL_VPORT_YSCALE` is negative by Metal's convention
(NDC y up, row 0 at the top): the probe's orientation line confirmed it (the lower-left half was drawn), and every
switch's default -- turnip's and fd6's values where they agree, the smaller set where they do not -- drew without a
bisect. Not yet, all of it refused by name (not served): vertex attributes beyond per-vertex Float..Float4, Half2, Half4 and UChar4Normalized (those are
served through the vertex descriptor: `tri_attr_vs.ll`'s Float2 position and UChar4Normalized colour from one interleaved buffer, 4032 of 4032
pixels exact on the phone, `logs/142841-run.log`), buffers in a vertex or fragment stage beyond a `constant T&` read at fixed offsets (that one is a pushed range and is served: `tri_cflat_fs.ll`'s colour from `[[buffer(0)]]`, two colours in two draws exact on the phone, `logs/142053-run.log`; an indexed one is served through a UBO descriptor, and a `device` one through the 3D IBO table, below), textures and
samplers in a vertex stage, images in a draw, stencil, depth formats other than Depth32Float, a
fragment depth output, dual-source blending, GMEM (tiled)
rendering, MSAA, a base instance, a negative base vertex, points and lines, several colour attachments, a program whose stages bind what
the draw does not serve.

A load action of Clear is served the way turnip clears a single-sampled sysmem attachment: one `CP_BLIT` solid fill of
the 2D engine over the whole target before any draw state (`clear_sysmem_attachment`, `r2d_*` in `tu_clear_blit.cc`),
the colour packed as `r2d_clear_value` packs it, then the colour CCU cleaned and invalidated. A pass with no draw is a
pass of its own (`CommandBuffer::clear`, `hakios_metal_cb_clear`), what an encoder that recorded nothing needs. On the
phone (hakios `logs/145325-run.log`, probe line 9i, 0 failures) every pixel holds the packed colour and no byte outside
the rectangle changes, in RGBA8Unorm, BGRA8Unorm (B first in memory), RGBA8Unorm_sRGB (linear 0.5 stored as 188, alpha
linear), RGBA16Float (down to a subnormal half), R32Uint, RGBA32Float (-0 kept) and a 48x20 target whose row padding and
4-row pad stay canary; a Clear before `tri_flat` leaves the triangle's colour on its interior and the clear colour on the
exterior and the diagonal. The fake GPU carries the fill out into memory (the one thing it does carry out) after checking
that the 2D registers hold together, so the host tests compare bytes too, and refuse nine mutated fills.

Blending is served for colour attachment 0 as `tu6_emit_blend` writes it: `MTLBlendOperation`'s numbers are the
opcodes, `MTLBlendFactor` maps onto `adreno_rb_blend_factor` as `tu6_blend_factor` maps Vulkan's, `MTLColorWriteMask`'s
bits are reversed into the component enable, the blend colour goes to `RB_BLEND_*_F32`; the Source1 factors (dual
source) and blending into an integer format are refused by name. On the phone (hakios `logs/150855-run.log`, probe line
9j, 0 failures) `tri_cflat`'s half-transparent colour over a cleared target matches Metal's equation at every one of
2016 pixels with a largest deviation of 0, for straight and premultiplied source-over, ReverseSubtract clamped at 0,
Max with Min for alpha, the BlendColor and BlendAlpha factors, and a Red | Alpha write mask without blending; outside
the triangle the cleared colour stays exact.

Sampled textures are served in the fragment stage. The texture and sampler descriptors go into the pass's table as
Gallium's FS_TEX group loads them (`SB6_FS_TEX`, `SP_FS_TEX_SAMP/CONST/COUNT`, `fd6_texture.cc`), and a sample whose
coordinate is a varying, which ir3 turns into a prefetch the FS issues before its first instruction, goes out as
`SP_FS_PREFETCH_CMD` with the slots in the command as fd6 writes it for A6XX. The stage fixtures carry the prefetch
commands now (`ir3-dump.patch`), and the compiler in the driver fills them the same way. On the phone (hakios
`logs/152752-run.log`, probe line 9k, 0 failures) `tri_tex_fs.ll` samples a 4x4 texture at `tri_color_vs`'s interpolated
(red, green), and 3840 of 3840 judged pixels hold exactly the texel their coordinate names; 192 pixels within 1/64 of
a texel edge are left unjudged, and none shows the row-flipped texel.

A Depth32Float depth attachment is served with the depth test and write of `MTLDepthStencilDescriptor`. The hardware
has no linear depth buffer ("Linear D/S is not supported by HW", `fd6_layout.c`), so the texture is laid out TILE6_3
(the pitch a multiple of 64 pixels, the rows counted to 16, the base 4096-aligned) and served as a render target alone:
the CPU cannot read its rows and nothing samples it yet. The registers are `tu6_emit_zs` and `tu6_emit_depth_stencil`
with turnip's Z mode rule; a depth Clear is the 2D engine's R2D_FLOAT32 fill into the tiled image, and because every draw
here is a pass of its own the tail flushes the depth CCU (`PC_CCU_FLUSH_DEPTH_TS`, waited for) so that the next draw's
test finds it. On the phone (hakios `logs/154449-run.log`, probe line 9l, 0 failures) two triangles at z 0.25 and
0.75, drawn in separate passes, keep exactly the pixels the test says in six setups: Less with write in either order,
Greater over a clear to 0, Less without write, and Metal's default Always in both orders.

A texture can be made over an IOSurface (`Texture::wrapSurface`, `hakios_metal_texture_new_iosurface`), the shape a
CAMetalLayer drawable has: hakios's kernel hands a surface's memory to the GPU as a buffer by the surface's ID (its
own request, `surfaceBuffer` in `hakios.gpu.cpp`), and pushes the surface out of the processor's cache when it is
submitted. The kernel answers a surface it already gave this client with the same handle, so the driver keeps one
Buffer per surface and closes the handle once. The rows must start and run in multiples of 64 bytes and hold the width,
and the surface's FourCC must be the format's. On the phone (hakios `logs/155934-run.log`, probe line 9m, 0 failures) a
64x64 'BGRA' surface made with `IOSurfaceCreate` is cleared and drawn into with `tri_flat`, and read back through
`IOSurfaceLock` and the surface's own base address: 4096 of 4096 pixels right, and a canary the processor wrote into
the surface before a Load pass is what the GPU kept outside the triangle.

Indexed draws are the 7-word `CP_DRAW_INDX_OFFSET` turnip writes (`tu_CmdDrawIndexed`): 16- or 32-bit indices from the
index buffer's offset, `MAX_INDICES` what the buffer holds from there, the base vertex in `VFD_INDEX_OFFSET`, and Metal's
primitive restart at the type's all-ones index (`PC_PRIMITIVE_CNTL_0`, `PC_RESTART_INDEX`), off for a draw without
indices. Instances are `NUM_INSTANCES`, and an attribute that steps per instance is `VFD_DECODE_INSTR.INSTANCED` with the
step rate as its divisor. The fake reads the index buffer to find the highest vertex a draw fetches. On the phone (hakios
`logs/161507-run.log`, probe line 9n, 0 failures) a triangle strip restarted at 0xffff covers all 4096 pixels in its
colour, 32-bit indices with a base vertex of 3 draw exactly the second triangle, and three instances at a step rate of 2
under One + One blending leave every pixel at 2 * C0 + C1.

A `constant` buffer a draw indexes is read through a UBO descriptor: where ir3 cannot push a read into the constant file
(an index it cannot bound, as `tri_upos_vs.ll`'s `pos[vid]`), it leaves an `ldc` through UBO n, Metal's
`[[buffer(n)]]`, and the draw loads the descriptors (the address past the binding's offset, the length in vec4) into the
stage's block from the packet, as Gallium's `fd6_emit_ubos` does. On the phone (hakios `logs/162726-run.log`, probe line
9o, 0 failures) the pair's positions pulled by vertex id from a buffer bound at offset 32 cover all 4096 pixels, and the
junk before the offset, drawn on its own, touches none. A `device` buffer is an SSBO read and written through the 3D pipeline's IBO table:
one table for every graphics stage (`SP_IBO`, `SP_IBO_COUNT`, loaded by the plain `CP_LOAD_STATE6` as ST6_SHADER into
SB6_IBO, the shape both witnesses load it in, from memory here where they are bindless), so the vertex and fragment
stages must bind the same buffer wherever both address an index. The same probe line with `tri_dpos_vs.ll`'s
`device const float2*` (9p) covers all 4096 pixels too: the non-bindless table is taken by this GPU.

A sampler a function declares itself (`constexpr sampler`) is made at compile time from the state word the front end
decodes (AIR's address modes onto Metal's numbering; MSL's `none` and `never` compare functions, CoreAnimation's every
one, as no comparison) and bound by the C interface at the index the front end gave it, past the API's own. On the phone
(hakios `logs/165801-run.log`, probe line 9q) a repeat sampler at a coordinate running to 4 reads the repeated texel at
3072 of 3072 judged pixels.

Draws into the same attachments share a pass: the command buffer keeps the pass it opened and joins the next draw to
it when the textures, the actions and the options are the same and no Clear is asked for, so the batch start, the pass
begin and the tail (the CCU and cache flushes, each waited for) are paid once per pass and not once per draw; anything
else recorded, and the submission itself, closes the pass with its tail first. On the phone (hakios
`logs/170720-run.log`, probe line 9r) 200 additive draws in one command buffer leave every pixel at exactly 200, in
591 us of GPU time against 8576 us for the same draws as 200 command buffers.

The tables and the program words are pieces of a few larger buffers a command buffer holds, and a program's words are
uploaded once per command buffer (found by the caller's pointer, taken only when the words are still the same: the
copy compared is a CPU one). Three buffers asked of the kernel for every draw had made the CPU the cost, about 200 us a
draw (42 ms for those 200); now the same 200 draws take 2.6 ms from the first call to the last pixel read, 419 us of it
on the GPU (hakios `logs/171921-run.log`), and the plug-in's render test passes on both roads (`logs/171949-run.log`).

`tools/sweep.cpp` (`make sweep MESA=1`) runs every function of the metallibs named on its command line through the
compiler a draw or a dispatch uses, each in a child process so that an assertion of this host's Mesa build is a counted
cause and not the end, and prints the counts and the refusals grouped by their sentence. The libraries are read where
they lie; nothing of them is kept. Over the 121 metallibs of the iOS 16.5 system image: CoreAnimation's own 39 of 39
vertex functions and 95 of its 95 fragment functions compile, and of everything,
247 of 272 vertex functions, 643 of 678 fragment functions and 3158 of 3397 kernels; the rest is, commonest first,
bicubic samplers, the front end's pointer phis, tile shaders, and so on down.

The two that came last are QuartzCore's YCbCr uber shaders, which ask for Metal's null texture: the front end gives it
a reserved slot past the module's own, and a draw binds the queue's own 1x1 RGBA8Unorm texture of zeros there
(`CommandQueue::nullTexture`), over anything the application bound at that index. `fixed_frag_lph_cph` samples the
one slot as 2D and as 3D, and the table has no seventeenth slot for a second descriptor; on the phone a 3D sample
(`sam.3d`) through the 2D 1x1 descriptor reads its texel exactly as the 2D sample (the prefetch) does -- the probe
writes the texel with a value that is not zero first, so that reading it and reading nothing differ, and every pixel
carried it from both (hakios `logs/173744-run.log`, probe line 9s). That a null texture reads as zero is an
assumption nothing reachable confirms (hakios's `nir_air.h` says why), and the queue's texture is the one place it lives.

A draw writes up to eight colour targets (MRT): each `[[color(n)]]` the fragment function writes goes to colour
attachment n of the pass (RB_MRT[n], SP_FS_OUTPUT_REG[n], its own blend), every attachment the size of the first; an
attachment the function does not write is cleared when it asks for a Clear and otherwise left as it is. Each Clear
waits on a table word of its own -- one shared word would let the second Clear's wait be met by the first one's
write. On the phone (hakios `logs/182202-run.log`, probe line 9t) `tri_mrt_fs` puts its two colours into two
targets, 4096 of 4096 pixels each, and with the second cleared first under one triangle the clear colour and the
draw share it exactly; through Metal, the plug-in's checks (p) and (q) pass on both roads (`logs/182352-run.log`).
CoreAnimation's `attachment_clear_frag_lph`, which writes two colours, is the pipeline its window server exited on.

A fragment function may read its own colour target (framebuffer fetch, `[[color(n)]]` as an argument). ir3 reads it
with an isamm of the texture slot after the function's last (the compiler asks for the non-bindless form, as turnip
does), and a draw puts colour attachment 0's view there. The compiler lowers the fetch itself, a step before ir3 would,
so that the layer and the sample it reads are the 0 they always are here (no layered target, no MSAA): left to ir3
they were a varying no vertex stage writes and a sample id. Before such a draw the colour CCU is flushed and waited
for and the texture caches dropped, so that it reads what the draws before it wrote. On the phone (hakios
`logs/202759-run.log`, probe line 9u) one draw over a Clear and two draws in one pass both come out exact in 4096 of
4096 pixels, and the same two draws without that barrier read the Clear in all 4096: the barrier is what makes the
second draw see the first. Within one draw, overlapping primitives are not ordered against each other's writes.

A pipeline's function constants reach the front end (`compileStage` / `compileAir` take them; the C interface's
`hakios_metal_program_new_bitcode_constants`): each supplied value with its `supplied` flag, so that
`is_function_constant_defined` tells "given as zero" from "not given" -- `tools/kernels/tri_fc_fs.ll` folds to a
different colour for each of four sets, and with the flag dropped two of them collide. Before, every constant was
taken unsupplied, which is wrong for most of CoreAnimation's: 123 of the corpus's constants are ones Metal obliges a
pipeline to give.
CoreAnimation drawing through Metal is behind all of that.

## Build and test

```sh
make test     # on a Mac or on Linux: the tests against the fake GPU
make tsan     # the same under ThreadSanitizer
make asan     # ... and AddressSanitizer
make ios      # out/ios/hakios-metal-probe and libhakios-metal.a, for iOS 16.5 (the SDK as hakios lays it out)
make check    # the tests, then the Metal plug-in (plugin/) on this Mac, without and with hakios-metal
make test MESA=1   # with the compiler linked in, against a Mesa host build carrying hakios's AIR front end
make ios MESA=1    # ... and on the phone, against hakios's iOS build of the same libraries (tools/compiler.mk)
```

On Linux `make ios` takes hakios's cross compiler from `../workspace/xnu-deps/host/bin` (HAKIOS= another tree) and
archives with `llvm-ar --format=darwin`: GNU ar's archive is one Apple's ld does not read.

The kernels: `make fixtures` assembles `tools/kernels/*.asm` into `tests/fixtures/` with `tools/ir3-kernel`, built
against a Mesa host build (MESA_SRC=, MESA_BUILD=); `make air-fixtures` compiles `tools/kernels/*.ll` with an
`ir3_compiler` carrying `tools/mesa/ir3-dump.patch`. `python3 -I tools/check-a6xx.py DIR` checks `a6xx.hpp` against
the generated headers in DIR.

## On the phone

`hakios-metal-probe` does on the real GPU what the tests do on the fake one, one line per check, and then what only
the real GPU can do: run the kernels. In hakios, as the GPU's only client (no window server):

```sh
EXTRA="../hakios-metal/out/ios/hakios-metal-probe" RC=rc.ios.metalprobe userland/build.sh
XL_CMDLINE="-v bootcpu=7 cpus=8 cpufreq=1 gpu=1 gpu_recover=1" XL_SECS=100 ./xl run
```

or beside the window server, with `RC_JOBS=rc.jobs.metalprobe`. `gpu_recover=1` is not optional: a dispatch the GPU
never finishes is found by the wait's timeout alone (no fault register was set in any hang measured), and without it
the kernel waits for nothing again in that run. With it the GPU is reset in about a tenth of a second, FAULTS (GET_PARAM
9) counts one more, and the probe says which line hung. `--recover` hangs the GPU on purpose at the end to show that;
`--bisect` runs `end_only` with each part of the dispatch stream switched off, and `tri_flat` with each group of the
draw stream switched off and each alternative value on; `--metallib FILE FUNCTION` compiles a kernel of the image on
the phone and runs it (a probe built with `MESA=1`).

Stage 9 of the probe is the first draw (measured 29 October, `logs/051851-run.log`): 9a one triangle of `tri_flat` into a canary-filled 64x64
RGBA8Unorm target (2016 interior pixels the colour, 2016 exterior canary, the 64 diagonal pixels 0 or 64 coloured --
either fill rule, anything else a fault -- and the orientation line: the lower-left half is Metal's, the upper-left says
the y scale sign is wrong), 9b the pair (4096 of 4096), 9c the controls (a half viewport: `j >= 2i + 1` in columns 0..31;
the second triangle alone through `vertexStart` 3; a draw of 0 vertices and a draw into a texture without the render
usage refused by name), 9d `tri_color` (every pixel's bytes its barycentric weights within one unit, the diagonal
excluded), 9e `--bisect`, 9f the four stages compiled on the phone against the host's fixtures and the phone-compiled
program drawn. Every line prints the command buffer's time and FAULTS.

Measured on 28 October with the whole stream (hakios `logs/205809-run.log`, 0 failures): `end_only` completes in
38 us; `fill_id` writes 1024 of 1024 words as the kernel computes them in 42 us, another `c` moves every word, 8 groups
leave the other 512 words canary; `fill_id_air` the same, its `c` from a constant buffer; 262 144 words at 4096 groups
in 287 us; the hang on purpose reset in 2.1 s and the fill after it landed.

## How the parts fit

```
 a Metal program
   |  MTLDevice, MTLCommandQueue, MTLCommandBuffer, MTLBlitCommandEncoder, MTLComputeCommandEncoder   (hakios's plug-in, userland/src/HakiosMetal.m)
   |  MTLRenderPipelineState, MTLRenderCommandEncoder -> hakios_metal_program, hakios_metal_cb_draw   (hakios's plug-in, userland/src/HakiosMetal.m)
   v
 command.hpp   queue, command buffers, blits -> packets (pm4.hpp)
 compute.hpp   a Kernel (kernel.hpp) over a grid -> registers and packets (a6xx.hpp)
 render.hpp    a Program (two Stages, linked) over a Texture made as a render target -> one sysmem pass: registers, the
               two program preloads, CP_DRAW_INDX_OFFSET, the CCU and cache flushes (a6xx.hpp)
   v
 gpu.hpp       GEM buffers, submit, fences   (msm DRM requests)
   v
 Transport     iokit_transport.cpp on the phone  |  fake_gpu.cpp in the tests
   v
 the kernel's HakiosGPU (hakios: drivers/gpu/hakios.gpu.cpp) -> the Adreno 7c+ Gen 3 (chip id 0x06030500)
```

UniXNU's AdrenoMetal goes another way (Metal to Vulkan, then Mesa's Turnip). This driver does not use its code. The
numbers that describe the GPU are Mesa's (NOTICE).

## License

MIT, as hakios's own user-space code is. See LICENSE.
