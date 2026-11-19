/*
 * libhakios-poster.dylib: inside PosterBoard, a first wallpaper that can be drawn here.
 *
 * A new device gets the "hero" poster as its first lock screen and home screen wallpaper. PosterBoard
 * picks it in _defaultLockScreenWallpaperExtensionIdentifier(): on two product types the WallpaperKit
 * collections poster (com.apple.WallpaperKit.CollectionsPoster) showing the device's default wallpaper
 * ([[PBFWallpaperKitBridge defaultBridge] defaultWallpaperIdentifier], which the image builder makes
 * Stripe, drawn by CoreAnimation: darwin/tools/mkimage26.py default_wallpaper()); on every other one
 * com.apple.MercuryPoster, whose 3D scene is drawn with Metal, which this phone has not got: black.
 * (Without the hero poster PosterBoard makes a legacy poster from the wallpaper of an older Darwin, and on a
 * new device there is none: black as well.)
 *
 * That function is plain C, called from inside the shared cache, and cannot be replaced from here. What
 * it chose goes on through ObjC, inside +[PBFInitialPosterHelper initialPosterConfigurationsWithModelCoordinatorProvider:role:]:
 * the extension's store coordinator (-[PBFPosterExtensionDataStore pbf_posterExtensionStoreCoordinatorForProviderIdentifier:error:]),
 * then the descriptor's (-[PBFPosterExtensionStoreCoordinator staticDescriptorStoreCoordinatorForIdentifier:],
 * dynamic..., createDynamic...:role:error:) for the poster's template, then the descriptor identifier in
 * the userInfo of +[PBFInitialPosterHelper incomingPosterConfigurationForRole:templatePath:userInfo:].
 * During that setup, and only then, where Mercury was asked for the collections poster with the default
 * wallpaper is, as on those two product types. (The provider is an object, not a block: 24B119's
 * method makes a block of its own that asks it.)
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
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define MERCURY     CFSTR("com.apple.MercuryPoster")
#define COLLECTIONS CFSTR("com.apple.WallpaperKit.CollectionsPoster")

typedef id (*passthrough_fn)(id, SEL, id, id);
typedef id (*incoming_fn)(id, SEL, id, id, id);
typedef id (*coordinator_fn)(id, SEL, id, id *);
typedef id (*descriptor_fn)(id, SEL, id);
typedef id (*create_descriptor_fn)(id, SEL, id, id, id *);

static passthrough_fn original_initial;
static incoming_fn original_incoming;
static coordinator_fn original_coordinator;
static descriptor_fn original_static_descriptor, original_dynamic_descriptor;
static create_descriptor_fn original_create_descriptor;

static bool setting_up;                 /* inside +initialPosterConfigurationsWithModelCoordinatorProvider:role: */
static bool replacing;                  /* Mercury was asked for: its descriptor is next */
static id mercury_descriptor;           /* the descriptor identifier Mercury came with */
static id default_wallpaper;            /* what the collections poster shows instead */

static void
say(const char *text)
{
	write(2, text, strlen(text));
}

static bool
same(id object, CFTypeRef value)
{
	return object != nil && value != NULL && CFEqual((CFTypeRef)object, value);
}

/* nil where the receiver has not got the method in this build: an unrecognized selector would end PosterBoard */
static id
send(id receiver, const char *selector)
{
	SEL sel = sel_registerName(selector);

	if (receiver == nil || !class_respondsToSelector(object_getClass(receiver), sel)) {
		return nil;
	}
	return ((id (*)(id, SEL))objc_msgSend)(receiver, sel);
}

static id
initial_posters(id self, SEL selector, id provider, id role)
{
	id result;

	setting_up = true;
	result = original_initial(self, selector, provider, role);
	setting_up = replacing = false;
	return result;
}

