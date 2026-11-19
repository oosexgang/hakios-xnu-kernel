/*
 * libhakios-brightness.dylib: Darwin's brightness slider and its automatic brightness switch, to the kernel's
 * backlight (hw.backlight.*: drivers/board/backlight.c).
 *
 * On an iPhone the slider of Control Center and of Settings, and the switch of Auto-Brightness, are
 * properties of CoreBrightness: a process sets "DisplayBrightness" (0.0 to 1.0) or "DisplayBrightnessAuto"
 * on a BrightnessSystemClient, which hands it to the BrightnessSystem in backboardd, which sets the panel.
 * Here CoreBrightness has no panel it knows (there is no Apple display driver), so the value goes nowhere.
 *
 * This library is put into the processes that set those properties and into backboardd (root26-darwin.sh's
 * inject.conf), and replaces -setProperty:forKey: and -copyPropertyForKey: of both classes, whichever of
 * them the process has:
 *
 *   set DisplayBrightness          hw.backlight.level = value x 1000, then CoreBrightness's own as before
 *   set DisplayBrightnessAuto      hw.backlight.auto
 *   copy DisplayBrightness         the light on the panel now (it moves when the brightness is automatic)
 *   copy DisplayBrightnessAuto     the kernel's switch
 *
 * so the kernel decides, and every process sees the same values. Nothing else is touched. A method that is
 * not there, or whose types are not the expected ones, is left alone and said so on standard error. A class
 * whose framework is loaded later (a Control Center module's) is looked for again once an image is loaded, on the
 * main queue: the Objective-C runtime tells of a new image with its own lock held, and looking a class up takes it.
 *
 * Boot-arg bl_trace=1: every key set or asked for through these methods, with its value, on standard error
 * (to find the keys of a newer Darwin).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

typedef signed char (*set_fn)(id, SEL, id, id);
typedef id (*copy_fn)(id, SEL, id);

/* the two classes: client (any process) and server (backboardd) */
enum { CLIENT, SERVER, CLASSES };
static const char *const class_names[CLASSES] = { "BrightnessSystemClient", "BrightnessSystem" };
static set_fn original_set[CLASSES];
static copy_fn original_copy[CLASSES];
static bool trace;
static char process[64] = "?";

static void
say(const char *text)
{
	write(2, text, strlen(text));
}

static bool
read_value(const char *name, int *value)
{
	size_t size = sizeof(*value);

	return sysctlbyname(name, value, &size, NULL, 0) == 0;
}

static bool
write_value(const char *name, int value)
{
	return sysctlbyname(name, NULL, NULL, &value, sizeof(value)) == 0;
}

/* a number or a boolean, as a double; false if it is neither */
static bool
number_of(CFTypeRef value, double *out)
{
	if (value == NULL) {
		return false;
	}
	if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
		*out = CFBooleanGetValue((CFBooleanRef)value) ? 1.0 : 0.0;
		return true;
	}
	if (CFGetTypeID(value) == CFNumberGetTypeID()) {
		return CFNumberGetValue((CFNumberRef)value, kCFNumberDoubleType, out);
	}
	return false;
}

static bool
key_is(CFTypeRef key, CFStringRef name)
{
	return key != NULL && CFGetTypeID(key) == CFStringGetTypeID() && CFEqual(key, name);
}

static void
trace_key(const char *what, int which, CFTypeRef key, CFTypeRef value)
{
	char k[96] = "?", line[256];
	double number = 0;

	if (!trace) {
		return;
	}
	if (key != NULL && CFGetTypeID(key) == CFStringGetTypeID()) {
		CFStringGetCString((CFStringRef)key, k, sizeof(k), kCFStringEncodingUTF8);
	}
	if (number_of(value, &number)) {
		snprintf(line, sizeof(line), "hakios-brightness: %s: %s %s %s = %g\n", process, class_names[which], what, k, number);
	} else {
		snprintf(line, sizeof(line), "hakios-brightness: %s: %s %s %s%s\n", process, class_names[which], what, k,
		    value != NULL ? " (not a number)" : "");
	}
	say(line);
}

/* What the kernel is told, for a property of ours (any other is not looked at). */
static void
to_kernel(CFTypeRef key, CFTypeRef value)
{
	double number;

	if (!number_of(value, &number)) {
		return;
	}
	if (key_is(key, CFSTR("DisplayBrightness"))) {
		/*
		 * (kept to 0..1 as a double, before it is made an integer: one out of an int's range does not convert,
		 * and a NaN, which no comparison keeps out, became level 0 -- a dark panel -- on this processor)
		 */
		if (!(number == number)) {
			return;
		}
		number = number < 0.0 ? 0.0 : number > 1.0 ? 1.0 : number;
		write_value("hw.backlight.level", (int)(number * 1000.0 + 0.5));
	} else if (key_is(key, CFSTR("DisplayBrightnessAuto"))) {
		write_value("hw.backlight.auto", number != 0.0);
	}
}

/* The kernel's answer for one of ours, retained (a copy); NULL for any other key. */
static CFTypeRef
from_kernel(CFTypeRef key)
{
	int value;

	if (key_is(key, CFSTR("DisplayBrightness"))) {
		int automatic = 0;
		float level;

		if (!read_value(read_value("hw.backlight.auto", &automatic) && automatic ? "hw.backlight.shown" : "hw.backlight.level",
		    &value)) {
			return NULL;
		}
		level = (float)value / 1000.0f;
		return CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &level);
	}
	if (key_is(key, CFSTR("DisplayBrightnessAuto"))) {
		if (!read_value("hw.backlight.auto", &value)) {
			return NULL;
		}
		return CFRetain(value ? kCFBooleanTrue : kCFBooleanFalse);
	}
	return NULL;
}

