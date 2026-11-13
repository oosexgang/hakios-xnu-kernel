# arm64e on non-Apple ARMv9: plan

Goal: run arm64e user space (Darwin 18 and later ship nothing else) on CPUs with the standard ARMv8.3
pointer authentication, first in QEMU (`QEMU_CPU=cortex-a710`), later on Snapdragon 8 Gen 1 and
newer. Work happens on the kernel of this repository because it links no closed Apple kext:
switching it to arm64e touches only code we build.

## Where it stands

| Milestone | State |
| --- | --- |
| 0. Kernel built on macOS, `hello` (arm64) as process 1 in QEMU | done |
| 1. arm64e kernel with a standard-PAC key backend; `hello` built `-arch arm64e` as process 1 | done: boots to `Hello from user space on Darwin! I am process 1` under `QEMU_CPU=cortex-a710`; a bent signed return address is killed at the `retab` (`userland/src/pac_test.c`) |
| 2. A test program exercising return-address signing, signed function pointers, `pacga`, signal delivery and `thread_set_state` | done: `userland/src/pac_test.c` (return addresses), `userland/src/pac_fn.c` (signed function pointers + `pacga`); signal delivery and `thread_set_state` are exercised for real by the booting Darwin daemons (milestone 3), not a standalone program |
| 3. Darwin 16.5 arm64e user space (iPhone XS image, A12): dyld shared cache, launchd, SpringBoard | done: under `QEMU_CPU=cortex-a710` the arm64e kernel boots the iPhone XS cache; dyld, launchd and ~24 daemons (backboardd, runningboardd, powerd, wifid, containermanagerd, ...) spawn, and SpringBoard reaches `job state = running`, with no PAC fault or panic (see "What milestone 3 took") |
| 4. Port the backend to xnu-12377 (Darwin 26) | **kernel done: the arm64e xnu-12377 (Darwin 26) kernel boots `hello` as process 1** under `QEMU_CPU=cortex-a710` (`Hello from user space on Darwin! I am process 1`), with no PAC fault during the run. The whole hakios kernel is rebased onto xnu-12377, `mach.development.bcm2837` links as an arm64e Mach-O with the HAS_STD_PAC key block (4a), and it boots fault-by-fault to PID 1 (4b; see below). The 17 deferred userland/fs feature rejects are now also resolved and the kernel still builds (4c), so the kernel is ready for the full Darwin 26 user space. Remaining: re-acquire the Darwin 26 user-space data (root fs + arm64e dyld cache -- a previous session's extract is no longer on disk) and assemble/boot it as in milestone 3. |
| 5. arm64e on real ARMv8.3+ silicon (Snapdragon 8 Gen 1+ / 8 Elite) | not started; well scoped. Real-hardware boot already works in **arm64** on the POCO X5 Pro 5G (778G, no PAC); arm64e+standard-PAC has only run in QEMU `cortex-a710`. Milestone 5 merges the two on a PAC-capable, bootloader-unlockable device via the existing porting flow. |

## What milestone 1 took

Done and checked in QEMU (`QEMU_CPU=cortex-a710`, macOS host): the arm64e kernel boots, runs
`hello` built `-arch arm64e` as process 1, and the corrupt-return test dies at its `retab`.

- **The linker writes signed pointers into an arm64e kernel, and only the loader can apply them.**
  `-arch arm64e` makes ld64 encode every `__ptrauth`-carrying pointer -- C++ vtables, the syscall
  and startup tables, and the fields XNU declares with `XNU_PTRAUTH_SIGNED_PTR` -- as chained
  "auth" fixups: 28k signed and 35k plain rebases in this kernel. Apple's kernelcache generator
  does that on the device, with the keys; our loader did not, so the kernel ran on encoded
  pointers. `xnuloader/loader.c` now makes five random keys per boot, programs them into the CPU,
  sets `SCTLR_EL1.EnIA/EnIB/EnDA/EnDB`, signs the auth chains and flattens the rebases.
- **The keys travel through the kernel image itself.** They go into `__DATA,__pac_keys` (80 bytes,
  a section of its own so the loader needs no symbols) and `PAC_INIT_KEY_STATE` loads them into
  the key registers of every CPU. Until the loader has filled the section they are zero, which is
  self-consistent but not secret.
- **A signature is only valid in the translation regime it was made in.** The loader runs with the
  firmware's `TCR_EL1`; a kernel (TTBR1) pointer signed there does not authenticate after the
  kernel installs its own `T1SZ_BOOT`/`TBI1`/`TBID1`, and the first authentication faults (FPAC)
  before a line is printed. The loader now matches those fields before signing.
- **Address-diversified discriminators are not userland blends.** With a diversity constant the
  modifier is the slot address with its top 16 bits replaced; with none it is the whole 64-bit
  address, so a kernel address keeps its `0xffff` top. Truncating to 48 bits, as a userland blend
  does, faults every such pointer.
- **The switch itself**: EL1 always runs on the kernel's keys; the return path (`locore.s`,
  `Lexception_return_restore_registers`) loads the process's A and B keys, derived from its
  `jop_pid`/`rop_pid` and the kernel's secrets (`PAC_STD_DERIVE_KEY`), before the `eret`; the
  entry path puts the kernel's back after `SPILL_REGISTERS`. `ml_enable_user_jop_key` swaps IA/DA
  for `pmap_sign_user_ptr`.

