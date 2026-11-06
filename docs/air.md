# AIR → NIR: a Metal shader frontend for Mesa

This is the compiler third of running Metal on the Adreno of this port: a translator from Apple's
shader bitcode (AIR, the contents of a `.metallib`) to Mesa's NIR, so that ir3 — the same compiler
that already runs the port's OpenGL ES through freedreno — turns Metal shaders into Adreno code.

The other two thirds are **not** here and are not started: the `_MTLDevice`/`IOGPUMetalDevice`
object graph (AGXMetalA11 alone is 53 classes and 1158 methods, IOGPU.framework 711 more), and the
kernel ABI that submits work. Where a decision below constrains them it says so, and section 7
collects those constraints in one list.

Why this is worth doing at all, and what it costs, is argued in `docs/metal.md` section 9. This
document assumes those measurements and records the decisions taken on top of them.

## 1. Where the frontend lives: inside Mesa, as `src/compiler/air`

**Decision: a Mesa frontend in Mesa's tree, shaped like `src/compiler/spirv`.** Its entry point
is one function, `air_to_nir()` (`nir_air.h`), the analogue of `spirv_to_nir()`. In this
repository the sources are `tools/mesa/air/`; `tools/mk-mesa.sh` and `tools/air/mk-host.sh` copy
them to `src/compiler/air/` of the pinned Mesa release, exactly as `tools/mesa/redwood-drm/` is
copied to `src/redwood-drm/`, and `tools/mesa/air.patch` adds the two lines of meson that build
it. The tree is the shape an upstream merge request would have.

Why not a standalone library that Mesa links:

- **NIR has no stable ABI.** A `nir_shader` is a graph of Mesa-internal structures that change
  every release; `nir_builder` is a header of inline functions. A library outside Mesa producing
  NIR would have to be built against one exact Mesa and versioned with it — it would be inside
  Mesa in everything but name. A library producing something *else* (a serialized NIR, or its own
  IR) would add a second translation for nothing.
- **Everything else that feeds ir3 is in Mesa too:** GLSL, SPIR-V, TGSI, CL. Living beside them
  means the same NIR passes, the same `nir_validate`, the same test framework (`nir_tests`), and
  ir3 changes that break us break in Mesa's own CI.

What it costs the eventual Metal runtime, which must compile from `newLibraryWithData:` (or, more
exactly, at pipeline creation — see 7.1) in-process: **nothing extra**. The runtime must have ir3
in its process anyway, and ir3 is only reachable as part of Mesa; the port already ships Mesa as
one dylib (`libOSMesa.8.dylib`, freedreno and ir3 inside). The frontend adds a few hundred KB of
code to that dylib and no dependency. The cost is the inverse one: the runtime's Mesa must be a
build with this frontend in it, and the runtime calls a Mesa-internal API (`air_to_nir`, then
ir3), so it is compiled against Mesa's headers and moves with Mesa's version.

## 2. How the bitcode is read: our own reader, not LLVM's

This is the biggest commitment of the project, so both sides in full.

**For linking LLVM's bitcode reader** (`llvm::parseBitcodeFile`, then walk an `llvm::Module`):

- It is finished work. It reads every bitcode version, upgrades old constructs (typed pointers,
  old intrinsics) and verifies the module. All 456 modules of the corpus are already read by
  stock `llvm-dis` 22.1.8 without a failure; day one would be spent on translation, not parsing.
- It comes with passes. If a shader ever needs SROA, inlining or `mem2reg` before translation, LLVM
  has them, and a hand-written reader means hand-written equivalents (or NIR's, after translation).
- Future AIR versions (iOS 17, 18…) come from newer Apple LLVMs; LLVM's reader follows them for
  free, ours must be extended.

**Against it, and for our own reader:**

- **The cost lands in every Metal process.** libLLVM is 50–100 MB of C++ with global constructors,
  a static option registry and its own copy of libc++ assumptions, loaded into every app that
  touches Metal. On this port that is every app: CoreAnimation, CoreImage and UIKit all link
  Metal. The port's Mesa is built with `-Dllvm=disabled` (`tools/mk-mesa.sh`) for exactly this
  reason today.
- **It would have to be cross-built for iOS** with the port's toolchain (the `xnu-clang` wrapper,
  the iOS SDK, ld64). That is a multi-hour build and a second large dependency to keep building
  — the project's experience with V8 on musl says what such a build costs to keep alive.
- **Apple already has an LLVM in the shared cache** (MTLCompiler, AGXCompilerCore — 3.6 MB). Ours
  would be a second one in the same process, of a different version. Two-level namespaces keep the
  symbols apart, but it is a hazard nobody needs.
- **The subset is small and measured.** Over all 456 modules (`llvm-bcanalyzer -dump`, then a
  histogram of records), the whole of what occurs is: 17 instruction records (BINOP, CAST, LOAD,
  STORE, GEP, CALL, RET, BR, PHI, CMP2, VSELECT, EXTRACTELT, INSERTELT, SHUFFLEVEC, EXTRACTVAL,
  INSERTVAL, ALLOCA), 6 constant records (INTEGER, FLOAT, NULL, UNDEF, AGGREGATE, DATA,
  CE_CAST), 13 type records and 8 metadata records. No switch, no invoke, no atomics as
  instructions (they are `air.*` calls), no `ptrtoint`/`inttoptr`, no `addrspacecast`, no debug
  info. The bitstream container itself is documented ("LLVM Bitcode File Format") and is a few
  hundred lines.
- **The reader has an exact oracle.** `llvm-dis` prints every module; our reader prints its own
  parse in the same `.ll` syntax for the constructs it supports, and the corpus check diffs the
  two. A reader that agrees with LLVM on 456 modules is not a guess. LLVM stays a *test* tool, not
  a runtime one.
- **Mesa already chose this once.** The DXIL backend (`src/microsoft/compiler/dxil_module.c`)
  writes LLVM 3.7 bitcode by hand rather than link LLVM. The same reasoning applies to reading.

