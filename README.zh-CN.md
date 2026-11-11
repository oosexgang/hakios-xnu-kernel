# HakiOS ARM64e 移植

[English](README.md) | 简体中文

## 当前 Renoir 配置

当前目标为小米 Renoir（骁龙 780G / SM7350），使用 iOS 27.2（24B119）用户态和 `renoir/kernel/` 的兼容性补丁。

- 通过原厂 `fastboot boot` 启动常驻 USB 引导器，只在 RAM 中运行。
- 使用 CPU／软件渲染（`gpu=0`），不包含外部硬件 Metal 插件及其构建依赖。
- 保留静态壁纸、显示完成回调、激活／设置注入和用户确认的状态栏时钟偏移。
- 软件渲染不支持液态玻璃，部分界面元素可能显示不完整。
- 2026-11-10 已在 Renoir 真机验证：重建的软件渲染内核通过 RAM 启动进入主屏幕，40 项引导检查全部通过。长期稳定性尚未验证。
- 当前默认关闭 Wi-Fi、声音、相机、触觉和电池支持；尚未实现温控及稳定 60 FPS。

构建及 RAM 启动步骤见 [docs/ios26-boot.md](docs/ios26-boot.md)。

> 实验性项目，需要解锁 Bootloader。Renoir 流程不得刷写、擦除或写入手机存储。仓库中其他机型的工具不属于此 RAM 启动流程。

## 目录

| 目录 | 内容 |
| --- | --- |
| `kernel/` | 相对 Apple xnu-12377.121.6 的内核修改及 `apply.sh` |
| `renoir/` | Renoir 内核适配、用户态注入、镜像构建与 RAM 启动工具 |
| `base/` | HakiOS 引导器、内核构建基础设施、设备配置和工具 |
| `userland/` | HakiOS 用户态源码、注入和音频 HAL 插件 |
| `tools/` | 发布检查与触摸固件转换工具 |

许可证见 [NOTICE.md](NOTICE.md) 和各源码文件头。Apple 系统文件、SDK、设备固件及构建镜像均为本地构建输入／输出，不属于仓库内容。

## 致谢

- [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel)
- [Data A Live](https://github.com/ShueiTsuki/data-a-live)
- [Mesa](https://mesa3d.org/)
- [Project Silicium / Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)
- Apple 开源 XNU 和各源码文件中注明的 Linux 驱动

Apple、iOS、Metal 是 Apple Inc. 的商标。本项目与 Apple、高通、小米均无关联。
