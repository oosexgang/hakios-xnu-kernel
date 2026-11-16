# arch/arm64/tools

| File | What |
|---|---|
| `pacstrip.c` | Darwin 26/27 is arm64e: its code signs and checks pointers (PAC), which the phone's ARMv8.2 cores (Cortex-A78, A55) do not have. `pacstrip` rewrites every PAC instruction into its plain equivalent, in a copy of a dyld shared cache (`pacstrip cache OUTDIR FILE...`) or of a Mach-O (`pacstrip macho IN OUT`), and makes the code signature's page hashes again. `census` only counts. Needs OpenSSL's libcrypto; `darwin/userland/env.sh` builds it here when it is missing or older than its source. |
| `a64census.c` | Counts, in the executable mappings of a dyld shared cache, the instructions an ARMv8.2 core cannot execute (what `arch/arm64/kernel/a64emu.c` must do in their place, or `pacstrip` rewrite). |

```
cc -O2 -o arch/arm64/tools/pacstrip arch/arm64/tools/pacstrip.c -lcrypto
cc -O2 -o arch/arm64/tools/a64census arch/arm64/tools/a64census.c
```
