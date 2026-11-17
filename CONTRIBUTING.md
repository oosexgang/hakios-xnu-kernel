# Contributing

## Before you send anything

```sh
make check
```

It must pass. It runs the lint, validates the board and SoC files, checks that `Documentation/boot-args.md` is
current and runs the host tests. While you work, `scripts/tests-for.py --changed` says which of them hold the files
you changed, each with the command that runs it alone (`--run` runs them) (the parts and their tests are in `MAINTAINERS`; a new file
goes into its part there, and `make check` says so when one does not). CI runs the same, plus `kernel/verify.sh` and `kernel/syntax-check.sh`. After
a change to a driver, `kernel/syntax-check.sh --tree DIR` says in seconds whether it still compiles,
without the kernel toolchain; before you send a change to the kernel, `make kernel-check` compiles
every file and links the whole kernel with LLVM's tools in a few minutes, and lists any warning in
HakiOS' files. There are none now: keep it so.

## Where a change goes

| The change | Goes in | Then |
| --- | --- | --- |
| A new phone, or a value of an existing one | Its device package on the `devices` branch: `NAME/board.dts`, with its README and notes. Not `arch/arm64/boot/dts/NAME` on the main branch, which git ignores there. | `tools/boards.py check`, with the package in `arch/arm64/boot/dts/` of a checkout of the main branch (`tools/device.py add NAME --from DIR` copies it there from a directory and checks it) |
| A new SoC, or a value of an existing one | `NAME/soc.dtsi` of each package with that chip | `tools/boards.py check` |
| A driver, or anything else HakiOS adds to the kernel | `kernel/overlay/` | — |
| A change to one of Apple's files | An assembled tree, then `kernel/export.sh` | Never edit `kernel/patches/*.patch` by hand: `kernel/verify.sh` rejects it. |
| A boot argument | A comment naming it as `name=…` beside its read | `make docs` |
| The loader | `arch/arm64/boot/loader.c`, `arch/arm64/boot/src/` | `arch/arm64/boot/build.sh`; boards come from the board files, not from code. |
| A library inserted into a Darwin process | `darwin/userland/` | — |

[Documentation/architecture.md](Documentation/architecture.md) explains the split; [kernel/README.md](kernel/README.md)
has the two ways of working on the kernel. If you have a branch or a patch from before the
restructuring: [Documentation/migrating.md](Documentation/migrating.md).

## Rules that keep the project portable

- **No device knowledge in device-independent code.** An address, a pin or a range that differs
  between phones belongs in the board or SoC file, and reaches the code through `hakios_board()`.
- **A driver for one part checks its feature.** After its boot argument, before it touches anything:
  `if (!hakios_board_has(HAKIOS_HAS_…))`, with a message that names the missing line.
- **Off by default, read before write.** A new driver does nothing without its boot argument, and its
  first level only reads and logs.
- **Check what you replace.** A library that swaps a method of Darwin checks the method's type encoding
  first and leaves it alone, saying so, when it is not what it expects.
- **Test what can run without the phone.** Arithmetic and state machines go in a header the host
  tests compile; a driver's register traffic can be tested against a model (`workbench/`).
- **What two drivers need is written once.** Registers, waits, the log, the SoC's pins, the PMICs and
  RPMh have a header each in `drivers/board/`, with a test.
  [Documentation/writing-a-driver.md](Documentation/writing-a-driver.md) lists them, and has an example driver to
  start from.
- **Say what was verified.** "Compiles", "passes the host tests" and "seen working on the phone" are
  three different statements. Commit messages and documents say which one applies.

## What must never be committed

`tools/check-publishable.sh` (part of `make check`) looks for these, but the rule is yours to keep:

- Apple's files: anything from an IPSW, SDK headers, shaders, the kernel itself.
- Firmware and binaries of the phone's maker: DSP, GPU, Wi-Fi, touch firmware, device trees, UEFI drivers.
- Built output, home-directory paths, keys, other people's e-mail addresses.

A value read from a vendor's device tree or driver (an address, a range) is a fact and is fine, with
its source named in a comment.

## Commits

- One change per commit, in the imperative, with a body that says what changed and why.
- Name the paths. Say how it was verified, and what was not.
- Regenerate before committing: `make docs`, `kernel/export.sh` if you worked in a tree. The board
  tables are not committed: the builds make them from the packages that are present.

## Style

C in the kernel follows the files around it: tabs, the comment above the code explains *why*, a
driver starts with a comment that says what the hardware is and where each number comes from (`.clang-format` is
that style for an editor: a start for new files, and for the lines a change touches). Shell
scripts in `kernel/`, `workbench/` and `tools/` pass `shellcheck`. Python needs nothing outside the
standard library.

## Reports

A run on a phone, good or bad, has its form ("A run on a phone": what `tools/logcheck.py` said, what was seen);
something that does not work, "Something does not work". A security problem is reported privately:
[SECURITY.md](SECURITY.md).
