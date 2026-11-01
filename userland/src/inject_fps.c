/*
 * libhakios.fps: every application's frames at the panel's rate (the Xiaomi 12S: 120 a second).
 *
 * On a phone QuartzCore holds a process to 60 frames a second unless its bundle's Info.plist says
 * CADisableMinimumFrameDurationOnPhone, which the 120 Hz iPhones' applications do and the iPhone X's
 * system image (the one on the phone's flash, as it came out of the IPSW) does not. mkhighrate.py
 * writes the key into a system image built here; the flash's is Apple's own. So the key is answered
 * here instead, in every program of an application bundle (etc/inject.conf: "@apps", which the kernel
 * matches against the program's path): the bundle's Info.plist lookups, by CoreFoundation and by
 * Foundation, say YES for that key and are left alone for every other.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <mach-o/dyld.h>

extern const char *getprogname(void);

/* a line to the console (an application's own stderr goes nowhere) */
static void
say(const char *format, ...)
{
	static int console = -2;
	char line[512];
	va_list arguments;
	int n;

	if (console == -2) {
		console = open("/dev/console", O_WRONLY | O_NOCTTY);
	}
	if (console < 0) {
		return;
	}
	va_start(arguments, format);
	n = vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	if (n > 0) {
		write(console, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
	}
}

/*
 * Settings > Battery > Battery Health & Charging (BatteryUsageUI's BatteryHealthUIController) shows a spinner for good:
 * what it asks for and is told, to the console, in Settings only, once its bundle is loaded.
 */
static IMP bh_get_auth, bh_start_spinner, bh_clear_spinner, bh_is_auth_spinner, bh_set_genuine, bh_max;

static id
my_getAuthData(id self, SEL selector)
{
	id result = ((id (*)(id, SEL))bh_get_auth)(self, selector);

	say("hakios-battery: getAuthData -> %p\n", (void *)result);
	return result;
}

static void
my_startSpinner(id self, SEL selector)
{
	say("hakios-battery: startSpinner\n");
	((void (*)(id, SEL))bh_start_spinner)(self, selector);
}

static void
my_clearSpinner(id self, SEL selector)
{
	say("hakios-battery: clearSpinner\n");
	((void (*)(id, SEL))bh_clear_spinner)(self, selector);
}

static BOOL
my_isAuthSpinner(id self, SEL selector)
{
	BOOL value = ((BOOL (*)(id, SEL))bh_is_auth_spinner)(self, selector);

	say("hakios-battery: isAuthSpinner -> %d\n", value);
	return value;
}

static void
my_setGenuineBatteryStatus(id self, SEL selector, long status)
{
	say("hakios-battery: setGenuineBatteryStatus: %ld\n", status);
	((void (*)(id, SEL, long))bh_set_genuine)(self, selector, status);
}

static id
my_getMaximumCapacity(id self, SEL selector)
{
	id result = ((id (*)(id, SEL))bh_max)(self, selector);

	say("hakios-battery: getMaximumCapacity -> %p\n", (void *)result);
	return result;
}

static void
hook(Class class, const char *name, IMP replacement, IMP *original)
{
	Method method = class_getInstanceMethod(class, sel_registerName(name));

	if (method != NULL && *original == NULL) {
		*original = method_setImplementation(method, replacement);
		say("hakios-battery: -[%s %s] watched\n", class_getName(class), name);
	}
}

static void
image_added(__unused const struct mach_header *header, __unused intptr_t slide)
{
	static bool done;
	Class class;

	if (done || (class = objc_getClass("BatteryHealthUIController")) == Nil) {
		return;
	}
	done = true;
	{
		/* what the controller has, to find what its spinner waits for */
		unsigned count = 0;
		Method *methods = class_copyMethodList(class, &count);
		char line[400] = "";
		size_t used = 0;

		for (unsigned i = 0; i < count; i++) {
			used += (size_t)snprintf(line + used, sizeof(line) - used, " %s", sel_getName(method_getName(methods[i])));
			if (used > 300 || i + 1 == count) {
				say("hakios-battery: methods:%s\n", line);
				used = 0;
				line[0] = 0;
			}
		}
		free(methods);
	}
	hook(class, "getAuthData", (IMP)my_getAuthData, &bh_get_auth);
	hook(class, "startSpinner", (IMP)my_startSpinner, &bh_start_spinner);
	hook(class, "clearSpinner", (IMP)my_clearSpinner, &bh_clear_spinner);
	hook(class, "isAuthSpinner", (IMP)my_isAuthSpinner, &bh_is_auth_spinner);
	hook(class, "setGenuineBatteryStatus:", (IMP)my_setGenuineBatteryStatus, &bh_set_genuine);
	hook(class, "getMaximumCapacity", (IMP)my_getMaximumCapacity, &bh_max);
}

#define HIGH_RATE_KEY   "CADisableMinimumFrameDurationOnPhone"

static bool
is_high_rate_key(CFStringRef key)
{
	return key != NULL && CFGetTypeID(key) == CFStringGetTypeID() &&
	    CFStringCompare(key, CFSTR(HIGH_RATE_KEY), 0) == kCFCompareEqualTo;
}

static CFTypeRef
my_CFBundleGetValueForInfoDictionaryKey(CFBundleRef bundle, CFStringRef key)
{
	if (is_high_rate_key(key)) {
		return kCFBooleanTrue;
	}
	return CFBundleGetValueForInfoDictionaryKey(bundle, key);
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
	const void *replacement, *original;
} interposers[] = {
	{ (const void *)my_CFBundleGetValueForInfoDictionaryKey, (const void *)CFBundleGetValueForInfoDictionaryKey },
};

/*
 * Boot-arg island=1: the Dynamic Island. iOS 16's SpringBoard has the "system aperture" on every phone and turns it on
 * by the device's artwork subtype, which MobileGestalt answers (ArtworkDeviceSubType; its obfuscated name
 * oPeik/9e8lQWMszEjbPzng): 2556 is the iPhone 14 Pro's, as Cowabunga and its kind set it on older phones in 2023.
 * Every application (SpringBoard is one: "@apps") is told 2556 here; nothing else of the device changes.
 */
#define ISLAND_SUBTYPE  2556
static int island = -1;

extern CFTypeRef MGCopyAnswer(CFStringRef question);
extern CFTypeRef MGCopyAnswerWithError(CFStringRef question, void *options, int *error);
extern int32_t MGGetSInt32Answer(CFStringRef question, int32_t fallback);
extern int64_t MGGetSInt64Answer(CFStringRef question, int64_t fallback);
extern int sysctlbyname(const char *, void *, size_t *, void *, size_t);

static bool
is_subtype_question(CFStringRef question)
{
	if (island == -1) {
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		island = sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "island=1") != NULL;
	}
	return island == 1 && question != NULL && CFGetTypeID(question) == CFStringGetTypeID() &&
	       (CFStringCompare(question, CFSTR("ArtworkDeviceSubType"), 0) == kCFCompareEqualTo ||
	       CFStringCompare(question, CFSTR("oPeik/9e8lQWMszEjbPzng"), 0) == kCFCompareEqualTo);
}

