# Writing a driver

How a driver for one more part is written here, with an example that is built and tested by
`make check`: [examples/example_led.c](examples/example_led.c), a light on one of the SoC's pins, and
its test [examples/example_led_test.c](examples/example_led_test.c). Read the two beside this page and
copy them. [drivers.md](drivers.md) lists the drivers that exist; [porting.md](porting.md), step 6, says
when a phone needs a new one.

## The rules, and why

Each is one place in the example.

| A driver | Why | In the example |
| --- | --- | --- |
| starts only with its boot argument | A driver nobody asked for must not touch hardware. `led=0` and no argument are the same: nothing read, nothing said. | `example_led_init()`, 1 |
| starts only on a board that has the part | The kernel is one for every board. A part's driver asks `hakios_board_has(HAKIOS_HAS_…)`, and says which line of `board.toml` is missing when it is refused. | `example_led_init()`, 2 |
| takes addresses from the board | Where a block of the chip is comes from `hakios_board()`, never from a constant. A machine nobody described has no board (`hakios_board()` is NULL). | `ml_io_map(hakios_board()->pins.tlmm, …)` |
| has a level that only reads | The first run on a new phone should not write. Level 1 reads and logs; what it logs tells whether level 2 is safe. | `look()` |
| checks before it writes | What is there may not be what the driver expects: another part, a pin in use. Say so and leave it alone. | "is in use as function" |
| says what it does | `hakios_say()` puts a line in the loader's log at once, where it can be read while the phone runs, and in the kernel's. The lines are also what the test checks. | every `hakios_say()` |
| does not hold up the boot | Work that waits goes into a thread of the driver's own. | `kernel_thread_start()` |
| leaves the hardware as it found it | On the way back to the loader each driver undoes what it did. Nothing there sleeps or takes a lock. | `example_led_quiesce()` |

## What a driver has at hand

Headers in `kernel/overlay/bsd/dev/`. Each says in its first lines what it is for; each that touches
hardware has a test of its own in `tests/`.

| Header | What it gives | Where the data is |
| --- | --- | --- |
| `hakios_board.h` | `hakios_board()`: the board's row, with every address. `hakios_board_has()`: whether the board lists a feature. | The device package and the SoC file |
| `hakios_log.h` | `hakios_say()`: a line to both logs; to the loader's only where the kernel's may not be written. | — |
| `hakios_mmio.h` | `rd()`, `wr()`: 32-bit registers. `HAKIOS_MMIO_SYNC` when a write must have arrived before the next line, `HAKIOS_MMIO_EXACT` for what the hypervisor imitates. With `HAKIOS_MMIO_TEST` they are the test's. | — |
| `hakios_time.h` | `sleep_ms()`, `sleep_us()`, `spin_us()` (where nothing may sleep), `now_us()`. | — |
| `hakios_tlmm.h` | The SoC's pins: `gpio_configure()`, `gpio_output()`, `gpio_set()`, `gpio_get()`. | `[pins]` of the SoC file |
| `hakios_spmi.h` | The PMICs, through the arbiter: a channel's lookup, one read, one write, what a status means. Under `redwood_spmi_lock()`. | `[spmi]` |
| `hakios_cmd_db.h` | RPMh's resources by name: a supply rail and its levels, a regulator, a bus clock manager. | `[rpmh]` |
| `hakios_rpmh.h` | `hakios_rpmh_send()`: a request to RPMh and the wait for it. Under `redwood_rpmh_lock()`. | `[rpmh]` |
| `hakios_smem.h` | The memory the SoC's processors share: the partition of two processors, and its items by number. | `[ipc]` |

What is not there yet is written in the driver first, and moves to a header when a second driver
needs it.

## Steps

1. **Copy the example** to `kernel/overlay/bsd/dev/NAME.c`. Files named `hakios_*` work on any board
   from its data; a driver of one part is named after the part.
2. **Data.** A part gets a feature: a `HAKIOS_HAS_…` bit in `hakios_board.h` and its name in
   `tools/boards.py`. An address the driver needs that no section has yet becomes a key:
   [devices/README.md](../devices/README.md), "Adding a kind of data", has the seven places.
3. **Compile it** against an assembled tree, in a second, without the kernel toolchain:

   ```sh
   kernel/syntax-check.sh --tree ../xnu-12377-hakios bsd/dev/NAME.c
   ```

4. **Test it on the host.** Copy the example's test to `tests/NAME_driver_test.c` and add its name to
   the list in `tests/run.sh`. The test puts a model of the hardware behind `rd()` and `wr()`,
   includes the driver's own file, and runs one scenario a process. Models that exist:
   `tests/tlmm_model.h` (pins), `tests/spmi_model.h` (the PMIC arbiter), `tests/rsc_model.h` (RPMh's
   sequencer), `tests/cmd_db_image.h` (the command database). Write first the scenarios of what the
   driver must never do. Then break the driver on purpose, one line at a time, and see that a
   scenario fails each time: a test that passes whatever the driver does tests nothing.
5. **Add it to the build**: its line in `bsd/conf/files` of an assembled tree and its call at boot
   (most drivers are started from `redwood_input_init()` in `bsd/dev/redwood_input.c`), then
   `kernel/export.sh` ([kernel/README.md](../kernel/README.md)).
6. **Document it**: a comment in the file that names each boot argument as `name=…` (`make docs` puts
   them in [boot-args.md](boot-args.md)), and a row in [drivers.md](drivers.md).
7. **On the phone**: level 1 first. Send the log lines with the change, and say in the device
   package's README what was seen ([CONTRIBUTING.md](../CONTRIBUTING.md)).

## What the host test can and cannot show

It shows what the driver does with what it reads: which registers it touches, in which order, what it
refuses, that it cleans up. It runs the file the kernel compiles, on the host and as arm64 code.

It cannot show that the model is the hardware. A register layout taken from Linux's driver and a model
written from the same source agree with each other, not necessarily with the phone. That is what
level 1 on the phone is for.
