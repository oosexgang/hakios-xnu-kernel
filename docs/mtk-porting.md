# XNU on another MediaTek phone: the first boot

What the Infinix Hot 20 (X6826B, MT6768) taught, as an order of work for another MediaTek phone: from a stock Android phone to the kernel's own log,
all cores, the root on the flash and the home screen. The later pieces have their own guides: [Bluetooth](bluetooth-mtk-porting.md),
[the camera](camera-mtk-porting.md). Every number here is the X6826B's and is named as such; none of them may be carried to another phone
unread.

No second MediaTek phone has been ported with this text yet. It says what one phone needed, in the order it was needed, and where each thing is in
the tree. Expect a step it does not know about.

## What is yours and what is not

| The same on every phone (already written) | Yours to find and write |
| --- | --- |
| The kernel: Apple's xnu-8796.121.2 + `xnu/redwood.patch` + `xnu/files/`, built as `DEVELOPMENT_ARM64_BCM2837` | The loader's board constants (the table below) |
| GICv3 (`osfmk/arm64/gic_v3_irq.c`), PSCI cores (`psci_smp.c`), the return to the loader (`loader_return.c`), the platform expert (`iokit/Kernel/GenericARMPE.cpp`) | The order of the processors and which are the big ones |
| The image format and its packer (`tools/chainboot.py`), the run cycle (`tools/xnu-cycle.sh`, `tools/boot-xnu.sh`) | The storage driver if yours is not an eMMC on MSDC0 left ready by LK |
| The iOS side: `ios165/fetch.sh`, `userland/build.sh`, the injected libraries | The touch controller, the GPU's power sequence, the keys, the radios, the camera |
| The log's road back through ramoops (`mtkloader/mtkdbg.h`) | The addresses of the ramoops zones and of LK's framebuffer |

A phone with another SoC of the same family keeps most of the left column and all of the method. A phone that is not MediaTek keeps the kernel and
the iOS side only: `mtkloader` assumes LK, MSDC, TOPRGU and OVL.

## What the phone must allow

1. **An unlocked bootloader** and a way to write one partition (`fastboot flash boot_a`).
2. **Root under Android** (the X6826B has Magisk in `boot_a`). The run cycle asks for an XNU boot through `/dev/pmsg0`, reads the log from
   `/sys/fs/pstore` and writes the system image with `dd`: all three need `su`. `mtkloader/build_flashed.py` packs the loader into **your own**
   rooted boot image, `backup/boot_a_magisk.img` by default; it is never in the repository.
3. **ramoops in Android's kernel**: `/sys/fs/pstore/console-ramoops-0` exists after a reboot. This is the only way out for a log: the X6826B has no
   reachable serial port, and nothing else survives the reset back into Android.
4. **A way back that does not need the partition you are writing.** On an A/B phone recovery and fastbootd live inside `boot`, so a `boot_a` that
   hangs before LK's keys are read leaves no fastboot. On the X6826B that happened once and the boot ROM's download mode with mtkclient
   (`mtk.py w boot_a <the rooted image>`) was the rescue. Check that your phone's boot ROM is reachable **before** the first flash, and keep the
   rooted image at hand. Slot B is never written by anything here.
5. **Space for the system image**: a partition of its own (4 GiB on the X6826B, `xnusys`, carved off the end of `userdata`).

## The boot chain

```
boot ROM -> preloader -> LK (stock, untouched)
   LK loads boot_a as an Android boot image (version 2) and starts its "kernel" as a Linux arm64 Image:
   at 0x40080000, MMU and caches off, x0 = LK's device tree
      the "kernel" is gzip of:   mtkloader.bin | padding to CHAIN_OFFSET (0x40000) | Android's own Image | "XNUP" payload (XNU, small root)
         mtkloader: Android, unless one XNU boot was asked for
            XNU: Mach-O copied to free DRAM, a device tree in Apple's format built in memory, boot_args revision 2, LK's framebuffer
               ... the run ends (time, panic, checkpoint) -> back into the loader -> log into ramoops -> warm reset -> Android
```

- `tools/chainboot.py` packs the image; the arm64 header's `res2` is the length of Android's Image and `res3` the payload's offset
  (`mtkloader/start.S`). Android's ramdisk, dtb and command line are kept as they were.
- The payload is `build-payload.bin`, made by `xnuloader/mkpayload.py` as `XL_RAMDISK=userland/root.apfs ./xl_x6826b build` calls it. The
  loader takes two of its entries: `kernel` and `ramdisk` (the small root, md1). `mtkloader/build_flashed.py` then makes
  `xnu-flashed-slot_a.img`.
