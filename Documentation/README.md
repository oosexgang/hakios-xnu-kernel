# Documentation

| Document | What it answers |
| --- | --- |
| [layout.md](layout.md) | Where everything is: each directory, and which directory of the kernel's tree it is. |
| [architecture.md](architecture.md) | How the boot chain, the kernel, the loader and Darwin user space fit together; what is data, what is a driver, what is the same for every device; what is not in the repository. |
| [porting.md](porting.md) | How to bring the project up on another Qualcomm phone, step by step. |
| [building.md](building.md) | What each build needs and produces; what has been verified and where. |
| [qemu.md](qemu.md) | The kernel in QEMU: booted as a phone boots it, its own root and shell; the network, a second disk, sound, input, processes, threads; what `make qemu` holds. |
| [darwin27-ubuntu.ru.md](darwin27-ubuntu.ru.md) | In Russian: what the project has done, and from an Ubuntu machine to Darwin 27 on a phone, step by step. |
| [drivers.md](drivers.md) | Every driver: its part, its boot argument and feature, how tied it is to one phone. |
| [features.md](features.md) | Every feature a board can list: the sections and keys it needs first, and what starts it. |
| [writing-a-driver.md](writing-a-driver.md) | How a driver is written here: the rules and why, what a driver has at hand, the steps, with an example driver and its test that `make check` builds and runs. |
| [boot-args.md](boot-args.md) | Every boot argument the kernel reads. Generated from the sources. |
| [backlight.md](backlight.md) | Brightness and Auto-Brightness: design, boot arguments, sysctls, testing. |
| [thermal.md](thermal.md) | The thermal limiter: what it does at which temperature, tuning, testing. |
| [metal-glass.md](metal-glass.md) | What exists for Metal on the Adreno, and the steps to Liquid Glass on Darwin 27. |
| [darwin26-boot.md](darwin26-boot.md) | The lab notebook: how Darwin 26 and 27 were brought to the home screen on Renoir, the build and boot commands, measurements. |
| [ram-budget.md](ram-budget.md) | Fitting the Darwin system image into the phone's RAM. |
| [troubleshooting.md](troubleshooting.md) | What a message or a symptom means and what to do: the tools, the builds, the kernel's log at boot, each driver, the checks. |
| [glossary.md](glossary.md) | The words of three worlds, one line each: this project's, Qualcomm's and Android's, Apple's. |
| [migrating.md](migrating.md) | For existing branches and patches: where everything went, and how to carry work over. |

Next to the code:

| Document | |
| --- | --- |
| [../devices/README.md](../devices/README.md) | Device packages, the board and SoC files. |
| [../workbench/README.md](../workbench/README.md) | What is tested without a phone, and how a driver runs without a kernel. |
| [../tools/README.md](../tools/README.md) | The tools, one line each. |
| [../kernel/README.md](../kernel/README.md) | How the kernel's sources are kept and worked on. |
| [../kernel/patches/README.md](../kernel/patches/README.md) | What each patch changes in Apple's files. |
| [../arch/arm64/boot/README.md](../arch/arm64/boot/README.md) | The loader. |
| [../darwin/README.md](../darwin/README.md) | What Darwin's user space needs: each library and tool, and what each needs. |
| [../CONTRIBUTING.md](../CONTRIBUTING.md) | Where a change goes, and the rules. |
| [../components/upstream.dts](../components/upstream.dts) | Everything taken from elsewhere, at the exact version, with its licence. |

`legacy/README.md` and `legacy/docs/` describe the first generation (Darwin 16.5 on the first phone and QEMU).
