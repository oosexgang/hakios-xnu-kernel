# Thermal limiter

The phone heats up: before graphics were removed, a GPU sensor read 69.1 °C (`ios26-boot.md`, item 14), and
`cpufreq=1` runs every core group at its top row with nothing holding it back. The kernel side is
`kernel/overlay/bsd/dev/hakios_thermal.c` (the arithmetic is in
`hakios_thermal_core.h`). It is device-independent: a board needs one line with its sensor and frequency
controller addresses.

## What it does

Once a second (`th_period_ms`), it reads every sensor of both TSENS controllers that has a reading. It never
writes to them, and it ignores implausible readings (below −40 °C or above 150 °C). Then, from the hottest reading:

| Hottest | Processors (each EPSS group's frequency row) | Panel |
| --- | --- | --- |
| ≥ `th_trip` (70.0 °C) | one row lower each second, down to the group's floor (1.0 GHz for the small cores, 1.1 GHz for the big ones) | capped, linearly down to 60 % |
| between `th_clear` and `th_trip` | stays where it is (hysteresis) | stays |
| ≤ `th_clear` (62.0 °C) | one row back each second | the cap rises with it |
| ≥ `th_critical` (95.0 °C) | the floor at once | 60 % |
| no reading at all | given back as if cool: a silent sensor is no reason to stay slow; the hardware's own reset still guards the critical point | |

The limiter never raises a group above the row it found at start: without `cpufreq=1` the groups stay at their
300 MHz and it never writes. At the end of the run the rows are restored, so the loader is not left slow.

The panel's cap goes to `hakios_backlight_set_cap()` (`renoir/docs/backlight.md`, `hw.backlight.cap`): an AMOLED
at full white is heat as well, and an iPhone dims when hot. The GPU's speed is not changed here, because it
belongs to its GMU (`adreno_speed`). `hakios_thermal_step()` exposes how far things are held back, for when the
GPU driver wants to follow.

## Boot arguments and sysctl

| Boot argument | Default (`boot26.py`) | |
| --- | --- | --- |
| `thermal=0/1/2` | 0 (1) | off; read and log the hottest sensor every 10 s; hold back |
| `th_trip=`, `th_clear=`, `th_critical=` | 700, 620, 950 | tenths of a degree; must satisfy clear < trip < critical, otherwise the defaults are used |
| `th_period_ms=` | 1000 | 100 to 10000 |

`sysctl hw.thermal`: `hottest` (tenths of a degree), `step`, `most`, and `trip`/`clear`/`critical`, which root can
change while running, for tuning without a new kernel.

## Profiles

`renoir` (SM7350) and `redwood` (SM7325), both lahaina family:
- TSENS `tsens_tm_physical` at 0x0c263000 and 0x0c265000;
- EPSS at 0x18591000, three groups of 0x1000;
- floors 1000/1100/1100 MHz.

To add a device, take those addresses from its device tree.

## Testing

`tests/run.sh` runs:
- `thermal_test.c`: the arithmetic (sensor word, hottest reading, steps, hysteresis, rows, panel cap).
- `thermal_driver_test.c`: the driver itself, unchanged, against a model of two TSENS controllers and three EPSS
  groups. Scenarios:
  - off by default
  - looking only
  - heating then cooling, row by row, with the panel cap
  - hysteresis
  - critical, with restore at the run's end
  - never raising when `cpufreq` is off
  - silent sensors
  - tuning at run time
  - an unknown board
  - a contradictory policy

The tests run on the host and under qemu-aarch64.

On the phone:
1. `boot26.py` (`thermal=1`): the log gets `thermal: hottest sensor NN.N C` every ten seconds. These are the first
   numbers to compare with Android's readings for the same sensors.
2. `--boot-arg thermal=2`: under load (`cpuload=1000`), `thermal: ... held back N of M` should appear around
   70 °C, and the temperature should level off.
