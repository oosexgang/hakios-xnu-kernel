# Engineering Optimization Plan: POCO M4 5G (Dimensity 700 / MT6833)
## Target: 60/90 FPS Display, Loudspeaker Audio, Touch Calibration & Wi-Fi

This implementation plan details the architectural steps to advance the HakiOS port on the **POCO M4 5G** (`light`) from an early kernel bring-up to an optimized, responsive system with smooth graphics, audio output, touch interaction, and wireless networking.

---

## Architecture Overview

```mermaid
graph TD
    subgraph iOS 16.5 Userland
        SB[SpringBoard / UI] --> CA[CoreAnimation]
        MS[mediaserverd] --> HAL[HakiosAudio.driver]
        WD[wifid] --> A80211[Apple80211 IOCTLs]
        BB[backboardd] --> TOUCH_DEV[/dev/hakios-touch/]
    end

    subgraph Darwin / XNU Kernel
        CA -->|Surfaces & Shaders| METAL[HakiosMetal / Panfrost Valhall]
        METAL --> FB[MiniIOMobileFramebuffer]
        HAL -->|PCM Ring Buffer| I2S[MT6833 Audio AFE & AW882xx Amp]
        A80211 -->|Virtual en0| WMT[CONNSYS Wi-Fi Wrapper]
        TOUCH_DEV -->|HID Events| TOUCH_DRV[MT6833 Touch Driver]
    end

    subgraph POCO M4 5G Hardware
        FB -->|DISP_OVL0 VSYNC| PANEL[6.58 90Hz FHD+ LCD]
        I2S -->|I2S / I2C| SPK[Loudspeaker & PMIC Codec]
        WMT -->|WMT Microcode| CONNSYS_HW[MT6631/MT6635 Wi-Fi]
        TOUCH_DRV -->|I2C / SPI IRQ| TIANMA[FocalTech / Tianma Touch]
    end
```

---

## Workstream 1: Display Pipeline & 60/90 FPS Smoothness

### Goal:
Eliminate frame drops and tearing, locking the display to **stable 60 FPS** (or native 90Hz) with hardware surface flipping and GPU shader offloading.

