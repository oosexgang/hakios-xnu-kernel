# Licences and third-party notices

## Our work

- `kernel/overlay/`, `kernel/patches/` and the parts of the kernel's tree kept at the top (`arch/arm64/kernel/`, `arch/arm64/include/`, `arch/arm64/configs/xnu/`, `crypto/`, `fs/`, `security/`; `kernel/lib.sh` lists them) modify and extend Apple's xnu, published under the Apple Public Source
  License 2.0, and are distributed under the same licence (`LICENSES/other/APSL-2.0`).
- `userland/` is HakiOS (hakios-xnu-kernel; the user space has since moved into it) with our changes; it keeps HakiOS's
  licence (`LICENSES/other/APSL-2.0`). So do `arch/arm64/boot/` and `kernel/deps/`, which were HakiOS's `xnuloader/` and
  `xnu-deps/`.
- `components/`: each file keeps the licence of where it came from. What came from `userland/` and `darwin/`
  (the Metal device and its compiler's tests, the camera's capture device, the audio plug-in, the libraries put
  into Darwin's programs) keeps HakiOS's, as above; `components/metal/plugin/` (hakios-metal) and the files with an
  `SPDX-License-Identifier` say their own.
- `tools/`, `workbench/`, `arch/arm64/boot/dts/`, `bench`, `build.config.*` of this repository and the documentation written
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

The Bluetooth controller's firmware is sent as Linux's Qualcomm Bluetooth driver (btqca.c) sends it (the files' TLV and ELF
forms, the segments of the vendor command 0xfc00, which answers a download mode leaves out): `board/bt_qca.h` is written
from what that driver does, not from its code.

## Other ports of Darwin to phones

What other ports found about Darwin's own side -- what its daemons ask a driver and what they do with the answer --
is credited where it is used, in the comment beside the code and in the commit: the OnePlus Nord 2 (MediaTek) port of
iOS, `frxncyyy/ios-denniz`, branch `nord2-port-20261116` (its kernel pieces under APSL-2.0, its own credits in its
LICENSES-AND-CREDITS.md), and through it HakiOS's MediaTek port: what bluetoothd opens and asks of a controller
(`board/bt_hci.h`, `board/bt_host.c`, `iokit/Kernel/BoardBluetooth.cpp`), Wi-Fi's RSSI bounds, the sandbox's
extensions, the application log's name alone. From its own findings, not its code: what holds locationd here (its FairPlay
signer waiting on fairplayd, `components/location/inject_locationd.c`) and the motor's tap as one short tick at a low
fixed gain (`board/haptic.c`, `haptic_tap_gain=`). Nothing of its GPU stack, which it says derives from sources whose licence it has not checked,
is taken; nor its device data.
Nine more are taken as ideas only, written anew here with nothing of its code or text, because what it wrote for them
is not, or not wholly, under the APSL by its own notes (a file of ios-sweet's, a file a patch adds, its own tools): a
floor under the clock (its piece 0530; `drivers/iokit/build_date.h`, `init/init.c`), `/private/var` named "apfs" to
fileproviderd (0734; `fs/ramfs/ramfs.c`), a container path for application extensions (0730;
`kernel/overlay/bsd/kern/kern_minimac.c`), the time of day kept by the PMIC's clock and a saved difference (0540;
`drivers/board/rtc.c`, `init/init.c`), dasd's 1970 modulation time taken out of a kept data volume (its dasdmod1;
`init/init.c`), fairplayd's restarts spaced out, where it takes the job out (its respawn1; `userland/userland/mkjobs.py`),
Messages' last build written for it (its 0277y; `darwin/userland/root26-darwin.sh`, `userland/userland/build.sh`),
Messages' BlastDoor told it is sandboxed (its 0277v; `kernel/overlay/bsd/kern/sandbox_blastdoor.h`, `kern_minimac.c`),
and the IOSurfaces marked volatile counted, which its 0417 and 0418 purge (`drivers/iokit/MiniIOSurface.cpp`: only
counted here, nothing purged).

## Excluded from this repository

Firmware and binaries of others are not repository content; `tools/check-publishable.sh` looks for them:
Apple's Darwin files and shaders, Qualcomm/Xiaomi firmware (ADSP, GPU, Wi-Fi, Bluetooth, touch), device trees,
prebuilt UEFI drivers, and the touch firmware header `focaltech_fw.h`
(make it from your own phone with `tools/mk-focaltech-fw.py`).
