# Kernel

Apple's open-source XNU, built for Qualcomm phones: plain arm64 (no pointer authentication), one kernel
for every supported board. The board is identified at boot from `/chosen/darwin-board`.

## How the sources are kept

| Path | What it is |
| --- | --- |
| `UPSTREAM` | The Apple release the kernel is made from (URL, tag, commit). |
| `overlay/` | The files HakiOS adds, as ordinary source files at their place in the XNU tree. |
| `patches/` | The changes to Apple's own files, one patch per topic. `patches/series` says which file belongs to which patch; [patches/README.md](patches/README.md) says what each patch is for. |
| `assemble.sh` | Makes the tree that is compiled: the release + `overlay/` + `patches/`. |
| `export.sh` | Takes work done in a tree back into `overlay/` and `patches/`. |
| `verify.sh` | Checks that assembling and exporting give back exactly what is committed. |
| `build.sh`, `env.sh` | The build on Linux. |
| `host/`, `iig-impl/` | Build wrappers: clang and stand-ins for Apple's closed `iig`. |

Where things are in `overlay/`:

| Path | Contents |
| --- | --- |
| `bsd/dev/` | Device drivers: display and GPU, touch, buttons, battery, audio, camera, Wi-Fi, storage, CPU frequency, backlight, thermal. Files named `hakios_*` are device-independent; `redwood_*` is the historical prefix of the platform layer (the first phone), not a statement about which phone the code is for. |
| `iokit/Kernel/` | Stand-ins for Apple's closed kernel extensions: IOMobileFramebuffer, IOSurface, AppleKeyStore, the GPU service, the battery, the platform expert. |
| `bsd/miscfs/apfs/`, `ramfs/` | A read/write APFS implementation and the RAM file system the root lives in. |
| `osfmk/arm64/` | Platform support: PSCI and CPU bring-up, GICv3, return to the loader, profiling. |
| `libkern/amfi/`, `osfmk/minicrypto/` | Code-signing policy and crypto stand-ins. |

## Building

```sh
kernel/build.sh             # or: kernel/build.sh -j12
```

The first run makes the tree next to the repository (`../xnu-12377-hakios`), then the build
dependencies, then the kernel: `BUILD/obj/mach.development.bcm2837` in the tree. Later runs bring the
tree up to date with the repository and recompile only what changed. Requirements and details:
[docs/building.md](../docs/building.md).

## Working on the kernel

Either edit in the repository:

```sh
$EDITOR kernel/overlay/bsd/dev/hakios_thermal.c
kernel/build.sh                                  # updates the tree, then builds
```

or edit in the tree, which is needed for Apple's own files, and take the work back:

```sh
$EDITOR ../xnu-12377-hakios/bsd/kern/kern_exec.c
kernel/build.sh
kernel/export.sh ../xnu-12377-hakios             # the repository's overlay/ and patches/ now have it
git diff                                         # review, commit
```

- `assemble.sh --update` (which `build.sh` runs) refuses to overwrite work in the tree that has not been
  exported. `--discard` throws that work away.
- A changed file of Apple's that no line of `patches/series` claims stops the export. Add the file to the
  topic it belongs to.
- To take over a `.patch` file written against the tree, apply it there with `git apply`, then export.
- New source files also need a line in the tree's `bsd/conf/files` (or `iokit/conf/files`,
  `osfmk/conf/files.arm64`), like their neighbours.

## Checks

```sh
kernel/verify.sh            # the release assembles and exports back unchanged
tests/run.sh                # host tests of drivers, also under qemu-aarch64
```

`verify.sh --from DIR` takes the release from a clone on this machine instead of the network.
