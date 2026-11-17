# Security

This kernel runs Darwin on phones it was not made for, from RAM, started by hand: it is for development, not for a
phone anyone depends on. Much of what keeps an iPhone safe is not here -- Apple's secure enclave and its keys, signed
code enforced end to end, the boot chain's measurements -- and stand-ins take its place (`security/amfi/`,
`components/sep/`, `kernel/overlay/bsd/kern/kern_minimac.c`), which allow what Darwin needs to start. Do not keep
anything on a phone running it that you would not show to whoever holds it.

## Reporting

A way to run code in the kernel from an application, to read another process's memory, to reach the phone's storage
past the one partition a write may touch (`storage { }` of the board), or to make the loader or a tool write where it
must not: please report it privately, through the repository's "Report a vulnerability" (GitHub's private advisories),
not in an issue. Say how to reproduce it, on a phone or in QEMU (`make qemu`), and what it reaches.

What is not a vulnerability here: that the stand-ins grant what Apple's policies would refuse (they do, on purpose,
and say so in their heads), or that a phone running this can be read by whoever has it in hand.

## What the tools never do

- Write to a phone's storage, but for the one partition its board file names, and only when asked
  (`arch/arm64/boot/src/own_partition.c`, `dsu_write.c`, held by `workbench/loader_own_partition_test.c`).
- Put Apple's files, a phone maker's firmware or a Wi-Fi passphrase into the repository or into a published image
  (`tools/check-publishable.sh`, `tools/image.py publish`, `userland/userland/build.sh` with `SHARE=1`).
