# hakios-metal

The Metal driver of [hakios](https://github.com/oosexgang/hakios), in C++: Metal's command model on the phone's
Adreno GPU (an Adreno 7c+ Gen 3, chip id 0x06030500), through the kernel's own GPU driver (HakiosGPU), without Mesa
in between.

What there is today, and what each part is checked by:

| Part | What it does | Checked by |
|---|---|---|
| `pm4.hpp` | the command processor's packets (type 4 and type 7, with their parity bits) | every header against Mesa's `pm4_pkt4_hdr` / `pm4_pkt7_hdr`, all 128 opcodes; opcodes against `adreno_pm4.xml` |
| `a6xx.hpp` | the registers, packet words and descriptors (buffer, 2D texture, 2D image, sampler) a compute dispatch writes, under Mesa's names | `tools/check-a6xx.py`: 580 constants compared by name with the headers Mesa generates from its register XML; a changed value fails it; `a6xx_test` holds the encoders' words against literals worked out by hand (the RGBA8Unorm 64x64 descriptor, Metal's default sampler) |
| `gpu.hpp` | buffers (GEM objects), submissions, fences, through the msm DRM requests the kernel driver answers | the fake GPU; on the phone, `hakios-metal-probe` |
| `command.hpp` | `CommandQueue`, `CommandBuffer` (Metal's states, completed handlers in commit order, `waitUntilCompleted`), blits: copy and fill done by the GPU's command processor (CP_MEMCPY, CP_MEM_WRITE) | tests against the fake GPU, which runs the packets; under ThreadSanitizer and AddressSanitizer too; on the phone, the probe (28 October: a copy of 500 000 bytes exact, fills of every awkward length, 40 command buffers in order) |
| `compute.hpp` | the compute dispatch: a `Kernel` over a grid, as Mesa's computerator runs one on an A6XX, plus the driver params and pushed constant ranges an AIR-compiled kernel needs, and its textures and samplers: an image (a texture the kernel writes) as a 2D storage descriptor at IBO slot `numSsbos + index`, sampled textures and samplers in the two tables Gallium loads into `SB6_CS_TEX` (`SP_CS_TEX_CONST/SAMP/COUNT`), the TP's ISAM mode and border colour registers | the fake GPU, which checks that the dispatch holds together (below), and sixteen tests that mutate an accepted dispatch into one it must refuse; every refusal by its sentence; on the phone, two buffer kernels of our own writing every word as computed (28 October); the texture path is not yet measured there |
| `texture.hpp` | `Texture`: a 2D image of one level in GPU memory laid out as Mesa's `fdl6_layout` lays a linear image out (pitch = `align(width * cpp, 64 * cpp)`, `align(align(height, 4) * pitch, 4096)` bytes), with `replace`/`read` converting the caller's rows to the GPU's, and its two descriptors; `Sampler`: `MTLSamplerDescriptor` as the four `TEX_SAMP` words Gallium's `fd6_sampler_state_create` would write | `texture_test`: the pitch and size rule as a table, a round trip at a stride wider than a row, a sub-region with every other byte of the object canary-checked, the descriptor literals, the sampler words, every refusal by name; `compute_test`: the descriptors the fake reads back at `CP_EXEC_CS` carry the texture's format, size, pitch and address |
| `kernel.hpp`, `tools/kernels/` | a kernel as the dispatch registers want it: machine code and every layout field; our own kernels, as ir3 text and as AIR (LLVM IR) | `tools/ir3-kernel` assembles the text with Mesa's ir3 assembler and reads the words back; `tools/mesa/ir3-dump.patch` gives Mesa's `ir3_compiler` the same output for the AIR; both regenerate the fixtures byte for byte |
| `compiler.hpp` | the compiler inside the driver: AIR to NIR by hakios's front end, NIR to ir3 by Mesa, for this chip, in this process; built in with `MESA=1`, a refusal by name without | `compiler_test`: fill_id.ll compiled here is `tests/fixtures/fill_id_air.hpp` field for field and word for word (a changed field fails it); on the phone, the same bitcode compiled on the device gave the same 64 words and ran (28 October) |
| `metallib.hpp` | `.metallib` files: thin and universal (the AIR64 slice), every offset checked | built files, good and broken; every truncation of a good one is refused |
| `hakios_metal.h` | the core as C, for the plug-in (`HakiosMetal.m`): device, buffers, textures and samplers in Metal's own numbers, libraries, kernels compiled in-process with what they bind (and, per texture resource, the dimension, element class and access the instructions make), queue, command buffers with copy, fill and dispatch (`dispatch2` with textures and samplers), one completion callback | `c_api_test` on the fake GPU, with the compiler (fill_id's resources by kind and index, a dispatch missing a buffer refused by name, one with both completing) and without (compiling refuses by name); on the phone, through Metal's own objects, hakios's `metal_compute_test` (`logs/222820-run.log`) |
| `fake_gpu.hpp` | a GPU for the tests: the requests answered in the process, the packets run by an interpreter as strict as the command processor (bad parity, a count past the end, an unknown opcode, an address outside every buffer: a fault), a dispatch checked against the registers it wrote, its texture state included (the two `SB6_CS_TEX` loads against `SP_CS_TEX_*`, every 2D descriptor's format, pitch and rows against the buffers, every sampler's wrap modes and border table) | itself, by the tests above; and it fails as the kernel fails: a hang ends the submission's wait, and either the GPU is reset and FAULTS counts it (boot-arg `gpu_recover=1`) or nothing is waited for again |

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
samplers, constexpr samplers, the front end's null texture, textures over buffers (`bytesPerRow` of the caller's
choosing), UBO descriptors for an indexed `constant` load ir3 did not push, private memory, threadgroup memory, anything
that draws (each refused by name, not served).
CoreAnimation drawing through Metal is behind all of that.

## Build and test

```sh
make test     # on a Mac or on Linux: the tests against the fake GPU
make tsan     # the same under ThreadSanitizer
make asan     # ... and AddressSanitizer
make ios      # out/ios/hakios-metal-probe and libhakios-metal.a, for iOS 16.5 (the SDK as hakios lays it out)
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
`--bisect` runs `end_only` with each part of the dispatch stream switched off; `--metallib FILE FUNCTION` compiles a
kernel of the image on the phone and runs it (a probe built with `MESA=1`).

Measured on 28 October with the whole stream (hakios `logs/205809-run.log`, 0 failures): `end_only` completes in
38 us; `fill_id` writes 1024 of 1024 words as the kernel computes them in 42 us, another `c` moves every word, 8 groups
leave the other 512 words canary; `fill_id_air` the same, its `c` from a constant buffer; 262 144 words at 4096 groups
in 287 us; the hang on purpose reset in 2.1 s and the fill after it landed.

## How the parts fit

```
 a Metal program
   |  MTLDevice, MTLCommandQueue, MTLCommandBuffer, MTLBlitCommandEncoder, MTLComputeCommandEncoder   (hakios's plug-in, userland/src/HakiosMetal.m)
   v
 command.hpp   queue, command buffers, blits -> packets (pm4.hpp)
 compute.hpp   a Kernel (kernel.hpp) over a grid -> registers and packets (a6xx.hpp)
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
