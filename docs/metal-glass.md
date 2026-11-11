# Metal on the Adreno, iOS 27: Liquid Glass drawn by the GPU

Liquid Glass, CASDF shapes and the large lock-screen clock are drawn by CoreAnimation's **Metal** renderer, in
backboardd. iOS 27 has no OpenGL renderer left, and its software renderer leaves these effects out. That is what
`gpu=0` shows: the glass incomplete (`docs/ios26-boot.md`, item 10).

With `gpu=1 metal=1` and the pieces below, backboardd takes a Metal device of ours, and CoreAnimation draws
the screen on the Adreno.

## State (redwood, iOS 27.2 24B119)

Drawn by the GPU, as CoreAnimation means them to look:
- the lock screen with its glass clock;
- the home screen with its widgets and icons, the dock's and the search field's glass;
- a system alert's blurred glass over the home screen.

Four boots of 240–300 s ran with no crash of backboardd. The last one, with every fix below, had no refusal by
the plug-in before the run's end, except an accounting setter it takes without keeping.

Speed: the GPU works about 50 ms per command buffer on the glass screens (HakiosGPU's five-second lines). That
gives about 20 frames a second. With the software renderer the lock screen ran at 60, without the glass.

## The pieces

| Piece | Where | What it does |
| --- | --- | --- |
| GPU service | `kernel/overlay/iokit/Kernel/HakiosGPU.cpp`, `bsd/dev/hakios_adreno.c` | msm-shaped buffers, submissions and fences through IOKit; an IOSurface's memory or a program's own pages as a GPU buffer (requests 0x7e, 0x7d); recovery after a hang (`gpu_recover`) |
| The plug-in's name | `IOAcceleratorES` in `MiniIOMobileFramebuffer.cpp` | with `metal=1`: `MetalPluginName` = HakiosMetal, `MetalPluginClassName` = HakiosMetalDevice. User space may remove them (an empty string) to go back to the software renderer without a reboot |
| The driver | `metal/` (hakios-metal, with its history) | Metal's command model on the Adreno: blits, compute, the render pass, textures, IOSurface textures. Its tests run against a fake GPU; `make test` |
| The compiler | `metal/tools/mesa/air` and `metal/src/compiler.cpp` | Apple's AIR to NIR (our front end), NIR to ir3 (Mesa), inside the driver, at pipeline creation |
| The plug-in | `metal/plugin/HakiosMetalCA.m` | the `MTLDevice` and its objects as Metal's private base classes want them, over the driver; the one that serves CoreAnimation |
| Mesa for the phone | `metal/tools/mk-mesa.sh` | Mesa 25.0.7's compiler archives, cross-built arm64e |
| Mesa for this machine | `metal/tools/mk-mesa-host.sh` | the same with the front end, for `make test MESA=1` and `make sweep MESA=1` |
| The bundle | `ios/userland/metal.sh` | the plug-in over the driver and Mesa, arm64e, pacstrip, ldid → `$OBJ/HakiosMetal` |
| The images | `ios/userland/root26-ios.sh`, `ios/tools/mkimage26.py --metal` | the bundle in the small image (`/redwood/lib/HakiosMetal.bundle`); in the system image the link `/System/Library/Extensions/HakiosMetal.bundle` to it, and the system's `.metallib` files |

Only the window server is shown the device (the plug-in's default). `metal_all=1` shows it to every process.

## Building and booting

On top of the iOS 27 steps in `docs/ios26-boot.md`, with the kernel built:

```sh
bash ios/userland/metal.sh            # Mesa's compiler, the driver, the plug-in: $OBJ/HakiosMetal
bash ios/userland/root26-ios.sh       # the small image, with lib/HakiosMetal.bundle
LAUNCHDAEMONS="$IOS/jobs26" WORK="$IOS/image-tree-prune" \
    python3 ios/tools/mkimage26.py --prune --metal "$IOS/system27-metal.apfs"
python3 ios/tools/darwin-write.py plan "$IOS/system27-metal.apfs" 0
DARWIN_WRITE=yes python3 ios/tools/darwin-write.py write "$IOS/system27-metal.apfs" 0
./xl enter; ./xl ramdisk "$IOS/flashboot.apfs"
XL_RAMDISK="$IOS/root26-ios.apfs" XL_CMDLINE="gpu=1 metal=1 wifi=1" ./xl run
```

Notes:
- `--metal` keeps 280 Metal libraries, CoreAnimation's among them. That puts the image past fastboot's
  4 GiB - 1, so it is for the flash (`darwin-write.py`).
- A new plug-in needs only a new small image. The system image links to it and does not change.
- `RC_JOBS=ios/userland/rc.jobs.unlock` for `root26-ios.sh` swipes the lock screen away 100 s after the jobs
  start, which shows the home screen.

## Finding what is not served

**On the phone.** Every line of the plug-in starts `HakiosMetal[PROCESS PID]:`. Every selector or descriptor it
does not serve says `refused:` with the caller. Lines near the end of a run are not refusals of the plug-in's:
there the kernel answers its requests "not ready" because the run is ending (`--- loader_return_after expired ---`
in the log).

**On this machine.** The sweep compiles every function of a library, then lists the refusals by how often they
occur, in seconds:

```sh
metal/tools/mk-mesa-host.sh
make -C metal sweep MESA=1
metal/out/host-mesa/sweep "$IOS/rootfs/System/Library/Frameworks/QuartzCore.framework/default.metallib"
```

For iOS 27.2's QuartzCore (249 functions):

| Kind | Compiled |
| --- | --- |
| Vertex | 39 of 39 |
| Fragment | 154 of 156 |
| Kernel | 8 of 54 |

The 46 kernels left are tile shaders, which CoreAnimation asks only of a device that supports them. The two
fragment functions are the uber shaders `fixed_frag_lph_cph`/`_cpf`, whose pipelines specialise them first.

## What iOS 27 asked that 16.5 had not

Each item was found on the phone, in this order:

1. `-targetDeviceArchitecture` (Metal's loader asserts on nil): a `MTLTargetDeviceArchitecture` of cpu type 0, so
   no precompiled slice is ours and every pipeline is compiled from AIR.
2. `-setResponsibleTaskIDs:count:` on every command buffer, and `-setResponsibleProcess:` on buffers. These are
   accounting setters that no protocol declares. Setters like these are now taken and logged instead of raising.
3. Texture usage `0x10005`: the private hints 0x10000/0x20000. They change nothing for a linear texture and are
   dropped.
4. `air.get_read_sampler()`: a texel read takes a sampler and an offset (16 functions, `compute_average_luma`
   among them). Also `llvm.umax` and its kin (2 functions).
5. 48 bytes of constants bound at the last 48 of a page, for a range ir3 pads to 64. The driver now loads such a
   range itself, from the processor, instead of refusing the draw.

## What is next

- **Speed.** On the lock screen a command buffer takes about 37 ms of GPU time, measured with `metal_fps=draws`.
  One full-screen pass, 90 draws in 56 tiles of GMEM, takes 32 ms of it. Its cost is iOS 27's path renderer,
  which 16.5 did not have:
  - `path_cubic_*` draws a path's coverage into a float render target (up to 1696 instances a draw);
  - `path_blit_frag` reads that coverage back by framebuffer fetch and composites it;
  - `path_clear_frag` resets the target.

  Kept in system memory (`metal_gmem_draws=1000`), the same pass takes 95 ms. There `path_blit` costs 5.2 ms a draw
  and `path_clear` 1.6 ms, about 12 times per buffer, so GMEM is already worth 3 times. What is left is the fetch
  barrier, paid per tile per path. `SINGLE_PRIM_MODE` instead of it broke SpringBoard on 16.5. That is the place
  to measure next, with the probe.
- **Tile shaders** stay refused while `supportsTileShaders` is NO, as it is.
- **Other phones' GPUs.** The driver's tables are the A635's (chip 0x06030500, redwood's Adreno 642L). It
  refuses another chip by name rather than hang it (`hakios_metal_chip_known`). Renoir's Adreno 642
  (0x06060010) needs its registers checked against `a6xx.hpp` before it is let through.

## The first generation

`userland/userland/src/hakiosmetal.m` and `userland/tools/mesa/air` are the Gallium-based plug-in and its front end
of the iOS 16/26 work. That plug-in found most of what iOS 26 asks (items 1, 2 and 4 above came from it). Neither
is built for iOS 27. The glass validation tools are with them: `userland/tools/air/glassref.m`, `glasscase.c`.
