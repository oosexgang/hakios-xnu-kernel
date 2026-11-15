# Metal for Liquid Glass on Renoir (Darwin 27): state and plan

Liquid Glass, CASDF shapes and the large lock-screen clock are drawn by CoreAnimation's **Metal** renderer, in
backboardd. Darwin 27 has no OpenGL renderer left, and its software renderer skips these effects. That is why the
current `gpu=0` configuration shows them incomplete (`docs/darwin26-boot.md`, item 10). The glass needs three
things, all of them device-independent apart from the kernel's GPU profile:
- a Metal device for backboardd;
- a compiler from Apple's shader bitcode (AIR) to the Adreno;
- a kernel path that runs the work.

## What already exists in this repository

| Piece | Where | State |
| --- | --- | --- |
| Adreno 642 bring-up, Renoir profile (chip 0x06060010, 285–490 MHz, zap shader via TrustZone) | `kernel/overlay/bsd/dev/board/adreno.c`; the profile is `gpu { }` of the renoir package's board file (`renoir/board.dts` on the `devices` branch) | `gpu=1` started it on 2026-11-07: GPU clock measured at 490,001,684 Hz |
| GPU service for user space (msm-shaped GEM/submit through IOKit) | `iokit/Kernel/BoardGPU.cpp` | Mesa's shim and the Metal device both use it |
| AIR → NIR → ir3 compiler (Mesa frontend) | `userland/tools/mesa/air/`, `air-*.patch` | 321/321 entry points of the Darwin 16 corpus compile; 255 numerically identical to LLVM; 30 ran on an Adreno (`userland/docs/air.md`) |
| Metal device plug-in (`_MTLDevice` subclass, buffers, textures, render/compute, depth/stencil, argument buffers) | `userland/userland/src/metal_driver.m` | Written and measured against Darwin 16 and 26's Metal; not packaged for Darwin 27 |
| Liquid Glass validation: QuartzCore's own pipelines drawn by Apple's GPU and by ours, compared pixel by pixel | `userland/tools/air/glassref.m`, `glasscase.c`, `glasscases.sh` | Built for Darwin 26's `default.metallib`; needs a Mac with an Apple GPU to record cases |
| MetalPerformanceShaders for CA's blurs (`productName` = "Paravirtual device") | `metal_driver.m` | Darwin 26 |

## What stands between that and glass on Darwin 27, in order

1. **Kernel, `gpu=1` on the 12377 kernel.** Boot with `--boot-arg gpu=1` and the phone's GPU firmware in the small image, and check
   that `HakiosGPU` publishes and that a submit retires.
   Heat: at 490 MHz the GPU sensor read 69.1 °C (`darwin26-boot.md`, item 14). hxff's thermal work has to cap the
   GPU's OPP (the profile's 285/365/443/490 table) before long runs. Until then, `adreno_speed=N` (an index into
   that table, 1 and up) is the safe start.
2. **Mesa for arm64e Darwin 27.** `darwin/userland/mesa26e.sh` builds OSMesa+freedreno for the GL shim. Extend it
   to build `libhakios-air.dylib` (Mesa with `air-frontend.patch`, `air-ir3.patch` and `air-metal.patch`,
   `air_metal.h` contract), then run it through pacstrip and `ldid -Cadhoc` like the other libraries
   (root26-darwin.sh).
3. **Metal device on Darwin 27.**
   - Diff Metal's requirements between 26 and 27 with the existing tools (`userland/tools/metal/owes.py`,
     `refusals.py`, `protometh.py` against Darwin 27's Metal): `MTLAddDevice` asserts on `MTLDevice`/`MTLDeviceSPI`
     conformance and four methods, and Darwin 27 may add required SPI.
   - Rebuild `metal_driver.m` for 24B119 and put it in the root image.
   - Publish the plug-in name on the accelerator service. On Darwin 16.5 that was the only thing keeping Metal from
     finding a driver of ours. The 12377 cleanup had removed it; `IOAcceleratorES::setProperties` in `kernel/overlay/iokit/Kernel/MiniIOMobileFramebuffer.cpp`
     restores it. With boot-arg `metal=1`, a user process may set `MetalPluginName`/`MetalPluginClassName` on
     `IOAcceleratorES`; nothing else is accepted, and an empty string removes them. Without it (the default),
     CoreAnimation keeps its software renderer, as in the release.
   - Keep the system image's QuartzCore `default.metallib` (mkimage26 `KEPT_FILES` already keeps it) and its
     `default.pipelinelib`. The plug-in's `newLibraryWithFile` path was the first crash on 26 (`metal_driver.m`).
4. **QuartzCore's Darwin 27 shaders.**
   - Extract the AIR slice of Darwin 27's `QuartzCore.framework/default.metallib` (`userland/tools/metallib.py`).
   - Run `userland/tools/air/check.sh` over it: what our reader, the NIR translation and ir3 accept.
   - On a Mac running macOS 27, `glasscases.sh` records reference cases for every render pipeline glassref
     can feed. On the phone, `glasscase` replays them on the Adreno. Every difference is a translator bug, found
     shader by shader rather than as a wrong-looking screen.