- **The default is Android.** XNU starts only when the loader finds the words `DARWIN-REDWOOD:BOOT-XNU` in ramoops' pmsg zone, written from
  Android by `tools/boot-xnu.sh`, and takes them away: the boot after XNU is Android again. A kernel that hangs costs a power cycle.
  (`-DMENU_DEFAULT_XNU=1` reverses that; an image that always started XNU, with no watchdog, is how the phone was trapped.)
- The loader runs with the MMU off, so all memory is device memory: no SIMD, no unaligned access, no compiler builtins
  (`-mgeneral-regs-only -mstrict-align -fno-builtin`, `mtkloader/build.sh`). Code added to it must hold to that.
- A marker in DRAM (`MARKER_PA`, three copies) says how the last run ended: none, run; `NXRN`, XNU was cut short, so copy its log out of DRAM
  and start Android; `NXDN`, XNU came back and its log is saved, start Android.

There are two image scripts in the tree. `xl_x6826b boot` + `mkxnuimg_x6826b.py` is the first one (`fastboot boot` of a one-off image); it is still
what builds the payload. Everything after the first boots uses `tools/xnu-cycle.sh`.

## Read these from your phone first

All under Android, as root, before any code is written. `tools/fdtfind.py <file.dtb> <word>` searches a device tree saved to a file; the live one is
under `/sys/firmware/devicetree/base`.

| What | Where it goes | The X6826B's value | Where to read it |
| --- | --- | --- | --- |
| Where LK loads the kernel | `mtkloader/link.ld`, `KERNEL_PA` | `0x40080000` | `/proc/iomem` ("Kernel code"), the boot image header |
| ramoops: console zone, its size, pmsg zone | `RAMOOPS_CONSOLE`, `RAMOOPS_CONSOLE_SIZE`, `RAMOOPS_PMSG`, `RAMOOPS_PMSG_SIZE` (`mtkdbg.h`) | `0x4d09f000`, `0x40000`, `0x4d0e0000`, `0x10000` | the device tree's `reserved-memory` pstore node and Android's kernel log: the region is cut into dump records, console, ftrace, pmsg in that order (`0x4d010000` + `0xe0000` here) |
| A range of DRAM nothing else uses | `XNU_PHYS_BASE`, `XNU_MEM_SIZE` | `0xc0000000`, `0xfe000000` | `reserved-memory` and `/proc/iomem`: clear of LK, Android's Image and ramdisk, ATF, the TEE, the framebuffer, ramoops |
| A place the marker survives a warm reset | `MARKER_PA`, `PANIC_LOG_BASE` | `0x80000000`, `0x80100000` | fbdiag's phase 2 (below) says whether DRAM is kept |
| LK's framebuffer, its pitch, the panel | `LK_FB_BASE`, `LK_FB_PITCH`, `PANEL_W`, `PANEL_H` | `0x787b0000`, 2944 (736 pixels: LK rounds 720 up to 32), 720 x 1640 | `reserved-memory`, then fbdiag, which reads the address the display engine scans |
| The display engine's first layer | `OVL0_BASE`, `DISP_MUTEX_*` | `0x1400b000`, `0x14001020` | the device tree (`ovl`, `mutex`); offsets from the vendor's display driver |
| The watchdog | `TOPRGU_BASE` | `0x10007000` | the device tree (`toprgu`) |
| The interrupt controller | `GICD_BASE`, the redistributors | `0x0c000000`, `0x0c040000` | the device tree's `interrupt-controller` |
| The UART (for the device tree, even if no pin is reachable) | `MT6768_UART_BASE` | `0x11002000` | the device tree |
| The eMMC's controller | `MSDC0_BASE` (`msdc.h`) | `0x11230000` | the device tree (`mmc0`/`msdc0`). **UFS phones need another reader** |
| The processors | `core_order[]`, `XNU_CORES` | `{0, 7, 6, 1, 2, 3, 4, 5}`: the boot core, the two A75s, the other A55s | `/proc/cpuinfo` (CPU part), the device tree's `cpus` |
| The PLL of the big cluster, if LK leaves it slow | `APMIXED_BASE`, `ARMPLL_CON1`, `BIG_MHZ` | `0x1000c000`, `0x20c`, 1176 | the vendor's cpufreq table for your chip. Leave `big_cores_clock()` out until you have read it: it writes a PLL |
| The name of the system partition | `SYSTEM_PART` | `xnusys` | yours to choose |
| The board's name for the kernel | `darwin-board` in the device tree the loader builds | `x6826b` | yours to choose |

Three constants are the kernel's and not the phone's: `KERNEL_VIRT_BASE` (`0xfffffff004000000`), `PAGE_SIZE_MAX`, the boot-args format.

`XNU_SERIAL` is a placeholder (`HAKIOS0000000000`). A real build passes its own with `-DXNU_SERIAL=...`; it is never committed.

## The order of work

