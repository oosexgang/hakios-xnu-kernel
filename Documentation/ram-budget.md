# RAM budget: does Darwin 26 fit renoir's 6 GB as an in-RAM APFS image?

Date: 2026-10-30. Inputs: `darwin-renoir` (Darwin 16.5 port, read only), `darwin26/` (23G90, iPhone12,1).
No phone involved. All sizes are MiB (2^20) unless a line says GB (10^9).

## Verdict

**Not with the Darwin 16.5 recipe as it is. It does fit once you prune hard.**

| scenario | image | RAM left for kernel + processes |
|---|---|---|
| Darwin 16.5 port today (measured) | 2429 MiB = 2.55 GB | 3.40 GB (runs fine) |
| **A** Darwin 26, same `select-system.py` rules, full dyld cache | 4430 MiB = 4.64 GB | **1.30 GB: no** |
| **B** A, plus loctables cut to en/Base, poster apps dropped, 9 big apps dropped, AppFonts dropped | 3761 MiB = 3.94 GB | 2.00 GB: marginal |
| **C** B, plus the cache pages of images no boot or app uses zero-filled (closure "apps") | 3283 MiB = 3.44 GB | 2.50 GB: workable |
| C' same as C, closure "jobs" (boot to SpringBoard only) | 3156 MiB = 3.31 GB | 2.64 GB |
| **D** C, plus Maps, Setup and Music dropped, CameraEffectsKit/WeatherUI/AvatarKit resources dropped, no Sileo `/jb` | 2979 MiB = 3.12 GB | 2.82 GB: **yes** |

"RAM left" is `5.95 GB − image`. The 5.95 GB comes from the 16.5 figures: a 2.55 GB image and about
3.4 GB left. The loader caps the window at 6 GiB (`MAX_MEM_SIZE 0x180000000`) and refuses a
resident image larger than `window − 128 MiB − 768 MiB` (`RESIDENT_RD_OFFSET`, `RESIDENT_RD_RESERVE`).
That limit is 5248 MiB for a full 6 GiB window and about 4775 MiB for a 5.95 GB window. Even A
passes the loader check. The real problem is what is left over.

Why about 2.5–3 GB left is the bar: Darwin 26's smallest supported devices are the iPhone SE 2 (3 GB)
and the iPhone 11 (4 GB). Those totals include the kernel and the page cache [INFERENCE: Apple's
support list, not measured here]. Our kernel and processes get the RAM left after the image, and the
page cache is part of that. Every page a process touches from the compressed image is decompressed
into the UBC. Those pages are clean and can be evicted, but they still use RAM. Example: the
SpringBoard working set of the dyld cache.

**What it takes (target: image ≤ about 3.1–3.4 GB):**
1. Cut every `.loctable` to en/Base. This saves 354 MiB (measured: 363.7 → 10.2 MiB stored) and is
   the biggest safe step.
2. Drop the poster apps: 99 MiB. `KaleidoscopePosterApp.app` alone is 91 MiB.
3. Drop the large applications SpringBoard does not need: Podcasts, Freeform, Books, Bridge, Journal,
   Fitness, SOSBuddy, Home, FindMy. Together 189 MiB.
4. Trim the emoji font the way `font_trim.py` does for 16.5: about 70 MiB. This is already counted in
   A–D.
5. Zero-fill the dyld-cache pages of images outside the boot/app closure, in the code subcaches and
   `.dylddata`. This saves 478 MiB (closure "apps") up to 605 MiB (closure "jobs"). It needs:
   - a kernel that does not validate shared-cache pages against the cache's code signature (needed
     for the PAC rewrite anyway);
   - an exact keep-list taken from a traced boot. dlopen() is invisible to the static closure used
     here, and a zeroed image that is opened later crashes its process.
6. If 3.4 GB free is required as on 16.5, go further: keep about 10 applications and prune asset
   catalogs (539 MiB of `.car` files in the selection).