Still simplified: `XNU_PTRAUTH_SIGNED_PTR` fields are plain pointers in this tree
(`libkern/os/base.h`), so kernel data pointers are not authenticated; user pointers are signed
regardless (milestone 3).

## What milestone 3 took

The iPhone XS (A12) arm64e Darwin 16.5 is fetched and the user space is built for arm64e
(`darwin165/fetch.sh` and `userland/build.sh`, both now parameterised by device/arch; see the
hakios-xnu-userland side). Booting it on the arm64e kernel took three signing fixes, each found by
tracing the fault PC under `qemu/run-darwin.sh ... -d int` and disassembling the cache:

- **The user shared cache must be signed even for the default region.** The kernel signs the slid
  cache and dyld-linked images with the process's A key (`vm_shared_region_slide_page_v3`,
  `vm_dyld_pager`'s `signPointer`). Both skipped signing when the region key was 0 -- the default
  region launchd runs in -- because on Apple 0 means "the hardware default key". The standard-PAC
  backend derives a real key from the jop value (0 included) and the process authenticates with it,
  so a key of 0 must still be signed. launchd FPAC'd on its first cache pointer until this was fixed.
- **Don't hand-sign a pointer the tree keeps plain.** `kauth_cred_create` signs `cr_label` by hand
  under a bare `#if HAS_APPLE_PAC`, but `cr_label` is an `OS_PTRAUTH_SIGNED_PTR` field, which this
  tree leaves plain (`XNU_PTRAUTH_DATA_PTRS_PLAIN`), and the readers (`mac_cred_label()`) don't
  strip. So the field held a signed pointer the readers used raw, and freeing the MAC label handed
  it to `kvtophys` as a VA (`pmap.c` panic) during launchd's first `posix_spawn`. Gate the
  hand-signing with `!XNU_PTRAUTH_DATA_PTRS_PLAIN`.

With these, dyld, launchd and ~24 daemons start and SpringBoard runs, with no PAC fault or panic.

## What XNU expects (read in xnu-8796.121.2 and xnu-12377.121.6)

- Nothing on disk carries a signed pointer. dyld signs user pointers at load, the kernel signs the
  shared cache pages it slides (`vm_shared_region_pager.c`), and the kernel signs its own at boot.
  So the signing algorithm does not matter: QARMA on a Cortex core works as well as Apple's.
- Apple's hardware keeps separate keys for EL1 and EL0. User processes get an A-key diversifier
  (`jop_pid`, shared by the processes of one shared region) and B keys (`rop_pid`, per process).
  The kernel never switches keys on exception entry (`locore.s` has no key code at all).
- In the open source the macros that program user keys (`pac_asm.h`: `SET_JOP_KEY_REGISTERS`,
  `SET_ROP_KEY_REGISTERS`, `PAC_INIT_KEY_STATE`; `ml_enable_user_jop_key`) are empty except for the
  paravirtualized path of Apple's own VMs (`HAS_PARAVIRTUALIZED_PAC`, `hvc` calls).
- Standard ARM has one set of key registers (`APIAKey_EL1` and the rest) for EL1 and EL0 together.

## Milestone 1, in steps

1. **Configuration.** Build `BCM2837` as arm64e (`ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64e`) and
   define a new `HAS_STD_PAC` that turns on the `HAS_APPLE_PAC` code paths (cpu data `jop_key`/`rop_key`,
   thread state signing, user pointer signing) without Apple's hardware specifics. The pthread kext and
   firehose libraries are rebuilt for arm64e.
2. **Kernel keys.** Five random 128-bit keys at boot (IA, IB, DA, DB, GA), loaded before the first signed
   pointer is used (`start.s`, `PAC_INIT_KEY_STATE`), and on every secondary CPU.
3. **User keys.** Derived, not stored: `key = KDF(boot secret, rop_pid or jop_pid, key name)`, so two
   processes with the same `jop_pid` get the same A keys, as the shared region requires.
4. **Switching.** Exception entry from EL0 (`locore.s`, before any C or signed pointer): load the kernel
   keys. Return to EL0: load the current thread's user keys (`eret` synchronizes the context). Context
   switch (`cswitch.s`, `REPROGRAM_*_KEYS`): only record which user keys the next return loads.
