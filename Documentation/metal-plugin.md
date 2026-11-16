# Metal on the Adreno, Darwin 27: Liquid Glass drawn by the GPU (the hakios-metal driver)

Liquid Glass, CASDF shapes and the large lock-screen clock are drawn by CoreAnimation's **Metal** renderer, in
backboardd. iOS 27 has no OpenGL renderer left, and its software renderer leaves these effects out. That is what
`gpu=0` shows: the glass incomplete (`Documentation/darwin26-boot.md`, item 10).

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
| GPU service | `drivers/iokit/BoardGPU.cpp`, `bsd/dev/board/adreno.c` | msm-shaped buffers, submissions and fences through IOKit; an IOSurface's memory or a program's own pages as a GPU buffer (requests 0x7e, 0x7d); recovery after a hang (`gpu_recover`) |
| The plug-in's name | `IOAcceleratorES` in `MiniIOMobileFramebuffer.cpp` | with `metal=1`: `MetalPluginName` = HakiosMetal, `MetalPluginClassName` = HakiosMetalDevice. User space may remove them (an empty string) to go back to the software renderer without a reboot |
| The driver | `components/metal/plugin/` (hakios-metal, with its history) | Metal's command model on the Adreno: blits, compute, the render pass, textures, IOSurface textures. Its tests run against a fake GPU; `make test` |
| The compiler | `components/metal/plugin/tools/mesa/air` and `components/metal/plugin/src/compiler.cpp` | Apple's AIR to NIR (our front end), NIR to ir3 (Mesa), inside the driver, at pipeline creation |
| The plug-in | `components/metal/plugin/plugin/HakiosMetalCA.m` | the `MTLDevice` and its objects as Metal's private base classes want them, over the driver; the one that serves CoreAnimation |
| Mesa for the phone | `components/metal/plugin/tools/mk-mesa.sh` | Mesa 25.0.7's compiler archives, cross-built arm64e |
| Mesa for this machine | `components/metal/plugin/tools/mk-mesa-host.sh` | the same with the front end, for `make test MESA=1` and `make sweep MESA=1` |
| The bundle | `darwin/userland/metal.sh` | the plug-in over the driver and Mesa, arm64e, pacstrip, ldid → `$OBJ/HakiosMetal` |
| The images | `darwin/userland/root26-darwin.sh`, `darwin/tools/mkimage26.py --metal` | the bundle in the small image (`/hakios/lib/HakiosMetal.bundle`); in the system image the link `/System/Library/Extensions/HakiosMetal.bundle` to it, and the system's `.metallib` files |