**Decision: our own reader** (`air_bitcode.c`: the bitstream; `air_module.c`: records → an
in-memory IR of the subset). The price is accepted with open eyes: about 2–3 thousand lines of C,
and every new record kind in a future AIR version is work. Unknown records are never skipped
silently: they make the module fail with the record's name, and the corpus report counts them, so
the price is visible as a number rather than as a wrong shader.

**It is written, and it agrees with LLVM on all 456 modules of the corpus** —
`tools/air/reader-check.sh`, 1 900 lines of C for the reader and its printer. What the check
compares is described in section 4; the two things it does *not* keep are function and parameter
attributes (`nounwind`, `captures(none)`, the attribute groups) and the order of declarations,
because the translation uses neither. Everything else — types, constants, every instruction,
metadata, fast-math flags, alignments, calling conventions, linkage — comes back byte for byte.

What would reverse this: if a later AIR (a newer `.metallib` corpus) turned out to need large
parts of LLVM's auto-upgrade machinery, or if translation turned out to need LLVM's own passes
before NIR. Neither is true of iOS 16.5.

## 3. What a translation unit is

A `.metallib` is a container of **one LLVM module per function** (`docs/metal.md` 9.1). The unit
of translation is therefore **one module → one `nir_shader`**, and the stage of that shader is read
from the module's named metadata, never from the container: the container's `TYPE` tag and the
metadata are cross-checked by the corpus tool and disagreements are reported.

| module carries | NIR shader | notes |
| --- | --- | --- |
| `!air.vertex` | `MESA_SHADER_VERTEX`, one entry point | |
| `!air.fragment` | `MESA_SHADER_FRAGMENT`, one entry point | |
| `!air.kernel` | `MESA_SHADER_COMPUTE`, one entry point | threadgroup size is an API value (7.4) |
| `!air.visible` | a library shader: one exported, non-entry `nir_function` | linked at pipeline time (7.5) |

