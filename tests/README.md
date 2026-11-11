# Tests

Everything here runs on the machine you build on: no phone, no iOS files, no kernel toolchain.

```sh
make check          # the lint, the board files, the generated reference, and tests/run.sh
tests/run.sh        # the tests alone
```

They need a C compiler, Python 3.11 or newer and git. With `gcc-aarch64-linux-gnu` and `qemu-user`
the driver tests are also built for arm64 and run under `qemu-aarch64`; with `clang` and `lld` the
loader is built. What is not installed is left out, and said.

## What is tested

| Test | What it runs | What it shows |
| --- | --- | --- |
| `board_test.c` | The kernel's board code (`hakios_board.c`) with a table generated for the tests | The row is found by the loader's name; an unknown board has no feature; `hw.board.*`; the GPU profile a driver is handed. |
| `backlight_driver_test.c` | The backlight driver, unchanged, against a model of a DSI controller and a panel | What is written to the registers and when: both panel modes, the read-only level, Auto-Brightness, a controller that does not answer. |
| `thermal_driver_test.c` | The thermal limiter, unchanged, against models of the sensors and the CPU frequency block | Heating and cooling, the critical temperature, sensors that fall silent, tuning while running. |
| `spmi_test.c` | What the drivers of PMICs share (`hakios_spmi.h`), by itself, against the model of the arbiter | The arbiter is mapped where the SoC file says and for the system it names; what a read and a write put on the bus; what each status is said to mean (refused, nobody there, dropped, no answer); a board nobody described has no arbiter and nothing is touched. |
| `cmd_db_test.c` | The lookup in RPMh's command database that the drivers share (`hakios_cmd_db.h`), against databases built in the test | A resource is found by its whole name, among one kind or any; its own data; names of eight letters; a database that says wrong things of itself (entries or data outside what was mapped, a count nobody believes) is not followed there. |
| `cpufreq_driver_test.c` | The CPU frequency driver, unchanged, against a model of the EPSS block | Every group with a core of the kernel's is asked for the top row of its own table, a group without one is left alone; a chip with two groups at another address; tables of one row and of every row; nothing written without the boot argument or the feature. |
| `buttons_driver_test.c` | The buttons' driver, unchanged, against a model of the PMIC arbiter and the two peripherals the keys are on | Each key is read where the board says it is, on two different wirings; a key held at the start, a part of another kind, an arbiter of another version; that the driver writes nothing but read commands. |
| `torch_driver_test.c` | The flashlight's driver, unchanged, against the same arbiter with a flash LED module and the PMIC's temperature alarm behind it | What a driver that writes to a PMIC must never do: a write outside the module's six registers, a current above Android's, a light left on after a fault, past its time limit or at the run's end. Also `/dev/redwood-torch` as iOS uses it, and the same part on another chip. |
| `battery_driver_test.c` | The battery's driver, unchanged, against the page of memory the loader publishes the firmware's readings in | The readings reach the sysctls and a change is followed; an update caught half written, a charge that cannot be, a page that is not the loader's are not taken; what the audio DSP reports instead is handed on. |
| `backlight_test.c`, `thermal_test.c` | The arithmetic of both drivers (`hakios_*_core.h`) | Curves, ranges and steps, over every input. |
| `boards_test.py` | `tools/boards.py` and `tools/devicetree.py` | What a board file may say and what is refused, with the reason; what is read from a device tree; where packages are found; the tables written. |
| `device_test.py` | `tools/device.py`, in a repository of its own | A package is brought in from a branch or a directory, listed, exported and removed; work that is not the tool's is left alone. |
| `porting_test.sh` | `docs/porting.md`, on a made-up phone with a made-up SoC | The guide's tool and data steps work for a device nobody has described, with none of the addresses of the phones the code was written on. |
| `kernel_tools_test.sh` | `kernel/assemble.sh`, `export.sh`, `verify.sh`, `import-patch.sh`, `syntax-check.sh`, on a small made-up release | A tree is what the repository says; work in a tree is never run over; an export brings it back. |
| `bootargs_test.py` | `tools/bootargs.py` | Every boot argument the kernel reads is found and described; `docs/boot-args.md` is current. |
| `tools_test.py` | `tools/check-links.py`, `tools/components.py` | The repository's own checks find what they are for. |

## How a driver runs without a kernel

`fakekernel/` has just enough of XNU's interfaces for a driver's own source file to be compiled as a
host program: threads are pthreads, the clock is the host's, sysctls go into a table the test reads
and writes, boot arguments are a string the test sets. A driver reaches hardware through one seam: its `rd()` and
`wr()` are `hakios_mmio.h`'s, and built with `HAKIOS_MMIO_TEST` they are the test's `hakios_test_rd()`
and `hakios_test_wr()`, behind which the test puts a model of the registers. The model of the PMIC arbiter is shared (`tests/spmi_model.h`), and a driver's
`/dev` node is read and written by its name (`fake_dev_read()`, `fake_dev_write()`). The file that is
tested is the file the kernel compiles.

A scenario is a process: a driver looks its board up once and starts its threads once, as in the
kernel. Run without an argument a test lists its scenarios; `tests/run.sh` runs each.

## The tests' phones

The repository describes no phone, so the tests have their own in `tests/devices`:

| Board | What it stands for |
| --- | --- |
| `phone-cmd` | A phone with a command-mode panel on an SM7350, with the keys, the audio DSP, a GPU profile and boot defaults. |
| `phone-video` | A phone with a video-mode panel on an SM7325, every part driver listed, known to the loader by its framebuffer. |
| `phone-keys` | A phone wired unlike the real ones: its keys on other PMIC peripherals, behind an arbiter at other addresses that knows this system as its third, on a made-up chip (`tests/soc`). What is still written into a driver shows here. |

The first two carry the values of real phones, so that a test computes what a phone computes; their
names are not real, so that a real package beside them never collides. `tests/run.sh` generates the tables the
tests are built with from these and the repository's own boards.

## Adding a test

- **For a driver:** keep its arithmetic in a header of its own and test that directly; put hardware
  behind one seam and model it. `backlight_driver_test.c` is the example.
- **For a tool:** a small repository or directory of its own for each test, never the checkout
  (`boards_test.py`: `Sandbox`).
- A test says what it is for in its first lines, and a check says what was expected when it fails.
- Add it to `tests/run.sh`.
