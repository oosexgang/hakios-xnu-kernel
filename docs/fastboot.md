# Running HakiOS through fastboot on zeus (Xiaomi 12 Pro, SM8450)

Everything below starts HakiOS from RAM with `fastboot boot`. Nothing is flashed unless a step says so, and a reboot hands the phone back to Android. You need an unlocked bootloader, a computer with `fastboot` and `adb`, and a USB cable.

The repo's `xl` wrapper (`hakios-xnu-kernel/xl`) drives fastboot for you. On macOS it adds `tools/macos-bin` to `PATH`.

zeus is not the reference device (redwood is), so every `make`/`xl` command needs `DEVICE=zeus`.

## Prerequisites

```sh
cd hakios-xnu-kernel        # your clone

# check the phone is reachable and what state it is in
./xl state            # prints: absent | android | fastboot | loader

# raw checks
fastboot devices      # serial  fastboot   (phone in bootloader)
adb devices           # serial  device     (phone in Android)
```

If `./xl state` prints `android`, the phone is in Android. If it prints `fastboot`, it's already in the bootloader. If `absent`, plug it in with USB debugging on.

## Step 0: pull your phone's firmware (never committed, do once)

HakiOS carries nothing of the vendor's. Pull it from your own zeus over adb:

```sh
make firmware DEVICE=zeus      # = devices/zeus/pull-firmware.sh
```

This writes GPU, audio DSP, speaker, modem, Wi-Fi and Bluetooth firmware into `fw-*/` (gitignored). The `zeus-firmware/` directory at the repo root already holds an extracted `uefi_b.img` and `xbl_b.img` / `xbl_config_b.img` from a development phone.

Optionally refresh the SoC boot-args from the phone's device tree:

```sh
devices/zeus/pull-soc.sh   # rewrites devices/zeus/soc.args from /sys/firmware/fdt
```

## Step 1: the kernel and loader are already built

Everything needed already exists — no build required before the first run:

| File | Size | Purpose |
|------|------|---------|
| `out/xnu-zeus-usb.img` | 1.8 MB | zeus boot image (UEFI + shim + payload, payload at 0xDC000000) |
| `out/loader-zeus-id.efi` | 71 KB | zeus loader with iPhone identity |
| `out/loader-zeus.efi` | 66 KB | zeus loader without identity |
| `out/usb-payload.bin` | 66 KB | USB payload for `xl enter` |
| `hakios-kernel` | 16 MB | built XNU kernel |
| `prebuilt/hakios-kernel` | 16.8 MB | prebuilt kernel fallback |
| `prebuilt/loader.efi` | 66 KB | prebuilt loader fallback |
| `userland/system.apfs` | 2.5 GB | Darwin system image (for `xl ramdisk`) |
| `userland/root.apfs` | 4 MB | Darwin root image |
| `devices/zeus/prebuilt/SILICIUM_UEFI.fd` | 3 MB | zeus UEFI FD |
| `devices/zeus/prebuilt/device-tree.dtb` | 820 KB | zeus device tree |

If you ever need to rebuild:

```sh
make DEVICE=zeus zeus_defconfig    # common kernel + sm8450 family + zeus fragment
make DEVICE=zeus kernel            # builds the XNU kernel
make DEVICE=zeus loader            # builds xnuloader with identity from your IPSW
```

## Step 2: enter the HakiOS loader (fastboot boot — nothing written)

