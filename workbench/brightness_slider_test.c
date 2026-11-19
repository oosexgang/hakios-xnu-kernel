/*
 * Host tests of the brightness slider's library (components/brightness/inject_brightness.c), the file itself included
 * here: what it is given of CoreFoundation, the Objective-C runtime, libdispatch and sysctlbyname() is this file's
 * (just enough of each). Its constructor finds CoreBrightness's two classes in the test's runtime, as it would in
 * SpringBoard or backboardd, and replaces their methods; the scenarios call what the methods have then, as
 * CoreBrightness's callers would, and look at what the kernel's hw.backlight.* was told. Darwin's headers are not on a
 * Linux host: workbench/run.sh builds this against empty ones of the same names in a directory of its own.
 *
 *   brightness_slider_test               the scenarios' names
 *   brightness_slider_test SCENARIO      one scenario a process
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

/* ---- CoreFoundation: booleans, numbers and strings, each a struct of the test's ------------------------------ */

typedef unsigned char Boolean;
typedef long CFIndex;
typedef unsigned long CFTypeID;
typedef uint32_t CFStringEncoding;
typedef const void *CFTypeRef, *CFAllocatorRef;
typedef enum { kCFNumberFloatType = 5, kCFNumberDoubleType = 6 } CFNumberType;

enum { TYPE_BOOLEAN = 1, TYPE_NUMBER, TYPE_STRING };
struct test_cf {
	CFTypeID type;
	double number;                  /* a boolean's value, a number's */
	const char *string;
};
typedef const struct test_cf *CFStringRef, *CFBooleanRef, *CFNumberRef;

#define kCFAllocatorDefault     NULL
#define kCFStringEncodingUTF8   0x08000100
#define CFSTR(s)                ((CFStringRef)&(const struct test_cf){ TYPE_STRING, 0, (s) })
static const struct test_cf true_object = { TYPE_BOOLEAN, 1, NULL }, false_object = { TYPE_BOOLEAN, 0, NULL };
#define kCFBooleanTrue          (&true_object)
#define kCFBooleanFalse         (&false_object)

static CFTypeID CFBooleanGetTypeID(void) { return TYPE_BOOLEAN; }
static CFTypeID CFNumberGetTypeID(void) { return TYPE_NUMBER; }
static CFTypeID CFStringGetTypeID(void) { return TYPE_STRING; }
static CFTypeID CFGetTypeID(CFTypeRef object) { return ((const struct test_cf *)object)->type; }
static Boolean CFBooleanGetValue(CFBooleanRef b) { return b->number != 0; }
static CFTypeRef CFRetain(CFTypeRef object) { return object; }

static int numbers_made;                /* CFNumberCreate()s less CFRelease()s of them */

static Boolean
CFNumberGetValue(CFNumberRef number, CFNumberType type, void *out)
{
	if (type == kCFNumberDoubleType) {
		*(double *)out = number->number;
	} else {
		*(float *)out = (float)number->number;
	}
	return true;
}

static CFNumberRef
CFNumberCreate(CFAllocatorRef allocator, CFNumberType type, const void *value)
{
	struct test_cf *n = calloc(1, sizeof(*n));

	(void)allocator;
	n->type = TYPE_NUMBER;
	n->number = type == kCFNumberDoubleType ? *(const double *)value : *(const float *)value;
	numbers_made++;
	return n;
}

static void
CFRelease(CFTypeRef object)
{
	if (object != kCFBooleanTrue && object != kCFBooleanFalse && CFGetTypeID(object) == TYPE_NUMBER) {
		numbers_made--;
		free((void *)object);
	}
}

static Boolean
CFEqual(CFTypeRef a, CFTypeRef b)
{
	const struct test_cf *x = a, *y = b;

	if (x->type != y->type) {
		return false;
	}
	return x->type == TYPE_STRING ? strcmp(x->string, y->string) == 0 : x->number == y->number;
}

static Boolean
CFStringGetCString(CFStringRef string, char *out, CFIndex size, CFStringEncoding encoding)
{
	(void)encoding;
	if (strlen(string->string) >= (size_t)size) {
		return false;
	}
	strcpy(out, string->string);
	return true;
}

static struct test_cf *
number(double value)
{
	return (struct test_cf *)CFNumberCreate(NULL, kCFNumberDoubleType, &value);
}

/* ---- the Objective-C runtime: CoreBrightness's two classes, with a method each that the library replaces ---- */