static CFTypeRef
subtype_number(void)
{
	int32_t value = ISLAND_SUBTYPE;

	return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
}

/*
 * The frameworks ask MobileGestalt from inside the shared cache, where interposing does not reach (island1: no island),
 * and this system has no MobileGestalt cache file to change (mg_island: none). So the exported answers' first
 * instructions are replaced, in this process, by a jump here (what an inline hook of a tweak does): MGCopyAnswer,
 * MGGetSInt32Answer and MGGetSInt64Answer. Every other question goes on to MGCopyAnswerWithError, which is left as it is.
 */
#include <mach/mach.h>
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>

static CFTypeRef
my_MGCopyAnswer(CFStringRef question)
{
	return is_subtype_question(question) ? subtype_number() : MGCopyAnswerWithError(question, NULL, NULL);
}

static int64_t
answer_number(CFStringRef question, int64_t fallback)
{
	CFTypeRef value;
	int64_t number = fallback;

	if (is_subtype_question(question)) {
		return ISLAND_SUBTYPE;
	}
	if ((value = MGCopyAnswerWithError(question, NULL, NULL)) == NULL) {
		return fallback;
	}
	if (CFGetTypeID(value) == CFNumberGetTypeID()) {
		CFNumberGetValue((CFNumberRef)value, kCFNumberSInt64Type, &number);
	} else if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
		number = CFBooleanGetValue((CFBooleanRef)value);
	}
	CFRelease(value);
	return number;
}

static int32_t
my_MGGetSInt32Answer(CFStringRef question, int32_t fallback)
{
	return (int32_t)answer_number(question, fallback);
}

static int64_t
my_MGGetSInt64Answer(CFStringRef question, int64_t fallback)
{
	return answer_number(question, fallback);
}

/* ldr x16, #8; br x16; .quad replacement */
__attribute__((unused)) static bool
jump(const char *name, void *replacement)
{
	uint8_t *at = dlsym(RTLD_DEFAULT, name);
	uint32_t code[4] = { 0x58000050, 0xd61f0200, (uint32_t)(uintptr_t)replacement, (uint32_t)((uintptr_t)replacement >> 32) };
	vm_address_t page;
	kern_return_t kr;

	if (at == NULL) {
		return false;
	}
	at = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0xfff000000000000ull);  /* (no pointer authentication here, but in case) */
	page = (vm_address_t)((uintptr_t)at & ~(uintptr_t)(vm_page_size - 1));
	kr = vm_protect(mach_task_self(), page, vm_page_size * 2, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		say("hakios-island: %s at %p: cannot be written (%d)\n", name, (void *)at, kr);
		return false;
	}
	memcpy(at, code, sizeof(code));
	vm_protect(mach_task_self(), page, vm_page_size * 2, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(at, sizeof(code));
	return true;
}

