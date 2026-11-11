/*
 * A tiny tweak, to prove the port loads tweaks: injected into a process by ElleKit's loader
 * (/var/jb/usr/lib/TweakLoader.dylib, which the kernel inserts into the processes named in
 * etc/inject.conf), it says on the console that it is there, and hooks one SpringBoard method
 * with MSHookMessageEx (from libsubstrate -> libellekit) to show that the hooking engine works.
 */
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <objc/message.h>
#include <objc/runtime.h>

extern void MSHookMessageEx(Class cls, SEL sel, IMP imp, IMP *result);

static void say(const char *s) {
	int fd = open("/dev/console", O_WRONLY);
	if (fd >= 0) { (void)write(fd, s, strlen(s)); close(fd); }
}

/* -[SpringBoard applicationDidFinishLaunching:] has already fired by the time we load, so hook
 * something that keeps being called: -[SBIconView setIcon:] runs whenever an icon is laid out. */
static void (*orig_setIcon)(id, SEL, id);
static int hit;
static void my_setIcon(id self, SEL _cmd, id icon) {
	if (!hit) { hit = 1; say("hakios: tweak hook fired: -[SBIconView setIcon:]\n"); }
	orig_setIcon(self, _cmd, icon);
}

__attribute__((constructor))
static void redtest_init(void) {
	say("hakios: test tweak loaded\n");
	Class c = objc_getClass("SBIconView");
	SEL s = sel_registerName("setIcon:");
	if (c && class_getInstanceMethod(c, s)) {
		MSHookMessageEx(c, s, (IMP)my_setIcon, (IMP *)&orig_setIcon);
		say("hakios: tweak hooked -[SBIconView setIcon:]\n");
	} else {
		say("hakios: tweak: no SBIconView setIcon:\n");
	}
}