/* -[PBFPosterExtensionDataStore pbf_posterExtensionStoreCoordinatorForProviderIdentifier:error:] */
static id
coordinator_for(id self, SEL selector, id extension, id *error)
{
	if (setting_up && same(extension, MERCURY)) {
		id bridge = send((id)objc_getClass("PBFWallpaperKitBridge"), "defaultBridge");
		id wallpaper = bridge ? send(bridge, "defaultWallpaperIdentifier") : nil;

		if (wallpaper != nil) {
			if (default_wallpaper == nil) {
				default_wallpaper = (id)CFRetain((CFTypeRef)wallpaper);
			}
			replacing = true;
			extension = (id)COLLECTIONS;
			say("hakios: PosterBoard: the collections poster with the default wallpaper, not Mercury (Metal)\n");
		} else {
			say("hakios: PosterBoard: no default wallpaper identifier; Mercury left as it was\n");
		}
	}
	return original_coordinator(self, selector, extension, error);
}

/* Mercury's descriptor identifier, met right after its coordinator was asked for: the default wallpaper's */
static id
replaced_descriptor(id descriptor)
{
	if (!replacing || descriptor == nil || same(descriptor, (CFTypeRef)default_wallpaper)) {
		return descriptor;
	}
	if (mercury_descriptor == nil) {
		mercury_descriptor = (id)CFRetain((CFTypeRef)descriptor);
	}
	return same(descriptor, (CFTypeRef)mercury_descriptor) ? default_wallpaper : descriptor;
}

static id
static_descriptor(id self, SEL selector, id descriptor)
{
	return original_static_descriptor(self, selector, replaced_descriptor(descriptor));
}

static id
dynamic_descriptor(id self, SEL selector, id descriptor)
{
	return original_dynamic_descriptor(self, selector, replaced_descriptor(descriptor));
}

static id
create_descriptor(id self, SEL selector, id descriptor, id role, id *error)
{
	return original_create_descriptor(self, selector, replaced_descriptor(descriptor), role, error);
}

/* +[PBFInitialPosterHelper incomingPosterConfigurationForRole:templatePath:userInfo:]: the wallpaper it represents */
static id
incoming_poster(id self, SEL selector, id role, id template_path, id user_info)
{
	CFStringRef *key = (CFStringRef *)dlsym(RTLD_DEFAULT, "PBFWallpaperKitBridgeWallpaperRepresentingIdentifierUserInfoKey");

	if (mercury_descriptor != nil && key != NULL && *key != NULL && user_info != nil &&
	    CFGetTypeID((CFTypeRef)user_info) == CFDictionaryGetTypeID() &&
	    same((id)CFDictionaryGetValue((CFDictionaryRef)user_info, *key), (CFTypeRef)mercury_descriptor)) {
		CFMutableDictionaryRef info = CFDictionaryCreateMutableCopy(NULL, 0, (CFDictionaryRef)user_info);
		id result;

		CFDictionarySetValue(info, *key, (CFTypeRef)default_wallpaper);
		result = original_incoming(self, selector, role, template_path, (id)info);
		CFRelease(info);
		return result;
	}
	return original_incoming(self, selector, role, template_path, user_info);
}

/*
 * a method's type encoding without the offsets and the type qualifiers ("@32@0:8@16o^@24" -> "@@:@^@");
 * simple types only
 */
static bool
types_of(Method m, char *out, size_t size)
{
	const char *t = m ? method_getTypeEncoding(m) : NULL;
	size_t n = 0;

	if (t == NULL) {
		return false;
	}
	for (; *t != '\0' && n + 1 < size; t++) {
		if ((*t < '0' || *t > '9') && strchr("rnNoORV", *t) == NULL) {
			out[n++] = *t;
		}
	}
	out[n] = '\0';
	return true;
}

static Method
method_of(const char *class_name, bool class_method, const char *selector, const char *types)
{
	Class cls = objc_getClass(class_name);
	SEL sel = sel_registerName(selector);
	Method m = cls == NULL ? NULL : class_method ? class_getClassMethod(cls, sel) : class_getInstanceMethod(cls, sel);
	char found[48] = "?", line[300];

	if (!types_of(m, found, sizeof(found)) || strcmp(found, types) != 0) {
		snprintf(line, sizeof(line), "hakios: PosterBoard: %c[%s %s] is %s, not %s\n", class_method ? '+' : '-',
		    class_name, selector, found, types);
		say(line);
		return NULL;
	}
	return m;
}

