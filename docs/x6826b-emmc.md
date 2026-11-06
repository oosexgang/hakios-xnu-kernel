# The system image on the eMMC, not in memory (X6826B)

Until 28 October 2026 mtkloader read the whole system image — `userland/system.apfs`, 2.4 GB, in the
`xnusys` partition — into DRAM as the RAM disk md0, inside the 4 GB the kernel is given (0xC0000000 +
0xFE000000). Everything below `topOfKernelData` counts as the kernel's, so it was wired from the start:

| | md0 (run 320) | eMMC (runs 323–324) |
| --- | --- | --- |
| `vm_page_bootstrap` | 97 218 free, **162 878 wired** pages (2.5 GB) | 254 397 free, **5 699 wired** (89 MB) |
| a few minutes in | free 10–26 MB, available 250–360 MB, compressor 90–138 MB, "System is unhealthy", jetsam killing | free ~1 GB, available ~2.5 GB, compressor 0 |
| the loader reading the image | 203 s (11 MB/s) | none |

## How

- `bsd/dev/redwood_emmc.c`: the partition `xnusys` as a block device, read when asked for. MSDC0
  (0x11230000) in PIO mode, CMD17 / CMD18 + CMD12, as `mtkloader/msdc.h`: LK has set the card up and
  left it in the transfer state, nothing is initialised. The GPT is read for `xnusys`, the device is
  as long as the APFS containers in it. Read only by construction. 4 MB in 256 ms (16 MB/s).
  `/dev/emmc0`, `/dev/remmc0`; `kern.redwood.emmc` counts reads and errors.
- Boot-arg `emmc_root=1` makes it the root (`IOFindBSDRoot`). `rd=md0` stays on the command line:
  libignition takes its "ramdisk" boot from it — with `rd=emmc` it wanted a preboot volume and
  rebooted (run 322) — and the kernel skips the sealed-root check for it, as for md0 (run 321 panicked
  "rootvp not authenticated").
- mtkloader: `XNU_ROOT_EMMC` (1 by default) skips the read, passes `rd=md0 emmc_root=1`, and the
  payload's small image stays md1 (`RAMDisk1`; the kernel adds it as md1 even without an md0).
  `-DXNU_ROOT_EMMC=0` brings the old way back.
- The loader is built with `-Os`: the kernel with this driver was 2 KB over the boot partition at -O2.

## What it showed next

With the memory there, nothing is killed any more, and the CPU is the limit: in run 324 all four
cores were busy on the home screen with nothing touched (394–400% of one core), SpringBoard 220%,
and twenty-odd widget extensions (GeneralMapsWidget 86%, Calendar, Weather, Stocks, News, Screen
Time, World Clock...) most of the rest. `psgrep -cpu SECONDS` (userland/src/psgrep.c) says this.

## And then: MediaRemote

`sb_prof=SECONDS` (src/inject_sb.c) samples SpringBoard's running threads from inside for ten seconds
and keeps the stacks seen most. In run 328 they were MediaRemote's XPC handler spinning on
`dispatch_sync` and semaphores: `com.apple.mediaremoted`, its server, was not among the jobs (with
mediaserverd running, which the wallpaper needs, SpringBoard keeps hearing from the media side and
finds no server). With it (now among the stock jobs, userland/build.sh):

| run | the CPU on the idle home screen (of 400%) | SpringBoard |
| --- | --- | --- |
| 324 (widgets on) | 394–400% | 220% |
| 326 (no chronod) | 220% | 202% |
| 329 (mediaremoted, no chronod) | 14% | 2.5% |
| 330 (mediaremoted, widgets on) | 10% | 0.8% |

`DROP_JOBS="label ..."` (userland/build.sh) leaves jobs out of a run.

What is left in run 330 is the GPU: 55–59% of the busy frames within 18 ms with the wallpaper,
most of the rest within 35 ms (a frame every second refresh) — 92–96% without it before.