5. **Borrowing user keys.** `ml_enable_user_jop_key` / `ml_disable_user_jop_key` swap IA and DA for the
   user's A keys and back; their callers (`pmap_sign_user_ptr`, `pmap_auth_user_ptr`) already run with
   interrupts off and make no indirect call in between.
6. **exec.** Accept `CPU_SUBTYPE_ARM64E` only when `ID_AA64ISAR1_EL1` reports address and generic
   authentication; otherwise keep refusing with EBADARCH (Snapdragon 888 and older).
7. **Check.** `hello` built `-arch arm64e` reaches its first `write` as process 1 under
   `QEMU_CPU=cortex-a710`, and a deliberately corrupted return address faults (`ESR_EC_PAC_FAIL` or the
   poisoned-pointer abort) instead of returning.

Cost: up to ten system register writes on each kernel entry and exit, as Linux pays for in-kernel
pointer authentication. Keys live in kernel memory, weaker than Apple's locked key registers; that is
accepted here.

## Milestone 4, where it stands

The target is **Darwin 26**, whose kernel is
**xnu-12377.121.6** (Apple's year-based rename of what would have been Darwin 19; it is the newest XNU
open-sourced, after xnu-11417 = Darwin 18.x).

**Done (this session):**

- **The Darwin 26 image is fetched and validated as arm64e.** iPhone 17 Pro Max (iPhone18,2, A19 Pro)
  Darwin 26.6.2 (23G90), the newest 26.x. Darwin 26 ships the root/cryptex filesystems as `.aea`
  (Apple Encrypted Archive); macOS `/usr/bin/aea` + the FCS key from Apple decrypts them. The
  complete **arm64e dyld shared cache** (84 subcaches `.00`-`.83` + `.atlas` + `.symbols`) lives in
  the `Cryptex1,SystemOS` cryptex and was extracted. Device identity for a profile: `v54ap`,
  ApChipID `0x8150`, ApBoardID `0x0E`.
- **The three milestone-3 signing fixes apply to xnu-12377.** `vm_shared_region.c`,
  `vm_dyld_pager.c`, `kern_credential.c` -- identical but for field-name churn (the `#if CONFIG_SPTM`
  non-SPTM path is ours). Present in the xnu-12377 checkout.
- **The standard-PAC board is resurrected on xnu-12377.** Darwin 26's XNU **removed `BCM2837.h`** --
  the only arm64 boards left are the real Apple SoCs (T6000/T8101/.../H13-H17) and `VMAPPLE`, and
  `VMAPPLE` is `HAS_PARAVIRTUALIZED_PAC` (hvc to Apple's hypervisor) + 16 KB/mixed pages, not the
  standard hardware PAC on a Cortex core we target. So our `BCM2837.h` (the standard-PAC, GICv3,
  PSCI-SMP, PL011/QEMU-virt board from milestone 1) is copied into xnu-12377 and registered in
  `makedefs/MakeInc.def` (`SUPPORTED_ARM64_MACHINE_CONFIGS`, `MACHINE_FLAGS_ARM64_BCM2837`) and
  `pexpert/pexpert/arm64/board_config.h`. Saved as `xnu12377-bcm2837-board.patch`.

**The real shape of milestone 4, now that it is mapped:** it is milestone 1 redone on xnu-12377,
which is **two major versions ahead of xnu-8796 and has the board removed.** The HAS_STD_PAC backend
files all still exist in xnu-12377 (`osfmk/arm64/{pac_asm.h,proc_reg.h,locore.s,start.s,cswitch.s,
machine_routines.c,pcb.c}`), so the architecture code carries over conceptually, but the 3591-line
the old kernel's patch (`xnu/` in the first phone's package) must be rebased hunk by hunk across the version gap, the `xnuloader` boot-time
signer re-pointed at xnu-12377's chained-fixup layout, and the build harness retargeted. The selected route:

- **A -- Darwin 26 now (the explicit target).** Rebase the arm64e + HAS_STD_PAC kernel onto xnu-12377
  (board done), build it (`hello` arm64e as process 1 under `QEMU_CPU=cortex-a710`, like milestone
  1), then boot the Darwin 26 arm64e user space. Large and iterative; a boot is not a one-session job.

### Route A rebase status (in progress)

Route A was chosen. The rebase of the hakios kernel (the old kernel's patch (`xnu/` in the first phone's package), against xnu-8796) onto a
checkout of xnu-12377.121.6 has a baseline: **110 of the 208 hunks apply cleanly, 98 reject** across
45 files, and all of `xnu/files/` (the port's new drivers, `pac_std_asm.h`, GenericARMPE,
miniamfi, ...) is copied in. Half the hunks landing across a two-major-version gap is better than
feared -- the PAC code regions (`osfmk/arm64/{pac_asm.h,proc_reg.h,locore.s,start.s,
machine_routines*}`) are close enough between 8796 and 12377 that most of the HAS_STD_PAC additions
applied.

