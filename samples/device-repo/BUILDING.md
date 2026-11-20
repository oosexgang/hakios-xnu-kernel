# Building this phone

This repository describes one phone for the HakiOS kernel: `board.dts` (what the phone has and where), `soc.dtsi`
(where its chip's blocks are), and `README.md` (what works on it, and how that was seen). The kernel, the loader and
every driver are in the kernel's repository; nothing of them is copied here. Every file and directory of a
repository like this one, who writes it and whether it is committed: the kernel's Documentation/porting.md, "Or keep
it in a repository of your own".

In a checkout of the kernel's repository:

```sh
make phone FROM=URL-OF-THIS-REPOSITORY DEVICE=NAME   # this package brought in, the kernel and the loader built, one image
make image DEVICE=NAME DEFCONFIG=phone_look       # again, for another kind of run
make publish DEVICE=NAME REPO=PATH-OF-A-CHECKOUT-OF-THIS-REPOSITORY   # the image into images/ here
```

`FROM=` also takes the directory of a checkout of this repository, for work that is not pushed yet. The image is
`build/images/NAME/` of the kernel's checkout; it has a `boot.img` for `fastboot boot` when the phone's UEFI build is in
`firmware/NAME/uefi/` there and `board.dts` has a `uefi { }` section (Documentation/porting.md, step 2).

To run it from the phone's RAM (nothing is written to its storage) with the kernel's `./bench`:

```sh
BENCH_BOARD=NAME make ready                     # what the runs need, all here? before the phone is connected
BENCH_BOARD=NAME make day                       # the runs in order; their logs and a summary in logs/day-TIME/
```

`tools/device.py add --from URL#v1` takes this repository at a tag or a commit, `tools/device.py update NAME` takes
it again as it is now; `tools/boards.py check` says what is not right in it yet and `tools/boards.py status NAME` what
the phone can turn on next. The kernel's Documentation/porting.md goes through a port step by step, and
Documentation/images.md through images.

## What is not here, and never is

- Apple's files: no IPSW, no system image, no identity made from one. An image published in `images/` carries none;
  whoever boots it gives it their own (`tools/image.py personalize`).
- The phone's device tree as Android has it, and its firmware files: they are the vendor's.
- The UEFI build (`firmware/NAME/uefi/` in the kernel's checkout) unless you may publish its vendor's drivers: then
  `tools/image.py publish --with-uefi` puts `boot.img` beside the payload.

`.gitignore` keeps the usual ones out, and `tools/device.py add` leaves the same files where they are when it brings
the package in from a directory; the check in `.github/workflows/` holds every push to the kernel's checks of a
package.