typedef struct objc_object *id;
typedef struct objc_selector *SEL;
typedef void (*IMP)(void);
typedef struct test_method *Method;
typedef struct test_class *Class;
struct mach_header;

struct test_method {
	const char *name, *types;
	IMP imp;
};
struct test_class {
	const char *name;
	struct test_method methods[2];
};

/* what CoreBrightness's own methods were last given */
static struct {
	int calls;
	id self, value, key;
} their_set;
static int their_copies;

static signed char
their_set_property(id self, SEL selector, id value, id key)
{
	(void)selector;
	their_set.calls++;
	their_set.self = self;
	their_set.value = value;
	their_set.key = key;
	return 1;
}

static id
their_copy_property(id self, SEL selector, id key)
{
	(void)self, (void)selector, (void)key;
	their_copies++;
	return (id)(void *)number(0.25);
}

static struct test_class classes[] = {
	{ "BrightnessSystemClient", {
		{ "setProperty:forKey:", "B32@0:8@16@24", (IMP)their_set_property },
		{ "copyPropertyForKey:", "@24@0:8@16", (IMP)their_copy_property },
	} },
	/* (backboardd's: a build whose setter returns nothing is not CoreBrightness's as this library knows it) */
	{ "BrightnessSystem", {
		{ "setProperty:forKey:", "v32@0:8@16@24", (IMP)their_set_property },
		{ "copyPropertyForKey:", "@24@0:8@16", (IMP)their_copy_property },
	} },
};

static Class
objc_getClass(const char *name)
{
	for (unsigned i = 0; i < sizeof(classes) / sizeof(classes[0]); i++) {
		if (strcmp(classes[i].name, name) == 0) {
			return &classes[i];
		}
	}
	return NULL;
}

static SEL sel_registerName(const char *name) { return (SEL)(void *)name; }
static const char *method_getTypeEncoding(Method m) { return m->types; }
static IMP method_getImplementation(Method m) { return m->imp; }

static Method
class_getInstanceMethod(Class cls, SEL sel)
{
	for (unsigned i = 0; i < 2; i++) {
		if (strcmp(cls->methods[i].name, (const char *)sel) == 0) {
			return &cls->methods[i];
		}
	}
	return NULL;
}

static IMP
method_setImplementation(Method m, IMP imp)
{
	IMP was = m->imp;

	m->imp = imp;
	return was;
}

static void (*load_image_func)(const struct mach_header *);
static void objc_addLoadImageFunc(void (*func)(const struct mach_header *)) { load_image_func = func; }

/* ---- libdispatch: what is put on the main queue is not run ------------------------------------------------- */

typedef void *dispatch_queue_t;
typedef void (*dispatch_function_t)(void *);
static dispatch_queue_t dispatch_get_main_queue(void) { return NULL; }
static void dispatch_async_f(dispatch_queue_t q, void *context, dispatch_function_t f) { (void)q, (void)context, (void)f; }

/* ---- the kernel's hw.backlight ----------------------------------------------------------------------------- */

static struct {
	const char *name;
	int value;
	int writes;
} sysctls[] = {
	{ "hw.backlight.level", 600, 0 },
	{ "hw.backlight.auto", 0, 0 },
	{ "hw.backlight.shown", 300, 0 },
};

static int
sysctlbyname(const char *name, void *old, size_t *old_size, void *new, size_t new_size)
{
	static const char bootargs[] = "debug=0x8 bl_trace=1", backend[] = "soft";

	if (strcmp(name, "kern.bootargs") == 0 || strcmp(name, "hw.backlight.backend") == 0) {
		const char *text = name[0] == 'k' ? bootargs : backend;

		if (old == NULL || *old_size < strlen(text) + 1) {
			return -1;
		}
		memcpy(old, text, strlen(text) + 1);
		*old_size = strlen(text) + 1;
		return 0;
	}
	for (unsigned i = 0; i < sizeof(sysctls) / sizeof(sysctls[0]); i++) {
		if (strcmp(sysctls[i].name, name) != 0) {
			continue;
		}
		if (old != NULL) {
			if (*old_size < sizeof(int)) {
				return -1;
			}
			memcpy(old, &sysctls[i].value, sizeof(int));
			*old_size = sizeof(int);
		}
		if (new != NULL) {
			if (new_size != sizeof(int)) {
				return -1;
			}
			memcpy(&sysctls[i].value, new, sizeof(int));
			sysctls[i].writes++;
		}
		return 0;
	}
	return -1;
}

