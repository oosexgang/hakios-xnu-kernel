/*
 * What a driver says, where it can be read while the phone runs.
 *
 * The kernel's own log reaches the loader when a run is over, and a run that ends in a reset without
 * a word leaves none. The loader also keeps a log the kernel writes to directly (osfmk/arm64/
 * loader_return.c), which it can show during a run: a driver's lines go there at once, and to the
 * kernel's log as well where that may be written.
 *
 * It may not be written with interrupts off: the console panics there ("Console I/O from
 * interrupt-disabled context"), which is where a driver is on the way back to the loader.
 */
#ifndef _HAKIOS_LOG_H
#define _HAKIOS_LOG_H

#include <stdarg.h>
#include <libkern/libkern.h>
#include <machine/machine_routines.h>

extern void loader_log_putc(char c);

/* The line to the loader's log, and to the kernel's if `kernel_log_too`. Returns its length. */
static inline int __printflike(2, 0)
hakios_vsay(bool kernel_log_too, const char *format, va_list list)
{
	char line[400];
	int length = vsnprintf(line, sizeof(line), format, list);

	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (kernel_log_too) {
		printf("%s", line);
	}
	return length;
}

/* To both logs; with interrupts off, to the loader's only. What a driver wants unless it knows better. */
static inline int __printflike(1, 2)
hakios_say(const char *format, ...)
{
	va_list list;
	int length;

	va_start(list, format);
	length = hakios_vsay(ml_get_interrupts_enabled(), format, list);
	va_end(list);
	return length;
}

/*
 * A driver's own say(): to the loader's log, and to the kernel's while `kernel_log_too` holds -- its
 * own rule, as "not on the way back to the loader" (where interrupts are off).
 */
#define HAKIOS_DEFINE_SAY(kernel_log_too)                                       \
	static int __printflike(1, 2)                                           \
	say(const char *format, ...)                                            \
	{                                                                       \
		va_list list;                                                   \
		int length;                                                     \
                                                                                \
		va_start(list, format);                                         \
		length = hakios_vsay((kernel_log_too), format, list);           \
		va_end(list);                                                   \
		return length;                                                  \
	}

#endif /* _HAKIOS_LOG_H */
