/*
 * Move Renoir's leading status-bar region past its top-left hole punch by 24 points.
 * iOS 27's live status bar uses STUIStatusBarVisualProvider, not UIKit's
 * retained _UIStatusBarVisualProvider classes. Both use leadingCenteringOffset.
 * Keep native layout and apply the offset once across superclass calls.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SHIFT_POINTS 24.0

static const char *const providers[] = {
	"_UIStatusBarVisualProvider_Split", "_UIStatusBarVisualProvider_Split828", "_UIStatusBarVisualProvider_Split1080",
	"_UIStatusBarVisualProvider_Split1080_Wide", "_UIStatusBarVisualProvider_Split1125",
	"_UIStatusBarVisualProvider_Split1170", "_UIStatusBarVisualProvider_Split1170_Wide",
	"_UIStatusBarVisualProvider_Split1242", "_UIStatusBarVisualProvider_Split1284",
	"_UIStatusBarVisualProvider_Split1284_Wide", "_UIStatusBarVisualProvider_FixedSplit",
	"_UIStatusBarVisualProvider_CustomSplit",
	"STUIStatusBarVisualProvider_FixedSplit", "STUIStatusBarVisualProvider_Split1125",
	"STUIStatusBarVisualProvider_Split1080", "STUIStatusBarVisualProvider_Split1080_Wide",
	"STUIStatusBarVisualProvider_Split1170_Wide", "STUIStatusBarVisualProvider_Split828",
	"STUIStatusBarVisualProvider_Split1284_Wide",
};

static __thread int depth;      /* > 0 inside a hooked metric: a call to the superclass's is not shifted again */

static int
is_renoir(void)
{
	int match = 0;
	io_registry_entry_t chosen = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/chosen");

	if (chosen) {
		CFTypeRef board = IORegistryEntryCreateCFProperty(chosen, CFSTR("darwin-board"), kCFAllocatorDefault, 0);
		if (board && CFGetTypeID(board) == CFDataGetTypeID()) {
			CFIndex n = CFDataGetLength((CFDataRef)board);
			const UInt8 *p = CFDataGetBytePtr((CFDataRef)board);
			match = (n == 6 || (n == 7 && p[6] == 0)) && memcmp(p, "renoir", 6) == 0;
		}
		if (board) CFRelease(board);
		IOObjectRelease(chosen);
	}
	return match;
}

/* Ignore inherited methods: changing one must not hook its superclass twice. */
static Method
own_class_method(Class cls, SEL sel)
{
	Method method = class_getClassMethod(cls, sel);
	Class parent = class_getSuperclass(cls);
	return method && (!parent || method != class_getClassMethod(parent, sel)) ? method : NULL;
}

__attribute__((constructor))
static void
renoir_statusbar26_init(void)
{
	SEL sel = sel_registerName("leadingCenteringOffset");
	unsigned hooked = 0;

	if (!is_renoir()) return;
	for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); i++) {
		Class cls = objc_getClass(providers[i]);
		Method method = cls ? own_class_method(cls, sel) : NULL;

		if (method == NULL || strcmp(method_getTypeEncoding(method), "d16@0:8") != 0) continue;
		double (*original)(id, SEL) = (double (*)(id, SEL))method_getImplementation(method);
		double (^shifted)(id) = ^double (id self) {
			depth++;
			double offset = original(self, sel);
			depth--;
			return depth == 0 ? offset + SHIFT_POINTS : offset;
		};
		method_setImplementation(method, imp_implementationWithBlock((id)(void *)shifted));
		hooked++;
	}
	int fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		dprintf(fd, "renoir: statusbar %s[%d]: leading metric +%.0f pt in %u UIKit/StatusKitUI providers\n",
		    getprogname(), getpid(), SHIFT_POINTS, hooked);
		close(fd);
	}
}
