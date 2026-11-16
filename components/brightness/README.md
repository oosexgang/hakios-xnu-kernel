# Brightness

The screen's brightness: Control Center's and Settings' slider, Auto-Brightness, and the display modes
(on, dimmed, off) Darwin takes the panel through. On an iPhone CoreBrightness talks to the display
driver; this phone's panel is driven by the kernel ([`Documentation/backlight.md`](../../Documentation/backlight.md)).

| Where | Inside | What it does |
|---|---|---|
| `inject_brightness.c` | `backboardd` and the slider's processes | The slider and Auto-Brightness to the kernel's `hw.backlight.*` sysctls, for any device. |
| `inject_backlight.c` | `SpringBoard` | Tells BacklightServices that a display mode was reached once its ramp is over, as a display driver would: without it Darwin 27's backlight watchdog aborts SpringBoard again and again, and the screen stays black. |

## In the kernel

[`kernel/overlay/bsd/dev/board/backlight.c`](../../kernel/overlay/bsd/dev/board/backlight.c) drives the
panel's brightness (the panel's own DCS commands, as the board file says, or a dimming in software), with the curve of
`backlight_core.h`; the ambient light sensor's daemon and the thermal limiter's cap act through it.
`darwin/userland/inj26.sh` builds both libraries, `darwin/userland/root26-darwin.sh` puts them in.