static int *
sysctl_value(const char *name)
{
	for (unsigned i = 0; i < sizeof(sysctls) / sizeof(sysctls[0]); i++) {
		if (strcmp(sysctls[i].name, name) == 0) {
			return &sysctls[i].value;
		}
	}
	abort();
}

static int
sysctl_writes(void)
{
	int n = 0;

	for (unsigned i = 0; i < sizeof(sysctls) / sizeof(sysctls[0]); i++) {
		n += sysctls[i].writes;
	}
	return n;
}

/* ---- the C library of Darwin's that Linux's has not got -------------------------------------------------------- */

static const char *getprogname(void) { return "SpringBoard"; }

static size_t
test_strlcpy(char *out, const char *in, size_t size)
{
	size_t n = strlen(in);

	if (size > 0) {
		size_t k = n < size - 1 ? n : size - 1;

		memcpy(out, in, k);
		out[k] = '\0';
	}
	return n;
}
#define strlcpy test_strlcpy

/* (what the library says, to standard error as there; glibc's write() would have its answer looked at) */
static ssize_t
test_write(int fd, const void *data, size_t size)
{
	return fwrite(data, 1, size, fd == 2 ? stderr : stdout) == size ? (ssize_t)size : -1;
}
#define write test_write

#include "../components/brightness/inject_brightness.c"

/* ---- the scenarios ----------------------------------------------------------------------------------------- */

static id the_client = (id)(void *)&classes[0];

/* the slider set to VALUE through the client's -setProperty:forKey:, as Control Center sets it */
static void
set(const char *key, struct test_cf *value)
{
	set_fn method = (set_fn)classes[CLIENT].methods[0].imp;
	CFStringRef k = CFSTR(key);
	signed char answer;

	their_set.calls = 0;
	answer = method(the_client, sel_registerName("setProperty:forKey:"), (id)(void *)value, (id)(void *)k);
	CHECK(answer == 1, "%s: CoreBrightness's answer not passed back: %d", key, answer);
	CHECK(their_set.calls == 1 && their_set.self == the_client && their_set.value == (id)(void *)value &&
	    their_set.key == (id)(void *)k, "%s: CoreBrightness's own method not called with what it was given (%d calls)",
	    key, their_set.calls);
}

static void
set_level(double value)
{
	struct test_cf *v = number(value);

	set("DisplayBrightness", v);
	CFRelease(v);
}

/* the methods are ours where their types are the expected ones, and left alone elsewhere */
static void
scenario_hooked(void)
{
	CHECK(classes[CLIENT].methods[0].imp == set_imps[CLIENT], "the client's setter is not ours");
	CHECK(classes[CLIENT].methods[1].imp == copy_imps[CLIENT], "the client's copier is not ours");
	CHECK(original_set[CLIENT] == (set_fn)their_set_property && original_copy[CLIENT] == (copy_fn)their_copy_property,
	    "the client's originals not kept");
	CHECK(classes[SERVER].methods[0].imp == (IMP)their_set_property && original_set[SERVER] == NULL,
	    "a setter of other types replaced");
	CHECK(classes[SERVER].methods[1].imp == copy_imps[SERVER], "the server's copier is not ours");
	CHECK(load_image_func != NULL, "classes loaded later not looked for");
}

/* a level of the slider, 0 to 1, is the kernel's level, 0 to 1000; beyond either end is that end */
static void
scenario_level(void)
{
	static const struct { double slider; int level; } cases[] = {
		{ 0.5, 500 }, { 0, 0 }, { 1, 1000 }, { 0.2504, 250 }, { 0.0006, 1 }, { 0.9996, 1000 },
		{ 1.7, 1000 }, { -0.3, 0 }, { 0.42, 420 },
	};

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		*sysctl_value("hw.backlight.level") = 77;
		set_level(cases[i].slider);
		CHECK(*sysctl_value("hw.backlight.level") == cases[i].level, "slider %g: level %d, not %d", cases[i].slider,
		    *sysctl_value("hw.backlight.level"), cases[i].level);
	}
}

/*
 * A value no slider has -- not a number, infinite, past what an int holds -- never takes the panel dark by itself:
 * one past an end is that end, and one that is not a number is not passed on (the panel keeps its level).
 */
