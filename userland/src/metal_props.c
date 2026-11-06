/*
 * metal_props: put Metal's two discovery properties on the IOAcceleratorES service, read them
 * back, and take them off again.
 *
 * Metal finds a GPU in exactly one way (docs/metal.md 1): MTLRegisterDevices() enumerates
 * IOServiceMatching("IOAcceleratorES"), reads "MetalPluginName" off each service, composes
 * "/System/Library/Extensions/" + that + ".bundle", opens it with NSBundle and takes the class
 * "MetalPluginClassName" names. This port publishes such a service ("sgx",
 * iokit/Kernel/MiniIOMobileFramebuffer.cpp) deliberately without those two properties, so the
 * chain breaks at the first read and nothing in the system ever walks the Metal road.
 *
 * This program is the switch, and the whole of what it does is one IOKit call. It needs the
 * kernel booted with metal=1: without it the service's setProperties returns kIOReturnUnsupported
 * (0xe00002c7), which is also what every kernel before this change did
 * (IORegistryEntry::setProperties is "return kIOReturnUnsupported").
 *
 * It deliberately does NOT link or call Metal. The conditions Metal puts on a device object are
 * asserts (docs/metal.md 10.2-10.3): asking it from the same process that flipped the switch
 * would turn a measurement into an abort, and would say nothing about whether the properties took.
 * Asking Metal is bin/metal_drv_test's job, in a process of its own, afterwards.
 *
 *   metal_props                 show what the service carries now
 *   metal_props on              set RedwoodMetal / RedwoodMetalDevice (what this port installs)
 *   metal_props on NAME CLASS   set those two instead
 *   metal_props off             remove both (an empty string removes, see setProperties)
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

/*
 * The call itself, under a name of ours. IOKit's header marks IORegistryEntrySetCFProperties
 * API_UNAVAILABLE(ios), which is a statement about the declaration and not about the code: iOS
 * 16.5's IOKit exports _IORegistryEntrySetCFProperties all the same (the SDK's own IOKit.tbd
 * lists it), so the symbol is reached by an asm label and the link finds it. Redeclaring it by
 * its own name does not work -- the header's attribute stays attached to that name.
 */
extern kern_return_t set_cf_properties(io_registry_entry_t entry, CFTypeRef properties)
    __asm("_IORegistryEntrySetCFProperties");

#define kPluginNameKey  "MetalPluginName"
#define kPluginClassKey "MetalPluginClassName"

/* print one string property of the service, or "(none)" -- the state metal_test reports today */
static void
show_one(io_service_t service, const char *key)
{
	CFStringRef name = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	CFTypeRef value = name ? IORegistryEntryCreateCFProperty(service, name, NULL, 0) : NULL;
	char buf[256];

	if (value != NULL && CFGetTypeID(value) == CFStringGetTypeID() &&
	    CFStringGetCString(value, buf, sizeof(buf), kCFStringEncodingUTF8)) {
		printf("metal_props: %s = %s\n", key, buf);
	} else if (value != NULL) {
		printf("metal_props: %s is there but is not a CFString -- Metal would ignore it\n", key);
	} else {
		printf("metal_props: %s (none)\n", key);
	}
	if (value != NULL) {
		CFRelease(value);
	}
	if (name != NULL) {
		CFRelease(name);
	}
}

/* the path Metal would compose from MetalPluginName, and whether anything is at it */
static void
show_path(io_service_t service)
{
	CFStringRef key = CFSTR(kPluginNameKey);
	CFTypeRef value = IORegistryEntryCreateCFProperty(service, key, NULL, 0);
	char name[128], path[256];

	if (value == NULL || CFGetTypeID(value) != CFStringGetTypeID() ||
	    !CFStringGetCString(value, name, sizeof(name), kCFStringEncodingUTF8)) {
		if (value != NULL) {
			CFRelease(value);
		}
		return;
	}
	CFRelease(value);
	snprintf(path, sizeof(path), "/System/Library/Extensions/%s.bundle", name);
	printf("metal_props: Metal would open %s: %s\n", path,
	    access(path, F_OK) == 0 ? "it is there" : "NOT THERE (userland/build.sh links it from /redwood/lib)");
}

static int
set_both(io_service_t service, const char *name, const char *cls)
{
	CFStringRef keys[2] = { CFSTR(kPluginNameKey), CFSTR(kPluginClassKey) };
	CFStringRef values[2];
	CFDictionaryRef dict;
	kern_return_t kr;

	values[0] = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	values[1] = CFStringCreateWithCString(NULL, cls, kCFStringEncodingUTF8);
	if (values[0] == NULL || values[1] == NULL) {
		printf("metal_props: out of memory\n");
		return 1;
	}
	dict = CFDictionaryCreate(NULL, (const void **) keys, (const void **) values, 2,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFRelease(values[0]);
	CFRelease(values[1]);
	if (dict == NULL) {
		printf("metal_props: out of memory\n");
		return 1;
	}
	kr = set_cf_properties(service, dict);
	CFRelease(dict);
	if (kr != KERN_SUCCESS) {
		printf("metal_props: IORegistryEntrySetCFProperties failed %#x%s\n", kr,
		    kr == kIOReturnUnsupported ? " (kIOReturnUnsupported: boot the kernel with metal=1)" : "");
		return 1;
	}
	printf("metal_props: set %s=%s, %s=%s\n", kPluginNameKey, name, kPluginClassKey, cls);
	return 0;
}

int
main(int argc, char **argv)
{
	io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("IOAcceleratorES"));
	const char *what = argc > 1 ? argv[1] : "show";
	int bad = 0;

	if (service == IO_OBJECT_NULL) {
		printf("metal_props: no IOAcceleratorES service in the registry\n");
		return 1;
	}
	if (strcmp(what, "on") == 0) {
		bad = set_both(service, argc > 3 ? argv[2] : "RedwoodMetal",
		    argc > 3 ? argv[3] : "RedwoodMetalDevice");
	} else if (strcmp(what, "off") == 0) {
		bad = set_both(service, "", "");
	} else if (strcmp(what, "show") != 0) {
		printf("metal_props: usage: metal_props [show | on [NAME CLASS] | off]\n");
		bad = 1;
	}
	show_one(service, kPluginNameKey);
	show_one(service, kPluginClassKey);
	show_path(service);
	IOObjectRelease(service);
	return bad;
}
