# crypto

minicrypto: the kernel's cryptography provider, in place of Apple's corecrypto kext (`com.apple.kec.corecrypto`),
which the open sources do not have. Everything here is `osfmk/minicrypto/` of the kernel's tree, where
`kernel/assemble.sh` puts it (`kernel/lib.sh`, `OVERLAY_PLACES`).

| File | What |
|---|---|
| `mc_provider.c` | Registers the provider with the kernel's crypto interface. |
| `mc_aes.c` | AES (FIPS 197) in ECB, CBC and CTR modes, in the descriptor format the interface expects (`struct ccmode_*`). |
| `mc_digests.c` | The message digests the interface needs beyond the SHA-256 of the open tree's built-in corecrypto. |
| `minicrypto.h` | What they share. |

What it protects is nothing secret of this phone's: there is no Secure Enclave and no device key here (see
[`components/sep`](../components/sep)).