**The PAC-core rejects are resolved.** The four files that actually carry the arm64e signing
backend are rebased onto xnu-12377 by hand: `proc_reg.h` (the `#endif /* HAS_STD_PAC */` that closes
the standard SCTLR_EnIA/IB/DA/DB block), `pac_asm.h` (the `#endif /* !HAS_STD_PAC */` that fences
off Apple's key macros so `pac_std_asm.h` provides them instead -- placed after `PAC_INIT_KEY_STATE`
so xnu-12377's new `PACGA_*`/`PACGA_IRG_RESEED` stay common), `base.h` (the whole
`XNU_PTRAUTH_DATA_PTRS_PLAIN` block and both `&& !XNU_PTRAUTH_DATA_PTRS_PLAIN` guards), and
`machine_routines_asm.s` (the two `#elif HAS_STD_PAC` arms of `ml_enable`/`ml_disable_user_jop_key`
that load `pac_std_keys` with `PAC_STD_LOAD_A_KEYS`/`PAC_STD_LOAD_KEYS`). `#if`/`#endif` balance
checked on all four. The BCM2837 board is re-registered (the baseline reset had reverted it).

What was listed as PAC but turned out to be the **platform/loader layer**, deferred to the next
phase: `start.s` (the loader exception vectors `Lloader_early_exception`/`loader_stub_slot`, the PSCI
secondary vectors, `LOADER_CHECKPOINT` -- only one guard line in it, `HAS_APPLE_PAC && !HAS_STD_PAC`,
is PAC, and it is entangled with that loader hunk), `arm_init.c` (only `loader_checkpoint()` calls
and `psci_smp_cpu_reached`), `machine_routines.c` (slow-core pairing, EL1 virtual-timer trap),
`cpu.c` (PSCI idle). `pmap.h`'s reject is a `reserved10`->`reserved12` rename in the `#else`
(non-PAC) branch we do not compile -- not applicable.

**The arch + loader layer is resolved; 32 userland-layer rejects remain.** Done this phase, by hand
against xnu-12377's restructured code: the build registration (`osfmk/conf/files.arm64` adds
`loader_return.c`/`psci_smp.c`/`gic_v3_irq.c`/`generic_arm64_support.c`/`user_exception_trace.c` and
comments out the Apple `amcc_rorgn` files; `bsd/conf/files` adds the port's 31 devices and
`kern_minimac.c`; `libkern/conf/files` adds `miniamfi`); `cpu.c` (PSCI idle keeps its cache);
`machine_routines.c` (`ml_choose_slow_core_partners`/`ml_pair_slow_processors` call sites -- no-ops
on homogeneous QEMU cores -- and the EL1 physical-timer trap guard; the big.LITTLE DT `small-core`
hook is deferred, as 12377 rewrote the topology parser and it does not fire on QEMU); and `start.s`
(the whole loader exception-vector block -- `Lloader_early_exception`, `loader_stub_slot`, and the
`GENERIC_PSCI_SMP` secondary vectors -- which the already-applied `LOADER_CHECKPOINT` macro and
secondary-start code reference, plus the `HAS_APPLE_PAC && !HAS_STD_PAC` guard that stops the std
build from running `PAC_INIT_KEY_STATE` twice; only the optional `LOADER_CHECKPOINT 4/5` markers were
skipped, and the Raspberry-Pi `#if defined(BCM2837)` timer-routing block that 12377 no longer has).
`#if`/`#endif` balance checked on every edited header and asm file.

The remaining 32 rejects are the **userland/platform-integration layer**: bsd/kern 8 (exec,
memorystatus, credential, socket, sys_generic...), bsd/vfs 7 and bsd/nfs 1 (the mockfs/union root and
attrlist/quota hooks), osfmk/kern 7 (the `sched_slow_secondaries` scheduler wiring, kalloc/percpu/
work_interval), security 1 (minimac MAC policy), plus `MakeInc` LD flags, `video_console`,
`vm_shared_region`, iokit `KernelConfigTables`, and the deferred `arm_init.c` boot-progress markers.
These wire the port's devices, the MAC policy and the root filesystem into bsd/osfmk -- the rest of
the bring-up.

