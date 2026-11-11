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

3. **`0003-arm64e-without-pac.patch`**:
   - Enables plain arm64 ABI compatibility without PAC instruction crashes on ARMv8.2.

4. **`0004-renoir-drivers.patch`**:
   - Ports core renoir hardware drivers and platform support from darwin-renoir (xnu-8796) into xnu-12377.

5. **`0023-universal-backlight.patch`**:
   - `bsd/dev/hakios_backlight.c`: brightness and automatic brightness for any device: `hw.backlight.*`, board profiles (Renoir, Redwood: MIPI DCS 0x51 through the Qualcomm DSI command FIFO; QEMU and unknown boards: software dimming in `MiniIOMobileFramebuffer`). See `renoir/docs/backlight.md`; tests in `renoir/tests/`.

6. **`0024-metal-plugin-gate.patch`**:
   - `IOAcceleratorES::setProperties` back from the 16.5 kernel: with boot-arg `metal=1`, user space may name a Metal plug-in (`MetalPluginName`, `MetalPluginClassName`) and nothing else. Off by default. See `renoir/docs/metal-glass.md`.
7. **`0025-thermal-limiter.patch`**:
   - `bsd/dev/hakios_thermal.c`: every TSENS sensor read each second; the EPSS groups held back a row at a time above `th_trip`, given back below `th_clear`, at their floors at `th_critical`; the panel's brightness capped with them. `thermal=1` looks only, `thermal=2` acts. See `renoir/docs/thermal.md`.

## renoir drivers

Hardware support brought into XNU 12377 from `darwin-renoir`:

### What Was Ported

1. **Display & Framebuffer (`MiniIOMobileFramebuffer.cpp` & `bsd/dev/redwood_adreno.c`)**:
   - Command-mode panel scan synchronization (`redwood_display_scan_start`, `redwood_display_scan` reading INTF1 `INTF_LINE_COUNT` and `INTF_FRAME_COUNT` on Renoir DSI interface `0x0ae6b800`).
   - Frame timing model calibrated to the panel's TE refresh rate (~60 Hz) without timer drift.
   - Striped row drawing (`drawShare`) ahead of the panel readout beam for tear-free output.
   - Dual-buffer RAM backbuffer (`fb_double=1`) option for tear elimination.
   - Screen console release (`kPEReleaseScreen`) on handover to graphics mode.
   - Small-core thread binding (`redwood_slow_cores`, `redwood_bind_slow_core` in `osfmk/arm64/psci_smp.c`): binds presenter and scaling helper threads to Cortex-A55 cores (`fb_small=1`) to keep Cortex-A78 cores clear for UI and compositor threads.

2. **Touchscreen (`bsd/dev/renoir_fts.c`, `bsd/dev/renoir_fts_protocol.h`, `bsd/dev/redwood_input.c`)**:
   - FocalTech FT3658U SPI driver ported from `darwin-renoir`.
   - Runtime board identification via `redwood_board_is("renoir")` using DT `/chosen/darwin-board`.
   - APPS DRV2 TCS0 serialization lock (`redwood_rpmh_lock` / `redwood_rpmh_unlock`) in `redwood_input.c`.
   - Conditional driver dispatch in `redwood_input_init()`: initializes `renoir_fts` on Renoir and `redwood_goodix` on Redwood.

3. **CPU Frequency & DDR Voting (`bsd/dev/redwood_cpufreq.c`)**:
   - Enabled EPSS 3-domain CPU frequency scaling for Renoir SM7350 (`redwood_board_is("renoir")`).
   - Enabled RPMh memory bandwidth votes (MC0, SH0, ACV) for Renoir LPDDR4X.
   - Synchronized RPMh TCS0 slot transactions with `redwood_rpmh_lock()` / `unlock()`.