static void
scenario_not_a_number(void)
{
	static const struct { double slider; int level; } cases[] = {
		{ 1e300, 1000 }, { -1e300, 0 }, { 3e9, 1000 }, { -3e9, 0 }, { INFINITY, 1000 }, { -INFINITY, 0 },
		{ 2147483.6475, 1000 },
	};
	int writes;

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		*sysctl_value("hw.backlight.level") = 77;
		set_level(cases[i].slider);
		CHECK(*sysctl_value("hw.backlight.level") == cases[i].level, "slider %g: level %d, not %d", cases[i].slider,
		    *sysctl_value("hw.backlight.level"), cases[i].level);
	}
	*sysctl_value("hw.backlight.level") = 640;
	writes = sysctl_writes();
	set_level(NAN);
	CHECK(*sysctl_value("hw.backlight.level") == 640, "a slider that is not a number: level %d, not left at 640",
	    *sysctl_value("hw.backlight.level"));
	CHECK(sysctl_writes() == writes, "a slider that is not a number: the kernel told something");
	set_level(-NAN);
	CHECK(*sysctl_value("hw.backlight.level") == 640, "a slider that is -NaN: level %d", *sysctl_value("hw.backlight.level"));
}

/* the switch of Auto-Brightness, as a boolean or a number; another key, or no number, tells the kernel nothing */
static void
scenario_auto(void)
{
	struct test_cf *zero = number(0), *one = number(1);
	int writes;

	set("DisplayBrightnessAuto", (struct test_cf *)kCFBooleanTrue);
	CHECK(*sysctl_value("hw.backlight.auto") == 1, "auto %d", *sysctl_value("hw.backlight.auto"));
	set("DisplayBrightnessAuto", zero);
	CHECK(*sysctl_value("hw.backlight.auto") == 0, "auto %d", *sysctl_value("hw.backlight.auto"));
	set("DisplayBrightnessAuto", one);
	CHECK(*sysctl_value("hw.backlight.auto") == 1, "auto %d", *sysctl_value("hw.backlight.auto"));
	set("DisplayBrightnessAuto", (struct test_cf *)kCFBooleanFalse);
	CHECK(*sysctl_value("hw.backlight.auto") == 0, "auto %d", *sysctl_value("hw.backlight.auto"));
	writes = sysctl_writes();
	set("DisplayNightShift", one);
	set("DisplayBrightness", (struct test_cf *)CFSTR("bright"));
	CHECK(sysctl_writes() == writes, "the kernel told of a key or a value not ours");
	CFRelease(zero);
	CFRelease(one);
}

/* what is asked of the client: the kernel's for our two keys, CoreBrightness's for any other */
static void
scenario_copy(void)
{
	copy_fn method = (copy_fn)classes[CLIENT].methods[1].imp;
	SEL sel = sel_registerName("copyPropertyForKey:");
	const struct test_cf *answer;

	*sysctl_value("hw.backlight.auto") = 0;
	*sysctl_value("hw.backlight.level") = 420;
	answer = (const struct test_cf *)method(the_client, sel, (id)(void *)CFSTR("DisplayBrightness"));
	CHECK(answer != NULL && answer->type == TYPE_NUMBER && fabs(answer->number - 0.42) < 1e-6, "by hand: %g",
	    answer ? answer->number : -1);
	CFRelease(answer);
	*sysctl_value("hw.backlight.auto") = 1;
	answer = (const struct test_cf *)method(the_client, sel, (id)(void *)CFSTR("DisplayBrightness"));
	CHECK(answer != NULL && fabs(answer->number - 0.3) < 1e-6, "automatic: the panel's now, %g", answer ? answer->number : -1);
	CFRelease(answer);
	answer = (const struct test_cf *)method(the_client, sel, (id)(void *)CFSTR("DisplayBrightnessAuto"));
	CHECK(answer == kCFBooleanTrue, "the switch");
	their_copies = 0;
	answer = (const struct test_cf *)method(the_client, sel, (id)(void *)CFSTR("DisplayNightShift"));
	CHECK(their_copies == 1 && answer != NULL && answer->number == 0.25, "another key not CoreBrightness's");
	CFRelease(answer);
	CHECK(numbers_made == 0, "%d numbers made and not released", numbers_made);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "hooked", scenario_hooked },
		{ "level", scenario_level },
		{ "not_a_number", scenario_not_a_number },
		{ "auto", scenario_auto },
		{ "copy", scenario_copy },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			printf("brightness_slider_test %s: %d checks, %d failed\n", argv[1], checks, failures);
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
