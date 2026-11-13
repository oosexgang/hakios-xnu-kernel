# Metal for Liquid Glass on Renoir (Darwin 27): state and plan

Liquid Glass, CASDF shapes and the large lock-screen clock are drawn by CoreAnimation's **Metal** renderer, in
backboardd. Darwin 27 has no OpenGL renderer left, and its software renderer skips these effects. That is why the
current `gpu=0` configuration shows them incomplete (`docs/ios26-boot.md`, item 10). The glass needs three
things, all of them device-independent apart from the kernel's GPU profile:
- a Metal device for backboardd;
- a compiler from Apple's shader bitcode (AIR) to the Adreno;
- a kernel path that runs the work.

## What already exists in this repository

| Piece | Where | State |
| --- | --- | --- |
| Adreno 642 bring-up, Renoir profile (chip 0x06060010, 285–490 MHz, zap shader via TrustZone) | `kernel/overlay/bsd/dev/hakios_adreno.c`; the profile is `gpu { }` of the renoir package's board file (`renoir/board.dts` on the `devices` branch) | `gpu=1` started it on 2026-11-07: GPU clock measured at 490,001,684 Hz |
| GPU service for user space (msm-shaped GEM/submit through IOKit) | `iokit/Kernel/HakiosGPU.cpp` | Mesa's shim and the Metal device both use it |
| AIR → NIR → ir3 compiler (Mesa frontend) | `userland/tools/mesa/air/`, `air-*.patch` | 321/321 entry points of the Darwin 16 corpus compile; 255 numerically identical to LLVM; 30 ran on an Adreno (`userland/docs/air.md`) |
| Metal device plug-in (`_MTLDevice` subclass, buffers, textures, render/compute, depth/stencil, argument buffers) | `userland/userland/src/hakiosmetal.m` | Written and measured against Darwin 16 and 26's Metal; not packaged for Darwin 27 |
| Liquid Glass validation: QuartzCore's own pipelines drawn by Apple's GPU and by ours, compared pixel by pixel | `userland/tools/air/glassref.m`, `glasscase.c`, `glasscases.sh` | Built for Darwin 26's `default.metallib`; needs a Mac with an Apple GPU to record cases |
| MetalPerformanceShaders for CA's blurs (`productName` = "Paravirtual device") | `hakiosmetal.m` | Darwin 26 |

## What stands between that and glass on Darwin 27, in order

1. **Kernel, `gpu=1` on the 12377 kernel.** Boot with `--boot-arg gpu=1` and the phone's GPU firmware in the small image, and check
   that `HakiosGPU` publishes and that a submit retires.
   Heat: at 490 MHz the GPU sensor read 69.1 °C (`ios26-boot.md`, item 14). hxff's thermal work has to cap the
   GPU's OPP (the profile's 285/365/443/490 table) before long runs. Until then, `adreno_speed=N` (an index into
   that table, 1 and up) is the safe start.
2. **Mesa for arm64e Darwin 27.** `ios/userland/mesa26e.sh` builds OSMesa+freedreno for the GL shim. Extend it
   to build `libhakios-air.dylib` (Mesa with `air-frontend.patch`, `air-ir3.patch` and `air-metal.patch`,
   `air_metal.h` contract), then run it through pacstrip and `ldid -Cadhoc` like the other libraries
   (root26-ios.sh).
3. **Metal device on Darwin 27.**
   - Diff Metal's requirements between 26 and 27 with the existing tools (`userland/tools/metal/owes.py`,
     `refusals.py`, `protometh.py` against Darwin 27's Metal): `MTLAddDevice` asserts on `MTLDevice`/`MTLDeviceSPI`
     conformance and four methods, and Darwin 27 may add required SPI.
   - Rebuild `hakiosmetal.m` for 24B119 and put it in the root image.
   - Publish the plug-in name on the accelerator service. On Darwin 16.5 that was the only thing keeping Metal from
     finding a driver of ours. The 12377 cleanup had removed it; `IOAcceleratorES::setProperties` in `kernel/overlay/iokit/Kernel/MiniIOMobileFramebuffer.cpp`
     restores it. With boot-arg `metal=1`, a user process may set `MetalPluginName`/`MetalPluginClassName` on
     `IOAcceleratorES`; nothing else is accepted, and an empty string removes them. Without it (the default),
     CoreAnimation keeps its software renderer, as in the release.
   - Keep the system image's QuartzCore `default.metallib` (mkimage26 `KEPT_FILES` already keeps it) and its
     `default.pipelinelib`. The plug-in's `newLibraryWithFile` path was the first crash on 26 (`hakiosmetal.m`).
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

## Why it was not done in this pass

Each of steps 2–6 needs at least one thing this environment does not have:
- Darwin 27's own files: the IPSW and its `default.metallib`. Apple's files are local build inputs, never repository
  content.
- A Mesa build: it needs a third-party toolchain, and this session may not build or run external code.
- The phone, or a Mac with an Apple GPU for the reference cases.

The brightness work, by contrast, was finished and tested here (`docs/backlight.md`). It is independent of
the GPU: under Metal the `dcs` backend is unchanged, and the `soft` backend dims whatever the framebuffer copies.

## What the next person with the phone can do in an afternoon

1. `python3 ios/tools/boot26.py --wifi-mac … --boot-arg gpu=1 --return-after 300`, with the GPU firmware in
   place and the phone's package brought in before the kernel was built (`tools/device.py add NAME`).
   Confirm the GPU block starts on 12377, and read the temperature lines.
2. On the build machine: `userland/tools/metallib.py` on Darwin 27's `QuartzCore.framework/default.metallib`, then
   `userland/tools/air/check.sh`. The number of Darwin 27 pipelines our compiler accepts tells how far step 4 is.