## 1. How the Darwin 16.5 image is chosen (darwin-renoir)

`darwin165/select-system.py` reads `7z l -slt rootfs.dmg` and writes `system.files` (what to extract)
and `system.manifest` (mode/uid/gid/symlinks, used by `tools/apfs/mkapfs.py`). Rules (`wanted_file`):

- Taken whole: `usr/{libexec,sbin,bin,lib,share}`, `sbin`, `bin`, `private/etc`, `System/Library`,
  `Applications`, `Library`.
- Left out entirely:
  - these `System/Library` subdirectories: LinguisticData, PreinstalledAssetsV2, NanoTimeKit, Audio,
    Assistant, AccessibilityBundles, VideoProcessors, ScreenReader, Photos, Carrier Bundles,
    NanoPreferenceBundles, ImagingNetworks, ProductDocuments, AppPlaceholders, UserNotifications,
    Caches;
  - `usr/share/{tokenizer,firmware,mecabra}`, `usr/standalone`, `Library/{Audio,Ringtones}`.
  - Exceptions: the AUDIO list (HAL plug-ins, D22/Generic tunings, UISounds without nano).
- `.lproj` directories: only en/Base/English/en_US/en_GB are kept.
- Programs in `usr/libexec` and `usr/sbin`: only those ≤ 10 MB.
- Framework directories (the code itself lives in the dyld cache):
  - resources ≤ 4 MB, without ML models (`.mlmodelc`), shaders, media, `.dat`/`.bin` (except the
    TextInputUI/CoreEmoji/CFNetwork tables);
  - `.car` files ≤ 4 MB, except SFSymbols (taken whatever the size);
  - `.loctable` files ≤ 16 MB, each holding every language;
  - XPCServices/Support/Helpers ≤ 32 MB.
- Applications: taken without their media or ML models. `private/var/staged_system_apps` moves into
  `/Applications`. The rest of `private/var` is left out.
- Fonts: those ≤ 25 MB, plus AppleColorEmoji and PingFang, which are trimmed afterwards
  (`userland/tools/font_trim.py`).

`darwin165/fetch.sh` adds the OS cryptex. It leaves out the dyld cache and DriverKit, then adds the dyld
cache separately without `.symbols`. `userland/userland/build.sh` (system image section) does the rest:

- hard-links the selection into `sysroot`;
- replaces LaunchDaemons with a link to `/hakios/LaunchDaemons`;
- puts stage0 at `/sbin/launchd`;
- adds the ADSP firmware (`fw-audio-renoir`), Sileo's `/jb`, and the cryptex with the cache at
  `System/Cryptexes/OS/System/Library/Caches/com.apple.dyld`;
- runs `mkapfs.py -z`: 64 KiB zlib-9 pieces in a decmpfs resource fork (type 4), or the whole file in
  the attribute (type 3) when it fits in 3804 bytes; raw when that does not save blocks.

Measured from `userland/system.apfs` (2,546,769,920 B; `7z l -slt`, "Packed Size" = stored blocks):

| part of the 16.5 image | files | raw MiB | stored MiB |
|---|---:|---:|---:|
| dyld cache, code subcaches (main + numbered) | 39 | 1983.2 | 814.9 |
| dyld cache `.dyldlinkedit` | 2 | 337.5 | 77.5 |
| dyld cache `.dylddata` | 2 | 275.7 | 55.3 |
| **dyld cache total** (43 files, no `.dyldreadonly` in 16.5) | 43 | **2596.4** | **947.7** |
| Darwin files without the cache | 44,400 | 2786.0 | 1380.7 |
| `/jb` (Procursus + Sileo) | 5,258 | 114.1 | 41.6 |
| `fw-audio-renoir` (stored plain) | 37 | 20.9 | 21.0 |
| sum of files | 49,738 | 5517.4 | 2390.9 |
| APFS metadata (image − sum) | | | 37.9 |

