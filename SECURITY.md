# Security

This kernel runs Darwin on phones it was not made for, from RAM, started by hand: it is for development, not for a
phone anyone depends on. Much of what keeps an iPhone safe is not here -- Apple's secure enclave and its keys, signed
code enforced end to end, the boot chain's measurements -- and stand-ins take its place (`security/amfi/`,
`components/sep/`, `kernel/overlay/bsd/kern/kern_minimac.c`), which allow what Darwin needs to start. Do not keep
anything on a phone running it that you would not show to whoever holds it.

The keychain is not encrypted. The stand-in for AppleKeyStore (`drivers/iokit/MiniAppleKeyStore.cpp`) has no secret
to wrap the items' keys with, so its wrapping can be undone by anyone. By default that matters only while the phone
runs: the keychain lives in `/private/var`, in memory. The data volume kept in the flash (`var_store=1 ufs_write=1`,
`init/var_store.c`) leaves the keychain out unless `var_store_keychain=1` asks for it, and a keychain kept that way
is in the flash as good as in plaintext: whoever reads the flash reads every item. `var_store erase` zeroes what
the store held, not only its headers.

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
  (`tools/check-publishable.sh`, `tools/image.py publish`, `userland/userland/build.sh` and
  `darwin/userland/root26-darwin.sh` with `SHARE=1`).