/*
 * THE POSTER EXTENSION'S WATCHDOG, under an emulator. PosterBoard runs the wallpaper in an extension process
 * and gives its scene a few seconds to be made ("Provision violated for watchdog scene-create"); with Metal
 * on the processor (boot-arg rwa_software=1, QEMU) the collections poster takes longer than that to draw
 * its first frame, and PosterBoard killed it and started another, eleven times in a minute, so no wallpaper
 * was ever shown. There the termination a watchdog asks of its delegate
 * (-watchdog:requestsTerminationOf:forReason:, in whichever class implements it) is said and not done.
 * On the phone, without that boot-arg, nothing here changes.
 */
static unsigned watchdog_spared;

static void
spare_from_watchdog(id self, SEL selector, id watchdog, id process, id reason)
{
	char line[200];

	(void)self, (void)selector, (void)watchdog, (void)reason;
	if (watchdog_spared++ < 20) {
		snprintf(line, sizeof(line), "hakios: PosterBoard: a watchdog asked for %s to be ended; under the "
		    "emulator it is left running\n", process != nil ? object_getClassName(process) : "a process");
		say(line);
	}
}

static bool
software_metal(void)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;

	return sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "rwa_software=1") != NULL;
}

/* the classes FrontBoard makes a watchdog's delegate, looked at first: asking the runtime for every class
 * realizes all of them, tens of thousands, which under the emulator is time of its own */
static const char *const watchdog_delegates[] = {
	"FBProcess", "FBApplicationProcess", "FBExtensionProcess", "FBProcessManager", "FBWorkspace",
	"FBSceneManager", "FBProcessWatchdog",
};

static unsigned
spare_in(Class *classes, unsigned count, SEL sel)
{
	unsigned spared = 0;
	char line[200], types[48] = "?";

	for (unsigned i = 0; classes != NULL && i < count; i++) {
		if (classes[i] == Nil) {
			continue;
		}
		unsigned n = 0;
		Method *methods = class_copyMethodList(classes[i], &n);

		for (unsigned k = 0; k < n; k++) {
			if (method_getName(methods[k]) != sel) {
				continue;
			}
			if (!types_of(methods[k], types, sizeof(types)) || strcmp(types, "v@:@@@") != 0) {
				snprintf(line, sizeof(line), "hakios: PosterBoard: -[%s %s] is %s, left alone\n",
				    class_getName(classes[i]), sel_getName(sel), types);
				say(line);
				continue;
			}
			method_setImplementation(methods[k], (IMP)spare_from_watchdog);
			snprintf(line, sizeof(line), "hakios: PosterBoard: -[%s watchdog:requestsTerminationOf:forReason:] "
			    "says and does not end (rwa_software=1)\n", class_getName(classes[i]));
			say(line);
			spared++;
		}
		free(methods);
	}
	return spared;
}

/* ... and the policy a provider of them answers (FBProcessWatchdogProviding's
 * -watchdogPolicyForProcess:eventContext:): none, so that no watchdog is made at all */
static id
no_watchdog_policy(id self, SEL selector, id process, id context)
{
	(void)self, (void)selector, (void)process, (void)context;
	if (watchdog_spared++ < 20) {
		say("hakios: PosterBoard: no watchdog policy for a process (rwa_software=1)\n");
	}
	return nil;
}

