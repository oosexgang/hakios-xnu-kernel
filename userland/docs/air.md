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

## Where it stands, and what that does not mean

Of the 456 modules of the device's system `.metallib` files (`tools/air/check.sh`, and
`docs/air-report.md` for the breakdown):

| | of 456 |
| --- | --- |
| read by our own bitcode reader, byte for byte as `llvm-dis` reads them | 456 |
| translated to NIR, passing `nir_validate` | 321 |
| compiled by ir3 into Adreno machine code | 321 |
| numerically identical to LLVM's own execution, and drawn | 255 |
| **run on the phone's GPU with a result it finished** — 7 drawn, 23 kernels | **30** |
| dispatched on the phone's GPU without it finishing them (section 5) | 24 |

And of the `air.*` operations Apple's own code calls (`tools/air/intrincheck.py`, which reads
every `.metallib` in the device image and not only the corpus's five — 821 declarations, 119 of
them the corpus's, and 14 libraries of 128 it cannot read at all):

| | of 821 |
| --- | --- |
| checked numerically, one operation at a time, against a reference outside this tree | 552 |
| of those, exactly defined and agreeing on every case (313 752 cases) | 438 |
| of those, agreeing on every case they are *claimed* to, with 23 cases set aside by name (`fmod` above a representable quotient) | 5 |
| of those, approximate, so measured against libm and nothing concluded | 109 |
| of those, also *run* by a Gallium driver rather than folded (softpipe, 3 424 cases) | 168 |
| of those, run **on the phone's own Adreno** and agreeing there — every exactly defined one the suite had on 20 October | 71 |
| of those, disagreeing before the `ir3` rounding fix this port now carries (0 after) | 5 |
| the atomics and the barrier, against kernels written for them (`atomiccheck.py`) | 10 |
| texture operations verified exactly, on several texels each (`texopcheck.py`) | 13 |
| the `constexpr sampler` word: its behaviour against Apple's rules, on softpipe **and the Adreno** (`sampcheck.py`; the three words the corpus carries, 15 cases) | 15 of 15 |
| texture operations with a case softpipe cannot run: a 16-bit result | 7 |
| texture operations with no case: multisample, depth, write | 11 |

321 of 456 is **every entry point** of the corpus: 271 fragment shaders, 47 compute kernels and 3
vertex shaders. The other 135 modules are `air.visible` library functions, not entry points at
all, and they have a measurement of their own:

| the 135 library functions | |
| --- | --- |
| wrapped in a generated entry point and translated (`--library`) | **135** |
| dispatched on softpipe and read back | 134 |
| of those, writing exactly the words our folder computes | 120 |
| ran, but the folder did not reduce the result to a constant | 14 |
| softpipe asserts in `tgsi_ureg` (`_pf_pickThreshold`): its limit, not ours | 1 |
| **agreeing with LLVM on this machine, bit for bit** | **21 of 21** |
| agreeing with LLVM *plus our own definitions* of the exactly defined `air.*` they call | 54 of 57 |
| on the phone's Adreno, of the 124 that could be wrapped before the texture arguments: writing exactly what the folder computes | 111 |
| of those, differing with an approximate `air.*` in them (licensed) | 5 |
| of those, differing by 1 to 16 ulp, where the hardware contracts multiply-add | 3 |
| of those, **differing with no explanation** | **0** |

The folder-did-not-reduce row was 20, of the 124 then wrappable, until the folder was allowed to
simplify control flow. Folding arithmetic is not enough for a shader with an `if` in it: the
comparison has folded, the branch under it is dead and the phi below it has one live source, but
`--eval` ran only `nir_opt_constant_folding` and friends and stopped at the branch.
`nir_opt_dead_cf` and `nir_opt_if` are not algebraic rewriting — they change no arithmetic — so
adding them costs the comparison nothing and bought 15 of those 20. It is worth naming because
the missing 20 looked like a property of those shaders and were a property of the pass list.

`tools/air/libcheck.py` prints that table and refuses to print it when the rows do not add up.
The last two rows are the ones that count, and they are deliberately two. LLVM is a full
reference only for a function that calls no `air.*` at all, because an `air.*` call is an
undefined symbol on the host — 21 of those, and all 21 agree. For the 57 that call only exactly
defined operations, `tools/air/airdefs.py` writes an LLVM-IR body for each operation and llc can
then execute the function: 54 of the 57 agree bit for bit, 0 differ, and the 3 left out are ones
our own folder does not reduce to a constant, so the missing side is ours. That row is weaker and
kept apart from the 21: there, LLVM decides everything; here, LLVM decides the function's own
arithmetic and we decide what its operations mean. What makes it worth having anyway is where the
operations' meaning comes from — `intrincheck.py`'s reference, which is pinned to the Adreno on
1 748 cases over 71 declarations, so the chain is measurement to measurement rather than
measurement to opinion. The remaining 46 call an approximate operation (`air.fast_pow` and the
like), which nothing compares bit-exactly anywhere, and `airdefs` refuses to invent a body for
one rather than making the agreement circular.

**The last eleven took a calling convention rather than a check.** CoreImage hands a stitched
function a texture, a sampler and a transform, and Apple passes the first two as one-field structs
— `metal::texture2d` and `metal::sampler`, wrapping exactly the address-space-1 and -2 pointers an
entry point's texture and sampler arguments are — with the transform as a bare constant-space
pointer. A texture is not a value in this frontend (section 7.4), so the wrapper resolves each of
the three to a binding instead of loading it, and `translate_callee` had to learn that a struct
holding a pointer is bound too rather than passed: passing one inserted a call whose source had no
value, which NIR dereferences while adding uses — a segfault rather than a message.

The transform is the one place this harness pretends to know what a buffer means. Filled with the
byte pattern it sends every sample somewhere arbitrary and the result says nothing; filled with
the identity — which the resource can ask for, because `air.arg_type_name` says `float4x4` and
that name is Apple's — `_pf_sobelH` answers `(0, 8, 8, 1)` on the texel ramp instead of
`(0, 0, 0, 1)`. Eight is what a Sobel kernel returns for a unit slope. That is still not a check:
nothing here predicts those numbers, and the row says dispatched, not correct.

**An exactly defined operation is still not bit-identical across implementations.** Three of
those functions differ from our own folding, and the cause is contraction, not arithmetic:
`_pf_portraitConvertToGrayscale` is one `air.dot.v3f32` and nothing else, and evaluating that dot
as an fma chain in Python gives the Adreno's word exactly — `-0.249999732`, bits 3196059630 —
while evaluating it left to right in binary32, rounding each step, gives the folder's
`-0.249999762`, bits 3196059632. Metal compiles with `air.compile.fast_math_enable`, which permits
the hardware to fuse; so does ir3. The other two carry more arithmetic and are consistent with the
same cause rather than demonstrated. `tools/air/libcheck.py` puts a difference down to contraction
only within 32 ulp, and anything beyond that stays in a row called *no explanation*, which is the
row that would mean a defect.

**Random bytes are not a universal input.** The argument buffer holds floats, not the usual byte
pattern, because these modules are compiled with `no-nans-fp-math` and `no-infs-fp-math`: the
shader promises its inputs are finite, and arbitrary bytes are mostly infinities and NaNs. With
those, one function folded to NaN where softpipe produced `+inf` — and under that contract both
answers are permitted, so the disagreement said nothing about either side. A shader has a domain,
and a check that leaves it measures the check.

**What a library function's measurement is not: evidence about Metal's stitching.** Metal links
these into a caller at pipeline time through the ABI their own metadata describes
(`air.stitching_info`, with the four functions that do it and each argument's size and
alignment). That ABI is the runtime's, not the compiler's, and it is not implemented. The wrapper
is this project's own calling convention, invented to make the bodies runnable.

**And what the 456 itself is not: the number of shaders on the device.** It is the number of
modules in the `.metallib` *files* under `ios165/rootfs`. A library can also be a blob inside a
dylib — `GPUCompiler`'s own `GNR.videoprocessor` carries one in `__DATA`, found while looking for
something else — so every statement here of the form "the corpus contains no X" is bounded by how
the corpus was collected, not by what the system contains. Where that distinction matters, this
document says so; extending the corpus to embedded libraries would move this denominator and
every percentage computed from it, so it is a change to make in one commit and recount, not a
figure to adjust.

The 71 were run in three sessions and the cases are not added together carelessly: 714 cases over
61 declarations on 20 October, then the ten `half`-returning ones in two runs of 98 and 936 cases
once they became measurable at all. They became measurable because the suite now writes a half
result to a **half** render target instead of widening it with an `fpext` inside the shader: that
widening let NIR fold the conversion into a 32-bit one, and `air.convert.f.f16.u.i32` came out as
four `cov.u32f32` where it now comes out as one `cov.u32f16 (even)` — the shader was computing
f32(x) while the reference computed f16(x), and ten declarations were reported as agreeing on that
basis until the comparison was taken away from them.

`half4` as a render-target type name is Apple's own and not an invention: `GNR.videoprocessor`,
built by the same compiler for the same AIR version, carries 24 such declarations over 18 fragment
entry points. It is absent from the 456-module corpus because that corpus is the `.metallib`
*files* in `rootfs` and that library is a blob inside a dylib — a fact about our sampling.

**And the first run of those ten was a pass about nothing.** Emitted with `--cases 8`, it reported
all ten agreeing; but the integer pool is ordered for f32's interesting values, and its first eight
are small enough to be exact in half, so the four `air.convert.*.f16` declarations never once
performed the rounding that is the whole of what they do. `intrincheck --check` now says so by
name: a declaration that narrows to half and has no case needing a rounded result is named instead
of counted. The numbers above are from suites where 11 of each conversion's 22 inputs force one.

**What those numbers are not**, because they will be quoted without this paragraph otherwise:

- **The arithmetic operations are checked; two of them have now been checked against the
  Adreno, and the other 76 have not.** 134 of the image's declarations agree with C's own
  functions on every one of 102 103 cases, and 60 of them agree again when *executed* by softpipe
  instead of folded — which is a real answer to what used to be the largest hole here (section 4,
  item 4), and found, getting there, three bugs in the lowering, three limits in the engines it
  runs on and three wrong expectations in its own reference. But those two engines are both on an
  x86 host. An
  opcode whose Adreno instruction differs from what `nir_opt_constant_folding` and TGSI compute is
  invisible to all 102 911 of those cases, and the 75 narrow-type declarations softpipe cannot run
  at all — plus `air.fma.v4f32`, which TGSI's `MAD` may round twice — are exactly the ones where
  ir3 differs most from it.

  On 20 October that stopped being true. All 78 declarations ran on the phone's own Adreno, and
  twice: once as the port stood, and once with the one-line `ir3` rounding fix this tree now
  carries (`tools/mesa/ir3-round-default.patch`). **The 56 declarations the reader has reached
  since have not been asked there**, and four of them are exactly where the 21 October
  disagreements were (section 4, item 4): the phone's row in the table above is still the suite of
  20 October.

  | | agree | differ | set aside | cases |
  | --- | --- | --- | --- | --- |
  | as the port stood (`logs/090218-run.log`) | 56 | **5** | 10 | 714 |
  | with the rounding fix (`logs/090918-run.log`) | **61** | **0** | 10 | 714 |
  | the ten `half` ones, conversions (`logs/173828-run.log`) | **5** | 0 | — | 98 |
  | the ten `half` ones, the rest (`logs/173853-run.log`) | **5** | 0 | — | 936 |

  The cases are listed per run and not added up: three sessions, three suites, and a total would
  hide that the last two exist only because the suite changed between them.

  The 5 were the whole point of having gone there, because every engine on this machine agrees
  with C about them. All five are integer-to-float conversions:
  `air.convert.f.{f32,v2f32,v3f32}.u.*` and, once the pool was sharpened to ask,
  `air.convert.f.{f32,v2f32}.s.*` as well — `i2f` truncates exactly as `u2f` does, which the
  earlier pool had no value to notice. With the fix all five agree and nothing else moved, which is
  the useful shape of an after-measurement: the thing that was wrong is right, and nothing that was
  right has changed.

  **An earlier version of this paragraph said 66 agree on 808 cases, and that was 10 declarations
  too many.** It also said the 24 narrow-type declarations softpipe cannot run had all run on the
  Adreno and agreed, which was the part that mattered and the part that was wrong: ten of them
  return `half`, and the suite was widening that half inside the shader, so the reference described
  an operation the shader did not perform. Those ten —
  `air.convert.f.f16.{s.i32,u.i16,u.i32,u.i8}`, `air.fmax.{f16,v4f16}`, `air.fmin.{f16,v4f16}`,
  `air.mix.f16`, `air.saturate.f16` — were set aside by name for as long as that was true, and have
  since been run properly and agree (two runs, 1 034 cases; section 4 item 4 has the story).
  56 + 5 + 10 = 71 closed the account then and 71 + 0 closes it now. Nothing refused to run at any
  point: no assertion, no fallback, no module missing.

  The row "run on the phone's GPU" in the first table above does not count these: it counts
  modules of the 456-module corpus, and these are generated modules that are not in it. The two
  measurements are deliberately not added together. That row reads **30** — 7 fragment shaders
  drawn and 23 compute kernels the GPU finished, which are disjoint sets
  because a kernel is not a fragment shader. It read 7 until the kernels ran, then 47, then 51 and
  54 as more were dispatched — and **down** to 30 when `air_run` learned to wait on a fence and it
  turned out that 24 of those dispatches were never finished by the GPU. A figure that only ever
  goes up is a figure nobody is checking; this one went up three times for work and down once for
  a better instrument.
- **Two traps this frontend's own checks fell into, kept here for the instances; the general forms
  are in the "How our own checks lie" notes (docs/checks.md in hakios-xnu-kernel), which collects all six and belongs there because
  the others came from the camera, the services and the build rather than from AIR.** Comparing one
  file of Mesa's `main` against ours had to start from a fresh tarball extract, because ours carries
  our own patch — 202 487 bytes against 202 144 clean — so a comparison from this tree would have
  differed by exactly our own change. And `patch -s --dry-run` exiting 0 against that file was
  written down as "applies cleanly" when without `-s` it says "succeeded at 410 (offset 2 lines)":
  the context matched and the line number had moved, which is the more useful statement and the one
  the patch header now carries.

- **A `continue` that adds nothing to a total is where this check has gone wrong three times.**
  Not a maxim but a thing to grep for, and the general form is in README beside the other five.
  Three times on 20 October an absent check was dressed as a passing one, and each time the shape
  was identical: a loop skipped a case and incremented no counter, so "not examined" was reported
  as "examined and agreed".

  - `if idx >= len(flat): continue` dropped every case whose value was missing from the log, and
    the total fell by exactly as much as nobody was watching;
  - `read_whole`'s predecessors read a configuration file with one `read()` into a fixed buffer, so
    a file past its buffer silently lost its tail and the jobs in it never ran;
  - `check_log` skipped every infinite expectation, which is how ten `half`-returning declarations
    came to be reported as agreeing on every case when three of every four cases were not compared
    at all.

  So the rule is not "skip less". It is that every case set aside must reach the summary with its
  own number and its own reason, and that a verdict resting on very few cases must say how few —
  `air.all.v2i1` agrees on two cases, and that is worth knowing before quoting it. Green has to
  mean "looked at and agreed", never "did not look".

- **The Adreno truncates `u32 → f32` where C, NIR and softpipe round.** This is the first thing
  found here that no engine on this machine could have found, and it is why the suite was carried
  to the phone at all. `air.convert.f.f32.u.i32` and its 2- and 3-component forms, given
  `0xFFFFFFFF` (4 294 967 295 unsigned), return **4 294 967 040** where C returns
  **4 294 967 296**.

  The arithmetic is worth writing out, because the two numbers look like a printing difference and
  are not. In `binary32` the spacing between representable values in [2³¹, 2³²) is 2⁸ = 256, so the
  neighbours of 2³² − 1 are 2³² − 256 and 2³². Their distances from it are 255 and 1, so
  round-to-nearest gives 2³² = `0x4f800000`, which is what `(float)4294967295u` is in C, checked
  by compiling it. The Adreno's answer, 2³² − 256, is exactly `0xFFFFFFFF & 0xFFFFFF00`: the low
  eight bits dropped. That is truncation toward zero, not rounding. Metal's conversions are
  OpenCL's, which require round-to-nearest-even, so this is a real divergence and not a matter of
  taste. Only the cases where rounding decides anything differ — 1 of 4, 2 of 8 and 3 of 12 — and
  every value that is exactly representable agrees.

  **Where it comes from is settled as far as inspection can settle it, and no further.** Two of the
  three possible culprits are eliminated by reading code rather than by guessing:

  - the frontend emits `nir_u2fN`, the correct opcode (`air_intrinsics.c`, the `convert` branch);
  - ir3 turns that into a *single* `cov.u32f32` with no compensation. For a 32-bit source
    `create_cov` has no special case at all — the special cases there are for 8-bit sources and
    for `f32 → u8`. The generated Adreno code for the module is five instructions, four of them
    `cov.u32f32` reading straight out of the constant buffer, with nothing between the load and the
    conversion.

  So the truncation is the behaviour of the a6xx `cov.u32f32` instruction itself. **Whose job it is
  to compensate is not established**, and needs the a6xx ISA documentation, which is not to hand:
  if `cov` is specified to round, this is a hardware erratum ir3 should work around; if it is
  specified to truncate, then every API that requires round-to-nearest — OpenCL, Metal, and Vulkan's
  `ConvertUToF` — has to compensate, and ir3 does not. Not worked around in the frontend, because
  the frontend is emitting the right opcode and a workaround there would hide a fault that belongs
  to everything ir3 compiles.

  **The prediction that followed has been tested and holds, by a peer session rather than here.**
  The port's OpenGL ES goes through the same ir3 and the same instruction, so a GLES shader should
  truncate identically — and `userland/src/u2f_test.c` (`RC=rc.u2f`) asks GLSL ES 3.0 the same
  question through the same Mesa, with no AIR anywhere in it. Four of its six inputs disagree with
  C on `freedreno; Adreno 7c+ Gen 3; OpenGL ES 3.2 Mesa 25.0.7`, and the two that agree are exactly
  the two where truncation and rounding give the same answer — so agreement there is the absence of
  a question, not a confirmation. **Everything ir3 compiles on this phone converts unsigned to
  float by truncating, the window server's drawing included**, and this frontend is not party to
  it: it is physically absent from that test.

  **The cause is ir3 and not the a6xx instruction, and an earlier draft of this section had that
  wrong.** It said inspection had eliminated two of three culprits and the remaining one was the
  hardware. The steps were right and the conclusion was not: `create_cov` ends with a rounding
  field that the reading stopped short of. `cov` *has* a rounding mode — `ROUND_ZERO = 0`,
  `ROUND_EVEN = 1` in `instr-a3xx.h` — and `ir3_compiler_nir.c` sets it for a destination of
  `TYPE_F16` or `TYPE_F32` from `float_controls_execution_mode`, which is a SPIR-V declaration.
  `nir_get_rounding_mode_from_float_controls` returns `nir_rounding_mode_undef` when nothing is
  declared, so neither branch fires and the field keeps its zero, which is truncation. GLSL ES
  declares nothing and this frontend declares nothing, which is exactly why two independent paths
  returned the same wrong number. **The hardware can round; ir3 only asks when someone else has
  asked first, and supplies no default where IEEE-754 and GLSL both require round-to-nearest.**
  **How much this reaches is a number and not an adjective.** Of the 456 modules of the corpus, 20
  contain a conversion the patch can change — a `u2f`, an `i2f` or an `f2f16`, the three that
  round; none contains only the exact kinds (`f2f32`, `b2f`), where setting the mode cannot alter a
  value. So on the Metal side the blast radius is 20 modules, not all 313 that translate, and
  whether each of those 20 actually changes depends on the values it meets at run time.

  That bounds this frontend's half. **The other half has now been measured too, by a peer session,
  and the answer has a shape worth keeping.** The port's OpenGL ES reaches ir3 through Mesa's own
  GLSL compiler, which the count above does not look at, so what the patch does to the screen could
  only be answered by looking at the screen: one image, one command line, an ordinary boot to the
  home screen, patched against unpatched.

  | | differ by 1–8 | differ by more than 8 |
  | --- | --- | --- |
  | patched against unpatched (4 min 33 s apart) | 10 835 | 2 508 |
  | unpatched against unpatched (8 min 36 s apart) | **13 738** | 2 286 |

  Both large patches in the pair were cut out and looked at rather than guessed from their
  coordinates: the Clock icon, whose hands had moved, and the status bar's clock. **No geometry
  moved and no colour changed.** The small residue sat in the bottom bands — search, dock, page
  dots, the transparent things with a blur behind them — exactly where a rounding change would show
  and at exactly the magnitude predicted, which is why the control mattered: two runs of the *same*
  binary, with a larger gap, differ by **more**. So the residue belongs to the run and not to the
  patch, and the honest statement is not "nothing changed" but **"any change is smaller than what
  the screen does to itself between launches"**.

  That is the useful form here, because the pair alone would have supported a confident wrong
  answer. The pixels were in the right place, of the right size, and told a coherent story about
  blur arithmetic; a measurement without a control answers a question nobody asked, and it cost one
  extra run to find out.

  The fix is one line, and unlike the `ir3_cmdline` bug this one is in a file upstream still
  carries, with reproduction down two independent paths — so it is the first thing in this work
  that can actually be reported. The change is now carried here as `tools/mesa/ir3-round-default.patch`, measured on the
  phone before and after down both paths.

  **The same branch covers `TYPE_F16`, and that half is answered now: the ten `half`-returning
  declarations ran on the Adreno and agree.** Getting there took removing two wrong claims about
  them in turn. First they were reported as agreeing while being tested on almost nothing: the pool
  gave them only values that overflow `f16`, so their expectations read `[0, inf, inf, inf]`, and
  the comparison skipped every infinity without counting it, leaving the zero to carry the verdict.
  Then, with that fixed, they were set aside for a real reason — `air_run` read back through a
  32-bit float target, the half was widened at once, NIR folded the conversion into a 32-bit one,
  and the Adreno code for `air.convert.f.f16.u.i32` was four `cov.u32f32` rather than one
  `cov.u32f16`.

  The pool has the value that would decide it. In `f16` integers are exact to 2048 and spaced by 2
  above, so **2051** is the discriminator — between 2050 and 2052 with an odd lower neighbour,
  where round-to-nearest-even gives 2052 and truncation gives 2050 — and 2049 is carried beside it
  as a midpoint where both rules agree, to show that a midpoint is not automatically a test.

  **Both causes had to go, and the second one needed a source rather than a guess.** The 16-bit
  render target in `air_run` (`R16G16B16A16_FLOAT`) was necessary and not sufficient: the generator
  packed every float-ish result into a `float4` with an explicit `fpext half ... to float`
  (`intrincheck.py`, `emit_buffered`), so the widening happened inside the shader and no choice of
  target could undo it. Writing a `half4` render target instead needed
  `!"air.render_target" … !"air.arg_type_name", !"half4"` metadata, and for a while this document
  stopped there on purpose: of the 456 modules the render-target type names are `float4` (275),
  `int4` (268), `uint4` (268) and `ushort` (2), with **no `half4` among them**, and the corpus is
  the authority on that vocabulary — `air.*` meaning was read out of `AGXCompilerCore` rather than
  invented, so `half4` by analogy would have been a guess dressed as metadata.

  **The source turned up, and it is a quotation.** `GNR.videoprocessor`'s own library — same
  compiler build, same `air.version 2.5.0`, same triple — carries 24 `half` render-target
  declarations over 18 fragment entry points, in exactly that form over a `define` returning
  `<4 x half>`, and `AGXCompilerCore` carries `half4` in the same run of type names that holds
  `float4` and the `air.*` vocabulary this frontend was built from. It is absent from our 456
  because our corpus is the `.metallib` **files** under `rootfs` and that library is a blob inside
  a dylib — the sampling, not Metal. With the half4 target the same module compiles to one
  `cov.u32f16 (even)`, and the ten declarations agree on the Adreno over 1 034 cases
  (`logs/173828-run.log`, `logs/173853-run.log`).

  **The first run of them was still a pass about nothing**, which is the part worth keeping: it was
  emitted with `--cases 8`, the integer pool is ordered for f32's interesting values, and its first
  eight are all exact in `half` — so the four conversions never performed the rounding that is the
  whole of what they do. `intrincheck --check` now names a declaration that narrows to `half` with
  no case needing a rounded result, instead of counting it as agreement.

  **And there is a second half to the f16 question that no harness can settle, because nothing is
  required.** One F16 path through that `create_cov` branch is `pack_half_2x16`, and
  `nir_lower_mediump.c` says of it, in Mesa's own words, "pack_half rounding is undefined". Where
  the specification requires nothing, a truncating `packHalf2x16` is a permitted choice and not a
  fault, so a measurement of it would be an observation about an implementation rather than an
  argument about conformance — and the patch's header now scopes its case to the destinations where
  a mode *is* required, integer to float, naming the `pack_half` path as a reviewer's question
  instead of leaving it to be found. A peer session had planned to reach the F16 branch through
  `packHalf2x16` for exactly that reason and dropped it on this, which is the better call: the
  experiment would have answered our internal question and been useless for the report.

  So the f16 question is two questions. For integer-to-half, round-to-nearest-even is required and
  this instrument cannot ask. For `pack_half`, nothing is required and there is nothing to ask.

  And the spelling is the smaller problem. A `half` render target appears in **no** module of the
  corpus, so a shader built around one would exercise a path through this frontend that Apple's own
  shaders never take and that nothing else here tests. A disagreement would then sit between two
  explanations — the GPU rounds wrong, or this frontend mishandles a construct it has never seen —
  and an ambiguous measurement is worth less than a stated gap. So the ten declarations stay set
  aside, and **the rounding fix is measured for 32-bit destinations only**, which is what the
  patch's header claims and no more.

  **Which rule it is, as against merely "not round-to-nearest", is settled by one value.**
  `0x01000003` is 2²⁴+3, half way between the representable 2²⁴+2 and 2²⁴+4; round-half-to-even and
  round-half-away-from-zero both give 2²⁴+4, and the GPU gives 2²⁴+2. That is truncation and nothing
  else. `0xFFFFFFFF` alone could never have shown this, and neither could `0x01000001`, the other
  midpoint, where truncation and round-to-even happen to agree — which is why the pool carries both
  and why one midpoint is not a test of a rounding rule. What remains unestablished is only whose
  job the compensation is, and that still needs the a6xx ISA documentation.

- **`air.fast_pow` returns NaN on the Adreno for a negative base, where softpipe returns the exact
  value.** `fast_pow(-0.5, 1)` is `-0.5` in C and on softpipe, and NaN on the phone. This is
  recorded rather than reported as a bug: Metal's `fast::` functions, like OpenCL's `native_`
  ones, are explicitly undefined outside a restricted domain, and a negative base is outside it.
  It is worth naming because it sits inside the "approximate, nothing concluded" bucket, which is
  exactly where a real difference can hide behind a reasonable-looking ulp figure — the line says
  "worst inf ulp", and an inf ulp is not a rounding difference. Any corpus shader that calls
  `fast_pow` with a base that can go negative will behave differently on this phone than in any
  test on this machine.
- **The atomics compute the right thing and the barrier waits; whether the atomics are *atomic*
  is still open.** `tools/air/atomiccheck.py` dispatches two kernels written for the purpose
  (section 4, item 5) and all eight operations the corpus uses agree — five on device memory
  (`add.s`, `load`, `max.s`, `min.s`, `store`), three on threadgroup memory (`add.u`, `load`,
  `store`) across two `air.wg.barrier`s. Among them a signed minimum taken across negative
  values, which is where an unsigned one passes unnoticed. The barrier is genuinely under test:
  softpipe advances its invocations in groups of four, so making the barrier a no-op leaves every
  thread seeing 4 of 16 increments, and the check says exactly that. **Atomicity is not under
  test**, because softpipe applies a group's atomic lane by lane and so cannot lose an update
  however the lowering is written. That needs the phone. Two things are settled without running
  anything: every atomic in the corpus asks for `memory_order_relaxed`, and a stronger order is
  now refused by name instead of ignored, which is what the lowering used to do.
- **All 119 declarations are now accounted for**, which is new as of tonight and is a weaker
  claim than "all checked" — the distinction matters and an earlier draft of this paragraph, and
  the commit message of `bbf6c3c`, ran the two together:

  | | of 119 |
  | --- | --- |
  | verified: the answer was compared with one computed elsewhere and agreed | 94 |
  | measured, no verdict: approximate arithmetic, against this machine's libm | 7 |
  | has a case, which softpipe cannot run: a 16-bit result or operand | 7 |
  | has no case, for a reason given in section 4 item 6: multisample, depth, write | 11 |

  The 94 are 71 arithmetic operations, 10 atomics and the barrier, and 13 texture operations. The
  derivatives and the simd reductions are in none of these rows because **no module of the corpus
  calls them**: unexercised code, not untested behaviour, and saying "unverified" of them would
  overstate what is at stake.
- **The weakest of the three groups is the textures**, and what was missing there was never the
  fetch — checked at the corners as well as the middle, for integer and unsigned textures too —
  but everything Metal's sampler state decides. That is no longer silence: the word is decoded,
  applied and checked against Apple's own rules on 15 cases (section 3.3). What the texture rows
  above still do not cover is the filtering itself, because every coordinate in them is a texel
  centre or a clean footprint corner, where nearest has no weights to round and clamping no
  arithmetic; the six texture shaders that drew on the Adreno took their sampler from the API, so
  they are evidence about binding, descriptors and ir3's texture instructions and nothing else.
  The modes the corpus never uses — `repeat`, bicubic, the mip filters, anisotropy, the LOD clamp,
  the comparison function — are decoded and unexercised. **Ten**
  modules of the corpus declare a `constexpr sampler`, carrying three distinct state words, all
  ten in `PortraitFilters.cifilter/default.metallib`; everywhere else the API supplies it. This
  paragraph said "two" for a while, while the mapping table in section 7 said ten: two numbers for
  one fact in one file, and the disagreement was enough to settle it without any new evidence.
  Where the "two" came from is not established. What is worth recording is how the ten were
  counted, because the obvious way does not work: a plain `grep` for `__air_sampler_state` over
  the 456 modules finds **none**, since bitcode is bit-packed and a name matches only when it
  happens to land on byte boundaries. The count above is from `llvm-dis` output, module by
  module.
- **A function of the module that takes a texture: done, and the diagnosis was the work.** All
  four such entry points translate — three with this fix, the fourth once the shuffle mask it also
  stopped on was decoded. The fix is a split of `translate_globals`, and arriving at it took two
  reverts and three failed guesses, which is the part worth keeping.

  A texture is not a value in this frontend: it is resolved at translation time into a binding,
  and `tex_handle` wants an `AIR_VAL_PTR` rather than an SSA def. So a pointer-class argument is
  bound from the caller's own resolution and takes no NIR parameter, which specialises the callee
  to its call site — free here, since each of the four has exactly one. That much worked at once.

  What did not was everything after. The callee needs the module's globals, and copying the entry
  point's values into it crashed three of the four in `nir_lower_goto_ifs` → `nir_repair_ssa` →
  `nir_block_is_unreachable`. **Two hypotheses died with evidence before the right one arrived**:
  the callee has no unreachable block at all, and `nir_validate_shader` — the test a peer session
  proposed for it — fails identically on `fbs_slice_trilinear`, which translates perfectly, so
  its complaint about block indices in an unstructured impl was true and about something else.
  The peer's reading of the assertion was right and their route was not: it asks *the block's own*
  impl for dominance metadata, so a use in another function's block asks the wrong function — but
  the cross-function value arrived through the globals, not through an argument. A guard refusing
  a pointer argument with a byte offset was evaluated 26 times over the corpus and never fired, so
  arguments are not the carrier; the count is the evidence, because a silent guard and an
  unreachable guard look the same.

  So `translate_globals` is now two. `plan_globals` decides what each global is and runs **once**,
  because it appends inline samplers to `info->resources` and lays out shared and scratch memory.
  `materialise_globals` builds the values and runs **per function**, because a byte offset is an
  `nir_def` and a def belongs to the impl that made it. Module-level constants needed neither:
  `air_get` already materialises one in whichever builder asks, which was checked rather than
  assumed.

