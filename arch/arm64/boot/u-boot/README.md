# U-Boot phone profiles

The shared layer builds upstream U-Boot v2026.07 with a board profile. The current hardware profile is Redwood (POCO X5 Pro, Qualcomm SM7325); it provides the visible boot menu, interactive shell, USB ACM console, and UEFI `bootefi` service. The XNU loader remains a separate EFI application. See [platform profiles](PLATFORMS.md) for support status and porting requirements.

## Build on macOS

Install GNU make, GNU Bash, Python 3.12, OpenSSL 3, and the AArch64 bare-metal GCC toolchain. The build script uses Homebrew paths and keeps source and output under the ignored `build/` directory.

```sh
arch/arm64/boot/u-boot/build.sh
python3.12 arch/arm64/boot/u-boot/pack.py \
  --loader arch/arm64/boot/loader.efi \
  --dtb firmware/redwood/uefi/device-tree.dtb \
  -o build/redwood-u-boot.img
```

`pack.py` also accepts `--payload PATH` for an XNUP container. The default image contains the U-Boot menu and loader but no kernel payload; the loader's USB recovery path then stays available.

## Console and RAM boot

Start the image from the phone's stock fastboot with `fastboot boot build/redwood-u-boot.img`. Select **USB serial console** in the U-Boot menu, then open the new `/dev/cu.usbmodem*` device on the Mac at 115200 baud, for example with `screen`. Type `help`, `printenv`, `bdinfo`, or `hakios info` at the U-Boot prompt. `exit` the terminal with `Ctrl-A`, then `K`.

Select **Start XNU loader** to enter `loader.efi`; the loader handles XNUP and kernel boot. The menu's U-Boot fastboot option is a RAM transfer path. The default menu does not mark slots successful, change the active slot, or flash partitions.

The USB ACM console is a USB serial interface, not an on-screen keyboard. The phone screen shows U-Boot's menu and video console; commands are typed from the Mac terminal.

## Latest device run

The stock bootloader accepted the diagnostic image through `fastboot boot`, but the phone did not re-enumerate as loader fastboot afterward. The diagnostic XNUP uses `cpus=4`, `-v`, `console_screen=1`, and `loader_return_after=120` so the next run can show progress and return to the loader for its log. It remains in the ignored `build/` directory and was not added to the repository. No phone partition was flashed.