static unsigned
policies_off(void)
{
	Protocol *providing = objc_getProtocol("FBProcessWatchdogProviding");
	SEL sel = sel_registerName("watchdogPolicyForProcess:eventContext:");
	unsigned count = 0, spared = 0;
	Class *classes = providing != NULL ? objc_copyClassList(&count) : NULL;
	char line[200], types[48] = "?";

	for (unsigned i = 0; classes != NULL && i < count; i++) {
		if (!class_conformsToProtocol(classes[i], providing)) {
			continue;
		}
		Method m = class_getInstanceMethod(classes[i], sel);
		if (m == NULL || !types_of(m, types, sizeof(types)) || strcmp(types, "@@:@@") != 0) {
			snprintf(line, sizeof(line), "hakios: PosterBoard: %s provides watchdogs, its policy method %s\n",
			    class_getName(classes[i]), m == NULL ? "not found" : types);
			say(line);
			continue;
		}
		method_setImplementation(m, (IMP)no_watchdog_policy);
		snprintf(line, sizeof(line), "hakios: PosterBoard: -[%s watchdogPolicyForProcess:eventContext:] answers "
		    "none (rwa_software=1)\n", class_getName(classes[i]));
		say(line);
		spared++;
	}
	free(classes);
	return spared;
}

static void
watchdogs_off(void)
{
	SEL sel = sel_registerName("watchdog:requestsTerminationOf:forReason:");
	Class first[sizeof(watchdog_delegates) / sizeof(watchdog_delegates[0])];
	unsigned n = sizeof(first) / sizeof(first[0]);

	for (unsigned i = 0; i < n; i++) {
		first[i] = objc_getClass(watchdog_delegates[i]);
	}
	unsigned spared = spare_in(first, n, sel);
	if (spared == 0) {
		unsigned count = 0;
		Class *classes = objc_copyClassList(&count);

		spared = spare_in(classes, count, sel);
		free(classes);
	}
	spared += policies_off();
	if (spared == 0) {
		say("hakios: PosterBoard: no watchdog delegate or policy found to leave the poster extension running\n");
	}
}

/*
 * The snapshots PosterBoard asks of a poster extension -- the lock screen's own among them -- are
 * given a capture time and a request time (-initWithMaximumConcurrentSnapshotters:captureTimeoutInterval:
 * requestTimeoutInterval:), and the extension is ended when they run out ("timeout hit"). With Metal on
 * the processor under the emulator the collections poster draws a frame in minutes, so every snapshot
 * timed out, the extension was ended and started again, and backboardd composited a lock screen with no
 * wallpaper in it: all black. Under rwa_software=1 both times are made SNAPSHOT_STRETCH times longer.
 */
#define SNAPSHOT_STRETCH 60.0

typedef id (*snapshotters_init_fn)(id, SEL, unsigned long, double, double);
static snapshotters_init_fn original_snapshotters_init;
static Class snapshotters_class;

static id
snapshotters_init_longer(id self, SEL selector, unsigned long most, double capture, double request)
{
	char line[200];

	snprintf(line, sizeof(line), "hakios: PosterBoard: snapshotters (%lu at a time) given %.0f s to capture "
	    "and %.0f s a request instead of %.1f and %.1f (rwa_software=1)\n", most, capture * SNAPSHOT_STRETCH,
	    request * SNAPSHOT_STRETCH, capture, request);
	say(line);
	return __atomic_load_n(&original_snapshotters_init, __ATOMIC_ACQUIRE)(self, selector, most, capture * SNAPSHOT_STRETCH,
	    request * SNAPSHOT_STRETCH);
}