**Every build/link-critical reject is now resolved; 18 runtime-feature/filesystem/optional rejects
remain.** Resolved this phase (the ones a compile or link would trip on): the two signing fixes that
had re-rejected (`vm_shared_region.c` v3 *and* v4 slide variants -- the `jop_key != 0` guard dropped
so the default region signs; `kern_credential.c`'s `cr_label` hand-sign gated by
`!XNU_PTRAUTH_DATA_PTRS_PLAIN`); `sched_slow_secondaries` defined + declared (`sched_prim.c`,
`sched.h`) so the `machine_routines.c` references link; `kern_work_interval_type_of_thread()` defined
(`sys_work_interval.c` calls it); the GenericARMPE platform expert row in `KernelConfigTables.cpp`
(how a non-Apple board finds its PE); and `kern_exec.c`'s `exec_applog_oslog`/`exec_applog_path`
(applied call sites reference them). A large batch turned out **already fixed upstream in xnu-12377
and was discarded as N/A**: every `uio_stackbuf_t` reject (12377 replaced the over-aligned-char
typedef with a `UIO_STACKBUF(name, niov)` macro over `uint64_t`) and `percpu.h`'s
`percpu_foreach_secondary` (12377 rewrote it with `__PERCPU_CAST`/`vm_address_t`); `disk.h`'s
`dk_memdev_info_t` and `uio_internal.h`'s typedef were removed upstream.

The 18 that remain need no symbol the build already references, so they are deferred to the boot
phase: the mockfs/union-root filesystem hooks (`bsd/vfs` x6, `bsd/nfs`), the MAC policy
(`mac_vfs_subr.c`), `kern_exec.c`'s persona/xpcproxy features (package-manager and RunningBoard
support), `kern_memorystatus.c`'s jetsam tuning, the big.LITTLE scheduler behaviour
(`sched_clutch.c`, `sched_prim.c`), and the optional `kalloc.h` (Linux-only), `video_console.c`
(framebuffer perf), `MakeInc.cmd`/`.def` (build-host tooling, LD flags) and `arm_init.c` progress
markers. Artifacts: `scratchpad/xnu12377-rebase-wip.patch` (76k lines, the whole rebase),
`-rejects.txt` (the 18), `-bcm2837-board.patch`.

### Build harness retargeted; the rebase compiles

