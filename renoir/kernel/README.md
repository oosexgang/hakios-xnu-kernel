# Renoir Kernel (XNU 12377)

Build of Apple's XNU `xnu-12377.121.6` (iOS 26.6.2, Darwin 26.5.0) for the Xiaomi Mi 11 Lite 5G ("renoir", Qualcomm SM7350: Cortex-A78/A55, ARMv8.2, no PAC, 6 GB RAM).

Built as plain **arm64** (`XNU_ARCH=arm64 MACHINE=BCM2837 KERNEL_CONFIGS=DEVELOPMENT`) on Linux (WSL Arch Linux) using the `darwin-renoir` toolchain.

## Prerequisites

- Arch Linux packages: `base-devel`, `git`, `curl`, `clang`, `lld`, `python`, `perl`, `xz`
- Theos toolchain and SDK at `~/theos` or `$DARWIN_RENOIR/build-tools/theos` (cctools `ld64`, `nm`, `otool`, `strip`, `iPhoneOS16.5.sdk`)
- `darwin-renoir/xnu-deps` (stage 1 setup done: `mig`, `tcsh`, `unifdef`, `unzbm`, `libdispatch`, `libpthread`, `MacOSX13.3.sdk`)

## Building

```sh
# From the repository root:
renoir/kernel/build.sh              # builds with -j$(nproc)
# or pass arguments to make:
renoir/kernel/build.sh -j12
```

The script manages the entire build pipeline reproducibly from scratch:

1. **Kernel source tree (`$XNU_SRC`, default `../xnu-12377-renoir`)**:
   - Clones Apple's tag `xnu-12377.121.6` via `kernel/apply.sh`
   - Applies the HakiOS base patch `kernel/xnu-12377.121.6-hakios.patch` and creates a local baseline commit
   - Applies renoir patches from `renoir/kernel/patches/` (recorded in `.renoir-patches`)
   - Generates the empty FocalTech firmware stub header `bsd/dev/redwood_focaltech_fw.h`

2. **Dependency overlay (`$XNUDEPS`, default `../xnu-12377-renoir-deps`)**:
   - Symlinks host build tools from `darwin-renoir/xnu-deps` (`mig`, `tcsh`, `unifdef`) and `base/xnu-deps` (`codesign`, `xcrun`, etc.)
   - Installs the wrappers `host/xnu-clang` and `host/iig`
   - Downloads and unpacks `MacOSX26.5.sdk` Kernel.framework for DriverKit headers required by XNU 12377
   - Creates the SDK overlay symlinking `iPhoneOS16.5.sdk` plus `usr/local` headers
   - Generates availability headers and `libdispatch` firehose headers

3. **Headers & Static Libraries**:
   - Runs `xnumake exporthdrs` to produce `$XNU_SRC/BUILD/obj/EXPORT_HDRS`
   - Compiles `libfirehose_kernel.a` and `libpthread_kext.a` against the exported headers

4. **Kernel compilation and linking**:
   - Invokes `xnumake` with `DEFAULT_TARGET=build`, `XNU_ARCH=arm64`, `MACHINE=BCM2837`, `KERNEL_CONFIGS=DEVELOPMENT`
   - Does not link an external Wi-Fi driver object
   - Produces the Mach-O binary at `$XNU_SRC/BUILD/obj/mach.development.bcm2837`

## Patches Applied

The renoir port adds minimal patches in `renoir/kernel/patches/`:

1. **`0001-no-pac-hakios-key-diagnostics.patch`**:
   - Guards PAC system register accesses (`APIAKeyLo_EL1`, `APGAKeyLo_EL1`) in `osfmk/arm64/sleh.c` with `#if HAS_STD_PAC`.
   - Guards `hk_ib_report` in `osfmk/arm/arm_init.c` and its call in `osfmk/arm64/loader_return.c` with `#if HAS_STD_PAC`.
   - Guards early diagnostic exception PAC register reporting in `osfmk/arm64/start.s` with `#if HAS_STD_PAC`.
   - Prevents undefined instruction faults on ARMv8.2 cores (Cortex-A78/A55) which do not implement ARMv8.3 PAC registers.

2. **`0002-leave-out-TestDextRematch.patch`**:
   - Modifies `iokit/conf/files` to make `./Tests/TestServices/TestDextRematch.iig.cpp` and `iokit/Tests/TestServices/TestDextRematchKernel.cpp` optional under `iig_test_dext_rematch`.
   - Leaves out internal DriverKit tests whose pre-generated headers are not published in Apple SDKs.

## Disabled / Stubbed Drivers

To comply with licensing and redistribution requirements:

1. **FocalTech Touch Firmware (`bsd/dev/redwood_focaltech_fw.h`)**:
   - Proprietary touchscreen firmware is not redistributed.
   - `build.sh` stubs this with `#define FOCALTECH_FW_LENGTH 0` and a dummy 1-byte array so `redwood_goodix.c` compiles without external firmware.

2. **DriverKit IOExtensiblePaniclog (`iig-impl/IOExtensiblePaniclog.iig.cpp`)**:
   - Stand-in stub implementing the RPC Dispatch entry points returning `kIOReturnUnsupported`.

## Kernel Output

- **Path**: `xnu-12377-renoir/BUILD/obj/mach.development.bcm2837`
  (symlinked to `DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`)
- **File Type**: Mach-O 64-bit arm64 executable, flags `<NOUNDEFS|PIE>`
- **Size**: ~18.9 MB (18,941,368 bytes)
- **CPUTYPE / SUBTYPE**: `CPU_TYPE_ARM64` (0x0100000c) / `CPU_SUBTYPE_ARM64_ALL` (0)
