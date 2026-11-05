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
- the GPU's Vulkan: the same Mesa's Turnip -- `tools/mk-turnip.sh`, the changes in `tools/mesa/turnip-darwin.patch` and `tools/mesa/hakios-drm/`
- Metal, the direct path: `hakios-metal/` in this tree -- `userland/build.sh METAL_PLUGIN=ca`, with `userland/src/HakiosMetalCA.m`
- Metal, on Vulkan: [UniXNU/AdrenoMetal](https://github.com/UniXNU/AdrenoMetal) at `243d63c` and [steelbrain/metal2vulkan](https://github.com/steelbrain/metal2vulkan) at `624bab3` with AdrenoMetal's 30 patches -- `tools/mk-m2v.sh`, the changes in `tools/m2v/`
- the build tools: Apple's [libdispatch](https://github.com/apple-oss-distributions/libdispatch), [libpthread](https://github.com/apple-oss-distributions/libpthread) and [AvailabilityVersions](https://github.com/apple-oss-distributions/AvailabilityVersions), [bootstrap_cmds](https://github.com/markmentovai/bootstrap_cmds), [unifdef](https://github.com/fanf2/unifdef), the [macOS 13.3 SDK](https://github.com/joseluisq/macosx-sdks) and the [iPhoneOS 16.5 SDK](https://github.com/theos/sdks) -- `xnu-deps/setup.sh`
- not fetched by anything here: the Darwin 16.5 user space, which you lay out from your own IPSW (`sysimage/`), and the phone's own firmware, pulled from its Android (`fw-*`)

## 2026-11-05: 2.3.4

- metal: a pipeline CoreAnimation makes at run time, which its library does not hold, gets the nearest variant of the same kind and not one that draws something else. That was the red at the top of the default wallpaper (its head was not drawn), the white band behind the status bar (`darkenSourceOver`), and materials like the Weather widget's
- touch: a finger's lift 5 ms after its last move at most 6, on the phone under real fingers; 88 to 169 before. `touch: N strokes` in the log says so without `touch_lat=1`'s lines
- metal: at the default screen with the right shaders, on the phone: 109.6 frames a second over the bench's gestures, 1.3% of them over 16.7 ms. 120 is not reached: what is left is SpringBoard's main thread at the start of a gesture (up to 100 ms on the bench, up to 300 under fingers), and the GPU at the home button and opening an app
- windows: `tools/metal-run.sh` renders at the iPhone X's layout at 2 pixels a point by default, 750x1624 stretched to the panel, in place of the iPhone XR's screen: at that class a swipe from the bottom edge opened a sheet under real fingers. Not yet seen under real fingers at this one
- ports: `devices/<name>/metal.args` holds a phone's own arguments for `tools/metal-run.sh`, and a phone other than redwood no longer gets redwood's
- docs: the READMEs, the index and the Windows guide name `hakios-metal/` of this tree; the repository beside it says it has moved

## 2026-11-05: 2.3.3

- metal: `hakios-metal/` is in this tree with its history, and `userland/build.sh` links it: one checkout builds everything
- framebuffer: `fb_beam` and `fb_measure` read the display interface's registers only on `sc7280` or with `fb_intf=`: they read SM7325's address on any SoC before
- hakios-metal: a GPU of another chip ID is refused by name, and the window server stays on OpenGL ES
- tools: `tools/metal-run.sh` takes the phone's own arguments from `DEVICE_ARGS=`; [Port a device](docs/porting/index.md) says what Metal needs from a port
- touch: with a finger down every look reads a report; a finger's lift no longer waits up to 169 ms for the look of every 80 ms
- framebuffer: slow frames are filtered at once, so a still screen no longer moves back and forth 10 times a second; nothing is drawn again from a surface the GPU has taken back

## 2026-11-04

- metal: 120 frames a second is not reached: hakios-metal 101 to 103 in page swipes, about 90 in Settings' scroll, 89 to 92 at the home button, 4 to 6% of frames over 16.7 ms; after the finger lifts, 185 of a swipe's 205 frames take 8.3 ms, while it drags 75 to 80 a second; `docs/reference/metal-measurements.md`
- springboard: `wallpaper_freeze=1` makes the wallpaper one picture, from the IOSurface on the panel (`kern.hakios.fb_surface`): home button 92 frames a second, against 28 with `wallpaper_cache=1` and 50 without either; the icons blink once, the wallpaper does not change until the next boot, and the lock screen shows the same picture
- windows: `tools/metal-run.sh` sets `wallpaper_freeze=1` and `metal_defer=1`; an argument in `MORE` takes the place of a default of the same name
- metal: 250 touch samples a second instead of 125 change nothing (100 to 102); `touch_poll_us` stays 4000 in `tools/metal-run.sh`
- springboard: the status bar's legibility gradient (compositing filter `darkenSourceOver`) came out solid white over the top eighth of the home screen, on both Metal paths; it is drawn without the filter now, the wallpaper reaches the top and the status bar reads (2.3.4 found why, and took this out)
- windows: `tools/metal-run.sh` renders at the iPhone XR's screen by default, 828x1792 stretched to the panel: Settings' scroll 118 frames a second, swipes 110 to 115, home button 106, 1% of frames over 16.7 ms; the wallpaper is the picture a boot at the iPhone X's screen kept
- display: `fb_beam=1`, the default: each thread's rows are written behind the panel's scan line (2433 lines, 8338 us), and to the eye the moving bands are gone; it costs no frames (Settings' scroll 91 against 89, home button 89 to 92 against 81, swipes 101 against 99)
- docs: a short README and a front page by what you came to do; `docs/reference/metal.md` and its measurements; every boot-argument of both Metal paths in the catalogue (`tools/boot-args.py` reads `hm_boot_arg_set`)
- licenses: Turnip, AdrenoMetal, metal2vulkan and the plug-in's origin in `THIRD_PARTY_LICENSES.md`; a binary built with `M2V=1` has obligations and is **to verify** before any distribution

## 2026-11-03

- metal: Metal on Vulkan, as UniXNU runs it: `tools/mk-turnip.sh`, `tools/mk-m2v.sh`, `userland/build.sh TURNIP=1 M2V=1`; nothing of it in `prebuilt/`, you fetch and build it
- metal: `vk_test` passes on the phone (`RC_JOBS=rc.jobs.vktest`): Vulkan 1.3.305, a 4 MB fill checked word by word, a submission and its fence wait 2.0 ms on average
- metal: `m2v_test` passes on the phone (`RC_JOBS=rc.jobs.m2vtest`): the device in 134 ms, a 1080 x 2400 clear checked pixel by pixel, a full-screen clear committed and waited for in 1.08 ms
- metal: `metal_m2v=1`: the window server draws through metal2vulkan, correctly since it opens QuartzCore's pipeline library and picks CoreAnimation's shader variants by label: swipes 50 to 57, scroll 41, the GPU 14 to 21 ms a frame; `tu_debug=` and `m2v_utrace=1` say why
- gpu: HakiosGPU's request `0x7d`: the client's own pages as a buffer; Turnip takes any page-aligned pointer through `VK_EXT_external_memory_host` (grey icons and text before)
- gpu: Turnip has `VK_NV_linear_color_attachment` on Darwin, unused so far: the window server's pass has a second attachment that is not linear
- springboard: `wallpaper_cache=1` rasterizes the wallpaper's window: page swipes 85-91 to 97-103, an app's close slower (22-30 against 40); `wallpaper_freeze=2` hides the wallpaper: home button 78 frames a second against 28, swipes unchanged; `wallpaper_freeze=1`'s picture comes out one colour here, and is not used
- metal: `metal_fps=mainpass` prints the draws of a heavy pass, `metal_fps=passes` each pass's attachments
- display: surfaces let go of when their pixels are on the panel, not when the next frame comes: no change in frame rate
- metal: luma_boost is left out when QuartzCore's library has its own (a later system's, off a Mac): it was 83% of the GPU's timed passes; `metal_luma_boost=1` draws it
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