## 2. Darwin 26 dyld cache, raw and as mkapfs stores it

Method: `darwin/tools/ram-budget/zchunks.py` reproduces `compressed_fork`/`zlib_rsrc` from `mkapfs.py`
exactly. It was checked byte-exact against three files in the 16.5 image (SpringBoard
112048 → 16384, `dyld_shared_cache_arm64.35` 115834880 → 48005120, `.32` 606208 → 159744). It runs as
16 processes over 4 MB spans: 24 s for 5.2 GB.

| Darwin 26 cache part | files | raw MiB | stored MiB | ratio |
|---|---:|---:|---:|---:|
| main + numbered code subcaches (.01–.32, .36–.69, .73–.80) | 75 | 3760.5 | 1483.4 | 39.4 % |
| `.dylddata` (.33 .70 .81) | 3 | 494.7 | 84.1 | 17.0 % |
| `.dyldreadonly` (.34 .71 .82) | 3 | 134.7 | 37.9 | 28.1 % |
| `.dyldlinkedit` (.35 .72 .83) | 3 | 570.2 | 134.8 | 23.6 % |
| **total, all 84 files dyld needs** | 84 | **4960.0** | **1741.0** | 35.1 % |
| not needed: `.symbols` | 1 | 1004.3 | n/a | |
| not needed: `.atlas` | 1 | 2.7 | n/a | |

Compared with 16.5, the cache is 1.84× larger stored (1741 vs 948 MiB). Per-file numbers:
`$RB/dyld26.txt` (regenerate with the commands below). The PAC rewrite turns instructions into
NOP/RET/BR/BLR, so the stored size should stay the same or shrink slightly [INFERENCE].

## 3. Which cache files must exist, and what zero-filling would save

Header of `dyld_shared_cache_arm64e` (parsed by `cache_images.py`):

- `subCacheArrayOffset 0x436d8`, `subCacheArrayCount 83`. These are v2 entries of 56 bytes (UUID,
  cacheVMOffset, suffix). The suffixes are `.01` … `.83`, including all three `.dylddata`,
  `.dyldreadonly` and `.dyldlinkedit` files.
- `.symbols` is not in the array. It is referenced only by `symbolFileUUID`, and
  `localSymbolsOffset` is 0.
- `.atlas` is not in the array, and `cacheAtlasOffset` is 0.

So the main file and all 83 subcaches must be present at full length. dyld opens every listed
subcache, checks its UUID, and hands all their mappings to the kernel as one shared region
[INFERENCE from dyld's SharedCacheRuntime, not re-read here]. With `dylibsExpectedOnDisk` off on
Darwin, a missing or short file leaves no fallback. `.symbols` and `.atlas` can be left out (16.5
already leaves out `.symbols`).

Mappings by kind (MiB):

- code subcaches: 3732.8 `r-x`, plus 19.0 `r-x` stub islands (flag 0x8, the small even-numbered
  files);
- `.dylddata`:
  - 232.5 AUTH_CONST (`r--`, flags 0x5)
  - 121.8 DATA (`rw-`)
  - 81.9 DATA_CONST (`r--`, 0x4)
  - 57.2 AUTH (`rw-`, 0x1)
- `.dyldreadonly`: 134.3 `r--` (0x20, read-only data: ObjC/Swift optimisation tables);
- `.dyldlinkedit`: 569.0 `r--`. There are only 3 distinct `__LINKEDIT` ranges, one per linkedit file,
  shared by 2322, 1346 and 637 images. Linkedit cannot be pruned per image without rebuilding it.

So the `.dylddata`/`.dyldlinkedit` files are mapped whole, but processes touch only the pages of
loaded images. `.dylddata` pages are slid by the kernel when they are faulted in. The parts of
`.dylddata` that belong to unused images can be zeroed; `.dyldlinkedit` cannot.

Image closure: static closure over LC_LOAD/WEAK/REEXPORT/UPWARD_DYLIB from root programs.

- **jobs**: the 22 launchd jobs `build.sh` loads (DARWIN_JOBS + wifid), plus SpringBoard, xpcproxy and
  launchd.
- **apps**: jobs plus the 15 applications in the 16.5 port's screenshots (Settings, Safari, Calendar,
  Clock, Health, Maps, Messages, Music, News, Phone, Reminders, Wallet, App Store, Notes, Weather),
  with their extensions.
