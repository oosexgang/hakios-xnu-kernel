# HakiOS

[English](README.md) · 中文 · [Українська](README.uk.md)

**运行在 Android 手机上的 Apple 开源 Darwin 内核（XNU，xnu-8796.121.2）**，就像 Android 是手机上的 Linux 一样：有自己的引导加载器、自己的驱动、自己的配置系统和自己的版本。同一个内核可以在 QEMU 中运行，不需要任何 Apple 的东西，也不需要任何手机的东西（`make qemu-open`）。这是一个研究平台，不是 ROM：默认从内存运行，重启后手机回到 Android。

**文档：** [docs/](docs/index.md)（英文），以及网站 [hackios-web.vercel.app](https://hackios-web.vercel.app)。

**动态，2026 年 11 月 4 日：** 在 POCO X5 Pro 5G 上，窗口服务器通过 Metal 在 Adreno 642L 上绘制，有两条路径。hakios-metal：翻页每秒 110 至 115 帧，Settings 滚动 118 帧，主屏幕按钮 106 帧（以 828x1792 渲染并拉伸到面板；完整 1125x2436 时为 101、约 90、约 90）。UniXNU 的 Metal on Vulkan：翻页 50 至 57 帧，滚动 41 帧。屏幕是 120 Hz，**尚未达到每秒 120 帧**。[Adreno 上的 Metal](docs/reference/metal.md)，[测量结果](docs/reference/metal-measurements.md)。

从 [Getting started](docs/getting-started/index.md)、[On the phone](docs/getting-started/phone.md) 或 [Port a device](docs/porting/index.md) 开始。各部分的工作情况：[文档首页](docs/index.md#what-works)。

## 仓库

| 仓库 | 内容 |
|---|---|
| [hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) | **公共部分**：内核移植、所有公共驱动、引导加载器、用户空间、工具、本文档以及 `devices/qemu` |
| [hakios-metal](https://github.com/oosexgang/hakios-metal) | 通过内核的 GPU 驱动，在 Adreno 上实现 Metal 的命令模型 |
| [hakios-sm7325](https://github.com/oosexgang/hakios-sm7325) | POCO X5 Pro 5G：defconfig、状态、UEFI 平台、固件脚本及其 SoC 层（Qualcomm SC7280 系列） |
| [hakios-mt6768](https://github.com/oosexgang/hakios-mt6768) | Infinix Hot 20（MediaTek Helio G85）：实验性移植，有自己的引导加载器。不在本仓库的清单中 |
| [hakios-uefi](https://github.com/oosexgang/hakios-uefi) | 启动引导加载器的固件：带有 HakiOS 启动路径、横幅和名称的 Mu-Silicium，以及 HakiOS 的 U-Boot |
| [hackios-web](https://github.com/oosexgang/hackios-web) | 网站 |
| [hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland) | 旧仓库：只为保留链接而存在的指向。用户空间现在在这里的 `userland/` |

它们如何组合：[仓库说明](docs/porting/repositories.md)。

## 你需要自带什么，以及这里永远没有什么

这些仓库中没有任何 Apple 的东西，也没有厂商固件。如果需要 Darwin 用户空间，你要用自己获取的 IPSW 自行布置；固件取自你自己的手机；没有任何东西会替你下载、解密或绕过。Apple 的服务（激活、Apple ID、iCloud、App Store）不受支持，也不是目标。[仓库里有什么，永远没有什么](docs/index.md#what-is-in-the-repository-and-what-never-is)。

## 参与贡献

见 [CONTRIBUTING.md](CONTRIBUTING.md) 和 [COMMUNITY.md](COMMUNITY.md)。

## 许可与法律

XNU 补丁及其派生文件采用 Apple Public Source License 2.0（[LICENSE.APSL](LICENSE.APSL)），其余采用 MIT（[LICENSE](LICENSE)），以文件头为准。第三方组件：[NOTICE](NOTICE)、[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)、[COMPLIANCE.md](COMPLIANCE.md)。分发什么、不分发什么：[LEGAL.md](LEGAL.md)。以上都不构成法律意见。

HakiOS 是一个独立的研究项目，**与 Apple 没有关联，也未获其认可**；提及 “Darwin”、“iPhone”、“XNU” 和 “Apple” 只是为了说明代码面向什么。如果你认为这里的某个文件侵犯了任何人的权利，请提交 issue，它会被移除。

## 团队

hakios 是一个实验性项目，致力于把 darwin 带到 android 硬件上。我们的目标是让 hakios 免费并人人可用。

@oodze — 创始人，@netusd — 推广 / ceo-dev，@wannqn — 开发，@hxfuxyyXD — 开发，Rhaenja Hun（杭润嘉）— 开发 / 赞助

感谢 silicium 团队提供的源代码！