`xnu-build.sh` builds xnu-12377 with `XNU_SRC=scratchpad/xnu12377 XNU_ARCH=arm64e MACHINE=BCM2837
RC_DARWIN_KERNEL_VERSION=12377.121.6`. Four harness fixes were needed, none touching the kernel
source: set `RC_DARWIN_KERNEL_VERSION` (the 16.5 SDK's System.kext version does not match Darwin 26);
the `iig` shim (`xnu-deps/host/bin/iig`) gained an Xcode-SDK fallback and the two new iig headers
(`IOExtensiblePaniclog.h`, `TestDextRematch.h`) were copied into the 13.3 SDK; `BCM2837.h` was added
to pexpert's `PRIVATE_KERNELFILES` so `board_config.h` can include it; and `proc_reg.h` got a
`#elif defined(BCM2837)` MMU-cache-line branch (xnu-12377 dropped the Raspberry-Pi one with the
board). With those, **the kernel source compiles** -- thousands of files, the build reaches deep
into bsd/osfmk/iokit, and the rebase itself is sound: the ~424 remaining errors are config,
driver-API drift and SDK gaps, not bad hunks. They fall in three bounded groups:

- **The CPU-performance-counter subsystem (~260): `cpc_arm64.{h,c}`, `kperf_kpc.h`,
  `kpc_common.c`, `apple_arm64_cpu.h`.** All want Apple CPMU macros (`CPMU_PMC_COUNT`, ...) that a
  non-Apple board has not. Root cause: xnu-12377 removed `config/MASTER.arm64.bcm2837`, so `doconf`
  fell back to the generic arm64 config with `config_cpu_counters` on. Fix: restore a
  `MASTER.arm64.bcm2837` for xnu-12377 (the xnu-8796 one has `kperf kpc` but no `config_cpu_counters`
  /`monotonic`), reconciled with xnu-12377's option set.
- **The port's drivers (~100): `hakios_camera.c`, `hakios_cpufreq.c`, `hakios_adreno.c`, ...**
  written against xnu-8796's `kmem_alloc` flags (`KMA_KOBJECT`/`KMA_DATA`) and other APIs that
  xnu-12377 renamed. Driver-layer porting.
- **Three framework headers newer than the 16.5 SDK: `TrustCache/API.h`,
  `CoreEntitlements/V2/API.h`, `CodeSignature/Entitlements.h`** (AMFI / trust-cache / code-signing),
  plus small fixes in our own `user_exception_trace.c` and `machine_routines.c`.

Build log + error summary: `scratchpad/xnu12377-build.log`.

**Progress: 1328 -> 122 errors.** The CPU-performance-counter cluster (~260) is gone: xnu-12377
dropped `config/MASTER.arm64.bcm2837`, so `doconf` fell back to the generic arm64 config with
`config_cpu_counters` on; a restored `MASTER.arm64.bcm2837` (a copy of `MASTER.arm64` with
`CPU_COUNTERS_BASE` emptied -- no Apple CPMU on a Cortex core) turns it off and drops `cpc_arm64`,
`kperf_kpc`, `kpc_thread` and `cpc.c` from the build. `TrustCache/API.h` (43) is resolved by copying
`EXTERNAL_HEADERS/TrustCache/` forward from xnu-8796 (xnu-12377 made it an external framework; the
older tree still ships it). Two of our own files are fixed: `machine_routines.c`'s slow-core pairing
is now a no-op (xnu-12377's scheduler refactor removed `processor_primary`/`processor_set_primary`,
and big.LITTLE pairing is moot on QEMU's homogeneous cores), and `user_exception_trace.c` includes
xnu-12377's new `vm/vm_map_xnu.h` and `vm/vm_shared_region_xnu.h` (the kernel-internal VM API split
out of the public headers).

The 122 that remain are three bounded jobs:

- **The port's drivers (~90): `hakios_camera.c`, `hakios_cpufreq.c`, `hakios_adreno.c`,
  `hakios_virtio_*`, ... and `kern_minimac.c`.** Written against xnu-8796's `kmem_alloc` flags
  (`KMA_KOBJECT`/`KMA_DATA`) and other APIs xnu-12377 renamed or moved. Systematic driver-layer port.
- **The AMFI / CoreEntitlements subsystem (~11): `CoreEntitlements/V2/API.h` (pulled by
  `libkern/amfi/amfi.h`) and `CodeSignature/Entitlements.h` (by `kern_exec.c`, `necp_client.c`).**
  Darwin 26 reorganized CoreEntitlements under `V2/` and added the CodeSignature framework; xnu-8796's
  `EXTERNAL_HEADERS/CoreEntitlements` has the old flat layout and no CodeSignature. Needs the V2
  headers forward-ported or `amfi.h`/the miniamfi replacement reworked to the old API.
- **`apple_arm64_cpu.h` (10): `MPIDR_CORETYPE_*` undeclared** -- an Apple-CPU header pulled into the
  BCM2837 build; guard it for non-Apple or define the macros.

Build log: `scratchpad/xnu12377-build.log`.

### Milestone 4a reached: the arm64e xnu-12377 (Darwin 26) kernel builds

**The kernel compiles and links.** `BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`
(21 MB stripped; `.unstripped` 25 MB) is a `Mach-O 64-bit executable arm64e` with `_arm_init`,
`_kernel_bootstrap` and `_pac_std_keys`, and a `__pac_keys` section of 0x50 bytes (the five
standard-PAC keys) -- the HAS_STD_PAC backend is in the kernel. This is the Darwin-26 counterpart of
milestone 1's "arm64e kernel builds". Full rebase+build diff: `scratchpad/xnu12377-rebase-full.patch`
(~78k lines). Build it with:

```
XNU_SRC=<xnu-12377 tree> XNU_ARCH=arm64e MACHINE=BCM2837 RC_DARWIN_KERNEL_VERSION=12377.121.6 \
  ./xnu-build.sh -j10 -k
```

The last stretch from the driver/framework layer took, beyond the earlier fixes: the port's drivers
and `ramfs.c`/`kern_minimac.c`/`loader_return.c`/`user_exception_trace.c` include xnu-12377's split
internal VM headers (`vm/vm_kern_xnu.h`, `vm/vm_map_xnu.h`, `vm/vm_shared_region_xnu.h`,
`vm/vm_compressor_xnu.h`); `vm_map_switch` became `vm_map_switch_to`/`vm_map_switch_back` (an opaque
context); the three `CoreEntitlements/V2/*` and `CodeSignature/Entitlements.h` headers Darwin 26 made
closed are provided as thin EXTERNAL_HEADERS shims (the flat CoreEntitlements types plus opaque
`CEContext_t`/`CEKernelAPI_t`); the `iig` shim now runs the **real** Xcode `iig` (so IOService/
IOUserServer's new kernel dispatch -- `Panic`, `SetPowerOverride`, ... -- is generated correctly),
falling back to SDK headers; `TrustCache/*` is carried forward from xnu-8796; the BCM2837 board was
re-registered in `board_config.h`/`MakeInc.def`/`proc_reg.h` with a restored `MASTER.arm64.bcm2837`
(a copy of `MASTER.arm64` with `CPU_COUNTERS_BASE` emptied, `nos_arm_pmap nos_arm_asm`,
`mockfs ramfs apfs`, `ubsan_minimal`, no `sched_hygiene_debug`); the KernelSupport force-load and the
Apple AMCC/CTRR files are skipped for the non-Apple board; and the handful of symbols Darwin 26 moved
into the closed per-SoC KernelSupport library that are absent from open source (`master_cpu`,
`ml_device_is_prod_fused`, the two `memorystatus_available_pages_critical*` thresholds) are supplied
by a small `osfmk/arm64 kernel-support file`, with `san/memory/ubsan_minimal.c` registered to
provide the `__ubsan_handle_*_minimal` traps.

### Milestone 4b reached: the arm64e xnu-12377 (Darwin 26) kernel boots `hello` as PID 1 in QEMU

Done under `QEMU_CPU=cortex-a710`: the Darwin 26 arm64e kernel boots all the way to

```
Hello from user space on Darwin!
I am process 1, uid 0, console fd 0.
```

with no PAC fault during the run, then panics `initproc failed to start ... (signal 0, exit status
0)` -- the expected terminal state when the trivial PID-1 `hello` prints and returns (XNU always
panics when initproc exits), exactly as in milestone 1.

No prelink is needed (hakios builds its drivers into the kernel; the `hello` binary is itself the
RAM disk that mockfs boots as `/sbin/launchd`, exactly as in milestones 0-1). The boot is just:

```
xnuloader/mkpayload.py -o qemu/payload.bin kernel=<xnu-12377 mach.development.bcm2837> \
  ramdisk=userland/hello --cmdline "cpus=1 serial=3 debug=0x8"
QEMU_CPU=cortex-a710 qemu/run.sh 90
```

**The `xnuloader` PAC signer works unchanged on xnu-12377.** It signed `0x75fa` (30714) chained-auth
pointers and flattened 12505 rebases in the arm64e kernel, programmed the keys, and set
`SCTLR_EL1`/`TCR_EL1`; the chained-fixup layout is the same `CHAINED_PTR_ARM64E_KERNEL` as xnu-8796,
so no retarget was required.

**What milestone 4b took (fault-by-fault, `-d int` + `qemu/kernel.syms`).** Beyond the early
entropy-seed fix (loader `seed[64]` -> `seed[256]`), six boot-debug fixes, each a case of the
xnu-12377 restructuring colliding with the standard-PAC backend or the non-Apple board, most of them
a hakios fix the xnu-8796 tree already carried that the rebase dropped:

1. **kstackptr / upcb / istackptr PAC-DA auth (`machine_load_context`).** The asm
   `AUTDA_DIVERSIFIED` (`machine_routines_asm.h`, used by `LOAD_KERN_STACK_TOP` etc.) unconditionally
   `autda`'d fields the C side now stores plain under `XNU_PTRAUTH_DATA_PTRS_PLAIN` (the
   `XNU_PTRAUTH_SIGNED_PTR` qualifier expands empty on this tree) -> FPAC. Gated the `autda` on
   `!XNU_PTRAUTH_DATA_PTRS_PLAIN`, and mirrored that macro into `proc_reg.h` so the assembler (which
   includes `proc_reg.h`, not `base.h`) sees the same condition as the C field qualifiers.
2. **`image4 interface not available` (`kern_trustcache.c`).** `trust_cache_runtime_init()` panics
   when no img4if/amfi interface is registered. Restored the xnu-8796 `#ifndef BCM2837` guard around
   the trust-cache init + static-cache load in `osfmk/kern/startup.c` -- the non-Apple board has no
   Image4/AMFI closed kexts and needs no trust cache.
3. **`_unsafe_kernel_text_initialized` assertion (`machine_routines.c`).**
   `ml_unsafe_kernel_text_init()` only set the flag when the device tree `/chosen` has iBoot's
   `kernel-ctrr-to-be-enabled`; QEMU's DT has none. Added an else-branch defaulting the flag to
   initialized + "text is unsafe" -- true, since the board does no CTRR/AMCC lockdown.
4. **`iomediaClass` assertion (`IOService.cpp`).** Darwin 26's new `publishHiddenMedia()` (called from
   `setRootMedia` while bringing up the md0 RAM disk) asserts the `IOMedia` metaclass, which lives in
   the IOStorageFamily kext that is not built into the bring-up kernel. Made it a safe early-return
   when the metaclass is absent (no IOMedia class -> no hidden media to publish).
