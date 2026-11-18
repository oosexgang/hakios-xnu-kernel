# What the patches change in Apple's files

Everything HakiOS *adds* is in `kernel/overlay/`. These seven patches are the changes to files Apple
ships, grouped by subsystem. `series` assigns every file to one patch; `kernel/export.sh` regenerates
the patches from an assembled tree, so they are never edited by hand. The reason for each change is in
a comment beside it in the code. This page is the map.

## 0001-build: what is compiled, and how

- Building on Linux with upstream clang and the cctools linker instead of Xcode: `Makefile`, `makedefs/`,
  `SETUP/`. `kextsymboltool` accepts the DriverKit dispatch symbols this kernel does not have.
- `config/MASTER` and the `conf/files` lists: the files of `kernel/overlay/` are added here, and the
  board is a generic one (`BCM2837` is the open configuration the port reuses).
- Signed data pointers are left plain (`libkern/os/base.h`, `XNU_PTRAUTH_DATA_PTRS_PLAIN`): signing them
  at load time needs the keys, which only dyld or Apple's kernelcache generator has.
- The firehose and pthread kernel libraries are linked from the build's own SDK; objects made outside
  the tree can be linked in (`HAKIOS_OBJECTS`).
- An xnu-internal DriverKit test with no published `iig` output is left out.

## 0002-platform-arm64: a generic arm64 machine

- The boot protocol of the HakiOS loader: boot-args layout (`boot.h`, 608 bytes), a 64-byte entropy seed
  (`prng_random.c`), bring-up checkpoints and the return to the loader instead of powering off.
- CPUs without pointer authentication (ARMv8.2: Cortex-A78/A55 and older): PAC register accesses are
  guarded by `HAS_STD_PAC`; arm64e code runs as arm64 (see `bsd/dev/arm/kern_machdep.c` in 0006 and
  `osfmk/arm64/a64emu.c` in the overlay).
- Standard ARMv8.3 PAC, where a CPU has it: keys handed over by the loader and switched by the kernel.
- Secondary cores started through PSCI. The slower cores are known from the device tree (`small-core`,
  `l2-access-penalty`); pairing them with faster ones for the scheduler is prepared but does nothing on
  this release, whose scheduler lost the fields it used (`ml_pair_slow_processors`).
- Interrupts through a GICv3 (`pe_fiq.c`), a performance-counter unit with no counters (`kpc.c`).
- Memory: bootstrap tables for up to 8 GB, DMA memory outside the kernel's window mapped non-cacheable,
  no `DC ZVA` on mappings the Qualcomm hypervisor treats as device memory.
- The console: `/dev/console` is every daemon's standard error and is copied to the loader's log; the
  text console does not scroll by reading the framebuffer back.

## 0003-iokit: IOKit without Apple's platform drivers

- The system never sleeps (`IOPMrootDomain`): there is nothing to sleep into or wake from.
- `IOMedia` publication is a no-op without IOStorageFamily; a second RAM image becomes `/dev/md1`.
- The platform UUID is made once the crypto stand-in has registered.
- Boot-arg `regtrace=1`: the registry properties user space asks for and does not find.

## 0004-mach: scheduling, IPC, panics

- Slower cores as "secondaries" (`sched_slow_secondaries`): a thread moves to a faster core as soon as
  one goes idle. Inactive while the pairing in 0002 does nothing.
- Work-interval calls of Darwin's CoreAudio answered instead of refused (50 log lines a second otherwise).
- IPC: boot-arg `ipc_legacy=1` for an older user space; the sandbox message-filter query answers
  "nothing to check" (Darwin 26's libxpc drops every client otherwise).
- No trust-cache or Image4 runtime: the AMFI and Sandbox stand-ins register instead
  (`bsd/kern/kern_minimac.c` in the overlay).
- Panics, fatal user exceptions and unrecoverable `brk` traps are written to the loader's log.
- No kext and no dext is ever loaded (`libkern/c++/OSKext.cpp`): backtraces that name the kexts in them
  (trap telemetry) work with no kext summaries, and there is no IOKit daemon to wait for, so the
  registry becomes quiet once matching is done.

## 0005-vm: the shared region and the dyld pager

- The shared region is 7 GiB: Darwin 27's dyld cache with its maximum slide does not fit in 6.
- The dyld pager and shared-region slide apply arm64e fixups without signing: on a CPU without PAC an
  authenticated pointer stays a plain one.
- A cryptex image mounted as its own volume inside Preboot counts as part of it.

## 0006-bsd-process: exec, signing, and what newer Darwin asks of the kernel

- Exec: arm64e binaries accepted on arm64 CPUs; libraries inserted into programs by name
  (`kern.exec_inject`, 4096 bytes); boot-arg `exec_swap=NAME:PATH` runs another file in a program's place.
- Privileges an entitlement would grant are answered by the AMFI stand-in.
- Memory pressure thresholds counted from the memory there is, not from a window half taken by the RAM
  disk.
- What Darwin 26 and 27 need that xnu-12377 lacks or does differently:
  - `rd=md0` hidden from `kern.bootargs` (launchd takes it for the restore environment);
  - `security.mac.lockdown_mode_state_public`;
  - the longer `posix_spawnattr` of Darwin 27.
- Diagnostics: `systrace=`, `trace_sysctl=1`, and who killed a process and why, in the loader's log.

## 0007-vfs: file systems

- The root on a RAM disk of 4 GB or more (`memdev.c`), read in place by the APFS implementation.
- `apfs` and `ramfs` registered (`vfs_conf.c`); `/dev/console` readable by every daemon.
- `ATTR_CMN_FULLPATH` listed as valid on every volume: without it Darwin 27 registers no application.
- Process 1 may bind-mount without an entitlement.
- The type name statfs, getfsstat and getattrlist report asked of ramfs first (`vfs_subr.c`): `/private/var` is
  "apfs" to fileproviderd, for "On My iPhone" (`var_apfs=`, fs/ramfs/ramfs.c).
- Diagnostics: `trace_open=NAME,...` and `trace_attr=NAME` log the paths and attributes a process asks
  for.
