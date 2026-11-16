# arch/arm64/configs

- `*_defconfig`: the boot arguments of one kind of run, one a line (`tools/defconfig.py --list` says which there
  are, `tools/defconfig.py NAME` their line). `./bench` takes one with `BENCH_DEFCONFIG=NAME`,
  `tools/qemu-kernel.sh` with `--defconfig NAME`.
- `xnu/`: the kernel's build configuration, `config/` of its tree: `MASTER.arm64.bcm2837`, the options of the
  arm64 kernel this port builds (`kernel/build.sh`, `MACHINE_CONFIG=BCM2837` of `build.config.arm64`).
