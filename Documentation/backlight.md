# Brightness and Auto-Brightness

The panel's brightness and automatic brightness, for any device. Kernel: `drivers/board/backlight.c`
(`bsd/dev/board/backlight.c`, the arithmetic in `bsd/dev/board/backlight_core.h`). Darwin side:
`components/brightness/inject_brightness.c` (`libhakios-brightness.dylib`).

## How it fits together

```
Control Center / Settings slider ──┐  BrightnessSystemClient -setProperty:forKey:
Auto-Brightness switch ────────────┤  ("DisplayBrightness", "DisplayBrightnessAuto")
                                   ▼
        libhakios-brightness.dylib (SpringBoard, Preferences, backboardd)
                                   │  sysctl hw.backlight.level / hw.backlight.auto
                                   ▼
        kernel: hakios_backlight ── thread "backlight", 20 steps a second while it moves
              │          ▲
              │          └── light readings: hw.backlight.lux (virtual) or hakios_als_report() (a sensor driver)
              ▼
     backend dcs:  MIPI DCS 0x51 through the Qualcomm DSI controller's command FIFO
     backend soft: the display driver darkens each pixel on its way to the panel (QEMU, unknown panels)
```

The display mode follows Darwin too. `libhakios-backlight.dylib` already completes SpringBoard's display-mode
transitions; it now also looks up Darwin's own name for each mode, using the function behind the watchdog's
"ActiveOn":
- a mode with "Off" in its name makes the panel dark once the ramp is over;
- a mode with "Dim" in its name dims it;
- any other mode lights it at once.

So the side button really turns the panel dark (DCS brightness 0 is black on AMOLED; on the `soft` backend
every pixel is black), and a tap or the button lights it again at the slider's level.

CoreBrightness has no panel it recognises here, so the library takes the two properties in `setProperty:forKey:`
and answers them in `copyPropertyForKey:`. The kernel is the single source of truth: every process sees the same
values, and the slider follows the light when brightness is automatic.

## Boot arguments

| Boot argument | Default | Meaning |
| --- | --- | --- |
| `backlight=0/1/2` | 0 (`boot26.py`: 1) | 0 off; 1 read the DSI controller and log it, write nothing; 2 control the panel |
| `bl_level=N` | 600 | Brightness at boot, 0 to 1000 |
| `bl_auto=1` | 0 | Automatic brightness from boot |
| `bl_backend=dcs/soft/none` | per board | Overrides the board profile |
| `bl_dsi=0x...` | per board | Physical address of the DSI controller |
| `bl_min=`, `bl_max=` | per board | Panel values for the lowest and highest brightness |
| `bl_bytes=1/2`, `bl_le=1` | per board | Width of the 0x51 parameter; `bl_le=1` sends the least significant byte first |
| `bl_floor=N` | 24 | `soft` backend: share of each pixel kept at level 0, out of 256 |
| `bl_trace=1` | 0 | The library logs every CoreBrightness key it sees (to find the keys of a newer Darwin) |

## sysctl `hw.backlight`

| Name | Access | |
| --- | --- | --- |
| `level` | any user, rw | Slider position, 0 to 1000. Written while brightness is automatic, it shifts the curve by the difference |
| `auto` | any user, rw | Automatic brightness |
| `power` | any user, rw | Display: 1 on, 0 off (panel dark), 2 dimmed (at most level 150). `inject_backlight.c` sets it from Darwin's display modes |
| `lux` | any user, rw | Ambient light in thousandths of a lux. Writing it feeds the "virtual" sensor |
| `shown` | ro | Level on the panel now (it ramps towards the target) |
| `raw` | ro | Last value sent to the panel (`dcs`) |
| `offset` | ro | User's shift of the automatic curve |
| `backend`, `als` | ro | `dcs`/`soft`/`none`; source of the light readings |

## Automatic brightness

The curve maps log2(1 + lux) to a level: 0 lux → 60, 10 → 220, 100 → 410, 300 (an office) → 530,
1000 → 670, 10 000 → 930, 30 000 lux and above → 1000. Changes of less than about a fifth (a quarter
of a stop) are ignored. The light brightens within about a second and darkens within about four. If the user
moves the slider while brightness is automatic, the curve keeps that offset, as on an iPhone. Turning automatic
brightness off resets it.

Light readings: until a sensor driver reports through `hakios_als_report(millilux, "name")`, the light comes from
`hw.backlight.lux` (default 300 lux). On the phones so far the ambient light sensor sits behind the sensor DSP
(SSC, QMI), so it needs a QMI sensor client. That driver feeds this path when it lands, and nothing else changes.

## Device data

