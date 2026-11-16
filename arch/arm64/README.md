# arch/arm64

What is particular to the arm64 machines this port runs on, as Linux's (and Android's kernel) tree keeps it:

| Directory | What | Used by |
|---|---|---|
| [`boot/`](boot) | The UEFI application that starts the kernel (the loader), and the tools that pack its payload and the phone's boot image. | `boot/build.sh`, `./bench`, `tools/qemu-kernel.sh` |
| [`kernel/`](kernel) | The kernel's arm64 code of ours: `osfmk/arm64` of its tree. | `kernel/assemble.sh` puts it there |
| [`include/`](include) | The machine's headers: `include/pexpert` is `pexpert/pexpert/arm64` of the tree (`BCM2837.h`, the generic machine this port builds on). | `kernel/assemble.sh` |
| [`configs/`](configs) | `*_defconfig`: the boot arguments of each kind of run (`tools/defconfig.py`); `xnu/`: the kernel's own build configuration, `config/` of its tree (`MASTER.arm64.bcm2837`). | `./bench`, `tools/qemu-kernel.sh`; `kernel/assemble.sh` |
| [`tools/`](tools) | Tools for arm64 code: `pacstrip` (arm64e made to run without pointer authentication), `a64census` (what an ARMv8.2 core cannot execute in a dyld shared cache). | `darwin/userland/env.sh` builds `pacstrip` |

`kernel/lib.sh` (`OVERLAY_PLACES`) is the list of what here is a directory of the kernel's tree.