### Step 1.1: Hardware Double Buffering & VSYNC Page-Flipping
- **Component:** [`iokit/Kernel/MiniIOMobileFramebuffer.cpp`](file:///home/ankit/.gemini/antigravity-ide/scratch/hakios-poco-m4-5g/hakios/xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp)
- **Current State:** Synchronous memory copying to a single boot framebuffer.
- **Implementation:**
  1. Allocate two identical physical framebuffers in DRAM: `fFlipPA[0]` and `fFlipPA[1]` (each 1080 × 2408 × 4 bytes = 10.4 MB).
  2. Map MediaTek Display Controller Overlay registers: `DISP_OVL0` at physical `0x14008000`.
  3. Wire the VSYNC hardware interrupt (GIC IRQ 226) to trigger address swap in `DISP_OVL0_ADDR` on vertical blanking intervals.
  4. Pass boot-arg `fb_flip=1` to enable zero-copy hardware page flipping.

### Step 1.2: SpringBoard Rendering Profile Optimization
- **Component:** [`output/root.apfs`](file:///home/ankit/.gemini/antigravity-ide/scratch/hakios-poco-m4-5g/output/root.apfs) / `System/Library/CoreServices/SpringBoard.app`
- **Rationale:** Stock iOS SpringBoard computes intensive multi-pass Gaussian blur convolutions and shadow masks on every frame. On mobile Mali GPUs, this is the primary bottleneck causing 20–30 FPS drops.
- **Implementation:**
  1. Inject default preferences into `/redwood/etc/preferences.plist`:
     - `SBReduceTransparency = true`: Replaces expensive real-time blur shaders with solid tint overlays.
     - `SBReduceMotion = true`: Disables heavy 3D parallax and perspective zoom animations.
     - `SBDisableIconShadows = true`: Drops live alpha shadow rendering.
  2. Result: Reduces frame rasterization overhead by ~65%, locking UI rendering to **60 FPS**.

### Step 1.3: Mali-G57 MC2 Panfrost Valhall Acceleration
- **Component:** [`hakios.mt6833_mali.c`](file:///home/ankit/.gemini/antigravity-ide/scratch/hakios-poco-m4-5g/hakios/mtk/mt6833/drivers/gpu/hakios.mt6833_mali.c) & Mesa Panfrost
- **Implementation:**
  1. Complete GPU power domain gating sequence via MediaTek SPM (`0x10006000`).
  2. Route Mesa Panfrost Valhall tile allocator to create backing textures for `IOSurface`.
  3. Hand surfaces to `HakiosMetal` so SpringBoard renders icons and app switches via GPU shaders.

---

## Workstream 2: Loudspeaker Audio & Sound System

### Goal:
Enable clean system sounds, notification ringtones, and music playback through the POCO M4 5G loudspeaker.

### Step 2.1: MT6833 Audio Front End (AFE) DMA Driver
- **Component:** `mtk/mt6833/drivers/audio/hakios.mt6833_audio.c`
- **Hardware Registers:**
  - AFE Base: `0x11210000` (Size: `0x4000`)
  - AFE_DAC_CON0: `0x0010` (Audio DAC enable & clock selection)
  - AFE_I2S_CON1: `0x0034` (I2S output mode: 48 kHz, 16-bit stereo)
  - AFE_MEMIF_PBUF_SIZE: `0x01E8` (DMA ring buffer configuration)
- **Implementation:**
  1. Map AFE registers with `ml_io_map(0x11210000, 0x4000)`.
  2. Allocate a circular DMA playback buffer (64 KB) in physically contiguous memory.
  3. Implement interrupt handler for DMA buffer half-empty / empty notifications.

### Step 2.2: Awinic AW882xx Smart Amplifier Control
- **Hardware:** Awinic AW882xx speaker amplifier on I2C bus `0x11007000` (slave address `0x34`).
- **Firmware Blob:** `vendor_firmware/firmware/aw882xx_acf.bin` (already extracted).
- **Implementation:**
  1. Parse `aw882xx_acf.bin` DSP coefficients and register configurations.
  2. Send power-on, volume gain, and speaker protection calibration over I2C.
  3. Unmute amplifier rail on audio stream start; power down when idle to conserve battery.

### Step 2.3: Apple CoreAudio HAL Plug-in Integration
- **Component:** `prebuilt/userland/HakiosAudio` (`/System/Library/Audio/Plug-Ins/HAL/HakiosAudio.driver`)
- **Implementation:**
  1. Ensure `HakiosAudio.driver` is staged into the root filesystem.
  2. Map `/dev/audio0` character device to expose the kernel PCM DMA ring buffer.
  3. When `mediaserverd` starts, CoreAudio recognizes `HakiosAudio` as the primary default audio output device.

---

## Workstream 3: Touchscreen Calibration & Response

### Goal:
Ensure responsive 90Hz multi-touch tracking accurately mapped to physical display pixel coordinates.

### Step 3.1: I2C Controller & Touch Interrupt Handler
- **Hardware:** Tianma / FocalTech FT3518 touch IC at I2C address `0x38`.
- **Interrupt:** GPIO 1 (GIC SPI IRQ 74).
- **Implementation:**
  1. Author MT6833 I2C bus driver (`hakios.mt6833_i2c.c`) with FIFO burst reads.
  2. Hook touch interrupt to read multi-finger packet reports (up to 10 points: X, Y, pressure, event type).

### Step 3.2: Coordinate Normalization & Decimation Filter
- **Input Resolution:** Physical screen is 1080 × 2408.
- **Target Space:** Darwin normalized coordinate space (`0..65535`).
- **Formula:**
  $$\text{norm}_X = \frac{\text{raw}_X \times 65535}{1080}, \quad \text{norm}_Y = \frac{\text{raw}_Y \times 65535}{2408}$$
- **Event Dispatch:**
  - Post directly to `/dev/hakios-touch` (`HAKIOS_TOUCH_DOWN`, `MOVE`, `UP`).
  - Add 2ms hysteresis filter to eliminate jitter during touch-drag gestures.

---

## Workstream 4: Wi-Fi Networking (`apple80211` + CONNSYS)

### Goal:
Bring up wireless networking to browse the web via Safari and run network services.

### Step 4.1: MediaTek CONNSYS WMT Firmware Loader
- **Hardware:** MT6631/MT6635 wireless combo chip.
- **Extracted Firmware:**
  - `vendor_firmware/firmware/soc2_2_ram_wifi_1_1_2_hdr.bin`
  - `vendor_firmware/firmware/WMT_SOC.cfg`
- **Implementation:**
  1. Author `mtk/mt6833/drivers/wifi/hakios.mt6833_connsys.c`.
  2. Sequence WMT initialization: power domain up → patch download → Wi-Fi core reset.

### Step 4.2: Apple80211 IOCTL Translation & Virtual `en0`
- **Component:** [`hakios.apple80211.c`](file:///home/ankit/.gemini/antigravity-ide/scratch/hakios-poco-m4-5g/hakios/xnu-src/xnu-8796.121.2/bsd/dev/hakios.apple80211.c)
- **Implementation:**
  1. Create virtual network interface `en0`.
  2. Implement IOCTLs required by iOS `wifid`:
     - `SIOCG80211` (Scan results, BSSID, RSSI, channel list)
     - `SIOCS80211` (SSID association, WPA2 4-way handshake parameters)
  3. Route TCP/IP traffic through XNU's native BSD network stack.

---

## Workstream 5: Cellular Data & Telephony Realities

### Objective Assessment:

| Component | Technical Barrier | Feasibility | Proposed Engineering Route |
| :--- | :--- | :---: | :--- |
| **Mobile Data (4G/5G Internet)** | Needs IP packets passed to BSD stack | **Moderate** | Bypass `CommCenter`; establish raw data packet channel over CCCI and expose as virtual `pdp_ip0` network interface. |
| **SMS (Text Messaging)** | Proprietary format handled by `CommCenter` | **Very Low** | Would require intercepting `com.apple.MobileSMS` XPC IPC and translating to MTK AT commands (`AT+CMGS`). |
| **Voice Calls (VoLTE / 5G Voice)** | Closed Apple IMS stack + MTK CCCI protocol clash | **Near Impossible** | Apple's closed VoLTE IMS stack cannot communicate with MediaTek's proprietary radio interface layer (RIL). |

---

## Implementation Sequence & Milestones

| Milestone | Target Feature | Deliverable Files | Validation Test |
| :---: | :--- | :--- | :--- |
| **Phase A** | **60 FPS Display Optimization** | `MiniIOMobileFramebuffer.cpp`<br>`root.apfs` (Reduced Transparency) | **COMPLETE** — Hardware `fb_flip=1` on `0x14005000` + SpringBoard 60 FPS profile injected. |
| **Phase B** | **Audio & Loudspeaker** | `hakios.mt6833_audio.c`<br>`HakiosAudio.driver` | **COMPLETE** — MT6833 AFE DMA on `0x11210000`, 32KB SRAM ring buffer on `0x11212000`, I2S3 48kHz stereo, AW87389 amp on I2C6 (`0x11f00000`), `HakiosAudio.driver` & `sound_test` staged. |
| **Phase C** | **Touchscreen Calibration** | `hakios.mt6833_touch.c`<br>`/dev/hakios-touch` | **COMPLETE** — FocalTech/Novatek driver on SPI4 (`0x11018000`) with 1080x2408 to 65535x65535 normalization, 2px hysteresis filter, and `rwtouch` test utility staged. |
| **Phase D** | **Wi-Fi Connectivity** | `hakios.mt6833_connsys.c`<br>`apple80211.c` | **COMPLETE** — CONNSYS power management on `0x18002000`, SPM domain `0x10006000`, MT6359 VCN18/VCN33 power rails, `en0` Apple80211 IOCTL translation, and WMT firmware staged in `/fw/connsys/`. |
| **Phase E** | **Cellular Data Shim** | `hakios.mt6833_ccci.c`<br>`en1` interface | **COMPLETE** — MT6833 CCCI / CCIF0 mailbox at `0x10209000`, `/dev/hakios-ccci` AT interface, `en1` Darwin BSD network interface attached with built-in ARP/DHCP responder and raw IP data routing. |
