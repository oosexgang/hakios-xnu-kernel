/*
 * libhakios-tweaks.dylib: the tweak loader of this port.
 *
 * A tweak is a dylib that a package manager put in /var/jb/usr/lib/TweakInject, beside a property
 * list of the same name whose Filter says which programs it belongs in. Something has to be inside
 * every program to read that directory and open the tweaks that name it; on a jailbroken phone the
 * jailbreak's own hook does it, and ElleKit ships one (ellekit/libinjector.dylib). That one opens
 * here but loads nothing and says nothing about why, so this is ours, which says what it does.
 *
 * ElleKit is still what does the work a tweak asks for: the tweaks link against
 * /var/jb/usr/lib/libsubstrate.dylib (libhooker, CydiaSubstrate), which is ElleKit's libellekit,
 * and that is the engine that replaces functions and methods. Only the finding and opening is ours.
 *
 * It goes into a program either as the one library the kernel inserts (etc/inject.conf, see
 * init/init.c) or because a library of ours that is already there opens it
 * (userland/userland/src/inject_sb.c does,
 * for SpringBoard). Loading it twice is harmless: the second time does nothing.
 *
 * A Filter says any of:
 *   Bundles       the identifier of a bundle the program has loaded, its main one or a framework
 *                 (com.apple.springboard, com.apple.UIKit)
 *   Executables   the file name of the program (SpringBoard)
 *   Classes       an Objective-C class the program has
 *   CoreFoundationVersion   [min] or [min, max]: the tweak is for a CoreFoundation at least min and
 *                 below max (kCFCoreFoundationVersionNumber), and not loaded at all where it is not,
 *                 or where the list is not one or two numbers
 * and takes the tweak if every one of the first three that is given matches, or, with Mode "Any", if
 * one of them does. A Filter that gives only CoreFoundationVersion takes it in every program of that
 * CoreFoundation; one that says none of these (a misspelt key, a list where a string should be) in
 * none. A list with no Filter at all belongs in every program, as it does on a phone.
 *
 * Safe mode, for a tweak that makes SpringBoard crash as it starts, over and over: with the boot-arg
 * notweaks=1 no tweak is opened in any program. And SpringBoard leaves a mark (TWEAKS_STARTING) before
 * it opens its tweaks and takes it away once it has run for half a minute; a SpringBoard that finds
 * the mark there is one started after a start that did not get that far, and opens none. The mark is
 * in /private/var, which starts empty at every boot here: the next boot tries the tweaks again.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

/*
 * Both places a package may put a tweak: the one ElleKit uses now, and the one Cydia Substrate used,
 * which is a symbolic link to it on a phone -- but a package unpacked on this machine can leave it a
 * real directory, and some packages still write there. Looking in both costs nothing.
 */
static const char *const tweak_dirs[] = {
	"/var/jb/usr/lib/TweakInject",
	"/var/jb/Library/MobileSubstrate/DynamicLibraries",
};

/* SpringBoard's mark of a start that is opening its tweaks (a directory of mobile's that every boot makes; root writes there too) */
#define TWEAKS_STARTING "/var/mobile/tmp/tweaks-starting"
#define TWEAKS_STARTED_AFTER 30         /* seconds of SpringBoard's main queue running, after which the mark goes */

static void
say(const char *format, ...)
{
	char line[512];
	va_list args;
	int n, fd;

	va_start(args, format);
	n = vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	if (n <= 0) {
		return;
	}
	fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		(void)write(fd, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
		close(fd);
	}
}

/* The name this program was started as, and its bundle identifier when it has one yet. */
static const char *
executable_name(void)
{
	static char path[1024];
	uint32_t size = sizeof(path);
	char *slash;

	if (_NSGetExecutablePath(path, &size) != 0) {
		return NULL;
	}
	slash = strrchr(path, '/');
	return slash != NULL ? slash + 1 : path;
}

static bool
bundle_identifier(char *out, size_t size)
{
	CFBundleRef bundle = CFBundleGetMainBundle();
	CFStringRef identifier;

	if (bundle == NULL) {
		return false;
	}
	identifier = CFBundleGetIdentifier(bundle);
	return identifier != NULL && CFStringGetCString(identifier, out, (CFIndex)size, kCFStringEncodingUTF8);
}

/* Is one of the bundles the array names loaded in this process (as Substrate and ElleKit look: any bundle, not the main one only)? */
static bool
array_has_loaded_bundle(CFArrayRef array, const char *main_identifier)
{
	CFIndex count = array != NULL ? CFArrayGetCount(array) : 0;

	for (CFIndex i = 0; i < count; i++) {
		CFStringRef string = CFArrayGetValueAtIndex(array, i);
		char text[256];

		if (CFGetTypeID(string) != CFStringGetTypeID()) {
			continue;
		}
		if (main_identifier != NULL && CFStringGetCString(string, text, sizeof(text), kCFStringEncodingUTF8) &&
		    strcmp(text, main_identifier) == 0) {
			return true;
		}
		if (CFBundleGetBundleWithIdentifier(string) != NULL) {
			return true;            /* (com.apple.UIKit and the like: the frameworks the program has loaded) */
		}
	}
	return false;
}

