# firmware/

What belongs to one phone and is never published: its own files, kept here, by the phone's package name, for the tools
that look for them. Git ignores everything in this directory but this file (`.gitignore`), and
`tools/check-publishable.sh` and `tools/image.py publish` hold that line too.

```
firmware/NAME/
  uefi/SILICIUM_UEFI.fd     the phone's UEFI, built for it (Mu-Silicium: Build/<board>Pkg/RELEASE_CLANGPDB/FV/)
  uefi/BootShim.bin         the shim that starts it, built for the same memory map (its header says where the UEFI goes)
  uefi/device-tree.dtb      the phone's device tree, as its UEFI port has it
  wifi-mac                  the radio's own address, one line (./bench gives it to a run with wifi=)
  identity.bin              which iPhone Darwin is told it is, made from your IPSW (mkidentity.py --blob); ./bench sends
                            it with each run (BENCH_IDENTITY names another)
```

| Who reads it | What |
| --- | --- |
| `arch/arm64/boot/mkboot.py`, `./bench enter` | `uefi/`: the boot image `fastboot boot` takes, the loader in it (Documentation/porting.md, steps 2 and 3) |
| `tools/image.py build` | `uefi/`: an image's `boot.img` (Documentation/images.md); without it, the payload alone |
| `./bench run` | `wifi-mac`, `identity.bin` |

The firmware of the phone's parts -- the GPU's, the Wi-Fi DSP's, the audio DSP's, the Bluetooth controller's -- goes
into the small image instead, from `fw-*` directories beside the workspace (`userland/README.md`): the kernel reads it
from the RAM disks.
