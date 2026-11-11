# Moving work from the old layout

For people with a branch, a patch or a habit from before the restructuring. Nothing in the kernel's
behaviour on redwood changed, and one thing changed on Renoir (below).

## Where things went

| Before | Now |
| --- | --- |
| `kernel/xnu-12377.121.6-hakios.patch`, `kernel/apply.sh` | `kernel/overlay/` (files HakiOS adds), `kernel/patches/` (changes to Apple's files), `kernel/assemble.sh` |
| `renoir/kernel/patches/00NN-*.patch` | Folded into the same two places. No patch series any more. |
| `renoir/kernel/build.sh`, `xnu-env.sh` | `kernel/build.sh`, `kernel/env.sh` |
| `../xnu-12377-renoir` (the tree) | `../xnu-12377-hakios`, made by `kernel/assemble.sh`. A tree of the old kind is refused: move it away. |
| `renoir/userland/`, `renoir/tools/` | `ios/userland/`, `ios/tools/` |
| `renoir/docs/` | `docs/` |
| `renoir/tests/` | `tests/` |
| `base/xnuloader/` | `loader/` (a link stays in `base/`) |
| `base/xnu-deps/` | `kernel/deps/` (a link stays in `base/`) |
| Board checks in drivers (`redwood_board_is("renoir")`) | `devices/NAME/board.toml` and `hakios_board_has()` for whether a driver starts; the name check remains where two phones really differ. |
| Default boot arguments in `boot26.py` | `devices/renoir/board.toml`, `[boot]` |

## Carrying a patch over

A patch against the kernel tree still applies to the tree: its paths are the same. One command puts
its content where it belongs:

```sh
kernel/import-patch.sh my-0026-something.patch      # several patches: in order
git status                                          # new files under kernel/overlay, changed topic patches
```

It assembles a tree (or updates yours), applies the patch there and exports. If the patch does not
apply, it says so and exports nothing.

## Rebasing a branch

Git follows the renames for files that moved whole. What it cannot follow:

- **A change to the monolithic patch or to a `renoir/kernel/patches/` file.** Turn your change into a
  patch against the tree (`git diff` in your old tree), then `kernel/import-patch.sh`.
- **A new `00NN` patch.** The same.

## Working on the kernel now

Edit a driver in `kernel/overlay/` and run `kernel/build.sh`, or edit in the tree and run
`kernel/export.sh ../xnu-12377-hakios`. The second is the only way for Apple's own files.
[kernel/README.md](../kernel/README.md) has both.

## What behaves differently

- **A driver wired for one phone's parts needs that part in the board file.** `haptic=`, `torch=`,
  `cam=`, `speaker_probe=`, `sound=`, `mic=`, `wifi=` and `touchscreen=` now also need the matching
  feature in `features`. redwood lists all of its parts. **Renoir lists only its touch controller**:
  the speaker, microphone, haptic, flashlight, camera and Wi-Fi drivers in the tree are redwood's
  wiring and have no Renoir code path. Asked for on Renoir, they print which line is missing instead
  of writing to redwood's pins. When you port one of them to Renoir, add its feature to
  `hakios_board.h` and to `devices/renoir/board.toml`.
- **`kernel/build.sh` no longer needs a darwin-renoir checkout** for the host tools. It still uses one
  if it is there.
- **The loader's boards and the kernel's boards come from the same files.** Do not edit
  `loader/platforms.h` or `hakios_board_table.h`: change `devices/` or `soc/` and run
  `tools/boards.py gen`.
- **`make check` runs in CI on every push.** A stale generated file or a hand-edited patch fails it.
