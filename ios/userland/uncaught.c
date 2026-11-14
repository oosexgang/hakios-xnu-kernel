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

/*
 * A file URL from a nil path throws NSInvalidArgumentException, which nothing catches: Camera and Photos
 * aborted after photographs, in the reply of -[LNClientConnection fetchAppShortcutParametersForMangledName:
 * withCompletionHandler:] -- App Intents builds a URL from a provider's support path, which is nil for a
 * provider this minimal image has no file for (fileproviderd and BackgroundShortcutRunner abort the same
 * way). A nil path is given a nil URL instead of a thrown exception: the one shortcut is skipped, not the app.
 * NSURL's documented answer to a nil path is to throw, so no correct caller reaches this -- it only changes
 * the crashing case. (The two-argument init is what +fileURLWithPath: and the one-argument init reach.)
 */
static id (*original_file_url)(id, SEL, id, BOOL);

static id
file_url_with_path(id self, SEL selector, id path, BOOL is_directory)
{
	static int told;

	if (path == nil) {
		if (!told) {
			told = 1;
			say("hakios: %s: a file URL from a nil path given nil, not an exception (App Intents)\n", getprogname());
		}
		((void (*)(id, SEL))objc_msgSend)(self, sel_registerName("release"));
		return nil;
	}
	return original_file_url(self, selector, path, is_directory);
}

__attribute__((constructor))
static void
uncaught_init(void)
{
	Class url = objc_getClass("NSURL");
	Method m = url != Nil ? class_getInstanceMethod(url, sel_registerName("initFileURLWithPath:isDirectory:")) : NULL;

	previous_handler = objc_setUncaughtExceptionHandler(uncaught);
	if (m != NULL) {
		original_file_url = (id (*)(id, SEL, id, BOOL))method_getImplementation(m);
		method_setImplementation(m, (IMP)file_url_with_path);
	}
}