static void
snapshot_timeouts_longer(void)
{
	SEL sel = sel_registerName("initWithMaximumConcurrentSnapshotters:captureTimeoutInterval:requestTimeoutInterval:");
	unsigned count = 0;
	Class *classes;
	char line[200], types[48];

	if (original_snapshotters_init != NULL) {
		return;
	}
	classes = objc_copyClassList(&count);
	for (unsigned i = 0; classes != NULL && i < count; i++) {
		unsigned n = 0;
		Method *methods = class_copyMethodList(classes[i], &n);

		for (unsigned k = 0; k < n; k++) {
			if (method_getName(methods[k]) != sel) {
				continue;
			}
			if (!types_of(methods[k], types, sizeof(types)) ||
			    (strcmp(types, "@@:Qdd") != 0 && strcmp(types, "@@:qdd") != 0)) {
				snprintf(line, sizeof(line), "hakios: PosterBoard: -[%s %s] is %s, left alone\n",
				    class_getName(classes[i]), sel_getName(sel), types);
				say(line);
				continue;
			}
			if (original_snapshotters_init != NULL) {
				snprintf(line, sizeof(line), "hakios: PosterBoard: a second class with snapshot timeouts, %s, "
				    "left alone (%s has them longer)\n", class_getName(classes[i]), class_getName(snapshotters_class));
				say(line);
				continue;
			}
			/* (the original stored before the method is replaced: from then on another thread may call it) */
			IMP was = method_getImplementation(methods[k]);
			if (was == (IMP)snapshotters_init_longer) {
				continue;
			}
			snapshotters_class = classes[i];
			__atomic_store_n(&original_snapshotters_init, (snapshotters_init_fn)was, __ATOMIC_RELEASE);
			method_setImplementation(methods[k], (IMP)snapshotters_init_longer);
			snprintf(line, sizeof(line), "hakios: PosterBoard: -[%s %s] stretches its timeouts (rwa_software=1)\n",
			    class_getName(classes[i]), sel_getName(sel));
			say(line);
		}
		free(methods);
	}
	free(classes);
	if (original_snapshotters_init == NULL) {
		say("hakios: PosterBoard: no class takes snapshot timeouts yet\n");
	}
}

/*
 * The throttle policy above is made once, before this library can reach it, and the time that runs out
 * is not only its own: PosterUIFoundation asks its render sessions for -pui_renderSessionTimeoutInterval,
 * and its snapshot classes carry more. Every getter of a time-out (a selector with "TimeoutInterval" in
 * or "imeout" in it, returning a double) of a PUI, PBF or PR class, and pui_renderSessionTimeoutInterval wherever it
 * is, answers SNAPSHOT_STRETCH times what it would have. Each replaced method gets a getter of its own
 * that holds its own original: one shared getter looking its original up by the object's class found the
 * subclass's again when that called its superclass's, and called it forever. The table only keeps a
 * method from being replaced twice.
 */
#define STRETCHED_MAX 48

static struct {
	Class cls;
	SEL sel;
	IMP getter;             /* ours, which the method has now */
} stretched[STRETCHED_MAX];
static unsigned stretched_count;

static __thread unsigned stretching;    /* > 0 inside a stretched getter: one that it calls is not stretched again */

static IMP
timeout_longer(SEL sel, double (*original)(id, SEL))
{
	double (^longer)(id) = ^double (id self) {
		stretching++;
		double t = original(self, sel);
		stretching--;
		return stretching == 0 ? t * SNAPSHOT_STRETCH : t;
	};
	return imp_implementationWithBlock((id)(void *)longer);
}

static bool
timeout_getter(const char *class_name, const char *selector)
{
	if (strcmp(selector, "pui_renderSessionTimeoutInterval") == 0) {
		return true;
	}
	return (strstr(selector, "imeout") != NULL || strstr(selector, "Timeout") != NULL) &&
	    strchr(selector, ':') == NULL &&
	    (strncmp(class_name, "PUI", 3) == 0 || strncmp(class_name, "PBF", 3) == 0 ||
	     strncmp(class_name, "PR", 2) == 0);
}

static void
stretch_in(Class cls, bool meta)
{
	unsigned n = 0;
	Method *methods = class_copyMethodList(cls, &n);
	const char *name = class_getName(cls);
	char line[200], types[48];

	for (unsigned k = 0; k < n && stretched_count < STRETCHED_MAX; k++) {
		SEL sel = method_getName(methods[k]);
		IMP was = method_getImplementation(methods[k]);
		bool done = false;

		if (!timeout_getter(name, sel_getName(sel)) || !types_of(methods[k], types, sizeof(types)) ||
		    strcmp(types, "d@:") != 0) {
			continue;
		}
		for (unsigned i = 0; i < stretched_count; i++) {
			done |= (stretched[i].cls == cls && stretched[i].sel == sel) || stretched[i].getter == was;
		}
		if (done) {
			continue;
		}
		/* (the getter holds the original before the method has it: another thread may call it at once) */
		stretched[stretched_count].cls = cls;
		stretched[stretched_count].sel = sel;
		stretched[stretched_count].getter = timeout_longer(sel, (double (*)(id, SEL))was);
		method_setImplementation(methods[k], stretched[stretched_count].getter);
		stretched_count++;
		snprintf(line, sizeof(line), "hakios: PosterBoard: %c[%s %s] answers %.0f times longer (rwa_software=1)\n",
		    meta ? '+' : '-', name, sel_getName(sel), SNAPSHOT_STRETCH);
		say(line);
	}
	free(methods);
}

