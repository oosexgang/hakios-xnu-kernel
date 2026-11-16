# The Secure Enclave's keystore

On an iPhone the keychain's keys are wrapped and unwrapped by the Secure Enclave (SEP), through the
kernel's AppleKeyStore, which `securityd`, MobileKeyBag.framework and libaks talk to. This phone has no
Secure Enclave. What stands in for it here describes one fixed situation: a device with no passcode,
never locked, unlocked since boot.

| Where | What |
|---|---|
| [`drivers/iokit/MiniAppleKeyStore.cpp`](../../drivers/iokit/MiniAppleKeyStore.cpp) | The kernel's `AppleKeyStore`, as MobileKeyBag and libaks see it: the lock state, the system keybag's handle, the device's state (DER), and the wrapping and unwrapping of each item's key by its protection class, so that the keychain works. Requests that would need real keys (creating or loading bags) are refused, each logged once. |
| `keychain_test.c` | Writes a keychain item, reads it back and deletes it, printing what Security.framework answers each time; `keep` and `read` across boots, for a data volume kept in the flash. Signed with `keychain_test.entitlements`; built by `userland/userland/build.sh`, run by the `rc.jobs.persist*` jobs. |

## What it is not

There are no secrets here. The "wrapping" is a reversible transformation, not a key of a Secure Enclave:
it protects nothing, and the keychain lives in `/private/var`, in memory unless the data volume is kept
in the flash. What needs a real passcode class cannot be had at all: Apple ID sign-in stores one token in
such a class, and [`../apple-id`](../apple-id) (`src/inject_kc.c`) lets that one store through.
