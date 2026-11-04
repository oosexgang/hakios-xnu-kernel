# Third-party licenses

Every third-party component HakiOS redistributes or fetches at build time, with its origin,
license, and status. Where "origin uncertain" or "to verify" is written, the item is **not**
asserted to be cleared for redistribution; treat it as pending review (see `COMPLIANCE.md`).

This supersedes the summary in `NOTICE`, which stays as the short, canonical list.

## Redistributed in this repository

| Component | Path | Origin | License | Notes |
|---|---|---|---|---|
| XNU modifications | `xnu/hakios.patch`, `xnu/files/`, `drivers/` | derivative of Apple XNU | APSL 2.0 (`LICENSE.APSL`) | base tag `xnu-8796.121.2`, fetched by `xnu/apply.sh` |
| libdeflate | `xnu/files/bsd/miscfs/apfs/libdeflate/` | Eric Biggers | MIT | see its `COPYING` |
| libzbitmap | `xnu/files/bsd/miscfs/apfs/libzbitmap/`, `xnu-deps/build/lzbitmap/` | Corellium LLC (port of eafer/libzbitmap) | GPL-2.0+ OR MIT | used under MIT |
| LinuxKPI / Linux headers | `wifi/lx/include/linux/`, `wifi/lx/include/net/` | Linux / FreeBSD LinuxKPI | GPL-2.0 (Linux) / BSD-2-Clause (FreeBSD) | small compatibility headers; see each file's notice |
| Linux DRM shim | `tools/mesa/hakios-drm/`, `xf86drm.h` | Linux / X.Org | MIT | build-time shim for Mesa |
| Mesa patches | `tools/mesa/*.patch` (`turnip-darwin.patch` among them) | changes to Mesa, HakiOS | MIT (Mesa's terms for the files they change) | patches only; Mesa itself is fetched |
| AdrenoMetal patch | `tools/m2v/adrenometal-ios16.patch` | changes to UniXNU/AdrenoMetal, HakiOS | MIT, Copyright (c) 2026 NativeMetal contributors | patch only; the source is fetched. `tools/m2v/ios16-compat.h` is our own, MIT |
| metal2vulkan patch | `tools/m2v/metal2vulkan-hakios.patch` | changes to steelbrain/metal2vulkan, HakiOS | GPL-2.0-or-later OR LGPL-3.0-or-later, as the code it changes; **not MIT** | patch only, and its context lines are their code. The file carries no notice of its own: **to verify** that this is enough |
| The window server's Metal plug-in | `userland/src/HakiosMetalCA.m` | begun from vcvkk's `userland/src/HakiosMetal.m` (github.com/vcvkk/darwin-redwood, branch hakios-workspace, at eb9fdf5), with this tree's changes | **to verify**: that repository states no license (read 2026-11-04); this tree's changes are MIT | not in `prebuilt/` |
| AIR / Metal-library tooling | `tools/mesa/air/` | HakiOS (Darwin on redwood contributors) | MIT | our own; LLVM bitcode layout is a published format |
| mkbootimg.py | `devices/redwood/prebuilt/mkbootimg.py` | Android Open Source Project | Apache-2.0 | |
| Mu-Silicium / Project Mu derived files | `devices/redwood/uefi/` | Project Silicium, Microsoft, contributors | BSD-2-Clause-Patent; some GPL-2.0+ OR MIT | derived parts keep upstream license |
| Qualcomm/Xiaomi UEFI driver binaries | `devices/redwood/uefi/files/Binaries/` | Qualcomm / Xiaomi (from device `xbl.img`) | vendor's — **to verify** | needed only to rebuild UEFI; removable, re-extract with UEFIReader |
| FocalTech touch firmware header | `drivers/input/hakios.focaltech_fw.h` | FocalTech (device firmware blob) | vendor's — **to verify** | candidate to move to pull-from-device |
| Apple host headers (build only) | `xnu-deps/hostinc/mach-o/`, `.../mach/` | Apple (open-source cctools/xnu headers) | APSL 2.0 | host-side Mach-O headers for the cross build |
| Pre-built kernel & user space | `prebuilt/` | built from this repo | APSL 2.0 / MIT (as source) | reproducible from source here |

## Fetched at build time, never committed

| Component | Fetched by | Origin | License |
|---|---|---|---|
| Darwin userland 16.5 IPSW (user space of the system image) | `sysimage/unpack.sh` | Apple | Apple's — user supplies, user's own copy |
| iPhoneOS 16.5 SDK | `xnu-deps/setup.sh` (github.com/theos/sdks) | Apple (headers) via theos | Apple's / theos |
| Apple open sources (AvailabilityVersions, libdispatch, libpthread, cctools, bootstrap_cmds) | `xnu-deps/setup.sh` | Apple | APSL 2.0 |
| MacOSX13.3 SDK (Kernel.framework DriverKit headers only) | `xnu-deps/setup.sh` | Apple via joseluisq/macosx-sdks | Apple's |
| lzfse | `xnuloader/mkidentity.sh` (when no decoder present) | Apple, github.com/lzfse/lzfse | BSD-3-Clause |
| ath11k (Wi-Fi driver) | `tools/mk-ath11k.sh` | Linux | BSD-3-Clause-Clear |
| Mesa (GL=1 images) | `tools/mk-mesa.sh` | Mesa project | MIT and others |
| Turnip, Mesa's Vulkan driver (TURNIP=1) | `tools/mk-turnip.sh`, in `tools/mk-mesa.sh`'s tree | Mesa project, 25.0.7 | MIT and others |
| hakios-metal (METAL_PLUGIN=ca) | beside this tree, `userland/build.sh` | [oosexgang/hakios-metal](https://github.com/oosexgang/hakios-metal) | MIT |
| AdrenoMetal's frontend and bridge (M2V=1) | `tools/mk-m2v.sh`, pinned at `243d63c` | [UniXNU/AdrenoMetal](https://github.com/UniXNU/AdrenoMetal) | MIT, Copyright (c) 2026 NativeMetal contributors |
| metal2vulkan (M2V=1) | `tools/mk-m2v.sh`, pinned at `624bab3`, with AdrenoMetal's 30 patches | [steelbrain/metal2vulkan](https://github.com/steelbrain/metal2vulkan) | `GPL-2.0-or-later OR LGPL-3.0-or-later`, the user's choice (its `LICENSE`) |
| Rust crates of that build (M2V=1) | `cargo`, by `tools/mk-m2v.sh`; `xnu-deps/build/m2v/bridge/Cargo.lock` | crates.io: ash, libc, libloading, sha2 and their dependencies, 34 packages | those read: MIT OR Apache-2.0 (22), MIT (3), ISC (1), Unlicense OR MIT (1); the 7 not found in the local registry (proc-macro2, quote, serde, serde_derive, syn, unicode-ident, windows-link): **to verify** |
| Rust's standard library (M2V=1) | the Rust 1.98 toolchain, target `aarch64-apple-ios` | the Rust project | MIT OR Apache-2.0 |
| QuartzCore's `default.metallib` for the window server's Metal | `tools/ca-metallib.py`, from your own Mac or a later system's IPSW (`CA_QC_METALLIB=`) | Apple | Apple's: user supplies; the result stays on your machine and is never committed |
| Procursus / Sileo packages | `tools/mk-jb.sh` | Procursus team | their own licenses |
| Phone firmware (GPU, Wi-Fi, DSP, modem, haptics, cameras) | `devices/redwood/pull-firmware.sh` | Qualcomm / Xiaomi | vendor's — user's own device |

## A binary built with `M2V=1`: to verify

`userland/build.sh TURNIP=1 M2V=1` links `libadreno_metal_bridge.a`, which holds the metal2vulkan runtime (`GPL-2.0-or-later OR
LGPL-3.0-or-later`), statically into the Metal plug-in (`HakiosMetal`) and into `m2v_test`, beside AdrenoMetal's frontend (MIT), the plug-in's
own code and hakios-metal. Whoever distributes such a binary takes on that license's obligations for it, whichever of the two is chosen: under
the LGPL, among others, a recipient's ability to relink against a changed library; under the GPL, the whole binary's source under compatible
terms. What exactly that requires here, and whether the parts in the row above marked **to verify** allow it, is **to verify**, and has not
been settled. Until it is, no such binary is distributed: nothing of it is in `prebuilt/` (`prebuilt/userland` has no `HakiosMetal`), and you
fetch and build it yourself, on your machine. `libvulkan_freedreno.dylib` (Turnip) is a separate library, loaded at run time, under Mesa's terms.

## How to rebuild the inventory

- Tracked non-text/binary files: `git ls-files -z | xargs -0 file --mime-type | grep -v text/`
- License mentions in tracked files: `git grep -li "SPDX-License-Identifier\|GPL\|BSD\|MIT\|APSL\|copyright"`

If a component's origin is uncertain, it is marked **to verify** above and must be treated as not
yet cleared for redistribution until confirmed with its owner.
