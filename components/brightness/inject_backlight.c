/*
 * libhakios-backlight.dylib: inside SpringBoard, a display that says it has changed mode.
 *
 * BacklightServicesHost asks CoreBrightness to take the panel to a display mode (on, dimmed, off:
 * -[BLSHOSDisplayInterface transitionToDisplayMode:withDuration:didChangeAlwaysOnBrightnessCurve:])
 * and waits for -[BLSHOSDisplayInterface didCompleteTransitionToDisplayMode:withError:]. That answer
 * comes from the display driver through CoreBrightness, and this phone's panel has no such driver:
 * the panel is on all the time and nothing ever answers. Darwin 27's backlight watchdog then aborts
 * SpringBoard ("BLS CB_DM watchdog: OSDI:...:LCD transitionToDisplayMode:ActiveOn withDuration:0.167400s"),
 * over and over, and the screen stays black.
 *
 * Here the request still goes where it went, and once its ramp is over the interface is told it is
 * done on SpringBoard's main queue, as the driver would. A transition that a newer one replaces before
 * its ramp is over is told done as the newer one begins: each is told in the order it was asked for, and
 * the mode Darwin hears completed last is the one it asked for last (a short "on" after a long "off"
 * would otherwise be completed first, and the stale "off" after it).
 *
 * With display modes that complete, SpringBoard's idle timer does what it does on a phone: the lock
 * screen dims and goes dark some ten seconds after the last touch. On this phone that came seconds after
 * the first frame (the user interface takes minutes to come up), and the panel cannot really dim. Every
 * timeout of -[SBBacklightIdleTimer _effectiveTimeoutForBaseTimeout:] (dim, sleep) is a day longer:
 * the screen stays lit until the side button turns it off.
 *
 * The panel follows the display mode as well, through the kernel's backlight (hw.backlight.power,
 * Documentation/backlight.md): a mode named Off makes the panel dark when its ramp is over, one with Dim in
 * its name dims it, any other lights it at once. The names are Darwin's own (its function that names a mode,
 * the one the watchdog's "ActiveOn" comes from), so no number of Apple's enumeration is assumed; where that
 * function is not found the panel is left as it is. Boot-arg bl_trace=1 logs every mode asked for.
 *
 * Methods are found by name and checked by their type encoding; a build where they differ is left
 * alone and says so.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

typedef void (*transition_fn)(id, SEL, long long, double, id);
typedef void (*complete_fn)(id, SEL, long long, id);

typedef double (*timeout_fn)(id, SEL, double);

typedef CFStringRef (*mode_name_fn)(long long);

static transition_fn original_transition;
static mode_name_fn mode_name;
static bool trace;
static timeout_fn original_timeout;
static SEL complete_sel;

static void
say(const char *text)
{
	write(2, text, strlen(text));
}

/*
 * a method's type encoding without the offsets ("v40@0:8q16d24@?32" -> "v@:qd@?"); what is checked:
 * (mode, ramp duration, the block told of a new always-on brightness curve) and (mode, error)
 */
static bool
types_of(Method m, char *out, size_t size)
{
	const char *t = method_getTypeEncoding(m);
	size_t n = 0;

	if (t == NULL) {
		return false;
	}
	for (; *t != '\0' && n + 1 < size; t++) {
		if (*t < '0' || *t > '9') {
			out[n++] = *t;
		}
	}
	out[n] = '\0';
	return true;
}

/* the mode: an integer of either width */
static bool
is_integer(char c)
{
	return c == 'q' || c == 'Q' || c == 'i' || c == 'I';
}

/* the kernel's backlight: 0 dark, 1 lit, 2 dimmed (nothing happens where the kernel has none) */
static void
set_power(int power)
{
	sysctlbyname("hw.backlight.power", NULL, NULL, &power, sizeof(power));
}

/* what a display mode means for the panel, by Darwin's name for it: -1 when it cannot be told */
static int
power_for(long long mode)
{
	char name[64] = "";
	CFStringRef text = mode_name ? mode_name(mode) : NULL;
	int power = -1;

	if (text != NULL && CFGetTypeID(text) == CFStringGetTypeID()) {
		CFStringGetCString(text, name, sizeof(name), kCFStringEncodingUTF8);
		power = strstr(name, "Off") != NULL ? 0 : strstr(name, "Dim") != NULL ? 2 : 1;
	}
	if (trace) {
		char line[128];

		snprintf(line, sizeof(line), "hakios: SpringBoard: display mode %lld (%s): panel %s\n", mode, name[0] ? name : "?",
		    power == 0 ? "dark" : power == 2 ? "dimmed" : power == 1 ? "lit" : "left alone");
		say(line);
	}
	return power;
}

static bool mode_is_32;                 /* the methods take the mode as a 32-bit integer (their type says i or I) */
static volatile long transitions;       /* how many have begun: a ramp's end acts only if no other began since */

/*
 * The transition whose completion has not been told yet: at most one, the newest; touched on the main queue only.
 * Its interface is kept (retained) until it is told.
 */
static struct {
	id interface;
	long long mode;
	long number;                    /* its place in `transitions`; 0: none */
} pending;
static long begun;                      /* the newest transition the main queue has taken in */

static void
complete(id interface, long long mode)
{
	((complete_fn)objc_msgSend)(interface, complete_sel, mode, nil);
	CFRelease((CFTypeRef)interface);
}

/* (cleared before the interface is told: what it does then may begin the next transition) */
static void
complete_pending(void)
{
	id interface = pending.interface;

	if (pending.number == 0) {
		return;
	}
	pending.number = 0;
	pending.interface = nil;
	complete(interface, pending.mode);
}