Only the window server is shown the device (the plug-in's default). `metal_all=1` shows it to every process.

## Building and booting

On top of the iOS 27 steps in `Documentation/darwin26-boot.md`, with the kernel built:

```sh
bash darwin/userland/metal.sh            # Mesa's compiler, the driver, the plug-in: $OBJ/HakiosMetal
bash darwin/userland/root26-darwin.sh       # the small image, with lib/HakiosMetal.bundle
LAUNCHDAEMONS="$IOS/jobs26" WORK="$IOS/image-tree-prune" \
    python3 darwin/tools/mkimage26.py --prune --metal "$IOS/system27-metal.apfs"
python3 darwin/tools/darwin-write.py plan "$IOS/system27-metal.apfs" 0
DARWIN_WRITE=yes python3 darwin/tools/darwin-write.py write "$IOS/system27-metal.apfs" 0
./bench enter; ./bench ramdisk "$IOS/flashboot.apfs"
BENCH_RAMDISK="$IOS/root26-darwin.apfs" BENCH_CMDLINE="gpu=1 metal=1 wifi=1" ./bench run
```

Notes:
- `--metal` keeps 280 Metal libraries, CoreAnimation's among them. That puts the image past fastboot's
  4 GiB - 1, so it is for the flash (`darwin-write.py`).
- A new plug-in needs only a new small image. The system image links to it and does not change.
- `RC_JOBS=darwin/userland/rc.jobs.unlock` for `root26-darwin.sh` swipes the lock screen away 100 s after the jobs
  start, which shows the home screen.

## Finding what is not served

**On the phone.** Every line of the plug-in starts `HakiosMetal[PROCESS PID]:`. Every selector or descriptor it
does not serve says `refused:` with the caller. Lines near the end of a run are not refusals of the plug-in's:
there the kernel answers its requests "not ready" because the run is ending (`--- loader_return_after expired ---`
in the log).

**On this machine.** The sweep compiles every function of a library, then lists the refusals by how often they
occur, in seconds:

```sh
components/metal/plugin/tools/mk-mesa-host.sh
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

## Where the time went

What made the frames faster, in the order it was found (each commit has its measurements):

1. **The path renderer** (iOS 27's, not in 16.5): its coverage mask blitted and cleared over whole bands of the
   screen for every path. `ca_paths.c` keeps a map of where the mask can be non-zero and cuts `path_blit` and
   `path_clear` to it. The default wallpaper's six stripes are rasterized once
   (`components/wallpaper/inject_wallpaper.m`) rather than drawn by the path renderer every frame.
2. **The driver's per-draw cost**: small bytes in an arena of the command buffer, CoreAnimation's buffers read
   with `memcpy` from write-combined memory, each thread its own compiler.
3. **GMEM**: a fetch barrier as turnip's; fetching passes weigh toward GMEM; an attachment the draws do not touch
   has no bin; memoryless attachments are neither stored nor made anew each frame.
4. **Specialisation**: while a pipeline's uber shader is specialised for its buffer, CoreAnimation draws with the
   uber shader itself, 17.7 ms for one full-screen draw. A specialisation took 1.3–6 s on the phone; now about
   0.3 s (the folded branches taken away early, unread scratch dropped, the front end's NIR kept per module).
5. **Binning** in GMEM passes (the hardware gives each tile only the draws that reach it), redwood's default;
   a pass tiled over a corner of the screen is not binned, which had left the low-battery alert out of frames.
6. **One frame at a time no more.** The window server wraps its display's page in a texture anew every frame;
   the plug-in opened the surface's buffer in the kernel and closed it after the frame, and the close waited for
   the GPU's lock with the next frame's wrap waiting behind it. The last four surface buffers are kept now
   (found with `waitstack=1`: the render thread's waits, sampled in backboardd).

To measure: `metal_fps=1` (every two seconds), `metal_fps=timeline` (a heavy frame's passes), `metal_fps=draws`
(each draw of a sysmem pass timed), `gpu_log=1 touch_lat=1` with `frames.py`. To reproduce a specialisation on
this machine: `metal_air_dump=1`, `components/metal/plugin/tools/airdump.py`, `sweep -a MODULE.air -k 0=HEX -c ...`
(`HAKIOS_METAL_PHASES=1` times each phase).

## What is next

- **Opening and closing an app.** The rasterized wallpaper is drawn again into its 896x1856 texture, by the path
  renderer, in a pass of a handful of draws: 293 ms when it went to sysmem.
- **The small cores.** This kernel's scheduler (Clutch, without SMT) puts the window server's threads on the
  Cortex-A55s as often as on the A78s; with `cpus=4` (the big cores alone) the home screen's pages ran at 51.5
  frames a second against 47.8, with a third of the long gaps.
- **120 Hz.** The panel runs at 120 Hz (`display_probe=1`: 2432 lines at 119.926 Hz) and the window server is
  told so (`fb_hz=120`, redwood's default). 16.5 reached 100–118 frames a second. What stands in the way now is the
  presenter's copy of each frame to the panel: scaled 828x1792 to 1080x2400 on the processor, in stripes behind
  the panel's readout so that no picture tears, it takes 10.7–11 ms a frame however few frames come -- a ceiling
  near 92 a second; at 69 a second 271 of 346 swaps waited for the frame before. Past it: the display block
  scanning the surface out itself, scaled by its own scaler, with no copy. (`bench screenshot` reads the panel's
  memory while a copy writes it, so a screenshot in motion holds rows of two frames; the recording does not.)
- **Compiled stages across boots.** `/private/var` starts empty at every boot of iOS 27 here. The stages a run
  compiled are taken off the phone over SSH (`darwin/tools/metal-cache-pull.sh`) into the next small image, where
  the plug-in reads them after its own cache (`/hakios/share/hakios-metal-cache`).
- **Other processes.** Only the window server is shown the device (`metal_procs=NAME,NAME` shows it to others).
  iconservicesagent draws iOS 26's icons with RenderBox, SwiftUI's renderer, and downsamples them with CoreImage.
  With `metal_procs=iconservicesagent` every pipeline it asks for now compiles: its custom effects link the visible
  functions they call (`custom_fn`, through the pipeline's linked functions), and CoreImage's stitched kernels
  (`ci_asgDownH_affine_nearest` and its kin) build their samplers in a function that returns them. RenderBox
  accumulates a path's coverage by programmable blending, every edge of the path in one draw; in a sysmem pass
  those draws are now kept in raster order (`FLUSH_PER_OVERLAP_AND_OVERWRITE`), without which the icons lost rows
  and had their right edges cut straight. The Settings rows looked at then have their icons, after some 20 s of
  compiles on the first look. Not yet the default: the stages it compiles are not in the seeded cache, and the whole
  of it has had one boot.
  Without the device it draws the home screen's icons on the processor and leaves some of Settings' rows empty.
- **Tile shaders** stay refused while `supportsTileShaders` is NO, as it is.
- **Other phones' GPUs.** The driver's tables are the A635's (chip 0x06030500, redwood's Adreno 642L). It
  refuses another chip by name rather than hang it (`hakios_metal_chip_known`). Renoir's Adreno 642
  (0x06060010) needs its registers checked against `a6xx.hpp` before it is let through.

## The first generation

`components/metal/src/metal_driver.m` and `components/metal/tools/mesa/air` are the Gallium-based plug-in and its front end
of the iOS 16/26 work. That plug-in found most of what iOS 26 asks (items 1, 2 and 4 above came from it). Neither
is built for iOS 27. The glass validation tools are with them: `components/metal/tools/air/glassref.m`, `glasscase.c`.
