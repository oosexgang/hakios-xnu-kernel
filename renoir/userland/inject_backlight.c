/*
 * libredwood-backlight.dylib: inside SpringBoard, a display that says it has changed mode.
 *
 * BacklightServicesHost asks CoreBrightness to take the panel to a display mode (on, dimmed, off:
 * -[BLSHOSDisplayInterface transitionToDisplayMode:withDuration:didChangeAlwaysOnBrightnessCurve:])
 * and waits for -[BLSHOSDisplayInterface didCompleteTransitionToDisplayMode:withError:]. That answer
 * comes from the display driver through CoreBrightness, and this phone's panel has no such driver:
 * the panel is on all the time and nothing ever answers. iOS 27's backlight watchdog then aborts
 * SpringBoard ("BLS CB_DM watchdog: OSDI:...:LCD transitionToDisplayMode:ActiveOn withDuration:0.167400s"),
 * over and over, and the screen stays black.
 *
 * Here the request still goes where it went, and once its ramp is over the interface is told it is
 * done on SpringBoard's main queue, as the driver would.
 *
 * With display modes that complete, SpringBoard's idle timer does what it does on a phone: the lock
 * screen dims and goes dark some ten seconds after the last touch. On this phone that came seconds after
 * the first frame (the user interface takes minutes to come up), and the panel cannot really dim. Every
 * timeout of -[SBBacklightIdleTimer _effectiveTimeoutForBaseTimeout:] (dim, sleep) is a day longer:
 * the screen stays lit until the side button turns it off.
 *
 * Methods are found by name and checked by their type encoding; a build where they differ is left
 * alone and says so.
 */
#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef void (*transition_fn)(id, SEL, long long, double, id);
typedef void (*complete_fn)(id, SEL, long long, id);

typedef double (*timeout_fn)(id, SEL, double);

static transition_fn original_transition;
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

static void
transition(id self, SEL selector, long long mode, double duration, id curve_changed)
{
	original_transition(self, selector, mode, duration, curve_changed);
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(duration * 1e9)),
	    dispatch_get_main_queue(), ^{
		((complete_fn)objc_msgSend)(self, complete_sel, mode, nil);
	});
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
		snprintf(line, sizeof(line), "redwood: SpringBoard: no -[SBBacklightIdleTimer _effectiveTimeoutForBaseTimeout:] (%s); idle timer left alone\n", types);
		say(line);
		return;
	}
	original_timeout = (timeout_fn)method_setImplementation(m, (IMP)timeout);
	say("redwood: SpringBoard: the idle timer dims and sleeps a day later\n");
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
	complete_sel = sel_registerName("didCompleteTransitionToDisplayMode:withError:");
	done = interface ? class_getInstanceMethod(interface, complete_sel) : NULL;
	if (m == NULL || done == NULL) {
		say("redwood: SpringBoard: no BLSHOSDisplayInterface display mode methods; backlight left alone\n");
		return;
	}
	if (!types_of(m, types, sizeof(types)) || !types_of(done, done_types, sizeof(done_types)) ||
	    strcmp(types + 4, "d@?") != 0 || strncmp(types, "v@:", 3) != 0 || !is_integer(types[3]) ||
	    strlen(done_types) != 5 || strncmp(done_types, "v@:", 3) != 0 || !is_integer(done_types[3]) || done_types[4] != '@') {
		snprintf(line, sizeof(line), "redwood: SpringBoard: display mode methods are %s / %s; backlight left alone\n",
		    types, done_types);
		say(line);
		return;
	}
	original_transition = (transition_fn)method_setImplementation(m, (IMP)transition);
	snprintf(line, sizeof(line), "redwood: SpringBoard: display mode changes complete after their ramp (%s)\n", types);
	say(line);
}