The driver has no table of its own. It reads the board's row (`hakios_board()`), which comes from
`backlight { }` in the board file of the phone's device package (`arch/arm64/boot/dts/NAME/board.dts` once
`tools/device.py add NAME` has brought it in) and `clocks { }` and `display { }` in the SoC's file:

| Board | Panel | Backend | DSI | Range | Order | Mode |
| --- | --- | --- | --- | --- | --- | --- |
| a phone's package | its panel | dcs or soft | its SoC's | its panel's | its panel's | command or video |
| `qemu-virt` | framebuffer | soft | — | — | — | — |
| anything else | — | soft | — | — | — | — |

Each phone's row is its package's, kept on the `devices` branch, whose README says which panel it has. A
kernel has a phone's row only when it was built with the package present.

Adding a device is a `backlight { }` section in its board file. `tools/boards.py new --dts` fills it in
from the Android device tree:
- `mdss_dsi0` `reg` "dsi_ctrl" gives the DSI address (the SoC file's `display { }`).
- The panel node gives `qcom,mdss-dsi-bl-min-level` and `-max-level`.
- `qcom,mdss-dsi-bl-inverted-dbv` present means MSB first.
- `qcom,mdss-brightness-init-level` is the level left for the loader.

The display clock controller's MDSS GDSC and `mdss_ahb` offsets are not in a device tree. They come from
Linux `dispcc-*.c` and go in the SoC file's `clocks { }`.

To try values before writing them down, pass the `bl_*` boot arguments. A board with no description at
all gets the `soft` backend.

### The DCS path, step by step

This mirrors Linux's SDE driver in its secure-session mode, which cannot use memory:

1. Clock chain, as for the display scan: GCC `disp_ahb`, MDSS GDSC on, `mdss_ahb` on.
2. Wait for the right moment: video mode waits for `VIDEO_DONE`; command mode waits until `DSI_STATUS` shows the MDP idle.
3. Enable the command engine (`DSI_CTRL` bit 2) if it is off.
4. Load the command: `TEST_PATTERN_GEN_CTRL` = TPG + DMA FIFO, the packet words into `CMD_DMA_INIT_VAL` (an even count), embedded mode in `DMA_CTRL`, the length, `SW_TRIGGER`.
5. Wait up to 20 ms for `CMD_MODE_DMA_DONE`. If it does not arrive, stop using DCS for the rest of the run.
6. Clean up: TPG off, FIFO reset, `DMA_CTRL` and `DSI_CTRL` restored.

When the kernel returns to the loader, the panel goes back to the profile's init level.

## Testing

`workbench/run.sh` covers three things:
- `backlight_test.c`: the arithmetic (10k checks).
- `backlight_driver_test.c`: the driver itself, unchanged, on stubs of the kernel interfaces (`workbench/fakekernel`), against a model of the DSI controller and the display clocks. The model rejects anything outside the protocol: a trigger without the command engine, the FIFO written with the TPG off, a length that does not match the words, an access to a register the driver has no business with.
- The scenarios:
  - a command-mode and a video-mode panel, on two of the tests' stand-in phones (`workbench/devices/`)
  - `backlight=1` reads only
  - off by default
  - an unpowered display block
  - a controller that never reports done
  - the `soft` backend on an unknown board
  - automatic brightness: dark room, sun, ignored flicker, office, user offset, back to manual
  - display power: dark, slider moved while dark, lit at the new level, dimmed, the loader gets a lit panel; the same on `soft`

The driver test runs twice: built for the host, and for aarch64 under `qemu-aarch64`, so the arm64 code runs
with its barriers. The kernel sources were also checked with clang against xnu-12377's headers: no errors, no new
warnings.

On the phone, in order:
1. `boot26.py` as usual (`backlight=1`). The loader log should have a line like
   `backlight: dcs: DSI controller at 0xae94000: version 2.5 ... (on, command)`.
2. `--boot-arg backlight=2`. Move the Control Center slider: the panel should follow, and
   `backlight: dcs: ... NOT SENT` must not appear.
3. Press the side button: the panel should go dark, and light again on wake (`--boot-arg bl_trace=1` logs the
   mode names).
4. To check on the first phone boot with `bl_trace=1`: whether backboardd's own CoreBrightness calls
   `setProperty:DisplayBrightness` by itself (for example, a stored value at start). Such a call is passed to the
   kernel like the slider's, and with Auto-Brightness on it would move the curve. If the log shows it, drop the
   server-side hook (`SERVER` in `inject_brightness.c`) and keep the client side.
5. `--boot-arg bl_auto=1`, then for example `sysctl hw.backlight.lux=0` and `=30000000` over SSH, to watch the ramps.
