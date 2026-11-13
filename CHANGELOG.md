# Changelog

What changed in HakiOS, for people who use it, build it or port it to a phone. Changes are grouped by
what a reader does with the project, not by commit: `git log` has the detail. There are no release
tags yet, so the current work is one section.

## Unreleased: the restructuring

The project's structure was reworked: where a phone is described, how the kernel's sources are kept,
the documentation and the checks. What works on which phone did not change, with one exception on
Renoir ("What you will notice" below). What has not been verified since is at the end of this section.

### Porting to a new phone

- **A phone is data.** `devices/NAME/board.dts` says what a phone has. `soc/SOC.dtsi` says where the
  blocks of its chip are, once for every phone with that chip. Before, each driver carried its own list
  of phones and addresses, and the loader described each machine in C. Every key is explained in
  `devices/_template/board.dts` and `devices/_template/soc.dtsi`; [devices/README.md](devices/README.md)
  has every section, and the steps for making one more constant data.
- **A tool starts both files from the phone's Android device tree.** `tools/boards.py new NAME --soc SOC
  --dts FILE` reads the DSI controller, the temperature sensors, the CPU frequency block and its groups
  of cores, the clock controllers' addresses, the interrupt controller, the cores, the PSCI method, the
  panel's brightness range and mode, the PMIC arbiter with the keys and the flashlight, the SoC's own
  pins, its SMMU, the way to its other processors, RPMh, the UFS host controller, the reserved memory of
  the audio and Wi-Fi DSPs, the radio's pins, the sound's four pins, the microphones' codec reset and the blocks on their path. It
  prints what it could not find. Two things are never in a device tree and are filled in by hand: the
  display clocks' register offsets and the I/O window.
- **`tools/boards.py compare NAME --dts FILE`** holds a board and its SoC file against a device tree:
  each value as in the tree, different, or not in any tree. The SoC file a tool starts explains
  itself: every value is written with what the template says of its key.
- **`tools/boards.py status NAME`** lists every feature for a board: on, ready to be listed, or the
  section it still waits for. `tools/boards.py check` validates every file and says which and why.
- **Drivers start by feature, not by a phone's name.** A board lists the drivers it may start in
  `features`. A driver asks for its feature after its boot argument. On a board that does not list it,
  the driver prints which line is missing and does nothing. A machine nobody described has no feature.
- **What was a constant in a driver and is a section now:**
  - in the board file: `backlight { }` (how brightness is set, its range and byte order), `thermal { }`
    (the floors), `buttons { }` (the keys' PMIC peripherals), `adsp { }` (firmware directory, memory, the
    gauge's units), `gpu { }` (the Adreno's profile), `wifi { }` (the radio, its early memory pool, the
    Wi-Fi DSP's memory, the radio's pins and its supplies' names), `mic { }` (the codec's reset pin),
    `camera { }` (the sensors' PMIC bus, and one `sensor@N { }` table a sensor: its chip, CCI
    master and pins, reset, MCLK, CSI receiver, supplies and lens motor's supply; the driver's table of
    four is gone; `darwin-sensor` and `darwin-front` say which sensors are Darwin's back and front cameras, and
    `upside-down` a sensor whose pictures come out turned half round), `touch { }` `coordinates` (the controller's range), `wifi { }` `board-data` and
    `regdb` (the radio's files),
    `screen { }` (the status bar's shift past the camera), `loader { }` (framebuffer base,
    `uefi-charger`), `boot { }` (the boot tool's defaults);
  - in the SoC file: `platform { }` (interrupt controller, cores, PSCI, I/O window), `clocks { }`,
    `display { }` (DSI controller, the panel's interface), `spmi { }` (PMIC arbiter), `rpmh { }`, `thermal { }`
    (TSENS), `cpufreq { }` (the EPSS block and its groups of cores), `smmu { }` `audio-stream`,
    `wifi-stream`, `camera-stream` (the SMMU streams the parts' DMA comes in on), `lpass { }`
    `swr-tx-pins` and `swr-tx-slew` (the microphones' SoundWire pins).
- **Phones are device packages, kept apart from the code.** The main branch describes no phone: it has
  the emulator's board (`devices/qemu-virt/`) and the template. A phone is a directory with its
  `board.dts` and README on the `devices` branch. `tools/device.py`
  brings one into a checkout and takes it out again: `available`, `add`, `list`, `remove`, `export`
  (`make device DEVICE=NAME` is `add`). Packages elsewhere are found through `HAKIOS_DEVICES`.
- **The board tables are build outputs.** `tools/boards.py gen` writes the kernel's table, the loader's
  and the injected libraries' into `build/boards/`, and every build runs it. None is in the repository.
  The kernel, the loader and the libraries know the boards whose packages were present when built.
- **A porting guide.** [docs/porting.md](docs/porting.md) goes from the device tree to sending the
  package back in eight steps, with the log line that shows each piece worked, and ends with what is
  still tied to the first two phones. `tests/porting_test.sh` runs its tool and data steps (1, 3, 8 and
  the data side of 4 and 5) on a made-up phone with a made-up SoC, on every push.
- **What each feature needs, key by key.** [docs/features.md](docs/features.md) is written by
  `tools/boards.py features` from the tables the checker uses: for every feature, the sections and keys
  of the SoC file and of the board file it needs first, and the boot argument that starts it.
- **A catalog of drivers.** [docs/drivers.md](docs/drivers.md) lists every driver with its part, its
  feature and boot argument, and how tied it is to one phone.

### Building or changing the kernel

- **What the drivers share is written once.** Next to the headers they already shared (registers,
  waits, RPMh, the command database, pins), `hakios_cache.h` has the cache maintenance and the barrier
  that seven drivers each wrote out, `hakios_smmu.h` the SMMU's registers that four defined, `hakios_scm.h`
  the call to TrustZone that three made, and `hakios_kernel.h` the kernel's own functions that 32
  declared for themselves. On a host the first two hold only the compiler, so a driver that uses them
  compiles into a host test as it is.

- **The sources are ordinary files.** The kernel was one 75,000-line patch against Apple's release with
  25 patches stacked on it. It is three things now: `kernel/UPSTREAM` pins the Apple release by commit;
  `kernel/overlay/` holds the files HakiOS adds, at their place in the XNU tree; `kernel/patches/`
  holds the changes to Apple's own 89 files as 7 patches by topic (`kernel/patches/series` says which
  file belongs to which).
- **Tools for the round trip.** `kernel/assemble.sh` makes the tree that is compiled.
  `kernel/export.sh` takes work done in a tree back. `kernel/verify.sh` checks that assembling and
  exporting give back exactly what is committed. `kernel/import-patch.sh` takes a patch written against
  the tree into the overlay and the topic patches. An update never overwrites work in a tree that has
  not been exported.
- **The kernel builds without theos' toolchain.** `kernel/build.sh` takes LLVM's own Mach-O linker and
  tools (`ld64.lld`, `llvm-libtool-darwin` ...) when theos is not installed, with the Darwin SDK's headers
  alone (`DARWIN_SDK`): `kernel/host/llvm` makes up for the two places they differ from cctools. It needs
  clang 19 or newer and GNU awk, which the build now says when they are missing. LLVM's linker does not
  lay the kernel out as cctools' does (`-image_base`, `-segaddr`): such a kernel is a check that every
  file compiles and links, not one to boot.
- **`kernel/build.sh` is the one command**, with theos installed or LLVM's tools. It assembles or updates the tree,
  fetches and builds its host tools (`kernel/deps/stage1.sh`), makes the board table and builds. A
  separate darwin-renoir checkout is no longer needed; it is still used if it is there.
- **A syntax check without the Darwin toolchain.** `kernel/syntax-check.sh` compiles what HakiOS adds
  for syntax and types with the machine's own clang, in seconds against a tree that is there, and for
  the formats of what it prints, which the real build treats as errors. It does not build or link
  the kernel.
- **What drivers share is written once.** Eleven drivers each had their own copy of the function that
  writes to the loader's log: `hakios_say()` in `hakios_log.h`. Fourteen had their own register
  access, in three variants: `hakios_mmio.h` has the three and says when each is needed. Waits and the
  clock are `hakios_time.h`. The PMIC arbiter's protocol, written out in three drivers, is
  `hakios_spmi.h`; the lookup in RPMh's command database, written out in eight, is
  `hakios_cmd_db.h`; a request to RPMh is `hakios_rpmh.h`; the SoC's pins are `hakios_tlmm.h`, and I2C by hand on two
  of them, which three drivers had, is `hakios_i2c_gpio.h`. What
  the audio DSP's driver and the Wi-Fi's both had of the way to another processor is shared as
  well: the shared memory (`hakios_smem.h`), SMP2P's items (`hakios_smp2p.h`), the arithmetic of
  GLINK's queues and the length of its messages (`hakios_glink_queue.h`), QRTR's framing
  (`hakios_qrtr_frame.h`), the checks of a DSP's firmware image (`hakios_mdt.h`), the mailbox's and the
  doorbells' registers and whether the mailbox is usable (`hakios_ipc.h`). Each that has logic has a test
  of its own; the four checks that wrapped in both drivers were the first to move there.
- **The loader is parts by topic.** `loader/loader.c` was one file of 5100 lines; it is the entry
  point and a list of 24 parts in `loader/src/` now (the device tree's writer, the kernel's memory,
  the USB commands, the storage, ...), still one compilation unit. The built `loader.efi` is the
  same byte for byte.
- **The loader's plain-C parts run on the host**: its memory functions, the device tree's writer, the
  NVRAM image, the payload's directory, the choice of the kernel's memory from a firmware's map, and
  the reader of Android's partition description (`tests/loader_test.c`, `loader_window_test.c`,
  `loader_dsu_test.c`). A port's own memory map can be tried there.
- **One list starts every driver.** The starts were spread over the touch queue's file, the audio
  device's and `bsd_init()`; they are `bsd/dev/hakios_drivers.c` now, in the same order, and a new
  driver is one line there. A test runs the list on every test board.
- **One file, one job.** The CPU frequency driver also voted for the memory's bandwidth and measured
  memory speed; those are `hakios_ddr.c` and `hakios_bench.c` now. It and the keys' driver work
  from a board's data and are named so: `hakios_cpufreq.c`, `hakios_buttons.c`.
- **The partition a write may reach is the board's.** The loader and the kernel each had the GPT
  entry of the first phone's own partition in their code: nothing could be written on another phone
  without changing both. It is `storage { }` of the board file; a board without it is never written.
  `tools/boards.py check` refuses an entry that cannot be one, and the loader's answer to "is this
  the partition?" has a test in which every field is off alone.
- **A touch controller on another serial engine or other pins is a board file.** The two touch
  drivers had the engine's address, its clocks and votes, the pins and the supplies' names of the
  phone each was written on. They are `touch { }` now. A test compiles each driver on the host and
  compares every value with the number that was in its code, and with those of a board wired
  otherwise; the drivers before the change pass the first and fail the second.
- **Three parts on other wiring are a board file.** Where the flashlight's module is on the PMICs' bus
  (`torch { }`), which pins the vibration motor's chip is on, with the motor's calibration
  (`haptic { }`), and the speaker amplifiers' bus, addresses and interrupts with the four pins the
  sound goes to them on, those pins' function and the audio DSP's TDM interface (`speaker { }`) were in
  the drivers. Each driver's test runs a second phone with the part elsewhere.
- **The speaker amplifiers' driver runs on the host.** Two modelled amplifiers on a modelled bus and a
  configuration file built in the test: what is written and in which order, the volume's steps, the
  monitor, and that both are powered down at once on a missing clock, a fault or a write that does
  not read back (`tests/speaker_driver_test.c`, 22 scenarios, on the host and under qemu-aarch64).
- **A guide to writing a driver**, with an example driver and its test that `make check` builds and
  runs: [docs/writing-a-driver.md](docs/writing-a-driver.md).
- **Where the chip's blocks are is data in every driver that was touched.** The pin controller, the
  clock controller, RPMh, the SMMU, the PMIC arbiter and the way to the other processors were
  constants in up to eight drivers each; they are `pins { }`, `clocks { }`, `rpmh { }`, `smmu { }`, `spmi { }`
  and `ipc { }` of the SoC file. No driver names an address of a chip any more, and `make check`
  fails one that does. What is left in some part drivers is the part's own wiring.
- **No board names in code.** Nothing compares the board's name any more: the name is looked up once,
  for the board's row. [CONTRIBUTING.md](CONTRIBUTING.md) has this and the other rules that keep the
  project portable, and says where each kind of change goes.
- **Every boot argument is listed.** `tools/bootargs.py` writes [docs/boot-args.md](docs/boot-args.md)
  from the comments beside the code: 236 arguments, 34 of them without a description yet.
- **Pinned inputs in one place.** `components.toml` lists the sixteen things fetched from elsewhere at
  their exact versions, and the lint checks it against the scripts that fetch. The macOS SDK archive is
  checked against its SHA-256 before unpacking.
- **Build scripts run on any machine.** The Darwin image scripts named one home directory. They take their
  directories from where the checkout lies or from the environment.

### What you will notice

- **Bring the phone's package in before you build**: `tools/device.py add renoir`. A kernel or loader
  built with no package knows only the emulator's board and starts no driver of a phone.
- **On Renoir, the drivers wired for the first phone's parts no longer start.** The speaker, microphone,
  haptic, flashlight, camera and Wi-Fi drivers have no Renoir code path. Asked for there, they print
  which line is missing instead of writing to another phone's pins. Renoir's boot defaults had them off.
- **The assembled tree is `../xnu-12377-hakios`.** A tree made by the earlier `apply.sh` is refused.
- **The boot tool reads its defaults from the board file.** `darwin/tools/boot26.py` takes `--board NAME`,
  or the one phone whose package is present.
- **A branch or patch from the old layout** is carried over with `kernel/import-patch.sh`.
  [docs/migrating.md](docs/migrating.md) has the steps.

- **The loader refuses a system image that does not lie in userdata, however it is described.** The
  check of an extent's start and length added the two, and a start or a length near 2^64 made the
  sum small: such an extent was taken. Found by the new test of that reader, which runs it on
  descriptions built to be wrong.
- **An item of the processors' shared memory is given to a driver only if it lies inside it.** The
  reader of that memory (`hakios_smem.h`) followed no header outside the area, but it gave out an
  item as its entry described it: an entry with more padding than length, or pushed past its
  partition by its header's padding, came out with an address or a length outside the area, which
  the Wi-Fi's and the audio DSP's drivers then read. Such an entry ends the walk now.
- **The line that says a board's features is not cut**: its buffer was one byte from full for the
  phone with the most parts.
- **The backlight no longer sleeps through a request.** A level, or the display switched on,
  asked for in the moment between the driver's thread reading its state and going to sleep woke
  nobody, and the thread slept until something else poked it: the panel stayed at the old level, or
  dark. Found when a check of the host tests failed once; the fake kernel can now put a request
  into exactly that moment, and the test does.
- **The sparse image reads nothing from outside itself.** A table entry that pointed past the image's
  blocks was followed into whatever memory lay after the RAM disk, and the check that the image fits
  the RAM disk added offsets and sizes that could wrap. Both are refused now; a damaged image is
  reported.
- **The loader's USB commands check their bounds without sums that wrap.** `oem ramdisk patch` followed by
  `oem ramdisk none` left the patch armed with no image under it, and the next `download:` was allowed a
  size that wrapped and wrote past the memory window; `oem read` took an offset near 2^64 as inside the
  partition. Both are refused now.
- **What the DSPs and the radio's driver send is held to the bounds of what was received.** A QRTR
  datagram's size near 2^32 passed the check against its message (in the audio DSP's driver and the
  Wi-Fi's), an APR packet smaller than its own header made its payload's length wrap, and a network's
  name or elements longer than their arrays were read past the record, the name copied out to the
  process that asked. Each is held to its bounds now.
- **Barriers keep the order they are written for.** Forty-nine `dsb`, `dmb`, `isb` and `dc` in the
  kernel's drivers and the loader did not tell the compiler they touch memory, so it was free to move the
  writes into a DSP's shared memory, a camera frame's reads, or the values handed to the loader, across
  them. They do now, and a repository check keeps it so.
- **An APFS image of several volumes shows all of them.** `mkapfs.py` wrote a container's limit of
  volumes from its size alone, so a small image of two volumes said it held one, and the kernel's reader
  saw only the first.
- **The Darwin image tool does not stop on a file that only looks like a Mach-O.** A Java class file
  begins with the fat Mach-O magic; the tool read past what it had read of it and ended on an error.
- **The crash line of a process gives its ESR and slide whole**: they were printed 32 bits wide.
- **A loader's storage page that no request could be made with is not used.** A buffer that is not
  whole blocks, a block size of zero or not a power of two, a partition size that overflows: the kernel
  divided and counted with them. It refuses such a page now, with the reason.
- **With no free memory for the kernel the loader says so**, instead of asking the firmware for a
  window of no pages.
- **`ddr:` no longer speaks on every boot** of a board without the memory's vote.
- **The touch drivers' messages reach the loader's log as they are said**, like every other
  driver's, not only when a run is over.

### Repository layout

| Before | Now |
| --- | --- |
| The single kernel patch, `apply.sh`, the numbered patches | `kernel/overlay/`, `kernel/patches/`, `kernel/assemble.sh` |
| `renoir/kernel/build.sh` and its environment script | `kernel/build.sh`, `kernel/env.sh` |
| `renoir/userland/`, `renoir/tools/` | `darwin/userland/`, `darwin/tools/` |
| `renoir/docs/`, `renoir/tests/` | `docs/`, `tests/` |
| `base/xnuloader/`, `base/xnu-deps/` | `loader/`, `kernel/deps/` (links stay in `base/`) |
| Lists of phones inside drivers and the loader | Device packages on the `devices` branch, `soc/` |
| Files, functions, `/dev` nodes, the small image's libraries and mount point named after the first phone, `renoir_fts*`, `devices/*.toml` | `hakios_*`, `libhakios*`, `/hakios`, `board.dts` and `soc/*.dtsi`; the first phone's package lists every former name |
| The first phone's own files in `base/` (its xnu-8796 kernel, UEFI platform, device profile, the build and tools of that kernel) | `legacy/` of its package on the `devices` branch |

The full table is in [docs/migrating.md](docs/migrating.md). [docs/architecture.md](docs/architecture.md)
says how the parts fit together, and [docs/README.md](docs/README.md) is the index of the documentation.

### Checks

- **`make check`** runs everything that needs no phone, no Darwin files and no kernel toolchain: the lint,
  the validation of the board and SoC files, whether `docs/boot-args.md` is current, the host tests.
- **The lint** (`tools/lint.sh`) checks that every script parses, runs shellcheck, checks that every
  link and repository path in the documentation exists, checks the pins of `components.toml`, looks
  for files that must never be published, and holds the Darwin side's copies of the kernel's device
  interfaces (`userland/userland/src/hakios_*.h`) to what the kernel's say.
- **The host tests** (`tests/run.sh`) run the drivers listed in [docs/drivers.md](docs/drivers.md)
  unchanged against models of their hardware, on the host and as arm64 code under `qemu-aarch64`: among
  them the backlight, the thermal limiter, CPU frequency, the memory vote through RPMh, the keys, the
  flashlight, the battery, the vibration motor, the speaker amplifiers, the wiring of the touch
  controllers, the cameras, the sound's pins and the microphones' blocks (the last two on a board and a
  chip unlike the first phones', which is how the sound driver was found watching that phone's pins), the sparse image and the storage driver, whose test holds that the loader is never asked for a write
  the port may not make, and Darwin's Wi-Fi requests, whose test holds that nothing a radio reports goes
  back to Darwin past the room Apple's layouts and the caller's buffer have. What the drivers share
  (the PMIC arbiter's protocol, the lookup in RPMh's command database) has tests of its own. They
  also test the kernel's board code, the board and package tools, the kernel source tools, the boot-argument reference and the
  porting guide, and build the loader. They use stand-in phones in `tests/devices/` and a made-up chip in
  `tests/soc/`.
  [tests/README.md](tests/README.md) lists each test.
- **CI** runs `make check`, `kernel/verify.sh` and `kernel/syntax-check.sh` on every push and pull
  request. The kernel is not built or linked there.

### New drivers and features

- **Each phone has an identity of its own, of the real forms.** The loader gave every phone the same
  serial number (`HAKIOS000000`), ECID and addresses, some of them spelling the first phone's name. It now
  makes them from one 64-bit seed (`loader/src/unit.c`): a serial number of 10 and an MLB serial of 17
  characters (digits and capitals without I and O, as Apple's randomised ones), a 52-bit ECID, unicast,
  locally administered Wi-Fi and Bluetooth addresses, a 48-byte manifest hash. `tools/identity.py NAME`
  makes a phone's seed once and keeps it outside the checkout; `darwin/tools/boot26.py` passes it
  (`identity=`), so a phone is the same device at every boot. Without a seed the loader takes a random
  one, from the firmware's generator where it has one, which now also fills the kernel's random seed.
- **Brightness and Auto-Brightness** (`hakios_backlight.c`, [docs/backlight.md](docs/backlight.md)).
  Brightness is set by MIPI DCS through the Qualcomm DSI controller, or in software on boards without a
  description. The side button and the idle timer make the panel dark or dimmed. Tested on the host and
  under `qemu-aarch64`. Not yet verified on a phone. The default, `backlight=1`, only reads. There is
  no light sensor driver yet: the light reading is the sysctl `hw.backlight.lux`.
- **A thermal limiter** (`hakios_thermal.c`, [docs/thermal.md](docs/thermal.md)). It reads the
  temperature sensors and, when hot, lowers the CPU groups' frequency and caps the panel's brightness.
  Tested on the host and under `qemu-aarch64`. Not yet verified on a phone. The default, `thermal=1`,
  only logs the hottest sensor; `thermal=2` holds back.
- **The Metal plug-in gate** is back in the 12377 kernel, behind `metal=1`. It is one step of the plan
  in [docs/metal-glass.md](docs/metal-glass.md). The Metal device plug-in is not packaged for Darwin 27.

### Not changed, and not yet verified

Not changed:

- Sound, microphones, camera and Wi-Fi are still written for the first phone's parts: one codec, one radio, the
  sensor chips of its cameras. Which pins they are on, the radio's supplies and how each camera sensor is
  wired are board data now; how each part is spoken to is not. They are the largest share of the work on a new phone.
- The GPU driver's sequence has only been run on the two phones' chips: another generation of Adreno
  needs work in the driver. The memory bandwidth vote expects the RPMh layout both phones have.
- The first phone has not been booted with Darwin 26/27. The emulator's board is not set up for this kernel.

Not yet verified ([docs/building.md](docs/building.md) has the table):

- **A build with Apple's toolchain and a boot have not been repeated since the restructuring.** They were
  last done by the team before it; `make kernel-check` compiles and links the kernel with LLVM's tools
  after every change, which is a check and not a kernel to boot. Renoir's boot to the home screen was verified on the phone on 2026-11-10.
  The assembled tree is byte for byte what the previous patch series produced, but the build's first
  steps changed.
- **No driver whose constants became board data has run on a phone since.** The GPU's profile, the
  audio DSP's firmware and memory, the keys, the PMIC arbiter and RPMh are named there. The values are
  the ones the code had, the tests hold them, and the drivers compile. That is all that is known.
- **The loader** compiles on every push. Its last run on a phone was before its board table was
  generated and before `uefi-charger` replaced a board's name in its code.
- **`kernel/deps/stage1.sh` and the whole kernel build** have been run end to end once since, on Ubuntu
  24.04 with LLVM 20's Mach-O tools instead of theos': every file compiled, HakiOS' without a warning,
  and the kernel linked with nothing undefined. With theos' cctools, as the team builds, it has not been
  repeated; a kernel linked by LLVM's linker is not laid out for a phone and has not been booted.
- **The syntax check is not a build.** Ten files and linking still need the real build.
- **The porting procedure has not been followed on a third real phone.** The test cannot show the
  hardware answering.
- **Renoir cannot be booted from a clean checkout.** Its resident loader, its recovery payload tool and
  the user-space root tree are in the team's working tree, which is not published.
  `darwin/tools/boot26.py` and the image scripts in `darwin/userland/` need it.

## Before the restructuring

The first generation ran Darwin 16.5 on the first phone and in QEMU. Its workspace is kept in
`base/`, and [base/README.md](base/README.md) describes it. Parts of it are still the only place for
something: the UEFI port, the Wi-Fi compatibility layer and the QEMU scripts. Its kernel is superseded
by `kernel/`. The notes on how Darwin 26 and 27 were brought to the home screen on Renoir, written before
the restructuring, are in [docs/darwin26-boot.md](docs/darwin26-boot.md).