static void
transition(id self, SEL selector, long long mode, double duration, id curve_changed)
{
	int power;
	long mine;

	if (mode_is_32) {
		mode = (int)mode;               /* (the upper half of the register is not the method's to read) */
	}
	power = power_for(mode);
	mine = __atomic_add_fetch(&transitions, 1, __ATOMIC_SEQ_CST);
	if (power > 0) {
		set_power(power);               /* lit or dimmed from the start of the ramp */
	}
	original_transition(self, selector, mode, duration, curve_changed);
	/* (the object kept until its transition has been told complete: a block of C does not keep it) */
	CFRetain((CFTypeRef)self);
	/*
	 * The one this replaces is told now, ahead of this one: the main queue runs this before the end of this one's
	 * ramp, which is put on it after. One that comes in behind a newer one (begun on another thread) is told at once.
	 */
	dispatch_async(dispatch_get_main_queue(), ^{
		if (mine < begun) {
			complete(self, mode);
			return;
		}
		begun = mine;
		complete_pending();
		pending.interface = self;
		pending.mode = mode;
		pending.number = mine;
	});
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(duration * 1e9)),
	    dispatch_get_main_queue(), ^{
		if (pending.number != mine) {
			return;                 /* replaced, and told so when the next one began */
		}
		/*
		 * (dark once the ramp is over -- unless the display was turned on again meanwhile: an "off" whose ramp ends after
		 * an "on" would leave the panel dark with Darwin believing it lit)
		 */
		if (power == 0 && __atomic_load_n(&transitions, __ATOMIC_SEQ_CST) == mine) {
			set_power(0);
		}
		complete_pending();
	});
}

/* Darwin's function that names a display mode, by the names it has had */
static void
mode_name_init(void)
{
	static const char *const names[] = {
		"NSStringFromBLSBacklightDisplayMode", "NSStringFromBLSDisplayMode", "NSStringFromBLSHDisplayMode",
		"BLSBacklightDisplayModeDescription",
	};
	char arguments[1024], line[160];
	size_t size = sizeof(arguments);

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		arguments[sizeof(arguments) - 1] = '\0';
		trace = strstr(arguments, "bl_trace=1") != NULL;
	}
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]) && mode_name == NULL; i++) {
		mode_name = (mode_name_fn)dlsym(RTLD_DEFAULT, names[i]);
		if (mode_name != NULL) {
			snprintf(line, sizeof(line), "hakios: SpringBoard: display modes named by %s: the panel follows them\n", names[i]);
			say(line);
		}
	}
	if (mode_name == NULL) {
		say("hakios: SpringBoard: no function that names a display mode: the panel does not follow them\n");
	}
}

static double
timeout(id self, SEL selector, double base)
{
	double t = original_timeout(self, selector, base);

	return t > 0 ? t + 24 * 60 * 60 : t;
}

static void
idle_timer_init(void)
{
	Class timer = objc_getClass("SBBacklightIdleTimer");
	Method m = timer ? class_getInstanceMethod(timer, sel_registerName("_effectiveTimeoutForBaseTimeout:")) : NULL;
	char types[32] = "?", line[160];

	if (m == NULL || !types_of(m, types, sizeof(types)) || strcmp(types, "d@:d") != 0) {
		snprintf(line, sizeof(line), "hakios: SpringBoard: no -[SBBacklightIdleTimer _effectiveTimeoutForBaseTimeout:] (%s); idle timer left alone\n", types);
		say(line);
		return;
	}
	original_timeout = (timeout_fn)method_getImplementation(m);     /* (kept before ours can be called) */
	method_setImplementation(m, (IMP)timeout);
	say("hakios: SpringBoard: the idle timer dims and sleeps a day later\n");
}

__attribute__((constructor)) static void
backlight_init(void)
{
	Class interface = objc_getClass("BLSHOSDisplayInterface");
	SEL transition_sel = sel_registerName("transitionToDisplayMode:withDuration:didChangeAlwaysOnBrightnessCurve:");
	Method m = interface ? class_getInstanceMethod(interface, transition_sel) : NULL;
	Method done;
	char types[32] = "?", done_types[32] = "?", line[256];

	idle_timer_init();
	mode_name_init();
	complete_sel = sel_registerName("didCompleteTransitionToDisplayMode:withError:");
	done = interface ? class_getInstanceMethod(interface, complete_sel) : NULL;
	if (m == NULL || done == NULL) {
		say("hakios: SpringBoard: no BLSHOSDisplayInterface display mode methods; backlight left alone\n");
		return;
	}
	if (!types_of(m, types, sizeof(types)) || !types_of(done, done_types, sizeof(done_types)) ||
	    strcmp(types + 4, "d@?") != 0 || strncmp(types, "v@:", 3) != 0 || !is_integer(types[3]) ||
	    strlen(done_types) != 5 || strncmp(done_types, "v@:", 3) != 0 || !is_integer(done_types[3]) || done_types[4] != '@') {
		snprintf(line, sizeof(line), "hakios: SpringBoard: display mode methods are %s / %s; backlight left alone\n",
		    types, done_types);
		say(line);
		return;
	}
	mode_is_32 = types[3] == 'i' || types[3] == 'I';
	original_transition = (transition_fn)method_getImplementation(m);
	method_setImplementation(m, (IMP)transition);
	snprintf(line, sizeof(line), "hakios: SpringBoard: display mode changes complete after their ramp (%s)\n", types);
	say(line);
}
