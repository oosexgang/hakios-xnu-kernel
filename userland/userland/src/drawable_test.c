/*
 * drawable_test: an IOSurface of ours, imported as a texture of the GPU, and the one
 * question that tells a mapping from a copy.
 *
 *   drawable_test [WIDTH HEIGHT]      the whole of it; 1080x2400 by default, the panel's size
 *   drawable_test --refusals          only the cases the bridge must decline, by name
 *
 * WHY IT IS A PROGRAM AND NOT A UNIT TEST. The drawable path of the Metal driver
 * (src/redwooddrawable.m, docs/metal.md 10.46) ends in a texture laid over memory the
 * kernel allocated for an IOSurface, and everything about it can be made to look right
 * without being right: the import can succeed over the wrong pages, over the right pages
 * at the wrong stride, or over a staging copy that is written back later -- and in all
 * three cases the library returns a texture and the driver returns an object. So this
 * program writes through ONE view of the memory and reads through the OTHER, both ways:
 *
 *   1  the processor writes a pattern through IOSurfaceGetBaseAddress, and the texture is
 *      read with rwa_texture_read. Same values => the import found the surface's pages.
 *   2  rwa_texture_write puts different values in through the texture, and they are read
 *      back through IOSurfaceGetBaseAddress. Same values => the texture IS those pages
 *      and not a copy of them.
 *
 * The pattern is made from both coordinates (x * 7 + y * 13, and x ^ y in the low byte),
 * which is what makes a stride mistake visible instead of merely possible: a texture that
 * read the surface with the wrong bytes-per-row would return a value belonging to another
 * texel, and every value here names its own texel. The texels checked are chosen for the
 * same reason -- (0,0), the last column of the first row, the first column of the last
 * row, and the far corner -- because those four are exactly where a pitch confusion,
 * a height confusion and an offset confusion each show first.
 *
 * WHAT IT NEEDS: a kernel booted with gpu=1 (without it the library has no GPU to import
 * into and says so by name, which this program prints and reports as not-measured rather
 * than as a failure), and /redwood/lib/libredwoodair.dylib in the image (GL=1).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "redwood_drawable.h"

static int failed, checks;

#define CHECK(cond, ...) do { \
		checks++; \
		if (!(cond)) { failed++; printf("drawable_test: FAILED: " __VA_ARGS__); printf("\n"); } \
	} while (0)

/*
 * The three calls of the compiler library this program needs, looked up the way the driver
 * looks them up (src/redwoodmetal.m): the library is opened by its path on the device, and a
 * symbol that is not there is a sentence rather than a failed link. rwa_texture_from_surface
 * is NOT among them -- redwooddrawable.m finds that one itself, and this program goes through
 * redwooddrawable.m on purpose, because the bridge is what is under test.
 */
static struct {
	void *handle;
	struct rwa_device *(*device_open)(char *, size_t);
	void (*device_close)(struct rwa_device *);
	const char *(*device_name)(struct rwa_device *);
	int (*texture_read)(struct rwa_texture *, unsigned, unsigned, const unsigned[3],
			    const unsigned[3], void *, size_t, char *, size_t);
	int (*texture_write)(struct rwa_texture *, unsigned, unsigned, const unsigned[3],
			     const unsigned[3], const void *, size_t, char *, size_t);
	void (*texture_destroy)(struct rwa_texture *);
} mesa;

static int
open_library(void)
{
	mesa.handle = dlopen("/redwood/lib/libredwoodair.dylib", RTLD_NOW);
	if (mesa.handle == NULL) {
		printf("drawable_test: NOT MEASURED: no compiler library: %s\n", dlerror());
		return -1;
	}
	*(void **)&mesa.device_open = dlsym(mesa.handle, "rwa_device_open");
	*(void **)&mesa.device_close = dlsym(mesa.handle, "rwa_device_close");
	*(void **)&mesa.device_name = dlsym(mesa.handle, "rwa_device_name");
	*(void **)&mesa.texture_read = dlsym(mesa.handle, "rwa_texture_read");
	*(void **)&mesa.texture_write = dlsym(mesa.handle, "rwa_texture_write");
	*(void **)&mesa.texture_destroy = dlsym(mesa.handle, "rwa_texture_destroy");
	if (mesa.device_open == NULL || mesa.device_close == NULL || mesa.device_name == NULL ||
	    mesa.texture_read == NULL || mesa.texture_write == NULL ||
	    mesa.texture_destroy == NULL) {
		printf("drawable_test: NOT MEASURED: the compiler library is there but does not "
		       "export the five calls this needs\n");
		return -1;
	}
	return 0;
}

