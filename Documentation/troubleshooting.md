# Troubleshooting

What a message or a symptom means and what to do about it, sorted by where it shows. Every entry
comes from a document or from the source that prints the message, and points at it.

Look at these first:

```sh
tools/boards.py check            # every board and SoC file that is present: says which file and why
tools/boards.py status NAME      # one board: what is on, what could be, what each of the rest waits for
```

On the phone, the kernel's `board:` line in the log says which board it found, and `sysctl hw.board`
in a running system shows the same (`name`, `soc`, `description`, `features`).

Below, `NAME`, `SOC`, `DIR`, `FILE`, `KEY`, `N` and `...` stand for what the real text has there.

## The board files and the tools

`tools/boards.py check` prints one line for each problem: the file (and section), a colon, the reason.
The builds and `make check` run the same check and stop on the same lines. Every key is explained in
`arch/arm64/boot/dts/_template/board.dts` and `arch/arm64/boot/dts/_template/soc.dtsi`.

| What you see | What it means | What to do |
| --- | --- | --- |
| `FILE: line N: ...` | A board or SoC file is not written as the format wants (arch/arm64/boot/dts/README.md, "The format"): a property without its `;`, a value that is neither `"..."` nor `<...>`, a label or a reference. | Fix the syntax on that line. Nothing else of that file is checked until it parses. |
| `unknown key "KEY" (known: ...)`, `unknown section SECTION { } (known: ...)` | The file has a key or a section this checkout does not know: a typo, or a package written for newer code. | Compare with the list in the message. A package and the code change together when a key is added ([arch/arm64/boot/dts/README.md](../arch/arm64/boot/dts/README.md#adding-a-kind-of-data)). |
| `"KEY" is missing`, `KEY = VALUE: ... expected`, `KEY = VALUE: one of ...` | A required key is not there, or a value is of the wrong kind. | The template says what each key takes. |
| `name = 'X': the directory is arch/arm64/boot/dts/NAME` | `name` in the board file is not the name of its directory. | Make the two the same. Names are lower-case letters, digits and dashes. |
| `there is no soc.dtsi beside it`, `soc = 'SOC': its soc.dtsi is 'OTHER'` | The package has no SoC file, or the board names another SoC than its `soc.dtsi` does. | Copy the `soc.dtsi` of a package with that SoC, or write one from `arch/arm64/boot/dts/_template/soc.dtsi` ([porting.md](porting.md), step 1); correct whichever name is wrong. |
| `feature "X" is not one the kernel knows (...)` | `X` is not a `HAKIOS_HAS_*` of `drivers/board/board.h`: a typo, or a package that needs newer code. | Use a name from the list in the message. |
| `feature "X" needs SECTION { } in soc.dtsi`, `feature "X" needs SECTION { } in the board file`, `feature "wifi-wcn6750" needs dsp-memory and dsp-memory-size in wifi { } of the board file` | A board cannot list a feature its files have no addresses for. The same goes for `needs channels in spmi { } of ...`, `needs intf1 in display { } and clocks { } in ...` and `needs a command-mode panel (backlight { } says video)`. | Fill in the section, or take the feature out of `features`. `tools/boards.py status NAME` says the same for every feature (`needs ...`, or `on: STILL NEEDS ...`). |
| `type "dcs" needs "KEY"`, `backlight type "dcs" needs SECTION { } in soc.dtsi`, `backlight type "dcs" needs gcc-disp-ahb-cbcr, ... in clocks { } of soc.dtsi` | DCS brightness needs the panel's range and mode, the DSI controller's address and three clock offsets that no device tree has. | Until you have them, set `type = "soft"`, which works anywhere ([porting.md](porting.md), step 1). |
| `loader { } needs platform { } in soc.dtsi`, `loader { } needs io-base and io-size in platform { } of soc.dtsi`, `io-base must not be 0 (XNU then takes the machine for one without a SoC)` | The loader's description of the machine comes from `platform { }` of the SoC file. | Fill it in. `io-base` and `io-size` are one window over the SoC's peripherals, not starting at 0 ([porting.md](porting.md), step 1). |
| `more than one board with loader { } and no fb-base (...): the loader tells boards apart by it, and only the emulator goes without` | A phone's board file has a `loader { }` section without `fb-base`. | Leave `loader { }` out until you have the address: build and start the loader, read its line `display W x H at ADDRESS`, then add `loader { }` with `fb-base` ([porting.md](porting.md), step 3). |
| `fb-base 0x... is also that of NAME` | Two boards that are present give the same framebuffer address, and the loader tells boards apart by nothing else. | Check the value against the loader's `display W x H at ADDRESS` on your phone. |
| `domain_cores adds up to N cores, platform { } has N`, `floor_mhz: one value for each of the N groups of cores (...)` | The groups of cores in `cpufreq { }`, the number of cores in `platform { }` and the board's thermal floors must agree. | Correct `domain-cores` (the cores of each frequency group) or `floor-mhz` (one floor for each group). |
| `no board "NAME" (tools/boards.py list)` | `status` or `show` was asked for a board whose package is not present. | `tools/device.py add NAME`, or name the directory it is in with `--devices DIR` or `HAKIOS_DEVICES`. |
| `note: ...` after `tools/boards.py new --dts FILE`, for example `note: [SECTION] not found in the tree` or `note: no panel node in the tree (it may be in the dtbo: dump that as well)` | What the device tree did not say. The files were written all the same. | Fill those values in by hand. With several panels pass `--panel NODE`. The live tree of a running Android has the overlays applied ([porting.md](porting.md), "What you need from the phone"). |

## Device packages

`tools/device.py` brings a phone's package into `arch/arm64/boot/dts/` from the `devices` branch, or from
`--from SOURCE`: a directory, a branch or a commit. More: [arch/arm64/boot/dts/README.md](../arch/arm64/boot/dts/README.md).

| What you see | What it means | What to do |
| --- | --- | --- |
| `... has no package "NAME" (it has: ...)` | The source has no directory `NAME` with a `board.dts` in it. | `tools/device.py available` lists what a source has; `--from SOURCE` names another one. |
| `devices is neither a directory nor a branch or commit of this repository (packages are kept on the branch "devices": git fetch origin devices)` | The branch could not be fetched from `origin` and this clone has none of that name. With `--from X` the line starts with `X`. | `git fetch origin devices`, or give `--from` a directory that has the package. |
| `arch/arm64/boot/dts/NAME is there and was not brought in by this tool: it may be work of yours. Move it away first (or tools/device.py remove NAME)` | The directory was made by `tools/boards.py new` or by hand, and `add` never replaces such work. | Keep the work (`tools/device.py export NAME DIR`, or move the directory), then add again. |
| `the package does not fit this checkout: ...` | The board check found problems in the package, printed above the line: it has a key or a feature this checkout has not, or lacks one the checkout needs. The package was copied all the same. | Bring the checkout up to date, or take the package as another commit has it (`--from COMMIT`). Until it fits, `tools/device.py remove NAME`: every build stops on a board that fails the check. |
| `arch/arm64/boot/dts/NAME is not right yet: not exported` | The board check found problems in the package, printed above the line. What is sent is what was validated. | Fix them (first section), then export again. |
| `DIR is not a directory (a checkout of the devices branch: git worktree add DIR devices)` | `export` copies into a checkout of the `devices` branch that is there already. | `git worktree add ../hakios-devices devices`, then `tools/device.py export NAME ../hakios-devices` ([porting.md](porting.md), step 8). |
| `URL: the repository cannot be cloned: ...` | `--from` named a repository's address that git could not clone: the address, the network, or a private repository without the credentials git uses. | `git clone URL` by hand says why; a checkout of it on this machine works too (`--from DIR`). |
| `DIR is there and not empty: a repository is made in a new directory` | `tools/device.py init` makes a phone's repository only where there is nothing. | Another directory, or move what is there. |
| `git status` shows nothing of `arch/arm64/boot/dts/NAME/` | Git ignores packages in a checkout of the main branch: they are kept on the `devices` branch. | Send the package with `export`, as above. A new SoC file is an ordinary file of the main branch ([CONTRIBUTING.md](../CONTRIBUTING.md)). |

## The kernel's sources (assemble, export, verify)

Each line starts with the script's name (`assemble.sh: ...`). An assembled tree is Apple's release with
`kernel/overlay/` copied in and `kernel/patches/` applied; [kernel/README.md](../kernel/README.md) has
the two ways of working on it.

| What you see | What it means | What to do |
| --- | --- | --- |
| `DIR is not a tree made by assemble.sh`, or from the build `DIR is not a tree made by kernel/assemble.sh (one of the old apply.sh, perhaps): move it away or set XNU_SRC` | The directory is not a git repository with the tag `upstream`: a tree of the earlier `apply.sh`, or something else. | Move it away, or point `XNU_SRC` elsewhere ([building.md](building.md), [migrating.md](migrating.md)). |
| `DIR is of another release than kernel/UPSTREAM names: make a new tree` | The tree was made from another commit of Apple's than `kernel/UPSTREAM` pins now. | Move the tree away. The next `kernel/build.sh` makes a new one. |
| `... has TAG at COMMIT, not at COMMIT (kernel/UPSTREAM): not using it` | The repository the release was fetched from has the tag at another commit than the pinned one. No tree is left behind. | Fetch from Apple's (no `--from`), or give `--from` a clone on this machine that has the pinned commit. |
| `DIR has work that is not in the repository yet (kernel/export.sh DIR takes it over): left as it is` | Not an error. The tree was edited and the repository has nothing newer; an update never runs over such work. | Go on building. `kernel/export.sh DIR` when the work is ready. |
| `DIR has work that is not in the repository, and the repository has changed since the tree was made:` and a list of files | Both sides changed, and an update would overwrite the tree's side. | `kernel/export.sh DIR` and settle the two in the repository (`git diff`), or throw the tree's work away with `kernel/assemble.sh --update --discard DIR`. |
| `patches/NAME.patch does not apply to TAG` | A patch of the series does not apply to the release with the overlay and the patches before it, for example after an edit by hand. | Restore the patch from git. A change to one of Apple's files is made in a tree and exported, never in the patch ([CONTRIBUTING.md](../CONTRIBUTING.md)). |
| `changed files of Apple's that no line of kernel/patches/series claims:` and a list of files | The export found changes to files that belong to no topic. Nothing was written. | Add each file to the topic it belongs to in `kernel/patches/series`, then export again. |
| `kernel/overlay is not what an export of the assembled tree gives (above)`, `kernel/patches/NAME is not what an export of the assembled tree gives (above): assemble, then export` | `kernel/verify.sh`: a patch was edited by hand in a way the tools would not write, or a file sits in the wrong place. The difference is printed above the line. | Assemble a tree, run `kernel/export.sh` on it, and commit what that gives. |
| `DIR has work that is not in the repository: export it first (kernel/export.sh DIR)` | `kernel/import-patch.sh` applies no patch to a tree with work that was not exported. | Export that work, or discard it, then import again. |
| `NAME.patch does not apply to the tree as the repository has it (nothing was exported)` | The patch was written against other sources. The tree is left as far as the patches before it got. | Look at it with `git -C DIR status`, correct the patch, import again ([migrating.md](migrating.md)). |
| `FILE is not a file of the tree (a path as in kernel/overlay)` | `kernel/syntax-check.sh` takes paths as they are in the tree. | `bsd/dev/board/thermal.c`, not the path in the repository. |
| `FILE compiles here: take it off kernel/syntax-check.skip` | A file on the list of those only the real build can check compiles after all. The check fails so that the list says what is true. | Remove its line from `kernel/syntax-check.skip`. |
| `N of N files failed (above)` | The first errors of each file are above the line. `N C++ files not checked (no --driverkit)` in the `ok:` line is not a failure. | Fix the errors. Only a file that includes what the real build alone has belongs on the skip list. `--driverkit DIR` checks the C++ files as well. |

## Building

What each build needs is in [building.md](building.md). The kernel, with LLVM 20's tools and the SDK outside
theos, and the Darwin 27 libraries and small image were built from this layout (arch/, fs/, components/).
If the kernel's build fails, look first at steps 1 and 2 of `kernel/build.sh` and at the ten files
`kernel/syntax-check.sh` cannot check ([building.md](building.md), "What has been verified, and where").

| What you see | What it means | What to do |
| --- | --- | --- |
| A build stops after lines like `arch/arm64/boot/dts/NAME/board.dts: ...` | Every build makes its table of boards with `tools/boards.py gen`, which takes no board that fails the check. | First section. Or take the package out: `tools/device.py remove NAME`. |
| `build.sh: boards: ...: 1 boards (qemu-virt); ...` | The kernel is built with no phone's package. It will know only the emulator's board: on a phone it finds no row for itself and starts no driver of a phone. | `tools/device.py add NAME`, or set `HAKIOS_DEVICES`, and build again. The same holds for `arch/arm64/boot/build.sh`. |
| `build.sh: no iPhoneOS16.5.sdk at DIR (theos' sdks/, or DARWIN_SDK=DIR)` | The kernel's build uses that SDK's headers, and they are not where it looked. | Install theos with that SDK in `~/theos` (or set `THEOS`), or point `DARWIN_SDK` at a copy of the SDK's directory ([building.md](building.md)). |
| `build.sh: no Mach-O linker: theos' toolchain in DIR, or LLVM's (ld64.lld) in XNU_CLANG_DIR` | Neither theos' cctools nor LLVM's Mach-O linker was found. | Either one: theos with its Linux toolchain, or LLVM 19 or newer with `ld64.lld` and `XNU_CLANG_DIR` pointing at its `bin` ([building.md](building.md)). `TOOLCHAIN=cctools` or `TOOLCHAIN=llvm` chooses when both are there. |
| `kern/bits.h: ... use of undeclared identifier '__builtin_clzg'` (or `__builtin_ctzg`) | The kernel is compiled with a clang older than 19. | A newer clang beside the system's, and `XNU_CLANG_DIR` pointing at it (Ubuntu 24.04: `clang-20`, `/usr/lib/llvm-20/bin`). |
| `build.sh: GNU awk (gawk) is needed: ...`, or `init_sysent.c: error: expected expression` hundreds of times | The syscall table is generated with awk, and only GNU awk writes it right. | Install `gawk`. A build from before this check made the table with the system's awk: remove `BUILD/obj/*/bsd/*/init_sysent.c` from the tree and build again. |
| `stage1: not installed: ...` | Programs the dependency step needs are not on this machine. | Install the packages [building.md](building.md) lists for the kernel. |
| `stage1: ... is not the archive this was written with (its SHA-256 differs): not unpacked`, `build.sh: ...: unexpected checksum` | A downloaded macOS SDK archive is not the one the script pins. Nothing was unpacked. | Run the build again: the archive is fetched anew. If it differs again, the file at that address is no longer the pinned one (`components/upstream.dts` lists the pins): report it. |
| `stage1: cannot fetch ... from ...`, `download failed: ...` | The first build fetches Apple's open sources at pinned commits and two SDK archives. | It needs the network once. What is there already is left alone, so run it again. |
| `stage1: mig did not build, see DIR/mig-build.log`, `stage1: tcsh did not build, see DIR/tcsh-build.log`, `build.sh: exporthdrs failed, see DIR/exporthdrs.log` | A step failed, and its output is in the file named. | Read the file. Its last lines belong in a report. |
| `System image exceeds fastboot's 4 GiB - 1 download limit` | `darwin/tools/mkimage26.py`: fastboot sends a download's size in eight hex digits. | Build the image with `--prune` ([darwin26-boot.md](darwin26-boot.md), Darwin 27, item 1). |
| `inject.c: fatal error: 'ptrauth.h' file not found` from `darwin/userland/inj26.sh` | `xnu-clang` ran Ubuntu's `/usr/bin/clang` (18), which has no `<ptrauth.h>`: the kernel was built before `kernel/build.sh` kept the clang it used in `$XNUDEPS/host/clang-dir`. | Build the kernel again, or set `XNU_CLANG_DIR` for the scripts as for the kernel. |
| `'CoreFoundation/CoreFoundation.h' file not found` from the scripts under `darwin/userland/` | The SDK is not in theos' `sdks/`. | `DARWIN_SDK=DIR`, as for the kernel. |
| `pacstrip refused FILE: not an arm64e Mach-O` | A library was linked as plain arm64; dyld would not load it in an arm64e process. | Leave `DARWIN_LD` unset or point it at cctools' `ld64` ([building.md](building.md), "Darwin's libraries"). A file of your own (`EXTRA_BIN`) is built with `-arch arm64e`. |
| `no ld64 at FILE ...: ours are linked by ld64.lld, as arm64` | Not an error: there is no cctools ld64, and the libraries are built the way that works without it. | Nothing; `DARWIN_LD` names an ld64 when there is one. |
| The scripts under `darwin/` stop at a missing `$IOS/...` file | They take an unpacked IPSW (`darwin/tools/unpack.sh`: `rootfs/`, `cryptex/`, `BuildManifest.plist`, `var.skel`) beside the repository, which is never published. | Unpack the IPSW first ([darwin26-boot.md](darwin26-boot.md)). `make check` and the kernel build do not need it. |

## At boot: the kernel's log

The kernel's log comes back through the loader. Boot first with one core (`cpus=1`) and no hardware
feature on ([porting.md](porting.md), step 4).

| What you see | What it means | What to do |
| --- | --- | --- |
| The loader prints `platform qemu-virt` on a phone | The framebuffer the UEFI reports matches no board's `fb-base`, and such a machine is taken for the emulator. Or the loader was built without the phone's package. | Compare its line `display W x H at ADDRESS` with `fb-base` in `loader { }`. `tools/device.py list`, then `arch/arm64/boot/build.sh` again ([arch/arm64/boot/README.md](../arch/arm64/boot/README.md)). |
| The loader prints `no free range of 32 MB in the firmware's memory map: the kernel has nowhere to run`, or `phys window ..., claim status 8000...` | The firmware's memory map has no free range the kernel can be given, or the firmware refused the range the loader chose. The kernel gets the largest free range, clear of the payload, in whole blocks of 32 MB. | `fastboot oem memmap` prints the map as the loader sees it. Put your phone's ranges into a scenario of `workbench/loader_window_test.c` to see what is chosen and why; `PAYLOAD_BASE` and `PAYLOAD_MAX_SIZE` (`arch/arm64/boot/src/conventions.h`) say where the payload is taken to be. |
| `board: not one of the N this kernel knows (/chosen/darwin-board): no device of a phone is touched` | The loader passed another name or none, or the kernel was built without the phone's package. `hw.board.name` is `unknown`, no feature is on, and brightness is the `soft` backend. | Look at the `build.sh: boards:` line of the kernel's build and at the loader's `platform` line. Bring the package in and build again. |
| `board: NAME (DESCRIPTION, SOC): no features listed` | The kernel found its row, and `features` is empty (the SoC is said when it has a name of its own, apart from the board's). This is what a first boot should say. | Turn hardware on one piece at a time (next section). |
| `a core of the last kernel is still running: no kernel is started over it (restart the phone: ./bench fresh)` | The run before left one of its cores on (`THE KERNEL LEFT CORE ... RUNNING` in its log, a kernel that hung, or a core that was started and never arrived): it executes from the window the next kernel would be copied into. | `./bench fresh` (or `./bench reload`). `scripts/phone-day.sh` does it before every run. |
| `the kernel's time was N s, and 180 s later it has not come back: taken for hung` | The kernel beats but its way back to the loader is stuck (a driver's stop holding a lock). | Report the log: its last lines say which driver it was stopping (`loader_return`'s order is in `arch/arm64/kernel/loader_return.c`). |
| `pac: the keys of this session's first run, again` | A second run in one session of the loader: the cores keep the keys first written to them, so every run of a session gets the same. | Nothing. A second kernel in one session used to die at once for want of this; until that is seen to work, restarting between runs (`./bench fresh`) stays the safe way. |
| A hang at the second core | PSCI. | Check `psci` and `core-shift` in `platform { }` of the SoC file ([porting.md](porting.md), step 4). |
| `...: disabled: this board's board.dts lists no "FEATURE"` | A boot argument asked for a driver whose feature the board does not list, or the kernel does not know the board. Deliberate: these drivers write to pins and PMIC registers. | `tools/boards.py status NAME`. List the feature if the phone has that part wired the same way. Renoir lists only its touch controller ([migrating.md](migrating.md), "What behaves differently"). |
| `touch: touchscreen=1, but this board's board.dts lists no touch controller ("touch-...")` (or `touchscreen=-1`) | Neither touch feature is in `features`. | The same part wired the same way: list it. Another part needs a driver ([porting.md](porting.md), step 6). |
| `ddr: no RPMh vote on this board (its board.dts lists no "ddr-rpmh")`, `ddr: no RPMh vote: the SoC file has no rpmh { }` | `ddr=` was passed without the feature, or without RPMh's addresses. | Fill in `rpmh { }` in the SoC file and list `ddr-rpmh`. `ddr=1` only looks. |
| `MiniIOMobileFramebuffer: panel scan: no readout counters on this board: frames written at once` | The board does not list `display-scan`, or its SoC file has no `intf1` in `display { }` or no `clocks { }`, or the display block could not be read. Frames are written on a timer, which works. | Nothing for a video-mode panel. For a command-mode panel: `display-scan` and `intf1`; then `panel scan:` lines with a period near 16.7 ms ([porting.md](porting.md), step 5). |
| `ramfs: files have half of the kernel's shared data range: pages go to the private data range` | `/private/var` (ramfs, in RAM) holds more than half of the kernel's shared data range in files, and its pages now come from the private one. The shared range is also where the kernel keeps what it shares with other domains -- the data of Mach messages among them -- and a kernel that let the files fill it saw SpringBoard and a dozen daemons die within a second ("Too many corpses being created"). | Nothing: this is the limit working. A later `ramfs: kmem_alloc refused a page` means both ranges are as full as files may make them, and writes fail with `ENOSPC`. |
| `lsd` (or `installd`, `pkd`, `fileproviderd`, `containermanagerd`) killed for its memory limit (a jetsam report with `per-process limit`), an application installed from a file that does not start | The daemon's own limit, from Apple's jetsam properties, is a few megabytes, and building the LaunchServices database from nothing takes more. The kernel raises those five daemons' limits to a floor: `memfloor: lsd[PID]: ... raised to 128 and 128 MB` in the log. | Without that line, the kernel predates it or was booted with `memfloor=0`. If it is there and the daemon is still killed, try a higher floor: `memfloor=256`. |
| Darwin 27 stays at the boot spinner with `fbscale=2` | `fbscale=2` does not work with Darwin 27. | Leave it out ([darwin26-boot.md](darwin26-boot.md), Darwin 27, item 9). |
| A driver behaves differently than before the restructuring | Constants that were in the drivers became data of the board and SoC files. | Compare `sysctl hw.board` and the driver's first log lines with the board file: every value is printed or can be read there ([building.md](building.md)). |

## Hardware, one piece at a time

A driver starts only with its feature listed and its boot argument passed. Most have a level that only
reads and logs: use it first (`backlight=1`, `thermal=1`; the buttons' driver only ever reads). The
order to try them in is [porting.md](porting.md), step 5; every driver is in [drivers.md](drivers.md),
every argument in [boot-args.md](boot-args.md). The GPU is not needed for a picture: Darwin draws in
software without it.

### Brightness ([backlight.md](backlight.md))

| What you see | What it means | What to do |
| --- | --- | --- |
| The panel does not follow the slider; the log has `backend dcs (backlight=1: looking only)` | Level 1 reads the DSI controller and logs it. It writes nothing. | `backlight=2`. |
| `backlight: board unknown, backend soft ...`, or the picture darkens while the panel's light stays | The kernel does not know the board, or `backlight { }` says `soft`: the display driver darkens each pixel. | For an unknown board see `board: not one of ...` above. For a panel that takes DCS brightness fill in `backlight { }`, and `clocks { }` and `display { }` of the SoC file. |
| `backlight: NAME: no complete DCS profile (DSI 0x..., N to N, N bytes): nothing done` | The backend is `dcs` (the board's, or `bl_backend=dcs`), but the DSI controller's address, a clock controller's address, the range or the number of bytes is missing or cannot be. | Fill in the three sections. `bl_dsi=`, `bl_min=`, `bl_max=` and `bl_bytes=` try values before they are written down. |
| `backlight: dcs: the display block is off or its registers have no clock: nothing done` | At the offsets the SoC file gives, the display's bus clock in the GCC did not turn on, the MDSS power domain reads as off, or the MDSS bus clock did not turn on. | Check `gcc-disp-ahb-cbcr`, `dispcc-mdss-gdscr` and `dispcc-mdss-ahb-cbcr` in `clocks { }` against Linux's `gcc-*.c` and `dispcc-*.c` for the SoC. |
| `backlight: dcs: not a DSI controller of 2.2 or later (no command FIFO): nothing done` | What was read at the address is not such a controller. The line before it shows the version read. | Check `dsi0` in `display { }` (the device tree's `mdss_dsi0`, "dsi_ctrl"). `bl_dsi=0x...` tries another address. |
| `backlight: dcs: the controller is off (the firmware did not leave the panel lit through it): nothing done` | The panel is not lit through the controller at that address (`OFF` in the line before). | The same: the address of the controller the panel is on. |
| `backlight: dcs: the controller runs a video-mode panel, the profile says command mode: going by the controller` (or the other way round) | `panel-mode` in `backlight { }` is not what the controller runs. The driver goes on in the controller's mode. | Correct `panel-mode`. |
| `backlight: dcs: brightness N NOT SENT in 20 ms (int ..., status ...): no more commands this run` | The controller did not report the command as done: the wrong controller or mode. DCS is not used again in this run. | Check `dsi0` and `panel-mode`. Go back to `backlight=1` and read the line `backlight: dcs: DSI controller at ...`. |

### Temperature ([thermal.md](thermal.md))

| What you see | What it means | What to do |
| --- | --- | --- |
| `thermal: this board's file has no thermal { } (arch/arm64/boot/dts/NAME/board.dts): nothing read` | The board file has no `thermal { }` section, or the kernel does not know the board. | Add `thermal { }` with `floor-mhz`, one value for each group of cores. The SoC file needs `thermal { }` and `cpufreq { }` first. |
| `thermal: this board's SoC file has no [cpufreq]: looking only` | `thermal=2` has no frequency block to hold back, and goes on as `thermal=1`. | Add `cpufreq { }` to the SoC file. |
| `thermal: th_clear < th_trip < th_critical does not hold (N, N, N): the defaults` | The `th_*` boot arguments contradict each other. 62.0, 70.0 and 95.0 °C are used. | Pass tenths of a degree, with clear below trip below critical. |
| `thermal: hottest sensor -273.0 C` | No sensor has a reading: none is valid, or all are outside −40 to 150 °C and ignored. The limiter then holds nothing back. | Check `tsens` in `thermal { }` of the SoC file (the device tree's `tsens_tm_physical`), and compare with Android's readings. |
| `thermal=2`, the phone is hot, and no `held back N of N` line | The limiter never raises a group above the row it found at start. Without `cpufreq=1` the groups stay at 300 MHz and it never writes. | Pass `cpufreq=1` as well. Under load (`cpuload=1000`) the line should appear around 70 °C. |

### CPU frequency

| What you see | What it means | What to do |
| --- | --- | --- |
| `cpufreq: disabled: this board's board.dts lists no "cpufreq-epss"` | `cpufreq=1` was passed and the board does not list the feature. | `cpufreq { }` in the SoC file, then `cpufreq-epss` in `features`. |
| `cpufreq: group N (...): the hardware is not in charge of its clock: left alone` | The group's block reads its enable bit clear: the firmware runs that group's clock itself, or `cpufreq { }` is not the block (a wrong `epss`, `domains`, or `block` of the other layout). | Check `cpufreq { }` against the tree's `qcom,cpufreq-hw` or `-epss` node ([chipsets.md](chipsets.md)); `tools/boards.py new --dts` writes it. |
| Everything is slow | Without `cpufreq=1` all cores run at 300 MHz. | `cpufreq=1`. It worked when the log has `cpufreq: group N ...: row N -> N of the frequency table, N MHz` for each group. |
| The phone gets hot with `cpufreq=1` | It runs every group of cores at its top row with nothing holding it back. | `thermal=2` ([thermal.md](thermal.md)). |

### Buttons

| What you see | What it means | What to do |
| --- | --- | --- |
| `buttons: disabled: no spmi { } in the SoC file or no buttons { } in the board file` | The feature is listed, but the arbiter's address or a key's peripheral is not there. | Fill in both sections. |
| `buttons: this driver knows versions 5, 7 and 8 of the arbiter only: no buttons` | The version read, in the line before it, is below 5; or 8 without `channel-map` in `spmi { }`; or 7 with a bus that has no channels (the count is in the same line) or a `bus` other than 0 and 1; or 5 or 8 with `bus = <1>`, which only a version 7 arbiter has. | Check `core`, for version 7 `bus` (the node's `qcom,bus-id`) and for version 8 `channel-map` in `spmi { }` ([chipsets.md](chipsets.md)). Versions 2 and 3 need work in `board/spmi.h`. |
| `buttons: no channel of the arbiter is NAME (slave N, peripheral 0x...)` | The arbiter's table has no channel for that peripheral. | Check `pon` and `volume-up` in `buttons { }`: the SPMI slave times 256, plus the high byte of the peripheral's registers (`0x18d`: slave 1, registers `0x8d00`). |
| `buttons: ... a power-on block of subtype 0x..., which has no keys this driver reads: left alone` | `pon` in `buttons { }` is a power-on block without keys: a secondary PMIC's, or the half of a PMK8350's that is not the operating system's (subtype 8). | `pon` is the primary PMIC's block (subtype 1 or 4), or a PMK8350's `pon_hlos` (subtype 9) ([chipsets.md](chipsets.md)). |
| `buttons: ... which is not what this driver knows (type 0x..., subtype 0x... to 0x...): left alone` | The peripheral at that address is not a power-on block, or not a GPIO. The line before it shows the type it reports. | The same two values. |
| `buttons: NAME: no answer from the arbiter`, `... the arbiter refuses`, `... the transfer failed (nobody at that address?)`, `... the transfer was dropped` | A read on the SPMI bus failed; the text is what the arbiter's status said. | A read goes through `observer` of `spmi { }`, for the system `ee` names, to the peripheral of `buttons { }`: check those three. |
| `buttons: volume up's pin is not an enabled input with a pull-up. ...: volume up is left alone` | Making it one is a write to the PMIC, which this driver does not do. The line before it shows the pin's mode, pull and whether it is enabled. | Check that `volume-up` in `buttons { }` is the key's GPIO. |
| `buttons: a key reads as held before anything was watched: .... It is not reported until it has been seen released` | A key read as pressed when the driver started. | Release it. `buttons: NAME is released now: believed from here on` follows. |
| `buttons: nothing that can be read: no buttons`, `buttons: more than half of N reads have failed: giving up` | Neither peripheral passed the checks above, or the bus stopped answering. | The lines before it say which check failed. |

### Parts on the SoC's pins

| What you see | What it means | What to do |
| --- | --- | --- |
| `touch: disabled: ...`, `fts: disabled: ...`, `speaker: disabled: ...`, `camera: disabled: the SoC file does not say where the pins, the clock controller, RPMh, the SMMU and the camera subsystem's blocks are (pins { }, clocks { }, rpmh { }, smmu { }, camera { })`, `haptic: disabled: the SoC file does not say where the pins are (pins { })`, `mic: disabled: the SoC file does not say where the pins and the audio subsystem's blocks are (pins { }, lpass { })`, `adsp: disabled: the SoC file does not say where RPMh and the way to the other processors are (rpmh { }, ipc { }): ...`, `sound: disabled: the SoC file does not say where the pins and the SMMU are (pins { }, smmu { })`, `wifi: disabled: the SoC file does not say where ... are (rpmh { }, pins { }, spmi { }, ipc { }, smmu { }), or the board file where the Wi-Fi DSP's memory is (wifi { } dsp_memory): ...` | The driver's feature is listed, but the SoC file lacks a section the driver takes addresses from. `tools/boards.py check` refuses such a board, so this is a kernel built from other files than the ones checked. | Fill in the section (`arch/arm64/boot/dts/_template/soc.dtsi` says where each value is in the device tree) and build again. |

### Writing to the storage

| What you see | What it means | What to do |
| --- | --- | --- |
| `ufs: darwin: no write will be served: this board's file does not say which partition that is (storage { })`, `ufs: nothing takes writes: this board's file does not say which partition is the port's own (storage { })` | `ufs_write=1` on a board without `storage { }`. Nothing is written, by design. | Make the partition, then describe it: `arch/arm64/boot/dts/_template/board.dts` says which line of the partition table each value is. |
| `ufs: darwin: no write will be served: its unique GUID is not the one it had` (or its type GUID, where it begins and ends, its blocks, its logical unit, read only) | A partition named `darwin` is there, and one field of its GPT entry is not what `storage { }` says. The loader refuses the whole run's writes. | Compare `storage { }` with the phone's partition table. A partition made anew has a new unique GUID. |
| `ufs: the loader offers no partition to write into (...)` | The kernel was asked to write and the loader did not offer the partition, or offers it from another block than this kernel's board file says. | `ufs_write=1` goes on the loader's command line too; build the loader and the kernel with the same device package. |

### Reading the storage and the RAM disks

| What you see | What it means | What to do |
| --- | --- | --- |
| `ufs: the loader's page at ADDR is not used: REASON` | The page through which the kernel asks the loader for the storage's blocks says something no request could be made with: a buffer that is not whole blocks, a block size that is not a power of two, a partition whose size overflows. No device is made. | The loader and the kernel are not of the same build, or the loader's page was overwritten: build both from the same checkout. |
| `ufs: native: the whitelist is WRONG, ...` | The kernel's own UFS driver tried its check of commands on made-up ones before its first command, and the check let one through that it must not, or refused one it must pass. The driver is not used; reads go through the loader. | The kernel was built from a changed `board/ufshc.c`: `workbench/ufs_whitelist_test.c` (part of `make check`) shows what. |
| `sparse: N of those have an entry that points past the image's blocks: the image is damaged` | The sparse image's table names blocks that are not in it; they read as zeros. | Make the image again with `mksparse.py`. |
| `hakios_image: /DIR/NAME: a path longer than 95 bytes is not looked up` | A driver asked for a firmware file by a path too long for the lookup. | A shorter name or directory in the board file (`adsp { } firmware`, `gpu { } firmware`). |

### Touch

| What you see | What it means | What to do |
| --- | --- | --- |
| `touch: disabled: the board file does not say how the controller is wired (touch { }, with analog_enable and vendor_pins)`, `fts: disabled: ... (touch { }, with analog_supply)` | The feature is listed without `touch { }`, or without the keys that controller's driver uses; `tools/boards.py check` refuses such a board. | Fill in `touch { }`: `arch/arm64/boot/dts/_template/board.dts` says where each value comes from. |
| `goodix: as found (touchscreen=-1: looking only): votes ..., clocks ...; pins ...`, `fts: as found (touchscreen=-1: looking only): ...` | `phone_look`'s level for the touch screen: what the firmware left, read from the clock and pin controllers; the controller itself is neither powered nor spoken to. | Compare the pins' configuration (`cfg`) with Android's (`/sys/kernel/debug/gpio`, or the device tree's `pinctrl` of the touch node): the function and the drive should be the same once Android has started the touch screen. `touchscreen=1` starts it. |
| `focaltech: no firmware built in (focaltech_fw.h is empty): the chip is not started` | The panel is the FocalTech FT3680 variant (its straps say so), which keeps no firmware of its own: the driver sends it, and this kernel was built without it. | `tools/mk-focaltech-fw.py` makes `focaltech_fw.h` from the phone's own firmware file; build again. |
| `goodix: clocks stay off: ...`, `fts: clocks stay off: ...` | The serial engine's clocks did not start after they were voted for. | `clocks`, `vote-register` and `votes` of `touch { }` are the engine's and its wrapper's in your SoC's clock driver (`gcc-SOC.c`): the three `halt_reg`, the `enable_reg` and the five `enable_mask` bits. An engine of another wrapper has other bits. `tools/qupclocks.py gcc-SOC.c WRAPPER ENGINE` prints the three lines. |
| `goodix: RPMh: no regulator "NAME"`, `fts: no RPMh regulator NAME` | The command database has no regulator of that name. | `io-supply` and `analog-supply` are RPMh's names: regulator L2 of the third PMIC is `ldoc2`. `ddr=1` and `speaker_probe=1` print names the database has. |
| `fts: no application ID 0x5652 ...`, `goodix: ... no answer` | The controller does not answer on the bus. | With the clocks running this is the pins: `bus-pins`, `reset` and `interrupt` against the bus's and the controller's nodes in the device tree. Both drivers also try the bus by hand on the same pins and say which data pin answered. |
| `ft5x06: disabled: the board file does not say how the controller is wired (touch-i2c { })`, `... names a supply, and the SoC file does not say where RPMh is` | `touch-ft5x06` is listed without its section, or the section names a regulator on a SoC file without `rpmh { }`. | Fill in `touch-i2c { }` (`arch/arm64/boot/dts/_template/board.dts`); `tools/boards.py check` says the same before the build. |
| `ft5x06: nobody answers at 0x38: a chip that is not there, not powered or held in reset` | No acknowledgement of the address. At `touchscreen=-1` nothing was powered: a chip the firmware left off does not answer, and the line before says if its reset is held. | Try `touchscreen=1`, which powers and resets it. Then: `sda`, `scl` and `address` against the device tree, and the supplies -- name the regulators (`io-supply`, `analog-supply`) if nobody else keeps them on. |
| `ft5x06: the chip at 0x38 reads identity 0xff (or 0x00): one that runs no firmware` | The chip acknowledges and has nothing to say: it is in its boot ROM. The parts without flash (FT8006, FT8716, FT8719 ...) are given their firmware by Android's driver at every power-on. | Nothing here: the driver uploads no firmware. |
| `ft5x06: ... answers identity 0x..., NOT ONE THIS DRIVER HAS A NAME FOR` | A chip the driver has no name for; it is read as the family's all the same, and nothing is written to it. | If touches come out right, tell us the identity; if `ft5x06: a report that cannot be one` follows, it is not of this family. |
| `ft5x06: a report that cannot be one: status ..., N slots, ...` | More fingers than `contacts`, a finger twice, or a position past `coordinates`. | `contacts` and `coordinates` of `touch-i2c { }` against the device tree (`focaltech,max-touch-number`, `focaltech,display-coords`); `ft5x06_trace=1` shows what the chip gives. |
| `ft5x06: the I2C bus is not at rest (gpioN reads 0, ...)` | A line of the bus is low before the driver touches it: no pull-up while the supply is off, or another device holds it. | The I/O supply (`io-supply`), and that `sda` and `scl` are this bus's pins. |

### Speaker amplifiers

| What you see | What it means | What to do |
| --- | --- | --- |
| `speaker: disabled: the board file does not say which pins the amplifiers are on (speaker { })` | The feature is listed without `speaker { }`; `tools/boards.py check` refuses such a board. | The bus's two lines, the two addresses, the two interrupt pins and the sound's four pins with their function: `arch/arm64/boot/dts/_template/board.dts` says where each is in the device tree. |
| `speaker: nobody at I2C address 0xNN: not powered, or not there` | Nothing acknowledged that address. With `speaker_probe=2` before the audio DSP runs this is usual: the amplifiers' supply may be off. | Compare `addresses` with the amplifiers' `reg` in the device tree; look again with `adsp=1`. |
| `speaker: the I2C bus is not at rest (gpioN reads 0, ...): not touched` | A line of the bus is low before anything was sent. The driver does not drive a bus it did not find idle -- except to power the amplifiers down at once, when it clocks the bus to free a data line that an amplifier cut off in the middle of a byte holds low (`the I2C bus was held low: ... after N clocks`). | Check `sda` and `scl` against the bus's pinctrl node; level 1 (`speaker_probe=1`) shows how each pin was found. |
| `speaker: the I2C bus is not at rest (...): nine clocks did not free it`, `speaker: the amplifier 0xNN, slot N: 0x04 <- 0x.... NOT ACKNOWLEDGED: NOT KNOWN TO BE POWERED DOWN, to be tried again` | A power-down at once did not read back: the amplifier may still be on. The driver tries it again at every stop (with `sound=4` every 200 ms) and on a panic's way out, and `sound:` sends the DSP silence in place of Darwin's sound until the run ends (`THE AMPLIFIERS ARE GIVEN UP ON`). `IS POWERED DOWN, at try N` says when it took. | A line held low for good is a wiring or supply fault: look at the pins with `speaker_probe=1` after a reboot. |
| `speaker: the amplifier 0xNN, slot N is there: chip ID 0x...., an amplifier this list does not know` | Something answers, but it is not a chip of the family the driver names. | The driver configures only an AW88261 (ID 0x2113); another chip needs its own register profile. |

### Vibration motor

| What you see | What it means | What to do |
| --- | --- | --- |
| `haptic: disabled: the board file does not say which pins the chip is on (haptic { })` | The feature is listed without `haptic { }`; `tools/boards.py check` refuses such a board. | The chip's I2C lines, reset and interrupt as pins of the SoC, and `f0-calibration` from Android's log on the same phone (`f0_cali_data`). |
| `haptic: nobody answers at 0x5a on the bus of gpioN/N`, then `(the reset pin does not drive high: the chip is held in reset or not powered)` | With `haptic=1` this is expected: a look leaves the chip in reset. | `haptic=2` releases the reset and reads the chip, still writing nothing to it. |
| `haptic: ...: THE CHIP IS PUT INTO RESET (gpioN low) and nothing more is done` | A write was not acknowledged or did not read back, the chip reported a fault, or it is not the chip expected. The reason is the start of the line. | For "NOT THE AW86927 EXPECTED": another chip needs its own driver. For a write that does not stick: check the pins in `haptic { }`. |

### Motion sensor

| What you see | What it means | What to do |
| --- | --- | --- |
| `imu: disabled: the board file does not say which pins the chip is on, its address, its range and how it is turned (imu { })` | The feature is listed without a usable `imu { }`; `tools/boards.py check` refuses such a board. | The chip's I2C lines and interrupt as pins of the SoC, its address (`reg` of its node), its range, and `mount` (the template explains it). |
| `imu: nobody answers at 0x6a on the bus of gpioN/N` | No chip acknowledges its address on those pins. | Check the pins and the address (an LSM6DSO: 0x6a or 0x6b, by how SA0 is strapped; a BMI160: 0x68 or 0x69, by SDO). On most phones the chip is the sensor DSP's, on a bus the application processor does not drive: then this driver is not for that phone. |
| `imu: the chip at 0x.. answers WHO_AM_I 0x..: NOT THE LSM6DSO'S 0x6c` (or `CHIP_ID 0x..: NOT THE BMI160'S 0xd1`) | Something answers, but it is not the chip of the feature the board lists. | If it is the other of the two, list its feature instead (`imu-lsm6dso`, `imu-bmi160`; a BMI160 asked for an LSM6DSO's identity answers with a byte of its gyroscope's output, any number). Another chip (a BMI260 or BMI270, say, which is not a BMI160 with another number) needs its own driver; `sensors=2` writes nothing to it. |
| `imu: 0x7e <- 0x..., PMU_STATUS reads 0x..: NOT CARRIED OUT in N ms` | A BMI160 took the command that switches its accelerometer (0x11) or gyroscope (0x15) on and does not say it measures. | Nothing is known to cause it; the driver's waits and the meaning of PMU_STATUS are the datasheet's as remembered (`drivers/board/bmi160_core.h`), not yet seen on a chip: send the whole `imu:` log. |
| `imu: disabled: this board's board.dts lists both "imu-lsm6dso" and "imu-bmi160"` | Two chips for the one `imu { }`; `tools/boards.py check` refuses such a board. | List the one the phone has. |
| `imu: ...: NOTHING MORE IS DONE with the chip in this run` | A byte was not acknowledged, a write did not read back, the reset did not end or the bus was not at rest. The reason is the start of the line. | For a bus not at rest: something else uses those pins (check that no serial engine of the SoC is set up on them). |
| `imu: portrait (...)` when the phone lies on its side | The board's `mount` is not how the chip is turned. | Read the chip's own numbers in the same line, held each way, and write `mount` from them. |

### Flashlight

| What you see | What it means | What to do |
| --- | --- | --- |
| `torch: disabled: the board file does not say which peripherals of the PMIC the light is (torch { })` | The feature is listed without `torch { }`; `tools/boards.py check` refuses such a board. | `flash` and `alarm` in `torch { }`: the PM8350C's `qcom,flash_led@ee00` and `qcom,temp-alarm@a00`, each as slave << 8 and the high byte of its registers. `tools/boards.py new --dts` reads both. |
| `torch: no channel of the arbiter is the PM8350C's flash LED module` | The arbiter's table has no channel for the peripheral `torch { }` names. | Check the slave: the number behind `qcom,pm8350c@` in the tree. |

### GPU ([metal-glass.md](metal-glass.md))

| What you see | What it means | What to do |
| --- | --- | --- |
| `adreno: disabled: the board lists "gpu-a660" but its board.dts has no usable gpu { } section` | The board asks for the GPU, and its `gpu { }` is missing, or has no chip id or speeds. (A board without `gpu-a660` says nothing of the GPU.) | Fill in `gpu { }` from the phone's Android device tree ([porting.md](porting.md), step 5), or take `gpu-a660` out. |
| `adreno: disabled: the SoC file does not say where the GPU's blocks are (adreno { }, clocks { }, rpmh { })` | One of the addresses the driver needs is not there. | Fill in the three sections of the SoC file. |
| `adreno: THE GPU IS NOT STARTED: the small image has no DIR/FILE (the phone's /vendor/firmware/FILE goes into fw-gpu/); the system draws with its software renderer` | `gpu=1` was passed, and a firmware file is not in the directory `firmware` of `gpu { }` names. | `adb pull` the file from the same phone and build the small image again. Firmware is never committed. |
| `adreno: step N: FAILED; the steps after it are not attempted`, then `adreno: THE GPU IS NOT STARTED: step N (...) failed; the system draws with its software renderer` | The bring-up stopped at that step. The step's own lines are above it. | Compare `gpu { }` and `adreno { }` with the device tree. The sequence has only been run on two chips: another generation of Adreno needs work in the driver ([porting.md](porting.md), "What is still tied to the first two phones"). |
| No application icons, no glass, parts of the interface missing, with `gpu=0` | They are drawn with the GPU, and the software renderer leaves them out. Expected. | [darwin26-boot.md](darwin26-boot.md), Darwin 27, item 10. |

### Bluetooth ([drivers.md](drivers.md), "Wi-Fi, network")

| What you see | What it means | What to do |
| --- | --- | --- |
| `bt-uart: no /fw/NAME in the RAM disks: ...` | `bt=2`, and the patch or the configuration `bluetooth { }` names is not in the small image. | `adb pull /vendor/bt_firmware/image/ userland/fw-bt/` from the same phone, and build the small image again (`userland/README.md`); or `bt_fw=0` to try the controller's ROM. |
| `bt-uart: /fw/NAME (N bytes) is neither a TLV file nor an ELF one` | The file is not the controller's firmware, or is cut short. | Compare its size with the phone's; the names of `rampatch` and `nvm` with the files of `vendor/bt_firmware/image`. |
| `bt-uart: the serial engine's clocks are off: its registers are read once they are on` | The bootloader left the UART's clocks off. A run that only looks (`bt=-1`) stops there; `bt=1` and `bt=2` read the engine once they have voted for its clocks. | Nothing: it is said so that the next lines are read knowing it. A line `GCC clock N stays off` after it is the problem. |
| `bt-uart: a packet of N bytes went out in part, or not at all: the controller is out of step ...` | The UART did not finish sending one of bluetoothd's packets (the controller held its line, or the engine failed): the controller would read what follows wrong, so nothing more is sent to it. | Bluetooth is off until a restart. The lines before it (`TX FIFO stays full`, `TX error`, `TX timed out`) say how; report them with `uart_state`'s. |
| `bt-uart: segment N refused (status 0x..)`, or `segment N: no answer within 2 s` | The controller would not take the patch: another chip's, or a step of the protocol it wants and this driver does not send. | The log's `product id`, `ROM version` and `SoC id` lines say which chip it is: the file must be the one the phone's Android sends it. Report the lines. |
| `bt: com.apple.uart.sk.bluetooth connected by bluetoothd` never comes | bluetoothd did not find the controller, or left before it opened the kernel control. | `bt: service "bluetooth" registered (address ...)` must be there first; without `/chosen mac-address-bluetooth0` (the loader's) there is no service. |

### Wi-Fi ([drivers.md](drivers.md), "Wi-Fi, network")

| What you see | What it means | What to do |
| --- | --- | --- |
| `wifi: N: TRUSTZONE DOES NOT SHUT THE DSP DOWN: the call returns ..., its answer is ...` | The DSP is still running at the run's end. The radio's stream in the SMMU is sent to fault, so it cannot write into memory that is not the kernel's any more; the next run starts from a radio that is not quiet. | Power the phone off and on before the next run with `wifi=1`. Report the line with the ones before it (`the DSP is asked to stop`). |
| The phone goes down, or the log stops, just as the network is lost or the run ends with Wi-Fi joined | Perhaps the known race: a frame sent by Darwin while the station is being taken apart ([drivers.md](drivers.md), "Wi-Fi, network"). | Report the log's last lines. A run without `wifi=1` is not affected. |

## Images ([images.md](images.md))

| What you see | What it means | What to do |
| --- | --- | --- |
| `image.py: which phone? --board NAME (here: ...)` | More than one package with a `uefi { }` node, or none. | `--board NAME`, or `make image DEVICE=NAME`. |
| `image.py: no kernel ...: kernel/build.sh` | The kernel has not been built, or was built elsewhere. | `make kernel`, or `--kernel FILE`. |
| `image.py: ... kernel ... does not know NAME` | The kernel was built before the phone's package was brought in: its board table has no row for the phone, and on it every driver would stay off. | `make kernel`, then the image again. |
| `image.py: ... does not know NAME: it was built before the package was here` | The loader was built before `tools/device.py add` brought the phone's package in: it knows only the boards it was built with. | `make loader` (`arch/arm64/boot/build.sh`), then the image again. |
| `image.py: the loader was built with an identity from an IPSW (Apple's data) ...` | `publish` refuses a loader that carries the identity of an iPhone: an image that is passed around must not. | Remove `arch/arm64/boot/identity.h`, `arch/arm64/boot/build.sh` without a `DeviceTree.bin` beside the IPSW, `make image`, `make publish` again. Your own copy can carry it: `tools/image.py personalize`. |
| `image.py: FILE holds an identity blob (IDNT)` (or `an Apple image file (IM4P)`, `an iPhone's device tree`) | The file's bytes hold Apple's data, whatever it was put under. | Build the image again from the checkout (`make image`); a personalized one (`*-mine.*`) is for your own phone only. |
| `verify`: `NOT AS PUBLISHED: files manifest.json does not list: ...` or `files in no image: ...` | Something lies in the phone's `images/` that no image's manifest names: a personalized copy, a `DeviceTree.bin`, an IPSW. | Take it out of the repository (`git rm`); only `tools/image.py publish` puts files there. |
| `image.py: ... is not what manifest.json says it is` | A file of the image changed after it was built. | Build the image again. |
| `payload entry 'NAME' says N bytes, M are left: the payload is damaged`, `... has no end to its text`, `the payload says it has N bytes, more than ...` | The loader held the payload's sizes to what is there and they did not fit: a file cut short in the copy, or not one `mkpayload.py` or `image.py` made. Nothing of it is run. | Build or download the image again; `tools/image.py verify` holds one to its manifest. |
| `identity: the payload's is N bytes, more than the 32768 there is room for: the one built in` | The identity is larger than the device tree has room for beside the loader's own nodes. | One made by `mkidentity.py --blob` from an iPhone's device tree is a few kilobytes: make it again from `DeviceTree.bin`. |
| `the device tree does not fit its 10000 bytes: the kernel is not started` | The tree the loader writes for the kernel would have gone past its 64 KB, over the boot arguments after it. | Report it with the log: something in the payload (the identity, most likely) is larger than any phone's. |
| `rdres N: past the RAM given to the kernel` | A payload's word for a root image already in memory (QEMU's `--rdres`) that does not fit the window. | Nothing of a phone's: a payload made by hand for QEMU, to be made again. |
| `identity: the payload's is not one (N bytes): the one built in` | The loader found an `identity` entry it cannot read: not made by `mkidentity.py --blob`, or of another version. | `tools/image.py personalize IMAGE --identity DeviceTree.bin` again, with the same checkout's tools. |

## A day with the phone (`scripts/phone-day.sh`, `scripts/phone-ready.py`, `tools/logcheck.py`)

| What you see | What it means | What to do |
| --- | --- | --- |
| `phone-day.sh: not ready (above): no run started` | `scripts/phone-ready.py` found something `./bench` needs MISSING; its lines are above and in `summary.md`. | What each MISSING line says; `--no-check` begins all the same. |
| `OLD  kernel  ..., built ..., before commit ...` | The kernel (or the loader) was built before the newest change to its sources, a commit or a file not committed: the phone would run what was there before. | `make kernel` (`make loader`, then `./bench reload`). A kernel named by `BENCH_KERNEL` is not looked at. |
| `MISSING  loader  ... does not know NAME` | The loader was built before the phone's package was brought in. | `make loader`, then `./bench reload`. |
| `MISSING  kernel  ... does not know NAME`, `bench: warning: ... does not know NAME` | The kernel was built before the phone's package was brought in: no row of its board table is the phone's, so every driver stays off. | `make kernel`. |
| `MISSING  identity  none: the loader was built without one` | Darwin would not be told which iPhone it runs on. | The `DeviceTree.bin` of your own IPSW as `firmware/NAME/identity.bin`, or `BENCH_IDENTITY=FILE`. |
| `NOTE  phone_bt: bt firmware  no ... in userland/fw-bt` | The run at `bt=2` would find no patch for the controller. | `adb pull /vendor/bt_firmware/image/ userland/fw-bt/`, the small image again; or that run without `phone_bt`. |
| `no log: the run did not come back` | `./bench run` left no new log: the kernel hung before the loader could fetch one, or the phone was not there. What `./bench` said is in `RUN.bench` beside it. | `./bench state`; the kernel's last words, if it hung, are in `logs/*.log.live` (`BENCH_FOLLOW`). |
| `RUN did not go all the way (above)` | `tools/logcheck.py` found a part STOPPED, a panic or no kernel in that run's log; the day stops there. | The summary has the line that decides it; [phone-test.ru.md](phone-test.ru.md) says what each run must say. `--keep-going` runs the rest all the same. |
| `kernel: NOT STARTED (no Darwin Kernel Version line)` | The log has nothing of the kernel: the loader did not start it (its own lines are above), or the log is not a run's. | `./bench log` for the loader's side; [porting.md](porting.md), step 4. |
| A part `said`, neither ok nor STOPPED | It wrote lines, and none of the words that say it came up or stopped: a look-only level, or a driver that ended between the two. | `tools/logcheck.py --all LOG` shows every line of it. |

## The checks (`make check`)

`make check` is the lint, the board check of the first section (it covers every package that was
brought in), the boot-argument reference and `workbench/run.sh` ([workbench/README.md](../workbench/README.md)).
CI runs the same, plus `kernel/verify.sh` and `kernel/syntax-check.sh`.

| What you see | What it means | What to do |
| --- | --- | --- |
| ``Documentation/boot-args.md is not what the sources say: run `make docs` `` | A boot argument or its comment changed in the sources. The reference is generated from them. | `make docs`, and commit the result. |
| ``FILE.md:N: link to ..., which is not there``, ``FILE.md:N: `...` is not in the repository`` | `tools/check-links.py`: a relative link, or a path in backticks that starts at one of the repository's top directories, points at nothing. | Correct it. A path with `NAME`, `SOC`, `*` or `...` in it is not checked. Paths that are gone on purpose go between the two comment marks `Documentation/migrating.md` uses. |
| `components: ...: FILE does not pin "..." (it changed there, or here)` | A script that fetches something and the manifest name different versions. | Change the two together. |
| `binary: ./FILE (...)`, `firmware/Apple file name: ./FILE`, `home path: ...`, `secret: ...`, `byte array (N rows): ...` | `tools/check-publishable.sh` found something that must not be published. It looks at every file in the checkout, ignored ones too: a `loader.efi` you built there is enough. | Move the file out of the checkout ([CONTRIBUTING.md](../CONTRIBUTING.md), "What must never be committed"). |
| `FAILED (failures=N)` under one of the `== ...` headings of `workbench/run.sh` | A test of the tools failed. `workbench/run.sh` shows only the last three lines of those. | Run it by itself for the reason: `python3 workbench/boards_test.py`. One reason, `[SECTION] KEY is not explained in the template`: a key was added without its line in `arch/arm64/boot/dts/_template/` ([arch/arm64/boot/dts/README.md](../arch/arm64/boot/dts/README.md#adding-a-kind-of-data)). |
| `lint: no shellcheck on this machine: only the syntax was checked`, `== (no aarch64-linux-gnu-gcc or qemu-aarch64: the aarch64 run is left out)`, `== (no clang or lld-link: the loader is not built)` | Not failures. What is not installed is left out, and said. CI has all of it. | `sudo apt-get install gcc-aarch64-linux-gnu qemu-user shellcheck clang lld`. |
| `make check` passes and CI fails | CI also assembles Apple's release and compiles the overlay against it. | `make verify-kernel` and `make syntax-kernel`, which need the network. After a change to a driver, `kernel/syntax-check.sh --tree DIR` answers in seconds. |

## Where to ask

The repository's issue forms (`.github/ISSUE_TEMPLATE/`): one for something that does not work, one for
a new phone, one for the documentation. Say what was verified, and paste the log lines. Do not attach
Apple's files, the phone maker's firmware or device trees: a value read from one is a fact and is fine
([CONTRIBUTING.md](../CONTRIBUTING.md)).
