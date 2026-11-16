# Licences and third-party notices

## Our work

- `kernel/overlay/`, `kernel/patches/` and the parts of the kernel's tree kept at the top (`arch/arm64/kernel/`, `arch/arm64/include/`, `arch/arm64/configs/xnu/`, `crypto/`, `fs/`, `security/`; `kernel/lib.sh` lists them) modify and extend Apple's xnu, published under the Apple Public Source
  License 2.0, and are distributed under the same licence (`LICENSES/other/APSL-2.0`).
- `base/` and `userland/` are HakiOS (hakios-xnu-kernel; the user space has since moved into it) with our changes; they keep
  HakiOS's licence (`base/LICENSE`, APSL 2.0). So do `arch/arm64/boot/` and `kernel/deps/`, which were `base/xnuloader/` and
  `base/xnu-deps/`.
- `components/`: each file keeps the licence of where it came from. What came from `userland/` and `darwin/`
  (the Metal device and its compiler's tests, the camera's capture device, the audio plug-in, the libraries put
  into Darwin's programs) keeps HakiOS's, as above; `components/metal/plugin/` (hakios-metal) and the files with an
  `SPDX-License-Identifier` say their own.
- `tools/`, `workbench/`, `devices/`, `bench`, `build.config.*` of this repository and the documentation written
  for it: MIT (`LICENSES/preferred/MIT`).

## Files inside the kernel overlay that keep their own licence

- `fs/apfs/libdeflate/` (the tree's `bsd/miscfs/apfs/libdeflate/`), `apfs_deflate.c`: libdeflate, MIT (Eric Biggers; Google LLC)
- `fs/apfs/libzbitmap/`, `apfs_zbitmap.c`: libzbitmap, GPL-2.0+ OR MIT, taken under MIT (Corellium LLC)

## Linux

Our drivers (`bsd/dev/board/*.c`, `BoardGPU.cpp`, ...) name the Linux drivers whose register layouts and bring-up
order they follow (msm/adreno, qcom clocks and PHYs, cs35l41, spi-geni, ...). They are written for xnu, not copied.
The optional Wi-Fi build script (`kernel/lx/build.sh`) fetches Linux's ath11k
(BSD-3-Clause-Clear, with an ISC header) and FreeBSD's LinuxKPI headers (BSD-2-Clause).
Those fetched sources are not in this repository. The current Renoir configuration does not link
an external Wi-Fi object.

## Excluded from this repository

Firmware and binaries of others are not repository content; `tools/check-publishable.sh` looks for them:
Apple's Darwin files and shaders, Qualcomm/Xiaomi firmware (ADSP, GPU, Wi-Fi, Bluetooth, touch), device trees,
prebuilt UEFI drivers, and the touch firmware header `focaltech_fw.h`
(make it from your own phone with `tools/mk-focaltech-fw.py`).