static signed char
set_property(int which, id self, SEL selector, id value, id key)
{
	trace_key("set", which, (CFTypeRef)key, (CFTypeRef)value);
	to_kernel((CFTypeRef)key, (CFTypeRef)value);
	return original_set[which](self, selector, value, key);
}

static id
copy_property(int which, id self, SEL selector, id key)
{
	CFTypeRef ours = from_kernel((CFTypeRef)key);
	id theirs;

	if (ours != NULL) {
		trace_key("copy (ours)", which, (CFTypeRef)key, ours);
		return (id)ours;
	}
	theirs = original_copy[which](self, selector, key);
	trace_key("copy", which, (CFTypeRef)key, (CFTypeRef)theirs);
	return theirs;
}

static signed char client_set(id s, SEL c, id v, id k) { return set_property(CLIENT, s, c, v, k); }
static signed char server_set(id s, SEL c, id v, id k) { return set_property(SERVER, s, c, v, k); }
static id client_copy(id s, SEL c, id k) { return copy_property(CLIENT, s, c, k); }
static id server_copy(id s, SEL c, id k) { return copy_property(SERVER, s, c, k); }
static const IMP set_imps[CLASSES] = { (IMP)client_set, (IMP)server_set };
static const IMP copy_imps[CLASSES] = { (IMP)client_copy, (IMP)server_copy };

/* a method's type encoding without the offsets ("B32@0:8@16@24" -> "B@:@@") */
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

static bool looked[CLASSES];           /* (written on the main queue, or in the constructor before anything else runs) */

static void
hook(int which)
{
	Class cls = __atomic_load_n(&looked[which], __ATOMIC_ACQUIRE) ? NULL : objc_getClass(class_names[which]);
	Method set = cls ? class_getInstanceMethod(cls, sel_registerName("setProperty:forKey:")) : NULL;
	Method copy = cls ? class_getInstanceMethod(cls, sel_registerName("copyPropertyForKey:")) : NULL;
	char set_types[32] = "?", copy_types[32] = "?", line[256];

	if (cls == NULL) {
		return;                 /* (this process has not got that half, or not yet) */
	}
	__atomic_store_n(&looked[which], true, __ATOMIC_RELEASE);
	if (set != NULL && types_of(set, set_types, sizeof(set_types)) &&
	    (strcmp(set_types, "B@:@@") == 0 || strcmp(set_types, "c@:@@") == 0)) {
		/* (the original kept before ours is in: another thread may call it the moment it is) */
		original_set[which] = (set_fn)method_getImplementation(set);
		method_setImplementation(set, set_imps[which]);
	}
	if (copy != NULL && types_of(copy, copy_types, sizeof(copy_types)) && strcmp(copy_types, "@@:@") == 0) {
		original_copy[which] = (copy_fn)method_getImplementation(copy);
		method_setImplementation(copy, copy_imps[which]);
	}
	snprintf(line, sizeof(line), "hakios-brightness: %s: %s: setProperty:forKey: %s (%s), copyPropertyForKey: %s (%s)\n",
	    process, class_names[which], original_set[which] ? "ours" : "LEFT ALONE", set_types,
	    original_copy[which] ? "ours" : "LEFT ALONE", copy_types);
	say(line);
}

static bool hook_queued;                /* a look on the main queue is asked for and has not begun */

static void
hook_late(void *context __attribute__((unused)))
{
	__atomic_store_n(&hook_queued, false, __ATOMIC_SEQ_CST);
	for (int which = 0; which < CLASSES; which++) {
		hook(which);
	}
}

/*
 * (the Objective-C runtime has read an image's classes: those of a framework loaded only now). It calls this with
 * its lock held, and objc_getClass and the method functions take that lock again, which aborts the process: so
 * here only whether a class is still to be found is asked, and the looking is done on the main queue, outside it.
 */
static void
image_loaded(const struct mach_header *header __attribute__((unused)))
{
	bool wanted = false;

	for (int which = 0; which < CLASSES; which++) {
		wanted = wanted || !__atomic_load_n(&looked[which], __ATOMIC_ACQUIRE);
	}
	if (wanted && !__atomic_exchange_n(&hook_queued, true, __ATOMIC_SEQ_CST)) {
		dispatch_async_f(dispatch_get_main_queue(), NULL, hook_late);
	}
}

__attribute__((constructor)) static void
brightness_init(void)
{
	char arguments[1024];
	size_t size = sizeof(arguments);
	int backend_ok;
	char backend[16] = "?";
	size_t backend_size = sizeof(backend);

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		arguments[sizeof(arguments) - 1] = '\0';
		trace = strstr(arguments, "bl_trace=1") != NULL;
	}
	{
		const char *name = getprogname();

		if (name != NULL) {
			strlcpy(process, name, sizeof(process));
		}
	}
	backend_ok = sysctlbyname("hw.backlight.backend", backend, &backend_size, NULL, 0) == 0;
	if (!backend_ok || strcmp(backend, "none") == 0) {
		char line[160];

		snprintf(line, sizeof(line), "hakios-brightness: %s: the kernel has no backlight (%s): CoreBrightness left alone\n",
		    process, backend_ok ? backend : "no hw.backlight");
		say(line);
		return;
	}
	for (int which = 0; which < CLASSES; which++) {
		hook(which);
	}
	objc_addLoadImageFunc(image_loaded);
}
