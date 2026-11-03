# Changelog

What changed, newest first. One line a change: the part of the system, then what is different for whoever runs it.
The commits say how; `docs/` says why.

## Sources

What a build is made of, and where each part is fetched from. Nothing of these is kept in the repositories but the
changes to it; the scripts named fetch the release and put the changes on.

- the kernel: Apple's [XNU](https://github.com/apple-oss-distributions/xnu), tag `xnu-8796.121.2` (the release the Darwin 16.5 user space was built from) -- `xnu/apply.sh`, the changes in `xnu/hakios.patch` and `xnu/files/`, the drivers in `drivers/`
- the data volume: Apple's [HFS Plus](https://github.com/apple-oss-distributions/hfs), tag `hfs-627.100.6` -- `xnu/hfs/hfs.sh`, the changes in `xnu/hfs/hfs.patch`
- the boot: [U-Boot](https://source.denx.de/u-boot/u-boot) `v2026.07` -- `hakios-uefi/u-boot/apply.sh`, the changes in `hakios-uefi/u-boot/hakios.patch` and `files/`; the device's part in `hakios-sm7325/u-boot/`
- the UEFI (the other way to start): [Project Silicium's Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) at `e51cd877e31c` -- `hakios-uefi/apply.sh`
- the GPU's OpenGL ES: [Mesa](https://archive.mesa3d.org/) 25.0.7, the freedreno driver for the Adreno -- `tools/mk-mesa.sh`, the changes in `tools/mesa/`
- the build tools: Apple's [libdispatch](https://github.com/apple-oss-distributions/libdispatch), [libpthread](https://github.com/apple-oss-distributions/libpthread) and [AvailabilityVersions](https://github.com/apple-oss-distributions/AvailabilityVersions), [bootstrap_cmds](https://github.com/markmentovai/bootstrap_cmds), [unifdef](https://github.com/fanf2/unifdef), the [macOS 13.3 SDK](https://github.com/joseluisq/macosx-sdks) and the [iPhoneOS 16.5 SDK](https://github.com/theos/sdks) -- `xnu-deps/setup.sh`
- not fetched by anything here: the Darwin 16.5 user space, which you lay out from your own IPSW (`sysimage/`), and the phone's own firmware, pulled from its Android (`fw-*`)

## 2026-11-03

- metal: luma_boost is left out when QuartzCore's library has its own (an iOS 17 one, off a Mac): it was 83% of the GPU's timed passes; `metal_luma_boost=1` draws it
- metal: `metal_defer=1` records the window server's command buffers on a thread of hakios-metal's own -- a draw costs its render thread ~5 us instead of ~40
- metal: `metal_fps=passes` prints every pass of a slow frame; the full-screen pass of the home screen is 6 to 13 ms of the GPU's ~10 a frame
- display: `fbsubtype=N` with `fbwidth`, `fbheight` and `fbscale` reports another iPhone's screen class (the XR's 828 x 1792 at 2: no red rdar status bar)
- springboard: vibrant views keep their filters when the window server draws through Metal; `ui_lite=1` turns on Reduce Transparency and Reduce Motion, `ui_lite=0` turns them off
- windows: `tools/metal-run.sh` runs on eight cores at 120 Hz with six scaling threads and without `-v` (VERBOSE=1 brings it back), and its summary shows the frames on the panel; `tools/frames.py` counts the Metal road's frames and the share over 8.3 ms
- loader: `xnuloader/mkidentity.sh` builds lzfse by its default target (there is no `lzfse` one)

## 2026-11-02

- metal: the window server's compiled shader stages outlive the boot -- kept in `/var/db/hakios-metal-cache` (var_store left the old `/var/tmp` out), keyed by what the compiler is, so a rebuilt plug-in keeps them; and carried in the image (`tools/metal-cache-seed.sh`), so no boot compiles them
- metal: `metal_fps` says why each pass stayed in sysmem, the slowest and the heavy one included
- windows: `tools/metal-run.sh`, a measured Metal run in one command from Git Bash (`var_store` on by default); `tools/ca-metallib.py` off a Mac with `CA_QC_METALLIB=`

## 2026-10-30

- repositories: `main` is the branch of `hakios-xnu-kernel` -- what was `hakios` and then `u-boot`; `hakios` is gone, `git clone` needs no `-b`
- site: a Changelog page, this file
- site: the commands to start with clone the public repository (`hakios-xnu-kernel`); `oosexgang/hakios` is private and cloned for nobody
- site: the documentation synced, with the guide to installing beside Android and the U-Boot page in the table of contents

## 2026-10-29

- u-boot: HakiOS's own U-Boot, started by the phone's bootloader from RAM; it starts the loader and the kernel from memory, nothing of the vendor's firmware in between
- u-boot: the loader's fastboot works under U-Boot (USB 2.0 without the link power states that dropped the cable after 3 s, endpoints and packet sizes as the loader asks, DWC3 transfers started when queued)
- u-boot: every download and every step says what it does on the screen, tagged `[oosexgang]`
- u-boot: `hakios sum` checks a flashed partition from the phone itself; `hakios reboot bootloader` gets to fastboot
- install: HakiOS beside Android: U-Boot in `boot_a`, Android in slot b untouched, a menu at every start (Android after 10 s, HakiOS (Darwin), fastboot, U-Boot's shell)
- install: choosing Android marks HakiOS's slot unbootable and lets the bootloader switch; U-Boot never changes which slot is active (changing it by hand once left every image refused with "Device Error")
- install: `tools/userdata-carve.py --wipe` makes `darwin` at the end of userdata; `tools/sparse-split.py` cuts an 8 GB image into pieces Windows fastboot can send
- storage: the data volume is HFS Plus in `darwin` after the system, about 12 GB, journaled, kept between starts; before, `/private/var` was a memory volume shown as 1 GB and empty at every start
- storage: HFS Plus built into the kernel; Apple's sources are fetched at `xnu/apply.sh`, only this port's changes are kept
- storage: the data volume's writes go straight to the flash and the cache is flushed when the journal asks (a flush and a read back after every write made the system crawl)
- storage: the loader takes any phone's `darwin` (name, type, unit, at least 20 GiB), not only the first phone's place and identifier
- storage: `tools/mkdatavol.py` makes the empty volume as a 50 KB image that leaves the system volume alone
- graphics: the installed image draws with the GPU (Mesa, `GL=1`): smooth, and the wallpaper drawn
- graphics: the system's other services and the processes that draw parts of the interface are in the image (`REST=1`): the wallpaper and the applications that need them
- graphics: frames at 120 a second (`fb_hz=120`), and `fb_measure=1` writes to the log how often the panel itself refreshes
- loader: nothing printed over the system's screen while it runs; the log still takes every line
- loader: its log written under U-Boot too; a download the computer gave up on no longer hangs it
- wifi: the Wi-Fi controller is found where the system looks for its address, so registering applications no longer waits 5 s for each (found by the ios-sapphire port)
- tree: Darwin, not iOS, in the sources, the scripts, the file names and the documentation; no file of Apple's in the repositories
- docs: installing beside Android, step by step (`docs/dual-boot.md`, in Russian too), and the U-Boot page with everything as it was done and checked
