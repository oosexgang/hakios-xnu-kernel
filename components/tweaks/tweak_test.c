/*
 * hakios-tweaktest.dylib: a tweak as a package would install it, to check the tweak path on iOS 26/27 end to
 * end: the tweak loader (components/tweaks/inject_tweaks.c, in Settings through etc/inject.conf) finds it in
 * /var/jb/usr/lib/TweakInject by its property list (Filter: Settings), opens it, and it hooks a method through
 * ElleKit (MSHookMessageEx of the prefix's libsubstrate) the way tweaks do. What it says goes to the console:
 *
 *   hakios: tweaktest: loaded in Preferences
 *   hakios: tweaktest: MSHookMessageEx -[UIViewController viewDidLoad]: done
 *   hakios: tweaktest: the hook ran (-[UIViewController viewDidLoad] of PSRootController ...)
 *
 * The test (RC_JOBS) copies it and hakios-tweaktest.plist into /var/jb/usr/lib/TweakInject and opens Settings.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
say(const char *format, ...)
{
	char line[300];
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

static void (*original_view_did_load)(id self, SEL selector);

static void
view_did_load(id self, SEL selector)
{
	static int told;

	if (!told) {
		told = 1;
		say("hakios: tweaktest: the hook ran (-[UIViewController viewDidLoad] of %s)\n", object_getClassName(self));
	}
	original_view_did_load(self, selector);
}

__attribute__((constructor))
static void
tweak_test_init(void)
{
	void (*hook)(Class, SEL, IMP, IMP *);
	const char *program = getprogname();
	void *substrate;
	Class controller;

	say("hakios: tweaktest: loaded in %s\n", program != NULL ? program : "?");
	substrate = dlopen("/var/jb/usr/lib/libsubstrate.dylib", RTLD_NOW);
	if (substrate == NULL) {
		say("hakios: tweaktest: no libsubstrate: %s\n", dlerror());
		return;
	}
	hook = (void (*)(Class, SEL, IMP, IMP *))dlsym(substrate, "MSHookMessageEx");
	controller = objc_getClass("UIViewController");
	if (hook == NULL || controller == Nil) {
		say("hakios: tweaktest: %s\n", hook == NULL ? "no MSHookMessageEx" : "no UIViewController");
		return;
	}
	hook(controller, sel_registerName("viewDidLoad"), (IMP)view_did_load, (IMP *)&original_view_did_load);
	say("hakios: tweaktest: MSHookMessageEx -[UIViewController viewDidLoad]: %s\n",
	    original_view_did_load != NULL ? "done" : "no original returned");
}
