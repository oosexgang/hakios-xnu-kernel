/*
 * Copyright (c) 2016 Apple Inc. All rights reserved.
 */

#ifndef _PEXPERT_ARM_BCM2837_H
#define _PEXPERT_ARM_BCM2837_H

#define NO_MONITOR 1
#define NO_ECORE 1

#define BCM2837
#define BCM2837_BRINGUP
#define ARM_ARCH_TIMER
#define GENERIC_GIC_V3_IRQ                 1 /* interrupts via GICv3 Group 1, see osfmk/arm64/gic_v3_irq.c */
#define GENERIC_PSCI_SMP                  1 /* more cores are started through PSCI, see osfmk/arm64/psci_smp.c */

/*
 * The cores this kernel really runs on (Cortex-A55/A78, QEMU cortex-a76) are
 * ARMv8.2: they set PSTATE.PAN on every exception entry, so the kernel must
 * know to lift it around user memory accesses.
 */
#define __ARM_PAN_AVAILABLE__     1

#define __ARM_ARCH__              8
#define __ARM_VMSA__              8
#define __ARM_VFP__               4
#define __ARM_COHERENT_CACHE__    1
#define __ARM_DEBUG__             7
#define __ARM64_PMAP_SUBPAGE_L1__ 1

/*
 * Pointer authentication.  These cores are standard ARMv8.3+/ARMv9 ones, not Apple silicon: the
 * signing instructions are the architectural ones and the key registers (APIAKey_EL1 and the rest)
 * are shared by EL0 and EL1, so the kernel has to program them and switch them at the EL0 boundary
 * itself (osfmk/arm64/pac_asm.h, HAS_STD_PAC).  The rest of the kernel asks HAS_APPLE_PAC whether
 * the arm64e ABI is in use -- signed user thread state, per-process keys, signed credentials --
 * and that answer is yes here too, so it is defined.
 */
#if defined(__arm64e__)
#define HAS_APPLE_PAC             1
#define HAS_STD_PAC               1
#endif

/*
 * xnu-12377's apple_arm64_cpu.h (pulled in by sleh.c, pcb.c, machine_routines_common.c) reads the
 * MPIDR core-type field, defined per-board in the Apple apple_arm64_regs.h a non-Apple board does
 * not include.  A Cortex/QEMU core has one core type; report it as an E-core.
 */
#define MPIDR_CORETYPE_SHIFT  (16)
#define MPIDR_CORETYPE_WIDTH  (3)
#define MPIDR_CORETYPE_MASK   ((1ULL << MPIDR_CORETYPE_WIDTH) - 1)
#define MPIDR_CORETYPE_ACC_E  (0ULL)
#define MPIDR_CORETYPE_ACC_P  (1ULL)
#define MPIDR_CORETYPE_ACC_M  (4ULL)

#ifndef ASSEMBLER

#define PL011_UART                      /* pe_serial.c's PL011 driver: QEMU virt's console */

/*
 * No fixed PLATFORM_PANIC_LOG_PADDR: the Raspberry Pi address (0x3c0fc000) is
 * not RAM on other boards. The panic log region comes from the device tree
 * instead (/pram and /chosen/embedded-panic-log-size), like on Apple devices.
 */
#endif /* ! ASSEMBLER */

#endif /* ! _PEXPERT_ARM_BCM2837_H */
