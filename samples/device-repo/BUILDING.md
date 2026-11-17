# Building this phone

This repository describes one phone for the HakiOS kernel: `board.dts` (what the phone has and where), `soc.dtsi`
(where its chip's blocks are), and `README.md` (what works on it, and how that was seen). The kernel, the loader and
every driver are in the kernel's repository; nothing of them is copied here.

In a checkout of the kernel's repository:

```sh
make phone FROM=URL-OF-THIS-REPOSITORY          # this package brought in, the kernel and the loader built, one image
make image DEVICE=NAME DEFCONFIG=phone_look       # again, for another kind of run
make publish DEVICE=NAME REPO=PATH-OF-A-CHECKOUT-OF-THIS-REPOSITORY   # the image into images/ here
```

`tools/device.py add --from URL#v1` takes this repository at a tag or a commit, `tools/device.py update NAME` takes
it again as it is now; `tools/boards.py status NAME` says what
the phone can turn on next. The kernel's Documentation/porting.md goes through a port step by step, and
Documentation/images.md through images.

## What is not here, and never is

- Apple's files: no IPSW, no system image, no identity made from one. An image published in `images/` carries none;
  whoever boots it gives it their own (`tools/image.py personalize`).
- The phone's device tree as Android has it, and its firmware files: they are the vendor's.
- The UEFI build (`firmware/NAME/uefi/` in the kernel's checkout) unless you may publish its vendor's drivers: then
  `tools/image.py publish --with-uefi` puts `boot.img` beside the payload.

`.gitignore` keeps the usual ones out; the check in `.github/workflows/` holds every push to the kernel's checks of a
package.
