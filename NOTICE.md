# Licences and third-party notices

## Our work

- `kernel/overlay/` and `kernel/patches/` modify and extend Apple's xnu, published under the Apple Public Source
  License 2.0, and are distributed under the same licence (`LICENSES/APSL-2.0.txt`).
- `base/` and `userland/` are HakiOS (hakios-xnu-kernel; the user space has since moved into it) with our changes; they keep
  HakiOS's licence (`base/LICENSE`, APSL 2.0). So do `loader/` and `kernel/deps/`, which were `base/xnuloader/` and
  `base/xnu-deps/`.
- `tools/`, `tests/`, `devices/`, `soc/` of this repository and the documentation written for it: MIT (`LICENSES/MIT.txt`).

## Files inside the kernel overlay that keep their own licence

- `bsd/miscfs/apfs/libdeflate/`, `apfs_deflate.c`: libdeflate, MIT (Eric Biggers; Google LLC)
- `bsd/miscfs/apfs/libzbitmap/`, `apfs_zbitmap.c`: libzbitmap, GPL-2.0+ OR MIT, taken under MIT (Corellium LLC)

## Linux

Our drivers (`hakios_*.c`, `HakiosGPU.cpp`, ...) name the Linux drivers whose register layouts and bring-up
order they follow (msm/adreno, qcom clocks and PHYs, cs35l41, spi-geni, ...). They are written for xnu, not copied.
The optional Wi-Fi build script (`base/tools/mk-ath11k.sh`) fetches Linux's ath11k
(BSD-3-Clause-Clear, with an ISC header) and FreeBSD's LinuxKPI headers (BSD-2-Clause).
Those fetched sources are not in this repository. The current Renoir configuration does not link
an external Wi-Fi object.

## Excluded from this repository

Firmware and binaries of others are not repository content; `tools/check-publishable.sh` looks for them:
Apple's Darwin files and shaders, Qualcomm/Xiaomi firmware (ADSP, GPU, Wi-Fi, Bluetooth, touch), device trees,
prebuilt UEFI drivers, and the touch firmware header `hakios_focaltech_fw.h`
(make it from your own phone with `tools/mk-focaltech-fw.py`).