Other functions in a module (helpers, `stitching_traits` methods, `air.static_init` constructors)
become non-entry `nir_function`s and are inlined. Constructors listed in `@llvm.global_ctors`
run at the top of the entry point; that is how function-constant-derived program variables get
their values (AV1SW's `kPixelShift` is computed in one from `kBitDepth`).

**Pointers.** Metal buffers are pointers, and AIR passes them as `ptr addrspace(N)` parameters.
The translator keeps every pointer as *(where, byte offset)*: *where* is a buffer binding, the
threadgroup block, a private allocation or a constant global; the offset is an SSA value computed
from GEPs with the datalayout's sizes. This is NIR's `32bit_index_offset` model and is enough for
the whole corpus (no pointer is ever converted to an integer or between address spaces). Pointers
stored to memory — argument buffers, which no system shader here uses — will need 64-bit global
addresses instead (7.2).

### 3.1 The interface: AIR metadata → NIR

The entry-point node is `!{ptr @fn, !{outputs…}, !{inputs…}}`; each input starts with its
parameter number, each output is a field of the returned `<{…}>` struct, in order. Every entry is a
flat list of `!"air.<key>"` strings, some followed by values. This table is the specification the
translator and its tests are written against. "Seen" is the number of corpus modules carrying the
key (of 456), so the order of work follows it.

**Resources (inputs of every stage)**

| AIR | seen | NIR | binding rule |
| --- | --- | --- | --- |
| `air.buffer`, `air.address_space 2` (constant) | 393 total | `load_ubo` | UBO index = `air.location_index` |
| `air.buffer`, `air.address_space 1` (device) | (of buffers) | `load_ssbo`/`store_ssbo`/`ssbo_atomic` | SSBO index = `air.location_index` |
| `air.buffer`, `air.address_space 3` (threadgroup) | 3 | `load_shared`/`store_shared`, base from a driver uniform | sized at dispatch (7.4) |
| `air.buffer_size N` | 309 | UBO range hint (`range` of `load_ubo`) | |
| `air.read` / `air.write` / `air.read_write` | | `ACCESS_NON_WRITEABLE` etc. on the access | |
| `air.texture` + `air.sample`/`air.read` | 96 | `nir_tex_instr`, `texture_index` = `location_index` | sampled/read textures |
| `air.texture` + `air.write`/`air.read_write` | | `image_store`/`image_load`, image index = `location_index` | storage images (7.3) |
| `air.sampler` | 15 | `sampler_index` = `location_index` | |
| `!air.sampler_states` (inline `constexpr sampler`) | 10 | inline sampler, index after the API ones | state word decoded at translation (M3) |
| `!air.function_constants` | 11 | constant folded from the values given to `air_to_nir` | `!{ptr @g, type, name, index, i1}` |
| `air.struct_type_info`, `air.arg_type_*` | | reflection only, not used for code | layout comes from the LLVM types |

**Vertex stage**

| AIR | NIR |
| --- | --- |
| `air.vertex_input` + `air.location_index n` | `shader_in`, `VERT_ATTRIB_GENERIC0 + n` |
| `air.vertex_id` | `load_vertex_id` (includes the base vertex, as Metal's does) |
| `air.instance_id` | `load_instance_id + load_base_instance` (Metal's includes the base instance, GL's does not) |
| `air.base_vertex` / `air.base_instance` | `load_first_vertex` / `load_base_instance` |
| output `air.position` | `VARYING_SLOT_POS` |
| output `air.vertex_output "name"` | `VARYING_SLOT_VAR0 + k`, `k` = position among user outputs; the name is kept on the variable |
| output `air.point_size` | `VARYING_SLOT_PSIZ` |
| output `air.clip_distance` | `VARYING_SLOT_CLIP_DIST0` (array) |
| output `air.render_target_array_index` / `air.viewport_array_index` | `VARYING_SLOT_LAYER` / `VARYING_SLOT_VIEWPORT` |

**Fragment stage**

| AIR | NIR |
| --- | --- |
| `air.position` | `load_frag_coord`; `origin_upper_left`, half-integer centres (Metal's convention, and Adreno's natively); `.w` is 1/w as in GL |
| `air.fragment_input "name"` | `shader_in`, `VARYING_SLOT_VAR0 + k` matched to the vertex output **by name** (7.5) |
| `air.center` / `air.centroid` / `air.sample` | plain / `centroid` / `sample` on the variable |
| `air.perspective` / `air.no_perspective` / `air.flat` | `INTERP_MODE_SMOOTH` / `NOPERSPECTIVE` / `FLAT` |
| `air.front_facing` | `load_front_face` |
| `air.sample_id`, `air.sample_mask` (input) | `load_sample_id`, `load_sample_mask_in` |
| `air.point_coord` | `load_point_coord` |
| output `air.render_target i32 n, i32 idx` | `FRAG_RESULT_DATA0 + n`, dual-source index `idx` |
| output `air.depth` + `air.depth_qualifier any/greater/less` | `FRAG_RESULT_DEPTH`, `info.fs.depth_layout` |
| output `air.sample_mask` / `air.stencil` | `FRAG_RESULT_SAMPLE_MASK` / `FRAG_RESULT_STENCIL` |
| `air.discard_fragment` | `demote` (Metal keeps the fragment alive for derivatives) |

**Compute stage**

| AIR | NIR |
| --- | --- |
| `air.thread_position_in_grid` | `load_global_invocation_id` |
| `air.thread_position_in_threadgroup` | `load_local_invocation_id` |
| `air.threadgroup_position_in_grid` | `load_workgroup_id` |
| `air.threads_per_threadgroup` | `load_workgroup_size` |
| `air.threadgroups_per_grid` | `load_num_workgroups` |
| `air.threads_per_grid` | `load_num_workgroups × load_workgroup_size` |
| `air.thread_index_in_threadgroup` | `load_local_invocation_index` |
| `air.thread_index_in_simdgroup` / `air.simdgroup_index_in_threadgroup` / `air.threads_per_simdgroup` | `load_subgroup_invocation` / `load_subgroup_id` / `load_subgroup_size` |

The scalar-or-vector type of each of these (`uint`, `uint2`, `uint3`, `ushort…`) comes from the
LLVM parameter type, and the NIR value is cut or converted to it.

**Values and operations**

| AIR / LLVM | NIR |
| --- | --- |
| `i1/i8/i16/i32/i64`, `half/float/double`, vectors ≤ 4 | the same bit sizes; wider vectors are split |
| structs and arrays as SSA values (`insertvalue`, `extractvalue`) | trees of SSA values inside the translator, never NIR values |
| `br`, `phi` (unstructured CFG) | an unstructured `nir_function_impl`, then `nir_lower_goto_ifs` — the path Mesa's OpenCL frontend uses |
| `alloca` | `load_scratch`/`store_scratch` on a per-invocation block (ir3 private memory) |
| `addrspace(2)` global `constant` with initializer | `nir->constant_data` + `load_constant` |
| `addrspace(2)` global without `constant` (program-scope variables) | a private copy per invocation (written by the constructors) |
| `fast` flags on an instruction | the NIR ALU is not `exact`; without them it is |
| `air.<op>.<type suffix>` | one handler per op in `air_intrinsics.c`; the suffix is checked against the LLVM types, not trusted |

### 3.2 The vocabulary, read off Apple's own compiler

What each `air.*` name means was a guess until this was read, and one of the guesses was wrong.
`AGXCompilerCore` — the back end of Apple's own shader compiler, in **this device's** shared
cache (`ios165/20F66__iPhone10,3_6`, never another device's) — carries its whole operation
vocabulary as a run of C strings at file offsets `0x16cf9f`–`0x16d400`, which is virtual
`0x1bbf43f9f`–`0x1bbf44400` (`__cstring` begins at file 1 482 768 for `0x1bbf41010`). 137 names,
and they are suffixes: the full function is `air.` + name + `.` + a type suffix, which is why
grepping the library for `air.fast_exp` finds nothing.

```
.add .sub .or .xor .and .min .max  global local  .s .u  .max.u.i64 .min.u.i64 .xchg.i32
.cmpxchg.weak.i32 .store.i32 .load.i32  global.{add,sub,xchg,cmpxchg.weak,store,load}.f32
acos asin atan atan2 acosh asinh atanh pow powr cosh sinh tanh exp10 log10 cos sin tan sincos
rint round fmod ceil exp exp2 fabs floor fma fmax fmin fract ldexp log log2 mad rsqrt sqrt trunc
fast_{acos asin atan atan2 exp10 log10 acosh asinh atanh pow powr cosh sinh tanh cos sin tan
      sincos round fmod mod exp exp2 ldexp log log2 rsqrt sqrt ceil fabs floor fract rint trunc
      fmax fmin clamp saturate}
gamma_expand gamma_compress gamma_expand_xr gamma_compress_xr  convert pack unpack
abs abs_diff add_sat sub_sat hadd rhadd clamp clz ctz mad_hi mad_sat mul_hi rotate popcount
extract_bits insert_bits reverse_bits  any all any_msb all_msb  saturate mix sign dot
dfdx dfdy fwidth  atomic assert  get_simd_width get_thread_execution_width
```

**This settles the two operations whose meaning was a guess, and corrects one of them.** The list
distinguishes exactly the functions the Metal Shading Language Specification defines, one name per
function:

| the question | what the list says | what we had |
| --- | --- | --- |
| is `air.fast_exp` the natural `exp` or `exp2`? | `exp` and `exp2` are separate names, and so are `fast_exp` and `fast_exp2` | right: natural `exp` |
| does `air.fast_round` take a half away from zero (`round`) or to even (`rint`)? | `round` and `rint` are separate, **and so are `fast_round` and `fast_rint`** | **wrong**: it was `nir_fround_even`, which is `rint` |

`air.fast_round` is now `trunc(x)`, stepped one away from zero when `|x - trunc(x)| >= 0.5`.
Not `trunc(x + copysign(0.5, x))`, which is the obvious formula and is wrong: for an `x` just
below a half that addition itself rounds up and the answer comes out 1 where it should be 0.
The subtraction is exact for every float, so the version used needs no magnitude guard and keeps
the sign of a negative zero; it was checked bit-exact against C's `roundf` over the halves, the
sub-halves, both zeros and 2^24.

Reading the list also closed gaps that had not been noticed: `exp10`, `log10`, `powr`, `fmod`,
`mod`, `mad`, `abs`, `abs_diff`, `add_sat`, `sub_sat`, `mul_hi`, `popcount`, `clz`, `ctz`,
`rotate`, `reverse_bits`, `extract_bits`, `insert_bits`, `dfdx`, `dfdy`, `fwidth`,
`get_simd_width` and `rint` itself are all implemented now. None of them occurs in the 456-module
corpus, so none is tested by anything here; they are in because the list says they exist and
guessing at them later would be the same mistake again.

Still unimplemented, and now known by name rather than by surprise: the inverse and hyperbolic
trigonometry (`acos`, `asin`, `atan`, `acosh`, `asinh`, `atanh`, `cosh`, `sinh`, `tanh`),
`sincos` (two results from one call), `hadd`, `rhadd`, `mad_hi`, `mad_sat`, `any_msb`/`all_msb`,
the `gamma_*` conversions, `pack`/`unpack`, and `assert`.

## 4. How it is proven right

A shader is reported as *working* only when its output was compared, never because it compiled.
Three references, each independent of the translator:

1. **LLVM on the CPU (the strongest).** `tools/air/cpuref.py` changes the module's triple to the
   host's, compiles it with `llc`, fills a buffer per `air.buffer` argument with a fixed pattern
   and calls the entry point; `air2nir --eval` replaces the same buffer loads in *our* NIR with
   the same bytes and folds it with `nir_opt_constant_folding` (no algebraic rewriting, so the
   arithmetic stays IEEE and comparable); the two sets of numbers are compared. LLVM — not our
   reader, not our translator — decides what the shader means, and neither side can see the
   other's answer. Needs no Apple hardware and no GPU.

   **255 shaders agree, bit for bit, and none differ.** The call into the shader is made from a
   wrapper written in LLVM IR rather than from C, because a shader returns a struct of vectors by
   value and how `llc` lowers that is its own business: a C declaration of the same struct agrees
   for a single vector and stops agreeing past it (this cost an afternoon's false "243 differ").
   With the wrapper, only `void(const void *, void *)` crosses into C and the question does not
   arise.

   **And what those 255 do not cover: not one `air.*` function.** Every one of them is plain
   LLVM IR, so the agreement is a strong statement about the reader, the interface mapping
   (buffers to the right UBO index, result fields to the right render targets in the right order)
   and the instruction translation — and no statement at all about `air_intrinsics.c`. The
   shaders that do call `air.*` are exactly the ones this check cannot judge: the texture ones
   have a varying for a parameter, the math-heavy ones are compute kernels or `air.visible`
   library functions. So **no operation in `air_intrinsics.c` is numerically verified**, even the
   two whose *identity* section 3.2 has since settled against Apple's own compiler: knowing that
   `air.fast_round` is `round` and not `rint` says nothing about whether this file computes
   `round` correctly. What would settle that is
   a shader whose intrinsic result is observable through a device buffer, compared the same way
   — which needs the buffer-readback half of this tool, i.e. M4's acceptance test. Writing C
   implementations of the `air.*` functions for the CPU side would *not* settle them: those
   implementations would encode the same beliefs as the translator, and a reference that agrees
   with you by construction is not a reference.

   There is one oracle for the intrinsics that needs no hardware and is not circular, and
   section 3.2 is it: **Apple's own compiler, which knows what each name means because it
   compiles it.** Its vocabulary settled the two guesses and found one of them wrong. What that
   oracle gives is each operation's *identity*, not its numerical result, so it decides
   `round` against `rint` and `exp` against `exp2` and cannot decide how many ULP Apple's
   `fast_sin` is allowed to be out by. For that, M4's buffer readback is still the test.

   What it cannot decide, and says so rather than passing: a shader with a non-buffer argument —
   a varying, `air.position`, a thread id — because what is in it is the GPU's business (19 in
   AppleMetalGLRenderer, all of them the texture ones), and a compute kernel, which returns
   nothing and writes through pointers (47 of them). Reading the buffers back after the call
   would settle the kernels and is M4's acceptance test; it is not written.
2. **The same shader drawn by a Gallium driver, and the pixel read back.** `air_run.c` puts the
   translated NIR into a `pipe_shader_state` as `PIPE_SHADER_IR_NIR`, draws one quad into a
   32-bit render target and prints the pixel. Both drivers of interest take NIR directly: ir3 is
   a NIR compiler, and softpipe takes it through `nir_to_tgsi`.

   **255 shaders draw and the pixel matches LLVM, bit for bit, with no GPU** — `cpuref.py --all
   --draw` compares all three (LLVM's execution, the folded NIR, the drawn pixel). That is M1's
   acceptance test, run 255 times, with softpipe standing in for the Adreno.

   Three things this caught that compiling never would, each of which looks like a translation
   bug and is not one: an integer-returning shader read back through a float target (the bits are
   right, read the wrong way); a shader whose outputs outnumber the framebuffer's attachments,
   which draws nothing at all; and `clearFragment04`, which writes `FRAG_RESULT_DATA1` alone, so
   target 0 holds the clear colour. The harness now takes its format from the shader's output
   type, binds an attachment per render target, and reads back the lowest target the shader
   actually writes.
3. **NIR's own checks:** `nir_validate` after every pass (the translator runs with validation
   on), and unit tests on Mesa's `nir_tests` framework for the translator's building blocks.

Where a shader has a known GLSL equivalent (AppleMetalGLRenderer's shaders *are* the GL
pipeline's fixed functions), rendering both through the port's GLES and comparing images is a
fourth check; it is not built yet.

The bitcode reader has its own oracle (section 2): its parse is printed as `.ll` and diffed
against `llvm-dis` for every module of the corpus.

## 4a. What it does and does not keep

Written down because a reader that silently drops something is worse than one that refuses:

- **Not kept, and not needed:** attributes (`nounwind`, `memory(none)`, `captures(none)`,
  `dereferenceable`, the `#N` groups), the identification block, debug info, the symbol table's
  order, use lists, operand bundles. None of them carries meaning the translation reads.
- **Kept but unused so far:** `air.struct_type_info` and the `air.arg_type_*` keys (reflection:
  the runtime will want them for `MTLArgument`), TBAA, the module's hash.
- **Refused rather than guessed:** every record kind not in the subset, a pointer stored to memory
  or cast to an integer, a `switch`, a call through a pointer, a struct field chosen at run time,
  a vector `getelementptr`. Each refusal names what it was and lands in the report's table.

Two things the translation leaves to its consumer, by Mesa's convention rather than by choice:
**system values** (`load_global_invocation_id` and the rest are emitted as NIR's own, and the
driver lowers them with `nir_lower_system_values` / `nir_lower_compute_system_values`, exactly as
the SPIR-V frontend does) and **I/O lowering** (variables with locations, then `nir_lower_io`).

## 5. Milestones and how each is measured

The corpus report (`tools/air/corpus.py report`) is the progress meter: every number below comes
out of it, and it runs from `tools/air/check.sh` (section 6). Two corpora:

- **the device image** — the five `.metallib` files the port installs (`ios165/rootfs` +
  `ios165/cryptex`): 456 modules. This is the corpus the milestones are stated against.
- **the full IPSW** — all 134 MTLB libraries of the iOS 16.5 root filesystem and cryptexes,
  16 841 functions, extracted from `rootfs.dmg` with 7-Zip. Reported alongside, because the
  device image is a selection and the system's own shaders are this set.

Each module goes through stages, and the report counts how many reach each: *container* (the
module is found and unwrapped) → *llvm-dis* (stock LLVM reads it) → *reader* (ours reads it and
agrees with `llvm-dis`) → *translate* (`air_to_nir` succeeds and `nir_validate` passes) → *ir3*
(compiles for Adreno 7c+ Gen 3, chip 0x06030500, without error) → *softpipe* (output matches the
CPU reference) → *adreno* (output on the phone matches the CPU reference). A module that stops is
counted under the first stage it fails and the reason (an unknown record, an unhandled `air.*`,
an unhandled metadata key), so "features not yet handled" is a ranked list, not a guess.

- **M0** — container and corpus: every module reaches *llvm-dis*; the report exists.
  **Done**, and further than stated: all 456 also pass *reader*, our own reader agreeing with
  `llvm-dis` on every one.
- **M1** — `clearFragment01` reaches *adreno*: translated, compiled by ir3, rendered on the
  Adreno, framebuffer equal to the colour in its constant buffer. **Done** (20 October, section
  5a).
  **Done except for the Adreno itself.** It is `nir_load_ubo` + a store to `FRAG_RESULT_DATA0`
  as predicted, and ir3 turns it into five Adreno instructions (four `mov`s out of the constant
  file, where ir3 promoted the UBO load). Over a buffer of known bytes, LLVM's own execution of
  the module, our folded NIR, and **the pixel drawn by a Gallium driver and read back** all give
  the same four floats, bit for bit. What has not happened is the draw going through *ir3 on the
  phone's GPU*: the screen here is softpipe. Section 5a says what is left for that, and it is
  not much.
- **M2** — the 283 modules with no `air.*` call: a stated fraction reach *ir3*, a growing subset
  *softpipe*/*adreno*.
  **262 of the 283 reach ir3**, 255 of them agree with LLVM numerically, and those same 255 draw
  a matching pixel. (With textures done the whole corpus is at 313 of 456; this milestone's own
  count is the intrinsic-free subset.) The other 21 are
  `air.visible` library functions, which are not entry points at all (below). Counting every
  entry point rather than every module: 275 of 321, 86%. Nothing has run on the Adreno.
- **M3** — textures and samplers, each form with a test that renders a known image.
  **Translated, compiled, and six of them rendered on the Adreno and checked.** The family took the corpus from 275 modules to
  313, and all 313 compile for the Adreno. What is *not* done is M3's acceptance test: not one
  texture operation had been run until `air_run` learned to bind one. The forms written are 2D, 3D, 1D,
  cube and their array and multisample variants, for sampling (plain, explicit level, bias,
  minimum-level clamp), gathering, fetching by integer coordinate, writing, and the queries
  (width, height, depth, array size, level count, sample count). Refused with a named error, not
  guessed at: compare sampling (`sample_compare`, `gather_compare`), texture buffers, and a depth
  texture whose aspect selector is not 1.

  **Six texture shaders have now been run and return the exact texel**
  (`tools/air/texcheck.sh`). `air_run` binds a texture whose texel is
  `(100+x, 200+y, 300+x+y+z, 400)` — 4×4 for a 2D sampler, 4×4×4 for a 3D one — a nearest
  sampler with clamped addressing, and a coordinate at the centre of texel (1, 1). A single
  sample or fetch must then return that texel *exactly*: nearest filtering at a texel centre has
  no weights to round and clamping has no arithmetic. 2D and 3D, in float, int and uint, all six
  return it; **none returns anything else**.

  Binding the matching target mattered more than it looks: with a 2D texture bound for a 3D
  sampler the three 3D shaders also "passed", by accident. With a real 4×4×4 texture they return
  `101 201 303 400` — the 3D texel — and only then are they evidence.

  **And all six draw on the Adreno and return the same exact texels** (20 October,
  `logs/003327-run.log`, `RC_JOBS=rc.jobs.airtex`): three `101 201 302 400` for the 2D samplers
  and three `101 201 303 400` for the 3D ones, each under its own
  `air_run: … on Adreno 7c+ Gen 3 (the GPU, through the kernel's msm device)`. No crash, no
  fallback, both flash-root mount lines present, nothing said about missing firmware.

  That run was worth more than its five minutes, and a peer session had to argue for it: softpipe's
  agreement proves least exactly here. Softpipe takes a `pipe_sampler_view` and samples it in C,
  while a6xx wants real descriptors in memory, a sampler state block and the right binding slots
  for the texture instructions ir3 emits — so six exact texels on softpipe said nothing about any
  of it, and the whole texture path could have been one the phone never takes. It was also the
  most diagnostic moment this test will ever have: nearest filtering at a texel centre has no
  weights to round and clamped addressing has no arithmetic, so a mismatch could only have been
  binding or descriptors. That narrowness disappears as soon as real filtering is tested.

  Of 55 texture shaders, 49 are not run, and the reasons are the honest part: 24 are compute
  kernels (`air_run` draws, it does not dispatch); 11 are `air.visible` library functions; 4
  sample a multisample texture the harness does not bind; 1 writes only depth; 1 hits an
  assertion in softpipe's own NIR-to-TGSI; 8 are other translation refusals. **Not one returns a
  wrong number** — the failures are all a refusal to run, which is the shape of failure to want.

  `texturedFragment3DDepth` is worth naming because it cost a wrong-looking result: it returns
  `{undef, depth}` and stores *nothing* to its colour attachment, so reading the colour gave the
  clear value — a number that means nothing and looks like an answer. `air_run` now checks that
  the attachment it reads is one the shader actually stores to, and refuses when it is not.

  The sampler state is the gap that matters for correctness. Metal's `constexpr sampler` arrives
  as a global holding one 64-bit word, and **that word's layout is not decoded**: the frontend
  reports it to the runtime verbatim (`air_resource::sampler_state`) and assigns the sampler an
  index after the API's. Until it is decoded, nothing knows whether a sample is meant to be
  nearest or linear, clamped or repeated — which is exactly the sort of thing that renders
  plausibly and wrongly. `AGXCompilerCore` has a `sampler_words` symbol, so the layout is
  discoverable the same way the vocabulary was.
- **M4** — atomics, barriers, threadgroup memory, compute against the CPU reference.
  Atomics, the barrier and threadgroup memory are written (`air_intrinsics.c`) and 16 kernels
  compile through ir3 with them; none has been *run*, so M4's acceptance test is untouched.

### `air.visible`, and why 135 modules are none of these milestones

135 of the 456 modules — every one in `portrait_filters.metallib` — carry `!air.visible`
instead of a stage. They are CoreImage's stitchable kernels: library functions, with
`air.visible_input`/`air.visible_output` interfaces and `air.stitching_info` describing how
CoreImage assembles them into a shader at run time. They have no entry point, so there is no
`nir_shader` to make of one by itself; each becomes a `nir_function` linked into its caller at
pipeline time (`nir_link_shader_functions`). The translator refuses them with
`stage: air.visible` so that the report counts them as one known thing rather than as 135
failures. 21 of them call no `air.*` at all, which is why the intrinsic-free count (283) and the
M2 count (262) differ.

### 5a. Is there a path from a `nir_shader` to a draw on the phone? Yes.

This was the open question, because nothing here had run. The answer is that the harness can ride
the Mesa already on the phone rather than being built from nothing:

- The port's Mesa **already creates a Gallium screen for the Adreno**:
  `osmesa_create_gpu_screen()` in `tools/mesa/freedreno-darwin.patch` opens the kernel's `msm`
  driver through `tools/mesa/redwood-drm` and calls `fd_drm_screen_create_renderonly()`. It also
  already has `osmesa_surface_resource()`, which wraps an IOSurface as a `pipe_resource`. So the
  Gallium layer is reachable inside the library CoreAnimation already draws iOS through.
- **freedreno takes NIR natively** (`PIPE_SHADER_CAP_SUPPORTED_IRS` includes
  `PIPE_SHADER_IR_NIR`; `ir3_gallium.c` handles `cso->ir_type == PIPE_SHADER_IR_NIR`), so nothing
  has to go through GLSL on the way.
- `air_run.c` is written against that interface and nothing else. Here it asks for a software
  screen (`sw_screen_create(null_sw_create())`); on the phone it wants the screen
  `osmesa_create_gpu_screen()` makes, and **no line below that changes**.

**That is now built and in the image, and no export was needed.** `air_run` is built *inside* the
Mesa cross build (`AIR=1 tools/mk-mesa.sh`), so it links Gallium's own static libraries and opens
the Adreno itself — `drmOpenWithType("msm", …)` through `tools/mesa/redwood-drm`, then
`fd_drm_screen_create_renderonly()`, exactly as `osmesa_create_gpu_screen()` does. It needs
nothing exported from `libOSMesa` and does not load it: a 9.8 MB arm64 Mach-O against libSystem,
libz and IOKit alone. `userland/build.sh` puts it in `/redwood/bin`, and `rc.jobs.air` runs it.

The same source takes the GPU where there is one and falls back to software where there is not,
so it is the binary tested on this machine — not a variant of it. Its first line says which it
got (`air_run: clearFragment01 on softpipe` here), and that line must be read before any number
below it is believed. `AIR_RUN_SOFTWARE=1` forces software even on the phone, for comparing the
two on the same device.

**It runs on iOS, and the numbers are exact there.** In QEMU (`CMDLINE="cpus=4
RC_JOBS=rc.jobs.air" qemu/run-ios.sh 420`, a clean boot: 72 `[ ok ]` lines, SpringBoard up),
`air_run` translated `clearFragment01`, found no `msm` device, fell back to softpipe as designed,
drew the quad and read back

```
2.10170319e-18  0.178894654  1.48687698e+16  1.21394141e+33
```

— the same four floats, bit for bit, that LLVM computes on a computer and that softpipe draws
there. So the whole chain works on the target operating system: the container, the reader, the
translation, Gallium, the draw and the readback. What is left is one substitution — softpipe for
ir3 on the Adreno — and that needs the phone.

**On the phone, with the root on the flash, it runs and is exact — and it ran on softpipe.** The
first run of it (19 October, `logs/232842-run.log`) booted the flash-root recipe properly (both
`[ ok ] mount the iOS volume read only` and `[ ok ] the iOS volume is the root, the RAM part of
the boot on /private/preboot`), translated `clearFragment01` and read back the same four exact
floats as everywhere else. But its first line said `on softpipe`, and the kernel said why:

```
adreno: THE GPU IS NOT STARTED: the small image has no fw/a660_gmu.bin
        (the phone's /vendor/firmware/a660_gmu.bin goes into fw-gpu/)
```

The GPU driver had not started at all, because this working copy had no `fw-gpu/` — the phone's
own firmware, which is not in the repository. **Not the driver refusing work: no driver.** This is
exactly what that first line is for: the pixel was right, and reporting it as a result on the
Adreno would have been false. Read the line before the numbers.

**Second run, with the firmware in place: the GPU driver came up and `air_run` died reaching it.**
`logs/233855-run.log` has the Adreno starting properly — the GMU, the zap shader through
TrustZone, the SMMU — and then nothing from `air_run` but one line, which is the whole reason
that line exists:

```
air_run: signal 11 at 0x10, pc 0x10037e404, lr 0x1002829b0,
         called from 0x1002829b0 0x1000a139c 0x1000077e8 0x1c3fef344 (unslid)
```

Joined against `llvm-nm` of the binary by nearest preceding symbol, that is

```
pc   driParseConfigFiles + 0x54     <- the fault, at address 0x10
lr   fd_screen_create + 0x344
     u_pipe_screen_lookup_or_create + 0xa4
     main + 0x1f8
```

`fd_screen_create` passes the `pipe_screen_config` straight to `driParseConfigFiles`, which
dereferences it; this program handed it a zeroed struct. The port's own
`osmesa_create_gpu_screen()` fills it in with `driParseOptionInfo` and `driParseConfigFiles` over
`driinfo_freedreno.h`, and that sequence — the one CoreAnimation goes through on this device every
day — is now what `air_run` uses. **A machine with no `msm` device never reaches that code**, so
no amount of running it here would have found it; it took the first contact with real hardware.

Without the fault handler there would have been nothing at all to go on: iOS answers a crash here
with "Failed to send exception EXC_CORPSE_NOTIFY" and no report. Four lines of borrowed code
turned a silent exit into an exact address.

### It drew on the Adreno. M1 is finished.

20 October, `logs/000338-run.log`, with the config bug fixed:

```
air_run: clearFragment01 on Adreno 7c+ Gen 3 (the GPU, through the kernel's msm device)
...
air_run: read back from Adreno 7c+ Gen 3 (the GPU, through the kernel's msm device),
         ir3's instructions are above
air_run: the pixel at (0, 0) of render target 0 came back as:
  2.10170319e-18
  0.178894654
  1.48687698e+16
  1.21394141e+33
```

**A shader Apple compiled for Metal, translated here, compiled by ir3, run on the phone's GPU,
giving the colour out of its constant buffer bit for bit.** Four independent references now agree
on those four floats: LLVM's own execution of the module on a computer, our NIR folded over the
same bytes, softpipe, and the Adreno.

Three things had to be asserted rather than assumed, because the pixel looks identical whichever
path produced it:

- **which screen was created** — the first line, which is what caught the earlier run where the
  GPU driver had never started;
- **which screen the pixel came out of** — the same name printed again beside the numbers;
- **that the shader went through ir3 at all** — `--disasm`, which puts ir3's own output in the
  log. It is `; FRAG prog 1/1: 5 instr, 0 nops, 5 non-nops, 4 mov` and four `mov.u32u32 r0.x,
  c0.x`: the same five instructions, mov for mov, that `ir3_compiler` produced for this shader on
  the host months of work earlier. The constant file holds the Metal buffer, which is where ir3
  promoted the `nir_load_ubo` to.

### And the driver takes work that is not the window server's

`logs/000739-run.log`, `rc.jobs.air2`: the same draw after SpringBoard is up — 287 mentions of it
in the log, the ringer sounding at 29 s, the window server holding the GPU and drawing iOS through
`src/eagl.c`. `air_run` still got `Adreno 7c+ Gen 3` and still read back the same four floats.

So the kernel's GPU driver serves a second process while CoreAnimation is drawing. That question
was owed to the whole port rather than to this frontend, and the answer is yes. `rc.jobs.air` asks with the GPU idle; `rc.jobs.air2` asks again after SpringBoard
is up and holding it. A refusal is a finding the whole port needs, not just this frontend, so
either answer is worth the run.

(Counting anything out of a QEMU log here: `qemu/run-ios.sh` prints every line twice, and
`sort -u` does not catch it because the pairs are separated by other output.)

## 6. Running it

```
tools/air/mk-host.sh            # Mesa for this machine (x86-64): the frontend, ir3, softpipe
tools/air/check.sh              # the reader against llvm-dis, the corpus report, and LLVM
```

and on one shader:

```
build/mesa-host/build/src/compiler/air/air2nir --ll  FILE[.metallib] [FUNCTION]   # what the reader read
build/mesa-host/build/src/compiler/air/air2nir       FILE[.metallib] [FUNCTION]   # the NIR
build/mesa-host/build/src/compiler/air/air2nir --eval FILE[.metallib] [FUNCTION]  # folded to numbers
build/mesa-host/build/src/gallium/drivers/freedreno/ir3_compiler \
        --chip-id 0x007506030500 FILE[.metallib] [FUNCTION]                      # Adreno code
tools/air/cpuref.py --compare FILE[.metallib] [FUNCTION]   # ours against LLVM's own answer
tools/air/cpuref.py --all --draw FILE.metallib             # every function: LLVM, folded, drawn
build/mesa-host/build/src/gallium/drivers/freedreno/air_run \
        FILE[.metallib] [FUNCTION]                         # draw with it, print the pixel
```

`0x007506030500` is how freedreno names the redwood's Adreno 7c+ Gen 3 (A635); it has no
`gpu-id`, which is why `tools/mesa/air-ir3.patch` adds `--chip-id` to that tool.

There is no hosted CI: the corpus is Apple's code and stays out of the repository (and out of
GitHub). `check.sh` is the CI; it takes 20 seconds, so it runs before every commit that touches
the frontend, and the report it writes is committed as `docs/air-report.md` with the numbers of
that commit.

The frontend lives in `tools/mesa/air/` and `mk-host.sh` links it into the pinned Mesa as
`src/compiler/air` — a link, not a copy, so there is no stale copy to build by mistake (a trap
this project has already fallen into once with `tools/mk-mesa.sh` and the freedreno patches).
`tools/mesa/air-ir3.patch` is the two-file change that teaches `ir3_compiler` to read AIR and
declares `air_run` beside it; it also removes a call of `ir3_nir_lower_variant` that tool makes
itself, which is a bug of the tool — `ir3_context_init` already calls it on its own clone, and
the const-state setup inside asserts the second time. It shows on every vertex shader and on
anything with image dimensions, and hid on everything else.

**That bug cannot be sent upstream, because the file is gone: `ir3_cmdline` was deleted from
Mesa on 2025-07-23** ("freedreno: remove ir3_cmdline — Nobody is using it anymore", 986530d2,
MR 36313). It is real in the 25.0.7 this port pins, and in every release before the deletion,
and in nothing that upstream still carries. The other half of it is untouched in main
(`ir3_context.c` still calls `ir3_nir_lower_variant`), and with no second caller there is
nothing wrong there. So there is no report to file; checking before writing one saved a
maintainer the time.

It leaves a consequence worth being plain about: **the `ir3` column of the corpus report rests on
a tool upstream has deleted.** While the port stays on Mesa 25.0.7 that is fine — the tool is in
the tarball and the patch applies — but it is not a route that survives a Mesa upgrade, and it
could never have been upstreamed. `air_run` does not share the problem: it uses the ordinary
Gallium interface (`screen->context_create`, `create_fs_state`, `draw_vbo`), which is what the
drivers and the port's own Mesa are built on. The durable way to get the `ir3` number after an
upgrade is `air_run` against a freedreno screen, which means on the phone, because freedreno
needs a real `msm` device and this machine has none.

A working copy also needs the phone's own firmware for the GPU to start at all: `fw-gpu/`
(`a660_gmu.bin`, `a660_sqe.fw`, the `a660_zap.*` pieces), which is not in the repository and which
`tools/new-workspace.sh` does not copy. Without it the kernel says so plainly and the system falls
back to software, so a run that looks fine can be a run on no GPU.

Needed on this machine: meson, ninja, bison, flex, python3 with venv, and LLVM's `llvm-as` /
`llvm-dis` for the reader check (22.1.8 here). The host build also patches Mesa 25.0's
`src/c11/threads.h`, because glibc 2.43 and later declare C11's `once_flag` and `call_once` in
`<stdlib.h>` and Mesa defines them again; the iOS cross build has its own libc and does not.

## 7. What this decides for the runtime (2) and the kernel ABI (3)

1. **Compilation happens at pipeline creation, not at `newLibraryWithData:`.** Function constants
   (`newFunctionWithName:constantValues:`), vertex/fragment varying linking and visible-function
   linking are only known then. The library step can translate eagerly and cache NIR; the ir3
   step cannot run before the pipeline. (On real iOS the compile also leaves the process, for
   `MTLCompilerService`; whether this runtime does the same is a decision for (2), and NIR
   serialization — `nir_serialize` — makes either possible.)
2. **Buffers are bound per stage by index, as UBOs (constant) and SSBOs (device).**
   `setVertexBuffer:offset:atIndex:` maps to the UBO or SSBO slot `index` of that stage; the
   frontend reports which kind each index is. Adreno's UBO and SSBO base addresses have alignment
   rules Metal's offsets do not (Metal allows 4-byte offsets for constant buffers); the runtime
   must bind an aligned base and the frontend will take the remainder from a driver uniform when
   that becomes necessary. Argument buffers (pointers in memory) will need GPU virtual addresses
   of buffers stable across submissions — a requirement on (3) — and 64-bit global access.
3. **One Metal texture index may be a sampler view or an image** depending on the shader's access
   qualifier; the frontend reports which, the runtime binds accordingly.
4. **Threadgroup memory passed as an argument is sized at dispatch**
   (`setThreadgroupMemoryLength:atIndex:`): the translator addresses it from base offsets in a
   driver uniform and Gallium's `variable_shared_mem` carries the total (as rusticl does for
   OpenCL `__local` arguments). The threadgroup size is a dispatch argument too
   (`dispatchThreadgroups:threadsPerThreadgroup:`), so `workgroup_size` is not fixed in NIR.
5. **Vertex outputs and fragment inputs match by name** (`"generated(8texcoordDv2_f)"`, `user(...)`),
   not by location: a pipeline-time link step assigns the same `VARYING_SLOT_VAR*` to the same
   name on both sides. Visible functions (CoreImage's 135 stitchable kernels are all of this
   kind) are linked into the calling shader by name with `nir_link_shader_functions`.
6. **Clip space and the framebuffer are Metal's:** NDC depth is [0, 1] (`clip_halfz`), the
   framebuffer origin is the top-left; the frontend emits no flips, the runtime's viewport and
   rasterizer state must say this.
7. **Inline samplers** (`constexpr sampler` in the source, `!air.sampler_states`) are created by
   the runtime from the state the frontend decodes and bound after the API's samplers.
