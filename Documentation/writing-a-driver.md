# Writing a driver

How a driver for one more part is written here, with an example that is built and tested by
`make check`: [examples/example_led.c](examples/example_led.c), a light on one of the SoC's pins, and
its test [examples/example_led_test.c](examples/example_led_test.c). Read the two beside this page and
copy them. [drivers.md](drivers.md) lists the drivers that exist; [porting.md](porting.md), step 6, says
when a phone needs a new one.

## The rules, and why

Each is one place in the example, but the one about waiting.

| A driver | Why | In the example |
| --- | --- | --- |
| starts only with its boot argument | A driver nobody asked for must not touch hardware. `led=0` and no argument are the same: nothing read, nothing said. | `example_led_init()`, 1 |
| starts only on a board that has the part | The kernel is one for every board. A part's driver asks `hakios_board_has(HAKIOS_HAS_…)`, and says which line of `board.dts` is missing when it is refused. | `example_led_init()`, 2 |
| takes addresses from the board | Where a block of the chip is comes from `hakios_board()`, never from a constant. A machine nobody described has no board (`hakios_board()` is NULL). | `ml_io_map(hakios_board()->pins.tlmm, …)` |
| has a level that only reads | The first run on a new phone should not write. Level 1 reads and logs; what it logs tells whether level 2 is safe. | `look()` |
| checks before it writes | What is there may not be what the driver expects: another part, a pin in use. Say so and leave it alone. | "is in use as function" |
| says what it does | `hakios_say()` puts a line in the loader's log at once, where it can be read while the phone runs, and in the kernel's. The lines are also what the test checks. | every `hakios_say()` |
| does not hold up the boot | Work that waits goes into a thread of the driver's own. | `kernel_thread_start()` |
| declares its wait, then looks again | A thread that sleeps until it is poked reads its state, then sleeps. A poke between the two wakes nobody. So: `assert_wait()`, look once more at what the poke would have changed, `clear_wait()` and go round if it did, and only then `thread_block()`. A wait with a time (`assert_wait_timeout()`) is late at worst; one without sleeps on. | not in the example; `board/backlight.c` (`pokes`), `board/lx.c` |
| leaves the hardware as it found it | On the way back to the loader each driver undoes what it did. Nothing there sleeps or takes a lock. | `example_led_quiesce()` |

## What a driver has at hand

Headers in `drivers/board/`. Each says in its first lines what it is for; each that touches
hardware has a test of its own in `workbench/`.

| Header | What it gives | Where the data is |
| --- | --- | --- |
| `board/board.h` | `hakios_board()`: the board's row, with every address. `hakios_board_has()`: whether the board lists a feature. | The device package and the SoC file |
| `board/log.h` | `hakios_say()`: a line to both logs; to the loader's only where the kernel's may not be written. | — |
| `board/mmio.h` | `rd()`, `wr()`: 32-bit registers. `HAKIOS_MMIO_SYNC` when a write must have arrived before the next line, `HAKIOS_MMIO_EXACT` for what the hypervisor imitates. With `HAKIOS_MMIO_TEST` they are the test's. | — |
| `board/time.h` | `sleep_ms()`, `sleep_us()`, `spin_us()` (where nothing may sleep), `now_us()`. | — |
| `board/kernel.h` | What of the kernel its headers do not offer to `bsd/dev`: `ml_io_map()`, `kvtophys()`, `thread_terminate()`, the loader's log, the RAM disk. | — |
| `board/cache.h` | Memory another master reads or writes past the caches: `hakios_cache_clean()`, `hakios_cache_flush()`, `hakios_cache_invalidate()`; `hakios_barrier()`. On a host, only the compiler is held. | — |
| `board/tlmm.h` | The SoC's pins: `gpio_configure()`, `gpio_output()`, `gpio_set()`, `gpio_get()`. | `pins { }` of the SoC file |
| `board/i2c_gpio.h` | I2C by hand on two of those pins, for a part on a bus nobody here drives: the bus taken and given back, a start, a byte out, a byte in. | — |
| `board/spmi.h` | The PMICs, through the arbiter: a channel's lookup, one read, one write, what a status means. Under `hakios_spmi_lock()`. | `spmi { }` |
| `board/cmd_db.h` | RPMh's resources by name: a supply rail and its levels, a regulator, a bus clock manager. | `rpmh { }` |
| `board/rpmh.h` | `hakios_rpmh_send()`: a request to RPMh and the wait for it. Under `hakios_rpmh_lock()`. | `rpmh { }` |
| `board/smem.h` | The memory the SoC's processors share: the partition of two processors, and its items by number. | `ipc { }` |
| `board/ipc.h` | The registers of the always-on processor's mailbox and of the doorbells between the processors, and whether the mailbox it gives is one a message fits in. | `ipc { }` |
| `board/smp2p.h` | An SMP2P item's shape, and a word of it by name: how two processors tell each other their state. | — |
| `board/glink_queue.h` | The two queues GLINK runs over in that memory: the arithmetic of their marks, the messages' commands and how long each message is. | — |
| `board/mdt.h` | A processor's firmware image as Qualcomm splits it: the header file checked, its segments, which are loaded and whether each lies in the processor's memory. | The board's `adsp { }`, `wifi { }` (the memory) |
| `board/smmu.h` | The SMMU's registers (an MMU-500): stream matching, context banks, the translation settings. | `smmu { }` (where it is, and the parts' streams) |
| `board/scm.h` | `hakios_scm_call()`: a call to TrustZone, made again while it is preempted; the peripheral authentication service's calls. | — |
| `board/qrtr_frame.h` | QRTR's datagrams as they arrive over GLINK: the header in either version, and where the datagram's bytes are. | — |

