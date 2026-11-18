# Metal for Liquid Glass on Renoir (Darwin 27): state and plan

Liquid Glass, CASDF shapes and the large lock-screen clock are drawn by CoreAnimation's **Metal** renderer, in
backboardd. Darwin 27 has no OpenGL renderer left, and its software renderer skips these effects. That is why the
current `gpu=0` configuration shows them incomplete (`Documentation/darwin26-boot.md`, item 10). The glass needs three
things, all of them device-independent apart from the kernel's GPU profile:
- a Metal device for backboardd;
- a compiler from Apple's shader bitcode (AIR) to the Adreno;
- a kernel path that runs the work.

With `gpu=1 metal=1` and the pieces below, backboardd takes a Metal device of ours, and CoreAnimation draws
the screen on the Adreno.

## State (redwood, iOS 27.2 24B119)

Drawn by the GPU, as CoreAnimation means them to look:
- the lock screen with its glass clock;
- the home screen with its widgets and icons, the dock's and the search field's glass;
- a system alert's blurred glass over the home screen.

Four boots of 240–300 s ran with no crash of backboardd. The last one, with every fix below, had no refusal by
the plug-in before the run's end, except an accounting setter it takes without keeping.

Speed, measured with `rc.jobs.fps` and `darwin/tools/frames.py` (the 0.7 s after each gesture), with redwood's
defaults (`fb_hz=120`, `atm_diagnostic_config`, `metal_gmem_binning=1` in its `board.dts`; `logs/080215-run.log`,
`logs/081352-run.log`, `logs/082525-run.log`):

| Scene | Frames a second | Within 18 ms | GPU per frame |
| --- | --- | --- | --- |
| The lock screen, its clock and glass | 60 | | |
| The home screen's pages swiped | 66–70 | 87–89% | 10.1–10.6 ms |
| Settings' list flung under the glass search field | 80–82 | 95–97% | 9.7–10.1 ms |
| An app opening or closing (not measured again) | 12–20 | | up to 0.5 s |

The day before, the same scenario gave 51 and 57. Each frame now starts while the GPU still has the one before:
the window server's render thread used to wait for the GPU's end between frames on the plug-in's surface lock
(next section, item 6). In a long burst a frame is made every 10.5 ms, its GPU work done 19 ms after it was made
and on the panel at 30 ms; what paces that burst is the presenter's copy (What is next, 120 Hz).

At rest the home screen's frame is 16–19 ms of GPU time, drawn when the clock icon's hand moves (10 a second). A
week before these were 20 frames a second everywhere and 31–36 ms for the home screen's main pass alone.

## The pieces

| Piece | Where | What it does |
| --- | --- | --- |
| Adreno 642 bring-up, Renoir profile (chip 0x06060010, 285–490 MHz, zap shader via TrustZone) | `drivers/board/adreno.c`; the profile is `gpu { }` of the renoir package's board file (`renoir/board.dts` on the `devices` branch) | `gpu=1` started it on 2026-11-07: GPU clock measured at 490,001,684 Hz |
| GPU service for user space (msm-shaped GEM/submit through IOKit) | `iokit/Kernel/BoardGPU.cpp` | Mesa's shim and the Metal device both use it |
| AIR → NIR → ir3 compiler (Mesa frontend) | `components/metal/tools/mesa/air/`, `air-*.patch` | 321/321 entry points of the Darwin 16 corpus compile; 255 numerically identical to LLVM; 30 ran on an Adreno (`components/metal/docs/air.md`) |
| Metal device plug-in (`_MTLDevice` subclass, buffers, textures, render/compute, depth/stencil, argument buffers) | `components/metal/src/metal_driver.m` | Written and measured against Darwin 16 and 26's Metal; not packaged for Darwin 27 |
| Liquid Glass validation: QuartzCore's own pipelines drawn by Apple's GPU and by ours, compared pixel by pixel | `components/metal/tools/air/glassref.m`, `glasscase.c`, `glasscases.sh` | Built for Darwin 26's `default.metallib`; needs a Mac with an Apple GPU to record cases |
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
   - Diff Metal's requirements between 26 and 27 with the existing tools (`components/metal/tools/metal/owes.py`,
     `refusals.py`, `protometh.py` against Darwin 27's Metal): `MTLAddDevice` asserts on `MTLDevice`/`MTLDeviceSPI`
     conformance and four methods, and Darwin 27 may add required SPI.
   - Rebuild `metal_driver.m` for 24B119 and put it in the root image.
   - Publish the plug-in name on the accelerator service. On Darwin 16.5 that was the only thing keeping Metal from
     finding a driver of ours. The 12377 cleanup had removed it; `IOAcceleratorES::setProperties` in `drivers/iokit/MiniIOMobileFramebuffer.cpp`
     restores it. With boot-arg `metal=1`, a user process may set `MetalPluginName`/`MetalPluginClassName` on
     `IOAcceleratorES` to this port's own `HakiosMetal`/`HakiosMetalDevice` (Metal makes a bundle path of the name, so
     no other value is taken); nothing else is accepted, and an empty string removes them. Without it (the default),
     CoreAnimation keeps its software renderer, as in the release.
   - Keep the system image's QuartzCore `default.metallib` (mkimage26 `KEPT_FILES` already keeps it) and its
     `default.pipelinelib`. The plug-in's `newLibraryWithFile` path was the first crash on 26 (`metal_driver.m`).