static CFNumberRef
num(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

/* A surface the way CoreAnimation makes a drawable's: BGRA, no explicit bytes-per-row, so
 * the kernel aligns the row itself (MINISURF_ROW_ALIGN, 64). Not asking for a row length is
 * deliberate -- it is what makes the stride the kernel's own answer rather than this
 * program's, and the stride is half of what the import depends on. */
static IOSurfaceRef
make_surface(int width, int height)
{
	CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFNumberRef n;
	IOSurfaceRef surface;

	if (props == NULL) {
		return NULL;
	}
	n = num(width); CFDictionarySetValue(props, kIOSurfaceWidth, n); CFRelease(n);
	n = num(height); CFDictionarySetValue(props, kIOSurfaceHeight, n); CFRelease(n);
	n = num(0x42475241 /* 'BGRA' */); CFDictionarySetValue(props, kIOSurfacePixelFormat, n);
	CFRelease(n);
	n = num(4); CFDictionarySetValue(props, kIOSurfaceBytesPerElement, n); CFRelease(n);
	surface = IOSurfaceCreate(props);
	CFRelease(props);
	return surface;
}

/* Every texel names itself, so a value read at the wrong place is recognisable as the wrong
 * place and not merely as wrong. */
static uint32_t
pattern(unsigned x, unsigned y, uint32_t salt)
{
	return 0xff000000u | ((uint32_t)(x * 7 + y * 13) & 0xffff) << 8 |
	       (((x ^ y) + salt) & 0xff);
}

static uint32_t
surface_texel(IOSurfaceRef surface, unsigned x, unsigned y)
{
	const char *base = (const char *)IOSurfaceGetBaseAddress(surface);
	size_t stride = IOSurfaceGetBytesPerRow(surface);

	return *(const uint32_t *)(base + (size_t)y * stride + (size_t)x * 4);
}

static void
put_surface_texel(IOSurfaceRef surface, unsigned x, unsigned y, uint32_t value)
{
	char *base = (char *)IOSurfaceGetBaseAddress(surface);
	size_t stride = IOSurfaceGetBytesPerRow(surface);

	*(uint32_t *)(base + (size_t)y * stride + (size_t)x * 4) = value;
}

/*
 * The cases the bridge owes a named refusal, run without a GPU and without a surface so that
 * they can be read on any boot. Each is a mistake a caller can really make: CoreAnimation
 * asks for plane 0 only, a surface that was never made shareable has no ID, and a format
 * outside -[CAMetalLayer setPixelFormat:]'s list cannot have come from a drawable.
 */
static int
refusals(void)
{
	char why[512];
	struct rwd_texture_shape shape = {
		.pixel_format = 80, .type = 2, .width = 4, .height = 4, .depth = 1,
		.array_length = 1, .mip_levels = 1, .sample_count = 1, .usage = 4,
	};
	uint32_t fourcc = 0;
	unsigned cpp = 0;

	why[0] = 0;
	CHECK(rwd_texture_for_surface(NULL, NULL, 0, &shape, NULL, why, sizeof(why)) == NULL,
	    "a texture with no device");
	printf("drawable_test: no device      -> %s\n", why);

	why[0] = 0;
	CHECK(rwd_format_of_metal(80, &fourcc, &cpp, why, sizeof(why)) == 0 &&
	    fourcc == 0x42475241 && cpp == 4, "MTLPixelFormatBGRA8Unorm is 'BGRA', 4 bytes");
	why[0] = 0;
	CHECK(rwd_format_of_metal(552, NULL, NULL, why, sizeof(why)) != 0,
	    "a ten-bit XR format must be refused: the kernel substitutes BGRA for it");
	printf("drawable_test: format 552     -> %s\n", why);
	why[0] = 0;
	CHECK(rwd_format_of_metal(7, NULL, NULL, why, sizeof(why)) != 0,
	    "a format this import cannot lay over one linear plane must be refused");
	printf("drawable_test: format 7       -> %s\n", why);
	/*
	 * 500 is the one the window server really asked for, 55 times in one boot
	 * (docs/metal.md 10.46): MTLPixelFormatYCBCR8_420_2P, two planes. The refusal must name
	 * it rather than say the number is unknown, and it must not be cut off -- a reason
	 * truncated mid-sentence is a reason nobody can act on, so its length is checked too.
	 */
	why[0] = 0;
	CHECK(rwd_format_of_metal(500, NULL, NULL, why, sizeof(why)) != 0,
	    "MTLPixelFormatYCBCR8_420_2P must be refused: it is two planes");
	CHECK(strstr(why, "YCBCR8_420_2P") != NULL,
	    "and the refusal must NAME it, not merely call the number unknown");
	CHECK(strlen(why) < sizeof(why) - 1,
	    "and must fit whole in the buffer it was given (%zu of %zu bytes)", strlen(why),
	    sizeof(why));
	printf("drawable_test: format 500     -> %s\n", why);
	why[0] = 0;
	CHECK(rwd_format_of_metal(629, NULL, NULL, why, sizeof(why)) != 0,
	    "629 is the last of Metal's YCbCr block and must be refused with it");
	why[0] = 0;
	CHECK(rwd_format_of_metal(630, &fourcc, &cpp, why, sizeof(why)) != 0,
	    "630 is past the YCbCr block and must fall to the general refusal");
	CHECK(strstr(why, "two planes") != NULL,
	    "... which still says what the block above it was");

	struct rwd_surface s = { .surface = 1, .width = 1080, .height = 2400, .rowBytes = 4320,
				 .size = 4320 * 2400, .planes = 1 };
	why[0] = 0;
	CHECK(rwd_surface_fits_adreno(&s, 4, why, sizeof(why)) != 0,
	    "4320 bytes a row is not a multiple of 64 and must be refused by that number");
	printf("drawable_test: row 4320       -> %s\n", why);
	s.rowBytes = 4352;
	s.size = (uint64_t)4352 * 2400;
	why[0] = 0;
	CHECK(rwd_surface_fits_adreno(&s, 4, why, sizeof(why)) == 0,
	    "4352 bytes a row is what the kernel gives and what Adreno takes");
	s.planes = 2;
	why[0] = 0;
	CHECK(rwd_surface_fits_adreno(&s, 4, why, sizeof(why)) != 0,
	    "two planes must be refused: a drawable's texture is one plane");
	printf("drawable_test: two planes     -> %s\n", why);
	return 0;
}

int
main(int argc, char **argv)
{
	unsigned width = 1080, height = 2400;
	char why[512] = { 0 };
	io_connect_t gpu = 0;
	struct rwd_surface described;
	struct rwd_surface took;
	struct rwa_device *device;
	struct rwa_texture *texture;
	IOSurfaceRef surface;
	uint32_t id;

	if (argc > 1 && strcmp(argv[1], "--refusals") == 0) {
		refusals();
		printf("drawable_test: %d of %d checks failed\n", failed, checks);
		return failed ? 1 : 0;
	}
	if (argc > 2) {
		width = (unsigned)atoi(argv[1]);
		height = (unsigned)atoi(argv[2]);
	}
	refusals();

	surface = make_surface((int)width, (int)height);
	if (surface == NULL) {
		printf("drawable_test: NOT MEASURED: IOSurfaceCreate would not make a %ux%u "
		       "BGRA surface\n", width, height);
		return 1;
	}
	id = IOSurfaceGetID(surface);
	printf("drawable_test: surface %u: %zux%zu, %zu bytes a row, %zu bytes, base %p\n", id,
	    IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface),
	    IOSurfaceGetBytesPerRow(surface), IOSurfaceGetAllocSize(surface),
	    IOSurfaceGetBaseAddress(surface));
	CHECK(id != 0, "a surface made here has an ID");
	CHECK(IOSurfaceGetBaseAddress(surface) != NULL, "and a mapping in this process");

	/*
	 * What the KERNEL says about the same surface, which is the number the import will be
	 * given. Printed next to IOSurface.framework's own answer on purpose: the two coming
	 * from different sides of the same call is what makes either of them worth quoting.
	 */
	if (rwd_gpu_open(&gpu, why, sizeof(why)) != 0) {
		printf("drawable_test: NOT MEASURED: %s\n", why);
		CFRelease(surface);
		return 1;
	}
	if (rwd_surface_describe(gpu, id, &described, why, sizeof(why)) != 0) {
		printf("drawable_test: NOT MEASURED: %s\n", why);
		rwd_gpu_close(gpu);
		CFRelease(surface);
		return 1;
	}
	printf("drawable_test: the kernel says: %ux%u '%c%c%c%c', %u bytes a row, %llu bytes "
	       "from offset %u, %u plane%s\n", described.width, described.height,
	    (char)(described.pixelFormat >> 24), (char)(described.pixelFormat >> 16),
	    (char)(described.pixelFormat >> 8), (char)described.pixelFormat, described.rowBytes,
	    (unsigned long long)described.size, described.offset, described.planes,
	    described.planes == 1 ? "" : "s");
	CHECK(described.width == width && described.height == height,
	    "the kernel and IOSurface.framework agree on the geometry");
	CHECK(described.rowBytes == (unsigned)IOSurfaceGetBytesPerRow(surface),
	    "... and on the row length: %u against %zu", described.rowBytes,
	    IOSurfaceGetBytesPerRow(surface));
	CHECK(described.rowBytes % 64 == 0,
	    "the row is a multiple of 64, which is what the import requires (%u)",
	    described.rowBytes);
	rwd_gpu_close(gpu);

	if (open_library() != 0) {
		CFRelease(surface);
		return 1;
	}
	device = mesa.device_open(why, sizeof(why));
	if (device == NULL) {
		printf("drawable_test: NOT MEASURED: %s\n", why);
		CFRelease(surface);
		return 1;
	}
	printf("drawable_test: the GPU is %s\n", mesa.device_name(device));

	/* STEP 1: the processor writes, the texture reads. */
	const unsigned corners[4][2] = {
		{ 0, 0 }, { width - 1, 0 }, { 0, height - 1 }, { width - 1, height - 1 }
	};
	for (unsigned i = 0; i < 4; i++) {
		put_surface_texel(surface, corners[i][0], corners[i][1],
		    pattern(corners[i][0], corners[i][1], 0));
	}

	struct rwd_texture_shape shape = {
		.pixel_format = 80 /* MTLPixelFormatBGRA8Unorm */,
		.type = 2 /* MTLTextureType2D */, .width = width, .height = height,
		.depth = 1, .array_length = 1, .mip_levels = 1, .sample_count = 1,
		.usage = 4 /* MTLTextureUsageRenderTarget */ | 1 /* ShaderRead */,
	};

	why[0] = 0;
	texture = rwd_texture_for_surface(device, (void *)surface, 0, &shape, &took, why,
	    sizeof(why));
	if (texture == NULL) {
		printf("drawable_test: NOT MEASURED: no texture over the surface: %s\n", why);
		mesa.device_close(device);
		CFRelease(surface);
		printf("drawable_test: %d of %d checks failed\n", failed, checks);
		return 1;
	}
	printf("drawable_test: a texture over surface %u exists\n", id);

	for (unsigned i = 0; i < 4; i++) {
		unsigned x = corners[i][0], y = corners[i][1];
		unsigned origin[3] = { x, y, 0 }, size[3] = { 1, 1, 1 };
		uint32_t got = 0, want = pattern(x, y, 0);

		why[0] = 0;
		if (mesa.texture_read(texture, 0, 0, origin, size, &got, 4, why,
		    sizeof(why)) != 0) {
			CHECK(0, "reading texel (%u,%u) through the texture: %s", x, y, why);
			continue;
		}
		CHECK(got == want, "texel (%u,%u) written through the surface reads back as "
		    "%#x through the texture, not %#x. A value belonging to another texel here "
		    "is a stride the two sides do not agree on", x, y, got, want);
	}

	/* STEP 2: the texture writes, the processor reads -- the direction that tells a
	 * mapping from a copy, because nothing writes the surface back. */
	for (unsigned i = 0; i < 4; i++) {
		unsigned x = corners[i][0], y = corners[i][1];
		unsigned origin[3] = { x, y, 0 }, size[3] = { 1, 1, 1 };
		uint32_t value = pattern(x, y, 0x5a);

		why[0] = 0;
		if (mesa.texture_write(texture, 0, 0, origin, size, &value, 4, why,
		    sizeof(why)) != 0) {
			CHECK(0, "writing texel (%u,%u) through the texture: %s", x, y, why);
			continue;
		}
	}
	for (unsigned i = 0; i < 4; i++) {
		unsigned x = corners[i][0], y = corners[i][1];
		uint32_t got = surface_texel(surface, x, y), want = pattern(x, y, 0x5a);

		CHECK(got == want, "texel (%u,%u) written through the texture reads back as "
		    "%#x through the surface, not %#x. This is the check that distinguishes a "
		    "texture over the surface's own pages from a texture over a copy of them",
		    x, y, got, want);
	}

	mesa.texture_destroy(texture);
	mesa.device_close(device);
	CFRelease(surface);
	printf("drawable_test: %d of %d checks failed\n", failed, checks);
	return failed ? 1 : 0;
}
