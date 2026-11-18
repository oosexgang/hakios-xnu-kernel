/*
 * Move the status bar's leading region past a hole-punch camera at the top left, by the points the
 * board's file gives (screen { } statusbar_leading_shift; nothing is done on a board that gives none).
 * Darwin 27's live status bar uses STUIStatusBarVisualProvider, not UIKit's
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

#include "user_boards.h"      /* generated from arch/arm64/boot/dts/NAME/board.dts by tools/boards.py gen */

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

/* The points this board's status bar is shifted by: its row of the generated table, by the name the
 * loader put in /chosen/darwin-board. 0 for a board that is not there or asks for none. */
static double
leading_shift(void)
{
	double points = 0;
	io_registry_entry_t chosen = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/chosen");

	if (chosen) {
		CFTypeRef board = IORegistryEntryCreateCFProperty(chosen, CFSTR("darwin-board"), kCFAllocatorDefault, 0);
		if (board && CFGetTypeID(board) == CFDataGetTypeID()) {
			CFIndex n = CFDataGetLength((CFDataRef)board);
			const UInt8 *p = CFDataGetBytePtr((CFDataRef)board);

			if (n > 0 && p[n - 1] == 0) n--;        /* (the property is a string with its end) */
			for (size_t i = 0; i < sizeof(hakios_user_boards) / sizeof(hakios_user_boards[0]); i++) {
				const char *name = hakios_user_boards[i].name;

				if (strlen(name) == (size_t)n && memcmp(p, name, (size_t)n) == 0) {
					points = hakios_user_boards[i].statusbar_leading_shift;
				}
			}
		}
		if (board) CFRelease(board);
		IOObjectRelease(chosen);
	}
	return points;
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
statusbar26_init(void)
{
	SEL sel = sel_registerName("leadingCenteringOffset");
	const double shift = leading_shift();
	unsigned hooked = 0;

	if (shift <= 0) return;
	for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); i++) {
		Class cls = objc_getClass(providers[i]);
		Method method = cls ? own_class_method(cls, sel) : NULL;
		const char *types = method ? method_getTypeEncoding(method) : NULL;

		if (types == NULL || strcmp(types, "d16@0:8") != 0) continue;
		double (*original)(id, SEL) = (double (*)(id, SEL))method_getImplementation(method);
		double (^shifted)(id) = ^double (id self) {
			depth++;
			double offset = original(self, sel);
			depth--;
			return depth == 0 ? offset + shift : offset;
		};
		method_setImplementation(method, imp_implementationWithBlock((id)(void *)shifted));
		hooked++;
	}
	int fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		dprintf(fd, "hakios: statusbar %s[%d]: leading metric +%.0f pt in %u UIKit/StatusKitUI providers\n",
		    getprogname(), getpid(), shift, hooked);
		close(fd);
	}
}