- **Of Apple's own 47 kernels, all 47 are dispatched on the Adreno and none crash — but
  dispatching is all that says.** The second row of this table has no single number for the
  Adreno on purpose: one kernel hangs the GPU and this port's driver does not recover, so how
  many results are real depends on where in the boot each kernel ran. The paragraphs after the
  table give the measurements.
  `tools/air/kernelcheck.py` prints where each one stops, as a table whose rows are checked
  against the total before it is printed:

  | | on softpipe | **on the Adreno** |
  | --- | --- | --- |
  | dispatched without being refused or crashing | 6 | **47** |
  | of those, whose written buffer came back changed | 6 | see below |
  | softpipe's `nir_to_tgsi`: scratch and intrinsics it lacks | 41 | — |
  | a shuffle mask our reader would not decode (ours, now fixed) | 0 | 0 |
  | **crashed `air_run`** (a defect here, not a limit) | 0 | **0** |

  `logs/170547-run.log`, 710 919 bytes with no truncation marker. The three that the shuffle-mask
  fix unblocked dispatched on the first run and read their images back; two of them bind six
  textures, which is well past where the sampler bug used to fault, so that fix is confirmed a
  second time by shaders it was not measured on.

  **"Dispatched" is all that row says, and the reason is the device.** `air_run` waits on a fence
  now and reports a dispatch the GPU did not finish, which turns the question from "did the
  numbers change" into "did the GPU answer". With that, one boot gives:

  | in one boot (`logs/185711`, `185825`, `190536`, all three identical) | |
  | --- | --- |
  | dispatched, and the GPU finished them | 23 |
  | **the GPU did not finish** | 24 |
  | of the 23, wrote nothing visible (and writes nothing on softpipe either) | 1 |
  | of the 23, the 4 that softpipe also runs, agreeing word for word | 4, with **0 differing** |

  So the honest figure for the kernels is **23 of 47 have produced a result the Adreno finished**,
  and the header's "47 dispatched" means dispatched: nothing refuses them and nothing crashes.

  After the first failure nothing finishes again: `gpuWait` in this port's driver latches after
  one timeout and every later wait returns false at once, and the buffers show the GPU really is
  not writing, so the rest of a boot is unmeasured rather than wrong. Three boots with different
  arrangements of the list finished **the same 23** and never the other 24, and the first failure
  landed at position 29 in each — which looked like a budget of about twenty-eight dispatches per
  boot.

  **It is not a budget, and it is not the GPU's fault either. Two kernels loop forever on what
  this harness feeds them.** Two runs found the kernels, and reading them found the cause:

  | | |
  | --- | --- |
  | the list reversed (`logs/190426-run.log`) | finished 3, then `MetalFaceMask_GenerateToothMask` at **position 4** did not |
  | eleven of the never-finishing kernels run first (`logs/190715-run.log`) | finished `fbs_bistochastize_init`, then `fbs_bistochastize_iter` at **position 4** did not |

  Four dispatches into a boot is not a budget. And both kernels have the same shape, which their
  LLVM says plainly. `fbs_bistochastize_iter` counts from one buffer value to another and leaves
  only on equality:

      %44 = phi i32 [ %52, %42 ], [ %19, %18 ]      ; starts at %19
      %52 = add nuw i32 %44, 1
      %53 = icmp eq i32 %52, %21                     ; leaves only when it equals %21
      br i1 %53, label %31, label %42

  where `%19` and `%21` are fields of a `HashEntry` **loaded from device buffer 0**. This harness
  fills that buffer with `(i * 7 + 13) & 0xff`, so the two are arbitrary 32-bit numbers. The
  earlier sentence here said the loop then "runs about four billion times whenever the second is
  smaller", and that is wrong in both halves: the loop sits behind a guard, `%30 = icmp ult i32
  %19, %21`, so when the second is smaller it is not entered at all, and when the second is larger
  the count is their difference and not a wrap-around. For thread 0 the difference can be read off
  the fill by hand: `%19` is 0 and `%21` is the four pattern bytes at offset 20 of the buffer,
  `99 a0 a7 ae` little-endian, so the loop runs **2 930 221 209** times. That is the cost, and it
  is as fatal as four billion would have been -- but a number that comes out of the fill is
  checkable, and "about four billion" was not.
  `MetalFaceMask_GenerateToothMask` is the same in a different dress: its loops walk a span table
  until they meet a sentinel `-1` (`icmp eq i16 %25, -1`), and `-1` does not occur in the pattern.

  **So the GPU is not hanging on a driver bug, an ir3 bug or a translation bug. It is faithfully
  executing an endless loop, because the harness gave these kernels noise where they expect
  structure — counts, indices, sentinels.** Any GPU would. The earlier sentence here, "particular
  kernels hang the GPU", put the blame on the port; the measurement says the instrument. Those two
  were also the two reproducers handed to the kernel line, and they are withdrawn as such.

  **Two degenerate fills get some of them to the end, and which one works says what the loop
  is.** `air_run --zero` fills every buffer with zeros and `--ones` with `0xff` bytes. A loop that
  counts up to a value from a buffer ends at once when that value is zero; a loop that walks until
  it meets a sentinel — MetalFaceMask's span tables leave on `-1` — never meets it in zeros and
  meets it immediately in `0xff`. Neither is an input these kernels were written for, so a finish
  proves the translated shader *runs* on the hardware and nothing about what it computes.

  Measured, each in a fresh boot (a hang wedges the GPU for the rest of one):

  | | with `--zero` | with `--ones` |
  | --- | --- | --- |
  | `MetalFaceMask_AddSeedPoint` | finishes | finishes |
  | `MetalFaceMask_DrawSpans` | finishes | — |
  | `MetalFaceMask_ConnectSpans` | **does not**, three boots out of three -- and the reason is now known: see below | finishes |
  | `fbs_bistochastize_iter` | **finishes**, three boots out of three | — |

  `fbs_bistochastize_iter` is the kernel this section opened with: it hung the GPU on the byte
  pattern at the fourth dispatch of a fresh boot, and with zeros it finishes every time. Its loop
  counts from one buffer value to another, and zero to zero is no iterations. So neither of the two
  kernels that looked like they "hang the GPU" hangs it: each hangs on an input this harness had no
  way to produce, and each reaches its end under the fill its own loop shape asks for. Three boots
  each, one fill each, both verdicts identical every time.

  `ConnectSpans` is the one worth keeping: it leaves its loop on `-1`, so zeros cannot end it and
  `0xff` ends it at once, and that is exactly what the two runs show. It also cost a wrong
  conclusion first: reading the `--ones` log as a `--zero` one made the kernel look
  nondeterministic, and "the hang does not repeat" would have undermined every other number here.
  Three boots with the same fill settled it. The lesson is the fill's name belongs in the log line
  — it is there now — and the log line belongs in the reading.

  **Those three boots were real and their cause is now found.** `ConnectSpans` walks a span
  table until `nextSpanID` is `-1`, and `nextSpanID` is a `ushort`: ir3 was addressing
  sub-dword SSBO access by the size of the instruction while the hardware counts in the IBO
  descriptor's texels, so every one of those reads landed four bytes apart instead of two and
  the four lists became a cycle with no sentinel in it
  (`tools/mesa/ir3-ssbo-subdword.patch`). With that patch in, the kernel finishes on the
  Adreno both ways -- given the structured input `tools/air/kinput.py` builds for it and given
  zeros -- and in both cases it changes not one byte, which is exactly what Apple's own
  instructions do with that input on the processor (`logs/102453-run.log`, and the canary
  wrote its six words all three of its turns). So the row above is superseded rather than
  wrong: the dispatch did not finish, and what did not finish was a loop the compiler had
  made endless.

  That leaves the real limit, which is the harness's, and it can be counted rather than gestured
  at. `tools/air/kstruct.py` reads every kernel and sorts them by what they do with their
  buffers:

  | | of 47 |
  | --- | --- |
  | **a loop leaves on a value from a buffer** — cannot be dispatched here at all | **14** |
  | index a buffer by a value from a buffer, with no such loop — they finish, but what they compute is addresses out of a byte pattern | 5 |
  | read their buffers as numbers, so the fill is a usable input | 28 |

  The 14 are the `fbs_*` solver iterations and the span-table walks of `MetalFaceMask_*`, and both
  kernels measured as not finishing are among them. None of the 14 finished on the device and none
  can: the loop leaves on a count or a sentinel that `(i * 7 + 13) & 0xff` does not contain, so no
  number of boots measures them. What would is a harness that knows what each buffer means, which
  is the runtime's knowledge and not the compiler's.

  **That last sentence has since been answered a different way, and this row is out of date.** The
  harness does not need to know what a buffer means to know where its fields are: the layout is in
  the module's own `air.struct_type_info`, and `tools/air/kinput.py` reads the exit values out of
  it. Eleven of the 14 now have a job line and end on the device. Section 5d has what they then
  measured, which was three compiler bugs rather than a column of agreements.

  The 5 are worth a separate row because lumping them in said the wrong thing once: four of them —
  the `FilmGrain*` pair in both formats — **did** finish on the device. A loaded index does not
  stop a kernel; it only means the addresses it computes come out of the pattern, so the numbers
  are arithmetic rather than a statement about what the kernel is for.

  And 28 of the 47 read their buffers as numbers, which is exactly the class this harness was
  built for. 23 of them have finished on the Adreno; the gap is the ones that sat after a hang.

  What does remain for the kernel line is the latch: one timeout should not kill the GPU for the
  rest of a boot. Linux's `msm` does a hangcheck, a reset and an error return; this port latches
  `gGPUReported` and answers every later wait at once (`RedwoodGPU.cpp`). That is robustness
  against bad input — worth having, and not the reason Apple's kernels do not run.

  **That latch now has a way out, behind a boot-arg — built, not yet run.** `gpu_recover=1`
  resets the 3D core's own half after a timeout, brings it back with the same two steps the boot
  uses, gives up the submission that hung and lets `gpuWait` wait again, so a hang costs one
  dispatch instead of the rest of a boot. `docs/metal.md` section 10.36 has what is done and
  which source says so; `userland/rc.jobs.metalrecover` is the measurement — the canary before
  and after three hangs of two different shapes, with both outcomes written down in advance.
  Until a boot says it works, every number in this section stands as it is and the ordering rules
  above — hangers last, a canary between jobs, nothing below a wedge is evidence — stay in force.

  This also withdraws something of its own. Two kernels that this check reported as disagreeing
  with softpipe — `image_calcminmax0` and `image_calcminmax2` — fall in the row the GPU did not
  finish, so the disagreement was never about the shaders, and with the fence in place the row
  for kernels that run and differ from softpipe reads **0**.

  Two facts recorded earlier fall under the same mechanism: `bo_allocate: alloc failed` after
  roughly 370 dispatches in one boot, and 711 modules of an over-long suite that never ran. The
  library wrappers are the control: 124 dispatches in one boot, every one of them writing
  (`logs/172758-run.log`), and not one of them binds a texture or an image — so whatever the
  cause is, it is not the number of dispatches alone. A leak of GPU memory was the first guess
  and a peer session's reading of the driver refused it: `clientClose`, `freeBuffer` and the
  per-client references do free a process's buffers, and the allocation failure after ~370 is the
  ring filling with submissions that never completed. What is left to explain is the first
  timeout, and that is the kernel's business rather than this frontend's.

  What belongs here is that the measurement knows about it: `air_run` waits on a fence, says
  "THE GPU DID NOT FINISH" and exits non-zero instead of printing the buffer it filled itself; it
  also names a buffer that came back bit-identical to the fill; `kernelcheck` gives both their
  own rows and reads the canary; and a suite meant to produce *values* rather than *capability*
  has to be split into boots of about twenty kernels.

  **The deferred question is answered: ir3 runs them.** 41 kernels stop at a software rasteriser
  that has no scratch memory and lacks compute intrinsics; on the hardware every one of the 47
  dispatches and reads its result back, images and all. The phone replaced softpipe, and both
  columns that were ours — the harness's refusals and the frontend's — are empty.

  What the rows do **not** say is right. There is no executed reference for these kernels
  anywhere: softpipe refuses 41 of them, which is why they came here, so a number out of the
  Adreno has nothing on this machine to disagree with. 47 have been shown to *run*, which is a
  statement about capability and not about correctness.

  **The last row was blaming the shader for our own gap.** "A shuffle mask that is not constant"
  was impossible as written: LLVM requires a `shufflevector` mask to be constant, so the message
  was describing something that cannot occur. What the three kernels actually carry is a mask like
  `<i32 0, i32 poison, i32 1, i32 poison>` — the shape a Metal `half2`-to-`half4` widening
  compiles to — and in it the zero is written `CST_CODE_NULL`, not `CST_CODE_INTEGER 0`, because
  that is how LLVM stores every zero of a scalar type. A mask is stored as an aggregate of
  element constants rather than as plain data exactly when one lane is poison, so those two
  encodings meet only in this case, and our reader accepted only the second. We refused on a zero
  we could read perfectly well, and recorded it as a limit of the shader.

  **And the decoder for it already existed.** `air_const_uint` (`air_module.h`) has read both
  spellings since long before this, with a comment beside one of its callers saying that a zero
  arrives as `CST_CODE_NULL` about as often as not. Four places tested `ckind == AIR_CONST_INT`
  by hand instead of calling it — the shuffle mask, the `llvm.memcpy` and `llvm.memset` lengths,
  and the `air.wg.barrier` flags — and the shuffle mask is the one where it cost something. All
  four go through the helper now, which removed more code than it added.

  The first write-up of this fix claimed it "closes a class" by adding that decoder. It did not:
  the class was already closed and one reader had opted out of it, which is a different and less
  flattering story. Worth keeping for the shape — a hand-rolled test beside an existing helper
  that does the same job, in a file big enough that neither author sees the other.

  The four that used to crash now dispatch, and the crash was this harness's:

      void *csampler = NULL;
      pipe->bind_sampler_states(pipe, PIPE_SHADER_COMPUTE, 0, num_tex, &csampler);

  `bind_sampler_states` reads `num_tex` pointers from the address it is given, and `&csampler` is
  the address of *one*, so it reads `num_tex - 1` words past that variable and stores whatever is
  there as `samplers[1..]`. The draw path beside it has always built a proper `void *samplers[]`
  array, so the new code failed to learn what the old code already knew. The fault was at
  `fd6_texture_state + 0xd4`, reached through `fd6_emit_cs_state` from `fd_launch_grid`: a wild
  pointer plus a small field offset, which is exactly the `0x100000000034` that was being
  dereferenced.

  **What that mechanism does and does not predict.** Both of freedreno's loops skip an empty slot
  (`if (!tex->samplers[i]) continue;` in `fd6_texture_state`, a zeroed dummy in the emit path), and
  `fd_sampler_states_bind` sets a slot's valid bit only for a non-NULL pointer — so a stack word
  that happens to be zero is read, stored and harmlessly ignored. The read goes out of bounds at
  `num_tex >= 2`; the *fault* needs one of those words to be non-zero. So the texture count is not
  a threshold, and the pre-fix run says so plainly (`logs/155708-run.log`): eight kernels with two
  textures (`BlitKernel_Unorm`, three `BlitLuma*`, `MetalFaceMask_GenerateMask`, `confidence`,
  `fbs_bistochastize_final`, `fbs_slice_trilinear`) and two with three (`BlitChromaKernel_Uint`,
  `BlitChromaKernel_Unorm`) all dispatched while the read was still out of bounds for every one of
  them. The four that died read 4, 4, 6 and 6. Said as a threshold of two, this would predict a
  crash for ten kernels that did not crash.

  **The feature that looked discriminating was a confound, and the run settled it.** Exactly 4 of
  the 47 kernels carry an inlined module-local function; three crashed and the fourth was refused
  earlier on the shuffle mask, so every one that reached dispatch died — 3 of 3, which is not a
  correlation so much as an invitation. `spillCorrectionPreProcess` carries no call at all and
  died with them, and all four now dispatch with the inlining path untouched. Had the inlining
  been "fixed" instead, three might have gone through and the fourth would have been left needing
  a second cause.

  Two other explanations were already dead before this one, and killing them was what left the
  adjacent line to be read: the driver's texture state is not uninitialised
  (`CALLOC_STRUCT(fd6_context)` zeroes it), and it was not "an image with no buffer", because
  nine kernels of exactly that shape — one image, written — dispatched throughout
  (`logs/155708-run.log`). A peer session's reading of the same
  function proposed `view_seqno[16]` overflowing against `textures[32]` — a real fragility in
  freedreno — and it is not reachable from here, because this harness refuses a slot index of 16
  or more by name. Checking whether someone else's bug was reachable, instead of looking one line
  further for our own, is how two of the three earlier attempts on this function were spent.

  `air_run` now prints the texture count beside the image count, which is what made the paragraph
  above a measurement instead of an argument — and the first thing written from that run was still
  the wrong generalisation, "two is the threshold", corrected with a guess ("there are surely
  kernels reading two among the forty") when the counts were already in the log. A peer session
  counted them. That is where accuracy usually goes: not in the first claim but in its repair,
  which arrives feeling like diligence and is rarely re-checked against the same evidence.

  The row for a call of a module-local function was 5 until those calls were translated; it is now
  gone, its five having moved to softpipe (four) and the shuffle mask (one) — and that last one
  moved again when the mask was decoded, so all five are softpipe's. Twice in a row the
  dispatched count on softpipe stayed at 6 while a column stopped being ours, and the run above is
  where that finally paid: those same kernels dispatch on the hardware. Until 20 October, 24 of these were
  refused by `air_run` itself — it bound textures and images for a draw and not for a dispatch —
  and binding them moved the count only from 5 to 6, because softpipe is the wall behind ours.
  What changed is whose wall it is: **nothing is refused by this harness any more**, and the 34
  are stopped by a software rasteriser that lacks scratch memory and some compute intrinsics,
  both of which ir3 has. They are a question for the phone now rather than a question for us,
  which they were not before. Claiming 24 unblocked would be false; they moved from one wall to
  another and the second is not ours.

  Images are read back after a dispatch, which also matters for the textures: most of these
  kernels write *only* an image, so without the readback a dispatch would have reported success
  with nothing examined — and that is the shape this check has been caught by three times
  (docs/checks.md in hakios-xnu-kernel). The readback is what `air.write_texture_2d/3d` needed
  as well, which `texopcheck.py` had listed as having no case for exactly this reason.

  So the atomics being checked (above) is still a statement about two kernels written here for
  that purpose, not about the 17 of Apple's 44 that use one.

**And every number above is about Apple's own shaders.** The first library that is not Apple's has
now been put through the same meter — the 7 879 Metal libraries inside Resident Evil 4's
`re_chunk_000.pak`, which is the game this port's owner named as the acceptance test. Reader
**7 879 of 7 879**, translate **258**, ir3 **258**, and the whole of that difference is one
interface word: 7 620 of them bind their resources through a Metal argument buffer
(`air.indirect_buffer`), which no module of the system corpus uses and this frontend does not
read. Section 8 is that census, the list of operations it calls that are not implemented, and what
the 258 is and is not.

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
  CE_CAST), 13 type records and 8 metadata records. No invoke, no atomics as
  instructions (they are `air.*` calls), no `ptrtoint`/`inttoptr`, no `addrspacecast`, no debug
  info. The bitstream container itself is documented ("LLVM Bitcode File Format") and is a few
  hundred lines.

  **This census was a census of one corpus, and a second corpus corrected it.** It used to say
  "no switch" as well, and `QuartzCore.framework/default.metallib` — the library that draws the
  phone's own interface — has four modules with `FUNC_CODE_INST_SWITCH` (record 12) in them: the
  GL-on-Metal fixed-function shaders, which switch on their texture-combine modes. They were the
  only four of 141 our reader could not read at all. Record 12 is read now, and the two ways its
  operands are numbered are the part worth writing down, because reading them alike is wrong in a
  way nothing would report: the **condition is a relative** value number, as `pushValue` writes
  it, and each **case constant is absolute**, as `getValueID` writes it. The blocks are block
  numbers, like `br`'s. A constant expression `getelementptr` was the other thing that corpus had
  and the first did not.
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

A constructor of **one block** is translated in place, at the top of the entry point. One with
branches cannot be, because the CFG walk owns the impl's start block and running it twice in one
impl would map two AIR blocks onto it — so a branching constructor becomes a `nir_function` of its
own and the entry point calls it, which `nir_inline_functions` folds back in. The stores it makes
and the loads the entry point makes reach the same memory because `plan_globals` decides one
layout for the whole module. QuartzCore's fixed-function fragment shaders need this: their
constructor decides **58** function constants and has a `switch` in it.

**Pointers.** Metal buffers are pointers, and AIR passes them as `ptr addrspace(N)` parameters.
The translator keeps every pointer as *(where, byte offset)*: *where* is a buffer binding, the
threadgroup block, a private allocation or a constant global; the offset is an SSA value computed
from GEPs with the datalayout's sizes. This is NIR's `32bit_index_offset` model and is enough for
the whole corpus (no pointer is ever converted to an integer or between address spaces). Pointers
stored to memory — argument buffers, which no system shader here uses — will need 64-bit global
addresses instead (7.2).

**That last sentence was true of one corpus and is not true in general, and six modules of
QuartzCore's library say so.** The GL-on-Metal "uber" shaders (`fixed_vert_*`, `fixed_frag_lph_c*`)
use a buffer pointer as an ordinary value: they `select` between a bound buffer and a module
constant array at run time, and they `alloca` a pointer and store one into it. Neither is
expressible as *(where, byte offset)* decided at translation time, and both are refused with
"a pointer where a number belongs". What it would take is below, under the walls those six are
behind.

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
| `!air.sampler_states` (inline `constexpr sampler`) | 10 | inline sampler, index after the API ones | the state word is decoded and applied, and checked against Apple's rules (section 3.3) |
| `!air.function_constants` | 11 | constant folded from the values given to `air_to_nir` | `!{ptr @g, type, name, index, i1}` |
| `air.struct_type_info`, `air.arg_type_*` | | reflection only, not used for code | layout comes from the LLVM types |

**Vertex stage**

| AIR | NIR |
| --- | --- |
| `air.vertex_input` + `air.location_index n` | `shader_in` at `VERT_ATTRIB_GENERIC0 + n`, always 32-bit and narrowed after; `inputs_read` and `air_shader_info.vertex_attributes` (see below, and the design for the `MTLVertexDescriptor` half) |
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
| `air.center` / `air.centroid` / `air.sample` | plain / `centroid` / `sample` on the variable (`air.center` is the default and is therefore read by not being looked for) |
| `air.perspective` / `air.no_perspective` / `air.flat` | `INTERP_MODE_SMOOTH` / `NOPERSPECTIVE` / `FLAT` |
| `air.front_facing` | `load_front_face` |
| `air.sample_id`, `air.sample_mask` (input) | `load_sample_id`, `load_sample_mask_in` |
| `air.point_coord` | `load_point_coord` |
| output `air.render_target i32 n, i32 idx` | `FRAG_RESULT_DATA0 + n`, dual-source index `idx` |
| **input** `air.render_target i32 n` (`[[color(n)]]`, framebuffer fetch) | `load_output` with `io_semantics.fb_fetch_output` at `FRAG_RESULT_DATA0 + n`, always four `f32` and narrowed after; `info.fs.uses_fbfetch_output` |
| output `air.depth` + `air.depth_qualifier any/greater/less` | `FRAG_RESULT_DEPTH`, `info.fs.depth_layout` |
| output `air.sample_mask` / `air.stencil` | `FRAG_RESULT_SAMPLE_MASK` / `FRAG_RESULT_STENCIL` |
| `air.discard_fragment` | `demote`, and `info.fs.uses_discard` set by hand (nothing on this path runs `nir_shader_gather_info`). **Not `terminate`** — see 8.5 for the two readings that decide it and for what the Adreno does with either |

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
| `air.simdgroups_per_threadgroup` | `load_num_subgroups` — ir3 computes it rather than reading a register, `1 + ((threads_per_threadgroup - 1) >> the SIMD-width shift)`, which folds to a constant for a fixed workgroup size |

The scalar-or-vector type of each of these (`uint`, `uint2`, `uint3`, `ushort…`) comes from the
LLVM parameter type, and the NIR value is cut or converted to it.

**Values and operations**

| AIR / LLVM | NIR |
| --- | --- |
| `i1/i8/i16/i32/i64`, `half/float/double`, vectors ≤ 4 | the same bit sizes; wider vectors are split |
| structs and arrays as SSA values (`insertvalue`, `extractvalue`) | trees of SSA values inside the translator, never NIR values |
| `br`, `phi` (unstructured CFG) | an unstructured `nir_function_impl`, then `nir_lower_goto_ifs` — the path Mesa's OpenCL frontend uses |
| `switch` | NIR has none: a chain of `nir_goto_if`, one test per case in a block of its own, the last falling to the default. Checked against LLVM on the processor (`tools/air/loweringcheck.py`) |
| a call returning a **struct** | one out-parameter per member, not one for the value: a NIR parameter carries a number. A member that is not a number (the `[8 x i8]` tail padding Metal puts on a vertex-output struct) takes no parameter and nothing may read it |
| a constant-expression `getelementptr` | the base pointer with a constant byte offset and a narrowed pointee, the index walk done in integers |
| `select` between two **pointers** | the choice is kept (`AIR_VAL_PTR_SELECT`); a later index is pushed into both sides and a load through it loads **both** and selects the values, which leaves a note |
| a **pointer stored** to memory | refused, by name: a pointer here is a resolution and not an address, so there are no bytes to write |
| `alloca` | `load_scratch`/`store_scratch` on a per-invocation block (ir3 private memory) |
| `addrspace(2)` global `constant` with initializer | `nir->constant_data` + `load_constant` |
| `addrspace(2)` global without `constant` (program-scope variables) | a private copy per invocation (written by the constructors) |
| `fast` flags on an instruction | the NIR ALU is not `exact`; without them it is |
| `air.<op>.<type suffix>` | one handler per op in `air_intrinsics.c`; the suffix is checked against the LLVM types, not trusted |

**`air.render_target` is two different things, and the second one is framebuffer fetch.** As an
*output* it is where a colour goes. As an *input* -- a parameter whose metadata node carries the
key -- it is Metal's `[[color(n)]]`, the value render target `n` holds *now*, read inside the
fragment shader. Metal calls this programmable blending. Which feature it is did not have to be
inferred from the name: every module that uses it carries
`!"air.compile.framebuffer_fetch_enable"` among its own `air.compile_options`, the option Apple's
compiler sets exactly when a fragment argument is `[[color(n)]]`.

The two forms differ in *shape* as well as in meaning, and reading one with the other's offsets
would quietly produce a dual-source index out of an argument name:

```
output  !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"color0"}
input   !{i32 1, !"air.render_target", i32 2, !"air.arg_type_name", !"float",  !"air.arg_name", !"d0"}
```

The output form has two integers after the key, the target and the dual-source index; the input
form has one, the target, after the parameter number. The argument's name is always `d0`.

**NIR has this, and so does the a6xx.** NIR's form of a framebuffer fetch is a *read of the
output*: `load_output` carrying `io_semantics.fb_fetch_output`, which is what `nir_lower_io` makes
of a GLSL `inout` colour and what `nir_lower_blend` emits for a blend it has to lower in software.
The frontend emits that intrinsic directly rather than a second `nir_var_shader_out` variable at
the same location, because both of this frontend's consumers lower IO before the backend sees it
and two output variables at one location would take two driver locations and two entries in ir3's
output table for one attachment. From there the road is Mesa's own:

| | |
| --- | --- |
| `nir_lower_fb_read` (`ir3_nir_post_finalize`) | rewrites `load_output` into a `nir_texop_txf_ms_fb` at the integer fragment coordinate, sample id and layer |
| `ir3_compiler_nir.c` | compiles that to `OPC_ISAMM` with `s2en`, sets `so->fb_read`, and takes the descriptor from `ir3_compiler_options.bindless_fb_read_{descriptor,slot}` |
| `freedreno_screen.c` | `caps->fbfetch = screen->max_rts` and `caps->fbfetch_coherent = true` on a6xx — the hardware can do it for every attachment |
| `fd6_image.cc`, `fd6_gmem.cc` | reserve a bindless texture slot per render target and patch a descriptor into it per batch: GMEM's points at the tile buffer, sysmem's is a 2D view of the attachment |
| `fd6_emit.cc` (`build_prim_mode`) | `FLUSH_PER_OVERLAP` in GMEM when the fetch is coherent, `FLUSH_PER_OVERLAP_AND_OVERWRITE` in sysmem — which is what makes the read see the primitives already drawn |

So nothing had to be invented on the hardware side. Three things the frontend has to get right,
all of them because of what the lowering does rather than what the metadata says:

- **Four 32-bit floats, always.** `nir_lower_fb_read` fixes its `txf_ms_fb` destination at `4 x
  f32` and rewrites the intrinsic's uses with it, so the intrinsic is emitted with that shape
  whatever the argument's type and the narrowing is done beside it: a `float` target (the coverage
  mask of `path_blit_frag`) loses three channels, a `half4` one (every `_lph` build) is converted
  with `f2f16`. Not with the `fit()` helper the system values use -- that widens with `u2f`, which
  would have read the float's bits as an integer.
- **`info.fs.uses_fbfetch_output` by hand.** `nir_shader_gather_info` would set it from the
  intrinsic, and ir3 runs that pass only for a binning variant, so on this path nothing would.
  Without the flag the draw gets no flush at all and the read returns whatever the tile buffer
  happened to hold -- a wrong picture from a shader that compiled.
- **An integer target is refused, not converted.** The lowering hands back floats converted from
  the attachment's format, so an `int4` or `uint4` `[[color(n)]]` would arrive as the wrong
  *value* and nothing would say so. The corpus declares `float4`, `half4` and `float` and nothing
  else, so this refusal guards a case not yet seen rather than hiding modules behind it.

**What is not checked, and cannot be here.** No value. `cpuref.py` runs the module under LLVM on
the processor, where there is no framebuffer; `air_run` draws it, but on softpipe, and softpipe's
TGSI interpreter does not implement `FBFETCH` at all (the opcode exists in `p_shader_tokens.h` and
`tgsi_exec.c` has no case for it). What `tools/air/fbfetchcheck.py` checks instead is the shape --
the intrinsic at the target the module names, the flag set, the narrowing done with a float
conversion, an integer target refused, an `air.arg_unused` one not turned into a fetch at all --
and that ir3 emits the instruction:

```
isamm.a.s2en.uniform.base0 (f32)(x)r2.z, r1.x, r2.x
```

That is `path_blit_frag_lpf`'s framebuffer read on the Adreno, with `r2.x = 2` the render target
and `r1.x, r1.y, r1.z` the integer fragment coordinate and the layer. **It compiled; whether it
returns the right pixel is a question only the phone can answer.**

#### The two things known about the fetch: one is closed, one cannot be closed here

Both were found while making the fetch work. They are written here rather than in a commit
message because a gap that exists only in a report is a gap nobody finds again -- and because one
of them turned out to be the opposite of what the first version of this section said.

**1. `air.raster_order_group` was dropped in silence. It is not any more.** Metal's
`[[raster_order_group(n)]]` on a resource means accesses to it from overlapping fragments happen
in rasterization order; `inplace_copy_{lpf,lph}` put it on the texture they write:

```
!21 = !{i32 4, !"air.texture", !"air.location_index", i32 3, i32 1, !"air.write",
       !"air.raster_order_group", i32 0, !"air.arg_type_name", !"texture2d<float, write>", …}
```

*Why it was invisible, which is the part worth keeping.* The branches of `translate_input` match
on the resource's **kind** and return, so the loop at the end that refuses an `air.*` key nobody
handled never runs over a node whose kind is recognised. Every other key in such a node is
therefore invisible unless something looks for it by name. That is the one place in the interface
where an unhandled key was dropped instead of refused, and it is a property of the branch order
rather than a decision anyone made.

*What a6xx can do, and it is not what was asked.* The ask is per **resource**. The only knob the
hardware has is per **draw**: `GRAS_SC_CNTL.SINGLE_PRIM_MODE`, whose `FLUSH_PER_OVERLAP` and
`FLUSH_PER_OVERLAP_AND_OVERWRITE` values the register documentation describes for coherent
blending, and which freedreno drives from `pipe_blend_state.blend_coherent` (`fd6_emit.cc`,
`build_prim_mode`); turnip uses the same field for
`VK_EXT_rasterization_order_attachment_access`. So a runtime that sets `blend_coherent` for any
pipeline whose shader declares a group gives the shader **more** synchronisation than it asked
for -- safe, and slower. There is no way to give exactly the ask and none to give less.

*So the shader is translated and the difference is said out loud.* `air_resource` now carries
`raster_order_group` and `air_shader_info` a `raster_order_groups` bitmask, and every resource
with a group leaves a **note**: a third thing beside a refusal and a clean translation, for a
shader that was accepted while something other than exactly what it asked for was done.
`air2nir --report` prints notes on its line and `tools/air/corpus.py` counts them under "What was
translated with a caveat", so a caveat is a number rather than a sentence in a document. A group
number past the bitmask's 32 bits is **refused**, because recording it would fold it onto a
different group, which is worse than not recording it.

**2. The layer the fetch reads is undefined, and the one-line fix this document claimed is
wrong.** `nir_lower_fb_read` builds its `txf_ms_fb` coordinate as `(x, y, layer)` with the layer
from `nir_load_layer_id`, and `ir3_nir_lower_layer_id` turns that into a **flat varying read of
`VARYING_SLOT_LAYER`** -- visible in the Adreno listing as `flat.b (ei)r1.z, 0, 0` and in ir3's
own input list as `r1.z (VARYING_SLOT_LAYER slot=22 ...)`. No vertex stage in this corpus writes
`air.render_target_array_index`, so `ir3_find_output` misses, `ir3_link_shaders` substitutes
`default_regid` -- `regid(63, 0)` on a6xx, since `fd6_program.cc` passes `pack_vs_out = true` --
and `ir3_link_add` then adds **no** variable entry for it, only the varmask bit. Nothing writes
that VPC location and the fragment shader reads it anyway.

*The fix this document proposed was one line: use `nir_imm_int(b, 0)` when the shader's
`info.inputs_read` has no `VARYING_BIT_LAYER`. It would break layered rendering, and the reason
is worth more than the patch would have been.* There are two cases with the **same fragment
shader**:

| | FS fb-reads, declares no `gl_Layer` | what the fetch should read |
| --- | --- | --- |
| nothing writes the layer | varying undefined | layer 0 |
| the vertex stage writes `gl_Layer` | `ir3_nir_lower_layer_id` invents the input, and `ir3_find_output` **matches it by slot** (`ir3_shader.h`), so the link works | the rasterized layer |

The second case is a legal GL program -- `EXT_shader_framebuffer_fetch` on a layered framebuffer
whose geometry stage sets `gl_Layer` and whose fragment shader does not read it -- and it works
today. The patch would have made it read layer 0. **No fragment-shader-only transform can tell
the two apart, because the fragment shader is byte-identical in both**, so the information the
fix needs is not in the module `nir_lower_fb_read` is handed. That is the whole answer, and it is
why no patch is in `tools/mesa/`.

*What the correct fix costs.* The information exists at **link** time: `ir3_link_shaders` knows
`ir3_find_output` missed. It cannot change the already-compiled fragment shader, so the fix has
to be on the producing side -- a variant bit (an `ir3_shader_key` flag like the existing
`ucp_enables`) that makes the last geometry stage store 0 to `VARYING_SLOT_LAYER` when the
fragment shader reads it and the stage writes none. That is a vertex variant per fragment shader,
which freedreno already does for clip planes, so the machinery is there; it is a driver change
and not a one-liner, and it buys only layered render targets, which nothing in this corpus has.

*Meanwhile the condition is in the log, not in a document.* Every framebuffer-fetch shader now
leaves a note saying that the layer comes from `VARYING_SLOT_LAYER`, that a layered target needs
the pipeline's vertex stage to write `air.render_target_array_index`, and that a single-layer
target is unaffected because there is no second layer in the descriptor to reach.


#### QuartzCore's library: what the last six modules are behind

The census of `QuartzCore.framework/default.metallib` is the one the port's own interface depends
on, and it moved from 102 translate / 100 ir3 of 141 to **135 and 135** — reader **141 of 141**.
What that took, in the order the groups were worked:

| modules | what stopped them | what it was |
| --- | --- | --- |
| 20 | `air.render_target` as an input | framebuffer fetch (above) |
| 8 | "an aggregate where a number belongs" at a call | a callee returning a struct: `path_cubic_vert_*`, `path_stroke_curve_vert_*` are each a wrapper around one `internal fastcc` function that returns a vertex-output struct, and `emit_return` was asking for the whole struct as a register |
| 4 | the reader: instruction record 12 | `switch` (section 2) |
| 2 | `ir3_context_error`: "Unhandled ALU op: f2u1" | `air.convert.u.i1.f.f32` — a **bool destination is not a conversion**. C's rule, which MSL inherits, is `x != 0`; asking `nir_f2uN` for one bit gives `f2u1`, which ir3 has no instruction for and no lowering of. The comparison is also the right answer: a conversion would have a rounding mode and `bool(0.5)` is true |
| 1 | `air.simdgroups_per_threadgroup`, then `air.simd_sum` | `load_num_subgroups` and `nir_reduce` with `nir_op_fadd`, both of which ir3 has |
| — | `air.is_function_constant_defined` | folds to a constant: whether the caller gave a value for that index. Its argument is a **pointer**, so it is handled before the arithmetic dispatcher, which resolves every argument as a number — that is why the four `fixed_vert_*` failed with no instruction named |
| — | `air.normalize_function_constant_predicate` | `x != 0`, and **the corpus cannot tell that from the identity**: every argument it is given is a `zext i1`, already 0 or 1. A byte of 2 would discriminate and nothing produces one. The name is in Apple's vocabulary list; no string or symbol in `AGXCompilerCore` says what it computes |
| — | `air.unpack.unorm4x8` | `nir_unpack_unorm_4x8`; ir3 sets `lower_unpack_unorm_4x8` and `nir_opt_algebraic` turns it into four `extract_u8` and a divide |
| — | a **vector** function constant | `uchar3` opcode triples. `air_constant_value` carries one 64-bit word, so component *i* is at bit *i*·width — the word a memcpy would produce on this little-endian target. **Nothing measures that packing**: every harness here supplies zero for every constant, so all components are zero whatever the packing is |
| — | a name 37 characters long | `split_name`'s per-part buffer was 32, so `air.normalize_function_constant_predicate.i8` was refused as "a name this cannot take apart" — which reads like a malformed module and was a buffer |
| — | a constant-expression `getelementptr` | the base pointer with a constant byte offset; the census in section 2 said the only constant expression in the corpus was a pointer bitcast, and that was a census of one corpus |
| — | a static initializer with branches | translated as a `nir_function` the entry point calls, rather than in place (section 3) |
| 4 | a pointer chosen at run time | `select i1 %c, ptr %a, ptr %b` and a `getelementptr` on the result. The choice is **kept** and made where the pointer is used: a load through it loads **both** sides and selects the values, and a later index is pushed into both. Checked against LLVM on the processor, each side taken once (`tools/air/loweringcheck.py`) |

**Seven of those rows bought nothing, and that is the useful part of the ledger.** Everything from
`air.is_function_constant_defined` down was found by peeling walls off the same six `fixed_*`
modules one at a time — the pointer dispatch, the 32-byte name buffer, the constant `getelementptr`,
the branching constructor, the vector function constant — and after all of it the count was
unchanged at 130/130, because the wall under all of them is the one below. Each piece is a real fix and stays;
none of them is progress on the number, and a group that moves no module is a group to stop rather
than to finish.

**And the 456-module corpus did not move, which is a fact with a reason rather than luck.** Of
everything above, exactly one group is reachable from that corpus at all — the struct-returning
call, which six of its entry points use — and `check.sh` was run on its own after that group and
came back with every counter at its baseline. Every later change is unreachable from those 456
modules, counted over `air2nir --ll` of all four libraries: **0** calls of
`air.is_function_constant_defined`, `air.normalize_function_constant_predicate`, `air.unpack.*`
or any `air.simd_*`; **0** `air.convert` with an `i1` *destination* (the two that mention `i1`
have it as the source); **0** vector function constants; **0** constant-expression
`getelementptr`; **0** `switch`; **0** `select` between pointers and **0** pointers stored to
memory; **0** `air.raster_order_group`; and of the eleven modules with an `air.static_init`
constructor, **0** whose constructor has a branch, so none of them takes the new callee path. The
reader is the one half that every module goes through, and `reader-check.sh` still prints all 456
identically to `llvm-dis`.

Those eleven constructor modules are named, because a different road to the last six would have
changed their path and this one does not: `BlitLumaKernel_{Uint,UintPacked,Unorm}`,
`BlitChromaKernel_{Uint,UintPacked,Unorm}`, `BlitKernel_Unorm`,
`FilmGrainLumaKernel_{Uint,Unorm}`, `FilmGrainChromaKernel_{Uint,Unorm}`, all of
`AV1SW.metallib`. Each was run on its own before and after — **NIR and Adreno machine code
identical for all eleven**.

Two kinds of refusal are left, and neither is a gap that more frontend work closes.

**Two modules: a pointer STORED into memory, which is not the wall the plan expected.** This
section used to say that all six GL-on-Metal uber shaders were behind one thing and that the road
was to fold the specialisation — every `select` between two buffers in them is controlled by a
byte only the `air.static_init` constructor writes, from function constants, so a constant
interpreter over that constructor would make the condition a constant and delete the select, the
way Metal's own pipeline specialisation does. **Reading the modules instruction by instruction
said otherwise, and four of the six are done without any folding at all.**

What the four vertex shaders needed was the pointer *choice* kept rather than resolved:

```
%13 = select i1 %12, ptr addrspace(2) getelementptr inbounds ([2 x <4 x float>],
                       ptr addrspace(2) @_ZL21unused_texture_matrix, i64 0, i64 0),
                     ptr addrspace(2) %6
%17 = getelementptr inbounds %"struct.metal::matrix.5", ptr addrspace(2) %13, i64 0, i32 0, i64 0
%18 = load <4 x float>, ptr addrspace(2) %17
```

A pointer here is *(where, byte offset)* resolved while translating, which is what lets the
frontend emit `load_ubo` at Metal's own index with no descriptor indirection. A pointer chosen at
run time has no such resolution — so the choice itself is the value (`AIR_VAL_PTR_SELECT`), a
later index is pushed into both sides, and a **load** through it loads both and selects the
values, which is the only form NIR has. The folding was not needed for this and would not have
been enough for the rest.

*It is not free of consequence, so it leaves a note:* both sides are read, so a side the pipeline
left unbound is read anyway and its value discarded. In these shaders one side is a module
constant array of NaNs and the other a buffer the pipeline binds, so both are readable.

**And the two fragment shaders are behind a chain the folding would not have touched.** They
begin by storing two of their arguments into locals:

```
%37 = alloca ptr addrspace(2)
       store ptr addrspace(2) %34, ptr %37
