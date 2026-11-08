# A camera on another MediaTek phone

What the Infinix Hot 20 (X6826B, MT6768, Samsung S5K JN1) taught, as an order of work for another MediaTek phone. The code that is the same on every
phone is `xnu/files/bsd/dev/redwood_mtkcam_skel.c`; what is different is a table of operations (`struct mtkcam_platform`) you fill in. The proven
sequence for one phone is `kernel/bsd/dev/redwood_mtkcam.c` (kernel) and `mtkloader/mtkcam.h` (the loader's probe) in the device repository
[hakios-mt6768](https://github.com/oosexgang/hakios-mt6768); how it was found, run by run, is its `docs/camera-x6826b.md` (in Russian).

## What the skeleton does, and what it refuses to do

`redwood_mtkcam_skel.c` makes `/dev/redwood-camera` (the ABI is `sys/redwood_camera.h`, header `RWFRAME1`): a session starts when something reads and ends
when nothing has for 4 s; the newest finished frame is handed over; the `EXPOSURE` command reaches the sensor. User space (`userland/src/camera_device.c`
in `mediaserverd`, `camera_isp.c`) is the same for every phone: it takes the frames as MIPI RAW10 and makes the pictures iOS's capture graph is given.

It does nothing unless the boot-arg `mtkcam_skel=1` is given **and** the platform is complete. Every field that is still missing is said in the log
(`mtkcam-skel: the platform lacks: ...`) and no node is made. Nothing has a default: a black level, a pitch or a bayer order that is guessed gives
pictures that are wrong in ways that are hard to see.

It is not in `bsd/conf/files` and must not be built together with `redwood_mtkcam.c` (both make the same node). Add
`bsd/dev/redwood_mtkcam_skel.c standard` to `bsd/conf/files`, call `redwood_mtkcam_skel_init()` where `redwood_mtkcam_init()` is called
(`redwood_input.c`), and fill in `mtkcam_platform`. It compiles with the kernel's own flags (checked with the build's `xnu-clang` command line); it has not
run on a phone: there is no port for it to run on yet.

## The order to find things out in (each step needs the one before it)

1. **Power** (`power_on`/`power_off`). Which rails, clocks, power domain, reset and MCLK pins the camera needs, and the order Android uses. On the X6826B:
   the PMIC (MT6358) rails VCAMIO and VCAMD (and VCAMD's voltage: LK leaves one that Android changes; write it **and read it back**), a GPIO for the
   analog supply, the CAM power domain in the SPM and its bus protection, the camera clocks in topckgen/mmsys/camsys, reset and MCLK GPIOs. The loader's
   probe (hakios-mt6768's `mtkloader/mtkcam.h`, built with `CAM_PROBE=1`, started by `tools/cam-probe.sh`) reads pins, rails and the I2C controller and reports, then goes
   back to Android: it only reads. A PMIC wrapper (WACS2) is shared with the keys and the connectivity drivers here: take `redwood_pwrap_lock()` around
   every transaction (hakios-mt6768's `kernel/patches/redwood_pwrap_lock.patch`), or two drivers will interleave.
2. **The sensor** (`sensor_start`/`sensor_stop`). Its I2C address and bus, the chip-ID register and the expected ID (the X6826B's is 0x38e1; check yours
   against Android's own log first), then the register table Android writes between the ID read and the write that starts streaming.
   The table is the vendor's: `tools/camtable.py` makes it on your machine from a capture of Android bringing the camera up, and it is never in git. A
   sensor's I2C may be done by hand on its pins (bit-banged, as here on GPIO105/106) until the I2C controller's clocks are known.
3. **The receiver and the DMA** (`rx_start`/`rx_stop`). The CSI-2 receiver (SENINF on MediaTek) and the DMA that writes frames (CAMSV here): the registers
   Android has while the camera HAL streams, from a live dump (`tools/camrx.py` makes the table from it and the reset values; also vendor's, also not in
   git), and the order of writes. Queue the buffers the way the HAL does.
4. **What the DMA wrote** (`frame_done`, `read_lines`). Do not assume the layout. On the X6826B the DMA, programmed as the HAL does, writes **one byte a
   pixel** (the top 8 of the 10 bits), so the pitch is 4080 for a 4080-pixel line and not the 5100 bytes of MIPI RAW10; the partial frames seen before that
   was found were that mismatch (runs 496-499). Look at a line's bytes before you trust a stride (`tools/camthumb.py` makes a picture from the log's
   thumbnail lines). `read_lines` is where such a layout is made MIPI RAW10 (4 pixels in 5 bytes) for user space. Here a frame counts as done when the
   last 64 bytes of its buffer have been written, not from an interrupt. Map the frame memory cached and invalidate (`dc civac`) what you read; three
   buffers and a cached read-out are what ended a stall after the first three frames.
5. **Exposure** (`set_exposure`). The sensor's exposure, gain and frame-length registers (here 0x0202, 0x0204, 0x0340), the gain register's value for 1x
   (`gain_unity`; 0x100 here, assumed from the 8.8 format), and the most the sensor takes; the line time from the line-length register and the pixel clock.

## Checking it

- Each operation says (`say()`) what it is about to do **before** a step that could hang a bus; the last line of a log that ends is the step that hung.
- `camera_test` (`userland/src/camera_test.c`) reads frames from the node and checks them; `tools/camthumb.py` makes a picture of one.
- The iOS side needs the sensor to be known to Camera.app: its sensor ID in the device's `CameraSetup.plist`, module info for the capture graph; that is
  `userland/src/camera_device.c`'s part (its header says what is answered).
- A Camera.app icon on the home screen needs `still-camera` in SpringBoard's visible icon tags (`libredwood-sb`, see `camera-x6826b.md`).

## What was not found

Autofocus (the lens actuator's tables), the other cameras (their sensors' tables and power sequences), 10-bit data (the DMA's 10-bit mode gives garbage
on this chip), saving a photo from Camera.app's shutter and video are not done on the X6826B either.