5. **`mockfs ... failed to create fictitious pages, rvalue = 5` (`disk_private.h`).** The memory
   device sits at kernel VA `0xfffffff008824000`, whose page number does not fit in 32 bits, but Darwin
   26 moved `dk_memdev_info_t` to `disk_private.h` with `mi_base` back to `uint32_t`, truncating the
   base page handed to mockfs so `pmap_find_phys` failed (`KERN_FAILURE` == 5). Widened `mi_base` to
   `uint64_t`, the xnu-8796 hakios fix.
6. **`PAC failure with IA key while branching to x22` (`fleh_dispatch64`, `locore.s`).** Darwin 26 moved
   the handler-pointer `pacia x1, sp` to the top of `fleh_dispatch64`, before the standard-PAC kernel
   key reload. Under HAS_STD_PAC an EL0-origin exception still has the process's keys loaded there, so
   the handler was signed with the user IA key but `braa x22, sp` authenticates with the kernel IA
   key -> FPAC on the first userspace trap during exec. Deferred the `pacia` to just after
   `PAC_STD_LOAD_KEYS`, as the xnu-8796 tree did. Boot log: `scratchpad/xnu12377-boot.log`.

**Still remaining (not blocking the `hello` milestone):** a panic-time printer nuisance --
`is_debug_ptr_in_ext_paniclog` takes a nested unaligned data abort on every panic (the loader still
captures the full kernel log, so it does not hide anything).