What is not there yet is written in the driver first, and moves to a header when a second driver
needs it.

## Steps

1. **Copy the example** to `drivers/board/NAME.c`. A driver works on any board from its
   data; one of a single part is named after the part. `scripts/new-driver.py NAME` does this step and the
   mechanical half of step 4 -- the test copied, in `workbench/run.sh`'s list, the part in `MAINTAINERS` -- and
   prints the rest.
2. **Data.** A part gets a feature: a `HAKIOS_HAS_…` bit in `board/board.h` and its name in
   `tools/boards.py`. An address the driver needs that no section has yet becomes a key:
   [arch/arm64/boot/dts/README.md](../arch/arm64/boot/dts/README.md), "Adding a kind of data", has the seven places.
3. **Compile it** against an assembled tree, in a second, without the kernel toolchain:

   ```sh
   kernel/syntax-check.sh --tree ../xnu-12377-hakios bsd/dev/NAME.c
   ```

4. **Test it on the host.** Copy the example's test to `workbench/NAME_driver_test.c` and add its name to
   the list in `workbench/run.sh`. The test puts a model of the hardware behind `rd()` and `wr()`,
   includes the driver's own file, and runs one scenario a process. Models that exist:
   `workbench/tlmm_model.h` (pins), `workbench/spmi_model.h` (the PMIC arbiter, which can also drop
   transfers), `workbench/i2c_device_model.h` (a chip on a bus of two pins), `workbench/rsc_model.h` (RPMh's
   sequencer), `workbench/cmd_db_image.h` (the command database). Write first the scenarios of what the
   driver must never do. Then break the driver on purpose, one line at a time, and see that a
   scenario fails each time (`workbench/mutate.sh` does it in a copy and says which scenario caught the
   change; `workbench/mutants.py TEST SOURCE` makes the simple changes itself and prints those nothing
   caught): a test that passes whatever the driver does tests nothing. On the host the tests run under
   the address and undefined-behaviour sanitizers: a read past a table, or an unaligned load, fails.
5. **Add it to the build**: its line in `bsd/conf/files` of an assembled tree, and its start function
   as one more line of the list in `bsd/dev/board/drivers.c`, after what it needs (the list is
   where every driver is started, in order; a driver decides for itself whether it has anything
   to do). Then `kernel/export.sh` ([kernel/README.md](../kernel/README.md)). That it compiles and
   links in the whole kernel is a build away, on any Linux machine, without theos: with LLVM's tools
   ([building.md](building.md)) a build from scratch takes minutes, and `XNU_WARNINGS=1` shows what the
   compiler says of the file (HakiOS' own files give no warning; keep it so). `make analyze-kernel`
   runs clang's static analyzer over them and lists what it finds in HakiOS' lines: a value used
   before it is set, a pointer that may be null, a division that may be by zero. It does not know
   that `uiomove()` fills what it is given, and says so of every read; the rest is worth a look.
6. **Document it**: a comment in the file that names each boot argument as `name=…` (`make docs` puts
   them in [boot-args.md](boot-args.md)), and a row in [drivers.md](drivers.md). A device node's
   interface in `bsd/sys/board/*.h` that the Darwin side compiles against has a copy in
   `userland/userland/src/board/`: change both (`make lint` holds what programs see of them to be the same).
7. **On the phone**: level 1 first. Send the log lines with the change, and say in the device
   package's README what was seen ([CONTRIBUTING.md](../CONTRIBUTING.md)).

## What the host test can and cannot show

It shows what the driver does with what it reads: which registers it touches, in which order, what it
refuses, that it cleans up. It runs the file the kernel compiles, on the host and as arm64 code.

It cannot show that the model is the hardware. A register layout taken from Linux's driver and a model
written from the same source agree with each other, not necessarily with the phone. That is what
level 1 on the phone is for.
