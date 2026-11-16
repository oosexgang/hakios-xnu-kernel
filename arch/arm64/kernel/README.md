# arch/arm64/kernel

What the kernel needs to run on an arm64 machine that is not Apple's: everything here is `osfmk/arm64/` of the
kernel's tree, where `kernel/assemble.sh` puts it (`kernel/lib.sh`, `OVERLAY_PLACES`) and from where
`kernel/export.sh` takes it back. Edit it here or in an assembled tree, as with `kernel/overlay`.

| File | What |
|---|---|
| `psci_smp.c`, `psci_smp.h` | The other processors, started and stopped through the firmware's PSCI calls, where Apple's closed drivers would. |
| `gic_v3_irq.c` | The interrupt controller (GICv3): the open kernel has no driver for one. |
| `cpu_idle.c` | How long each processor has been idle, and which cluster it is in: for the CPU frequency governor and the parking of the small cores. |
| `a64emu.c` | The instructions of arm64e user code an ARMv8.2 core (Cortex-A78, A55) does not have, done when they trap. |
| `generic_arm64_support.c`, `kernelsupport.c` | What the open tree references but defines only for Apple's SoCs, or moved into Apple's closed per-SoC library (xnu-12377). |
| `pac_std_asm.h`, `ppl/`, `tunables/` | The headers and tables a machine without pointer authentication and without a monitor (PPL) still needs. |
| `loader_return.c` | The way back to the resident loader, with the kernel's log, on a phone without a serial port. |
| `prof.c` | A sampling profiler for one process (boot-arg `hakios_prof=NAME`). |
| `user_exception_trace.c` | Every fatal-looking exception of a user thread, described (boot-arg `systrace`). |