### Milestone 4c: the 17 userland/filesystem rejects resolved

The 17 `.rej` hunks the rebase deferred are resolved and the kernel rebuilds (arm64e, exit 0).  Most
turned out to be either fixed upstream by Darwin 26 or mechanical -- the recurring pattern again being
the xnu-12377 restructuring meeting the hakios features:

- **8 rejects were obsolete** -- the whole `uio_stackbuf_t uio_buf[UIO_SIZEOF(n)]` sizing fix
  (`vfs_lookup/subr/vnops/syscalls/attrlist/quota.c`, `nfs_serv.c`, `security/mac_vfs_subr.c`).
  Darwin 26 replaced that idiom with a new `UIO_STACKBUF(name, niov)` macro
  (`uint64_t name[P2ROUNDUP(UIO_SIZEOF(niov), 8) / 8]`) that already does exactly the rounded-up
  sizing the hakios patch added by hand, so the code those hunks targeted no longer exists.
- **`sched_clutch.c` was obsolete** -- its `sched_clutch_thread_avoid_processor()` target was removed
  in 12377.
- **`sched_prim.c` (big.LITTLE slow-secondary scheduler):** the `sched_slow_secondaries` global was
  already applied; the three remaining `&& !sched_slow_secondaries` guards were reapplied (now inside
  12377's new `#if CONFIG_SCHED_SMT`). Inert at `cpus=1`.
- **`video_console.c`:** cacheable framebuffer mapping extended to the generic board
  (`HAS_UCNORMAL_MEM || APPLEVIRTUALPLATFORM || defined(BCM2837)`).
- **`kalloc.h`:** the `XNU_NO_TYPE_BUILTINS` fallback for `KALLOC_TYPE_IS_COMPATIBLE_PTR`, adapted to
  12377's `os_get_pointee_type` macro form (a defensive no-op; our toolchain has the builtin).
- **`kern_exec.c` (persona / xpcproxy):** the `applog` console-redirect helper was already present;
  reapplied the persona relaxation -- drop the `PERSONA_MGMT_ENTITLEMENT` requirement and the
  "persona not found -> ESRCH" failures so persona spawns work where nothing is entitled and no
  personas exist, adapting the missing-persona path to 12377's new block-based
  `persona_proc_adopt()` / `kauth_cred_proc_update(p, PROC_SETTOKEN_SETUGID, ^{...})` API; and the
  xpcproxy identity-version elision (keep xpcproxy's idversion so RunningBoard's audit tokens match),
  adapted to 12377's retry-loop idversion assignment.
- **`kern_memorystatus.c` (jetsam):** Darwin 26 consolidated the per-cent-of-memory math into one macro
  `MEMSTAT_PERCENT_TOTAL_PAGES(p)` plus `MEMSTAT_PAGE_SHORTAGE_THRESHOLD_MAX`, so the 5-hunk
  "use `sane_size`, not `max_mem`, so the RAM-disk image is not counted as manageable memory" fix
  lands in just those two macros (`#define memstat_memory_size sane_size`).
- **`arm_init.c`:** the one functional hunk -- `psci_smp_cpu_reached(PSCI_SMP_CPU_IN_KERNEL)` for the
  PSCI SMP bring-up -- was applied; the rest were `loader_checkpoint()` markers for the early
  pre-console path, which already boots cleanly to PID 1, so they were not re-placed.
- **`makedefs/MakeInc.def`** was already applied during the rebase (board `+pan`, `-lpthread_kext`,
  `the old kernel's object-list variable`); **`MakeInc.cmd`** (absolute -> bare tool paths) is only for non-macOS build
  hosts and is left as-is, since the absolute paths exist and the build succeeds on macOS.

**What this unblocks and what it does not.** The kernel is now ready for the full Darwin 26 arm64e user
space (launchd + daemons + SpringBoard, as in milestone 3).  The remaining blocker is *data, not
kernel code*: the extracted Darwin 26 root filesystem and its 84-subcache arm64e dyld shared cache are
not on disk (a previous session's scratch).  Re-acquiring them means re-downloading the iPhone 17
Pro Max Darwin 26.6.2 IPSW (~12 GB, Apple CDN -- the `/etc/hosts` unblock needs the user's sudo),
AEA-decrypting the root/cryptex (`/usr/bin/aea` + the FCS key from Apple), re-extracting the cache,
and writing an `darwin26/` selection config (the analog of `darwin165/select-system.py`).  Then
`userland/build.sh` assembles `system.apfs`/`root.apfs` and `qemu/run-darwin.sh` boots it -- pointed at
the xnu-12377 kernel (it currently hardcodes the xnu-8796 path).