5. **CoreAnimation's Metal context.** Boot with `gpu=1 metal=1` and `CA_DEBUG_*`/`trace_open=backboardd`. Then
   follow `CA::OGL::new_metal_context` until the home screen is drawn by the GPU, and the glass effects with it.
6. **Frame rate.** With the GPU drawing, `fb_scan` and `fb_double` frame timing (`MiniIOMobileFramebuffer.cpp`)
   applies again. This overlaps hxff's FPS work.

## Darwin 27's QuartzCore shaders through the frontend (2026-11-15)

With Darwin 27.2's files at hand (24B119, `darwin/tools/aea.py`), step 4 started:

- **The host build is Mesa 26.2.5.** `userland/tools/air/mk-host.sh` now builds the release and the patch the
  phone's library is built from (`userland/tools/mesa26/darwin.patch`); it had stayed on Mesa 25.0.7, whose NIR the
  frontend no longer compiles against. It needs `libdrm-dev` on Ubuntu for freedreno's DRM layer.
- **`userland/tools/mesa/air/nir_air.h` was missing** from the repository (every frontend source includes it). It is
  written again from what the sources use: the sampler word's fields and enumerations, the argument-buffer
  and raster-order-group fields of a resource, vertex attributes, notes, linked functions and four-word
  function constants.
- **Fat libraries.** Darwin 26 and 27 ship `QuartzCore.framework/default.metallib` as a fat file, an AIR slice
  (CPU type 0x01000017) beside Apple's precompiled GPU code; `air_metallib_parse`, `air2nir`, `air_run` and
  `air_metal_test` now take the AIR slice, as `userland/tools/metallib.py` did.

`air2nir --report` over that library: **249 functions, all 249 read, 203 translated to NIR.** The 46 that are
not are one case, `interface: input air.imageblock`: CoreAnimation's compute and tile-shading blurs
(`narrow_blur_*_compute`, `tile_simd_blur_*`, `downsample_*_compute`, `backdrop_capture_compute_tile_*`,
`compute_clear`), which keep pixels in an Apple GPU's tile memory. The same blurs exist as render pipelines
(`downsample_blur_vert_lpf`, `variable_blur_vert_lpf` and their fragment functions), which do translate; a Metal
device that does not report tile shading (`MTLGPUFamilyApple4` and up) should have CoreAnimation take those.
That is the next thing to check in `metal_driver.m`'s family answers.

