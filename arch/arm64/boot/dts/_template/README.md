# MAKER MODEL (`NAME`)

| | |
| --- | --- |
| Device | MAKER MODEL |
| SoC | CHIP (SOC); `soc.dtsi` |
| State | One sentence: how far it boots, with which Darwin build, and when that was last seen on the phone. |

Nothing in the procedure writes to the phone's storage. It is started with `fastboot boot` and runs
from RAM; a reboot brings Android back. It needs an unlocked bootloader.

## State

Say what was seen on the phone and what was not. "Compiles", "passes the host tests" and "seen working
on the phone" are three different statements: give the date and the Darwin build for the last one.

| What | Feature / boot argument | State |
| --- | --- | --- |
| Boot to the home screen | — | |
| Touch | | |
| Buttons | `buttons-pmic` / `buttons=1` | |
| CPU frequency | `cpufreq-epss` / `cpufreq=1` | |
| Brightness | `backlight { }` / `backlight=` | |
| Temperature | `thermal { }` / `thermal=` | |
| GPU | `gpu-a660` / `gpu=1` | |
| Sound, camera, Wi-Fi, haptics | | |

`tools/boards.py status NAME` on the main branch lists every feature with what it still needs.

## Where the values come from

For each section of `board.dts`: the node of the Android device tree, the file of the vendor's kernel
or of Linux, or what was measured on the phone. The device tree itself is never committed.

## What a boot needs besides this package

The UEFI port for the phone, the firmware files taken from the same phone, and anything that is not
published. Say plainly whether somebody else can reproduce the boot from this package and the main
branch alone.

## Notes

What the next person should know: what was tried and did not work, what is left alone on purpose.
