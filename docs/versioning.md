# The kernel's version

An Android kernel says `5.4.147-android12-9-...-g1a2b3c4d`: the Linux it is built on, the vendor's branch, and the
commit. This kernel says the same kind of thing about itself:

```
2.3.4-hakios-darwin22.5-sc7280-g1a2b3c4d[-dirty]
│ │  │  │      │          │      └ the commit; "-dirty" if a tracked file differs from it
│ │  │  │      │          └ the SoC family the configuration is for (CONFIG_SOC)
│ │  │  │      └ the Darwin it is built on (22.5.0, Apple's open xnu-8796.121.2)
│ │  │  └ EXTRAVERSION: what the tree is called
│ │  └ SUBLEVEL
│ └ PATCHLEVEL
└ VERSION
```

## Where it is shown

1. **The loader's banner** (`xnuloader/`), under `HAKIOS`: `hello hakios-kernel!` and `kernel.ver=<the string>`.
2. **The kernel**: a line in the boot log, and `sysctl kern.hakios.version` (`userland/src/hello.c` prints it).
3. **The payload**: `xnuloader/mkpayload.py` puts it in as the entry `kernel.ver`; the loader hands it to the kernel on
   its command line as `hakios_kver=`. A kernel started some other way (no loader) says `unknown`: it does not guess.

Darwin's own `uname -v` ("Darwin Kernel Version 22.5.0: ...") is Apple's and stays as it is, as Linux's `uname -r` stays
in an Android kernel next to the vendor's own string.

## Where it comes from

`VERSION`, at the top of the repository, in the form of the Makefile of Linux (`VERSION`, `PATCHLEVEL`, `SUBLEVEL`,
`EXTRAVERSION`, `NAME`, plus `DARWIN` and `XNU`: the release this port sits on). `tools/kernel-version.py` makes the
string from it, from `CONFIG_SOC` of the `.config` (see [configs.md](configs.md)) and from git:

```bash
tools/kernel-version.py            # 2.3.4-hakios-darwin22.5-sc7280-g1a2b3c4d (the -sc7280 part is there with a .config)
tools/kernel-version.py --short    # 2.3.4
tools/kernel-version.py --release  # 2.3.4-hakios: what a tag is called (v2.3.4-hakios)
tools/kernel-version.py --all      # every part, for a build log
```

`HAKIOS_VERSION_SUFFIX` in the environment replaces the git part (a reproducible build, or a tree that is not a
checkout).

The kernel is told its version and does not carry it, on purpose: it is built once from Apple's open source plus this
port, and the payload is made in a second. A version that is only the commit, or the SoC of one configuration, then does
not need the Mach-O rebuilt.

## When a number moves

| | moves when | example |
|---|---|---|
| **VERSION** (2) | a change that breaks what a user, a port or a boot image relies on | the payload format, the meaning of a boot-argument, the layout of the partition `darwin` |
| **PATCHLEVEL** (3) | a feature | a driver, a device, a tool |
| **SUBLEVEL** (4) | a fix | a bug in a driver, a wrong default |
| **EXTRAVERSION** | the tree is somebody's branch | a vendor writes its own here, as Android's `-android12-9` |

A boot-argument that is removed or changes its meaning is a **VERSION** change, which is why
[boot-args.md](boot-args.md) is made from the source and checked in CI: it is the list of what a version promises.

## Honest limits

- The number says which source the kernel was built from. It does not say that any given phone boots it; what has
  been seen to boot is in the README's status table and in [port-pkg110.md](port-pkg110.md).
- `-dirty` means a tracked file differs from the commit. A build from a modified tree says so.
- `Darwin 22.5` is a name for the open-source xnu release the kernel is built on. The kernel is not Apple's `kernel`
  and is not Darwin's.