/* Does this array of strings hold `want`? */
static bool
array_has(CFArrayRef array, const char *want)
{
	CFIndex count = array != NULL ? CFArrayGetCount(array) : 0;

	for (CFIndex i = 0; i < count; i++) {
		CFStringRef string = CFArrayGetValueAtIndex(array, i);
		char text[256];

		if (CFGetTypeID(string) == CFStringGetTypeID() &&
		    CFStringGetCString(string, text, sizeof(text), kCFStringEncodingUTF8) && strcmp(text, want) == 0) {
			return true;
		}
	}
	return false;
}

static bool
array_has_class(CFArrayRef array)
{
	CFIndex count = array != NULL ? CFArrayGetCount(array) : 0;

	for (CFIndex i = 0; i < count; i++) {
		CFStringRef string = CFArrayGetValueAtIndex(array, i);
		char text[256];

		if (CFGetTypeID(string) == CFStringGetTypeID() &&
		    CFStringGetCString(string, text, sizeof(text), kCFStringEncodingUTF8) && objc_getClass(text) != NULL) {
			return true;
		}
	}
	return false;
}

static CFArrayRef
filter_array(CFDictionaryRef filter, const char *key)
{
	CFStringRef name = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	CFArrayRef array = NULL;

	if (name != NULL) {
		CFTypeRef value = CFDictionaryGetValue(filter, name);

		if (value != NULL && CFGetTypeID(value) == CFArrayGetTypeID()) {
			array = value;
		}
		CFRelease(name);
	}
	return array;
}

/*
 * Whether this CoreFoundation is one CoreFoundationVersion names: [min] or [min, max], numbers; anything else
 * names none.
 */
static bool
core_foundation_fits(CFTypeRef versions)
{
	CFIndex count;
	double bound[2];

	if (CFGetTypeID(versions) != CFArrayGetTypeID()) {
		return false;
	}
	count = CFArrayGetCount(versions);
	if (count < 1 || count > 2) {
		return false;
	}
	for (CFIndex i = 0; i < count; i++) {
		CFTypeRef number = CFArrayGetValueAtIndex(versions, i);

		if (number == NULL || CFGetTypeID(number) != CFNumberGetTypeID() ||
		    !CFNumberGetValue(number, kCFNumberDoubleType, &bound[i])) {
			return false;
		}
	}
	return kCFCoreFoundationVersionNumber >= bound[0] && (count == 1 || kCFCoreFoundationVersionNumber < bound[1]);
}

/* Whether the list at `path` names this program. */
static bool
wanted_here(const char *path, const char *name, const char *identifier)
{
	CFDictionaryRef plist, filter;
	CFArrayRef bundles, executables, classes;
	CFStringRef mode_key, filter_key;
	CFDataRef data;
	CFTypeRef mode, versions;
	bool all, hits[3], wanted;
	int fd, used = 0;
	uint8_t buffer[65536];
	ssize_t n;

	fd = open(path, O_RDONLY);
	n = fd >= 0 ? read(fd, buffer, sizeof(buffer)) : -1;
	if (fd >= 0) {
		close(fd);
	}
	if (n <= 0) {
		return false;
	}
	data = CFDataCreate(NULL, buffer, (CFIndex)n);
	if (data == NULL) {
		return false;
	}
	plist = (CFDictionaryRef)CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL);
	CFRelease(data);
	if (plist == NULL || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		if (plist != NULL) {
			CFRelease(plist);
		}
		say("hakios: tweaks: %s is not a property list\n", path);
		return false;
	}
	filter_key = CFSTR("Filter");
	filter = CFDictionaryGetValue(plist, filter_key);
	if (filter == NULL || CFGetTypeID(filter) != CFDictionaryGetTypeID()) {
		CFRelease(plist);                       /* no Filter: it belongs everywhere */
		return true;
	}
	versions = CFDictionaryGetValue(filter, CFSTR("CoreFoundationVersion"));
	if (versions != NULL && !core_foundation_fits(versions)) {
		CFRelease(plist);                       /* for another CoreFoundation, or a range not understood */
		return false;
	}
	bundles = filter_array(filter, "Bundles");
	executables = filter_array(filter, "Executables");
	classes = filter_array(filter, "Classes");
	hits[0] = bundles != NULL && array_has_loaded_bundle(bundles, identifier);
	hits[1] = executables != NULL && name != NULL && array_has(executables, name);
	hits[2] = classes != NULL && array_has_class(classes);
	/* As Substrate and ElleKit: by default every kind that is given has to match; Mode "Any": one is enough. */
	mode_key = CFSTR("Mode");
	mode = CFDictionaryGetValue(filter, mode_key);
	all = !(mode != NULL && CFGetTypeID(mode) == CFStringGetTypeID() &&
	    CFStringCompare(mode, CFSTR("Any"), 0) == kCFCompareEqualTo);
	used = (bundles != NULL) + (executables != NULL) + (classes != NULL);
	if (used == 0) {
		/* (only a version: every program of it; nothing this knows: no program, rather than every one) */
		wanted = versions != NULL;
		if (!wanted) {
			say("hakios: tweaks: %s: its Filter names no bundle, program, class or version: not loaded\n", path);
		}
	} else if (all) {
		wanted = (bundles == NULL || hits[0]) && (executables == NULL || hits[1]) && (classes == NULL || hits[2]);
	} else {
		wanted = hits[0] || hits[1] || hits[2];
	}
	CFRelease(plist);
	return wanted;
}