%227 = bitcast ptr %37 to ptr
%228 = select i1 %144, ptr @_ZL14unused_sampler, ptr %227
%229 = getelementptr inbounds %"struct.metal::sampler", ptr %228, i64 0, i32 0
%230 = load ptr addrspace(2), ptr %229
```

A translation-time slot for that `alloca` was written and then **removed**, because it closed
nothing: following the chain past it reaches `load ptr addrspace(2)` — a **pointer loaded out of
memory** — and then a *sampler chosen at run time*, since `@_ZL14unused_sampler` is
`constant %"struct.metal::sampler" { ptr addrspace(2) @__air_sampler_state }` and the loaded
pointer is a sampler handle. A `nir_tex_instr`'s sampler is an index, not a value; a chosen one
needs the bindless form this frontend does not emit. So the refusal stops at the first link of
the chain and now **names it** — "a store of a pointer into memory, which this frontend has no
address to write" — instead of reporting the symptom "a pointer where a number belongs" at a
store whose value happened to be a pointer.

What the full road would cost, if these two are ever wanted: pointers as values — publish each
binding's GPU address and lower buffer access to `load_global`, which a6xx has — **plus** a
bindless sampler path in the frontend and the runtime. That is two features for two modules, and
the frontend would still need the slot that was removed. It is a clear stop rather than a step.

**Four modules: `air.imageblock`.** `tile_downsample_{1,2,4,8}` are Metal **tile shaders** — a
kernel dispatched inside a render pass that reads the tile's imageblock:

```
!17 = !{i32 0, !"air.imageblock", !"implicit", !"air.struct_type_info", !18, …}
!18 = !{i32 0, i32 8, i32 0, !"half4", !"color", !"air.render_target", i32 0}
       %8 = call <4 x half> @air.load.implicit_imageblock.v4f16(i32 0, <2 x i16> %7, i32 0, i16 0)
```

The imageblock *is* the colour attachment's tile storage, and the kernel reads a 2×2
neighbourhood of it and writes a texture. **a6xx cannot do this, and the source is the absence of
it:** Mesa 25.0.7 has no tile-memory or tile-shading support anywhere in `src/freedreno` or
`src/freedreno/vulkan` (the only `QCOM_TILED*` names in the tree are image modifiers), and the one
instruction that reads the tile buffer is fragment-only by assertion —
`ir3_compiler_nir.c`: `if (tex->op == nir_texop_txf_ms_fb) compile_assert(ctx, ctx->so->type ==
MESA_SHADER_FRAGMENT);`.

What the **driver** would have to do instead: break the render pass, resolve the attachment to
memory, and dispatch an ordinary kernel that samples it as a texture — which is exactly the
round trip tile shading exists to avoid, so the cost is one extra store and load of the whole
attachment per use, plus the pass split. The frontend's half would be small (lower
`air.load.implicit_imageblock` to a texture fetch at the given coordinate) and would be
**wrong on its own**: it would read memory where the shader asked for tile storage, which is only
the same value after that flush. So it belongs to whoever writes the pass-splitting, not here.

#### Vertex attributes: `[[stage_in]]`, and the design for the half that is not the frontend's

A vertex shader gets its vertices one of two ways. Either it reads them out of a buffer it indexes
itself with `air.vertex_id` — which needs nothing of the API but a buffer binding, and which is
what **every** vertex shader this project had met does: Apple's three in the GL renderer, and
twenty-five of QuartzCore's thirty-nine — or it declares a `[[stage_in]]` struct whose members
carry `[[attribute(n)]]`, and then the vertices come out of the vertex fetch hardware, laid out by
an `MTLVertexDescriptor` that lives in the API and not in the module. An ordinary application's
does the second. **942 of Resident Evil 4's 1 127 vertex modules do**
(section 8), so this is not a corner of the interface.

**AIR has no stage-in struct.** Apple's compiler flattens it: every member becomes an ordinary
parameter of the entry point with a node of its own, and the struct type does not appear in the
module at all. QuartzCore's `std_vert0_lpf`, which is where this was read rather than inferred:

```
define <{ <4 x float>, <4 x float>, <2 x float> }> @std_vert0_lpf(
    <4 x float> %0, <2 x float> %1, <4 x float> %2, ptr addrspace(2) %3, ptr addrspace(2) %4)