Each step has the check that proves it. Do not start a step whose predecessor's check has not been seen on the phone.

1. **Does code of ours run at all, and can it say so?** `mtkloader/build_fbdiag.sh` packs `fbdiag` in front of Android's kernel. It writes to
   the screen and into the ramoops console, waits five seconds and starts Android. *Check:* `/sys/fs/pstore/console-ramoops-0` begins with
   `fbdiag: phase 1` and a line with the exception level, SCTLR, CPACR, MPIDR and CNTFRQ. If the file is Android's old log, the console zone's address
   is wrong. Six bars on the screen say whether the layer has per-pixel alpha (bar D, white with alpha 0, is invisible if it has). Phase 2, after a
   watchdog reset, says how much of DRAM was kept: that decides whether a log can be rescued from a kernel that hung.
2. **The loader's own log.** Build `mtkloader` with `-DMSDC_TEST=1` (the next step's test: it starts no kernel). *Check:* its lines in the same file. Now every later
   step has a voice: the rule is that code says what it is about to do **before** anything that can hang, so the last line of a log is the step
   that hung.
3. **The flash.** `tools/msdc-test.sh` (`-DMSDC_TEST=1`): the loader reads the GPT through `msdc.h`, in PIO mode, with the card as LK left it:
   nothing is initialised. *Check:* `eMMC read test` in the log with your partition names. On a UFS phone this step is a driver you write
   (`bsd/dev/redwood_ufs.c` is the Qualcomm phone's and is a place to start reading, not a fit).
4. **Timers and interrupts, before the kernel.** `-DTIMER_PROBE=1` sets the GIC up as XNU does and arms each timer. Two things stopped the X6826B
   here, both silent:
   - `ICC_CTLR_EL1.EOImode` is left set by the firmware below (ATF/GenieZone). XNU writes only `ICC_EOIR1_EL1`, so with it set the first timer
     interrupt is also the last, and the kernel idles for ever "waiting for the root device". The loader clears it on the boot core
     (`gic_fix_eoimode`) and patches one instruction of the kernel's `gic_v3_irq_cpu_init` so that every other core clears it too.
   - XNU's timer is the virtual one (PPI 27). The loader prints CNTPCT against CNTVCT and the exception level it was started at.
   *Check:* the probe's lines name a timer that was taken.
5. **The kernel's first lines.** Build the kernel (`xnu/README.md`), make the payload, run `tools/xnu-cycle.sh` with `XNU_ARGS="cpus=1"`. There is
   no serial port, so the loader finds the kernel's console functions by symbol and points them at its own log buffer (`run_xnu`: the patches of
   `vmapple_uart`). If the kernel says nothing, `-DXNU_CHECKPOINT=n` makes it return at the n-th `LOADER_CHECKPOINT` of `osfmk/arm64/start.s` /
   `arm_init.c`, and the log says `(checkpoint)`: bisect with that. `-DXNU_VERBOSE=1` puts the text console on the panel; it is slow and iOS cannot
   draw under it. *Check:* the kernel's version line in the log.
6. **Memory and the device tree.** The loader builds the tree the kernel reads (`mtkloader.c`, the device tree builder): the root's identity,
   `chosen` (`dram-base`, `dram-size`, `darwin-board`, `nvram-proxy-data`, the memory map with the RAM disks), `cpus` (`reg` = core << 8),
   `arm-io` with the GIC and the UART, `psci-method` = `smc`. *Check:* the kernel reaches `vm_page_bootstrap` and prints its page counts.
7. **The other cores.** Raise `cpus=` one at a time. `XNU_CORES` and `cpus=` must be the same number (`xnu-cycle.sh` keeps them so): the tree
   offers exactly that many, from the head of `core_order`, and the next one is the watcher's. A watcher on a core the kernel wants hangs every
   rendezvous. *Check:* each processor checks in, in the log.
8. **The root.** `ios165/fetch.sh` (your own download of iOS 16.5, 20F66), `userland/build.sh` for `system.apfs`, the partition
   (`tools/gpt-carve.py` writes a new GPT **to files**; putting it on the phone is your step and the one that can lose `userdata`), then
   `tools/system-flash.sh`. With `emmc_root=1` the kernel reads the partition itself (`bsd/dev/redwood_emmc.c`, [the measurements](x6826b-emmc.md):
   holding the image in DRAM instead cost 2.5 GB wired and 203 s of every boot). *Check:* `launchd` starts; then the home screen appears on
   LK's framebuffer, drawn by the CPU.
9. **The screen as iOS wants it.** LK's layer has per-pixel alpha on and the kernel's console writes pixels with alpha 0: nothing is seen. The
   loader switches the alpha off and the other LK layers too. If your panel is command mode and not video mode, pixels written into the
   framebuffer do not appear by themselves: that was not met here.