/* notweaks=1 among the boot-args, as a word of its own */
static bool
tweaks_off(void)
{
	char arguments[1024];
	size_t size = sizeof(arguments) - 1;
	const char *at;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return false;
	}
	arguments[size < sizeof(arguments) ? size : sizeof(arguments) - 1] = '\0';
	for (at = strstr(arguments, "notweaks=1"); at != NULL; at = strstr(at + 1, "notweaks=1")) {
		if ((at == arguments || at[-1] == ' ') && (at[10] == '\0' || at[10] == ' ')) {
			return true;
		}
	}
	return false;
}

/* (on SpringBoard's main queue, half a minute after it began to run it: this start got through) */
static void
tweaks_started(void *context __attribute__((unused)))
{
	unlink(TWEAKS_STARTING);
}

/* (the main queue's first turn: SpringBoard is running, and the half minute counts from here, not from the constructor) */
static void
springboard_running(void *context __attribute__((unused)))
{
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, (int64_t)TWEAKS_STARTED_AFTER * (int64_t)NSEC_PER_SEC), dispatch_get_main_queue(),
	    NULL, tweaks_started);
}

/*
 * SpringBoard's safe mode: false (and said) when the mark of an earlier start is still there; otherwise the
 * mark is made. Either way it is taken away once this start has run long enough: also after a start in safe
 * mode, so that SpringBoard started again once the tweak is removed tries the tweaks again.
 */
static bool
springboard_may_open_tweaks(void)
{
	struct stat st;
	bool earlier = lstat(TWEAKS_STARTING, &st) == 0;
	int fd;

	if (earlier) {
		say("hakios: tweaks: SpringBoard's last start did not run %d s past opening its tweaks (%s is there): "
		    "none opened this time\n", TWEAKS_STARTED_AFTER, TWEAKS_STARTING);
	} else if ((fd = open(TWEAKS_STARTING, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600)) < 0) {
		say("hakios: tweaks: %s cannot be made: a start that fails will not be caught\n", TWEAKS_STARTING);
	} else {
		close(fd);
	}
	dispatch_async_f(dispatch_get_main_queue(), NULL, springboard_running);
	return !earlier;
}

static void
load_the_tweaks(void)
{
	char identifier[256];
	const char *name = executable_name();
	const char *who = bundle_identifier(identifier, sizeof(identifier)) ? identifier : NULL;
	int opened = 0, tried = 0;

	if (tweaks_off()) {
		say("hakios: tweaks: notweaks=1: none opened in %s\n", name != NULL ? name : "?");
		return;
	}
	if (name != NULL && strcmp(name, "SpringBoard") == 0 && !springboard_may_open_tweaks()) {
		return;
	}
	for (unsigned d = 0; d < sizeof(tweak_dirs) / sizeof(tweak_dirs[0]); d++) {
		const char *where = tweak_dirs[d];
		struct dirent *entry;
		struct stat st;
		DIR *dir;

		/* the second is usually a link to the first: then it is the same directory, done already */
		if (d > 0 && lstat(where, &st) == 0 && S_ISLNK(st.st_mode)) {
			continue;
		}
		dir = opendir(where);
		if (dir == NULL) {
			continue;                       /* no prefix, or no tweaks in it */
		}
		while ((entry = readdir(dir)) != NULL) {
			char plist[1024], dylib[1024];
			size_t length = strlen(entry->d_name);

			if (length < 7 || strcmp(entry->d_name + length - 6, ".plist") != 0) {
				continue;
			}
			snprintf(plist, sizeof(plist), "%s/%s", where, entry->d_name);
			snprintf(dylib, sizeof(dylib), "%s/%.*s.dylib", where, (int)(length - 6), entry->d_name);
			if (!wanted_here(plist, name, who)) {
				continue;
			}
			tried++;
			if (dlopen(dylib, RTLD_NOW | RTLD_GLOBAL) != NULL) {
				opened++;
				say("hakios: tweaks: %s\n", dylib);
			} else {
				say("hakios: tweaks: %s: %s\n", dylib, dlerror());
			}
		}
		closedir(dir);
	}
	if (tried > 0) {
		say("hakios: tweaks: %s (%s): %d of %d opened\n", name != NULL ? name : "?",
		    who != NULL ? who : "no bundle", opened, tried);
	}
}

__attribute__((constructor))
static void
hakios_tweaks_init(void)
{
	static bool done;

	if (done) {
		return;
	}
	done = true;
	load_the_tweaks();
}
