/*
 * A driver's access to memory-mapped registers: rd(address) and wr(address, value), 32 bits each,
 * on what ml_io_map() returned. One place for what every driver had a copy of, and for the three
 * ways an access is made. A driver says which it needs before it includes this file:
 *
 *   (nothing)                  a volatile load or store, as the compiler writes it. For registers
 *                              that are the hardware's own.
 *   #define HAKIOS_MMIO_SYNC   the same, and each write is followed by "dsb sy": the write has
 *                              reached the device before the next line runs. For sequences where
 *                              the order of a write and what follows it (a delay, a read of
 *                              another block) matters.
 *   #define HAKIOS_MMIO_EXACT  one access is one plain "ldr" or "str" of a general register, in a
 *                              function of its own. Some of what a phone's kernel touches (the
 *                              SMMU at least) is not the hardware but the hypervisor's imitation
 *                              of it: the access traps and is carried out on our behalf, which
 *                              works for the simplest instruction forms only, and the vendor's SMMU
 *                              driver warns that this imitation "goes horribly wrong" for a store
 *                              of the zero register. So the compiler does not get to choose the
 *                              instruction.
 *
 * A test of a driver on a host builds it with HAKIOS_MMIO_TEST defined and provides
 * hakios_test_rd() and hakios_test_wr(), behind which it puts a model of the registers
 * (workbench/README.md); the driver's code is the same either way.
 */
#ifndef HAKIOS_MMIO_H
#define HAKIOS_MMIO_H

#include <mach/mach_types.h>

#if defined(HAKIOS_MMIO_TEST)

extern uint32_t hakios_test_rd(vm_offset_t address);
extern void hakios_test_wr(vm_offset_t address, uint32_t value);
#define rd(a)           hakios_test_rd(a)
#define wr(a, v)        hakios_test_wr((a), (v))

#elif defined(HAKIOS_MMIO_EXACT)

static uint32_t __attribute__((noinline, unused))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline, unused))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

#else

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
#if defined(HAKIOS_MMIO_SYNC)
	__asm__ volatile ("dsb sy" ::: "memory");
#endif
}

#endif

#endif /* HAKIOS_MMIO_H */