10. **Touch.** Probe from the loader first (`tools/touch-probe.sh`, `-DTOUCH_PROBE=1`): a cycle is four minutes and no kernel is rebuilt. Then
    the kernel driver has one entry point, `redwood_touch_post(type, finger, x, y)`. The X6826B's controller (TD4160, `redwood_tcm.c`) keeps its
    firmware in RAM, so the loader uploads it on the way (`tcm.h`); yours may not need that.
11. **The keys and the clocks** (`redwood_mtkkeys.c`, through the PMIC wrapper), **the GPU** (`tools/gpu-probe.sh`, then `redwood_mali.c`, with Mesa's
    panfrost in user space), **the sensor hub** (`redwood_scp.c`, [sensors](sensors-x6826b.md)).
    `mtk_cpufreq=` and `mali_mhz=` change PLLs and, above some points, voltages: read your chip's table first.
12. **The radios, the camera, the battery**: [Wi-Fi](wifi-x6826b.md) (CONNSYS from the phone's own firmware), [Bluetooth](bluetooth-mtk-porting.md),
    [the camera](camera-mtk-porting.md), `redwood_mtkbatt.c` (the PMIC's gauge).

## One run

```
export HAKIOS_SERIAL=<your phone's adb serial>          # never in a file
./xnu-build.sh -j8 -k                                   # the kernel (xnu/README.md for the first time)
./userland/build.sh                                     # the small root, userland/root.apfs (its head comment lists the switches)
XL_RAMDISK=userland/root.apfs ./xl_x6826b build         # the payload: kernel + the small root
XNU_SECS=120 XNU_ARGS="cpus=4 touchscreen=2 mali=1" XNU_LOG_OUT=logs/run1.log tools/xnu-cycle.sh
```

`xnu-cycle.sh` builds the loader with the run's flags (anything after the script's name goes to the compiler, such as `-DXNU_CHECKPOINT=17`), packs
and flashes `boot_a` (and refuses if the current slot is not `a`), waits for Android, asks for one XNU boot, waits for Android again and reads the
log. The kernel returns by itself after `XNU_SECS` (`loader_return_after=`); a watcher on a spare core takes a snapshot and resets the board
`XNU_WATCH` seconds in if it has not. The log holds 256 KB of console and what the kernel kept for the host (`kern.redwood.host_keep`, through the
pmsg zone, 64 KB): a long run loses its beginning, so keep runs short and say less.

## The kernel's side

- A driver is a file `xnu/files/bsd/dev/redwood_<name>.c`, a line in `bsd/conf/files` (in `redwood.patch`), and a call to its `redwood_<name>_init()`
  in `redwood_input.c`. There is no clock, pin, regulator or bus framework: a driver maps its registers and writes them, and says in its head comment
  which Linux source each layout was read from. GPL sources are read and named, never copied.
- **Every driver is off without its boot-arg**, because the same kernel boots QEMU, the Qualcomm phone and this one, and every address in a driver
  belongs to one phone. The X6826B's: `emmc_root=1`, `touchscreen=2`, `buttons=2`, `mali=1`, `metal=1`, `connsys_step=17`, `mtkcam=1`, `mtkbatt=1`.
  Give your phone's drivers their own values or names; do not reuse a value that maps another phone's registers.
- Drivers that share the PMIC wrapper take `redwood_pwrap_lock()` around every transaction.
- Two skeletons hold the phone-independent half of a subsystem and refuse to run until a table of operations is complete:
  `redwood_mtkbt_skel.c`, `redwood_mtkcam_skel.c`. A new subsystem is worth writing that way from the start.

## What stopped the X6826B, in one list

| What was seen | What it was |
| --- | --- |
| Android's old log in pstore, nothing of ours | the wrong console zone address, or the loader not started (the image's header) |
| The kernel "waits for the root device" for ever | no timer interrupt after the first: EOImode (step 4) |
| A black panel while the log says the console draws | per-pixel alpha on LK's layer (step 9) |
| A 15 minute run on the Apple logo | the watcher on a core the kernel was offered (step 7) |
| Jetsam killing everything minutes in | the system image held in DRAM (step 8) |
| The boot image does not fit | the loader is built `-Os`; `boot_a` is 32 MB here and the kernel plus the small root must fit |
| `rootvp not authenticated`, or a reboot looking for a preboot volume | `rd=md0` must stay on the command line beside `emmc_root=1` ([why](x6826b-emmc.md)) |
| The phone boots only into a hung XNU | an image that always started XNU; the boot ROM was the way back |

## What is never in the tree

Nothing of Apple's, nothing of the vendor's (no firmware, no partition image, no device tree, no battery table), no serial number, no home path, no
network name. A port brings its own phone's files from its own phone.
