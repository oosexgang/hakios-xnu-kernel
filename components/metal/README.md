# Metal

Apple's Metal on the phone's Adreno: a device of Metal's own kind that Darwin's Metal framework finds and
loads, and a shader compiler that takes Apple's compiled shaders (AIR, LLVM bitcode in `.metallib` files)
through Mesa's NIR to the Adreno's ir3. With it, QuartzCore draws Darwin 26/27's interface, Liquid Glass
included, on our GPU.

| Where | What |
|---|---|
| `src/metal_driver.m` | The Metal device (`HakiosMetal.bundle`): `_MTLDevice` and the classes around it -- buffers, textures, samplers, render and compute pipelines and encoders, argument buffers, the command queue. It compiles and draws through `libhakios-air` (below). |
| `src/metal_plugin.plist` | The bundle's `Info.plist`, which Metal's discovery reads. |
| `src/metal_props.c` | Puts Metal's two discovery properties on the kernel's `IOAcceleratorES` (boot-arg `metal=1`). |
| `src/metal_drv_test.m`, `metal_test.m`, `metal_caps.m`, `metal_dev.m`, `metal_stubdev.m` | Test programs: the driver in a process of its own, what Darwin believes about Metal, its capabilities, Apple's own device's setup. |
| `src/rw_null.ll` | The driver's stand-in fragment shader, as AIR (`rw_null.metal` its source). |
| `tools/mesa/air/` | The AIR front end for Mesa: reader, AIR to NIR, and `air_metal.c`, the library the driver calls (`air_metal.h`). |
| `tools/mesa/*.patch`, `tools/mesa26/darwin.patch` | Mesa itself, made to build for Darwin and to take the front end; `tools/mesa/darwin-drm/` stands in for libdrm over the kernel's GPU driver. |
| `tools/mk-mesa26-linux.sh` | `libhakios-air.dylib` (Mesa 26.2.5 with the front end) and `air_run`, for Darwin, built on Linux. `mk-mesa26.sh` the same on a Mac. |
| `tools/mk-metal-linux.sh` | `HakiosMetal.bundle`, `metal_props` and `metal_drv_test`, for Darwin, built on Linux. |
| `tools/mk-mesa.sh` | Mesa's OpenGL ES (OSMesa) for the userland's EAGL (`userland/userland/src/eagl.c`). |
| `tools/air/` | The front end's tests on this machine (`mk-host.sh` builds Mesa for it): each `*check.sh` and `*check.py` a feature drawn or computed on softpipe and compared, `ir3sweep.sh` and `softpipesweep.sh` every function of a library through ir3 or softpipe, `check.sh` all of them over the corpus. |
| `tools/metallib.py` | Takes a `.metallib` apart: its function list and each function's bitcode. |
| `tools/metal/` | What Darwin's programs ask of Metal: the classes and methods they use, against what the driver has. |
| `docs/air.md` | The front end: the formats, what each part does, how it is tested. `air-report.md` the last run of `check.sh`. |
| `plugin/` | The second Metal device (below): its A6xx library, the plug-in CoreAnimation draws through, its tests against a fake GPU. Built with its own `Makefile` (`make -C plugin test`). |
| `build/` | What the scripts build (not kept). |

The scripts are run from here (`tools/air/check.sh`, `tools/mk-metal-linux.sh`); each finds this directory from
its own place.

## In the kernel

[`kernel/overlay/iokit/Kernel/BoardGPU.cpp`](../../kernel/overlay/iokit/Kernel/BoardGPU.cpp) is the GPU's
IOKit service (`IOAcceleratorES`, the user client Mesa's `darwin-drm` talks to);
[`kernel/overlay/bsd/dev/board/adreno.c`](../../kernel/overlay/bsd/dev/board/adreno.c) the Adreno itself:
its power, clocks, ring buffer and memory.

## The other Metal

[`plugin/`](plugin) is a second Metal device, written apart from this one: a library
of its own for the A6xx command stream with a QuartzCore plug-in over it
([`Documentation/metal-plugin.md`](../../Documentation/metal-plugin.md)).

## Where it stands

[`Documentation/metal-glass.md`](../../Documentation/metal-glass.md): what draws, what does not yet, and how it was measured.
