## Firmware Infos

- **Device:** POCO X5 Pro 5G (redwood)
- **Region:** Global
- **Version:** `OS1.0.3.0.UMSMIXM` / `BOOT.MXF.1.0-00852-LAHAINA-2.12801.8`

## Patches/Fixes

### ButtonsDxe:

- **Reason:** To make the Power Button usable in UEFI.
- **Patch:** The Special Qcom Key Code (`0x102`) has been Changed to the Key Code Enter (`0xD`).
- **Patch Creator:** [Gustave Monce](https://github.com/gus33000)

### DALSYSDxe:

- **Reason:** To avoid Mismatched Cached Copies.
- **Patch:** Enabled Cache Coherence for UFS.
- **Patch Creator:** [Gustave Monce](https://github.com/gus33000)

### TzDxeLA:

- **Reason:** To make UEFI not Crash during UEFI Boot.
- **Patch:** The Global TZ Applet Variable has been Changed to `TRUE` from `FALSE`.
- **Patch Creator:** [N1kroks](https://github.com/N1kroks/)

### UsbConfigDxe:

- **Reason:** To make USB work in the OS.
- **Patch:** The USB Deinit Code has been Removed from Exit Boot Services Event to allow the OS to continue using the USB Port.
- **Patch Creator:** [Gustave Monce](https://github.com/gus33000)

### UsbMsdDxe:

- **Reason:** To make the Internal Storage be recognised as Hard Drives instead of USB Drives in Mass Storage.
- **Patch:** Changed Removable State to Non-Removable.
- **Patch Creator:** [N1kroks](https://github.com/N1kroks)