Over every `.metallib` of Darwin 27.2 (42 libraries in the root file system and the OS cryptex), the entry
points -- vertex, fragment and kernel functions -- went from 904 of 1036 translated to **928 of 1036** with
what this pass added to the frontend: `llvm.minnum`/`maxnum`, `llvm.abs` and `llvm.bitreverse`; cube textures
read and written by face; the SIMD lane, group index and width asked for by a call; `sincos` and its pointer;
textures and samplers bound at program scope (`air.global_bindings`, MercuryPoster's posters). Of the 108 left,
95 are the tile-memory kernels above. The other 13: four MercuryPoster compute shaders that call a material
through a function pointer kept in memory; five argument-buffer layouts; two writes to a mip level chosen at
run time (Gallium chooses the level where the image is bound, not in the instruction); one tessellation input
(`air.patch_control_point_input`); one RawCamera kernel. The libraries' `air.visible` functions (3714, mostly
ShaderGraph's MaterialX and CoreImage) are not entry points: Metal stitches or links them into a pipeline.

`air_metal_test` on softpipe ran one of them end to end through `libhakios-air` (the library the Metal
driver links): `dynamic_system_ootf_decimate`, a compute kernel reading one texture and writing another,
dispatched and its output read back.

### The library built on Linux, run on Darwin 27

`userland/tools/mk-mesa26-linux.sh` builds `libhakios-air.dylib` and `air_run` for Darwin without a Mac (LLVM's
clang and ld64.lld, theos's SDK). Its `air_run`, put into the small image of a Darwin 27.2 boot under QEMU on
this kernel (no GPU: softpipe), drew QuartzCore's own `upsample_frag_lpf` from the system image's
`default.metallib`. The pixel it read back, against the same program built for the build machine:

| | x86-64 Linux | Darwin 27.2, arm64, QEMU |
| --- | --- | --- |
| r | 2.14462191e-16 | 2.14462191e-16 |
| g | 4.26559651e-16 | 4.26559572e-16 |
| b | 6.41021723e-16 | 6.41021723e-16 |
| a | 399.91925 | 399.91922 |

The last digits differ where fast math lets each compiler contract differently; the shader, the frontend and
softpipe are the same. So Mesa, the frontend and Gallium run in a Darwin 27 process on this kernel; what is
left for CoreAnimation is the Metal device itself (`metal_driver.m`, step 3 above) and, on the phone, the
GPU instead of softpipe.

### The Metal driver in Darwin 27's backboardd, under QEMU

With `userland/tools/mk-metal-linux.sh`'s `HakiosMetal.bundle` and `libhakios-air.dylib` in the small image,
`metal_props on` run by init before launchd and the kernel booted with `metal=1` (and `RWA_SOFTWARE=1` for
backboardd through `exec_env`: there is no GPU, so the library draws with softpipe), **Metal's own discovery
finds the driver in backboardd**: `IOAcceleratorES` names the bundle, Metal opens it, and CoreAnimation asks
the device for its render pipelines -- its blits, its clears, `home_affordance_base_lph`, the blurs
(`narrow_blur_19_frag_lph`, `variable_blur_frag_lph`) and its uber shader `A3Xghfc` (some 35 s to compile on
softpipe under TCG). `metal_drv_test` in the same boot compiles eight compute pipelines of the system's own
library through it.

What it took on softpipe, each an abort of backboardd before it (`libhakios-air` on the softpipe screen only,
`air_metal.c`): 8- and 16-bit values widened to 32 bits, ffma split, demote as terminate, the packed
normalised conversions as arithmetic. The freedreno screen is not touched by any of it.

Then the rest of the system, in the order it stopped:

1. **Every process, not only backboardd, opens Metal.** `rwa_software=1` in the boot-args gives every
   process softpipe (iconservicesagent had asked for a GPU client and failed).
2. **SpringBoard: "Failed to get MPSDevice".** Siri's orb makes an MPSImageGaussianBlur, and MPS asks for
   Apple8 down to Apple2; `metal_profile=1` claims Apple2 (profile 0 claims Apple1 alone). Apple4 and its tile
   shading are still not claimed, so CoreAnimation keeps its render-pipeline blurs.
3. **MPS: "metallib not found: default.metallib for identifier ...MPSImage".** `mkimage26.py` with `METAL=1`
   keeps MetalPerformanceShaders' kernel libraries (MPSCore, MPSImage, MPSFunctions).
4. **RenderBox** (SwiftUI's renderer, in SpringBoard and iconservicesagent): its own shaders, kept by `METAL=1`
   too. After that SpringBoard and iconservicesagent run on the Metal device without aborting.
5. **"CoreAnimation: Unable to query displays from server"** in PosterBoard and chronod, and their abort: the
   render server in backboardd is busy compiling CoreAnimation's specialised pipelines -- `A3Xghfc` takes
   some 32 s each on softpipe under TCG -- and the clients' display query gives up. This is the emulator's
   speed, not a missing piece: on the phone ir3 compiles on the real cores. Under QEMU the way on is a
   pipeline cache that survives a boot, or fewer variants asked for at once.

### The phone's GPU compiler over Darwin 27's shaders

`userland/tools/air/ir3sweep.sh` runs the phone's own path -- translate, finalise as for freedreno, compile with
ir3 for the Adreno 642L (chip 0x06060010) -- over a `.metallib` on Linux, with no GPU (`rwa_ir3_cost`):

| Library | Compiled by ir3 | Refused, and why |
| --- | --- | --- |
| QuartzCore `default.metallib` | **203 of 249** | the 46 tile-memory kernels (`air.imageblock`) |
| RenderBox `default.metallib` | **74 of 109**; **82** with `LINK=custom_fn=glassBackground_v1` (or any of its five `*_v1` effects) | the 27 `air.visible` functions -- the effects themselves, which are linked and not compiled alone; without an effect linked, the 8 custom-effect shaders |

RenderBox's had been 48: its fragment shaders call `custom_blend` and `custom_fn` behind bits of their
function constants and a pipeline links them only when the bit is set, but the translation stopped at the
call whatever the constants said; `custom_fn` also reaches `sample_custom` as a function pointer, and two of
the effects have static initializers of their own. All three are taken now (`air_to_nir.c`, `link_visible`).

No compiler crash in either. QuartzCore's 203 take 1.8 s of host time together; the costliest is Liquid
Glass's own `glass_background_sdf_all_lpf` (110 ms, 2,740 ir3 instructions), and the most registers any of them
needs is 21 full registers (`glass_background_all_lpf`). So the shaders the glass is drawn with are within
what the phone's compiler takes.

## Why it was not done in this pass

Each of steps 2–6 needs at least one thing this environment does not have:
- Darwin 27's own files: the IPSW and its `default.metallib`. Apple's files are local build inputs, never repository
  content.
- A Mesa build: it needs a third-party toolchain, and this session may not build or run external code.
- The phone, or a Mac with an Apple GPU for the reference cases.

The brightness work, by contrast, was finished and tested here (`docs/backlight.md`). It is independent of
the GPU: under Metal the `dcs` backend is unchanged, and the `soft` backend dims whatever the framebuffer copies.

## What the next person with the phone can do in an afternoon

1. `python3 darwin/tools/boot26.py --wifi-mac … --boot-arg gpu=1 --return-after 300`, with the GPU firmware in
   place and the phone's package brought in before the kernel was built (`tools/device.py add NAME`).
   Confirm the GPU block starts on 12377, and read the temperature lines.
2. On the build machine: `userland/tools/metallib.py` on Darwin 27's `QuartzCore.framework/default.metallib`, then
   `userland/tools/air/check.sh`. The number of Darwin 27 pipelines our compiler accepts tells how far step 4 is.
