# security

What decides, in the kernel, what a program may do: as Linux's tree keeps its security modules.

| Directory | In the kernel's tree | What |
|---|---|---|
| `amfi/` | `libkern/amfi/` | A stand-in for Apple's closed AppleMobileFileIntegrity kext: the table of functions the kernel reaches AMFI through, registered by us. No trust cache holds anything and no signature policy stands behind it; a process holds the entitlements its program's signature lists (`miniamfi_entitlements.cpp`), and a program without a signature holds none. |

The kernel's own MAC policy of ours is `bsd/kern/kern_minimac.c` in `kernel/overlay`; the keychain's keystore
is [`components/sep`](../components/sep).