/*
 * (island3: replacing MobileGestalt's first instructions crashed SpringBoard, SIGILL -- some of those functions are
 * shorter than the 16 bytes of the jump. Not done any more.) What decides in SpringBoard, by its strings: the function
 * SBUIIsSystemApertureEnabled (SpringBoardUI; SpringBoard's own executable calls it, so a dynamic interposing of that
 * executable's binding reaches it) and the property supportsSystemAperture of whichever classes have one (an
 * Objective-C method, which changing reaches every caller, the shared cache's too). Both say YES here.
 */
struct dyld_interpose_tuple {
	const void *replacement, *replacee;
};
extern void dyld_dynamic_interpose(const struct mach_header *mh, const struct dyld_interpose_tuple array[], size_t count);

static bool
yes_function(void)
{
	return true;
}

static BOOL
yes_method(__unused id self, __unused SEL selector)
{
	return YES;
}

static unsigned island_changed;

/* the classes of one image (as it is added): their supportsSystemAperture and the like say YES */
static void
island_classes(const char *image)
{
	static const char *const names[] = { "supportsSystemAperture", "_supportsSystemAperture", "isSystemApertureEnabled" };
	unsigned count = 0;
	const char **classes = objc_copyClassNamesForImage(image, &count);

	for (unsigned i = 0; classes != NULL && i < count; i++) {
		Class class = objc_getClass(classes[i]);

		for (unsigned n = 0; class != Nil && n < sizeof(names) / sizeof(names[0]); n++) {
			SEL selector = sel_registerName(names[n]);
			Method method = class_getInstanceMethod(class, selector), meta = class_getClassMethod(class, selector);

			if (method != NULL && method_getTypeEncoding(method) != NULL && method_getTypeEncoding(method)[0] == 'B' &&
			    method_getImplementation(method) != (IMP)yes_method) {
				method_setImplementation(method, (IMP)yes_method);
				island_changed++;
				say("hakios-island: %s: -[%s %s] says YES\n", getprogname(), classes[i], names[n]);
			}
			if (meta != NULL && method_getTypeEncoding(meta) != NULL && method_getTypeEncoding(meta)[0] == 'B' &&
			    method_getImplementation(meta) != (IMP)yes_method) {
				method_setImplementation(meta, (IMP)yes_method);
				island_changed++;
				say("hakios-island: %s: +[%s %s] says YES\n", getprogname(), classes[i], names[n]);
			}
		}
	}
	free(classes);
}

static void
island_image(const struct mach_header *header, __unused intptr_t slide)
{
	static bool interposed;
	Dl_info info;

	if (dladdr(header, &info) && info.dli_fname != NULL) {
		island_classes(info.dli_fname);
	}
	if (!interposed) {
		void *enabled = dlsym(RTLD_DEFAULT, "SBUIIsSystemApertureEnabled");

		if (enabled != NULL) {
			struct dyld_interpose_tuple tuple = { (const void *)yes_function, enabled };

			interposed = true;
			for (uint32_t i = 0; i < _dyld_image_count(); i++) {
				dyld_dynamic_interpose(_dyld_get_image_header(i), &tuple, 1);
			}
			say("hakios-island: %s: SBUIIsSystemApertureEnabled interposed (%u methods changed so far)\n", getprogname(),
			    island_changed);
		}
	}
}

static void
island_start(void)
{
	if (!is_subtype_question(CFSTR("ArtworkDeviceSubType"))) {
		return;         /* (island=1 not given) */
	}
	_dyld_register_func_for_add_image(island_image);       /* (called at once for every image already there) */
}

/* -[NSBundle objectForInfoDictionaryKey:], which does not go through the function above */
static IMP original_object_for_key;

static id
my_objectForInfoDictionaryKey(id self, SEL selector, id key)
{
	if (is_high_rate_key((CFStringRef)(void *)key)) {
		return (id)(void *)kCFBooleanTrue;
	}
	return ((id (*)(id, SEL, id))original_object_for_key)(self, selector, key);
}

__attribute__((constructor))
static void
fps_start(void)
{
	Class bundle = objc_getClass("NSBundle");
	Method method = bundle != Nil ? class_getInstanceMethod(bundle, sel_registerName("objectForInfoDictionaryKey:")) : NULL;

	if (method != NULL) {
		original_object_for_key = method_setImplementation(method, (IMP)my_objectForInfoDictionaryKey);
	}
	island_start();
	if (strcmp(getprogname(), "Preferences") == 0) {
		_dyld_register_func_for_add_image(image_added);
	}
}
