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
`devices/_template/board.toml` and `devices/_template/soc.toml`.

| What you see | What it means | What to do |
| --- | --- | --- |
| `not TOML: ...` | The file does not parse. The rest of the line is the parser's reason, with line and column. | Fix the syntax there. Nothing else of that file is checked until it parses. |
| `unknown key "KEY" (known: ...)`, `unknown section [SECTION] (known: ...)` | The file has a key or a section this checkout does not know: a typo, or a package written for newer code. | Compare with the list in the message. A package and the code change together when a key is added ([devices/README.md](../devices/README.md#adding-a-kind-of-data)). |
| `"KEY" is missing`, `KEY = VALUE: ... expected`, `KEY = VALUE: one of ...` | A required key is not there, or a value is of the wrong kind. | The template says what each key takes. |
| `name = 'X': the directory is devices/NAME`, `name = 'X': the file is SOC.toml` | `name` in the file is not the name of its directory (a board) or of the file (a SoC). | Make the two the same. Names are lower-case letters, digits and dashes. |
| `soc = 'SOC': there is no soc/SOC.toml` | The board names a SoC that has no file. | Correct the name, or write the SoC file from `devices/_template/soc.toml` ([porting.md](porting.md), step 1). |
| `feature "X" is not one the kernel knows (...)` | `X` is not a `HAKIOS_HAS_*` of `kernel/overlay/bsd/dev/hakios_board.h`: a typo, or a package that needs newer code. | Use a name from the list in the message. |
| `feature "X" needs [SECTION] in soc/SOC.toml`, `feature "X" needs [SECTION] in the board file`, `feature "wifi-wcn6750" needs dsp_memory and dsp_memory_size in [wifi] of the board file` | A board cannot list a feature its files have no addresses for. The same goes for `needs channels in [spmi] of ...`, `needs intf1 in [display] and [clocks] in ...` and `needs a command-mode panel ([backlight] says video)`. | Fill in the section, or take the feature out of `features`. `tools/boards.py status NAME` says the same for every feature (`needs ...`, or `on: STILL NEEDS ...`). |
| `type "dcs" needs "KEY"`, `backlight type "dcs" needs [SECTION] in soc/SOC.toml`, `backlight type "dcs" needs gcc_disp_ahb_cbcr, ... in [clocks] of soc/SOC.toml` | DCS brightness needs the panel's range and mode, the DSI controller's address and three clock offsets that no device tree has. | Until you have them, set `type = "soft"`, which works anywhere ([porting.md](porting.md), step 1). |
| `[loader] needs [platform] in soc/SOC.toml`, `[loader] needs io_base and io_size in [platform] of soc/SOC.toml`, `io_base must not be 0 (XNU then takes the machine for one without a SoC)` | The loader's description of the machine comes from `[platform]` of the SoC file. | Fill it in. `io_base` and `io_size` are one window over the SoC's peripherals, not starting at 0 ([porting.md](porting.md), step 1). |
| `more than one board with [loader] and no fb_base (...): the loader tells boards apart by it, and only the emulator goes without` | A phone's board file has a `[loader]` section without `fb_base`. | Leave `[loader]` out until you have the address: build and start the loader, read its line `display W x H at ADDRESS`, then add `[loader]` with `fb_base` ([porting.md](porting.md), step 3). |
| `fb_base 0x... is also that of NAME` | Two boards that are present give the same framebuffer address, and the loader tells boards apart by nothing else. | Check the value against the loader's `display W x H at ADDRESS` on your phone. |
| `domain_cores adds up to N cores, [platform] has N`, `floor_mhz: one value for each of the N groups of cores (...)` | The groups of cores in `[cpufreq]`, the number of cores in `[platform]` and the board's thermal floors must agree. | Correct `domain_cores` (the cores of each frequency group) or `floor_mhz` (one floor for each group). |
| `no board "NAME" (tools/boards.py list)` | `status` or `show` was asked for a board whose package is not present. | `tools/device.py add NAME`, or name the directory it is in with `--devices DIR` or `HAKIOS_DEVICES`. |
| `note: ...` after `tools/boards.py new --dts FILE`, for example `note: [SECTION] not found in the tree` or `note: no panel node in the tree (it may be in the dtbo: dump that as well)` | What the device tree did not say. The files were written all the same. | Fill those values in by hand. With several panels pass `--panel NODE`. The live tree of a running Android has the overlays applied ([porting.md](porting.md), "What you need from the phone"). |

## Device packages

`tools/device.py` brings a phone's package into `devices/` from the `devices` branch, or from
`--from SOURCE`: a directory, a branch or a commit. More: [devices/README.md](../devices/README.md).

| What you see | What it means | What to do |
| --- | --- | --- |
| `... has no package "NAME" (it has: ...)` | The source has no directory `NAME` with a `board.toml` in it. | `tools/device.py available` lists what a source has; `--from SOURCE` names another one. |
| `devices is neither a directory nor a branch or commit of this repository (packages are kept on the branch "devices": git fetch origin devices)` | The branch could not be fetched from `origin` and this clone has none of that name. With `--from X` the line starts with `X`. | `git fetch origin devices`, or give `--from` a directory that has the package. |
| `devices/NAME is there and was not brought in by this tool: it may be work of yours. Move it away first (or tools/device.py remove NAME)` | The directory was made by `tools/boards.py new` or by hand, and `add` never replaces such work. | Keep the work (`tools/device.py export NAME DIR`, or move the directory), then add again. |
| `the package does not fit this checkout: ...` | The board check found problems in the package, printed above the line: it has a key or a feature this checkout has not, or lacks one the checkout needs. The package was copied all the same. | Bring the checkout up to date, or take the package as another commit has it (`--from COMMIT`). Until it fits, `tools/device.py remove NAME`: every build stops on a board that fails the check. |
| `devices/NAME is not right yet: not exported` | The board check found problems in the package, printed above the line. What is sent is what was validated. | Fix them (first section), then export again. |
| `DIR is not a directory (a checkout of the devices branch: git worktree add DIR devices)` | `export` copies into a checkout of the `devices` branch that is there already. | `git worktree add ../hakios-devices devices`, then `tools/device.py export NAME ../hakios-devices` ([porting.md](porting.md), step 8). |
| `git status` shows nothing of `devices/NAME/` | Git ignores packages in a checkout of the main branch: they are kept on the `devices` branch. | Send the package with `export`, as above. A new SoC file is an ordinary file of the main branch ([CONTRIBUTING.md](../CONTRIBUTING.md)). |

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
| `FILE is not a file of the tree (a path as in kernel/overlay)` | `kernel/syntax-check.sh` takes paths as they are in the tree. | `bsd/dev/hakios_thermal.c`, not the path in the repository. |
| `FILE compiles here: take it off kernel/syntax-check.skip` | A file on the list of those only the real build can check compiles after all. The check fails so that the list says what is true. | Remove its line from `kernel/syntax-check.skip`. |
| `N of N files failed (above)` | The first errors of each file are above the line. `N C++ files not checked (no --driverkit)` in the `ok:` line is not a failure. | Fix the errors. Only a file that includes what the real build alone has belongs on the skip list. `--driverkit DIR` checks the C++ files as well. |

## Building

What each build needs is in [building.md](building.md). The full kernel build has not been repeated
since the restructuring: if it fails, look first at steps 1 and 2 of `kernel/build.sh` and at the ten
files `kernel/syntax-check.sh` cannot check ([building.md](building.md), "What has been verified, and where").

| What you see | What it means | What to do |
| --- | --- | --- |
| A build stops after lines like `devices/NAME/board.toml: ...` | Every build makes its table of boards with `tools/boards.py gen`, which takes no board that fails the check. | First section. Or take the package out: `tools/device.py remove NAME`. |
| `build.sh: boards: ...: 1 boards (qemu-virt); ...` | The kernel is built with no phone's package. It will know only the emulator's board: on a phone it finds no row for itself and starts no driver of a phone. | `tools/device.py add NAME`, or set `HAKIOS_DEVICES`, and build again. The same holds for `loader/build.sh`. |
| `build.sh: no theos toolchain/SDK in DIR` | `DIR` has no `toolchain/linux/iphone/bin/ld` or no `sdks/iPhoneOS16.5.sdk`. | Install theos with its Linux toolchain and that SDK in `~/theos`, or set `THEOS`. |
| `stage1: not installed: ...` | Programs the dependency step needs are not on this machine. | Install the packages [building.md](building.md) lists for the kernel. |
| `stage1: ... is not the archive this was written with (its SHA-256 differs): not unpacked`, `build.sh: ...: unexpected checksum` | A downloaded macOS SDK archive is not the one the script pins. Nothing was unpacked. | Run the build again: the archive is fetched anew. If it differs again, the file at that address is no longer the pinned one (`components.toml` lists the pins): report it. |
| `stage1: cannot fetch ... from ...`, `download failed: ...` | The first build fetches Apple's open sources at pinned commits and two SDK archives. | It needs the network once. What is there already is left alone, so run it again. |
| `stage1: mig did not build, see DIR/mig-build.log`, `stage1: tcsh did not build, see DIR/tcsh-build.log`, `build.sh: exporthdrs failed, see DIR/exporthdrs.log` | A step failed, and its output is in the file named. `kernel/deps/stage1.sh` has not been run end to end yet ([building.md](building.md)). | Read the file. Its last lines belong in a report. |
| `System image exceeds fastboot's 4 GiB - 1 download limit` | `ios/tools/mkimage26.py`: fastboot sends a download's size in eight hex digits. | Build the image with `--prune` ([ios26-boot.md](ios26-boot.md), iOS 27, item 1). |
| The scripts under `ios/` do not run from a clean checkout | They expect the team's working tree (`darwin-renoir`), which is not published, and an extracted IPSW beside the repository. | Nothing yet. `make check` and the kernel build do not depend on it ([architecture.md](architecture.md), "What is not in this repository"). |

## At boot: the kernel's log

The kernel's log comes back through the loader. Boot first with one core (`cpus=1`) and no hardware
feature on ([porting.md](porting.md), step 4).

| What you see | What it means | What to do |
| --- | --- | --- |
| The loader prints `platform qemu-virt` on a phone | The framebuffer the UEFI reports matches no board's `fb_base`, and such a machine is taken for the emulator. Or the loader was built without the phone's package. | Compare its line `display W x H at ADDRESS` with `fb_base` in `[loader]`. `tools/device.py list`, then `loader/build.sh` again ([loader/README.md](../loader/README.md)). |
| `board: not one of the N this kernel knows (/chosen/darwin-board): no device of a phone is touched` | The loader passed another name or none, or the kernel was built without the phone's package. `hw.board.name` is `unknown`, no feature is on, and brightness is the `soft` backend. | Look at the `build.sh: boards:` line of the kernel's build and at the loader's `platform` line. Bring the package in and build again. |
| `board: NAME (DESCRIPTION, SOC): no features listed` | The kernel found its row, and `features` is empty. This is what a first boot should say. | Turn hardware on one piece at a time (next section). |
| A hang at the second core | PSCI. | Check `psci` and `core_shift` in `[platform]` of the SoC file ([porting.md](porting.md), step 4). |
| `...: disabled: this board's board.toml lists no "FEATURE"` | A boot argument asked for a driver whose feature the board does not list, or the kernel does not know the board. Deliberate: these drivers write to pins and PMIC registers. | `tools/boards.py status NAME`. List the feature if the phone has that part wired the same way. Renoir lists only its touch controller ([migrating.md](migrating.md), "What behaves differently"). |
| `touch: touchscreen=1, but this board's board.toml lists no touch controller ("touch-...")` | Neither touch feature is in `features`. | The same part wired the same way: list it. Another part needs a driver ([porting.md](porting.md), step 6). |
| `ddr: no RPMh vote on this board (its board.toml lists no "ddr-rpmh")`, `ddr: no RPMh vote: the SoC file has no [rpmh]` | `ddr=` was passed without the feature, or without RPMh's addresses. | Fill in `[rpmh]` in the SoC file and list `ddr-rpmh`. `ddr=1` only looks. |
| `MiniIOMobileFramebuffer: panel scan: no readout counters on this board: frames written at once` | The board does not list `display-scan`, or its SoC file has no `intf1` in `[display]` or no `[clocks]`, or the display block could not be read. Frames are written on a timer, which works. | Nothing for a video-mode panel. For a command-mode panel: `display-scan` and `intf1`; then `panel scan:` lines with a period near 16.7 ms ([porting.md](porting.md), step 5). |
| iOS 27 stays at the boot spinner with `fbscale=2` | `fbscale=2` does not work with iOS 27. | Leave it out ([ios26-boot.md](ios26-boot.md), iOS 27, item 9). |
| A driver behaves differently than before the restructuring | Constants that were in the drivers became data of the board and SoC files. | Compare `sysctl hw.board` and the driver's first log lines with the board file: every value is printed or can be read there ([building.md](building.md)). |

## Hardware, one piece at a time

A driver starts only with its feature listed and its boot argument passed. Most have a level that only
reads and logs: use it first (`backlight=1`, `thermal=1`; the buttons' driver only ever reads). The
order to try them in is [porting.md](porting.md), step 5; every driver is in [drivers.md](drivers.md),
every argument in [boot-args.md](boot-args.md). The GPU is not needed for a picture: iOS draws in
software without it.

### Brightness ([backlight.md](backlight.md))

| What you see | What it means | What to do |
| --- | --- | --- |
| The panel does not follow the slider; the log has `backend dcs (backlight=1: looking only)` | Level 1 reads the DSI controller and logs it. It writes nothing. | `backlight=2`. |
| `backlight: board unknown, backend soft ...`, or the picture darkens while the panel's light stays | The kernel does not know the board, or `[backlight]` says `soft`: the display driver darkens each pixel. | For an unknown board see `board: not one of ...` above. For a panel that takes DCS brightness fill in `[backlight]`, and `[clocks]` and `[display]` of the SoC file. |
| `backlight: NAME: no complete DCS profile (DSI 0x..., N to N, N bytes): nothing done` | The backend is `dcs` (the board's, or `bl_backend=dcs`), but the DSI controller's address, a clock controller's address, the range or the number of bytes is missing or cannot be. | Fill in the three sections. `bl_dsi=`, `bl_min=`, `bl_max=` and `bl_bytes=` try values before they are written down. |
| `backlight: dcs: the display block is off or its registers have no clock: nothing done` | At the offsets the SoC file gives, the display's bus clock in the GCC did not turn on, the MDSS power domain reads as off, or the MDSS bus clock did not turn on. | Check `gcc_disp_ahb_cbcr`, `dispcc_mdss_gdscr` and `dispcc_mdss_ahb_cbcr` in `[clocks]` against Linux's `gcc-*.c` and `dispcc-*.c` for the SoC. |
| `backlight: dcs: not a DSI controller of 2.2 or later (no command FIFO): nothing done` | What was read at the address is not such a controller. The line before it shows the version read. | Check `dsi0` in `[display]` (the device tree's `mdss_dsi0`, "dsi_ctrl"). `bl_dsi=0x...` tries another address. |
| `backlight: dcs: the controller is off (the firmware did not leave the panel lit through it): nothing done` | The panel is not lit through the controller at that address (`OFF` in the line before). | The same: the address of the controller the panel is on. |
| `backlight: dcs: the controller runs a video-mode panel, the profile says command mode: going by the controller` (or the other way round) | `panel_mode` in `[backlight]` is not what the controller runs. The driver goes on in the controller's mode. | Correct `panel_mode`. |
| `backlight: dcs: brightness N NOT SENT in 20 ms (int ..., status ...): no more commands this run` | The controller did not report the command as done: the wrong controller or mode. DCS is not used again in this run. | Check `dsi0` and `panel_mode`. Go back to `backlight=1` and read the line `backlight: dcs: DSI controller at ...`. |

### Temperature ([thermal.md](thermal.md))

| What you see | What it means | What to do |
| --- | --- | --- |
| `thermal: this board's file has no [thermal] (devices/NAME/board.toml): nothing read` | The board file has no `[thermal]` section, or the kernel does not know the board. | Add `[thermal]` with `floor_mhz`, one value for each group of cores. The SoC file needs `[thermal]` and `[cpufreq]` first. |
| `thermal: this board's SoC file has no [cpufreq]: looking only` | `thermal=2` has no frequency block to hold back, and goes on as `thermal=1`. | Add `[cpufreq]` to the SoC file. |
| `thermal: th_clear < th_trip < th_critical does not hold (N, N, N): the defaults` | The `th_*` boot arguments contradict each other. 62.0, 70.0 and 95.0 °C are used. | Pass tenths of a degree, with clear below trip below critical. |
| `thermal: hottest sensor -273.0 C` | No sensor has a reading: none is valid, or all are outside −40 to 150 °C and ignored. The limiter then holds nothing back. | Check `tsens` in `[thermal]` of the SoC file (the device tree's `tsens_tm_physical`), and compare with Android's readings. |
| `thermal=2`, the phone is hot, and no `held back N of N` line | The limiter never raises a group above the row it found at start. Without `cpufreq=1` the groups stay at 300 MHz and it never writes. | Pass `cpufreq=1` as well. Under load (`cpuload=1000`) the line should appear around 70 °C. |

### CPU frequency

| What you see | What it means | What to do |
| --- | --- | --- |
| `cpufreq: disabled: this board's board.toml lists no "cpufreq-epss"` | `cpufreq=1` was passed and the board does not list the feature. | `[cpufreq]` in the SoC file, then `cpufreq-epss` in `features`. |
| Everything is slow | Without `cpufreq=1` all cores run at 300 MHz. | `cpufreq=1`. It worked when the log has `cpufreq: group N ...: row N -> N of the frequency table, N MHz` for each group. |
| The phone gets hot with `cpufreq=1` | It runs every group of cores at its top row with nothing holding it back. | `thermal=2` ([thermal.md](thermal.md)). |

### Buttons

| What you see | What it means | What to do |
| --- | --- | --- |
| `buttons: disabled: no [spmi] in the SoC file or no [buttons] in the board file` | The feature is listed, but the arbiter's address or a key's peripheral is not there. | Fill in both sections. |
| `buttons: this driver knows version 5 of the arbiter only: no buttons` | The version read, in the line before it, is below 5, or 7 or later, where the registers moved. | Check `core` in `[spmi]`. A newer arbiter needs work in the driver. |
| `buttons: no channel of the arbiter is NAME (slave N, peripheral 0x...)` | The arbiter's table has no channel for that peripheral. | Check `pon` and `volume_up` in `[buttons]`: the SPMI slave times 256, plus the high byte of the peripheral's registers (`0x18d`: slave 1, registers `0x8d00`). |
| `buttons: ... which is not what this driver knows (type 0x..., subtype 0x... to 0x...): left alone` | The peripheral at that address is not a power-on block, or not a GPIO. The line before it shows the type it reports. | The same two values. |
| `buttons: NAME: no answer from the arbiter`, `... the arbiter refuses`, `... the transfer failed (nobody at that address?)`, `... the transfer was dropped` | A read on the SPMI bus failed; the text is what the arbiter's status said. | A read goes through `observer` of `[spmi]`, for the system `ee` names, to the peripheral of `[buttons]`: check those three. |
| `buttons: volume up's pin is not an enabled input with a pull-up. ...: volume up is left alone` | Making it one is a write to the PMIC, which this driver does not do. The line before it shows the pin's mode, pull and whether it is enabled. | Check that `volume_up` in `[buttons]` is the key's GPIO. |
| `buttons: a key reads as held before anything was watched: .... It is not reported until it has been seen released` | A key read as pressed when the driver started. | Release it. `buttons: NAME is released now: believed from here on` follows. |
| `buttons: nothing that can be read: no buttons`, `buttons: more than half of N reads have failed: giving up` | Neither peripheral passed the checks above, or the bus stopped answering. | The lines before it say which check failed. |

### Parts on the SoC's pins

| What you see | What it means | What to do |
| --- | --- | --- |
| `touch: disabled: ...`, `fts: disabled: ...`, `speaker: disabled: ...`, `camera: disabled: the SoC file does not say where the pins, the clock controller, RPMh, the SMMU and the camera subsystem's blocks are ([pins], [clocks], [rpmh], [smmu], [camera])`, `haptic: disabled: the SoC file does not say where the pins are ([pins])`, `mic: disabled: the SoC file does not say where the pins and the audio subsystem's blocks are ([pins], [lpass])`, `adsp: disabled: the SoC file does not say where RPMh and the way to the other processors are ([rpmh], [ipc]): ...`, `sound: disabled: the SoC file does not say where the pins and the SMMU are ([pins], [smmu])`, `wifi: disabled: the SoC file does not say where ... are ([rpmh], [pins], [spmi], [ipc], [smmu]), or the board file where the Wi-Fi DSP's memory is ([wifi] dsp_memory): ...` | The driver's feature is listed, but the SoC file lacks a section the driver takes addresses from. `tools/boards.py check` refuses such a board, so this is a kernel built from other files than the ones checked. | Fill in the section (`devices/_template/soc.toml` says where each value is in the device tree) and build again. |

### GPU ([metal-glass.md](metal-glass.md))

| What you see | What it means | What to do |
| --- | --- | --- |
| `adreno: disabled: this board's board.toml has no "gpu-a660" with a [gpu] section` | The board has no GPU profile. The line is printed at every boot of such a board, with or without `gpu=1`. | Nothing, unless the phone has an Adreno 642, 642L or 643: then `[gpu]`, `[adreno]` in the SoC file, and `gpu-a660` ([porting.md](porting.md), step 5). |
| `adreno: disabled: the SoC file does not say where the GPU's blocks are ([adreno], [clocks], [rpmh])` | One of the addresses the driver needs is not there. | Fill in the three sections of the SoC file. |
| `adreno: THE GPU IS NOT STARTED: the small image has no DIR/FILE (the phone's /vendor/firmware/FILE goes into fw-gpu/); the system draws with its software renderer` | `gpu=1` was passed, and a firmware file is not in the directory `firmware` of `[gpu]` names. | `adb pull` the file from the same phone and build the small image again. Firmware is never committed. |
| `adreno: step N: FAILED; the steps after it are not attempted`, then `adreno: THE GPU IS NOT STARTED: step N (...) failed; the system draws with its software renderer` | The bring-up stopped at that step. The step's own lines are above it. | Compare `[gpu]` and `[adreno]` with the device tree. The sequence has only been run on two chips: another generation of Adreno needs work in the driver ([porting.md](porting.md), "What is still tied to the first two phones"). |
| No application icons, no glass, parts of the interface missing, with `gpu=0` | They are drawn with the GPU, and the software renderer leaves them out. Expected. | [ios26-boot.md](ios26-boot.md), iOS 27, item 10. |

## The checks (`make check`)

`make check` is the lint, the board check of the first section (it covers every package that was
brought in), the boot-argument reference and `tests/run.sh` ([tests/README.md](../tests/README.md)).
CI runs the same, plus `kernel/verify.sh` and `kernel/syntax-check.sh`.

| What you see | What it means | What to do |
| --- | --- | --- |
| ``docs/boot-args.md is not what the sources say: run `make docs` `` | A boot argument or its comment changed in the sources. The reference is generated from them. | `make docs`, and commit the result. |
| ``FILE.md:N: link to ..., which is not there``, ``FILE.md:N: `...` is not in the repository`` | `tools/check-links.py`: a relative link, or a path in backticks that starts at one of the repository's top directories, points at nothing. | Correct it. A path with `NAME`, `SOC`, `*` or `...` in it is not checked. Paths that are gone on purpose go between the two comment marks `docs/migrating.md` uses. |
| `components.toml: ...: FILE does not pin "..." (it changed there, or here)` | A script that fetches something and the manifest name different versions. | Change the two together. |
| `binary: ./FILE (...)`, `firmware/Apple file name: ./FILE`, `home path: ...`, `secret: ...`, `byte array (N rows): ...` | `tools/check-publishable.sh` found something that must not be published. It looks at every file in the checkout, ignored ones too: a `loader.efi` you built there is enough. | Move the file out of the checkout ([CONTRIBUTING.md](../CONTRIBUTING.md), "What must never be committed"). |
| `FAILED (failures=N)` under one of the `== ...` headings of `tests/run.sh` | A test of the tools failed. `tests/run.sh` shows only the last three lines of those. | Run it by itself for the reason: `python3 tests/boards_test.py`. One reason, `[SECTION] KEY is not explained in the template`: a key was added without its line in `devices/_template/` ([devices/README.md](../devices/README.md#adding-a-kind-of-data)). |
| `lint: no shellcheck on this machine: only the syntax was checked`, `== (no aarch64-linux-gnu-gcc or qemu-aarch64: the aarch64 run is left out)`, `== (no clang or lld-link: the loader is not built)` | Not failures. What is not installed is left out, and said. CI has all of it. | `sudo apt-get install gcc-aarch64-linux-gnu qemu-user shellcheck clang lld`. |
| `make check` passes and CI fails | CI also assembles Apple's release and compiles the overlay against it. | `make verify-kernel` and `make syntax-kernel`, which need the network. After a change to a driver, `kernel/syntax-check.sh --tree DIR` answers in seconds. |

## Where to ask

The repository's issue forms (`.github/ISSUE_TEMPLATE/`): one for something that does not work, one for
a new phone, one for the documentation. Say what was verified, and paste the log lines. Do not attach
Apple's files, the phone maker's firmware or device trees: a value read from one is a fact and is fine
([CONTRIBUTING.md](../CONTRIBUTING.md)).
