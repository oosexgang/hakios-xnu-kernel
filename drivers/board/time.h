/*
 * The few ways a driver waits and tells the time, which every driver had copies of.
 *
 *   sleep_us(), sleep_ms()     wait and let others run: from a driver's own thread only
 *   spin_us()                  wait without sleeping: also with interrupts off, and on a run's way
 *                              out when the other cores are stopped. For microseconds, not more.
 *   hakios_wait_ms()           milliseconds, slept -- or spun, on a run's way out (spin: true)
 *   now_us()                   microseconds since the kernel's clock began
 *
 * A wait in a bring-up sequence is part of the part's data sheet: say in a comment where the
 * number is from.
 */
#ifndef HAKIOS_TIME_H
#define HAKIOS_TIME_H

#include <kern/clock.h>
#include <mach/mach_types.h>
#include <stdbool.h>

static inline void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static inline void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static inline void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static inline void
hakios_wait_ms(uint32_t ms, bool spin)
{
	if (spin) {
		spin_us(ms * 1000);
	} else {
		sleep_ms(ms);
	}
}

static inline uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_USEC;
}

#endif /* HAKIOS_TIME_H */