- **broad**: every Mach-O executable in rootfs (1859 programs).

The per-image segments of all 4305 images add up to: `__TEXT` 3723.5, `__AUTH_CONST` 229.5, `__DATA`
120.4, `__OBJC_RO` 85.0, `__DATA_CONST` 80.8, `__DATA_DIRTY` 29.1, `__AUTH` 24.9 MiB raw.

| closure | images used / 4305 | stored MiB, used images (code + data + ro) | stored MiB, unused images | whole 64 K pieces saved by zero-fill | cache after zero-fill |
|---|---:|---:|---:|---:|---:|
| jobs | 1752 | 929.9 | 652.0 | **604.7** | 1136 |
| apps | 1970 | 1062.2 | 519.7 | **478.2** | 1263 |
| broad | 2886 | 1401.1 | 180.8 | **161.3** | 1580 |

(A zero 64 KiB piece costs about 90 bytes stored. Linkedit (134.8) and the non-image parts of the
other subcaches stay as they are.)

The static closure misses dlopen'd frameworks and bundles. A real keep-list should come from a traced
boot plus a session using the apps (a dyld/kernel log of mapped images on QEMU). Use "apps" as the
planning number and "broad" if every shipped program must still run.

## 4. Minimal rootfs: the 16.5 selection applied to Darwin 26

`rootfs_select.py` imports `wanted_file` from `darwin-renoir/darwin165/select-system.py` unchanged. It
walks `darwin26/rootfs` (staged apps → `/Applications`, rest of `private/var` left out), adds
`cryptex/os` without the dyld cache and DriverKit, adds `cryptex/app` whole, and compresses every
file the way mkapfs does (16 processes, 45 s). Hard-link copies are counted separately, as mkapfs
does with the 7-Zip extraction for 16.5.

| category | 16.5 stored MiB | Darwin 26 files | Darwin 26 raw MiB | Darwin 26 stored MiB |
|---|---:|---:|---:|---:|
| System/Library/PrivateFrameworks | 525.4 | 30,257 | 1898.7 | 913.1 |
| Applications | 300.9 | 6,649 | 1316.8 | 710.3 |
| System/Library (other) | 86.6 | 12,645 | 665.3 | 249.2 |
| System/Library/Fonts (26: emoji untrimmed 131.7) | 178.4 | 263 | 269.1 | 205.1 |
| System/Library/CoreServices | 38.9 | 928 | 169.9 | 121.5 |
| System/Library/Frameworks | 62.6 | 2,419 | 268.0 | 105.1 |
| usr/libexec | 52.6 | 577 | 239.0 | 92.3 |
| System/Library/PreferenceBundles | 24.6 | 1,234 | 135.9 | 52.2 |
| System/Library/Health | 33.2 | 356 | 77.5 | 50.1 |
| usr/share | 22.2 | 1,364 | 128.5 | 44.3 |
| Library | 28.8 | 1,618 | 53.3 | 35.7 |
| usr/lib (26: libCoreKE.dylib 18.8) | 1.3 | 392 | 47.3 | 29.5 |
| System/Cryptexes/OS (non-dyld) | 9.8 | 2,011 | 63.9 | 22.6 |
| usr/sbin | 13.7 | 46 | 22.6 | 10.3 |
| System/Cryptexes/App | n/a | 1,097 | 11.6 | 6.1 |
| usr/bin, sbin, private, bin | 1.7 | 58 | 6.5 | 1.9 |
| **total** | **1380.7** | **61,914** | **5374.0** | **2649.2** |