4. **QuartzCore's Darwin 27 shaders.**
   - Extract the AIR slice of Darwin 27's `QuartzCore.framework/default.metallib` (`components/metal/tools/metallib.py`).
   - Run `components/metal/tools/air/check.sh` over it: what our reader, the NIR translation and ir3 accept.
   - On a Mac running macOS 27, `glasscases.sh` records reference cases for every render pipeline glassref
     can feed. On the phone, `glasscase` replays them on the Adreno. Every difference is a translator bug, found
     shader by shader rather than as a wrong-looking screen.
5. **CoreAnimation's Metal context.** Boot with `gpu=1 metal=1` and `CA_DEBUG_*`/`trace_open=backboardd`. Then
   follow `CA::OGL::new_metal_context` until the home screen is drawn by the GPU, and the glass effects with it.
6. **Frame rate.** With the GPU drawing, `fb_scan` and `fb_double` frame timing (`MiniIOMobileFramebuffer.cpp`)
   applies again. This overlaps hxff's FPS work.

## Darwin 27's QuartzCore shaders through the frontend (2026-11-15)

With Darwin 27.2's files at hand (24B119, `darwin/tools/aea.py`), step 4 started:

- **The host build is Mesa 26.2.5.** `components/metal/tools/air/mk-host.sh` now builds the release and the patch the
  phone's library is built from (`components/metal/tools/mesa26/darwin.patch`); it had stayed on Mesa 25.0.7, whose NIR the
  frontend no longer compiles against. It needs `libdrm-dev` on Ubuntu for freedreno's DRM layer.
- **`components/metal/tools/mesa/air/nir_air.h` was missing** from the repository (every frontend source includes it). It is
  written again from what the sources use: the sampler word's fields and enumerations, the argument-buffer
  and raster-order-group fields of a resource, vertex attributes, notes, linked functions and four-word
  function constants.
- **Fat libraries.** Darwin 26 and 27 ship `QuartzCore.framework/default.metallib` as a fat file, an AIR slice
  (CPU type 0x01000017) beside Apple's precompiled GPU code; `air_metallib_parse`, `air2nir`, `air_run` and
  `air_metal_test` now take the AIR slice, as `components/metal/tools/metallib.py` did.

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

