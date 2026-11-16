# How our own checks lie

Eleven ways a check lies, and one habit against them. Each one cost us a run or a wrong line in the
README during the work on this port. They are written down because eleven of the twelve look the same:
**missing information reads as a missing problem.**

- **A skip that is not counted.** A `continue` that adds nothing to the total turns "not checked" into
  "checked and matched". Ten AIR declarations were listed as matching after being compared on one case of
  four (the other three were skipped on an endless wait); `if idx >= len(flat): continue` silently dropped
  short rows; a `read()` into a fixed buffer lost the tail of a file. The total still added up --
  68 + 3 + 7 = 78 -- because the deferred ones were counted as matches. What catches it is not the total but
  **a total in which every exception has its own column with a name and a number**. The extreme case of the
  same trouble is a check over an empty set: a loop of zero iterations prints "0 of 47" and looks exactly
  like an honest zero. It happened once -- the list of kernels lived in a directory that was cleaned between
  sessions -- and only the shell saying "no such file" separately saved us. A denominator lies as quietly as
  a summand.
- **A silenced command.** `patch -s --dry-run` returned 0 and said nothing, and the silence was read as
  "applies exactly"; without `-s` it said `succeeded at 410 (offset 2 lines)`.
- **Inference from absence.** "Not in the log" and "did not happen" are different things: the loader's log
  drops its middle (the sign is a size at the limit and the exact line `the log between the first 256 KB and
  the last 1 MB was dropped`, not the words `was dropped`, which the button driver also prints). A `grep`'s
  field of view reads the same way: six matches of `bin/sh` were `/var/jb/bin/sh`, and "`xl ramdisk` has no
  guard" missed a guard twenty lines higher. And a silent instrument first has to be shown that it **can**
  speak: a watchdog that "never fired" proves something only together with the number of times it ran (ours
  came out 26 of 26 with zero drift); a measurement that printed zero lines once turned out to be a `grep`
  for a pattern `printf` never printed -- a measurement that cannot fire looks exactly like an event that did
  not happen. That zero was caught not by doubting the instrument but by **contradicting what was already
  measured**: three modules translate thanks to pointer binding, so there could not be a zero there. When
  there is no control, consistency with earlier numbers plays its part -- and it works only if the earlier
  results were written down as numbers, not words.
- **A pair without a control.** "Before and after" without "before and before" answers the wrong question:
  10,835 differing pixels looked like the effect of a patch until two runs of the same build gave 13,738.
- **Your own tree instead of the subject.** Compare with someone else's tree only from a clean unpack: our
  copy of Mesa is patched (202,487 bytes against 202,144), and the difference would have been found "on their
  side".
- **A stale instrument in one column.** In the corpus table the "translates" column was counted with the
  frontend from 10:57 and the "compiles with ir3" column with a binary from 09:16: `ir3_compiler` is not part
  of the default target and an ordinary rebuild did not touch it. The result was "315 translate, 313 through
  ir3", and it made a plausible story -- "ir3 cannot do two of something" -- until it turned out the refusal
  message was text removed from the frontend an hour earlier. In fact it was 315/315. Two figures of one table
  measured by different generations of the instrument produce not an error but a **finding that does not
  exist**. The cure is a refusal, not a warning: `corpus.py` no longer runs if `ir3_compiler` is older than any
  frontend source.
- **A true complaint about the wrong thing.** A debugging instrument can report a truth that has nothing to do
  with the problem, and so hide the real cause. When three AIR modules crashed in `nir_repair_ssa`,
  `nir_validate_shader` reported four errors about block indices -- and **exactly the same ones** on a module
  that translates perfectly. That is the state of an unstructured impl before `lower_goto_ifs`, and harmless;
  taking the complaint for the answer, we would have fixed indexing and got nowhere. The real cause (values
  created in the entry point's impl and used in a callback) was found by printing, not by the validator. The
  rule: **take any diagnostic instrument's reading on a known-good input first** -- without it, its output
  cannot be told from an answer.
- **An instrument replaced during the measurement.** `check.sh` was running while `air2nir` was rebuilt under
  it. The run exited 0 -- and had checked a binary that no longer existed by the end. Unlike a stale
  instrument, this one is valid before and after and not the same in the middle; the green is
  indistinguishable from a real one. Same cure as everything else: measure on a tree nobody touches while the
  measurement runs.
- **A claim nobody could refute.** The AIR frontend refused to translate calls to functions defined in the
  module itself, and a comment gave the reason: system shaders do not need it, Apple's inliner flattens
  everything. Not true -- six entry points do it, five of them compute kernels. It survived not because nobody
  looked but because **there was nothing to check it with**: until `air_run` could dispatch compute kernels,
  nobody could run those five. A false claim covered by a missing capability outlives all the others: no
  control, sum or code reading catches it -- only the arrival of the instrument that was missing.
- **A correction nobody re-checks.** Accuracy is more often lost not in the first claim but in the one that
  fixes it: a correction comes with a feeling of diligence and so is not read with the same eyes. Here, after
  dropping "a threshold of two textures", the author put in its place "among forty there are surely some that
  read two" -- a guess, in the same paragraph that apologised for reasoning instead of a number, while the
  numbers were already in a log he had taught to print them. There were eight with two textures and two with
  three, and **all ten were dispatched before the fix too**, so there was no threshold at all. Check a
  corrected claim the way you checked the original, against the same data.
- **An instrument whose error has a direction.** `grep` for a global's name in LLVM bitcode finds **zero**
  almost always: bitcode is packed by bits, and the name matches only when it happens to land on byte
  boundaries. So any corpus count made with such a grep can **only undercount** -- and its zero is
  indistinguishable from an honest one. That is how "two modules declare a `constexpr sampler`" got into the
  notes where there were ten. Knowing the direction helps twice: a zero from an undercounting instrument means
  nothing, and a **non-zero** answer is a lower bound, which is already a fact. Before counting with
  something, ask which way it errs.
- **Write it for whoever will check it.** The cheapest of all: rephrase the claim for the person who will
  re-check it. A control costs a run, a rephrasing costs nothing, and it catches the same things: "checked on
  `main`" fell apart into "one file of two checked" the minute the file had to be handed to another session.
  The habit is not free, though: it needs someone **to** hand it to. Alone you have to imagine the checker,
  and that works worst exactly when you are sure you are right -- so this is more about how the work is
  organised than about self-discipline.
