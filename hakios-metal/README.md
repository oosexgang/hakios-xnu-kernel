# hakios-metal

The Metal driver of [hakios](https://github.com/oosexgang/hakios), in C++: Metal's command model on the phone's
Adreno GPU (an Adreno 7c+ Gen 3, chip id 0x06030500), through the kernel's own GPU driver (HakiosGPU), without Mesa
in between.

What there is today, and what each part is checked by:

| Part | What it does | Checked by |
|---|---|---|
| `pm4.hpp` | the command processor's packets (type 4 and type 7, with their parity bits) | every header against Mesa's `pm4_pkt4_hdr` / `pm4_pkt7_hdr`, all 128 opcodes; opcodes against `adreno_pm4.xml` |
| `a6xx.hpp` | the registers, packet words and buffer descriptor a compute dispatch writes, under Mesa's names | `tools/check-a6xx.py`: 341 constants compared by name with the headers Mesa generates from its register XML; a changed value fails it |
| `gpu.hpp` | buffers (GEM objects), submissions, fences, through the msm DRM requests the kernel driver answers | the fake GPU; on the phone, `hakios-metal-probe` |
| `command.hpp` | `CommandQueue`, `CommandBuffer` (Metal's states, completed handlers in commit order, `waitUntilCompleted`), blits: copy and fill done by the GPU's command processor (CP_MEMCPY, CP_MEM_WRITE) | tests against the fake GPU, which runs the packets; under ThreadSanitizer and AddressSanitizer too; on the phone, the probe (28 October: a copy of 500 000 bytes exact, fills of every awkward length, 40 command buffers in order) |
| `compute.hpp` | the compute dispatch: a `Kernel` over a grid, as Mesa's computerator runs one on an A6XX, plus the driver params and pushed constant ranges an AIR-compiled kernel needs | the fake GPU, which checks that the dispatch holds together (below), and seven tests that mutate an accepted dispatch into one it must refuse; on the phone, two kernels of our own writing every word of a buffer as computed (28 October) |
| `kernel.hpp`, `tools/kernels/` | a kernel as the dispatch registers want it: machine code and every layout field; our own kernels, as ir3 text and as AIR (LLVM IR) | `tools/ir3-kernel` assembles the text with Mesa's ir3 assembler and reads the words back; `tools/mesa/ir3-dump.patch` gives Mesa's `ir3_compiler` the same output for the AIR; both regenerate the fixtures byte for byte |
| `compiler.hpp` | the compiler inside the driver: AIR to NIR by hakios's front end, NIR to ir3 by Mesa, for this chip, in this process; built in with `MESA=1`, a refusal by name without | `compiler_test`: fill_id.ll compiled here is `tests/fixtures/fill_id_air.hpp` field for field and word for word (a changed field fails it); on the phone, the same bitcode compiled on the device gave the same 64 words and ran (28 October) |
| `metallib.hpp` | `.metallib` files: thin and universal (the AIR64 slice), every offset checked | built files, good and broken; every truncation of a good one is refused |
| `fake_gpu.hpp` | a GPU for the tests: the requests answered in the process, the packets run by an interpreter as strict as the command processor (bad parity, a count past the end, an unknown opcode, an address outside every buffer: a fault), a dispatch checked against the registers it wrote | itself, by the tests above; and it fails as the kernel fails: a hang ends the submission's wait, and either the GPU is reset and FAULTS counts it (boot-arg `gpu_recover=1`) or nothing is waited for again |

What the fake GPU does not do: run a shader instruction, or judge a register's value beyond the checks named in
`fake_gpu.hpp`. A dispatch it accepts is well formed and names only memory the GPU was given; whether the GPU runs it
is the phone's to say. It said no once for a reason the fake could not see: CP_EVENT_WRITE's TIMESTAMP bit makes the
GPU write a timestamp where the end-of-dispatch wait expects a value, and every dispatch hung until the bit was
cleared (`--bisect` found it, one boot). The fake writes a timestamp for that bit now, so that stream fails here too.

Not yet: Apple's shaders. The compiler that would take them is in the driver and runs on the phone; what has been
compiled and run so far is a kernel of our own, with one device buffer and one constant buffer. Textures, samplers,
private memory, threadgroup memory and anything that draws are refused by name, not served. CoreAnimation drawing
through Metal is behind all of that.

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
`--bisect` runs `end_only` with each part of the dispatch stream switched off.

Measured on 28 October with the whole stream (hakios `logs/205809-run.log`, 0 failures): `end_only` completes in
38 us; `fill_id` writes 1024 of 1024 words as the kernel computes them in 42 us, another `c` moves every word, 8 groups
leave the other 512 words canary; `fill_id_air` the same, its `c` from a constant buffer; 262 144 words at 4096 groups
in 287 us; the hang on purpose reset in 2.1 s and the fill after it landed.

## How the parts fit

```
 a Metal program
   |  MTLDevice, MTLCommandQueue, MTLCommandBuffer, MTLBlitCommandEncoder     (hakios's plug-in, userland/src/HakiosMetal.m: not yet)
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
