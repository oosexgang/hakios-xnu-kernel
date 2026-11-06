/*
 * surf_test: exercises the kernel's IOSurface stand-in through the real IOSurface.framework.
 *
 *   surf_test              the tests; starts itself again as a second process for the sharing part
 *   surf_test child ID     (internal) look surface ID up, check the pattern, change one pixel
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/mach.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int failed, checks;

#define CHECK(cond, ...) do { \
		checks++; \
		if (!(cond)) { failed++; printf("surf_test: FAILED: " __VA_ARGS__); printf("\n"); } \
	} while (0)

static CFNumberRef
num(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

static IOSurfaceRef
make_surface(int width, int height, uint32_t format, int bytes_per_element)
{
	CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFNumberRef n;
	IOSurfaceRef surface;

	n = num(width); CFDictionarySetValue(props, kIOSurfaceWidth, n); CFRelease(n);
	n = num(height); CFDictionarySetValue(props, kIOSurfaceHeight, n); CFRelease(n);
	n = num((int) format); CFDictionarySetValue(props, kIOSurfacePixelFormat, n); CFRelease(n);
	if (bytes_per_element) {
		n = num(bytes_per_element); CFDictionarySetValue(props, kIOSurfaceBytesPerElement, n); CFRelease(n);
	}
	surface = IOSurfaceCreate(props);
	CFRelease(props);
	return surface;
}

static uint32_t
pattern(int x, int y)
{
	return 0xff000000u | (uint32_t) (x * 7 + y * 13) << 8 | (uint32_t) ((x ^ y) & 0xff);
}

static int
child(uint32_t sid)
{
	IOSurfaceRef surface = IOSurfaceLookup(sid);
	uint32_t *row;
	int bad = 0;

	if (surface == NULL) {
		printf("surf_test: child: no surface %u\n", sid);
		return 2;
	}
	IOSurfaceLock(surface, 0, NULL);
	for (int y = 0; y < (int) IOSurfaceGetHeight(surface); y++) {
		row = (uint32_t *) ((char *) IOSurfaceGetBaseAddress(surface) + y * IOSurfaceGetBytesPerRow(surface));
		for (int x = 0; x < (int) IOSurfaceGetWidth(surface); x++) {
			bad += row[x] != pattern(x, y);
		}
	}
	row = (uint32_t *) IOSurfaceGetBaseAddress(surface);
	row[3] = 0x12345678;
	IOSurfaceUnlock(surface, 0, NULL);
	CFRelease(surface);
	printf("surf_test: child: surface %u, %d wrong pixels\n", sid, bad);
	return bad ? 3 : 0;
}

int
main(int argc, char **argv)
{
	IOSurfaceRef surface, again, yuv;
	uint32_t sid, seed, *row;
	mach_port_t port;
	CFStringRef text;
	CFTypeRef value;

	if (argc == 3 && strcmp(argv[1], "child") == 0) {
		return child((uint32_t) atoi(argv[2]));
	}

	surface = make_surface(64, 48, 'BGRA', 4);
	CHECK(surface != NULL, "IOSurfaceCreate");
	if (surface == NULL) {
		printf("surf_test: no IOSurface service, giving up\n");
		return 1;
	}
	sid = IOSurfaceGetID(surface);
	printf("surf_test: surface %u: %zux%zu, %zu bytes per row, %zu bytes, base %p\n", sid, IOSurfaceGetWidth(surface),
	    IOSurfaceGetHeight(surface), IOSurfaceGetBytesPerRow(surface), IOSurfaceGetAllocSize(surface),
	    IOSurfaceGetBaseAddress(surface));
	CHECK(sid != 0, "surface number");
	CHECK(IOSurfaceGetWidth(surface) == 64 && IOSurfaceGetHeight(surface) == 48, "size");
	CHECK(IOSurfaceGetBytesPerRow(surface) >= 256, "bytes per row");
	CHECK(IOSurfaceGetAllocSize(surface) >= 48 * 256, "alloc size");
	CHECK(IOSurfaceGetPixelFormat(surface) == 'BGRA', "pixel format");
	CHECK(IOSurfaceGetBytesPerElement(surface) == 4, "bytes per element");
	CHECK(IOSurfaceGetBaseAddress(surface) != NULL, "base address");

	seed = IOSurfaceGetSeed(surface);
	CHECK(IOSurfaceLock(surface, 0, NULL) == KERN_SUCCESS, "lock");
	for (int y = 0; y < 48; y++) {
		row = (uint32_t *) ((char *) IOSurfaceGetBaseAddress(surface) + y * IOSurfaceGetBytesPerRow(surface));
		CHECK(row[0] == 0 && row[63] == 0, "new surface not zeroed, row %d", y);
		for (int x = 0; x < 64; x++) {
			row[x] = pattern(x, y);
		}
	}
	CHECK(IOSurfaceUnlock(surface, 0, NULL) == KERN_SUCCESS, "unlock");
	CHECK(IOSurfaceGetSeed(surface) != seed, "seed did not change after writing (%u)", seed);
	seed = IOSurfaceGetSeed(surface);
	IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
	IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	CHECK(IOSurfaceGetSeed(surface) == seed, "seed changed after a read-only lock");

	/* values */
	text = CFSTR("a value");
	IOSurfaceSetValue(surface, CFSTR("redwood.test"), text);
	value = IOSurfaceCopyValue(surface, CFSTR("redwood.test"));
	CHECK(value != NULL && CFEqual(value, text), "value did not come back");
	if (value) {
		CFRelease(value);
	}
	value = num(42);
	IOSurfaceSetValue(surface, CFSTR("redwood.number"), value);
	CFRelease(value);
	value = IOSurfaceCopyAllValues(surface);
	CHECK(value != NULL && CFGetTypeID(value) == CFDictionaryGetTypeID() && CFDictionaryGetCount(value) >= 2,
	    "all values");
	if (value) {
		CFRelease(value);
	}
	IOSurfaceRemoveValue(surface, CFSTR("redwood.test"));
	value = IOSurfaceCopyValue(surface, CFSTR("redwood.test"));
	CHECK(value == NULL, "value still there after removing it");
	value = IOSurfaceCopyValue(surface, CFSTR("redwood.number"));
	CHECK(value != NULL, "the other value is gone");
	if (value) {
		CFRelease(value);
	}

	/* use count */
	CHECK(!IOSurfaceIsInUse(surface), "in use from the start");
	IOSurfaceIncrementUseCount(surface);
	CHECK(IOSurfaceIsInUse(surface), "not in use after increment");
	IOSurfaceDecrementUseCount(surface);
	CHECK(!IOSurfaceIsInUse(surface), "in use after decrement");

	/* the same surface through a Mach port and by number */
	port = IOSurfaceCreateMachPort(surface);
	CHECK(port != MACH_PORT_NULL, "IOSurfaceCreateMachPort");
	again = IOSurfaceLookupFromMachPort(port);
	CHECK(again != NULL && IOSurfaceGetID(again) == sid, "lookup from the Mach port");
	if (again) {
		CHECK(IOSurfaceGetBaseAddress(again) == IOSurfaceGetBaseAddress(surface), "second view of the same surface");
		CFRelease(again);
	}
	mach_port_deallocate(mach_task_self(), port);
	again = IOSurfaceLookup(sid);
	CHECK(again != NULL && IOSurfaceGetID(again) == sid, "lookup by number");
	if (again) {
		CFRelease(again);
	}
	CHECK(IOSurfaceLookup(sid + 1000) == NULL, "found a surface that does not exist");

	/* another process */
	{
		char number[16];
		char *args[] = { argv[0], "child", number, NULL };
		pid_t pid;
		int status = -1;

		snprintf(number, sizeof(number), "%u", sid);
		seed = IOSurfaceGetSeed(surface);
		if (posix_spawn(&pid, argv[0], NULL, NULL, args, environ) == 0) {
			waitpid(pid, &status, 0);
		}
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "second process: status 0x%x", status);
		row = (uint32_t *) IOSurfaceGetBaseAddress(surface);
		CHECK(row[3] == 0x12345678, "the pixel written by the second process: 0x%x", row[3]);
		CHECK(IOSurfaceGetSeed(surface) != seed, "seed did not change after the second process wrote");
	}
	CFRelease(surface);
	CHECK(IOSurfaceLookup(sid) == NULL, "surface still exists after the last release");

	/* two planes */
	yuv = make_surface(100, 50, '420v', 0);
	CHECK(yuv != NULL, "IOSurfaceCreate 420v");
	if (yuv) {
		CHECK(IOSurfaceGetPlaneCount(yuv) == 2, "plane count %zu", IOSurfaceGetPlaneCount(yuv));
		CHECK(IOSurfaceGetWidthOfPlane(yuv, 1) == 50 && IOSurfaceGetHeightOfPlane(yuv, 1) == 25, "chroma plane %zux%zu",
		    IOSurfaceGetWidthOfPlane(yuv, 1), IOSurfaceGetHeightOfPlane(yuv, 1));
		CHECK((char *) IOSurfaceGetBaseAddressOfPlane(yuv, 1) >= (char *) IOSurfaceGetBaseAddressOfPlane(yuv, 0) +
		    50 * IOSurfaceGetBytesPerRowOfPlane(yuv, 0), "planes overlap");
		CFRelease(yuv);
	}

	printf("surf_test: %d checks, %d failed\n", checks, failed);
	return failed != 0;
}
