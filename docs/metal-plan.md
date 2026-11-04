# Metal on the Adreno: the plan, and where it stands

**Where it stands, 4 November 2026.** The window server draws through Metal on the phone, on two paths, and the plan below is done as far
as that: hakios-metal at 100 to 104 frames a second in page swipes (about 90 in Settings' scroll), UniXNU's Metal on Vulkan at 50 to 57, on a 120 Hz panel. 120 has not
been reached. What to read now: [Metal on the Adreno](reference/metal.md) and [the measurements](reference/metal-measurements.md).
Everything below the next section is the record of how it went, with the dates, and says what was known then.

## What became of the plan

| Question the plan left open | The answer |
|---|---|
| Steps 3 to 7 (buffers, library, queue, render, present) | done on the phone, in [hakios-metal](https://github.com/oosexgang/hakios-metal) and `userland/src/HakiosMetalCA.m` (`METAL_PLUGIN=ca`) |
| Compiler or Metal on Vulkan, "which is less work from here"? | both were done; on this GPU the direct compiler is about twice as fast today |
| QuartzCore's `default.metallib`, which this system has not got | `tools/ca-metallib.py` makes it on your machine, from QuartzCore's own variants and a later CoreAnimation's library you supply |
| `metal=1` kills the window server | no longer: `tools/metal-run.sh` runs with it on, and it is still off by default |
| Whether the framebuffer counts frames | it does, every 5 s (`tools/metal-run.sh`'s summary); `tools/frames.py` counts them per gesture from the `tl:` lines |

Read `docs/ru/metal.md` (why Metal is not simply missing, and what is on each side of the wall) and `docs/air.md` (the
shader compiler) first. The rest of this file is the order of work, with how each step is checked and where.

## The three parts

| Part | What it is | State |
|---|---|---|
| **A. The shader compiler** | Apple's shader bitcode (AIR, in a `.metallib`) into Mesa's NIR, so that ir3 (the compiler behind the OpenGL ES that already runs) makes Adreno code | largely done: 321 of the system's 456 shader modules reach ir3, 54 have run on the phone's GPU (`docs/air.md`, `docs/air-report.md`) |
| **B. The device** | what Metal loads: a bundle with a subclass of Metal's private `_MTLDevice`, and the objects it hands out (buffers, libraries, pipeline states, queues, command buffers, encoders: 53 classes and about 1200 methods in Apple's own) | done for the window server: `userland/src/HakiosMetalCA.m` over hakios-metal; or UniXNU's frontend with `metal_m2v=1` |
| **C. The kernel side** | submitting work to the GPU for a Metal client, with memory and fences | done: `HakiosGPUUserClient` (`drivers/gpu/hakios.gpu.cpp`) serves both paths; request `0x7d` takes a client's own pages as a buffer |

What "real Metal" would be, and why games are out of reach, is argued in `docs/ru/metal.md` section 9.4: that has not
changed. What this plan aims at is the part that is within reach: **Metal clients of the system itself** (the window
server's compositor, CoreImage, the camera's viewfinder) running on the Adreno through B and C, with shaders from A.

## The order, and how each step is checked

| # | Step | Checked by | Where |
|---|---|---|---|
| 1 | **A device that exists.** `metal=1` makes the (so far empty) `IOAcceleratorES` service name `HakiosMetal`; the bundle makes `HakiosMetalDevice`, a `_MTLDevice` subclass; `MTLCreateSystemDefaultDevice()` returns it | `metal_test` (`RC_JOBS=rc.jobs.metal`) prints the device and `HakiosMetal:` lines say which messages Metal sent that the device did not answer | QEMU, no GPU needed |
| 2 | **Learn what Metal asks first**, from step 1's "not implemented" lines: properties, family support, queue, buffer | the log, in order | QEMU |
| 3 | **Buffers and textures** over the GPU memory the kernel hands out (`hakios.gpu.cpp`: `kMsmGemNew`), `IOSurface`-backed | a program writes a buffer and reads it back | phone |
| 4 | **A library and a compute pipeline**: `newLibraryWithData:` takes a `.metallib`, the function goes through A, the kernel runs | the CPU reference in `tools/air` and then the phone | phone |
| 5 | **Command queue, command buffer, compute encoder, commit and wait**, on fences | a compute kernel dispatched through the Metal API gives the numbers `tools/air/cpuref.py` expects | phone |
| 6 | **Render pipeline and render command encoder** for the three vertex and 271 fragment shaders A already handles | a triangle through Metal | phone |
| 7 | **Present**: a drawable, and the frame reaching the screen without being dropped | the frame counters (below) | phone |

Without a phone, steps 1 and 2 are all that can be done and checked. Steps 3 to 7 each need the GPU.

## Step 1: what is in the tree

- `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp`: with boot-arg `metal=1` the `IOAcceleratorES` service gets the
  properties `MetalPluginName` = `HakiosMetal` and `MetalPluginClassName` = `HakiosMetalDevice`. Without the argument
  nothing changes: **the properties stay off by default, on purpose**. With them a program that asks for Metal gets a
  device, and CoreAnimation would pick Metal for the screen; until that device can draw, the screen without it is the
  working one.
- `userland/src/HakiosMetal.m`, `userland/src/HakiosMetal.plist`: the bundle, built into `/redwood/lib/HakiosMetal.bundle` and
  linked at `/System/Library/Extensions/HakiosMetal.bundle` (`userland/build.sh`).
- `userland/src/metal_test.m` and `userland/rc.jobs.metal`: the check that already existed.

## What steps 1 and 2 found (QEMU, 27 October 2026, `metal=1`, a clean image)

Run in a loop of rebuild-image, boot, read the log; each pass gave the next requirement:

1. **Metal found the service, loaded the bundle and made the class**: `HakiosMetal: the class HakiosMetalDevice is registered`
   in `backboardd` and `mediaserverd`, then `a device for the accelerator port N (made)`: `-initWithAcceleratorPort:` was
   called and `_MTLDevice`'s own `-init` returned an object. So the discovery chain of `docs/ru/metal.md` section 1 is as described.
2. `MTLAddDevice` then **asserted that the class conforms to the protocol `MTLDevice`**, and, once it did, **to the private
   protocol `MTLDeviceSPI`** (neither is declared by `_MTLDevice` itself, so the class declares them).
3. With both declared, the first messages sent to the device, in this order: **`-featureProfile`**, **`-maxBufferLength`**,
   and then `backboardd` tries to **create a command queue** (`[Metal] Failed to create MTLCommandQueue`) and exits. The
   window server restarts and fails the same way (launchd: "service has crashed N times in a row"). That is what a Metal
   device that exists and cannot do anything does to the system, and the reason `metal=1` is off by default.

4. **A command queue** is now made (`HakiosMetalQueue`, a subclass of `_MTLCommandQueue`; it queues nothing). Metal then
   sends it `-setCompletionQueue:`, which is not in `MTLCommandQueue` but in the private protocol `MTLCommandQueueSPI`: found
   by taking every protocol of the process whose name begins with `MTLCommandQueue`. The window server then asks for
   `-setGPUPriority:` and `-newDepthStencilStateWithDescriptor:`.
5. **The objects a device hands out** are stub classes made from a table in `HakiosMetal.m` (depth-stencil state, sampler
   state, texture, render and compute pipeline states, event, fence): a subclass of Metal's private base where the system has
   one, the protocol family declared, every other message logged once and answered with zero. Textures and events have no
   private base class on this system (NSObject is used).
6. **The next wall**, with those in place: CoreAnimation's Metal renderer stops with
   `Assertion failed: (metallib_url != nil), function MetalContext, file ogl-metal.mm, line 3308.` It needs the URL of a
   `.metallib` (the system has `AppleMetalGLRenderer.bundle/default.metallib`, 274 shader modules, which the AIR work translates)
   and will then ask the device for a library from it, which is where the shader compiler (part A) is first needed for real.
   Where CoreAnimation takes that URL from is not known yet; the log shows the device was asked for nothing before it.

7. **Who is shown the device**, then, is a boot-arg level (the plug-in reads `sysctl kern.bootargs`): `metal=1` only the program
   `metal_test`, `metal=2` every process but the window server, `metal=3` every process. With the window server shown the device
   it stops on the missing metallib (QuartzCore.framework has a `default.pipelinelib` that names a `default.metallib` which is
   not in this image), so it cannot run on Metal here, and it has to be left on its OpenGL ES path. With `mediaserverd` shown it, it
   ends and starts again. At `metal=1` the window server, SpringBoard and mediaserverd were each logged as not shown the device
   and none of them ended in a QEMU run (the result of `metal_test` itself is not in this file yet).

So the road from here is the library (step 4 of the order below: `newLibraryWithURL:` / `…Data:` through the AIR compiler), then
buffers and textures with real memory, and the command encoders; none of that can be tried without the GPU.

## Step two: buffers, libraries, functions (27 October 2026)

What the plug-in now makes, in `userland/src/HakiosMetal.m`:

- **Buffers**: `-newBufferWithLength:options:`, `-newBufferWithBytes:length:options:`, `-newBufferWithBytesNoCopy:length:options:deallocator:`
  give a `HakiosMetalBuffer` (a subclass of `_MTLResource`, declaring `MTLBuffer`, `MTLResource` and their SPI protocols) over
  memory of the process: zeroed pages, the caller's own for `...NoCopy` (its deallocator called when the buffer goes). `-contents`,
  `-length`, the storage, cache and hazard modes of its options, `-allocatedSize`, a label. `-gpuAddress` is 0: the GPU's own
  buffers (`hakios.gpu.cpp`, `kMsmGemNew`, mapped into the GPU's address space) come with the GPU, and since the Adreno shares
  the system's memory, a shared-storage buffer's bytes can stay where they are when they do.
- **Libraries**: `-newLibraryWithURL:error:`, `...WithFile:`, `...WithData:`, `-newDefaultLibrary` and `...WithBundle:` read the
  `.metallib` with `userland/src/hakios_metallib.c` (the reading of `tools/metallib.py` in C, every offset checked; the file is
  the program's to give), and give a `HakiosMetalLibrary` (on `_MTLLibrary`) with `-functionNames`. A file that is not a metallib
  is an `MTLLibraryErrorDomain` error that says why.
- **Functions**: `-newFunctionWithName:` gives a `HakiosMetalFunction` (on `_MTLFunction`) with its name, its `MTLFunctionType`
  from the container's TYPE tag, and `-hakiosBitcode`, its AIR module unwrapped: what the shader compiler (part A) takes.
  Function constants are accepted and said not to be applied.

How it was checked, and what that does not show:

- `tools/metallib-test.sh DIR`: the C reading and `tools/metallib.py` agree on every function of every `.metallib` of the Darwin userland 16.5
  system (the five MTLB files; the five runtime libraries of `AGXMetalA11.bundle` are another format, and both refuse them) and of
  macOS's; a file made by the test and then cut short at every length, and with each offset and length bent outside it, is refused
  every time, under the address and undefined-behaviour sanitizers. One correction came out of it: the header's second word at
  0x20 is the function list's length after its count, not its end (`tools/metallib.py` calls it `funcs_end` and does not use it).
- `tools/metal-plugin-test.sh DIR`: the plug-in built for macOS, its device made the way Metal makes it, on the Mac's own
  `_MTLDevice`, `_MTLResource`, `_MTLLibrary`, `_MTLFunction`: buffers written, read back, copied, refused over `maxBufferLength`,
  freed (and the caller's deallocator called); the five Darwin libraries read (36, 135, 11, 274 and 0 functions; the last is
  SafariSharedUI's, a header and nothing else), every function made by its name with its type, every module LLVM bitcode. All
  under the address sanitizer. **macOS's Metal is not Darwin 16.5's**: this shows the plug-in's own logic, not that Darwin's Metal is
  content with these objects.
- Built for Darwin userland 16.5 (`-Wall -Wextra`, no warnings). Not yet run on Darwin: `metal_test` asks "what the device makes" in a process of
  its own (`RC_JOBS=rc.jobs.metal`, `metal=1`), and its line is the result that goes here.

Still nothing is drawn or computed: a pipeline made of these functions is the empty stand-in of step one. The next step is the
compiler behind `-newComputePipelineStateWithFunction:error:` (part A: the function's bitcode through NIR to ir3), and then the
command buffer and compute encoder on the GPU's buffers (part C), which need the phone.

## What UniXNU's AdrenoMetal showed, and what was taken

[UniXNU](https://github.com/UniXNU) runs iPadOS 17 on a Xiaomi Pad 6S Pro (SM8550, Adreno 740), and its
[AdrenoMetal](https://github.com/UniXNU/AdrenoMetal) (MIT) composes the system UI on the GPU. Read on 27 October 2026; the facts below
are written here in our own words and code, with the source named where it is used. Since 3 November `tools/mk-m2v.sh` also fetches and
builds it on your machine, and only our patches to it are in this repository (`tools/m2v/`): [Metal on the Adreno](reference/metal.md),
[third-party licenses](../THIRD_PARTY_LICENSES.md).

- **Their route is Metal on Vulkan**: the plug-in's device is a subclass of `_MTLDevice` (as ours), and every Metal object is
  translated by a runtime of theirs ("metal2vulkan", `bridge/NativeMetalObjects.rs`) onto Turnip, Mesa's Vulkan driver for Adreno,
  over the kernel's GPU interface. Ours is AIR through NIR to ir3 directly (part A), on the same Mesa. Turnip supports the A6xx
  generation our Adreno 642L belongs to, so their route was open to this port as well. On 3 November it was taken too, and the two
  now run side by side, chosen at boot.
- **Universal metallibs.** Newer systems ship framework libraries as a fat container (0xCAFEBABE) of an AIR64 slice and a binary
  for Apple's GPUs (QuartzCore's `default.metallib` on iPadOS 17). `hakios_metallib_thin()` takes the AIR64 slice, as Metal does;
  `tools/metallib-test.sh` and `tools/metal-plugin-test.sh` check it. Darwin 16.5's libraries are all thin.
- **CoreImage's limits.** `CIMetalDeviceGetLimits` reads `dedicatedMemorySize` and `sharedMemorySize`, which `_MTLDevice` does
  not answer; without them every `CIContext` throws (iconservicesagent ended over symbol glyphs there). The device now answers
  them, `isLowPower`, `isRemovable`, `isHeadless`, and `supportsFeatureSet:` (NO: the base reads Apple's tables).
- **Clients by a file.** A file beside the bundle names the processes shown the device. `HakiosMetal.clients` does the same here,
  on top of `metal=N`.
- **The window server needs QuartzCore's own `default.metallib`.** Their system image has it; Darwin 16.5's (this port's) does not,
  which is the wall of step 7 above. It is not something the plug-in or this repository can supply, the library being Apple's:
  `tools/ca-metallib.py` makes one on your machine from what you supply.

## Frame skips

(Written before Metal drew anything. Frames are counted now: [how to measure](reference/metal.md#measure).)

A dropped frame is not a Metal problem, and Metal would not cure it: the screen of this port was then drawn by
CoreAnimation's OpenGL ES renderer through Mesa (`gpu=1`), and its pace depends on the submit path, the fences and the
present. What to measure first, on the phone, is already counted: `gGPUStat` in `drivers/gpu/hakios.gpu.cpp` (submissions,
waits, what each cost, every five seconds). Whether the framebuffer stand-in (`MiniIOMobileFramebuffer.cpp`) counts
frames and drops is not known to me: it records when the first frame was shown and keeps a small copy of frames
(`record()`), and a counter of presented and dropped frames would be the first thing to add. A frame skip that is not seen
in those numbers is in the compositor, not the driver. This needs a stable GPU run on the phone first; the
`gpu=1` hang being bisected (see `docs/driver-review/README.md`) comes before any of it.

## What is not claimed

That 120 frames a second has been reached: it has not. That a Metal program in general runs: only the window server's use of Metal is
shown, and the checks of `metal_test`, `vk_test` and `m2v_test`. That games run: section 9.4 of `docs/ru/metal.md` still stands. The
sections above dated 27 October say what was seen in QEMU then; the phone came after them.
