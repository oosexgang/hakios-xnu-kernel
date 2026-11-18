/*
 * The processor's data caches and memory that another master reads or writes past them: a DSP, the
 * GPU, the camera's IFE, the loader on another core. By lines of 64 bytes, the smallest of the cores
 * these SoCs have; each is followed by a barrier, so that what follows comes after it. And that
 * barrier alone: every access before it has reached its device, or memory, before anything after it.
 *
 * On a host (the tests, workbench/fakekernel) there is no other master: only the compiler is held.
 */
#ifndef _HAKIOS_CACHE_H
#define _HAKIOS_CACHE_H

#include <stdint.h>

#define HAKIOS_CACHE_LINE       64u

#if defined(__arm64__) || defined(__aarch64__)
#define HAKIOS_CACHE_LINES(op, start, end) do { \
	for (uintptr_t line = (start) & ~(uintptr_t)(HAKIOS_CACHE_LINE - 1); line < (end); line += HAKIOS_CACHE_LINE) { \
		__asm__ volatile ("dc " op ", %0" : : "r" (line) : "memory"); \
	} \
	__asm__ volatile ("dsb sy" ::: "memory"); \
} while (0)
#else
#define HAKIOS_CACHE_LINES(op, start, end) do { (void)(start); (void)(end); __asm__ volatile ("" ::: "memory"); } while (0)
#endif

/* every access before this has reached its device, or memory, before anything after it */
static inline void
hakios_barrier(void)
{
#if defined(__arm64__) || defined(__aarch64__)
	__asm__ volatile ("dsb sy" ::: "memory");
#else
	__asm__ volatile ("" ::: "memory");
#endif
}

/* [start, end) written by the processor, for another master to read: out of the caches to memory */
static inline void
hakios_cache_clean(uintptr_t start, uintptr_t end)
{
	HAKIOS_CACHE_LINES("cvac", start, end);
}

/* [start, end) shared both ways: out to memory and dropped, so that what another master wrote is read afresh */
static inline void
hakios_cache_flush(uintptr_t start, uintptr_t end)
{
	HAKIOS_CACHE_LINES("civac", start, end);
}

/* [start, end) written by another master, to be read: dropped from the caches without being written out */
static inline void
hakios_cache_invalidate(uintptr_t start, uintptr_t end)
{
#if defined(FAKEKERNEL_H)
	/* (the tests, on an aarch64 host, run at EL0, where an invalidate may only come with a clean: ivac traps) */
	HAKIOS_CACHE_LINES("civac", start, end);
#else
	HAKIOS_CACHE_LINES("ivac", start, end);
#endif
}

#endif /* _HAKIOS_CACHE_H */