4. **GPU Adreno 642 (`bsd/dev/redwood_adreno.c`, `bsd/dev/redwood_adreno_profile.h`)**:
   - Adreno board profile definition for Renoir (chip ID `0x06060010`, GMEM 1536 KB, speed fuse `0x1e1`, gaming fuse `0x1ed`).
   - Shima speed bin OPP table (285 MHz, 365 MHz, 443 MHz, 490 MHz; default 490 MHz).
   - A642-specific GMU chip ID mapping (`adreno642_gmu_chip_id`).
   - Dual-queue almost-full configuration (`uche_cmdq_config` = `0x66906`).
   - TrustZone zap shader security checks ensuring authentication on Renoir.
   - Firmware directory redirected to `fw-renoir/`.

5. **CPU Topology & Small-Core Detection (`osfmk/arm64/machine_routines.c`)**:
   - Device tree parsing of `l2-access-penalty` and `small-core` in `ml_parse_cpu_topology()`.
   - Flags Cortex-A55 cores with `cpu->l2_access_penalty = 1`, making them visible to `redwood_slow_cores()`.
   - Note on scheduler pairing: XNU 12377's scheduler refactor removed `processor_primary` / `processor_set_primary`, so secondary pairing remains a no-op; kernel worker binding (`fb_small=1`) directs heavy streaming work onto small cores.

6. **Base Peripherals & Storage Fixes**:
   - `bsd/dev/redwood_battery.c`: enabled for Renoir resident UEFI state.
   - `bsd/dev/redwood_buttons.c`: enabled for Renoir PM8350 keys (SID1 GPIO6).
   - `bsd/dev/redwood_virtio_input.c` / `virtio_net.c`: gated on `redwood_board_is("qemu-virt")`.
   - `bsd/dev/redwood_lx.c` & `bsd/sys/redwood_lx.h`: early Renoir 80 MiB contiguous radio pool reservation (`lx_reserve_radio_pool`).
   - `bsd/miscfs/ramfs/ramfs.c`: non-atomic large memory allocation fix (`can_wait` parameter in `ramfs_grow_slots`).
   - `bsd/miscfs/apfs/apfs_vfsops.c`: RAM-only memory disk fast requests (`am_nx.cheap_requests = true`).

### What Was Not Ported

- **Speaker Amp (`renoir_tfa.c`) & Sound**: NXP TFA9873 amplifier and sound driver rewiring deferred to separate audio work.
- **Haptics (`renoir_haptic.c`)**: PM8350B linear resonant actuator driver deferred.
- **Wi-Fi Transport (`renoir_mhi_*`, `renoir_pcie_phy.h`)**: WCN6855 PCIe/MHI link initialization deferred.
- **Camera Core (`renoir_camera_core.h`, `redwood_camera.c` Renoir paths)**: ISP/CSIPHY pipeline deferred.

### Safe Boot Arguments for Renoir

The following boot-args are verified safe on Renoir:

| Boot Arg | Default | Description |
|---|---|---|
| `touchscreen=1` | 0 | Starts FocalTech FT3658U touch controller polling thread |
| `touch_flip=1` | 0 | Inverts touchscreen axes if required by panel orientation |
| `fb_scan=1` | 1 | Enables command-mode panel TE readout scan synchronization |
| `fb_small=1` | 1 | Binds framebuffer drawers and presenter to Cortex-A55 small cores |
| `fb_threads=4` | 4 | Number of scaling helper threads (1-8, clamped to small core count) |
| `fb_double=0` | 0 | Set to 1 to enable RAM backbuffering for tear elimination |
| `cpufreq=1` | 0 | Enables EPSS hardware CPU frequency scaling |
| `ddr=1` / `ddr=2` | 2 | Enables RPMh memory bus bandwidth voting |
| `gpu=1` | 0 | Boots Adreno 642 if `fw-renoir` firmware binaries are present (0021: checked there, not in `fw`) and keeps it for the service RedwoodGPU |
| `buttons=1` | 0 | Starts PM8350 side power and volume button monitoring thread |
| `battery=1` | 0 | Enables resident UEFI battery charge monitoring |
| `small_cores=2`| 2 | Slower core topology configuration (0: uniform, 1/2: small cores) |

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
- **Size**: ~18.9 MB (18,961,152 bytes)
- **CPUTYPE / SUBTYPE**: `CPU_TYPE_ARM64` (0x0100000c) / `CPU_SUBTYPE_ARM64_ALL` (0)
