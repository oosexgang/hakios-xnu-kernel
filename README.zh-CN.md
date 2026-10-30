# HakiOS ARM64e / Renoir

[English](README.md) | 简体中文

基于 Apple 开源 XNU xnu-12377.121.6 与 iOS 26 用户态，开展 Renoir（SM7350）的内核及 ARM64e 用户态适配。

本项目基于 [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel)，加入 ARM64e 与 Renoir 适配。各历史版本的构建及启动记录见 `renoir/docs/`。

> 实验性项目，需要解锁 Bootloader。请使用 Renoir 的纯 RAM 流程，不得刷写或擦除手机分区。

## 现状

| 部分 | 状态 |
| --- | --- |
| 目标 | Renoir / SM7350，无硬件 PAC |
| 内核 | XNU xnu-12377.121.6，含机型与 ARM64e 适配 |
| 用户态 | iOS 26 适配；各版本的验证进度见对应启动记录 |
| 渲染 | CPU／软件渲染（`gpu=0`） |

## 目录

| 目录 | 内容 | 许可证 |
| --- | --- | --- |
| `kernel/` | `xnu-12377.121.6-hakios.patch`：相对 Apple xnu-12377.121.6 的全部内核修改；`apply.sh` 用来生成内核源码树 | APSL 2.0 |
| `base/` | HakiOS EFI 引导器（`xnuloader/`）、构建工具、设备配置及文档 | 沿用 HakiOS（APSL 2.0） |
| `userland/` | HakiOS 用户态及我们的修改：iOS 26 启动、注入、音频 HAL 插件 | 沿用 HakiOS |
| `tools/` | `check-publishable.sh` 在提交前检查有没有不能公开的文件 | MIT |

## 本项目不包含

本仓库不含任何 Apple、高通、小米或其他厂商的二进制文件或固件。

## 致谢

- [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel)：本项目的基础
- [Data A Live](https://github.com/ShueiTsuki/data-a-live)：骁龙 8 Elite Gen 5 上 iOS 的 GPU、音频与调频参考
- [Mesa](https://mesa3d.org/)（Turnip / freedreno）
- [Project Silicium / Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)
- Linux 内核的 msm、qcom 驱动：寄存器布局和上电顺序参考

驱动是参照 Linux 的公开资料编写的。具体说明见 NOTICE.md。

Apple、iOS、Metal 是 Apple Inc. 的商标。本项目与 Apple、高通、小米均无关联。
