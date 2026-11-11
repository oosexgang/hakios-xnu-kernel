# 高通手机上的 HakiOS

[English](README.md) | 简体中文

在搭载高通芯片的 Android 手机上运行 Apple 开源 XNU 内核和 iOS 用户态。不刷写任何内容：UEFI 通过
`fastboot boot` 启动，系统只在 RAM 中运行，重启后回到 Android。

> 实验性项目，需要解锁 Bootloader。本仓库的任何工具都不得刷写、擦除或写入手机存储。

本文是 [README.md](README.md) 的译文；如有出入，以英文版为准。其余文档目前只有英文。

## 状态

| 板型 | 设备 | SoC | 状态 |
| --- | --- | --- | --- |
| `renoir` | 小米 11 青春版 5G（Mi 11 Lite 5G） | 骁龙 780G（SM7350） | iOS 27.2（24B119）从 RAM 启动到主屏幕。2026-11-10 已在真机验证。 |
| `redwood` | POCO X5 Pro 5G | 骁龙 778G（SM7325） | 最早的移植，基于 iOS 16.5（`base/`）。其驱动已在当前内核中，尚未用 iOS 26/27 启动。 |
| `qemu-virt` | QEMU `virt` | — | iOS 16.5 一代可在其上运行。尚未适配当前内核。 |

Renoir 目前：

- **可用：** 启动到主屏幕、触摸、按键、CPU 全速、小组件有内容。
- **软件渲染**（`gpu=0`）：液态玻璃、CASDF 形状和锁屏大时钟不会绘制。Metal 计划见
  [docs/metal-glass.md](docs/metal-glass.md)。
- **已编写，已在主机和 QEMU 下测试，尚未在真机验证：** 亮度与自动亮度（[docs/backlight.md](docs/backlight.md)）、
  温控限制器（[docs/thermal.md](docs/thermal.md)）。两者默认只读取，不写入。
- **关闭：** Wi-Fi、声音、相机、触觉、电池。这些驱动按 redwood 的器件接线编写。
- **未解决：** 稳定 60 FPS、长期稳定性。

## 从这里开始

| 你想要 | 阅读 |
| --- | --- |
| 了解整体结构 | [docs/architecture.md](docs/architecture.md) |
| 移植到自己的手机 | [docs/porting.md](docs/porting.md) |
| 构建内核或引导器 | [docs/building.md](docs/building.md) |
| 查某个驱动或启动参数的作用 | [docs/drivers.md](docs/drivers.md)、[docs/boot-args.md](docs/boot-args.md) |
| 修改并提交 | [CONTRIBUTING.md](CONTRIBUTING.md) |
| 迁移旧目录结构下的分支或补丁 | [docs/migrating.md](docs/migrating.md) |

不需要手机的检查，一条命令即可运行：

```sh
make check
```

## 目录

| 路径 | 内容 |
| --- | --- |
| `devices/`、`soc/` | **每个板型一个文件，每个 SoC 一个文件。** 设备只在这里描述；内核和引导器的表由此生成。 |
| `kernel/` | 内核：固定版本的 Apple 发布、HakiOS 新增的文件（`overlay/`）、对 Apple 文件的修改（`patches/`），以及组装和构建工具。 |
| `loader/` | 启动内核并常驻以接收日志的 UEFI 应用。 |
| `ios/` | iOS 26/27 用户态所需：注入少数进程的库、PAC 重写工具、镜像构建工具、启动工具。 |
| `userland/` | 仍在使用或正在移植的早期用户态源码：Metal 设备、供 Mesa 使用的着色器编译器、音频插件、APFS 镜像工具。 |
| `tests/` | 主机测试：内核中与设备无关的驱动（对照硬件模型）以及各工具。 |
| `tools/` | `boards.py`（板型文件）及 `devicetree.py`（从手机设备树读取的内容）、`bootargs.py`（生成的参考文档）、lint 与发布检查。 |
| `components.toml` | 从外部获取的所有内容及其固定版本。 |
| `docs/` | 文档。 |
| `base/` | 第一代工作区（redwood 和 QEMU 上的 iOS 16.5）。保留尚未迁移的部分：UEFI 移植、Wi-Fi 兼容层、QEMU 脚本。 |

## 一段话说明移植

一部手机由 `devices/NAME/board.toml` 描述，其芯片由 `soc/SOC.toml` 描述。
`tools/boards.py new NAME --soc SOC --dts phone.dts` 可从手机的 Android 设备树生成这两个文件的初稿。
内核只在列出了相应驱动的板型上启动该驱动，因此新手机一开始只有显示，之后逐行增加硬件；
`tools/boards.py status NAME` 会说明接下来可以启用什么，以及其余各项还缺什么。
尚无驱动的器件（触摸控制器、功放等）需要另写驱动。指南见 [docs/porting.md](docs/porting.md)。

## 不在本仓库中的内容

Apple 的文件（XNU 在构建时获取；iOS 来自你本机上的 Apple IPSW）、手机固件（取自同一部手机）以及第三方工具链。
此外，目前启动 Renoir 所用的常驻引导器和镜像工具位于团队尚未公开的工作树中：在其公开之前，
无法从一份干净的检出复现 Renoir 的启动。详见
[docs/architecture.md](docs/architecture.md#what-is-not-in-this-repository)。

## 许可证

见 [NOTICE.md](NOTICE.md) 和各源码文件头。Apple 系统文件、SDK、设备固件及构建镜像均为本地构建输入／输出，
不属于仓库内容；`tools/check-publishable.sh` 会检查它们。

## 致谢

- [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel)
- [Data A Live](https://github.com/ShueiTsuki/data-a-live)
- [Mesa](https://mesa3d.org/)
- [Project Silicium / Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)
- Apple 开源 XNU 和各源码文件中注明的 Linux 驱动

Apple、iOS、Metal 是 Apple Inc. 的商标。本项目与 Apple、高通、小米均无关联。
