# UEFI: what every port needs

The loader runs under a port of [Project Silicium / Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)
to the phone. What every such port needs is here; what a phone's own port is -- its platform package with
the memory map and driver list, its configuration, its device tree, the UEFI drivers taken from its firmware --
is in its device package on the `devices` branch (the first phone's has a whole port under `arch/arm64/boot/uefi/`, with its README).

- `BASE` -- the Mu-Silicium commit the ports are made on;
- `mu-silicium.patch` -- changes to existing files: `BootShim/BootShim.S` (copies our "XNUP" container, which
  follows the UEFI image, to 0xF0000000), `BootManagerLib` (starts the XNU loader from the container
  instead of the boot menu);
- `files/` -- the loader's hook, `Silicon/.../BootManagerLib/XnuLoader.[ch]`;
- `apply.sh PHONE_FILES` -- a clone of Mu-Silicium at `BASE` with these and a phone's platform put in;
- `export.sh [MAKER/PHONE PHONE_FILES]` -- the reverse.

How to port it to another phone: `Documentation/porting.md` of the main branch, "The loader".
