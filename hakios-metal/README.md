# hakios-metal

The Metal driver of [hakios](https://github.com/oosexgang/hakios-xnu-kernel/tree/hakios), in C++: Metal's command model on the phone's
Adreno GPU, through the kernel's own GPU driver (HakiosGPU), without Mesa in between.

What there is today, and what each part is checked by:

| Part | What it does | Checked by |
|---|---|---|
| `pm4.hpp` | the command processor's packets (type 4 and type 7, with their parity bits) | every header against Mesa's `pm4_pkt4_hdr` / `pm4_pkt7_hdr`, all 128 opcodes; opcodes against `adreno_pm4.xml` |
| `gpu.hpp` | buffers (GEM objects), submissions, fences, through the msm DRM requests the kernel driver answers | the fake GPU; on the phone, `hakios-metal-probe` |
| `command.hpp` | `CommandQueue`, `CommandBuffer` (Metal's states, completed handlers in commit order, `waitUntilCompleted`), blits: copy and fill done by the GPU's command processor (CP_MEMCPY, CP_MEM_WRITE) | 9 tests against the fake GPU, which runs the packets; under ThreadSanitizer and AddressSanitizer too |
| `metallib.hpp` | `.metallib` files: thin and universal (the AIR64 slice), every offset checked | built files, good and broken; every truncation of a good one is refused |
| `fake_gpu.hpp` | a GPU for the tests: the requests answered in the process, the packets run by an interpreter as strict as the command processor (bad parity, a count past the end, an unknown opcode, an address outside every buffer: a fault) | itself, by the tests above |

Not yet: shaders. Running a Metal function needs its AIR compiled for the Adreno (AIR, then NIR, then ir3). hakios
has an AIR front end inside its Mesa build (`tools/mesa/air`); joining it to this driver is the next step, and only
after that can CoreAnimation draw through Metal instead of through OpenGL ES.

## Build and test

```sh
make test     # on a Mac or on Linux: the tests against the fake GPU
make ios      # out/ios/hakios-metal-probe and libhakios-metal.a, for iOS 16.5 (the SDK as hakios lays it out)
```

## On the phone

What `hakios-metal-probe` said on the POCO X5 Pro's Adreno 642L (28 October 2026, `gpu=1`): every check passed. A copy of
500 000 bytes came out byte for byte with nothing around it touched (4.2 ms); fills of 4, 256, 260, 65 540 and 999 996
bytes; 40 command buffers in flight completed in order with their handlers; 16 MB copied in 182 ms (92 MB/s: the command
processor moves words one packet at a time, which is correct but slow; fast copies want the GPU's blitter, next). The
first try of the 16 MB copy as a single CP_MEMCPY hung the GPU: copies are cut into packets of 64 Ki words since.

`hakios-metal-probe` does on the real GPU what the tests do on the fake one, one line per check. In hakios:

```sh
EXTRA="../hakios-metal/out/ios/hakios-metal-probe" RC_JOBS=rc.jobs.metalprobe userland/build.sh
XL_CMDLINE="... gpu=1" ./xl run
```

## How the parts fit

```
 a Metal program
   |  MTLDevice, MTLCommandQueue, MTLCommandBuffer, MTLBlitCommandEncoder     (hakios's plug-in, userland/src/HakiosMetal.m)
   v
 command.hpp   queue, command buffers, blits -> packets (pm4.hpp)
   v
 gpu.hpp       GEM buffers, submit, fences   (msm DRM requests)
   v
 Transport     iokit_transport.cpp on the phone  |  fake_gpu.cpp in the tests
   v
 the kernel's HakiosGPU (hakios: drivers/gpu/hakios.gpu.cpp) -> the Adreno 660
```

UniXNU's AdrenoMetal goes another way (Metal to Vulkan, then Mesa's Turnip). This driver does not use its code.

## License

MIT, as hakios's own user-space code is. See LICENSE.
