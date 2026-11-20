# One image for a phone

A phone's kernel, the loader that starts it and the boot arguments it runs with, in one file: built once, booted with
one command, kept in the phone's own repository for others to boot. `tools/image.py` makes it; nothing in it is
Apple's, and nothing is written to the phone.

## What an image is

```
build/images/NAME/
  NAME.payload     the loader (loader.efi), the kernel and the boot arguments, in the XNUP container the phone's
                   UEFI starts: the loader runs the kernel from it, once, and resets when it is done
  boot.img         the phone's UEFI with that payload, as `fastboot boot` takes it -- when the phone's UEFI build is
                   in firmware/NAME/uefi/ (Documentation/porting.md, step 2)
  manifest.json    what went in: the code's commit, the device package and where it came from, the defconfig and the
                   boot arguments, the SHA-256 of the kernel, of the loader and of each file
  README.md        how to boot it
```

The boot arguments are the board's own (`boot { }` of its `board.dts`) and then those of a defconfig of
`arch/arm64/configs` (`phone_first` unless told), each replacing one of the same name: what `./bench` would run.

## Making one

```sh
make image DEVICE=mido DEFCONFIG=phone_look       # the kernel and the loader already built
make phone FROM=https://git.example/you/mido.git  # or all of it: the package, the kernel, the loader, the image
tools/image.py build --board mido --defconfig phone_bt --out /some/where
make images DEVICE=mido                            # one for each run (phone_* defconfigs): build/images/mido/RUN/
```

The kernel and the loader know the boards whose packages were there when they were built (`build/boards/`, each by its
name). A loader built before the phone's package was brought in would take the phone for the emulator, and such a
kernel would find no board on it, every driver of the phone off; so `build` looks for the board's name in both and
stops if it is not there: `kernel/build.sh` (`make kernel`) or `arch/arm64/boot/build.sh` (`make loader`), and the
image again. `make phone` builds them in that order.

## Booting one

From the phone's own bootloader (`adb reboot bootloader`):

```sh
fastboot boot build/images/mido/boot.img
```

The phone runs from RAM: a reboot brings Android back. Without a `boot.img` (no UEFI build here),
`arch/arm64/boot/mkboot.py --board mido -p mido.payload -o boot.img` makes one wherever the UEFI build is, or
`./bench` sends the loader and the kernel over USB as it always has.

In QEMU, with the image's own loader and payload untouched (the emulator's board, `qemu-virt`):

```sh
tools/image.py build --board qemu-virt --defconfig debug
tools/qemu-kernel.sh --image build/images/qemu-virt/qemu-virt.payload
```

## Which iPhone Darwin is told it is

Darwin's user space decides what device it runs on from the device tree: the model, the screen, the haptics. Those
properties are taken from the device tree of an iPhone's IPSW, and they are Apple's data: they are not in this
repository, and they are not in an image that is passed around. Two ways to have them:

- **Your own phone, your own build**: the loader built with them (`arch/arm64/boot/build.sh` finds the IPSW's
  `DeviceTree.bin`, Documentation/building.md), or the payload carrying them
  (`tools/image.py build --identity DeviceTree.bin`). Such an image is for you.
- **An image someone published**: give it yours.

  ```sh
  tools/image.py personalize boot.img --identity DeviceTree.bin     # boot-mine.img; or a .payload
  ```

  The loader takes the identity from the payload when it carries one (`arch/arm64/boot/src/identity_given.c`) and
  says so (`identity: from the payload (t8030, 828x1792)`). `--identity` takes the IPSW itself
  (`--model n104ap` when it has several iPhones' device trees: n104ap is the iPhone12,1), its
  `Firmware/all_flash/DeviceTree.*.im4p`, or the `DeviceTree.bin` `darwin/tools/unpack.sh` leaves beside it.
  A kernel of your own tried in QEMU takes it the same way: `tools/qemu-kernel.sh --identity DeviceTree.bin KERNEL`.

## Into a phone's repository

A phone kept in a repository of its own (Documentation/porting.md, "Or keep it in a repository of your own") can keep
its images there too:

```sh
make publish DEVICE=mido REPO=../mido          # or: tools/image.py publish build/images/mido ../mido --version v1
cd ../mido && git add images && git commit -m "images: v1"
```

`images/VERSION/` gets the payload, the manifest and the README, and `images/README.md` lists every version. What is
refused, whatever is asked -- judged by the files' bytes, not by what manifest.json says of them (anybody may edit it):
every payload in a file is read, a boot image's gzip stream opened, each entry looked at whatever its name, and what
lies between and after them:

| Refused | Why |
| --- | --- |
| A loader built with an identity from an IPSW | Apple's data. Build the loader without one for an image that is published. |
| A payload that carries an identity, under any name | The same. |
| An identity blob (`IDNT`), an Apple image file (`IM4P`) or an iPhone's device tree anywhere in a file | The same, wherever it was put. |
| `boot.img` the manifest does not list, and links | Only the image's own files are published. |
| A `--version` that is not a plain name, or that names a directory that is no image | Publishing writes `images/VERSION/` and nothing else. |
| A payload with a system image in it | The system image is Darwin's own files (`./bench ramdisk`), never part of an image. |
| An image that is not what its manifest says | A file changed after it was built. |
| `boot.img`, unless `--with-uefi` | It carries the phone's UEFI and its vendor's drivers: only the repository's owner knows whether they may publish them. |

`tools/image.py show FILE` says what an image holds and whether it is the file its manifest names;
`tools/image.py diff A B` what differs between two (the code, the run, each boot argument, the kernel: which image a
report was made with);
`tools/image.py verify DIR` holds every image under a phone's `images/` to its manifest and to the rules above (the
check `samples/device-repo/` gives a phone's repository runs it on every push).

## Under the hood

| Part | What it does |
| --- | --- |
| `tools/image.py` | Builds, publishes, personalizes, shows and takes apart images. |
| `arch/arm64/boot/mkpayload.py` | The XNUP container (`tools/image.py` writes the same bytes; `workbench/image_test.py` holds the two to it). |
| `arch/arm64/boot/mkboot.py` | The Android boot image of a UEFI and a payload. |
| `arch/arm64/boot/mkidentity.py --blob` | An identity as the payload carries it. |
| `arch/arm64/boot/src/identity_given.c` | The loader's side of it. |
| `workbench/image_test.py`, `workbench/loader_test.c` | Their tests: on the host, on a phone and an iPhone of their own. |
