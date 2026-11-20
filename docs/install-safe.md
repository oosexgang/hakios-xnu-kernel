# From RAM to the phone's own storage, without a brick: the order, the gates, and what has been done

[install.md](install.md) is the design and its rules. This is the order of work that follows from them, with a gate between
every two steps, and an honest account of how far it has got. **Nothing here has written a byte to a phone.** The first two
stages are tools that only read; the rest is a plan for a person at the desk.

## Why this order

A phone is bricked by writing the wrong thing to the chain it starts from, or by leaving it with no slot that starts. So:

1. nothing is written until what cannot be made again has been copied off and proven;
2. the system moves to the phone's own storage **before** the boot chain does: the loader is still started from RAM
   (`fastboot boot`), so a wrong system image costs a reboot into Android, not a phone;
3. the boot chain moves last, into the slot Android does **not** run from, with the other slot left exactly as it was;
4. the bootloader chain itself (`xbl`, `abl`, `tz`, `hyp` ...), the modem's and the radio's calibration, and the active slot are
   never written by this project. On Xiaomi phones a damaged bootloader chain can need an authorised service tool to revive;
   that is the one failure no step here may risk.

## The stages

| # | Stage | What happens to the phone | State |
|---|---|---|---|
| 0 | **Preflight.** Two slots? Which one is Android on? Is the bootloader really unlocked? Battery over 60% and the cable good? `darwin` already there? | nothing; reads | `make doctor` does most of it. The battery gate is NOT checked by anything yet (see "The phone that went quiet") |
| 1 | **Backup.** The boot-related partitions of both slots, `vbmeta*`, `persist`, `modemst1/2`, `fsg`, the partition tables, and with `--with-bootchain` the chain. Each copy is proven by three SHA-256s (the phone's before, the host's, the phone's after) | nothing; reads, as root | **written and tested** against a stand-in adb: `tools/backup-partitions.py` (`tools/backup-partitions-test.py`). Not run on a phone: the phone was not on USB when it was finished |
| 2 | **Carve `darwin`** off the end of Android's userdata | **Android's data is shrunk and moved**: the f2fs is resized and the GPT rewritten. Back up the phone's data first (the user's own, with Android's tools); the tool does not | plan only: `tools/userdata-carve.py` prints the commands and writes nothing |
| 3 | **Write the system image to `darwin`** with the guarded writer | writes `darwin` only; the writer refuses any other partition and reads every write back | exists: `tools/darwin-write.py`; its self-test refuses the wrong partition. Never run on a phone |
| 4 | **Boot from the phone's storage, still from RAM.** `fastboot boot` the loader, and the kernel mounts its root from `darwin` (`root_apfs=`) instead of from an image uploaded to RAM | `fastboot boot` writes nothing; `ufs_write` stays 0 | **UFS adoption in progress**: on sapphiren the controller survey and two list switches work without issuing a storage command; no block has been read yet. Repeat the eventual root boot until it is boring; this is the gate before stage 5 |
| 5 | **The boot image into the inactive slot**, `fastboot set_active` to it, once | writes the inactive slot's `boot` (and what its recipe needs); the active slot is not touched | **not done, not tried**; whether Project Silicium's boot manager behaves from a boot slot, and what `vbmeta` says to a modified boot image, are unknown |
| 6 | **A way back**, tested before it is needed: `fastboot set_active a` (or b) from the bootloader, and Android's own slot switch | writes the slot flag | the standard fastboot command; not tried here |

Stage 4 is the one that changes the project: it makes "the system lives in RAM for the length of a session" false, and it does so
without any step that can brick. A phone that boots from `darwin` by `fastboot boot` is already "not RAM"; stage 5 only removes the cable.

### Sapphiren UFS adoption

The Android device tree puts UFSHCI at `0x4804000`, not the SC7280 address the first native driver used. A read-only survey showed
UFSHCI 3.0 enabled, 32 transfer slots, 64-bit addressing, the device present and the transfer list ready. ABL's list lives in RAM
that XNU manages, so it is not a safe restoration target. For a native-only run the loader now replaces it, while idle, with an
inert list in loader-owned memory. Its slot 0 contains a whitelisted READ(10) shape only for inspection; its doorbell is never rung.

`ufs_native=2 ufs_hc_pa=0x4804000` validated the controller and standby list without changing the list. Then `ufs_native=3` switched
the kernel's 32-slot list in and out twice and restored the standby list, still with zero commands and zero reads. The next gate is
`ufs_native=4`: TEST UNIT READY followed by one READ(10) of LUN 0's GPT header and a signature/CRC check. It is compiled but has not
completed on the phone yet; partition creation and every write remain later stages.

## The gates

A stage is started only when the one before it has a recorded result and a person has read it:

- before stage 2: `manifest.json` of stage 1 exists and every partition in it is proven; a copy of the phone's data exists;
- before stage 3: the new GPT was read back from the phone and equals what the carve tool made; Android boots and its data is there;
- before stage 5: stage 4 has booted the system from `darwin` at least three times in a row, and the log says so;
- before every write: `tools/provenance.allow` lists the writer (CI fails if a writer appears that is not listed).

## The phone that went quiet

Writing this, the phone vanished from USB: not adb, not fastboot, not Qualcomm's emergency mode. The last run before that ended
normally and left the phone in the loader. The likeliest cause is the **battery**: nothing charges the phone while it sits in the
loader (the kernel's charging path is not written; the UEFI shell does not charge), the screen was on for hours, and a long session
is a flat battery. If so, plugging it into a charger for a few minutes brings it back to Android by itself. That has not been
confirmed. It is why stage 0 has a battery gate, and why an unattended run is not a safe thing to leave overnight until the kernel
reads the battery and stops the run (`thermal_stop` is the same idea for heat; a battery guard is the same again, and needs the
battery reading of the audio DSP's driver, which only runs with `adsp=1`).

## What the user decides

1. Whether to give up some of the phone's storage (stage 2): the size of `darwin`. The system image alone is 2.5 GB; room to live in
   is more.
2. Whether Android's data may be disturbed at all. If not, stages 2 to 5 wait for a second phone or a spare partition.
3. Who is at the desk for stage 5.

## What was not done tonight, on purpose

No partition was written, no slot was switched, `darwin` was not made, and the installer was not written: an installer that
has never run on a phone, written while its owner sleeps, would be a guess with a progress bar. The tools above are the part
that could be built and tested without a phone, and they are.