With the emoji font trimmed as on 16.5 (about −70 MiB), the total is about 2579 MiB: 1.87× the 16.5
selection.

Pruning candidates within that selection (stored MiB):

- **`.loctable`: 5012 files, 363.7 stored. Cut to en/Base: 10.2, saving 353.5 (measured,
  `loctable_trim.py`).**
- `.car` asset catalogs: 539.0 in total. SFSymbols (82.4) is required. Maps 44.0, MobileCoreTypes
  29.6, AccessibilitySettings 13.9, Health 12.6 …
- Poster apps (Kaleidoscope, Unity, Pride, Gradient, Emoji, WeatherPoster): 99.0.
- Applications, largest first:

  | app | stored MiB | app | stored MiB |
  |---|---:|---|---:|
  | Maps | 60.5 | FindMy | 16.2 |
  | Setup | 50.4 | Journal | 15.4 |
  | Podcasts | 38.3 | Notes | 14.6 |
  | Freeform | 34.1 | Health | 13.9 |
  | Music | 31.6 | Fitness | 12.9 |
  | Books | 30.3 | SOSBuddy | 12.7 |
  | Bridge | 17.4 | Home | 12.0 |

  All 255 app bundles together: 710.3.
- System/Library/ExtensionKit: 76.7. Check first: some Settings panes are extensions.
- CameraEffectsKit 55.1, WeatherUI 38.8, AvatarKit 25.9, PassKitUI 22.6.
- Fonts/AppFonts: 27.2.
- System/Library/Health: 50.1. 16.5 needed it for Health and healthd. If Health goes, it can go.

## 5. Image size estimate

| component | stored MiB |
|---|---:|
| dyld cache (84 files) | 1741.0 |
| rootfs + cryptex selection (16.5 rules) | 2649.2 |
| emoji trim (as 16.5) | −70 |
| `/jb` (Sileo, as 16.5) | 41.6 |
| `fw-audio-renoir` | 21.0 |
| APFS metadata (16.5: 37.9 for 49.7 K files, scaled) | ~47 |
| **A: image** | **≈ 4430 MiB = 4.64 GB** |

The scenarios B–D at the top subtract the measured savings from A:

- B: −669.0. Loctables −353.5, posters −99.0, 9 apps −189.3, AppFonts −27.2.
- C: B − 478.2 (cache zero-fill, closure "apps"). C' uses closure "jobs" instead: −604.7.
- D: C − 303.9 (Maps, Setup, Music, CameraEffectsKit, WeatherUI, AvatarKit, `/jb`).

## Reproduce

Run in WSL. Scripts are in `darwin/tools/ram-budget/`; intermediate files go to a directory of your own,
`$RB` below: `ram-budget` in `$XDG_CACHE_HOME` (`~/.cache`), or `RAM_BUDGET_CACHE`, which `cachedir.py` makes
0700 and prints. They are pickles, which run code as they are read: not in a shared `/tmp`.

```
RB=$(python3 .../cachedir.py)
cd DARWIN26/dyld        # (the unpacked Darwin 26 image's dyld directory)
python3 .../zchunks.py $RB/dyld26.pkl $(ls $PWD/dyld_shared_cache_arm64e* | grep -v -E '\.(symbols|atlas)$')
python3 .../cache_images.py $RB/dyld26.pkl jobs|apps|broad     # writes $RB/used_MODE.pkl
python3 .../rootfs_select.py            # writes $RB/rootfs26.pkl
python3 .../loctable_trim.py            # needs $RB/rootfs26.pkl
python3 .../prune_candidates.py         # needs $RB/rootfs26.pkl
7z l -slt userland/userland/system.apfs > $RB/sys165.slt && python3 .../slt_summary.py $RB/sys165.slt
```
