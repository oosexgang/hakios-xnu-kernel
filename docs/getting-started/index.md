# Getting started

Three ways in, from the one that needs nothing to the one that needs a phone. Each page says what you need, what to run, what you
should see, and what to check when you do not see it.

| | What you get | What you need | Time |
|---|---|---|---|
| [**The open kernel in QEMU**](qemu.md#the-open-kernel) | the kernel, the loader and a tiny user space, booting | a Linux or macOS machine, QEMU | minutes |
| [**Darwin in QEMU**](qemu.md#darwin-in-qemu) | the lock screen and the home screen in a window | Linux, 12 GB of disk, your own IPSW (nothing downloads it) | an hour, mostly downloads |
| [**On the phone**](phone.md) | the system on a POCO X5 Pro 5G, in RAM | the phone, its boot loader unlocked, `adb` and `fastboot` | an hour the first time |

Building from source is optional for all three: `prebuilt/` has a kernel, a loader and libraries made from these repositories.
[Building from source](building.md) when you change something.

## Quick start

```sh
git clone https://github.com/oosexgang/hakios-xnu-kernel.git hakios
cd hakios
make doctor           # what this machine lacks, and what to do
make qemu-open        # the open kernel in QEMU: nothing of Apple's, nothing of a phone's
```

A session on the phone, once [On the phone](phone.md) has been followed:

```sh
make redwood_defconfig   # the phone's configuration: the common kernel + the SC7280's + the phone's own
make kernel              # the kernel with the drivers that configuration builds
make phone SECS=600      # a 10-minute session on the phone, in RAM
```

The SoC, device and UEFI repositories, at the revisions `devices/manifest.conf` names:

```sh
tools/hakios-device.py sync
```

## Your machine

| Host | What works | Notes |
|---|---|---|
| Linux x86-64 | everything | the reference: CI runs here |
| macOS (Apple silicon) | building the kernel, the loader, user space; the tests | QEMU from Homebrew for running; the phone through `adb`/`fastboot` |
| Windows | everything, split between WSL 2 and Git Bash | [Windows](../windows.md) says what runs where |

`make doctor` checks the machine (and a phone on `adb`) for what HakiOS needs, and says what to install.

## Where to go next

- [Architecture](../concepts/architecture.md): how a boot goes, from UEFI to the home screen, and which part does what.
- [Configuration](../configs.md): defconfigs, fragments, and what a `CONFIG_` symbol turns into.
- [Port a device](../porting/index.md): your own phone, step by step.