/*
 * The time a snapshot request runs out after is also handed to it whole -- -[PUIPosterSnapshotRequest
 * initWithPath:sceneSettingsApplicator:priority:snapshotDescriptor:retryCount:timeout:] and
 * -[PUIPosterSnapshotHostConfigurationDescriptor copyWithCaptureTimeoutInterval:] -- and kept where a
 * getter of ours does not see it read (-[PUIPosterSceneSnapshotter _activateTimeoutTimer]): those
 * arguments are stretched too.
 */
typedef id (*request_init_fn)(id, SEL, id, id, long, id, unsigned long, double);
typedef id (*copy_with_time_fn)(id, SEL, double);
static request_init_fn original_request_init;
static copy_with_time_fn original_copy_with_time;

static id
request_init_longer(id self, SEL selector, id path, id applicator, long priority, id descriptor,
    unsigned long retries, double timeout)
{
	return __atomic_load_n(&original_request_init, __ATOMIC_ACQUIRE)(self, selector, path, applicator, priority,
	    descriptor, retries, timeout * SNAPSHOT_STRETCH);
}

static id
copy_with_time_longer(id self, SEL selector, double timeout)
{
	return __atomic_load_n(&original_copy_with_time, __ATOMIC_ACQUIRE)(self, selector, timeout * SNAPSHOT_STRETCH);
}

/* (each original stored before its method is replaced, as for the snapshotters above, and a method that
 * is ours already left as it is) */
static void
stretch_arguments(void)
{
	Method m;
	char types[48];

	if (original_request_init == NULL) {
		m = class_getInstanceMethod(objc_getClass("PUIPosterSnapshotRequest"),
		    sel_registerName("initWithPath:sceneSettingsApplicator:priority:snapshotDescriptor:retryCount:timeout:"));
		if (m != NULL && method_getImplementation(m) != (IMP)request_init_longer &&
		    types_of(m, types, sizeof(types)) && strcmp(types, "@@:@@?q@Qd") == 0) {
			__atomic_store_n(&original_request_init, (request_init_fn)method_getImplementation(m), __ATOMIC_RELEASE);
			method_setImplementation(m, (IMP)request_init_longer);
			say("hakios: PosterBoard: a snapshot request's timeout argument is stretched (rwa_software=1)\n");
		}
	}
	if (original_copy_with_time == NULL) {
		m = class_getInstanceMethod(objc_getClass("PUIPosterSnapshotHostConfigurationDescriptor"),
		    sel_registerName("copyWithCaptureTimeoutInterval:"));
		if (m != NULL && method_getImplementation(m) != (IMP)copy_with_time_longer &&
		    types_of(m, types, sizeof(types)) && strcmp(types, "@@:d") == 0) {
			__atomic_store_n(&original_copy_with_time, (copy_with_time_fn)method_getImplementation(m), __ATOMIC_RELEASE);
			method_setImplementation(m, (IMP)copy_with_time_longer);
			say("hakios: PosterBoard: a capture timeout handed to a copy is stretched (rwa_software=1)\n");
		}
	}
}

/* once: every selector with "ime" in it of PosterUIFoundation's snapshot classes, for finding which time
 * it is that runs out */
