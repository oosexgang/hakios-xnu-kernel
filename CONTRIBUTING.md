# Contributing

## Before you send anything

```sh
make check
```

It must pass. It runs the lint, validates the board files, checks that generated files are current and
runs the host tests. CI runs the same, plus `kernel/verify.sh`.

## Where a change goes

| The change | Goes in | Then |
| --- | --- | --- |
| A new phone, or a value of an existing one | `devices/NAME/board.toml`, `soc/SOC.toml` | `tools/boards.py gen` |
| A driver, or anything else HakiOS adds to the kernel | `kernel/overlay/` | — |
| A change to one of Apple's files | An assembled tree, then `kernel/export.sh` | Never edit `kernel/patches/*.patch` by hand: `kernel/verify.sh` rejects it. |
| A boot argument | A comment naming it as `name=…` beside its read | `make docs` |
| The loader | `loader/loader.c` | `loader/build.sh`; boards come from the board files, not from code. |
| A library inserted into an iOS process | `ios/userland/` | — |

[docs/architecture.md](docs/architecture.md) explains the split; [kernel/README.md](kernel/README.md)
has the two ways of working on the kernel.

## Rules that keep the project portable

- **No device knowledge in device-independent code.** An address, a pin or a range that differs
  between phones belongs in the board or SoC file, and reaches the code through `hakios_board()`.
- **A driver for one part checks its feature.** After its boot argument, before it touches anything:
  `if (!hakios_board_has(HAKIOS_HAS_…))`, with a message that names the missing line.
- **Off by default, read before write.** A new driver does nothing without its boot argument, and its
  first level only reads and logs.
- **Check what you replace.** A library that swaps a method of iOS checks the method's type encoding
  first and leaves it alone, saying so, when it is not what it expects.
- **Test what can run without the phone.** Arithmetic and state machines go in a header the host
  tests compile; a driver's register traffic can be tested against a model (`tests/`).
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
- Regenerate before committing: `tools/boards.py gen`, `make docs`, `kernel/export.sh` if you worked
  in a tree.

## Style

C in the kernel follows the files around it: tabs, the comment above the code explains *why*, a
driver starts with a comment that says what the hardware is and where each number comes from. Shell
scripts in `kernel/`, `tests/` and `tools/` pass `shellcheck`. Python needs nothing outside the
standard library.