`components/metal/tools/mk-mesa26-linux.sh` builds `libhakios-air.dylib` and `air_run` for Darwin without a Mac (LLVM's
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

With `components/metal/tools/mk-metal-linux.sh`'s `HakiosMetal.bundle` and `libhakios-air.dylib` in the small image,
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
   speed, not a missing piece: on the phone ir3 compiles on the real cores.

What the time was, measured (15 November, `rwa_render_times`, printed by the driver for any pipeline over a
second): of `A3Xghfc`'s 39 s, 5 were its translation and 33 softpipe's compiling of the fragment stage --
`nir_to_tgsi`'s own passes, by the one sample there was of it -- and the rest waiting. And it was asked for
again and again: 339 pipelines of 42 functions in one boot, a pipeline per blend and attachment format, each
translated and compiled from the start, all of it under the one lock the driver keeps around the Gallium
context, so the render server's draws waited behind it. Four changes, each a commit:

- the driver hands the library its lock (`rwa_set_pipe_lock`) and a pipeline is translated outside it;
- compiled stages are shared on softpipe too (the stage cache had left software screens out): the second
  `A3Xghfc` compiled in 0.45 s instead of 35;
- translations are kept (`air_to_nir_kept`): the same function in the same specialisation is translated once;
- multisampling is pretended with one sample where the screen asks for that (`fake_sw_msaa`): SpringBoard's
  first 4-sample attachment had been refused on softpipe, and with it everything it drew through Metal.

`sample_every=N` in the boot-args samples backboardd's and SpringBoard's threads in the library every N
seconds, and `userland/tools/samples.py --profile` names where the time went.

With all of it, the boot after: `A3Xghfc`'s later pipelines took 1 s instead of 40, backboardd sampled 124 times
was in the library in a handful of them, and the driver now writes what it drew into a surface copy back to the
surface (the display's included). Still no frame: with `swaplog=1`, not one IOMobileFramebuffer swap in 40
minutes -- CoreAnimation's first frame wants its whole set of pipelines, and the render server had made 93 of
them, the last `TmuaA2Xhf_Isrc_IsrcM1` (23 minutes alone the boot before), when the run ended. Under TCG the
first Metal frame is a matter of hours; on the phone those compiles are milliseconds each (the sweep above).

### What crashed the software path, and a table that read zeros (15 November, evening)

A boot with the day's library had backboardd restarting 17 times in 15 minutes: "Unknown NIR opcode:
unpack_32_2x16_split_x", in the software path's own lowering of narrow loads (QuartzCore's `p3_clamp` and
`edr` fragments). `components/metal/tools/air/softpipesweep.sh` now makes every fragment, vertex and kernel function
of a library into a pipeline on softpipe, a process each, and names the ones whose process dies -- under QEMU
that process is the window server. Over QuartzCore, VFX, RenderBox and MercuryPoster it found four causes:

- `widen_16bit` did not take halves of a word apart (the backboardd restarts);
- the argument buffer layouts and values of a shader were left in the translation's context and read after
  it was freed (VFX's particle shaders: a use after free, so the phone's path as much as the emulator's);
- `irem`, which nir_to_tgsi has no case for (VFX's lightfield);
- MercuryPoster's Flocon snowflakes: a 49,248-word local array, which overflows TGSI's 16-bit register
  numbers. That array was the module's colour ramps -- 197 KB of `constant` halves -- laid out in private
  memory that nothing ever wrote: **a constant table read at an index the shader computes read zeros**, on
  the Adreno too (`components/metal/tools/air/const_table.ll`). Such tables are now the shader's constant data, read
  with `load_constant` (ir3 uploads it with the shader; on softpipe it is bound as constant buffer 31), and
  folded to numbers where the offset is known. Flocon went from a crash to 2,655 ir3 instructions.

A stage that would still overflow TGSI is refused by name, a pipeline the emulator cannot make rather than a
render server that dies. After the fixes: QuartzCore 236 of 249 made on softpipe (the 13 others are tile
kernels, refused), VFX 559 of 601, MercuryPoster 51 of 52, RenderBox 74 of 82, and none dies.

### The wallpaper's process, ended by a watchdog

With the render server no longer dying, the console showed why no wallpaper had ever come: PosterBoard runs
the collections poster (Stripe Yellow, the image builder's default) in an extension process and gives its
scene a few seconds to be made -- "Provision violated for watchdog scene-create" -- and with Metal on the
emulated processor it never made it in time: 84 terminations in one boot, each followed by another process
that met the same end. `components/wallpaper/inject_poster.c` now, under `rwa_software=1` only, makes FrontBoard's
watchdog providers answer no policy (`-watchdogPolicyForProcess:eventContext:`) and a watchdog delegate's
termination request a line in the log instead (`-watchdog:requestsTerminationOf:forReason:`). FrontBoard is
not loaded when PosterBoard's libraries are, so it is looked for again 3 and 30 seconds in. On the phone,
without that boot-arg, nothing changes: there the GPU draws the first frame in time.

### The black lock screen, taken apart (16 November)

The display driver's swap lines (now by `printf`, which QEMU's serial console shows) said backboardd did swap
frames through Metal: surface 3, then 21 or 27, BGRA 1125 x 2436, each after a command buffer of 30 to 113
draws taking 7 to 21 minutes under TCG. Every pixel of them was zero, alpha included. What it took to say why,
each a line the driver or the kernel prints:

- `metal_driver.m` says what colour the first forty large surfaces were written back as and taken in as (the
  mean of each byte over a 32 x 32 grid), and `metal_sync=1` says, for each draw, its pipeline, attachments,
  clear, viewport, scissor and depth, and for the first sixteen into a screen-sized target the first words of
  every buffer bound and the pixel in the middle once the draw is done. The composite was CoreAnimation's own
  uber shaders (`fixed_vert_lph_gen` specialised as `VfxXgh`, the fragment `A3Xghfc`) over an empty layer
  tree: a lock screen with nothing in it.
- **No wallpaper because its snapshots timed out.** PosterBoard asks the collections poster for snapshots and
  ends the extension when they run out ("timeout hit", every 30 s, a new process each time). `inject_poster.c`
  now, under `rwa_software=1`, makes them sixty times longer: every getter of a timeout in PosterBoard's PUI,
  PBF and PR classes (`-[PBFPosterSnapshotThrottlePolicy captureTimeoutInterval]`,
  `-[PUIPosterSnapshotRequest timeoutInterval]`, `pui_renderSessionTimeoutInterval` and the rest, each said by
  name), and the timeout a request or a host configuration is handed when it is made. The collections poster
  then opened Metal for the first time, and a layer of the wallpaper's colours was drawn.
- **Every vector shape filled its whole layer.** That layer was one colour, (0, 157, 222) -- the blue stripe of
  Stripe Yellow -- on every pixel. CoreAnimation draws a vector shape in three steps: `path_rect` and
  `path_cubic` add up a coverage mask into an R32Float attachment with one-plus-one blending, and `path_blit`
  reads it back with a framebuffer fetch (`[[color(2)]]`) and colours the shape by it. softpipe's TGSI
  interpreter has no FBFETCH, so the mask read was whatever a register held, and the last shape covered
  everything. `air_metal.c` now makes a software fragment stage's fetch a texel fetch from a copy of the
  attachment taken before the draw (and again after its clear), bound past the shader's own textures;
  `components/metal/tools/air/fbfetchdraw.sh` checks it through the library. The phone's freedreno fetches for itself
  and is not touched.

With both, the boot after drew the lock screen's wallpaper through Metal: swap 3, surface 27, Stripe Yellow
whole -- the yellow ground (251, 205, 103) and its six stripes, every one at its own place -- composited by
backboardd's render server out of the collections poster's layer, every pixel of it through `HakiosMetal`,
`libhakios-air` and softpipe. The first Metal frame on the screen, two hours into a boot under TCG.

### The black ground that was a short run (16 November, afternoon)

After that boot, every run without `metal_sync=1` showed the stripes over black, and three changes made for it
(the library's flush after each software draw, the samplerless units for compute only, a no-copy buffer that
follows its caller's pages) did not bring the ground back. It was not the asynchronous path. The frames under TCG
take 8 to 27 minutes each, and the yellow ground first comes with the fourth: the one boot that showed it had four
swaps, every black one three (the earliest ended by a 30-minute limit on the shell around QEMU, the next ones by
the 6900 seconds `run27.sh` was given). A run given the time, with the kernel of the appstore-ios27-wip merge and
draws waited for once a command buffer (no `metal_sync`), drew Stripe Yellow whole at swap 4, surface 65.

### The phone's GPU compiler over Darwin 27's shaders

`components/metal/tools/air/ir3sweep.sh` runs the phone's own path -- translate, finalise as for freedreno, compile with
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

### Every library of Darwin 27, through the phone's compiler

The same sweep over all 258 `.metallib` files of the Darwin 27 image under 3 MB (everything but MPS's, Espresso's
and GESS's large ones), run as the 15 November library stood:

| | Functions |
| --- | --- |
| compiled by ir3 for the Adreno 642L | **3,843** (3,784 at the first sweep the same day) |
| not entry points -- `air.visible` library functions and modules with no entry-point metadata, which Metal links into a pipeline and never compiles alone | 2,438 |
| refused | 497 (556) |
| crashed the compiler | 2 (an ir3 register-allocation check in `NRFV1`'s BlinkDetection and `H13FastRawScale`; camera, not drawing) |

So 88.5% of the entry points Darwin 27 ships compile for the phone's GPU. And they do so without a memory
error: `components/metal/tools/air/memcheck.sh` runs the same path under valgrind's memcheck, a process per function,
and QuartzCore's 249, RenderBox's 109 and MercuryPoster's 52 are clean -- a read past a buffer there would be
a crash of backboardd or SpringBoard on the phone, where nothing would say why. What the sweep found, and what was
fixed for it on the way (each a commit of its own):

- **ir3's prefetched sample used the texture's index as its sampler's** (`components/metal/tools/mesa26/darwin.patch`, ir3_compiler_nir.c).
  GL never sees it; QuartzCore's own shaders sample texture 3 with sampler 0, and on the phone such a sample
  read sampler 3. The fix is in the library the image carries.
- **The two stages of a pipeline were linked by order, not by name, and every varying took a whole slot**
  (`link_varyings` in `air_metal.c`): MercuryPoster's tori wallpapers pass 40 and failed in ir3. They are now
  linked by name and packed; `ir3sweep -p VERTEX FRAGMENT` compiles a pair as the driver links it.
- **RenderBox's linked functions** (`custom_blend`, `custom_fn`) behind closed function constants, as function
  pointers, and with static initializers of their own: 48 of 109 to 74, and 82 with an effect linked.
- **A texture written at a mip level a constant buffer names** (VariableBlurrer, the wallpapers' blur): the
  level is read at the bind and the view made there.
- Arrays of textures indexed at run time (App Store's chicletFragment); a function table in a local array
  (MercuryPoster's shippou); the queries of a depth texture, which crashed the translation; return jumps left
  in an entry point, which crashed ir3; and the SIMD operations and builtins MetalFX and the video libraries use.

### All 300 libraries, MPS, Espresso and MLX among them

The sweep again, over every `.metallib` of the image this time -- 300 files, the large MPS, MLX (GESS,
CorePhotogrammetry) and Espresso libraries included -- a process per function, as the library stood after the
argument-buffer and dispatch work below and before tessellation:

| | Functions |
| --- | --- |
| compiled by ir3 for the Adreno 642L | **24,369** on 16 November (24,244 on the 15th; 22,972 before tessellation; 22,572 at the first sweep of the 300) |
| not entry points | 13,313 |
| refused | 3,122 on the 16th (3,246; 4,518; 4,659): 1,770 of them MLX's and Morpheus' simdgroup matrices (`<64 x float>`; GESS's 1,731 compile since the evening of the 16th), 505 ray tracing, 90 CoreImage's visible function tables that only a pipeline's linked functions fill |
| crashed the compiler | 15 on the 16th, 16 on the 15th (275): 14 of the ray-tracing BVH builder's kernels, where ir3 finds no end instruction -- a sweep's artefact: their loops count by `threadsPerThreadgroup`, a function constant the sweep gives no value, and a pipeline must; `NRFV1`'s BlinkDetection, since compiled (a predicated branch on a compare from another block, which ir3's predicate allocation reloads into only one way in; such an `if` now takes an ordinary branch, `components/metal/tools/mesa26/darwin.patch`); MPS's MPSLSTMMultiInputKernelFloat5, whose register allocation ir3's own validation rejects (it was refused before pointers in a local were kept as numbers) |

What the refusals are, the MLX libraries set apart (they are machine learning, not drawing, and most of what
is left in them is `<64 x float>` vectors and pointers kept in memory): tessellation (AvatarKit's 432 Memoji
shaders, which the next commits draw), ray tracing (GPUToolsReplay, 505), tile memory (224), visible function
tables (91), MPSNDArray's pointers stored in memory (~170), and a long tail. What was fixed for it, each a
commit of its own:

- **Argument buffers with values in them** (VFX, the dynamic wallpapers): an `air.indirect_constant`, an inline
  struct (its members flattened into the set, numbered from its own `[[id(n)]]`) or an atomic is bytes the
  application writes through `-constantDataAtIndex:`; the set is now also a buffer at its own index, read,
  written and atomically updated where the values lie (`components/metal/tools/air/argbufvalcheck.sh`). The encoder answers
  the set's own `air.buffer_size` and alignment, finds members by id, and hands back a value's real place. Sets
  nothing reads are not declared at all, the struct type is the parameter's pointee (`global_constants.634`),
  and padding inside a packed struct is passed over -- an array of bytes, or since 15 November a lone `i8`,
  which is how clang pads the byte between a bool and a half (the Ocean and clouds wallpapers'
  `external_entity_constants`; `argbuf_halfpad.ll`). VFX: 453 -> 571 of 651. The four fragment shaders of
  `DynamicBackgroundPosterExtension.appex` (Ocean twice, clouds, Glitter) all compile for the Adreno 642L once
  the function VFX links into them at run time is linked here too:

      components/metal/tools/air/ir3sweep.c built as components/metal/tools/air/ir3sweep.sh builds it, then
      ir3sweep -l 'vfx::api::applySystemTreatments=VFX/vfx::api::applySystemTreatments.bc' 0x06060010 \
               'Poster/vfx_custom_shader_Ocean_shader_31::vfx_fragment.bc=vfx_custom_shader_Ocean_shader_31::vfx_fragment'

  (each `.bc` taken out of its `default.metallib` by `components/metal/tools/metallib.py dump`).
- **Threadgroup buffers past index 0** (VFX's merge sorts, CoreRE): buffer n starts where 0..n-1 end, and only
  the dispatch knows where, so the base is folded in per dispatch from the lengths
  (`rwa_threadgroup_memory_at`, `components/metal/tools/air/tgcheck.sh`).
- **Dispatches whose last groups are partial**: `-dispatchThreads:` with a count its groups do not divide was
  refused, and every image-sized dispatch of Apple's frameworks is one. The library runs a variant of the
  kernel behind `thread_position_in_grid < threads`, the count handed over in a constant buffer, and
  `[[threads_per_grid]]` is the count (`components/metal/tools/air/gridcheck.sh`; `RWA_COST_PARTIAL=1` has the sweep compile
  that variant, and QuartzCore's 203 kernels compile either way).
- **RealityKit's stereo and layered inputs** (`amplification_id`, `amplification_count`, a fragment's
  `viewport_array_index`): the device amplifies nothing and draws one viewport. CoreRE 299 -> 356.
- **A struct input its function constant leaves out** is a tree of zeros: Maps (VectorKit 305 -> 313) and the
  camera pipeline (CMImaging 104 -> 153).
- Quad reductions, `asinh`/`acosh`/`atanh` (checked by `components/metal/tools/air/intrincheck.py`), OpenCL's grid queries,
  `atomic.fence`, and SIMD queries, `ctz` and `popcount` at the width the call returns -- four of which were
  asserts in the NIR builder, not refusals.
- Two crashes turned into refusals by name: a value a caller stored in a local and a callee read back (eight
  of RealityKit's shaders), and a vector length NIR has no register for.
- **Pointers compared** (Morpheus, MPS's in-place kernels): two buffer arguments are equal when the dispatch binds
  one buffer at both indices and the pointers are as far into it, which is folded in per dispatch like the
  threadgroup bases (`AIR_BUFFER_ID_INPUT`); a pointer walked up to an end pointer compares the two offsets
  (`components/metal/tools/air/bufcmpcheck.sh`). A callee may hand back an array of numbers, an element a parameter
  (`components/metal/tools/air/wordcheck.sh`), and a pointer phi may choose among 32 places. Morpheus 297 -> 355 of 364,
  MPSNeuralNetwork 726 -> 836 of 868.
- **Pointers kept in a local as numbers**: a pointer here is a place and an offset decided while translating,
  with no bits, which is why one stored into a local was followed by slots at constant offsets. MPS's LSTM
  kernels copy a table of their inputs in a loop, others store two pointers into one local on two paths,
  and the RNN kernels carry a pointer from input to input around a loop. Such a function is translated
  again with those pointers as two words, (offset, 1 + place), and a load of one is the choice among the
  places by the second word (`air_ptr_numeric`; again once more where a place is met after a load chose
  without it). `components/metal/tools/air/ptr_table.ll` and `ptr_loop.ll` check both on softpipe. MPSNeuralNetwork
  836 -> 854 of 868.
- **The long tail, 15 November night**, each with a case of its own on softpipe or against LLVM:
  an atomic through a pointer chosen at run time, behind an if as a store through one already was (Vista's
  splat culling; `components/metal/tools/air/ptr_atomic.ll`); a pointer through an integer and back,
  `inttoptr (ptrtoint %p + bytes)`, the integer kept as the pointer and the sum as the pointer moved
  (MPSImage's DilateFilter4 and ErodeFilter4, 1832 -> 1834 of 1837; `ptr_int.ll`); Metal 3.2's atomic
  wait, a loop reading the place until it changes, its notify, the device-coherent loads and stores and
  Apple's yield hint (MPSCore's single-pass scans, 49 -> 54; `wait_notify.ll`); and integers of two to
  thirty-one bits that NIR has no width for, kept zero-extended in 32 bits -- masked after arithmetic,
  their sign extended where it is read as signed (Vista's two-bit enum switch, 62 -> 73 with the atomic;
  `components/metal/tools/air/loweringcheck.py` against LLVM, 25 of 25). ImageIO, swept again, compiles all 27.
- **16 November**: arrays in argument buffers -- Metal gives an array member one element's size and its
  count in the field after it; a value array is checked at its whole size and an array of up to 32
  descriptors is a member per element, each at its own place and `[[id]]` (ClothPhysics, ComputeGraph, MXI;
  `argbufcheck.py`'s new case); a texture or sampler out of a local array at a computed index, chosen as a
  function out of a local table already was (CMImaging's srlApply and StyleEngine), with softpipe's compute
  fetches made to read their own texture's unit (`texarraycheck.sh`); RGB9E5 unpack and pack
  (`rgb9e5check.sh`). The phone's path under valgrind's memcheck after all of it: QuartzCore 249, RenderBox
  109 and CMImaging 215 functions clean, none with an error.
- **16 November, later**: a texture chosen between two members of an argument buffer and read through a
  64-bit integer -- the handle a member is, not its bytes -- split where it is sampled (VFX's
  particle_ribbon shaders, 580 -> 584 with the next; `tex_sel.ll`); an image read at the level a buffer
  holds, bound at that level as a write already was (vfx_reprojection_map; CoreRE 410 -> 421, CMImaging
  209 -> 212; `mip_read.ll`). That test found two older faults: a `read_write` texture was bound without
  read access, and softpipe's image load with no format of its own unpacked nothing -- every `read()` of a
  read_write texture on the QEMU bench returned zeros (the softpipe fix is in
  `components/metal/tools/mesa26/darwin.patch`). QuartzCore, HDRProcessing, CoreImage and MPSImage swept again
  with no function lost.
- **16 November, evening**: simdgroup matrices. Metal's `simdgroup_matrix<T, 8, 8>` is a `<64 x T>` in AIR
  of which a thread holds two elements, its `thread_elements()` in lanes 0 and 1 -- the only lanes MLX reads
  or writes -- so such a vector is those two lanes here, and
  `air.simdgroup_matrix_8x8_multiply_accumulate` reads row fm of A and column fn + j of B from the lanes
  holding them by shuffles within the group of 32, in Apple's layout (fm = 4 l4 + 2 l2 + l1,
  fn = 4 l3 + 2 l0). Softpipe has no shuffle, so `components/metal/tools/air/simdmatrixcheck.py` checks the lane
  arithmetic on 32 simulated lanes against a plain product; ir3 compiles all 1,731 of GESS's MLX kernels that
  were refused for it. A pointer kept as numbers in a local is decoded only into the places of its
  own address space (MLX's implicit_gemm_conv), and `air.quadgroups_per_threadgroup` is answered.

### Tessellation without a tessellator

A post-tessellation vertex function (`air.patch` on the entry point) is drawn as a grid: every patch an
instance of N x N cells, two triangles each for a quad patch and N^2 triangles for a triangle one,
`[[patch_id]]` the instance counted from the draw's first patch, and `[[position_in_patch]]` the grid point
the vertex index names. `-drawPatches:` is then an ordinary instanced draw of `rwa_render_patch_vertices`
vertices. N is 8 (`RWA_TESS_LEVEL`, 1 to 64), the same for every patch: the factors a tessellation kernel
wrote are not read, so a patch the application wanted finer is as fine as the others and one it wanted
coarser costs as much. `components/metal/tools/air/tesscheck.sh` draws two quad patches and a triangle one on softpipe and
checks every pixel's coordinates. AvatarKit's Memoji shaders read their control points out of buffers
(OpenSubdiv) and compile. Control points through `[[stage_in]]` (`patch_control_point<T>`, read through a
function Metal makes out of the vertex descriptor) are read since 16 November as a kernel's `[[stage_in]]` is,
by the render pipeline's vertex descriptor: the k-th control point of the patch, the patch an instance
(`tess_cp.ll`, drawn by `tesscheck.sh`). LocalAuthenticationPrivateUI's spline renderer -- the Face ID
animations -- compiles 12 of 12 (from 5), BiometricKitUI's MTLSpline 3 of 3. A patch index buffer and several
instances of the patches are refused by name.

### What the driver threw away

Four things a Metal encoder sets were accepted and dropped, and each changes pictures rather than failing:

- **Raster state**: `-setCullMode:`, `-setFrontFacingWinding:`, `-setTriangleFillMode:`, `-setDepthClipMode:` and
  `-setDepthBias:slopeScale:clamp:` -- every draw was two-sided, filled, clipped and unbiased. They ride in
  `rwa_draw` now, and the library keeps a raster state per distinct ask with the pipeline.
  `components/metal/tools/air/rastcheck.sh` draws a clockwise and a counter-clockwise triangle under every cull mode and
  winding, as lines, and clipped and clamped outside the depth range; its first run had every culled case the
  wrong way round, which is how Metal's winding turned out to be Gallium's own sense despite the viewport's
  flip of y.
- **Indirect draws and dispatches** were refused on the reading that their arguments are read when encoded.
  They are read when the work runs, in order, every dispatch waited for -- so what a kernel earlier in the
  command buffer wrote is there.
- **Packed 32-bit formats** (RGB10A2, RG11B10, RGB9E5, BGR10A2), which the home screen's icon service asks for,
  by DXGI's order (first channel at the low end) behind `RWA_PACKED32=0`, as the 16-bit four are behind
  `RWA_PACKED16=0`; `air_metal_test --pixel-formats` probes the two unorm ones field by field.
- **A callee reading its caller's values** -- a struct of pointers at computed offsets, a pointer read back from
  a local -- now takes them as parameters of its own (`components/metal/tools/air/importcheck.sh`); before, they were
  quietly undefined or crashed the compiler.

Two of these were found by booting, not by a sweep: Darwin 27 under QEMU with this library (softpipe in place of
the Adreno) had iconservicesagent refused RGB10A2Unorm 25 times a boot -- the home screen's icons -- and
backboardd refused a texture whose usage carried Apple's private 0x10000 twice. The boot after the fixes has no
refusal from the driver in any process; it built 39 of CoreAnimation's pipelines in an hour and had presented
no frame yet, which under TCG is the emulator's speed and not a failure (the phone compiles the same shaders in
milliseconds).

## What stays open

- **Tile memory** (`air.imageblock`, 134 functions, QuartzCore's 46 among them): Apple's GPU keeps a tile in fast
  memory between draws, and the Adreno has nothing a shader can address that way. CoreAnimation has paths
  without these kernels; whether every layer it draws does is for the phone to show.
- **Argument buffers written without an encoder**: resources put into a set by `gpuResourceID`/`gpuAddress`.
  The values in a set are read now; a descriptor still has to come through an `MTLArgumentEncoder`, and the
  driver answers that it has no bindless handles. Sets inside sets (VFX's instances, 11) are refused.
- **Visible function tables** (CoreImage's ubershader, 90): a table is now a buffer of handles -- a linked
  function's place in its pipeline's list, plus one -- that the driver makes (`newVisibleFunctionTableWithDescriptor:`,
  `functionHandleWithFunction:`) and binds as a buffer, and a call out of it is the choice among the linked
  functions (`components/metal/tools/air/vftcheck.sh`). What CoreImage links into the ubershader is decided at run time,
  so the sweep cannot compile those 90 by itself; on the phone they are for CoreImage's first frame to show.
- **Tessellation factors**: patches are drawn at one level (above), with no per-patch factors and no
  crack-free edges between patches of different levels.
- **More than 16 textures in a stage**: freedreno binds 16 without bindless, and so does this library.
- **Simdgroup matrices on the hardware**: the 8x8 products, loads and stores compile and their lane arithmetic
  is checked in Python, but softpipe has no shuffle, so no product has run on a GPU yet; the first MLX
  kernel on the phone is the check. Asynchronous simdgroup copies (`air.simdgroup_async_copy_2d`, MPS's
  convolutions and MatrixMultiplyA14X) are refused: what their offset and clamp arguments mean is not known
  well enough to be guessed.
- **Fragment barycentrics** (`air.barycentric_coord`, four debug and meshing shaders) and
  Apple's GPU tracepoint runtime (`libtracepoint_rt_ios`, thread-local globals) are refused.
- **Under QEMU** the driver runs on softpipe and the first frames wait on 30-second compiles; that is the
  emulator, not the phone, which compiles the same shaders in milliseconds (the costs above).

## What the next person with the phone can do in an afternoon

Everything that runs on the phone is built on Linux now, and every step below has been run here (the last
one under QEMU, on softpipe, which is the phone's path with the CPU in place of the Adreno):

1. The compiler library and the driver, for Darwin:
   `THEOS=… SDK=…/iPhoneOS16.5.sdk components/metal/tools/mk-mesa26-linux.sh` (out: `build/mesa26-linux/out/libhakios-air.dylib`,
   with the ir3 sampler fix of `components/metal/tools/mesa26/darwin.patch`) and `components/metal/tools/mk-metal-linux.sh`
   (out: `build/metal-linux/HakiosMetal.bundle`, `metal_props`, `metal_drv_test`).
2. The system image with what Metal needs: `METAL=1 python3 darwin/tools/mkimage26.py OUT.apfs` (the MPS and
   RenderBox metallibs, and the bundle's link to `/redwood/lib/HakiosMetal.bundle`), and the small image with the
   two built files in its `lib/` and `metal_props on` before launchd.
3. Boot with `--boot-arg gpu=1 --boot-arg metal=1 --boot-arg metal_profile=1`, the GPU firmware in place and the
   phone's package brought in before the kernel was built (`tools/device.py add NAME`). Confirm the GPU block
   starts on 12377 and read the temperature lines; `hakiosmetal[...]` lines say what the driver met, and
   `sample_every=10` with `userland/tools/samples.py --profile` says where a process spends its time in it.
4. Before that, on the build machine, what the phone's compiler makes of a library: `components/metal/tools/air/ir3sweep.sh`
   (and `-p` for a vertex and fragment pair), and `components/metal/tools/air/memcheck.sh` for the same under memcheck.