static void
say_snapshot_times(Class *classes, unsigned count)
{
	static bool said;
	char line[200], types[48];

	if (said) {
		return;
	}
	for (unsigned i = 0; classes != NULL && i < count; i++) {
		const char *name = class_getName(classes[i]);

		if (strncmp(name, "PUIPoster", 9) != 0 || strstr(name, "Snapshot") == NULL) {
			continue;
		}
		said = true;
		for (int meta = 0; meta < 2; meta++) {
			unsigned n = 0;
			Method *methods = class_copyMethodList(meta ? object_getClass((id)classes[i]) : classes[i], &n);

			for (unsigned k = 0; k < n; k++) {
				const char *sel = sel_getName(method_getName(methods[k]));

				if (strstr(sel, "ime") == NULL && strstr(sel, "Time") == NULL) {
					continue;
				}
				types_of(methods[k], types, sizeof(types));
				snprintf(line, sizeof(line), "hakios: PosterBoard: %c[%s %s] %s\n", meta ? '+' : '-', name, sel, types);
				say(line);
			}
			free(methods);
		}
	}
}

static void
render_timeouts_longer(void)
{
	unsigned count = 0;
	Class *classes = objc_copyClassList(&count);

	for (unsigned i = 0; classes != NULL && i < count; i++) {
		stretch_in(classes[i], false);
		stretch_in(object_getClass((id)classes[i]), true);
	}
	say_snapshot_times(classes, count);
	free(classes);
	stretch_arguments();
}

/* FrontBoard's classes are not all there when this library is loaded: looked for again once
 * PosterBoard is running, and once more later, off its main thread (realizing every class is slow under
 * the emulator; a method that is ours already is left as it is). Both on one serial queue: a pass that
 * took longer than the time between them would otherwise run beside the next, and both could replace
 * the same method, the second taking the first's hook for the original. */
static void
watchdogs_off_later(void *context)
{
	(void)context;
	watchdogs_off();
	snapshot_timeouts_longer();
	render_timeouts_longer();
}

__attribute__((constructor)) static void
poster_init(void)
{
	if (software_metal()) {
		watchdogs_off();
		snapshot_timeouts_longer();
		render_timeouts_longer();
		dispatch_queue_t later = dispatch_queue_create("poster timeouts",
		    dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
		dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), later, NULL, watchdogs_off_later);
		dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), later, NULL, watchdogs_off_later);
	}

	Method initial = method_of("PBFInitialPosterHelper", true, "initialPosterConfigurationsWithModelCoordinatorProvider:role:", "@@:@@");
	Method incoming = method_of("PBFInitialPosterHelper", true, "incomingPosterConfigurationForRole:templatePath:userInfo:", "@@:@@@");
	Method coordinator = method_of("PBFPosterExtensionDataStore", false, "pbf_posterExtensionStoreCoordinatorForProviderIdentifier:error:", "@@:@^@");
	Method static_d = method_of("PBFPosterExtensionStoreCoordinator", false, "staticDescriptorStoreCoordinatorForIdentifier:", "@@:@");
	Method dynamic_d = method_of("PBFPosterExtensionStoreCoordinator", false, "dynamicDescriptorStoreCoordinatorForIdentifier:", "@@:@");
	Method create_d = method_of("PBFPosterExtensionStoreCoordinator", false, "createDynamicDescriptorStoreCoordinatorForIdentifier:role:error:", "@@:@@^@");

	if (!initial || !incoming || !coordinator || !static_d || !dynamic_d || !create_d) {
		say("hakios: PosterBoard: initial poster left alone\n");
		return;
	}
	original_initial = (passthrough_fn)method_setImplementation(initial, (IMP)initial_posters);
	original_incoming = (incoming_fn)method_setImplementation(incoming, (IMP)incoming_poster);
	original_coordinator = (coordinator_fn)method_setImplementation(coordinator, (IMP)coordinator_for);
	original_static_descriptor = (descriptor_fn)method_setImplementation(static_d, (IMP)static_descriptor);
	original_dynamic_descriptor = (descriptor_fn)method_setImplementation(dynamic_d, (IMP)dynamic_descriptor);
	original_create_descriptor = (create_descriptor_fn)method_setImplementation(create_d, (IMP)create_descriptor);
	say("hakios: PosterBoard: a hero poster asked of Mercury becomes the collections poster\n");
}