This is the everyday way. `xl enter` builds a boot image (Project Silicium's UEFI + the shim + a payload) and starts it with `fastboot boot`. The loader lives in RAM only; a reboot returns Android.

For zeus the payload goes at `0xDC000000` (not `0xF0000000`: `0xE8800000-0xF4600000` is a hole on this phone), the UEFI FD at `0xA7100000`, header v4.

```sh
DEVICE=zeus ./xl enter
```

What `xl enter` does internally:
1. If the phone is in Android: `adb reboot bootloader`
2. Builds the USB payload: `xnuloader/mkpayload.py -o usb-payload.bin loader.efi=xnuloader/loader.efi --usb`
3. Builds the boot image: `mkxnuimg.py -d zeus -p usb-payload.bin -o xnu-usb.img`
4. Boots it: `fastboot boot xnu-usb.img`
5. Waits for the loader to answer on USB

When done, `./xl state` should print `loader`.

You can also boot the prebuilt image directly with raw fastboot:

```sh
fastboot boot out/xnu-zeus-usb.img
```

## Step 3: run a minimal kernel test (hello RAM disk, ~8 seconds)

```sh
DEVICE=zeus XL_RAMDISK=none XL_SECS=20 ./xl run
```

What `xl run` does internally:
1. Builds a kernel payload: `xnuloader/mkpayload.py -o run-payload.bin kernel=hakios-kernel ramdisk= --cmdline "loader_return_after=20 epoch=... sb_delay=8 -v"`
2. Uploads it: `fastboot stage run-payload.bin`
3. Tells the loader to run it: `fastboot oem run`
4. Polls `fastboot getvar result` until the run finishes
5. Fetches the log: `fastboot get_staged <logfile>`

The kernel boots, runs `hello` (prints "Hello from user space on Darwin!"), and returns after 20 seconds.

## Step 4: upload the Darwin system image (do once per session)

```sh
DEVICE=zeus ./xl ramdisk userland/system.apfs
```

This uploads the 2.5 GB system image to the phone's RAM. It stays there across subsequent `./xl run` calls, so you only do this once per session. The loader reports the upload size and speed.

## Step 5: full Darwin run (SpringBoard / home screen)

```sh
DEVICE=zeus XL_SB_DELAY=8 XL_SECS=150 \
  XL_CMDLINE="-v cpus=8 bootcpu=7 ufs=2 small_cores_after=0" \
  ./xl run
```

Boot-args that matter on zeus:

| Arg | Meaning |
|-----|---------|
| `-v` | Verbose boot (text on screen) |
| `cpus=8` | Use all 8 cores |
| `bootcpu=7` | Kernel runs on the X2 core (core 7) |
| `ufs=2` | UFS storage |
| `small_cores_after=0` | Start A510 small cores immediately (default waits 28 s) |
| `touchscreen=1` | ST FingerTip on spi4 |
| `gpu=1` | Adreno 730 on, kept for HakiosGPU |
| `adsp=1 amps=2 sound=3` | Audio DSP test tone through the four CS35L41 amps |
| `adsp=1 battmgr=2` | Battery level, charging, voltage, temperature |
| `display_hz=120` | Switch the panel to 120 Hz (ABL leaves it at 1 frame per second) |
| `display_every=1` | Auto-refresh: send a frame on every tear pulse (default 1; 0 leaves ABL's timing) |

## 120 Hz display (zeus panel)

The zeus panel (`l2-38-0c-0a-dsc`, Samsung LTPO) is 1440x3200. ABL brings it up at 60 Hz with auto-refresh set to one frame per second — enough for a still logo, choppy for anything moving. The A730 GPU driver switches it to 120 Hz:

```sh
DEVICE=zeus XL_SB_DELAY=8 XL_SECS=150 \
  XL_CMDLINE="-v cpus=8 bootcpu=7 ufs=2 small_cores_after=0 display_hz=120 display_every=1" \
  ./xl run
```

What `display_hz=120` does internally:
1. Turns on the two bus clocks needed to read the display controller (`disp_ahb` at 0x37004, `mdss_ahb` at 0xaf080a4)
2. Sends the panel's 120 Hz timing-switch command (`timing@wqhd_120hz_index_01`, 29 DSI packets via the DSI controller's command DMA)
3. Halves the tear check's line clock (`SYNC_CONFIG_VSYNC` count 100 → 50)
4. Result: DSI sends **119 frames per second** to the panel (counted on DSI 0's "command-mode frame done")

Window server settings for 120 Hz (already in `configs/zeus.config`):
- `CONFIG_FBHEIGHT=2500` — window server draws 1125x2500 (iPhone X width at the panel's 0.45 shape, scaled 1.28× both ways)
- `CONFIG_FB_HZ=120` — frame buffer target is 120 Hz

In `metal.args`:
```
bootcpu=7 cpus=8 fb_hz=120 fbheight=2500 fb_threads=3
```

Not done yet: LTPO (the panel's lower refresh rates when nothing moves), and handing the loader the panel as ABL left it (it stays at 120 Hz after the kernel returns, which does no harm).

This boots Darwin, starts launchd, SpringBoard, and reaches the home screen. After `XL_SECS` seconds the kernel returns to the loader.

## Step 6: return to Android

```sh
./xl reboot
```

This cold-resets the phone back to Android. You can also hold power + volume-down to enter fastboot manually, then `fastboot reboot`.

---

## Complete step-by-step example (first session)

```sh
# 1. cd into the kernel repo
cd hakios-xnu-kernel        # your clone

# 2. Check phone state
./xl state

# 3. Enter the loader (fastboot boot, nothing written)
DEVICE=zeus ./xl enter
./xl state                    # should print: loader

# 4. Run a minimal kernel test
DEVICE=zeus XL_RAMDISK=none XL_SECS=20 ./xl run

# 5. Upload the system image (once per session)
DEVICE=zeus ./xl ramdisk userland/system.apfs

# 6. Full Darwin run to home screen
DEVICE=zeus XL_SB_DELAY=8 XL_SECS=150 \
  XL_CMDLINE="-v cpus=8 bootcpu=7 ufs=2 small_cores_after=0" \
  ./xl run

# 7. Return to Android
./xl reboot
```

---

## Alternative: one boot image with loader + payload inside (`fastboot boot hakios-zeus.img`)

Build a single Android boot image holding Silicium's UEFI, the shim and a payload, then boot it with fastboot. Nothing is flashed.

```sh
xnuloader/mkpayload.py -o payload.bin kernel=hakios-kernel ramdisk=userland/hello --cmdline "-v"
DEVICE=zeus ./mkxnuimg.py -p payload.bin -o hakios-zeus.img
fastboot boot hakios-zeus.img
```

`mkxnuimg.py -d zeus` packs FD at `0xA7100000`, payload at `0xDC000000`, boot image header v4 (from `devices/zeus/uefi/files/Resources/Configs/zeus.toml`).

## Alternative: U-Boot (open-source firmware path, not yet ported for zeus)

HakiOS's U-Boot needs no vendor UEFI driver, but zeus lacks the SM8450 clock/pin drivers yet:

```sh
make u-boot DEVICE=zeus
make u-boot-img DEVICE=zeus
fastboot boot hakios-u-boot.img
XL_FIRMWARE=u-boot ./xl enter
./xl ramdisk userland/system.apfs
./xl run
```

---

## All xl commands

```sh
./xl state            # check phone state (absent | android | fastboot | loader)
./xl enter            # fastboot boot the loader (RAM only)
./xl run              # build + upload + run kernel + fetch log
./xl ramdisk FILE     # upload system image to RAM (stays across runs)
./xl log              # fetch loader log
./xl screenshot F.png # save framebuffer as PNG
./xl reboot           # cold-reset back to Android
./xl reload           # reboot phone + re-enter loader
```

After the phone has run a kernel once, a second kernel in the same loader session dies at once. Restart into the bootloader between runs (`./xl reload` does `fastboot reboot` then `enter` again).

---

## Standalone / persistent install (keeps running on the phone)

The RAM-based runs above always return to the loader after `XL_SECS` seconds — that's by design: nothing is written to the phone, and a reboot gives Android back.

To make HakiOS **standalone** — it starts on its own at every boot, no cable, no `fastboot boot` each time — you install it into the phone's storage. zeus is an A/B device: Android stays in one slot, HakiOS goes in the other, plus a `darwin` partition carved from userdata holds the Darwin system image.

This **writes the phone**. Back up first.

```sh
# 1. Back up the partitions (as root from Android)
python3 tools/backup-partitions.py --yes

# 2. Carve `darwin` (20 GiB) from the end of userdata, then erase Android's data
python3 tools/userdata-carve.py --wipe backups 20
fastboot -w

# 3. Write the Darwin system volume to `darwin`, in sparse pieces
python3 tools/sparse-split.py darwin.img pieces
fastboot flash darwin pieces/piece-00.simg   # ... every other piece

# 4. Make the data volume (HFS Plus, ~12 GB) in the rest of `darwin`
python3 tools/mkdatavol.py data.simg
fastboot flash darwin data.simg

# 5. Build and install the boot image into the inactive slot
tools/mkubootimg.py -d zeus --install -o hakios-install.img
fastboot flash boot_a hakios-install.img
fastboot set_active a
fastboot reboot
```

At every start: U-Boot shows a menu for 10 seconds, then boots Android by default. Choose HakiOS (Darwin) and it starts from `darwin` on the phone's own storage — standalone, no `XL_SECS` timeout, no loader returning to Android.

To return to Android: hold volume down + power to reach fastboot, then `fastboot set_active b && fastboot reboot`.
**Status on zeus: not yet verified.** Step 5 (`fastboot flash boot_a` + `set_active`) is **not done, not tried** on zeus, and booting the flash recipe's root from `darwin` is not confirmed. The RAM-based methods above (Steps 2–6) are the ones that have been run on zeus.

---

## Fastboot commands used across the project

| Command | Purpose |
|---|---|
| `fastboot boot IMG` | start a boot image from RAM, nothing flashed |
| `fastboot flash PART IMG` | write an image to a partition |
| `fastboot stage FILE` | upload a file for the running U-Boot/loader to take |
| `fastboot oem run:CMD` | ask U-Boot/the loader to run a command |
| `fastboot continue` | U-Boot continues booting (`hakios boot`) |
| `fastboot set_active SLOT` | choose which A/B slot boots next |
| `fastboot -w` | wipe userdata and metadata |
| `fastboot getvar product/version/result` | read loader variables (`product: xnuloader`, `version: 0.5`) |
| `fastboot devices` | list phones in the bootloader |
| `fastboot reboot` | reboot |

---

## Safety rules the project keeps

- Only the **inactive** slot is ever written (`fastboot flash boot_a` when Android runs on b).
- The bootloader chain, modem/radio calibration, and the phone's identity data are never written.
- Every write is read back and compared; a mismatch stops everything.
- `tools/provenance.allow` lists what may flash; CI fails if another writer appears.
- Dry run by default: each step prints its checks, and a person confirms each one.

## What is not yet verified on zeus

- Writing the boot image to a slot and switching to it (`fastboot flash boot_a` + `set_active`) is **not done, not tried** on zeus.
- Booting the flash recipe's root from `darwin` is not verified; the arguments it needs are not confirmed.
- Verified boot (`vbmeta`) for a modified boot image is unknown.
- U-Boot for zeus is not ported yet (no `clock-sm8450.c` / `pinctrl-sm8450.c`).

See [install.md](install.md) (the design), [install-safe.md](install-safe.md) (the staged order), [uefi-boot.md](uefi-boot.md) (UEFI boot paths), and [devices/zeus/README.md](devices/zeus/README.md) (the zeus port status) for the rest.
