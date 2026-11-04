# Legal

HakiOS is an independent open-source research project that ports the open-source XNU kernel to
ARM64 Qualcomm hardware. It is **not affiliated with, endorsed by, or connected to Apple Inc.**
"Darwin", "iPhone", "XNU", "Apple" and other marks belong to their owners and are used here only to
describe what the code works with.

This file explains, in plain terms, what is and is not distributed by this repository. It is a
description of the project's intent and current state, **not legal advice**, and it does **not**
claim that any use of the project is lawful in your jurisdiction. Running the full system image
requires software and firmware you must obtain and are licensed to use yourself; whether you may
do so is your responsibility. If in doubt, consult a lawyer.

## Three layers, and who decides what

HakiOS is an adaptation of the Darwin kernel to hardware that is not Apple's, in the way Android is an adaptation of
Linux to phones. It has three layers, and the project says plainly which one it stands behind.

1. **The open core.** The kernel port (a patch to Apple's open-source XNU, APSL 2.0), the boot loader, the drivers,
   the tools, the configurations. All of it is source in this repository, builds and runs with nothing of Apple's and
   nothing of any phone's (`make qemu-open`), and is what this project develops, documents and supports.
2. **What you put on top of it.** The kernel can start an operating-system user space that you supply. The project
   ships none, distributes none, and does not decrypt, circumvent or patch anything to obtain one: the tools only lay
   out, on your own machine, files that you have downloaded yourself from the vendor. **Whether you may use such
   software on this hardware is your decision and your responsibility.** Be aware of what you are deciding: Apple's
   software licence for Darwin ties it to Apple-branded hardware and restricts copying and modification, courts have
   treated that differently (see `docs/legal-review.md`), and the answer depends on your country and on what you do.
   The project does not tell you that it is permitted, and does not tell you that it is not; it tells you that it is
   not the project's to decide.
3. **Apple's services.** Activation, Apple ID, iCloud, iMessage, the App Store: not supported and not a goal. Nothing
   here makes a device pass as an iPhone to Apple's servers, and a change that tries to is refused.

## What the project writes to your phone

By default the boot loader runs from RAM (`fastboot boot`) and a reboot restores Android. It does write one thing: its
own log, a few KB, to the phone's `logdump` partition at the end of a run. Only if you turn on `ufs_write=1` does the
kernel write, and then only to a partition called `darwin` that you created yourself (`tools/userdata-carve.py` prints
the commands and changes nothing). Every other write is refused by the loader. `tools/provenance.allow` lists each
program that can write to flash, and CI fails if one appears that is not listed.

## What this repository contains

- **The kernel changes**: a patch to Apple's open-source XNU (`xnu/hakios.patch`) and added
  source files (`xnu/files/`, `drivers/`). These are a derivative work of XNU and are licensed under the
  Apple Public Source License 2.0 (`LICENSE.APSL`). See `COMPLIANCE.md` for how the APSL terms
  are met.
- **Our own original code**: the boot loader (`xnuloader/`), user-space programs
  (`userland/src/`), the tools (`tools/`), the build and run scripts, the documentation, and the
  device layer under `devices/`. These are licensed under the MIT License (`LICENSE`).
- **Third-party open-source components** that are redistributed or fetched at build time, each
  under its own license. They are listed with origin and license in `THIRD_PARTY_LICENSES.md`.
- **Pre-built artifacts** in `prebuilt/`: the kernel, user-space programs and libraries built
  from this repository's own source. They carry the same licenses as the source they are built
  from. Metal on Vulkan (`M2V=1`) and the window server's Metal plug-in are not among them: you fetch and
  build those yourself, and whether a binary of them may be distributed is to verify (`COMPLIANCE.md`).

## What this repository does NOT contain and never distributes

- **No part of Darwin**: no Darwin binary, image, cache, font, picture or screenshot. The user-space
  half of the system image is built on your machine from the Darwin userland 16.5 IPSW, which you
  get yourself, under Apple's license, and hand to `sysimage/unpack.sh` on your machine; no script
  here downloads it. Nothing of it is committed here.
- **No device firmware**: the GPU, Wi-Fi, audio DSP, modem, haptics and camera firmware is the
  vendor's (Qualcomm/Xiaomi). It is pulled from your own phone (`make firmware`) and is git-ignored.
- **No keys, certificates, activation data, or anything to impersonate a genuine Apple device to
  Apple's services.** Signing in to an Apple account is not possible here and is not a goal.

## The Apple-free configuration

The core kernel, the boot loader and the open tools build and run with **nothing of Apple's**:

    make qemu-open

builds the kernel and a placeholder loader (no IPSW needed, see `xnuloader/mkidentity.sh
--placeholder`) and boots a small raw-syscall user space (`userland/src/hello.c`) under QEMU.
This is the configuration a contributor can build and test without downloading any closed Apple
software. The full Darwin user space is an optional target that depends on software you supply.

## Reporting

If you believe a file in this repository infringes anyone's rights, open an issue (or email the
maintainer listed in the repository) and it will be reviewed and, where warranted, removed.
