# Metal on the Adreno

Two implementations of Metal draw the window server on the phone's Adreno 642L. This page says what each is, how to build, run and
measure it, and what is not known. The numbers: [Metal measurements](metal-measurements.md). How it got here, step by step:
[the plan](../metal-plan.md).

**Status, POCO X5 Pro 5G, 3 and 4 November 2026.** Both paths draw the system correctly. hakios-metal is the faster: page swipes 100
to 104 frames a second, Settings' scroll about 90, the home button about 90 at the full 1125x2436; rendering at 750x1624, stretched to the panel (`fbscale=2`, the default of `tools/metal-run.sh`), scroll 118 to 120, swipes 107 to 118, home button 95 to 111, about 110 over the bench as a whole. metal2vulkan: swipes 50 to 57, scroll 41. **120 frames a second has not been
reached on either.** Neither is on by default: without `metal=1` the window server draws through OpenGL ES, as it always has.


## The two paths

| | hakios-metal | metal2vulkan |
|---|---|---|
| Route | Metal, `HakiosMetalCA`, hakios-metal (the Adreno's own PM4 packets), HakiosGPU | Metal, AdrenoMetal's frontend, metal2vulkan, Turnip, `hakios-drm`, HakiosGPU |
| Shaders | AIR to NIR to ir3, by Mesa's compiler inside the process | AIR through metal2vulkan into Vulkan pipelines, which Turnip compiles |
| Where the code is | `userland/src/HakiosMetalCA.m` and `hakios-metal/` here | `tools/mk-turnip.sh`, `tools/mk-m2v.sh`, `tools/m2v/`, `tools/mesa/`; the rest fetched by those scripts |
| Whose | ours; the plug-in began as vcvkk's (origin **to verify**, [third-party licenses](../../THIRD_PARTY_LICENSES.md)) | [UniXNU](https://github.com/UniXNU/UniXNU)'s stack, ported from the Adreno 740 and a later system |
| Build | `METAL_PLUGIN=ca` | `METAL_PLUGIN=ca TURNIP=1 M2V=1` |
| Boot arguments | `gpu=1 metal=1 ca_metallib=...` | the same and `metal_m2v=1` |
| Page swipe, frames a second | 100 to 104 | 50 to 57 |
| GPU time a frame | about 10 ms at the home screen | 14 to 21 ms |
| In `prebuilt/` | no | no, and it must not be without the check below ([Licenses](#licenses)) |

The third path is the default one, and it is not Metal: CoreAnimation's OpenGL ES renderer through Mesa's freedreno (`gpu=1`, an image
built with `GL=1`).

## Interfaces

### HakiosGPU (`drivers/gpu/hakios.gpu.cpp`)

The kernel's GPU driver serves all three paths through the same IOKit user client. Its requests are Linux's msm DRM requests by
number, and two of its own:

| Request | Takes | Does |
|---|---|---|
| `0x7e` | an IOSurface's ID | a buffer over the surface; asked with `kHakiosDescribe`, only what it would be like |
| `0x7d` | an address and a size in whole pages of the caller's own (`struct hakios_user_memory`) | a buffer over those pages, Linux's userptr. Wired through an `IOMemoryDescriptor` over the caller's task for as long as the buffer lives, entered into the GPU's address space, pushed out of the CPU's cache by physical address at every submission naming them. Read-only pages are taken as such: a window server's view of another program's backing store. Unwired when the buffer goes, and never kept for reuse. Each refusal is logged with its reason |

### Turnip on Darwin (`tools/mesa/turnip-darwin.patch`, `tools/mesa/hakios-drm/`)

Mesa 25.0.7's Vulkan driver, cross-built with its msm backend and no DRM window system.

- `hakios-drm` stands in for libdrm. Requests become calls of HakiosGPU's user client. DRM syncobjs, which Turnip makes its fences and
  semaphores of, are kept in the process over the kernel's fences: timelines, waits, signal, query, transfer. There is one GPU and
  one connection in a process, which is why that is enough.
- `VK_EXT_external_memory_host`: an IOSurface's pointer is imported by the surface's ID (the frontend says which surface is mapped
  where, `hakios_drm_host_surface`), any other page-aligned pointer through request `0x7d`.
- `VK_NV_linear_color_attachment`: linear tiling already had every feature of optimal tiling in Turnip; the extension says so.
- `vkGetInstanceProcAddr` is exported: there is no Vulkan loader, and metal2vulkan opens the driver by that entry point.
- The cache line size comes from `hw.cachelinesize`, since `CTR_EL0` traps in user space; buffers are mapped through `hakios_drm_map`.

### The window server (`userland/src/HakiosMetalCA.m`)

QuartzCore on this system has its uber shaders only as specialised variants inside `default.pipelinelib`, and no `default.metallib` at
all. Two things follow, on either path:

- `tools/ca-metallib.py` makes the `default.metallib` the window server asks for, from QuartzCore's own variants and a later
  CoreAnimation's library that you supply. It stays on your machine, and `ca_metallib=` names it at boot.
- The plug-in picks the variant by the pipeline's label. For metal2vulkan that goes through `native_bridge_metal_variant_hook`, which
  `tools/m2v/adrenometal-ios16.patch` adds to the frontend. `HakiosMetalCA` also gives the metal2vulkan device its own opening of
  QuartzCore's pipeline library and a `targetDeviceInfo`: without them the pictures came out as grey shapes.

## Build

Everything runs on Linux or in WSL 2 (commands for Windows: [Windows](../windows.md#metal-a-run-measured-from-windows)). Neither path
works from `prebuilt/` alone: `PREBUILT=1` compiles nothing, and `prebuilt/userland` has no Metal plug-in, so these need the cross
toolchain of `xnu-deps/setup.sh`.

The library the window server asks for, for either path:

```sh
CA_QC_METALLIB=/path/to/default.metallib tools/ca-metallib.py ~/ca-share    # off a Mac; on a Mac it reads the Mac's own
```

**hakios-metal.** It is in this tree, in `hakios-metal/`; `HAKIOS_METAL=` names another copy:

```sh
(cd hakios-metal && make ios MESA=1)         # needs tools/mk-mesa.sh's tree
METAL_PLUGIN=ca RC_JOBS=rc.jobs.bench EXTRA_SHARE=~/ca-share REST=1 SYSTEM=1 userland/build.sh
```

**metal2vulkan.** Nothing of it is in `prebuilt/`: you fetch and build it yourself. You need what `tools/mk-mesa.sh` needs,
`glslangValidator` (apt: `glslang-tools`), rustup's cargo with Rust 1.98 and its `aarch64-apple-ios` target, `llvm-as` of LLVM 21
(apt: `llvm-21`), clang, and the theos iPhoneOS 16.5 SDK.

```sh
tools/mk-mesa.sh      # Mesa 25.0.7 with tools/mesa/*.patch
tools/mk-turnip.sh    # xnu-deps/build/mesa/out/libvulkan_freedreno.dylib
tools/mk-m2v.sh       # xnu-deps/build/m2v/out/: frontend.o, libadreno_metal_bridge.a
METAL_PLUGIN=ca TURNIP=1 M2V=1 RC_JOBS=rc.jobs.bench EXTRA_SHARE=~/ca-share REST=1 SYSTEM=1 userland/build.sh
```

Build these before the image: `userland/build.sh` links metal2vulkan into the plug-in only if `xnu-deps/build/m2v/out` is already there.
`tools/mk-m2v.sh` pins [UniXNU/AdrenoMetal](https://github.com/UniXNU/AdrenoMetal) at `243d63c` and
[steelbrain/metal2vulkan](https://github.com/steelbrain/metal2vulkan) at `624bab3` with AdrenoMetal's 30 patches, and adds
`tools/m2v/`. Programs and libraries over 4 MB go into the system image, with a link in the small one (`/usr/local/redwood`).

Two programs check the path before the window server is involved:

| Image | Boot | The program | On the phone (3 November) |
|---|---|---|---|
| `TURNIP=1 RC_JOBS=rc.jobs.vktest` | `gpu=1` | `userland/src/vk_test.c`: Turnip once round | `Adreno 7c+ Gen 3`, Vulkan 1.3.305, a 4 MB `vkCmdFillBuffer` checked word by word, a submission and its fence wait 0.5 ms at best, 2.0 on average: PASS |
| `TURNIP=1 M2V=1 RC_JOBS=rc.jobs.m2vtest` | `gpu=1` | `userland/src/m2v_test.m`: the Metal device, a blit, a clear, surfaces and own pages as textures | the device in 134 ms, a 4 MB blit fill and a 1080 x 2400 clear checked word by word and pixel by pixel, a full-screen clear committed and waited for in 1.08 ms: PASS |

## Boot arguments

Every argument is in [the catalogue](../boot-args.md), which is made from the source; this table says what the ones of Metal do.

| Argument | Layer | What it does |
|---|---|---|
| `metal=1` | kernel, plug-in | the accelerator names the Metal plug-in; without it the window server finds no device and stays on OpenGL ES, saying nothing. With `METAL_PLUGIN=ca` the window server alone is shown the device (`metal_all=1`: every process) |
| `ca_metallib=PATH` | `inject.c` | QuartzCore's library for the window server |
| `metal_m2v=1` | plug-in | the window server's device is metal2vulkan's. Needs an image built with `M2V=1` |
| `tu_debug=FLAGS` | plug-in | Turnip's `TU_DEBUG` in the window server (`sysmem`, `gmem`, `perf`, ...); with `metal_m2v=1` |
| `m2v_utrace=1` | plug-in | Turnip's GPU trace of every render pass into a file; the 30 commonest events of each 5 s printed. With `metal_m2v=1` |
| `metal_fps=1` | plug-in | frame and pass counters every 5 s: what `tools/metal-run.sh` sets |
| `metal_fps=passes` | plug-in | also every pass of one slow command buffer in 200, with its attached and touched colour attachments |
| `metal_fps=mainpass` | plug-in | also the draws of a pass of 40 draws or more over 4 ms (in sysmem, where draws are timed) |
| `metal_fps=draws` | plug-in | also each draw of a sysmem pass timed alone, and the time counted by program |
| `metal_defer=1` | plug-in | the window server's command buffers recorded on a thread of hakios-metal's own: a draw costs its render thread about 5 us instead of about 40. `tools/metal-run.sh` sets it |
| `metal_luma_boost=1` | plug-in | draws `luma_boost`, which is otherwise left out where the library has its own |
| `wallpaper_cache=1` | SpringBoard | the wallpaper's window rasterized by the window server |
| `wallpaper_freeze=1` | SpringBoard | the wallpaper as one still picture: at the home screen the window of the icons is hidden for a moment (they blink once a boot), the pixels of the IOSurface on the panel (`kern.hakios.fb_surface`) become one opaque view in the wallpaper's window, and its layers are hidden. It gives up: the wallpaper does not change until the next boot, and the lock screen shows the same picture. `tools/metal-run.sh` sets it |
| `wallpaper_freeze=2` | SpringBoard | the wallpaper hidden: what it costs, measured |
| `ui_lite=1`, `ui_lite=0` | SpringBoard | Reduce Transparency and Reduce Motion on, off again. Without it the settings are left alone |
| `fb_intf=` | framebuffer | the physical address of the display interface's registers, for a port: `fb_beam` and `fb_measure` read them. Without it they are known for sc7280 only (`CONFIG_SOC`); on another SoC nothing is read and frames stay on their beat |
| `fbwidth=`, `fbheight=`, `fbscale=`, `fbsubtype=` | framebuffer | another iPhone's screen class: `fbscale=2` alone is the iPhone X's layout at 750x1624 (the default of `tools/metal-run.sh`); `fbwidth=828 fbheight=1792 fbscale=2 fbsubtype=1792` is the XR's |
| `touch_poll_us=N` | touch driver | the touch screen read every N microseconds. The driver's default is 8000; `tools/metal-run.sh` sets 4000 |
| `fb_hz=120`, `fb_threads=6`, `fb_measure=1` | framebuffer | the driver's beat at the panel's rate, six processors scaling a frame onto the panel, the panel's own rate measured once |
| `fb_beam=1` | framebuffer | the default: each thread's rows are written behind the panel's scan line. `fb_beam=0`: whenever the beat comes, as before. [Tearing](metal-measurements.md#tearing) |

And one sysctl: `kern.hakios.fb_surface`, the IOSurface last put on the panel.

## Measure

```sh
tools/metal-run.sh                          # 180 s, hakios-metal; the summary in logs/<time>-metal.txt
MORE="metal_m2v=1" tools/metal-run.sh       # the same on metal2vulkan
MORE="wallpaper_cache=1" tools/metal-run.sh # any argument beside the defaults
```

An image built with `RC_JOBS=rc.jobs.bench` unlocks, waits a minute for the start-up work to end, and does the same round four times:
Settings opened from its icon, its list scrolled, the home button, a page swipe there and back. So each kind of motion is measured
four times in the same state of the system, and runs can be compared.

`tools/metal-run.sh` takes the phone from Android, its bootloader or the loader, uploads the system image, runs, and writes a summary:
`tools/frames.py`'s frames per gesture, the plug-in's `fps:` lines, what was refused, the frames on the panel, the shader cache and any
crash. It sets `gpu=1 metal=1 metal_fps=1 touch_lat=1 touchscreen=1 gpu_recover=1 anim_max=1 bootcpu=7 cpus=8 cpufreq=1 fb_hz=120
fb_measure=1 fb_threads=6 touch_poll_us=4000 wallpaper_freeze=1 metal_defer=1`, the path of QuartzCore's library, and (unless `PERSIST=0`) `ufs=2 ufs_write=1
var_store=1`, which keeps `/private/var` in your `darwin` partition so that shader stages are compiled once.

`MORE=` adds arguments at the end, and a default of the same name goes: `MORE="wallpaper_freeze=0 metal_defer=0"` runs without those two.

`tools/frames.py RUN.log [MORE.log ...]` on its own reads the `tl:` lines of `touch_lat=1`. For each gesture it gives the frames a
second while frames came, the share of frame steps over 16.7 ms, 33 ms and 8.3 ms, and where a slow frame's time went. A window server
drawing through Metal says no `tl: g`: its frames are counted by the swap that handed each over, so `draw` and `idle` are 0 there,
meaning unknown.

## Known gaps

- **Under 120.** While a finger drags, the window server waits 8 to 14 ms on GPU fences and frames come at 75 to 80 a second; after
  the finger lifts, 185 of 205 frames of a swipe take 8.3 ms. The cause of the wait is not established.
- **The wallpaper is frozen to get the home button's rate.** 92 frames a second with `wallpaper_freeze=1`, 50 without it or
  `wallpaper_cache=1`, 28 with `wallpaper_cache=1`. It is many full-screen layers from another process, and rasterizing its window does
  not stop them being composed again. A wallpaper that moves or changes costs that, and it is not solved.
- **metal2vulkan renders each frame to a tiled image and merges it back with a compute pass.** The linear attachment path stays unused
  because a second, non-linear attachment is in the pass, and the extension wants every colour attachment linear.
- **Tearing.** The panel reads the one buffer the driver writes. `fb_beam` writes behind the scan; 2 to 16 frames in 5 s still miss
  their scan, and whether the panel tears less is so far for the eye: [Tearing](metal-measurements.md#tearing).
- **Not the same stack as UniXNU's.** Theirs runs on the Adreno 740 with its own kernel driver, [Adreno740](https://github.com/UniXNU/UniXNU/blob/main/docs/drivers/adreno740.md),
  and a newer system. Only the part above that driver was taken. What is measured here says nothing about theirs.
- **Other systems' libraries.** The window server's library takes the rest of its shaders from a later CoreAnimation than this
  system's, because this system has none. That it behaves the same as a matched one is not shown.
- **Shaders chosen by their labels.** This system's QuartzCore holds its uber shaders as 451 specialised variants. A pipeline CoreAnimation
  makes at run time that is not among them gets the nearest of the same kind (`hm_ca_nearest()`, `HakiosMetalCA.m`): sampling, blend and
  varyings the same, then the fewest inputs changed. Before that it got one fixed variant, which was the red top of the default wallpaper,
  the white band behind the status bar and the opaque Weather widget. What the letters of a label mean is read from the labels, so a
  stand-in can differ in a flag; the log says each (`... stands in for it`).

## What is not verified

- The results are of one phone, a POCO X5 Pro 5G. No other Adreno has run either path, and QEMU has no GPU.
- They are of the bench's runs, 180 s by default. They do not say how the rates hold over a longer run or with a hot phone, and they
  take the GPU's clock as the 550 MHz it is set to, not as read back frame by frame.
- The only client they describe is the system's own interface. Nothing here shows that a Metal program in general runs, and games
  are not claimed.
- The build commands above were written from the scripts and were not re-run on a clean machine when this page was written.
- HakiosGPU's request `0x7d` lets a client wire its own pages. It has not been through a [driver review](../driver-review/gpu-display.md).
- The frame rates are what `tools/frames.py` counts from the window server's swaps and the driver's frames, not a camera on the panel.

## Licenses

metal2vulkan is `GPL-2.0-or-later OR LGPL-3.0-or-later`. `M2V=1` links its runtime statically into the plug-in and `m2v_test`, so a
binary built that way has obligations whichever you choose. Whether and how such a binary may be distributed is **to verify**, and none
is distributed: [third-party licenses](../../THIRD_PARTY_LICENSES.md), [compliance](../../COMPLIANCE.md).
