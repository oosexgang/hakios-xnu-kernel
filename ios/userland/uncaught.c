/*
 * An exception nothing catches, named on the console before the application aborts (linked into
 * libhakios-statusbar.dylib, which etc/inject.conf gives every application): an application's log does not reach
 * ours, and the kernel's backtrace of the abort shows only where the exception was rethrown. Camera and Photos
 * aborted so after photographs were taken (logs/093853-run.log, logs/095629-run.log in hakios-sm7325), both
 * where NSXPC rethrows (-[_NSXPCConnectionRequestedReplies endTransactionForSequence:completionHandler:]) from a
 * reply of -[LNClientConnection fetchAppShortcutParametersForMangledName:withCompletionHandler:].
 *
 * The addresses are where it was thrown; the kernel's "crash" line gives the shared cache's slide.
 * CoreFoundation's own handler (its "Terminating app" line) is called after.
 */
#include <fcntl.h>
#include <objc/message.h>
#include <objc/objc-exception.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static objc_uncaught_exception_handler previous_handler;

static void
say(const char *format, ...)
{
	char line[400];
	va_list ap;
	int fd;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		write(fd, line, strlen(line));
		close(fd);
	}
}

static const char *
utf8(id string)
{
	return string != nil ? ((const char *(*)(id, SEL))objc_msgSend)(string, sel_registerName("UTF8String")) : NULL;
}

static void
uncaught(id exception)
{
	id addresses = ((id (*)(id, SEL))objc_msgSend)(exception, sel_registerName("callStackReturnAddresses"));
	long count = addresses != nil ? ((long (*)(id, SEL))objc_msgSend)(addresses, sel_registerName("count")) : 0;
	const char *name = utf8(((id (*)(id, SEL))objc_msgSend)(exception, sel_registerName("name")));
	const char *reason = utf8(((id (*)(id, SEL))objc_msgSend)(exception, sel_registerName("reason")));
	char line[300];
	size_t used = 0;

	say("hakios: %s: uncaught %s: %.300s\n", getprogname(), name != NULL ? name : "?", reason != NULL ? reason : "(no reason)");
	for (long i = 0; i < count && i < 20; i++) {
		id number = ((id (*)(id, SEL, long))objc_msgSend)(addresses, sel_registerName("objectAtIndex:"), i);
		unsigned long a = ((unsigned long (*)(id, SEL))objc_msgSend)(number, sel_registerName("unsignedLongValue"));
		int w = snprintf(line + used, sizeof(line) - used, " %lx", a);

		if (w < 0 || (size_t)w >= sizeof(line) - used) {
			break;
		}
		used += (size_t)w;
	}
	say("hakios: %s: thrown from%s\n", getprogname(), used != 0 ? line : " (no addresses)");
	if (previous_handler != NULL) {
		previous_handler(exception);
	}
}

__attribute__((constructor))
static void
uncaught_init(void)
{
	previous_handler = objc_setUncaughtExceptionHandler(uncaught);
}
