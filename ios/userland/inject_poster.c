/*
 * libhakios-poster.dylib: inside PosterBoard, a first wallpaper that can be drawn here.
 *
 * A new device gets the "hero" poster as its first lock screen and home screen wallpaper. PosterBoard
 * picks it in _defaultLockScreenWallpaperExtensionIdentifier(): on two product types the WallpaperKit
 * collections poster (com.apple.WallpaperKit.CollectionsPoster) showing the device's default wallpaper
 * ([[PBFWallpaperKitBridge defaultBridge] defaultWallpaperIdentifier], which the image builder makes
 * Stripe, drawn by CoreAnimation: ios/tools/mkimage26.py default_wallpaper()); on every other one
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
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
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

static id
send(id receiver, const char *selector)
{
	return ((id (*)(id, SEL))objc_msgSend)(receiver, sel_registerName(selector));
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

__attribute__((constructor)) static void
poster_init(void)
{
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
