# License compliance

How HakiOS meets the terms of the licenses that apply to it. This is a working checklist, not a
legal opinion, and it does not assert that the project is free of all risk. See `LEGAL.md` and
`THIRD_PARTY_LICENSES.md`.

## Apple Public Source License 2.0 (the kernel)

`xnu/hakios.patch` and everything under `xnu/files/` and `drivers/` are modifications of Apple's XNU, which Apple
publishes under the APSL 2.0. The full text is in `LICENSE.APSL`. The obligations we track:

- **Source availability (APSL 2.3).** Our modifications are published here as source. The exact
  Apple base they apply to is the tag `xnu-8796.121.2` at
  `github.com/apple-oss-distributions/xnu`, fetched by `xnu/apply.sh`. CI checks that the patch
  still applies to that tag.
- **Retention of notices (APSL 2.1).** Apple's original copyright and license notices are kept in
  the files they belong to. Our added files carry their own notices and do not remove Apple's.
- **Marking modifications (APSL 2.2).** Changes are described in the patch and in the commit
  history; the project's own files are named `hakios.<thing>.c` so that what is ours is
  distinguishable from Apple's original code.
- **Executable distribution (APSL 2.3(c)).** Where a built kernel is distributed (`prebuilt/`),
  the corresponding source is in the same repository, and this file points at it.
- **No re-licensing.** The XNU-derived code stays under the APSL; it is **not** relabeled MIT. The
  MIT License in `LICENSE` applies only to our own non-XNU code.

What is NOT covered and must not be assumed covered by the APSL: Darwin user space, Apple firmware,
trademarks, and any Apple binary. None of those are in this repository.

## MIT (our own code)

The loader, user-space sources, tools, scripts, docs and the device layer are under the MIT
License (`LICENSE`). Contributions are accepted under the same terms (`CONTRIBUTING.md`).

## Third-party components

Each third-party component is listed in `THIRD_PARTY_LICENSES.md` with its origin, license, and
whether it is redistributed in the tree or fetched at build time. Components fetched at build time
(the Darwin SDK, Mesa and its Turnip, AdrenoMetal, metal2vulkan and its Rust crates, Procursus/Sileo, lzfse, ath11k, LinuxKPI headers) are
never committed; they are obtained from their own upstreams under their own licenses.

## Items still to verify

These are flagged for review and are **not** asserted to be settled:

- **UEFI driver binaries** under `devices/redwood/uefi/files/Binaries/` are the phone
  vendor's (Qualcomm/Xiaomi), extracted from the device's own `xbl.img`. They are needed only to
  rebuild the UEFI image and can be removed and re-extracted by each user from their own phone
  with UEFIReader. Whether redistributing them is permitted has not been confirmed with the
  vendor; a user who is unsure should delete them and extract their own. Tracked in
  `THIRD_PARTY_LICENSES.md`.
- **The FocalTech touch-controller firmware header** (`drivers/input/hakios.focaltech_fw.h`)
  is generated from a firmware blob on the device. Its redistribution terms are the touch
  vendor's and have not been confirmed; it is a candidate for moving to the pull-from-device
  model the rest of the firmware already uses.
- **Mu-Silicium / Project Mu** derived files under `devices/redwood/uefi/` carry upstream
  BSD-2-Clause-Patent and GPL-2.0-or-MIT headers; the derived parts keep those licenses.

- **Metal on Vulkan (`M2V=1`).** metal2vulkan is `GPL-2.0-or-later OR LGPL-3.0-or-later`, and `userland/build.sh` links it statically into the
  Metal plug-in and `m2v_test`. A binary so built has obligations; what they are here, and whether they can be met together with the other
  parts of that binary, has not been settled. **No such binary is distributed**: nothing of it is in `prebuilt/`, and each user fetches and
  builds it. Before anyone distributes one, this must be answered ([third-party licenses](THIRD_PARTY_LICENSES.md#a-binary-built-with-m2v1-to-verify)).
- **`tools/m2v/metal2vulkan-hakios.patch`** is in the tree and changes GPL/LGPL code, so it is under those terms and not the MIT License
  that `LICENSE` gives the rest of `tools/`. It carries no notice of its own.
- **`userland/src/HakiosMetalCA.m`** began as vcvkk's plug-in (github.com/vcvkk/darwin-redwood). Its header names the origin; that
  repository states no license (read 2026-11-04), so the permission to carry and change this file is not established.
- **The Rust crates of `M2V=1`** not read from a local registry (`THIRD_PARTY_LICENSES.md`).

If any of these cannot be confirmed as redistributable, the fallback is the same as for all
device firmware: remove the file from the tree and have each user obtain it from their own
hardware.