!19 = !{!20, !21, !22, !23, !24}
!20 = !{i32 0, !"air.vertex_input", !"air.location_index", i32 0, i32 1,
        !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!21 = !{i32 1, !"air.vertex_input", !"air.location_index", i32 1, i32 1,
        !"air.arg_type_name", !"float2", !"air.arg_name", !"texcoord0", !"air.arg_unused"}
!22 = !{i32 2, !"air.vertex_input", !"air.location_index", i32 3, i32 1,
        !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
```

Four things in that are each a wrong guess away from a shader drawn over the wrong vertex:

- **the index is Metal's `[[attribute(n)]]` and not a position in the list.** These three are
  0, 1 and **3**. A translator that numbered them in order would feed `color` from attribute 2,
  silently, and every synthetic test with one attribute would still pass.
- **`air.arg_unused` occurs on an attribute**, and on 8 of the 46 in QuartzCore's library. An
  attribute nothing reads needs no fetch and no binding, and Metal does not require the
  application's descriptor to configure one.
- **the attribute can be gated by a function constant.** `fixed_vert_lph_spc` carries
  `!{i32 1, !"air.function_constant", !31, !"air.vertex_input", !"air.location_index", i32 4, …}`
  with `!31 = !{ptr addrspace(2) @__metal_implicit_fc_pred_5, !"bool",
  !"vtx_attr_has_point_size"}`. The parameter exists either way — the signature is fixed — so the
  attribute is translated either way; the predicate decides whether the shader reads it, and it is
  not in `!air.function_constants`, because the module's `air.static_init` constructor computes it
  from the constants that are. There is therefore no index to hand a runtime, only the name.
- **the format, the offset, the stride and the buffer are nowhere in the module.** They are the
  descriptor's. All the module says is which indices it reads and what it wants each to be.

*What of this is read and what is inferred.* The keys, the node shapes, the flattening, the
non-contiguous indices, `air.arg_unused` on an attribute and the function-constant gate are all
read out of real modules — `air2nir --ll` of QuartzCore's library and of Resident Evil 4's. Two
things are not. The **second integer after `air.location_index`** is inferred to be the number of
consecutive indices the argument takes, which is Apple's form for an array of resources. Nothing
in reach carries another value: all **974** `air.location_index` nodes of the five system
libraries, on every key that has one, and all **4 764** on Resident Evil 4's vertex inputs — 5 738
nodes, no value but 1 — so a value other than 1 is refused by name rather than read from the first
index. And **`air.stage_in` occurs in no module of any library here** — it is a key the frontend
matches because it is in the vocabulary, with nothing behind it.

**The NIR input is 32 bits wide whatever the member's type, and the narrowing is an instruction.**
This is the one decision here that is not a transcription, and it has a measurement under it
rather than a convention copied from GL. ir3 is perfectly willing to put a 16-bit vertex input in
the **half register file** — a `half4` attribute compiles to `@in(hr0.x)` and a `cov.f16f32` —
but `ir3_shader_variant`'s input entries have no half flag (its *output* entries do), the variant
prints that input as `r0.x`, and `fd6_program.cc:emit_vfd_dest` programs
`VFD_DEST_CNTL_INSTR_REGID` straight out of that field. The fetch would write the full register
while the shader read the half one. So a `half`, `short` or `char` member becomes a 32-bit input
and an `f2f16`, `u2u16` or `u2u8` after it, which is the regime both freedreno drivers use and
which the VFD converts into either way.

| AIR | NIR |
| --- | --- |
| `air.vertex_input` + `air.location_index n, 1` | `shader_in` at `VERT_ATTRIB_GENERIC0 + n`, 32-bit, `info.inputs_read` set |
| ... a `half`/`short`/`char` member | the same, narrowed with `f2f16` / `u2u16` / `u2u8` |
| ... `air.arg_unused` | **no input at all**: `undef`, and `used = false` in `air_shader_info` |
| ... `air.function_constant` | translated, the predicate's **name** recorded (there being no index), and a **note** left: see below |
| `air.location_index n, k` with k ≠ 1 | refused: an attribute spanning several indices |
| `n` ≥ 16 | refused: NIR has sixteen `VERT_ATTRIB_GENERIC` slots, and Apple's feature tables give Metal 31 |
| two members at one `n` | refused: two inputs at one location, two fetches into one register |
| a member that is not a scalar or a vector (a matrix) | refused |
| 64-bit, or `i1` | refused: no `MTLVertexFormat` is 64 bits and MSL has no `double` |

The `n` ≥ 16 row replaces a silence, and that is what this work is worth most: before it,
attribute index 16 translated onto `VERT_ATTRIB_EDGEFLAG` and index 30 past the end of
`gl_vert_attrib` altogether, and **both compiled through ir3 without a word**. A `uchar4`
member — what every engine in the world sends a vertex colour as — did not even get that far: the
input variable was built 32-bit while the argument was `<4 x i8>`, and the first 8-bit instruction
asserted inside `nir_builder` (`src_bit_size == bit_size`, `nir_builder.c:93`).

*And the runtime needs more than the NIR.* `air_shader_info.vertex_attributes` carries, per
attribute, the Metal index, the NIR location, the component count, the width and base type the
shader wants, Apple's own type name, whether it is read at all, the gating predicate's name — and
the **`driver_location`**, which is the field a runtime cannot do without, because **Gallium
matches vertex elements to shader inputs by position and not by location**. ir3's `setup_input`
indexes `so->inputs[]` by `nir_intrinsic_base`, the `driver_location` that `nir_lower_io` wrote;
`so->inputs_count` is set to `nir->num_inputs` outright; `fd6_vertex_state_create` emits
`VFD_DECODE[i]` from `elements[i]` while `emit_vfd_dest` emits `VFD_DEST_CNTL_INSTR[i]` from
`vs->inputs[i]`. So `pipe_vertex_element[i]` feeds the input whose `driver_location` is `i`, the
array is exactly `nir->num_inputs` long, and the order is the order AIR lists the attributes
**with the unused ones skipped** — for `std_vert0_lpf`, element 0 is attribute 0 and element 1 is
attribute 3. Nothing about that is guessable from the attribute indices, and getting it wrong
feeds one attribute's buffer to another.

Also: the elements must be built from this table and not from ir3's own input table, because ir3
may delete the read. Resident Evil 4's `PhotonSplattingVS` loads `in_var_POSITION` and then writes
a constant position, so after ir3's optimisation its input entry is zero-filled — `slot=0`,
`cm=0`, `regid 63` — while `emit_vfd_dest` still counts it. The entry is harmless; reading the
interface off it would not be.

*The measurement.* `tools/air/vattrcheck.py`, a stage of `check.sh`, and it compares **no value**:
`air_run`'s only vertex stage is its own generated quad, so nothing on this machine draws with a
translated vertex shader, and the contract the Metal driver uses carries no vertex layout at all
(below). What it does check is sixteen generated modules — the attribute index reaching ir3's own
fetch table with the right write mask (`VERT_ATTRIB_GENERIC5 cm=3` for a `float2` at attribute 5),
the 32-bit input and its narrowing, the `driver_location` numbering, `inputs_read`, the whole
`air_shader_info` row, the note a function-constant-gated attribute must leave, and the seven
shapes that must be refused — and then the same translation against Apple's own metadata **read a
second time and a different way**: a regular expression over
`air2nir --ll` of QuartzCore's fourteen `[[stage_in]]` vertex shaders, attribute by attribute,
against what `air2nir --interface` says the struct holds. The two readings agree on all 46
attributes of all 14 modules; made to disagree on purpose, the index comparison fails all 14 and
the unused comparison fails the 8 modules that have an unused attribute, which is what says the
comparison is live rather than vacuous.

| what moved | before | after |
| --- | --- | --- |
| QuartzCore: reader / translate / ir3 of 141 | 141 / 135 / 135 | 141 / 135 / 135 |
| its 14 `[[stage_in]]` modules: NIR vertex inputs for 46 declared attributes | 46 | **38** |
| a `uchar4` attribute with 8-bit arithmetic | assertion in `nir_builder` | translates, validates, compiles |
| attribute 16 / attribute 30 | `VERT_ATTRIB_EDGEFLAG` / past the enum, both compiled | refused by name |
| `check.sh -q` | baseline | identical apart from the section added |

No module of the 456 is touched: not one of them carries an `air.vertex_input` at all, which is
why the census does not move and why the gate is the thing that proves it.

*The demand, counted.* QuartzCore's library: 14 of its 39 vertex modules, 46 attributes, 8 of them
`air.arg_unused` and 6 gated by a function constant, `float4` 20, `float2` 18, `half4` 6,
`float` 2, indices 0 to 4. Resident Evil 4, counted over the 7 879 libraries
`tools/air/repak.py` extracts (section 8.1) and over their `air2nir --ll`: **942 of 1 127** vertex
modules, **4 764 attributes**, up to 14 in one module, and the types are
`float4` 1 396, `float2` 1 180, `float3` 972, `uint` 782, `uint2` 263, `uint4` 115, `uint3` 48,
`float` 3, `int2` 2, `half3` 2, `half2` 1 — so **a quarter of a real engine's attributes are
integers**, and the highest index used anywhere is **13**, inside NIR's sixteen. 933 of those 942
are also behind the argument buffer of section 8.2 and so are not reachable yet; the other **9
translate and reach ir3 today**, with their attributes in the fetch table at component masks
`0x3`, `0x7` and `0xf` — the first vertices of the acceptance test to get that far.

##### The other half: what the contract must carry, and what a6xx cannot honour

This is a design and not an implementation: `air_metal.h` and the driver are not the frontend's,
and `rwa_render_shape` today carries no vertex layout — no formats, no strides, no offsets — which
is why `rwa_render_create` still refuses a shader with a `[[stage_in]]` input **by name** rather
than drawing it over garbage. What follows is what that half has to express, in the field names it
would translate from and to.

An `MTLVertexDescriptor` is two arrays: `attributes[]`, each with a `format`, an `offset` and a
`bufferIndex`, and `layouts[]`, each with a `stride`, a `stepFunction` and a `stepRate`. Both are
31 long by Apple's feature tables — a number from the API and not from this machine, though the
modules themselves do carry `air.max_device_buffers i32 31`, so the buffer half of it is on the
device. The contract needs exactly that, beside the shape it already has — per attribute the
Metal index, the format, the offset and the buffer index; per buffer index the stride, the step
function and the step rate. It becomes, in Gallium, one `pipe_vertex_element` per **used**
attribute in `driver_location` order plus one `pipe_vertex_buffer` per buffer index:

| Metal | Gallium | a6xx, and its limit |
| --- | --- | --- |
| `attributes[n].format` | `pipe_vertex_element.src_format` | `VFD_DECODE_INSTR.format`/`.swap`/`._float` via `fd6_vertex_format()` |
| `attributes[n].offset` | `.src_offset` | `VFD_DECODE_INSTR.offset`, **12 bits: ≤ 4095** |
| `attributes[n].bufferIndex` | `.vertex_buffer_index` | `VFD_DECODE_INSTR.idx`, **5 bits: ≤ 31** |
| `layouts[k].stride` | `.src_stride` (per element in Mesa 25.0, per buffer in the register) | `VFD_FETCH_STRIDE[k]`, **12 bits: ≤ 4095** |
| `layouts[k].stepFunction` = `PerVertex`, rate 1 | `.instance_divisor = 0` | the ordinary fetch |
| `layouts[k].stepFunction` = `PerInstance`, rate r | `.instance_divisor = r` | `VFD_DECODE_INSTR.instanced` + `VFD_DECODE[k].STEP_RATE` |
| `layouts[k].stepFunction` = `Constant` | `.src_stride = 0`, which is Gallium's own third category | a stride of zero; **never generated by the GL path, so untested on this hardware** |
| `setVertexBuffer:offset:atIndex:` | `pipe_vertex_buffer.buffer.resource` + `.buffer_offset`, at slot `k` | the fetch base |

`set_vertex_buffers` takes a **count** from slot 0, not a slot and a buffer, so a descriptor that
uses index 5 alone is still an array of six with five empty entries; `VFD_FETCH[k]` is addressed
directly, so Metal's index can stay Metal's index all the way down.

The formats are Apple's public API and this machine has no Metal headers to read them out of
(the sysroot's `Metal.framework` has no `Headers`, and the enumerators are a C enum, so no string
in the image names them). The **names** are corroborated on the device all the same:
`CoreRE.framework`'s `RealityKit-Schema-Fall2020.reschema` carries RealityKit's own `VertexFormat`
enumeration as strings — `UChar` … `UChar4Normalized_BGRA`, `UShort*Normalized`, `Half`…`Half4`,
`Float2`…`Float4`, `Int`…`UInt4` — and its `VertexStepFunction` as `Constant`, `PerVertex`,
`PerInstance`, `PerPatch`, `PerPatchControlPoint`, which is Metal's five. That schema is
RealityKit's own enumeration and not Metal's, so it corroborates and does not define: the four
names it does **not** carry — `Int1010102Normalized`, `UInt1010102Normalized`, `FloatRG11B10` and
`FloatRGB9E5` — are from Apple's API alone and have nothing on this machine behind them. The
numeric values must come from the SDK the driver is built against, and are deliberately not
quoted here.

The format mapping itself is one rule and four special cases, so there is no table to get wrong:
`Char`/`Short`/`Int` and their unsigned forms become `R8`/`R16`/`R32` `_SINT`/`_UINT` of the same
channel count, `…Normalized` becomes `_SNORM`/`_UNORM` instead, `Half` becomes `R16…_FLOAT` and
`Float` `R32…_FLOAT`; then `UChar4Normalized_BGRA` is `B8G8R8A8_UNORM`,
`Int1010102Normalized`/`UInt1010102Normalized` are `R10G10B10A2_SNORM`/`_UNORM`, `FloatRG11B10` is
`R11G11B10_FLOAT` and `FloatRGB9E5` is `R9G9B9E5_FLOAT`. Gallium's `_USCALED`/`_SSCALED` — an
integer in memory widened to a float in the register without normalising — has no Metal
counterpart at all and is never needed: Metal's plain integer formats feed an integer member,
which is what the frontend's `base_type` is there to check.

**What freedreno cannot honour**, read out of `fd6_format_table.c` (whose `V__`/`VT_`/`VTC` macros
say whether a `pipe_format` has a vertex entry at all) and out of the register definitions:

- **`MTLVertexFormatFloatRGB9E5`** — `PIPE_FORMAT_R9G9B9E5_FLOAT` is `_T_` in that table: texture
  and nothing else, `fd6_vertex_format()` returns `FMT6_NONE`. The VFD cannot fetch it. Every
  other format Metal has does have a vertex entry, including the three-component ones (`VT_`),
  `UChar4Normalized_BGRA` (`B8G8R8A8_UNORM`, whose channel order rides in
  `VFD_DECODE_INSTR.swap = WXYZ`), both `1010102Normalized` forms and `FloatRG11B10`.
- **`MTLVertexStepFunctionPerPatch` and `PerPatchControlPoint`** — tessellation, which has no
  `pipe_vertex_element` field and no stage in this port. Refuse by name.
- **`PerVertex` with a step rate other than 1** — Gallium has a divisor for instances and none for
  vertices, so there is nothing to express it with.
- **an attribute offset or a vertex stride above 4095 bytes** — the hardware fields are 12 bits
  wide (`A6XX_VFD_DECODE_INSTR_OFFSET__MASK 0x0001ffe0`, and the pack asserts
  `(stride & 0xfffff000) == 0`). Metal allows more. `fd6_vertex_state_create` builds the register
  with the masking setter and would **truncate silently**, so this refusal has to be ours.
- and the refusal has to be **ours** in the unsupported-format case too, for a sharper reason:
  `fd6_vertex_state_create` does `assert(fmt != FMT6_NONE)` and then programs the register with
  whatever it has. A driver that hands freedreno a format it cannot fetch gets an assertion in a
  debug build and a wrong fetch in a release one. The polite question to ask first is
  `screen->is_format_supported(..., PIPE_BIND_VERTEX_BUFFER)`, which `fd6_screen.cc` answers from
  `fd6_vertex_format() != FMT6_NONE`.

Five more things the contract half has to decide, which the frontend either cannot see or can
only flag:

1. **The buffer index space is shared.** `setVertexBuffer:offset:atIndex:` fills one table that
   both `vertexDescriptor.layouts[k]` and a `[[buffer(n)]]` argument read from. So an index is
   either a vertex stream or a constant/device buffer, never both, and the runtime has
   `air_shader_info.resources` for the second set and the descriptor's configured layouts for the
   first. If they overlap, the application is wrong and the pipeline should be refused by name.
2. **The shader's half must be checked against the descriptor's.** Every bit of
   `air_shader_info.vertex_attributes_read` must have a configured attribute in the descriptor, or
   the fetch reads whatever the VFD slot held; and the attribute's format must deliver the base
   type the shader declared — which is why the frontend records `AIR_ATTRIB_INT` against
   `AIR_ATTRIB_UINT` separately even though `int4` and `uint4` are the same `<4 x i32>` and the
   sign is only in Apple's `air.arg_type_name`.
3. **A function-constant-gated attribute must not make a binding compulsory.** The parameter
   exists whatever the predicate says, so the attribute is fetched and its bit is in
   `vertex_attributes_read`; a pipeline whose constants make the predicate false may legally
   configure nothing at that index. The frontend cannot decide which it is — the predicate's value
   is the pipeline's, written by the module's `air.static_init` constructor from the constants the
   API supplies — so it leaves a **note** naming the predicate on every such attribute, and the
   runtime has to read `air_vertex_attribute.function_constant` before demanding a binding.
   Six of QuartzCore's 46 attributes are gated this way and none of Resident Evil 4's 4 764 are.
4. **A shader all of whose attributes are unused reads no vertices**, and now makes no NIR input,
   so `vertex_reads_attributes()` no longer sees one and `rwa_render_create` would draw it. That
   is right rather than a hole — there is nothing to feed — but it is a change in what that
   refusal catches, and the contract half should know it. No module in reach has that shape.
5. **A vertex buffer is a resource with a lifetime**, which `rwa_render_shape` has no vocabulary
   for yet: the current contract passes buffers for the shader's arguments and the layout half
   needs the same thing for the streams, bound per draw rather than per pipeline, because Metal
   splits them that way — the descriptor is pipeline state and `setVertexBuffer:` is encoder
   state.

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

**One meaning the list cannot settle, because it is one name: `convert` with a bool destination.**
`air.convert.u.i1.f.f32` is in the vocabulary only as `convert`, and the two readings — round
toward zero and keep a bit, or compare with zero — give different answers for every `|x| < 1`.
What settles it is Apple's emitted code rather than Apple's strings: all four calls in the device
image (two in QuartzCore's `default.metallib`, two in AppleCVAPhoto's `cvadepth-iOS.metallib`)
feed a `br i1` directly, which is an `if (x)` on a float, and under the truncating reading that
branch is a branch on poison by LLVM's own rule for `fptoui float to i1`. So it is `x != 0`, C's
rule, which MSL inherits — the lowering had it right and this check's own reference had it wrong
(section 4, item 4).

Still unimplemented, and now known by name rather than by surprise: the inverse and hyperbolic
trigonometry (`acos`, `asin`, `atan`, `acosh`, `asinh`, `atanh`, `cosh`, `sinh`, `tanh`),
`sincos` (two results from one call), `hadd`, `rhadd`, `mad_hi`, `mad_sat`, `any_msb`/`all_msb`,
the `gamma_*` conversions, the `pack` family, and `assert`. Of `unpack` only `unorm4x8` is
implemented, because only that one occurs; of the seventeen `air.simd_*` reductions and shuffles
in Apple's list only `simd_sum` is, for the same reason.

### 3.3 The sampler state word, decoded, applied and checked

Metal's `constexpr sampler` — a sampler the shader declares itself rather than one the API binds —
arrives as a global holding one 64-bit word. **The layout is now known, the frontend decodes it,
`air_run` binds a sampler built from it, and the behaviour is checked against Apple's own rules.**

The guess this section used to record was close and useless: three three-bit address fields at
bits 0, 3 and 6 and two filter bits at 9 and 11, inferred from the three words the corpus carries
and deliberately not used by anything. What replaced it is three independent sources, which is
what it took:

- **Apple's own `metal_stdlib` on the device** — `GPUCompiler.framework/Libraries/lib/clang/
  31001.722/include/metal/metal_texture`, `struct _sampler_state` with every enum and
  `_get_sampler_state()` with every default. The whole standard library is there, not only the
  sampler part.
- **the encoder that writes the word**, `llvm::air::AIRSamplerStateEncoder::encode` in
  `libGPUCompilerImpl.dylib`.
- **the decoder that reads it back**, `AIRConstSampler::constructFlatReply` in `AGXCompilerCore`,
  with the flatbuffers schema beside it.

| bits | field | values |
| --- | --- | --- |
| 0–2, 3–5, 6–8 | address s, t, r | 0 `clamp_to_zero`, 1 `clamp_to_edge`, 2 `repeat`, 3 `mirrored_repeat`, 4 `clamp_to_border` |
| 9–10, 11–12 | mag, min filter | 0 nearest, 1 linear, 2 bicubic |
| 13–14 | mip filter | none, nearest, linear |
| 15 | coordinate | 0 normalized, 1 pixel |
| 16–19 | compare function | 0 none, 1 less, 2 less_equal, 3 greater, 4 greater_equal, 5 equal, 6 not_equal, 7 always, **8 never — and 8 is the default** |
| 20–23 | max anisotropy − 1 | |
| 24–39, 40–55 | lod min, lod max | halves |
| 56–57 | border colour | |
| 58–59 | reduction | AGX does not read it |
| 60–62 | not written by the encoder and not read by anything | zero in the corpus |
| 63 | the one-word form | the two-word form of AIR 2.6 carries a LOD bias in the second word |

**One source was not enough, and the trap is worth keeping.** `libGPUCompilerImpl` carries its own
`AIRAddressMode` printer in which zero prints as `clamp_to_border`, and the string `clamp_to_zero`
does not occur in that binary at all. Read alone, it gives a plausible and wrong meaning for the
commonest field in the corpus — and `clamp_to_zero` is not a border mode at all: outside the
texture it reads transparent black, zero in all four channels, and no border colour is
configurable for it.

Decoding the corpus is the first check that the layout is right. Ten modules declare a sampler,
carrying three distinct words, all ten in `PortraitFilters.cifilter/default.metallib`:

```
0x807bff0000088049   sampler(coord::pixel)                                           8 modules
0x807bff0000088a49   sampler(coord::pixel, filter::linear)                           confidence
0x807bff0000088a00   sampler(coord::pixel, filter::linear, address::clamp_to_zero)   MetalFaceMask_GenerateMask
```

Every other field comes out equal to the header's default, including a LOD range of `[0, 65504]` —
two separate 16-bit halves that a wrong layout would not land on — and `coord::pixel` is confirmed
by the arithmetic of the shaders themselves: `MetalFaceMask_DilateCubeLuma` samples at
`float(gid) + 0.5` with integer offsets, which is a pixel-space idiom.

**And decoding is not applying.** `tools/air/sampcheck.py` generates a fragment shader per (word,
coordinate), samples with the shader's own sampler, and compares against what Apple's rules say
the answer must be: texel *i* centred at pixel coordinate *i*+0.5, `nearest` by `floor(u)` with
the tie at a boundary going to the higher index, `linear` by `frac(u − 0.5)` so an integer
coordinate is an exact half-and-half mix, `clamp_to_edge` returning the nearest edge texel and
`clamp_to_zero` returning zeros. 15 cases, 15 exact, on softpipe **and 15 exact on the phone's
Adreno** (`logs/181303-run.log`), which is the run that matters: softpipe filters in C with exact
weights and hardware does not have to.

Those 15 are safe from the hang described in section 5, and not because the run was short. A
dispatch the GPU never finished returns whatever the harness put there, which is one value; these
15 cases expect 15 *different* answers — an exact texel, the exact mean of two, zeros in all four
channels — so a vacuous read cannot match them. A check whose cases expect different things is
also a check that cannot be faked by a dead GPU, which is worth knowing when choosing cases.

**What those 15 do not test is the precision of the filter's weights.** Every linear case lands on
an integer coordinate, where the weight is exactly 0.5 — representable in any format a texture
unit might use, including a few fixed-point bits. So the agreement is about the taps, their order
and the addressing. It could not be otherwise: Metal does not specify the precision of a linear
blend, so a case at a non-dyadic coordinate would have no expectation to compare against, only a
difference to report.

The expectations are Apple's and not this frontend's reading of the word, which is what makes a
match evidence about the word. And the check was made to prove it can fail: mapping
`clamp_to_zero` to `CLAMP_TO_EDGE` on purpose turns exactly one case red — the one that samples
outside with that word — and reverting it turns it green again. It also reports, before anything
runs, how many of its cases would catch each field read wrongly: 6 the filter, 3 the address mode,
14 the coordinate convention. A case where two readings agree proves nothing, and that is the same
shape as the half conversions that "agreed" over values needing no rounding.

**What is still not evidence.** The three words the corpus carries use `clamp_to_edge` and
`clamp_to_zero` with nearest and linear, so `repeat`, `mirrored_repeat`, `clamp_to_border` with a
real border colour, bicubic filtering, the mip filters, anisotropy and the LOD clamp are decoded
and **not** exercised: no module attests a word that uses them, and inventing one would be our
construction rather than Apple's. The ten modules themselves have no reference to be compared
against, so what they gain is a sampler that is theirs rather than the harness's — which changes
their output and is not by itself a verdict.

#### The comparison function: eight values nothing attests, and one that everything carries

The comparison function used to be in that list and is now the field this section has most to say
about, because reading it as unexercised was the same mistake as reading `clamp_to_border` off one
binary — and this one cost 653 draws.

**8 is the default.** `_get_sampler_state()` in Apple's own header initialises the field to
`__METAL_COMPARE_FUNC_NEVER__`, under `#if defined(__HAVE_ALWAYS_NEVER_COMPARE_FUNCTION__)`, which
`metal_config` defines for this language version. So every `constexpr sampler` whose source names
no `compare_func` carries 8 — as all three words of the corpus do, their every other unnamed field
also equal to the header's default. The frontend's refusal tested the field for `!= 0`, read `none`
as the default, and so refused **every inline sampler Apple's compiler has ever emitted**. In the
window server's first boot through this library that was 653 of 653 failed draws, every one of them
at value 8 (`logs/032854-run.log`). Not one was a shadow comparison.

**The nine values, and why a cast is the wrong instrument.** Three columns, none of them in the
same order:

| AIR, bits 16–19 | MTLCompareFunction | AGX's `_AGCCompareFunction` |
| --- | --- | --- |
| 0 `none` | — (no API value; carried as Never) | 0 NONE |
| 1 `less` | 1 Less | 2 LESS |
| 2 `less_equal` | 3 LessEqual | 3 LEQUAL |
| 3 `greater` | 4 Greater | 4 GREATER |
| 4 `greater_equal` | 6 GreaterEqual | 5 GEQUAL |
| 5 `equal` | 2 Equal | 6 EQUAL |
| 6 `not_equal` | 5 NotEqual | 7 NOTEQUAL |
| 7 `always` | 7 Always | 8 ALWAYS |
| 8 `never` | 0 Never | 1 NEVER |

Casting the AIR value into MTLCompareFunction is wrong for five of the nine and **reverses the
direction of two**: `less_equal` would arrive as Equal and `greater` as LessEqual. Casting it into
AGX's enumeration — the other plausible reading, since AGX is what Apple's own stack does with the
field — is wrong for eight of the nine and would make the default mean `always`. A shadow map
rendered either way is a picture, not an error.

**Where the values come from.** Not from the declaration order alone:

- the names and their order are in Apple's `metal_stdlib` on the device, `enum class compare_func`
  in `metal_texture`, with both `__HAVE_NONE_COMPARE_FUNCTION__` and
  `__HAVE_ALWAYS_NEVER_COMPARE_FUNCTION__` defined, so all nine exist;
- the **values** are clang's own, out of the macro table in `libGPUCompilerImpl` that defines
  `__METAL_COMPARE_FUNC_*__` with each value emitted beside its name — `NOT_EQUAL` 6, `ALWAYS` 7,
  `NEVER` 8;
- and the mapping is confirmed from the other side by Apple's **decoder**,
  `AIRConstSampler::constructFlatReply` in `AGXCompilerCore`, which reads this field as
  `ubfx x8, x8, #16, #4; sub x8, #1; lsr x9, 0x0108070605040302, x8*8` — a byte table in a register
  — so field 1 becomes AGC's LESS, field 8 becomes its NEVER and field 0 falls out of range to its
  NONE. The same function reads the address modes at bits 0–8 by the same idiom, the filters at
  9–14, and the two LOD halves at bytes 3 and 5, which is how this reading of the **whole** word is
  checked and not only of this field.

**What checks the table, and the case that distinguishes one direction from the other.** A
comparison's wrong answer is in range — 0 and 1 are both legal pixels — so the check cannot be
"it came back plausible". `tools/air/sampcheck.py` samples a **depth** texture with
`air.sample_compare_depth_2d` and the shader's own word. `air_run`'s depth texel is `(x + 4y)/16`,
so texel (1, 1) is 0.3125 and texel (3, 0) is 0.1875 — sixteen distinct values, every one exactly
representable in binary32, so a reference placed between two of them is exact on both sides. Three
references, either side of the first texel and on it:

| reference | 0.25 | 0.3125 | 0.375 |
| --- | --- | --- | --- |
| `less` | 1 | 0 | 0 |
| `less_equal` | 1 | 1 | 0 |
| `greater` | 0 | 0 | 1 |
| `greater_equal` | 0 | 1 | 1 |
| `equal` | 0 | 1 | 0 |
| `not_equal` | 1 | 0 | 1 |
| `always` | 1 | 1 | 1 |
| `never` | 0 | 0 | 0 |

**All eight triples are distinct**, so the three references together *identify* the function and no
permutation of the table passes. The direction case is reference 0.25 against texel 0.3125, where
`less` must answer 1 and `greater_equal` 0, and reference 0.375 against the same texel, where the
two swap; the adjacent pairs — `less` against `less_equal`, `greater` against `greater_equal` — are
separated only by the reference that lands exactly on the texel, which is why that reference is
there. A fourth case per function samples texel (3, 0) = 0.1875 with the reference 0.25, which is
*below* the first texel and *above* this one, so the four ordering functions must answer it the
opposite way round: a comparison that always read the same texel fails it.

#### The first device run failed, and what it ruled out

Against the **colour** texture the harness used to bind, every one of the 32 comparison cases came
back on the Adreno as **the texel** — 101 where a comparison must answer 0 or 1, and for `always`
and `never` too, which no compare function and no clamped reference can explain
(`logs/062829-run.log`; `sampcheck.py --check` read it with 0 cases missing, so the measurement
itself was sound). That one run ruled out everything above it:

- **the shadow instruction** — ir3 emits it, `samgq.s (f32)(x)r0.x, hr0.x, s#0, t#0`;
- **the sampler state** — `air_run` printed `APPLIED (a shadow sample reads this slot)` for all 32,
  so `compare_mode` and the mapped function reached the Gallium CSO, and
  `fd6_sampler_state_create` writes `A6XX_TEX_SAMP_1_COMPARE_FUNC` whenever `compare_mode` is set;
- **the compare function itself** — `always` and `never` failed too.

What was left is what the same log printed one line earlier: `1 texture bound: 4x4x1
r32g32b32a32_float`, a colour texture under a Metal `depth2d<float>`. **A shadow lookup on a
non-depth texture is undefined in GL and in Vulkan alike** and the texture unit does the comparison
in its depth path, so the hardware was right and the harness was wrong. `air_run` binds a real
depth texture now, and `texture_fits` in `air_metal.c` **refuses the colour case by name** rather
than letting a shadow lookup answer with the texel — which is the exact shape of wrongness this
port refuses on principle, and the hardware will not say no, so the library does.

Two candidates remain if the next device run still fails, and the suite separates them without a
line changing: **unnormalized coordinates** (every corpus word is `coord::pixel`, and Vulkan
forbids combining those with a comparison at all, so six cases now carry `coord::normalized`
instead — if only the pixel ones fail, that is the answer and the combination gets refused by
name), and **the uniform preamble** (these shaders have constant coordinates, so ir3 hoists the
sample into the early preamble and emits `samgq.s` rather than `sam.s`; a boot with
`IR3_SHADER_DEBUG=nopreamble` compiles the same modules to `sam.s` in the main shader and changes
nothing else).

One more correction the device made, and it is the shape this project keeps finding: the
API-sampler case passed **pixel** coordinates to a sampler built with normalized ones, so 1.5
landed outside and clamped to the far corner — and on softpipe the comparison answered 0 whatever
texel it read, so the wrong texel was invisible. The Adreno returned that texel, 103, and named the
mistake. The case uses normalized coordinates now.

Each case is run **twice**, through `air_run` and through `air_metal.c` — the library the Metal
driver links — because those are two tables that must agree and so two tables that would otherwise
drift. And one more case compares through a sampler **the API binds**, where neither harness lets a
caller choose the function and both bind MTLSamplerDescriptor's own default, Never: it must answer
0, and 101 would be the unmistakable failure, because 101 is the texel and means the non-comparing
state object was bound.

**Three things these cases deliberately do not claim.**

- **Nothing filtered.** softpipe filters the texels and compares once (`sp_tex_sample.c` calls
  `sample_compare` *after* `funcs->filter`), while a6xx's shadow sample compares each tap and
  filters the comparisons — percentage-closer filtering, which is what Metal describes. The two
  agree on one tap and disagree on four, so a linear comparison has a difference to report and no
  expectation to compare against. Every case here is `nearest`.
- **`none` has no answer and is refused.** MTLCompareFunction has eight values and no "not
  configured", so `compare_func::none` reaches a driver as Never — right for the plain sample that
  is all either can do, and *not* right for a comparison, where AGX keeps its NONE and its NEVER
  apart and what it does differently is not readable from here. Both `air_metal.c`'s `bind_stage`
  and `air_run` refuse a shadow instruction through such a sampler, in the same sentence, and one
  case of `sampcheck.py` passes by being refused.
- **`gather_compare` is refused by name.** Four comparisons in one instruction, attested by no
  module of the 456 and none of Resident Evil 4's 7 879, so there is nothing to check an answer
  against.

**Who decides that a comparison happens, and why no struct grew for it.** In Metal the
*instruction* decides: `sample()` and `sample_compare()` read one sampler and the sampler only says
*which* comparison. In Gallium the *sampler* decides, through `pipe_sampler_state.compare_mode`, and
softpipe compares on every sample of a sampler that has it set. One state object cannot serve both,
so `rwa_sampler_create` builds the pair and a stage binds the comparing one into exactly the slots
a shadow instruction reads — which the translated NIR states and `slot_shapes` reads off it. On
a6xx the same arrangement falls out of the hardware: there is no compare-enable bit at all, only
`A6XX_TEX_SAMP_1_COMPARE_FUNC`, which `fd6_sampler_state_create` writes when `compare_mode` is set
and which the hardware reads only for the shadow form of the sample instruction, selected by ir3
from `nir_tex_instr::is_shadow`. `struct rwa_sampler_state` did not have to grow: the pair lives in
`struct rwa_sampler`, which is private to `air_metal.c` and declared without a body in the header,
so no driver reads it and nothing in the ABI moved.

### 3.4 A question for daylight: what of Apple's may this repository contain?

Raised on 20 October and deliberately not settled at three in the morning. The working line is
**explanation, not input**: a short excerpt quoted beside an analysis so a reader can check it is
technical writing, while corpus bytes committed as an *input* — a fixture a test opens, a trimmed
`.metallib`, a dump something parses — is redistribution for use. The repository should never be a
place to get Apple's shaders; it may be a place that explains them.

By that line, two things in these documents are excerpts and are kept: the three sampler words
above, and the six-line listing of `clearFragment01` in `docs/metal.md` section 9.5. Neither
substitutes for the thing it came from, and both carry the analysis they exist to support. Nothing
committed anywhere is corpus bytes as an input: `docs/air-report.md` holds module names, counts and
error strings, which are facts about Apple's shaders and not Apple's shaders, and no test here has
a fixture — `tools/air/check.sh` reads `ios165/rootfs`, which is not in the repository and cannot
be.

The part needing a decision rather than a convention: whether that line is the right one, and what
it says about the twenty-odd screenshots in `docs/` of the running port, which are Apple's rendered
artwork and a different class again. That belongs to whoever owns the repository, awake.

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
   library functions. So this check says nothing about `air_intrinsics.c`; item 4 below is what
   does, and the paragraph that used to stand here — that nothing but M4's buffer readback could —
   was wrong about the reference being necessarily circular. The objection it raised is answered
   there rather than dropped, because it is the right objection.

   The first oracle for the intrinsics that needs no hardware is section 3.2: **Apple's own
   compiler, which knows what each name means because it compiles it.** Its vocabulary settled
   the two guesses and found one of them wrong. What that oracle gives is each operation's
   *identity*, not its numerical result, so it decides `round` against `rint` and `exp` against
   `exp2` and cannot decide how many ULP Apple's `fast_sin` is allowed to be out by. The second
   is item 4, which takes that identity and checks the arithmetic against C's own function; what
   neither can do is tell you what the Adreno computes.

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
4. **Each `air.*` operation on its own, against C's own functions.** `tools/air/intrincheck.py`
   takes the declarations Apple's own code actually uses — `air2nir --ll` over every `.metallib`
   in the device image, which is 222 declarations, of which 119 are the 456-module corpus's — and
   for each one generates a fragment shader that calls it on constant arguments and returns the
   result, assembles that with `llvm-as`, and folds it with `air2nir --eval`. The reference is the
   C library's own `roundf`, `floorf`, `fmaxf`, `ldexpf` and the IEEE definitions, evaluated at
   the operation's own precision.

   **134 exactly defined operations agree on every one of 102 103 cases; none differ.** Twenty are
   approximate (`fast_sin`, `fast_cos`, `fast_exp`, `fast_pow`, `fast_atan2`, `pow`, `powr`, …)
   and are reported with their worst distance from libm and counted as nothing — see below. 154 of
   the 222 declarations have a reference here and 68 do not, and the check prints that difference
   rather than hiding it; three more cases it declines to answer, by name and with the reason.

   **Why this is not the circular reference rejected under item 1,** which it looks like at first
   and which is the objection that kept it unwritten for a week. Writing C implementations of the
   `air.*` *operations* would be circular: they would encode the same belief as the lowering, by
   the same author, and agree by construction. What this does instead is split the claim in two,
   and each half has an oracle that is not this tree:

   - *which function each name is* — settled by Apple's own compiler, section 3.2. That is where
     `air.fast_round` was found to be `round` and not `rint`.
   - *whether the lowering computes that function* — settled by the C library, which implements
     `round` because the C standard says what `round` is, and has never heard of this port.

   Neither half alone is enough and together they are not circular. What makes it work is that
   Metal's math functions are OpenCL's, which are C's: there is a specification outside both
   Apple and Mesa for almost every one of them.

   **What it does not reach.** For the twenty approximate operations NIR's folder calls the same
   libm this script does, so `0 ulp` there carries no information beyond section 3.2 — it says
   the lowering picked `sinf` and not that the Adreno's `sin` instruction agrees with `sinf`,
   which it certainly does not to the last bit. Metal's `fast_` prefix permits that. And for all
   134 of the exact ones, what is folded is NIR's constant expression for each opcode on this
   machine: an opcode whose Adreno instruction differs from what `nir_opt_constant_folding`
   computes is invisible here. That gap closes only on the phone.

   **What it found.** Two things, in opposite directions:

   - `air.fast_atan2` was wrong at the origin. Mesa's `nir_atan2` returns 3π/4 for `atan2(0, 0)`,
     and its own comment says why: GLSL licenses an implementation to deviate from IEEE there, so
     it pretends 0/0 = 1. Metal's `atan2` is OpenCL's and keeps the IEEE values — `atan2(±0, x>0)`
     is `±0`, `atan2(±0, x<0)` is `±π`. A shader taking the angle of a vector that happens to be
     zero got 135° instead of 0°, which is not the kind of deviation `fast_` permits.
     `air_intrinsics.c` now handles the origin itself and leaves the rest to Mesa. Mesa's
     deviation is confined to the origin: the four `(±0, ±0)` cases, and no other.
   - NIR's `ldexp` loses an overflow, and that one is Mesa's. Its constant expression is
     `dst = ldexpf(src0, src1); /* flush denormals to zero. */ if (!isnormal(dst)) dst =
     copysignf(0.0f, src0);` — and `!isnormal` is true of an infinity as well as of a denormal, so
     `ldexp(0.5, 32767)` folds to `0` where Metal, OpenCL and C all say `+inf`. The comment names
     denormals; the condition catches more than it means to. Present in Mesa 26.2.5, the newest
     release at the time of writing, byte for byte — checked by extracting that one file from
     `archive.mesa3d.org`, which is a plain file server, because freedesktop's GitLab is behind
     bot protection and the GitHub mirror paths 404. The merge request that looks like its origin
     is freedesktop 3982, "nir: Flush to zero with OOB low exponents in ldexp", whose title says
     both what it was for and what it missed: out-of-bounds *low* exponents, the high end never
     considered. The MR itself could not be read, so treat the origin as likely and the state of
     the current release as established. A one-line fix would be `isfinite(dst) && !isnormal(dst)`.
     Worth reporting upstream by someone who can open the page; it is a bug for every Mesa
     frontend whose language keeps IEEE overflow, which is OpenCL and Metal at least. Not worked
     around here, because the frontend's `nir_ldexp` is the right opcode.

   **And on 21 October, four more — three of them in the reference, which is a finding and not an
   embarrassment.** That day's frontend work (constants built at a cursor that does not dominate,
   `f2u1`, `switch`, struct-returning calls, function constants, framebuffer fetch,
   `[[stage_in]]`) raised the declarations this check can *fold* from 71 to 134 without one line
   of `intrincheck.py` changing — `git log 816462e..HEAD -- tools/air/intrincheck.py` is empty —
   and four of the newly reached ones disagreed at once. The jump is in the reader's reach, not in
   the check: 688f5de's own message records `check.sh` still reading `intrincheck 71/0` with that
   commit's `f2u1` fix already in, so all four of these were first evaluated by the commits after
   it. That is the shape of a new measurement finding something, and what it found was mostly its
   own reference: an expectation nobody had ever folded had no reason to be right.

   - **`air.dot.v3f16` and `air.dot.v4f16`: the reference, which summed a half dot product in
     `float`.** `(-0.25, 8, -16)` against itself is exactly `320.0625`; `f16`'s step at 320 is
     `0.25`, so `320` is the only thing a `half` can hold and the frontend's `320` was the one
     possible answer. The `dot` branch of `reference()` rounded to `f32` whatever the declaration
     returned, so it expected a 13-bit significand from a type that has 11, and said so on 3 of
     those two declarations' 200 cases — the three where the exact sum needs the extra bits;
     `air.dot.v2f16` never reaches one and agreed throughout. Three sources agree on `320` and
     one did not: `f16` arithmetic itself, `llc` on this processor through `airdefs.py`'s `fmul
     half`/`fadd half` bodies, and `air2nir --eval`. The reference now rounds to the result's own
     precision, taken from the declaration's return type. The old comment's claim that the
     accumulation order cannot matter survives and is now checked rather than asserted: over all
     300 cases of the pool the left fold, the right fold, NIR's own pairwise order for `fdot3`
     and `fdot4`, and the once-rounded exact sum are one number, in `f16` as in `f32`.
   - **`air.convert.u.i1.f.f32`: the reference again, and the lowering was right before it was
     ever checked.** A bool destination was being treated as a conversion — truncate toward zero,
     keep one bit — which makes `bool(0.1)` false and `bool(2.0)` false again. The rule is C's,
     `x != 0`, and MSL inherits it through C++11, so the specification outside both Apple and Mesa
     that item 4 rests on says what the answer is. Apple's own compiler settles it without even
     that: all four calls in the device image — two in QuartzCore's `default.metallib`, two in
     AppleCVAPhoto's `cvadepth-iOS.metallib` — put the result straight into a branch,

     ```
     %590 = fpext half %336 to float
     %591 = tail call i1 @air.convert.u.i1.f.f32(float %590)
     br i1 %591, label %592, label %593
     ```

     which is an `if (x)` on a floating-point value and nothing else. Under the truncating
     reading that is a branch on **poison**, because LLVM's own rule for `fptoui float to i1` is
     poison whenever the truncated value does not fit the destination — every `|x| >= 2` — and no
     compiler emits a branch on poison for a condition. `air_intrinsics.c` lowers it as
     `nir_fneu_imm(x, 0.0)` and has since the `bits == 1` case was written, with the reason in a
     comment; the reference and `airdefs.py`'s `fptoui` body are both corrected to the
     comparison, and the declaration went from 10 comparable cases to 22, because the 12 the old
     reference called "outside the destination's range" have an answer after all.
   - **`air.fract.f16` at an infinity: nobody's, and it is left open rather than decided.** The
     reference answered `inf`, which is the one answer none of the three sources supports: the
     guard that keeps a NaN was returning the argument for an infinity too, bypassing that
     function's own clamp to below 1. The three sources are Metal, whose `fract` "returns a value
     in [0, 1)" — which an infinity is not, so the argument is outside the function as specified;
     OpenCL, whose math library MSL's is, with an edge-case table that gives `fract(±inf) = ±0`;
     and NIR, whose `ffract` is `src0 - floorf(src0)` (`nir_opcodes.py`), so the frontend folds
     `inf - inf` and gets a NaN. Nothing on this machine can choose between `±0` and NaN, and the
     case is only ever asked because the pool's `2^24`, `2^24+1` and `1e8` overflow `f16` — which
     makes `air.fract.f16` the one declaration here given an infinity at all. The device's own
     `metal_math` routes `half fract(half)` through `__METAL_MAYBE_FAST_MATH__`, which
     `metal_common` defines as `__METAL_FAST_MATH__` whenever `__FAST_MATH__` is set, and the
     corpus says it is — its modules carry `fmul fast` and `air.compile.denorms_disable` — so an
     implementation is there permitted to assume there are no infinities at all. The reference now
     *declines* those three cases, with the reason attached to the exception that declines them
     and printed in the summary: `3 cases the reference declines to answer -- fract of an
     infinity: ...`. A declined case must not look like a case that passed, which is the mistake
     the infinities in the half-returning conversions had already made once here.

   **Not one of the four was the lowering,** and that is the result rather than the absence of
   one: `air_to_nir.c` and `air_intrinsics.c` are untouched by all of it. Three were this check's
   own expectation — never once evaluated before, and an expectation that has never been evaluated
   is not evidence — and the fourth has no answer on this machine at all. The two bugs the same
   check found yesterday went the other way, which is what makes this worth writing down: the
   shape "new coverage, then a disagreement" does not say on which side the disagreement is, and
   deciding that needs the two independent references each time.

   **And on 22 October, twenty-one more — sixteen of them the harness, which is the worst place
   for a disagreement to be.** `intrincheck.py` reported 21 of its 441 exactly defined operations
   differing, and they were one family and one stray. The family read like this:

   ```
   WRONG   air.min.u.i64        24 of 192 cases differ
     -1 -1   [0] got 4294967295        want 18446744073709551615
   ```

   `0xFFFFFFFF` against `0xFFFFFFFFFFFFFFFF`: **a 64-bit result truncated to 32 bits**, across
   `air.min.u`, `air.max.u`, `air.clamp.u` in `i64`, `v2i64`, `v3i64` and `v4i64`, and the four
   `air.convert.*` that write a `v4i64` or read one — **16 declarations and 4 024 of 32 900
   cases**, measured by running the old code rather than reasoned about. Which of the three layers
   it was in decides everything, because only the first reaches a shader on the phone:

   - **not the lowering.** `air_intrinsics.c` emits `nir_umin`, which carries the operands' own
     bit size, and NIR's dump proves it did: `64 %8 = umin %1, %1` folding to
     `load_const (0xffffffffffffffff)`. A module that calls `air.min.u.i64(-1, -1)` and writes
     the result's two 32-bit halves into two lanes of its render target folds to
     `4294967295 4294967295`, and **softpipe executes the same module, with the operands loaded
     from a buffer so nothing can fold, to `4294967289 4294967295` for `umin(-1, -7)`** — the
     second engine, and a real execution rather than a constant expression.
   - **not the host definitions.** `airdefs.py`'s self-check compiles its own LLVM-IR bodies with
     `llc` and runs them on this processor, in 8-byte slots throughout — so it was carrying all
     64 bits while `intrincheck` was not. Over the `i64` conversions it emits, **2 052 cases
     agreed with the reference bit for bit** on the day of the accusation, the four
     `air.convert.{s,u}.v4i64.{s,u}.v4i{8,16,32}` that `intrincheck` called wrong among them, and
     **2 656 over 44 declarations** once the pool below and the gap further down were fixed.
   - **the harness.** `emit` and `emit_buffered` wrote `trunc i64 %c to i32` into the generated
     shader, because a render-target lane is 32 bits wide, and then compared the low half against
     a 64-bit reference. A measuring instrument that drops the top half of what it measures is
     worse than no instrument: this one accused the compiler, and the compiler was right.

   An `i64` component now takes two lanes, low word first, and is put back together on the host
   (`lanes_per_comp`, `comp_batches`, `unlane`, `split64`). A result that no longer fits in four
   lanes is split across shaders rather than cut to four — `air.min.u.v4i64` is two modules of
   two components each, which the manifest records so the judging side cannot guess wrong — and
   the packing raises an `AssertionError` instead of silently truncating if the two ever disagree.
   The 64-bit *inputs* were wrong in the run stage too, and for a neighbouring reason: an
   argument's components were one four-byte slot apart, so `load i64` of component 0 of a
   `v4i64` took component 1's low word as its own high word. `SLOTS_PER_ARG` is eight now.

   **All 16 agree after it, over the same 32 900 cases** — the same count before and after, which
   is the point: nothing was dropped, the cases were read correctly. With the pool strengthened as
   below it is **68 of 68 `i64` declarations agreeing over 46 816 cases**, and 40 of them agree
   again when softpipe *runs* them rather than folding them.

   **And the pool could not have caught the bug it was accusing the compiler of.** Every value of
   `INTS` fits in 32 bits, so a lowering that compared 64-bit operands *on their low words alone*
   would have answered correctly on all 192 cases of `air.min.u.i64`: the operand with the
   smaller low word is also the smaller operand. `I64S` adds seven whose two halves disagree —
   `2^32` above all, whose low word is 0, so a low-word comparison picks it every time while
   `umin(2^32, 1)` is 1 — and the pool now asks 42 of 276 cases that a low-word comparison gets
   wrong, where it asked 0 of 192 before. 48 catch a truncated result and 124 catch a signed
   comparison.

   **The same bug had a second head, and it had been refusing two declarations for its whole
   life.** `air.floor.f64` and `air.fmin.f64` are the corpus's only `double` operations, and they
   came back `REFUSED` with *'%c0' defined with type 'double' but expected 'float'* — counted as
   `2 could not be built or did not fold`, so a red rather than a silence, but a red whose cause
   was a formatting mistake in this harness and nothing about the frontend. A `REFUSED` row whose
   reason is our own harness is exactly the kind of thing that becomes permanent. The frontend was
   never the problem: a module that calls `air.floor.f64(0x1.ccccccccccccdp+0)` and writes the
   result's two words into two lanes folds to `0 1072693248`, which is `0x3FF0000000000000`,
   exactly 1.0.

   So a `double` goes through an `int4` as its two halves, like an `i64`, and three more things
   had to move with it, each of which would have made agreement mean something else: the
   reference rounded to `f32` for anything that was not a `half`, so it would have expected 24
   bits of mantissa from a type that has 53; the comparison was `to_f32_bits(f32(...))` on both
   sides, which rounds the shader's answer and the expectation to a type neither of them is; and
   the denormal threshold was f32's `1.17549435e-38`, which would call `1e-300` a denormal and
   drop a case that is perfectly normal where it lives. With all three, **`air.floor.f64` agrees
   on 22 cases and `air.fmin.f64` on 264, folded, and both agree again when softpipe runs them**
   — 12 cases, over a type this suite had never once executed. That it is a 64-bit comparison and
   not a 24-bit one was checked by breaking it: with the reference rounding a `double` to `f32`,
   the two declarations report 1 of 22 and 49 of 264 cases differing.

   **The stray was `fmod`, and it was two faults, not one.** 47 of 264 cases of `air.fmod.f32`
   and of `air.fast_fmod.f32`, of which **25 were the sign of a zero and 22 were the number**.
   `nir_frem`'s constant expression is literally `src0 - src1 * truncf(src0 / src1)`
   (`nir_opcodes.py`), and ir3 lowers it to the same three instructions (`.lower_fmod = true`).
   The subtraction loses `fmod(-0, 5) = -0` and `fmod(-1.5, 0.5) = -0`, which is the shape of the
   `air.sign` bug already recorded in `air_intrinsics.c`; and it rounds `y * trunc(x/y)`, which
   needs up to 48 significant bits, before subtracting — so `fmod(1.5, 0x1.fffffep-2)` came out
   `1.19e-07` against `8.94e-08`, a whole ulp of the dividend, and **`fmod(1e8, 3)` came out 0
   where the answer is 1**. A wrong number, not a wrong sign, and the kind that is still a number
   wherever it lands.

   *Whose definition.* Metal's, and it is not the formula in Metal's own table. The MSL
   specification's entry for `fmod` gives `x - y * trunc(x/y)`, inherited word for word from
   OpenCL — and it inherits OpenCL's accuracy table with it, where `fmod` is **0 ULP**, an
   exactly specified function and not a licensed approximation, and OpenCL's edge cases are C99
   Annex F's, which say `fmod(±0, y)` is `±0`. Two references on this machine agree with that
   reading and none with the formula: this machine's libm answers `-0` for `fmodf(-0, 0.5)` and
   `fmodf(-1.5, 0.5)`, and LLVM's own `frem`, JIT-executed by `lli` — a second engine and not a
   reading of a document — answers `-0` for both and for `frem(-2.5, -0.5)`. The corpus itself
   says nothing either way, which is worth stating as a negative: **not one of its 128 libraries
   contains a single `frem` instruction**, because Apple's compiler always emits the call.

   *What was done.* `air_fmod` in `air_intrinsics.c`: `q = trunc(x/y)`, `r = fma(-y, q, x)` —
   one rounding instead of two, and when `q` is an exact integer the true remainder is itself
   exactly representable, which is why IEEE 754 can require `remainder` to be exact, so one
   rounding is no rounding — then one correction each way for a quotient `fl(x/y)` rounded across
   an integer, then the zero's sign taken from the dividend with `iand`/`ior` rather than a
   multiply by zero, because this builder runs with `exact` off and `(('~fmul', a, 0.0), 0.0)`
   would throw the bit away. `air.fmod.f32` goes from **47 of 264 cases differing to 1**.
   On a630 it is 18 ALU instructions, counted off `ir3_compiler`'s own disassembly: `rcp` and
   `mul.f` for the divide, `trunc.f`, `mad.f32`, then four `cmps`, three `sel.b32`, two `add.f`,
   three `and.b`, an `or.b` and an `absneg.f` for the corrections and the signs. The `mad.f32` is
   the whole of the precision argument, and on the Adreno it is fused.

   *What it still does not do, measured and not guessed.* `q` must be exactly representable,
   which means `|x/y| < 2^24` for a float and `2^11` for a half. Over 195 906 random pairs of
   normal floats every single disagreement with this machine's libm has `|x/y| >= 2^24`, and over
   376 438 random pairs whose quotient **is** representable there is none. The exact answer above
   that needs an iterated reduction — scale `y` up by `2^k` so the partial quotient stays under
   `2^23`, subtract, repeat — and each pass removes only 22 bits of a difference that reaches 253
   for two normal floats, so twelve passes of about ten instructions would be the price. It is not
   done, for a reason that is not the price: **its exactness on the Adreno could not be
   established from this machine at all**, because ir3 lowers `fdiv` to `fmul(a, frcp(b))` and
   `frcp` is approximate — a simulation of the sequence under a 1-ulp divide still misses 0.26%
   of the representable-quotient pairs — so shipping it would mean shipping a routine whose only
   claim cannot be checked here.

   **So the suite's verdict is made to say that, rather than being red with a story attached.**
   A differing `fmod` case is set aside by name if, and only if, **that case** satisfies the
   condition: the ratio is recomputed at the result's own precision from the case's own operands,
   a finite pair with a non-zero divisor is required, and the count, the declarations and the
   whole reason are printed in the summary rather than implied. The stage then passes. Five
   declarations and **23 cases** are in it today.

   What makes that a set-aside and not a blanket excuse is that it is decided case by case, and
   that was checked by breaking the thing it exists to catch: with the reference replaced by the
   two-rounding formula `nir_frem` computes — which differs from `fmod` at SMALL ratios —
   `air.fmod.f32` reports **35 of 264 cases differing** and fails the stage, with only 6 of its
   disagreements absorbed. A declaration whose *every* compared case fell into the set-aside
   would decide nothing, so that is a `NOCASE` failure as well. `--check` applies the same rule,
   so a run on the phone reads the same way — and on the Adreno it is the cases with
   `|x/y| < 2^24` that would show `frcp` costing more than `air_fmod`'s two corrections can
   absorb, which is exactly what the condition leaves able to fail.

   *And softpipe cannot judge the new lowering,* for the reason already written down for
   `air.fma`: `nir_to_tgsi` sends `ffma` to TGSI's `MAD`, whose own specification lets the
   product be rounded first and whose interpreter does. Folding leaves `air.fmod.f32` differing
   on 1 of 264 cases and running the same modules on softpipe leaves 22, every one of the extra
   21 the pre-`fma` value to the bit. In one shape it is worse than an ulp: a rounded product can
   land exactly on the dividend, so `fmod(0.5, 0.1)` comes back 0 where the answer is
   `0.0999999940`, because a zero result is the one thing the correction for a too-large quotient
   cannot be distinguished from. That is the interpreter's `MAD`; `fmod` joins `fma` and
   `fmuladd` in the run stage's named set-aside and is a question for the phone.

   **And two declarations of a *defined* family had no definition,** which `airdefs.py`'s own
   self-check was reporting as a divergence and nobody had read: `air.convert.u.v4i1.s.v4i64` and
   `air.convert.u.v4i1.u.v4i64` came back "declared in a defined family but not emitted", because
   the bool-destination branch raised `no vector bool destination is attested`. That was true when
   it was written and false by the time the device image's own declarations were being counted.
   There is nothing to guess at: `x != 0` is elementwise, which is what the reference already
   computes component by component, and an LLVM comparison over a vector is the same instruction.
   Nine vector-bool destinations are defined now, and over the 91 declarations of the corpus that
   touch a `v*i1` **3 769 cases agree bit for bit and none diverge**.

   **The meter was under-reporting, and that is fixed before any of the above is believed.**
   `collect_declarations` was `except OSError: continue` around a subprocess whose exit code was
   never looked at, so every library `air2nir` could not be executed for — and this tree is
   shared, so the binary is relinked under the check several times a night, which is `EACCES` or
   exit 126 — dropped out of the vocabulary without a word. Fewer declarations is fewer operations
   tested and a *higher* pass rate, the one direction a broken meter must never move in; it is how
   a log came to read "134 agree, 0 differ" while the vocabulary is 553 declarations wide. And
   `check.sh` piped the section through `tail -8`, which cut off the very first line — the one
   that would have said so. Both halves are closed: each unread library gets an `unread` line of
   its own with the reason, the count goes out on stderr at the start and again in the summary at
   the end so that neither a `tail` nor a dropped stderr can lose it, and `check.sh` keeps every
   `intrincheck:` line and trims the per-operation detail instead.

   And a run that compared **nothing** no longer exits 0, which until now it did: point `AIR2NIR`
   at a binary that cannot be executed — which is what a relink in this shared tree looks like for
   the second it takes — and every library dropped out, the vocabulary came back empty, and the
   summary read "0 agree, 0 differ" and passed. Two rules close it, and they are deliberately
   different claims: nothing compared is not a result either way, and a library that could not be
   **executed** for (EACCES, exit 126, exit 127) means the run measured less than it believes. A
   library that is simply not bitcode is not in that second set, because that is a fact about the
   library and would make the check permanently red for the fourteen it is true of. Both were
   checked by reproducing the symptom: a `chmod -x` copy of `air2nir` and a stub that exits 126
   each now fail the run by name, where both used to pass it silently.

   The bare `continue` went with it, in the other place it was hiding something. A case the
   reference has **no answer** for — a conversion out of the destination's range, a function's
   domain — was simply skipped, so a declaration could lose most of its cases and still read
   "agrees on every case". Those are counted by reason and by declaration now, beside the ones the
   reference *declines* (which already were). It came to **963 cases** the first time it was
   counted and to **831** once the crash below was fixed: 264 outside a destination's range, 228
   an `invalid operation in fma`, 198 a domain error from `powr` of a negative, 62 an overflow in
   the expectation itself, and so on, every one of them named with its declaration.

   **And the first thing that count found was a crash in the reference and six declarations
   reported as agreeing over zero cases.** 123 of the 963 came back as *invalid literal for int()
   with base 10: 'alf'* and *'loat'* — `ref_convert`'s float-source branch fell straight through
   into its float-to-integer code for a float **destination** and evaluated `int("alf")` for a
   `half`. So every case of the corpus's six float/half conversions raised, every one was dropped
   in silence, and `if bad:` over an empty list is false, so each of the six went into the
   agreeing count: `exact air.convert.f.v2f32.f.v2f16 0 cases`. The conversion is one rounding to
   the destination's precision and nothing more; with that written, the two `v2` forms compare
   **44 cases each and agree**, and a declaration with no compared cases is now named `NOCASE`,
   counted apart and fatal. That is the whole argument for counting what you drop: the drop was
   not hiding a disagreement, it was hiding the fact that nothing had been asked.

   What it says now, which was invisible before: **14 of the 128 `.metallib` files under
   `ios165/rootfs` cannot be read at all,** and the reasons are four different things:

   - **nine are not LLVM bitcode** — no `BC` 0xC0DE magic, so a container this reader does not
     know rather than a parse failure. Five are AGXMetalA11's ray-tracing runtime (`ei_rt`,
     `metal_rt`, `metal_rt_precise`, `raytracing_rt`, `vft_rt`) and four are CoreImage's
     `*_archive_bin` (`photo_style`, `portrait_filters`, `portrait_filters_fullsize`,
     `redeye_repair`).
   - **three are empty**: Metal.framework's `default.metallib` and `MTLMeshShaderEmulator.metallib`
     and WeatherUI's `default.metallib` are 118 bytes each — the same 118 bytes as
     SafariSharedUI's, which the corpus list already names as holding no functions at all. Nothing
     is lost with these, and `air2nir` exits 1 for them without a word, which is worth its own
     message some day.
   - **one is a reader gap with a name**: `libtracepoint_rt_ios.metallib` stops at
     `__tracepoint_data.p0i8: thread-local global`.
   - **one is a resource limit**: `ci_filters.metallib` is killed by the kernel for memory
     (`exit -9`), and it is the one of the fourteen that could be hiding vocabulary, because it is
     the only one that is large, ordinary bitcode. It is also why the declaration count moves by
     one between runs — 552 or 553 — which is itself a thing this reporting makes visible instead
     of leaving as noise in a pass rate.

   None of those four is this task's to fix. What matters here is that the vocabulary rests on 114
   libraries of 128 and the check now says so, instead of reporting a pass rate as though it were
   all of them.

   **The check can fail,** which is the first thing to establish about any check in this
   repository, after four that passed for the wrong reason. Reinstating the `fast_round` bug —
   `nir_fround_even` in place of the trunc-and-step — is caught at `0.5`, `-0.5`, `±2.5` and in
   each component of the vector form separately; dropping the last component of `air.dot` is
   caught on 83 of 100 cases. Every component of a vector result is compared, and every component
   of a vector argument is given a *different* value, because with the same value in all of them a
   lowering that swizzled or dropped a component returns the right answer for the wrong reason.

   **Folded is not run, and the difference is not a matter of degree.** `--eval` asks NIR's
   constant folder; `intrincheck.py --run` builds the same operations into shaders that *load
   their arguments from a buffer* and hands them to `air_run`, which draws with them through a
   real driver's compilation and execution. The buffer is the whole point: ir3 folds too, so a
   shader calling `air.fast_round(0.5)` on an immediate is answered by the compiler and the
   hardware never sees the instruction — a GPU test built that way passes with the GPU switched
   off. A value loaded from a buffer cannot be folded, and `air_run` puts bytes the harness chose
   there (`AIR_RUN_BUFFER`, added for this).

   **60 of the 148 declarations the suite covers agree on every one of 808 cases, run by softpipe
   rather than folded; none differ.** Twelve more are the approximate ones. Seventy-five touch a
   type narrower than 32 bits (`i1`, `i8`, `i16`, `f16`) and TGSI's registers are 32-bit, so
   softpipe cannot answer for them: `nir_to_tgsi` asserts for the integers, which is loud and
   fine, and for a half it computes at 32 bits instead — it answered `32767` where a half rounds
   to `32768`, which is a verdict about softpipe and not about this frontend. The last one is
   `air.fma.v4f32`, for the reason in the third bullet below. `--check` reads the screen out of
   `air_run`'s own first line and sets all of them aside by name. ir3 has real 16-bit registers
   and a real `mad`, so both sets are a question for the phone and are open.

   Running them found two things the folding check could not, and could never have:

   - **`air.sign(-0)` gave `+0`.** Metal's `sign` is OpenCL's, where `sign(-0)` is `-0` and
     `sign(NaN)` is `+0`. `nir_fsign` does not promise either; its own documentation says that
     for a zero source "there is a preference for the result to have the same sign, but this is
     not required (it is required by OpenCL)". So a backend is licensed to lose it, NIR's
     *constant folder* happens not to, and softpipe does — which is exactly why folding the
     shader could not show it. Same shape as the atan2 bug: an opcode whose contract is weaker
     than Metal's requirement. `air_intrinsics.c` now builds `sign` from comparisons, which
     promise what Metal needs, rather than asking for a preference.
   - **The 16-bit gap above, which is softpipe's and not ours** — but it took a wrong answer
     (`32767` for a half that rounds to `32768`) to notice, not an assertion, and a check that
     had trusted the number would have recorded a frontend bug that does not exist.
   - **And a third, on 21 October: `air.fma.v4f32` is one ulp out on softpipe, and softpipe is
     allowed to be.** `fma(1.5, 0x1.fffffep-2, -0.5)` came back `0x3e7ffffc` where the fused
     answer is `0x3e7ffffd`: exactly the difference between rounding the product and not. The
     chain is quotable at every link. Metal's `fma` is OpenCL's, which is C's, which is required
     to round once. NIR's `ffma` is `fmaf(src0, src1, src2)` (`nir_opcodes.py`), also one
     rounding, which is why folding the same shader agrees with the reference bit for bit.
     `nir_to_tgsi` maps `nir_op_ffma` to `TGSI_OPCODE_MAD`, and TGSI's own specification says of
     `MAD`: "Perform a * b + c. The implementation is free to decide whether there is an
     intermediate rounding step or not" (`docs/gallium/tgsi.rst`). `tgsi_exec.c`'s `micro_mad`
     takes that permission — `src0->f[0] * src1->f[0] + src2->f[0]`, two roundings in this build,
     measured and not assumed — and `nir_to_tgsi_compiler_options` sets `.fuse_ffma32 = true`,
     which is the same statement from the other side: for this backend an `ffma` and a
     multiply-then-add are interchangeable. So this is **not** a bug in us and **not** a bug in
     ir3 — it is a capability TGSI does not have, and unlike the `ldexp` one there is no
     contradiction in Mesa to file: nothing there claims `MAD` is fused. What there is no way to
     express is a frontend that *needs* it, which OpenCL and Metal both are, and `nir_to_tgsi`
     has no `lower_ffma` to say so with. `--check` therefore sets an `fma` aside by name when the
     screen is softpipe, exactly as it does the narrow types, and says why in its own output. The
     Adreno's `mad.f32` is a question for the phone and is open.

   **It has now run on the phone, and two operations of it are answered there.** `--emit DIR`
   writes the modules, the bytes for their input buffer, a `manifest.json` of what each should
   return, a `run.sh` for this machine and an `rc.jobs` for the device. `--check DIR LOG` then
   judges the log wherever it was produced, so the arithmetic reference never leaves the host and
   the device needs nothing but `air_run`. `manifest.json` is deliberately not carried: nothing on
   the device reads it, and leaving it behind keeps the stronger claim literally true — the device
   cannot be made to agree by reading the answer.

   **The reason this took a night longer than it needed to was a sentence in this document, and
   it was wrong.** It said that "getting that directory into the boot image is the flash-root
   machinery of subsystem (2), which this work is not supposed to touch". It is not. `/redwood/share`
   already existed and already carried a data file that a test reads (`pattern.bin`, which `init`
   verifies through `read()` and `mmap()`), and `EXTRA` already carried arbitrary host files into
   `/redwood/bin`. `EXTRA_SHARE` is ten lines of `build.sh`. A boundary the brief drew for good
   reasons had been reached for as the reason not to look into something, and the check that would
   have cost a minute — reading what `/redwood/share` is for — was never made. A constraint is not
   evidence about a thing it does not cover.

   **What the first run on the phone actually found was in the harness, not the GPU**
   (`logs/074504-run.log`, 20 October 07:45). The flash root mounted, the Adreno came up, and
   `air_run` opened it: `clearFragment01` and then two generated modules each printed
   `on Adreno 7c+ Gen 3 (the GPU, through the kernel's msm device)`. `air.fast_sqrt.f32` returned
   `1.44972523e-09  2.15926367e+15` and `air.dot.v4f32` returned `-inf -inf` — bit for bit what
   the host returns for the same module over the same pattern buffer, which is the comparison
   those two were put in the job list to make: a job line cannot set `AIR_RUN_BUFFER`, so they
   read `air_run`'s own pattern bytes, which the host can read too, and their numbers are
   therefore comparable without the manifest and without a shell.

   The other 98 of the 100 modules carried did not run, because the job list ended with
   `/bin/sh /redwood/share/airops/run.sh` and the answer was
   **`/bin/sh: No such file or directory`, status 127. iOS 16.5's own root has no shell at all**
   — `/bin` holds `df` and `ps`, and nothing else. The `/var/jb/bin/sh` that `rc.jobs.jb` and
   `rc.jobs.sileo` run belongs to the package manager's prefix, which only a `JB=1` image carries.
   This was asserted, not checked: a `grep` for `bin/sh` matched six `rc.jobs*` files and the
   matches were read as `/bin/sh` without opening a line of any of them — the same shape of
   mistake as the claim about `air_run`'s texture format a day earlier, a conclusion drawn from a
   pattern match instead of from the code. It is now written beside `RC_JOBS` in `build.sh`,
   because anyone writing a job list with a shell in it will meet the same 127.

   So the shell was removed rather than replaced. `air_run` grew `--buffer FILE` — `AIR_RUN_BUFFER`
   as an argument, which is the one thing `init`'s `run_command` can express — and `--emit` now
   writes an `rc.jobs` with one line per module. On the phone 27 modules ran in 0.6 s, about 22 ms
   each including opening the GPU, so 225 separate processes cost about five seconds and a loop
   inside one process would buy nothing worth the refactor. (An earlier estimate of 50–150 ms per
   module, taken from the spacing of nearby `launchd` lines, was wrong by a factor of five; the
   figure above is from the run that executed them.)

   **Two more things had to be found before the suite would report anything, and neither was in the
   GPU.** The first was in `init` and is written up where it belongs (`userland/src/init.c`,
   `read_whole`): three configuration files were each read with one `read()` into a fixed buffer
   with no check, so a file longer than its buffer silently lost its tail. The job list is 21 KB
   against a 8 KB buffer, so 94 of its 244 lines were read and about 150 modules did not run —
   which reads exactly like a test that found nothing. Caught before a run rather than after,
   by counting how many lines fit before claiming that 225 modules had passed.

   The second is a property of the loader's log and will catch anything verbose:
   **the log keeps the first 256 KB and the last 1 MB and drops the middle**, saying so in one
   line. With `XL_SECS=120` and `REST=1`, iOS wrote more than a megabyte of `os_log` after the
   suite had finished, so the suite's own output was in the dropped middle — 27 modules survived,
   at the front. `XL_FOLLOW`'s intermediate log does not help, because the drop has already
   happened by the time it is fetched, and a filter removes lines that are already gone. Two levers
   keep a run under the cap and neither is sufficient alone — `QUIET=1`, which drops `launchd`'s own
   thousands of lines, and a short `XL_SECS` — because the volume comes from the number of services
   and from the length of the session both. What is actually diagnostic is the size of the file:
   1 466 58x bytes *is* the cap, so a log that size is truncated however many drop markers it
   carries (the intact run here has two and lost nothing; a truncated one had one). `README.md`
   carries this beside `./xl log`, with the three runs tabulated, because it catches anything
   verbose and not just this suite.

   **How much room is left is worth knowing before the suite grows.** In the run that worked, at
   `XL_SECS=30`, `air_run`'s own output was 93 459 bytes for 225 modules — 415 bytes each — and the
   other 1 092 137 bytes, 92% of the log, was iOS booting *with* `QUIET=1` already on. So the
   headroom is about 280 KB, or roughly 650 more modules if they cost nothing in time, and fewer
   because each one adds 22 ms of further boot noise. `--cases 8` should still fit; past that it
   will not, and the answer then is not a bigger cap but to stop booting iOS for a measurement at
   all: `init` runs programs itself, before `launchd`, where the log is nearly empty and nothing
   can push the output out of it (`RC=<file>`, as `rc.ios` does it). The GPU is up by then, since
   the kernel brings it up, and a question about what the hardware does to a number does not need
   a window server. That is the shape this suite should take if it ever outgrows `--cases 4`; it
   has not yet, and the 225 modules above all arrived.

5. **The atomics, with a kernel written to make a lost update show.** This document used to say
   that none of Apple's kernels could test an atomic and why: "a sum written by threads that never
   raced looks exactly like a sum written by threads that did. Detecting a lost update needs the
   expected total to depend on every thread having contributed — which needs a kernel whose
   arithmetic is known, i.e. one written for the purpose rather than one of Apple's."
   `tools/air/atomiccheck.py` is that kernel, generated as AIR, assembled with `llvm-as` and
   dispatched by `air_run` over its 4×4 threads, with both device buffers' initial bytes chosen
   rather than left to the harness's pattern.

   Four of its five checks are ordinary: the counter reaches the thread count, the atomic load
   returns what was put there, the signed maximum is the largest thread index, and the signed
   minimum of `index - 100` is `-100` — that last one is where an unsigned minimum would answer
   `0xffffff9c` as a huge number, and where it did when the lowering was broken on purpose to see
   whether this check works.

   The fifth is the one that can see a race, and it needs no knowledge of the order the threads
   ran in. Each thread writes the value its own `fetch_add` returned, so if the increment is
   atomic those sixteen returned values are the numbers 0 to 15 in *some* order. A lost update
   leaves a duplicate and a gap, and the gap names the increment that went missing. Each is
   written as `old * 16 + index + 1`, so a slot no thread wrote cannot look like a result and a
   thread writing another thread's slot does not read as correct either.

   A second kernel does the threadgroup atomics and the barrier, which the first cannot reach —
   they go through `nir_shared_atomic` and a different path for the memory. Threadgroup memory is
   not zeroed at dispatch, and neither Metal nor Mesa promises it is, so the kernel initialises
   it: every thread storing 0 to the same two words does that without needing a branch. Then each
   thread adds 1 to one word and `1 << its own index` to the other, and after the second barrier
   reads both back into its own slot of two device buffers. Every slot must hold 16 and `0xffff`,
   and the two ways it can fail are told apart by *which* slot is wrong: a lost add clears a bit
   in every slot and the bit names the thread whose increment went missing, while a barrier that
   did not wait leaves a smaller number in the slot of the thread that ran ahead.

   **All eight operations agree.** The barrier is genuinely under test, which was a surprise worth
   recording: softpipe does not advance its invocations one at a time but in groups of four, so
   with `barrier_op` made a no-op every thread sees 4 of 16 increments and the check says so,
   naming the twelve threads whose bits are missing.

   **Atomicity is still not under test.** Softpipe applies a group's atomic lane by lane, so it
   cannot lose an update however the lowering is written, and the permutation comes out correct by
   construction. The check passes there by being right rather than by being unopposed. What would
   make it evidence is the same two kernels on the Adreno, where sixteen threads really do race,
   and that has not happened. It is also only the eight operations the corpus proves exist, read
   off its own declarations; `air_intrinsics.c` also handles `and`, `or`, `xor` and `xchg`, and
   those stay untested because the names Apple would emit for them are a guess, and a test of a
   guessed name is a test of the guess.

6. **The texture operations, on more than one texel.** `tools/air/texcheck.sh` runs the corpus's
   own texture shaders and 6 of its 55 return the exact texel on the Adreno; 49 cannot be run,
   most of them because they are `air.visible` library functions and not entry points. Two things
   that leaves undone, and the second is the serious one:

   - whole operations have no coverage from it at all — the texel fetch, the size queries;
   - **every one of those six samples the same texel**, `(1, 1)`, because that is where
     `air_run`'s vertex shader puts the coordinate. A lowering that ignored the coordinate
     entirely and always fetched `(1, 1)` would pass all six of them.

   `tools/air/texopcheck.py` generates a fragment shader per operation *and coordinate*, asking
   for the corners as well as the middle. **All 35 calls return exactly what was put there**,
   covering 13 of the 31 texture declarations: `read_texture_2d` with 32- and with 16-bit
   coordinates, the two size queries, `sample_texture_2d` and `sample_texture_3d`, each of those
   for float, signed-integer and unsigned-integer textures, and `gather_texture_2d`. The
   coordinates are immediates here, unlike the arithmetic: a texture fetch is not constant-folded
   by NIR or by ir3, so there is nothing for the compiler to answer in the shader's place and no
   buffer is needed to defeat it.

   The integer-typed textures work because `air_run` already picks the texture's format from what
   the shader samples into, with its own comment saying why — "a Metal `texture2d<uint>` read out
   of a float texture gives numbers that are neither right nor obviously wrong". An earlier
   version of this section said those declarations could not be checked for exactly that reason,
   which was written from the probe's output without reading the code beside it. They can, and
   the render target's type has to follow as well or the bits come back read the wrong way.

   A gather returns one channel of the four texels around the coordinate, and Metal fixes the
   order they come back in. They came back in GL's order, `(i0j1, i1j1, i1j0, i0j0)`, but that
   Metal's convention is the same is not established here — so the four values are compared as a
   multiset. That checks the footprint and the channel selection without committing to an order,
   and since every channel gives a different multiset a wrong channel still shows. The coordinate
   is `1.75/4`, which is `1.25` in texel space, so the footprint is `{1,2}×{1,2}` with nothing
   ambiguous about the rounding.

   Made to fail: swapping the first two coordinate components is caught on all five asymmetric
   coordinates and correctly *not* on the three symmetric ones, which is the difference between
   this and `texcheck.sh` stated as a measurement rather than an argument.

   **What it does not check**, and these are the texture half's real remaining gaps:

   - Metal's own sampler state. The sampler is the API's — nearest, clamped — and every
     coordinate is a texel centre, where nearest filtering has no weights to round and clamping
     has no arithmetic. The 64-bit `constexpr sampler` word is decoded now, and what it
     decodes to is checked, but only the modes the corpus attests (section 3.3).
   - The seven declarations whose result or coordinate is 16-bit. They have cases and softpipe
     refuses them: TGSI's registers are 32-bit, so `nir_to_tgsi` asserts rather than answering
     wrongly, which is the better of its two failure modes. ir3 has real 16-bit registers, so
     these are a question for the phone.
   - `write_texture_2d` and `write_texture_3d` (4 declarations), which need the texture read back
     after the draw, and the multisample reads (5), which need a multisample texture. Neither is
     bound by `air_run`.
   - The depth sample and the multisample depth read (2): nothing binds a depth texture, and a
     shader reading an attachment nothing stored to is one of the four tests this port has
     already had pass for the wrong reason.

7. **Framebuffer fetch, where no value can be compared.** `tools/air/fbfetchcheck.py`, 5 cases.
   This is the one interface key whose result nothing on this machine can answer: LLVM has no
   framebuffer and softpipe has no `FBFETCH`, so the check is of the *shape* -- the intrinsic at
   the target the module names, `info.fs.uses_fbfetch_output` set, a half target narrowed with a
   float conversion, an integer target refused, an `air.arg_unused` one not turned into a fetch --
   plus the fact that ir3 emits the Adreno instruction. It says so in its own last line rather
   than letting five passes read as five verified shaders (section 3.1).

8. **The lowerings done by hand, against LLVM on the processor.** `tools/air/loweringcheck.py`,
   16 modules. Three things AIR has and NIR does not, so the frontend builds them out of something
   else: a `switch` (a chain of `nir_goto_if`), a call returning a struct (one out-parameter
   per member), and a pointer chosen at run time (the choice kept, both sides loaded at the use). Counting modules says nothing about whether either *computes* the right thing —
   `reader-check.sh` proves the switch record is **read** right, since a case constant taken with
   the wrong value numbering is a textual difference against `llvm-dis`, and a chain that always
   fell to the default would print exactly the same `.ll`; a struct return with two members
   swapped would translate and reach ir3 without complaint. So each module here takes its inputs
   out of a buffer and `cpuref.py --compare` has llc run it on this processor, then compares
   LLVM's result with our folded NIR field by field. **12 agree with LLVM and 1 is refused by
   name, and the four switch arms reached are all four of them** — that last number is what makes
   agreement mean anything, and the script fails on fewer than three distinct arms even if every
   comparison agreed.

   The switch cases include two values sharing one arm, a last case whose destination *is* the
   default (the one place the chain emits a plain `nir_goto`), and an `i8` condition, which is
   what QuartzCore's own modules carry. The struct-return cases include the `[8 x i8]` tail
   padding Metal puts on a vertex-output struct — the member that must take no parameter at all —
   a struct with a vector member, and a callee with **two returns**, so the out-parameters are
   stored on two different paths. The refusal is **a phi whose type is the struct**: an aggregate
   here is a tree of values and a phi is a register holding a number, so there is no register for
   one. Nothing in either corpus needs it — Apple's own callees build the struct with
   `insertvalue` in the block they return from — and closing it would mean one register per
   member, the same shape the out-parameters already have.

   The two chosen-pointer cases take **opposite sides of the same select**, which is the care that
   makes them worth running: `cpuref` fills every buffer with one pattern, so a select between two
   buffers would have both sides holding the same bytes and a lowering that always took one of
   them would agree with LLVM and prove nothing. They select between two *offsets* of one buffer
   instead, which the pattern makes different, and the two cases return visibly different
   `float4`s. The third pointer case is the refusal: a pointer stored into memory, by name.

   Two things this exercise found in the harness rather than in the frontend, both fixed in
   `cpuref.py`: it linked its generated harness as PIE, so a module with a global of its own could
   not be built at all (`R_X86_64_32` against `.rodata`) — `-no-pie` now, which changes how the
   harness is linked and not what it computes; and it folds buffer loads before it folds anything
   else, so a load whose offset is still an `iadd` of an `ishl` is left alone. The second is not
   fixed, and the generated modules work around it by indexing through struct fields, whose offsets
   the frontend emits as immediates.

9. **Vertex attributes, where no value can be drawn.** `tools/air/vattrcheck.py`, 16 generated
   modules and 14 real ones. The second interface key with no oracle here, and for a different
   reason from framebuffer fetch: `air_run`'s only vertex stage is its own generated quad, so
   nothing on this machine draws with a translated vertex shader at all. What *is* decidable is
   the shader's half of an `MTLVertexDescriptor` — the Metal attribute index reaching ir3's own
   fetch table with the right write mask, the 32-bit input and its narrowing, the
   `driver_location` numbering Gallium matches elements by, `inputs_read`, and the seven shapes
   that must be refused by name rather than read from the wrong vertex. Then the same translation
   against Apple's own metadata **read a second time with a regular expression** over
   `air2nir --ll`, attribute by attribute over QuartzCore's fourteen `[[stage_in]]` vertex
   shaders: 46 attributes, and made to disagree on purpose the index comparison fails all 14 and
   the unused comparison the 8 that have an unused attribute. Section 3.1 has what the keys mean
   and what the contract half still has to carry.

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
  a matching pixel. (With textures done the whole corpus reached 313 of 456, then 318 once calls
  to a function of the module were translated, and 321 -- every entry point -- once the shuffle
  mask was decoded; this milestone's own count is the intrinsic-free subset.) The other 21 are
  `air.visible` library functions, which are not entry points at all (below). Counting every
  entry point rather than every module: 275 of 321, 86%. Nothing has run on the Adreno.
- **M3** — textures and samplers, each form with a test that renders a known image.
  **Translated, compiled, and six of them rendered on the Adreno and checked.** The family took the corpus from 275 modules to
  313, and all 313 compile for the Adreno. What is *not* done is M3's acceptance test: not one
  texture operation had been run until `air_run` learned to bind one. The forms written are 2D, 3D, 1D,
  cube and their array and multisample variants, for sampling (plain, explicit level, bias,
  minimum-level clamp), gathering, fetching by integer coordinate, writing, and the queries
  (width, height, depth, array size, level count, sample count), and **depth comparison**
  (`sample_compare`, section 3.3). Refused with a named error, not guessed at:
  `gather_compare` — four comparisons in one instruction, attested by nothing measured here —
  a comparison of a colour texture, a comparison through a sampler whose word says
  `compare_func::none`, texture buffers, and a depth texture whose aspect selector is not 1.

  **And the four packed 16-bit colour formats, which are served now, in the mirror of the
  order this document first argued for.** The window server does not merely sample them: it
  **renders into** `MTLPixelFormatBGR5A1Unorm` (43) with a full write mask, and declining the
  four costs the home screen its icons.

  Gallium's side is mechanical — the generated unpackers in `u_format_table.c` say which bits
  each name means, first channel at the least significant end — and Gallium carries *both*
  spellings of each of these four, so the two readings of Metal's names are both expressible
  and they **exchange red and blue**, leaving the alpha where the name puts it.

  | Metal | colours from the low end | colours from the high end |
  | --- | --- | --- |
  | 40 `B5G6R5Unorm` | `B5G6R5_UNORM` | **`R5G6B5_UNORM`** |
  | 41 `A1BGR5Unorm` | `A1B5G5R5_UNORM` | **`A1R5G5B5_UNORM`** |
  | 42 `ABGR4Unorm` | `A4B4G4R4_UNORM` | **`A4R4G4B4_UNORM`** |
  | 43 `BGR5A1Unorm` | `B5G5R5A1_UNORM` | **`R5G5B5A1_UNORM`** |

  **A boot chose the right-hand column, and the left one was an inference that was wrong.**
  Served under the left column, Apple's own compositor drew the home screen's wallpaper layer
  — flat, unblended, the bottom of the stack — as exactly `(255, 12, 12)` over 8 004 sampled
  pixels, where the wallpaper is blue (`logs/packed.png`). That is the sign this document
  wrote down in advance: *under the other reading red and blue are exchanged.* Inverting that
  pixel through our own packing and back out through each candidate layout, the only readings
  that make it a blue are those with red and blue exchanged; and of those, only the **pure
  exchange** — same offsets, same widths — leaves the rest of the picture clean. Re-rendering
  the whole measured screen through it gives a solid blue ground, white labels and a
  blue-to-violet Weather icon with a white cloud.

  **A third reading is refuted rather than merely unchosen.** Taking the whole name from the
  high end, alpha included — so that 43 would be `A1R5G5B5_UNORM` — moves the one-bit alpha
  from one end to the other and shifts the three five-bit fields by a bit. Re-rendered, it
  puts horizontal striping across every icon, which the real screen does not have.

  **What is measured here, in both directions.** `air_metal_test --pixel-formats` makes a
  texture of each format whose every texel is the same 16-bit word with **one field set**,
  draws Apple's own `texturedFragment2DFloat` over it, and reads which channel came back 1;
  then it renders that result **into** a target of the same format and reads the raw 16-bit
  word back out. **30 probes, 30 exact** — a sampled direction and a written one, and the word
  that goes in is the word that comes out, which four floats could never show because they
  would have gone back out through the same unpacking the write used. That catches a wrong
  `pipe_format` in the table; what it cannot catch is which column Apple meant, because both
  ends of it are ours. That part the screen settled.

  `RWA_PACKED16=0` declines all four again, which is how the boot before this one ran. It is a
  measurement switch and not a setting.

  **And colour blending, which this contract did not have at all — an absence that was visible in
  a photograph and in no log.** `rwa_render_shape` carried no blend state, so `air_metal.c` made
  every pipeline with `create_blend_state(... .rt[0].colormask = RGBA)` and nothing else: blending
  off, every draw opaque. CoreAnimation composites its layers with **premultiplied alpha**, so a
  label layer whose background is transparent black was drawn as **opaque black** — every icon name
  on the home screen in a black box, the Search pill a black rectangle, the dock edged in dark red.
  Nothing refused and nothing crashed. By this project's own standard that is worse than a refusal:
  it drew something wrong.

  **What the compositor asks for was measured before it was served.** Every CoreAnimation pipeline
  is built in one function (`CA::OGL::MetalContext::create_pipeline_state`, `0x188378090`) out of a
  55-entry blend table at `0x18855c092`, and of the 1079 pipelines in this device's own precompiled
  archive — `QuartzCore.framework/default.pipelinelib`, whose names carry the mode — **472 are
  premultiplied source-over** (One, OneMinusSourceAlpha, Add), 33 `dot`, 31 `add`, 19 `mul`, 13
  `din`, 3 `sap`, 2 `sbs`, 1 `scn`, 1 `ldg`, and **504 carry no blend state at all** because those
  layers blend inside their fragment shader. That last number is the trap: such a descriptor
  arrives at Apple's own defaults, which
  `-[MTLRenderPipelineColorAttachmentDescriptorInternal init]` (`0x181a2e9a8`) writes as the word
  `0x78001080` — source factors One, destination factors Zero, both operations Add, writeMask
  `0xf`, `blendingEnabled` **NO**. Every field but one is non-zero, so any rule of the form "a
  field is set, therefore this descriptor means to blend" would have blended half the home screen
  twice. It is the same lesson as compare function 8 (section 3.3): a default that looks like zero
  may not be a default, and a meaningful zero is not an absence.

  **The mapping is a table and not a cast, three times over.** `MTLBlendFactor` and
  `pipe_blendfactor` agree on exactly **two of nineteen** values — Gallium's enumeration is built
  around an invert bit (`ZERO` is `INVERT_BIT | ONE` = 0x11), so the plain factors run 1–10 and
  their one-minus forms 0x11–0x1a with holes, while Metal's run 0–18 with each factor beside its
  own one-minus. A cast is right for One and SourceColor, lands on no enumerator at all for seven
  values, and is **silently wrong for ten** — including the compositor's own `OneMinusSourceAlpha`
  (5), which a cast reads as `DST_COLOR`: the destination multiplied by itself, a dark wash that
  deepens wherever two layers overlap. `MTLBlendOperation` *is* the identity and is written out
  anyway. `MTLColorWriteMask` is the one most likely to pass with the bits backwards: Metal has Red
  at `1 << 3` and Alpha at `1 << 0` where `PIPE_MASK_R` is `0x1`, so the nibble is **reversed** —
  and four of the sixteen masks are palindromes (None, All, green|blue, red|alpha), so a suite
  built out of "write everything" and "write nothing" would not notice. Two independent sources pin
  the numbering: Apple's `MTLRenderPipeline.h` for this release, which
  `userland/src/redwoodmetal.m` `_Static_assert`s against so a disagreement is a *build failure*;
  and `Metal.framework`'s own value-to-string code on the device, where `MTLBlendFactorString`
  (`0x181b08730`) bounds its argument at 18 over a nineteen-entry table and
  `MTLColorWriteMaskString` (`0x181b08754`) tests bit 3 for "R" and bit 0 for "A", which *is* the
  reversal, read out of the framework that defines it.

  **The check is differential and it prices the ways of being wrong**: `air_metal_test
  --draw-blend`, a stage of `check.sh`. A known translucent source over a known destination, 25
  configurations, and for each one the answer is computed beside what **six named mistakes** would
  have produced — the factors exchanged, premultiplied read as straight alpha, the alpha half taken
  from the RGB half and the reverse, the factor table cast, the write mask cast. Every input is a
  dyadic rational, so the float cases are compared with `==`; the one case in the display's own
  `BGRA8Unorm` is compared in **bytes** against softpipe's own `float_to_ubyte`, whose rounding is a
  bit trick that disagrees with `(int)(f * 255 + 0.5f)` at 128 of the 255 tie points. **25 of 25
  give the arithmetic exactly, 9 of 9 refusals fire by name, and every one of the six mistakes is
  distinguished by at least two cases** — the run fails if any mistake is distinguished by none,
  because a check that cannot fail for the right reason is the thing this project keeps relearning.

  **Five things are refused by name rather than drawn.** Metal's four blend-colour factors (the
  colour is encoder state, `-setBlendColorRed:`, and this contract has nowhere to carry it, so the
  blend would be against the zero a Gallium context starts with and the layer would *vanish*); the
  four Source1 factors (dual-source needs a second fragment output at `[[color(0), index(1)]]`,
  which neither this contract nor this frontend has — the gap is ours, not a6xx's, which reports
  one dual-source target); `SourceAlphaSaturated` as an *alpha* factor only, where GL defines the
  factor as 1 and Metal's one equation gives `min(As, 1 - Ad)`; `Min` or `Max` with any factor
  other than One, where Metal multiplies by the factors first and GL ignores them, and softpipe and
  `fd6_blend.cc` do different things with what survives; and blending on a pure-integer attachment.
  Not one of those appears in any of Apple's 1079 pipelines, so the refusals cost the home screen
  nothing. Every one is written *here* and not left to the driver below, because `fd_blend_factor`
  ends in `DBG("invalid blend factor"); return 0` — and 0 is `FACTOR_ZERO`, so freedreno answers an
  unrecognised factor by making the term disappear, with no diagnostic in an ordinary build.

  **And then the device said it in its own words.** `logs/080121-run.log` is a boot with these
  fields in the driver, and the descriptors the window server passes are now counted rather than
  predicted: over backboardd's two lives, **50** pipelines with
  `attachment 0 format 80 blend 1 rgb 1/5 op 0 alpha 1/5 op 0 writeMask 0xf` — premultiplied
  source-over on the display's own `BGRA8Unorm`, exactly the mode CoreAnimation's table said it
  would be — against **48** with `blend 0 rgb 1/0`, which is the untouched descriptor whose every
  other field is non-zero; **106** lines for *attachment 1* with `writeMask 0x0`, the
  multiple-attachment shape that makes `independent_blend_enable` load-bearing (5 pipelines came
  back `writes 0x000000f0`, attachment 0 writing nothing and attachment 1 writing RGBA, so the bit
  went on); 6 of One/One; and from SpringBoard a `rgb 4/1`. **Not one of the nine refusals fired in
  the whole boot**, which is what the archive predicted. 240 attachment lines over 127 pipelines,
  64 of those attachments blending.

  **What that boot could NOT measure, and it is the write mask.** Every one of those 240 lines
  carried `writeMask 0x0` or `writeMask 0xf`, and both are palindromes under the bit reversal — so
  the home screen would composite correctly with the nibble backwards, and says nothing whatever
  about it. The evidence for the reversal is `Metal.framework`'s own `MTLColorWriteMaskString` and
  the four host cases that ask for 0x8, 0x1, 0xe and 0x7. Nor does the log say the picture is
  right: it says the descriptors arrive, the mapping accepted them and the draws happened.

  **What still needs a boot**: whether the Adreno's blender agrees, which no log can say. The
  photograph is that measurement — icon labels as white text on the wallpaper with no box behind
  them, the Search pill translucent, the dock without its dark red border
  (`userland/rc.jobs.blend` has the greps and the two ways it could come back still wrong).

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
  kernels (`air_run` dispatches those as of 20 October, but softpipe's `nir_to_tgsi` stops most of
  them — see the kernel table above); 11 are `air.visible` library functions; 4
  sample a multisample texture the harness does not bind; 1 writes only depth; 1 hits an
  assertion in softpipe's own NIR-to-TGSI; 8 are other translation refusals. **Not one returns a
  wrong number** — the failures are all a refusal to run, which is the shape of failure to want.

  `texturedFragment3DDepth` is worth naming because it cost a wrong-looking result: it returns
  `{undef, depth}` and stores *nothing* to its colour attachment, so reading the colour gave the
  clear value — a number that means nothing and looks like an answer. `air_run` now checks that
  the attachment it reads is one the shader actually stores to, and refuses when it is not.

  The sampler state was the gap that mattered for correctness, and it has a section of its own
  (3.3). It is closed as far as the corpus reaches: the 64-bit word is decoded from three
  independent sources, the frontend hands the runtime the fields as well as the word, `air_run`
  builds a sampler from them, and `sampcheck.py` checks the behaviour against Apple's own rules —
  15 cases exact. What stays open is everything the corpus does not attest: `repeat`,
  `mirrored_repeat`, a real border colour, bicubic, the mip filters, anisotropy, the LOD clamp and
  the comparison function are decoded and unexercised.
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

### 5b. Dispatching a kernel

`air_run` dispatches a 4×4 workgroup, binds every buffer the kernel declares — constant ones as
UBOs, device ones as writable SSBOs, all filled with the same `(i * 7 + 13) & 0xff` pattern — and
prints the device buffers afterwards. `MetalFaceMask_InitMinMax` is the first kernel through it:

```
  [ 0] 0x7fffffff   [ 1] 0x7fffffff   [ 2] 0x7fffffff
  [ 3] 0x80000000   [ 4] 0x80000000   [ 5] 0x80000000
  [ 6] 0xcac3bcb5   <- still the input pattern
```

which is exactly its NIR: six `store_ssbo`s, `INT_MAX` at offsets 0, 4, 8 and `INT_MIN` at 0xc,
0x10, 0x14. Six words written, nothing beyond them touched, and the name means what it does — a
min/max table initialised to the extremes.

**Two things that only showed because the input was a distinctive pattern**, and would have been
invisible against a zeroed buffer:

- the first dispatch ran **zero threads** and the buffer came back untouched. Metal's threadgroup
  size is a dispatch argument, so the frontend rightly leaves `workgroup_size_variable` set
  (section 7.4) — but softpipe reads the block size from `TGSI_PROPERTY_CS_FIXED_BLOCK_*`, which
  is then zero. Whoever dispatches has to fix the size; `air_run` now does. A kernel called
  `InitMinMax` writing zeros to a zeroed buffer would have looked like a pass.
- word 6 holding the pattern is what says the kernel wrote *only* its own 24 bytes.

**What this does not test: atomicity.** Seventeen of the forty kernels that translate use a global
atomic, and a sum written by threads that never raced is identical to one written by threads that
did. A test that could fail needs a kernel whose arithmetic is known — one written for the purpose
rather than one of Apple's — with the thread's own identity in what it writes. Until that exists,
a kernel that uses atomics and produces sensible numbers here has been shown to *run*, and nothing
more.

### 5c. A suspected bug in this front end that the evidence does not support

Worth writing down because the next person will meet the same shape and should not spend the
evening I nearly spent on it.

`MetalFaceMask_MinMax` is the one texture-reading kernel of the corpus that dispatches on
softpipe, and it writes nothing: `air_run` prints the same silence on the pattern fill, on
`--zero` and on `--ones`. Reading its NIR turns up an 8-bit value that its `ssbo_atomic imin/imax`
stores are guarded by, and that value's incoming edge from the entry side is `undefined` --

```
    8       %0 = undefined
    8      %99 = phi b0: %0, b15: %291
```

-- while the module's own LLVM IR has a constant there:

```
    %34 = phi i8 [ 0, %6 ], [ %117, %45 ]
```

which looks exactly like a constant lost in translation, and the obvious guess is an `i8` source
that this front end failed to materialise.

**The guess does not survive a minimal reproduction.** A module with the same shape -- an 8-bit phi
whose entry edge is a constant, in a loop that never runs -- translates correctly:

```
    8      %3 = phi b0: %1 (0x5), b4: %9 (0x9)
```

and a 32-bit flag in the same module behaves identically, so there is no width-dependent path here.
(One thing the reproduction did show, and it is a limit of the harness rather than of the
translation: `air2nir --eval` folds no loop at all -- neither the 8-bit nor the 32-bit version
reduced to a constant -- so "did not fold" from `--eval` says nothing about a kernel with a loop.)

So the `undefined` in that kernel's NIR is consistent with something else entirely: a value that is
dead on that edge -- a loop that always runs at least once never uses the entry value of such a
flag -- and NIR replacing a dead source with undef is correct, not a loss. **Why the kernel writes
nothing is still unexplained**, and the two candidates not told apart here are "the guard is never
true for the inputs a harness can give it" (which would make the silence correct) and a real defect
elsewhere. What would settle it is reading the flag's computed value with an input that is known to
set it, which needs what the buffers mean -- CoreImage's knowledge, not the compiler's (section 5).

### 5d. The fourteen kernels, given the values their loops leave on

The table above says 14 of the 47 kernels cannot be dispatched here at all, because a loop that
leaves on a count or a sentinel from a buffer does not leave when the harness fills buffers with
noise. That is no longer the whole truth. `tools/air/kinput.py` works the exit values out of each
module's own metadata -- the struct layouts are in `air.struct_type_info`, so the harness can be
told where a `rowCount` and a `nextSpanID` live without being told what they mean -- and prints
them as `air_run --zero --buffer N@OFF=HEX` job lines (`userland/rc.jobs.kinput`). Eleven of the
fourteen have a line; with it, the kernels do end.

What the lines then bought was not a column of agreements. They bought **three compiler bugs, each
of which had been invisible for want of an input**, one correction to the reference, and two words
that turned out to belong to the harness's dispatch rather than to anybody's bug. None of the three
is in the translation of a loop; all three are in how a byte offset becomes an address.

Every number below was measured twice: once on this machine by Apple's own LLVM IR for the module,
compiled for the host and run over the same bytes (a scratch simulator, not a repo tool -- it
settles termination and control flow and says nothing about the hardware, nor about anything written
after a barrier: it runs the threads one after another, which is a schedule and not a reference, and
it has `--order` and `--block` so that a reader can see which of its numbers move when the schedule
does), and once on the phone's Adreno 7c+ Gen 3 through our translation.

**1. A sub-dword SSBO access on the Gallium freedreno driver reads and writes at the wrong
address.** `MetalFaceMask_AddSeedPoint` stores one `uchar` -- `spans[p].fillValue` -- and on the
CPU, with these bytes, byte 28 of its span table goes from `0xff` to `0x5a` and nothing else in any
buffer changes. On the Adreno the dispatch finished and byte 28 was untouched; `0x5a` appeared at
**byte 672** (`logs/065057-run.log`). 672 is not a near miss, it is the arithmetic:

- The `ushort` load of `spans[0].parentSpanID` is `ldib.b.untyped.1d.u16.1.imm hr1.x, r1.x+9, 3`.
  Element 9 of a 16-bit access is byte 18, and byte 18 holds 0. The device read byte **36**, which
  holds `spans[1].startX` = 10.
- With `p` = 10, the store's offset is `p * 14 + 28` = 168, and the instruction is
  `stib.b.typed.1d.u16.4.imm hr1.y, r2.x+28, 3`. Byte 168 of an 8-bit access is byte 168. The
  device wrote byte **672** = 168 x 4.

Twice the offset for a 16-bit access, four times for an 8-bit one: in both cases the ratio of four
bytes to the size of the access. Mesa says why. `ldib`/`stib` take an offset in the texels of the
IBO descriptor, and ir3 divides the byte offset by the size of the *instruction*
(`lower_offset_for_ssbo()`: shift 2, 1 or 0 for 32, 16 or 8 bits). Turnip keeps the two agreeing by
writing a descriptor per width and selecting one -- `write_buffer_descriptor_addr()` lays down
FMT6_16_UINT, FMT6_32_UINT and FMT6_8_UINT side by side and `tu_lower_io()` adds 1 to the
descriptor index for an 8-bit access. The Gallium driver writes one descriptor per SSBO and it is
always `PIPE_FORMAT_R32_UINT` (`fd6_ssbo_descriptor()`), and leaves
`ir3_compiler_options::storage_16bit` and `storage_8bit` false -- which ir3 already consults
elsewhere, for the buffer length, "a byte length divided by IBO_0_FMT". Nothing in GL reaches this,
because a GLSL SSBO member is 32 bits and 4-byte aligned; a Metal `ushort` field is not.

`tools/mesa/ir3-ssbo-subdword.patch` makes `ir3_mem_access_size_align()` ask for dwords when the
descriptor is one. `nir_lower_mem_access_bit_sizes` then reads with a masked offset and a shift and
writes with the `iand`/`ior` pair of 32-bit atomics it already keeps for this -- atomics, because
sixteen threads write `uchar` fields that share a dword. After it, the eight `MetalFaceMask_*`
kernels of the eleven have **no narrow IBO instruction left**: 6 of AddSeedPoint's 7 were narrow,
and 0 of 6 are.

**2. That same mis-scaling is why `MetalFaceMask_PropagateParents` wedged the GPU**, in
`logs/062627-run.log`, and the wedge cost the nine lines under it. Its loop is a self loop leaving
on `icmp eq i16 %35, -1` where `%35` is `spans[id].nextSpanID`. ir3 vectorises the two 16-bit reads
of a span into `ldib.b.untyped.1d.u16.2.imm hr0.z, r6.y+8, 1`, elements 8 and 9 -- bytes 16 and 18
of the span, `nextSpanID` and `parentSpanID`. At four bytes a texel those elements are bytes 32 and
36, and the input's four span lists turn into a cycle: `firstSpanInRows[tid]` read at byte `4*tid`
gives 0 for every thread, span 0's `nextSpanID` read at byte 32 gives 1, span 1's at byte 60 gives
3, span 3's at byte 116 gives 0 -- **0, 1, 3, 0, 1, 3** and no 0xffff anywhere in it. Correct
addressing reads 0xffff from byte 16 + 14*id and leaves after one iteration. So the loop is not
mistranslated; it is walking a list the addressing invented.

This also tells the two candidates apart, which the hang alone could not: our lost `-1` (below) is
*not* needed to explain it. With the device's thread shape every thread of this dispatch passes the
bounds check, so the edge that lost its constant is never taken -- and with correct addressing the
loop ends whether that edge is right or wrong.

**3. `(C + x) >> s` is not `(x >> s) + (C >> s)` when C is negative**, and ir3 was rewriting it
that way. `create_shift()` in `ir3_nir_lower_io_offsets.c` splits a constant out of the offset so
that `nir_opt_offsets` can fold it into an immediate. `grid_buf[id - 1].n` arrives as `iadd
0xfffffffc, id*24`; `(id*24 - 4) >> 2` borrows from the bits above the two it drops, and
`(id*24 >> 2) + (0xfffffffc >> 2)` adds 0x3fffffff where it needed 0xffffffff. The two differ by
2^30, the element offset lands a gigabyte past the buffer, and an out-of-range `ldib` returns zero
without saying so. `fbs_bistochastize_iter` reads exactly that, and its four outputs should all be
1/sqrt(3):

| word | the Adreno | which is | Apple's LLVM on the host |
| --- | --- | --- | --- |
| 0 | 0x3f13cd3a | 1/sqrt(3) | 0x3f13cd3a |
| 1 | 0x3f000000 | 1/sqrt(4) | 0x3f13cd3a |
| 2 | 0x3ee4f92e | 1/sqrt(5) | 0x3f13cd3a |
| 3 | 0x3ed105ec | 1/sqrt(6) | 0x3f13cd3a |

1/sqrt(3+i) in word i is the signature of that read returning 0 for every thread: the loop then
runs `i + 1` times instead of one, and sums `i + 1` ones onto its 2.0. The three `fbs_*` kernels of
the eleven carry the shape; the eight `MetalFaceMask_*` ones do not.
`tools/mesa/ir3-shift-split.patch` splits the constant out only when it survives the shift and
cannot be a subtraction. In the Adreno assembly the `0x3fffffff` in the constant file is replaced
by `add.u r0.w, -4, r0.w` before the shift.

**4. And one bug that is ours: a constant on a phi edge became `undefined`.** `AddSeedPoint`'s
`phi i16 [ %30, %27 ], [ -1, %22 ]` translated to `phi b3: %92, b4: %0` with `%0 = undefined`, and
the sentinel the kernel returns on was gone. The cause is not the phi. `air_get()` builds a
constant the first time it is asked for and caches the def; built at the cursor of that first use,
a constant LLVM shares between a conditional block and a block after it is a use its def does not
dominate -- and nothing complains, because `nir_lower_goto_ifs` runs `nir_repair_ssa`, which
repairs such a use into a phi whose other edge is `undef`. `tools/air/phiconst.ll` reproduces it in
twenty-five lines, and a one-character change to them -- `-1` on the edge spelled `-2`, so that it
is no longer the same constant as the compare's -- makes it translate correctly even without the
fix, which is what says the sharing and not the width is the cause. Constants are now built at the top of the entry block, which
dominates everything. Over the corpus that changes **2 modules of 456** (`AddSeedPoint` and
`PropagateParents`); the 47 others whose NIR holds an `undefined` keep it, and the ones read are
unused vector components, which is what `undefined` is for. `container 456, reader 456, translate
321, ir3 321` before and after.

**5. The reference was wrong about one thread id, and right about the other.** A scalar
`thread_position_in_grid` in a 4x4x1 dispatch is the x component -- 0..3, four threads to a value
-- which is what `air_to_nir.c` emits and what the device does. The simulator had that right. What
it had wrong was `thread_position_in_threadgroup`, declared `ushort` by `fbs_pcg_init` and
`fbs_pcg_iter1`: it had been given the flattened `y * 4 + x`, which is
`thread_index_in_threadgroup`, a system value neither kernel asks for. Corrected, those two write
one byte more each than the reference recorded. **`fbs_bistochastize_iter`'s reference did not
change**, so the 1/sqrt table above is not a thread-shape artefact.

**6. The two words still left over are the dispatch's, not the compiler's.** After 1 and 3, two
words of the ladder differ from the reference, and both are the same word of the same idiom.
`fbs_pcg_init` ends by summing all 128 entries of a `threadgroup uint[128]` and adding the sum to
`lnew_out_buf` with one `atomic_fetch_add`; `fbs_pcg_iter1` does the same into `alpha_out_buf`; each
guards the sum with `if (tid == 0)`. That idiom is written for a one-dimensional threadgroup of 128
threads -- the array is 128 long, the reduction's bound is the literal 128, and exactly one thread
of such a group has `tid == 0`. `air_run` dispatches 4x4x1 in one workgroup, and in a
two-dimensional group a scalar `thread_position_in_threadgroup` is the x component
(`thread_index_in_threadgroup` is the flattened one, and neither kernel asks for it), so **four**
threads have `tid == 0` and only four of the 128 entries are ever written. The word that comes back
is therefore

    4 x (the four entries this dispatch wrote + whatever the other 124 held)

and every boot that has run these lines says exactly that. Each sum below is read off the same run's
own output buffers, which the log prints beside it: the entry a thread stores is `(uint)(ldexp(r*d,
20) + 0.5)` for `fbs_pcg_init`, from the `r_out_buf` and `d_out_buf` words of that run, and
`(int)(ldexp(q*d_in, 20) + 0.5)` for `fbs_pcg_iter1`, from its `q_out_buf` words and the input's
`d_in` of 1.

| boot | kernel | sum of the entries written | the device's word | left in the other 124 |
| --- | --- | --- | --- | --- |
| 065057 | fbs_pcg_init | 0x00060000 | 0xe2180000 | 0x38800000 |
| 065057 | fbs_pcg_iter1 | 0x00300000 | 0x00c00000 | 0 |
| 075648 | fbs_pcg_init | 0x00100000 | 0x00400000 | 0 |
| 075648 | fbs_pcg_iter1 | 0x00600000 | 0xe3800000 | 0x38800000 |

The factor is exactly four in all four rows, over three different sums, and the leftover is either
nothing or the same 0x38800000 -- one value, in two different kernels in two different boots, which
is what threadgroup memory nobody wrote looks like. (The `065057` sums are small because that boot
predates the two patches above; the arithmetic here uses that run's own numbers, so the two findings
do not lean on each other.)

**What else would give the same answer.** The arithmetic alone cannot tell "four adders over four
entries" from "one adder over sixteen", because 4 x 4 is 16 x 1. Three places decide it, and none of
them is arithmetic: our NIR asks for the x component and nothing else (`%17 =
@load_local_invocation_id`, `%18 = mov %17.x`, `%19 = u2u16 %18`, which is `fit()` trimming a vector
system value to the one component the shader declared); ir3 takes `load_local_invocation_id` from a
three-component hardware input and derives the flattened index from the id and never the other way
round (`create_sysval_input(SYSTEM_VALUE_LOCAL_INVOCATION_ID, 0x7)` and `lower_cs_local_index_to_id
= true`), so `hr0.x` in the assembly is x; and `air_run` dispatches `.block = {GRID, GRID, 1}` with
`.grid = {1, 1, 1}`, one workgroup of sixteen threads rather than sixteen workgroups of one. So the
four is four threads, and the translation of this kernel is right.

**And the reference cannot state this word either, at any dispatch size.** The simulator's
0x00100000 and 0x00600000 are its schedule and not the module's arithmetic: it runs the sixteen
threads one after another, so the four adds never accumulate -- every thread with `gid == 0` (also
the x component, also four threads) re-zeroes the counter at the top of the kernel, so only the last
of the four adds survives. Walk the same sixteen threads with x in the outer loop instead of the
inner one, changing nothing else, and the same bytes give 0x00040000 and 0x00180000, a quarter of
the recorded numbers, from the same program over the same input. A value written after
`air.wg.barrier` is not something a one-thread-at-a-time reference can have an opinion about, so the
simulator now counts barrier crossings and says so, and these two are the only ones of the eleven
that cross one at all.

**What the right dispatch would be, and what it predicts.** 128x1x1 in one workgroup. Then `tid`
runs 0..127, every entry of the array is written (a thread with `gid >= N` stores 0 -- that is what
the kernel's other branch is for), exactly one thread adds, and the word becomes a function of the
input alone. With the ladder's bytes, which set `N = 4`, it is 0x00100000 for `fbs_pcg_init` and
0x00600000 for `fbs_pcg_iter1` -- the two numbers the `AFTER` table already held, right for the
dispatch the kernel wants and wrong for the one it got. `air_run` hard-codes its threadgroup shape,
so making these two rows equalities means giving it a per-job one. Until then the table requires
neither word and says why.

**What is not claimed.** The three patches are shown to change the Adreno assembly in the predicted
way on this machine; **none of them has run on the phone**, and until one does, every "after" in
this subsection is a prediction. Of the eleven kernels, two were measured against the CPU in the
`065057` boot and differ for the reasons above, one agrees by writing nothing (`ResolveAssociations`
-- its loop is never entered, so the agreement is weak), one printed its bindings and then hung the
process rather than the GPU (`DrawSpans`, below), and three never got their turn. `GenerateMask`'s
reference is order-dependent -- sixteen threads claim span indices through an atomic counter, and
which thread gets which index decides the bytes -- so its `AFTER` table is one legal outcome and
not the only one.

**A fourth thing to look at, from the host only.** `DrawSpans` declares
`texture2d<half, write>` and stores `air.write_texture_2d.v4f16`, so ir3 emits
`stib.b.typed.2d.u16.4` -- a 16-bit typed store. `air_run`'s `image_shapes()` picks the image
format from the base type alone and binds `R32G32B32A32_FLOAT` for anything that is not an integer,
so the store is 16 bits wide against a descriptor that says 32. That is the same shape of mistake
as 1 above, in the image path instead of the buffer path, and it is untested ground in Mesa: GL has
no 16-bit image store and turnip matches the shader's width to the format. **22 kernels of the
corpus emit a narrow typed image store**, `DrawSpans` and `DrawEye` among the eleven. The draw path
of `air_run` already chooses `R16G16B16A16_FLOAT` when a fragment shader returns halves; the
compute image path does not. Whether that is what hung the process is not settled here -- it would
have to be a fault that `pipe->flush` never returns from, since nothing after `launch_grid` printed.

## 6. Running it

```
tools/air/mk-host.sh            # Mesa for this machine (x86-64): the frontend, ir3, softpipe
tools/air/check.sh              # the reader against llvm-dis, the corpus report, LLVM, the operations
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
tools/air/intrincheck.py [-v] [OP ...]                     # one air.* operation at a time, folded
tools/air/intrincheck.py --run --few [OP ...]              # the same, drawn by a real driver
tools/air/intrincheck.py --emit DIR --few                  # modules + inputs + run.sh, to carry
tools/air/intrincheck.py --check DIR RUN.LOG               # judge what the runner printed
tools/air/atomiccheck.py [--keep DIR]                      # the atomics, with its own kernel
build/mesa-host/build/src/gallium/drivers/freedreno/air_run \
        FILE[.metallib] [FUNCTION]                         # draw with it, print the pixel
tools/air/texcheck.sh [FILE.metallib ...]                  # every texture shader, the texel it returns
tools/air/texopcheck.py [-v] [--keep DIR]                  # the texture calls, several texels each
tools/air/vattrcheck.py [-v]                               # [[stage_in]] attributes, and what must be refused
build/mesa-host/build/src/compiler/air/air2nir \
        --interface FILE[.metallib] [FUNCTION]             # what air_shader_info says the shader expects
tools/air/texdecl.sh [FILE.metallib ...]                   # what each function declares per texture slot
build/mesa-host/build/src/gallium/drivers/freedreno/air_metal_test \
        --textures FILE.metallib [FUNCTION]                # the same, one library, from the library itself
```

`texdecl.sh` prints an expectation, not a measurement: the slot, `MTLTextureType`, the element class and
`air.arg_type_name` itself -- what `rwa_module_textures` answers the driver (`tools/mesa/air/air_metal.h`).
`userland/src/metal_drv_test.m --draw-all` uses that same answer to bind textures **per shader**: before it, the
sweep bound one view type and one format for the whole library, and 13 of the 271 fragment shaders of
`AppleMetalGLRenderer` were refused by name -- they declare `texture3d`, `texture2d<int>` or a
multisampled texture. The refusal was honest, but it was about the sweep, not about the shaders.

`intrincheck.py` with no argument checks every operation the corpus calls; with one or more names
it checks only those, which is how it is used while changing a lowering:
`tools/air/intrincheck.py atan2`.

`--run` is the same operations *executed* instead of folded, and it is a different check, not a
slower one — section 4 item 4 says what each can see that the other cannot. `--emit` writes the
modules, the bytes for their input buffer, a `manifest.json` of what each should return and a
`run.sh` that uses only `for`, `echo` and parameter expansion, because what runs it may be the
phone's Darwin userland and not this machine's shell. `--check` then judges the log wherever it
was produced: the arithmetic reference never leaves the host.

`0x007506030500` is how freedreno names the redwood's Adreno 7c+ Gen 3 (A635); it has no
`gpu-id`, which is why `tools/mesa/air-ir3.patch` adds `--chip-id` to that tool.

There is no hosted CI: the corpus is Apple's code and stays out of the repository (and out of
GitHub). `check.sh` is the CI; it takes about five minutes, most of it the two per-operation checks, so it runs before every commit that touches
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

## 6a. Handing this to a runtime: what it has to do, and what it gets for free

Written for whoever builds subsystem (2), the `_MTLDevice`/`IOGPUMetalDevice` object graph
(`docs/metal.md` section 4 weighs the three paths). This section is the interface, not advice: it
says what this frontend hands over and what it needs back, so that nobody has to read
`air_run.c` to find out. Where a claim here has been exercised, it says by what; the rest is a
promise this side keeps and nothing has tested.

**One call does the translation.** `air_to_nir(mem_ctx, bitcode, size, entry_point, options,
nir_options, info, &error)` takes one module of a `.metallib` — `air_metallib_parse` splits the
container — and returns a `nir_shader` plus an `air_shader_info`. On failure it returns NULL and
an error of the form `<category>: <what>`, where the category is one of `reader`, `stage`,
`interface`, `instruction`, `intrinsic` or `library`, so a runtime can tell "we do not read this
bitcode" from "we do not implement this operation" without parsing prose. Nothing in it needs a
GPU, a device or a file: it is a pure function of the bytes.

**What `air_shader_info` tells the runtime, field by field.**

| | |
| --- | --- |
| `stage` | `MESA_SHADER_VERTEX`/`FRAGMENT`/`COMPUTE`, or `MESA_SHADER_NONE` for an `air.visible` library function — which is not a pipeline stage and has to be linked into a caller |
| `entry_point` | the function's own name, for caching translations by it |
| `resources[]` | everything the shader expects bound, each with Metal's **own** index: `AIR_RESOURCE_CONSTANT_BUFFER` (a UBO), `DEVICE_BUFFER` (an SSBO), `TEXTURE` (a sampler view), `IMAGE` (a storage image), `SAMPLER`, `THREADGROUP_BUFFER` |
| `resources[].read/write` | from the shader's own access qualifier: a texture it writes is an image and has its own binding space |
| `resources[].size` | `air.buffer_size` when the module states it, else 0 — a runtime that wants bounds has to get them from the API call |
| `resources[].name`, `.type_name` | Apple's `air.arg_name` and `air.arg_type_name`, carried through unchanged: `"float4"`, `"texture2d<float, sample>"`, `"float4x4"` |
| `resources[].inline_sampler`, `.sampler_state`, `.sampler` | a `constexpr sampler` the shader declares itself: the raw 64-bit word **and** its fields decoded (section 3.3) |
| `varyings[]` | vertex outputs and fragment inputs by **name**, with the slot this frontend assigned: a pipeline-time link must give the same name the same slot on both sides |
| `num_render_targets`, `threadgroup_bytes` | what the fragment stage writes, and the static threadgroup memory the kernel needs |
| `raster_order_groups`, `resources[].raster_order_group` | the raster order groups the shader declares, as a bitmask and per resource (−1 for none). a6xx orders a whole draw or nothing, so this is a thing the runtime has to over-provide rather than match — see the fetch's two known things above |
| `notes[]` | what was translated **with a caveat**: the shader was accepted and something other than exactly what it asked for was done. Not errors (an error is a refusal, and `air_to_nir` returns NULL) and not advice. `air2nir --report` prints them; `corpus.py` counts them |
| `fbfetch_targets` | the render targets the fragment stage **reads** (`[[color(n)]]`), one bit each. Not implied by `num_render_targets`: `inplace_copy` reads target 0 and declares no colour output at all, and `path_blit_frag` reads target 2 while writing 0, 1 and 2 — so a runtime that sizes the attachment list from the writes alone would leave the read one unbound |

**What the runtime owes in return**, in the order a frame needs it:

1. **Bind by Metal's indices.** `setFragmentBuffer:offset:atIndex:` goes to the UBO or SSBO slot
   `atIndex` of that stage, whichever kind `resources[]` says that index is. The frontend emits
   `load_ubo`/`load_ssbo` at exactly that index and takes no offset of its own, so a non-zero
   Metal offset is the runtime's to fold into the binding — and Adreno's base alignment is
   coarser than Metal's 4 bytes, so a runtime that cannot align will have to pass the remainder
   in a driver uniform, which this side does not yet read. *(Exercised only through `air_run`,
   which binds by these indices and gets the right numbers back.)*
2. **Create a sampler from the decoded fields** for every `inline_sampler` resource and bind it at
   `index`. `air_run`'s `sampler_from_air` is a worked example, including the one mapping with no
   Gallium equivalent: Metal's `clamp_to_zero` is `CLAMP_TO_BORDER` with a zero border, not a
   border mode with a default. *(Checked: 15 cases against Apple's rules, on softpipe and on the
   Adreno.)*
3. **Size threadgroup memory at dispatch** and set the workgroup size there: neither is fixed in
   the NIR, because in Metal both are dispatch arguments.
4. **Link, then compile.** Function constants, varying names and visible-function stitching are
   only known at pipeline creation, so ir3 cannot run before it. Translation can happen earlier
   and the NIR can be cached (`nir_serialize`).
5. **Implement the stitching ABI for library functions.** It is in their own metadata —
   `air.stitching_info` names `load_argument`, `copy_from_buffer`, `store_return_value` and
   `destroy`, and each argument carries its size and alignment. This frontend's `--library`
   wrapper is a test harness with an invented convention and is **not** that ABI; what it did
   establish is how Apple passes a texture and a sampler to such a function — one-field structs
   `metal::texture2d` and `metal::sampler` wrapping the address-space-1 and -2 pointers — which is
   a fact the runtime needs.
6. **Wait for the GPU and report a timeout.** Not a Metal requirement but a lesson paid for here:
   a submission that never completes looks exactly like one that wrote nothing, and on this port
   one timeout wedges the GPU for the rest of the boot (section 5).
7. **Pack a vector function constant little-endian into `air_constant_value.bits`.** One word
   carries one constant, so a `uchar3` or `float2` has to be fields of it, and the frontend reads
   component *i* from bit *i*·width — the word a `memcpy` of the value produces on this target.
   This is a **contract this frontend defines**, not something read out of Apple's metadata: no
   harness here supplies a non-zero constant, so every component is zero whatever the packing is,
   and a runtime that packed them the other way round would get a wrong picture with nothing in
   this tree disagreeing. Two of QuartzCore's fixed-function fragment shaders declare such a
   constant (`fc_color_opcodes0/1`, `<3 x i8>`).
8. **Set `pipe_blend_state.blend_coherent` when `raster_order_groups` is non-zero.** freedreno
   already honours that bit (`fd6_emit.cc`, `build_prim_mode`), so no driver change is needed; what
   the runtime must accept is that the ordering it gets is per draw and the shader asked per
   resource, which is more synchronisation than the ask and not less. The frontend leaves a note
   saying so for every such shader.
9. **Bind every target in `fbfetch_targets`, whether or not the shader writes it.** A fragment
   shader that reads `[[color(n)]]` needs attachment `n` in the framebuffer state: freedreno
   patches a framebuffer-read descriptor for `nr_cbufs` buffers and no more (`fd6_image.cc`), so
   a target the pipeline left unbound has no descriptor to read through. `num_render_targets`
   does not imply this — `inplace_copy` reads target 0 and declares no colour output at all.

**What it does not have to do:** nothing in the frontend needs the sampler word interpreted, the
buffers filled a particular way, a particular dispatch size, or any Apple library present. The one
place the test harness pretends to know a buffer's meaning — the identity matrix for a `float4x4`
constant buffer — is a measurement convenience and is marked as one.

## 7. What this decides for the runtime (2) and the kernel ABI (3)

**Read this as intent, not as design.** The runtime and the kernel ABI do not exist; nothing below
has been built against, and none of it is a measurement. What each item *is* is the consequence
that this frontend's choices force on whoever writes those parts — so an item can be wrong in two
ways: the consequence may be mis-stated, or the frontend may have changed since. Where the
frontend already does its half, the item says so; where it does not, the item is a promise nobody
has kept yet. The one thing here that has been exercised at all is item 2's buffer binding, and
only through `air_run`, which is not a Metal runtime and binds by the same indices the frontend
reports.


1. **Compilation happens at pipeline creation, not at `newLibraryWithData:`.** Function constants
   (`newFunctionWithName:constantValues:`), vertex/fragment varying linking and visible-function
   linking are only known then. The library step can translate eagerly and cache NIR; the ir3
   step cannot run before the pipeline. (On real iOS the compile also leaves the process, for
   `MTLCompilerService`; whether this runtime does the same is a decision for (2), and NIR
   serialization — `nir_serialize` — makes either possible.)
2. **Buffers are bound per stage by index, as UBOs (constant) and SSBOs (device).**
   *(The frontend's half is done: it emits `load_ubo`/`load_ssbo` at Metal's own index and reports
   the kind per index in `air_shader_info`. `air_run` binds by those indices and the results come
   back right, which is the only thing that has tested this.)*
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
6. *(Half done: the frontend sets `origin_upper_left` on the fragment shader. `clip_halfz` is
   rasterizer state and nothing sets it — `air_run` never needed it, because its quad is already
   in clip space.)* **Clip space and the framebuffer are Metal's:** NDC depth is [0, 1] (`clip_halfz`), the
   framebuffer origin is the top-left; the frontend emits no flips, the runtime's viewport and
   rasterizer state must say this.
7. **Inline samplers** (`constexpr sampler` in the source, `!air.sampler_states`) are created by
   the runtime and bound after the API's samplers.
   *(Done on this side now, and checked: the frontend assigns the index, passes the state word
   through **and** hands over its fields decoded — `air_resource.sampler`, section 3.3 — and the
   behaviour those fields describe was verified against Apple's own rules on softpipe and on the
   Adreno. What the runtime owes is the `pipe_sampler_state` (or its own equivalent) built from
   those fields, which `air_run` does in `sampler_from_air` if an example is wanted. The one
   mapping with no Gallium equivalent is `clamp_to_zero`, which is `CLAMP_TO_BORDER` with a zero
   border. Earlier wording here said the word was not decoded, and before that said it was
   decoded when it was not; this is the third version and the first one a check stands behind.)*

8. **A visible function is linked into its caller, and the wrapper in this frontend is not that.**
   CoreImage's 135 stitchable kernels arrive with the ABI in their own metadata
   (`air.stitching_info`: `load_argument`, `copy_from_buffer`, `store_return_value`, `destroy`,
   and each argument's size and alignment), and the runtime has to implement it — Metal calls
   these at pipeline time, not the shader. The frontend's `--library` wrapper is a *test* harness:
   it invents a calling convention (argument *i* from byte 16*i of one buffer) so the bodies can
   be run and compared at all, and nothing about it is evidence that the stitching ABI works.
   Section 5 keeps those numbers in their own table for the same reason.
   *(What is done on this side: the bodies translate and compile, including the eleven that take
   a texture, a sampler and a transform, which told us how Apple passes those — one-field structs
   `metal::texture2d` and `metal::sampler`, and a bare constant-space pointer. That is a fact
   about the ABI the runtime will need.)*

9. **The harness guesses one thing, and a runtime must not.** `air_run` fills a constant buffer
   whose `air.arg_type_name` is `float4x4` with the identity matrix, because the alternative is a
   transform made of a byte pattern that sends every sample somewhere arbitrary. That is a
   measurement convenience with no counterpart in Metal: the real transform comes from the caller.
   It is listed here so that nobody reads `sampler_from_air`'s neighbour as a contract.

## 8. The first library that is not Apple's: Resident Evil 4

Everything above is measured over Apple's own shaders, because they were the only ones on the
device. The acceptance test this port is aimed at is a game — Resident Evil 4 through TrollStore —
and a game's shaders are a different population: compiled by a different toolchain, from a
different source language, for a different engine's idea of how resources reach a shader. This
section is the census of that population, taken with the same meter as section 5
(`tools/air/corpus.py`, stage by stage), so the two numbers can be put beside each other.

The three numbers first, of the **7 879 Metal library modules** in the game's package:

| stage | modules | of 7 879 |
| --- | --- | --- |
| container: found and unwrapped | 7 879 | 100% |
| reader: read by our own reader, byte for byte as `llvm-dis` reads them | 7 879 | 100% |
| **translate**: NIR produced, `nir_validate` passed | **258** | **3%** |
| **ir3**: compiled into Adreno machine code | **258** | **3%** |

| what the module is | modules | translate | ir3 |
| --- | --- | --- | --- |
| fragment | 3 741 | 106 | 106 |
| kernel | 3 011 | 130 | 130 |
| vertex | 1 127 | 22 | 22 |

Those columns are the frontend as built on 21 October at 19:44. `air2nir` and `ir3_compiler` were
copied aside with their timestamps before the run, so that a rebuild by someone else in this shared
tree could not change the compiler under the measurement — and the staleness guard in `corpus.py`
still judges the copies by their real age. Reproducing the four numbers is
`tools/air/repak.py libs` followed by `corpus.py --ir3 --list`, about a minute in all.

**No byte of this library is in this tree**, as no byte of Apple's is: the pak was read out of the
`.ipa` in a scratch directory, the containers were written there, and what came back into the
repository is this section, the numbers in it and the two tools that produced them
(`tools/air/repak.py`, `tools/air/decltry.py`). The repository may explain these shaders; it is
not a place to get them.

### 8.1 Getting them out: the pak opened, and how that reading was checked

The shaders are not in a `.metallib` file. There is none in the package: the only Metal libraries
are inside `re_chunk_000.pak` (383 413 153 bytes of the 482 MB `.ipa`), RE Engine's own archive,
and `newLibraryWithData:` is how they reach Metal. The format is Capcom's **KPKA**, version 4 —
a 16-byte header (magic, version, file count, a word) and then one 48-byte record per file:
two name-hash words, then the offset, the compressed size, the uncompressed size, an attribute
word and a checksum. 2 303 records here, and the attribute word's low nibble is the compression:
**zstd for 1 153 of them, deflate for 884, stored for 266, and encrypted for none**.
`tools/air/repak.py check` reads that table and prints the checks below.

Four things say that reading is right rather than plausible, which matters because a
mis-parsed table produces garbage that still decompresses sometimes:

- every record's extent lies inside the file, and no two overlap partially;
- the first record's data begins exactly where the table ends — 110 560 = 16 + 2 303 × 48 — and
  the last record ends exactly at the last byte of the file;
- every entry decompressed to exactly the length its record claims, with no failures;
- 179 of the 2 303 records are a second name for an extent another record already claims (one
  blob is named 51 times, and no extent is partially shared), which leaves **2 124 distinct
  extents** and is the shape a deduplicating packer produces, not the shape of a misread table.

The names are hashes and were not resolved, which costs nothing here: a Metal library is findable
by content. `tools/air/repak.py libs` looks for the MTLB magic anywhere in a decoded entry and
validates the container header the way `tools/metallib.py` reads it — the total size, and the
function table where a library always puts it — before keeping it:
**20 877 containers in 302 of the 2 124 entries**, one candidate rejected by that header check, and
12 998 of the 20 877 are byte-for-byte duplicates of another, leaving **7 879 distinct libraries**.
The whole pass is one forward read of the zip member: the table is at the head, so the entries can
be walked in offset order and nothing bigger than one entry is ever held.

Each of the 7 879 containers holds **exactly one function** — RE Engine ships a library per shader
variant rather than one library per program — so libraries and modules are the same count here,
against 671 modules in 6 files on the system side. The container's
`NAME` tag is kept: 1 707 distinct function names, led by `VfxPixelShader` (631 variants),
`VfxVertexShader` (498) and `InjectShadedVolumeDataCS` (378).

Two facts about the toolchain, from the modules' own metadata:

- They are **AIR 2.3.0, Metal language 2.3**, where every module of the system corpus is AIR 2.5.0
  / Metal 3.0. The reader was written against 2.5.0 and read all 7 879 of these without a change.
- `air.compile.denorms_disable` and `air.compile.framebuffer_fetch_enable` on every one of a
  394-module sample; `air.compile.fast_math_enable` on 384 of those and
  `air.compile.fast_math_disable` on 10. What is said above about Apple's shaders — that they
  promise finite inputs, so arbitrary bytes are outside their domain, and that fast math lets the
  hardware contract a multiply-add — holds for these too, and those ten withdraw the second
  permission.

Nothing else in the package carries a Metal library: the 295 MB executable and all 26 remaining
files were scanned the same way and hold **0** valid containers (the string `MTLB` occurs in the
executable; no container header follows it). The `.ipa` also lists on-demand resources
(`OnDemandResources.plist`) that are not in it, so this census is bounded by the package, the way
section 5's 456 is bounded by the `.metallib` files in `rootfs`.

### 8.2 One wall, and it is not an operation

The whole difference between 7 879 and 258 is a single interface word:

| modules | where it stops |
| --- | --- |
| 7 620 | `interface: input air.indirect_buffer` |
| 1 | `intrinsic: air.discard_fragment` |

That is the entire refusal histogram — two rows for 7 621 modules. And the 7 620 are exactly the
7 620 modules that declare an argument buffer: every module that has one is refused, every module
refused has one, and the 258 that translate are the ones that do not.

An `air.indirect_buffer` is Metal's argument buffer, and these shaders have one because they were
not written in Metal. The interface says so by name:

```
!19 = !{i32 2, !"air.indirect_buffer", !"air.buffer_size", i32 24, !"air.location_index", i32 0,
        i32 1, !"air.read", !"air.struct_type_info", !20, !"air.arg_type_size", i32 24,
        !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"spvDescriptorSetBuffer0",
        !"air.arg_name", !"spvDescriptorSet0"}
```

`spvDescriptorSet0` is SPIRV-Cross's name for a Vulkan descriptor set packed into one Metal
argument buffer. These are HLSL shaders compiled to SPIR-V and translated to MSL, and that is why
this wall is invisible from Apple's side: **0 of the 671 system modules carry an
`air.indirect_buffer`**, and 7 620 of 7 879 of these do. A frontend measured only against the
system's libraries would have no reason to suspect that one missing interface word is 97% of a
game.

The shape of the work, counted over all 7 620: 7 382 declare one descriptor set and 238 declare
two; the structs hold **30 947 textures, 23 488 constant-space buffer pointers, 18 477
device-space buffer pointers, 9 101 samplers** and 29 depth textures, with up to 43 members in a
single struct. The metadata describes each member the way a direct argument is described — the
nested `air.indirect_argument` node is an ordinary `air.buffer` / `air.texture` / `air.sampler`
descriptor with its own `air.location_index` — so the binding information the frontend needs is
all there and in a vocabulary it already reads. What is new is how the shader reaches it:

```
%struct.spvDescriptorSetBuffer0 = type { ptr addrspace(2), ptr addrspace(2),
                                         %"struct.metal::sampler", %"struct.metal::texture2d",
                                         %"struct.metal::texture2d" }
  %6  = getelementptr inbounds %struct.spvDescriptorSetBuffer0, ptr addrspace(2) %0, i64 0, i32 0
  %7  = load ptr addrspace(2), ptr addrspace(2) %6
  %8  = getelementptr inbounds %struct.type_BloomSettings, ptr addrspace(2) %7, i64 0, i32 0
  %38 = getelementptr inbounds %struct.spvDescriptorSetBuffer0, ptr addrspace(2) %0, i64 0, i32 2, i32 0
```

A pointer in this frontend is *(which resource, which byte offset)*, resolved while translating
(section 3), which is what lets it emit `load_ubo` at Metal's own index with no descriptor
indirection. An argument buffer keeps that resolvable and moves it one step: a buffer member is a
pointer **loaded** out of the struct, and a texture or sampler member is the same one-field
`metal::texture2d` / `metal::sampler` wrapper the CoreImage stitched functions arrive with
(section 7.8), here as a struct member reached by a `getelementptr`. So the work is to follow a
GEP-and-load chain to the binding the metadata names, not to invent a new kind of value — and it
is the same mechanism for all three resource kinds.

#### What that wall is not: the descriptors are decidable at translation time

This frontend has two pointer walls, and one of them is named after this very feature. A pointer
**stored** into memory is refused because a pointer here is a resolution and has no bytes
(`air_to_nir.c`: "a store of a pointer into memory, which this frontend has no address to write").
A pointer **loaded** out of memory is refused by `air_load_ptr` with the parenthesis
`(argument buffers)` already in its message. So the first question is not how to write the code:
it is whether an argument buffer needs the *general* form of the second wall — every pointer a
64-bit address, which is a different frontend — or whether its particular form can be resolved
while translating, the way every other pointer here is. **It can, and both halves of that are
measured rather than argued.**

**The metadata names the binding of every member.** The argument buffer's own node carries an
`air.struct_type_info` whose members each end in a nested descriptor:

```
!30 = !{i32 13, !"air.indirect_buffer", !"air.buffer_size", i32 80, !"air.location_index", i32 0,
        i32 1, !"air.read", !"air.struct_type_info", !31, …,
        !"air.arg_type_name", !"spvDescriptorSetBuffer0", !"air.arg_name", !"spvDescriptorSet0"}
!31 = !{i32 0,  i32 8, i32 0, !"type_Polygon3DConstant", !"Polygon3DConstant",
        !"air.indirect_argument", !32,
        i32 16, i32 8, i32 0, !"sampler", !"PointClamp", !"air.indirect_argument", !36,
        i32 40, i32 8, i32 0, !"texture2d<float, sample>", !"primTex",
        !"air.indirect_argument", !39, … }
!36 = !{i32 2, !"air.sampler", !"air.location_index", i32 2, i32 1,
        !"air.arg_type_name", !"sampler", !"air.arg_name", !"PointClamp"}
!39 = !{i32 5, !"air.texture", !"air.location_index", i32 5, i32 1, !"air.sample",
        !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"primTex"}
```

Each member is five fixed fields — byte offset, size, a flag, the type name, the member name —
and then the key and the node. **The node is an ordinary resource descriptor**: `!36` and `!39`
differ from a direct `air.sampler` or `air.texture` argument in exactly one operand, the leading
integer, which is the member's index in the set where a direct argument's is its parameter number.
Everything the frontend needs to bind it — the kind, `air.location_index`, the access, the type
name — is in the vocabulary `translate_input` has read since the beginning.

Two things about those members are measured over all of them and neither is guessable:

- **not one carries `air.address_space`.** Over the 7 620 modules' 7 858 argument buffers and
  their members, zero. For a direct `air.buffer` argument that key is how the frontend tells a
  constant buffer from a device buffer; here it is absent, and the only thing that says which is
  **the LLVM type of the pointer itself** — `load ptr addrspace(2)` against
  `load ptr addrspace(1)`, which is on the load instruction, and equivalently the struct member's
  own type, which the `getelementptr`'s source element type gives. A frontend that reused the
  direct-argument path unchanged would read address space 0 and refuse, which is the better of the
  two ways to be wrong.
- **every member's location span is 1.** No member is an array of descriptors, so no module in
  this library can index one at run time, which is the case that would need real indirection.

**And the body reaches every member by a chain of constant indices.** There are three shapes and
SPIRV-Cross emits all three:

```
; a buffer member: the member, then the pointer in it
%6  = getelementptr inbounds %struct.spvDescriptorSetBuffer0, ptr addrspace(2) %4, i64 0, i32 0
%7  = load ptr addrspace(2), ptr addrspace(2) %6
; a sampler member: the member and the wrapper's one field in one getelementptr
%332 = getelementptr inbounds %struct.spvDescriptorSetBuffer0, ptr addrspace(2) %13, i64 0, i32 3, i32 0
%333 = load ptr addrspace(2), ptr addrspace(2) %332
; a texture member: the member, then the wrapper, then the handle
%330 = getelementptr inbounds %struct.spvDescriptorSetBuffer0, ptr addrspace(2) %13, i64 0, i32 7
%334 = getelementptr inbounds %"struct.metal::texture2d", ptr addrspace(2) %330, i64 0, i32 0
%335 = load ptr addrspace(1), ptr addrspace(2) %334
```

The wrapper is the same one-field `metal::texture2d` / `metal::sampler` the CoreImage stitched
functions arrive with (section 7.8) and which the library wrapper already resolves to a binding —
here as a struct member instead of a parameter, and with the handle **loaded** rather than passed.

So the whole of it is: follow a constant byte offset into the set, look the offset up in the
member table, and emit the binding the member's descriptor names. No address, no descriptor
indirection, and no change to the `32bit_index_offset` model the rest of the frontend is built on.

**How much of the library that reaches.** Every use of the argument buffer's own pointer in every
one of the 7 620 modules, classified — a use is decidable when it is a `getelementptr` with
constant indices, a no-op pointer bitcast, or the load at the end of such a chain:

| modules | what the shader does with the set's pointer |
| --- | --- |
| **7 619** | nothing but constant chains: **142 733** resource handles loaded out of a set, through 63 736 two-index and 57 728 three-index chain steps, 3 171 no-op bitcasts, and **207** `phi`s of two pointers that are two copies of **one** `getelementptr` — LLVM having duplicated it on two paths, so both sides name the same member |
| **1** | the indirect-command-buffer kernel: a bitcast of the set's pointer to a generic one, a descriptor index that is not a constant, `air.command_buffer` and `air.render_pipeline_state` members and a nested `air.indirect_buffer` |

and over the 82 059 members of those 7 858 argument buffers: 41 971 buffers, 30 976 textures,
9 101 samplers, **0 with `air.address_space`**, **0 with a location span other than 1**, and
exactly **11 members that are none of the three kinds — all eleven in that one kernel**, which
section 8.3 already names for its intrinsics. The other pointer walls are not in the way either,
counted over all 7 620: **0** store a pointer into memory, **0** convert a pointer to or from an
integer, **0** `alloca` a local to hold one, and the 58 that `select` between pointers select
between two offsets of one **threadgroup** array — `ptr addrspace(3)` into a module-scope
`[64 x half]` — which is the case `AIR_VAL_PTR_SELECT` already carries, both sides being the same
block and both legal to read.

So the answer to "is this the pointer wall" is **no — for 7 619 of 7 620 modules**, and the one
left is the one module in this library that encodes draws on the GPU. What makes it so is not that
these shaders are simple: it is that an argument buffer is *declared*, member by member, in
metadata this frontend already reads, and a declared descriptor needs no address.

**What the general fix would have cost**, for the record, since this is the second time it has been
named as the thing standing in the way. A pointer here is *(which resource, which byte offset)*
resolved while translating; the general form is a pointer that is a **number** — Mesa's
`nir_address_format_64bit_global`, `load_global`/`store_global` in place of `load_ubo` at Metal's
own index, every buffer bound by GPU address instead of by binding, and bindless descriptors for
textures. Both halves exist in this tree already, which is the useful part of the estimate:
turnip asks for `nir_address_format_64bit_global` (`tu_shader.cc`) and ir3 has the bindless path
(`ir3_a6xx.c`, `ir3_nir_lower_io_offsets.c`). That is not a patch to this frontend, it is a second
pointer model beside it, and what needs it is: a pointer **stored** into memory (2 modules of
QuartzCore's library), a descriptor indexed at run time (1 module of 7 620 here), and an array of
descriptors (**0** modules of either corpus). Four modules of 8 335. The resolution path above is
the cheaper half by a wide margin, and it is not a special case for SPIRV-Cross output: it reads
Apple's own declaration of the argument buffer, which any MSL `[[id(n)]]` struct has.

##### The two halves, if it is built

*The frontend's half*, which is the one this section is a design for rather than a measurement of:

1. `translate_input` grows a branch for `air.indirect_buffer`: read `air.location_index` (the
   Metal buffer index the set is bound at), walk `air.struct_type_info` into a member table keyed
   by **byte offset** — not by member index — holding each member's kind, its own
   `air.location_index`, its access and its type name. The parameter becomes an `air_ptr` of a new
   class whose `.binding` is the set. The address space is not in that table and must not be
   guessed into it: it is read off the pointer the shader loads (`air_type.addrspace` of the load's
   own result type), because the metadata has none and the parameter's type is opaque.
2. `AIR_OP_LOAD` resolves before it loads: a load through an argument-buffer pointer whose byte
   offset is a constant looks the offset up in that table and becomes an `air_val_ptr` with the
   member's class and binding, instead of an instruction. Keying on the byte offset rather than
   the member index is what makes the three shapes above one case: a `getelementptr` into a
   `metal::texture2d` wrapper adds zero bytes, so the texture shape resolves by the same lookup as
   the buffer shape with nothing special about it.
3. The indices have to be re-allocated, and that is a decision rather than a transcription: a
   member's own `air.location_index` is per set, so set 0's member 0 and set 1's member 0 both say
   0, and 238 modules have two sets. The frontend allocates one flat index per kind in
   (set, member) order and records, on each `struct air_resource`, both the index it chose and the
   Metal `(set, index)` it came from — two new fields — because the runtime has to bind by the
   first and the application names by the second.
4. Refused by name, each because something would otherwise be drawn over the wrong resource: a
   member kind that is not buffer, texture or sampler; a member whose location span is not 1; a
   nested `air.indirect_buffer`; a load through the set at an offset that is not constant; and a
   `phi` or `select` between pointers naming **different** members — which could instead go
   through the `AIR_VAL_PTR_SELECT` machinery that already loads both sides and selects, but no
   module in reach needs it and an untested path is worse than a refusal.

*The runtime's half*, which belongs to whoever holds `air_metal.h` and the driver. The shape of it
follows from the frontend having resolved every descriptor: **there is no argument buffer to
write.** Metal's `MTLArgumentEncoder` writes 8-byte handles into a plain buffer (8 bytes per
member is what the metadata says: a two-member set is `air.arg_type_size 16`, a ten-member set
80), and those handles mean nothing to a Gallium driver that binds by index. So the encoder cannot
be a writer; it has to be a **recorder**:

- `-[MTLDevice newArgumentEncoderWithArguments:]` and
  `-[MTLFunction newArgumentEncoderWithBufferIndex:]` return an object that answers
  `encodedLength` and `alignment` from the frontend's member table — the application allocates
  its buffer from those numbers and must get Apple's own answers.
- `setArgumentBuffer:offset:` names a destination; `setBuffer:offset:atIndex:`,
  `setTexture:atIndex:`, `setSamplerState:atIndex:` and `constantDataAtIndex:` then **record**
  (resource, offset) against (destination buffer, destination offset, index) instead of writing
  bytes. The records live with the destination buffer, because Metal lets one buffer hold several
  encodings at different offsets and lets them be reused across many draws.
- At draw or dispatch, for each argument buffer the pipeline's shaders declare, the driver finds
  the record for whatever buffer is bound at that Metal index and binds every member to the
  Gallium slot the frontend assigned it. The buffer's *contents* are never read.
- And therefore: **an argument buffer the application did not fill through an encoder must be
  refused by name.** One written by the CPU directly, written by a compute kernel, or produced by
  a blit has handles in it that this driver cannot interpret, and nothing distinguishes it from a
  correctly encoded one except the presence of a record. No record for (buffer, offset) at draw
  time is a refusal, not a guess. `useResource:`/`useHeap:` are then no-ops here — every resource
  is bound explicitly — and saying so out loud is better than implementing nothing in silence.

##### 8.2.1 Built: what the frontend's half cost and what it measures

The frontend's half of the four steps above is implemented (`air_to_nir.c`), and the two corpus
numbers are the whole claim:

| corpus | read | translate | through ir3 |
| --- | --- | --- | --- |
| iOS 16.5's system libraries, 456 modules | 456 | **321** | **321** |
| Resident Evil 4, 7 879 modules | 7 879 | **2 990** | **2 978** |

The system corpus did not move, and "did not move" is stated module for module rather than as a
count: the same 456 read, the same 321 translate, the same 321 compile, and **not one module's NIR
instruction count or Adreno instruction count changed** against the committed
`build/air-report.json`. That is the stronger statement, and it is available because the report
carries both numbers per module. Resident Evil 4 went from 258 to 2 990, which is the measurement
this section was a design for: **the interface word that stopped 7 620 modules stops none**, and
`interface: input air.indirect_buffer` does not occur in its refusal histogram at all.

Those 2 990 are this change and nothing else, measured with a translator built from this work
alone, and the number in the tree is already higher: with the intrinsic work going on beside it the
same corpus reads **4 447 translate, 4 435 through ir3**, and the 1 457 extra are gradient
sampling, `air.discard_fragment`, array textures, the SIMD-group operations, `llvm.fmuladd` and
cube sampling — every one of them a module that used to stop at an `intrinsic:` row, and not one
module lost. Which half bought which is worth keeping separable, because the interface and the
vocabulary are independent walls and the same shader can be behind both.

**What the 4 169 turned out to be worth.** Section 8.3 counts 4 169 of the 7 620 as calling no
`air.*` this frontend refuses, and says in as many words that such a module "can still stop at
another interface word, an instruction, or in ir3, and which of those it does cannot be known
until the interface exists." It exists, so here is the answer: of the 7 620 modules that declare an
argument buffer, **2 732 translate and 2 720 reach ir3**, and all 2 732 are inside the 4 169 —
nothing outside it translates, which is the operation census checking itself. The 1 437 of the
4 169 that still stop, by what stops them:

| modules | what stops it, and why the operation census could not see it |
| --- | --- |
| 870 | an **instruction** this frontend does not handle — 858 of them `a bitcast that changes the shape of its value`, and 12 `insertelement` at a run-time index |
| 331 | an integer of a width NIR does not have — a **type**, 296 of them `i2` |
| 88 | `llvm.fmuladd` and `llvm.fabs` — **`llvm.*`**, and `decltry.py` probes `air.*` |
| 79 | an **aggregate output**: `gl_ClipDistance` arriving as an array (see below) |
| 64 | a **`phi` of pointers** this frontend refuses (see below) |
| 5 | an interface key: `air.thread_execution_width` |

So "4 169 would translate the moment this exists" was optimistic by 1 437, and the reason is
structural rather than an error in the count: a census of the *operation vocabulary* is blind to
instructions, to types, to output declarations and to `llvm.*`, which is four of those six rows.
The biggest single thing left in this library is now `a bitcast that changes the shape of its
value`, which 1 224 of the 7 879 hit and which has nothing to do with argument buffers.

**Where this departs from the design above, with the evidence.** Step 1 says the address space
"is not in that table and must not be guessed into it". It is not guessed, and it *is* in the
table: `air_argbuf_member.addrspace` comes from the member's type in **the module's own struct
type**, which `air.arg_type_name` names (`%struct.spvDescriptorSetBuffer0`) — a second record of
the same layout, not an inference from the first. The reason it has to be there is the resource
list rather than the resolution: `air_shader_info.resources` needs a *kind* per member at
interface time, because the runtime's `encodedLength` is over every member a set declares whether
this shader reads it or not, and a numbering that skipped the unread ones would differ between two
shaders sharing one set. The design's rule survives where it matters — the authority is still the
pointer the shader loads — and the two are **checked against each other at every load**, with a
refusal by name when they disagree. The evidence that this is a reading and not a guess, over
`argbufcheck.py --census` on all 7 879 modules:

- **7 857 of 7 857** argument buffers have their struct type declared in their own module, and
  **7 857 of 7 857** have as many members in it as `air.struct_type_info` declares. Zero missing,
  zero disagreeing.
- the struct type's member types are **23 488 `ptr addrspace(2)` plus 18 477 `ptr addrspace(1)`,
  which is 41 965 — exactly the number of `air.buffer` members**, and 30 976 `metal::texture*` /
  `metal::depth*` wrappers and 9 101 `metal::sampler` wrappers, exactly the texture and sampler
  counts. Two independent descriptions of the same 82 048 members, agreeing kind for kind.
- **0 members carry `air.address_space`**, and **0 span a number of descriptor indices other than
  1**. Both are printed as zeros by the census rather than left absent, because a counter that is
  merely missing says nothing.

**And it says 7 857 where section 8.2 above says 7 858**, which is one module and worth settling
rather than leaving as two numbers in one document. The committed census counts the argument
buffers an entry point *declares as parameters*; the throwaway classifier above counted
`air.indirect_buffer` nodes, and the indirect-command-buffer kernel has **two** — its own set, and
a nested one inside a member's descriptor. So 7 857 sets are bound by 7 620 modules, of which
**237** bind two and 7 383 bind one (8.2's "238 declare two" counts that kernel's nested node as
its second set), and the 7 858th `air.indirect_buffer` is the member that kernel is refused for.
The member totals reconcile the same way, and exactly: that nested set has **11** members,
and 82 048 + 11 is 8.2's 82 059. How those 11 divide by kind is where the two readings still
differ, and it is not resolved here — the kernel is refused at its first member either way, so
nothing downstream depends on which of the two splits is right.

The other three steps are as designed. The member table is keyed on the byte offset and the three
`getelementptr` shapes resolve through one lookup; the flat per-kind index is allocated in
(set, member) order with the Metal `(set, id)` kept beside it on `struct air_resource`
(`argbuf_set`, `argbuf_id`, `nir_air.h`) — and the counters start **above the highest index any
direct argument of that kind took**, which the design did not say and which nothing in reach
exercises: none of these 7 620 modules has a direct resource at all, so the collision it prevents
is a hypothetical, and it is cheap.

**The refusals, and which of them ever fired.** The design names five. The implementation names
**23** around argument buffers, and the honest headline is that **exactly one of the 23 fires on
either corpus**. Counted over both — the system's 456 contribute 0 to every row, because none of
them has an argument buffer:

| modules | refused by name |
| --- | --- |
| 1 | a member that is **not a buffer, a texture or a sampler**: the `air.command_buffer` member of the one kernel in this library that encodes draws on the GPU |
| 0 | the design's other four: a member spanning more than one descriptor index; a **nested** argument buffer; a load through the set at an offset that is **not constant**; a `select` between two different places in a set |
| 0 | and the eighteen the implementation adds, of which the ones worth naming are: a set whose struct type the module does not declare; a member the struct type puts somewhere else or sizes differently; a pointer loaded in an address space the struct type contradicts; a **value** read out of the set instead of a descriptor (Metal's `constantDataAtIndex:`); two sets at one Metal buffer index; a set whose declared size is not eight bytes per member; a **store** into a set |

That the other 22 never fire is not a reason to drop them, and the one module that does fire says
why: it is refused at its first `air.command_buffer` member, so it never reaches the run-time
descriptor index or the nested set that section 8.2 also lists for it. Each unexercised refusal
stands where the alternative is binding a descriptor that is not there — and three of them are
cross-checks between two records of one layout, which only ever fire if the two disagree.

**What argument buffers did make reachable is a different refusal, and it is the one with modules
behind it:** a `phi` of pointers, 88 of them.

| modules | refused by name |
| --- | --- |
| 43 | a `phi` of pointers whose operand comes in over a **loop edge**, so which resource it is depends on the iteration |
| 41 | a `phi` between pointers that name **different places** |
| 4 | a `phi` of pointers one of whose operands is itself a pointer chosen at run time |

**And one prediction above was wrong, in a way worth being precise about.** The design says a
`phi` between pointers naming different members needs no handling because "no module in reach
needs it": 88 modules need it. That is not a contradiction of the census, which was a
classification of **uses of the set's own pointer** and is still right — 207 such phis, every one
two copies of one `getelementptr`. These 88 are phis of the handles **loaded out of** the set,
which that classification never looked at, so the population was missed rather than miscounted.
What they would need is the `AIR_VAL_PTR_SELECT` machinery the design names and declines: a
`select` between two loaded buffer members already goes through it today (it loads both sides and
selects, which is sound because both members are bound), and a `phi` of the same shape is refused.
That asymmetry is deliberate for now — a phi has any number of operands and the loop-edge case
cannot be answered by loading both sides at all — and it is the next thing to buy here, worth
about 88 modules.

**Two crashes this work had to stop being crashes.** Both are older than argument buffers and were
reachable only once the bodies behind the wall could be translated; an abort is not a refusal,
since it says nothing a report can count and takes the run with it.

- **A 2-, 4- or 3-bit integer.** NIR has 1, 8, 16, 32 and 64; an HLSL bit field reaches AIR as
  `i2` or `i4` through SPIRV-Cross (6 035 `i2` and 809 `i4` declarations in a 200-module sample),
  and every NIR builder handed one asserts — "Invalid Boolean size", "Invalid bit size", "Invalid
  nir alu bit size", **495 modules** between them. `air_nir_bit_size` now names the width and the
  shader is discarded; **514** of the 7 879 are refused this way, and the widths are `i2` (438),
  `i3` (40), `i4` (12), `i31` (10), `i5` (10), `i6` (2) and `i9` (2) — every one of them a bit
  field, which is the honest form of the same fact.
- **A constant `getelementptr` into a threadgroup array.** A cached constant is built in the const
  region at the top of the entry block; a module-scope variable's byte offset is built where
  `materialise_globals` runs, which is below that region. So `load i32, ptr addrspace(3)
  getelementptr([16 x i32], ptr @tbl, i64 0, i64 9)` — how SPIRV-Cross reads a fixed slot of one —
  became `iadd %globaloffset, 36` sitting **above the def of `%globaloffset`**, which
  `nir_validate` rejects ("src->_parent & SRC_TAG_SEEN"). **115 modules.** `air_ptr_add` now folds
  two constant offsets into one, which removes the reference entirely, and it changed no existing
  module: all 456 of the system corpus produce byte-identical NIR and Adreno instruction counts,
  so no module of that corpus had ever added two constant offsets. **The ordering hazard itself is
  still there** and is written down rather than fixed: a constant whose value refers to anything
  built below the const cursor is a use above its def, and only the fact that a global's offset is
  always an immediate makes the folding sufficient. The fix would be to materialise the globals
  *into* the const region and move the cursor past them, which is three lines in the entry-point
  path and a fourth, less obvious one in the callee path, where the builder's cursor and the const
  cursor start at the same point.

**And one silent wrong answer the same reachability uncovered**, which is not a crash and is worse
for it. `air_nir_bit_size` has no answer for an aggregate and used to return its zero unexamined,
so an output declared `float` in Apple's reflection string and arriving as `[N x float]` in the
LLVM type — a **clip-distance array**, which 86 of these modules declare — became a
*one-component* float output at `VARYING_SLOT_CLIP_DIST0`. Three of four clip planes dropped, the
shader compiled, and nothing in any log said so. It is refused by name now
(`translate_output`); NIR's own form for it is an output of several slots, which this frontend does
not build, so this is a gap with 86 modules behind it and not a guard on a shape nobody uses.

The same zero had a second victim with no modules behind it, and it is worth a line because it
moved a refusal `loweringcheck.py` pins: a `phi` of a STRUCT used to be given a NIR register of
zero bits and failed further down at "a returned struct that was never built" — true of the
module, and not what is wrong with it. It is refused at the phi now, where a struct is a tree of
values and never a NIR value, so nothing carries one across an edge.

**And a third thing that was never a crash and never a refusal: every string in
`air_shader_info` was a pointer into freed memory.** `air_md_string` returns a NUL-terminated
copy allocated as a child of the metadata node it came from, so an `air.arg_name` or an
`air.arg_type_name` lives under the module, which lives under the scratch context `air_to_nir`
frees before it returns. The arrays were copied out to `mem_ctx`; the strings in them were not,
and `air_shader_info.vertex_attributes` was not copied at all. Every caller has been reading freed
memory since those fields existed — `air2nir --interface`, `air_run` (which decides a constant
buffer holds the identity matrix by comparing `type_name` against `"float4x4"`), and
`air_metal.c`'s reflection.

It mostly got away with it, which is why it survived: freed ralloc memory usually still holds its
bytes. What it looks like when it does not is `--interface` printing `name=on=1` for an attribute
called `texcoord1` — and that is how it was found, because the garbled line carried a stray tab
and `vattrcheck.py` therefore counted **four** attributes in a three-attribute shader and failed.
Nothing about argument buffers caused it; what argument buffers did was change what the heap looks
like at that moment. It is fixed by copying the strings and the attribute array out with
everything else, and the lesson is the one this project keeps relearning: the check that caught it
was the one that re-reads Apple's metadata a second way and compares, not any of the ones that
count modules.

**The check stage: `tools/air/argbufcheck.py`**, which the paragraph this replaces asked for. It is
in `check.sh`, it needs no corpus, and it is three references rather than one:

- the modules are **written by the script**, so what the set declares is known rather than read:
  the interface `air2nir --interface` reports is compared member for member against the table the
  module was generated from — kind, the flat index the frontend allocated, and the Metal
  `(set, id)` it came from. That is the only check that looks at the **allocation**, which is a
  decision and not a transcription.
- the kernel is **dispatched on softpipe with different bytes in every slot** (`--buffer N@off=`).
  That is the point of it: every other buffer check in `check.sh` fills all buffers with one
  pattern, so a shader that read the wrong slot would return the right answer — the mistake this
  project has made three times. Slot 1 holds `256+i`, slot 2 holds `512+i`, the kernel puts one in
  each half of its result, and a swap, an unbound slot and an off-by-one are all visible.
- **LLVM runs the same module on this processor**: the air64 triple and datalayout dropped, the
  address spaces flattened, and a harness appended that builds the set out of real pointers. It
  agrees with softpipe on all sixteen words. Two references sharing no code — which is what this
  project means by a second reference.

It also carries the nine refusals, each checked to be refused with the message the design names
rather than with a symptom further down, and `--census` re-derives every metadata number in this
section from a parse of `air2nir --ll` that does not go through the frontend.

Two things it does **not** show, said here rather than left to be assumed. A **texture member's
index cannot be checked by running it**: `air_run` puts `1000 * slot` in each texel precisely so a
misbinding shows, and for a compute kernel on softpipe it does not — both a sample and a texel
fetch come back with the view bound at slot 0. The script demonstrates that with two **direct**
texture arguments, where no argument buffer is involved, and then checks the texture member on the
NIR instruction (`txf ... 1 (texture)`) instead of on the pixel. And a **sampler member's index**
is not checked at all, only that it resolves and samples: `air_run` binds one sampler state,
nearest and clamp, to every slot, so a sampler read from the wrong slot gives the right texel.
**Both are closed from the API side in 8.2.2**, by drawing a fragment shader through the runtime's
own encoder with two different sampler states and two different textures in one set.

**What the runtime now has to do**, which is the contract this half implies and is implemented in
8.2.2 below. `struct air_resource` gained `argbuf_set` and
`argbuf_id`: `~0u` for a direct argument, and otherwise the Metal `[[buffer(n)]]` the SET is bound
at and the member's `[[id(n)]]` inside it, with `.index` being this frontend's own number and
**not** Metal's. So the recorder's job is exactly: for each argument buffer the pipeline's shaders
declare, find the record for whatever buffer is bound at `argbuf_set`, and bind the resource
recorded at `argbuf_id` to the Gallium slot `.index` in the space `.kind` names. The member list a
set declares is complete — every member gets a resource, used or not — so `encodedLength` is
`8 × (the members with that argbuf_set)` and needs nothing the frontend did not already report.

**And where the structure grew, because that decides what a stale driver does.** Measured rather
than asserted, by compiling the two headers side by side: `struct air_resource` goes from **112 to
120 bytes**, and the two fields are **at the end** — `raster_order_group` stays at offset 104,
`argbuf_set` is at 108 and `argbuf_id` at 112. No existing field moved, and
`struct air_shader_info` (104) and `struct air_vertex_attribute` (56) are untouched. So a driver
compiled against the old header reads every field it knows at the right offset and then **strides
`resources[]` by 112 where the library writes 120**: resource 0 is right and every one after it is
read from eight bytes before itself, which is a texture index landing where a size was. One
resource is the common case and would look perfectly well, which is the dangerous part. Both
sides have to be built from one header, which is what the staleness stamp on `tools/mesa/air/`
exists to enforce.

##### 8.2.2 Built: the runtime's half, where an encoder records and nothing is written

The design above says the runtime "cannot be a writer; it has to be a **recorder**". It is one
now, in the library (`air_metal.c`, `air_metal.h`) and in the driver (`redwoodmetal.m`), and the
measurement is differential: **the same pixel comes out of an argument buffer as out of direct
arguments**, and every record deliberately made wrong prints a different picture that is named in
advance. `tools/air/argbufruncheck.sh` is that run — **20 checks, 0 failed**, on this machine,
through the contract and nothing else: three about what a module declares and what the two
pipelines report, three about the differential pixel and the arithmetic behind it, two records
exchanged on purpose, two offsets on a member and three on a direct argument, five refusals, and
two about the records themselves -- that four encodings live in one buffer at four offsets, and
that destroying one member's texture leaves the other three encodings drawing what they drew.

**What the encoder answers, and where the two numbers come from.** `encodedLength` is
`8 × the members the set declares` and `alignment` is 8, read out of the module by
`rwa_module_argbufs` — which is `rwa_module_textures`' shape for `rwa_module_textures`' reason: a
driver answering `-[MTLFunction newArgumentEncoderWithBufferIndex:]` has a function's bitcode and
no pipeline and no descriptor to read a layout off. Those numbers have to be **Apple's**, because
the application allocates its buffer from them and then encodes at multiples of them, and the
census says they are. `tools/air/argbuflen.py` parses the metadata a second time, in Python,
without going through the frontend, over all 7 879 modules of Resident Evil 4
(`repak.py libs` first, then `argbuflen.py --list`):

| sets | what the metadata declares |
| --- | --- |
| **7 858** | `air.arg_type_align_size` is **8** |
| **7 856** | `air.arg_type_size` is **8 × members** — and `air.buffer_size` on the same node says it again |
| **7 856** | the members' ids are **0..n-1**, with no gap and nothing twice |
| 2 | neither: 6 members declaring 20 496 bytes with ids `0,1,2,3,4,2052`, and 11 members declaring 192 bytes with ids `0..8,24,25` |

and **both of those two are in one module**, `CS_BuildICB` — the single kernel of this library
that builds indirect command buffers, which the frontend refuses at its first `air.command_buffer`
member before any encoder could be made for it.

That is the metadata read on its own. The other half is the library's answer against it, on real
game shaders rather than on the written ones: `air_metal_test --argbufs` over the first **300** of
Resident Evil 4's modules, against the same Python parse, set by set — **125 modules translate and
declare a set, and on every set of every one of them the library's `encodedLength`, `alignment` and
member count are the metadata's `air.arg_type_size`, 8 and member count. 0 disagree.** (The other
175 are 170 the frontend refuses for something else entirely and 5 with no set.) One of those 125,
to show what the call answers: `LayoutRibbonCollectorCS`, a set at `[[buffer(0)]]` of
`encodedLength 40`, five `device` buffer members named `EmitterInstances`, `ParticleStructures`,
`RibbonPoints`, `ItemBuffer` and `LayoutRibbonResult`, four of them written, at flat indices 0 to
4. So `8 × members` is right for every set in reach,
and for the two it is not right for the refusal that comes first is the frontend's. The id density
is checked rather than assumed, and a set whose ids have a gap is **refused by name**: Metal's own
`encodedLength` is over the highest id, so a member count would be a length the application
allocates too little memory from.

**The sixteen selectors.** `appasks.py` counted 16 `MTLArgumentEncoder` selectors Resident Evil 4
sends that this driver did not answer. Every method the protocol requires is answered now — the
four properties, the three that record, their `withRange:` forms, and the sixteen — and of those
sixteen, **four do something and twelve decline by name**:

- served: `setArgumentBuffer:offset:` (which names the destination and is where an encoding
  begins), `setArgumentBuffer:startOffset:arrayElement:` (the same with Metal's own arithmetic,
  `encodedLength` per element), `setLabel:`, and `constantDataAtIndex:`.
- declined, each because the frontend refuses the matching shader when it translates it, so a
  record could never be asked for: `newArgumentEncoderForBufferAtIndex:` (a set inside a set),
  and the eleven that set a render or compute pipeline state, an indirect command buffer, an
  acceleration structure, a visible function table or an intersection function table into a
  member.

`constantDataAtIndex:` is worth its own line because the honest answer is not NULL. It hands back
the argument buffer's own bytes at that member's place, as Metal does — the application's write
then lands in the application's buffer and nothing is corrupted — and says once that **nothing
here reads those bytes**: a shader that reads a *value* out of a set rather than a descriptor is
refused when it is translated (8.2.1), so there is no shader of ours those bytes could reach.
Answering NULL would be a crash in the application's `memcpy`.

**`-[MTLFunction newArgumentEncoderWithBufferIndex:]` is Metal's own method, and Metal's own
implementation cannot work here.** That was read in the cache before anything was written:
`newArgumentEncoderWithBufferIndex:` (0x181aef7f4) forwards to `…reflection:binaryArchives:`
(0x181aef85c), which asks `-[_MTLFunction reflectionWithOptions:binaryArchives:]` (0x181aef458)
for the function's reflection; that one calls `-compiler`, which on this port is
`-[_MTLDevice compiler]` (0x181aac128), a two-instruction stub answering nil. The builder
(0x181aef8a4) then walks `[reflection arguments]`, finds a count of zero and takes its failure
path (0x181aef924):

```
MTLReportFailure(0, "-[_MTLFunction newArgumentEncoderWithBufferIndex:reflection:"
                 "functionReflection:]", 0x22fc,
                 @"bufferIndex %lu does not identify an argument buffer")
```

and returns **nil**. So the choice is not between Apple's implementation and ours; it is between
nil and an encoder — and the information Metal was missing is the information this port has:
`-[_MTLFunction bitcodeData]` works (docs/metal.md 10.12), and the set's layout is in those
bytes. The driver therefore **replaces that method at run time**, which is the first time this
port has overridden a method of Apple's own class rather than subclassed one. What that costs is
said where it is written: the method is replaced for every MTLFunction in the process, Apple's
daemons included, and nothing is lost by that because the implementation it replaces returns nil
for all of them.

**Why at run time and not as a category**, which was the first version and is the obvious way: a
category on `_MTLFunction` makes the driver's dylib import `_OBJC_CLASS_$__MTLFunction`, and a
private class symbol that does not bind is a dylib that does not **load** — which would take the
whole driver down for every Metal client in the process rather than just this selector. The one
private class symbol this driver does import, `_OBJC_CLASS_$__MTLDevice`, is imported by
IOGPU.framework as well and has been measured loading on the phone (docs/metal.md 10.7);
`ipsw dyld symaddr -b` finds nothing in the cache that imports `_MTLFunction`'s, so nothing says
it would bind. `class_replaceMethod` on `objc_lookUpClass("_MTLFunction")` needs no symbol at all,
and it is the same move `ensure_spi` already makes for MTLDeviceSPI. Measured rather than
reasoned: the dylib's undefined private-class imports are the same nine
(`_MTLCommandBuffer`, `_MTLCommandEncoder`, `_MTLCommandQueue`, `_MTLComputePipelineState`,
`_MTLDepthStencilState`, `_MTLDevice`, `_MTLRenderPipelineState`, `_MTLResource`,
`_MTLSamplerState`) that it imported before this work, and it has no `__objc_catlist` section at
all.

**The differential, and the pictures.** `tools/air/argbuf_set.ll` is a fragment shader whose five
resources all arrive through one `air.indirect_buffer`; `tools/air/argbuf_direct.ll` is the same
shader with five direct arguments at the indices the frontend allocates for those members. Both
are drawn on softpipe through `rwa_render_draw`, and the four channels of one pixel are four
members:

| | R | G | B | A |
| --- | --- | --- | --- | --- |
| what it is | texture member 2 through sampler member 1 (nearest) | texture member 3 through sampler member 4 (linear) | buffer member 0 | texture member 2 **fetched** at texel (2,3) |
| direct arguments | 101 | 1100.75 | 7.5 | 102 |
| **through the encoder** | **101** | **1100.75** | **7.5** | **102** |
| the samplers exchanged | 100.75 | 1101 | 7.5 | 102 |
| the textures exchanged | 1101 | 100.75 | 7.5 | 1102 |
| buffer member at offset 64 | 101 | 1100.75 | **9.25** | 102 |
| buffer member at offset 4 | 101 | 1100.75 | **3.25** | 102 |

The first two rows being equal is the claim; the rest is what makes the equality worth anything.
Three of the four reference channels are exact arithmetic over the texel pattern
(`100 + x + 1000 × slot`) and are checked against it, so "the two agree" cannot be two draws
agreeing on the same wrong resource. The four channels are four different numbers, so exchanging
any two members has to show. And the two wrong pictures differ from each other as well as from the
right one — in A, which is a fetch and so moves with the texture and not with the sampler.

**Both gaps 8.2.1 left are closed, from the API side.** That section says a texture member's index
cannot be checked by running it and a sampler member's index is not checked at all, and names the
reasons: softpipe's compute path answers every texture from the view bound at slot 0, and
`air_run` binds one sampler state to every slot. A fragment shader and two real
`MTLSamplerDescriptor`-shaped states answer both — exchanging the two **sampler** records exchanges
the two filters and leaves the fetch alone; exchanging the two **texture** records moves 1000
through every channel that names a texture, the sampled and the fetched alike.

**One thing had to be measured before that shader could be written, and it is a limit of the
harness and not of the port.** `nir_to_tgsi.c` emits `ureg_DECL_sampler(c->ureg,
instr->sampler_index)` and **never looks at `texture_index`** ("It seems we can't get separate
tex/sampler on GL"), so on softpipe the texture unit IS the sampler index. The first version of
this shader sampled one texture through two samplers; the frontend emitted
`0 (texture), 1 (sampler)` for the second one, which is right, and softpipe answered with the
texels of the texture bound at slot 1. The Adreno does not have that limit — `ir3_compiler_nir.c`
collects `tex->texture_index` and `tex->sampler_index` as two immediates of one `sam` with S2EN —
so the shader is written in the one shape **both** back ends express: each sample uses the sampler
whose index equals its texture's, and the fetch uses texture 0, which is unit 0 either way.
Three references for that, sharing no code: Mesa's source, the NIR the frontend printed, and the
pixel.

**The refusal this feature is mostly made of, and what fires it.** An argument buffer the
application did not fill through an encoder is refused by name, and the sentence says the whole of
why — a buffer written by the processor, by a kernel or by a blit holds handles this driver cannot
interpret, and nothing distinguishes it from a correctly encoded one except the presence of a
record. The test binds a buffer whose bytes *look like* handles (`0x0000000100000000 + 0x40 × i`,
not zeros, so a pass cannot come from the bytes being obviously unreadable) and the draw is
refused. Six refusals are exercised, each on the words and not merely on the failure:

| refused | and the words |
| --- | --- |
| an argument buffer nobody encoded | `was never filled through an MTLArgumentEncoder` |
| nothing bound at the set's own Metal index | `nothing is bound there` |
| a member nobody set, in an encoding that exists | `never set on the encoder` |
| a buffer encoded where the shader declares a texture | `and a buffer was encoded there` |
| a resource destroyed after it was encoded | `has been destroyed since` |
| — and the other encodings in that buffer are untouched by the destroy | |

The destroyed case is the one thing here Metal does not have: there, a stale handle is the
application's business (which is what `-useResource:` is for), while a record of ours holds a
pointer this library would dereference, so `rwa_buffer_destroy`, `rwa_texture_destroy` and
`rwa_sampler_destroy` scrub themselves out of every record that names them and the draw names the
member. `useResource:`/`useHeap:` are therefore no-ops on this driver and say so once: every
member is bound explicitly, so there is no residency for an application to arrange.

**The buffer offset, which is not about argument buffers and was costing the window server real
bindings.** A recorded member arrives as (buffer, offset), and so does an ordinary
`-setFragmentBuffer:offset:atIndex:` — and that second one was refused: *"a non-zero offset is not
implemented, so this binding is refused"*, **55 times in one boot**, from the window server
compositing through this driver. Both are now one rule and one array: `struct rwa_buffer_bind`,
Metal's buffer table for a stage with the offset beside each entry, which `rwa_bindings.buffers`
has nowhere to put.

- a `constant` member or argument is served at **any** offset. One the GPU can fetch from directly
  is passed as the binding's own offset and nothing is copied; one it cannot — a6xx wants
  multiples of 64 and softpipe 16, where Metal allows any multiple of 4 — is bound as Gallium's
  `user_buffer`, which both back ends upload themselves (`upload_user_buffer` through
  `u_upload_data` at an alignment of 64; softpipe wraps the pointer). What is uploaded is
  `air.buffer_size` where the metadata carries one, which is all the shader can read.
- a `device` member or argument at an offset the GPU cannot fetch from is **refused by name**, and
  the asymmetry is the point: the shader may WRITE an SSBO, so the copy that serves a UBO would
  take the writes with it when it was thrown away — which is why Gallium has no `user_buffer` on a
  shader buffer either. **This refusal is written and unexercised**: no shader in reach binds a
  device buffer at a non-zero offset.

Measured on this machine, on the direct path and on the member path alike: offset 64 reads the
float at 64, offset 4 reads the float at 4, and **offset 1164 — the window server's own number —
reads the float at 1164**, with no other channel moving.

**Where the structures grew, because that decides what a stale driver does.** All of it is at the
**end**, and one struct was deliberately **not** grown:

| | bytes | and the field a stale driver already reads |
| --- | --- | --- |
| `struct rwa_draw` | 240 → **272** (+4 fields: the two buffer tables and their counts) | `primitive` is at +168 in both, `vertex_stream_count` at +232 in both |
| `struct rwa_render_info` | 100 → **108** (`vertex_argbuf_mask`, `fragment_argbuf_mask`) | `vertex_stream_mask` at +96 in both |
| `struct rwa_compute_info` | 24 → **28** (`argbuf_mask`) | `samplers` at +20 in both |
| `struct rwa_bindings` | 48 → **48**: nothing | — |

Those are compiled offsets of the two headers side by side and not an argument about where
the fields were typed.

`rwa_bindings` is the one that matters. It is embedded **by value** in `rwa_draw`, twice, so a
field added inside it would move `primitive`, `vertex_start`, `vertex_count` and every index field
after them for a driver built against the other header — which is exactly how a primitive type
once came back as **1871292816**. That is why the offsets are a second array beside it and why the
compute path got a new symbol (`rwa_dispatch_buffers`) instead of two more fields: a symbol that
is absent in an older library is something `dlsym` can see, and a struct that is a different shape
is not. The two info structs are filled in BY the library, so a driver built against the shorter
one is handed more than it has room for; both sides are built from one header and the staleness
stamp on `tools/mesa/air/` is what enforces it.

**What this does not establish.**

- **No draw goes through an argument buffer on the device.** That needs a `.metallib` holding a
  shader that declares one, and this port has no container writer: Apple's libraries have none
  (0 of 456) and the game's are not something a job list can reach. `userland/rc.jobs.argbuf`
  measures the three things the phone *can* answer — whose answer `newArgumentEncoderWithBufferIndex:`
  gives, the descriptor-built encoder's length, alignment and record count, and the window
  server's own offset through Apple's own shader against the host's reference words — and says so
  in its own text.
- **Nothing is timed.** A record lookup is a linked list walked once per set per draw; an
  application with thousands of encodings in one buffer is the thing to measure before that is a
  table.
- **The corpus did not move and could not have:** 456 read, **321** translate, **321** through
  ir3, the same numbers as 8.2.1, and nothing of this half is in the translation path.

### 8.3 The vocabulary: 254 operations, 136 the frontend had never seen

`corpus.py` reports what a module *calls* whether or not the module translates, so the operation
census survives the wall: **254 distinct `air.*` declarations**, 118 of them also called by the
system corpus and **136 new to this frontend**. 593 of the 7 879 modules call no `air.*` at all.

What the frontend does with each of the 254 cannot be read off the corpus run, because 7 620 of
these modules stop before an intrinsic is translated. So each declaration was put in a shader of
its own, called on constants with its result stored, and translated —
**`tools/air/decltry.py`**, whose verdict is the frontend's own message and not a judgement:

| declarations | verdict |
| --- | --- |
| 217 | translated (99 of them new to the frontend) |
| 31 | refused by name |
| 6 | not probeable: the indirect-command-buffer family, whose handle this cannot bind honestly |

**The probe's own arguments had to be measured, not chosen.** Its first version passed 3 for every
integer argument, and these signatures end in flags: it asked every texture write for mip level 3
and every atomic for memory order 3, and the frontend refused 20 declarations for the probe's
arguments rather than for the operation. Zero is the plain form, and that it is also *this
library's* form is a measurement over the call sites: all **3 168** atomic calls in the 7 879
modules pass memory order 0 (relaxed) — 0 of them anything else — and all **3 052** texture writes
pass mip level 0. The trailing flag word is 0, 2 or 3 at real call sites and the frontend ignores
it, which was checked by probing `read_texture_2d` with 3 and `write_texture_2d` with 2; the
sample calls split 23 823 level-form against 5 961 bias-form, and both forms translate.

**And the probe itself was checked where the answer is already known.** Over the system corpus's
own 222 declarations it refuses 8, and those 8 are accounted for. Two are the names that corpus's
own run names in an `intrinsic:` row, with the message word for word the same:
`air.is_null_texture_2d` (3 modules) and `air.sample_texture_2d_array.v4f16` ("with a 2-component
coordinate, wanting 3", which is what stops `alphaBlendRGBATextureArray`). Four are operations
whose callers stop before the call, which is the case this probe exists for —
`air.load.implicit_imageblock` and `air.quad_shuffle_xor.v4f16`, called only by the four
`tile_downsample_*` kernels that are refused at `interface: input air.imageblock`, so the corpus
run has nothing to say about them. The last two are the probe's own doing: a depth texture's plane
selector is the one integer argument whose plain value is 1 and not 0, so `air.sample_depth_2d` and
`air.read_depth_2d_ms` come back "with aspect 0" — `const`'s choice, not a gap, and nothing in
RE4's list depends on it, since its depth calls are compare samples refused before the plane is
read.

The 31 refusals and the 6 unbindable, grouped, with the number of modules each name appears in:

| modules (largest name) | names | the gap |
| --- | --- | --- |
| 1 468 | 2 | **gradient sampling** — `air.sample_texture_2d{,_array}_grad`. `parse_tex_name` reads the dimension suffix exactly, so `2d_grad` matches nothing: "a texture name this cannot take apart" |
| 1 318 | 8 | **array textures** — `sample`/`read`/`write`/`gather` of `2d_array`, `1d_array`. The array index is a separate argument after the coordinate, and the frontend wants it inside the coordinate vector: "with a 2-component coordinate, wanting 3" |
| 1 288 | 1 | **`air.discard_fragment`** |
| 621 | 1 | **cube sampling** — the cube signature has no offset vector where the 2d one does, so the frontend reads the argument after it as a flag: "argument 5 is not a constant" |
| 222 | 7 | **SIMD-group and quad operations** — `simd_broadcast_first`, `simd_ballot`, `simd_all`, `simd_is_helper_thread`, `simd_shuffle`, `quad_shuffle_xor` |
| 183 | 7 | **inverse trigonometric and hyperbolic** — `fast_acos`, `fast_asin` (in two widths), `fast_atan`, `fast_sinh`, `fast_cosh`, `tanh` |
| 28 | 2 | **depth compare sampling** — `sample_compare_depth_2d{,_array}`. ~~Refused on purpose~~ **translated now**, with the comparison checked on real texels in both directions (section 3.3). The module count is the one measured when the refusal was there; the library is not on this machine to re-measure |
| 8 | 2 | **atomic compare-exchange** — `atomic.{global,local}.cmpxchg.weak.i32` |
| 4 | 1 | **`air.get_sample_position`** |
| 1 | 6 | **indirect command buffers** — `reset_render_command`, `set_pipeline_state_render_command`, `set_{vertex,fragment}_buffer_render_command`, `draw_indexed_primitives_render_command`: one compute kernel in the whole library encodes draws on the GPU |

The same 37 flat, in the order of what closing one buys — the module count is how many of the 7 879 call that name, and the right-hand column is what `air2nir` answered the probe:

| modules | `air.*` | the frontend's answer |
| --- | --- | --- |
| 1 468 | `air.sample_texture_2d_grad.v4f32` | air.sample_texture_2d_grad (a texture name this cannot take apart) |
| 1 318 | `air.sample_texture_2d_array.v4f32` | air.sample_texture_2d_array with a 2-component coordinate, wanting 3 |
| 1 288 | `air.discard_fragment` | no case in the dispatcher |
| 1 176 | `air.sample_texture_1d_array.v4f32` | air.sample_texture_1d_array with a 1-component coordinate, wanting 2 |
| 913 | `air.read_texture_2d_array.v4f32` | air.read_texture_2d_array with a 2-component coordinate, wanting 3 |
| 621 | `air.sample_texture_cube.v4f32` | argument 5 is not a constant |
| 222 | `air.simd_broadcast_first.u.i32` | no case in the dispatcher |
| 214 | `air.read_texture_2d_array.u.v4i32` | air.read_texture_2d_array with a 2-component coordinate, wanting 3 |
| 183 | `air.fast_acos.f32` | no case in the dispatcher |
| 170 | `air.fast_atan.f32` | no case in the dispatcher |
| 112 | `air.sample_texture_2d_array_grad.v4f32` | air.sample_texture_2d_array_grad (a texture name this cannot take apart) |
| 102 | `air.simd_ballot.i64` | no case in the dispatcher |
| 101 | `air.simd_all` | no case in the dispatcher |
| 75 | `air.write_texture_2d_array.v4f32` | argument 3 is not a constant |
| 59 | `air.fast_asin.f32` | no case in the dispatcher |
| 57 | `air.simd_is_helper_thread` | no case in the dispatcher |
| 28 | `air.sample_compare_depth_2d_array.f32` | ~~air.sample_compare_depth_2d_array (compare sampling, not yet)~~ — translated (section 3.3) |
| 14 | `air.simd_broadcast_first.s.i32` | no case in the dispatcher |
| 10 | `air.gather_texture_2d_array.v4f32` | air.gather_texture_2d_array with a 2-component coordinate, wanting 3 |
| 9 | `air.write_texture_2d_array.u.v4i32` | argument 3 is not a constant |
| 8 | `air.atomic.local.cmpxchg.weak.i32` | atomic cmpxchg |
| 6 | `air.atomic.global.cmpxchg.weak.i32` | atomic cmpxchg |
| 4 | `air.get_sample_position.v2f32` | no case in the dispatcher |
| 4 | `air.simd_shuffle.u.i16` | no case in the dispatcher |
| 3 | `air.quad_shuffle_xor.u.i32` | no case in the dispatcher |
| 2 | `air.fast_asin.v2f32` | no case in the dispatcher |
| 1 | `air.draw_indexed_primitives_render_command.p2i16` | an indirect command buffer handle, which this probe cannot bind |
| 1 | `air.draw_indexed_primitives_render_command.p2i32` | an indirect command buffer handle, which this probe cannot bind |
| 1 | `air.fast_cosh.f32` | no case in the dispatcher |
| 1 | `air.fast_sinh.f32` | no case in the dispatcher |
| 1 | `air.reset_render_command` | an indirect command buffer handle, which this probe cannot bind |
| 1 | `air.sample_compare_depth_2d.f32` | ~~air.sample_compare_depth_2d (compare sampling, not yet)~~ — translated (section 3.3) |
| 1 | `air.set_fragment_buffer_render_command.p2i8` | an indirect command buffer handle, which this probe cannot bind |
| 1 | `air.set_pipeline_state_render_command` | an indirect command buffer handle, which this probe cannot bind |
| 1 | `air.set_vertex_buffer_render_command.p2i8` | an indirect command buffer handle, which this probe cannot bind |
| 1 | `air.tanh.f32` | no case in the dispatcher |
| 1 | `air.write_texture_1d_array.v4f32` | argument 3 is not a constant |

None of these 37 names is called by the system corpus, so none of them could have been found there
— but one of the *gaps* could: the array-coordinate one already stops `alphaBlendRGBATextureArray`
in QuartzCore's library through `air.sample_texture_2d_array.v4f16`, a half-typed variant of the
same shape, and `air.write_texture_2d_array.i16.v4f16` is declared there too. Everything else here
is new ground.

**And the order of work is a number rather than a guess.** Of the 7 620 modules behind the
argument buffer, **4 169 call nothing else this frontend refuses** — 1 770 kernels, 1 359 fragment
shaders and 1 040 vertex shaders. So the argument-buffer interface alone governs whether roughly
4 427 of the 7 879 (56%) can be attempted at all, and the table above says what the next 3 451 are
waiting for, biggest first. That is a statement about the operation vocabulary and nothing else: a
module with no refused call can still stop at another interface word, an instruction, or in ir3,
and which of those it does cannot be known until the interface exists.

### 8.4 What these numbers are not

- **258 of 7 879 is not "3% of the game runs".** Those 258 are almost entirely the shaders that do
  nothing an argument buffer is needed for: 140 of them compile to a *single* Adreno instruction,
  only 81 of the whole 7 879 produce more than four, and only **17** of the 258 call any `air.*`
  operation at all — full-screen-triangle vertex shaders, cleared pixel shaders, empty kernels.
  The right reading of the pair 258 / 7 879 is that this library is behind one wall, not that 3%
  of it works.
- **Reaching ir3 means it compiled**, here as everywhere in this document. Nothing in this section
  was run, on the Adreno or anywhere else; no result of these shaders has been compared with
  anything. The strongest check applied to them is the reader's: `reader-check.sh` canonicalises
  our reader's output and `llvm-dis`'s and compares them exactly, and it agrees on all 7 879.
- **The 217 "translated" verdicts are about a probe, not about this library's modules.** Each is
  one call in a shader written here, with constants this section measured; a real call site brings
  a coordinate computed from a varying, a sampler from an argument buffer, and neighbours. The
  verdict is only the frontend's answer to "do you know this operation at all", which is exactly
  what the gap list needs and no more.
- **`newLibraryWithSource:` is out of scope.** Some of this binary's shaders are compiled at run
  time from MSL text by Apple's own compiler, which is not something this frontend can be asked
  about; the census covers the libraries that arrive as AIR.

### 8.5 Four of those gaps closed: 2 990 → 4 447, and the two readings that cost nothing to get wrong

Section 8.3's list is in the order of what closing one buys. This is what the first four bought,
measured with the same tree on both sides — the *before* binary is the frontend's own
`air_intrinsics.c` from the commit before the work, linked against everything else unchanged, so
the difference is this file and nothing else:

| corpus | read | translate | through ir3 |
| --- | --- | --- | --- |
| iOS 16.5's system libraries, 456 modules | 456 | **321** | **321** |
| Resident Evil 4, 7 879 modules, before | 7 879 | **2 990** | 2 978 |
| Resident Evil 4, 7 879 modules, after | 7 879 | **4 447** | **4 435** |

The system corpus does not move, and that is stated module for module rather than as a count:
against the committed `build/air-report.json`, the same 456 modules, **not one** whose verdict
changed, **not one** whose NIR instruction count changed and **not one** whose Adreno instruction
count changed. The 12-module gap between translating and compiling in Resident Evil 4 is likewise
the **same 12 modules** in both runs, compared path by path and not by count — kernels that ir3
refuses for a half/full register mismatch, none of which calls anything added here — measured one
by one in 8.6, that is 7 of the 12, and the other 5 are an ir3 opcode gap, `uclz`. (The before
run has its own `ir3_compiler` as well as its own `air2nir`: that binary links the frontend too,
so a before column measured with the current one would have been the after column.)

| what the module is | modules | before | after | through ir3 |
| --- | --- | --- | --- | --- |
| fragment | 3 741 | 1 202 | **2 123** | 2 123 |
| kernel | 3 011 | 1 490 | **1 893** | 1 881 |
| vertex | 1 127 | 298 | **431** | 431 |

Per refusal site, which is the honest form of the number because a module that stops at one site
goes on to stop at another:

| modules before | after | the site |
| --- | --- | --- |
| 858 | 0 | `air.sample_texture_2d_grad (a texture name this cannot take apart)` |
| 769 | 0 | `air.discard_fragment` |
| 501 | 0 | `air.sample_texture_2d_array with a 2-component coordinate, wanting 3` |
| 324 | 0 | `air.read_texture_2d_array ...` |
| 101 | 0 | `air.simd_broadcast_first.u.i32` |
| 79 | 0 | `argument 5 is not a constant` — the cube signature |
| 71 | 0 | `llvm.fmuladd.v4f32` |
| 60 | 0 | `air.sample_texture_1d_array with a 1-component coordinate, wanting 2` |
| 54 | 0 | `air.fast_acos.f32` |
| 36 | 0 | `llvm.fabs.f32` |

2 886 modules no longer stop at one of those, **1 457 of them translate**, and the other 1 429 stop
at the next thing — 1 255 of them at `instruction: a bitcast that changes the shape of its value`,
which has nothing to do with any of this and is now the largest single refusal in the library.
(1 255 is how many *arrived* there; 1 224 were already there and the row is 2 479 — see 8.6.) The
whole `intrinsic:` category is down to four rows and 132 modules: `air.simd_ballot` (96),
`air.sample_compare_depth_2d_array` (28), `air.get_sample_position` (4) and
`air.atomic.*.cmpxchg` (4). **The 28 are now translated** (section 3.3); what that does to this
library's totals was *not* re-measured, because the package is not on this machine and nothing of
it is or may be in this repository. The 456-module corpus's numbers below are the measured ones.

#### The signatures, and where they were read

Three bracketed arguments in section 3.2's texture signatures, each read off **Apple's own
metal_stdlib on the device** (`GPUCompiler.framework/.../include/metal/metal_texture`, where every
`texture2d_array<T>::sample` and its fifty neighbours is a one-line forwarder to the builtin, so
the header states the argument order for nine dimensionalities and six operations) and then
confirmed against the **LLVM declaration in the modules that call them**. The two agree everywhere:

- **an array index is its own argument**, straight after the coordinate, in every one of sample,
  gather, read and write — `air.sample_texture_2d_array.v4f32(ptr, ptr, <2 x float>, i32, i1, <2 x i32>, i1, float, float, i32)`.
  The frontend wanted it inside the coordinate vector, and said so: "with a 2-component
  coordinate, wanting 3".
- **the gradient form reorders what follows it**: `dPdx, dPdy, min_lod` come *before* the offset
  pair, where the plain form has the offset pair first and the level last. A reader that assumed
  one order for both takes a gradient for an offset flag.
- **a cube has no offset pair at all**. Reading the 2d shape there consumed the level flag and the
  level, and left the float minimum level to be read as the flags word — which is the whole of the
  621-module "argument 5 is not a constant".

#### `air.discard_fragment` is `demote`, and the reasoning is not a preference

Metal's discard does not end the thread, so this is NIR's `demote` (the lane becomes a helper
invocation: it keeps executing, its writes are dropped, and it still supplies the neighbour values
a derivative needs) and not NIR's `terminate`. Nothing in a corpus count can decide that — both
translate, both validate, both compile — so it is decided from two independent readings, and both
say the thread lives:

1. **The shaders.** `PS_DrawByTiles` calls `air.discard_fragment` at the ninth instruction of a
   1 800-line function and then calls `air.simd_is_helper_thread()` **eight times** further down
   it. A terminated thread cannot be asked whether it is a helper thread. 24 of the library's
   1 288 discard-calling modules call that function at all.
2. **Apple's own declaration.** `llvm-dis` of that module reads
   `declare void @air.discard_fragment() local_unnamed_addr #8` with `attributes #8 = { nounwind }`
   — and no `noreturn`. LLVM may delete everything after a call it is told does not return, and
   Apple's compiler kept the rest of the function; of 85 sampled call sites, 73 are followed by
   `br` and none by `unreachable`.

And the cost of the other choice is not hypothetical: of those 85 sites, **45 are followed in the
same function by an `air.sample_*` with an implicit level of detail and 6 by `air.dfdx`**, both of
which read the other lanes of the quad.

**What this hardware does with either is a third question, and the answer is "the same thing".**
ir3 keeps demote and terminate apart in its own IR, and `nir_to_tgsi` sends them to TGSI's
separate DEMOTE and KILL — but the a6xx instruction encoder maps one onto the other
(`} else if (instr->opc == OPC_DEMOTE) { return OPC_KILL; }`, `src/freedreno/isa/encode.c`) and
the ISA description has no demote bitset at all (`ir3-cat0.xml` carries `kill` and nothing beside
it). So the distinction this frontend makes in NIR does not reach the machine code on this GPU.
That is a gap to name rather than a reason to translate it as `terminate`: terminate would lose
the same lanes and would be wrong on a backend that does tell them apart.

**Stores are not one of the differences**, and that sentence replaces one that said they were.
NIR's own definition of `demote` is "Any stores to memory after this instruction are suppressed
and the fragment does not write outputs to the framebuffer. Unlike discard, demote needs to ensure
that derivatives will still work for invocations that were not demoted" — so demote drops the
lane's later writes too, and differs from terminate in the derivatives and in the lane going on
executing, not in the memory. Whether *Metal* drops them is a third question that nothing on this
machine answers: no header on the device says so, and a corpus cannot show it. If Metal keeps
them, both of NIR's intrinsics are wrong about it by the same amount.

**No value was compared for it anywhere.** softpipe cannot: `tgsi_exec.c` has a case for KILL and
none for DEMOTE, so its interpreter falls through to its own `assert(0)` — which is why `air_run`
now refuses a discarding shader by name instead of aborting inside it, and why that refusal is
also how `info.fs.uses_discard` is checked (the refusal reads that flag and nothing else).
`tools/air/discardcheck.py` is what is decidable here — it translates, the NIR carries `demote`,
ir3 compiles it, the Adreno instruction is `kill` — and `--emit` writes the two modules that would
settle the rest on the phone: one that discards every fragment (the attachment must still hold the
clear colour) and one that keeps the pixel at (0, 0), discards its three quad partners and takes a
derivative (3 if the neighbour's value was still there). `userland/rc.jobs.airdiscard` is that job
list, with the greps that read its log.

#### `llvm.fmuladd` is translated fused, and the fast-math flag is not what licenses it

383 of a 394-module sample carry `air.compile.fast_math_enable`, so contraction is permitted under
the stricter reading too — but that measurement is not the licence and the eleven modules without
the flag are not a different case. LLVM's own Language Reference says of `llvm.fmuladd` that the
expression is equivalent to `a * b + c` "except that it is unspecified whether rounding will be
performed between the multiplication and addition steps", and that a caller who needs the fusion
*guaranteed* must use `llvm.fma` instead. A producer that emitted `llvm.fmuladd` rather than
`fmul` + `fadd` accepted both answers when it chose the name; which one it gets is the back end's,
and this frontend's is the fused one (`nir_ffma`, one rounding).

Checked by folding against C's `fma` over **9 534 cases** in three widths and two vector
shapes — `tools/air/intrincheck.py` reads `llvm.fmuladd` and `llvm.fabs` now, not only `air.*`.
LLVM on this processor is *not* a second reference for it: `llc` without `-mattr=+fma` lowers the
intrinsic to a multiply and an add, which is the other legal answer, so a disagreement would be a
measurement of LLVM's choice. The Adreno's `mad` is a question for the phone, and
`intrincheck.py --run` sets `fma` and `fmuladd` aside on softpipe for exactly that reason (TGSI's
own specification lets MAD round the product first, and its interpreter does).

#### The SIMD-group operations take the group this implementation has

Metal does not fix the SIMD-group's width — a shader asks with `air.get_simd_width`, a host asks
`MTLComputePipelineState.threadExecutionWidth` — and it is 32 on every Apple GPU, which is why a
shader compiled for one may have the number built into it. This hardware's is 64 or 128
(`threadsize_base` 64 for the A635, doubled per variant in the compute and fragment stages). So
`air.simd_all`, `air.simd_any`, `air.simd_broadcast_first` and `air.simd_shuffle` become NIR's
`vote_all`, `vote_any`, `read_first_invocation` and `shuffle` over **the group this implementation
has**, and each leaves a note saying so: the width a shader is told and the width its votes are
taken over are the same number, so a shader that reads `air.get_simd_width` is right and one with
32 written into it is not — as it would be on any Metal implementation whose SIMD-group is not 32
wide.

`air.simd_is_helper_thread` needs no such note (a helper thread is a property of the thread) and
neither do the **quad** operations: a quad is four consecutive lanes on both machines — it is what
a fragment derivative is computed over — so `air.quad_shuffle_xor` with a constant mask is one of
NIR's three exact quad swaps, and `nir_lower_subgroups` leaves the quad intrinsics alone for ir3.

**Two are refused by name instead, and a named refusal is the deliverable there:**

- `air.simd_ballot` returns a 64-bit mask with one bit per lane, and this frontend cannot bound
  the lane count at 64 — ir3 runs 64 **or 128**. A truncated mask makes `popcount` answer a
  smaller number rather than an obviously wrong one, so 96 modules stay refused.
- `air.get_sample_position`: NIR has the operation and the Adreno has the instruction
  (`load_sample_pos_from_id`, which ir3 emits as `RGETPOS`), but that intrinsic's convention is an
  **offset from the pixel centre** — ir3's own lowering of `gl_SamplePosition` adds (0.5, 0.5) to
  it and cites the dEQP case that fixes the difference — and Metal's convention is stated by
  nothing on this machine: not by the `metal_graphics` header on the device, which only names the
  function and its MSL section, and not by any string in the compiler libraries. The two readings
  differ by exactly half a pixel, which looks like a slightly soft image rather than like a bug.
  4 modules, and the way to settle it is a measurement on the phone.

#### What the textures were proved with, and the one thing they were not

`tools/air/texopcheck.py` goes from 35 texture calls to **62, every one returning exactly the
texel that was put there**, through softpipe — a second engine, since it executes TGSI and its
answer is not pre-decided by our folder. The new 27 are array samples, reads, gathers and
1d-array samples at coordinates whose **layer differs from their x and y**, so a layer read out of
the wrong component cannot agree either, and gradient samples whose gradients are three distinct
non-zero pairs. `air_run` grew the array binding that needs: a layer index handed to a 2D texture
is ignored, so without a real array texture every layer would have answered with layer 0's texels
and the check would have been empty. The negative control was run by hand — changing one shader's
layer from 2 to 3 moves the answer from 304 to 305.

**The level of detail a gradient selects is not checked by any of that**, and it cannot be with
this harness: `air_run`'s texture has one mip level, so every level clamps to it and a level
computed from the wrong pair of numbers would still read level 0. What the gradient cases do
establish is that the gradients are consumed *as gradients* — three distinct non-zero pairs, so a
lowering that read one of them as the coordinate, as the offset vector or as the level flag
answers with a different texel or refuses. Checking the level itself needs a mipmapped texture and
a mip filter that is not NONE, which would change what every other check in that file samples.

#### The inverse trigonometric and hyperbolic functions

`acos` and `asin` reduce to `atan2`, which is exact as a reduction rather than an approximation of
its own — the polynomial in Mesa's SPIR-V front end is the alternative, and its own comment says
it "isn't precise enough to meet half-float precision requirements" and names this formula as the
one it rejected for cost. The root is of `(1 - x)(1 + x)` and not of `1 - x*x`: the same number in
exact arithmetic, and not in a float near |x| = 1, which is where a normal dotted with a light
direction spends its time. `sinh` and `tanh` hand back their argument below 2^-12 (2^-5 for a
half), where `x^3/6` is under the type's own precision — with Mesa's threshold of zero,
`intrincheck.py` reported `tanh(1e-8)` coming back as 0, which is an infinite relative error
rather than a licensed one.

These are **approximate** and are reported as such: the reference is this machine's libm, Metal's
`fast_` prefix licenses a deviation, and the numbers are measurements and not verdicts. Over 172
folded cases: worst 13 ulp for `acos`, 1 for `cosh`, 2 for `sinh` and `tanh` with the threshold
above (841 731 191 without it, which is what answering 0 for 1e-8 reads as), and 233 at x = 1e-8
for `atan` and for `asin`, which goes through it — that one is Mesa's own `nir_atan` polynomial
losing the low bits of a tiny argument and not this file's arithmetic.

### 8.6 The bitcast that changes shape: 4 447 → 6 057, and the census that made it two cases

Section 8.5 left `instruction: a bitcast that changes the shape of its value` as the largest single
row in this library's refusal histogram. It is one case in `translate_cast` now, and the whole of
the work was deciding that it is **one** case:

| corpus | read | translate | through ir3 |
| --- | --- | --- | --- |
| iOS 16.5's system libraries, 456 modules | 456 | **321** | **321** |
| Resident Evil 4, 7 879 modules, before | 7 879 | **4 447** | 4 435 |
| Resident Evil 4, 7 879 modules, after | 7 879 | **6 057** | **6 032** |

| what the module is | modules | before | after | through ir3 |
| --- | --- | --- | --- | --- |
| fragment | 3 741 | 2 123 | **2 905** | 2 905 |
| kernel | 3 011 | 1 893 | **2 648** | 2 623 |
| vertex | 1 127 | 431 | **504** | 504 |

The system corpus does not move, and here it is stated in the strongest form the report allows:
`tools/air/check.sh` regenerated `build/air-report.json` and `docs/air-report.md` with the new
frontend and both came out **byte for byte identical** to the committed ones — the same 456 rows,
the same verdicts, the same NIR instruction count and the same Adreno instruction count on every
one of them, so `git diff` on the report is the comparison and it is empty. For this change that
was predictable *before* the run as well, which is unusual and worth saying: the system corpus
contains **0 shape-changing bitcasts** (below), so there was no module there for it to reach.

**First, a number corrected.** 8.5 says "1 255 of them at `a bitcast that changes the shape of its
value`, which … is now the largest single refusal in the library". The 1 255 is the number of
modules that *arrived* at that refusal when the intrinsic work pushed them past an `intrinsic:`
row; 1 224 were already there, and the row's real height was **2 479 of 7 879**. A delta and a
total in the same sentence is how a measurement drifts, so: 2 479 is what this change was aimed at,
1 610 of them now translate, and the other 869 stop at the next thing — 853 at a narrow integer,
6 at `air.atomic.local.cmpxchg`, 5 at a `phi` between pointers and 5 at `insertelement` at a
run-time index.

#### What a census buys that a histogram cannot

A refusal stops a module at its **first** offending instruction, so the histogram says how many
modules stop and nothing about what they would have asked for next. A case written for what the
histogram shows would mangle whatever was behind it. `tools/air/typecensus.py` reads every
`bitcast` of every module out of `llvm-dis` — LLVM's own reading of the types, not ours — and
classifies each by air_to_nir's own shape test, component count and scalar width. Over **all 7 879
modules**, 111 657 bitcasts in 14 distinct type pairs, and exactly **two** of the fourteen change
shape:

| occurrences | modules | pair |
| --- | --- | --- |
| 44 136 | 2 927 | `i32` → `<2 x half>` |
| 4 333 | 688 | `<2 x half>` → `i32` |

Everything else is same-shape (`i32` ↔ `float`, 22 561; `<3 x i32>` ↔ `<3 x float>`, 5 218;
`<4 x i32>` ↔ `<4 x float>`, 5 357; `<2 x i32>` ↔ `<2 x float>`, 175) or a pointer (29 877, which
the frontend translates by rewriting the pointee and never by moving bits). And over the 456
modules of the system corpus: **0 shape-changing, 22 same-shape, 1 144 pointers**.

So there is no third shape to be found behind these two, and that is what makes the fix
`nir_unpack_32_2x16` and `nir_pack_32_2x16` rather than a repacker: NIR has an operation for
exactly one redistribution at exactly one width, and it is the only one asked for. The frontend's
own reading agrees with llvm-dis after the fact as well as before it — the refusal that replaced
the old one spells out the two shapes it was given (`a bitcast from %u x %u bits to %u x %u bits`)
and **does not occur once** in the 7 879-module histogram.

The case is keyed on the shape and not on the AIR type names. NIR's SSA has no 32-bit float
distinct from a 32-bit integer, so `float -> <2 x half>` — which this library does not contain — is
the same bit movement as `i32 -> <2 x half>` and is right for the same reason; a case keyed on the
names would buy a refusal for a module whose bits were being moved correctly.

This is how an HLSL `min16float2` reaches here through SPIRV-Cross: the pair travels through the
shader packed in one 32-bit register and is unpacked where it is used.

#### Which half is which, and what the wrong answer would have looked like

LLVM lays element 0 of a vector at the lowest address, so element 0 of the `<2 x half>` is bits
0..15 of the `i32`; `unpack_32_2x16` is defined `dst.x = src0.x; dst.y = src0.x >> 16` and
`pack_32_2x16` as `dst.x = src0.x | (src0.y << 16)` — the same halves in the same order. Getting
that backwards is the failure this operation invites, because it does not crash and does not look
wrong: both halves are still finite halves, so the shader would draw with every pair of 16-bit
values transposed.

So `tools/air/loweringcheck.py` grew four cases, with halves that cannot be mistaken for each
other, checked **twice**:

| the module | the four lanes | what the transposed order would have given |
| --- | --- | --- |
| `i32` → `<2 x half>`, halves 13 and 113 | 13, 113, −100, 126 | 113, 13, **+100**, 126 |
| `i32` → `<2 x half>`, halves 1 and −2 | 1, −2, 3, −1 | **−2**, 1, **−3**, −1 |
| `i32` → `<2 x half>`, the buffer's own word `0x221B140D` | 0.000988960266, 0.0119247437, −0.0109357834, 0.0129137039 | 0.0119247437, 0.000988960266, **+0.0109357834**, 0.0129137039 |
| `<2 x half>` → `i32`, the halves of 13 and 113 | 19072, 22288, −3216, 41360 | 22288, 19072, **+3216**, 41360 |

The lanes are the two halves, their difference and their sum, and the last two are there so that a
failure says *which* of the two things went wrong: transposing negates the difference and leaves
the sum alone. The first reference is LLVM compiling the same module for this processor and running
it over the same buffer bytes, against our own NIR folded over those bytes — bit-exact on all four.
The second is the four numbers themselves, which are written out in the case list and come from
**Python's own IEEE-754 binary16 codec** and an explicit `(high << 16) | low`. That second one is
not pedantry: LLVM compiles for this processor, which is little-endian, and our reading of which
half is element 0 is little-endian too, so agreement between two readings of one convention would
be just as perfect if the convention were the wrong one. There are three statements of which half
is which here — LLVM's, ours and Python's — and the third is the only one that is not a reading of
the first.

Two things these cases deliberately are **not**. They are not a round trip: a pack fed by an
unpack comes back correct with both halves consistently transposed, so it would pass for the wrong
reason, and the two directions are kept apart with different values going in than come out. And
they are not constant-folded away before they are tested: the word under test is
`(the buffer's first byte − 13) + K`, which is `K` only for a frontend that actually read the
buffer, because `cpuref` fills every buffer with `(i * 7 + 13) & 0xff`.

**What could not be checked, and why it is not this lowering's fault.** Every other numeric check
in this tree can also go through a Gallium driver, which is a second *executor* rather than a second
opinion. Not these: `nir_to_tgsi` has no 16-bit arithmetic at all. A module that does nothing but
`fptrunc float to half` and `fpext half to float`, with no bitcast anywhere in it, aborts softpipe
with `Unknown NIR opcode: u2f16` — so the drawn reference is unavailable for anything with a `half`
in it. The 1 610 modules this change admits are, so far, carried by LLVM on this processor and
by arithmetic, and by no execution on any GPU at all.

What *is* available without a boot is the machine code, which is weaker than running it and is
not nothing: `ir3_compiler` on those four modules puts the whole computation in the shader
preamble — Adreno instructions, executed by the shader, reading the buffer out of the constant
file — and the half order is legible in it. Unpacking:

```
and.b   r48.x, c4.x, 255        ; the buffer's first byte, c4.x being the UBO load
add.u   r48.x, c8.x, r48.x      ; + 0x57104A73, the constant with the -13 folded in
cov.u32u16 hr48.z, r48.x        ; bits 0..15 into a HALF register -- element 0
cov.f16f32 (even)r48.z, hr48.z  ;   and out as lane 0
shr.b   hr48.x, r48.x, 16       ; bits 16..31 -- element 1
cov.f16f32 (even)r48.w, hr48.x  ;   and out as lane 1
```

and packing, where `shlg` is shift-left-and-or and does the whole of it in one instruction:

```
cov.u32f16 (even)hr48.x, r48.x  ; half(13)
cov.u16u32 r48.z, hr48.x        ;   its bits -- the LOW field
cov.f32f16 (even)hr48.z, r48.y  ; half(113)
cov.u16u32 r48.w, hr48.z        ;   its bits -- the HIGH field
shlg    r48.w, 16, r48.w, r48.z ; (high << 16) | low
```

So the a6xx takes the low half of the word as element 0 of the vector, which is the thing the
transposition would have got wrong, and it is in the instruction stream rather than inferred. It
is still not a measurement: nobody has read a pixel back. That half is written as a job list
rather than claimed: `userland/rc.jobs.pack16`, emitted by
`loweringcheck.py --emit <dir>` and judged afterwards by `loweringcheck.py --check <dir> <log>`
against expectations that are not in the image. The greps are in its header, and one of them is
`grep -ac 'Unknown NIR opcode' LOG` — which must be 0, because that sentence is `nir_to_tgsi`, so
it would mean softpipe answered and the Adreno was never asked.

#### The thirteen modules ir3 gained and would not compile

6 057 translate and 6 032 reach ir3. The 25 that do not are the **same 12** as before plus 13, and
the 13 are not a new kind of failure: every one is `Unhandled ALU op: uclz`, which is
`air.clz` → `nir_uclz` (`air_intrinsics.c`) meeting an ir3 that has no `uclz`. They are 9 further
variants of `CS_Finalize` and 4 of `CubemapFilterCS`/`CubemapFitlerCopyCS`, and 5 of the
pre-existing 12 fail the same way — 3 of those 5 being further variants of `CS_Finalize` itself and
2 of them `CS_LightCulling`. **0** modules that compiled before stopped compiling, and **0**
changed their Adreno instruction count.

While reading those: 8.5 describes the 12 as "kernels that ir3 refuses for a half/full register
mismatch". Measured one by one, that is 7 of them (`RenderCloudscapeCS`, which fails
`ir3_validate.c:361`, `type_size(cat1.src_type) <= 16 == !!(src->flags & IR3_REG_HALF)`) and the
other 5 are `uclz`. Worth correcting because the half/full one is the failure a 16-bit pack could
plausibly have *caused*, and the measurement says this change caused none: all 13 of its new ir3
failures are the opcode gap and not the register one.

#### What is now the largest wall, and the census for it

An integer of a width NIR does not have — **1 388 of 7 879**, by width 2 → 867, 4 → 437, 3 → 60,
5 → 10, 31 → 10, 6 → 2, 9 → 2. It is the same question one level over, so it was censused the same
way, and the answer is as narrow: `typecensus.py --widths` over all 7 879 modules finds 29 278
instructions mentioning a non-native width, and the **entire vocabulary** is

| occurrences | what it is |
| --- | --- |
| 19 218 | a `switch` case label (`i2 0, label %347`) |
| 5 143 | the `switch` itself |
| 4 898 | `trunc i32 %x to iN`, which produces the selector |
| 19 | `phi iN` (18) and one `select i1, i3, i3` |

and **nothing else**: not one `add`, `and`, `shl`, `icmp`, `zext`, `sext`, `load`, `store`,
`bitcast` or call with a non-native width in 7 879 modules, and **0** of either in the 456-module
system corpus. A narrow integer in this library is a switch selector and is never arithmetic.

That bounds the work: represent a non-native `iN` as a 32-bit def holding the value in its low N
bits, mask at the `trunc` that makes it, mask each case constant at the `switch` that consumes it,
carry 32 bits through a `phi` or a `select`, and refuse everything else by name — with one trap
visible in the census, that the case labels are **signed** N-bit constants (`i5 -13`, `i6 -14`,
`i7 -57`), so the comparison has to be between two N-bit patterns and not between two i32s.

It was not done here, and the reason is not the length of the code: `air_nir_bit_size` is called
from **22** places (18 in `air_to_nir.c`, 4 in `air_intrinsics.c`), including the ones that declare
an interface and the ones that load and store, and making it answer 32 for an `i2` would change
what every one of them does silently. A
width that is wrong at a load is wrong bytes, which is the failure this project is built to refuse
rather than to risk. It is a second clean change, not an addendum to this one.

#### The 86 that were a different thing wearing the same sentence, measured

The group that used to read `a 0-bit integer, which is a width NIR does not have` is already a
refusal of its own (`air_to_nir.c`, the output declaration): **0** modules of either corpus now
stop at the no-single-width refusal. What the 86 are is worth one more measurement, because the
sentence that replaced it was also saying something it had not checked. Over all 86 — every one a
vertex shader — the output's own type in the module is **`[1 x float]`**, an array of exactly
**one**, in every one of the 86, and Metal's `air.arg_type_name` on the `air.clip_distance` node
says `float`. There is no module in reach that declares two clip planes, so the shape in the way is
an array of one: a scalar wearing a bracket, and not the `[4 x float]` the code's comment named.
The message now carries the length.

That is left refused rather than half built, and deliberately: closing it is two things, the
declaration taking the element type *and* `emit_return` storing `elems[0]` of a one-element
aggregate instead of skipping a field that is not an `AIR_VAL_SSA` — which is what would silently
drop the plane. A clip distance written to nothing is a shader that draws without the clipping it
asked for, and nothing in this tree can check a vertex clip distance numerically.

#### What the run that measured this found about the run itself

`tools/air/check.sh` does **not** exit 0 in this tree, and it did not before this change either. One
section fails, `== the same operations run, not folded` (`intrincheck.py --run --cases 4`), with
**34 of 171 exactly defined operations differing** from the reference when executed on softpipe
rather than folded. Three measurements say it is not this change:

- the suite it runs is **1 922 generated modules, and not one of them contains a bitcast of any
  kind** — so the case added above is unreachable from this check;
- the **pre-change binaries, run today**, give the same 34 operations with the same per-case
  counts: 137 agree, 34 differ, 3 508 cases. The frontend before and the frontend after are
  indistinguishable here;
- one operation taken on its own through both binaries gives **byte-identical wrong answers**:
  `air.clamp.s.v2i64` of `(0,1)` by `(0,1)`,`(0,1)` comes back `(0,0)` from each, and
  `air.fmod.v2f32` loses a `-0` to `+0` in component 1 in each.

What differs is **how much of the suite ran**, and that is worth writing down because it is the
project's own meter misreading itself. Every earlier log of this section tonight reports 604, 612,
808 or 840 cases and 0 or 1 differing; the leftover `/tmp/intrincheck.run.*` directories record the
suites those runs actually wrote — **225 modules, then 435, then 1 816, then 1 922** — and
`emit_suite` never invokes the frontend, so a suite that grew fourfold did not grow because of a
compiler. It grew because the declaration set it is built from grew, and that set is read by
running `air2nir --ll` over all 128 `.metallib` files of the image in `collect_declarations`,
which swallows a failed run:

```python
try:
    ll = subprocess.run([AIR2NIR, "--ll", lib], capture_output=True).stdout
except OSError:
    continue
```

An `OSError` is skipped and a non-zero exit is not looked at at all, so every library `air2nir`
could not be executed for — and it is relinked constantly while anyone is working in this tree —
drops out of the vocabulary silently. A quarter of the operations then never reach softpipe, and
the section prints `0 differ` about the quarter that did. The line that would have said so,
`N modules are in the manifest and not in the log`, is printed above the summary and
`check.sh` pipes this section through `tail -6`, which cuts it off.

So the 34 are not new and the 0 was not right: they are i64 `min`/`max`/`clamp`/`convert`, where a
64-bit vector component comes back zero, and `fmod`, where a negative zero comes back positive —
on **softpipe**, which is the same kind of verdict as the two already set aside by name in that
file (a narrow type, and `ffma` through TGSI's `MAD`). Whether the Adreno agrees is a different
question and this file cannot answer it. Two things would make the meter honest and neither is
this change: `collect_declarations` should fail loudly on a library it could not read, and the
`tail -6` should be wide enough to show the line that says how much of the suite was judged.
