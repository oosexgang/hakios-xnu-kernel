# POCO M4 5G (`light`) Hardware Register & Memory Map

Extracted directly from HyperOS stock firmware (`miui_LIGHTINGlobal_OS1.0.4.0.ULSINXM`) via `boot.img` DTB and `dtbo.img`.

---

## 1. Core Platform & CPU Architecture

| Property | Value | Description |
| :--- | :--- | :--- |
| **SoC** | MediaTek MT6833 / MT6833V (Dimensity 700) | 7nm Octa-core 5G SoC |
| **CPU Cluster 0** | 6× Cortex-A55 @ 2.0 GHz (`cpu@000` to `cpu@005`) | Little cores, power efficient |
| **CPU Cluster 1** | 2× Cortex-A76 @ 2.2 GHz (`cpu@100` to `cpu@101`) | Big cores, performance |
| **CPU Method** | `arm,psci` | Power State Coordination Interface |
| **Architecture** | ARMv8.2-A (AArch64) | 64-bit Darwin execution |

---

## 2. Physical Memory Map & Boot Offsets

```
0x40000000  +---------------------------------------+  Physical DRAM Base
            | Reserved Memory / Firmware Shared RAM  |
0x40080000  +---------------------------------------+  Kernel Load Address (boot.img)
            | Linux / XNU Kernel Image               |
0x47C80000  +---------------------------------------+  Kernel Tags & DTB Load Address
            | Device Tree Blob                       |
0x51100000  +---------------------------------------+  Ramdisk Load Address
            | Initrd / XNU Ramdisk                   |
            +---------------------------------------+
```

- **Physical DRAM Base**: `0x40000000` (1 GB boundary)
- **Kernel Load Address**: `0x40080000`
- **DTB Load Address**: `0x47C80000`
- **Ramdisk Load Address**: `0x51100000`
- **Page Size**: `2048` bytes (boot.img header v2)

---

## 3. Interrupt Controller (GIC)

The MT6833 uses the **ARM GIC-500** (`mediatek,gic500`), which implements the **ARM GICv3** architecture.

| Block | Physical Base Address | Size | Description |
| :--- | :--- | :--- | :--- |
| **GICD (Distributor)** | `0x0C000000` | `0x400000` (4 MB) | Global interrupt configuration & routing |
| **GICR / GICC (Redistributor)** | `0x0C400000` | `0x40000` (256 KB) | Per-core redistributor & CPU interfaces |

---

## 4. System Timers

- **Compatible**: `arm,armv8-timer` (ARM Architected Generic Timer)
- **System Counter Frequency (`CNTFRQ_EL0`)**: `0x00C65D40` = **13,000,000 Hz (13 MHz)**
- **Timer Interrupts (PPI)**:
  - Hypervisor Timer: PPI `13`
  - Virtual Timer: PPI `14`
  - Non-Secure Physical Timer: PPI `11`
  - Secure Physical Timer: PPI `10`

---

## 5. Early Serial Console (UART)

| Channel | Physical Base Address | Size | Compatible | Default Baud | Clock |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **UART0 (Debug)** | `0x11002000` | `0x1000` | `mediatek,mt6577-uart` | **921600 8N1** | 26 MHz (`dummy26m`) |
| **UART1** | `0x11003000` | `0x1000` | `mediatek,mt6577-uart` | 115200 / 921600 | 26 MHz |

- **Default Boot Console**: `console=ttyS0,921600n1`

---

## 6. Storage Controller (UFS 2.2)

POCO M4 5G uses **UFS 2.2** storage instead of eMMC.

| Block | Physical Base Address | Size | Interrupt (GIC) | Compatible |
| :--- | :--- | :--- | :--- | :--- |
| **UFSHCI Controller** | `0x11270000` | `0x2300` | SPI `105` (`0x69`) / IRQ `137` | `mediatek,mt8183-ufshci` |
| **UFS M-PHY** | `0x11FA0000` | `0xC000` | - | `mediatek,ufs_mphy` |
| **Regulator (VCC/LDO)** | `ldo_vufs` | - | - | `2.6V - 2.95V` |

---

## 7. Graphics Processing Unit (Mali-G57 Valhall)

| Property | Value | Description |
| :--- | :--- | :--- |
| **GPU Model** | ARM Mali-G57 MC2 | 2 Execution Engines, Valhall architecture |
| **Node** | `mali@13000000` | Compatible: `mediatek,mali`, `arm,mali-valhall` |
| **Physical Base** | `0x13000000` | Size: `0x4000` |
| **GPU Interrupt** | SPI `364` (`0x16C`) | General GPU interrupt |
| **MMU Interrupt** | SPI `365` (`0x16D`) | Page fault / TLB interrupt |
| **JOB Interrupt** | SPI `366` (`0x16E`) | Job completion interrupt |
| **EVENT Interrupt** | SPI `367` (`0x16F`) | Performance & counter events |
| **PWR Interrupt** | SPI `368` (`0x170`) | Power state change interrupt |

---

## 8. Input & Touchscreen

- **Primary Touchscreen**: Novatek SPI Touch (`novatek,NVT-ts-spi`)
- **Secondary / Fallback Panel Touch**: FocalTech I2C (`focaltech@0`)
- **Fingerprint Sensor**: Goodix / FPC1022 (`mediatek,fpc1022_irq`, `mediatek,goodix-fp`)

---

## 9. Audio & Display Backlight

- **Smart Audio Amplifier**: Silicon Inc. SIA8152 (`sia81xx@L` / `sia8152`)
- **Backlight Drivers**: Texas Instruments LM3697 (`lm3697@36`) / Kinetic KTD (`ktd@37`)
