# Samples

Starting points to copy, as Linux keeps its own in `samples/`: each directory is something you make your own.

| Sample | What it is | How it is used |
| --- | --- | --- |
| `device-repo/` | What a phone's own repository holds besides its package: how to build from it, what git leaves out, a check that runs on every push. | `tools/device.py init DIR --name NAME --soc SOC` makes the repository: the package (`board.dts`, `soc.dtsi`, `README.md`) and these files. Documentation/porting.md, "Or keep it in a repository of your own", has the whole layout: every file, who writes it, whether it is committed. |
| `my_defconfig` | A defconfig of your own: the boot arguments of a kind of run, with what each line means. | Copy it to `arch/arm64/configs/NAME_defconfig`, or pass the file itself (`tools/defconfig.py FILE`, `tools/image.py build --defconfig FILE`). |
