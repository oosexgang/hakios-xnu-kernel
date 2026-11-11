# Documentation

| Document | What it answers |
| --- | --- |
| [architecture.md](architecture.md) | How the boot chain, the kernel, the loader and iOS user space fit together; what is data, what is a driver, what is the same for every device; what is not in the repository. |
| [porting.md](porting.md) | How to bring the project up on another Qualcomm phone, step by step. |
| [building.md](building.md) | What each build needs and produces; what has been verified and where. |
| [drivers.md](drivers.md) | Every driver: its part, its boot argument and feature, how tied it is to one phone. |
| [boot-args.md](boot-args.md) | Every boot argument the kernel reads. Generated from the sources. |
| [backlight.md](backlight.md) | Brightness and Auto-Brightness: design, boot arguments, sysctls, testing. |
| [thermal.md](thermal.md) | The thermal limiter: what it does at which temperature, tuning, testing. |
| [metal-glass.md](metal-glass.md) | What exists for Metal on the Adreno, and the steps to Liquid Glass on iOS 27. |
| [ios26-boot.md](ios26-boot.md) | The lab notebook: how iOS 26 and 27 were brought to the home screen on Renoir, the build and boot commands, measurements. |
| [ram-budget.md](ram-budget.md) | Fitting the iOS system image into the phone's RAM. |
| [glossary.md](glossary.md) | The words of three worlds, one line each: this project's, Qualcomm's and Android's, Apple's. |
| [migrating.md](migrating.md) | For existing branches and patches: where everything went, and how to carry work over. |

Next to the code:

| Document | |
| --- | --- |
| [../devices/README.md](../devices/README.md) | Device packages, the board and SoC files. |
| [../soc/README.md](../soc/README.md) | The SoC files: which chips there are, what each section is for. |
| [../tests/README.md](../tests/README.md) | What is tested without a phone, and how a driver runs without a kernel. |
| [../tools/README.md](../tools/README.md) | The tools, one line each. |
| [../kernel/README.md](../kernel/README.md) | How the kernel's sources are kept and worked on. |
| [../kernel/patches/README.md](../kernel/patches/README.md) | What each patch changes in Apple's files. |
| [../loader/README.md](../loader/README.md) | The loader. |
| [../ios/README.md](../ios/README.md) | What iOS's user space needs: each library and tool, and what each needs. |
| [../CONTRIBUTING.md](../CONTRIBUTING.md) | Where a change goes, and the rules. |
| [../components.toml](../components.toml) | Everything taken from elsewhere, at the exact version, with its licence. |

`base/README.md` and `base/docs/` describe the first generation (iOS 16.5 on redwood and QEMU).
