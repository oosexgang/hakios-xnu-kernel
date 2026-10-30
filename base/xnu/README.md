# Kernel

Our changes to XNU `xnu-8796.121.2` (the iOS 16.5 kernel, Darwin 22.5.0):

- `redwood.patch` -- changes to Apple's files;
- `files/` -- new files, at their paths in the XNU tree;
- `apply.sh` -- takes Apple's sources and applies all of this; `export.sh` -- the reverse.

It builds on Linux with stock clang, without macOS or Xcode.

## Building

You need:

- packages (Arch): `sudo pacman -S --needed base-devel git curl clang lld python perl xz`
  (Debian/Ubuntu: `build-essential git curl clang lld python3 perl xz-utils`);
- [theos](https://theos.dev/docs/installation-linux) in `~/theos` (or `$THEOS`): it provides cctools
  (`toolchain/linux/iphone/bin`: the ld64 linker, nm, otool, strip...) and `sdks/iPhoneOS16.5.sdk`.

```sh
xnu/apply.sh             # ~6 s: tag xnu-8796.121.2 of apple-oss-distributions/xnu + our changes
                         #       -> xnu-src/xnu-8796.121.2, branch redwood, changes uncommitted
xnu-deps/setup.sh        # ~30 s: dependency sources, mig/unifdef/tcsh, the SDK overlay, two libraries
tools/mk-ath11k.sh       # optional, once: the phone's Wi-Fi driver (fetches Linux and FreeBSD sources)
tools/mk-wlfw.sh         #   ... and its firmware-service message tables
./xnu-build.sh -j8 -k    # ~45 s on 12 threads
```

Result: `xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`.
`./xl run` and `qemu/run-ios.sh` take the kernel from there by themselves (and from `prebuilt/` only when it is not there).
`common/build.sh <device>` runs all of these steps.

Checked: in an empty directory holding only the repository, these commands produced a kernel of the same
size and the same set of symbols as the working one. Disk: ~600 MB, not counting theos.

### On macOS

The same commands work on a Mac with Xcode (tested: macOS 26.6, Xcode 27.1, Apple M5), without theos: the
cctools, `mig` and the iOS SDK come from Xcode, `unifdef` and `csh` from the system (`xnu-env.sh` sets
`XNU_HOST_DARWIN`, and `xnu-deps/setup.sh` then takes those instead of fetching and building them). QEMU
comes from Homebrew (`brew install qemu`), and `qemu/run.sh` finds its UEFI firmware there.

```sh
xnu/apply.sh && xnu-deps/setup.sh && ./xnu-build.sh -j"$(sysctl -n hw.ncpu)" -k
source xnu-env.sh && cd userland && xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding \
  -fno-stack-protector -fno-builtin -Wl,-e,_start -Wl,-pagezero_size,0x100000000 -o hello src/hello.c && cd ..
xnuloader/mkpayload.py -o qemu/payload.bin \
  kernel=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 ramdisk=userland/hello
qemu/run.sh 90          # "Hello from user space on Darwin! I am process 1", then the expected panic
```

Things to know:

- **`-k` is required.** The build errors are harmless and every one of them happens after the kernel is
  linked: the export check and three symbolsets (`config/Makefile:121` and `:108`). All 217 complaints
  `exported name not in import list` are about DriverKit RPC names from the `iig` stubs (see below). make
  exits with status 2; the kernel is ready by then -- look at the file's date, not at make's exit status.
- The path to the repository must not contain spaces (XNU's makefiles do not survive them).
- The wrapper `xnu-deps/host/bin/xnu-clang` calls `/usr/bin/clang`. Tested with clang 22.1.
- After changing kernel headers that two outside libraries see (the pthread kext, firehose):
  `xnu-deps/setup.sh --rebuild-libs`.
- Apple's DriverKit generator (`iig`) is closed source. In its place are the stubs
  `xnu-deps/iig-impl/OSObject.iig.cpp`, generated from the linker's list of undefined symbols. If that set
  changes: `xnu-deps/gen-iig-stubs.py xnu-deps/iig-impl/undefined-all.txt xnu-deps/iig-impl qemu/kernel.syms`,
  then `rm -rf BUILD`.
- For `qemu/sym.py` after a rebuild: `nm -n .../mach.development.bcm2837.unstripped > qemu/kernel.syms`.

## Working on the tree

Changes are made directly in `xnu-src/xnu-8796.121.2` (an ordinary git tree of Apple's, on branch `redwood`).
Before committing to this repository run `xnu/export.sh` (it regenerates the patch and `files/`); to update
the ready-made kernel in `prebuilt/`, `tools/update-prebuilt.sh`.

**Two traps, each of which cost a build.**

`export.sh` regenerates the patch **from your whole tree**, not from your changes. If the tree has fallen
behind the branch -- somebody changed the kernel after you set yours up -- the export silently rolls back
their work: the patch comes out without their files. Before exporting, bring the tree to the current patch
(`git checkout -- .` in `xnu-src`, then `git apply xnu/redwood.patch` and `cp -a xnu/files/. .`); if the change
is a single one, it is more honest to paste its diff into the patch by hand and say so in the commit. The sign
of a tree that fell behind is a kernel that builds and **crashes at boot** right after `Stackshot Succeeded`.

The kernel build here **normally ends with the error** `check_all_exports` (IOKit symbols in `config/`): that is
a check of the export lists, not compilation. `mach.development.bcm2837` is linked and usable by then.
The error was there before any of our changes.

## What was added to the kernel

| Files | What |
|---|---|
| `osfmk/arm64/gic_v3_irq.c`, `generic_arm64_support.c`, `kpc.c`, `ppl/`, `tunables/` | the platform: GICv3, the timer, stubs for files Apple left out |
| `osfmk/arm64/psci_smp.c` | several processors: starting cores through PSCI (boot-arg `cpus=N`), stopping them before returning to the loader |
| `osfmk/arm64/loader_return.c` | return to the loader with the log, a heartbeat, the Qualcomm hypervisor's watchdog |
| `osfmk/arm64/user_exception_trace.c` | user-space debugging: `usertrap:`, `userabort:`, thread stacks (`ustacks`) |
| `osfmk/minicrypto/` | a replacement for corecrypto: MD5/SHA-1/SHA-2, HMAC, DRBG, AES (Apple's corecrypto sources are not used) |
| `iokit/Kernel/GenericARMPE.cpp` | the platform expert and the CPU |
| `iokit/Kernel/MiniIOSurface.*`, `MiniIOMobileFramebuffer.cpp`, `MiniAppleKeyStore.cpp` | replacements for closed drivers: surfaces, the display (scaled to the panel, with screen recording), the key store |
| `bsd/miscfs/apfs/`, `bsd/miscfs/ramfs/` | our file systems: APFS (the root on flash and the images in memory) and a writable file system in memory |
| `bsd/kern/kern_minimac.c`, `libkern/amfi/` | AMFI and Sandbox stand-ins (MAC policies), entitlements from the program's signature |
| `bsd/dev/redwood_input.c`, `redwood_goodix.c`, `redwood_virtio_input.c` | input: `/dev/redwood-touch`, the Goodix GT9916 driver (SPI bit-banged over GPIO), virtio-input for QEMU |
| `bsd/dev/redwood_cpufreq.c` | CPU frequency (Qualcomm EPSS) |
| `bsd/dev/redwood_net.c`, `bsd/sys/redwood_net.h`, `redwood_virtio_net.c` | the network interface `en0` on the kernel's network KPI (without IONetworkingFamily) and the boundary between it and whatever carries the frames (a "link": three functions in `redwood_net.h`); the first link is virtio-net for QEMU |
| `bsd/dev/redwood_adreno.c`, `iokit/Kernel/RedwoodGPU.cpp` | the phone's GPU (Adreno 642L): step-by-step bring-up (`adreno=N`: power, GMU, the zap shader, microcode), buffers and the command queue; an IOKit client with a subset of msm DRM for Mesa |
| `bsd/dev/redwood_wifi.c`, `bsd/sys/redwood_qrtr.h` | the phone's Wi-Fi (WCN6750): Qualcomm shared memory (SMEM), the signalling bits between us and the Wi-Fi processor (SMP2P), the power processor's mailbox, loading the Wi-Fi processor's firmware, starting it through TrustZone, GLINK and QRTR on top of it; `redwood_qrtr.h` is a datagram port for the rest of the kernel |
| `bsd/dev/redwood_qmi.c`, `bsd/sys/redwood_qmi.h` | QMI, the message protocol of the services on Qualcomm processors: table-driven encoding, requests and replies, indications; the interface is Linux's (`qmi_*`) so that ath11k's `qmi.c` builds unchanged; tested on the computer (`tools/qmi-test/run.sh`) |
| `bsd/dev/redwood_wlfw.c` | the conversation with the Wi-Fi firmware service; the message descriptions are taken from the ath11k sources at build time (`tools/mk-wlfw.sh`) and are not in git |
| `bsd/dev/redwood_audio.c`, `redwood_virtio_sound.c`, `redwood_speaker.c` | sound: `/dev/redwood-audio` (where our output device in mediaserverd writes what iOS plays), recording it to memory, virtio-sound for QEMU; a first look at the phone's speaker amplifiers |
| `bsd/pthread/pthread_kext_static.c` | the pthread kext, linked into the kernel |

Besides the hooks for all of this, the patch fixes what stops the open XNU from building and running outside
Apple: makefiles for Linux, the `BCM2837` configuration (taken as the base), bugs in abandoned code (mockfs,
the non-PPL pmap, percpu on a single processor) and so on.

License: a derivative work of XNU, APSL 2.0.
