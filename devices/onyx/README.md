# Device: Qualcomm Technologies, Inc. Onyx based on SM8735 (onyx)

Made by `tools/new-device.py` on 2026-11-01, from the file recovery.fdt. Edit it as the port goes.

| What | Value |
|---|---|
| Board (the tree's `model`) | Qualcomm Technologies, Inc. Onyx based on SM8735 |
| Compatible | `qcom,tuna-mtp qcom,tuna qcom,tunap-mtp qcom,tunap qcom,mtp` |

## The SoC addresses

No reference tree to compare with (devices/redwood/prebuilt/device-tree.dtb is missing).

| Argument | Value | Against the reference |
|---|---|---|
| `soc_ipa_base` | `0x3e40000` |  |
| `soc_gsi_base` | `0x3e04000` |  |
| `soc_ipcc` | `0x406000` |  |
| `soc_aoss` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_rsc` | `0x17a20000` |  |
| `soc_apps_smmu` | `0x15000000` |  |
| `soc_cmd_db` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_mpss_mem` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `modem_ipa_fw_at` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_smem` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_imem` | `0x14600000` |  |
| `soc_ipa_stream` | `0x4a0` |  |
| `soc_ipa_stream_uc` | `0x4a2` |  |
| `soc_pas_mpss` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_pas_ipa` | `0xf` |  |
| `soc_ipa_fw` | `ipa_fws` |  |

`soc.args` is what `tools/soc-args.py` read out of the tree; `pull-soc.sh` makes it again from the phone on
adb. The run scripts add it to the kernel command line. Where each argument comes from in the tree:
`devices/redwood/README.md`.

## What is this phone's own

The kernel, loader and user space know nothing about a phone: addresses, interrupts, rails and firmware
come from its tree and its Android. What is written for one phone is the list below. The head of each file
says where its numbers came from; that is the map of what to rewrite.

- [ ] **UEFI**: the firmware that starts the loader: devices/redwood/uefi/ is a port of Mu-Silicium; the vendor's drivers are extracted from your own phone (uefi/README.md)
- [ ] **Display, buttons, touch**: drivers/input/hakios.input.c, hakios.buttons.c, hakios.goodix.c (the reference phone's controller; another phone has another)
- [ ] **Storage**: hakios.ufs.c and hakios.ufshc.c (UFS); a phone with eMMC needs another driver
- [ ] **CPU clocks**: hakios.cpufreq.c
- [ ] **GPU**: hakios.adreno.c (Adreno 642L here: another generation needs its own firmware and registers)
- [ ] **Wi-Fi**: hakios.wifi.c, hakios.lx.c and wifi/lx/ (the WCN6750 radio of the reference phone)
- [ ] **Audio and battery**: hakios.adsp.c, hakios.audio.c, hakios.sound.c, hakios.speaker.c, hakios.mic.c, hakios.battery.c
- [ ] **Cellular**: hakios.mpss.c, hakios.qmi.c, hakios.nas.c, hakios.ipa.c, hakios.cell.c; soc.args carries the addresses, the IPA layout is for IPA 4.11 (docs/cellular.md)
- [ ] **Haptics and cameras**: hakios.haptic.c, hakios.camera.c

## Configuration

The kernel's settings are layered the way an Android kernel's are: the common kernel, the SoC's, this phone's
(`docs/configs.md`). Two short files make this phone a configuration:

    configs/onyx.config          CONFIG_DEVICE="onyx"   and whatever is true of this phone only
    configs/onyx_defconfig       #include common_defconfig
                                   #include <the SoC's fragment: sc7280.config for the SC7280 family>
                                   #include onyx.config

Then `make onyx_defconfig`. Turn on only what this phone has been shown to do; `tools/hakios-config.py check` says what the
choice needs.

The drivers are part of the configuration too (`docs/drivers-config.md`): every one is built unless a configuration leaves it out,
and the addresses, pins and rails in each are the reference phone's. Leave out in `configs/onyx.config` what has not been
ported to this phone yet (`# CONFIG_HAKIOS_GOODIX is not set`, ...; `configs/qemu_defconfig` leaves out all of them), and put
each one back when it has been made to work here. `tools/hakios-config.py show` lists what is built in.

## Firmware

Pulled from your own phone, never committed: `devices/redwood/pull-firmware.sh`, or
`pull-firmware-root.sh` where the firmware is readable by root only. Check with `make doctor`.

## Before you open a pull request

`tools/provenance-check.py` must pass: no vendor firmware, no UEFI drivers, no Apple files in the tree.

## The repository

A HakiOS device repository: what is true of this device and of no other. The kernel, the loader, the drivers and the tools are
the common repository's; the sm8735 family's facts are in `soc/`, carried with the device. How the three are put together:
[docs/porting/repositories.md](https://github.com/oosexgang/hakios-xnu-kernel/blob/main/docs/porting/repositories.md).

| File | What |
|---|---|
| `device.conf` | what the device is, and how far each part of it works: its page in the documentation is made from it |
| `configs/onyx_defconfig` | the common kernel, the SoC's fragment, this device's: `make onyx_defconfig` |
| `configs/onyx.config` | what is this device's own: the drivers built in, the arguments it needs |
| `soc/` | its SoC family's layer (sm8735): `soc.conf`, `configs/sm8735.config`, the SoC's drivers; carried here so that this one repository is all the device needs |
| `drivers/` | drivers of its own, if it needs any (`drivers/<subsystem>/conf`, as in the common repository) |

Every phone driver of the common repository is left out to begin with: their addresses and pins are the phone they were written
for. Put one back in `configs/onyx.config` when it has been made to work here, and say so in `device.conf`.

Check with `tools/hakios-device.py check onyx`.
